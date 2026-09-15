/* c11 엄격 모드에서 POSIX 함수(sigaction, localtime_r)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/timerfd.h>

#include "log.h"
#include "db.h"
#include "access.h"
#include "hal.h"
#include "config.h"
#include "loop.h"
#include "events.h"
#include "users.h"
#include "devset.h"
#include "userbin.h"
#include "net.h"
#include "discover.h"
#include "protocol.h"

/*
 * ACU 데몬 (5단계: 네트워크 통신부 TCP/IDTi 프로토콜 V2 추가)
 * - 무한 루프로 상시 동작
 * - SIGTERM/SIGINT 를 받으면 깔끔하게 종료
 * - SIGHUP 을 받으면 config.json을 다시 읽어 재시작 없이 설정을 반영한다
 *   (db_path가 바뀌면 DB를 다시 열고, door_open_seconds는 바로 다음 판정부터 적용,
 *   tcp_port가 바뀌면 네트워크 서버도 재시작 없이 새 포트로 재개설)
 * - 카드 입력/도어 릴레이/센서는 모두 hal.h 인터페이스로만 접근한다.
 *   실제 GPIO/Wiegand 구현체(6단계)가 없는 지금은 hal_mock.c(FIFO 주입)를 사용한다.
 * - **이벤트 구동**: select는 loop.c 한 곳에만 있고, 네트워크(리슨·클라이언트) · UDP 탐색 ·
 *   카드 입력 fd가 모두 거기에 등록된다. 예전에는 main이 2초마다 hal_read_card()를 부르고
 *   그 사이를 net_poll()의 select가 채웠는데, 그래서 카드가 최대 2초 늦게 처리됐다.
 *   지금은 카드가 들어오는 즉시 그 fd가 깨어난다.
 * - 시작 시 PID 파일을 남긴다 (4단계 웹 설정 인터페이스가 SIGHUP을 보낼 대상을 알기 위함)
 * - 모든 경로는 cwd에 의존하지 않게 지정할 수 있다. systemd로 띄우면 cwd가 "/"라서
 *   상대경로로는 설정을 못 찾고 DB도 만들지 못한다. config 파일 경로는 -c 옵션으로,
 *   나머지(db/pid/log)는 config.json 안에서 절대경로로 준다.
 * - 출입 판정 결과는 net.h를 통해 상위 시스템(PC)에 IDTi Event Log(History)로 보고한다.
 *   네트워크 초기화가 실패해도(포트 사용 중 등) 출입 판정 자체는 계속 동작한다(fail-safe).
 * - discover.h로 UDP 브로드캐스트 탐색(netmodule 프로토콜)에 응답한다. 상위 PC가 장치 IP를
 *   몰라도 장비를 찾을 수 있게 하는 경로다. 이것도 실패해도 데몬 본체는 계속 돈다.
 */

#define DEFAULT_CONFIG_PATH "config.json" /* -c 로 덮어쓸 수 있다. 데몬으로 띄울 때는 절대경로를 준다 */

/*
 * 점검 타이머 주기(초). 소켓·카드처럼 "무슨 일이 일어나서" 깨어나는 것이 아니라,
 * **아무 일도 없을 때** 확인해야 하는 것들을 여기서 본다:
 *   - 유휴 타임아웃 (조용한 것 자체가 판단 근거라 소켓 이벤트로는 알 수 없다)
 *   - 탐색 응답에 실을 상위 시스템 연결 상태
 * 카드 조회는 여기 들어가지 않는다 - 그것이 예전 2초 지연의 원인이었다.
 */
#define HOUSEKEEPING_INTERVAL_SEC 1

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "사용법: %s [-c CONFIG_PATH]\n"
            "  -c PATH   설정 파일 경로 (기본값: %s)\n"
            "  -h        이 도움말\n"
            "\n"
            "db_path / pid_path / log_path 는 설정 파일 안에서 지정한다.\n",
            argv0, DEFAULT_CONFIG_PATH);
}

/* config의 I/O 구성 값을 net에 넘긴다 (여러 곳에서 같은 일을 하므로 한 곳으로 모았다) */
static void apply_module_layout(AcuNet *net, const AcuConfig *cfg)
{
    AcuModuleLayout layout;
    layout.module_count = cfg->module_count;
    layout.readers = cfg->module_readers;
    layout.inputs = cfg->module_inputs;
    layout.outputs = cfg->module_outputs;
    layout.alarm_fire_inputs = cfg->module_alarm_fire_inputs;
    layout.module_type = cfg->module_type;
    layout.install_type = cfg->module_install_type;
    net_set_module_layout(net, &layout);
}

/*
 * 루프에 등록한 fd들이 깨어날 때 쓰는 묶음.
 * 콜백은 user 포인터 하나만 받으므로 함께 봐야 하는 것을 여기 담아 넘긴다.
 */
typedef struct {
    sqlite3          *db;      /* 설정 — 유효기간·시간대 그룹 */
    AcuUsers         *users;   /* 사용자 명단 */
    AcuNet           *net;
    AcuDiscover      *disc;
    const AcuConfig  *cfg;

    /* 알람·화재 입력의 **직전 상태**. 변화할 때만 이벤트를 올리기 위해 들고 있는다 */
    int alarm_active;
    int fire_active;
} AcuRuntime;

/*
 * 강제 개방 상태가 바뀌면 net이 부른다 (DM Object 206 명령, 화재 정책).
 * net은 상태·DM 보고만 맡고 실제 릴레이는 여기서 HAL로 움직인다.
 */
static void on_force_open_changed(int on, void *user)
{
    (void)user;
    hal_set_force_open(on);
}

static int module_addr_for_rru(int rru); /* 정의는 아래 */

/*
 * RRU 링크가 끊겼을 때 DM에 올린다.
 *
 * **새 코드를 만들 수 없다** — 기존 IDTi 장비 대체라 DM·Platinum이 아는 코드에서 골랐다.
 * `0x20030102` H/W No Response (2026-09-11 Platinum 회신으로 확정).
 * `0x20010102` Comm Halted는 **금지** — DM이 ACU의 TCP 단절에 쓰는 코드다.
 *
 * 주소는 **그 RRU의 첫 모듈, Reader 0** (RRU 단위로 1건 — DM 회신 권고).
 * **복구는 올리지 않는다** (기존 장비가 그렇다).
 *
 * ⚠ 지금은 부르는 곳이 없다. 6단계에서 `PING` 3회 실패(약 6초)나 USB 노드가 사라졌을 때
 * 부른다 — 설계는 `rru/ACU_RRU_USB_프로토콜_설계_초안.md` 6절.
 */
static void report_rru_disconnected(AcuRuntime *rt, int rru)
{
    char line[120];
    snprintf(line, sizeof(line), "RRU %d 단절 -> DM에 H/W No Response 보고", rru);
    log_msg(line);

    net_push_system_event(rt->net, IDTI_EVENT_HW_NO_RESPONSE,
                          module_addr_for_rru(rru), IDTI_EVENT_ADDR_NONE);
}

/*
 * 알람·화재 입력이 바뀌었는지 보고, 바뀌었으면 DM에 이벤트를 올린다.
 *
 * 주소 규칙 (2026-09-11 DM 회신, HARDWARE.md "이벤트 주소"):
 *   알람  Module = 모듈, Reader = 13   (실측 `01 0D`)
 *   화재  반응 장치가 리더면 그 (모듈, 리더), 아니면 (0, 0)
 *         -> 입력 설정(Object 44)을 아직 받지 않으므로 지금은 `(0, 0)`
 *
 * 알람 릴레이는 **ACU가 구동한다** — RRU는 릴레이를 스스로 움직이지 않는다.
 * 어느 출력이 Alarm인지는 출력 설정(Object 45)을 받아야 알 수 있어, 지금은 mock이 흉내만 낸다.
 */
static void check_alarm_fire(AcuRuntime *rt)
{
    int alarm = (hal_read_sensor(HAL_SENSOR_ALARM_INPUT) == HAL_SENSOR_ACTIVE) ? 1 : 0;
    int fire = (hal_read_sensor(HAL_SENSOR_FIRE_INPUT) == HAL_SENSOR_ACTIVE) ? 1 : 0;

    if (alarm != rt->alarm_active)
    {
        rt->alarm_active = alarm;
        net_push_system_event(rt->net,
                              alarm ? IDTI_EVENT_ALARM_DETECTED : IDTI_EVENT_ALARM_RESTORED,
                              IDTI_EVENT_ADDR_FIRST_MODULE, IDTI_EVENT_ADDR_ALARM_READER);
        hal_set_alarm_relays(alarm);
        log_msg(alarm ? "알람 입력 동작 -> DM 보고 + 알람 릴레이 켬"
                      : "알람 입력 복구 -> DM 보고 + 알람 릴레이 끔");
    }

    if (fire != rt->fire_active)
    {
        rt->fire_active = fire;
        net_push_system_event(rt->net,
                              fire ? IDTI_EVENT_FIRE_DETECTED : IDTI_EVENT_FIRE_RESTORED,
                              IDTI_EVENT_ADDR_NONE, IDTI_EVENT_ADDR_NONE);
        hal_set_alarm_relays(fire);
        /*
         * ⚠ **문을 여는 것은 우리가 하지 않는다.** 화재 시 전체 개방은 **DM 정책**이 처리하고,
         * DM이 강제 개방(Object 206) 명령을 내려보낸다 (2026-09-11 실측 4회 확인).
         * 우리는 이벤트만 올린다 — 여기서 문을 같이 열면 DM 정책과 이중으로 동작한다.
         */
        log_msg(fire ? "화재 입력 동작 -> DM 보고 (문 개방은 DM 정책이 명령한다)"
                     : "화재 입력 복구 -> DM 보고");
    }
}

/*
 * db_path가 있는 디렉터리에 name 파일을 둔다.
 * 두 DB를 따로 백업·이관하는 일이 없게 같은 디렉터리에 모은다.
 */
static void path_next_to_db(char *out, size_t out_len, const AcuConfig *cfg, const char *name)
{
    const char *slash = strrchr(cfg->db_path, '/');
    if (slash)
    {
        int dir_len = (int)(slash - cfg->db_path);
        snprintf(out, out_len, "%.*s/%s", dir_len, cfg->db_path, name);
    }
    else
    {
        snprintf(out, out_len, "%s", name);
    }
}

/*
 * events.db 경로를 정한다. 설정이 비어 있으면 **db_path 옆에** 둔다 —
 * 두 DB를 따로 백업·이관하는 일이 없게 같은 디렉터리에 두는 편이 운용에 낫다.
 */
static void resolve_events_path(char *out, size_t out_len, const AcuConfig *cfg)
{
    if (cfg->events_db_path[0] != '\0')
    {
        snprintf(out, out_len, "%s", cfg->events_db_path);
        return;
    }

    path_next_to_db(out, out_len, cfg, "events.db");
}

/* users.db 경로. 설정이 비어 있으면 db_path 옆에 둔다 */
static void resolve_users_path(char *out, size_t out_len, const AcuConfig *cfg)
{
    if (cfg->users_db_path[0] != '\0')
    {
        snprintf(out, out_len, "%s", cfg->users_db_path);
        return;
    }
    path_next_to_db(out, out_len, cfg, "users.db");
}

/* UDP 탐색 소켓이 읽을 수 있을 때 */
static void on_discover_readable(int fd, unsigned events, void *user)
{
    (void)fd; (void)events;
    discover_service((AcuDiscover *)user);
}

/*
 * 카드가 올라온 RRU 번호를 이벤트 주소(모듈 번호)로 바꾼다.
 * **RRU 번호 하나가 모듈 2칸**을 차지하므로 RRU r의 첫 모듈은 2r-1이다
 * (HARDWARE.md "RRU 여러 대 구성"). 리더 번호는 RRU 프레임이 알려 주게 될 값이라
 * 지금은 첫 리더로 둔다 - mock에는 리더 구분이 없다.
 */
static int module_addr_for_rru(int rru)
{
    if (rru < 1 || rru > HAL_RRU_MAX)
    {
        rru = 1;
    }
    return (rru - 1) * 2 + IDTI_EVENT_ADDR_FIRST_MODULE;
}

/*
 * hex 문자열 카드값을 원시 8byte로 바꾼다 (짧으면 뒤를 0으로 채운다).
 * mock HAL이 사람이 읽는 형식으로 주기 때문에 필요한 변환이다.
 */
static void hex_to_card(const char *hex, uint8_t out[ACU_USER_CARD_LEN])
{
    memset(out, 0, ACU_USER_CARD_LEN);
    size_t bytes = strlen(hex) / 2;
    if (bytes > ACU_USER_CARD_LEN)
    {
        bytes = ACU_USER_CARD_LEN;
    }
    for (size_t i = 0; i < bytes; i++)
    {
        unsigned int v = 0;
        sscanf(hex + i * 2, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* 지금 문 상태를 읽어 IDTI_DOOR_STATUS_* 로 돌려준다 */
static int read_door_status(void)
{
    return (hal_read_sensor(HAL_SENSOR_DOOR_CONTACT) == HAL_SENSOR_ACTIVE)
           ? IDTI_DOOR_STATUS_OPEN : IDTI_DOOR_STATUS_CLOSED;
}

/*
 * 카드 입력 fd가 깨어났을 때. 한 번에 여러 장이 들어와 있을 수 있으므로 큐가 빌 때까지 판정한다.
 * 도어 접점도 같은 입력으로 바뀌므로(mock은 FIFO, 실제로는 RRU 프레임) 여기서 함께 갱신한다.
 */
static void on_card_readable(int fd, unsigned events, void *user)
{
    (void)events;
    AcuRuntime *rt = (AcuRuntime *)user;

    hal_service_fd(fd);

    int door_status = read_door_status();
    net_set_door_status(rt->net, door_status);

    /* 알람·화재도 같은 입력으로 들어온다 (mock은 FIFO, 6단계에서는 RRU 프레임) */
    check_alarm_fire(rt);

    /* 링크가 끊긴 RRU가 보고됐으면 DM에 올린다 (mock은 FIFO의 "rru down N") */
    int down_rru;
    while ((down_rru = hal_take_disconnected_rru()) > 0)
    {
        report_rru_disconnected(rt, down_rru);
    }

    for (;;)
    {
        int rru = 1;
        char card_hex[17];
        int has_card = hal_read_card(&rru, card_hex, sizeof(card_hex));
        if (has_card == 0)
        {
            break;
        }
        if (has_card < 0)
        {
            log_msg("카드 리더 조회 오류");
            break;
        }

        /*
         * HAL은 아직 카드값을 hex 문자열로 준다(mock FIFO에 사람이 넣는 형식이라).
         * 저장·이벤트는 규약 원시값을 쓰므로 여기서 8byte로 바꾼다.
         * 6단계에서 RRU가 프레임으로 원시값을 주면 이 변환은 사라진다.
         */
        uint8_t card_id[ACU_USER_CARD_LEN];
        hex_to_card(card_hex, card_id);

        AcuUserRecord user;
        AccessResult result = access_judge(rt->users, rt->db, card_id, &user);
        access_log_result(card_hex, result);

        /* 허용이면 User ID, 거부면 카드값을 싣는다 (IDTi Event Structure의 Access ID 규칙) */
        const uint8_t *access_id = (result == ACCESS_GRANTED) ? user.user_id : card_id;
        net_push_event(rt->net, result, access_id, door_status,
                       module_addr_for_rru(rru), IDTI_EVENT_ADDR_FIRST_READER);

        if (result == ACCESS_GRANTED)
        {
            hal_open_door(rt->cfg->door_open_seconds);
        }
    }
}

/* 점검 타이머가 만료될 때 (HOUSEKEEPING_INTERVAL_SEC 주기) */
static void on_housekeeping(int fd, unsigned events, void *user)
{
    (void)events;
    AcuRuntime *rt = (AcuRuntime *)user;

    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations))
    {
        return; /* 못 읽어도 다음 주기에 다시 온다 */
    }

    net_check_inactivity(rt->net);
    discover_set_connected(rt->disc, net_is_connected(rt->net));
}

/* 카드 입력 fd들을 루프에 등록한다 (6단계에서는 RRU 1~7의 USB fd가 여기로 들어온다) */
static void register_input_fds(AcuLoop *loop, AcuRuntime *rt)
{
    int fds[HAL_INPUT_FD_MAX];
    int n = hal_input_fds(fds);
    for (int i = 0; i < n; i++)
    {
        if (loop_add(loop, fds[i], ACU_LOOP_READ, on_card_readable, rt) != 0)
        {
            log_msg("입력 fd를 루프에 등록하지 못했다 - 그 입력은 처리되지 않는다");
        }
    }
    char line[80];
    snprintf(line, sizeof(line), "입력 fd %d개를 이벤트 루프에 등록 (RRU 최대 %d대)", n, HAL_RRU_MAX);
    log_msg(line);
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

int main(int argc, char **argv)
{
    const char *config_path = DEFAULT_CONFIG_PATH;

    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1)
    {
        switch (opt)
        {
        case 'c':
            config_path = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* 시그널 핸들러 등록 */
    struct sigaction sa_stop = {0};
    sa_stop.sa_handler = on_stop;
    sigaction(SIGTERM, &sa_stop, NULL);
    sigaction(SIGINT,  &sa_stop, NULL);

    struct sigaction sa_reload = {0};
    sa_reload.sa_handler = on_reload;
    sigaction(SIGHUP, &sa_reload, NULL);

    /*
     * SIGPIPE 무시: 상위 시스템이 연결을 끊은 직후 응답을 보내면 기본 동작이 프로세스 종료다.
     * 데몬이 통째로 죽으면 안 되므로 무시하고, send() 실패는 net.c에서 errno로 처리한다.
     */
    struct sigaction sa_ignore = {0};
    sa_ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ignore, NULL);

    /*
     * 로그 대상이 config.json 안에 있으므로 설정을 먼저 읽는다.
     * 그 전에 나는 로그(설정 파싱 실패 등)는 stdout으로 가는데, systemd로 띄우면 stdout이
     * journald로 들어가므로 유실되지 않는다.
     */
    AcuConfig cfg;
    config_set_defaults(&cfg);
    config_load(config_path, &cfg); /* config.json이 없거나 잘못돼도 기본값으로 계속 진행 */

    log_open(cfg.log_path); /* 빈 문자열이면 stdout 유지 */
    log_msg("ACU 데몬 시작");

    {
        char line[320];
        snprintf(line, sizeof(line), "설정 파일: %s", config_path);
        log_msg(line);
    }

    /*
     * PID 파일 경로는 실행 중 바뀌어도 따라가지 않는다(종료 시 지울 대상이 흔들리면 안 되므로).
     * 여기서 복사해 두고 끝까지 이 값을 쓴다.
     */
    char pid_path[sizeof(cfg.pid_path)];
    snprintf(pid_path, sizeof(pid_path), "%s", cfg.pid_path);

    FILE *pidf = fopen(pid_path, "w");
    if (pidf)
    {
        fprintf(pidf, "%d\n", (int)getpid());
        fclose(pidf);
    }
    else
    {
        char line[400];
        snprintf(line, sizeof(line),
                 "PID 파일 생성 실패 (%s) - 웹 설정 인터페이스의 리로드 신호 전송이 안 될 수 있음",
                 pid_path);
        log_msg(line);
    }

    if (hal_init() != 0)
    {
        log_msg("HAL 초기화 실패 -> 종료");
        remove(pid_path); /* 죽은 PID가 남으면 웹 설정 화면이 엉뚱한 프로세스에 신호를 보낸다 */
        log_close();
        return 1;
    }

    sqlite3 *db = db_open(cfg.db_path);
    if (!db)
    {
        log_msg("DB 초기화 실패 -> 종료");
        hal_shutdown();
        remove(pid_path);
        log_close();
        return 1;
    }
    db_seed_dummy_data(db);
    devset_init(db); /* 장치 설정 표 (입력·출력 등) — 없어도 판정은 돈다 */

    /*
     * 이벤트 저장소. DB를 못 열어도 NULL을 돌려주지 않고 메모리 전용 모드로 돈다
     * (저장은 못 해도 출입 판정과 상위 보고는 계속돼야 한다 - events.h 머리말).
     */
    char events_path[300];
    resolve_events_path(events_path, sizeof(events_path), &cfg);
    AcuEvents *events = events_open(events_path, cfg.events_capacity);

    /* 사용자 명단. 이것 없이는 판정을 할 수 없으므로 실패하면 종료한다 */
    char users_path[300];
    resolve_users_path(users_path, sizeof(users_path), &cfg);
    AcuUsers *users = users_open(users_path);
    if (!users)
    {
        log_msg("사용자 저장 초기화 실패 -> 종료");
        events_close(events);
        db_close(db);
        hal_shutdown();
        remove(pid_path);
        log_close();
        return 1;
    }
    users_seed_dummy(users);

    /* 사용자 바이너리 전송 수신기. 없으면 그 명령에 Fail로 답할 뿐 나머지는 정상 동작한다 */
    AcuUserBin *userbin = userbin_create(users);
    if (!userbin)
    {
        log_msg("사용자 바이너리 수신기 생성 실패 - 전체 다운로드를 받지 못한다");
    }

    AcuNet *net = net_init(cfg.tcp_port); /* 실패해도 net=NULL로 계속 진행 (출입 판정은 네트워크 없이도 동작) */

    net_set_inactivity_timeout(net, cfg.inactivity_seconds);
    net_set_device_identity(net, cfg.device_category, cfg.device_type);
    net_set_time_sync(net, cfg.time_sync_enabled);
    net_set_event_store(net, events, cfg.events_batch_size);
    net_set_userbin(net, userbin);
    net_set_users(net, users);
    net_set_settings_db(net, db);
    apply_module_layout(net, &cfg);

    /* UDP 탐색. 실패해도 NULL로 두고 계속 간다 */
    AcuDiscover *disc = discover_init(&cfg);

    /*
     * 이벤트 루프. 여기가 이 데몬의 유일한 select다.
     * 이것마저 못 만들면 기다릴 방법이 없어 종료한다 (calloc 실패 수준의 상황이다).
     */
    AcuLoop *loop = loop_create();
    if (!loop)
    {
        log_msg("이벤트 루프 생성 실패 -> 종료");
        discover_shutdown(disc);
        net_shutdown(net);
        userbin_destroy(userbin);
        events_close(events);
        users_close(users);
        db_close(db);
        hal_shutdown();
        remove(pid_path);
        log_close();
        return 1;
    }

    AcuRuntime rt = { db, users, net, disc, &cfg, 0, 0 };

    /* 강제 개방은 net이 상태를 들고, 실제 릴레이는 rt를 통해 HAL로 나간다 */
    net_set_force_open_handler(net, on_force_open_changed, &rt);

    if (net)
    {
        net_attach_loop(net, loop);
    }
    if (disc)
    {
        loop_add(loop, discover_fd(disc), ACU_LOOP_READ, on_discover_readable, disc);
    }
    register_input_fds(loop, &rt);

    /* 점검 타이머 (유휴 타임아웃·연결 상태). 없어도 본체는 돌아야 하므로 실패는 로그만 남긴다 */
    int housekeeping_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (housekeeping_fd >= 0)
    {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = HOUSEKEEPING_INTERVAL_SEC;
        its.it_interval.tv_sec = HOUSEKEEPING_INTERVAL_SEC;
        if (timerfd_settime(housekeeping_fd, 0, &its, NULL) != 0 ||
            loop_add(loop, housekeeping_fd, ACU_LOOP_READ, on_housekeeping, &rt) != 0)
        {
            log_msg("점검 타이머 등록 실패 - 유휴 타임아웃이 동작하지 않는다");
            close(housekeeping_fd);
            housekeeping_fd = -1;
        }
    }
    else
    {
        log_msg("점검 타이머 생성 실패 - 유휴 타임아웃이 동작하지 않는다");
    }

    /* 메인 루프: 종료 시그널이 올 때까지 계속 돈다 */
    while (g_running)
    {
        if (g_reload)
        {
            g_reload = 0;
            log_msg("설정 리로드 요청 감지 -> config.json 다시 읽는 중");

            /*
             * logrotate가 로그 파일을 옮겨 갔을 수 있으므로 같은 경로로 다시 연다.
             * (stdout을 쓰는 중이면 아무 일도 하지 않는다)
             */
            log_reopen();

            AcuConfig new_cfg = cfg;
            if (config_load(config_path, &new_cfg) == 0)
            {
                if (strcmp(new_cfg.log_path, cfg.log_path) != 0)
                {
                    log_msg("로그 경로 변경 감지 -> 새 대상으로 전환");
                    log_open(new_cfg.log_path);
                    log_msg("로그 경로 변경 적용됨");
                }
                if (strcmp(new_cfg.pid_path, pid_path) != 0)
                {
                    /* 종료 시 지울 파일이 달라지면 PID 파일이 남아 떠돌게 된다 */
                    log_msg("pid_path 변경은 재시작해야 반영된다 -> 이번에는 무시함");
                    snprintf(new_cfg.pid_path, sizeof(new_cfg.pid_path), "%s", pid_path);
                }
                if (strcmp(new_cfg.db_path, cfg.db_path) != 0)
                {
                    sqlite3 *new_db = db_open(new_cfg.db_path);
                    if (new_db)
                    {
                        db_close(db);
                        db = new_db;
                        rt.db = db;
                        db_seed_dummy_data(db);
                        devset_init(db);
                        net_set_settings_db(net, db);
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
                        net_shutdown(net); /* 옛 소켓의 루프 등록도 여기서 풀린다 */
                        net = new_net;
                        rt.net = net;
                        /* net을 새로 만들었으니 설정값과 루프 등록을 다시 얹어 준다 */
                        net_set_inactivity_timeout(net, new_cfg.inactivity_seconds);
                        net_set_device_identity(net, new_cfg.device_category, new_cfg.device_type);
                        net_set_time_sync(net, new_cfg.time_sync_enabled);
                        net_set_event_store(net, events, new_cfg.events_batch_size);
                        net_set_userbin(net, userbin);
                        net_set_users(net, users);
                        net_set_settings_db(net, db);
                        net_set_force_open_handler(net, on_force_open_changed, &rt);
                        apply_module_layout(net, &new_cfg);
                        net_attach_loop(net, loop);
                        log_msg("네트워크 포트 변경 적용됨 (재시작 없이 전환)");
                    }
                    else
                    {
                        log_msg("새 네트워크 포트 열기 실패 -> 기존 포트 유지");
                        new_cfg.tcp_port = cfg.tcp_port;
                    }
                }
                cfg = new_cfg;
                /* 인터페이스/포트/비밀번호 등 탐색이 보는 값들을 한 번에 반영한다 */
                discover_apply_config(disc, &cfg);
                net_set_inactivity_timeout(net, cfg.inactivity_seconds);
                net_set_device_identity(net, cfg.device_category, cfg.device_type);
                net_set_time_sync(net, cfg.time_sync_enabled);
                net_set_event_store(net, events, cfg.events_batch_size);
                apply_module_layout(net, &cfg);
                /*
                 * events_db_path·events_capacity는 재시작해야 반영된다 - 돌고 있는 저장소를
                 * 바꾸면 아직 안 올라간 이벤트의 전송 위치가 어긋난다. 묶음 크기만 바로 반영한다.
                 */
                log_msg("설정 리로드 완료 (이벤트 저장 경로·한도는 재시작 후 반영)");
            }
            else
            {
                log_msg("설정 리로드 실패 -> 기존 설정 유지");
            }
        }

        /*
         * 등록된 fd 중 하나가 준비될 때까지 기다린다. 깨어날 이유가 없으면 그냥 자고 있는다 -
         * 주기적으로 무언가를 조회하지 않으므로 카드가 없는 동안 이 데몬은 아무 일도 하지 않는다.
         * 시그널이 오면 select가 EINTR로 돌아오고, 위의 종료·리로드 검사로 이어진다.
         */
        if (loop_wait(loop, -1) < 0 && errno != EINTR)
        {
            log_msg("이벤트 루프 대기 오류 - 잠시 쉬었다 계속한다");
            struct timespec pause_ts = { 0, 100 * 1000000L };
            nanosleep(&pause_ts, NULL);
        }
    }

    if (housekeeping_fd >= 0)
    {
        loop_remove(loop, housekeeping_fd);
        close(housekeeping_fd);
    }
    loop_destroy(loop);
    discover_shutdown(disc);
    net_shutdown(net);
    userbin_destroy(userbin); /* net보다 뒤에 - net이 받던 전송을 참조한다 */
    events_close(events);
    users_close(users);
    db_close(db);
    hal_shutdown();
    remove(pid_path);
    log_msg("ACU 데몬 정상 종료");
    log_close();
    return 0;
}
