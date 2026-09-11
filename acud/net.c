/* c11 엄격 모드에서 POSIX 함수(localtime_r 등)를 노출시키기 위해 필요 */
#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "protocol.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define NET_RECV_BUF_CAP 512
#define NET_RESP_BUF_CAP 1024 /* 응답 1건 최대: Header 44 + DeviceStatus 234 + Firmware 268 + Tail 2 = 548 */
#define NET_SEND_BUF_CAP 4096 /* 응답 1건(316byte)보다 넉넉히. 부분 전송분을 담아 둔다 */
#define NET_EVENT_QUEUE_CAP 32

typedef struct {
    uint32_t event_code;
    uint8_t  id[8];
    uint8_t  door_status;
    time_t   ts;
} NetEvent;

struct AcuNet {
    int listen_fd;
    int client_fd; /* -1 이면 미연결 (동시 1개 연결만 지원) */

    uint8_t recv_buf[NET_RECV_BUF_CAP];
    size_t  recv_len;

    /* 송신 대기 버퍼: 논블로킹 소켓이라 send()가 일부만 보내고 반환할 수 있다 */
    uint8_t send_buf[NET_SEND_BUF_CAP];
    size_t  send_len; /* 버퍼에 쌓인 총 바이트 */
    size_t  send_off; /* 그중 이미 보낸 바이트 */

    int door_status; /* IDTI_DOOR_STATUS_*, Device Status 응답에 반영 */

    NetEvent queue[NET_EVENT_QUEUE_CAP];
    size_t queue_head;
    size_t queue_count;

    /* net_poll의 select에 얹어 주는 외부 fd (UDP 탐색 등). -1이면 없음 */
    int   aux_fd;
    void (*aux_on_readable)(void *user);
    void *aux_user;

    /*
     * 유휴 타임아웃 (netmodule의 InactivityTime, 초). 0이면 끄기.
     * 케이블만 빠진 것처럼 상대가 조용히 사라지면 TCP는 한참 뒤에야 알아챈다.
     * 그동안 연결이 살아 있는 것처럼 남아 있으므로 직접 정리한다.
     */
    int inactivity_seconds;
    struct timespec last_activity; /* CLOCK_MONOTONIC. client_fd가 유효할 때만 의미 있다 */

    /*
     * Device Status / Firmware Info의 앞 2byte. PC에 등록한 모델과 맞아야 하므로 설정으로 받는다
     * (protocol.h의 IDTI_DEVICE_CATEGORY_* / IDTI_CONTROLLER_TYPE_* 참고).
     */
    int device_category;
    int device_type;

    /*
     * 시각 동기화 (netmodule이 아니라 IDTi 프레임의 IsTimeSync 비트).
     * DM은 폴링 요청마다 자기 시각을 실어 보낸다 - **고립망에서는 이것이 유일한 시각 공급원**이다
     * (NTP 서버가 없고, 이 보드의 RTC는 I2C 풀업 누락으로 죽어 있다).
     */
    int time_sync_enabled;

    AcuModuleLayout layout; /* 상위 시스템에 보고할 I/O 구성 */
};

/* 클라이언트가 뭔가를 했다고 표시한다 (접속/수신/송신) */
static void net_touch_activity(AcuNet *net)
{
    clock_gettime(CLOCK_MONOTONIC, &net->last_activity);
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* 연결이 끊겼을 때 소켓과 송수신 버퍼를 함께 정리한다 */
static void net_drop_client(AcuNet *net, const char *reason)
{
    if (net->client_fd >= 0)
    {
        close(net->client_fd);
        net->client_fd = -1;
    }
    net->recv_len = 0;
    net->send_len = 0;
    net->send_off = 0;
    if (reason)
    {
        log_msg(reason);
    }
}

/*
 * 송신 버퍼에 남은 바이트를 보낼 수 있는 만큼 보낸다.
 * 논블로킹 소켓이므로 커널 송신 버퍼가 차면 EAGAIN으로 일부만 나갈 수 있다 -> 나머지는 남겨 두고
 * 다음 net_poll()에서 쓰기 가능해질 때 이어 보낸다. SIGPIPE는 MSG_NOSIGNAL로 막는다.
 */
static void net_flush_send(AcuNet *net)
{
    while (net->client_fd >= 0 && net->send_off < net->send_len)
    {
        ssize_t n = send(net->client_fd, net->send_buf + net->send_off,
                         net->send_len - net->send_off, MSG_NOSIGNAL);
        if (n > 0)
        {
            net->send_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            return; /* 지금은 더 못 보냄. 남은 만큼 다음 기회에 이어 보낸다 */
        }
        net_drop_client(net, "네트워크: 전송 실패 -> 연결 종료 (상위 시스템이 먼저 끊은 것으로 보임)");
        return;
    }

    if (net->send_off >= net->send_len)
    {
        net->send_len = 0;
        net->send_off = 0;
    }
}

/*
 * 응답을 송신 버퍼에 넣고 곧바로 보낼 수 있는 만큼 보낸다.
 * 반환: 0=성공(전부 보냈거나 버퍼에 남김), -1=버퍼가 부족해 보내지 못함.
 */
static int net_queue_send(AcuNet *net, const uint8_t *data, size_t len)
{
    if (net->client_fd < 0)
    {
        return -1;
    }

    /* 이미 보낸 앞부분을 걷어내 남은 공간을 확보한다 */
    if (net->send_off > 0)
    {
        memmove(net->send_buf, net->send_buf + net->send_off, net->send_len - net->send_off);
        net->send_len -= net->send_off;
        net->send_off = 0;
    }

    if (net->send_len + len > NET_SEND_BUF_CAP)
    {
        log_msg("네트워크: 송신 버퍼가 가득 차 응답을 보내지 못함");
        return -1;
    }

    memcpy(net->send_buf + net->send_len, data, len);
    net->send_len += len;
    net_flush_send(net);
    return 0;
}

AcuNet *net_init(int port)
{
    AcuNet *net = calloc(1, sizeof(AcuNet));
    if (!net)
    {
        return NULL;
    }
    net->client_fd = -1;
    net->door_status = IDTI_DOOR_STATUS_NONE;
    net->aux_fd = -1; /* calloc이 0으로 채우므로 명시적으로 -1을 넣어야 한다 */
    net->inactivity_seconds = 0; /* main이 설정값으로 덮어쓴다. 0이면 타임아웃 없음 */
    net->device_category = IDTI_DEVICE_CATEGORY_DEFAULT;
    net->device_type = IDTI_DEVICE_TYPE_DEFAULT;
    net->time_sync_enabled = 0; /* main이 설정값으로 덮어쓴다 */
    /* layout은 calloc으로 0이다. main이 설정값으로 채운다 */

    net->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (net->listen_fd < 0)
    {
        log_msg("네트워크: 소켓 생성 실패");
        free(net);
        return NULL;
    }

    int opt = 1;
    setsockopt(net->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(net->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_msg("네트워크: bind 실패 (포트 사용 중이거나 권한 없음)");
        close(net->listen_fd);
        free(net);
        return NULL;
    }
    if (listen(net->listen_fd, 1) < 0)
    {
        log_msg("네트워크: listen 실패");
        close(net->listen_fd);
        free(net);
        return NULL;
    }
    set_nonblocking(net->listen_fd);

    char line[64];
    snprintf(line, sizeof(line), "네트워크(TCP) 서버 시작 - 포트 %d 대기 중", port);
    log_msg(line);

    return net;
}

void net_shutdown(AcuNet *net)
{
    if (!net)
    {
        return;
    }
    if (net->client_fd >= 0)
    {
        close(net->client_fd);
    }
    close(net->listen_fd);
    free(net);
}

void net_set_door_status(AcuNet *net, int door_status)
{
    if (!net)
    {
        return;
    }
    net->door_status = door_status;
}

static uint32_t event_code_for_result(AccessResult result)
{
    switch (result)
    {
        case ACCESS_GRANTED:          return IDTI_EVENT_ACCESS_AUTH_BY_CARD;
        case ACCESS_DENIED_DISABLED:  return IDTI_EVENT_ACCESS_DENIED_NOT_ENABLED;
        case ACCESS_DENIED_TIMEZONE:  return IDTI_EVENT_ACCESS_DENIED_BY_TIME;
        case ACCESS_DENIED_NOT_FOUND:
        case ACCESS_DENIED_VALIDATION:
        default:                      return IDTI_EVENT_ACCESS_DENIED_BY_CARD;
    }
}

/* "0011...FF" 형태의 hex 문자열을 raw byte로 변환한다 (out_len을 넘는 부분은 버림) */
static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    memset(out, 0, out_len);
    size_t hex_len = strlen(hex);
    size_t byte_count = hex_len / 2;
    if (byte_count > out_len)
    {
        byte_count = out_len;
    }
    for (size_t i = 0; i < byte_count; i++)
    {
        unsigned int v = 0;
        sscanf(hex + i * 2, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

void net_push_event(AcuNet *net, AccessResult result, const char *id_hex, int door_status)
{
    if (!net || result == ACCESS_DENIED_DB_ERROR)
    {
        return; /* DB 조회 자체의 오류는 상위 시스템에 보고할 실질적 의미가 없음 */
    }

    if (net->queue_count == NET_EVENT_QUEUE_CAP)
    {
        /* 큐가 가득 차면 가장 오래된 이벤트를 버리고 계속 진행 (fail-safe) */
        net->queue_head = (net->queue_head + 1) % NET_EVENT_QUEUE_CAP;
        net->queue_count--;
        log_msg("네트워크: 이벤트 큐가 가득 차 가장 오래된 이벤트를 버림");
    }

    size_t idx = (net->queue_head + net->queue_count) % NET_EVENT_QUEUE_CAP;
    NetEvent *ev = &net->queue[idx];
    ev->event_code = event_code_for_result(result);
    hex_to_bytes(id_hex, ev->id, sizeof(ev->id));
    ev->door_status = (uint8_t)door_status;
    ev->ts = time(NULL);
    net->queue_count++;
}

/* Protocol V2 Device Status(234byte) = DeviceType(2)+CurDateTime(6)+ExistedModule(2)+IOModuleStatus(16*14).
 * 단일 도어/단일 리더 컨트롤러라 IO 확장 모듈이 없으므로 ExistedModule=0, 모듈 배열은 전부 0으로 채운다. */
/*
 * I/O 구성을 Device Status의 모듈 배열에 채운다.
 *
 * 모듈 하나는 14슬롯을 순서대로 채운다: 카드리더 -> 입력 -> 출력 -> 알람·화재.
 * 마지막 2개는 **알람·화재** 입력이다. 보드 공통 입력의 사본이 아니라 모듈마다 실제로 배선된 입력이다.
 *
 * **DM은 이 정보를 받아야 장치 트리(카테고리)를 만든다.** 전부 0으로 보고하면 리더도 입출력도
 * 없는 장치로 보여 사용자·도어 설정을 내려보내지 못한다.
 */
static void fill_modules(uint8_t out[IDTI_DEVICE_STATUS_V2_LEN], const AcuModuleLayout *layout)
{
    /* 한 모듈 안에서 채울 순서 */
    const struct {
        int count;
        int io_type;
    } slots[] = {
        { layout->readers,       IDTI_IOTYPE_PROXIMITY_READER },
        { layout->inputs,        IDTI_IOTYPE_INPUT_SENSOR     },
        { layout->outputs,       IDTI_IOTYPE_OUTPUT_RELAY     },
        { layout->alarm_fire_inputs, IDTI_IOTYPE_INPUT_SENSOR },
    };

    int module_count = layout->module_count;
    if (module_count > IDTI_MODULE_COUNT_V2)
    {
        module_count = IDTI_MODULE_COUNT_V2;
    }

    unsigned existed = 0;

    for (int m = 0; m < module_count; m++)
    {
        uint8_t *entry = out + IDTI_MODULE_ARRAY_OFFSET + m * IDTI_MODULE_ENTRY_LEN;
        entry[0] = (uint8_t)layout->module_type;
        entry[1] = (uint8_t)layout->install_type;

        int slot = 0;
        for (size_t g = 0; g < sizeof(slots) / sizeof(slots[0]); g++)
        {
            for (int i = 0; i < slots[g].count && slot < IDTI_MODULE_IO_SLOTS; i++, slot++)
            {
                /* 상위 니블 = IOType, 하위 니블 = IOStatus */
                entry[2 + slot] = (uint8_t)((slots[g].io_type << 4) | IDTI_IOSTATUS_INACTIVE);
            }
        }
        /* 남는 슬롯은 0(IOType=None) 그대로 둔다 */

        existed |= (1u << m);
    }

    /* IsExistModule: 빅엔디안 16bit, 모듈 N = bit N */
    out[8] = (uint8_t)((existed >> 8) & 0xFF);
    out[9] = (uint8_t)(existed & 0xFF);
}

static void build_device_status(uint8_t out[IDTI_DEVICE_STATUS_V2_LEN], int door_status, time_t now,
                                int category, int type, const AcuModuleLayout *layout)
{
    (void)door_status; /* V2 Device Status 구조체 자체에는 도어 상태 필드가 없음(Event Info 쪽에만 존재) */

    memset(out, 0, IDTI_DEVICE_STATUS_V2_LEN);
    out[0] = (uint8_t)category;
    out[1] = (uint8_t)type;

    struct tm tmv;
    localtime_r(&now, &tmv);
    out[2] = idti_to_bcd((tmv.tm_year + 1900) % 100);
    out[3] = idti_to_bcd(tmv.tm_mon + 1);
    out[4] = idti_to_bcd(tmv.tm_mday);
    out[5] = idti_to_bcd(tmv.tm_hour);
    out[6] = idti_to_bcd(tmv.tm_min);
    out[7] = idti_to_bcd(tmv.tm_sec);

    fill_modules(out, layout);
}

/*
 * Firmware Info(268byte) = Category(1)+DeviceType(1)+Version(4)+DateTime(6, BCD)+Reserved(256).
 * PC(DM)가 접속 후 장치를 확인할 때 쓰는 응답 데이터다
 * (근거: PC 소스 `isldev/clsDevDeviceSetting.cs`의 GetFirmwareInfo).
 */
static void build_firmware_info(uint8_t out[IDTI_FIRMWARE_INFO_LEN], int category, int type)
{
    memset(out, 0, IDTI_FIRMWARE_INFO_LEN);
    out[0] = (uint8_t)category;
    out[1] = (uint8_t)type;
    out[2] = IDTI_FW_VERSION_MAJOR;
    out[3] = IDTI_FW_VERSION_MINOR;
    out[4] = IDTI_FW_VERSION_PATCH;
    out[5] = IDTI_FW_VERSION_BUILD;
    out[6]  = idti_to_bcd(IDTI_FW_DATE_YEAR);
    out[7]  = idti_to_bcd(IDTI_FW_DATE_MONTH);
    out[8]  = idti_to_bcd(IDTI_FW_DATE_DAY);
    out[9]  = idti_to_bcd(IDTI_FW_DATE_HOUR);
    out[10] = idti_to_bcd(IDTI_FW_DATE_MINUTE);
    out[11] = idti_to_bcd(IDTI_FW_DATE_SECOND);
    /* out[12..267] Reserved = 0 (memset로 처리됨) */
}

static void build_event_info(uint8_t out[IDTI_EVENT_INFO_LEN], const NetEvent *ev)
{
    memset(out, 0, IDTI_EVENT_INFO_LEN);
    out[0] = (uint8_t)(ev->event_code >> 24);
    out[1] = (uint8_t)(ev->event_code >> 16);
    out[2] = (uint8_t)(ev->event_code >> 8);
    out[3] = (uint8_t)(ev->event_code);
    out[4] = IDTI_OPMODE_CARD;
    out[5] = 0x00; /* Reserved */
    out[6] = 0x00; /* Module Address (단일 컨트롤러 -> 0) */
    out[7] = 0x00; /* Reader Address (단일 리더 -> 0) */
    out[8] = ev->door_status;
    out[9] = IDTI_FUNC_NONE;

    struct tm tmv;
    localtime_r(&ev->ts, &tmv);
    out[10] = idti_to_bcd((tmv.tm_year + 1900) % 100);
    out[11] = idti_to_bcd(tmv.tm_mon + 1);
    out[12] = idti_to_bcd(tmv.tm_mday);
    out[13] = idti_to_bcd(tmv.tm_hour);
    out[14] = idti_to_bcd(tmv.tm_min);
    out[15] = idti_to_bcd(tmv.tm_sec);

    memcpy(&out[16], ev->id, 8); /* Access ID */

    out[24] = 0x00; out[25] = 0x00; out[26] = 0x00; out[27] = 0x01; /* Revision ID: fixed */
    /* out[28..35] Reserved = 0 (memset로 처리됨) */
}

/*
 * 응답 한 건을 만들어 송신 큐에 넣는다.
 *
 * 응답 데이터는 [Device Status(234)] + [오브젝트 데이터] 순서로 실린다. 요청의 Frame Option에
 * IsExcludeDeviceStatus가 켜져 있으면 Device Status를 빼고, 그 비트를 응답에도 그대로 실어
 * PC가 응답 안에 Device Status가 들어 있는지 알 수 있게 한다.
 *
 * 반환: 0=송신 큐에 들어감, -1=실패(패킷 생성 실패 또는 송신 버퍼 부족).
 */
static int send_response(AcuNet *net, const IdtiHeader *hdr,
                         uint8_t command, uint8_t sub_command, uint8_t object,
                         const uint8_t *data, size_t data_len,
                         uint16_t block_start, uint16_t block_end,
                         uint16_t block_count, uint16_t block_one_len)
{
    /* Device Status + 가장 큰 오브젝트 데이터(Firmware Info)를 담을 수 있어야 한다 */
    uint8_t payload[IDTI_DEVICE_STATUS_V2_LEN + IDTI_FIRMWARE_INFO_LEN];
    size_t payload_len = 0;

    int exclude_status = (hdr->frame_option & IDTI_FOPT_EXCLUDE_DEVICE_STATUS) ? 1 : 0;
    if (!exclude_status)
    {
        build_device_status(payload, net->door_status, time(NULL),
                            net->device_category, net->device_type, &net->layout);
        payload_len = IDTI_DEVICE_STATUS_V2_LEN;
    }
    if (data_len > 0 && data != NULL)
    {
        memcpy(payload + payload_len, data, data_len);
        payload_len += data_len;
    }

    /*
     * 응답 주소 처리 (단순화): TCP는 1:1 연결이라 RS-485 다중 장치 버스 주소 지정이 실질적
     * 의미가 없다. Destination은 상대가 보낸 Source(5byte, Host/ComSlot/Controller/Module/Device)를
     * 그대로 앞 5byte에 옮기고 나머지 3byte(Device 확장 bitmask)는 0으로 채운다.
     * Source는 문서상 고정값(Host/ComSlot/Controller/Module/Device 전부 0x01)을 그대로 쓴다.
     */
    uint8_t dest_addr[IDTI_ADDR_DEST_LEN] = {0};
    memcpy(dest_addr, hdr->src_addr, IDTI_ADDR_SRC_LEN);
    static const uint8_t src_addr[IDTI_ADDR_SRC_LEN] = {0x01, 0x01, 0x01, 0x01, 0x01};

    uint16_t resp_option = exclude_status ? IDTI_FOPT_EXCLUDE_DEVICE_STATUS : 0;

    uint8_t out[NET_RESP_BUF_CAP];
    int n = idti_build_packet(out, sizeof(out), dest_addr, src_addr,
                               resp_option, hdr->frame_index, hdr->password,
                               command, sub_command, object,
                               hdr->start_item, hdr->end_item,
                               block_start, block_end, block_count, block_one_len,
                               payload, payload_len);
    if (n <= 0)
    {
        log_msg("네트워크: 응답 패킷 생성 실패");
        return -1;
    }

    return net_queue_send(net, out, (size_t)n);
}

/* Event Log(History, Object 0x01) 조회 요청 처리 */
/* 시각을 바꿀 때 이 값보다 작은 차이는 무시한다. 매 폴링(3초)마다 시계를 건드리지 않기 위함 */
#define NET_TIME_SYNC_MIN_DRIFT_SEC 2

/*
 * 요청에 실려 온 시각(BCD 7byte)으로 시스템 시계를 맞춘다.
 *
 * **왜 필요한가**: 설치 현장은 외부와 끊긴 로컬망이라 NTP 서버가 없을 수 있다. 그러면 시계를
 * 맞출 방법이 상위 시스템(DM)뿐이다. IDTi 이벤트는 BCD 시각을 싣기 때문에 시계가 틀리면
 * 출입 기록이 증거로서 무의미해진다.
 *
 * **주의**: 시각을 네트워크에서 받아들이는 것은 신뢰 결정이다. 시간대별 출입 제한을 쓰는 경우
 * 시계를 옮기면 허용 시간이 바뀔 수 있으므로, 설정으로 끌 수 있게 해 두고 적용할 때마다 로그를 남긴다.
 */
static void apply_time_sync(AcuNet *net, const uint8_t *data, size_t data_len)
{
    if (!net->time_sync_enabled || data_len < IDTI_TIME_SYNC_LEN)
    {
        return;
    }

    int yy = idti_from_bcd(data[0]);
    int mm = idti_from_bcd(data[1]);
    int dd = idti_from_bcd(data[2]);
    /* data[3]은 요일. 우리가 쓰지 않는다 */
    int hh = idti_from_bcd(data[4]);
    int mi = idti_from_bcd(data[5]);
    int ss = idti_from_bcd(data[6]);

    if (yy < 0 || mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
        hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59)
    {
        log_msg("네트워크: 시각 동기화 값이 이상해 무시한다");
        return;
    }

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year  = 2000 + yy - 1900;
    tmv.tm_mon   = mm - 1;
    tmv.tm_mday  = dd;
    tmv.tm_hour  = hh;
    tmv.tm_min   = mi;
    tmv.tm_sec   = ss;
    tmv.tm_isdst = -1;

    /* DM이 보내는 것은 지역 시각이다 (실제 패킷에서 KST와 일치하는 것을 확인했다) */
    time_t want = mktime(&tmv);
    if (want == (time_t)-1)
    {
        log_msg("네트워크: 시각 동기화 값을 시간으로 바꾸지 못했다");
        return;
    }

    time_t now = time(NULL);
    long drift = (long)(want - now);
    if (drift > -NET_TIME_SYNC_MIN_DRIFT_SEC && drift < NET_TIME_SYNC_MIN_DRIFT_SEC)
    {
        return; /* 이미 맞다 */
    }

    struct timespec ts;
    ts.tv_sec = want;
    ts.tv_nsec = 0;

    char line[200];
    if (clock_settime(CLOCK_REALTIME, &ts) != 0)
    {
        snprintf(line, sizeof(line),
                 "네트워크: 시각 동기화 실패 (%s) - 유닛에 CAP_SYS_TIME이 있는지 확인할 것",
                 strerror(errno));
        log_msg(line);
        return;
    }

    snprintf(line, sizeof(line),
             "네트워크: 상위 시스템 시각으로 %ld초 보정 -> %04d-%02d-%02d %02d:%02d:%02d",
             drift, 2000 + yy, mm, dd, hh, mi, ss);
    log_msg(line);
}

static void handle_history_request(AcuNet *net, const IdtiHeader *hdr)
{
    uint8_t event[IDTI_EVENT_INFO_LEN];
    size_t data_len = 0;
    uint16_t start = 0, end = 0, count = 0, one_len = 0;

    int event_attached = 0;
    if (net->queue_count > 0)
    {
        /* 큐에서 꺼내는 것은 전송에 성공한 뒤에 한다 (전송이 실패하면 이벤트가 유실되므로) */
        build_event_info(event, &net->queue[net->queue_head]);
        data_len = IDTI_EVENT_INFO_LEN;
        start = 1; end = 1; count = 1;
        one_len = IDTI_EVENT_INFO_LEN;
        event_attached = 1;
    }

    if (send_response(net, hdr, IDTI_CMD_SND_DATA, IDTI_SUBCMD_READ, IDTI_OBJ_HISTORY,
                      event, data_len, start, end, count, one_len) == 0)
    {
        if (event_attached)
        {
            net->queue_head = (net->queue_head + 1) % NET_EVENT_QUEUE_CAP;
            net->queue_count--;
        }
    }
    else if (event_attached)
    {
        log_msg("네트워크: 응답을 보내지 못해 이벤트를 큐에 남겨 둠 (다음 요청 때 다시 전송)");
    }
}

/*
 * Firmware(Object 0x2A) 상태 요청 처리.
 * PC(DM/Platinum)가 접속한 뒤 장치를 확인할 때 보내는 첫 명령이다
 * (`frmNetworkStatus.cs`의 SettingControllerFirmwareCheck = RequestStatus/Read/Firmware).
 * 장치 시각 확인(DeviceDateTimeCheck)도 같은 명령을 쓰므로, 함께 실리는 Device Status의
 * CurDateTime이 PC가 보는 장치 시각이 된다.
 *
 * 응답 Command는 SendStatus(0x03)로 보낸다 - 요청이 RequestStatus(0x04)이므로 Command Table의
 * Send/Request 짝(3<->4, 5<->6)을 History 응답(RequestData 0x06 -> SendData 0x05)과 같은 방식으로 맞춘 것.
 * 실제 장치가 무엇을 쓰는지는 PC와 붙여 확인할 것.
 */
static void handle_firmware_request(AcuNet *net, const IdtiHeader *hdr)
{
    uint8_t firmware[IDTI_FIRMWARE_INFO_LEN];
    build_firmware_info(firmware, net->device_category, net->device_type);

    send_response(net, hdr, IDTI_CMD_SND_STATUS, IDTI_SUBCMD_READ, IDTI_OBJ_FIRMWARE,
                  firmware, sizeof(firmware),
                  1, 1, 1, IDTI_FIRMWARE_INFO_LEN);
}

/*
 * LCD 관련 명령 4종에 답한다. **우리 장비에는 LCD가 없지만, SSC-324처럼 성공으로 답하고 내용은 버린다.**
 *
 * DM 규약 문서 5절 개정(2026-09-11) 권고를 따른다. "미지원(Fail)"으로 답하면 기존 장비와 화면 흐름이
 * 달라져 운영자가 고장으로 읽고, SSC-324와 섞여 도는 현장에서 같은 버튼이 장비마다 다르게 반응한다.
 * 목표가 기존 장비 대체이므로 "더 정직한 응답"보다 "기존 장비와 같은 응답"이 맞다.
 *
 *   LCDControl 확인     RequestData/Read/49   -> LCD 설정 40byte 기본값 (백라이트 Default, yyyyMMdd)
 *   LCDControl 변경     SendData/Change/49    -> Success(1), 받은 설정은 버림
 *   MultiLanguage 확인  RequestData/Read/165  -> 언어 English(1)
 *   MultiLanguage 변경  SendStatus/Change/165 -> Success(1), 받은 언어는 버림
 *
 * 응답 Command는 Interphone SDK의 장치 측 ACK처럼 요청의 Command/Sub/Object를 그대로 되돌린다.
 * 처리했으면 1, LCD 명령이 아니면 0을 돌려준다.
 */
static int handle_lcd_request(AcuNet *net, const IdtiHeader *hdr)
{
    int is_lcd_control = (hdr->object == IDTI_OBJ_LCD_CONTROL);
    int is_language = (hdr->object == IDTI_OBJ_MULTI_LANGUAGE);
    if (!is_lcd_control && !is_language)
    {
        return 0;
    }

    int is_query = (hdr->command == IDTI_CMD_REQ_DATA && hdr->sub_command == IDTI_SUBCMD_READ);

    uint8_t data[IDTI_LCD_INFO_LEN];
    size_t data_len = 1;
    const char *what;

    memset(data, 0, sizeof(data));
    if (is_lcd_control && is_query)
    {
        /* 시작·종료 시각 00:00, 사용자 날짜 형식 없음, 예약 0 — memset이 이미 채웠다 */
        data[0] = IDTI_LCD_BACKLIGHT_DEFAULT;
        data[5] = IDTI_LCD_DATEFORMAT_YYYYMMDD;
        data_len = IDTI_LCD_INFO_LEN;
        what = "LCD 설정 기본값 40byte";
    }
    else if (is_language && is_query)
    {
        data[0] = IDTI_LANGUAGE_ENGLISH;
        what = "언어 English(1)";
    }
    else
    {
        data[0] = IDTI_ACK_SUCCESS; /* 변경 요청: 성공으로 답하고 내용은 버린다 */
        what = "Success(1), 내용 버림";
    }

    send_response(net, hdr, hdr->command, hdr->sub_command, hdr->object,
                  data, data_len, 1, 1, 1, (uint16_t)data_len);

    char line[160];
    snprintf(line, sizeof(line), "네트워크: LCD 명령(cmd=0x%02x sub=0x%02x obj=0x%02x) - LCD 없음, %s 응답",
             hdr->command, hdr->sub_command, hdr->object, what);
    log_msg(line);
    return 1;
}

static void handle_request(AcuNet *net, const IdtiHeader *hdr, const uint8_t *pkt)
{
    /*
     * 시각 동기화를 응답보다 먼저 처리한다. 응답에 실리는 Device Status와 Event가 모두
     * 현재 시각을 쓰므로, 보정 후의 시각으로 답하는 편이 일관적이다.
     */
    if (hdr->frame_option & IDTI_FOPT_TIME_SYNC)
    {
        apply_time_sync(net, pkt + IDTI_HEADER_LEN_V2, hdr->data_len);
    }

    if (hdr->command == IDTI_CMD_REQ_DATA && hdr->sub_command == IDTI_SUBCMD_READ &&
        hdr->object == IDTI_OBJ_HISTORY)
    {
        handle_history_request(net, hdr);
        return;
    }

    if (hdr->command == IDTI_CMD_REQ_STATUS && hdr->sub_command == IDTI_SUBCMD_READ &&
        hdr->object == IDTI_OBJ_FIRMWARE)
    {
        handle_firmware_request(net, hdr);
        return;
    }

    if (handle_lcd_request(net, hdr))
    {
        return;
    }

    char line[96];
    snprintf(line, sizeof(line),
             "네트워크: 지원하지 않는 요청 (cmd=0x%02x sub=0x%02x obj=0x%02x) 무시",
             hdr->command, hdr->sub_command, hdr->object);
    log_msg(line);
}

/* 상위 시스템이 지금 붙어 있는지. netmodule IMIN의 Connect 필드에 실린다 */
int net_is_connected(const AcuNet *net)
{
    return (net && net->client_fd >= 0) ? 1 : 0;
}

/*
 * Device Status / Firmware Info에 실을 장치 식별자를 설정한다.
 * PC에 등록한 모델과 맞아야 한다 (예: SSC-324로 등록했으면 category=3, type=33).
 */
void net_set_device_identity(AcuNet *net, int category, int type)
{
    if (net)
    {
        net->device_category = category;
        net->device_type = type;
    }
}

/* 상위 시스템에 보고할 I/O 구성을 설정한다 */
void net_set_module_layout(AcuNet *net, const AcuModuleLayout *layout)
{
    if (net && layout)
    {
        net->layout = *layout;
    }
}

/* 상위 시스템이 보내는 시각으로 시계를 맞출지 설정한다 */
void net_set_time_sync(AcuNet *net, int enabled)
{
    if (net)
    {
        net->time_sync_enabled = enabled ? 1 : 0;
    }
}

/* 유휴 타임아웃(초)을 설정한다. 0이면 끈다 */
void net_set_inactivity_timeout(AcuNet *net, int seconds)
{
    if (net)
    {
        net->inactivity_seconds = (seconds > 0) ? seconds : 0;
    }
}

void net_set_aux_reader(AcuNet *net, int fd, void (*on_readable)(void *user), void *user)
{
    if (!net)
    {
        return;
    }
    net->aux_fd = fd;
    net->aux_on_readable = on_readable;
    net->aux_user = user;
}

void net_poll(AcuNet *net, int timeout_ms)
{
    if (!net)
    {
        return;
    }

    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(net->listen_fd, &readfds);
    int maxfd = net->listen_fd;
    if (net->aux_fd >= 0)
    {
        FD_SET(net->aux_fd, &readfds);
        if (net->aux_fd > maxfd)
        {
            maxfd = net->aux_fd;
        }
    }
    if (net->client_fd >= 0)
    {
        FD_SET(net->client_fd, &readfds);
        if (net->send_off < net->send_len)
        {
            FD_SET(net->client_fd, &writefds); /* 보내다 만 응답이 남아 있으면 쓰기 가능해질 때 이어 보낸다 */
        }
        if (net->client_fd > maxfd)
        {
            maxfd = net->client_fd;
        }
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(maxfd + 1, &readfds, &writefds, NULL, &tv);

    /*
     * 유휴 타임아웃 검사. select가 timeout으로 깨어난 경우에도 해야 하므로 rc 검사보다 앞에 둔다.
     * (아무 일도 일어나지 않는 것이 바로 우리가 잡으려는 상황이다)
     */
    if (net->client_fd >= 0 && net->inactivity_seconds > 0)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long idle = now.tv_sec - net->last_activity.tv_sec;
        if (idle >= net->inactivity_seconds)
        {
            char line[160];
            snprintf(line, sizeof(line),
                     "네트워크: %ld초 동안 조용해 연결을 닫는다 (InactivityTime %d초)",
                     idle, net->inactivity_seconds);
            net_drop_client(net, line);
        }
    }

    if (rc <= 0)
    {
        return;
    }

    if (net->aux_fd >= 0 && FD_ISSET(net->aux_fd, &readfds) && net->aux_on_readable)
    {
        net->aux_on_readable(net->aux_user);
    }

    if (net->client_fd >= 0 && FD_ISSET(net->client_fd, &writefds))
    {
        net_flush_send(net);
    }

    if (FD_ISSET(net->listen_fd, &readfds))
    {
        int new_fd = accept(net->listen_fd, NULL, NULL);
        if (new_fd >= 0)
        {
            if (net->client_fd >= 0)
            {
                net_drop_client(net, "네트워크: 기존 연결을 새 연결로 교체함 (동시 1개 연결만 지원)");
            }
            set_nonblocking(new_fd);
            net->client_fd = new_fd;
            net_touch_activity(net);
            net->recv_len = 0;
            net->send_len = 0;
            net->send_off = 0;
            log_msg("네트워크: 상위 시스템 연결됨");
        }
    }

    if (net->client_fd >= 0 && FD_ISSET(net->client_fd, &readfds))
    {
        ssize_t n = recv(net->client_fd, net->recv_buf + net->recv_len,
                          sizeof(net->recv_buf) - net->recv_len, 0);
        if (n <= 0)
        {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return;
            }
            net_drop_client(net, "네트워크: 상위 시스템 연결 끊김");
            return;
        }
        net->recv_len += (size_t)n;
        net_touch_activity(net);

        size_t consumed = 0;
        while (net->recv_len - consumed >= IDTI_HEADER_LEN_V2)
        {
            IdtiHeader hdr;
            const uint8_t *pkt = net->recv_buf + consumed;
            size_t avail = net->recv_len - consumed;

            if (idti_header_parse(pkt, avail, &hdr) != 0)
            {
                log_msg("네트워크: 잘못된 패킷 헤더 수신 -> 수신 버퍼 초기화");
                consumed = net->recv_len;
                break;
            }
            if (hdr.packet_length > NET_RECV_BUF_CAP)
            {
                log_msg("네트워크: 패킷 길이가 수신 버퍼보다 커서 버림");
                consumed = net->recv_len;
                break;
            }
            if (avail < hdr.packet_length)
            {
                break; /* 아직 패킷 전체가 도착하지 않음 */
            }

            handle_request(net, &hdr, pkt);
            consumed += hdr.packet_length;
        }

        if (consumed > 0)
        {
            memmove(net->recv_buf, net->recv_buf + consumed, net->recv_len - consumed);
            net->recv_len -= consumed;
        }
    }
}
