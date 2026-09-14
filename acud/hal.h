#ifndef ACU_HAL_H
#define ACU_HAL_H

#include <stddef.h>

/*
 * 하드웨어 추상화 계층(HAL) 인터페이스 (2단계).
 * 실제 GPIO/Wiegand 통신(6단계: rk3566 시리얼 / rk3568 CAN bus)은 아직 없으므로,
 * 지금은 이 인터페이스의 모의(mock) 구현체(hal_mock.c)만 존재한다.
 * 6단계에서 hal_rk3566.c / hal_rk3568.c 같은 실제 구현체로 교체해도
 * main.c/access.c는 이 헤더만 보고 그대로 동작해야 한다.
 */

/* IDTi Device Type Table(7. Device Type Table.doc) 참고 코드 - 최종 장치 타입은 추후 확정 */
#define HAL_DEVICE_TYPE_ISC101 0x29

/*
 * ACU 한 대에 붙는 RRU의 최대 대수. **RRU 번호 = DIP 값 = 1~7**이고 번호 하나가 모듈 2칸을
 * 차지한다 (HARDWARE.md "RRU 여러 대 구성"). 규약의 모듈 칸이 14개뿐이라 7대가 한계다.
 */
#define HAL_RRU_MAX 7

/* 루프에 걸 수 있는 입력 fd의 최대 개수 = RRU 7대 + mock 자동 태그 타이머 1 */
#define HAL_INPUT_FD_MAX (HAL_RRU_MAX + 1)

/* 센서 채널 식별자 (IDTi Device Input(Sensor), Object 0x2C 대응) */
typedef enum {
    HAL_SENSOR_DOOR_CONTACT = 0, /* Door Contact (Device Type 0xac) */
    HAL_SENSOR_EXIT_BUTTON  = 1, /* Door Control/Exit Button (Device Type 0xae) */
} HalSensorId;

typedef enum {
    HAL_SENSOR_INACTIVE = 0,
    HAL_SENSOR_ACTIVE   = 1,
} HalSensorState;

/* HAL을 초기화한다 (실제 구현체에서 GPIO/UART 핸들 등을 연다). 반환: 0=성공, -1=실패 */
int hal_init(void);

/* HAL 자원을 정리한다. */
void hal_shutdown(void);

/*
 * 감시할 입력 fd 목록을 out[]에 채우고 개수를 반환한다 (0 이상).
 *
 * 메인 루프는 이 fd들을 select에 걸어 두고, 읽기 가능해지면 hal_service_fd()를 부른다.
 * 예전처럼 주기적으로 hal_read_card()를 부르지 않는다 - 카드가 조회 주기만큼 늦게
 * 처리되던 이유가 그것이었다.
 *
 * 지금(mock)은 카드 주입 FIFO 1개(+ 자동 태그 타이머)지만, 6단계에서는 **RRU 1~7의 USB fd**가
 * 이 목록을 채운다. 그래서 개수를 HAL이 정해서 돌려준다 - main은 몇 개인지 알 필요가 없다.
 */
int hal_input_fds(int out[HAL_INPUT_FD_MAX]);

/*
 * hal_input_fds()가 준 fd가 읽기 가능해졌을 때 부른다.
 * 들어온 것을 읽어 카드 대기 큐와 센서 상태를 갱신한다 (실제로 카드가 아닐 수도 있다).
 */
void hal_service_fd(int fd);

/*
 * 카드 대기 큐에서 카드 한 장을 꺼낸다 (non-blocking).
 * 카드가 있으면 out_card_id에 채우고 1, 없으면 0, 오류면 -1을 반환한다.
 * out_rru에는 그 카드가 올라온 **RRU 번호(1~HAL_RRU_MAX)** 를 넣는다 - 이벤트 주소를 만들 때 쓴다.
 * 한 번 깨어났을 때 여러 장이 들어와 있을 수 있으므로 0이 나올 때까지 반복해 부른다.
 */
int hal_read_card(int *out_rru, char *out_card_id, size_t out_len);

/*
 * 도어 릴레이를 seconds초 동안 동작시킨다 (IDTi Relay ActiveType=1, Door Relay).
 * 반환: 0=성공, -1=실패
 */
int hal_open_door(int seconds);

/* 센서 상태를 읽는다. 반환: HalSensorState 값, 오류 시 -1 */
int hal_read_sensor(HalSensorId id);

#endif /* ACU_HAL_H */
