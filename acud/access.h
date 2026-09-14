#ifndef ACU_ACCESS_H
#define ACU_ACCESS_H

#include "db.h"
#include "users.h"

/*
 * 출입 판정 결과.
 * 추후(2단계 이후) IDTi 이벤트 코드로 매핑할 것:
 *   ACCESS_GRANTED       -> "Access Authorized By Card"
 *   ACCESS_DENIED_*      -> 각각 대응하는 거부 이벤트 코드
 */
typedef enum {
    ACCESS_GRANTED = 0,
    ACCESS_DENIED_NOT_FOUND,   /* 등록되지 않은 카드 */
    ACCESS_DENIED_DISABLED,    /* 등록은 되어 있으나 비활성화된 카드 */
    ACCESS_DENIED_VALIDATION,  /* 유효기간(Validation) 그룹을 벗어남 또는 그룹 미설정 */
    ACCESS_DENIED_TIMEZONE,    /* 출입 가능 시간대(Timezone) 밖 또는 그룹 미설정 */
    ACCESS_DENIED_DB_ERROR,    /* DB 조회 자체가 실패 */
} AccessResult;

/*
 * 카드값(원시 8byte)으로 출입 판정을 내린다.
 *
 * users: 사용자 명단(`users.db`) — 카드 -> 사용자를 찾는다
 * db   : 설정(`acud.db`) — 유효기간·시간대 **그룹 정의**가 있다.
 *        사용자는 그룹 번호만 들고 있고 그룹의 내용은 장치 설정이라 저장소가 다르다.
 * 찾은 경우 out_user에 사용자 레코드를 채운다 (판정 결과와 무관하게).
 */
AccessResult access_judge(AcuUsers *users, sqlite3 *db,
                          const uint8_t card_id[ACU_USER_CARD_LEN],
                          AcuUserRecord *out_user);

/* 판정 결과를 사람이 읽을 수 있는 한 줄 로그로 남긴다 (card_hex는 표시용) */
void access_log_result(const char *card_hex, AccessResult result);

#endif /* ACU_ACCESS_H */
