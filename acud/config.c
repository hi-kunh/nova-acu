#include "config.h"
#include "log.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACU_DEFAULT_DB_PATH "acud.db"
#define ACU_DEFAULT_DOOR_OPEN_SECONDS 3
/* 최초 기본값. 반드시 웹 설정 화면에서 변경해야 하는 값 - 절대 이 기본값 그대로 배포하면 안 됨 */
#define ACU_DEFAULT_ADMIN_PASSWORD "0000"

void config_set_defaults(AcuConfig *cfg)
{
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", ACU_DEFAULT_DB_PATH);
    cfg->door_open_seconds = ACU_DEFAULT_DOOR_OPEN_SECONDS;
    snprintf(cfg->admin_password, sizeof(cfg->admin_password), "%s", ACU_DEFAULT_ADMIN_PASSWORD);
}

/* 정확히 숫자 4자리 문자열인지 확인한다 */
static int is_valid_admin_password(const char *s)
{
    if (!s || strlen(s) != 4)
    {
        return 0;
    }
    for (int i = 0; i < 4; i++)
    {
        if (!isdigit((unsigned char)s[i]))
        {
            return 0;
        }
    }
    return 1;
}

/* path의 파일 전체를 malloc한 버퍼에 읽어들인다. 실패 시 NULL. 호출자가 free 해야 한다. */
static char *read_file_all(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_len] = '\0';
    return buf;
}

int config_load(const char *path, AcuConfig *cfg)
{
    char *text = read_file_all(path);
    if (!text)
    {
        log_msg("config.json 읽기 실패 (파일 없음 또는 열기 실패) - 기존 설정 유지");
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);

    if (!root)
    {
        log_msg("config.json 파싱 실패 (JSON 형식 오류) - 기존 설정 유지");
        return -1;
    }

    const cJSON *db_path = cJSON_GetObjectItemCaseSensitive(root, "db_path");
    const cJSON *door_open_seconds = cJSON_GetObjectItemCaseSensitive(root, "door_open_seconds");
    const cJSON *admin_password = cJSON_GetObjectItemCaseSensitive(root, "admin_password");

    if (!cJSON_IsString(db_path) || db_path->valuestring[0] == '\0')
    {
        log_msg("config.json: db_path 필드가 없거나 잘못됨 - 기존 설정 유지");
        cJSON_Delete(root);
        return -1;
    }
    if (!cJSON_IsNumber(door_open_seconds) ||
        door_open_seconds->valueint < 1 || door_open_seconds->valueint > 99)
    {
        /* IDTi Device Output(Relay) ActiveTime 범위(1~99초)와 맞춤 */
        log_msg("config.json: door_open_seconds 필드가 없거나 1~99 범위를 벗어남 - 기존 설정 유지");
        cJSON_Delete(root);
        return -1;
    }
    if (!cJSON_IsString(admin_password) || !is_valid_admin_password(admin_password->valuestring))
    {
        log_msg("config.json: admin_password 필드가 없거나 숫자 4자리가 아님 - 기존 설정 유지");
        cJSON_Delete(root);
        return -1;
    }

    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", db_path->valuestring);
    cfg->door_open_seconds = door_open_seconds->valueint;
    snprintf(cfg->admin_password, sizeof(cfg->admin_password), "%s", admin_password->valuestring);

    cJSON_Delete(root);
    return 0;
}
