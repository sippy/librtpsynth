#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#if !defined(__cplusplus) && !defined(nullptr)
#define nullptr NULL
#endif

#include <Python.h>
#include <structmember.h>

#include "rtp.h"
#include "rtp_info.h"
#include "rtpjbuf.h"

#define MODULE_NAME "rtpsynth.RtpJBuf"
#define NO_INPUT_PTR ((const unsigned char *)1)

typedef struct {
    PyObject_HEAD
    struct rtp_info info;
} PyRTPInfo;

typedef struct {
    PyObject_HEAD
    struct rtp_packet pkt;
    PyObject *info_obj;
} PyRTPPacket;

typedef struct {
    PyObject_HEAD
    struct ers_frame ers;
} PyERSFrame;

typedef struct {
    PyObject_HEAD
    PyObject *rtp;
    PyObject *ers;
} PyRTPFrameUnion;

typedef struct {
    PyObject_HEAD
    int type;
    PyObject *frame;
} PyRTPFrame;

typedef struct {
    PyObject_HEAD
    PyObject *content;
    PyObject *data;
    PyObject *rtp_data;
} PyFrameWrapper;

typedef struct {
    PyObject_HEAD
    void *jb;
    PyObject **refs;
    const unsigned char **ptrs;
    unsigned int capacity;
    uint64_t dropped;
} PyRtpJBuf;

typedef struct {
    unsigned long long rtpinfo_created;
    unsigned long long rtpinfo_freed;
    unsigned long long rtppacket_created;
    unsigned long long rtppacket_freed;
    unsigned long long ersframe_created;
    unsigned long long ersframe_freed;
    unsigned long long rtpframeunion_created;
    unsigned long long rtpframeunion_freed;
    unsigned long long rtpframe_created;
    unsigned long long rtpframe_freed;
    unsigned long long framewrapper_created;
    unsigned long long framewrapper_freed;
    unsigned long long rtpjbuf_created;
    unsigned long long rtpjbuf_freed;
} DeallocCounters;

typedef struct {
    DeallocCounters counters;
    bool counting_enabled;
} DeallocCounts;

static DeallocCounts g_counts;

static PyObject *RTPParseError;

static PyTypeObject PyRTPInfoType;
static PyTypeObject PyRTPPacketType;
static PyTypeObject PyERSFrameType;
static PyTypeObject PyRTPFrameUnionType;
static PyTypeObject PyRTPFrameType;
static PyTypeObject PyFrameWrapperType;
static PyTypeObject PyRtpJBufType;

static void
count_inc(unsigned long long *counter)
{
    if (g_counts.counting_enabled)
        *counter += 1;
}

static PyObject *
py_ulonglong_from_size(size_t v)
{
    return PyLong_FromUnsignedLongLong((unsigned long long)v);
}

static PyObject *
PyRTPInfo_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.rtpinfo_created);
    return obj;
}

static PyObject *
PyRTPInfo_FromInfo(const struct rtp_info *info)
{
    PyRTPInfo *obj = PyObject_New(PyRTPInfo, &PyRTPInfoType);
    if (obj == NULL)
        return NULL;
    count_inc(&g_counts.counters.rtpinfo_created);
    obj->info = *info;
    return (PyObject *)obj;
}

static void
PyRTPInfo_dealloc(PyRTPInfo *self)
{
    count_inc(&g_counts.counters.rtpinfo_freed);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRTPInfo_init(PyRTPInfo *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "RTPInfo() takes no arguments");
        return -1;
    }
    (void)kwds;
    memset(&self->info, 0, sizeof(self->info));
    return 0;
}

static PyObject *PyRTPInfo_get_data_size(PyRTPInfo *self, void *closure) {
    (void)closure;
    return py_ulonglong_from_size(self->info.data_size);
}

static PyObject *PyRTPInfo_get_data_offset(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromLong(self->info.data_offset);
}

static PyObject *PyRTPInfo_get_nsamples(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromLong(self->info.nsamples);
}

static PyObject *PyRTPInfo_get_ts(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLong(self->info.ts);
}

static PyObject *PyRTPInfo_get_seq(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLong(self->info.seq);
}

static PyObject *PyRTPInfo_get_ssrc(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLong(self->info.ssrc);
}

static PyObject *PyRTPInfo_get_appendable(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(self->info.appendable != 0);
}

static PyObject *PyRTPInfo_get_rtp_profile(PyRTPInfo *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong((unsigned long long)(uintptr_t)self->info.rtp_profile);
}

static PyGetSetDef PyRTPInfo_getset[] = {
    {"data_size", (getter)PyRTPInfo_get_data_size, NULL, NULL, NULL},
    {"data_offset", (getter)PyRTPInfo_get_data_offset, NULL, NULL, NULL},
    {"nsamples", (getter)PyRTPInfo_get_nsamples, NULL, NULL, NULL},
    {"ts", (getter)PyRTPInfo_get_ts, NULL, NULL, NULL},
    {"seq", (getter)PyRTPInfo_get_seq, NULL, NULL, NULL},
    {"ssrc", (getter)PyRTPInfo_get_ssrc, NULL, NULL, NULL},
    {"appendable", (getter)PyRTPInfo_get_appendable, NULL, NULL, NULL},
    {"rtp_profile", (getter)PyRTPInfo_get_rtp_profile, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PyRTPInfoType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RTPInfo",
    .tp_basicsize = sizeof(PyRTPInfo),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRTPInfo_new,
    .tp_init = (initproc)PyRTPInfo_init,
    .tp_dealloc = (destructor)PyRTPInfo_dealloc,
    .tp_getset = PyRTPInfo_getset,
};

static PyObject *
PyRTPPacket_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.rtppacket_created);
    return obj;
}

static PyObject *
PyRTPPacket_NewSteal(const struct rtp_packet *pkt, PyObject *info_obj)
{
    PyRTPPacket *obj = PyObject_New(PyRTPPacket, &PyRTPPacketType);
    if (obj == NULL) {
        Py_DECREF(info_obj);
        return NULL;
    }
    count_inc(&g_counts.counters.rtppacket_created);
    obj->pkt = *pkt;
    obj->info_obj = info_obj;
    return (PyObject *)obj;
}

static void
PyRTPPacket_dealloc(PyRTPPacket *self)
{
    count_inc(&g_counts.counters.rtppacket_freed);
    Py_XDECREF(self->info_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRTPPacket_init(PyRTPPacket *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "RTPPacket() takes no arguments");
        return -1;
    }
    (void)kwds;
    memset(&self->pkt, 0, sizeof(self->pkt));
    Py_XDECREF(self->info_obj);
    self->info_obj = PyRTPInfo_FromInfo(&self->pkt.info);
    if (self->info_obj == NULL)
        return -1;
    return 0;
}

static PyObject *PyRTPPacket_get_info(PyRTPPacket *self, void *closure) {
    (void)closure;
    if (self->info_obj == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(self->info_obj);
    return self->info_obj;
}

static PyObject *PyRTPPacket_get_lseq(PyRTPPacket *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(self->pkt.lseq);
}

static PyObject *PyRTPPacket_get_data(PyRTPPacket *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong((unsigned long long)(uintptr_t)self->pkt.data);
}

static PyGetSetDef PyRTPPacket_getset[] = {
    {"info", (getter)PyRTPPacket_get_info, NULL, NULL, NULL},
    {"lseq", (getter)PyRTPPacket_get_lseq, NULL, NULL, NULL},
    {"data", (getter)PyRTPPacket_get_data, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PyRTPPacketType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RTPPacket",
    .tp_basicsize = sizeof(PyRTPPacket),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRTPPacket_new,
    .tp_init = (initproc)PyRTPPacket_init,
    .tp_dealloc = (destructor)PyRTPPacket_dealloc,
    .tp_getset = PyRTPPacket_getset,
};

static PyObject *
PyERSFrame_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.ersframe_created);
    return obj;
}

static PyObject *
PyERSFrame_FromErs(const struct ers_frame *ers)
{
    PyERSFrame *obj = PyObject_New(PyERSFrame, &PyERSFrameType);
    if (obj == NULL)
        return NULL;
    count_inc(&g_counts.counters.ersframe_created);
    obj->ers = *ers;
    return (PyObject *)obj;
}

static void
PyERSFrame_dealloc(PyERSFrame *self)
{
    count_inc(&g_counts.counters.ersframe_freed);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyERSFrame_init(PyERSFrame *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "ERSFrame() takes no arguments");
        return -1;
    }
    (void)kwds;
    memset(&self->ers, 0, sizeof(self->ers));
    return 0;
}

static PyObject *PyERSFrame_get_lseq_start(PyERSFrame *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(self->ers.lseq_start);
}

static PyObject *PyERSFrame_get_lseq_end(PyERSFrame *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(self->ers.lseq_end);
}

static PyObject *PyERSFrame_get_ts_diff(PyERSFrame *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLong(self->ers.ts_diff);
}

static PyObject *PyERSFrame_get_type(PyERSFrame *self, void *closure) {
    (void)self;
    (void)closure;
    return PyLong_FromLong(RFT_ERS);
}

static PyGetSetDef PyERSFrame_getset[] = {
    {"lseq_start", (getter)PyERSFrame_get_lseq_start, NULL, NULL, NULL},
    {"lseq_end", (getter)PyERSFrame_get_lseq_end, NULL, NULL, NULL},
    {"ts_diff", (getter)PyERSFrame_get_ts_diff, NULL, NULL, NULL},
    {"type", (getter)PyERSFrame_get_type, NULL, NULL, NULL},
    {NULL}
};

static PyTypeObject PyERSFrameType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".ERSFrame",
    .tp_basicsize = sizeof(PyERSFrame),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyERSFrame_new,
    .tp_init = (initproc)PyERSFrame_init,
    .tp_dealloc = (destructor)PyERSFrame_dealloc,
    .tp_getset = PyERSFrame_getset,
};

static PyObject *
PyRTPFrameUnion_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.rtpframeunion_created);
    return obj;
}

static PyObject *
PyRTPFrameUnion_NewSteal(PyObject *rtp_obj, PyObject *ers_obj)
{
    PyRTPFrameUnion *obj = PyObject_New(PyRTPFrameUnion, &PyRTPFrameUnionType);
    if (obj == NULL) {
        Py_XDECREF(rtp_obj);
        Py_XDECREF(ers_obj);
        return NULL;
    }
    count_inc(&g_counts.counters.rtpframeunion_created);
    if (rtp_obj == NULL) {
        rtp_obj = Py_None;
        Py_INCREF(rtp_obj);
    }
    if (ers_obj == NULL) {
        ers_obj = Py_None;
        Py_INCREF(ers_obj);
    }
    obj->rtp = rtp_obj;
    obj->ers = ers_obj;
    return (PyObject *)obj;
}

static void
PyRTPFrameUnion_dealloc(PyRTPFrameUnion *self)
{
    count_inc(&g_counts.counters.rtpframeunion_freed);
    Py_XDECREF(self->rtp);
    Py_XDECREF(self->ers);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRTPFrameUnion_init(PyRTPFrameUnion *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "RTPFrameUnion() takes no arguments");
        return -1;
    }
    (void)kwds;
    Py_XDECREF(self->rtp);
    Py_XDECREF(self->ers);
    self->rtp = Py_None;
    self->ers = Py_None;
    Py_INCREF(Py_None);
    Py_INCREF(Py_None);
    return 0;
}

static PyMemberDef PyRTPFrameUnion_members[] = {
    {"rtp", T_OBJECT_EX, offsetof(PyRTPFrameUnion, rtp), READONLY, NULL},
    {"ers", T_OBJECT_EX, offsetof(PyRTPFrameUnion, ers), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyRTPFrameUnionType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RTPFrameUnion",
    .tp_basicsize = sizeof(PyRTPFrameUnion),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRTPFrameUnion_new,
    .tp_init = (initproc)PyRTPFrameUnion_init,
    .tp_dealloc = (destructor)PyRTPFrameUnion_dealloc,
    .tp_members = PyRTPFrameUnion_members,
};

static PyObject *
PyRTPFrame_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.rtpframe_created);
    return obj;
}

static PyObject *
PyRTPFrame_NewSteal(int type, PyObject *frame_union)
{
    PyRTPFrame *obj = PyObject_New(PyRTPFrame, &PyRTPFrameType);
    if (obj == NULL) {
        Py_DECREF(frame_union);
        return NULL;
    }
    count_inc(&g_counts.counters.rtpframe_created);
    obj->type = type;
    obj->frame = frame_union;
    return (PyObject *)obj;
}

static void
PyRTPFrame_dealloc(PyRTPFrame *self)
{
    count_inc(&g_counts.counters.rtpframe_freed);
    Py_XDECREF(self->frame);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRTPFrame_init(PyRTPFrame *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "RTPFrame() takes no arguments");
        return -1;
    }
    (void)kwds;
    self->type = RFT_RTP;
    Py_XDECREF(self->frame);
    self->frame = Py_None;
    Py_INCREF(Py_None);
    return 0;
}

static PyMemberDef PyRTPFrame_members[] = {
    {"type", T_INT, offsetof(PyRTPFrame, type), READONLY, NULL},
    {"frame", T_OBJECT_EX, offsetof(PyRTPFrame, frame), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyRTPFrameType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RTPFrame",
    .tp_basicsize = sizeof(PyRTPFrame),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRTPFrame_new,
    .tp_init = (initproc)PyRTPFrame_init,
    .tp_dealloc = (destructor)PyRTPFrame_dealloc,
    .tp_members = PyRTPFrame_members,
};

static PyObject *
PyFrameWrapper_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.framewrapper_created);
    return obj;
}

static PyObject *
PyFrameWrapper_NewSteal(PyObject *content, PyObject *data, PyObject *rtp_data)
{
    PyFrameWrapper *obj = PyObject_New(PyFrameWrapper, &PyFrameWrapperType);
    if (obj == NULL) {
        Py_DECREF(content);
        Py_XDECREF(data);
        Py_XDECREF(rtp_data);
        return NULL;
    }
    count_inc(&g_counts.counters.framewrapper_created);
    if (data == NULL) {
        data = Py_None;
        Py_INCREF(data);
    }
    if (rtp_data == NULL) {
        rtp_data = Py_None;
        Py_INCREF(rtp_data);
    }
    obj->content = content;
    obj->data = data;
    obj->rtp_data = rtp_data;
    return (PyObject *)obj;
}

static void
PyFrameWrapper_dealloc(PyFrameWrapper *self)
{
    count_inc(&g_counts.counters.framewrapper_freed);
    Py_XDECREF(self->content);
    Py_XDECREF(self->data);
    Py_XDECREF(self->rtp_data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyFrameWrapper_init(PyFrameWrapper *self, PyObject *args, PyObject *kwds)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "FrameWrapper() takes no arguments");
        return -1;
    }
    (void)kwds;
    Py_XDECREF(self->content);
    Py_XDECREF(self->data);
    Py_XDECREF(self->rtp_data);
    self->content = Py_None;
    self->data = Py_None;
    self->rtp_data = Py_None;
    Py_INCREF(Py_None);
    Py_INCREF(Py_None);
    Py_INCREF(Py_None);
    return 0;
}

static PyObject *
PyFrameWrapper_str(PyFrameWrapper *self)
{
    if (self->content != NULL && PyObject_TypeCheck(self->content, &PyRTPFrameType)) {
        PyRTPFrame *rf = (PyRTPFrame *)self->content;
        if (rf->frame != NULL && PyObject_TypeCheck(rf->frame, &PyRTPFrameUnionType)) {
            PyRTPFrameUnion *fu = (PyRTPFrameUnion *)rf->frame;
            if (fu->rtp != NULL && PyObject_TypeCheck(fu->rtp, &PyRTPPacketType)) {
                PyRTPPacket *pkt = (PyRTPPacket *)fu->rtp;
                return PyUnicode_FromFormat("RTP_Frame(seq=%llu)",
                    (unsigned long long)pkt->pkt.lseq);
            }
        }
        return PyUnicode_FromString("RTP_Frame");
    }
    if (self->content != NULL && PyObject_TypeCheck(self->content, &PyERSFrameType)) {
        PyERSFrame *ers = (PyERSFrame *)self->content;
        return PyUnicode_FromFormat("RTP_Erasure(seq_range=%llu -- %llu)",
            (unsigned long long)ers->ers.lseq_start,
            (unsigned long long)ers->ers.lseq_end);
    }
    return PyUnicode_FromString("FrameWrapper");
}

static PyMemberDef PyFrameWrapper_members[] = {
    {"content", T_OBJECT_EX, offsetof(PyFrameWrapper, content), READONLY, NULL},
    {"data", T_OBJECT_EX, offsetof(PyFrameWrapper, data), READONLY, NULL},
    {"rtp_data", T_OBJECT_EX, offsetof(PyFrameWrapper, rtp_data), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyFrameWrapperType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".FrameWrapper",
    .tp_basicsize = sizeof(PyFrameWrapper),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyFrameWrapper_new,
    .tp_init = (initproc)PyFrameWrapper_init,
    .tp_dealloc = (destructor)PyFrameWrapper_dealloc,
    .tp_members = PyFrameWrapper_members,
    .tp_str = (reprfunc)PyFrameWrapper_str,
    .tp_repr = (reprfunc)PyFrameWrapper_str,
};

static PyObject *
build_wrapper_rtp(struct rtp_frame *fp, PyObject *data_obj)
{
    PyObject *info_obj = PyRTPInfo_FromInfo(&fp->rtp.info);
    PyObject *pkt_obj = NULL;
    PyObject *union_obj = NULL;
    PyObject *frame_obj = NULL;
    PyObject *rtp_data = NULL;
    PyObject *wrapper = NULL;
    const unsigned char *payload = NULL;
    size_t payload_size = fp->rtp.info.data_size;

    if (info_obj == NULL) {
        Py_DECREF(data_obj);
        return NULL;
    }
    pkt_obj = PyRTPPacket_NewSteal(&fp->rtp, info_obj);
    info_obj = NULL;
    if (pkt_obj == NULL) {
        Py_DECREF(data_obj);
        return NULL;
    }
    union_obj = PyRTPFrameUnion_NewSteal(pkt_obj, NULL);
    pkt_obj = NULL;
    if (union_obj == NULL) {
        Py_DECREF(data_obj);
        return NULL;
    }
    frame_obj = PyRTPFrame_NewSteal(RFT_RTP, union_obj);
    union_obj = NULL;
    if (frame_obj == NULL) {
        Py_DECREF(data_obj);
        return NULL;
    }
    if (payload_size > 0) {
        payload = fp->rtp.data + fp->rtp.info.data_offset;
    }
    rtp_data = PyBytes_FromStringAndSize((const char *)payload, payload_size);
    if (rtp_data == NULL) {
        Py_DECREF(frame_obj);
        Py_DECREF(data_obj);
        return NULL;
    }
    wrapper = PyFrameWrapper_NewSteal(frame_obj, data_obj, rtp_data);
    if (wrapper == NULL)
        return NULL;
    return wrapper;
}

static PyObject *
build_wrapper_ers(struct rtp_frame *fp)
{
    PyObject *ers_obj = PyERSFrame_FromErs(&fp->ers);
    if (ers_obj == NULL)
        return NULL;
    return PyFrameWrapper_NewSteal(ers_obj, NULL, NULL);
}

static PyObject *
fetch_data_ref(PyRtpJBuf *self, const unsigned char *ptr, PyObject *input_bytes,
    const unsigned char *input_ptr)
{
    assert(self->refs != NULL);
    assert(self->ptrs != NULL);
    assert(ptr != NULL);
    assert(input_ptr != NULL);
    assert(input_bytes != NULL);
    if (ptr == input_ptr) {
        Py_INCREF(input_bytes);
        return input_bytes;
    }
    for (unsigned int i = 0; i < self->capacity; i++) {
        if (self->ptrs[i] == ptr) {
            PyObject *obj = self->refs[i];
            self->refs[i] = NULL;
            self->ptrs[i] = NULL;
            return obj;
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

static int
store_data_ref(PyRtpJBuf *self, PyObject *input_bytes, const unsigned char *ptr)
{
    assert(self->refs != NULL);
    assert(self->ptrs != NULL);
    assert(ptr != NULL);
    for (unsigned int i = 0; i < self->capacity; i++) {
        if (self->refs[i] == NULL) {
            Py_INCREF(input_bytes);
            self->refs[i] = input_bytes;
            self->ptrs[i] = ptr;
            return 0;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "RtpJBuf reference buffer exhausted");
    return -1;
}

static int
list_has_data_ptr(struct rtp_frame *fp, const unsigned char *ptr)
{
    assert(ptr != NULL);
    for (; fp != NULL; fp = fp->next) {
        if (fp->type == RFT_RTP && fp->rtp.data == ptr)
            return 1;
    }
    return 0;
}

static void
process_drop_list(PyRtpJBuf *self, struct rtp_frame *fp, PyObject *input_bytes,
    const unsigned char *input_ptr);

static PyObject *
process_ready_list(PyRtpJBuf *self, struct rtp_frame *fp, PyObject *input_bytes,
    const unsigned char *input_ptr)
{
    PyObject *ready_list = PyList_New(0);
    if (ready_list == NULL)
        return NULL;

    while (fp != NULL) {
        struct rtp_frame *next = fp->next;
        PyObject *wrapper = NULL;

        if (fp->type == RFT_RTP) {
            PyObject *data_obj = fetch_data_ref(self, fp->rtp.data, input_bytes, input_ptr);
            if (data_obj == NULL) {
                Py_DECREF(ready_list);
                return NULL;
            }
            wrapper = build_wrapper_rtp(fp, data_obj);
            rtpjbuf_frame_dtor(fp);
        } else {
            wrapper = build_wrapper_ers(fp);
        }

        if (wrapper == NULL) {
            process_drop_list(self, next, input_bytes, input_ptr);
            Py_DECREF(ready_list);
            return NULL;
        }
        if (PyList_Append(ready_list, wrapper) != 0) {
            Py_DECREF(wrapper);
            process_drop_list(self, next, input_bytes, input_ptr);
            Py_DECREF(ready_list);
            return NULL;
        }
        Py_DECREF(wrapper);
        fp = next;
    }
    return ready_list;
}

static void
process_drop_list(PyRtpJBuf *self, struct rtp_frame *fp, PyObject *input_bytes,
    const unsigned char *input_ptr)
{
    while (fp != NULL) {
        struct rtp_frame *next = fp->next;
        if (fp->type == RFT_RTP) {
            self->dropped += 1;
            PyObject *data_obj = fetch_data_ref(self, fp->rtp.data, input_bytes, input_ptr);
            Py_XDECREF(data_obj);
            rtpjbuf_frame_dtor(fp);
        }
        fp = next;
    }
}

static int
ensure_bytes(PyObject *obj, PyObject **out_bytes, const unsigned char **data, Py_ssize_t *size)
{
    if (PyBytes_Check(obj)) {
        Py_INCREF(obj);
        *out_bytes = obj;
    } else {
        PyObject *bytes_obj = PyObject_Bytes(obj);
        if (bytes_obj == NULL)
            return -1;
        *out_bytes = bytes_obj;
    }
    if (PyBytes_AsStringAndSize(*out_bytes, (char **)data, size) != 0) {
        Py_DECREF(*out_bytes);
        return -1;
    }
    return 0;
}

static PyObject *
PyRtpJBuf_udp_in(PyRtpJBuf *self, PyObject *args)
{
    PyObject *data_obj = NULL;
    PyObject *input_bytes = NULL;
    const unsigned char *data = NULL;
    Py_ssize_t size = 0;
    struct rjb_udp_in_r ruir;
    PyObject *ready_list = NULL;
    int retained = 1;

    if (!PyArg_ParseTuple(args, "O:udp_in", &data_obj))
        return NULL;
    if (self->jb == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpJBuf handle is not initialized");
        return NULL;
    }
    if (ensure_bytes(data_obj, &input_bytes, &data, &size) != 0)
        return NULL;

    ruir = rtpjbuf_udp_in(self->jb, data, (size_t)size);
    if (ruir.error != 0) {
        if (ruir.drop != NULL) {
            process_drop_list(self, ruir.drop, input_bytes, data);
        }
        Py_DECREF(input_bytes);
        if (ruir.error < RTP_PARSER_OK) {
            PyErr_Format(RTPParseError, "rtpjbuf_udp_in(): error %d", ruir.error);
            return NULL;
        }
        PyErr_Format(PyExc_RuntimeError, "rtpjbuf_udp_in(): error %d", ruir.error);
        return NULL;
    }

    if (list_has_data_ptr(ruir.ready, data) || list_has_data_ptr(ruir.drop, data))
        retained = 0;
    if (retained != 0) {
        if (store_data_ref(self, input_bytes, data) != 0) {
            process_drop_list(self, ruir.ready, input_bytes, data);
            process_drop_list(self, ruir.drop, input_bytes, data);
            Py_DECREF(input_bytes);
            return NULL;
        }
    }

    ready_list = process_ready_list(self, ruir.ready, input_bytes, data);
    process_drop_list(self, ruir.drop, input_bytes, data);
    Py_DECREF(input_bytes);
    return ready_list;
}

static PyObject *
PyRtpJBuf_flush(PyRtpJBuf *self, PyObject *args)
{
    struct rjb_udp_in_r ruir;
    PyObject *ready_list;

    if (!PyArg_ParseTuple(args, ":flush"))
        return NULL;
    if (self->jb == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpJBuf handle is not initialized");
        return NULL;
    }

    ruir = rtpjbuf_flush(self->jb);
    ready_list = process_ready_list(self, ruir.ready, Py_None, NO_INPUT_PTR);
    process_drop_list(self, ruir.drop, Py_None, NO_INPUT_PTR);
    return ready_list;
}

static void
PyRtpJBuf_dealloc(PyRtpJBuf *self)
{
    count_inc(&g_counts.counters.rtpjbuf_freed);
    if (self->jb != NULL) {
        for (;;) {
            struct rjb_udp_in_r ruir = rtpjbuf_flush(self->jb);
            if (ruir.ready == NULL && ruir.drop == NULL)
                break;
            process_drop_list(self, ruir.ready, Py_None, NO_INPUT_PTR);
            process_drop_list(self, ruir.drop, Py_None, NO_INPUT_PTR);
        }
        rtpjbuf_dtor(self->jb);
        self->jb = NULL;
    }
    if (self->refs != NULL) {
        for (unsigned int i = 0; i < self->capacity; i++) {
            Py_XDECREF(self->refs[i]);
        }
        PyMem_Free(self->refs);
        self->refs = NULL;
    }
    if (self->ptrs != NULL) {
        PyMem_Free(self->ptrs);
        self->ptrs = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
PyRtpJBuf_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = PyType_GenericNew(type, args, kwds);
    if (obj != NULL)
        count_inc(&g_counts.counters.rtpjbuf_created);
    return obj;
}

static int
PyRtpJBuf_init(PyRtpJBuf *self, PyObject *args, PyObject *kwds)
{
    unsigned int capacity = 0;
    if (!PyArg_ParseTuple(args, "I:RtpJBuf", &capacity))
        return -1;
    (void)kwds;
    self->jb = rtpjbuf_ctor(capacity);
    if (self->jb == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "rtpjbuf_ctor() failed");
        return -1;
    }
    self->capacity = capacity;
    self->dropped = 0;
    self->refs = PyMem_Calloc(capacity, sizeof(*self->refs));
    self->ptrs = PyMem_Calloc(capacity, sizeof(*self->ptrs));
    if (self->refs == NULL || self->ptrs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpJBuf reference buffer allocation failed");
        if (self->refs != NULL) {
            PyMem_Free(self->refs);
            self->refs = NULL;
        }
        if (self->ptrs != NULL) {
            PyMem_Free(self->ptrs);
            self->ptrs = NULL;
        }
        rtpjbuf_dtor(self->jb);
        self->jb = NULL;
        return -1;
    }
    return 0;
}

static PyMethodDef PyRtpJBuf_methods[] = {
    {"udp_in", (PyCFunction)PyRtpJBuf_udp_in, METH_VARARGS, NULL},
    {"flush", (PyCFunction)PyRtpJBuf_flush, METH_VARARGS, NULL},
    {NULL}
};

static PyMemberDef PyRtpJBuf_members[] = {
    {"dropped", T_ULONGLONG, offsetof(PyRtpJBuf, dropped), READONLY, NULL},
    {NULL}
};

static PyTypeObject PyRtpJBufType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpJBuf",
    .tp_basicsize = sizeof(PyRtpJBuf),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyRtpJBuf_new,
    .tp_init = (initproc)PyRtpJBuf_init,
    .tp_dealloc = (destructor)PyRtpJBuf_dealloc,
    .tp_methods = PyRtpJBuf_methods,
    .tp_members = PyRtpJBuf_members,
};

static int
add_count(PyObject *dict, const char *name, unsigned long long value)
{
    PyObject *obj = PyLong_FromUnsignedLongLong(value);
    if (obj == NULL)
        return -1;
    if (PyDict_SetItemString(dict, name, obj) != 0) {
        Py_DECREF(obj);
        return -1;
    }
    Py_DECREF(obj);
    return 0;
}

static PyObject *
PyRtpJBuf_get_dealloc_counts(PyObject *self, PyObject *args)
{
    PyObject *dict;

    (void)self;
    if (!PyArg_ParseTuple(args, ":_get_dealloc_counts"))
        return NULL;

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    if (add_count(dict, "RTPInfo_created", g_counts.counters.rtpinfo_created) != 0 ||
        add_count(dict, "RTPInfo_freed", g_counts.counters.rtpinfo_freed) != 0 ||
        add_count(dict, "RTPPacket_created", g_counts.counters.rtppacket_created) != 0 ||
        add_count(dict, "RTPPacket_freed", g_counts.counters.rtppacket_freed) != 0 ||
        add_count(dict, "ERSFrame_created", g_counts.counters.ersframe_created) != 0 ||
        add_count(dict, "ERSFrame_freed", g_counts.counters.ersframe_freed) != 0 ||
        add_count(dict, "RTPFrameUnion_created", g_counts.counters.rtpframeunion_created) != 0 ||
        add_count(dict, "RTPFrameUnion_freed", g_counts.counters.rtpframeunion_freed) != 0 ||
        add_count(dict, "RTPFrame_created", g_counts.counters.rtpframe_created) != 0 ||
        add_count(dict, "RTPFrame_freed", g_counts.counters.rtpframe_freed) != 0 ||
        add_count(dict, "FrameWrapper_created", g_counts.counters.framewrapper_created) != 0 ||
        add_count(dict, "FrameWrapper_freed", g_counts.counters.framewrapper_freed) != 0 ||
        add_count(dict, "RtpJBuf_created", g_counts.counters.rtpjbuf_created) != 0 ||
        add_count(dict, "RtpJBuf_freed", g_counts.counters.rtpjbuf_freed) != 0) {
        Py_DECREF(dict);
        return NULL;
    }
    return dict;
}

static PyObject *
PyRtpJBuf_reset_dealloc_counts(PyObject *self, PyObject *args)
{
    (void)self;
    if (!PyArg_ParseTuple(args, ":_reset_dealloc_counts"))
        return NULL;
    memset(&g_counts.counters, 0, sizeof(g_counts.counters));
    Py_RETURN_NONE;
}

static PyObject *
PyRtpJBuf_set_dealloc_counting(PyObject *self, PyObject *args)
{
    PyObject *flag = NULL;
    int enabled = 0;

    (void)self;
    if (!PyArg_ParseTuple(args, "O:_set_dealloc_counting", &flag))
        return NULL;
    enabled = PyObject_IsTrue(flag);
    if (enabled < 0)
        return NULL;
    g_counts.counting_enabled = enabled != 0;
    Py_RETURN_NONE;
}

static PyObject *
PyRtpJBuf_get_dealloc_counting(PyObject *self, PyObject *args)
{
    (void)self;
    if (!PyArg_ParseTuple(args, ":_get_dealloc_counting"))
        return NULL;
    return PyBool_FromLong(g_counts.counting_enabled ? 1 : 0);
}

static PyObject *
make_frame_type(void)
{
    PyObject *name = PyUnicode_FromString("RTPFrameType");
    PyObject *bases = NULL;
    PyObject *dict = NULL;
    PyObject *type_obj = NULL;

    if (name == NULL)
        return NULL;
    bases = PyTuple_Pack(1, (PyObject *)&PyBaseObject_Type);
    if (bases == NULL) {
        Py_DECREF(name);
        return NULL;
    }
    dict = PyDict_New();
    if (dict == NULL) {
        Py_DECREF(name);
        Py_DECREF(bases);
        return NULL;
    }
    {
        PyObject *rtp_val = PyLong_FromLong(RFT_RTP);
        if (rtp_val == NULL)
            goto error;
        if (PyDict_SetItemString(dict, "RTP", rtp_val) != 0) {
            Py_DECREF(rtp_val);
            goto error;
        }
        Py_DECREF(rtp_val);
    }
    {
        PyObject *ers_val = PyLong_FromLong(RFT_ERS);
        if (ers_val == NULL)
            goto error;
        if (PyDict_SetItemString(dict, "ERS", ers_val) != 0) {
            Py_DECREF(ers_val);
            goto error;
        }
        Py_DECREF(ers_val);
    }
    type_obj = PyObject_CallFunctionObjArgs((PyObject *)&PyType_Type,
        name, bases, dict, NULL);
    if (type_obj == NULL)
        goto error;

    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(dict);
    return type_obj;

error:
    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(dict);
    Py_XDECREF(type_obj);
    return NULL;
}

static PyMethodDef RtpJBuf_module_methods[] = {
    {"_get_dealloc_counts", (PyCFunction)PyRtpJBuf_get_dealloc_counts, METH_VARARGS, NULL},
    {"_reset_dealloc_counts", (PyCFunction)PyRtpJBuf_reset_dealloc_counts, METH_VARARGS, NULL},
    {"_set_dealloc_counting", (PyCFunction)PyRtpJBuf_set_dealloc_counting, METH_VARARGS, NULL},
    {"_get_dealloc_counting", (PyCFunction)PyRtpJBuf_get_dealloc_counting, METH_VARARGS, NULL},
    {NULL}
};

static struct PyModuleDef RtpJBuf_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = "Python interface to the RTP jitter buffer.",
    .m_size = -1,
    .m_methods = RtpJBuf_module_methods,
};

PyMODINIT_FUNC
PyInit_RtpJBuf(void)
{
    PyObject *module;
    PyObject *frame_type;

    if (PyType_Ready(&PyRTPInfoType) < 0)
        return NULL;
    if (PyType_Ready(&PyRTPPacketType) < 0)
        return NULL;
    if (PyType_Ready(&PyERSFrameType) < 0)
        return NULL;
    if (PyType_Ready(&PyRTPFrameUnionType) < 0)
        return NULL;
    if (PyType_Ready(&PyRTPFrameType) < 0)
        return NULL;
    if (PyType_Ready(&PyFrameWrapperType) < 0)
        return NULL;
    if (PyType_Ready(&PyRtpJBufType) < 0)
        return NULL;

    module = PyModule_Create(&RtpJBuf_module);
    if (module == NULL)
        return NULL;

    RTPParseError = PyErr_NewException(MODULE_NAME ".RTPParseError", NULL, NULL);
    if (RTPParseError == NULL)
        return NULL;
    PyModule_AddObject(module, "RTPParseError", RTPParseError);

    frame_type = make_frame_type();
    if (frame_type == NULL)
        return NULL;
    PyModule_AddObject(module, "RTPFrameType", frame_type);

    Py_INCREF(&PyRTPInfoType);
    Py_INCREF(&PyRTPPacketType);
    Py_INCREF(&PyERSFrameType);
    Py_INCREF(&PyRTPFrameUnionType);
    Py_INCREF(&PyRTPFrameType);
    Py_INCREF(&PyFrameWrapperType);
    Py_INCREF(&PyRtpJBufType);
    PyModule_AddObject(module, "RTPInfo", (PyObject *)&PyRTPInfoType);
    PyModule_AddObject(module, "RTPPacket", (PyObject *)&PyRTPPacketType);
    PyModule_AddObject(module, "ERSFrame", (PyObject *)&PyERSFrameType);
    PyModule_AddObject(module, "RTPFrameUnion", (PyObject *)&PyRTPFrameUnionType);
    PyModule_AddObject(module, "RTPFrame", (PyObject *)&PyRTPFrameType);
    PyModule_AddObject(module, "FrameWrapper", (PyObject *)&PyFrameWrapperType);
    PyModule_AddObject(module, "RtpJBuf", (PyObject *)&PyRtpJBufType);

    PyModule_AddIntConstant(module, "RTP_PARSER_OK", RTP_PARSER_OK);
    PyModule_AddIntConstant(module, "RJB_ENOMEM", RJB_ENOMEM);

    return module;
}
