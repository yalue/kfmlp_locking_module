Linux Kernel Module to Support a K-FMLP Locking
===============================================

First of all, note that this module is intended to be used primarily for my own
research, and may have some design choices that reflect my specific needs.

Kernel Module to Provide a K-exclusion variant of the FMLP (Flexible/FIFO
Multiprocessor Locking Protocol) on Linux. (In English: up to _k_ processes
can hold the lock at a time, and any additional processes wait for one of the
_k_ slots in FIFO order.)

This module should run on Vanilla Linux, but is only usable by `SCHED_FIFO`
tasks, possibly with a few other small restrictions.

Usage
-----

First, compile the module by running `make`.  Next, load the module:
`sudo insmod kfmlp_locking_module.ko`.

Userspace programs interact with the module via the chardev at
`/dev/kfmlp_module`. You can look at the ioctls defined in `kfmlp_module.h`,
or, more easily, use the python interface defined in the `python_module/`
directory (which contains its own README).

