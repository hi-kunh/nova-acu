#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "loop.h"
#include "log.h"

typedef struct {
    int        fd;
    unsigned   events;
    AcuLoopCb  cb;
    void      *user;
} LoopEntry;

struct AcuLoop {
    LoopEntry ent[ACU_LOOP_MAX_FDS];
    int       count;
};

/* 등록 목록에서 fd의 자리를 찾는다. 없으면 -1 */
static int find_entry(const AcuLoop *l, int fd)
{
    for (int i = 0; i < l->count; i++)
    {
        if (l->ent[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

AcuLoop *loop_create(void)
{
    return calloc(1, sizeof(AcuLoop));
}

void loop_destroy(AcuLoop *l)
{
    free(l);
}

int loop_add(AcuLoop *l, int fd, unsigned events, AcuLoopCb cb, void *user)
{
    if (!l || fd < 0 || fd >= FD_SETSIZE || !cb)
    {
        return -1;
    }

    int i = find_entry(l, fd);
    if (i < 0)
    {
        if (l->count >= ACU_LOOP_MAX_FDS)
        {
            log_msg("루프: 감시할 수 있는 fd 수를 넘었다 - 등록 거부");
            return -1;
        }
        i = l->count++;
    }

    l->ent[i].fd = fd;
    l->ent[i].events = events;
    l->ent[i].cb = cb;
    l->ent[i].user = user;
    return 0;
}

int loop_mod(AcuLoop *l, int fd, unsigned events)
{
    if (!l)
    {
        return -1;
    }
    int i = find_entry(l, fd);
    if (i < 0)
    {
        return -1;
    }
    l->ent[i].events = events;
    return 0;
}

void loop_remove(AcuLoop *l, int fd)
{
    if (!l)
    {
        return;
    }
    int i = find_entry(l, fd);
    if (i < 0)
    {
        return;
    }
    /* 마지막 항목을 빈자리로 옮긴다 (순서는 의미가 없다) */
    l->ent[i] = l->ent[l->count - 1];
    l->count--;
}

int loop_wait(AcuLoop *l, int timeout_ms)
{
    if (!l || l->count == 0)
    {
        return -1;
    }

    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    int maxfd = -1;

    for (int i = 0; i < l->count; i++)
    {
        const LoopEntry *e = &l->ent[i];
        if (e->events & ACU_LOOP_READ)
        {
            FD_SET(e->fd, &readfds);
        }
        if (e->events & ACU_LOOP_WRITE)
        {
            FD_SET(e->fd, &writefds);
        }
        if ((e->events & (ACU_LOOP_READ | ACU_LOOP_WRITE)) && e->fd > maxfd)
        {
            maxfd = e->fd;
        }
    }

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms >= 0)
    {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int rc = select(maxfd + 1, &readfds, &writefds, NULL, ptv);
    if (rc <= 0)
    {
        return rc;
    }

    /*
     * 준비된 fd를 먼저 모아 둔 뒤에 콜백을 부른다.
     * 콜백이 등록을 바꿀 수 있기 때문이다 - 예를 들어 리슨 fd의 콜백은 새 클라이언트 fd를
     * 등록하고, 수신 콜백은 연결이 끊기면 자기 fd를 해제한다. 목록을 돌면서 부르면
     * 방금 옮겨진 항목을 건너뛰거나 두 번 부르게 된다.
     */
    struct { int fd; unsigned events; } ready[ACU_LOOP_MAX_FDS];
    int ready_count = 0;

    for (int i = 0; i < l->count; i++)
    {
        unsigned ev = 0;
        if ((l->ent[i].events & ACU_LOOP_READ) && FD_ISSET(l->ent[i].fd, &readfds))
        {
            ev |= ACU_LOOP_READ;
        }
        if ((l->ent[i].events & ACU_LOOP_WRITE) && FD_ISSET(l->ent[i].fd, &writefds))
        {
            ev |= ACU_LOOP_WRITE;
        }
        if (ev)
        {
            ready[ready_count].fd = l->ent[i].fd;
            ready[ready_count].events = ev;
            ready_count++;
        }
    }

    for (int i = 0; i < ready_count; i++)
    {
        /* 앞선 콜백이 이 fd를 해제했을 수 있으므로 매번 다시 찾는다 */
        int idx = find_entry(l, ready[i].fd);
        if (idx >= 0)
        {
            l->ent[idx].cb(ready[i].fd, ready[i].events, l->ent[idx].user);
        }
    }

    return rc;
}
