#ifndef ACU_NET_H
#define ACU_NET_H

#include "access.h"

/*
 * 네트워크 통신부 (5단계, IDTi 프로토콜 V2, TCP 전용).
 * 상위 시스템(PC)이 접속해 Event Log(History, Object 0x01)를 요청하면
 * 출입 판정 결과를 IDTi Event Structure(36byte)로 응답한다.
 */

typedef struct AcuNet AcuNet;

/*
 * 상위 시스템에 보고할 I/O 구성. **DM은 이걸 받아야 장치 트리(카테고리)를 만든다** —
 * 전부 0으로 보고하면 리더/입출력이 없는 장치로 보여 사용자·도어 설정을 내려보내지 못한다.
 *
 * 모듈 하나는 14슬롯을 꽉 채운다: 리더2 + 입력6 + 출력4 + 알람·화재2.
 * 모듈 4개면 리더 8 / 입력 32 / 출력 16 으로 RRU 구성과 맞는다.
 * ModuleType 코드는 아직 확정 전이라 설정으로 받는다 (protocol.h의 IDTI_MODULE_TYPE_* 참고).
 */
typedef struct {
    int module_count;    /* 보고할 모듈 개수 */
    int readers;         /* 모듈당 카드리더 */
    int inputs;          /* 모듈당 입력 */
    int outputs;         /* 모듈당 출력 */
    int common_inputs;   /* 모듈당 알람·화재 입력. 이름과 달리 모듈마다 실제로 배선된 입력이다 */
    int module_type;     /* clsDevParams.ModuleType */
    int install_type;    /* IDTI_MODULE_INSTALL_* */
} AcuModuleLayout;

/* 보고할 I/O 구성을 설정한다 */
void net_set_module_layout(AcuNet *net, const AcuModuleLayout *layout);

/* TCP 서버를 초기화한다 (지정한 port로 listen). 실패 시 NULL (네트워크 없이도 출입 판정은 계속 동작해야 함). */
AcuNet *net_init(int port);

/* 네트워크 자원을 정리한다. net이 NULL이어도 안전. */
void net_shutdown(AcuNet *net);

/*
 * 접속/수신/응답을 최대 timeout_ms 동안 처리한다 (select 기반, non-blocking).
 * main 루프에서 sleep() 대신 주기적으로 호출한다. net이 NULL이면 아무 일도 하지 않는다.
 */
void net_poll(AcuNet *net, int timeout_ms);

/*
 * net_poll의 select에 같이 감시할 읽기 fd를 하나 등록한다.
 * UDP 탐색처럼 별도 소켓을 쓰는 기능이 자기 select 루프를 따로 돌지 않아도 되게 하기 위함이다
 * (루프가 둘이면 한쪽이 블록되는 동안 다른 쪽 응답이 늦어진다).
 * net.c는 그 fd가 무엇인지 알 필요가 없다 - 읽기 가능해지면 on_readable(user)를 부를 뿐이다.
 * fd < 0 이면 등록을 해제한다.
 */
void net_set_aux_reader(AcuNet *net, int fd, void (*on_readable)(void *user), void *user);

/*
 * 출입 판정 결과를 IDTi Event Log(History, Object 0x01)로 상위 시스템에 보고할 큐에 넣는다.
 * id_hex: 허용 시 User ID, 거부 시 Card ID (IDTi Event Structure의 Access ID 규칙과 동일), 16자 hex 문자열.
 * door_status: IDTI_DOOR_STATUS_* 값 (판정 시점의 문 상태).
 * DB 오류(ACCESS_DENIED_DB_ERROR)는 상위 시스템에 보고할 실질적 의미가 없어 무시한다.
 */
void net_push_event(AcuNet *net, AccessResult result, const char *id_hex, int door_status);

/* 상위 시스템이 지금 붙어 있는지 (netmodule IMIN의 Connect 필드에 실린다). net이 NULL이면 0 */
int net_is_connected(const AcuNet *net);

/*
 * Device Status / Firmware Info에 실을 장치 식별자를 설정한다.
 * category는 clsDevParams.DeviceType(Controller=3), type은 ControllerType(SSC_324=33 등).
 * **PC에 등록한 모델과 맞아야 한다.**
 */
void net_set_device_identity(AcuNet *net, int category, int type);

/*
 * 상위 시스템(DM)이 요청에 실어 보내는 시각으로 시스템 시계를 맞출지 설정한다.
 * 고립망에는 NTP 서버가 없을 수 있어 이것이 유일한 시각 공급원이 된다.
 * 시계를 네트워크에서 받는 것은 신뢰 결정이므로 끌 수 있게 해 둔다.
 */
void net_set_time_sync(AcuNet *net, int enabled);

/*
 * 유휴 타임아웃(초)을 설정한다. 0이면 끈다.
 * netmodule의 InactivityTime에 대응한다 - 그 시간 동안 클라이언트 활동이 없으면 소켓을 닫는다.
 * 케이블만 빠진 것처럼 상대가 조용히 사라지면 TCP는 한참 뒤에야 알아채므로 직접 정리한다.
 */
void net_set_inactivity_timeout(AcuNet *net, int seconds);

/* 현재 도어 센서 상태를 갱신한다 (Device Status 응답에 사용, IDTI_DOOR_STATUS_* 값) */
void net_set_door_status(AcuNet *net, int door_status);

#endif /* ACU_NET_H */
