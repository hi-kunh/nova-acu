#ifndef ACU_DISCOVER_H
#define ACU_DISCOVER_H

#include "config.h"

/*
 * UDP 브로드캐스트 탐색/설정 (netmodule 프로토콜 호환).
 *
 * 기존 IntelliScan Device Manager는 UDP 255.255.255.255:1460 으로 요청을 뿌리고
 * 자기 IP의 5001 포트에서 응답을 받는다. 같은 형식으로 답하면
 * **PC 쪽 변경 없이 기존 도구가 우리 ACU를 발견하고 설정할 수 있다.**
 *
 * 이것이 필요한 이유: webui는 접속하려면 이미 IP를 알아야 하는데, 고쳐야 하는 대상이
 * 바로 그 IP다(닭-달걀). 브로드캐스트는 L2라서 장치 IP를 몰라도, 대역이 달라도 닿는다.
 *
 *   FIND(4)  -> IMIN(50)         현재 설정 보고
 *   SETT(58) -> SETC(58)/FAIL(58) 설정 변경
 *
 * 프레임 상세는 README "netmodule UDP 탐색/설정 프로토콜" 참고.
 */

typedef struct AcuDiscover AcuDiscover;

/*
 * UDP 1460 수신 소켓을 연다. cfg에서 인터페이스/TCP 포트/비밀번호/요청 파일 경로를 가져온다.
 * 실패하면 NULL - 탐색이 안 돼도 데몬 본체는 계속 동작해야 한다.
 */
AcuDiscover *discover_init(const AcuConfig *cfg);

/* 정리한다. d가 NULL이어도 안전. */
void discover_shutdown(AcuDiscover *d);

/* 설정 리로드(SIGHUP) 결과를 반영한다. 인터페이스가 바뀌면 다시 찾는다. */
void discover_apply_config(AcuDiscover *d, const AcuConfig *cfg);

/* select()에 넣을 수신 fd. d가 NULL이면 -1. */
int discover_fd(const AcuDiscover *d);

/* fd가 읽기 가능할 때 호출한다. 요청 한 건을 처리한다. */
void discover_service(AcuDiscover *d);

/*
 * net_poll을 쓸 수 없을 때(TCP 서버 초기화 실패) 쓰는 자체 대기 루프.
 * 오히려 그때가 PC가 장비를 찾아야 하는 상황이라 탐색은 살아 있어야 한다.
 */
void discover_wait(AcuDiscover *d, int timeout_ms);

#endif /* ACU_DISCOVER_H */
