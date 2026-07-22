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
 * 카드 리더에서 카드 ID를 읽는다 (non-blocking 폴링 방식).
 * 카드가 있으면 out_card_id에 채우고 1, 없으면 0, 오류면 -1을 반환한다.
 */
int hal_read_card(char *out_card_id, size_t out_len);

/*
 * 도어 릴레이를 seconds초 동안 동작시킨다 (IDTi Relay ActiveType=1, Door Relay).
 * 반환: 0=성공, -1=실패
 */
int hal_open_door(int seconds);

/* 센서 상태를 읽는다. 반환: HalSensorState 값, 오류 시 -1 */
int hal_read_sensor(HalSensorId id);

#endif /* ACU_HAL_H */
