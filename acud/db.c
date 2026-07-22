#define _POSIX_C_SOURCE 200809L

#include "db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS cards ("
    "    card_id         TEXT PRIMARY KEY,"
    "    user_id         TEXT NOT NULL,"
    "    is_enabled      INTEGER NOT NULL DEFAULT 1,"
    "    level           INTEGER NOT NULL DEFAULT 0,"
    "    validation_code INTEGER NOT NULL DEFAULT 0,"
    "    timezone_code   INTEGER NOT NULL DEFAULT 0"
    ");"
    /* IDTi Validation ID(1~1024) 대응: 유효기간 그룹 하나당 시작일~종료일 한 세트 */
    "CREATE TABLE IF NOT EXISTS validations ("
    "    validation_id INTEGER PRIMARY KEY,"
    "    start_date    TEXT NOT NULL," /* 'YYYY-MM-DD' */
    "    end_date      TEXT NOT NULL"  /* 'YYYY-MM-DD' */
    ");"
    /* IDTi Timezone ID(1~1024) 대응: 그룹 하나당 슬롯(Timezone Value 1~4) 최대 4개 */
    "CREATE TABLE IF NOT EXISTS timezones ("
    "    timezone_id  INTEGER NOT NULL,"
    "    slot_index   INTEGER NOT NULL," /* 0~3 */
    "    start_hour   INTEGER NOT NULL,"
    "    start_min    INTEGER NOT NULL,"
    "    end_hour     INTEGER NOT NULL,"
    "    end_min      INTEGER NOT NULL,"
    "    week_select  INTEGER NOT NULL," /* 16bit bitmask: bit15=일 ... bit9=토, bit8~0=공휴일1~9 */
    "    PRIMARY KEY (timezone_id, slot_index)"
    ");";

sqlite3 *db_open(const char *path)
{
    sqlite3 *db = NULL;

    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        fprintf(stderr, "DB 열기 실패: %s\n", db ? sqlite3_errmsg(db) : "알 수 없는 오류");
        if (db)
        {
            sqlite3_close(db);
        }
        return NULL;
    }

    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "스키마 생성 실패: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

void db_close(sqlite3 *db)
{
    if (db)
    {
        sqlite3_close(db);
    }
}

void db_seed_dummy_data(sqlite3 *db)
{
    /*
     * 테스트용 더미 카드 5장으로 판정 경로를 모두 검증한다:
     *   1. 활성 + 제한없음(0,0)                         -> 허용
     *   2. 비활성화                                      -> 거부 (비활성)
     *   3. 활성 + 유효기간(validation_id=1, 2020년) 만료  -> 거부 (유효기간)
     *   4. 활성 + 유효기간/시간대(validation_id=2, timezone_id=1) 모두 통과 -> 허용
     *   5. 활성 + 시간대(timezone_id=2, 요일 전부 미선택) 불일치           -> 거부 (시간대)
     */
    static const char *SEED_SQL =
        "INSERT OR IGNORE INTO cards (card_id, user_id, is_enabled, level, validation_code, timezone_code) VALUES"
        "  ('04A1B2C3D4E5F600', '0000000000000001', 1, 1, 0, 0),"
        "  ('AABBCCDD11223300', '0000000000000002', 0, 1, 0, 0),"
        "  ('1122334455667700', '0000000000000003', 1, 1, 1, 0),"
        "  ('2233445566778800', '0000000000000004', 1, 1, 2, 1),"
        "  ('3344556677889900', '0000000000000005', 1, 1, 0, 2);"

        "INSERT OR IGNORE INTO validations (validation_id, start_date, end_date) VALUES"
        "  (1, '2020-01-01', '2020-12-31'),"
        "  (2, date('now','localtime','-1 year'), date('now','localtime','+1 year'));"

        "INSERT OR IGNORE INTO timezones (timezone_id, slot_index, start_hour, start_min, end_hour, end_min, week_select) VALUES"
        "  (1, 0, 0, 0, 23, 59, 0xFFFF),"  /* 모든 요일/공휴일 + 하루 종일 -> 항상 허용 */
        "  (2, 0, 0, 0, 23, 59, 0x0000);"; /* 어떤 요일도 선택 안 됨 -> 항상 불일치 */

    char *err = NULL;
    if (sqlite3_exec(db, SEED_SQL, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "더미 데이터 삽입 실패: %s\n", err);
        sqlite3_free(err);
    }
}

int db_lookup_card(sqlite3 *db, const char *card_id, CardRecord *out)
{
    static const char *SQL =
        "SELECT card_id, user_id, is_enabled, level, validation_code, timezone_code "
        "FROM cards WHERE card_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "조회 준비 실패: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, card_id, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    int result;

    if (rc == SQLITE_ROW)
    {
        snprintf(out->card_id, sizeof(out->card_id), "%s", (const char *)sqlite3_column_text(stmt, 0));
        snprintf(out->user_id, sizeof(out->user_id), "%s", (const char *)sqlite3_column_text(stmt, 1));
        out->is_enabled      = sqlite3_column_int(stmt, 2);
        out->level           = sqlite3_column_int(stmt, 3);
        out->validation_code = sqlite3_column_int(stmt, 4);
        out->timezone_code   = sqlite3_column_int(stmt, 5);
        result = 1;
    }
    else if (rc == SQLITE_DONE)
    {
        result = 0; /* 조회 결과 없음 */
    }
    else
    {
        fprintf(stderr, "조회 실패: %s\n", sqlite3_errmsg(db));
        result = -1;
    }

    sqlite3_finalize(stmt);
    return result;
}

int db_check_validation(sqlite3 *db, int validation_code)
{
    if (validation_code == 0)
    {
        return 1; /* 그룹 없음 -> 무조건 유효 */
    }

    /* 존재하지 않는 validation_id와 "기간 벗어남"을 굳이 구분하지 않고 둘 다 무효(0)로 취급한다 */
    static const char *SQL =
        "SELECT EXISTS("
        "  SELECT 1 FROM validations WHERE validation_id = ?"
        "    AND date('now','localtime') BETWEEN start_date AND end_date"
        ");";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "유효기간 조회 준비 실패: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, validation_code);

    int result;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        result = sqlite3_column_int(stmt, 0);
    }
    else
    {
        fprintf(stderr, "유효기간 조회 실패: %s\n", sqlite3_errmsg(db));
        result = -1;
    }

    sqlite3_finalize(stmt);
    return result;
}

int db_check_timezone(sqlite3 *db, int timezone_code)
{
    if (timezone_code == 0)
    {
        return 1; /* 그룹 없음 -> 무조건 허용 */
    }

    static const char *SQL =
        "SELECT start_hour, start_min, end_hour, end_min, week_select "
        "FROM timezones WHERE timezone_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "타임존 조회 준비 실패: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, timezone_code);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int cur_minutes = tm_now.tm_hour * 60 + tm_now.tm_min;
    /* tm_wday: 0=일 ... 6=토. Week Select bit15=일 ... bit9=토 이므로 bit = 15 - tm_wday */
    int week_bit = 15 - tm_now.tm_wday;

    int has_slot = 0;
    int matched  = 0;
    int rc;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        has_slot = 1;

        int start_minutes = sqlite3_column_int(stmt, 0) * 60 + sqlite3_column_int(stmt, 1);
        int end_minutes    = sqlite3_column_int(stmt, 2) * 60 + sqlite3_column_int(stmt, 3);
        int week_select    = sqlite3_column_int(stmt, 4);

        int week_ok = (week_select >> week_bit) & 1;
        int time_ok = (cur_minutes >= start_minutes) && (cur_minutes <= end_minutes);

        if (week_ok && time_ok)
        {
            matched = 1;
            break;
        }
    }

    int result;
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    {
        fprintf(stderr, "타임존 조회 실패: %s\n", sqlite3_errmsg(db));
        result = -1;
    }
    else
    {
        result = (has_slot && matched) ? 1 : 0;
    }

    sqlite3_finalize(stmt);
    return result;
}
