#ifndef ACU_DISCOVER_H
#define ACU_DISCOVER_H

/*
 * UDP 브로드캐스트 탐색 (netmodule 프로토콜 호환).
 *
 * 기존 IntelliScan Device Manager는 UDP 255.255.255.255:1460 으로 "FIND"를 뿌리고
 * 자기 IP의 5001 포트에서 응답을 받는다. 여기에 같은 형식으로 답하면
 * **PC 쪽 변경 없이 기존 도구가 우리 ACU를 발견한다.**
 *
 * 이것이 필요한 이유: webui는 접속하려면 이미 IP를 알아야 하는데, 고쳐야 하는 대상이
 * 바로 그 IP다(닭-달걀). 브로드캐스트는 L2라서 장치 IP를 몰라도, 대역이 달라도 닿는다.
 *
 * 프레임 상세는 README "netmodule UDP 탐색/설정 프로토콜" 참고.
 */

typedef struct AcuDiscover AcuDiscover;

/*
 * UDP 1460 수신 소켓을 연다.
 * iface: 보고할 네트워크 인터페이스 이름 (예: "eth0"). MAC/IP/넷마스크를 여기서 읽는다.
 * tcp_port: IMIN 응답의 RemotePort로 실을 값 (우리 IDTi TCP 수신 포트).
 * 실패하면 NULL을 반환한다 - 탐색이 안 돼도 데몬 본체는 계속 동작해야 한다.
 */
AcuDiscover *discover_init(const char *iface, int tcp_port);

/* 정리한다. d가 NULL이어도 안전. */
void discover_shutdown(AcuDiscover *d);

/* select()에 넣을 수신 fd. d가 NULL이면 -1. */
int discover_fd(const AcuDiscover *d);

/* fd가 읽기 가능할 때 호출한다. 요청 한 건을 처리한다. */
void discover_service(AcuDiscover *d);

/* IMIN 응답에 실을 TCP 포트를 갱신한다 (config 리로드로 tcp_port가 바뀐 경우). */
void discover_set_tcp_port(AcuDiscover *d, int tcp_port);

/*
 * net_poll을 쓸 수 없을 때(TCP 서버 초기화 실패) 쓰는 자체 대기 루프.
 * 최대 timeout_ms 동안 기다렸다가 요청이 있으면 처리한다.
 */
void discover_wait(AcuDiscover *d, int timeout_ms);

#endif /* ACU_DISCOVER_H */
