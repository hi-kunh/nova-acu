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

    int door_status; /* IDTI_DOOR_STATUS_*, Device Status 응답에 반영 */

    NetEvent queue[NET_EVENT_QUEUE_CAP];
    size_t queue_head;
    size_t queue_count;
};

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
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
static void build_device_status(uint8_t out[IDTI_DEVICE_STATUS_V2_LEN], int door_status, time_t now)
{
    (void)door_status; /* V2 Device Status 구조체 자체에는 도어 상태 필드가 없음(Event Info 쪽에만 존재) */

    memset(out, 0, IDTI_DEVICE_STATUS_V2_LEN);
    out[0] = (uint8_t)(IDTI_DEVICE_TYPE_ISC101 >> 8);
    out[1] = (uint8_t)(IDTI_DEVICE_TYPE_ISC101 & 0xFF);

    struct tm tmv;
    localtime_r(&now, &tmv);
    out[2] = idti_to_bcd((tmv.tm_year + 1900) % 100);
    out[3] = idti_to_bcd(tmv.tm_mon + 1);
    out[4] = idti_to_bcd(tmv.tm_mday);
    out[5] = idti_to_bcd(tmv.tm_hour);
    out[6] = idti_to_bcd(tmv.tm_min);
    out[7] = idti_to_bcd(tmv.tm_sec);

    /* out[8..9] ExistedModule = 0 (확장 IO 모듈 없음), out[10..233] IOModuleStatus = 0 (memset로 처리됨) */
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

static void handle_request(AcuNet *net, const IdtiHeader *hdr)
{
    if (!(hdr->command == IDTI_CMD_REQ_DATA && hdr->sub_command == IDTI_SUBCMD_READ &&
          hdr->object == IDTI_OBJ_HISTORY))
    {
        char line[96];
        snprintf(line, sizeof(line),
                 "네트워크: 지원하지 않는 요청 (cmd=0x%02x sub=0x%02x obj=0x%02x) 무시",
                 hdr->command, hdr->sub_command, hdr->object);
        log_msg(line);
        return;
    }

    uint8_t payload[IDTI_DEVICE_STATUS_V2_LEN + IDTI_EVENT_INFO_LEN];
    build_device_status(payload, net->door_status, time(NULL));
    size_t payload_len = IDTI_DEVICE_STATUS_V2_LEN;

    uint16_t cur = 0, end = 0, total = 0, one_len = 0;

    if (net->queue_count > 0)
    {
        build_event_info(payload + IDTI_DEVICE_STATUS_V2_LEN, &net->queue[net->queue_head]);
        payload_len += IDTI_EVENT_INFO_LEN;
        cur = 1; end = 1; total = 1;
        one_len = IDTI_EVENT_INFO_LEN;

        net->queue_head = (net->queue_head + 1) % NET_EVENT_QUEUE_CAP;
        net->queue_count--;
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

    uint8_t out[512];
    int n = idti_build_packet(out, sizeof(out), dest_addr, src_addr,
                               hdr->frame_index, hdr->password,
                               IDTI_CMD_SND_DATA, IDTI_SUBCMD_READ, IDTI_OBJ_HISTORY,
                               hdr->start_item, hdr->end_item,
                               cur, end, total, one_len,
                               payload, payload_len);
    if (n > 0 && net->client_fd >= 0)
    {
        send(net->client_fd, out, (size_t)n, 0);
    }
}

void net_poll(AcuNet *net, int timeout_ms)
{
    if (!net)
    {
        return;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(net->listen_fd, &readfds);
    int maxfd = net->listen_fd;
    if (net->client_fd >= 0)
    {
        FD_SET(net->client_fd, &readfds);
        if (net->client_fd > maxfd)
        {
            maxfd = net->client_fd;
        }
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (rc <= 0)
    {
        return;
    }

    if (FD_ISSET(net->listen_fd, &readfds))
    {
        int new_fd = accept(net->listen_fd, NULL, NULL);
        if (new_fd >= 0)
        {
            if (net->client_fd >= 0)
            {
                close(net->client_fd);
                log_msg("네트워크: 기존 연결을 새 연결로 교체함 (동시 1개 연결만 지원)");
            }
            set_nonblocking(new_fd);
            net->client_fd = new_fd;
            net->recv_len = 0;
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
            close(net->client_fd);
            net->client_fd = -1;
            net->recv_len = 0;
            log_msg("네트워크: 상위 시스템 연결 끊김");
            return;
        }
        net->recv_len += (size_t)n;

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

            handle_request(net, &hdr);
            consumed += hdr.packet_length;
        }

        if (consumed > 0)
        {
            memmove(net->recv_buf, net->recv_buf + consumed, net->recv_len - consumed);
            net->recv_len -= consumed;
        }
    }
}
