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
};

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

    int event_attached = 0;
    if (net->queue_count > 0)
    {
        /* 큐에서 꺼내는 것은 전송에 성공한 뒤에 한다 (전송이 실패하면 이벤트가 유실되므로) */
        build_event_info(payload + IDTI_DEVICE_STATUS_V2_LEN, &net->queue[net->queue_head]);
        payload_len += IDTI_EVENT_INFO_LEN;
        cur = 1; end = 1; total = 1;
        one_len = IDTI_EVENT_INFO_LEN;
        event_attached = 1;
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
    if (n <= 0)
    {
        log_msg("네트워크: 응답 패킷 생성 실패");
        return;
    }

    if (net_queue_send(net, out, (size_t)n) == 0)
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
    if (rc <= 0)
    {
        return;
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
