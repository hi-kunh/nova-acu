#include "hal.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>

/*
 * HAL 모의(mock) 구현체.
 * 실제 리더기/릴레이/센서(6단계 예정)가 없는 동안, 더미 카드ID를 순회하며
 * 상위 로직(main.c/access.c)이 HAL 인터페이스만으로 동작하는지 검증한다.
 */

static const char *DUMMY_CARD_IDS[] = {
    "04A1B2C3D4E5F600", /* 활성 + 유효기간/시간대 제한없음 -> 허용 */
    "AABBCCDD11223300", /* 비활성화된 카드 -> 거부 */
    "FFFFFFFFFFFFFFFF", /* 미등록 카드 -> 거부 */
    "1122334455667700", /* 활성 + 유효기간(2020년) 만료 -> 거부 */
    "2233445566778800", /* 활성 + 유효기간/시간대 그룹 모두 통과 -> 허용 */
    "3344556677889900", /* 활성 + 시간대 그룹과 요일 불일치 -> 거부 */
};
#define DUMMY_CARD_COUNT (sizeof(DUMMY_CARD_IDS) / sizeof(DUMMY_CARD_IDS[0]))

static size_t g_card_idx = 0;

int hal_init(void)
{
    log_msg("HAL(mock) 초기화 - 실제 GPIO/Wiegand 리더기 없음, 더미 카드ID 순회로 대체");
    return 0;
}

void hal_shutdown(void)
{
    /* 모의 구현체는 정리할 자원이 없다 */
}

int hal_read_card(char *out_card_id, size_t out_len)
{
    snprintf(out_card_id, out_len, "%s", DUMMY_CARD_IDS[g_card_idx]);
    g_card_idx = (g_card_idx + 1) % DUMMY_CARD_COUNT;
    return 1; /* 모의 구현체: 매 호출마다 카드가 있는 것으로 시뮬레이션 */
}

int hal_open_door(int seconds)
{
    char line[64];
    snprintf(line, sizeof(line), "HAL(mock): 도어 릴레이 %d초 동작", seconds);
    log_msg(line);
    return 0;
}

int hal_read_sensor(HalSensorId id)
{
    (void)id;
    return HAL_SENSOR_INACTIVE; /* 모의 구현체: 항상 비활성 (문 닫힘 / 버튼 안눌림) */
}
