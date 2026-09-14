/*
 * users.db 등록·조회 성능을 재는 도구.
 *
 * 재는 것 (TODO 9/16 끝 기준: "보드에서 26만 명 등록 시간·조회 시간 측정"):
 *   1) 전체 다운로드 — 새 표에 받아 한 번에 교체하기까지
 *   2) 카드 조회 1건 (판정 경로)
 *   3) 개별 등록 1건 (DM의 UserSend)
 *
 * 빌드: make -C acud usersbench   실행: ./acud/users_bench <경로> <인원>
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../acud/users.h"
#include "../acud/log.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * i번째 사용자의 ID와 카드값을 만든다 (둘 다 8byte).
 *
 * **바이트 순서가 성능을 크게 바꾼다.** 둘 다 BLOB 기본키라 SQLite가 바이트 순으로 비교하는데,
 * 리틀엔디언으로 만들면 앞 바이트가 빠르게 바뀌어 키가 흩어지고 B-tree가 여기저기 쪼개진다.
 * 보드 실측으로 **2배 넘게 느려졌다.**
 *
 *   user_id : **빅엔디언 = 증가 순서**. 규약의 User ID가 그렇고(`3.` 문서 A절: 숫자만,
 *             0x00..0x01 ~ 0x99..0x99), DM도 순서대로 보낸다 -> 뒤에 붙기만 한다
 *   card_id : **흩어지게** 만든다. 카드 번호는 순서와 무관한 실제 값이라 이쪽이 현실에 가깝다
 */
static void make_ids(long long i, uint8_t user_id[8], uint8_t card_id[8])
{
    uint64_t uid = (uint64_t)(i + 1);
    uint64_t cid = (uint64_t)i * 2654435761ULL + 0x5A5A0000ULL; /* 곱셈 해시로 흩뜨린다 */

    for (int b = 0; b < 8; b++)
    {
        user_id[b] = (uint8_t)((uid >> ((7 - b) * 8)) & 0xFF);
        card_id[b] = (uint8_t)((cid >> ((7 - b) * 8)) & 0xFF);
    }
}

static void fill_record(AcuUserRecord *r, long long i)
{
    memset(r, 0, sizeof(*r));
    uint8_t card_id[8];
    make_ids(i, r->user_id, card_id);
    r->access_option = 0x00000001; /* Enable */
    r->level = 31;
    r->expired_date = 0xFFFFFF;
    r->revision_id = 1;
    memset(r->lcd_name, ' ', sizeof(r->lcd_name));
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "사용법: %s <users.db 경로> <인원>\n"
                        "  예) %s /tmp/users.db 260000\n", argv[0], argv[0]);
        return 1;
    }

    const char *path = argv[1];
    long long count = atoll(argv[2]);

    log_open(""); /* stdout */

    AcuUsers *u = users_open(path);
    if (!u)
    {
        fprintf(stderr, "users.db를 열지 못했다\n");
        return 1;
    }

    /* --- 1) 전체 다운로드 --- */
    double t0 = now_sec();
    if (users_load_begin(u) != 0)
    {
        fprintf(stderr, "다운로드 시작 실패\n");
        users_close(u);
        return 1;
    }

    AcuUserRecord rec;
    AcuUserCard card;
    memset(&card, 0, sizeof(card));

    for (long long i = 0; i < count; i++)
    {
        fill_record(&rec, i);
        if (users_load_put(u, &rec) != 0)
        {
            fprintf(stderr, "적재 실패 (%lld명째)\n", i);
            users_load_abort(u);
            users_close(u);
            return 1;
        }
        make_ids(i, card.user_id, card.card_id);
        if (users_load_put_card(u, &card) != 0)
        {
            fprintf(stderr, "카드 적재 실패 (%lld명째)\n", i);
            users_load_abort(u);
            users_close(u);
            return 1;
        }
    }
    double t_put = now_sec() - t0;

    double t1 = now_sec();
    if (users_load_commit(u) != 0)
    {
        fprintf(stderr, "교체 실패\n");
        users_close(u);
        return 1;
    }
    double t_swap = now_sec() - t1;
    double t_total = now_sec() - t0;

    printf("\n== 전체 다운로드 ==\n");
    printf("  받기   %lld명  %.2f초  (명당 %.3fms, 초당 %.0f명)\n",
           count, t_put, t_put * 1000.0 / (double)count, count / t_put);
    printf("  교체            %.3f초  (표를 통째로 바꿔 끼운다)\n", t_swap);
    printf("  합계            %.2f초\n", t_total);
    printf("  등록 결과       %lld명\n", users_count(u));

    /* --- 2) 카드 조회 (판정 경로) --- */
    const int PROBES = 20000;
    AcuUserRecord out;
    uint8_t user_id[8], card_id[8];
    long long found = 0;

    t0 = now_sec();
    for (int p = 0; p < PROBES; p++)
    {
        /* 명단 전체에 고루 흩어지게 (캐시가 한쪽만 덥히지 않도록) */
        long long i = ((long long)p * 7919) % count;
        make_ids(i, user_id, card_id);
        if (users_lookup_by_card(u, card_id, &out) == 1)
        {
            found++;
        }
    }
    double t_probe = now_sec() - t0;

    printf("\n== 카드 조회 (판정 경로) ==\n");
    printf("  %d건  %.3f초  (건당 %.4fms)  찾음 %lld건\n",
           PROBES, t_probe, t_probe * 1000.0 / PROBES, found);

    /* 없는 카드도 같은 비용인지 (미등록 카드 거부 경로) */
    t0 = now_sec();
    for (int p = 0; p < PROBES; p++)
    {
        memset(card_id, 0xEE, sizeof(card_id));
        card_id[0] = (uint8_t)(p & 0xFF);
        card_id[1] = (uint8_t)((p >> 8) & 0xFF);
        users_lookup_by_card(u, card_id, &out);
    }
    double t_miss = now_sec() - t0;
    printf("  미등록 %d건  %.3f초  (건당 %.4fms)\n",
           PROBES, t_miss, t_miss * 1000.0 / PROBES);

    /* --- 3) 개별 등록 (DM의 UserSend) --- */
    const int SINGLES = 200;
    t0 = now_sec();
    for (int p = 0; p < SINGLES; p++)
    {
        fill_record(&rec, count + p);
        users_put(u, &rec);
    }
    double t_single = now_sec() - t0;
    printf("\n== 개별 등록 (UserSend) ==\n");
    printf("  %d명  %.3f초  (명당 %.3fms)\n",
           SINGLES, t_single, t_single * 1000.0 / SINGLES);

    users_close(u);
    log_close();
    return 0;
}
