#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdio.h>
#include <time.h>

void log_msg(const char *msg)
{
    time_t now = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);  /* headless 환경에서 로그가 즉시 보이도록 버퍼를 비운다 */
}
