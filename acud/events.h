#ifndef ACU_EVENTS_H
#define ACU_EVENTS_H

#include <stdint.h>
#include <time.h>

/*
 * 이벤트 영속 저장 (`events.db`).
 *
 * **왜 필요한가** — 지금까지 이벤트는 net.c의 **메모리 큐 32칸**뿐이었다. 재부팅이나 전원 차단이면
 * 전부 사라지고, 33건째부터는 가장 오래된 것을 버렸다. 출입 기록은 사후에 증거로 쓰이는
 * 자료라서 "상위 시스템이 가져가기 전에 사라질 수 있다"는 성질을 그대로 둘 수 없다.
 *
 * 그래서 별도 SQLite 파일에 적재하고, **상위 시스템(DM)이 어디까지 가져갔는지**를 같은 파일에
 * 함께 둔다. 둘이 같은 파일이어야 전원이 끊겨도 "적재"와 "전송 위치"가 어긋나지 않는다.
 *
 * **사용자 DB와 파일을 나눈 이유** — 사용자 DB는 수십만 건을 한 번에 받아 통째로 교체하는 쓰기,
 * 이벤트는 한 건씩 계속 붙는 쓰기다. 성격이 다르고, 이벤트 링 삭제(오래된 것 지우기)가
 * 사용자 DB 쪽 트랜잭션을 붙잡으면 안 된다.
 *
 * **DB를 열지 못하면** 같은 API 그대로 **메모리 전용 모드**로 돈다. 저장은 못 해도 출입 판정과
 * 상위 보고는 계속돼야 하기 때문이다 (이 데몬의 다른 부분과 같은 원칙). 이때는 로그로 크게 남긴다.
 */

typedef struct AcuEvents AcuEvents;

/* 이벤트 한 건. IDTi Event Structure(36byte)로 옮겨질 값들이다 (protocol.h `build_event_info`) */
typedef struct {
    int64_t  seq;         /* 저장소가 매기는 단조 증가 번호. 전송 위치의 기준 (append 시 채워진다) */
    uint32_t event_code;
    uint8_t  op_mode;     /* IDTI_OPMODE_* */
    uint8_t  module_addr; /* 이벤트 주소 byte 6 — 1부터 */
    uint8_t  reader_addr; /* 이벤트 주소 byte 7 — 1부터 */
    uint8_t  door_status; /* IDTI_DOOR_STATUS_* */
    uint8_t  func_code;   /* IDTI_FUNC_* */
    time_t   ts;
    uint8_t  id[8];       /* Access ID */
} AcuEvent;

/*
 * 저장소를 연다.
 * path     : `events.db` 경로. 빈 문자열이면 처음부터 메모리 전용 모드
 * capacity : 보관할 최대 건수 (링 삭제 기준). 0 이하면 기본값
 * 반환: 실패는 메모리 할당 실패뿐 (그때만 NULL)
 */
AcuEvents *events_open(const char *path, long long capacity);

/* 정리한다. ev가 NULL이어도 안전 */
void events_close(AcuEvents *ev);

/*
 * 이벤트 한 건을 적재한다. e->seq는 무시하고 저장소가 새 번호를 매긴다.
 * 반환: 0=성공, -1=실패(실패해도 데몬은 계속 돈다)
 */
int events_append(AcuEvents *ev, const AcuEvent *e);

/*
 * 아직 상위 시스템이 가져가지 않은 이벤트를 오래된 순으로 최대 max건 꺼낸다.
 * **꺼내도 전송 위치는 움직이지 않는다** — 실제로 나갔는지는 보낸 쪽만 안다.
 * 전송에 성공하면 events_ack()로 위치를 전진시킨다. 그래야 응답을 보내지 못한 이벤트가 사라지지 않는다.
 * 반환: 채운 건수 (0 이상), 오류면 -1
 */
int events_fetch(AcuEvents *ev, AcuEvent *out, int max);

/* 전송 위치를 upto_seq까지 전진시킨다 (그 이하는 전송된 것으로 본다). 반환: 0=성공, -1=실패 */
int events_ack(AcuEvents *ev, int64_t upto_seq);

/* 아직 보내지 않은 건수. 오류면 -1 */
long long events_pending(const AcuEvents *ev);

/* 저장소에 남아 있는 총 건수. 오류면 -1 */
long long events_total(const AcuEvents *ev);

/*
 * **읽기 위치를 검사하고 어긋났으면 바로잡는다.**
 *
 * 현장 SSC-324에서 실제로 난 고장이다 (DM 9/14 회신 6절): 읽기 인덱스가 쓰기 위치보다 앞서 나가
 * "새 이벤트 없음"이라고 답하면서 **이벤트를 하나도 올리지 못했다.** 우리 구조에서는
 * `전송 위치 > 가장 새 번호`가 그 상태다 — DB를 갈아 끼우거나 복구했을 때 생길 수 있고,
 * 그대로 두면 새 이벤트가 그 번호를 넘을 때까지 **아무것도 보내지 않는다.**
 * 반환: 1=바로잡음, 0=정상, -1=오류
 */
int events_repair_index(AcuEvents *ev);

/*
 * 읽기 위치를 **지정 시각 이후의 첫 이벤트 앞**으로 옮긴다 (EventIndexChange 타입 3).
 * 그 시각 이후 이벤트가 다시 올라간다. 반환: 다시 보낼 건수(0 이상), 오류면 -1
 */
long long events_rewind_to_time(AcuEvents *ev, time_t from);

/*
 * 모든 이벤트를 **보낸 것으로 표시**한다 (EventReset).
 * ⚠ 지우지 않는다 — 출입 기록은 되돌릴 수 없는 증거라, 필요하면 `events_rewind_to_time()`으로
 *   되살릴 수 있게 둔다. 링 삭제 한도는 그대로 적용된다.
 * 반환: 0=성공, -1=실패
 */
int events_mark_all_sent(AcuEvents *ev);

/* 메모리 전용 모드인지 (1=저장 안 됨). 로그·상태 표시용 */
int events_is_memory_only(const AcuEvents *ev);

#endif /* ACU_EVENTS_H */
