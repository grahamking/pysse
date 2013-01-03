/*
 * Python extension wrapper part of pysse.
 */

/*
 * Copyright 2012 Graham King <graham@gkgk.org>
 *
 * What should license be: LGPL? BSD?
 * It's a library.
 */

#include <Python.h>

int start(const char *address, int port);

static PyObject *pysse_start(PyObject *self, PyObject *args) {

    const char *address;
    int port;

    printf("Start\n");

    if (!PyArg_ParseTuple(args, "si", &address, &port)) {
        return NULL;
    }

    int pipefd = start(address, port);

    return Py_BuildValue("i", pipefd);
}

static PyMethodDef HelloMethods[] = {
    {"start", pysse_start, METH_VARARGS, "Start server-sent event server on given port"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initpysse(void) {
    (void) Py_InitModule("pysse", HelloMethods);
}
