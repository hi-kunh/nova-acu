/*
 * events.db 적재·조회 성능과 링 삭제를 재는 도구.
 *
 * 왜 필요한가 — 보관 한도가 1,000만 건이라 "그 규모에서도 카드 태그가 늦지 않는가"를
 * 말이 아니라 숫자로 확인해야 한다. mock FIFO로는 한 번에 16장(큐 한계)밖에 못 넣어
 * 실제 규모를 못 만든다. 이 도구는 events.c를 직접 부른다.
 *
 * 빌드: make -C acud bench     실행: ./acud/events_bench <경로> <건수> [한도] [묶음]
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "../acud/events.h"
#include "../acud/log.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "사용법: %s <events.db 경로> <적재 건수> [보관 한도] [묶음 크기]\n"
                "        %s <events.db 경로> seed <건수>\n"
                "  예) %s /tmp/ev.db 100000 10000 500\n"
                "      %s /tmp/ev.db seed 10000000   # 대규모 상태를 빨리 만든다\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    /*
     * seed 모드 — 1,000만 건짜리 저장소를 만들어 두고 "그 규모에서 적재가 느려지지 않는가"를
     * 재기 위한 것이다. 정상 경로(events_append)는 이벤트마다 fsync 하므로 1,000만 건이면
     * eMMC에서 대여섯 시간이 걸린다. 여기서는 **한 트랜잭션에 몰아서** 넣는다 - 시험 전용이고
     * 제품 코드(events.c)는 건드리지 않는다.
     */
    if (strcmp(argv[2], "seed") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "seed 건수를 지정하라\n");
            return 1;
        }
        long long n = atoll(argv[3]);

        log_open("");
        AcuEvents *seed_ev = events_open(argv[1], 0); /* 스키마를 만들게 한 번 연다 */
        events_close(seed_ev);

        sqlite3 *db = NULL;
        if (sqlite3_open(argv[1], &db) != SQLITE_OK)
        {
            fprintf(stderr, "열기 실패\n");
            return 1;
        }
        sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;", NULL, NULL, NULL);

        char sql[512];
        snprintf(sql, sizeof(sql),
                 "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<%lld)"
                 " INSERT INTO events(event_code, op_mode, module_addr, reader_addr,"
                 "                    door_status, func_code, ts, access_id)"
                 " SELECT 16843010, 2, 1, 1, 2, 255, strftime('%%s','now'),"
                 "        randomblob(8) FROM n", n);

        double t = now_sec();
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
        double took = now_sec() - t;
        if (rc != SQLITE_OK)
        {
            fprintf(stderr, "seed 실패: %s\n", err ? err : "?");
            sqlite3_free(err);
            sqlite3_close(db);
            return 1;
        }
        printf("seed %lld건  %.1f초\n", n, took);
        sqlite3_close(db);
        log_close();
        return 0;
    }

    const char *path = argv[1];
    long long count = atoll(argv[2]);
    long long capacity = (argc > 3) ? atoll(argv[3]) : 0;
    int batch = (argc > 4) ? atoi(argv[4]) : 200;

    log_open(""); /* stdout */

    AcuEvents *ev = events_open(path, capacity);
    if (!ev)
    {
        fprintf(stderr, "저장소를 열지 못했다\n");
        return 1;
    }
    printf("모드: %s\n", events_is_memory_only(ev) ? "메모리 전용" : "SQLite(WAL)");

    /* --- 적재 --- */
    AcuEvent e;
    memset(&e, 0, sizeof(e));
    e.event_code = 0x01010102;
    e.op_mode = 0x02;
    e.module_addr = 1;
    e.reader_addr = 1;
    e.door_status = 2;
    e.func_code = 0xFF;

    double t0 = now_sec();
    for (long long i = 0; i < count; i++)
    {
        e.ts = time(NULL);
        for (int b = 0; b < 8; b++)
        {
            e.id[b] = (uint8_t)((i >> (b * 8)) & 0xFF);
        }
        if (events_append(ev, &e) != 0)
        {
            fprintf(stderr, "적재 실패 (%lld번째)\n", i);
            break;
        }
    }
    double t_append = now_sec() - t0;

    printf("\n적재  %lld건  %.2f초  (초당 %.0f건, 건당 %.3fms)\n",
           count, t_append, count / t_append, t_append * 1000.0 / (double)count);
    printf("보관  %lld건 / 미전송 %lld건\n", events_total(ev), events_pending(ev));

    /* --- 조회(전송 묶음) --- */
    AcuEvent *out = calloc((size_t)batch, sizeof(AcuEvent));
    if (!out)
    {
        events_close(ev);
        return 1;
    }

    t0 = now_sec();
    long long fetched = 0;
    int rounds = 0;
    for (;;)
    {
        int n = events_fetch(ev, out, batch);
        if (n <= 0)
        {
            break;
        }
        fetched += n;
        rounds++;
        if (events_ack(ev, out[n - 1].seq) != 0)
        {
            break;
        }
    }
    double t_fetch = now_sec() - t0;

    printf("조회  %lld건  %.2f초  (%d묶음, 묶음당 %.2fms, 초당 %.0f건)\n",
           fetched, t_fetch, rounds,
           rounds ? t_fetch * 1000.0 / rounds : 0.0,
           t_fetch > 0 ? fetched / t_fetch : 0.0);
    printf("남은  미전송 %lld건 / 보관 %lld건\n", events_pending(ev), events_total(ev));

    free(out);
    events_close(ev);
    log_close();
    return 0;
}
