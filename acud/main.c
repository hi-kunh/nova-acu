/* c11 엄격 모드에서 POSIX 함수(sigaction, localtime_r)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "log.h"
#include "db.h"
#include "access.h"
#include "hal.h"
#include "config.h"
#include "net.h"
#include "protocol.h"

/*
 * ACU 데몬 (5단계: 네트워크 통신부 TCP/IDTi 프로토콜 V2 추가)
 * - 무한 루프로 상시 동작
 * - SIGTERM/SIGINT 를 받으면 깔끔하게 종료
 * - SIGHUP 을 받으면 config.json을 다시 읽어 재시작 없이 설정을 반영한다
 *   (db_path가 바뀌면 DB를 다시 열고, door_open_seconds는 바로 다음 판정부터 적용,
 *   tcp_port가 바뀌면 네트워크 서버도 재시작 없이 새 포트로 재개설)
 * - 카드 입력/도어 릴레이/센서는 모두 hal.h 인터페이스로만 접근한다.
 *   실제 GPIO/Wiegand 구현체(6단계)가 없는 지금은 hal_mock.c(더미 카드ID 순회)를 사용한다.
 * - 시작 시 PID 파일을 남긴다 (4단계 웹 설정 인터페이스가 SIGHUP을 보낼 대상을 알기 위함)
 * - 출입 판정 결과는 net.h를 통해 상위 시스템(PC)에 IDTi Event Log(History)로 보고한다.
 *   네트워크 초기화가 실패해도(포트 사용 중 등) 출입 판정 자체는 계속 동작한다(fail-safe).
 */

#define CONFIG_PATH "config.json"
#define PID_PATH    "acud.pid"
#define CARD_POLL_INTERVAL_MS 2000 /* 카드 리더 조회 주기 (네트워크 처리량과 무관하게 항상 유지) */

/* a - b를 밀리초로 반환한다 (CLOCK_MONOTONIC 기준) */
static long timespec_diff_ms(struct timespec a, struct timespec b)
{
    return (a.tv_sec - b.tv_sec) * 1000L + (a.tv_nsec - b.tv_nsec) / 1000000L;
}

static void timespec_add_ms(struct timespec *ts, long ms)
{
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec  += 1;
    }
}

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

    FILE *pidf = fopen(PID_PATH, "w");
    if (pidf)
    {
        fprintf(pidf, "%d\n", (int)getpid());
        fclose(pidf);
    }
    else
    {
        log_msg("PID 파일 생성 실패 (웹 설정 인터페이스의 리로드 신호 전송이 안 될 수 있음)");
    }

    AcuConfig cfg;
    config_set_defaults(&cfg);
    config_load(CONFIG_PATH, &cfg); /* config.json이 없거나 잘못돼도 기본값으로 계속 진행 */

    if (hal_init() != 0)
    {
        log_msg("HAL 초기화 실패 -> 종료");
        return 1;
    }

    sqlite3 *db = db_open(cfg.db_path);
    if (!db)
    {
        log_msg("DB 초기화 실패 -> 종료");
        hal_shutdown();
        return 1;
    }
    db_seed_dummy_data(db);

    AcuNet *net = net_init(cfg.tcp_port); /* 실패해도 net=NULL로 계속 진행 (출입 판정은 네트워크 없이도 동작) */

    struct timespec next_card_check;
    clock_gettime(CLOCK_MONOTONIC, &next_card_check);

    /* 메인 루프: 종료 시그널이 올 때까지 계속 돈다 */
    while (g_running)
    {
        if (g_reload)
        {
            g_reload = 0;
            log_msg("설정 리로드 요청 감지 -> config.json 다시 읽는 중");

            AcuConfig new_cfg = cfg;
            if (config_load(CONFIG_PATH, &new_cfg) == 0)
            {
                if (strcmp(new_cfg.db_path, cfg.db_path) != 0)
                {
                    sqlite3 *new_db = db_open(new_cfg.db_path);
                    if (new_db)
                    {
                        db_close(db);
                        db = new_db;
                        db_seed_dummy_data(db);
                        log_msg("DB 경로 변경 적용됨 (재시작 없이 전환)");
                    }
                    else
                    {
                        log_msg("새 DB 경로 열기 실패 -> 기존 DB 유지");
                        snprintf(new_cfg.db_path, sizeof(new_cfg.db_path), "%s", cfg.db_path);
                    }
                }
                if (new_cfg.tcp_port != cfg.tcp_port)
                {
                    AcuNet *new_net = net_init(new_cfg.tcp_port);
                    if (new_net)
                    {
                        net_shutdown(net);
                        net = new_net;
                        log_msg("네트워크 포트 변경 적용됨 (재시작 없이 전환)");
                    }
                    else
                    {
                        log_msg("새 네트워크 포트 열기 실패 -> 기존 포트 유지");
                        new_cfg.tcp_port = cfg.tcp_port;
                    }
                }
                cfg = new_cfg;
                log_msg("설정 리로드 완료");
            }
            else
            {
                log_msg("설정 리로드 실패 -> 기존 설정 유지");
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = timespec_diff_ms(next_card_check, now);

        if (remaining_ms <= 0)
        {
            /*
             * 카드 리더 조회는 네트워크 트래픽량과 무관하게 항상 일정 주기로 실행돼야 한다.
             * (net_poll은 상위 시스템 요청이 몰리면 즉시 반환하므로, 그 경우에도 카드 조회
             * 주기가 무너지지 않도록 별도 스케줄로 관리한다)
             */
            int door_status = (hal_read_sensor(HAL_SENSOR_DOOR_CONTACT) == HAL_SENSOR_ACTIVE)
                              ? IDTI_DOOR_STATUS_OPEN : IDTI_DOOR_STATUS_CLOSED;
            net_set_door_status(net, door_status);

            char card_id[17];
            int has_card = hal_read_card(card_id, sizeof(card_id));

            if (has_card > 0)
            {
                CardRecord record;
                AccessResult result = access_judge(db, card_id, &record);
                access_log_result(card_id, result);

                const char *id_hex = (result == ACCESS_GRANTED) ? record.user_id : card_id;
                net_push_event(net, result, id_hex, door_status);

                if (result == ACCESS_GRANTED)
                {
                    hal_open_door(cfg.door_open_seconds);
                }
            }
            else if (has_card < 0)
            {
                log_msg("카드 리더 조회 오류");
            }

            timespec_add_ms(&next_card_check, CARD_POLL_INTERVAL_MS);
            remaining_ms = CARD_POLL_INTERVAL_MS;
        }

        if (net)
        {
            net_poll(net, (int)remaining_ms); /* 다음 카드 조회 시각까지 상위 시스템 접속/요청 처리 */
        }
        else
        {
            struct timespec sleep_ts = { remaining_ms / 1000, (remaining_ms % 1000) * 1000000L };
            nanosleep(&sleep_ts, NULL);
        }
    }

    net_shutdown(net);
    db_close(db);
    hal_shutdown();
    remove(PID_PATH);
    log_msg("ACU 데몬 정상 종료");
    return 0;
}
