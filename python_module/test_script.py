# This is a quick script for testing the module and lock contention between
# multiple processes.
import kfmlp_control
import multiprocessing
import random
import time

def task_entry(task_num):
    kfmlp_control.reset_module_handle()
    kfmlp_control.start_sched_fifo()
    # Sleep a random amount of time before releasing.
    time.sleep(2.0 * random.random())
    print("Task %d waiting for release." % (task_num,))
    kfmlp_control.wait_for_ts_release()
    for i in range(10):
        slot = kfmlp_control.acquire_lock()
        print("Task %d in lock slot %d." % (task_num, slot))
        time.sleep(random.random() * 0.5)
        kfmlp_control.release_lock()
    kfmlp_control.end_sched_fifo()

def main():
    kfmlp_control.reset_module_handle()
    kfmlp_control.set_k(3)
    processes = []
    process_count = 20
    for i in range(process_count):
        p = multiprocessing.Process(target=task_entry, args=(i,))
        p.start()
        processes.append(p)
    print("Waiting for processes to be ready.")
    while kfmlp_control.get_ts_waiter_count() != process_count:
        time.sleep(0.05)
    print("All tasks ready. Releasing.")
    kfmlp_control.start_sched_fifo()
    kfmlp_control.release_ts()
    for p in processes:
        p.join()
    print("All processes complete OK")

if __name__ == "__main__":
    main()

