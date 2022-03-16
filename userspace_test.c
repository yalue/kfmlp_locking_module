// This file simply serves to test the interface for the locking module. I
// won't include it in the Makefile, just build it using:
// gcc -O3 -Wall -Werror -o userspace_test ./userspace_test.c
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include "gpu_locking_module.h"

// The number of threads to create to contend for the lock.
#define THREADS_TO_CREATE (10)

static int GetTID(void) {
  pid_t tid = syscall(SYS_gettid);
  return (int) tid;
}

// Acquires the lock with the given ID. Returns 0 on error.
static int AcquireLock(int fd, int lock) {
  GPULockArgs args;
  int result;
  args.lock_id = lock;
  result = ioctl(fd, GPU_LOCK_ACQUIRE_IOC, &args);
  if (result != 0) {
    printf("Acquire ioctl returned an error: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Releases the lock for the given lock. Returns 0 on error. Returns an error
// if the lock for the given lock isn't held by the caller.
static int ReleaseLock(int fd, int lock) {
  GPULockArgs args;
  int result;
  args.lock_id = lock;
  result = ioctl(fd, GPU_LOCK_RELEASE_IOC, &args);
  if (result != 0) {
    printf("Release ioctl returned an error: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Sets the deadline associated with the current process. Returns 0 on error.
static int SetDeadline(int fd, uint64_t deadline) {
  SetDeadlineArgs args;
  int result;
  args.deadline = deadline;
  result = ioctl(fd, GPU_LOCK_SET_DEADLINE_IOC, &args);
  if (result != 0) {
    printf("Set deadline ioctl returned an error: %s\n", strerror(errno));
    return 0;
  }
  return 1;
}

// Tests acquiring and releasing the lock. Returns 0 if an error occurs at any
// step of the process.
static int TestIoctl(int fd) {
  char my_info[256];
  memset(my_info, 0, sizeof(my_info));
  snprintf(my_info, sizeof(my_info) - 1, "PID %d, TID %d", (int) getpid(),
    GetTID());
  printf("%s attempting to acquire lock.\n", my_info);
  if (!AcquireLock(fd, 0)) {
    printf("%s failed acquiring lock.\n", my_info);
    return 0;
  }
  printf("%s acquired lock successfully. Sleeping a bit.\n", my_info);
  sleep(1);
  printf("%s attempting to set deadline.\n", my_info);
  if (!SetDeadline(fd, 100)) {
    printf("%s failed setting deadline.\n", my_info);
    return 0;
  }
  printf("%s acquiring lock again.\n", my_info);
  if (!AcquireLock(fd, 0)) {
    printf("%s failed acquiring lock a 2nd time.\n", my_info);
    return 0;
  }
  printf("%s releasing lock 1st time.\n", my_info);
  if (!ReleaseLock(fd, 0)) {
    printf("%s failed releasing lock 1st time.\n", my_info);
    return 0;
  }
  printf("%s unsetting deadline.\n", my_info);
  if (!SetDeadline(fd, 0)) {
    printf("%s failed unsetting deadline.\n", my_info);
    return 0;
  }
  if (!ReleaseLock(fd, 0)) {
    printf("%s failed releasing lock a 2nd time.\n", my_info);
    return 0;
  }
  printf("%s released lock 2nd time successfully.\n", my_info);
  return 1;
}

int main(int argc, char **argv) {
  int fd;
  fd = open("/dev/gpu_locking_module", O_RDWR);
  if (fd < 0) {
    printf("Error opening /dev/gpu_locking_module: %s\n", strerror(errno));
    return 1;
  }
  printf("Tester PID %d, TID %d\n", (int) getpid(), GetTID());
  if (TestIoctl(fd)) {
    printf("The test succeeded!\n");
  } else {
    printf("Some tests failed.\n");
  }
  close(fd);
  return 0;
}
