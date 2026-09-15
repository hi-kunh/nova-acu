/*
 * 링 삭제(되감김) 경계에서 읽기 위치가 어긋나지 않는지 확인한다.
 * DM 9/14 회신 6절 ③: "링버퍼가 한 바퀴 돌 때 인덱스가 어긋나지 않게, 경계를 꼭 시험해 주십시오"
 *
 *   [1] 한도 1,000에 2,500건 -> 링 삭제가 돌고, 미전송 = 보관 전부
 *       (링 삭제는 한도를 1,000건(EVENTS_TRIM_SLACK) 넘길 때마다 한 번에 지운다 —
 *        매 건마다 지우면 카드 처리가 늦어진다. 그래서 보관 수는 잠시 한도+1,000까지 올라간다)
 *   [2] 절반 보내고 다시 1,500건 -> 보낸 위치가 지워진 구간에 들어가도 미전송 = 보관 전부
 *   [3] 전부 보냄 표시 -> 0,  지정 시각부터 되돌림 -> 보관 전부
 *   [4] 읽기 위치를 앞으로 밀어 두고 한 건 쓰기 -> 그 한 건은 미전송으로 남는다
 *
 * 빌드·실행: make -C acud wraptest && ./acud/events_wrap_test
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "../acud/events.h"
#include "../acud/log.h"

static int fails = 0;
static void want(const char *what, long long got, long long exp)
{
    if (got == exp) printf("  ✓ %s: %lld\n", what, got);
    else { printf("  ✗ %s: %lld (기대 %lld)\n", what, got, exp); fails++; }
}

static void put(AcuEvents *ev, int n)
{
    AcuEvent e;
    memset(&e, 0, sizeof(e));
    e.event_code = 0x01010102;
    for (int i = 0; i < n; i++)
    {
        e.ts = time(NULL);
        events_append(ev, &e);
    }
}

/* 최대 n건 보내고 ack */
static int send_some(AcuEvents *ev, int n)
{
    static AcuEvent buf[500];
    int sent = 0;
    while (sent < n)
    {
        int want_n = (n - sent < 500) ? n - sent : 500;
        int got = events_fetch(ev, buf, want_n);
        if (got <= 0) break;
        events_ack(ev, buf[got - 1].seq);
        sent += got;
    }
    return sent;
}

int main(void)
{
    const char *path = "/tmp/events_wrap_test.db";
    remove(path);
    char wal[64], shm[64];
    snprintf(wal, sizeof(wal), "%s-wal", path); remove(wal);
    snprintf(shm, sizeof(shm), "%s-shm", path); remove(shm);

    log_open("/dev/null");
    AcuEvents *ev = events_open(path, 1000);

    printf("[1] 한도 1,000에 2,500건 (링 삭제가 돈다)\n");
    put(ev, 2500);
    long long total = events_total(ev);
    want("보관이 한도+1,000 안쪽", total <= 2000, 1);
    want("링 삭제가 돌았다", total < 2500, 1);
    want("미전송 = 보관 전부", events_pending(ev), total);

    printf("[2] 600건 보내고 다시 1,500건 (보낸 위치가 지워진 구간으로 들어간다)\n");
    want("보낸 건수", send_some(ev, 600), 600);
    put(ev, 1500);
    total = events_total(ev);
    want("미전송 = 보관 전부", events_pending(ev), total);
    want("다 받으면 보관 전부", send_some(ev, 5000), total);
    want("남은 미전송", events_pending(ev), 0);

    printf("[3] 전부 보냄 표시 / 지정 시각부터 되돌림\n");
    put(ev, 10);
    want("쓰고 난 미전송", events_pending(ev), 10);
    want("전부 보냄 표시", events_mark_all_sent(ev), 0);
    want("미전송", events_pending(ev), 0);
    total = events_total(ev);
    want("되돌린 건수 = 보관 전부", events_rewind_to_time(ev, 0), total);
    want("미전송 = 보관 전부", events_pending(ev), total);
    send_some(ev, 5000);

    printf("[4] 읽기 위치가 앞서 있을 때 쓰기\n");
    {
        sqlite3 *db = NULL;
        sqlite3_open(path, &db);
        sqlite3_exec(db, "UPDATE event_state SET value = value + 5000 WHERE key='sent_seq'", 0, 0, 0);
        sqlite3_close(db);
    }
    put(ev, 1);
    want("그 한 건은 미전송", events_pending(ev), 1);
    want("가져와진다", send_some(ev, 10), 1);

    events_close(ev);
    printf("\n%s\n", fails ? "실패 있음" : "전부 통과");
    return fails ? 1 : 0;
}
