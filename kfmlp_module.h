// This file contains structs and definitions used when interfacing with the
// GPU locking module.

#ifndef KFMLP_MODULE_H
#define KFMLP_MODULE_H
#include <linux/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

// The maximum 'k' value supported by the module.
#define KFMLP_MAX_K (32)

// The name of the character device.
#define DEVICE_NAME "kfmlp_module"

// A pointer to this struct is used as an argument to KFMLP_LOCK_ACQUIRE_IOC.
// If the lock is successully acquired, the "slot" field will be set to the
// lock slot (between 0 and k-1) the caller is in. The lock_slot must be
// ignored if the ioctl to acquire the lock fails.
typedef struct {
  uint32_t lock_slot;
} AcquireKFMLPLockArgs;

// A pointer to this struct is used as an argument to KFMLP_SET_K_IOC. The 'k'
// field will be used to set the maximum number of concurrent lock holders.
typedef struct {
  uint32_t k;
} SetKFMLPLockKArgs;

// A pointer to this struct is used as an argument to
// KFMLP_GET_TS_WAITER_COUNT_IOC. The 'count' field is filled in with the
// number of waiters.
typedef struct {
  uint32_t count;
} GetTSWaiterCountArgs;

// This must be called once, by any process, prior to any attempts to acquire
// the lock. Sets the lock's current 'k' value. Will fail with EINVAL if the
// value of k is too large, or if any process currently holds the lock.
#define KFMLP_SET_K_IOC _IOW('h', 0xaa, SetKFMLPLockKArgs)

// Send this ioctl to acquire the lock. Fails with EINTR if interrupted before
// the lock is acquired.
#define KFMLP_LOCK_ACQUIRE_IOC _IOR('h', 0xab, AcquireKFMLPLockArgs)

// Send this ioctl to release the lock. Fails with EINVAL if the lock wasn't
// held.
#define KFMLP_LOCK_RELEASE_IOC _IO('h', 0xac)

// Send this ioctl to wait for some other process to release the "task system."
// Analogous to wait_for_ts_release in LITMUS^RT. May fail with EINTR if
// interrupted prior to being released normally.
#define KFMLP_WAIT_FOR_TS_RELEASE_IOC _IO('h', 0xad)

// Send this ioctl to release all tasks that are currently waiting with
// KFMLP_WAIT_FOR_TS_RELEASE.
#define KFMLP_RELEASE_TS_IOC _IO('h', 0xae)

// This ioctl obtains the number of processes that are currently waiting after
// calling KFMLP_WAIT_FOR_TS_RELEASE.
#define KFMLP_GET_TS_WAITER_COUNT_IOC _IOR('h', 0xaf, GetTSWaiterCountArgs)

// This ioctl makes the calling process SCHED_FIFO, with low priority. The
// process must be SCHED_NORMAL, otherwise this will return EINVAL. Basically
// is a shortcut to bypass needing to have CAP_SYS_NICE.
#define KFMLP_START_SCHED_FIFO_IOC _IO('h', 0xb0)

// This ioctl makes the calling process SCHED_NORMAL. Returns -EINVAL and does
// nothing if the process is not SCHED_FIFO or SCHED_NORMAL prior to calling
// this. (SCHED_NORMAL is OK simply so this can be unconditionally called when
// cleaning up a task.)
#define KFMLP_END_SCHED_FIFO_IOC _IO('h', 0xb1)

// Returns the module's current 'k' value. Takes the same args as the SET_K_IOC
// but fills in the struct's 'k' field.
#define KFMLP_GET_K_IOC _IOR('h', 0xb2, SetKFMLPLockKArgs)

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // KFMLP_MODULE_H

