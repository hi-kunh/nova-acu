#ifndef ACU_DB_H
#define ACU_DB_H

#include <sqlite3.h>

/*
 * cards 테이블 한 행을 담는 구조체.
 * IDTi User Info(32byte)의 필드 중 카드 출입 판정에 필요한 부분만 반영한다.
 */
typedef struct {
    char card_id[17];    /* 카드 ID (hex 문자열, IDTi User ID(8byte)를 hex 16자로 표현) */
    char user_id[17];    /* 사용자 ID (hex 문자열) */
    int  is_enabled;     /* User Option bitflag 중 Enable 여부 (0/1) */
    int  level;          /* Level(1) */
    int  validation_code;/* Validation Code(2): 유효기간 그룹 (0=제한없음, validations 테이블 참조) */
    int  timezone_code;  /* Timezone Code(2): 출입 가능 시간대 그룹 (0=제한없음, timezones 테이블 참조) */
} CardRecord;

/* DB 파일을 열고 스키마가 없으면 생성한다. 실패 시 NULL 반환. */
sqlite3 *db_open(const char *path);

/* DB 핸들을 닫는다. db가 NULL이어도 안전. */
void db_close(sqlite3 *db);

/* 테스트 단계용: 더미 카드 데이터를 없을 때만 채워 넣는다. */
void db_seed_dummy_data(sqlite3 *db);

/*
 * card_id로 카드를 조회한다.
 * 찾으면 out에 채우고 1, 못 찾으면 0, 오류면 -1을 반환한다.
 */
int db_lookup_card(sqlite3 *db, const char *card_id, CardRecord *out);

/*
 * validation_code(유효기간 그룹, IDTi Validation ID)를 확인한다.
 * 0이면 그룹 없음(무조건 유효)으로 취급한다.
 * 그 외에는 validations 테이블에서 오늘 날짜가 start_date~end_date 범위에 있는지 확인한다.
 * 반환: 1=유효, 0=무효(그룹이 없거나 기간을 벗어남), -1=DB 오류
 */
int db_check_validation(sqlite3 *db, int validation_code);

/*
 * timezone_code(출입 가능 시간대 그룹, IDTi Timezone ID)를 확인한다.
 * 0이면 그룹 없음(무조건 허용)으로 취급한다.
 * 그 외에는 timezones 테이블의 슬롯들 중 현재 요일 + 현재 시각이 포함되는 슬롯이 있는지 확인한다.
 * (공휴일 비트는 아직 공휴일 캘린더가 없어 반영하지 않는다 - 향후 Device Holiday 구현 시 확장)
 * 반환: 1=허용, 0=거부(그룹이 없거나 일치하는 슬롯이 없음), -1=DB 오류
 */
int db_check_timezone(sqlite3 *db, int timezone_code);

#endif /* ACU_DB_H */
