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

/* ------------------------------------------------------------------ */
/* 프로토콜 상수 (PC 소스 islnetmodule에서 확인)                        */
/* ------------------------------------------------------------------ */

/*
 *   PC   -> 장치 : UDP 255.255.255.255 : 1460
 *   장치 -> PC   : UDP 255.255.255.255 : 5001
 *
 * 응답을 유니캐스트가 아니라 브로드캐스트로 보내는 이유: 장치 IP가 PC와 다른 대역에
 * 잘못 잡혀 있을 수 있고, 그게 바로 탐색이 필요한 상황이다. 유니캐스트로 답하면
 * 라우팅이 없어 나가지 못한다. 브로드캐스트는 인터페이스로 그냥 나간다.
 */
#define NM_PORT_LISTEN 1460
#define NM_PORT_REPLY  5001

#define NM_CMD_LEN       4
#define NM_FRAME_LEN     50  /* FIND 응답(IMIN) 및 SETT의 앞부분 */
#define NM_FRAME_PW_LEN  58  /* NM_FRAME_LEN + 비밀번호 4 + 4 */
#define NM_FIND_REQ_LEN  4   /* "FIND" 네 글자가 전부 */

#define NM_CMD_FIND "FIND"
#define NM_CMD_IMIN "IMIN"
#define NM_CMD_SETT "SETT"
#define NM_CMD_SETC "SETC"
#define NM_CMD_FAIL "FAIL"

/* 수신 버퍼. 규격상 가장 긴 요청이 58byte라 넉넉하다 */
#define NM_RECV_CAP 256

/*
 * 설정 프레임의 필드 오프셋.
 * 프레임을 만들 때와 읽을 때가 **같은 표**를 보게 해서, 한쪽만 고쳐 어긋나는 일을 막는다.
 * 순서/길이 근거는 clsnmSettingFrame.BuildBytesSetParams().
 */
enum {
    NM_OFF_COMMAND        = 0,   /* 4byte ASCII */
    NM_OFF_MAC            = 4,   /* 6 */
    NM_OFF_TCP_MODE       = 10,  /* 1 */
    NM_OFF_IP             = 11,  /* 4  장치 자신의 IP (원문 이름은 RemoteIPAddress) */
    NM_OFF_NETMASK        = 15,  /* 4 */
    NM_OFF_GATEWAY        = 19,  /* 4 */
    NM_OFF_PORT           = 23,  /* 2  빅엔디안 */
    NM_OFF_PEER_IP        = 25,  /* 4  접속해 갈 상위 PC (우리는 서버라 사용 안 함) */
    NM_OFF_PEER_PORT      = 29,  /* 2 */
    NM_OFF_SERIAL_BPS     = 31,  /* 1 */
    NM_OFF_SERIAL_DATABIT = 32,  /* 1 */
    NM_OFF_SERIAL_PARITY  = 33,  /* 1 */
    NM_OFF_SERIAL_STOPBIT = 34,  /* 1 */
    NM_OFF_SERIAL_FLOW    = 35,  /* 1 */
    NM_OFF_DP_CHAR        = 36,  /* 1 */
    NM_OFF_DP_SIZE        = 37,  /* 2 */
    NM_OFF_DP_TIME        = 39,  /* 2 */
    NM_OFF_INACTIVITY     = 41,  /* 2 */
    NM_OFF_DEBUG          = 43,  /* 1 */
    NM_OFF_FIRMWARE       = 44,  /* 2  "주.부" */
    NM_OFF_DHCP           = 46,  /* 1 */
    NM_OFF_UDP            = 47,  /* 1 */
    NM_OFF_CONNECT        = 48,  /* 1 */
    NM_OFF_PW_SET_FLAG    = 49,  /* 1  Off=0 / On=1 */
    NM_OFF_PW_COMPANY     = 50,  /* 4  SETT에만 있음. "IDTi" 고정 */
    NM_OFF_PW_CUSTOM      = 54   /* 4  SETT에만 있음. 단말기 비밀번호 */
};

/* TcpMode: Client=0 / Mixed=1 / Server=2. 우리 ACU는 PC가 접속해 오는 서버다 */
#define NM_TCPMODE_SERVER 2

/* PasswordSetFlag가 On일 때만 PC가 비밀번호를 실어 보낸다 */
#define NM_PW_FLAG_ON  1
#define NM_PW_FLAG_OFF 0
#define NM_PW_COMPANY  "IDTi"   /* clsnmSettingFrame의 asciiCompanyFixedPasswordValue */

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

/* 넷마스크로 받아들일 prefix 범위. acu-netcfg의 검증 범위와 맞춘다 */
#define NM_PREFIX_MIN 8
#define NM_PREFIX_MAX 30

struct AcuDiscover {
    int      fd;
    int      tcp_port;
    char     iface[IFNAMSIZ];
    unsigned ifindex;                 /* 0이면 인터페이스 필터를 걸지 않는다 */
    char     sett_password[8];        /* SETT에 요구할 비밀번호. 빈 문자열이면 요구하지 않는다 */
    int      inactivity_seconds;      /* IMIN에 실을 InactivityTime */
    int      connected;               /* 상위 시스템이 붙어 있는지. main이 카드 조회 주기(2초)마다 갱신하므로
                                       * 최대 2초까지 실제 상태보다 늦을 수 있다. 사람이 조회하는
                                       * 상태 필드라 이 정도 지연은 문제되지 않는다 */
    char     request_path[256];       /* SETT를 받아 적을 파일. 빈 문자열이면 SETT 거절 */
};

/* ------------------------------------------------------------------ */
/* 작은 유틸                                                           */
/* ------------------------------------------------------------------ */

/* 값을 빅엔디안 2byte로 쓴다 (netmodule의 CalcByteLengthToByte와 같은 순서) */
static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

/* 빅엔디안 2byte를 읽는다 */
static unsigned get_be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

/* 4byte IPv4를 "a.b.c.d" 문자열로 만든다 */
static void ip_to_text(const unsigned char ip[4], char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/*
 * 넷마스크 4byte를 prefix 길이로 바꾼다.
 * 1이 앞쪽에 연속으로 몰려 있어야 유효하다 (255.255.0.255 같은 것은 거절).
 * 유효하지 않으면 -1.
 */
static int netmask_to_prefix(const unsigned char mask[4])
{
    unsigned long m = ((unsigned long)mask[0] << 24) | ((unsigned long)mask[1] << 16) |
                      ((unsigned long)mask[2] << 8)  | (unsigned long)mask[3];

    int bits = 0;
    while (bits < 32 && (m & (1UL << (31 - bits))))
    {
        bits++;
    }

    /* 남은 자리에 1이 하나라도 있으면 연속이 아니다 */
    unsigned long rest = (bits == 32) ? 0UL : (m & ((1UL << (32 - bits)) - 1));
    return (rest == 0) ? bits : -1;
}

/* 두 IPv4가 같은 prefix 대역에 있는지 본다 */
static int same_subnet(const unsigned char a[4], const unsigned char b[4], int prefix)
{
    for (int i = 0; i < prefix; i++)
    {
        int byte = i / 8;
        int bit  = 7 - (i % 8);
        if (((a[byte] >> bit) & 1) != ((b[byte] >> bit) & 1))
        {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 장치 정보 읽기                                                      */
/* ------------------------------------------------------------------ */

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
 * ARM64/x86 리눅스는 모두 리틀엔디안이므로 하위 바이트부터 꺼낸다.
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

/* ------------------------------------------------------------------ */
/* 프레임 만들기                                                       */
/* ------------------------------------------------------------------ */

/*
 * 현재 장치 상태를 담은 설정 프레임 50byte를 만든다.
 * cmd4는 "IMIN"(탐색 응답) / "SETC"(설정 성공) / "FAIL"(설정 거절) 중 하나.
 */
static void build_frame(const AcuDiscover *d, const char *cmd4, unsigned char out[NM_FRAME_LEN])
{
    memset(out, 0, NM_FRAME_LEN);

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
         * 조용히 0.0.0.0을 보고하면 PC 화면에는 장비가 보이는데 주소만 비어 있어
         * 원인을 찾기 어렵다. getifaddrs()는 AF_NETLINK 소켓을 쓰므로, systemd 유닛의
         * RestrictAddressFamilies에 AF_NETLINK이 빠져 있으면 여기서 실패한다.
         */
        char w[220];
        snprintf(w, sizeof(w),
                 "탐색: %s IPv4 주소를 읽지 못했다 (getifaddrs 실패) - 0.0.0.0으로 보고한다. "
                 "systemd RestrictAddressFamilies에 AF_NETLINK이 있는지 확인할 것", d->iface);
        log_msg(w);
    }
    read_gateway(d->iface, gw);

    memcpy(out + NM_OFF_COMMAND, cmd4, NM_CMD_LEN);
    memcpy(out + NM_OFF_MAC, mac, 6);
    out[NM_OFF_TCP_MODE] = NM_TCPMODE_SERVER;
    memcpy(out + NM_OFF_IP, ip, 4);
    memcpy(out + NM_OFF_NETMASK, mask, 4);
    memcpy(out + NM_OFF_GATEWAY, gw, 4);
    put_be16(out + NM_OFF_PORT, (unsigned)d->tcp_port);
    /* PeerIP/PeerPort는 0. 우리는 서버라 접속해 갈 대상이 없다 */

    out[NM_OFF_SERIAL_BPS]     = NM_SERIAL_BPS_115200;
    out[NM_OFF_SERIAL_DATABIT] = NM_SERIAL_DATABIT;
    out[NM_OFF_SERIAL_PARITY]  = NM_SERIAL_PARITY;
    out[NM_OFF_SERIAL_STOPBIT] = NM_SERIAL_STOPBIT;
    out[NM_OFF_SERIAL_FLOW]    = NM_SERIAL_FLOW;

    put_be16(out + NM_OFF_INACTIVITY, (unsigned)d->inactivity_seconds);

    out[NM_OFF_FIRMWARE]     = NM_FW_MAJOR;
    out[NM_OFF_FIRMWARE + 1] = NM_FW_MINOR;

    /*
     * DhcpMode는 0(고정) 고정이다. 우리는 DHCP를 지원하지 않는다 - 제품은 고정 IP를 쓰고,
     * SETT의 DhcpMode도 적용하지 않는다. 지원하게 되면 그때 실제 상태를 채워야 한다.
     */

    /* 상위 시스템이 붙어 있는지 실제 상태를 보고한다 */
    out[NM_OFF_CONNECT] = (unsigned char)(d->connected ? 1 : 0);

    /*
     * 비밀번호를 실제로 요구할 때만 1로 보고한다.
     * 기존 IntelliScan Device Manager는 이 값을 IMIN에서 읽어 SETT에 그대로 실어 보내지만,
     * **Custom 비밀번호를 채울 수단이 없다**(UI에 입력란이 없고 소스에서도 대입하지 않는다).
     * 그래서 우리가 1로 보고해도 도구는 Company("IDTi")만 보내고 Custom은 0으로 채운다.
     * 요구하지 않을 때 1로 보고하는 것은 거짓말이므로 설정에 맞춰 정직하게 보고한다.
     */
    out[NM_OFF_PW_SET_FLAG] = (d->sett_password[0] != '\0') ? NM_PW_FLAG_ON : NM_PW_FLAG_OFF;
}

/* 255.255.255.255:5001 로 브로드캐스트 응답을 보낸다 */
static void send_reply(const AcuDiscover *d, const unsigned char *buf, size_t len)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(NM_PORT_REPLY);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (sendto(d->fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "탐색: 응답 전송 실패 (%s)", strerror(errno));
        log_msg(line);
    }
}

/* 설정 요청에 대한 응답(SETC 또는 FAIL) 58byte를 보낸다 */
static void send_set_reply(const AcuDiscover *d, const char *cmd4)
{
    unsigned char ack[NM_FRAME_PW_LEN];
    memset(ack, 0, sizeof(ack));
    build_frame(d, cmd4, ack); /* 앞 50byte만 채우고 비밀번호 8byte는 0으로 둔다 */
    send_reply(d, ack, sizeof(ack));
}

/* ------------------------------------------------------------------ */
/* SETT (설정 변경) 처리                                                */
/* ------------------------------------------------------------------ */

/*
 * SETT가 우리에게 온 것인지 본다.
 * PC는 MAC으로 장치를 지목하므로, 다른 장치 앞으로 온 브로드캐스트는 우리가 답하면 안 된다.
 */
static int sett_is_for_us(const AcuDiscover *d, const unsigned char *req)
{
    unsigned char mine[6];
    if (read_mac(d->iface, mine) != 0)
    {
        return 0; /* 내 MAC을 모르면 남의 것인지 판단할 수 없다 - 답하지 않는다 */
    }
    return memcmp(req + NM_OFF_MAC, mine, 6) == 0;
}

/*
 * SETT의 비밀번호를 검증한다. 맞으면 0, 아니면 -1.
 * 설정에 비밀번호가 없으면(기본) 아무것도 요구하지 않는다.
 *
 * **Company("IDTi")를 검사하지 않는 이유** (2026-09-10 실제 도구로 확인):
 * 도구는 `SetPasswordCompany("IDTi", passSetFlag)`로 Company를 채우는데, 그 `passSetFlag`는
 * **우리가 IMIN으로 보낸 값을 그대로 되돌려준 것**이다. 우리가 0을 보고하면 도구는 Company도
 * 0으로 채워 보낸다. 즉 Company는 인증 수단이 아니라 우리 응답의 메아리라서 검사 대상이 못 된다.
 *
 * **Custom도 기본으로는 요구하지 않는 이유**: 기존 IntelliScan Device Manager는 Custom 비밀번호를
 * 보낼 수단이 아예 없다. UI에 입력란이 없고, `frmNetworkModule.cs`가 `PassCustom`을 한 번도
 * 대입하지 않으며, 비밀번호 설정 명령(`PASS`)은 소스에 `// Later~`만 있는 미구현 상태다.
 * 요구하면 **제품이 기존 도구로 설정되지 않는다.**
 *
 * **그래서 실질적인 필터는 프레임 자체다**: 정확히 58byte이고, 명령이 `SETT`이고,
 * **우리 MAC을 지목**해야 한다(`sett_is_for_us`). 고정 문자열보다 이쪽이 훨씬 강한 조건이다.
 *
 * 비밀번호를 설정하면 Custom 일치를 요구하고 IMIN의 PasswordSetFlag도 1로 보고한다.
 * 그 대신 기존 Device Manager로는 SETT를 못 하게 되는 것을 감수한다는 뜻이 된다.
 */
static int sett_password_ok(const AcuDiscover *d, const unsigned char *req)
{
    if (d->sett_password[0] == '\0')
    {
        return 0; /* 비밀번호를 요구하지 않는 설정 (기본) */
    }

    if (memcmp(req + NM_OFF_PW_COMPANY, NM_PW_COMPANY, 4) != 0)
    {
        return -1;
    }

    char custom[5];
    memcpy(custom, req + NM_OFF_PW_CUSTOM, 4);
    custom[4] = '\0';

    return (strncmp(custom, d->sett_password, 4) == 0) ? 0 : -1;
}

/*
 * SETT가 실어 온 네트워크 설정이 쓸 만한지 본다.
 * 문제가 없으면 NULL, 있으면 거절 사유 문자열을 돌려준다 (로그에 그대로 쓴다).
 * prefix는 넷마스크에서 계산해 out_prefix로 넘긴다.
 */
static const char *sett_validate(const unsigned char *req, int *out_prefix)
{
    const unsigned char *ip   = req + NM_OFF_IP;
    const unsigned char *mask = req + NM_OFF_NETMASK;
    const unsigned char *gw   = req + NM_OFF_GATEWAY;

    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0)
    {
        return "IP가 0.0.0.0이다";
    }
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255)
    {
        return "IP가 브로드캐스트 주소다";
    }

    int prefix = netmask_to_prefix(mask);
    if (prefix < 0)
    {
        return "넷마스크가 연속된 비트가 아니다";
    }
    if (prefix < NM_PREFIX_MIN || prefix > NM_PREFIX_MAX)
    {
        return "넷마스크 범위가 /8~/30을 벗어난다";
    }
    if (memcmp(ip, gw, 4) == 0)
    {
        return "IP와 게이트웨이가 같다";
    }
    if (!same_subnet(ip, gw, prefix))
    {
        return "게이트웨이가 IP 대역 밖이다";
    }

    *out_prefix = prefix;
    return NULL;
}

/*
 * 적용할 설정을 요청 파일에 적는다. 성공하면 0.
 *
 * acud는 직접 네트워크를 바꾸지 않는다. 유닛에 NoNewPrivileges=yes가 걸려 있어 sudo를 쓸 수 없고,
 * 그 하드닝은 네트워크에서 들어온 입력을 파싱하는 데몬에 꼭 필요하다.
 * 대신 요청을 파일로 남기면 root로 도는 acu-netcfg-apply.path/service가 집어 가 적용한다
 * (거기서 arping 충돌 검사까지 한다).
 *
 * 부분적으로 쓰인 파일을 감시자가 집어 가면 안 되므로 임시 파일에 쓰고 rename으로 갈아 끼운다.
 */
static int sett_write_request(const AcuDiscover *d, const unsigned char *req, int prefix)
{
    char ip[16], gw[16];
    ip_to_text(req + NM_OFF_IP, ip, sizeof(ip));
    ip_to_text(req + NM_OFF_GATEWAY, gw, sizeof(gw));

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", d->request_path);

    FILE *fp = fopen(tmp, "w");
    if (!fp)
    {
        char line[400];
        snprintf(line, sizeof(line), "탐색: 요청 파일을 쓰지 못했다 (%s: %s)", tmp, strerror(errno));
        log_msg(line);
        return -1;
    }

    /*
     * SETT 프레임에는 DNS 필드가 없다(시리얼-이더넷 모듈에는 필요 없었다).
     * 로컬 네트워크에서는 게이트웨이가 DNS를 겸하는 것이 보통이라 게이트웨이를 쓴다.
     */
    fprintf(fp, "ip=%s\nprefix=%d\ngateway=%s\ndns=%s\n", ip, prefix, gw, gw);
    int bad = (fflush(fp) != 0);
    fclose(fp);

    if (bad || rename(tmp, d->request_path) != 0)
    {
        char line[400];
        snprintf(line, sizeof(line), "탐색: 요청 파일 교체 실패 (%s)", strerror(errno));
        log_msg(line);
        unlink(tmp);
        return -1;
    }
    return 0;
}

/*
 * SETT 한 건을 처리하고 SETC 또는 FAIL로 답한다.
 * 우리 앞으로 온 것이 아니면 아무것도 하지 않는다(다른 장치의 설정을 가로채면 안 된다).
 */
static void handle_sett(AcuDiscover *d, const unsigned char *req, const char *who)
{
    char line[300];

    if (!sett_is_for_us(d, req))
    {
        return; /* 다른 장치 앞으로 온 브로드캐스트 */
    }

    if (d->request_path[0] == '\0')
    {
        log_msg("탐색: SETT를 받았으나 요청 파일 경로가 없어 거절한다");
        send_set_reply(d, NM_CMD_FAIL);
        return;
    }

    if (sett_password_ok(d, req) != 0)
    {
        snprintf(line, sizeof(line),
                 "탐색: SETT 인증 실패로 거절 (%s)%s", who,
                 (d->sett_password[0] != '\0')
                     ? " - discovery_sett_password를 요구하는 설정이다. 기존 Device Manager는"
                       " 비밀번호를 보낼 수단이 없으니 이 값을 비우면 통한다"
                     : "");
        log_msg(line);
        send_set_reply(d, NM_CMD_FAIL);
        return;
    }

    int prefix = 0;
    const char *why = sett_validate(req, &prefix);
    if (why)
    {
        snprintf(line, sizeof(line), "탐색: SETT 거절 - %s (%s)", why, who);
        log_msg(line);
        send_set_reply(d, NM_CMD_FAIL);
        return;
    }

    if (sett_write_request(d, req, prefix) != 0)
    {
        send_set_reply(d, NM_CMD_FAIL);
        return;
    }

    /*
     * SETT는 장치의 TCP 수신 포트(RemotePort)도 실어 온다. 그런데 그 값은 config.json에 있고
     * config.json은 webui가 관리한다. acud가 여기서 config.json을 덮어쓰면 webui의 편집과
     * 충돌하므로 아직 반영하지 않는다. 조용히 무시하면 PC는 포트가 바뀐 줄 알고 접속에 실패하니
     * 다르면 로그로 남긴다.
     */
    unsigned want_port = get_be16(req + NM_OFF_PORT);
    if (want_port != (unsigned)d->tcp_port)
    {
        snprintf(line, sizeof(line),
                 "탐색: SETT가 TCP 포트 %u를 요청했지만 포트 변경은 아직 지원하지 않는다 "
                 "(현재 %d 유지). config.json에서 바꿀 것", want_port, d->tcp_port);
        log_msg(line);
    }

    char ip[16];
    ip_to_text(req + NM_OFF_IP, ip, sizeof(ip));
    snprintf(line, sizeof(line),
             "탐색: SETT 수락 (%s) -> %s/%d 적용 요청. 실제 적용과 충돌 검사는 acu-netcfg가 한다",
             who, ip, prefix);
    log_msg(line);

    /*
     * SETC는 "받아들였다"는 뜻이다. 원래 모듈도 설정을 받고 재부팅하는 방식이라
     * 적용 완료를 기다렸다 답하지 않는다. PC는 다시 탐색해서 결과를 확인하면 된다.
     */
    send_set_reply(d, NM_CMD_SETC);
}

/* ------------------------------------------------------------------ */
/* 공개 API                                                            */
/* ------------------------------------------------------------------ */

/* cfg의 값들을 내부 상태에 옮겨 담는다. 인터페이스가 바뀌면 인덱스를 다시 찾는다 */
static void load_config(AcuDiscover *d, const AcuConfig *cfg)
{
    snprintf(d->iface, sizeof(d->iface), "%s",
             (cfg->net_iface[0] != '\0') ? cfg->net_iface : "eth0");
    snprintf(d->sett_password, sizeof(d->sett_password), "%s", cfg->discovery_sett_password);
    d->inactivity_seconds = cfg->inactivity_seconds;
    snprintf(d->request_path, sizeof(d->request_path), "%s", cfg->netcfg_request_path);
    d->tcp_port = cfg->tcp_port;

    d->ifindex = if_nametoindex(d->iface);
    if (d->ifindex == 0)
    {
        char w[160];
        snprintf(w, sizeof(w), "탐색: %s 인터페이스를 찾지 못했다 - 모든 인터페이스에 응답한다", d->iface);
        log_msg(w);
    }
}

/* UDP 1460 수신 소켓을 열고 탐색을 시작한다. 실패하면 NULL */
AcuDiscover *discover_init(const AcuConfig *cfg)
{
    AcuDiscover *d = calloc(1, sizeof(*d));
    if (!d)
    {
        return NULL;
    }

    load_config(d, cfg);

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
     * eth0와 wlan0가 같은 대역에 함께 떠 있으면 브로드캐스트가 양쪽으로 들어와
     * 한 번의 FIND에 여러 번 답하게 된다. 우리는 한 인터페이스의 설정을 보고하므로
     * 그 인터페이스로 들어온 것만 답한다.
     * (SO_BINDTODEVICE는 CAP_NET_RAW가 필요해 쓰지 않는다)
     */
    if (setsockopt(d->fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) != 0)
    {
        log_msg("탐색: IP_PKTINFO 설정 실패 - 인터페이스 구분 없이 응답한다");
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

    char line[200];
    snprintf(line, sizeof(line),
             "탐색(UDP) 시작 - %s 정보를 포트 %d에서 대기, 응답은 브로드캐스트 %d",
             d->iface, NM_PORT_LISTEN, NM_PORT_REPLY);
    log_msg(line);
    return d;
}

/* 소켓을 닫고 자원을 반납한다 */
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

/* SIGHUP으로 설정이 바뀌었을 때 반영한다 (소켓은 그대로 쓴다) */
void discover_apply_config(AcuDiscover *d, const AcuConfig *cfg)
{
    if (d)
    {
        load_config(d, cfg);
    }
}

/* 상위 시스템 연결 상태를 알려 준다. IMIN의 Connect 필드에 실린다 */
void discover_set_connected(AcuDiscover *d, int connected)
{
    if (d)
    {
        d->connected = connected;
    }
}

/* select()에 넣을 수신 fd */
int discover_fd(const AcuDiscover *d)
{
    return d ? d->fd : -1;
}

/*
 * 요청 한 건을 받아 처리한다.
 * 우리가 설정을 보고하는 인터페이스로 들어온 것만 다루고, 나머지는 조용히 버린다.
 */
void discover_service(AcuDiscover *d)
{
    if (!d)
    {
        return;
    }

    unsigned char buf[NM_RECV_CAP];
    unsigned char cmsgbuf[256];
    struct sockaddr_in from;

    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name       = &from;
    msg.msg_namelen    = sizeof(from);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t n = recvmsg(d->fd, &msg, 0);
    if (n < NM_CMD_LEN)
    {
        return; /* 너무 짧으면 우리 프로토콜이 아니다 */
    }

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
            return; /* 다른 인터페이스로 들어온 브로드캐스트 */
        }
    }

    char who[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &from.sin_addr, who, sizeof(who));

    if (n == NM_FIND_REQ_LEN && memcmp(buf, NM_CMD_FIND, NM_CMD_LEN) == 0)
    {
        unsigned char ack[NM_FRAME_LEN];
        build_frame(d, NM_CMD_IMIN, ack);
        send_reply(d, ack, sizeof(ack));

        char line[160];
        snprintf(line, sizeof(line), "탐색: FIND 수신 (%s) -> IMIN %d byte 응답", who, NM_FRAME_LEN);
        log_msg(line);
        return;
    }

    if (n == NM_FRAME_PW_LEN && memcmp(buf, NM_CMD_SETT, NM_CMD_LEN) == 0)
    {
        handle_sett(d, buf, who);
        return;
    }

    char line[200];
    snprintf(line, sizeof(line), "탐색: 알 수 없는 요청 %ld byte (%s) - 무시", (long)n, who);
    log_msg(line);
}

/* TCP 서버 없이 도는 동안 탐색만 기다린다 (최대 timeout_ms) */
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
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(d->fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(d->fd, &rfds))
    {
        discover_service(d);
    }
}
