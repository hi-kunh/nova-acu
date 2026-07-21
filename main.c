/* c11 엄격 모드에서 POSIX 함수(sigaction, localtime_r)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

/*
 * ACU 데몬 뼈대 (0단계)
 * - 무한 루프로 상시 동작
 * - SIGTERM/SIGINT 를 받으면 깔끔하게 종료
 * - SIGHUP 을 받으면 "설정 리로드" 신호로 처리 (3단계에서 실제 구현 예정)
 */

/* 시그널 핸들러에서는 이 플래그만 건드린다 (핸들러 안에서 복잡한 일을 하면 안 됨) */
static volatile sig_atomic_t g_running = 1;  /* 0이 되면 메인 루프 종료 */
static volatile sig_atomic_t g_reload  = 0;  /* 1이 되면 설정 다시 읽기 */

/* 종료 시그널 핸들러 (SIGTERM, SIGINT) */
static void on_stop(int sig) {
    (void)sig;
    g_running = 0;
}

/* 리로드 시그널 핸들러 (SIGHUP) */
static void on_reload(int sig) {
    (void)sig;
    g_reload = 1;
}

/* 타임스탬프가 붙은 간단한 로그 출력 */
static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);  /* headless 환경에서 로그가 즉시 보이도록 버퍼를 비운다 */
}

int main(void) {
    /* 시그널 핸들러 등록 */
    struct sigaction sa_stop = {0};
    sa_stop.sa_handler = on_stop;
    sigaction(SIGTERM, &sa_stop, NULL);
    sigaction(SIGINT,  &sa_stop, NULL);

    struct sigaction sa_reload = {0};
    sa_reload.sa_handler = on_reload;
    sigaction(SIGHUP, &sa_reload, NULL);

    log_msg("ACU 데몬 시작");

    /* 메인 루프: 종료 시그널이 올 때까지 계속 돈다 */
    while (g_running) {
        if (g_reload) {
            g_reload = 0;
            log_msg("설정 리로드 요청 감지 (추후 config.json 재적용 예정)");
        }

        /* TODO(1단계): 여기서 카드 ID 입력 확인 -> SQLite3 조회 -> 출입 판정 */
        log_msg("동작 중... (heartbeat)");

        sleep(2);  /* 지금은 2초마다 살아있음을 알린다 */
    }

    log_msg("ACU 데몬 정상 종료");
    return 0;
}
