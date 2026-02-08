#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <Python.h>
#include <structmember.h>

#include "SPMCQueue.h"
#include "rtp_sync.h"

#define MODULE_NAME "rtpsynth.RtpServer"
#define DEFAULT_TICK_HZ 200U
#define MAX_UDP_PACKET 65535
#define CHANNEL_OUTQ_CAPACITY 32U
_Static_assert((CHANNEL_OUTQ_CAPACITY & (CHANNEL_OUTQ_CAPACITY - 1)) == 0,
    "CHANNEL_OUTQ_CAPACITY must be a power of two");

typedef struct rtp_send_item {
    const unsigned char *data;
    size_t size;
    PyObject *data_ref;
} RtpSendItem;

typedef struct {
    int fd;
    PyObject *pkt_in_cb;
    SPMCQueue *out_q;
} RtpChannelDetached;

typedef struct rtp_channel_state {
    int active;
    uint64_t id;
    int fd;
    int has_target;
    struct sockaddr_storage target_addr;
    socklen_t target_len;
    PyObject *pkt_in_cb;
    SPMCQueue *out_q;
} RtpChannelState;

typedef enum {
    CMD_ADD_CHANNEL = 1,
    CMD_REMOVE_CHANNEL,
    CMD_SET_TARGET,
    CMD_SHUTDOWN,
} RtpCommandType;

typedef rtp_sync_waiter RtpCmdWaiter;

typedef struct rtp_server_cmd {
    RtpCommandType type;
    struct rtp_server_cmd *next;
    RtpCmdWaiter *waiter;
    union {
        struct {
            uint64_t id;
            int fd;
            size_t queue_size;
            PyObject *pkt_in_cb;
            SPMCQueue *out_q;
        } add_channel;
        struct {
            uint64_t id;
        } remove_channel;
        struct {
            uint64_t id;
            struct sockaddr_storage addr;
            socklen_t addrlen;
        } set_target;
    } u;
} RtpServerCmd;

typedef struct {
    PyObject_HEAD
    pthread_t worker;
    int worker_running;
    int mutex_inited;
    int cond_inited;
    int cmd_waiter_inited;
    int cmd_waiter_busy;
    int shutdown_queued;
    int accepting_commands;
    uint64_t next_channel_id;
    uint64_t tick_ns;
    clockid_t cmd_cv_clock;
    pthread_mutex_t cmd_lock;
    pthread_cond_t cmd_cv;
    RtpCmdWaiter cmd_waiter;
    RtpServerCmd *cmd_head;
    RtpServerCmd *cmd_tail;
    RtpChannelState *channels;
    size_t channels_cap;
    size_t channels_active;
    struct pollfd *pollfds;
    RtpChannelState **pollfds_index;
    size_t pollfds_len;
    size_t pollfds_cap;
    int pollfds_dirty;
} PyRtpServer;

typedef struct {
    PyObject_HEAD
    PyObject *server_obj;
    uint64_t id;
    int closed;
    int has_target;
    SPMCQueue *out_q;
    struct sockaddr_storage local_addr;
    socklen_t local_len;
} PyRtpChannel;

static PyTypeObject PyRtpServerType;
static PyTypeObject PyRtpChannelType;
static PyObject *RtpQueueFullError;

static uint64_t
now_ns_monotonic(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static uint64_t
now_ns_for_clock(clockid_t clock_id)
{
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0)
        abort();
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static rtp_sync_cmdq
server_cmdq_ctx(PyRtpServer *self)
{
    rtp_sync_cmdq ctx = {
        .head = (void **)&self->cmd_head,
        .tail = (void **)&self->cmd_tail,
        .next_off = offsetof(RtpServerCmd, next),
    };
    return ctx;
}

static rtp_sync_cond_ctx
server_cmdcv_ctx(PyRtpServer *self)
{
    rtp_sync_cond_ctx ctx = {
        .cv = &self->cmd_cv,
        .lock = &self->cmd_lock,
        .clock_id = &self->cmd_cv_clock,
    };
    return ctx;
}

static int
parse_bind_family(PyObject *obj, int *out_family)
{
    const char *name;
    long value;

    if (out_family == NULL)
        return -1;

    if (obj == NULL || obj == Py_None) {
        *out_family = AF_UNSPEC;
        return 0;
    }

    if (PyLong_Check(obj)) {
        value = PyLong_AsLong(obj);
        if (PyErr_Occurred())
            return -1;
        if (value == 0 || value == AF_UNSPEC) {
            *out_family = AF_UNSPEC;
            return 0;
        }
        if (value == 4 || value == AF_INET) {
            *out_family = AF_INET;
            return 0;
        }
        if (value == 6 || value == AF_INET6) {
            *out_family = AF_INET6;
            return 0;
        }
        PyErr_SetString(PyExc_ValueError,
            "bind_family must be one of: 0, 4, 6, AF_UNSPEC, AF_INET, AF_INET6");
        return -1;
    }

    if (PyUnicode_Check(obj)) {
        name = PyUnicode_AsUTF8(obj);
        if (name == NULL)
            return -1;
        if (strcmp(name, "auto") == 0 || strcmp(name, "unspec") == 0 ||
                strcmp(name, "any") == 0) {
            *out_family = AF_UNSPEC;
            return 0;
        }
        if (strcmp(name, "ipv4") == 0 || strcmp(name, "inet") == 0 ||
                strcmp(name, "af_inet") == 0) {
            *out_family = AF_INET;
            return 0;
        }
        if (strcmp(name, "ipv6") == 0 || strcmp(name, "inet6") == 0 ||
                strcmp(name, "af_inet6") == 0) {
            *out_family = AF_INET6;
            return 0;
        }
        PyErr_SetString(PyExc_ValueError,
            "bind_family string must be one of: auto, ipv4, ipv6");
        return -1;
    }

    PyErr_SetString(PyExc_TypeError,
        "bind_family must be an int, str, or None");
    return -1;
}

static void
close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int
set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void
py_xdecref_on_worker(PyObject *obj)
{
    PyGILState_STATE gstate;
    assert(obj != NULL);
    gstate = PyGILState_Ensure();
    Py_DECREF(obj);
    PyGILState_Release(gstate);
}

static void
free_send_item(RtpSendItem *item)
{
    assert(item != NULL);
    assert(item->data_ref != NULL);
    py_xdecref_on_worker(item->data_ref);
    item->data_ref = NULL;
    free(item);
}

static void
free_send_queue(SPMCQueue *queue)
{
    assert(queue != NULL);
    for (;;) {
        void *obj = NULL;
        if (!try_pop(queue, &obj))
            break;
        free_send_item((RtpSendItem *)obj);
    }
}

static void
destroy_send_queue(SPMCQueue **queuep)
{
    assert(queuep != NULL);
    assert(*queuep != NULL);
    free_send_queue(*queuep);
    destroy_queue(*queuep);
    *queuep = NULL;
}

static void
detached_init(RtpChannelDetached *dc)
{
    assert(dc != NULL);
    dc->fd = -1;
    dc->pkt_in_cb = NULL;
    dc->out_q = NULL;
}

static void
detached_cleanup(RtpChannelDetached *dc)
{
    assert(dc != NULL);
    close_fd(&dc->fd);
    py_xdecref_on_worker(dc->pkt_in_cb);
    dc->pkt_in_cb = NULL;
    destroy_send_queue(&dc->out_q);
}

static void
release_channel_state(RtpChannelState *channel)
{
    assert(channel != NULL);
    if (!channel->active)
        return;
    close_fd(&channel->fd);
    destroy_send_queue(&channel->out_q);
    py_xdecref_on_worker(channel->pkt_in_cb);
    channel->pkt_in_cb = NULL;
    channel->id = 0;
    channel->has_target = 0;
    channel->target_len = 0;
    channel->active = 0;
}

static void
clear_channels(PyRtpServer *self)
{
    size_t i;

    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++)
        release_channel_state(&self->channels[i]);
    free(self->channels);
    self->channels = NULL;
    self->channels_cap = 0;
    self->channels_active = 0;
}

static int
ensure_channel_capacity(PyRtpServer *self, size_t need)
{
    size_t new_cap;
    RtpChannelState *new_channels;
    size_t old_cap;

    assert(self != NULL);
    if (need <= self->channels_cap)
        return 0;

    new_cap = self->channels_cap == 0 ? 4 : self->channels_cap;
    while (new_cap < need) {
        if (new_cap > (SIZE_MAX / 2))
            return -1;
        new_cap *= 2;
    }

    old_cap = self->channels_cap;
    new_channels = realloc(self->channels, new_cap * sizeof(*self->channels));
    if (new_channels == NULL)
        return -1;
    memset(new_channels + old_cap, 0, (new_cap - old_cap) * sizeof(*new_channels));
    for (size_t i = old_cap; i < new_cap; i++)
        new_channels[i].fd = -1;
    self->channels = new_channels;
    self->channels_cap = new_cap;
    self->pollfds_dirty = 1;
    return 0;
}

static ssize_t
alloc_channel_slot(PyRtpServer *self)
{
    size_t i;
    size_t old_cap;

    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++) {
        if (!self->channels[i].active)
            return (ssize_t)i;
    }

    old_cap = self->channels_cap;
    if (old_cap > (SIZE_MAX / 2))
        return -1;
    if (ensure_channel_capacity(self, old_cap == 0 ? 4 : (old_cap * 2)) != 0)
        return -1;
    return (ssize_t)old_cap;
}

static ssize_t
find_channel_index(PyRtpServer *self, uint64_t id)
{
    size_t i;

    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++) {
        if (self->channels[i].active && self->channels[i].id == id)
            return (ssize_t)i;
    }
    return -1;
}

static RtpChannelState *
find_channel(PyRtpServer *self, uint64_t id)
{
    ssize_t idx = find_channel_index(self, id);
    if (idx < 0)
        return NULL;
    return &self->channels[idx];
}

static int
remove_channel(PyRtpServer *self, uint64_t id, RtpChannelDetached *detached)
{
    ssize_t idx;
    RtpChannelState *ch;

    assert(self != NULL);
    idx = find_channel_index(self, id);
    if (idx < 0)
        return 0;

    ch = &self->channels[idx];
    if (detached != NULL) {
        detached->fd = ch->fd;
        detached->pkt_in_cb = ch->pkt_in_cb;
        detached->out_q = ch->out_q;
    } else {
        close_fd(&ch->fd);
        py_xdecref_on_worker(ch->pkt_in_cb);
        destroy_send_queue(&ch->out_q);
    }
    ch->fd = -1;
    ch->pkt_in_cb = NULL;
    ch->out_q = NULL;
    ch->id = 0;
    ch->has_target = 0;
    ch->target_len = 0;
    ch->active = 0;
    if (self->channels_active > 0)
        self->channels_active -= 1;
    return 1;
}

static void
clear_poll_cache(PyRtpServer *self)
{
    assert(self != NULL);
    free(self->pollfds);
    free(self->pollfds_index);
    self->pollfds = NULL;
    self->pollfds_index = NULL;
    self->pollfds_len = 0;
    self->pollfds_cap = 0;
    self->pollfds_dirty = 0;
}

static void
server_waiter_release(PyRtpServer *self)
{
    if (self == NULL || !self->mutex_inited)
        return;
    if (pthread_mutex_lock(&self->cmd_lock) == 0) {
        self->cmd_waiter_busy = 0;
        pthread_mutex_unlock(&self->cmd_lock);
    }
}

static int
server_waiter_acquire(PyRtpServer *self, RtpCmdWaiter **out_waiter)
{
    assert(self != NULL);
    assert(out_waiter != NULL);

    if (!self->cmd_waiter_inited) {
        PyErr_SetString(PyExc_RuntimeError, "RtpServer waiter is not initialized");
        return -1;
    }
    if (pthread_mutex_lock(&self->cmd_lock) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to lock command queue");
        return -1;
    }
    if (self->cmd_waiter_busy) {
        pthread_mutex_unlock(&self->cmd_lock);
        PyErr_SetString(PyExc_RuntimeError,
            "another synchronous command is already in progress");
        return -1;
    }
    self->cmd_waiter_busy = 1;
    pthread_mutex_unlock(&self->cmd_lock);

    if (rtp_sync_waiter_reset(&self->cmd_waiter) != 0) {
        server_waiter_release(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to reset command waiter");
        return -1;
    }

    *out_waiter = &self->cmd_waiter;
    return 0;
}

static int
refresh_poll_cache(PyRtpServer *self)
{
    size_t need;
    size_t i = 0;
    size_t j;

    assert(self != NULL);
    if (!self->pollfds_dirty)
        return 0;

    need = self->channels_active;
    if (need == 0) {
        self->pollfds_len = 0;
        self->pollfds_dirty = 0;
        return 0;
    }

    if (need > self->pollfds_cap) {
        struct pollfd *new_pfds;
        RtpChannelState **new_index;

        new_pfds = realloc(self->pollfds, need * sizeof(*self->pollfds));
        if (new_pfds == NULL)
            return -1;
        self->pollfds = new_pfds;

        new_index = realloc(self->pollfds_index,
            need * sizeof(*self->pollfds_index));
        if (new_index == NULL)
            return -1;
        self->pollfds_index = new_index;
        self->pollfds_cap = need;
    }

    for (j = 0; j < self->channels_cap; j++) {
        if (!self->channels[j].active)
            continue;
        self->pollfds[i].fd = self->channels[j].fd;
        self->pollfds[i].events = POLLIN;
        self->pollfds[i].revents = 0;
        self->pollfds_index[i] = &self->channels[j];
        i += 1;
    }
    self->pollfds_len = i;
    self->pollfds_dirty = 0;
    return 0;
}

static void
free_command(RtpServerCmd *cmd)
{
    assert(cmd != NULL);

    if (cmd->type == CMD_ADD_CHANNEL) {
        if (cmd->u.add_channel.pkt_in_cb != NULL) {
            py_xdecref_on_worker(cmd->u.add_channel.pkt_in_cb);
            cmd->u.add_channel.pkt_in_cb = NULL;
        }
        close_fd(&cmd->u.add_channel.fd);
        if (cmd->u.add_channel.out_q != NULL)
            destroy_send_queue(&cmd->u.add_channel.out_q);
    }
    free(cmd);
}

static void
free_command_list(RtpServerCmd *head)
{
    while (head != NULL) {
        RtpServerCmd *next = head->next;
        free_command(head);
        head = next;
    }
}

static int
enqueue_command(PyRtpServer *self, RtpServerCmd *cmd, int with_error)
{
    int rejected = 0;

    assert(self != NULL);
    assert(cmd != NULL);
    if (!self->mutex_inited || !self->cond_inited) {
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "RtpServer is not initialized");
        free_command(cmd);
        return -1;
    }

    if (pthread_mutex_lock(&self->cmd_lock) != 0) {
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "failed to lock command queue");
        free_command(cmd);
        return -1;
    }

    if (!self->accepting_commands) {
        rejected = 1;
    } else {
        rtp_sync_cmdq cmdq = server_cmdq_ctx(self);
        rtp_sync_cmdq_push(&cmdq, cmd);
        pthread_cond_signal(&self->cmd_cv);
    }

    pthread_mutex_unlock(&self->cmd_lock);

    if (rejected) {
        free_command(cmd);
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "RtpServer is shutting down");
        return -1;
    }

    return 0;
}

static RtpServerCmd *
detach_commands(PyRtpServer *self)
{
    RtpServerCmd *head = NULL;

    assert(self != NULL);
    if (!self->mutex_inited)
        return NULL;

    if (pthread_mutex_lock(&self->cmd_lock) != 0)
        return NULL;

    {
        rtp_sync_cmdq cmdq = server_cmdq_ctx(self);
        head = (RtpServerCmd *)rtp_sync_cmdq_detach_all(&cmdq);
    }

    pthread_mutex_unlock(&self->cmd_lock);
    return head;
}

static int
wait_for_commands(PyRtpServer *self, uint64_t wait_until_ns, int wait_forever)
{
    int rc = 0;

    if (pthread_mutex_lock(&self->cmd_lock) != 0)
        return -1;

    if (wait_forever) {
        while (self->cmd_head == NULL && rc == 0)
            rc = pthread_cond_wait(&self->cmd_cv, &self->cmd_lock);
    } else {
        while (self->cmd_head == NULL) {
            rtp_sync_cond_ctx cond_ctx = server_cmdcv_ctx(self);
            rc = rtp_sync_cond_timedwait_abs_ns(&cond_ctx, wait_until_ns);
            if (rc != 0)
                break;
        }
    }

    pthread_mutex_unlock(&self->cmd_lock);
    if (rc == ETIMEDOUT)
        return 0;
    return rc;
}

static int
sockaddr_to_tuple(const struct sockaddr *sa, socklen_t salen, PyObject **out)
{
    char host[INET6_ADDRSTRLEN + 16];
    int port = 0;

    (void)salen;

    if (sa == NULL || out == NULL)
        return -1;

    memset(host, 0, sizeof(host));
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL)
            return -1;
        port = (int)ntohs(sin->sin_port);
        *out = Py_BuildValue("(si)", host, port);
        return *out == NULL ? -1 : 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) == NULL)
            return -1;
        port = (int)ntohs(sin6->sin6_port);
        *out = Py_BuildValue("(si)", host, port);
        return *out == NULL ? -1 : 0;
    }

    *out = Py_BuildValue("(si)", "", 0);
    return *out == NULL ? -1 : 0;
}

static int
resolve_udp_addr(const char *host, int port, int passive, int family_hint,
    struct sockaddr_storage *out, socklen_t *outlen, int *outfamily,
    int with_error)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char port_buf[16];
    int gai_rc;

    if (out == NULL || outlen == NULL)
        return -1;

    if (port < 0 || port > 65535) {
        if (with_error)
            PyErr_SetString(PyExc_ValueError, "port must be in range 0..65535");
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = (family_hint == AF_INET || family_hint == AF_INET6) ?
        family_hint : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    if (passive)
        hints.ai_flags |= AI_PASSIVE;

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    gai_rc = getaddrinfo((host != NULL && host[0] != '\0') ? host : NULL,
        port_buf, &hints, &res);
    if (gai_rc != 0 || res == NULL) {
        if (with_error) {
            PyErr_Format(PyExc_OSError, "getaddrinfo failed: %s",
                gai_strerror(gai_rc));
        }
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
            continue;
        if ((size_t)ai->ai_addrlen > sizeof(*out))
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out, ai->ai_addr, (size_t)ai->ai_addrlen);
        *outlen = (socklen_t)ai->ai_addrlen;
        if (outfamily != NULL)
            *outfamily = ai->ai_family;
        freeaddrinfo(res);
        return 0;
    }

    freeaddrinfo(res);
    if (with_error)
        PyErr_SetString(PyExc_OSError, "failed to resolve a UDP address");
    return -1;
}

static void
invoke_pkt_callback(PyObject *pkt_in_cb, const unsigned char *data, size_t size,
    const struct sockaddr *sa, socklen_t salen, uint64_t rtime)
{
    PyGILState_STATE gstate;
    PyObject *pkt = NULL;
    PyObject *addr = NULL;
    PyObject *rtime_obj = NULL;
    PyObject *result = NULL;

    if (pkt_in_cb == NULL)
        return;

    gstate = PyGILState_Ensure();

    pkt = PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)size);
    if (pkt == NULL)
        goto out;

    if (sockaddr_to_tuple(sa, salen, &addr) != 0)
        goto out;

    rtime_obj = PyLong_FromUnsignedLongLong((unsigned long long)rtime);
    if (rtime_obj == NULL)
        goto out;

    result = PyObject_CallFunctionObjArgs(pkt_in_cb, pkt, addr, rtime_obj, NULL);
    if (result == NULL) {
        PyErr_WriteUnraisable(pkt_in_cb);
    }

out:
    Py_XDECREF(result);
    Py_XDECREF(rtime_obj);
    Py_XDECREF(pkt);
    Py_XDECREF(addr);
    PyGILState_Release(gstate);
}

static void
receive_for_channel(RtpChannelState *ch, uint64_t rtime)
{
    unsigned char buf[MAX_UDP_PACKET];

    if (ch == NULL || ch->fd < 0)
        return;

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peerlen = sizeof(peer);
        ssize_t nread;

        nread = recvfrom(ch->fd, buf, sizeof(buf), 0,
            (struct sockaddr *)&peer, &peerlen);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }

        invoke_pkt_callback(ch->pkt_in_cb, buf, (size_t)nread,
            (const struct sockaddr *)&peer, peerlen, rtime);
    }
}

static void
drain_outputs(PyRtpServer *self)
{
    size_t i;

    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++) {
        RtpChannelState *ch = &self->channels[i];
        int has_target;
        if (!ch->active)
            continue;
        has_target = ch->has_target;
        for (;;) {
            void *obj = NULL;
            RtpSendItem *item;
            if (!try_pop(ch->out_q, &obj))
                break;
            item = (RtpSendItem *)obj;
            if (has_target) {
                (void)sendto(ch->fd, item->data, item->size, 0,
                    (const struct sockaddr *)&ch->target_addr,
                    ch->target_len);
            }
            free_send_item(item);
        }
    }
}

static int
poll_inputs(PyRtpServer *self)
{
    size_t nchan;
    size_t i = 0;
    int rc;

    assert(self != NULL);
    nchan = self->pollfds_len;
    if (nchan == 0)
        return 0;

    rc = poll(self->pollfds, (nfds_t)nchan, 0);
    if (rc > 0) {
        uint64_t rtime = now_ns_monotonic();
        for (i = 0; i < nchan; i++) {
            if ((self->pollfds[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0)
                receive_for_channel(self->pollfds_index[i], rtime);
        }
    }

    return 0;
}

static void
process_commands(PyRtpServer *self, int *shutdown_seen)
{
    RtpServerCmd *cmd;

    cmd = detach_commands(self);
    while (cmd != NULL) {
        RtpServerCmd *next = cmd->next;
        int cmd_status = 0;

        if (cmd->type == CMD_ADD_CHANNEL) {
            if (pthread_mutex_lock(&self->cmd_lock) == 0) {
                ssize_t slot = alloc_channel_slot(self);
                if (slot >= 0) {
                    RtpChannelState *ch = &self->channels[slot];
                    assert(cmd->u.add_channel.out_q != NULL);
                    assert(!ch->active);
                    ch->active = 1;
                    ch->id = cmd->u.add_channel.id;
                    ch->fd = cmd->u.add_channel.fd;
                    ch->has_target = 0;
                    ch->target_len = 0;
                    memset(&ch->target_addr, 0, sizeof(ch->target_addr));
                    ch->pkt_in_cb = cmd->u.add_channel.pkt_in_cb;
                    ch->out_q = cmd->u.add_channel.out_q;
                    cmd->u.add_channel.pkt_in_cb = NULL;
                    cmd->u.add_channel.fd = -1;
                    cmd->u.add_channel.out_q = NULL;
                    self->channels_active += 1;
                    self->pollfds_dirty = 1;
                } else {
                    cmd_status = ENOMEM;
                }
                pthread_mutex_unlock(&self->cmd_lock);
            } else {
                cmd_status = EBUSY;
            }
        } else if (cmd->type == CMD_REMOVE_CHANNEL) {
            RtpChannelDetached detached;
            int removed = 0;
            detached_init(&detached);
            if (pthread_mutex_lock(&self->cmd_lock) == 0) {
                if (remove_channel(self, cmd->u.remove_channel.id, &detached) != 0) {
                    removed = 1;
                    self->pollfds_dirty = 1;
                }
                pthread_mutex_unlock(&self->cmd_lock);
            }
            if (removed)
                detached_cleanup(&detached);
        } else if (cmd->type == CMD_SET_TARGET) {
            if (pthread_mutex_lock(&self->cmd_lock) == 0) {
                RtpChannelState *ch = find_channel(self, cmd->u.set_target.id);
                if (ch != NULL) {
                    ch->target_addr = cmd->u.set_target.addr;
                    ch->target_len = cmd->u.set_target.addrlen;
                    ch->has_target = 1;
                } else {
                    cmd_status = ENOENT;
                }
                pthread_mutex_unlock(&self->cmd_lock);
            } else {
                cmd_status = EBUSY;
            }
        } else if (cmd->type == CMD_SHUTDOWN) {
            if (shutdown_seen != NULL)
                *shutdown_seen = 1;
        }

        if (cmd->waiter != NULL)
            rtp_sync_waiter_complete(cmd->waiter, cmd_status);
        free_command(cmd);
        cmd = next;
    }
}

static void *
rtp_server_worker(void *arg)
{
    PyRtpServer *self = (PyRtpServer *)arg;
    uint64_t next_tick_ns = 0;

    for (;;) {
        int shutdown_seen = 0;
        size_t active;
        uint64_t now_ns;

        process_commands(self, &shutdown_seen);
        if (shutdown_seen)
            break;

        if (refresh_poll_cache(self) != 0) {
            if (self->pollfds_len == 0) {
                next_tick_ns = 0;
                (void)wait_for_commands(self, 0, 1);
            }
            continue;
        }
        active = self->pollfds_len;
        if (active == 0) {
            next_tick_ns = 0;
            (void)wait_for_commands(self, 0, 1);
            continue;
        }

        now_ns = now_ns_for_clock(self->cmd_cv_clock);
        if (next_tick_ns == 0)
            next_tick_ns = now_ns;
        if (now_ns < next_tick_ns) {
            (void)wait_for_commands(self, next_tick_ns, 0);
            continue;
        }

        (void)poll_inputs(self);
        drain_outputs(self);

        if (UINT64_MAX - next_tick_ns < self->tick_ns) {
            next_tick_ns = now_ns;
        } else {
            next_tick_ns += self->tick_ns;
            while (next_tick_ns <= now_ns) {
                if (UINT64_MAX - next_tick_ns < self->tick_ns) {
                    next_tick_ns = now_ns;
                    break;
                }
                next_tick_ns += self->tick_ns;
            }
        }
    }

    clear_channels(self);
    clear_poll_cache(self);
    free_command_list(detach_commands(self));
    return NULL;
}

static int
rtp_server_shutdown_internal(PyRtpServer *self)
{
    RtpServerCmd *cmd;

    assert(self != NULL);
    if (!self->worker_running)
        return 0;

    if (pthread_mutex_lock(&self->cmd_lock) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to lock command queue");
        return -1;
    }

    if (!self->shutdown_queued) {
        cmd = calloc(1, sizeof(*cmd));
        if (cmd == NULL) {
            pthread_mutex_unlock(&self->cmd_lock);
            PyErr_NoMemory();
            return -1;
        }
        cmd->type = CMD_SHUTDOWN;
        rtp_sync_cmdq cmdq = server_cmdq_ctx(self);
        rtp_sync_cmdq_push(&cmdq, cmd);
        self->shutdown_queued = 1;
        self->accepting_commands = 0;
        pthread_cond_signal(&self->cmd_cv);
    }

    pthread_mutex_unlock(&self->cmd_lock);

    Py_BEGIN_ALLOW_THREADS
    pthread_join(self->worker, NULL);
    Py_END_ALLOW_THREADS

    self->worker_running = 0;
    return 0;
}

static PyObject *
PyRtpServer_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyRtpServer *self;

    (void)args;
    (void)kwds;

    self = (PyRtpServer *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    self->worker_running = 0;
    self->mutex_inited = 0;
    self->cond_inited = 0;
    self->cmd_waiter_inited = 0;
    self->cmd_waiter_busy = 0;
    self->shutdown_queued = 0;
    self->accepting_commands = 1;
    self->next_channel_id = 1;
    self->tick_ns = 1000000000ULL / DEFAULT_TICK_HZ;
    self->cmd_cv_clock = CLOCK_REALTIME;
    self->cmd_head = NULL;
    self->cmd_tail = NULL;
    self->channels = NULL;
    self->channels_cap = 0;
    self->channels_active = 0;
    self->pollfds = NULL;
    self->pollfds_index = NULL;
    self->pollfds_len = 0;
    self->pollfds_cap = 0;
    self->pollfds_dirty = 1;
    return (PyObject *)self;
}

static int
PyRtpServer_init(PyRtpServer *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"tick_hz", NULL};
    unsigned int tick_hz = DEFAULT_TICK_HZ;

    if (self->worker_running || self->mutex_inited || self->cond_inited ||
            self->cmd_waiter_inited)
        return 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|I:RtpServer", kwlist, &tick_hz))
        return -1;

    if (tick_hz == 0) {
        PyErr_SetString(PyExc_ValueError, "tick_hz must be > 0");
        return -1;
    }

    self->tick_ns = 1000000000ULL / (uint64_t)tick_hz;
    if (self->tick_ns == 0)
        self->tick_ns = 1;

    if (pthread_mutex_init(&self->cmd_lock, NULL) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "pthread_mutex_init failed");
        goto fail;
    }
    self->mutex_inited = 1;

    {
        rtp_sync_cond_ctx cond_ctx = server_cmdcv_ctx(self);
        if (rtp_sync_cond_init_monotonic(&cond_ctx) != 0) {
            PyErr_SetString(PyExc_RuntimeError, "pthread_cond_init failed");
            goto fail_cmd_lock;
        }
    }
    self->cond_inited = 1;

    if (rtp_sync_waiter_init(&self->cmd_waiter) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize command waiter");
        goto fail_cmd_cv;
    }
    self->cmd_waiter_inited = 1;
    self->cmd_waiter_busy = 0;

    if (pthread_create(&self->worker, NULL, rtp_server_worker, self) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create worker thread");
        goto fail_cmd_waiter;
    }

    self->worker_running = 1;
    self->accepting_commands = 1;
    self->shutdown_queued = 0;
    return 0;

fail_cmd_waiter:
    rtp_sync_waiter_destroy(&self->cmd_waiter);
    self->cmd_waiter_inited = 0;
fail_cmd_cv:
    pthread_cond_destroy(&self->cmd_cv);
    self->cond_inited = 0;
fail_cmd_lock:
    pthread_mutex_destroy(&self->cmd_lock);
    self->mutex_inited = 0;
fail:
    return -1;
}

static void
PyRtpServer_dealloc(PyRtpServer *self)
{
    if (self->worker_running)
        (void)rtp_server_shutdown_internal(self);
    if (self->mutex_inited)
        free_command_list(detach_commands(self));
    clear_poll_cache(self);
    if (self->cmd_waiter_inited) {
        rtp_sync_waiter_destroy(&self->cmd_waiter);
        self->cmd_waiter_inited = 0;
    }
    if (self->cond_inited) {
        pthread_cond_destroy(&self->cmd_cv);
        self->cond_inited = 0;
    }
    if (self->mutex_inited) {
        pthread_mutex_destroy(&self->cmd_lock);
        self->mutex_inited = 0;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
build_udp_socket(const struct sockaddr_storage *bind_addr, socklen_t bind_len,
    int family, int *out_fd, struct sockaddr_storage *out_local,
    socklen_t *out_local_len)
{
    int fd;

    if (out_fd == NULL || out_local == NULL || out_local_len == NULL)
        return -1;

    fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)bind_addr, bind_len) != 0) {
        close(fd);
        return -1;
    }

    *out_local_len = (socklen_t)sizeof(*out_local);
    if (getsockname(fd, (struct sockaddr *)out_local, out_local_len) != 0) {
        close(fd);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

static PyObject *
PyRtpServer_create_channel(PyRtpServer *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"pkt_in", "bind_host", "bind_port", "queue_size",
        "bind_family", NULL};
    PyObject *pkt_in = NULL;
    const char *bind_host = NULL;
    const char *effective_bind_host = NULL;
    int bind_port = 0;
    unsigned long long queue_size_ull = CHANNEL_OUTQ_CAPACITY;
    size_t queue_size = CHANNEL_OUTQ_CAPACITY;
    PyObject *bind_family_obj = Py_None;
    int family_hint = AF_UNSPEC;
    struct sockaddr_storage bind_addr;
    socklen_t bind_len = 0;
    struct sockaddr_storage local_addr;
    socklen_t local_len = 0;
    int family = AF_UNSPEC;
    int fd = -1;
    RtpServerCmd *cmd = NULL;
    RtpCmdWaiter *waiter = NULL;
    int cmd_status = 0;
    PyRtpChannel *channel = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ziKO:create_channel", kwlist,
        &pkt_in, &bind_host, &bind_port, &queue_size_ull, &bind_family_obj))
        return NULL;

    if (!PyCallable_Check(pkt_in)) {
        PyErr_SetString(PyExc_TypeError, "pkt_in must be callable");
        return NULL;
    }
    if (queue_size_ull == 0) {
        PyErr_SetString(PyExc_ValueError, "queue_size must be > 0");
        return NULL;
    }
    if ((queue_size_ull & (queue_size_ull - 1)) != 0) {
        PyErr_SetString(PyExc_ValueError, "queue_size must be a power of two");
        return NULL;
    }
    if (queue_size_ull > SIZE_MAX) {
        PyErr_SetString(PyExc_OverflowError, "queue_size is too large");
        return NULL;
    }
    queue_size = (size_t)queue_size_ull;

    if (parse_bind_family(bind_family_obj, &family_hint) != 0)
        return NULL;
    effective_bind_host = bind_host;
    if (effective_bind_host == NULL) {
        effective_bind_host = (family_hint == AF_INET6) ? "::" : "0.0.0.0";
    }

    if (resolve_udp_addr(effective_bind_host, bind_port, 1, family_hint,
            &bind_addr, &bind_len, &family, 1) != 0) {
        return NULL;
    }

    if (build_udp_socket(&bind_addr, bind_len, family, &fd, &local_addr,
            &local_len) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto fail_fd;
    }

    channel = PyObject_New(PyRtpChannel, &PyRtpChannelType);
    if (channel == NULL)
        goto fail_fd;

    channel->server_obj = (PyObject *)self;
    Py_INCREF(self);
    channel->id = self->next_channel_id++;
    channel->closed = 0;
    channel->has_target = 0;
    channel->out_q = NULL;
    channel->out_q = create_queue(queue_size);
    if (channel->out_q == NULL) {
        PyErr_NoMemory();
        goto fail_channel_fd;
    }
    channel->local_addr = local_addr;
    channel->local_len = local_len;

    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) {
        PyErr_NoMemory();
        goto fail_channel_cmd;
    }

    cmd->type = CMD_ADD_CHANNEL;
    cmd->u.add_channel.id = channel->id;
    cmd->u.add_channel.fd = fd;
    fd = -1;
    cmd->u.add_channel.queue_size = queue_size;
    cmd->u.add_channel.pkt_in_cb = pkt_in;
    cmd->u.add_channel.out_q = channel->out_q;
    Py_INCREF(pkt_in);
    cmd->waiter = NULL;

    if (server_waiter_acquire(self, &waiter) != 0) {
        channel->out_q = NULL;
        goto fail_cmd_channel_fd;
    }
    cmd->waiter = waiter;

    if (enqueue_command(self, cmd, 1) != 0) {
        channel->out_q = NULL;
        cmd = NULL;
        goto fail_waiter_channel_fd;
    }
    cmd = NULL;

    Py_BEGIN_ALLOW_THREADS
    cmd_status = rtp_sync_waiter_wait(waiter);
    Py_END_ALLOW_THREADS

    if (cmd_status != 0) {
        if (cmd_status == ENOMEM) {
            PyErr_NoMemory();
        } else {
            PyErr_Format(PyExc_RuntimeError,
                "failed to add channel to worker (status=%d: %s)",
                cmd_status, strerror(cmd_status));
        }
        channel->out_q = NULL;
        goto fail_cmd;
    }
    server_waiter_release(self);

    return (PyObject *)channel;

fail_cmd:
fail_waiter_channel_fd:
    server_waiter_release(self);
fail_cmd_channel_fd:
    if (cmd != NULL)
        free_command(cmd);
fail_channel_cmd:
    if (channel != NULL)
        destroy_send_queue(&channel->out_q);
fail_channel_fd:
    if (channel != NULL) {
        channel->closed = 1;
        Py_CLEAR(channel->server_obj);
        Py_DECREF(channel);
    }
fail_fd:
    close_fd(&fd);
    return NULL;
}

static PyObject *
PyRtpServer_shutdown(PyRtpServer *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":shutdown"))
        return NULL;

    if (rtp_server_shutdown_internal(self) != 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyMethodDef PyRtpServer_methods[] = {
    {"create_channel", (PyCFunction)PyRtpServer_create_channel,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"shutdown", (PyCFunction)PyRtpServer_shutdown, METH_VARARGS, NULL},
    {NULL}
};

static PyMemberDef PyRtpServer_members[] = {
    {"tick_ns", T_ULONGLONG, offsetof(PyRtpServer, tick_ns), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyRtpServerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpServer",
    .tp_basicsize = sizeof(PyRtpServer),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRtpServer_new,
    .tp_init = (initproc)PyRtpServer_init,
    .tp_dealloc = (destructor)PyRtpServer_dealloc,
    .tp_methods = PyRtpServer_methods,
    .tp_members = PyRtpServer_members,
};

static PyObject *
PyRtpChannel_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    (void)type;
    (void)args;
    (void)kwds;
    PyErr_SetString(PyExc_TypeError,
        "RtpChannel objects are created by RtpServer.create_channel()");
    return NULL;
}

static int
rtp_channel_close_internal(PyRtpChannel *self, int with_error)
{
    RtpServerCmd *cmd;
    PyRtpServer *server;

    assert(self != NULL);
    if (self->closed) {
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "channel is already closed");
        return with_error ? -1 : 0;
    }

    if (self->server_obj == NULL) {
        self->closed = 1;
        self->out_q = NULL;
        return 0;
    }

    server = (PyRtpServer *)self->server_obj;
    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) {
        if (with_error)
            PyErr_NoMemory();
        return -1;
    }

    cmd->type = CMD_REMOVE_CHANNEL;
    cmd->u.remove_channel.id = self->id;

    if (enqueue_command(server, cmd, with_error) != 0) {
        if (with_error && !PyErr_ExceptionMatches(PyExc_RuntimeError))
            return -1;
        PyErr_Clear();
    }

    self->closed = 1;
    self->out_q = NULL;
    return 0;
}

static void
PyRtpChannel_dealloc(PyRtpChannel *self)
{
    (void)rtp_channel_close_internal(self, 0);
    Py_XDECREF(self->server_obj);
    self->server_obj = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
PyRtpChannel_close(PyRtpChannel *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":close"))
        return NULL;

    if (rtp_channel_close_internal(self, 1) != 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
PyRtpChannel_set_target(PyRtpChannel *self, PyObject *args)
{
    const char *host = NULL;
    int port = 0;
    struct sockaddr_storage target;
    socklen_t target_len = 0;
    int family_hint = AF_UNSPEC;
    RtpServerCmd *cmd;
    RtpCmdWaiter *waiter = NULL;
    int cmd_status;
    PyRtpServer *server;

    if (!PyArg_ParseTuple(args, "si:set_target", &host, &port))
        return NULL;

    if (self->closed) {
        PyErr_SetString(PyExc_RuntimeError, "channel is closed");
        return NULL;
    }
    if (self->server_obj == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "channel has no server");
        return NULL;
    }
    server = (PyRtpServer *)self->server_obj;

    if (self->local_len > 0)
        family_hint = ((struct sockaddr *)&self->local_addr)->sa_family;

    if (resolve_udp_addr(host, port, 0, family_hint, &target,
            &target_len, NULL, 1) != 0) {
        return NULL;
    }

    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL)
        return PyErr_NoMemory();

    cmd->type = CMD_SET_TARGET;
    cmd->u.set_target.id = self->id;
    cmd->u.set_target.addr = target;
    cmd->u.set_target.addrlen = target_len;
    cmd->waiter = NULL;

    if (server_waiter_acquire(server, &waiter) != 0) {
        free_command(cmd);
        return NULL;
    }
    cmd->waiter = waiter;

    if (enqueue_command(server, cmd, 1) != 0) {
        server_waiter_release(server);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    cmd_status = rtp_sync_waiter_wait(waiter);
    Py_END_ALLOW_THREADS
    server_waiter_release(server);

    if (cmd_status != 0) {
        if (cmd_status == ENOENT) {
            PyErr_SetString(PyExc_RuntimeError, "channel is no longer present");
        } else {
            PyErr_Format(PyExc_RuntimeError,
                "failed to set target on worker (status=%d: %s)",
                cmd_status, strerror(cmd_status));
        }
        return NULL;
    }

    self->has_target = 1;
    Py_RETURN_NONE;
}

static int
bytes_from_obj(PyObject *obj, const unsigned char **data, Py_ssize_t *size,
    PyObject **owner)
{
    if (PyBytes_Check(obj)) {
        Py_INCREF(obj);
        *owner = obj;
    } else {
        *owner = PyObject_Bytes(obj);
        if (*owner == NULL)
            return -1;
    }
    if (PyBytes_AsStringAndSize(*owner, (char **)data, size) != 0) {
        Py_DECREF(*owner);
        *owner = NULL;
        return -1;
    }
    return 0;
}

static PyObject *
PyRtpChannel_send_pkt(PyRtpChannel *self, PyObject *args)
{
    PyObject *data_obj = NULL;
    PyObject *bytes_owner = NULL;
    const unsigned char *data = NULL;
    Py_ssize_t size = 0;
    RtpSendItem *item = NULL;
    int queued = 0;
    PyRtpServer *server;

    if (!PyArg_ParseTuple(args, "O:send_pkt", &data_obj))
        return NULL;

    if (self->closed) {
        PyErr_SetString(PyExc_RuntimeError, "channel is closed");
        return NULL;
    }
    if (!self->has_target) {
        PyErr_SetString(PyExc_RuntimeError, "channel target is not set");
        return NULL;
    }
    if (self->server_obj == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "channel has no server");
        return NULL;
    }
    if (self->out_q == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "channel output queue is unavailable");
        return NULL;
    }

    if (bytes_from_obj(data_obj, &data, &size, &bytes_owner) != 0)
        return NULL;

    item = calloc(1, sizeof(*item));
    if (item == NULL) {
        Py_DECREF(bytes_owner);
        return PyErr_NoMemory();
    }

    item->data = data;
    item->size = (size_t)size;
    item->data_ref = bytes_owner;
    bytes_owner = NULL;

    server = (PyRtpServer *)self->server_obj;
    if (!server->worker_running || !server->accepting_commands) {
        free_send_item(item);
        PyErr_SetString(PyExc_RuntimeError, "RtpServer is shutting down");
        return NULL;
    }

    queued = try_push(self->out_q, item) ? 1 : 0;
    if (queued)
        pthread_cond_signal(&server->cmd_cv);

    if (!queued) {
        free_send_item(item);
        PyErr_SetString(RtpQueueFullError, "channel output queue is full");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *
PyRtpChannel_get_local_addr(PyRtpChannel *self, void *closure)
{
    PyObject *addr = NULL;

    (void)closure;

    if (self->local_len == 0) {
        Py_RETURN_NONE;
    }

    if (sockaddr_to_tuple((const struct sockaddr *)&self->local_addr,
            self->local_len, &addr) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    return addr;
}

static PyObject *
PyRtpChannel_get_closed(PyRtpChannel *self, void *closure)
{
    (void)closure;
    return PyBool_FromLong(self->closed ? 1 : 0);
}

static PyMethodDef PyRtpChannel_methods[] = {
    {"set_target", (PyCFunction)PyRtpChannel_set_target, METH_VARARGS, NULL},
    {"send_pkt", (PyCFunction)PyRtpChannel_send_pkt, METH_VARARGS, NULL},
    {"close", (PyCFunction)PyRtpChannel_close, METH_VARARGS, NULL},
    {NULL}
};

static PyGetSetDef PyRtpChannel_getset[] = {
    {"local_addr", (getter)PyRtpChannel_get_local_addr, NULL, NULL, NULL},
    {"closed", (getter)PyRtpChannel_get_closed, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PyRtpChannelType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpChannel",
    .tp_basicsize = sizeof(PyRtpChannel),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRtpChannel_new,
    .tp_dealloc = (destructor)PyRtpChannel_dealloc,
    .tp_methods = PyRtpChannel_methods,
    .tp_getset = PyRtpChannel_getset,
};

static struct PyModuleDef RtpServer_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = "RTP I/O thread and channel primitives.",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit_RtpServer(void)
{
    PyObject *module;
    PyObject *queue_full_error;

    if (PyType_Ready(&PyRtpServerType) < 0)
        return NULL;
    if (PyType_Ready(&PyRtpChannelType) < 0)
        return NULL;

    module = PyModule_Create(&RtpServer_module);
    if (module == NULL)
        return NULL;

    queue_full_error = PyErr_NewException(MODULE_NAME ".RtpQueueFullError", NULL, NULL);
    if (queue_full_error == NULL) {
        Py_DECREF(module);
        return NULL;
    }
    RtpQueueFullError = queue_full_error;
    PyModule_AddObject(module, "RtpQueueFullError", queue_full_error);

    Py_INCREF(&PyRtpServerType);
    Py_INCREF(&PyRtpChannelType);
    PyModule_AddObject(module, "RtpServer", (PyObject *)&PyRtpServerType);
    PyModule_AddObject(module, "RtpChannel", (PyObject *)&PyRtpChannelType);

    return module;
}
