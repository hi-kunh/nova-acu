#ifndef ACU_LOOP_H
#define ACU_LOOP_H

/*
 * 이벤트 루프 (fd 등록형 select 한 곳).
 *
 * 왜 만들었나 — 예전에는 select가 net.c 안에만 있었고(net_poll), 거기에 외부 fd를 **하나만**
 * 얹을 수 있었다(net_set_aux_reader). 그래서 카드 입력은 select에 못 들어가고 main이 2초마다
 * 따로 조회해야 했다 -> 카드가 최대 2초 늦게 처리됐다.
 *
 * 이제는 fd를 여럿 등록할 수 있는 루프가 하나 있고, 네트워크(리슨·클라이언트) · UDP 탐색 ·
 * 카드 입력(6단계에서는 RRU 1~7의 USB fd)이 각자 자기 fd를 여기에 건다.
 * 루프는 그 fd가 무엇인지 알 필요가 없다 - 준비되면 등록된 콜백을 부를 뿐이다.
 *
 * select를 쓰는 이유: 감시할 fd가 많아야 십여 개(RRU 7 + 소켓 3 + 타이머 1)라서 epoll의
 * 이점이 없고, 어떤 리눅스에서도 그대로 돈다. fd 개수가 FD_SETSIZE에 한참 못 미친다.
 */

typedef struct AcuLoop AcuLoop;

#define ACU_LOOP_READ  0x01u
#define ACU_LOOP_WRITE 0x02u

/* 등록할 수 있는 fd 개수. RRU 7 + 리슨 + 클라이언트 + 탐색 + 점검 타이머 = 11 -> 넉넉히 */
#define ACU_LOOP_MAX_FDS 32

/*
 * fd가 준비됐을 때 불린다. events는 실제로 준비된 것만 담는다(ACU_LOOP_READ/WRITE의 조합).
 * 콜백 안에서 loop_add/loop_remove를 불러도 된다 (그 회차의 나머지 처리에는 반영되지 않는다).
 */
typedef void (*AcuLoopCb)(int fd, unsigned events, void *user);

/* 루프를 만든다. 실패하면 NULL */
AcuLoop *loop_create(void);

/* 정리한다. fd 자체는 닫지 않는다 (연 쪽이 닫는다). l이 NULL이어도 안전 */
void loop_destroy(AcuLoop *l);

/*
 * fd를 등록한다. 이미 있는 fd면 관심 이벤트와 콜백을 덮어쓴다.
 * 반환: 0=성공, -1=실패(자리 부족 또는 잘못된 인자)
 */
int loop_add(AcuLoop *l, int fd, unsigned events, AcuLoopCb cb, void *user);

/*
 * 등록된 fd의 관심 이벤트만 바꾼다 (예: 보내다 만 응답이 있을 때만 WRITE를 켠다).
 * 반환: 0=성공, -1=등록되지 않은 fd
 */
int loop_mod(AcuLoop *l, int fd, unsigned events);

/* 등록을 해제한다. 없는 fd여도 안전 */
void loop_remove(AcuLoop *l, int fd);

/*
 * 최대 timeout_ms 동안 기다렸다가 준비된 fd의 콜백을 부른다.
 * timeout_ms < 0 이면 무한 대기. 반환: 준비된 fd 수, 시간 초과면 0, 오류면 -1
 * (시그널로 깨어난 경우 errno==EINTR로 -1 - main이 종료·리로드 요청을 볼 기회가 된다)
 */
int loop_wait(AcuLoop *l, int timeout_ms);

#endif /* ACU_LOOP_H */
