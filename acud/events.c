#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#include "events.h"
#include "log.h"

/*
 * 기본 보관 한도. 36byte 이벤트 1,000만 건이면 SQLite 오버헤드까지 대략 1GB 안쪽이다.
 * eMMC 용량과 링 삭제 비용을 함께 보고 정한 값이고, config로 바꿀 수 있다.
 */
#define EVENTS_DEFAULT_CAPACITY 10000000LL

/*
 * 링 삭제를 매 적재마다 하지 않는 이유: DELETE 한 번에 붙는 비용(인덱스 정리, WAL 기록)이
 * 건당으로 흩어지면 카드 태그 응답이 그만큼 늦어진다. 한도를 이만큼 넘겼을 때 한 번에 지운다.
 */
#define EVENTS_TRIM_SLACK 1000LL

/* DB를 못 열었을 때 메모리로 들고 있을 건수. 예전 net.c 큐(32칸)보다는 넉넉히 */
#define EVENTS_MEMORY_CAP 512

struct AcuEvents {
    sqlite3 *db;           /* NULL이면 메모리 전용 모드 */
    long long capacity;

    sqlite3_stmt *st_append;
    sqlite3_stmt *st_fetch;
    sqlite3_stmt *st_ack;
    sqlite3_stmt *st_min;   /* 가장 오래된 seq */
    sqlite3_stmt *st_max;   /* 가장 새로운 seq */
    sqlite3_stmt *st_sent;  /* 전송 위치 */

    long long next_trim_check; /* 총 건수가 이 값을 넘으면 링 삭제를 한 번 본다 */

    /* 메모리 전용 모드용 링 */
    AcuEvent mem[EVENTS_MEMORY_CAP];
    int      mem_head;
    int      mem_count;
    int64_t  mem_next_seq;
};

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS events ("
    "    seq         INTEGER PRIMARY KEY AUTOINCREMENT," /* 지워도 번호를 다시 쓰지 않게 AUTOINCREMENT */
    "    event_code  INTEGER NOT NULL,"
    "    op_mode     INTEGER NOT NULL,"
    "    module_addr INTEGER NOT NULL,"
    "    reader_addr INTEGER NOT NULL,"
    "    door_status INTEGER NOT NULL,"
    "    func_code   INTEGER NOT NULL,"
    "    ts          INTEGER NOT NULL,"  /* unix time */
    "    access_id   BLOB NOT NULL"      /* 8byte */
    ");"
    /* 전송 위치를 같은 파일에 둔다 - 전원이 끊겨도 적재와 위치가 어긋나지 않게 */
    "CREATE TABLE IF NOT EXISTS event_state ("
    "    key   TEXT PRIMARY KEY,"
    "    value INTEGER NOT NULL"
    ");"
    "INSERT OR IGNORE INTO event_state(key, value) VALUES('sent_seq', 0);";

/*
 * WAL + synchronous=FULL.
 *
 * WAL은 읽기(전송할 이벤트 꺼내기)와 쓰기(카드 이벤트 적재)가 서로를 막지 않게 한다.
 * synchronous는 **FULL**로 둔다 - NORMAL이면 프로세스가 죽어도 살아남지만 **전원이 끊기면
 * 마지막 몇 건을 잃을 수 있다.** 출입 기록에서 그건 곧 증거의 구멍이고, 요구사항이
 * "전원 차단 후에도 유지"다. 카드 태그는 초당 몇 건 수준이라 fsync 비용을 감당할 수 있다.
 */
static const char *PRAGMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=FULL;"
    "PRAGMA foreign_keys=ON;";

/* 준비된 문 하나를 돌려 정수 하나를 읽는다 (정의는 아래) */
int events_repair_index(AcuEvents *ev);
static long long scalar_with(AcuEvents *ev, sqlite3_stmt *st, long long def);

static void switch_to_memory(AcuEvents *ev, const char *why)
{
    char line[320];
    snprintf(line, sizeof(line),
             "이벤트 저장: %s -> **메모리 전용 모드**로 계속한다 (재부팅하면 사라진다, 최대 %d건)",
             why, EVENTS_MEMORY_CAP);
    log_msg(line);

    if (ev->db)
    {
        sqlite3_finalize(ev->st_append);
        sqlite3_finalize(ev->st_fetch);
        sqlite3_finalize(ev->st_ack);
        sqlite3_finalize(ev->st_min);
        sqlite3_finalize(ev->st_max);
        sqlite3_finalize(ev->st_sent);
        ev->st_append = ev->st_fetch = ev->st_ack = NULL;
        ev->st_min = ev->st_max = ev->st_sent = NULL;
        sqlite3_close(ev->db);
        ev->db = NULL;
    }
}

/* 준비된 문 하나를 만든다. 실패하면 0을 돌려준다 (호출자가 메모리 모드로 떨어뜨린다) */
static int prepare_one(AcuEvents *ev, sqlite3_stmt **out, const char *sql)
{
    if (sqlite3_prepare_v2(ev->db, sql, -1, out, NULL) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: SQL 준비 실패 (%s)", sqlite3_errmsg(ev->db));
        log_msg(line);
        return 0;
    }
    return 1;
}

AcuEvents *events_open(const char *path, long long capacity)
{
    AcuEvents *ev = calloc(1, sizeof(AcuEvents));
    if (!ev)
    {
        return NULL;
    }
    ev->capacity = (capacity > 0) ? capacity : EVENTS_DEFAULT_CAPACITY;
    ev->mem_next_seq = 1;
    ev->next_trim_check = ev->capacity + EVENTS_TRIM_SLACK;

    if (!path || path[0] == '\0')
    {
        switch_to_memory(ev, "저장 경로가 비어 있다");
        return ev;
    }

    if (sqlite3_open(path, &ev->db) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: %s 열기 실패 (%s)",
                 path, ev->db ? sqlite3_errmsg(ev->db) : "알 수 없는 오류");
        if (ev->db)
        {
            sqlite3_close(ev->db);
            ev->db = NULL;
        }
        switch_to_memory(ev, line);
        return ev;
    }

    char *err = NULL;
    if (sqlite3_exec(ev->db, PRAGMA_SQL, NULL, NULL, &err) != SQLITE_OK)
    {
        /* PRAGMA 실패는 치명적이지 않다 - 기본 모드로라도 저장하는 편이 낫다 */
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: PRAGMA 설정 실패 (%s) - 기본 모드로 계속", err ? err : "?");
        log_msg(line);
        sqlite3_free(err);
        err = NULL;
    }

    if (sqlite3_exec(ev->db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: 스키마 생성 실패 (%s)", err ? err : "?");
        sqlite3_free(err);
        switch_to_memory(ev, line);
        return ev;
    }

    if (!prepare_one(ev, &ev->st_append,
            "INSERT INTO events(event_code, op_mode, module_addr, reader_addr,"
            "                   door_status, func_code, ts, access_id)"
            " VALUES(?,?,?,?,?,?,?,?)") ||
        !prepare_one(ev, &ev->st_fetch,
            "SELECT seq, event_code, op_mode, module_addr, reader_addr,"
            "       door_status, func_code, ts, access_id"
            "  FROM events"
            " WHERE seq > (SELECT value FROM event_state WHERE key='sent_seq')"
            " ORDER BY seq LIMIT ?") ||
        !prepare_one(ev, &ev->st_ack,
            "UPDATE event_state SET value=? WHERE key='sent_seq' AND value < ?") ||
        /*
         * 건수를 COUNT(*)로 세지 않는다 — 1,000만 건이면 풀스캔이라 카드 이벤트 처리를 멈춘다
         * (실측: 20만 건에 1.22ms `SCAN events`, MIN/MAX는 0.01ms `SEARCH events`).
         * seq는 AUTOINCREMENT이고 삭제는 **항상 앞에서만** 하므로 중간에 구멍이 없다.
         * 따라서 `건수 = MAX - MIN + 1`이 정확하다.
         * MIN과 MAX를 한 문에 같이 넣으면 SQLite가 인덱스 최적화를 포기하므로 **따로 둔다.**
         */
        !prepare_one(ev, &ev->st_min, "SELECT MIN(seq) FROM events") ||
        !prepare_one(ev, &ev->st_max, "SELECT MAX(seq) FROM events") ||
        !prepare_one(ev, &ev->st_sent, "SELECT value FROM event_state WHERE key='sent_seq'"))
    {
        switch_to_memory(ev, "SQL 준비 실패");
        return ev;
    }

    events_repair_index(ev); /* 지난번에 어긋난 채로 꺼졌을 수 있다 */

    long long total = events_total(ev);
    long long pending = events_pending(ev);
    char line[320];
    snprintf(line, sizeof(line),
             "이벤트 저장: %s (WAL) - 보관 %lld건 / 미전송 %lld건, 한도 %lld건",
             path, total, pending, ev->capacity);
    log_msg(line);
    return ev;
}

void events_close(AcuEvents *ev)
{
    if (!ev)
    {
        return;
    }
    if (ev->db)
    {
        sqlite3_finalize(ev->st_append);
        sqlite3_finalize(ev->st_fetch);
        sqlite3_finalize(ev->st_ack);
        sqlite3_finalize(ev->st_min);
        sqlite3_finalize(ev->st_max);
        sqlite3_finalize(ev->st_sent);
        sqlite3_close(ev->db);
    }
    free(ev);
}

int events_is_memory_only(const AcuEvents *ev)
{
    return (!ev || !ev->db) ? 1 : 0;
}

/* 메모리 링에 한 건 넣는다 (가득 차면 가장 오래된 것을 버린다) */
static void mem_push(AcuEvents *ev, const AcuEvent *e)
{
    if (ev->mem_count == EVENTS_MEMORY_CAP)
    {
        ev->mem_head = (ev->mem_head + 1) % EVENTS_MEMORY_CAP;
        ev->mem_count--;
    }
    int idx = (ev->mem_head + ev->mem_count) % EVENTS_MEMORY_CAP;
    ev->mem[idx] = *e;
    ev->mem[idx].seq = ev->mem_next_seq++;
    ev->mem_count++;
}

/*
 * 한도를 넘긴 만큼 오래된 것부터 지운다.
 *
 * **미전송분도 지운다** - 한도까지 찼다는 것은 상위 시스템이 아주 오래 못 가져갔다는 뜻이고,
 * 그때 새 이벤트를 버리면 "지금 무슨 일이 일어나는지"를 잃는다. 오래된 쪽을 버리는 편이 낫다.
 * (기존 장비도 저장소가 가득 차면 어떻게 하는지 DM에 실측 요청해 둔 항목이다)
 *
 * `seq < 기준값` 범위 삭제라서 인덱스를 타고 지운 만큼만 건드린다.
 * 예전처럼 `seq IN (SELECT ... ORDER BY seq LIMIT n)` 로 하면 하위 질의가 표를 훑는다.
 */
static void trim_if_needed(AcuEvents *ev)
{
    long long total = events_total(ev);
    if (total <= ev->capacity)
    {
        return;
    }

    long long excess = total - ev->capacity;
    long long min_seq = scalar_with(ev, ev->st_min, 1);
    long long cut = min_seq + excess; /* 이 값보다 작은 seq를 지운다 */

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ev->db, "DELETE FROM events WHERE seq < ?", -1, &st, NULL) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: 삭제 SQL 준비 실패 (%s)", sqlite3_errmsg(ev->db));
        log_msg(line);
        return;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cut);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: 오래된 이벤트 삭제 실패 (%s)", sqlite3_errmsg(ev->db));
        log_msg(line);
        return;
    }

    char line[200];
    snprintf(line, sizeof(line),
             "이벤트 저장: 한도 %lld건을 넘겨 오래된 %lld건 삭제 (seq < %lld)",
             ev->capacity, excess, cut);
    log_msg(line);
}

int events_append(AcuEvents *ev, const AcuEvent *e)
{
    if (!ev || !e)
    {
        return -1;
    }
    if (!ev->db)
    {
        mem_push(ev, e);
        return 0;
    }

    /*
     * **쓰기 직전에** 읽기 위치를 검사한다.
     * 가져갈 때(fetch) 검사하면 이미 늦다 — 어긋난 동안 들어온 이벤트까지 "끝까지 읽음"으로
     * 덮어써 **잃는다**(시험으로 확인했다). 쓰기 전에 되돌리면 이번에 쓰는 이벤트는 반드시
     * 미전송으로 남는다. 인덱스 조회 두 번이라 비용은 무시할 만하다.
     */
    events_repair_index(ev);

    sqlite3_reset(ev->st_append);
    sqlite3_clear_bindings(ev->st_append);
    sqlite3_bind_int64(ev->st_append, 1, (sqlite3_int64)e->event_code);
    sqlite3_bind_int(ev->st_append, 2, e->op_mode);
    sqlite3_bind_int(ev->st_append, 3, e->module_addr);
    sqlite3_bind_int(ev->st_append, 4, e->reader_addr);
    sqlite3_bind_int(ev->st_append, 5, e->door_status);
    sqlite3_bind_int(ev->st_append, 6, e->func_code);
    sqlite3_bind_int64(ev->st_append, 7, (sqlite3_int64)e->ts);
    sqlite3_bind_blob(ev->st_append, 8, e->id, (int)sizeof(e->id), SQLITE_TRANSIENT);

    if (sqlite3_step(ev->st_append) != SQLITE_DONE)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: 적재 실패 (%s)", sqlite3_errmsg(ev->db));
        log_msg(line);
        sqlite3_reset(ev->st_append);
        return -1;
    }
    sqlite3_reset(ev->st_append);

    /* 링 삭제는 한도를 한참 넘겼을 때만 본다 (매 건마다 COUNT를 세지 않기 위함) */
    if (sqlite3_last_insert_rowid(ev->db) >= ev->next_trim_check)
    {
        trim_if_needed(ev);
        ev->next_trim_check = sqlite3_last_insert_rowid(ev->db) + EVENTS_TRIM_SLACK;
    }
    return 0;
}

int events_fetch(AcuEvents *ev, AcuEvent *out, int max)
{
    if (!ev || !out || max <= 0)
    {
        return -1;
    }

    if (!ev->db)
    {
        int n = (ev->mem_count < max) ? ev->mem_count : max;
        for (int i = 0; i < n; i++)
        {
            out[i] = ev->mem[(ev->mem_head + i) % EVENTS_MEMORY_CAP];
        }
        return n;
    }

    sqlite3_reset(ev->st_fetch);
    sqlite3_clear_bindings(ev->st_fetch);
    sqlite3_bind_int(ev->st_fetch, 1, max);

    int n = 0;
    int rc;
    while (n < max && (rc = sqlite3_step(ev->st_fetch)) == SQLITE_ROW)
    {
        AcuEvent *e = &out[n];
        e->seq         = sqlite3_column_int64(ev->st_fetch, 0);
        e->event_code  = (uint32_t)sqlite3_column_int64(ev->st_fetch, 1);
        e->op_mode     = (uint8_t)sqlite3_column_int(ev->st_fetch, 2);
        e->module_addr = (uint8_t)sqlite3_column_int(ev->st_fetch, 3);
        e->reader_addr = (uint8_t)sqlite3_column_int(ev->st_fetch, 4);
        e->door_status = (uint8_t)sqlite3_column_int(ev->st_fetch, 5);
        e->func_code   = (uint8_t)sqlite3_column_int(ev->st_fetch, 6);
        e->ts          = (time_t)sqlite3_column_int64(ev->st_fetch, 7);

        memset(e->id, 0, sizeof(e->id));
        const void *blob = sqlite3_column_blob(ev->st_fetch, 8);
        int blob_len = sqlite3_column_bytes(ev->st_fetch, 8);
        if (blob && blob_len > 0)
        {
            memcpy(e->id, blob, (size_t)blob_len < sizeof(e->id) ? (size_t)blob_len : sizeof(e->id));
        }
        n++;
    }
    sqlite3_reset(ev->st_fetch);

    /*
     * 보낼 것이 없다고 나올 때만 위치를 검사한다 (매번 하면 폴링마다 질의가 늘어난다).
     * "없다"가 진짜 없는 것인지, 위치가 앞서 나가 못 보는 것인지를 여기서 가른다.
     */
    if (n == 0 && events_repair_index(ev) == 1)
    {
        return events_fetch(ev, out, max);
    }
    return n;
}

int events_ack(AcuEvents *ev, int64_t upto_seq)
{
    if (!ev || upto_seq <= 0)
    {
        return -1;
    }

    if (!ev->db)
    {
        /* 메모리 모드: 보낸 만큼 앞에서 덜어낸다 */
        while (ev->mem_count > 0 && ev->mem[ev->mem_head].seq <= upto_seq)
        {
            ev->mem_head = (ev->mem_head + 1) % EVENTS_MEMORY_CAP;
            ev->mem_count--;
        }
        return 0;
    }

    sqlite3_reset(ev->st_ack);
    sqlite3_clear_bindings(ev->st_ack);
    sqlite3_bind_int64(ev->st_ack, 1, (sqlite3_int64)upto_seq);
    sqlite3_bind_int64(ev->st_ack, 2, (sqlite3_int64)upto_seq);

    if (sqlite3_step(ev->st_ack) != SQLITE_DONE)
    {
        char line[400];
        snprintf(line, sizeof(line), "이벤트 저장: 전송 위치 갱신 실패 (%s)", sqlite3_errmsg(ev->db));
        log_msg(line);
        sqlite3_reset(ev->st_ack);
        return -1;
    }
    sqlite3_reset(ev->st_ack);
    return 0;
}

/*
 * 준비된 문 하나를 돌려 정수 하나를 읽는다.
 * 행이 없거나 값이 NULL이면(빈 표의 MIN/MAX) def를 돌려준다.
 */
static long long scalar_with(AcuEvents *ev, sqlite3_stmt *st, long long def)
{
    if (!ev->db || !st)
    {
        return def;
    }
    sqlite3_reset(st);
    long long v = def;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    {
        v = (long long)sqlite3_column_int64(st, 0);
    }
    sqlite3_reset(st);
    return v;
}

long long events_pending(const AcuEvents *ev)
{
    if (!ev)
    {
        return -1;
    }
    AcuEvents *m = (AcuEvents *)ev; /* 준비된 문을 돌리려면 const를 벗어야 한다 */
    if (!m->db)
    {
        return m->mem_count;
    }

    long long max_seq = scalar_with(m, m->st_max, 0);
    if (max_seq == 0)
    {
        return 0; /* 비어 있다 */
    }
    long long min_seq = scalar_with(m, m->st_min, 1);
    long long sent = scalar_with(m, m->st_sent, 0);

    /*
     * 링 삭제로 아직 안 보낸 것까지 지워졌으면 전송 위치가 남아 있는 것보다 뒤처져 있다.
     * 그때는 "남아 있는 것 전부가 미전송"이다.
     */
    if (sent < min_seq - 1)
    {
        sent = min_seq - 1;
    }
    long long pending = max_seq - sent;
    return (pending > 0) ? pending : 0;
}

long long events_total(const AcuEvents *ev)
{
    if (!ev)
    {
        return -1;
    }
    AcuEvents *m = (AcuEvents *)ev;
    if (!m->db)
    {
        return m->mem_count;
    }

    long long max_seq = scalar_with(m, m->st_max, 0);
    if (max_seq == 0)
    {
        return 0;
    }
    long long min_seq = scalar_with(m, m->st_min, 1);
    return max_seq - min_seq + 1; /* 앞에서만 지우므로 중간에 구멍이 없다 */
}


/* ---- 읽기 위치 관리 (EventIndexChange / EventReset, 고장 복구) ---- */

/* 전송 위치를 값 그대로 쓴다 (앞으로만 가는 events_ack와 달리 뒤로도 간다) */
static int set_sent_seq(AcuEvents *ev, long long value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ev->db, "UPDATE event_state SET value=? WHERE key='sent_seq'",
                           -1, &st, NULL) != SQLITE_OK)
    {
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)value);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int events_repair_index(AcuEvents *ev)
{
    if (!ev || !ev->db)
    {
        return 0; /* 메모리 모드에는 따로 된 위치가 없다 */
    }

    long long max_seq = scalar_with(ev, ev->st_max, 0);
    long long sent = scalar_with(ev, ev->st_sent, 0);

    if (sent <= max_seq)
    {
        return 0;
    }

    char line[200];
    snprintf(line, sizeof(line),
             "이벤트 저장: 읽기 위치(%lld)가 쓰기 위치(%lld)보다 앞서 있다 - 되돌린다 "
             "(현장 SSC-324에서 이벤트가 끊겼던 그 상태)", sent, max_seq);
    log_msg(line);

    return (set_sent_seq(ev, max_seq) == 0) ? 1 : -1;
}

long long events_rewind_to_time(AcuEvents *ev, time_t from)
{
    if (!ev || !ev->db)
    {
        return -1;
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ev->db, "SELECT MIN(seq) FROM events WHERE ts >= ?",
                           -1, &st, NULL) != SQLITE_OK)
    {
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)from);

    long long first = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    {
        first = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    long long max_seq = scalar_with(ev, ev->st_max, 0);
    long long target = (first > 0) ? first - 1 : max_seq; /* 그 뒤 이벤트가 없으면 끝으로 */

    if (set_sent_seq(ev, target) != 0)
    {
        return -1;
    }

    long long again = max_seq - target;
    char line[200];
    snprintf(line, sizeof(line), "이벤트 저장: 읽기 위치를 되돌렸다 - %lld건을 다시 보낸다", again);
    log_msg(line);
    return again;
}

int events_rewind_to_seq(AcuEvents *ev, int64_t first_seq)
{
    if (!ev || !ev->db || first_seq <= 0)
    {
        return -1;
    }
    long long sent = scalar_with(ev, ev->st_sent, 0);
    long long target = (long long)first_seq - 1;
    if (target >= sent)
    {
        return 0; /* 이미 그 앞에 있다 - 앞으로 밀지는 않는다 */
    }
    return set_sent_seq(ev, target);
}

int events_mark_all_sent(AcuEvents *ev)
{
    if (!ev)
    {
        return -1;
    }
    if (!ev->db)
    {
        ev->mem_head = 0;
        ev->mem_count = 0;
        return 0;
    }
    long long max_seq = scalar_with(ev, ev->st_max, 0);
    if (set_sent_seq(ev, max_seq) != 0)
    {
        return -1;
    }
    log_msg("이벤트 저장: 모든 이벤트를 보낸 것으로 표시했다 (지우지는 않는다)");
    return 0;
}
