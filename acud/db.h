#ifndef ACU_DB_H
#define ACU_DB_H

#include <sqlite3.h>

/*
 * 설정 저장 (`acud.db`) — 유효기간·시간대 **그룹 정의**.
 *
 * 사용자(카드)는 여기 없다. 규약 필드를 그대로 담아야 하고 수십만 건을 통째로 교체하므로
 * `users.db`(users.h)로 따로 뺐다. 이 파일에는 **장치 설정**만 남는다.
 */

/* DB 파일을 열고 스키마가 없으면 생성한다. 실패 시 NULL 반환. */
sqlite3 *db_open(const char *path);

/* DB 핸들을 닫는다. db가 NULL이어도 안전. */
void db_close(sqlite3 *db);

/* 테스트 단계용: 더미 그룹 정의를 없을 때만 채워 넣는다 (사용자는 users_seed_dummy) */
void db_seed_dummy_data(sqlite3 *db);

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
