#include "access.h"
#include "log.h"

#include <stdio.h>

AccessResult access_judge(sqlite3 *db, const char *card_id, CardRecord *out_record)
{
    int rc = db_lookup_card(db, card_id, out_record);

    if (rc < 0)
    {
        return ACCESS_DENIED_DB_ERROR;
    }
    if (rc == 0)
    {
        return ACCESS_DENIED_NOT_FOUND;
    }
    if (!out_record->is_enabled)
    {
        return ACCESS_DENIED_DISABLED;
    }

    int valid = db_check_validation(db, out_record->validation_code);
    if (valid < 0)
    {
        return ACCESS_DENIED_DB_ERROR;
    }
    if (!valid)
    {
        return ACCESS_DENIED_VALIDATION;
    }

    int in_timezone = db_check_timezone(db, out_record->timezone_code);
    if (in_timezone < 0)
    {
        return ACCESS_DENIED_DB_ERROR;
    }
    if (!in_timezone)
    {
        return ACCESS_DENIED_TIMEZONE;
    }

    return ACCESS_GRANTED;
}

void access_log_result(const char *card_id, AccessResult result)
{
    char line[128];
    const char *reason;

    switch (result)
    {
        case ACCESS_GRANTED:          reason = "허용";                 break;
        case ACCESS_DENIED_NOT_FOUND: reason = "거부 (미등록 카드)";    break;
        case ACCESS_DENIED_DISABLED:  reason = "거부 (비활성화된 카드)"; break;
        case ACCESS_DENIED_VALIDATION: reason = "거부 (유효기간 벗어남)"; break;
        case ACCESS_DENIED_TIMEZONE:  reason = "거부 (출입 가능 시간대 아님)"; break;
        case ACCESS_DENIED_DB_ERROR:  reason = "거부 (DB 조회 오류)";   break;
        default:                      reason = "거부 (알 수 없는 사유)"; break;
    }

    snprintf(line, sizeof(line), "카드 판정 [%s] -> %s", card_id, reason);
    log_msg(line);
}
