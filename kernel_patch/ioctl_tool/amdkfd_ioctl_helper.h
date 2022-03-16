// This file defines a C library for working with the ioctls we've added to
// the /dev/kfd device. It must be used with a patched version of the kernel;
// see the patch contained in the parent directory.
#ifndef AMDKFD_IOCTL_HELPER_H
#define AMDKFD_IOCTL_HELPER_H
#include <asm/ioctl.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The arg struct to pass to the evict-queues ioctl. Must exactly match the
// definition from include/uapi/linux/kfd_ioctl.h in the patched kernel.
typedef struct {
  uint32_t pid;
  uint32_t pad;
} EvictQueuesArgs;

// The arg struct to pass to the restore-queues ioctl. Must exactly match the
// definition from include/uapi/linux/kfd_ioctl.h in the patched kernel.
typedef struct {
  uint32_t pid;
  uint32_t pad;
} RestoreQueuesArgs;

// These numbers and structs must exactly match the ones in the patched kernel
// source tree at include/uapi/linux/kfd_ioctl.h.
#define AMDKFD_IOC_EVICT_PROCESS_QUEUES _IOW('K', 0x20, EvictQueuesArgs)
#define AMDKFD_IOC_RESTORE_PROCESS_QUEUES _IOW('K', 0x21, RestoreQueuesArgs)

// Opens and returns a file descriptor to /dev/kfd. Returns a negative value on
// error. It is the caller's responsibility to close the returned fd when no
// longer needed.
int GetKFDFD(void);

// Evicts all GPU queues for the process with the given PID. Requires an fd to
// /dev/kfd (e.g. the value returned by GetKFDFD). Returns 0 on error.
int EvictQueues(int fd, int pid);

// Restores queues for the process with the given PID, reversing the operation
// made by EvictQueues.
int RestoreQueues(int fd, int pid);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // AMDKFD_IOCTL_HELPER_H

