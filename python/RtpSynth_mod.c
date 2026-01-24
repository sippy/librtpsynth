#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#if !defined(__cplusplus) && !defined(nullptr)
#define nullptr NULL
#endif

#include <Python.h>

#include "rtpsynth.h"

#define MODULE_NAME "rtpsynth.RtpSynth"

typedef struct {
    PyObject_HEAD
    void *rs;
} PyRtpSynth;

static void
PyRtpSynth_dealloc(PyRtpSynth *self)
{
    if (self->rs != NULL) {
        rsynth_dtor(self->rs);
        self->rs = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
PyRtpSynth_init(PyRtpSynth *self, PyObject *args, PyObject *kwds)
{
    int srate = 0;
    int ptime = 0;

    if (!PyArg_ParseTuple(args, "ii:RtpSynth", &srate, &ptime))
        return -1;
    (void)kwds;

    self->rs = rsynth_ctor(srate, ptime);
    if (self->rs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "rsynth_ctor() failed");
        return -1;
    }
    return 0;
}

static PyObject *
PyRtpSynth_next_pkt(PyRtpSynth *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"plen", "pt", "pload", NULL};
    int plen = 0;
    int pt = 0;
    PyObject *pload = Py_None;
    PyObject *payload_bytes = NULL;
    const char *payload_data = NULL;
    Py_ssize_t payload_len = 0;
    int filled = 0;
    int pktlen;
    int outlen;
    PyObject *out = NULL;
    char *buf = NULL;

    if (self->rs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpSynth handle is not initialized");
        return NULL;
    }

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|O:next_pkt", kwlist,
        &plen, &pt, &pload))
        return NULL;

    pktlen = plen + 32;
    if (pktlen <= 0) {
        PyErr_SetString(PyExc_ValueError, "invalid packet length");
        return NULL;
    }

    if (pload != Py_None) {
        if (PyBytes_Check(pload)) {
            Py_INCREF(pload);
            payload_bytes = pload;
        } else {
            payload_bytes = PyObject_Bytes(pload);
            if (payload_bytes == NULL)
                return NULL;
        }
        if (PyBytes_AsStringAndSize(payload_bytes, (char **)&payload_data,
            &payload_len) != 0) {
            Py_DECREF(payload_bytes);
            return NULL;
        }
        if (payload_len > pktlen) {
            Py_DECREF(payload_bytes);
            PyErr_SetString(PyExc_ValueError, "payload is larger than packet buffer");
            return NULL;
        }
        filled = 1;
    }

    out = PyBytes_FromStringAndSize(NULL, pktlen);
    if (out == NULL) {
        Py_XDECREF(payload_bytes);
        return NULL;
    }
    buf = PyBytes_AsString(out);
    if (buf == NULL) {
        Py_DECREF(out);
        Py_XDECREF(payload_bytes);
        return NULL;
    }
    if (filled != 0) {
        memset(buf, 0, (size_t)pktlen);
        memcpy(buf, payload_data, (size_t)payload_len);
    }

    outlen = rsynth_next_pkt_pa(self->rs, plen, pt, buf, (unsigned int)pktlen, filled);
    if (outlen < 0 || outlen > pktlen) {
        Py_DECREF(out);
        Py_XDECREF(payload_bytes);
        PyErr_SetString(PyExc_RuntimeError, "rsynth_next_pkt_pa() failed");
        return NULL;
    }

    if (outlen != pktlen) {
        PyObject *shrunk = PyBytes_FromStringAndSize(buf, outlen);
        Py_DECREF(out);
        out = shrunk;
        if (out == NULL) {
            Py_XDECREF(payload_bytes);
            return NULL;
        }
    }

    Py_XDECREF(payload_bytes);
    return out;
}

static PyObject *
PyRtpSynth_pkt_free(PyRtpSynth *self, PyObject *args)
{
    PyObject *obj = NULL;
    void *ptr = NULL;

    if (!PyArg_ParseTuple(args, "O:pkt_free", &obj))
        return NULL;
    (void)self;

    if (PyCapsule_CheckExact(obj)) {
        ptr = PyCapsule_GetPointer(obj, NULL);
    } else if (PyLong_Check(obj)) {
        ptr = (void *)(uintptr_t)PyLong_AsUnsignedLongLong(obj);
        if (PyErr_Occurred())
            return NULL;
    } else {
        PyErr_SetString(PyExc_TypeError, "pkt_free expects a capsule or integer pointer");
        return NULL;
    }

    if (ptr != NULL)
        rsynth_pkt_free(ptr);
    Py_RETURN_NONE;
}

static PyObject *
PyRtpSynth_set_mbt(PyRtpSynth *self, PyObject *args)
{
    unsigned int new_st;
    unsigned int old_st;

    if (self->rs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpSynth handle is not initialized");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "I:set_mbt", &new_st))
        return NULL;

    old_st = rsynth_set_mbt(self->rs, new_st);
    return PyLong_FromUnsignedLong(old_st);
}

static PyObject *
PyRtpSynth_resync(PyRtpSynth *self, PyObject *args)
{
    struct rsynth_seq seq;

    if (self->rs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpSynth handle is not initialized");
        return NULL;
    }

    if (PyTuple_GET_SIZE(args) == 0) {
        rsynth_resync(self->rs, NULL);
        Py_RETURN_NONE;
    }

    if (!PyArg_ParseTuple(args, "KK:resync", &seq.ts, &seq.seq))
        return NULL;

    rsynth_resync(self->rs, &seq);
    Py_RETURN_NONE;
}

static PyObject *
PyRtpSynth_skip(PyRtpSynth *self, PyObject *args)
{
    int npkts;

    if (self->rs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "RtpSynth handle is not initialized");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "i:skip", &npkts))
        return NULL;

    rsynth_skip(self->rs, npkts);
    Py_RETURN_NONE;
}

static PyMethodDef PyRtpSynth_methods[] = {
    {"next_pkt", (PyCFunction)PyRtpSynth_next_pkt, METH_VARARGS | METH_KEYWORDS, NULL},
    {"pkt_free", (PyCFunction)PyRtpSynth_pkt_free, METH_VARARGS, NULL},
    {"set_mbt", (PyCFunction)PyRtpSynth_set_mbt, METH_VARARGS, NULL},
    {"resync", (PyCFunction)PyRtpSynth_resync, METH_VARARGS, NULL},
    {"skip", (PyCFunction)PyRtpSynth_skip, METH_VARARGS, NULL},
    {NULL}
};

static PyTypeObject PyRtpSynthType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpSynth",
    .tp_basicsize = sizeof(PyRtpSynth),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)PyRtpSynth_init,
    .tp_dealloc = (destructor)PyRtpSynth_dealloc,
    .tp_methods = PyRtpSynth_methods,
};

static struct PyModuleDef RtpSynth_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = "Python interface to RTP packet generator.",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit_RtpSynth(void)
{
    PyObject *module;

    if (PyType_Ready(&PyRtpSynthType) < 0)
        return NULL;

    module = PyModule_Create(&RtpSynth_module);
    if (module == NULL)
        return NULL;

    Py_INCREF(&PyRtpSynthType);
    PyModule_AddObject(module, "RtpSynth", (PyObject *)&PyRtpSynthType);

    return module;
}
