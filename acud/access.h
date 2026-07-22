#ifndef ACU_ACCESS_H
#define ACU_ACCESS_H

#include "db.h"

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
 * card_id로 DB를 조회하여 출입 판정을 내린다.
 * 조회에 성공한 경우 out_record에 카드 정보를 채운다 (판정 결과와 무관하게).
 */
AccessResult access_judge(sqlite3 *db, const char *card_id, CardRecord *out_record);

/* 판정 결과를 사람이 읽을 수 있는 한 줄 로그로 남긴다. */
void access_log_result(const char *card_id, AccessResult result);

#endif /* ACU_ACCESS_H */
