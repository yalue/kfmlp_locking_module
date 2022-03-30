// Contains the top-level module definition for the KFMLP module.
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "kfmlp_module.h"

// Uncomment to enable priority boosting the lock holders.
// #define USE_PRIORITY_BOOSTING (1)

// Used when accessing any shared structure in this module.
struct mutex global_mutex;

// Faster for me to make this myself than to wrap my head around the kernel's
// wait_queue_t. Given more time, I'd properly use the existing code.
typedef struct QueueWaiter_s {
  struct QueueWaiter_s *prev;
  struct QueueWaiter_s *next;
  struct task_struct *p;
  // woken_ok will be set to nonzero if we were released properly.
  int woken_ok;
} QueueWaiter;

// The module's current 'k' value for k-FMLP.
static uint32_t kfmlp_k;

// The current lock holders. Only the first kfmlp_k entries of this array may
// be used. Available slots must be NULL.
static struct task_struct *lock_holders[KFMLP_MAX_K];

// The FIFO queue of tasks waiting for the KFMLP entries. Entries are added at
// the tail and removed from the head.
static QueueWaiter *lock_waiters_head;
static QueueWaiter *lock_waiters_tail;

// The number of waiters in the release_waiters list.
static unsigned int release_waiter_count;

// A linked list of processes waiting to be released.
static QueueWaiter *release_waiters;

// Forward declaration.
static long ReleaseKFMLPLock(int print_warning);

static void LockModule(void) {
  mutex_lock(&global_mutex);
}

static void UnlockModule(void) {
  mutex_unlock(&global_mutex);
}

static int KFMLPOpen(struct inode *n, struct file *f) {
  printk("KFMLP device opened by process %d.\n", current->pid);
  return 0;
}

static int KFMLPRelease(struct inode *n, struct file *f) {
  ReleaseKFMLPLock(0);
  printk("KFMLP device released by process %d.\n", current->pid);
  return 0;
}

static long StartSchedFIFO(struct task_struct *p) {
  if (p->policy != SCHED_NORMAL) {
    printk("Attempted to switch to SCHED_FIFO when not SCHED_NORMAL.\n");
    return -EINVAL;
  }
  // We *need* fifo_low here in order for the priority boosting using
  // sched_set_fifo() later.
  sched_set_fifo_low(p);
  return 0;
}

static long EndSchedFIFO(struct task_struct *p) {
  int policy = p->policy;
  if (policy == SCHED_NORMAL) return 0;
  if (policy != SCHED_FIFO) {
    printk("Attempting to switch to SCHED_NORMAL when not NORMAL or FIFO.\n");
    return -EINVAL;
  }
  sched_set_normal(p, 0);
  return 0;
}

// Adds the calling process to the list of waiters and begins waiting. Returns
// 0 on success, or -EINTR if interrupted before being woken up.
static long WaitForTSRelease(void) {
  QueueWaiter waiter;
  memset(&waiter, 0, sizeof(waiter));
  LockModule();
  release_waiter_count++;
  waiter.p = current;
  waiter.next = release_waiters;
  if (release_waiters) {
    release_waiters->prev = &waiter;
  }
  release_waiters = &waiter;

  // We're on the list, begin to wait.
  set_current_state(TASK_INTERRUPTIBLE);
  UnlockModule();
  schedule();

  // Return immediately if we were woken normally.
  if (waiter.woken_ok) return 0;

  // We were interrupted. However, we still need to check for the race
  // condition where we were "released" immediately after being interrupted.
  LockModule();
  if (waiter.woken_ok) {
    UnlockModule();
    return 0;
  }
  // We were interrupted and need to remove ourselves from the list.
  printk("PID %d interrupted while waiting for TS release.\n", current->pid);
  if (release_waiters == &waiter) release_waiters = waiter.next;
  if (waiter.prev) waiter.prev->next = waiter.next;
  if (waiter.next) waiter.next->prev = waiter.prev;
  release_waiter_count--;
  UnlockModule();
  return -EINTR;
}

// Releases all processes waiting to be released.
static long ReleaseTS(void) {
  QueueWaiter *w = release_waiters;
  struct task_struct *p = NULL;
  // Don't allow ourselves to be booted off the CPU until we've woken
  // everything.
  LockModule();
  preempt_disable();
  while (w) {
    // We need to copy p before waking the other process, as it lives on the
    // other process' stack.
    p = w->p;
    w->woken_ok = 1;
    w = w->next;
    wake_up_process(p);
  }
  release_waiters = NULL;
  release_waiter_count = 0;
  preempt_enable();
  UnlockModule();
  return 0;
}

static long GetWaiterCount(unsigned long arg) {
  GetTSWaiterCountArgs a;
  LockModule();
  a.count = release_waiter_count;
  UnlockModule();
  if (copy_to_user((void __user *) arg, &a, sizeof(a)) != 0) {
    printk("Error copying GetWaiterCount info to user.\n");
    return -EFAULT;
  }
  return 0;
}

// Returns the number of lock holders. Will return 0 if there are no lock
// holders, including if k is 0. Must be called while global_mutex is held.
static uint32_t GetLockHolderCount(void) {
  uint32_t i;
  uint32_t count = 0;
  if (kfmlp_k == 0) return 0;
  for (i = 0; i < kfmlp_k; i++) {
    if (lock_holders[i] != NULL) count++;
  }
  return count;
}

static long SetNewK(unsigned long arg) {
  SetKFMLPLockKArgs a;
  if (copy_from_user(&a, (void __user *) arg, sizeof(a)) != 0) {
    printk("Error copying SetNewK args from user.\n");
    return -EFAULT;
  }
  if (a.k > KFMLP_MAX_K) {
    printk("Got new K value (%d) that is too big.\n", (int) a.k);
    return -EINVAL;
  }
  if (a.k == 0) {
    // I don't think I'll make this an error; perhaps it would be useful to
    // prevent any attempts to acquire the lock.
    printk("Warning: got new K value of 0.\n");
  }
  LockModule();
  if (GetLockHolderCount() != 0) {
    printk("Can't change the K value while tasks hold the lock.\n");
    UnlockModule();
    return -EINVAL;
  }
  kfmlp_k = a.k;
  UnlockModule();
  printk("Set new K value for KFMLP locking to %d.\n", (int) kfmlp_k);
  return 0;
}

// Adds waiter w to the tail of the queue for the kfmlp lock. Must be called
// while holding global_mutex.
static void EnqueueWaiter(QueueWaiter *w) {
  w->next = NULL;
  if (lock_waiters_head == NULL) {
    // There are currently no waiters.
    w->prev = NULL;
    lock_waiters_head = w;
    lock_waiters_tail = w;
    return;
  }
  w->prev = lock_waiters_tail;
  lock_waiters_tail->next = w;
  lock_waiters_tail = w;
}

// Removes the QueueWaiter pointer from the head of the queue for the kfmlp
// lock and returns it. Returns NULL if there are no enqueued waiters. Must be
// called while holding global_mutex.
static QueueWaiter* DequeueWaiter(void) {
  QueueWaiter *to_return = NULL;
  if (lock_waiters_head == NULL) return NULL;
  to_return = lock_waiters_head;
  if (lock_waiters_head == lock_waiters_tail) {
    // There was only one waiter.
    lock_waiters_head = NULL;
    lock_waiters_tail = NULL;
    return to_return;
  }
  lock_waiters_head = lock_waiters_head->next;
  lock_waiters_head->prev = NULL;
  return to_return;
}

// Takes a pointer to a QueueWaiter *somewhere in the queue* and removes it.
// w *must* be in the queue.
static void RemoveQueueWaiter(QueueWaiter *w) {
  if (w == lock_waiters_head) {
    // This also takes care of the case where w was the only entry.
    DequeueWaiter();
    return;
  }
  // At this point, we know the queue contains at least two entries.
  if (w == lock_waiters_tail) {
    lock_waiters_tail = w->prev;
    lock_waiters_tail->next = NULL;
    return;
  }
  // w isn't the first or the last entry in the queue.
  w->prev->next = w->next;
  w->next->prev = w->prev;
  return;
}

// Searches the lock holders array for a free slot. Returns an arbitrary value
// greater than KFMLP_MAX_K if no free slot is available. The global_mutex must
// be held before calling this.
static uint32_t FindFreeSlot(void) {
  uint32_t i;
  for (i = 0; i < kfmlp_k; i++) {
    if (lock_holders[i] == NULL) return i;
  }
  // Will always be greater than kfmlp_k.
  return KFMLP_MAX_K + 1;
}

// Returns the lock slot of the calling process. Returns an arbitrary value
// greater than KFMLP_MAX_K if the lock holder isn't found.
static uint32_t FindOurSlot(void) {
  struct task_struct *p = current;
  uint32_t i;
  for (i = 0; i < kfmlp_k; i++) {
    if (lock_holders[i] == p) return i;
  }
  return KFMLP_MAX_K + 1;
}

// Returns after the calling process has acquired the lock. Boosts the caller's
// priority if the lock is acquired successfully.
static long AcquireKFMLPLock(unsigned long arg) {
  struct task_struct *p = current;
  QueueWaiter waiter;
  AcquireKFMLPLockArgs a;

  LockModule();
  if (kfmlp_k == 0) {
    UnlockModule();
    printk("Can't acquire KFMLP lock when k is set to 0.\n");
    return -EINVAL;
  }
  if (FindOurSlot() < kfmlp_k) {
    UnlockModule();
    printk("Nested KFMLP lock acquisition prevented.\n");
    return -EINVAL;
  }
  a.lock_slot = FindFreeSlot();
  if (a.lock_slot < kfmlp_k) {
    // We found a free slot immediately.
    lock_holders[a.lock_slot] = p;
    UnlockModule();
    goto success;
  }

  // All slots are occupied; we must wait.
  memset(&waiter, 0, sizeof(waiter));
  waiter.p = p;
  EnqueueWaiter(&waiter);
  set_current_state(TASK_INTERRUPTIBLE);
  UnlockModule();
  schedule();

  // Woken up; see if we got the lock; first without the mutex.
  if (waiter.woken_ok) {
    a.lock_slot = FindOurSlot();
    BUG_ON(a.lock_slot >= kfmlp_k);
    goto success;
  }

  LockModule();
  // We were interrupted. Check the race condition where we may have gotten the
  // lock between being interrupted and acquiring the global_mutex.
  if (waiter.woken_ok) {
    a.lock_slot = FindOurSlot();
    BUG_ON(a.lock_slot >= kfmlp_k);
    UnlockModule();
    goto success;
  }
  // We were interrupted and don't have the lock. Remove ourselves from the
  // wait queue.
  printk("PID %d interrupted while waiting for KFMLP lock.\n", current->pid);
  RemoveQueueWaiter(&waiter);
  UnlockModule();
  return -EINTR;

success:
  // At this point, a.lock_slot must be set.
  if (copy_to_user((void __user *) arg, &a, sizeof(a)) != 0) {
    printk("Error copying KFMLP lock slot to user process!\n");
    ReleaseKFMLPLock(1);
    return -EFAULT;
  }
#ifdef USE_PRIORITY_BOOSTING
  // Boost our priority.
  sched_set_fifo(p);
#endif
  return 0;
}

// Handles the lock-release ioctl, but also may be called anywhere we need to
// release the KFMLP lock held by the caller. Make sure the global_mutex is
// *not* held when calling this function from such a context. If print_warning
// is nonzero, this will print a message to the kernel log if the caller does
// not hold the lock. In all cases, returns -EINVAL if the lock wasn't held.
static long ReleaseKFMLPLock(int print_warning) {
  QueueWaiter *w = NULL;
  uint32_t our_slot;
  LockModule();
  our_slot = FindOurSlot();
  if (our_slot >= kfmlp_k) {
    UnlockModule();
    if (print_warning) {
      printk("Called ReleaseKFMLPLock when it wasn't held!\n");
    }
    return -EINVAL;
  }
  lock_holders[our_slot] = NULL;
  w = DequeueWaiter();
  if (w == NULL) {
    // There were no waiters for the lock.
    UnlockModule();
#ifdef USE_PRIORITY_BOOSTING
    // It would probably be better to save the task's old scheduler and
    // priority rather than assuming it was SCHED_FIFO.
    sched_set_fifo_low(current);
#endif
    return 0;
  }

  // Put the new waiter in the lock slot. It will boost its own priority upon
  // waking.
  lock_holders[our_slot] = w->p;
  w->woken_ok = 1;
  wake_up_process(w->p);
  w = NULL;

  UnlockModule();
#ifdef USE_PRIORITY_BOOSTING
  sched_set_fifo_low(p);
#endif
  return 0;
}

static long GetCurrentK(unsigned long arg) {
  SetKFMLPLockKArgs a;
  LockModule();
  a.k = kfmlp_k;
  UnlockModule();
  if (copy_to_user((void __user *) arg, &a, sizeof(a)) != 0) {
    printk("Error copying current K info to user.\n");
    return -EFAULT;
  }
  return 0;
}

static long KFMLPIoctl(struct file *f, unsigned int nr, unsigned long arg) {
  struct task_struct *p = current;
  switch (nr) {
  case KFMLP_START_SCHED_FIFO_IOC:
    return StartSchedFIFO(p);
  case KFMLP_END_SCHED_FIFO_IOC:
    return EndSchedFIFO(p);
  case KFMLP_WAIT_FOR_TS_RELEASE_IOC:
    return WaitForTSRelease();
  case KFMLP_RELEASE_TS_IOC:
    return ReleaseTS();
  case KFMLP_GET_TS_WAITER_COUNT_IOC:
    return GetWaiterCount(arg);
  case KFMLP_SET_K_IOC:
    return SetNewK(arg);
  case KFMLP_LOCK_ACQUIRE_IOC:
    return AcquireKFMLPLock(arg);
  case KFMLP_LOCK_RELEASE_IOC:
    return ReleaseKFMLPLock(1);
  case KFMLP_GET_K_IOC:
    return GetCurrentK(arg);
  }
  printk("Task %d sent an invalid IOCTL to the KFMLP module.\n", p->pid);
  return -EINVAL;
}

static struct file_operations kfmlp_fops = {
  .owner = THIS_MODULE,
  .unlocked_ioctl = KFMLPIoctl,
  .open = KFMLPOpen,
  .release = KFMLPRelease,
};

static struct miscdevice kfmlp_dev = {
  .name = DEVICE_NAME,
  .minor = MISC_DYNAMIC_MINOR,
  .fops = &kfmlp_fops,
  .mode = 0666,
};

static int __init InitModule(void) {
  int result;
  printk("Loading KFMLP module.\n");
  mutex_init(&global_mutex);
  release_waiter_count = 0;
  release_waiters = NULL;
  kfmlp_k = 0;
  lock_waiters_head = NULL;
  lock_waiters_tail = NULL;
  memset(lock_holders, 0, sizeof(lock_holders));
  result = misc_register(&kfmlp_dev);
  if (result != 0) {
    printk("Could not allocate %s device: %d\n", DEVICE_NAME, result);
    return result;
  }
  printk("KFMLP module loaded OK.\n");
  return 0;
}

static void __exit CleanupModule(void) {
  // TODO: Release all waiters prior to cleanup? Does Linux prevent unloading
  // if the device handles aren't closed?
  misc_deregister(&kfmlp_dev);
  printk("KFMLP module unloaded.\n");
}

module_init(InitModule);
module_exit(CleanupModule);
MODULE_LICENSE("GPL");

