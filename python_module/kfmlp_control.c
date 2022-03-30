#define PY_SSIZE_T_CLEAN
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <Python.h>
#include "../kfmlp_module.h"

static int module_handle = -1;

static PyObject* ResetModuleHandle(PyObject *self, PyObject *args) {
  const char *path = "/dev/" DEVICE_NAME;
  module_handle = open(path, O_RDWR);
  if (module_handle < 0) {
    PyErr_Format(PyExc_OSError, "Failed to open %s: %s", path,
      strerror(errno));
    return NULL;
  }
  Py_RETURN_NONE;
}

// Returns 0 if the given ioctl result isn't successful.
static int CheckIoctlResult(int result) {
  if (result != 0) {
    PyErr_Format(PyExc_OSError, "ioctl failed (returned %d): %s",
      result, strerror(errno));
    return 0;
  }
  return 1;
}

static PyObject* WaitForTSRelease(PyObject *self, PyObject *args) {
  int result;
  result = ioctl(module_handle, KFMLP_WAIT_FOR_TS_RELEASE_IOC);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* ReleaseTS(PyObject *self, PyObject *args) {
  int result = ioctl(module_handle, KFMLP_RELEASE_TS_IOC);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* GetTSWaiterCount(PyObject *self, PyObject *args) {
  GetTSWaiterCountArgs arg;
  int result = ioctl(module_handle, KFMLP_GET_TS_WAITER_COUNT_IOC,
    &arg);
  if (!CheckIoctlResult(result)) return NULL;
  return Py_BuildValue("I", (unsigned int) arg.count);
}

static PyObject* SetNewK(PyObject *self, PyObject *args) {
  SetKFMLPLockKArgs arg;
  unsigned int k;
  int result;
  if (!PyArg_ParseTuple(args, "I", &k)) return NULL;
  arg.k = k;
  result = ioctl(module_handle, KFMLP_SET_K_IOC, &arg);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* AcquireLock(PyObject *self, PyObject *args) {
  AcquireKFMLPLockArgs arg;
  int result = ioctl(module_handle, KFMLP_LOCK_ACQUIRE_IOC, &arg);
  if (!CheckIoctlResult(result)) return NULL;
  return Py_BuildValue("I", (unsigned int) arg.lock_slot);
}

static PyObject* ReleaseLock(PyObject *self, PyObject *args) {
  int result = ioctl(module_handle, KFMLP_LOCK_RELEASE_IOC);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* StartSchedFIFO(PyObject *self, PyObject *args) {
  int result = ioctl(module_handle, KFMLP_START_SCHED_FIFO_IOC);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* EndSchedFIFO(PyObject *self, PyObject *args) {
  int result = ioctl(module_handle, KFMLP_END_SCHED_FIFO_IOC);
  if (!CheckIoctlResult(result)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* GetCurrentK(PyObject *self, PyObject *args) {
  SetKFMLPLockKArgs arg;
  int result = ioctl(module_handle, KFMLP_GET_K_IOC, &arg);
  if (!CheckIoctlResult(result)) return NULL;
  return Py_BuildValue("I", (unsigned int) arg.k);
}

static PyMethodDef kfmlp_control_methods[] = {
  {
    "reset_module_handle",
    ResetModuleHandle,
    METH_NOARGS,
    "Opens (or reopens) the handle to the kernel device. No args. Must be "
       "called before using any other function in this library.",
  },
  {
    "wait_for_ts_release",
    WaitForTSRelease,
    METH_NOARGS,
    "Sleeps until another task calls release_ts.",
  },
  {
    "release_ts",
    ReleaseTS,
    METH_NOARGS,
    "Releases all tasks waiting on wait_for_ts_release.",
  },
  {
    "get_ts_waiter_count",
    GetTSWaiterCount,
    METH_NOARGS,
    "Returns the number of tasks waiting for release.",
  },
  {
    "set_k",
    SetNewK,
    METH_VARARGS,
    "Sets a new 'k' value for the KFMLP lock. Requires one arg: k.",
  },
  {
    "acquire_lock",
    AcquireLock,
    METH_NOARGS,
    "Acquires the KFMLP lock; returns the integer slot we obtained.",
  },
  {
    "release_lock",
    ReleaseLock,
    METH_NOARGS,
    "Releases the KFMLP lock if we currently hold it.",
  },
  {
    "start_sched_fifo",
    StartSchedFIFO,
    METH_NOARGS,
    "Puts the caller into SCHED_FIFO mode.",
  },
  {
    "end_sched_fifo",
    EndSchedFIFO,
    METH_NOARGS,
    "Makes a SCHED_FIFO caller into SCHED_NORMAL.",
  },
  {
    "get_k",
    GetCurrentK,
    METH_NOARGS,
    "Returns the current 'k' setting for the KFMLP lock.",
  },
  {NULL, NULL, 0, NULL},
};

static struct PyModuleDef kfmlp_control_module = {
  PyModuleDef_HEAD_INIT,
  "kfmlp_control",
  "A python wrapper around the KFMLP kernel module API.",
  -1,
  kfmlp_control_methods,
  NULL,
  NULL,
  NULL,
  NULL,
};

PyMODINIT_FUNC PyInit_kfmlp_control(void) {
  module_handle = -1;
  return PyModule_Create(&kfmlp_control_module);
}

