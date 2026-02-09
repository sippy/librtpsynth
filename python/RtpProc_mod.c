#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <Python.h>
#include <structmember.h>

#include "rtp_sync.h"

#define MODULE_NAME "rtpsynth.RtpProc"

typedef struct proc_channel_state ProcChannelState;

typedef enum {
    CMD_ADD_CHANNEL = 1,
    CMD_REMOVE_CHANNEL,
    CMD_SHUTDOWN,
} ProcCommandType;

typedef rtp_sync_waiter ProcCmdWaiter;

typedef struct {
    PyObject *type;
    PyObject *value;
    PyObject *tb;
} py_exc_info;

typedef struct proc_cmd {
    ProcCommandType type;
    struct proc_cmd *next;
    ProcCmdWaiter *waiter;
    union {
        struct {
            uint64_t id;
            PyObject *proc_in_cb;
        } add_channel;
        struct {
            uint64_t id;
        } remove_channel;
    } u;
} ProcCmd;

struct proc_channel_state {
    int active;
    int scheduled;
    uint64_t id;
    uint64_t next_run_ns;
    PyObject *proc_in_cb;
    py_exc_info cb_exc;
    ProcChannelState *sched_next;
};

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
    clockid_t cmd_cv_clock;
    pthread_mutex_t cmd_lock;
    pthread_cond_t cmd_cv;
    ProcCmdWaiter cmd_waiter;
    ProcCmd *cmd_head;
    ProcCmd *cmd_tail;
    py_exc_info close_exc;
    ProcChannelState *channels;
    size_t channels_cap;
    size_t channels_active;
    ProcChannelState *sched_head;
} PyRtpProc;

typedef struct {
    PyObject_HEAD
    PyObject *proc_obj;
    uint64_t id;
    int closed;
} PyRtpProcChannel;

static PyTypeObject PyRtpProcType;
static PyTypeObject PyRtpProcChannelType;
static PyRtpProc *g_rtp_proc_singleton = NULL;
static PyObject *ChannelProcError = NULL;

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

static uint64_t
monotonic_deadline_to_cmd_clock(PyRtpProc *self, uint64_t monotonic_deadline,
    uint64_t monotonic_now)
{
    uint64_t wait_ns;
    uint64_t cmd_now;

    assert(self != NULL);
#if defined(CLOCK_MONOTONIC)
    if (self->cmd_cv_clock == CLOCK_MONOTONIC) {
        if (monotonic_deadline <= monotonic_now)
            return monotonic_now;
        return monotonic_deadline;
    }
#endif
    cmd_now = now_ns_for_clock(self->cmd_cv_clock);
    if (monotonic_deadline <= monotonic_now)
        return cmd_now;
    wait_ns = monotonic_deadline - monotonic_now;
    if (UINT64_MAX - cmd_now < wait_ns)
        return UINT64_MAX;
    return cmd_now + wait_ns;
}

static rtp_sync_cmdq
proc_cmdq_ctx(PyRtpProc *self)
{
    rtp_sync_cmdq ctx = {
        .head = (void **)&self->cmd_head,
        .tail = (void **)&self->cmd_tail,
        .next_off = offsetof(ProcCmd, next),
    };
    return ctx;
}

static rtp_sync_cond_ctx
proc_cmdcv_ctx(PyRtpProc *self)
{
    rtp_sync_cond_ctx ctx = {
        .cv = &self->cmd_cv,
        .lock = &self->cmd_lock,
        .clock_id = &self->cmd_cv_clock,
    };
    return ctx;
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
py_exc_info_init(py_exc_info *exc)
{
    assert(exc != NULL);
    exc->type = NULL;
    exc->value = NULL;
    exc->tb = NULL;
}

static void
py_exc_info_clear(py_exc_info *exc)
{
    assert(exc != NULL);
    Py_XDECREF(exc->type);
    Py_XDECREF(exc->value);
    Py_XDECREF(exc->tb);
    py_exc_info_init(exc);
}

static int
py_exc_info_has(const py_exc_info *exc)
{
    assert(exc != NULL);
    return (exc->type != NULL || exc->value != NULL || exc->tb != NULL);
}

static void
py_exc_info_fetch_normalized(py_exc_info *exc)
{
    assert(exc != NULL);
    py_exc_info_init(exc);
    PyErr_Fetch(&exc->type, &exc->value, &exc->tb);
    PyErr_NormalizeException(&exc->type, &exc->value, &exc->tb);
}

static void
py_exc_info_clear_on_worker(py_exc_info *exc)
{
    PyGILState_STATE gstate;

    assert(exc != NULL);
    if (!py_exc_info_has(exc))
        return;
    gstate = PyGILState_Ensure();
    py_exc_info_clear(exc);
    PyGILState_Release(gstate);
}

static void
channel_clear_callback_exception(ProcChannelState *ch)
{
    assert(ch != NULL);
    if (py_exc_info_has(&ch->cb_exc)) {
        PyGILState_STATE gstate = PyGILState_Ensure();
        py_exc_info_clear(&ch->cb_exc);
        PyGILState_Release(gstate);
    }
}

static void
channel_take_callback_exception(ProcChannelState *ch, py_exc_info *exc_out)
{
    assert(ch != NULL);
    assert(exc_out != NULL);
    *exc_out = ch->cb_exc;
    py_exc_info_init(&ch->cb_exc);
}

static void
channel_set_callback_exception(ProcChannelState *ch, py_exc_info *exc)
{
    assert(ch != NULL);
    assert(exc != NULL);
    assert(py_exc_info_has(exc));
    channel_clear_callback_exception(ch);
    ch->cb_exc = *exc;
    py_exc_info_init(exc);
}

static void
proc_clear_close_exception(PyRtpProc *self)
{
    assert(self != NULL);
    py_exc_info_clear(&self->close_exc);
}

static void
proc_set_close_exception_steal(PyRtpProc *self, py_exc_info *exc)
{
    PyGILState_STATE gstate;

    assert(self != NULL);
    assert(exc != NULL);

    gstate = PyGILState_Ensure();
    proc_clear_close_exception(self);
    self->close_exc = *exc;
    py_exc_info_init(exc);
    PyGILState_Release(gstate);
}

static int
proc_pop_close_exception(PyRtpProc *self, py_exc_info *exc_out)
{
    assert(self != NULL);
    assert(exc_out != NULL);
    *exc_out = self->close_exc;
    py_exc_info_init(&self->close_exc);
    return py_exc_info_has(exc_out);
}

static int
raise_channel_proc_error_from(py_exc_info *cause_exc)
{
    PyObject *err = NULL;
    PyObject *cause = NULL;

    assert(cause_exc != NULL);
    assert(ChannelProcError != NULL);

    err = PyObject_CallFunction(ChannelProcError, "s",
        "channel processing callback failed");
    if (err == NULL) {
        py_exc_info_clear(cause_exc);
        return -1;
    }

    if (cause_exc->value != NULL) {
        cause = cause_exc->value;
        cause_exc->value = NULL;
        if (cause_exc->tb != NULL)
            (void)PyException_SetTraceback(cause, cause_exc->tb);
        PyException_SetCause(err, cause);
    }

    PyErr_SetObject(ChannelProcError, err);
    Py_DECREF(err);
    py_exc_info_clear(cause_exc);
    return -1;
}

static void
proc_waiter_release(PyRtpProc *self)
{
    if (self == NULL || !self->mutex_inited)
        return;
    if (pthread_mutex_lock(&self->cmd_lock) == 0) {
        self->cmd_waiter_busy = 0;
        pthread_mutex_unlock(&self->cmd_lock);
    }
}

static int
proc_waiter_acquire(PyRtpProc *self, ProcCmdWaiter **out_waiter)
{
    assert(self != NULL);
    assert(out_waiter != NULL);

    if (!self->cmd_waiter_inited) {
        PyErr_SetString(PyExc_RuntimeError, "RtpProc waiter is not initialized");
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
        proc_waiter_release(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to reset command waiter");
        return -1;
    }
    *out_waiter = &self->cmd_waiter;
    return 0;
}

static void
unschedule_channel(PyRtpProc *self, ProcChannelState *ch)
{
    ProcChannelState *prev = NULL;
    ProcChannelState *it;

    assert(self != NULL);
    assert(ch != NULL);
    if (!ch->scheduled)
        return;

    it = self->sched_head;
    while (it != NULL && it != ch) {
        prev = it;
        it = it->sched_next;
    }
    if (it == NULL) {
        ch->scheduled = 0;
        ch->sched_next = NULL;
        return;
    }
    if (prev == NULL)
        self->sched_head = ch->sched_next;
    else
        prev->sched_next = ch->sched_next;
    ch->scheduled = 0;
    ch->sched_next = NULL;
}

static void
schedule_channel(PyRtpProc *self, ProcChannelState *ch, uint64_t next_ns)
{
    ProcChannelState **it;

    assert(self != NULL);
    assert(ch != NULL);

    if (ch->scheduled)
        unschedule_channel(self, ch);

    ch->next_run_ns = next_ns;
    ch->scheduled = 1;
    ch->sched_next = NULL;

    it = &self->sched_head;
    while (*it != NULL && (*it)->next_run_ns <= next_ns)
        it = &(*it)->sched_next;
    ch->sched_next = *it;
    *it = ch;
}

static void
rebuild_schedule_list(PyRtpProc *self)
{
    size_t i;
    ProcChannelState *ch;
    ProcChannelState **it;

    assert(self != NULL);
    self->sched_head = NULL;
    for (i = 0; i < self->channels_cap; i++) {
        ch = &self->channels[i];
        if (!ch->active || !ch->scheduled) {
            ch->sched_next = NULL;
            continue;
        }
        ch->sched_next = NULL;
        it = &self->sched_head;
        while (*it != NULL && (*it)->next_run_ns <= ch->next_run_ns)
            it = &(*it)->sched_next;
        ch->sched_next = *it;
        *it = ch;
    }
}

static ProcChannelState *
pop_due_channel(PyRtpProc *self, uint64_t now_ns)
{
    ProcChannelState *ch;
    assert(self != NULL);
    ch = self->sched_head;
    if (ch == NULL || ch->next_run_ns > now_ns)
        return NULL;
    self->sched_head = ch->sched_next;
    ch->sched_next = NULL;
    ch->scheduled = 0;
    return ch;
}

static void
release_channel_state(PyRtpProc *self, ProcChannelState *ch)
{
    assert(self != NULL);
    assert(ch != NULL);
    if (!ch->active)
        return;
    unschedule_channel(self, ch);
    if (ch->proc_in_cb != NULL)
        py_xdecref_on_worker(ch->proc_in_cb);
    channel_clear_callback_exception(ch);
    ch->proc_in_cb = NULL;
    ch->id = 0;
    ch->active = 0;
    ch->next_run_ns = 0;
    ch->sched_next = NULL;
}

static void
clear_channels(PyRtpProc *self)
{
    size_t i;
    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++)
        release_channel_state(self, &self->channels[i]);
    free(self->channels);
    self->channels = NULL;
    self->channels_cap = 0;
    self->channels_active = 0;
    self->sched_head = NULL;
}

static int
ensure_channel_capacity(PyRtpProc *self, size_t need)
{
    size_t new_cap;
    ProcChannelState *old_channels;
    ProcChannelState *new_channels;
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
    old_channels = self->channels;
    new_channels = realloc(old_channels, new_cap * sizeof(*self->channels));
    if (new_channels == NULL)
        return -1;
    memset(new_channels + old_cap, 0, (new_cap - old_cap) * sizeof(*new_channels));
    self->channels = new_channels;
    self->channels_cap = new_cap;
    if (old_channels != new_channels && self->sched_head != NULL)
        rebuild_schedule_list(self);
    return 0;
}

static ssize_t
alloc_channel_slot(PyRtpProc *self)
{
    size_t i;
    size_t old_cap;

    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++) {
        if (!self->channels[i].active)
            return (ssize_t)i;
    }

    old_cap = self->channels_cap;
    if (ensure_channel_capacity(self, old_cap == 0 ? 4 : (old_cap * 2)) != 0)
        return -1;
    return (ssize_t)old_cap;
}

static ssize_t
find_channel_index(PyRtpProc *self, uint64_t id)
{
    size_t i;
    assert(self != NULL);
    for (i = 0; i < self->channels_cap; i++) {
        if (self->channels[i].active && self->channels[i].id == id)
            return (ssize_t)i;
    }
    return -1;
}

static int
remove_channel(PyRtpProc *self, uint64_t id, py_exc_info *exc_out)
{
    ssize_t idx;
    ProcChannelState *ch;

    assert(self != NULL);
    if (exc_out != NULL)
        py_exc_info_init(exc_out);

    idx = find_channel_index(self, id);
    if (idx < 0)
        return 0;

    ch = &self->channels[idx];
    if (exc_out != NULL)
        channel_take_callback_exception(ch, exc_out);
    release_channel_state(self, ch);
    if (self->channels_active > 0)
        self->channels_active -= 1;
    return 1;
}

static int
invoke_proc_callback(PyObject *cb, uint64_t now_ns, uint64_t deadline_ns,
    uint64_t *next_ns, int *has_next, py_exc_info *exc_out)
{
    PyGILState_STATE gstate;
    PyObject *arg_now = NULL;
    PyObject *arg_deadline = NULL;
    PyObject *res = NULL;
    int ok = 0;

    assert(cb != NULL);
    assert(next_ns != NULL);
    assert(has_next != NULL);
    assert(exc_out != NULL);
    py_exc_info_init(exc_out);

    gstate = PyGILState_Ensure();
    arg_now = PyLong_FromUnsignedLongLong((unsigned long long)now_ns);
    if (arg_now == NULL)
        goto out;
    arg_deadline = PyLong_FromUnsignedLongLong((unsigned long long)deadline_ns);
    if (arg_deadline == NULL)
        goto out;

    res = PyObject_CallFunctionObjArgs(cb, arg_now, arg_deadline, NULL);
    if (res == NULL) {
        py_exc_info_fetch_normalized(exc_out);
        goto out;
    }

    if (res == Py_None) {
        *has_next = 0;
        ok = 1;
        goto out;
    }

    *next_ns = (uint64_t)PyLong_AsUnsignedLongLong(res);
    if (PyErr_Occurred()) {
        py_exc_info_fetch_normalized(exc_out);
        goto out;
    }
    *has_next = 1;
    ok = 1;

out:
    Py_XDECREF(res);
    Py_XDECREF(arg_now);
    Py_XDECREF(arg_deadline);
    PyGILState_Release(gstate);
    return ok ? 0 : -1;
}

static void
free_command(ProcCmd *cmd)
{
    assert(cmd != NULL);

    if (cmd->type == CMD_ADD_CHANNEL) {
        if (cmd->u.add_channel.proc_in_cb != NULL) {
            py_xdecref_on_worker(cmd->u.add_channel.proc_in_cb);
            cmd->u.add_channel.proc_in_cb = NULL;
        }
    }
    free(cmd);
}

static void
free_command_list(ProcCmd *head)
{
    while (head != NULL) {
        ProcCmd *next = head->next;
        free_command(head);
        head = next;
    }
}

static int
enqueue_command(PyRtpProc *self, ProcCmd *cmd, int with_error)
{
    int rejected = 0;

    assert(self != NULL);
    assert(cmd != NULL);
    if (!self->mutex_inited || !self->cond_inited) {
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "RtpProc is not initialized");
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
        rtp_sync_cmdq cmdq = proc_cmdq_ctx(self);
        rtp_sync_cmdq_push(&cmdq, cmd);
        pthread_cond_signal(&self->cmd_cv);
    }

    pthread_mutex_unlock(&self->cmd_lock);

    if (rejected) {
        free_command(cmd);
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "RtpProc is shutting down");
        return -1;
    }
    return 0;
}

static ProcCmd *
detach_commands(PyRtpProc *self)
{
    ProcCmd *head = NULL;

    assert(self != NULL);
    if (!self->mutex_inited)
        return NULL;
    if (pthread_mutex_lock(&self->cmd_lock) != 0)
        return NULL;

    {
        rtp_sync_cmdq cmdq = proc_cmdq_ctx(self);
        head = (ProcCmd *)rtp_sync_cmdq_detach_all(&cmdq);
    }

    pthread_mutex_unlock(&self->cmd_lock);
    return head;
}

static int
wait_for_commands(PyRtpProc *self, uint64_t wait_until_ns, int wait_forever)
{
    int rc = 0;

    if (pthread_mutex_lock(&self->cmd_lock) != 0)
        return -1;

    if (wait_forever) {
        while (self->cmd_head == NULL && rc == 0)
            rc = pthread_cond_wait(&self->cmd_cv, &self->cmd_lock);
    } else {
        while (self->cmd_head == NULL) {
            rtp_sync_cond_ctx cond_ctx = proc_cmdcv_ctx(self);
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

static void
process_commands(PyRtpProc *self, int *shutdown_seen)
{
    ProcCmd *cmd;

    cmd = detach_commands(self);
    while (cmd != NULL) {
        ProcCmd *next = cmd->next;
        int cmd_status = 0;

        if (cmd->type == CMD_ADD_CHANNEL) {
            ssize_t slot = alloc_channel_slot(self);
            if (slot >= 0) {
                ProcChannelState *ch = &self->channels[slot];
                uint64_t next_ns = 0;
                uint64_t now_ns;
                int has_next = 0;
                py_exc_info exc;

                assert(!ch->active);
                ch->active = 1;
                ch->scheduled = 0;
                ch->id = cmd->u.add_channel.id;
                ch->next_run_ns = 0;
                ch->proc_in_cb = cmd->u.add_channel.proc_in_cb;
                ch->sched_next = NULL;
                cmd->u.add_channel.proc_in_cb = NULL;
                self->channels_active += 1;

                now_ns = now_ns_monotonic();
                if (invoke_proc_callback(ch->proc_in_cb, now_ns, 0,
                        &next_ns, &has_next, &exc) != 0) {
                    if (py_exc_info_has(&exc))
                        channel_set_callback_exception(ch, &exc);
                    else
                        assert(!py_exc_info_has(&exc));
                } else if (has_next) {
                    if (next_ns < now_ns)
                        next_ns = now_ns;
                    schedule_channel(self, ch, next_ns);
                }
            } else {
                cmd_status = ENOMEM;
            }
        } else if (cmd->type == CMD_REMOVE_CHANNEL) {
            {
                py_exc_info exc;

                (void)remove_channel(self, cmd->u.remove_channel.id, &exc);
                if (cmd->waiter != NULL && py_exc_info_has(&exc)) {
                    proc_set_close_exception_steal(self, &exc);
                }
                py_exc_info_clear_on_worker(&exc);
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
rtp_proc_worker(void *arg)
{
    PyRtpProc *self = (PyRtpProc *)arg;

    for (;;) {
        int shutdown_seen = 0;
        uint64_t now_ns;
        ProcChannelState *ch;

        process_commands(self, &shutdown_seen);
        if (shutdown_seen)
            break;

        now_ns = now_ns_monotonic();
        for (;;) {
            uint64_t deadline_ns;
            uint64_t next_ns = 0;
            int has_next = 0;
            py_exc_info exc;

            ch = pop_due_channel(self, now_ns);
            if (ch == NULL)
                break;
            if (!ch->active || ch->proc_in_cb == NULL)
                continue;
            deadline_ns = ch->next_run_ns;
            if (invoke_proc_callback(ch->proc_in_cb, now_ns, deadline_ns,
                    &next_ns, &has_next, &exc) == 0) {
                if (has_next && ch->active) {
                    schedule_channel(self, ch, next_ns);
                }
            } else {
                if (py_exc_info_has(&exc))
                    channel_set_callback_exception(ch, &exc);
                else
                    assert(!py_exc_info_has(&exc));
            }
            now_ns = now_ns_monotonic();
        }

        if (self->sched_head == NULL) {
            (void)wait_for_commands(self, 0, 1);
            continue;
        }

        now_ns = now_ns_monotonic();
        if (self->sched_head->next_run_ns <= now_ns)
            continue;
        {
            uint64_t wait_until_ns = monotonic_deadline_to_cmd_clock(self,
                self->sched_head->next_run_ns, now_ns);
            (void)wait_for_commands(self, wait_until_ns, 0);
        }
    }

    clear_channels(self);
    free_command_list(detach_commands(self));
    return NULL;
}

static int
rtp_proc_shutdown_internal(PyRtpProc *self)
{
    ProcCmd *cmd;

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
        rtp_sync_cmdq cmdq = proc_cmdq_ctx(self);
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
PyRtpProc_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyRtpProc *self;

    (void)args;
    (void)kwds;

    if (g_rtp_proc_singleton != NULL) {
        Py_INCREF(g_rtp_proc_singleton);
        return (PyObject *)g_rtp_proc_singleton;
    }

    self = (PyRtpProc *)type->tp_alloc(type, 0);
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
    self->cmd_cv_clock = CLOCK_REALTIME;
    self->cmd_head = NULL;
    self->cmd_tail = NULL;
    py_exc_info_init(&self->close_exc);
    self->channels = NULL;
    self->channels_cap = 0;
    self->channels_active = 0;
    self->sched_head = NULL;
    g_rtp_proc_singleton = self;
    return (PyObject *)self;
}

static int
PyRtpProc_init(PyRtpProc *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {NULL};

    if (self->worker_running || self->mutex_inited || self->cond_inited ||
            self->cmd_waiter_inited) {
        if (PyTuple_Size(args) == 0 && (kwds == NULL || PyDict_Size(kwds) == 0))
            return 0;
    }

    if (!PyArg_ParseTupleAndKeywords(args, kwds, ":RtpProc", kwlist))
        return -1;

    if (self->worker_running || self->mutex_inited || self->cond_inited ||
            self->cmd_waiter_inited)
        return 0;

    if (pthread_mutex_init(&self->cmd_lock, NULL) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "pthread_mutex_init failed");
        goto fail;
    }
    self->mutex_inited = 1;
    {
        rtp_sync_cond_ctx cond_ctx = proc_cmdcv_ctx(self);
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

    if (pthread_create(&self->worker, NULL, rtp_proc_worker, self) != 0) {
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
PyRtpProc_dealloc(PyRtpProc *self)
{
    if (g_rtp_proc_singleton == self)
        g_rtp_proc_singleton = NULL;
    if (self->worker_running)
        (void)rtp_proc_shutdown_internal(self);
    if (self->mutex_inited)
        free_command_list(detach_commands(self));
    clear_channels(self);
    proc_clear_close_exception(self);
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

static PyObject *
PyRtpProc_create_channel(PyRtpProc *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"proc_in", NULL};
    PyObject *proc_in = NULL;
    PyRtpProcChannel *channel = NULL;
    ProcCmd *cmd = NULL;
    ProcCmdWaiter *waiter = NULL;
    int cmd_status = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:create_channel", kwlist, &proc_in))
        return NULL;
    if (!PyCallable_Check(proc_in)) {
        PyErr_SetString(PyExc_TypeError, "proc_in must be callable");
        return NULL;
    }

    channel = PyObject_New(PyRtpProcChannel, &PyRtpProcChannelType);
    if (channel == NULL)
        return NULL;
    channel->proc_obj = (PyObject *)self;
    Py_INCREF(self);
    channel->id = self->next_channel_id++;
    channel->closed = 0;

    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) {
        Py_DECREF(channel);
        return PyErr_NoMemory();
    }
    cmd->type = CMD_ADD_CHANNEL;
    cmd->u.add_channel.id = channel->id;
    cmd->u.add_channel.proc_in_cb = proc_in;
    Py_INCREF(proc_in);
    cmd->waiter = NULL;

    if (proc_waiter_acquire(self, &waiter) != 0) {
        free_command(cmd);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError,
                "failed to acquire command waiter");
        }
        channel->closed = 1;
        Py_CLEAR(channel->proc_obj);
        Py_DECREF(channel);
        return NULL;
    }
    cmd->waiter = waiter;

    if (enqueue_command(self, cmd, 1) != 0) {
        proc_waiter_release(self);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError,
                "failed to enqueue add-channel command");
        }
        channel->closed = 1;
        Py_CLEAR(channel->proc_obj);
        Py_DECREF(channel);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    cmd_status = rtp_sync_waiter_wait(waiter);
    Py_END_ALLOW_THREADS
    proc_waiter_release(self);

    if (cmd_status != 0) {
        if (cmd_status == ENOMEM) {
            PyErr_NoMemory();
        } else {
            PyErr_Format(PyExc_RuntimeError,
                "failed to add channel to worker (status=%d: %s)",
                cmd_status, strerror(cmd_status));
        }
        channel->closed = 1;
        Py_CLEAR(channel->proc_obj);
        Py_DECREF(channel);
        return NULL;
    }

    return (PyObject *)channel;
}

static PyObject *
PyRtpProc_shutdown(PyRtpProc *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":shutdown"))
        return NULL;
    if (rtp_proc_shutdown_internal(self) != 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef PyRtpProc_methods[] = {
    {"create_channel", (PyCFunction)PyRtpProc_create_channel,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"shutdown", (PyCFunction)PyRtpProc_shutdown, METH_VARARGS, NULL},
    {NULL}
};

static PyTypeObject PyRtpProcType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpProc",
    .tp_basicsize = sizeof(PyRtpProc),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRtpProc_new,
    .tp_init = (initproc)PyRtpProc_init,
    .tp_dealloc = (destructor)PyRtpProc_dealloc,
    .tp_methods = PyRtpProc_methods,
};

static int
rtp_proc_channel_close_internal(PyRtpProcChannel *self, int with_error)
{
    ProcCmd *cmd = NULL;
    ProcCmdWaiter *waiter = NULL;
    PyRtpProc *proc;
    int waiter_acquired = 0;
    int cmd_status = 0;
    py_exc_info exc;

    py_exc_info_init(&exc);

    assert(self != NULL);
    if (self->closed) {
        if (with_error)
            PyErr_SetString(PyExc_RuntimeError, "channel is already closed");
        return with_error ? -1 : 0;
    }

    if (self->proc_obj == NULL) {
        self->closed = 1;
        return 0;
    }

    proc = (PyRtpProc *)self->proc_obj;
    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) {
        if (with_error)
            PyErr_NoMemory();
        return -1;
    }
    cmd->type = CMD_REMOVE_CHANNEL;
    cmd->u.remove_channel.id = self->id;
    cmd->waiter = NULL;

    if (with_error) {
        proc_clear_close_exception(proc);
        if (proc_waiter_acquire(proc, &waiter) != 0)
            goto fail;
        waiter_acquired = 1;
        cmd->waiter = waiter;
    }

    if (enqueue_command(proc, cmd, with_error) != 0) {
        if (waiter_acquired)
            proc_waiter_release(proc);
        if (with_error && !PyErr_ExceptionMatches(PyExc_RuntimeError))
            return -1;
        PyErr_Clear();
    } else if (waiter_acquired) {
        Py_BEGIN_ALLOW_THREADS
        cmd_status = rtp_sync_waiter_wait(waiter);
        Py_END_ALLOW_THREADS
        proc_waiter_release(proc);

        if (cmd_status != 0) {
            PyErr_Format(PyExc_RuntimeError,
                "failed to remove channel from worker (status=%d: %s)",
                cmd_status, strerror(cmd_status));
            return -1;
        }
    }

    self->closed = 1;
    if (with_error && proc_pop_close_exception(proc, &exc))
        return raise_channel_proc_error_from(&exc);
    return 0;

fail:
    free_command(cmd);
    return -1;
}

static PyObject *
PyRtpProcChannel_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    (void)type;
    (void)args;
    (void)kwds;
    PyErr_SetString(PyExc_TypeError,
        "RtpProcChannel objects are created by RtpProc.create_channel()");
    return NULL;
}

static void
PyRtpProcChannel_dealloc(PyRtpProcChannel *self)
{
    (void)rtp_proc_channel_close_internal(self, 0);
    Py_XDECREF(self->proc_obj);
    self->proc_obj = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
PyRtpProcChannel_close(PyRtpProcChannel *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":close"))
        return NULL;
    if (rtp_proc_channel_close_internal(self, 1) != 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *
PyRtpProcChannel_get_closed(PyRtpProcChannel *self, void *closure)
{
    (void)closure;
    if (self->closed)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *
PyRtpProcChannel_get_id(PyRtpProcChannel *self, void *closure)
{
    (void)closure;
    return PyLong_FromUnsignedLongLong((unsigned long long)self->id);
}

static PyMethodDef PyRtpProcChannel_methods[] = {
    {"close", (PyCFunction)PyRtpProcChannel_close, METH_VARARGS, NULL},
    {NULL}
};

static PyGetSetDef PyRtpProcChannel_getset[] = {
    {"closed", (getter)PyRtpProcChannel_get_closed, NULL, NULL, NULL},
    {"id", (getter)PyRtpProcChannel_get_id, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PyRtpProcChannelType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpProcChannel",
    .tp_basicsize = sizeof(PyRtpProcChannel),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRtpProcChannel_new,
    .tp_dealloc = (destructor)PyRtpProcChannel_dealloc,
    .tp_methods = PyRtpProcChannel_methods,
    .tp_getset = PyRtpProcChannel_getset,
};

static struct PyModuleDef RtpProc_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = "Generic singleton RTP processing scheduler.",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit_RtpProc(void)
{
    PyObject *module;

    if (PyType_Ready(&PyRtpProcType) < 0)
        return NULL;
    if (PyType_Ready(&PyRtpProcChannelType) < 0)
        return NULL;

    module = PyModule_Create(&RtpProc_module);
    if (module == NULL)
        return NULL;

    ChannelProcError = PyErr_NewException(
        MODULE_NAME ".ChannelProcError", PyExc_RuntimeError, NULL);
    if (ChannelProcError == NULL) {
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&PyRtpProcType);
    PyModule_AddObject(module, "RtpProc", (PyObject *)&PyRtpProcType);
    Py_INCREF(&PyRtpProcChannelType);
    PyModule_AddObject(module, "RtpProcChannel", (PyObject *)&PyRtpProcChannelType);
    PyModule_AddObject(module, "ChannelProcError", ChannelProcError);

    return module;
}
