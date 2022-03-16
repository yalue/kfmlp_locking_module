// Contains the top-level module definition for the GPU-locking module. This
// module supports a coarse implementation of EDF enforcement on AMD GPUs,
// optionally with preemptivity.  It is mostly based on my simpler
// priority_locking_module.  It requires a modified amdkfd driver in order to
// have access to my additional kfd_evict_process_queues_by_pid and
// kfd_restore_process_queues_by_pid functions.
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "binheap.h"
#include "gpu_locking_module.h"

// The name of the character device.
#define DEVICE_NAME "gpu_locking_module"

// Prepend this to all log messages.
#define PRINTK_TAG "GPU locking: "

// The number, class and device references set during initialization, and
// needed when destroying the chardev.
static int major_number;
// Keeps track of whether we've added the DEVMODE=0666 environment variable to
// the device class.
static int devmode_var_added = 0;
static struct class *gpu_lock_device_class = NULL;
static struct device *gpu_lock_chardev = NULL;

extern int kfd_evict_process_queues_by_task_struct(struct task_struct *task);
extern int kfd_restore_process_queues_by_task_struct(struct task_struct *task);

// Forward declaration.
struct TaskInfo_s;

// This holds information about a single enqueued waiter in a queue for a lock.
// This struct should only be accessed/modified while holding TaskInfo->mutex.
struct GPULockWaiter {
  // A pointer to the parent TaskInfo containing this struct.
  struct TaskInfo_s *parent;
  // The number of outstanding kernels/operations/etc. Basically, issuing an
  // ioctl to acquire the lock when the task already is in the priority queue
  // simply causes this to be incremented. Releasing the lock causes this to be
  // decremented. When it reaches 0, this waiter will be removed from the
  // queue. The lock must *not* be in the queue if this is zero.
  int pending_count;
  // The priority specified along with this request.
  uint64_t priority;
  // Needed to function as an entry in the binary heap.
  struct binheap_node node;
};

// This holds task-local data for every task that opens the chardev. We store
// a pointer to this in the private_data field of the "struct file".
typedef struct TaskInfo_s {
  // The task_struct we expect to be using this TaskInfo.
  struct task_struct *task;
  // A copy of task->tgid for debugging messages.
  int tgid;
  // This will be 0 if no deadline has been set, and we should fall back on the
  // default FIFO-based deadline for this process' locks.
  int use_deadline;
  // This is nonzero if the task's GPU queues are currently evicted. This will
  // only be 1 if we're waiting for a lock. In other words, we're only evicted
  // when we first need to wait for a lock, or if we hold the lock and are
  // preempted by something higher-priority.
  int queues_evicted;
  // The task's deadline, in nanoseconds. Initially based on
  // ktime_get_real_ns(). The top bit must not be set.
  uint64_t deadline_ns;
  // We pre-allocate one GPULockWaiter struct for each lock in the per-task
  // data.
  struct GPULockWaiter per_lock_info[GPU_LOCK_COUNT];
  // Protects concurrent access to the contents of this struct. Must be
  // acquired *before* acquiring any GPULock mutex.
  struct mutex mutex;
} TaskInfo;

// This defines the top-level GPU-lock structure. The idea is to have one of
// these structs per partition.
struct GPULock {
  // The ID of this GPU lock. Not really necessary, but useful for debugging.
  int id;
  // The handle to the priority queue for keeping track of waiters.
  struct binheap queue;
  // The mutex used when adding or removing waiters to the queue, or checking
  // current_owner.
  struct mutex queue_mutex;
};

// A monotonically increasing counter, used to ensure FIFO ordering for
// best-effort work.
static uint64_t fifo_sequence_number = 0;

// Protects access to the module-wide data. Always acquire this *after*
// acquiring the specific TaskInfo.mutex or barrier_sync.mutex.
static struct mutex global_mutex;

// This is the actual list of locks used by the module. Each lock is
// initialized during InitModule.
static struct GPULock gpu_locks[GPU_LOCK_COUNT];

// Holds possible statuses of tasks waiting on a barrier sync.
typedef enum {
  // This will be our status after successfully being woken up normally.
  BARRIER_WAITER_WOKEN_OK = 0,
  // This will be the our status before going to sleep. If sleep was
  // interrupted due to an actual interrupt, then we'd expect this to still
  // be our status value.
  BARRIER_WAITER_WAITING = 1,
  // This will be our status if we were woken up due to an error in a
  // different waiter.
  BARRIER_WAITER_FORCE_WOKEN = 2,
} BarrierSyncStatus;

// Information about a single process that is currently waiting on the barrier
// sync.
struct BarrierSyncWaiter {
  // The waiting task's task_struct.
  struct task_struct *waiter_task;
  // The BarrierSyncStatus enum value.
  BarrierSyncStatus status;
  // The next waiter in the singly-linked list, or NULL if this is the last
  // waiter.
  struct BarrierSyncWaiter *next;
};

// Global state of our barrier-sync operations.
static struct {
  // The head of the list of waiters. Will be NULL if empty.
  struct BarrierSyncWaiter *waiters;
  // The number of expected waiters. It's an error to call the ioct; if this is
  // nonzero and doesn't match the specified "count". Will be 0, however, if no
  // process is waiting.
  int max_waiters;
  // The number of waiters currently waiting.
  int current_waiters;
  // A mutex used to control access to this barrier-sync state.
  struct mutex mutex;
} barrier_sync;

// Returns the current owner of the given GPU lock. Must only be called when
// holding gl->queue_mutex. Returns NULL if the queue is empty.
static struct GPULockWaiter* LockOwner(struct GPULock *gl) {
  struct GPULockWaiter *owner = NULL;
  if (binheap_empty(&(gl->queue))) return NULL;
  owner = binheap_top_entry(&(gl->queue), struct GPULockWaiter, node);
  return owner;
}

// Evicts the given task's GPU queues if they're not already evicted. Simply
// prints any error returned by kfd_evict_process_queues_by_task_struct to the
// kernel log. Should be called when holding the task's mutex.
static void TryEvictingGPUQueues(TaskInfo *task) {
  int result = 0;
  pr_debug("Evicting GPU queues for task TGID %d. Currently evicted = %d.\n",
    task->tgid, task->queues_evicted);

  // Don't evict the task's queues if they're already evicted.
  if (task->queues_evicted) return;

  pr_debug("Calling kfd_evict_process_queues_by_task_struct\n");
  result = kfd_evict_process_queues_by_task_struct(task->task);
  if (result != 0) {
    printk(PRINTK_TAG "Failed evicting GPU queues for TGID %d\n", task->tgid);
  } else {
    pr_debug("Successfully evicted GPU queues for TGID %d\n", task->tgid);
    task->queues_evicted = 1;
  }
}

// Restores the given task's GPU queues if they've been evicted. Simply prints
// any error returned by kfd_restore_process_queues_by_task_struct to the
// kernel log. Should be called when holding the task's mutex.
static void TryRestoringGPUQueues(TaskInfo *task) {
  int result = 0;
  pr_debug("Restoring GPU queues for task TGID %d. Currently evicted = %d.\n",
    task->tgid, task->queues_evicted);

  // Don't try to restore the task's queues if they haven't been evicted.
  if (!task->queues_evicted) return;

  result = kfd_restore_process_queues_by_task_struct(task->task);
  if (result != 0) {
    printk(PRINTK_TAG "Failed restoring GPU queues for TGID %d\n", task->tgid);
  } else {
    pr_debug("Successfully restored GPU queues for TGID %d\n", task->tgid);
    task->queues_evicted = 0;
  }
}

// Returns the priority for the given TGID, based on either a set priority, or
// a best-effort FIFO sequence number. Callers must hold task_state->mutex, but
// *not* global_mutex.
static uint64_t GetPriority(TaskInfo *task_state) {
  uint64_t to_return;

  // If this task has a specified deadline, use it.
  if (task_state->use_deadline) {
    to_return = task_state->deadline_ns;
    return to_return;
  }

  // We didn't have a per-task deadline, so use the global FIFO sequence number
  // instead.
  mutex_lock(&global_mutex);
  to_return = fifo_sequence_number;
  fifo_sequence_number++;
  mutex_unlock(&global_mutex);
  to_return |= (1ULL << 63ULL);
  return to_return;
}

// Implements most of the logic for the set-deadline ioctl. Returns 0 on
// success, or a negative error code on failure. The caller must *not* be
// holding any locks, including task_state->mutex.
static int SetTaskDeadline(TaskInfo *task_state,
    uint64_t relative_deadline_ns) {
  struct GPULock *gl = NULL;
  struct GPULockWaiter *w = NULL;
  struct GPULockWaiter *prev_owner = NULL;
  struct GPULockWaiter *new_owner = NULL;
  uint64_t new_priority = 0;
  int i;
  mutex_lock(&(task_state->mutex));

  if (relative_deadline_ns == 0) {
    // The caller requested to clear the deadline and/or update the best-effort
    // FIFO priority, so set use_deadline to 0 and call GetPriority to get a
    // new FIFO priority number. Technically, this is *slightly* different from
    // the behavior of GetPriority, since it may set the same best-effort
    // priority for waiters in different queues, but it's best effort so who
    // really cares.
    task_state->use_deadline = 0;
    task_state->deadline_ns = 0;
    new_priority = GetPriority(task_state);
    pr_debug("Updating/changing to FIFO order for TGID %d. New priority=%lu\n",
      task_state->tgid, (unsigned long) new_priority);
  } else {
    // The caller specified a new relative deadline, so compute the absolute
    // deadline and use it as the new priority.
    new_priority = ktime_get_real_ns() + relative_deadline_ns;
    if (new_priority >> 63L) {
      printk(PRINTK_TAG "Attempting to set a deadline that's too big: %lu.\n",
        (unsigned long) new_priority);
      mutex_unlock(&(task_state->mutex));
      return -EINVAL;
    }
    pr_debug("Updating/setting deadline for TGID %d. New priority=%lu\n",
      task_state->tgid, (unsigned long) new_priority);
    task_state->deadline_ns = new_priority;
    task_state->use_deadline = 1;
  }

  // Now we have a new priority, so we need to update the task in the queue for
  // every lock it's enqueued for.
  for (i = 0; i < GPU_LOCK_COUNT; i++) {
    gl = gpu_locks + i;
    w = task_state->per_lock_info + i;
    // We're only enqueued if we have a positive pending_count.
    if (w->pending_count <= 0) continue;

    // We're in a lock's queue, so remove ourselves, update our priority, then
    // reinsert ourselves. We don't use RemoveFromLockQueue here, since it
    // modifies pending_count may do an extra unneeded queue eviction.
    mutex_lock(&(gl->queue_mutex));
    pr_debug("About to call binheap_delete.\n");
    prev_owner = LockOwner(gl);
    binheap_delete(&(w->node), &(gl->queue));
    w->priority = new_priority;
    binheap_add(&(w->node), &(gl->queue), struct GPULockWaiter, node);
    new_owner = LockOwner(gl);
    mutex_unlock(&(gl->queue_mutex));

    // If a new owner took the lock here, then we'll need to evict the old
    // owner and grant access to the new owner. Neither of these will be NULL,
    // since we already know that at least w is/was waiting for this lock.
    if (prev_owner != new_owner) {
      pr_debug("Updating deadline changed lock %d owner from TGID %d to %d\n",
        i, prev_owner->parent->tgid, new_owner->parent->tgid);
      TryEvictingGPUQueues(prev_owner->parent);
      TryRestoringGPUQueues(new_owner->parent);
    }
  }

  // We only unlock this now that we're done modifying its entry in potentially
  // every lock's queue.
  mutex_unlock(&(task_state->mutex));
  return 0;
}

// The comparator function needed by the priority queue.
static int PriorityComparator(struct binheap_node *a, struct binheap_node *b) {
  struct GPULockWaiter *a_data = (struct GPULockWaiter *) a->data;
  struct GPULockWaiter *b_data = (struct GPULockWaiter *) b->data;
  return a_data->priority < b_data->priority;
}

// Enqueues the task's GPULockWaiter into the queue for the corresponding lock.
// *It is the caller's responsibility to ensure that the task isn't already in
// the queue!* Expected to be used when first adding a task to the queue. Does
// not check or change pending_count. May preempt the task at the head of the
// queue. If the head of the queue changes, tasks will be evicted from or
// allowed to access the GPU as necessary. The caller must hold
// task_state->mutex. Returns zero on error.
static int EnqueueTaskWaiter(TaskInfo *task_state, int lock_id) {
  struct GPULock *gl = NULL;
  struct GPULockWaiter *w = NULL;
  struct GPULockWaiter *prev_front = NULL;
  struct GPULockWaiter *new_front = NULL;
  gl = gpu_locks + lock_id;
  w = task_state->per_lock_info + lock_id;

  // Insert the new waiter into the queue, keeping track of what was at the
  // front in case it changes.
  mutex_lock(&(gl->queue_mutex));
  prev_front = LockOwner(gl);
  binheap_add(&(w->node), &(gl->queue), struct GPULockWaiter, node);
  new_front = LockOwner(gl);
  mutex_unlock(&(gl->queue_mutex));

  // Evict ourselves from the GPU if we didn't acquire the lock right away.
  if (new_front != w) TryEvictingGPUQueues(w->parent);

  // Preempt whatever was running on the GPU, and allow whatever's now at the
  // front to start running.
  if (new_front != prev_front) {
    // The queue may have been empty previously, but won't be now.
    if (prev_front) TryEvictingGPUQueues(prev_front->parent);
    TryRestoringGPUQueues(new_front->parent);
  }

  return 1;
}

// Acquires the lock with the given ID. Returns -EINVAL on error. If the task
// already has a nonzero pending_count, its pending_count is simply
// incremented. If this is the first time the lock is "acquired", then the
// waiter struct will be enqueued, potentially preempting the current lock
// owner.
static long AcquireLock(TaskInfo *task_state, int lock_id) {
  int tgid = task_state->tgid;
  struct GPULockWaiter *w = NULL;
  int pending;
  mutex_lock(&(task_state->mutex));

  w = task_state->per_lock_info + lock_id;
  pending = w->pending_count;
  pr_debug("TGID %d acquiring lock %d. Pending = %d.\n", tgid, lock_id,
    pending);

  // If the pending_count for this lock is already positive, then we only need
  // to increment it; we're already in the queue.
  if (pending > 0) {
    w->pending_count++;
    mutex_unlock(&(task_state->mutex));
    return 0;
  }

  // pending_count was 0, so set our priority and enqueue ourselves.
  w->priority = GetPriority(task_state);
  w->pending_count = 1;
  if (!EnqueueTaskWaiter(task_state, lock_id)) {
    printk(PRINTK_TAG "Error enqueuing TGID %d for lock %d.\n", tgid, lock_id);
    mutex_unlock(&(task_state->mutex));
    return -EINVAL;
  }
  mutex_unlock(&(task_state->mutex));
  pr_debug("Enqueued TGID %d for lock %d OK.\n", tgid, lock_id);
  return 0;
}

// Removes the task's waiter for the given lock_id from the corresponding
// lock's queue. The caller must already hold task_state->mutex, but *not* the
// queue_mutex for the lock. If the owner of the lock changes, then this will
// re-enable GPU access for the new owner *as well as the task that was
// removed* (since it shouldn't have any GPU work anyway). Does nothing if the
// task isn't waiting on the given lock. Sets pending_count to 0 if it's
// positive. Returns 0 on error.
static int RemoveFromLockQueue(TaskInfo *task_state, int lock_id) {
  struct GPULock *gl = NULL;
  struct GPULockWaiter *w = NULL;
  struct GPULockWaiter *prev_owner = NULL;
  struct GPULockWaiter *new_owner = NULL;
  gl = gpu_locks + lock_id;
  w = task_state->per_lock_info + lock_id;
  pr_debug("About to remove task TGID %d from queue for lock %d. Has %d "
    "pending.\n", task_state->tgid, gl->id, w->pending_count);

  if (w->pending_count <= 0) return 1;
  w->pending_count = 0;

  mutex_lock(&(gl->queue_mutex));
  if (binheap_empty(&(gl->queue))) {
    printk(PRINTK_TAG "Error! Can't remove TGID %d from lock %d; it has no "
      "waiters!\n", task_state->tgid, gl->id);
    mutex_unlock(&(gl->queue_mutex));
    return 0;
  }

  // Do the actual removal, keeping track of if the lock owner changed.
  pr_debug("About to call binheap_delete.\n");
  prev_owner = LockOwner(gl);
  binheap_delete(&(w->node), &(gl->queue));
  new_owner = LockOwner(gl);
  mutex_unlock(&(gl->queue_mutex));

  if (prev_owner && (prev_owner != w)) {
    // It doesn't really make sense for this to happen, since the owner should
    // never change after *removing a non-owner*. But we'll check for it
    // anyway, perhaps if two waiters share a priority and the lock doesn't
    // have stable sorting. We'd need to evict the previous owner's queues in
    // this case.
    printk(PRINTK_TAG "Odd behavior! Lock %d owner changed from TGID %d to %d "
      " after removing TGID %d.\n", gl->id, prev_owner->parent->tgid,
      new_owner->parent->tgid, w->parent->tgid);
    TryEvictingGPUQueues(prev_owner->parent);
  }

  // Restore the GPU queues since we shouldn't have any outstanding work
  // anyway when removing ourselves from a queue.  (Unless there's an error, in
  // which case it's probably better to just go ahead and let things run in
  // hopes of rescuing the system.)
  TryRestoringGPUQueues(task_state);
  if (new_owner) {
    pr_debug("New owner of lock %d: TGID %d.\n", gl->id,
      new_owner->parent->tgid);
    TryRestoringGPUQueues(new_owner->parent);
  }

  return 1;
}

// Releases the lock with the given ID. Returns -EINVAL on error. Removes the
// task from the lock's priority queue if its pending_count would reach 0,
// otherwise simply decrements pending_count. task_state->mutex must *not* be
// held when calling this.
static long ReleaseLock(TaskInfo *task_state, int lock_id) {
  int tgid = task_state->tgid;
  struct GPULockWaiter *w = NULL;
  int pending, ok;
  mutex_lock(&(task_state->mutex));

  w = task_state->per_lock_info + lock_id;
  pending = w->pending_count;
  pr_debug("TGID %d releasing lock %d. Pending = %d.\n", tgid, lock_id,
    pending);

  // Basic sanity test; may happen if the waiter was forcibly removed from the
  // lock by some other ioctl (e.g. GPU_LOCK_RELEASE_ALL_IOC).
  if (pending <= 0) {
    printk(PRINTK_TAG "Error releasing lock %d: TGID %d's pending_count is "
      "%d.\n", lock_id, tgid, pending);
    mutex_unlock(&(task_state->mutex));
    return -EINVAL;
  }

  // If our pending_count would reach 0, remove the task from the lock's queue.
  // Do this when pending_count is 1 since RemoveFromLockQueue will set it to 0
  if (pending == 1) {
    ok = RemoveFromLockQueue(task_state, lock_id);
    mutex_unlock(&(task_state->mutex));
    if (!ok) {
      printk(PRINTK_TAG "Error removing TGID %d from lock %d queue.\n", tgid,
        lock_id);
      return -EINVAL;
    }
    pr_debug("Removed TGID %d from lock %d queue OK.\n", tgid, lock_id);
    return 0;
  }

  // We have more pending stuff, so just decrement pending_count and don't
  // leave the queue yet.
  w->pending_count--;
  pr_debug("Reduced TGID %d's pending_count for lock %d to %d.\n", tgid,
    lock_id, w->pending_count);
  mutex_unlock(&(task_state->mutex));
  return 0;
}

// For GPU_LOCK_ACQUIRE_IOC
static long AcquireGPULockIoctl(void __user *arg, TaskInfo *task_state) {
  GPULockArgs a;
  if (copy_from_user(&a, arg, sizeof(a)) != 0) {
    printk(PRINTK_TAG "Failed copying lock-acquire args.\n");
    return -EFAULT;
  }
  pr_debug("Got ioctl from TGID %d to acquire lock %d.\n", (int) current->tgid,
    a.lock_id);
  if (a.lock_id >= GPU_LOCK_COUNT) {
    printk(PRINTK_TAG "Got request for invalid lock ID %d.\n", a.lock_id);
    return -EINVAL;
  }

  return AcquireLock(task_state, a.lock_id);
}

// For GPU_LOCK_RELEASE_IOC
static long ReleaseGPULockIoctl(void __user *arg, TaskInfo *task_state) {
  GPULockArgs a;
  if (copy_from_user(&a, arg, sizeof(a)) != 0) {
    printk(PRINTK_TAG "Failed copying lock-release args.\n");
    return -EFAULT;
  }
  pr_debug("Got ioctl from TGID %d to release lock %d.\n", (int) current->tgid,
    a.lock_id);
  if (a.lock_id >= GPU_LOCK_COUNT) {
    printk(PRINTK_TAG "Got request to release invalid lock ID %d.\n",
      a.lock_id);
    return -EINVAL;
  }

  return ReleaseLock(task_state, a.lock_id);
}

static long SetDeadlineIoctl(void __user *arg, TaskInfo *task_state) {
  SetDeadlineArgs a;
  int result;
  if (copy_from_user(&a, arg, sizeof(a)) != 0) {
    printk(PRINTK_TAG "Failed copying set-deadline args.\n");
    return -EFAULT;
  }
  pr_debug("Got ioctl to set TGID %d's deadline to %lu.\n", task_state->tgid,
    (unsigned long) a.deadline);

  result = SetTaskDeadline(task_state, a.deadline);
  if (result != 0) {
    printk(PRINTK_TAG "Failed setting deadline for TGID %d.\n",
      task_state->tgid);
    return -EINVAL;
  }
  return 0;
}

// Wakes up all tasks waiting on the barrier-sync. Sets each waiting task's
// status to the given value. (It should be BARRIER_WAITER_WOKEN_OK if this
// is called due to the last waiter successfully reaching the barrier.) The
// caller must already hold barrier_sync.mutex. Rests the rest of the
// barrier_sync state so that nothing is waiting.
static void WakeupAllBarrierWaiters(BarrierSyncStatus new_status) {
  struct BarrierSyncWaiter *w = barrier_sync.waiters;
  struct BarrierSyncWaiter *next = NULL;
  struct task_struct *to_wake = NULL;

  // Wake up everything.
  while (w != NULL) {
    w->status = new_status;
    // Copy the fields as the struct may become invalid as soon as the process
    // wakes up (it lives on the waiter's stack).
    to_wake = w->waiter_task;
    next = w->next;
    wake_up_process(to_wake);
    w = next;
  }
  barrier_sync.current_waiters = 0;
  barrier_sync.max_waiters = 0;
  barrier_sync.waiters = NULL;
}

static long BarrierSyncIoctl(void __user *arg, TaskInfo *task_state) {
  struct BarrierSyncWaiter waiter;
  BarrierSyncArgs a;
  struct mutex *m = &(barrier_sync.mutex);
  if (copy_from_user(&a, arg, sizeof(a)) != 0) {
    printk(PRINTK_TAG "Failed copying barrier-sync args.\n");
    return -EFAULT;
  }
  pr_debug("Got barrier-sync ioctl from TGID %d.\n", (int) current->tgid);

  // We don't support barrier-wait on any number of processes under 1.
  if (a.count <= 1) {
    printk(PRINTK_TAG "Invalid barrier-wait count: %d.\n", a.count);
    return -EINVAL;
  }

  mutex_lock(m);
  if (barrier_sync.max_waiters != 0) {
    if (barrier_sync.max_waiters != a.count) {
      printk(PRINTK_TAG "Invalid barrier-wait count: %d (expected %d).\n",
        a.count, barrier_sync.max_waiters);
      WakeupAllBarrierWaiters(BARRIER_WAITER_FORCE_WOKEN);
      mutex_unlock(m);
      return -EINVAL;
    }
  } else {
    barrier_sync.max_waiters = a.count;
  }

  // If we were the last necessary waiter, wake up everything with success.
  barrier_sync.current_waiters++;
  if (barrier_sync.current_waiters == barrier_sync.max_waiters) {
    pr_debug("PID %d, TGID %d was the final process to reach the barrier.\n",
      (int) current->pid, (int) current->tgid);
    WakeupAllBarrierWaiters(BARRIER_WAITER_WOKEN_OK);
    mutex_unlock(m);
    return 0;
  }

  // We weren't the last; go to sleep until we're woken.
  memset(&waiter, 0, sizeof(waiter));
  waiter.waiter_task = current;
  waiter.status = BARRIER_WAITER_WAITING;
  waiter.next = barrier_sync.waiters;

  // Add ourselves to the linked list before releasing the mutex and sleeping.
  barrier_sync.waiters = &waiter;
  set_current_state(TASK_INTERRUPTIBLE);
  mutex_unlock(m);
  schedule();

  // We were woken up. Grab the mutex first, so we know that nobody else will
  // try to change our status.
  mutex_lock(m);

  // If we were woken up due to a success we can return now.
  if (waiter.status == BARRIER_WAITER_WOKEN_OK) {
    mutex_unlock(m);
    pr_debug("PID %d, TGID %d woke from barrier OK.\n", (int) current->pid,
      (int) current->tgid);
    return 0;
  }

  // If we were woken up due to a different process' error, we can also return,
  // but with an error.
  if (waiter.status == BARRIER_WAITER_FORCE_WOKEN) {
    mutex_unlock(m);
    pr_debug("PID %d, TGID %d woke from barrier due to other error.\n",
      (int) current->pid, (int) current->tgid);
    return -EINTR;
  }

  // If we were interrupted, we'll return with an error, but first we'll wake
  // up everything else too.
  printk(PRINTK_TAG "Barrier-waiter PID %d, TGID %d interrupted.\n",
    (int) current->pid, (int) current->tgid);
  WakeupAllBarrierWaiters(BARRIER_WAITER_FORCE_WOKEN);
  mutex_unlock(m);
  return -EINTR;
}

static long DeviceIoctl(struct file *file, unsigned int cmd,
    unsigned long arg) {
  TaskInfo *task_state = (TaskInfo *) file->private_data;

  // Quickly check that the task_state exists.
  if (!task_state) {
    printk(PRINTK_TAG "Internal error: Missing task_state for ioctl.\n");
    return -EINVAL;
  }

  switch (cmd) {
  case GPU_LOCK_ACQUIRE_IOC:
    return AcquireGPULockIoctl((void __user *) arg, task_state);
  case GPU_LOCK_RELEASE_IOC:
    return ReleaseGPULockIoctl((void __user *) arg, task_state);
  case GPU_LOCK_SET_DEADLINE_IOC:
    return SetDeadlineIoctl((void __user *) arg, task_state);
  case GPU_LOCK_BARRIER_SYNC_IOC:
    return BarrierSyncIoctl((void __user *) arg, task_state);
  default:
    break;
  }
  printk(PRINTK_TAG "Invalid ioctl command!\n");
  return -EINVAL;
}

// Fills in the fields of a newly allocated TaskInfo struct.
static void InitializeTaskInfo(TaskInfo *task_state) {
  struct GPULockWaiter *lock_waiter = NULL;
  int i;
  memset(task_state, 0, sizeof(*task_state));
  task_state->task = current;
  task_state->tgid = (int) current->tgid;
  mutex_init(&(task_state->mutex));
  for (i = 0; i < GPU_LOCK_COUNT; i++) {
    lock_waiter = task_state->per_lock_info + i;
    lock_waiter->parent = task_state;
    INIT_BINHEAP_NODE(&(lock_waiter->node));
  }
}

static int OpenDevice(struct inode *inode, struct file *file) {
  TaskInfo *task_state = NULL;
  printk(PRINTK_TAG "Device opened by TGID %d.\n", (int) current->tgid);
  if (file->private_data != NULL) {
    printk("Error! PID %d, TGID %d opened chardev but already has "
      "private_data?\n", (int) current->pid, (int) current->tgid);
    return -EINVAL;
  }
  task_state = (TaskInfo *) kmalloc(sizeof(*task_state), GFP_KERNEL);
  if (!task_state) {
    printk("Failed allocating memory for state for PID %d, TGID %d.\n",
      (int) current->pid, (int) current->tgid);
    return -ENOMEM;
  }
  InitializeTaskInfo(task_state);
  file->private_data = task_state;
  return 0;
}

static int ReleaseDevice(struct inode *inode, struct file *file) {
  int i;
  int tgid = current->tgid;
  TaskInfo *task_state = (TaskInfo *) file->private_data;
  if (!task_state) {
    printk(PRINTK_TAG "Error! Got release call without task_state.\n");
    return -EINVAL;
  }
  if (task_state->tgid != current->tgid) {
    printk(PRINTK_TAG "Error! Got release call with wrong task_state->tgid. "
      "Expected %d, but current->tgid is %d.\n", task_state->tgid,
      (int) current->tgid);
    return -EINVAL;
  }
  printk(PRINTK_TAG "Device closed by TGID %d.\n", tgid);

  // Remove ourselves from any lock queues we're part of, regardless of the
  // pending lock-acquire counts.
  mutex_lock(&(task_state->mutex));
  for (i = 0; i < GPU_LOCK_COUNT; i++) {
    if (!RemoveFromLockQueue(task_state, i)) {
      printk(PRINTK_TAG "Failed removing TGID %d from lock %d's queue.\n",
        tgid, i);
    }
  }
  mutex_unlock(&task_state->mutex);

  kfree(task_state);
  file->private_data = NULL;
  return 0;
}

static struct file_operations fops = {
  .open = OpenDevice,
  .release = ReleaseDevice,
  .unlocked_ioctl = DeviceIoctl,
};

// Intended to be executed when the chardev is created. Assigns 0666
// permissions to the device.
static int LockingClassUdevEventCallback(struct device *dev,
    struct kobj_uevent_env *env) {
  if (devmode_var_added) return 0;
  devmode_var_added = 1;
  pr_debug("Got first uevent, adding DEVMODE uevent var.\n");
  return add_uevent_var(env, "DEVMODE=0666");
}

static void InitBarrierWait(void) {
  mutex_init(&barrier_sync.mutex);
  barrier_sync.max_waiters = 0;
  barrier_sync.current_waiters = 0;
  barrier_sync.waiters = NULL;
}

// Initializes a single GPULock struct. The lock_id must match the index of the
// GPULock struct in the gpu_locks array.
static void InitializeGPULock(struct GPULock *gl, int lock_id) {
  pr_debug("Initializing lock ID %d.\n", lock_id);
  memset(gl, 0, sizeof(*gl));
  INIT_BINHEAP_HANDLE(&(gl->queue), PriorityComparator);
  mutex_init(&(gl->queue_mutex));
  gl->id = lock_id;
}

static int __init InitModule(void) {
  int i;
  pr_debug("Found evict process queues function @ %p.\n",
    kfd_evict_process_queues_by_task_struct);
  pr_debug("Found restore process queues function @ %p.\n",
    kfd_restore_process_queues_by_task_struct);
  mutex_init(&global_mutex);
  fifo_sequence_number = 0;
  InitBarrierWait();

  // First, initialize each of our priority lock structs.
  for (i = 0; i < GPU_LOCK_COUNT; i++) {
    InitializeGPULock(gpu_locks + i, i);
  }

  major_number = register_chrdev(0, DEVICE_NAME, &fops);
  if (major_number < 0) {
    printk(PRINTK_TAG "Failed registering chardev.\n");
    return 1;
  }
  gpu_lock_device_class = class_create(THIS_MODULE, "gpu_locking");
  if (IS_ERR(gpu_lock_device_class)) {
    printk(PRINTK_TAG "Failed to register device class.\n");
    unregister_chrdev(major_number, DEVICE_NAME);
    return 1;
  }
  // Make our chardev have 0666 permissions.
  devmode_var_added = 0;
  gpu_lock_device_class->dev_uevent = LockingClassUdevEventCallback;

  gpu_lock_chardev = device_create(gpu_lock_device_class, NULL,
    MKDEV(major_number, 0), NULL, DEVICE_NAME);
  if (IS_ERR(gpu_lock_chardev)) {
    printk(PRINTK_TAG "Failed to create chardev.\n");
    class_destroy(gpu_lock_device_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    return 1;
  }
  printk(PRINTK_TAG "Loaded, chardev major number %d.\n", major_number);
  return 0;
}

static void __exit CleanupModule(void) {
  // NOTE: Is it even possible for tasks to still be in queues here? I don't
  // think so, since they'd need to have an open file handle...
  printk(PRINTK_TAG "Unloading...\n");
  device_destroy(gpu_lock_device_class, MKDEV(major_number, 0));
  class_destroy(gpu_lock_device_class);
  unregister_chrdev(major_number, DEVICE_NAME);
  printk(PRINTK_TAG "Unloaded.\n");
}

module_init(InitModule);
module_exit(CleanupModule);
MODULE_LICENSE("GPL");

