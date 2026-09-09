#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* NULL이면 stdout을 쓴다는 뜻 (열린 파일이 없는 상태) */
static FILE *g_file = NULL;
static char  g_path[256] = "";

static FILE *log_stream(void)
{
    return g_file ? g_file : stdout;
}

void log_open(const char *path)
{
    log_close();

    if (!path || path[0] == '\0')
    {
        g_path[0] = '\0';
        return;
    }

    snprintf(g_path, sizeof(g_path), "%s", path);

    g_file = fopen(g_path, "a");
    if (!g_file)
    {
        /* 로그를 못 여는 것이 데몬을 멈출 이유는 아니다. stdout으로 계속 간다 */
        char line[320];
        snprintf(line, sizeof(line), "로그 파일 열기 실패 (%s) -> stdout으로 출력한다", g_path);
        g_path[0] = '\0';
        log_msg(line);
    }
}

void log_reopen(void)
{
    if (!g_file)
    {
        return; /* stdout을 쓰는 중 */
    }

    char path[sizeof(g_path)];
    snprintf(path, sizeof(path), "%s", g_path);
    log_open(path);
}

void log_close(void)
{
    if (g_file)
    {
        fclose(g_file);
        g_file = NULL;
    }
}

const char *log_path(void)
{
    return g_path;
}

void log_msg(const char *msg)
{
    time_t now = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    FILE *out = log_stream();
    fprintf(out, "[%s] %s\n", ts, msg);
    fflush(out);  /* headless 환경에서 로그가 즉시 보이도록 버퍼를 비운다 */
}
