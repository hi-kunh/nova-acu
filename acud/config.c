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
#define ACU_DEFAULT_TCP_PORT 9870
#define ACU_DEFAULT_PID_PATH "acud.pid"
#define ACU_DEFAULT_LOG_PATH ""   /* 빈 문자열 = stdout */
#define ACU_DEFAULT_NET_IFACE "eth0"
#define ACU_DEFAULT_NETCFG_REQUEST_PATH "/run/acud/netcfg-request"
/*
 * 기본은 "비밀번호 요구 안 함"이다. 기존 IntelliScan Device Manager는 SETT에 비밀번호를
 * 실어 보낼 수단이 아예 없다(UI에 입력란이 없고, 소스에서도 PassCustom을 채우지 않는다).
 * 요구하도록 기본값을 잡으면 제품이 기존 도구로 설정되지 않는다.
 */
#define ACU_DEFAULT_SETT_PASSWORD ""
/* DM이 보여 주는 기본값과 같은 값으로 맞춘다 (10분) */
#define ACU_DEFAULT_INACTIVITY_SECONDS 600
#define ACU_INACTIVITY_MAX 65535  /* 프레임의 2byte 필드 한계 */

void config_set_defaults(AcuConfig *cfg)
{
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", ACU_DEFAULT_DB_PATH);
    cfg->door_open_seconds = ACU_DEFAULT_DOOR_OPEN_SECONDS;
    snprintf(cfg->admin_password, sizeof(cfg->admin_password), "%s", ACU_DEFAULT_ADMIN_PASSWORD);
    cfg->tcp_port = ACU_DEFAULT_TCP_PORT;
    snprintf(cfg->pid_path, sizeof(cfg->pid_path), "%s", ACU_DEFAULT_PID_PATH);
    snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", ACU_DEFAULT_LOG_PATH);
    snprintf(cfg->net_iface, sizeof(cfg->net_iface), "%s", ACU_DEFAULT_NET_IFACE);
    snprintf(cfg->netcfg_request_path, sizeof(cfg->netcfg_request_path), "%s",
             ACU_DEFAULT_NETCFG_REQUEST_PATH);
    snprintf(cfg->discovery_sett_password, sizeof(cfg->discovery_sett_password), "%s",
             ACU_DEFAULT_SETT_PASSWORD);
    cfg->inactivity_seconds = ACU_DEFAULT_INACTIVITY_SECONDS;
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
    const cJSON *tcp_port = cJSON_GetObjectItemCaseSensitive(root, "tcp_port");
    const cJSON *pid_path = cJSON_GetObjectItemCaseSensitive(root, "pid_path");
    const cJSON *log_path = cJSON_GetObjectItemCaseSensitive(root, "log_path");
    const cJSON *net_iface = cJSON_GetObjectItemCaseSensitive(root, "net_iface");
    const cJSON *netcfg_request_path =
        cJSON_GetObjectItemCaseSensitive(root, "netcfg_request_path");
    const cJSON *sett_password =
        cJSON_GetObjectItemCaseSensitive(root, "discovery_sett_password");
    const cJSON *inactivity =
        cJSON_GetObjectItemCaseSensitive(root, "inactivity_seconds");

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
    if (!cJSON_IsNumber(tcp_port) || tcp_port->valueint < 1 || tcp_port->valueint > 65535)
    {
        log_msg("config.json: tcp_port 필드가 없거나 1~65535 범위를 벗어남 - 기존 설정 유지");
        cJSON_Delete(root);
        return -1;
    }

    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", db_path->valuestring);
    cfg->door_open_seconds = door_open_seconds->valueint;
    snprintf(cfg->admin_password, sizeof(cfg->admin_password), "%s", admin_password->valuestring);
    cfg->tcp_port = tcp_port->valueint;

    /*
     * 선택 필드. 없거나 빈 문자열이면 기본값으로 되돌린다
     * (키를 지우는 것만으로 기본 동작을 되찾을 수 있게 하기 위함).
     */
    if (cJSON_IsString(pid_path) && pid_path->valuestring[0] != '\0')
    {
        snprintf(cfg->pid_path, sizeof(cfg->pid_path), "%s", pid_path->valuestring);
    }
    else
    {
        snprintf(cfg->pid_path, sizeof(cfg->pid_path), "%s", ACU_DEFAULT_PID_PATH);
    }

    /* log_path는 빈 문자열 자체가 "stdout"이라는 유효한 값이다 */
    if (cJSON_IsString(log_path))
    {
        snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", log_path->valuestring);
    }
    else
    {
        snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", ACU_DEFAULT_LOG_PATH);
    }

    if (cJSON_IsString(net_iface) && net_iface->valuestring[0] != '\0')
    {
        snprintf(cfg->net_iface, sizeof(cfg->net_iface), "%s", net_iface->valuestring);
    }
    else
    {
        snprintf(cfg->net_iface, sizeof(cfg->net_iface), "%s", ACU_DEFAULT_NET_IFACE);
    }

    /* 빈 문자열 자체가 "SETT를 받지 않는다"는 유효한 값이다 */
    if (cJSON_IsString(netcfg_request_path))
    {
        snprintf(cfg->netcfg_request_path, sizeof(cfg->netcfg_request_path), "%s",
                 netcfg_request_path->valuestring);
    }
    else
    {
        snprintf(cfg->netcfg_request_path, sizeof(cfg->netcfg_request_path), "%s",
                 ACU_DEFAULT_NETCFG_REQUEST_PATH);
    }

    /* 0 자체가 "타임아웃 없음"이라는 유효한 값이다 */
    if (cJSON_IsNumber(inactivity) &&
        inactivity->valueint >= 0 && inactivity->valueint <= ACU_INACTIVITY_MAX)
    {
        cfg->inactivity_seconds = inactivity->valueint;
    }
    else
    {
        cfg->inactivity_seconds = ACU_DEFAULT_INACTIVITY_SECONDS;
    }

    /* 빈 문자열 자체가 "비밀번호를 요구하지 않는다"는 유효한 값이다 */
    if (cJSON_IsString(sett_password))
    {
        snprintf(cfg->discovery_sett_password, sizeof(cfg->discovery_sett_password), "%s",
                 sett_password->valuestring);
    }
    else
    {
        snprintf(cfg->discovery_sett_password, sizeof(cfg->discovery_sett_password), "%s",
                 ACU_DEFAULT_SETT_PASSWORD);
    }

    cJSON_Delete(root);
    return 0;
}
