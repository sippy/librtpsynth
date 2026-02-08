#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

#if !defined(__cplusplus) && !defined(nullptr)
#define nullptr NULL
#endif

#include <Python.h>

#define MODULE_NAME "rtpsynth.RtpUtils"
#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

static PyTypeObject PyRtpUtilsType;
static PyObject *g_array_ctor = NULL;

static uint8_t g_pcm16_to_ulaw[65536];
static int16_t g_ulaw_to_pcm[256];
static int g_tables_ready = 0;

static int
floor_ll(double x, long long *out)
{
    long long iv;
    if (out == NULL)
        return -1;
    if (x > (double)LLONG_MAX || x < (double)LLONG_MIN)
        return -1;
    iv = (long long)x;
    if ((double)iv > x)
        iv -= 1;
    *out = iv;
    return 0;
}

static int
round_half_even_ll(double x, long long *out)
{
    long long fl;
    double frac;
    if (out == NULL)
        return -1;
    if (floor_ll(x, &fl) != 0)
        return -1;
    frac = x - (double)fl;
    if (frac < 0.5) {
        *out = fl;
        return 0;
    }
    if (frac > 0.5) {
        if (fl == LLONG_MAX)
            return -1;
        *out = fl + 1;
        return 0;
    }
    if ((fl & 1LL) == 0) {
        *out = fl;
        return 0;
    }
    if (fl == LLONG_MAX)
        return -1;
    *out = fl + 1;
    return 0;
}

static uint8_t
linear2ulaw_scalar(long long sample)
{
    int sign = sample < 0 ? 0x80 : 0;
    int exponent = 7;
    int mask = 0x4000;
    int mantissa;
    int ulaw;
    long long mag = sample;

    if (mag < 0)
        mag = -mag;
    if (mag > ULAW_CLIP)
        mag = ULAW_CLIP;
    mag += ULAW_BIAS;

    while (exponent > 0 && ((mag & mask) == 0)) {
        exponent -= 1;
        mask >>= 1;
    }

    mantissa = (int)((mag >> (exponent + 3)) & 0x0F);
    ulaw = ~(sign | (exponent << 4) | mantissa) & 0xFF;
    return (uint8_t)ulaw;
}

static int16_t
ulaw2linear_scalar(uint8_t ulaw)
{
    int sign;
    int exponent;
    int mantissa;
    int sample;

    ulaw = (uint8_t)((~ulaw) & 0xFF);
    sign = ulaw & 0x80;
    exponent = (ulaw >> 4) & 0x07;
    mantissa = ulaw & 0x0F;
    sample = ((mantissa << 3) + ULAW_BIAS) << exponent;
    sample -= ULAW_BIAS;
    if (sign != 0)
        sample = -sample;
    return (int16_t)sample;
}

static void
ensure_tables(void)
{
    int i;

    if (g_tables_ready != 0)
        return;

    for (i = -32768; i <= 32767; i++) {
        g_pcm16_to_ulaw[(uint16_t)(i + 32768)] = linear2ulaw_scalar((long long)i);
    }
    for (i = 0; i < 256; i++)
        g_ulaw_to_pcm[i] = ulaw2linear_scalar((uint8_t)i);
    g_tables_ready = 1;
}

static PyObject *
array_h_from_pcm16(const int16_t *samples, Py_ssize_t nsamples)
{
    PyObject *arr = NULL;
    PyObject *raw = NULL;
    PyObject *frombytes_rv = NULL;

    if (g_array_ctor == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "array.array constructor is not initialized");
        return NULL;
    }

    arr = PyObject_CallFunction(g_array_ctor, "s", "h");
    if (arr == NULL)
        return NULL;

    if (nsamples == 0)
        return arr;

    raw = PyBytes_FromStringAndSize((const char *)samples,
        nsamples * (Py_ssize_t)sizeof(*samples));
    if (raw == NULL) {
        Py_DECREF(arr);
        return NULL;
    }

    frombytes_rv = PyObject_CallMethod(arr, "frombytes", "O", raw);
    Py_DECREF(raw);
    if (frombytes_rv == NULL) {
        Py_DECREF(arr);
        return NULL;
    }
    Py_DECREF(frombytes_rv);
    return arr;
}

static int
pcm16_from_obj(PyObject *obj, int16_t **samples_out, Py_ssize_t *nsamples_out,
    Py_buffer *view_out, int16_t **owned_out)
{
    Py_buffer view;
    int16_t *owned = NULL;
    int16_t *samples = NULL;
    Py_ssize_t nsamples = 0;

    if (samples_out == NULL || nsamples_out == NULL || view_out == NULL ||
            owned_out == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "invalid pcm16 parser outputs");
        return -1;
    }

    memset(&view, 0, sizeof(view));
    if (PyObject_GetBuffer(obj, &view, PyBUF_CONTIG_RO) != 0)
        return -1;

    if (view.len < 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "invalid buffer length");
        return -1;
    }

    if (view.itemsize == 2) {
        if ((view.len % 2) != 0) {
            PyBuffer_Release(&view);
            PyErr_SetString(PyExc_ValueError, "PCM16 buffer must be 2-byte aligned");
            return -1;
        }
        samples = (int16_t *)view.buf;
        nsamples = view.len / 2;
    } else if (view.itemsize == 1) {
        if ((view.len % 2) != 0) {
            PyBuffer_Release(&view);
            PyErr_SetString(PyExc_ValueError, "PCM16 byte buffer length must be even");
            return -1;
        }
        nsamples = view.len / 2;
        if (nsamples > 0) {
            owned = PyMem_Malloc((size_t)nsamples * sizeof(*owned));
            if (owned == NULL) {
                PyBuffer_Release(&view);
                PyErr_NoMemory();
                return -1;
            }
            memcpy(owned, view.buf, (size_t)view.len);
            samples = owned;
        } else {
            samples = NULL;
        }
    } else {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_TypeError,
            "expected a PCM16 buffer (itemsize 2) or PCM16 bytes");
        return -1;
    }

    *samples_out = samples;
    *nsamples_out = nsamples;
    *view_out = view;
    *owned_out = owned;
    return 0;
}

static PyObject *
PyRtpUtils_resample_linear(PyObject *self, PyObject *args)
{
    PyObject *pcm_obj;
    int in_rate;
    int out_rate;
    Py_buffer view;
    int16_t *in_samples = NULL;
    int16_t *owned_in = NULL;
    Py_ssize_t n_in = 0;
    Py_ssize_t n_out;
    int16_t *out_samples = NULL;
    double ratio;
    Py_ssize_t i;
    PyObject *out = NULL;
    (void)self;

    if (!PyArg_ParseTuple(args, "Oii:resample_linear", &pcm_obj, &in_rate, &out_rate))
        return NULL;
    if (in_rate <= 0 || out_rate <= 0) {
        PyErr_SetString(PyExc_ValueError, "in_rate and out_rate must be > 0");
        return NULL;
    }

    if (pcm16_from_obj(pcm_obj, &in_samples, &n_in, &view, &owned_in) != 0)
        return NULL;

    if (in_rate == out_rate) {
        out = array_h_from_pcm16(in_samples, n_in);
        goto done;
    }

    if (n_in == 0) {
        out = array_h_from_pcm16(NULL, 0);
        goto done;
    }

    {
        double n_out_f = ((double)n_in * (double)out_rate) / (double)in_rate;
        long long n_out_ll = 0;
        if (round_half_even_ll(n_out_f, &n_out_ll) != 0 ||
                n_out_ll > (long long)(((size_t)-1) >> 1)) {
            PyErr_SetString(PyExc_OverflowError, "resampled output is too large");
            goto done;
        }
        if (n_out_ll < 1)
            n_out_ll = 1;
        n_out = (Py_ssize_t)n_out_ll;
    }

    out_samples = PyMem_Malloc((size_t)n_out * sizeof(*out_samples));
    if (out_samples == NULL) {
        PyErr_NoMemory();
        goto done;
    }

    ratio = (double)in_rate / (double)out_rate;
    for (i = 0; i < n_out; i++) {
        double pos = (double)i * ratio;
        Py_ssize_t idx = (Py_ssize_t)pos;
        double frac = pos - (double)idx;
        double sample;
        long long val_ll;
        int val;

        if (idx >= (n_in - 1)) {
            sample = (double)in_samples[n_in - 1];
        } else {
            double s0 = (double)in_samples[idx];
            double s1 = (double)in_samples[idx + 1];
            sample = s0 + (s1 - s0) * frac;
        }

        if (round_half_even_ll(sample, &val_ll) != 0) {
            PyErr_SetString(PyExc_OverflowError, "resample rounding overflow");
            goto done;
        }
        if (val_ll > 32767)
            val = 32767;
        else if (val_ll < -32768)
            val = -32768;
        else
            val = (int)val_ll;

        out_samples[i] = (int16_t)val;
    }

    out = array_h_from_pcm16(out_samples, n_out);

done:
    PyMem_Free(out_samples);
    PyMem_Free(owned_in);
    PyBuffer_Release(&view);
    return out;
}

static PyObject *
PyRtpUtils_linear2ulaw(PyObject *self, PyObject *args)
{
    PyObject *obj;
    PyObject *out = NULL;
    Py_buffer view;
    int16_t *samples = NULL;
    int16_t *owned = NULL;
    Py_ssize_t nsamples = 0;
    unsigned char *outp;
    Py_ssize_t i;
    (void)self;

    ensure_tables();

    if (!PyArg_ParseTuple(args, "O:linear2ulaw", &obj))
        return NULL;

    if (PyLong_Check(obj)) {
        long long sample = PyLong_AsLongLong(obj);
        if (PyErr_Occurred())
            return NULL;
        return PyLong_FromLong((long)linear2ulaw_scalar(sample));
    }

    if (pcm16_from_obj(obj, &samples, &nsamples, &view, &owned) != 0)
        return NULL;

    out = PyBytes_FromStringAndSize(NULL, nsamples);
    if (out == NULL)
        goto done;
    outp = (unsigned char *)PyBytes_AS_STRING(out);

    for (i = 0; i < nsamples; i++) {
        uint16_t idx = (uint16_t)((int)samples[i] + 32768);
        outp[i] = g_pcm16_to_ulaw[idx];
    }

done:
    PyMem_Free(owned);
    PyBuffer_Release(&view);
    return out;
}

static PyObject *
PyRtpUtils_ulaw2linear(PyObject *self, PyObject *args)
{
    PyObject *obj;
    PyObject *out = NULL;
    Py_buffer view;
    int16_t *out_samples = NULL;
    Py_ssize_t i;
    (void)self;

    ensure_tables();

    if (!PyArg_ParseTuple(args, "O:ulaw2linear", &obj))
        return NULL;

    if (PyLong_Check(obj)) {
        long long ulaw = PyLong_AsLongLong(obj);
        if (PyErr_Occurred())
            return NULL;
        return PyLong_FromLong((long)g_ulaw_to_pcm[(uint8_t)(ulaw & 0xFFLL)]);
    }

    memset(&view, 0, sizeof(view));
    if (PyObject_GetBuffer(obj, &view, PyBUF_CONTIG_RO) != 0)
        return NULL;

    if (view.itemsize != 1) {
        PyErr_SetString(PyExc_TypeError, "ulaw2linear expects bytes-like input");
        goto done;
    }

    if (view.len > 0) {
        const unsigned char *in = (const unsigned char *)view.buf;
        out_samples = PyMem_Malloc((size_t)view.len * sizeof(*out_samples));
        if (out_samples == NULL) {
            PyErr_NoMemory();
            goto done;
        }
        for (i = 0; i < view.len; i++)
            out_samples[i] = g_ulaw_to_pcm[in[i]];
    }

    out = array_h_from_pcm16(out_samples, view.len);

done:
    PyMem_Free(out_samples);
    PyBuffer_Release(&view);
    return out;
}

static PyMethodDef PyRtpUtils_methods[] = {
    {"resample_linear", (PyCFunction)PyRtpUtils_resample_linear, METH_VARARGS | METH_STATIC, NULL},
    {"linear2ulaw", (PyCFunction)PyRtpUtils_linear2ulaw, METH_VARARGS | METH_STATIC, NULL},
    {"ulaw2linear", (PyCFunction)PyRtpUtils_ulaw2linear, METH_VARARGS | METH_STATIC, NULL},
    {NULL}
};

static PyMethodDef RtpUtils_module_methods[] = {
    {"resample_linear", (PyCFunction)PyRtpUtils_resample_linear, METH_VARARGS, NULL},
    {"linear2ulaw", (PyCFunction)PyRtpUtils_linear2ulaw, METH_VARARGS, NULL},
    {"ulaw2linear", (PyCFunction)PyRtpUtils_ulaw2linear, METH_VARARGS, NULL},
    {NULL}
};

static PyTypeObject PyRtpUtilsType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME ".RtpUtils",
    .tp_basicsize = sizeof(PyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_methods = PyRtpUtils_methods,
};

static struct PyModuleDef RtpUtils_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = "High-performance RTP audio utility helpers.",
    .m_methods = RtpUtils_module_methods,
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit_RtpUtils(void)
{
    PyObject *module = NULL;
    PyObject *array_mod = NULL;

    if (PyType_Ready(&PyRtpUtilsType) < 0)
        return NULL;

    module = PyModule_Create(&RtpUtils_module);
    if (module == NULL)
        return NULL;

    array_mod = PyImport_ImportModule("array");
    if (array_mod == NULL) {
        Py_DECREF(module);
        return NULL;
    }

    g_array_ctor = PyObject_GetAttrString(array_mod, "array");
    Py_DECREF(array_mod);
    if (g_array_ctor == NULL) {
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&PyRtpUtilsType);
    PyModule_AddObject(module, "RtpUtils", (PyObject *)&PyRtpUtilsType);

    return module;
}
