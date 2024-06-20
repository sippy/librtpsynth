#include <assert.h>
#include <stdbool.h>

#include <Python.h>

#define MODULE_BASENAME _rtpsynth

#define CONCATENATE_DETAIL(x, y) x##y
#define CONCATENATE(x, y) CONCATENATE_DETAIL(x, y)

#if !defined(DEBUG_MOD)
#define MODULE_NAME MODULE_BASENAME
#else
#define MODULE_NAME CONCATENATE(MODULE_BASENAME, _debug)
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define MODULE_NAME_STR TOSTRING(MODULE_NAME)
#define PY_INIT_FUNC CONCATENATE(PyInit_, MODULE_NAME)

typedef struct {
    PyObject_HEAD
} PyRtpSynth;

static int PyRtpSynth_init(PyRtpSynth* self, PyObject* args, PyObject* kwds) {

    return 0;
}

// The __del__ method for PyRtpSynth objects
static void PyRtpSynth_dealloc(PyRtpSynth* self) {
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyMethodDef PyRtpSynth_methods[] = {
    {NULL}  // Sentinel
};

static PyTypeObject PyRtpSynthType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = MODULE_NAME_STR "." MODULE_NAME_STR,
    .tp_doc = "[TODO]",
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
    .m_name = MODULE_NAME_STR,
    .m_doc = "Python interface XXX.",
    .m_size = -1,
};

// Module initialization function
PyMODINIT_FUNC PY_INIT_FUNC(void) {
    PyObject* module;
    if (PyType_Ready(&PyRtpSynthType) < 0)
        return NULL;

    module = PyModule_Create(&RtpSynth_module);
    if (module == NULL)
        return NULL;

    Py_INCREF(&PyRtpSynthType);
    PyModule_AddObject(module, MODULE_NAME_STR, (PyObject*)&PyRtpSynthType);

    return module;
}
