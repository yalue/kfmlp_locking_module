// This file defines a command-line utility for testing experimental ioctls
// added to /dev/kfd.  See the README for more information, or run the tool
// with the -h argument for detailed usage instructions.
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

static void PrintUsage(const char *name) {
  printf("Usage: %s <options>\n\n", name);
  printf("Options may be one or more of the following:\n"
    "  -h: Prints these instructions.\n"
    "  -e <PID>: Evicts the queues of the process with the given PID.\n"
    "     Can be repeated for multiple PIDs.\n"
    "  -r <PID>: Restores the queues of the process with the given PID.\n"
    "     Can be repeated for multiple PIDs.\n"
    "");
}

// Returns an integer at the argument after index in argv. Exits if the integer
// is invalid. Takes the index before the expected int value in order to print
// better error messages.
static int ParseIntArg(int argc, char **argv, int index) {
  char *tmp = NULL;
  int to_return = 0;
  if ((index + 1) >= argc) {
    printf("Argument %s needs a value.\n", argv[index]);
    PrintUsage(argv[0]);
  }
  to_return = strtol(argv[index + 1], &tmp, 10);
  // Make sure that, if tmp is a null character, that the argument wasn't
  // simply a string with no content.
  if ((*tmp != 0) || (argv[index + 1][0] == 0)) {
    printf("Invalid number given to argument %s: %s\n", argv[index],
      argv[index + 1]);
    PrintUsage(argv[0]);
  }
  return to_return;
}

// Parses the argument list and carries out the requested action. Returns 0 on
// error.
static int ParseArgumentsAndRun(int argc, char **argv) {
  int pid, fd, ok, i;
  ok = 1;
  fd = GetKFDFD();
  if (fd < 0) return 0;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      ok = 0;
      break;
    }
    if (strcmp(argv[i], "-e") == 0) {
      pid = ParseIntArg(argc, argv, i);
      i++;
      if (!EvictQueues(fd, pid)) {
        ok = 0;
        break;
      }
      continue;
    }
    if (strcmp(argv[i], "-r") == 0) {
      pid = ParseIntArg(argc, argv, i);
      i++;
      if (!RestoreQueues(fd, pid)) {
        ok = 0;
        break;
      }
      continue;
    }
  }
  close(fd);
  return ok;
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    printf("Invalid or missing arguments.\n");
    if (argc < 1) {
      PrintUsage("<program name>");
    } else {
      PrintUsage(argv[0]);
    }
    return 1;
  }
  if (!ParseArgumentsAndRun(argc, argv)) {
    printf("One or more errors occurred.\n");
    return 1;
  }
  printf("All operations completed OK.\n");
  return 0;
}
