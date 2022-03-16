// This file defines a simple HIP program that does a counter-spin kernel and
// attempts to evict its own queues from the GPU.
//
// Compile by running `make hip_tester`.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <hip/hip_runtime.h>
#include "amdkfd_ioctl_helper.h"

// The number of blocks with which to run the kernel.
#define BLOCK_COUNT (240)

// The number of threads per block in the kernel.
#define THREAD_COUNT (512)

// The number of iterations for which to spin.
#define SPIN_ITERATIONS (10 * 1000 * 1000)

// Takes a hipError_t value and exits if it isn't hipSuccess.
#define CheckHIPError(val) (InternalHIPErrorCheck((val), #val, __FILE__, __LINE__))

// Intended to be used by the CheckHIPError macro.
static void InternalHIPErrorCheck(hipError_t result, const char *fn,
    const char *file, int line) {
  if (result == hipSuccess) return;
  printf("HIP error %d: %s. In %s, line %d (%s)\n", (int) result,
    hipGetErrorString(result), file, line, fn);
  exit(1);
}

// Returns a floating-point number of seconds, used for timing.
static double CurrentSeconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    printf("Error getting time.\n");
    exit(1);
  }
  return ((double) ts.tv_sec) + (((double) ts.tv_nsec) / 1e9);
}

// Prints the min, max, and mean block times to stdout. Requires a list of
// block start and end times.
static void PrintBlockTimesStats(uint64_t *times, int block_count) {
  double min, max, sum, duration;
  int i;
  min = 1e30;
  max = -1e30;
  sum = 0;
  for (i = 0; i < block_count; i++) {
    duration = (double) (times[i * 2 + 1] - times[i * 2]);
    if (duration < 0) {
      printf("Warning: Block %d shows negative duration: %f\n", i, duration);
      duration = 0;
    }
    if (duration < min) min = duration;
    if (duration > max) max = duration;
    sum += duration;
  }
  printf("For %d blocks: min = %f, max = %f, mean = %f\n", block_count, min,
    max, sum / ((double) block_count));
}

// The kernel for spinning a certain number of iterations. The dummy argument
// should be NULL, it exists to prevent optimizing out the loop.
__global__ void CounterSpinKernel(uint64_t max_iterations, uint64_t *dummy,
  uint64_t *block_times) {
  uint64_t i, accumulator;
  uint64_t start_clock = clock64();
  // This requires that the block start times be initialized to the max uint64
  // value.
  if (start_clock < block_times[hipBlockIdx_x * 2]) {
    block_times[hipBlockIdx_x * 2] = start_clock;
  }
  accumulator = 0;
  for (i = 0; i < max_iterations; i++) {
    accumulator += i % hipBlockIdx_x;
  }
  if (dummy) *dummy = accumulator;
  block_times[hipBlockIdx_x * 2 + 1] = clock64();
}

static void PrintUsage(const char *name) {
  printf("Usage: %s <seconds to evict queues for>\n\n"
    "If the number of seconds is negative, then queues will not be evicted\n"
    "at all. If it is zero, queues will be evicted and immediately restored.\n"
    "If positive, then the program will sleep() for the specified number of\n"
    "seconds between evicting and restoring queues.\n", name);
}

int main(int argc, char **argv) {
  uint64_t *device_block_times = NULL;
  uint64_t *host_block_times = NULL;
  double start_time, end_time;
  int evict_seconds = -1;
  int pre_evict_seconds = 3;
  int kfd_fd, pid;
  size_t size;

  if (argc != 2) {
    PrintUsage(argv[0]);
    exit(1);
  }
  kfd_fd = GetKFDFD();
  pid = getpid();
  if (kfd_fd < 0) return 1;
  evict_seconds = atoi(argv[1]);
  printf("Initializing and allocating memory.\n");
  CheckHIPError(hipSetDevice(0));
  size = 2L * BLOCK_COUNT * sizeof(uint64_t);
  CheckHIPError(hipHostMalloc(&host_block_times, size));
  CheckHIPError(hipMalloc(&device_block_times, size));
  CheckHIPError(hipMemset(device_block_times, 0xff, size));
  CheckHIPError(hipDeviceSynchronize());

  printf("Warming up kernel.\n");
  hipLaunchKernelGGL(CounterSpinKernel, BLOCK_COUNT, THREAD_COUNT, 0, 0,
    SPIN_ITERATIONS, (uint64_t *) NULL, device_block_times);
  CheckHIPError(hipDeviceSynchronize());
  // Reset the device-side block times so that the idiom for recording the
  // block start times works.
  CheckHIPError(hipMemset(device_block_times, 0xff, size));
  CheckHIPError(hipDeviceSynchronize());
  printf("Warm-up done OK.\n");

  // Now we'll run the kernel and do the eviction if needed.
  hipLaunchKernelGGL(CounterSpinKernel, BLOCK_COUNT, THREAD_COUNT, 0, 0,
    SPIN_ITERATIONS, (uint64_t *) NULL, device_block_times);
  if (evict_seconds >= 0) {
    printf("Evicting queues for %d s, after letting them run for %d s.\n",
      evict_seconds, pre_evict_seconds);
    sleep(pre_evict_seconds);
    if (!EvictQueues(kfd_fd, pid)) {
      printf("Failed evicting queues.\n");
      exit(1);
    }
    printf("Queues evicted.\n");
    if (evict_seconds > 0) {
      sleep(evict_seconds);
    }
    if (!RestoreQueues(kfd_fd, pid)) {
      printf("Failed restoring queues.\n");
      exit(1);
    }
    printf("Queues restored.\n");
  } else {
    printf("Kernel launched. Not evicting queues.\n");
  }
  start_time = CurrentSeconds();
  CheckHIPError(hipDeviceSynchronize());
  end_time = CurrentSeconds();
  printf("Kernel completed OK. Sync took %f s.\n", end_time - start_time);


  CheckHIPError(hipMemcpy(host_block_times, device_block_times, size,
    hipMemcpyDeviceToHost));
  CheckHIPError(hipDeviceSynchronize());
  PrintBlockTimesStats(host_block_times, BLOCK_COUNT);
  hipFree(device_block_times);
  hipHostFree(host_block_times);
  printf("All tests completed OK.\n");
  return 0;
}

