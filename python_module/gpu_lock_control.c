// This file contains definitions for a C python module for interacting with
// the GPU-locking module.  Basically, it wraps the ioctls to
// /dev/gpu_locking_module.  If the kernel module isn't loaded, then this will
// simply print a warning on import, and all functions will silently do nothing
// and return None.

#define PY_SSIZE_T_CLEAN
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <Python.h>
#include "../gpu_locking_module.h"

// A file descriptor for the kernel module's chardev. Opened once during
// initialization. Must be -1 if the chardev isn't available.
static int module_fd = -1;

// Holds basic information about the number of times various ioctls are called
// and how long they take.
static struct {
  int set_deadline_count;
  double set_deadline_time;
} ioctl_stats;

// Returns a time in seconds, used for timing underlying ioctls.
static double CurrentSeconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    printf("Error getting time.\n");
    exit(1);
  }
  return ((double) ts.tv_sec) + (((double) ts.tv_nsec) / 1e9);
}

// Does what it says on the box. Saves a bit of typing.
static PyObject* IncrementAndReturnNone(void) {
  Py_INCREF(Py_None);
  return Py_None;
}

// Collects the contents of the global ioctl_stats info into a python object.
static PyObject* GetIoctlStats(void) {
  return Py_BuildValue("{s:i, s:d}",
    "set_deadline_count", ioctl_stats.set_deadline_count,
    "set_deadline_time", ioctl_stats.set_deadline_time);
}

// Implementation of the wrapper for the set-deadline ioctl.
static PyObject* gpu_lock_control_set_deadline(PyObject *self,
    PyObject *args) {
  SetDeadlineArgs ioctl_args;
  unsigned long long deadline;
  double start_time;
  int result;
  if (module_fd < 0) return IncrementAndReturnNone();
  if (!PyArg_ParseTuple(args, "K", &deadline)) return NULL;

  ioctl_args.deadline = deadline;
  start_time = CurrentSeconds();
  result = ioctl(module_fd, GPU_LOCK_SET_DEADLINE_IOC, &ioctl_args);
  ioctl_stats.set_deadline_time += CurrentSeconds() - start_time;
  ioctl_stats.set_deadline_count++;
  if (result != 0) {
    PyErr_Format(PyExc_OSError, "Set GPU-lock deadline ioctl failed: %s",
      strerror(errno));
    return NULL;
  }

  return IncrementAndReturnNone();
}

// Implementation of the wrapper for the ACQUIRE ioctl.
static PyObject* gpu_lock_control_acquire_lock(PyObject *self,
    PyObject *args) {
  GPULockArgs ioctl_args;
  unsigned int lock_id;
  int result;
  if (module_fd < 0) return IncrementAndReturnNone();
  if (!PyArg_ParseTuple(args, "I", &lock_id)) return NULL;
  ioctl_args.lock_id = lock_id;
  result = ioctl(module_fd, GPU_LOCK_ACQUIRE_IOC, &ioctl_args);
  if (result != 0) {
    PyErr_Format(PyExc_OSError, "Ioctl to acquire lock %u failed: %s", lock_id,
      strerror(errno));
    return NULL;
  }
  return IncrementAndReturnNone();
}

// Implementation of the wrapper for the RELEASE ioctl.
static PyObject* gpu_lock_control_release_lock(PyObject *self,
    PyObject *args) {
  GPULockArgs ioctl_args;
  unsigned int lock_id;
  int result;
  if (module_fd < 0) return IncrementAndReturnNone();
  if (!PyArg_ParseTuple(args, "I", &lock_id)) return NULL;
  ioctl_args.lock_id = lock_id;
  result = ioctl(module_fd, GPU_LOCK_RELEASE_IOC, &ioctl_args);
  if (result != 0) {
    PyErr_Format(PyExc_OSError, "Ioctl to release lock %u failed: %s", lock_id,
      strerror(errno));
    return NULL;
  }
  return IncrementAndReturnNone();
}

static PyObject* gpu_lock_control_barrier_sync(PyObject *self,
    PyObject *args) {
  BarrierSyncArgs ioctl_args;
  int waiter_count;
  int result;
  if (module_fd < 0) return IncrementAndReturnNone();
  if (!PyArg_ParseTuple(args, "i", &waiter_count)) return NULL;
  ioctl_args.count = waiter_count;
  result = ioctl(module_fd, GPU_LOCK_BARRIER_SYNC_IOC, &ioctl_args);
  if (result != 0) {
    PyErr_Format(PyExc_OSError, "Iocl to barrier-wait failed: %s",
      strerror(errno));
    return NULL;
  }
  return IncrementAndReturnNone();
}

static PyObject* gpu_lock_control_get_ioctl_stats(PyObject *self,
    PyObject *args) {
  return GetIoctlStats();
}

static PyMethodDef gpu_lock_control_methods[] = {
  {
    "set_deadline",
    gpu_lock_control_set_deadline,
    METH_VARARGS,
    "Sets the relative deadine, in nanoseconds, for all subsequent GPU kernels"
      " launched by this process. Requires a single argument: an integer "
      "number of nanoseconds. Passing an argument of 0 *removes* the current "
      "process' deadline, reverting it to best-effort FIFO.",
  },
  {
    "acquire_lock",
    gpu_lock_control_acquire_lock,
    METH_VARARGS,
    "Intended for testing only! The HIP runtime should already be instrumented"
      " to acquire and release locks properly--using this in conjunction with "
      "HIP will very likely cause hangs. Acquires a lock with the specified "
      "ID. Requires a single argument: a positive integer specifying the ID of"
      " the lock to acquire.",
  },
  {
    "release_lock",
    gpu_lock_control_release_lock,
    METH_VARARGS,
    "Intended for testing only! The same notes apply here as to acquire_lock. "
      "Releases the lock with the specified ID. Requires a single argument: a "
      "positive integer specifying the ID of the lock to release.",
  },
  {
    "barrier_sync",
    gpu_lock_control_barrier_sync,
    METH_VARARGS,
    "Wait for barrier-synchronization with a number of other processes. Puts "
      "the caller to sleep until all other processes have reached the "
      "\"barrier\". Requires the number of processes to wait for. If other "
      "processes are already waiting, and the given number is positive and not"
      " equal to the other process' numbers, then this raises an error and all"
      " waiters will wake up.",
  },
  {
    "get_ioctl_stats",
    gpu_lock_control_get_ioctl_stats,
    METH_NOARGS,
    "Returns stats about underlying ioctls, including the number of times that"
      " each is called, and the total time required. May not include "
      "information about each ioctl. Returns a dict.",
  },
  {NULL, NULL, 0, NULL},
};

static struct PyModuleDef gpu_lock_control_module = {
    PyModuleDef_HEAD_INIT,
    "gpu_lock_control",
    "Provides functions for interacting with the GPU-locking module. Unless "
      "otherwise specified, all functions will return None on success and "
      "raise an exception on error. If the locking-module's chardev isn't "
      "present, importing this module will print a warning, and the functions "
      "will silently do nothing. ",
    -1,
    gpu_lock_control_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_gpu_lock_control(void) {
  module_fd = open("/dev/gpu_locking_module", O_RDWR);
  if (module_fd < 0) {
    module_fd = -1;
    PyErr_WarnFormat(PyExc_Warning, 1, "Couldn't open /dev/gpu_locking_module:"
      " %s. Won't use locking.", strerror(errno));
  }
  return PyModule_Create(&gpu_lock_control_module);
}
