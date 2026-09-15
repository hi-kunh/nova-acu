#include <stdio.h>
#include <string.h>

#include "devset.h"
#include "log.h"

int devset_init(sqlite3 *db)
{
    static const char *SQL =
        "CREATE TABLE IF NOT EXISTS device_settings ("
        "    object INTEGER NOT NULL,"
        "    module INTEGER NOT NULL,"
        "    slot   INTEGER NOT NULL,"
        "    data   BLOB    NOT NULL,"   /* 받은 byte 그대로 */
        "    PRIMARY KEY (object, module, slot)"
        ");";

    char *err = NULL;
    if (sqlite3_exec(db, SQL, NULL, NULL, &err) != SQLITE_OK)
    {
        char line[300];
        snprintf(line, sizeof(line), "장치 설정: 표 생성 실패 (%s)", err ? err : "?");
        log_msg(line);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int devset_get(sqlite3 *db, uint8_t object, int module, int slot, uint8_t *out, size_t out_len)
{
    if (!db || !out)
    {
        return -1;
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT data FROM device_settings WHERE object=? AND module=? AND slot=?",
            -1, &st, NULL) != SQLITE_OK)
    {
        return -1;
    }
    sqlite3_bind_int(st, 1, object);
    sqlite3_bind_int(st, 2, module);
    sqlite3_bind_int(st, 3, slot);

    int result = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob && (size_t)n == out_len)
        {
            memcpy(out, blob, out_len);
            result = 1;
        }
        else
        {
            /* 크기가 다르면 다른 형식으로 저장된 것이다 - 틀린 값을 돌려주느니 없는 것으로 친다 */
            char line[160];
            snprintf(line, sizeof(line),
                     "장치 설정: (0x%02x, 모듈 %d, 칸 %d) 크기가 %d로 기대(%zu)와 다르다",
                     object, module, slot, n, out_len);
            log_msg(line);
            result = -1;
        }
    }
    sqlite3_finalize(st);
    return result;
}

int devset_put(sqlite3 *db, uint8_t object, int module, int slot, const uint8_t *data, size_t len)
{
    if (!db || !data || len == 0)
    {
        return -1;
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO device_settings(object, module, slot, data) VALUES(?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
    {
        return -1;
    }
    sqlite3_bind_int(st, 1, object);
    sqlite3_bind_int(st, 2, module);
    sqlite3_bind_int(st, 3, slot);
    sqlite3_bind_blob(st, 4, data, (int)len, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
