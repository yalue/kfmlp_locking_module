Linux Kernel Module to Support a K-FMLP Locking
===============================================
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

More information will follow after I've finished implementing stuff.

