#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pycore_frame.h"
#include "internal/pycore_pystate.h"
static _PyFrameEvalFunction previous;
static unsigned long calls;
static PyObject*hook(PyThreadState*t,_PyInterpreterFrame*f,int flag){calls++;return previous(t,f,flag);}
static PyObject*install(PyObject*s,PyObject*a){
 PyInterpreterState*i=PyThreadState_Get()->interp;
 if(previous){PyErr_SetString(PyExc_RuntimeError,"already installed");return NULL;}
 previous=_PyInterpreterState_GetEvalFrameFunc(i);calls=0;
 _PyInterpreterState_SetEvalFrameFunc(i,hook);Py_RETURN_NONE;
}
static PyObject*restore(PyObject*s,PyObject*a){
 PyInterpreterState*i=PyThreadState_Get()->interp;
 if(previous&&_PyInterpreterState_GetEvalFrameFunc(i)==hook)_PyInterpreterState_SetEvalFrameFunc(i,previous);
 previous=NULL;return PyLong_FromUnsignedLong(calls);
}
static PyMethodDef methods[]={{"install",install,METH_NOARGS,NULL},{"restore",restore,METH_NOARGS,NULL},{NULL}};
static struct PyModuleDef mod={PyModuleDef_HEAD_INIT,"_hook_probe",NULL,-1,methods};
PyMODINIT_FUNC PyInit__hook_probe(void){return PyModule_Create(&mod);}
