/* c11 엄격 모드에서 POSIX 함수(sigaction, localtime_r)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "log.h"
#include "db.h"
#include "access.h"

/*
 * ACU 데몬 (1단계: SQLite3 + 출입 판정 로직)
 * - 무한 루프로 상시 동작
 * - SIGTERM/SIGINT 를 받으면 깔끔하게 종료
 * - SIGHUP 을 받으면 "설정 리로드" 신호로 처리 (3단계에서 실제 구현 예정)
 * - 실제 카드 리더기가 없는 6단계 이전까지는, 더미 카드 ID 목록을 순회하며
 *   SQLite3 조회 -> 출입 판정 로직을 검증한다.
 */

#define DB_PATH "acud.db"

/* 테스트용 더미 카드ID 목록 (실제 리더기 연동은 6단계에서 진행) */
static const char *DUMMY_CARD_IDS[] = {
    "04A1B2C3D4E5F600", /* 활성 + 유효기간/시간대 제한없음 -> 허용 */
    "AABBCCDD11223300", /* 비활성화된 카드 -> 거부 */
    "FFFFFFFFFFFFFFFF", /* 미등록 카드 -> 거부 */
    "1122334455667700", /* 활성 + 유효기간(2020년) 만료 -> 거부 */
    "2233445566778800", /* 활성 + 유효기간/시간대 그룹 모두 통과 -> 허용 */
    "3344556677889900", /* 활성 + 시간대 그룹과 요일 불일치 -> 거부 */
};
#define DUMMY_CARD_COUNT (sizeof(DUMMY_CARD_IDS) / sizeof(DUMMY_CARD_IDS[0]))

/* 시그널 핸들러에서는 이 플래그만 건드린다 (핸들러 안에서 복잡한 일을 하면 안 됨) */
static volatile sig_atomic_t g_running = 1;  /* 0이 되면 메인 루프 종료 */
static volatile sig_atomic_t g_reload  = 0;  /* 1이 되면 설정 다시 읽기 */

/* 종료 시그널 핸들러 (SIGTERM, SIGINT) */
static void on_stop(int sig)
{
    (void)sig;
    g_running = 0;
}

/* 리로드 시그널 핸들러 (SIGHUP) */
static void on_reload(int sig)
{
    (void)sig;
    g_reload = 1;
}

int main(void)
{
    /* 시그널 핸들러 등록 */
    struct sigaction sa_stop = {0};
    sa_stop.sa_handler = on_stop;
    sigaction(SIGTERM, &sa_stop, NULL);
    sigaction(SIGINT,  &sa_stop, NULL);

    struct sigaction sa_reload = {0};
    sa_reload.sa_handler = on_reload;
    sigaction(SIGHUP, &sa_reload, NULL);

    log_msg("ACU 데몬 시작");

    sqlite3 *db = db_open(DB_PATH);
    if (!db)
    {
        log_msg("DB 초기화 실패 -> 종료");
        return 1;
    }
    db_seed_dummy_data(db);

    size_t card_idx = 0;

    /* 메인 루프: 종료 시그널이 올 때까지 계속 돈다 */
    while (g_running)
    {
        if (g_reload)
        {
            g_reload = 0;
            log_msg("설정 리로드 요청 감지 (추후 config.json 재적용 예정)");
        }

        /* 실제 리더기(6단계) 연동 전까지: 더미 카드ID를 하나씩 순회하며 판정 검증 */
        const char *card_id = DUMMY_CARD_IDS[card_idx];
        card_idx = (card_idx + 1) % DUMMY_CARD_COUNT;

        CardRecord record;
        AccessResult result = access_judge(db, card_id, &record);
        access_log_result(card_id, result);

        sleep(2);  /* 지금은 2초마다 한 건씩 처리한다 */
    }

    db_close(db);
    log_msg("ACU 데몬 정상 종료");
    return 0;
}
