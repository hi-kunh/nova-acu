/*
 * users.c 동작 확인 — 전체 다운로드의 어려운 부분만 골라 본다.
 *
 *   · 받는 동안에도 **기존 명단으로 판정이 계속되는가** (DM이 26만 명을 1분 넘게 보낸다)
 *   · 중간에 끊기면 **반쪽 명단이 남지 않고 원래대로 돌아가는가**
 *   · 교체 뒤에도 개별 등록·삭제가 되는가 (교체가 준비된 SQL을 무효화한다)
 *
 * 빌드·실행: make -C acud userstest && ./acud/users_test
 */
#include <stdio.h>
#include <string.h>
#include "../acud/users.h"
#include "../acud/log.h"

static int fails = 0;
static void check(const char *what, int got, int want)
{
    if (got != want) { printf("  ✗ %s: %d (기대 %d)\n", what, got, want); fails++; }
    else             { printf("  ✓ %s\n", what); }
}

static void ids(int n, uint8_t uid[8], uint8_t cid[8])
{
    memset(uid, 0, 8); memset(cid, 0, 8);
    uid[0] = (uint8_t)n; cid[0] = (uint8_t)(0x80 + n);
}

int main(void)
{
    log_open("");
    remove("/tmp/utest.db"); remove("/tmp/utest.db-wal"); remove("/tmp/utest.db-shm");

    AcuUsers *u = users_open("/tmp/utest.db");
    if (!u) { printf("열기 실패\n"); return 1; }

    AcuUserRecord r; AcuUserCard c; AcuUserRecord out;
    uint8_t uid[8], cid[8];

    /* 기존 명단: 1번 사용자 */
    memset(&r, 0, sizeof(r)); memset(&c, 0, sizeof(c));
    ids(1, uid, cid);
    memcpy(r.user_id, uid, 8); r.level = 7; r.access_option = 1;
    memcpy(c.user_id, uid, 8); memcpy(c.card_id, cid, 8);
    users_put(u, &r); users_put_card(u, &c);

    printf("\n[1] 기존 명단\n");
    check("1번 카드로 찾힌다", users_lookup_by_card(u, cid, &out), 1);
    check("레벨이 보존된다", out.level, 7);
    check("인원 1명", (int)users_count(u), 1);

    /* 전체 다운로드 시작 — 2번만 담는다 */
    printf("\n[2] 전체 다운로드를 받는 중\n");
    check("다운로드 시작", users_load_begin(u), 0);
    check("받는 중 표시", users_load_in_progress(u), 1);

    memset(&r, 0, sizeof(r)); memset(&c, 0, sizeof(c));
    ids(2, uid, cid);
    memcpy(r.user_id, uid, 8); r.level = 9; r.access_option = 1;
    memcpy(c.user_id, uid, 8); memcpy(c.card_id, cid, 8);
    users_load_put(u, &r); users_load_put_card(u, &c);

    ids(1, uid, cid);
    check("받는 동안에도 1번 카드로 판정된다", users_lookup_by_card(u, cid, &out), 1);
    ids(2, uid, cid);
    check("아직 2번은 안 보인다", users_lookup_by_card(u, cid, &out), 0);

    /* 중단 */
    printf("\n[3] 중단하면 되돌아간다\n");
    users_load_abort(u);
    check("받는 중 아님", users_load_in_progress(u), 0);
    ids(1, uid, cid);
    check("1번 그대로", users_lookup_by_card(u, cid, &out), 1);
    ids(2, uid, cid);
    check("2번 없음", users_lookup_by_card(u, cid, &out), 0);
    check("인원 1명 유지", (int)users_count(u), 1);

    /* 다시 받아 교체 */
    printf("\n[4] 다시 받아 교체\n");
    users_load_begin(u);
    memset(&r, 0, sizeof(r)); memset(&c, 0, sizeof(c));
    ids(2, uid, cid);
    memcpy(r.user_id, uid, 8); r.level = 9; r.access_option = 1;
    memcpy(c.user_id, uid, 8); memcpy(c.card_id, cid, 8);
    users_load_put(u, &r); users_load_put_card(u, &c);
    check("교체", users_load_commit(u), 0);

    ids(2, uid, cid);
    check("2번이 보인다", users_lookup_by_card(u, cid, &out), 1);
    check("레벨 9", out.level, 9);
    ids(1, uid, cid);
    check("1번은 사라졌다", users_lookup_by_card(u, cid, &out), 0);
    check("인원 1명", (int)users_count(u), 1);

    /* 교체 뒤에도 개별 등록·삭제가 된다 (준비된 문 재준비 확인) */
    printf("\n[5] 교체 뒤 개별 변경\n");
    memset(&r, 0, sizeof(r)); memset(&c, 0, sizeof(c));
    ids(3, uid, cid);
    memcpy(r.user_id, uid, 8); r.level = 5; r.access_option = 1;
    memcpy(c.user_id, uid, 8); memcpy(c.card_id, cid, 8);
    check("개별 등록", users_put(u, &r), 0);
    check("카드 등록", users_put_card(u, &c), 0);
    check("3번 찾힌다", users_lookup_by_card(u, cid, &out), 1);
    check("사용자 ID로도 찾힌다", users_lookup_by_id(u, uid, &out), 1);
    check("삭제", users_delete(u, uid), 0);
    check("삭제 뒤 카드도 안 찾힌다", users_lookup_by_card(u, cid, &out), 0);
    check("인원 1명", (int)users_count(u), 1);

    users_close(u);
    printf("\n%s (실패 %d건)\n", fails ? "실패" : "전부 통과", fails);
    log_close();
    return fails ? 1 : 0;
}
