/* c11 엄격 모드에서 POSIX 함수(mkfifo, open 등)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include "hal.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * HAL 모의(mock) 구현체.
 * 실제 리더기/릴레이/센서(6단계 예정)가 없는 동안 상위 로직(main.c/access.c)이 HAL 인터페이스만으로
 * 동작하는지 검증한다.
 *
 * 기본 동작은 "아무 일도 일어나지 않음"이고, 테스트 입력은 FIFO(named pipe)로 주입한다.
 *   echo 04A1B2C3D4E5F600 > acud_mock.fifo   # 카드 태그 1회
 *   echo "door open"      > acud_mock.fifo   # 도어 접점 = 열림
 *   echo "auto on"        > acud_mock.fifo   # 더미 카드 자동 순회(예전 동작) 켜기
 *
 * 예전에는 hal_read_card()가 호출될 때마다(2초 주기) 무조건 더미 카드를 태그한 것처럼 굴었는데,
 * 그러면 상위 시스템과 통신을 테스트하는 동안 이벤트 큐(32개)가 1분 남짓이면 가득 차 버려
 * 무엇을 보고 있는지 알 수 없었다. 그래서 주입식으로 바꾸고, 예전 동작은 "auto on"으로 남겨 뒀다.
 */

/*
 * 기본은 cwd 상대경로지만, 데몬으로 띄우면 cwd가 "/"라 쓸 수 없다.
 * 환경변수 ACU_MOCK_FIFO로 절대경로를 줄 수 있게 해 뒀다 (systemd 유닛의 Environment=).
 * mock 전용이라 config.json(제품 설정)에는 넣지 않는다 - 6단계에서 실제 HAL로 교체되면 사라질 값이다.
 */
#define MOCK_FIFO_PATH_DEFAULT "acud_mock.fifo"
#define MOCK_FIFO_PATH_ENV     "ACU_MOCK_FIFO"
#define MOCK_LINE_CAP  256

static const char *DUMMY_CARD_IDS[] = {
    "04A1B2C3D4E5F600", /* 활성 + 유효기간/시간대 제한없음 -> 허용 */
    "AABBCCDD11223300", /* 비활성화된 카드 -> 거부 */
    "FFFFFFFFFFFFFFFF", /* 미등록 카드 -> 거부 */
    "1122334455667700", /* 활성 + 유효기간(2020년) 만료 -> 거부 */
    "2233445566778800", /* 활성 + 유효기간/시간대 그룹 모두 통과 -> 허용 */
    "3344556677889900", /* 활성 + 시간대 그룹과 요일 불일치 -> 거부 */
};
#define DUMMY_CARD_COUNT (sizeof(DUMMY_CARD_IDS) / sizeof(DUMMY_CARD_IDS[0]))

static size_t g_card_idx = 0;
static int    g_auto_mode = 0; /* 1이면 예전처럼 조회할 때마다 더미 카드를 순회한다 */

static int g_fifo_fd = -1;
static char   g_line[MOCK_LINE_CAP];
static size_t g_line_len = 0;

static int g_door_active = 0; /* 1이면 도어 접점 활성(=문 열림) */
static int g_exit_active = 0; /* 1이면 Exit 버튼 눌림 */

/*
 * 주입된 카드 대기 큐.
 * 카드 조회는 2초에 한 번인데 FIFO에는 한 번에 여러 줄이 들어올 수 있다. 읽은 것을 여기 쌓아 두고
 * hal_read_card() 호출마다 한 장씩 꺼낸다 (실제 리더기도 한 번에 한 장씩 올라온다).
 */
#define MOCK_CARD_QUEUE_CAP 16
static char   g_cards[MOCK_CARD_QUEUE_CAP][17];
static size_t g_cards_head = 0;
static size_t g_cards_count = 0;

static void push_card(const char *card_id)
{
    if (g_cards_count == MOCK_CARD_QUEUE_CAP)
    {
        log_msg("HAL(mock): 카드 대기 큐가 가득 참 - 가장 오래된 것을 버림");
        g_cards_head = (g_cards_head + 1) % MOCK_CARD_QUEUE_CAP;
        g_cards_count--;
    }
    size_t idx = (g_cards_head + g_cards_count) % MOCK_CARD_QUEUE_CAP;
    snprintf(g_cards[idx], sizeof(g_cards[idx]), "%s", card_id);
    g_cards_count++;
}

/* 문자열 양끝 공백/개행을 제거한다 (in-place) */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    {
        s[--n] = '\0';
    }
    return s;
}

/* 카드 ID로 쓸 수 있는 hex 문자열인지 확인한다 (2~16자, 짝수 길이) */
static int is_card_hex(const char *s)
{
    size_t n = strlen(s);
    if (n < 2 || n > 16 || (n % 2) != 0)
    {
        return 0;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return 0;
        }
    }
    return 1;
}

/*
 * 카드가 아닌 명령을 처리한다. 반환: 1=처리함, 0=모르는 명령.
 * (main.c는 아직 Exit 버튼을 판정 흐름에 쓰지 않지만, 센서 상태 주입 경로는 미리 열어 둔다)
 */
static int handle_command(const char *line)
{
    char msg[128];

    if (strcmp(line, "door open") == 0 || strcmp(line, "door closed") == 0)
    {
        g_door_active = (strcmp(line, "door open") == 0);
        snprintf(msg, sizeof(msg), "HAL(mock): 도어 접점 = %s", g_door_active ? "열림" : "닫힘");
        log_msg(msg);
        return 1;
    }
    if (strcmp(line, "exit on") == 0 || strcmp(line, "exit off") == 0)
    {
        g_exit_active = (strcmp(line, "exit on") == 0);
        snprintf(msg, sizeof(msg), "HAL(mock): Exit 버튼 = %s (main 루프는 아직 Exit 버튼을 처리하지 않음)",
                 g_exit_active ? "눌림" : "안눌림");
        log_msg(msg);
        return 1;
    }
    if (strcmp(line, "auto on") == 0 || strcmp(line, "auto off") == 0)
    {
        g_auto_mode = (strcmp(line, "auto on") == 0);
        snprintf(msg, sizeof(msg), "HAL(mock): 더미 카드 자동 순회 = %s", g_auto_mode ? "켜짐" : "꺼짐");
        log_msg(msg);
        return 1;
    }
    if (strcmp(line, "list") == 0)
    {
        for (size_t i = 0; i < DUMMY_CARD_COUNT; i++)
        {
            snprintf(msg, sizeof(msg), "HAL(mock): 더미 카드 %zu - %s", i + 1, DUMMY_CARD_IDS[i]);
            log_msg(msg);
        }
        return 1;
    }
    return 0;
}

/* FIFO에 들어온 줄을 전부 처리한다 (명령은 즉시 반영, 카드는 대기 큐에 쌓는다) */
static void drain_fifo(void)
{
    if (g_fifo_fd < 0)
    {
        return;
    }

    char buf[MOCK_LINE_CAP];
    ssize_t n;
    while ((n = read(g_fifo_fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            if (buf[i] != '\n' && g_line_len < sizeof(g_line) - 1)
            {
                g_line[g_line_len++] = buf[i];
                continue;
            }

            g_line[g_line_len] = '\0';
            g_line_len = 0;

            char *line = trim(g_line);
            if (*line == '\0' || handle_command(line))
            {
                continue;
            }
            if (is_card_hex(line))
            {
                /* 16자보다 짧게 넣어도 되도록 뒤를 0으로 채운다 (DB의 card_id는 hex 16자 고정) */
                char padded[17];
                snprintf(padded, sizeof(padded), "%-16s", line);
                for (size_t k = 0; k < 16; k++)
                {
                    padded[k] = (padded[k] == ' ') ? '0' : (char)toupper((unsigned char)padded[k]);
                }
                push_card(padded);
                continue;
            }

            char msg[160];
            snprintf(msg, sizeof(msg), "HAL(mock): 알 수 없는 입력 \"%s\" (카드 hex 또는 door/exit/auto/list)", line);
            log_msg(msg);
        }
    }
}

/* 실제로 쓸 FIFO 경로. hal_init()에서 한 번 정해 두고 hal_shutdown()까지 같은 값을 쓴다 */
static char g_fifo_path[256] = MOCK_FIFO_PATH_DEFAULT;

int hal_init(void)
{
    const char *env = getenv(MOCK_FIFO_PATH_ENV);
    if (env && env[0] != '\0')
    {
        snprintf(g_fifo_path, sizeof(g_fifo_path), "%s", env);
    }

    if (mkfifo(g_fifo_path, 0666) != 0 && errno != EEXIST)
    {
        log_msg("HAL(mock): 카드 주입 FIFO 생성 실패 - 카드 입력 없이 동작함");
        return 0; /* FIFO가 없어도 데몬 자체는 계속 동작해야 한다 */
    }

    /*
     * O_RDWR로 여는 이유: 읽기 전용으로 열면 쓰는 쪽이 없을 때 read()가 계속 EOF(0)를 돌려주고,
     * 쓰는 쪽이 붙었다 떨어질 때마다 상태가 흔들린다. 우리가 쓰기 끝도 함께 붙들고 있으면 항상
     * "열려 있는 파이프"가 되어 EOF 없이 논블로킹으로 계속 읽을 수 있다.
     */
    g_fifo_fd = open(g_fifo_path, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0)
    {
        log_msg("HAL(mock): 카드 주입 FIFO 열기 실패 - 카드 입력 없이 동작함");
        return 0;
    }

    char msg[400];
    snprintf(msg, sizeof(msg),
             "HAL(mock) 초기화 - 실제 리더기 없음. 테스트 입력은 %s 로 주입", g_fifo_path);
    log_msg(msg);
    snprintf(msg, sizeof(msg),
             "HAL(mock)   예) echo 04A1B2C3D4E5F600 > %s   (카드 태그)", g_fifo_path);
    log_msg(msg);
    snprintf(msg, sizeof(msg),
             "HAL(mock)   예) echo \"door open\" > %s   / \"auto on\" 이면 예전처럼 자동 순회",
             g_fifo_path);
    log_msg(msg);
    return 0;
}

void hal_shutdown(void)
{
    if (g_fifo_fd >= 0)
    {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    unlink(g_fifo_path);
}

int hal_read_card(char *out_card_id, size_t out_len)
{
    drain_fifo();

    if (g_cards_count > 0)
    {
        snprintf(out_card_id, out_len, "%s", g_cards[g_cards_head]);
        g_cards_head = (g_cards_head + 1) % MOCK_CARD_QUEUE_CAP;
        g_cards_count--;
        return 1;
    }

    if (g_auto_mode)
    {
        snprintf(out_card_id, out_len, "%s", DUMMY_CARD_IDS[g_card_idx]);
        g_card_idx = (g_card_idx + 1) % DUMMY_CARD_COUNT;
        return 1;
    }

    return 0; /* 주입된 카드 없음 */
}

int hal_open_door(int seconds)
{
    char line[64];
    snprintf(line, sizeof(line), "HAL(mock): 도어 릴레이 %d초 동작", seconds);
    log_msg(line);
    return 0;
}

int hal_read_sensor(HalSensorId id)
{
    switch (id)
    {
        case HAL_SENSOR_DOOR_CONTACT: return g_door_active ? HAL_SENSOR_ACTIVE : HAL_SENSOR_INACTIVE;
        case HAL_SENSOR_EXIT_BUTTON:  return g_exit_active ? HAL_SENSOR_ACTIVE : HAL_SENSOR_INACTIVE;
        default:                      return -1;
    }
}
