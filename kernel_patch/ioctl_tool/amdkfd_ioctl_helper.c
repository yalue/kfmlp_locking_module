// This implements the functions defined in amdkfd_ioctl_helper.h.
#include <asm/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "amdkfd_ioctl_helper.h"

int GetKFDFD(void) {
  int fd = open("/dev/kfd", O_RDWR);
  if (fd < 0) {
    printf("Error opening /dev/kfd: %s\n", strerror(errno));
    return -1;
  }
  return fd;
}

int EvictQueues(int fd, int pid) {
  EvictQueuesArgs args;
  int result;
  memset(&args, 0, sizeof(args));
  args.pid = pid;
  result = ioctl(fd, AMDKFD_IOC_EVICT_PROCESS_QUEUES, &args);
  if (result != 0) {
    printf("Evicting queues for %d failed: %s\n", pid, strerror(errno));
    return 0;
  }
  return 1;
}

int RestoreQueues(int fd, int pid) {
  RestoreQueuesArgs args;
  int result;
  memset(&args, 0, sizeof(args));
  args.pid = pid;
  result = ioctl(fd, AMDKFD_IOC_RESTORE_PROCESS_QUEUES, &args);
  if (result != 0) {
    printf("Restoring queues for %d failed: %s\n", pid, strerror(errno));
    return 0;
  }
  return 1;
}

