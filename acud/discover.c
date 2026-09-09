#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  /* getifaddrs */

#include "discover.h"
#include "log.h"

#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * netmodule 프로토콜 (PC 소스 islnetmodule에서 확인)
 *
 *   PC   -> 장치 : UDP 255.255.255.255 : 1460
 *   장치 -> PC   : UDP 255.255.255.255 : 5001
 *
 * 응답을 유니캐스트가 아니라 **브로드캐스트로 보내는 이유**: 장치 IP가 PC와 다른 대역에
 * 잘못 잡혀 있을 수 있다. 그게 바로 탐색이 필요한 상황이다. 유니캐스트로 답하면
 * 라우팅이 없어 나가지 못한다. 브로드캐스트는 인터페이스로 그냥 나간다.
 */
#define NM_PORT_LISTEN 1460
#define NM_PORT_REPLY  5001

#define NM_CMD_LEN      4
#define NM_FIND_REQ_LEN 4
#define NM_FIND_ACK_LEN 50   /* IMIN */
#define NM_SET_REQ_LEN  58   /* SETT = IMIN 50 + 비밀번호 4+4 */
#define NM_SET_ACK_LEN  58   /* SETC / FAIL */

#define NM_CMD_FIND "FIND"
#define NM_CMD_IMIN "IMIN"
#define NM_CMD_SETT "SETT"
#define NM_CMD_FAIL "FAIL"

/* 수신 버퍼. 규격상 가장 긴 요청이 58byte라 넉넉하다 */
#define NM_RECV_CAP 256

/*
 * TcpMode: Client=0 / Mixed=1 / Server=2 (clsnmParams.NetworkMode).
 * 우리 ACU는 PC가 접속해 오는 서버다.
 */
#define NM_TCPMODE_SERVER 2

/*
 * Serial 계열 필드는 시리얼-이더넷 변환 모듈 시절의 잔재다. 우리는 시리얼을 쓰지 않으므로
 * 그럴듯한 고정값을 채워 보낸다 (PC 화면에 표시만 될 뿐 동작에 영향이 없다).
 * SerialBPS는 비선형 코드다: 244=9600, 250=19200, 253=38400, 254=57600, 255=115200
 */
#define NM_SERIAL_BPS_115200 255
#define NM_SERIAL_DATABIT    8
#define NM_SERIAL_PARITY     0
#define NM_SERIAL_STOPBIT    1
#define NM_SERIAL_FLOW       0

/* FirmwareVersion은 2byte("주.부") */
#define NM_FW_MAJOR 1
#define NM_FW_MINOR 0

struct AcuDiscover {
    int  fd;
    int  tcp_port;
    char iface[IFNAMSIZ];
    unsigned ifindex; /* 0이면 인터페이스 필터를 걸지 않는다 */
};

/* 값을 빅엔디안 2byte로 쓴다 (netmodule의 CalcByteLengthToByte와 동일) */
static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

/* /sys/class/net/<iface>/address 에서 MAC 6byte를 읽는다. 실패하면 0으로 채우고 -1 */
static int read_mac(const char *iface, unsigned char out[6])
{
    memset(out, 0, 6);

    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return -1;
    }

    unsigned int b[6];
    int n = fscanf(fp, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    fclose(fp);

    if (n != 6)
    {
        return -1;
    }
    for (int i = 0; i < 6; i++)
    {
        out[i] = (unsigned char)b[i];
    }
    return 0;
}

/* 인터페이스의 IPv4 주소와 넷마스크를 읽는다. 없으면 0.0.0.0으로 두고 -1 */
static int read_ipv4(const char *iface, unsigned char ip[4], unsigned char mask[4])
{
    memset(ip, 0, 4);
    memset(mask, 0, 4);

    struct ifaddrs *head = NULL;
    if (getifaddrs(&head) != 0)
    {
        return -1;
    }

    int found = -1;
    for (struct ifaddrs *it = head; it; it = it->ifa_next)
    {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if (strcmp(it->ifa_name, iface) != 0)
        {
            continue;
        }

        struct sockaddr_in *sa = (struct sockaddr_in *)it->ifa_addr;
        memcpy(ip, &sa->sin_addr.s_addr, 4); /* s_addr은 이미 네트워크 바이트 순서 */

        if (it->ifa_netmask)
        {
            struct sockaddr_in *nm = (struct sockaddr_in *)it->ifa_netmask;
            memcpy(mask, &nm->sin_addr.s_addr, 4);
        }
        found = 0;
        break;
    }

    freeifaddrs(head);
    return found;
}

/*
 * /proc/net/route에서 해당 인터페이스의 기본 경로 게이트웨이를 읽는다.
 *
 *   Iface  Destination  Gateway   Flags ...
 *   eth0   00000000     0100A8C0  0003  ...
 *
 * Gateway 열은 in_addr의 s_addr을 호스트 정수로 찍은 값이다. 리틀엔디안 기계에서
 * 0x0100A8C0은 메모리상 C0 A8 00 01 = 192.168.0.1 이 된다.
 * ARM64/x86 리눅스는 모두 리틀엔디안이므로 아래처럼 하위 바이트부터 꺼낸다.
 */
static int read_gateway(const char *iface, unsigned char gw[4])
{
    memset(gw, 0, 4);

    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp)
    {
        return -1;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) /* 헤더 한 줄 버림 */
    {
        fclose(fp);
        return -1;
    }

    int found = -1;
    while (fgets(line, sizeof(line), fp))
    {
        char name[IFNAMSIZ];
        unsigned long dest = 0, gateway = 0;

        if (sscanf(line, "%15s %lx %lx", name, &dest, &gateway) != 3)
        {
            continue;
        }
        if (dest != 0 || strcmp(name, iface) != 0)
        {
            continue; /* 기본 경로(destination 0.0.0.0)만 본다 */
        }

        gw[0] = (unsigned char)(gateway & 0xFF);
        gw[1] = (unsigned char)((gateway >> 8) & 0xFF);
        gw[2] = (unsigned char)((gateway >> 16) & 0xFF);
        gw[3] = (unsigned char)((gateway >> 24) & 0xFF);
        found = 0;
        break;
    }

    fclose(fp);
    return found;
}

/*
 * 설정 프레임 50byte를 만든다. cmd4는 "IMIN" 또는 "FAIL" 같은 4byte 명령.
 * 필드 순서는 clsnmSettingFrame.BuildBytesSetParams()와 같다.
 */
static void build_setting_frame(const AcuDiscover *d, const char *cmd4, unsigned char out[NM_FIND_ACK_LEN])
{
    memset(out, 0, NM_FIND_ACK_LEN);

    unsigned char mac[6], ip[4], mask[4], gw[4];
    if (read_mac(d->iface, mac) != 0)
    {
        char w[160];
        snprintf(w, sizeof(w), "탐색: %s MAC을 읽지 못했다 - 0으로 보고한다", d->iface);
        log_msg(w);
    }
    if (read_ipv4(d->iface, ip, mask) != 0)
    {
        /*
         * 조용히 0.0.0.0을 보고하면 PC 화면에는 장비가 보이는데 주소가 비어 있어
         * 원인을 찾기 어렵다. getifaddrs()는 AF_NETLINK 소켓을 쓰므로, systemd 유닛의
         * RestrictAddressFamilies에 AF_NETLINK이 빠져 있으면 여기서 실패한다.
         */
        char w[200];
        snprintf(w, sizeof(w),
                 "탐색: %s IPv4 주소를 읽지 못했다 (getifaddrs 실패) - 0.0.0.0으로 보고한다. "
                 "systemd RestrictAddressFamilies에 AF_NETLINK이 있는지 확인할 것", d->iface);
        log_msg(w);
    }
    read_gateway(d->iface, gw);

    size_t o = 0;
    memcpy(out + o, cmd4, NM_CMD_LEN);          o += NM_CMD_LEN;   /* [0..3]   Command */
    memcpy(out + o, mac, 6);                    o += 6;            /* [4..9]   MacAddress */
    out[o++] = NM_TCPMODE_SERVER;                                  /* [10]     TcpMode */
    memcpy(out + o, ip, 4);                     o += 4;            /* [11..14] RemoteIP (장치 자신) */
    memcpy(out + o, mask, 4);                   o += 4;            /* [15..18] SubnetMask */
    memcpy(out + o, gw, 4);                     o += 4;            /* [19..22] GateWay */
    put_be16(out + o, (unsigned)d->tcp_port);   o += 2;            /* [23..24] RemotePort */
    o += 4;                                                        /* [25..28] PeerIP - 서버라 없음(0) */
    o += 2;                                                        /* [29..30] PeerPort */
    out[o++] = NM_SERIAL_BPS_115200;                               /* [31]     SerialBPS */
    out[o++] = NM_SERIAL_DATABIT;                                  /* [32]     Databit */
    out[o++] = NM_SERIAL_PARITY;                                   /* [33]     Parity */
    out[o++] = NM_SERIAL_STOPBIT;                                  /* [34]     Stopbit */
    out[o++] = NM_SERIAL_FLOW;                                     /* [35]     Flow */
    o += 1;                                                        /* [36]     DatapackingChar */
    o += 2;                                                        /* [37..38] DatapackingSize */
    o += 2;                                                        /* [39..40] DatapackingTime */
    o += 2;                                                        /* [41..42] InactivityTime */
    o += 1;                                                        /* [43]     SerialDebugMode */
    out[o++] = NM_FW_MAJOR;                                        /* [44..45] FirmwareVersion */
    out[o++] = NM_FW_MINOR;
    o += 1;                                                        /* [46]     DhcpMode (0=고정) */
    o += 1;                                                        /* [47]     UdpMode */
    o += 1;                                                        /* [48]     Connect */
    o += 1;                                                        /* [49]     PasswordSetFlag */

    /* 위 오프셋이 규격과 어긋나면 PC가 엉뚱하게 해석한다 */
    if (o != NM_FIND_ACK_LEN)
    {
        log_msg("탐색: 내부 오류 - 응답 프레임 길이가 50byte가 아니다");
    }
}

/* 255.255.255.255:5001 로 브로드캐스트 응답 */
static void send_reply(const AcuDiscover *d, const unsigned char *buf, size_t len)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(NM_PORT_REPLY);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    ssize_t n = sendto(d->fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
    if (n < 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "탐색: 응답 전송 실패 (%s)", strerror(errno));
        log_msg(line);
    }
}

AcuDiscover *discover_init(const char *iface, int tcp_port)
{
    AcuDiscover *d = calloc(1, sizeof(*d));
    if (!d)
    {
        return NULL;
    }

    snprintf(d->iface, sizeof(d->iface), "%s", (iface && iface[0]) ? iface : "eth0");
    d->tcp_port = tcp_port;

    d->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (d->fd < 0)
    {
        log_msg("탐색: UDP 소켓 생성 실패");
        free(d);
        return NULL;
    }

    int on = 1;
    /* 응답을 브로드캐스트로 보내려면 필요하다 */
    if (setsockopt(d->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0)
    {
        log_msg("탐색: SO_BROADCAST 설정 실패 - 응답이 나가지 못할 수 있다");
    }
    setsockopt(d->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /*
     * 어느 인터페이스로 들어온 요청인지 알기 위해 필요하다.
     * 보드에 eth0와 wlan0가 같은 대역에 함께 떠 있으면 브로드캐스트가 양쪽으로 들어와
     * 한 번의 FIND에 여러 번 답하게 된다. 우리는 eth0 설정을 보고하므로 eth0로 들어온 것만 답한다.
     * (SO_BINDTODEVICE는 CAP_NET_RAW가 필요해 쓰지 않는다)
     */
    if (setsockopt(d->fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) != 0)
    {
        log_msg("탐색: IP_PKTINFO 설정 실패 - 인터페이스 구분 없이 응답한다");
    }

    d->ifindex = if_nametoindex(d->iface);
    if (d->ifindex == 0)
    {
        char w[160];
        snprintf(w, sizeof(w), "탐색: %s 인터페이스를 찾지 못했다 - 모든 인터페이스에 응답한다", d->iface);
        log_msg(w);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NM_PORT_LISTEN);

    if (bind(d->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "탐색: UDP %d 바인드 실패 (%s)", NM_PORT_LISTEN, strerror(errno));
        log_msg(line);
        close(d->fd);
        free(d);
        return NULL;
    }

    char line[160];
    snprintf(line, sizeof(line),
             "탐색(UDP) 시작 - %s 정보를 포트 %d에서 대기, 응답은 브로드캐스트 %d",
             d->iface, NM_PORT_LISTEN, NM_PORT_REPLY);
    log_msg(line);
    return d;
}

void discover_shutdown(AcuDiscover *d)
{
    if (!d)
    {
        return;
    }
    if (d->fd >= 0)
    {
        close(d->fd);
    }
    free(d);
}

int discover_fd(const AcuDiscover *d)
{
    return d ? d->fd : -1;
}

void discover_set_tcp_port(AcuDiscover *d, int tcp_port)
{
    if (d)
    {
        d->tcp_port = tcp_port;
    }
}

void discover_service(AcuDiscover *d)
{
    if (!d)
    {
        return;
    }

    unsigned char buf[NM_RECV_CAP];
    struct sockaddr_in from;
    unsigned char cmsgbuf[256];

    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t n = recvmsg(d->fd, &msg, 0);
    if (n < NM_CMD_LEN)
    {
        return; /* 너무 짧으면 우리 프로토콜이 아니다 */
    }

    /* 우리가 설정을 보고하는 인터페이스로 들어온 요청만 처리한다 */
    if (d->ifindex != 0)
    {
        unsigned in_ifindex = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        {
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
            {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(c), sizeof(pi));
                in_ifindex = (unsigned)pi.ipi_ifindex;
                break;
            }
        }
        if (in_ifindex != 0 && in_ifindex != d->ifindex)
        {
            return; /* 다른 인터페이스로 들어온 브로드캐스트 - 조용히 버린다 */
        }
    }

    char who[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &from.sin_addr, who, sizeof(who));

    if (n == NM_FIND_REQ_LEN && memcmp(buf, NM_CMD_FIND, NM_CMD_LEN) == 0)
    {
        unsigned char ack[NM_FIND_ACK_LEN];
        build_setting_frame(d, NM_CMD_IMIN, ack);
        send_reply(d, ack, sizeof(ack));

        char line[160];
        snprintf(line, sizeof(line), "탐색: FIND 수신 (%s) -> IMIN %d byte 응답", who, NM_FIND_ACK_LEN);
        log_msg(line);
        return;
    }

    if (n == NM_SET_REQ_LEN && memcmp(buf, NM_CMD_SETT, NM_CMD_LEN) == 0)
    {
        /*
         * 설정 변경은 아직 구현하지 않았다. 무응답으로 두면 PC가 타임아웃까지 기다리므로
         * FAIL로 명확히 답한다. FAIL 응답도 58byte(설정 프레임 50 + 비밀번호 8)다.
         */
        unsigned char ack[NM_SET_ACK_LEN];
        memset(ack, 0, sizeof(ack));
        build_setting_frame(d, NM_CMD_FAIL, ack);
        send_reply(d, ack, sizeof(ack));

        char line[200];
        snprintf(line, sizeof(line),
                 "탐색: SETT 수신 (%s) - 설정 변경은 미구현이라 FAIL로 응답", who);
        log_msg(line);
        return;
    }

    char line[200];
    snprintf(line, sizeof(line), "탐색: 알 수 없는 요청 %ld byte (%s) - 무시", (long)n, who);
    log_msg(line);
}

void discover_wait(AcuDiscover *d, int timeout_ms)
{
    if (!d || d->fd < 0)
    {
        return;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(d->fd, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(d->fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(d->fd, &rfds))
    {
        discover_service(d);
    }
}
