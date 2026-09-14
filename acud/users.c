#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#include "users.h"
#include "log.h"

/*
 * 전체 다운로드 중 몇 명마다 커밋할지.
 *
 * 1명씩 커밋하면 보드에서 13.6ms/명 -> 26만 명에 한 시간이다(2026-09-11 실측).
 * 반대로 끝까지 한 트랜잭션으로 열어 두면 WAL이 명단 전체만큼 커진다.
 * 수천 건마다 끊는 것이 둘 사이의 타협이다 (HARDWARE.md "설계 원칙").
 */
#define USERS_LOAD_COMMIT_EVERY 5000

/* 받는 동안 쓰는 표 이름. 다 받으면 이 표가 운영 표 자리로 들어간다 */
#define T_USERS_LOAD "users_load"
#define T_CARDS_LOAD "user_cards_load"

struct AcuUsers {
    sqlite3 *db;

    sqlite3_stmt *st_by_card;
    sqlite3_stmt *st_by_id;
    sqlite3_stmt *st_count;
    sqlite3_stmt *st_put;
    sqlite3_stmt *st_put_card;
    sqlite3_stmt *st_delete;
    sqlite3_stmt *st_card_by_user;
    sqlite3_stmt *st_read_after;
    sqlite3_stmt *st_read_at;

    /* 전체 다운로드용 */
    sqlite3_stmt *st_load_put;
    sqlite3_stmt *st_load_put_card;
    int       loading;        /* 1이면 받는 중 */
    int       in_txn;         /* 묶음 트랜잭션이 열려 있는지 */
    long long load_rows;      /* 이번 묶음에 넣은 수 (커밋 주기 판단) */
    long long load_total;     /* 이번 다운로드로 받은 총 사용자 수 */
};

/*
 * 컬럼은 규약 필드를 그대로 옮긴 것이다. 값을 가공해 저장하지 않는다 —
 * DM이 되돌려 받을 때(`UserReceive`) 보낸 것과 같아야 한다.
 *
 * user_id가 BLOB 8byte인 이유: 규약의 원시값이 그렇다. 문자열로 바꿔 두면 표현이 갈린다
 * (예전 `cards` 표는 hex 문자열이었다).
 */
#define USERS_COLUMNS \
    "user_id, general_group, revision_id, access_option, level," \
    " validation_code, timezone_code, expired_date, prox_type, prox_wiegand," \
    " bio_type, bio_type_sub, template_count, access_password, canteen_code," \
    " lcd_name, restrict_type, restrict_limit_type, restrict_limit_count, group_codes"

#define USERS_PLACEHOLDERS "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?"

/*
 * 같은 컬럼들을 표 이름으로 한정한 것. `user_cards`와 JOIN 할 때 `user_id`가 양쪽에 있어
 * 한정하지 않으면 "ambiguous column name"으로 준비가 실패한다.
 * **위 USERS_COLUMNS와 순서가 같아야 한다** — 읽는 쪽(read_record)이 순서를 전제한다.
 */
#define USERS_COLUMNS_Q \
    "users.user_id, users.general_group, users.revision_id, users.access_option, users.level," \
    " users.validation_code, users.timezone_code, users.expired_date, users.prox_type," \
    " users.prox_wiegand, users.bio_type, users.bio_type_sub, users.template_count," \
    " users.access_password, users.canteen_code, users.lcd_name, users.restrict_type," \
    " users.restrict_limit_type, users.restrict_limit_count, users.group_codes"

#define USERS_TABLE_DDL(name) \
    "CREATE TABLE IF NOT EXISTS " name " (" \
    "    user_id              BLOB PRIMARY KEY," \
    "    general_group        INTEGER NOT NULL DEFAULT 0," \
    "    revision_id          INTEGER NOT NULL DEFAULT 0," \
    "    access_option        INTEGER NOT NULL DEFAULT 0," \
    "    level                INTEGER NOT NULL DEFAULT 0," \
    "    validation_code      INTEGER NOT NULL DEFAULT 0," \
    "    timezone_code        INTEGER NOT NULL DEFAULT 0," \
    "    expired_date         INTEGER NOT NULL DEFAULT 16777215," \
    "    prox_type            INTEGER NOT NULL DEFAULT 0," \
    "    prox_wiegand         INTEGER NOT NULL DEFAULT 0," \
    "    bio_type             INTEGER NOT NULL DEFAULT 0," \
    "    bio_type_sub         INTEGER NOT NULL DEFAULT 0," \
    "    template_count       INTEGER NOT NULL DEFAULT 0," \
    "    access_password      INTEGER NOT NULL DEFAULT 0," \
    "    canteen_code         INTEGER NOT NULL DEFAULT 0," \
    "    lcd_name             BLOB," \
    "    restrict_type        INTEGER NOT NULL DEFAULT 0," \
    "    restrict_limit_type  INTEGER NOT NULL DEFAULT 0," \
    "    restrict_limit_count INTEGER NOT NULL DEFAULT 0," \
    "    group_codes          BLOB" \
    ");"

/*
 * 인증 수단 표. 지금은 카드뿐이지만 지문·안면도 같은 모양(수단값 -> 사용자 ID)으로 붙는다.
 * card_id가 PRIMARY KEY라 판정 조회가 인덱스 한 번이다.
 */
#define CARDS_TABLE_DDL(name) \
    "CREATE TABLE IF NOT EXISTS " name " (" \
    "    card_id  BLOB PRIMARY KEY," \
    "    user_id  BLOB NOT NULL," \
    "    prox_raw BLOB" \
    ");"

static const char *SCHEMA_SQL =
    USERS_TABLE_DDL("users")
    CARDS_TABLE_DDL("user_cards")
    "CREATE INDEX IF NOT EXISTS idx_user_cards_user ON user_cards(user_id);";

/*
 * WAL + synchronous=NORMAL.
 *
 * 이벤트(events.c)는 FULL을 쓴다 — 전원이 끊겨 마지막 몇 건을 잃으면 증거의 구멍이 되기 때문이다.
 * 사용자 명단은 성격이 다르다. **원본이 DM에 있고 다시 내려받을 수 있다.** 반대로 26만 명을
 * 받는 동안 커밋마다 fsync를 걸면 다운로드가 그만큼 길어진다. 그래서 NORMAL로 둔다.
 * (전원이 끊겨도 DB가 깨지지는 않는다. 마지막 묶음이 날아갈 뿐이고, 받다 만 명단은 어차피 버린다)
 */
static const char *PRAGMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;";

static int exec_sql(AcuUsers *u, const char *sql, const char *what)
{
    char *err = NULL;
    if (sqlite3_exec(u->db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "사용자 저장: %s 실패 (%s)", what, err ? err : "?");
        log_msg(line);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int prepare_one(AcuUsers *u, sqlite3_stmt **out, const char *sql)
{
    if (sqlite3_prepare_v2(u->db, sql, -1, out, NULL) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "사용자 저장: SQL 준비 실패 (%s)", sqlite3_errmsg(u->db));
        log_msg(line);
        return 0;
    }
    return 1;
}

AcuUsers *users_open(const char *path)
{
    if (!path || path[0] == '\0')
    {
        log_msg("사용자 저장: 경로가 비어 있다");
        return NULL;
    }

    AcuUsers *u = calloc(1, sizeof(AcuUsers));
    if (!u)
    {
        return NULL;
    }

    if (sqlite3_open(path, &u->db) != SQLITE_OK)
    {
        char line[400];
        snprintf(line, sizeof(line), "사용자 저장: %s 열기 실패 (%s)",
                 path, u->db ? sqlite3_errmsg(u->db) : "알 수 없는 오류");
        log_msg(line);
        sqlite3_close(u->db);
        free(u);
        return NULL;
    }

    sqlite3_exec(u->db, PRAGMA_SQL, NULL, NULL, NULL); /* 실패해도 기본 모드로 돈다 */

    if (exec_sql(u, SCHEMA_SQL, "스키마 생성") != 0)
    {
        users_close(u);
        return NULL;
    }

    /* 지난번 다운로드가 끊긴 채 남아 있으면 버린다 — 반쪽 명단을 물려받지 않는다 */
    exec_sql(u, "DROP TABLE IF EXISTS " T_USERS_LOAD ";"
                "DROP TABLE IF EXISTS " T_CARDS_LOAD ";", "받다 만 표 정리");

    if (!prepare_one(u, &u->st_by_card,
            "SELECT " USERS_COLUMNS " FROM users"
            " WHERE user_id = (SELECT user_id FROM user_cards WHERE card_id = ?)") ||
        !prepare_one(u, &u->st_by_id,
            "SELECT " USERS_COLUMNS " FROM users WHERE user_id = ?") ||
        !prepare_one(u, &u->st_count, "SELECT COUNT(*) FROM users") ||
        !prepare_one(u, &u->st_put,
            "INSERT OR REPLACE INTO users(" USERS_COLUMNS ") VALUES(" USERS_PLACEHOLDERS ")") ||
        !prepare_one(u, &u->st_put_card,
            "INSERT OR REPLACE INTO user_cards(card_id, user_id, prox_raw) VALUES(?,?,?)") ||
        !prepare_one(u, &u->st_delete, "DELETE FROM users WHERE user_id = ?") ||
        !prepare_one(u, &u->st_card_by_user,
            "SELECT card_id, user_id, prox_raw FROM user_cards WHERE user_id = ? LIMIT 1") ||
        !prepare_one(u, &u->st_read_after,
            "SELECT " USERS_COLUMNS_Q ", c.card_id, c.prox_raw"
            "  FROM users LEFT JOIN user_cards c ON c.user_id = users.user_id"
            " WHERE users.user_id > ? ORDER BY users.user_id LIMIT ?") ||
        !prepare_one(u, &u->st_read_at,
            "SELECT " USERS_COLUMNS_Q ", c.card_id, c.prox_raw"
            "  FROM users LEFT JOIN user_cards c ON c.user_id = users.user_id"
            " ORDER BY users.user_id LIMIT ? OFFSET ?"))
    {
        users_close(u);
        return NULL;
    }

    char line[320];
    snprintf(line, sizeof(line), "사용자 저장: %s (WAL) - 등록 %lld명", path, users_count(u));
    log_msg(line);
    return u;
}

void users_close(AcuUsers *u)
{
    if (!u)
    {
        return;
    }
    sqlite3_finalize(u->st_by_card);
    sqlite3_finalize(u->st_by_id);
    sqlite3_finalize(u->st_count);
    sqlite3_finalize(u->st_put);
    sqlite3_finalize(u->st_put_card);
    sqlite3_finalize(u->st_delete);
    sqlite3_finalize(u->st_card_by_user);
    sqlite3_finalize(u->st_read_after);
    sqlite3_finalize(u->st_read_at);
    sqlite3_finalize(u->st_load_put);
    sqlite3_finalize(u->st_load_put_card);
    if (u->db)
    {
        if (u->in_txn)
        {
            sqlite3_exec(u->db, "ROLLBACK", NULL, NULL, NULL);
        }
        sqlite3_close(u->db);
    }
    free(u);
}

/* 준비된 INSERT 문 하나에 레코드를 묶는다 (운영 표·받는 표가 컬럼이 같아 함께 쓴다) */
static void bind_record(sqlite3_stmt *st, const AcuUserRecord *r)
{
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    int i = 1;
    sqlite3_bind_blob(st, i++, r->user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, i++, r->general_group);
    sqlite3_bind_int(st, i++, r->revision_id);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)r->access_option);
    sqlite3_bind_int(st, i++, r->level);
    sqlite3_bind_int(st, i++, r->validation_code);
    sqlite3_bind_int(st, i++, r->timezone_code);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)r->expired_date);
    sqlite3_bind_int(st, i++, r->prox_type);
    sqlite3_bind_int(st, i++, r->prox_wiegand);
    sqlite3_bind_int(st, i++, r->bio_type);
    sqlite3_bind_int(st, i++, r->bio_type_sub);
    sqlite3_bind_int(st, i++, r->template_count);
    sqlite3_bind_int(st, i++, r->access_password);
    sqlite3_bind_int(st, i++, r->canteen_code);
    sqlite3_bind_blob(st, i++, r->lcd_name, ACU_USER_NAME_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, i++, r->restrict_type);
    sqlite3_bind_int(st, i++, r->restrict_limit_type);
    sqlite3_bind_int(st, i++, r->restrict_limit_count);
    sqlite3_bind_blob(st, i++, r->group_codes, ACU_USER_GROUP_LEN, SQLITE_TRANSIENT);
}

/* BLOB 컬럼 하나를 고정 길이 버퍼로 읽는다 (짧으면 나머지는 0) */
static void column_blob_fixed(sqlite3_stmt *st, int col, uint8_t *out, size_t out_len)
{
    memset(out, 0, out_len);
    const void *blob = sqlite3_column_blob(st, col);
    int n = sqlite3_column_bytes(st, col);
    if (blob && n > 0)
    {
        memcpy(out, blob, ((size_t)n < out_len) ? (size_t)n : out_len);
    }
}

static void read_record(sqlite3_stmt *st, AcuUserRecord *r)
{
    memset(r, 0, sizeof(*r));
    int i = 0;
    column_blob_fixed(st, i++, r->user_id, ACU_USER_ID_LEN);
    r->general_group   = (uint8_t)sqlite3_column_int(st, i++);
    r->revision_id     = (uint16_t)sqlite3_column_int(st, i++);
    r->access_option   = (uint32_t)sqlite3_column_int64(st, i++);
    r->level           = (uint8_t)sqlite3_column_int(st, i++);
    r->validation_code = (uint16_t)sqlite3_column_int(st, i++);
    r->timezone_code   = (uint16_t)sqlite3_column_int(st, i++);
    r->expired_date    = (uint32_t)sqlite3_column_int64(st, i++);
    r->prox_type       = (uint8_t)sqlite3_column_int(st, i++);
    r->prox_wiegand    = (uint8_t)sqlite3_column_int(st, i++);
    r->bio_type        = (uint8_t)sqlite3_column_int(st, i++);
    r->bio_type_sub    = (uint8_t)sqlite3_column_int(st, i++);
    r->template_count  = (uint8_t)sqlite3_column_int(st, i++);
    r->access_password = (uint16_t)sqlite3_column_int(st, i++);
    r->canteen_code    = (uint8_t)sqlite3_column_int(st, i++);
    column_blob_fixed(st, i++, r->lcd_name, ACU_USER_NAME_LEN);
    r->restrict_type        = (uint8_t)sqlite3_column_int(st, i++);
    r->restrict_limit_type  = (uint8_t)sqlite3_column_int(st, i++);
    r->restrict_limit_count = (uint8_t)sqlite3_column_int(st, i++);
    column_blob_fixed(st, i++, r->group_codes, ACU_USER_GROUP_LEN);
}

/* 키 하나를 묶어 한 행을 읽어 온다 */
static int lookup_with(AcuUsers *u, sqlite3_stmt *st, const uint8_t *key, int key_len,
                       AcuUserRecord *out)
{
    if (!u || !key || !out)
    {
        return -1;
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_blob(st, 1, key, key_len, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
    {
        read_record(st, out);
        sqlite3_reset(st);
        return 1;
    }
    sqlite3_reset(st);
    if (rc == SQLITE_DONE)
    {
        return 0;
    }

    char line[400];
    snprintf(line, sizeof(line), "사용자 저장: 조회 실패 (%s)", sqlite3_errmsg(u->db));
    log_msg(line);
    return -1;
}

int users_lookup_by_card(AcuUsers *u, const uint8_t card_id[ACU_USER_CARD_LEN],
                         AcuUserRecord *out)
{
    return lookup_with(u, u ? u->st_by_card : NULL, card_id, ACU_USER_CARD_LEN, out);
}

int users_lookup_by_id(AcuUsers *u, const uint8_t user_id[ACU_USER_ID_LEN],
                       AcuUserRecord *out)
{
    return lookup_with(u, u ? u->st_by_id : NULL, user_id, ACU_USER_ID_LEN, out);
}

int users_lookup_card_by_user(AcuUsers *u, const uint8_t user_id[ACU_USER_ID_LEN],
                              AcuUserCard *out)
{
    if (!u || !u->st_card_by_user || !user_id || !out)
    {
        return -1;
    }
    sqlite3_reset(u->st_card_by_user);
    sqlite3_clear_bindings(u->st_card_by_user);
    sqlite3_bind_blob(u->st_card_by_user, 1, user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);

    int rc = sqlite3_step(u->st_card_by_user);
    if (rc == SQLITE_ROW)
    {
        memset(out, 0, sizeof(*out));
        column_blob_fixed(u->st_card_by_user, 0, out->card_id, ACU_USER_CARD_LEN);
        column_blob_fixed(u->st_card_by_user, 1, out->user_id, ACU_USER_ID_LEN);
        column_blob_fixed(u->st_card_by_user, 2, out->prox_raw, ACU_USER_PROX_LEN);
        sqlite3_reset(u->st_card_by_user);
        return 1;
    }
    sqlite3_reset(u->st_card_by_user);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* 준비된 조회문 하나를 돌려 recs/cards를 채운다 (컬럼 배치가 같다) */
static int read_rows(sqlite3_stmt *st, int max,
                     AcuUserRecord *recs, AcuUserCard *cards)
{
    int n = 0;
    int rc;
    while (n < max && (rc = sqlite3_step(st)) == SQLITE_ROW)
    {
        read_record(st, &recs[n]);

        memset(&cards[n], 0, sizeof(cards[n]));
        memcpy(cards[n].user_id, recs[n].user_id, ACU_USER_ID_LEN);
        /* LEFT JOIN이라 카드가 없으면 NULL이 온다 - 그때는 0으로 둔다 */
        int base = 20; /* USERS_COLUMNS의 컬럼 수 */
        column_blob_fixed(st, base,     cards[n].card_id, ACU_USER_CARD_LEN);
        column_blob_fixed(st, base + 1, cards[n].prox_raw, ACU_USER_PROX_LEN);
        n++;
    }
    sqlite3_reset(st);
    return n;
}

int users_read_after(AcuUsers *u, const uint8_t *after_id, int max,
                     AcuUserRecord *recs, AcuUserCard *cards)
{
    if (!u || !u->st_read_after || !recs || !cards || max <= 0)
    {
        return -1;
    }
    static const uint8_t ZERO_ID[ACU_USER_ID_LEN] = {0};

    sqlite3_reset(u->st_read_after);
    sqlite3_clear_bindings(u->st_read_after);
    sqlite3_bind_blob(u->st_read_after, 1, after_id ? after_id : ZERO_ID,
                      ACU_USER_ID_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_int(u->st_read_after, 2, max);
    return read_rows(u->st_read_after, max, recs, cards);
}

int users_read_at(AcuUsers *u, long long offset, int max,
                  AcuUserRecord *recs, AcuUserCard *cards)
{
    if (!u || !u->st_read_at || !recs || !cards || max <= 0 || offset < 0)
    {
        return -1;
    }
    sqlite3_reset(u->st_read_at);
    sqlite3_clear_bindings(u->st_read_at);
    sqlite3_bind_int(u->st_read_at, 1, max);
    sqlite3_bind_int64(u->st_read_at, 2, (sqlite3_int64)offset);
    return read_rows(u->st_read_at, max, recs, cards);
}

long long users_count(const AcuUsers *u)
{
    if (!u || !u->db || !u->st_count)
    {
        return -1;
    }
    AcuUsers *m = (AcuUsers *)u;
    sqlite3_reset(m->st_count);
    long long v = -1;
    if (sqlite3_step(m->st_count) == SQLITE_ROW)
    {
        v = (long long)sqlite3_column_int64(m->st_count, 0);
    }
    sqlite3_reset(m->st_count);
    return v;
}

/* 준비된 문 하나를 실행하고 리셋한다 */
static int step_done(AcuUsers *u, sqlite3_stmt *st, const char *what)
{
    if (sqlite3_step(st) != SQLITE_DONE)
    {
        char line[400];
        snprintf(line, sizeof(line), "사용자 저장: %s 실패 (%s)", what, sqlite3_errmsg(u->db));
        log_msg(line);
        sqlite3_reset(st);
        return -1;
    }
    sqlite3_reset(st);
    return 0;
}

int users_put(AcuUsers *u, const AcuUserRecord *rec)
{
    if (!u || !rec)
    {
        return -1;
    }
    bind_record(u->st_put, rec);
    return step_done(u, u->st_put, "사용자 저장");
}

int users_put_card(AcuUsers *u, const AcuUserCard *card)
{
    if (!u || !card)
    {
        return -1;
    }
    sqlite3_reset(u->st_put_card);
    sqlite3_clear_bindings(u->st_put_card);
    sqlite3_bind_blob(u->st_put_card, 1, card->card_id, ACU_USER_CARD_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_blob(u->st_put_card, 2, card->user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_blob(u->st_put_card, 3, card->prox_raw, ACU_USER_PROX_LEN, SQLITE_TRANSIENT);
    return step_done(u, u->st_put_card, "카드 저장");
}

int users_delete(AcuUsers *u, const uint8_t user_id[ACU_USER_ID_LEN])
{
    if (!u || !user_id)
    {
        return -1;
    }

    /* 사용자를 지우면 그 사람의 인증 수단도 함께 지운다 (남으면 주인 없는 카드가 통과한다) */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(u->db, "DELETE FROM user_cards WHERE user_id = ?", -1, &st, NULL)
        == SQLITE_OK)
    {
        sqlite3_bind_blob(st, 1, user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    sqlite3_reset(u->st_delete);
    sqlite3_clear_bindings(u->st_delete);
    sqlite3_bind_blob(u->st_delete, 1, user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);
    return step_done(u, u->st_delete, "사용자 삭제");
}

/* ---- 전체 다운로드 ---- */

int users_load_in_progress(const AcuUsers *u)
{
    return (u && u->loading) ? 1 : 0;
}

/* 묶음 트랜잭션을 열고 닫는다 */
static int txn_begin(AcuUsers *u)
{
    if (u->in_txn)
    {
        return 0;
    }
    if (exec_sql(u, "BEGIN", "트랜잭션 시작") != 0)
    {
        return -1;
    }
    u->in_txn = 1;
    u->load_rows = 0;
    return 0;
}

static int txn_commit(AcuUsers *u)
{
    if (!u->in_txn)
    {
        return 0;
    }
    u->in_txn = 0;
    return exec_sql(u, "COMMIT", "트랜잭션 커밋");
}

int users_load_begin(AcuUsers *u)
{
    if (!u)
    {
        return -1;
    }
    if (u->loading)
    {
        log_msg("사용자 저장: 이미 전체 다운로드를 받는 중이다 - 앞의 것을 버리고 다시 시작한다");
        users_load_abort(u);
    }

    if (exec_sql(u,
            "DROP TABLE IF EXISTS " T_USERS_LOAD ";"
            "DROP TABLE IF EXISTS " T_CARDS_LOAD ";"
            USERS_TABLE_DDL(T_USERS_LOAD)
            CARDS_TABLE_DDL(T_CARDS_LOAD),
            "받는 표 생성") != 0)
    {
        return -1;
    }

    if (!prepare_one(u, &u->st_load_put,
            "INSERT OR REPLACE INTO " T_USERS_LOAD "(" USERS_COLUMNS ")"
            " VALUES(" USERS_PLACEHOLDERS ")") ||
        !prepare_one(u, &u->st_load_put_card,
            "INSERT OR REPLACE INTO " T_CARDS_LOAD "(card_id, user_id, prox_raw) VALUES(?,?,?)"))
    {
        return -1;
    }

    u->loading = 1;
    u->load_total = 0;
    if (txn_begin(u) != 0)
    {
        u->loading = 0;
        return -1;
    }
    log_msg("사용자 저장: 전체 다운로드 시작 (새 표에 받는다 - 그동안 기존 명단으로 판정한다)");
    return 0;
}

/* 묶음이 찼으면 커밋하고 다음 묶음을 연다 */
static int rotate_txn_if_needed(AcuUsers *u)
{
    if (u->load_rows < USERS_LOAD_COMMIT_EVERY)
    {
        return 0;
    }
    if (txn_commit(u) != 0)
    {
        return -1;
    }
    return txn_begin(u);
}

int users_load_put(AcuUsers *u, const AcuUserRecord *rec)
{
    if (!u || !u->loading || !rec)
    {
        return -1;
    }
    bind_record(u->st_load_put, rec);
    if (step_done(u, u->st_load_put, "사용자 적재") != 0)
    {
        return -1;
    }
    u->load_rows++;
    u->load_total++;
    return rotate_txn_if_needed(u);
}

int users_load_put_card(AcuUsers *u, const AcuUserCard *card)
{
    if (!u || !u->loading || !card)
    {
        return -1;
    }
    sqlite3_reset(u->st_load_put_card);
    sqlite3_clear_bindings(u->st_load_put_card);
    sqlite3_bind_blob(u->st_load_put_card, 1, card->card_id, ACU_USER_CARD_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_blob(u->st_load_put_card, 2, card->user_id, ACU_USER_ID_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_blob(u->st_load_put_card, 3, card->prox_raw, ACU_USER_PROX_LEN, SQLITE_TRANSIENT);
    if (step_done(u, u->st_load_put_card, "카드 적재") != 0)
    {
        return -1;
    }
    u->load_rows++;
    return rotate_txn_if_needed(u);
}

void users_load_abort(AcuUsers *u)
{
    if (!u)
    {
        return;
    }
    if (u->in_txn)
    {
        sqlite3_exec(u->db, "ROLLBACK", NULL, NULL, NULL);
        u->in_txn = 0;
    }
    sqlite3_finalize(u->st_load_put);
    sqlite3_finalize(u->st_load_put_card);
    u->st_load_put = NULL;
    u->st_load_put_card = NULL;

    exec_sql(u, "DROP TABLE IF EXISTS " T_USERS_LOAD ";"
                "DROP TABLE IF EXISTS " T_CARDS_LOAD ";", "받던 표 버리기");
    if (u->loading)
    {
        log_msg("사용자 저장: 전체 다운로드를 중단하고 받던 것을 버렸다 (기존 명단은 그대로)");
    }
    u->loading = 0;
    u->load_total = 0;
}

int users_load_commit(AcuUsers *u)
{
    if (!u || !u->loading)
    {
        return -1;
    }

    if (txn_commit(u) != 0)
    {
        users_load_abort(u);
        return -1;
    }

    /*
     * 교체 — 한 트랜잭션 안에서 옛 표를 버리고 받은 표를 그 자리에 넣는다.
     * 중간에 전원이 끊기면 통째로 되돌아가므로 **반쪽 명단은 생기지 않는다.**
     * 준비된 문들이 옛 표를 붙들고 있으면 DROP이 막히므로 먼저 정리하고 다시 준비한다.
     */
    sqlite3_finalize(u->st_by_card);  u->st_by_card = NULL;
    sqlite3_finalize(u->st_by_id);    u->st_by_id = NULL;
    sqlite3_finalize(u->st_count);    u->st_count = NULL;
    sqlite3_finalize(u->st_put);      u->st_put = NULL;
    sqlite3_finalize(u->st_put_card); u->st_put_card = NULL;
    sqlite3_finalize(u->st_delete);   u->st_delete = NULL;
    sqlite3_finalize(u->st_card_by_user); u->st_card_by_user = NULL;
    sqlite3_finalize(u->st_read_after);    u->st_read_after = NULL;
    sqlite3_finalize(u->st_read_at);       u->st_read_at = NULL;
    sqlite3_finalize(u->st_load_put);      u->st_load_put = NULL;
    sqlite3_finalize(u->st_load_put_card); u->st_load_put_card = NULL;

    int rc = exec_sql(u,
        "BEGIN;"
        "DROP TABLE IF EXISTS users;"
        "DROP TABLE IF EXISTS user_cards;"
        "ALTER TABLE " T_USERS_LOAD " RENAME TO users;"
        "ALTER TABLE " T_CARDS_LOAD " RENAME TO user_cards;"
        "CREATE INDEX IF NOT EXISTS idx_user_cards_user ON user_cards(user_id);"
        "COMMIT;", "명단 교체");

    long long total = u->load_total;
    u->loading = 0;
    u->load_total = 0;

    if (rc != 0)
    {
        sqlite3_exec(u->db, "ROLLBACK", NULL, NULL, NULL);
    }

    /* 조회용 문을 다시 준비한다 (교체 성공이든 실패든 운영 표를 다시 봐야 한다) */
    if (!prepare_one(u, &u->st_by_card,
            "SELECT " USERS_COLUMNS " FROM users"
            " WHERE user_id = (SELECT user_id FROM user_cards WHERE card_id = ?)") ||
        !prepare_one(u, &u->st_by_id,
            "SELECT " USERS_COLUMNS " FROM users WHERE user_id = ?") ||
        !prepare_one(u, &u->st_count, "SELECT COUNT(*) FROM users") ||
        !prepare_one(u, &u->st_put,
            "INSERT OR REPLACE INTO users(" USERS_COLUMNS ") VALUES(" USERS_PLACEHOLDERS ")") ||
        !prepare_one(u, &u->st_put_card,
            "INSERT OR REPLACE INTO user_cards(card_id, user_id, prox_raw) VALUES(?,?,?)") ||
        !prepare_one(u, &u->st_delete, "DELETE FROM users WHERE user_id = ?") ||
        !prepare_one(u, &u->st_card_by_user,
            "SELECT card_id, user_id, prox_raw FROM user_cards WHERE user_id = ? LIMIT 1") ||
        !prepare_one(u, &u->st_read_after,
            "SELECT " USERS_COLUMNS_Q ", c.card_id, c.prox_raw"
            "  FROM users LEFT JOIN user_cards c ON c.user_id = users.user_id"
            " WHERE users.user_id > ? ORDER BY users.user_id LIMIT ?") ||
        !prepare_one(u, &u->st_read_at,
            "SELECT " USERS_COLUMNS_Q ", c.card_id, c.prox_raw"
            "  FROM users LEFT JOIN user_cards c ON c.user_id = users.user_id"
            " ORDER BY users.user_id LIMIT ? OFFSET ?"))
    {
        log_msg("사용자 저장: 교체 뒤 SQL을 다시 준비하지 못했다 - 재시작이 필요하다");
        return -1;
    }

    if (rc != 0)
    {
        return -1;
    }

    char line[200];
    snprintf(line, sizeof(line), "사용자 저장: 전체 다운로드 완료 - %lld명으로 교체", total);
    log_msg(line);
    return 0;
}

/*
 * 테스트용 더미 사용자 5명. 판정 경로를 모두 밟게 짠 것이고, 카드값·사용자 ID는
 * 예전 `cards` 표의 hex 문자열을 **원시 8byte로** 옮긴 것이다 (같은 카드로 계속 시험할 수 있게).
 *
 *   1. 활성 + 제한없음                     -> 허용
 *   2. Enable 꺼짐                         -> 거부 (비활성)
 *   3. 활성 + 유효기간 그룹 1 (2020년 만료) -> 거부 (유효기간)
 *   4. 활성 + 유효기간 2 · 시간대 1         -> 허용
 *   5. 활성 + 시간대 2 (요일 전부 미선택)   -> 거부 (시간대)
 */
void users_seed_dummy(AcuUsers *u)
{
    static const struct {
        const char *card_hex;
        uint8_t user_no;
        int enabled;
        uint16_t validation_code;
        uint16_t timezone_code;
    } SEED[] = {
        { "04A1B2C3D4E5F600", 1, 1, 0, 0 },
        { "AABBCCDD11223300", 2, 0, 0, 0 },
        { "1122334455667700", 3, 1, 1, 0 },
        { "2233445566778800", 4, 1, 2, 1 },
        { "3344556677889900", 5, 1, 0, 2 },
    };

    if (!u || users_count(u) > 0)
    {
        return; /* 이미 명단이 있으면 건드리지 않는다 */
    }

    for (size_t k = 0; k < sizeof(SEED) / sizeof(SEED[0]); k++)
    {
        AcuUserRecord r;
        AcuUserCard c;
        memset(&r, 0, sizeof(r));
        memset(&c, 0, sizeof(c));

        r.user_id[ACU_USER_ID_LEN - 1] = SEED[k].user_no; /* 0000000000000001 .. 05 */
        r.access_option = SEED[k].enabled ? ACU_USER_OPT_ENABLE : 0;
        r.level = 1;
        r.validation_code = SEED[k].validation_code;
        r.timezone_code = SEED[k].timezone_code;
        r.expired_date = 0xFFFFFF;
        r.revision_id = 1;
        memset(r.lcd_name, ' ', sizeof(r.lcd_name));

        memcpy(c.user_id, r.user_id, ACU_USER_ID_LEN);
        for (int b = 0; b < ACU_USER_CARD_LEN; b++)
        {
            unsigned int v = 0;
            sscanf(SEED[k].card_hex + b * 2, "%2x", &v);
            c.card_id[b] = (uint8_t)v;
        }

        if (users_put(u, &r) != 0 || users_put_card(u, &c) != 0)
        {
            log_msg("사용자 저장: 더미 사용자 넣기 실패");
            return;
        }
    }
    log_msg("사용자 저장: 테스트용 더미 사용자 5명 등록 (mock HAL 카드와 짝)");
}
