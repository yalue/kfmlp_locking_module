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

// The maximum 'k' value supported by the module.
#define KFMLP_MAX_K (32)

// The name of the character device.
#define DEVICE_NAME "kfmlp_module"

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
static unsigned int kfmlp_k;

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

static void LockModule(void) {
  mutex_lock(&module_mutex);
}

static void UnlockModule(void) {
  mutex_unlock(&module_mutex);
}

static int KFMLPOpen(struct inode *n, struct file *f) {
  struct task_struct *p = current;
  printk("KFMLP device opened by process %d.\n", p->pid);
  return 0;
}

static int KFMLPRelease(struct inode *n, struct file *f) {
  // TODO: Release the lock if this process held it.
  printk("KFMLP device released by process %d.\n", p->pid);
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
  if ((policy != SCHED_FIFO) && (policy != SCHED_NORMAL)) {
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
  if (!copy_to_user((void __user *) arg, &a, sizeof(a))) {
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
  // TODO (next): SET_K_IOC, LOCK_ACQUIRE_IOC, LOCK_RELEASE_IOC
  //  - Make sure that kfmlp_k is nonzero first. (Set in SET_K)
  //    - In SET_K just check that all slots are NULL, if k isn't 0.
  //  - Write enqueue and dequeue helper functions that maintain
  //    lock_waiters_head and lock_waiters_tail properly.
  //  - When first acquiring the lock, see if any slots are NULL; we could take
  //    it immediately. HOWEVER make sure we aren't already in a slot
  //    ourselves.
  //  - When unlocking the lock, remove the thing at the head of the queue and
  //    put it in our (former) slot.
  //  - If we were interrupted, remove ourselves from the list.
  //  - When we acquire a lock, boost our priority using sched_set_fifo
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
  memset(lock_hoders, 0, sizeof(lock_holders));
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
  misc_deregister(&kfmlp_ctrl_dev);
}

module_init(InitModule);
module_exit(CleanupModule);
MODULE_LICENSE("GPL");

