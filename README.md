Kernel Module to Support a Priority-queue Lock for AMD GPU Usage
================================================================
Kernel Module to Provide a K-exclusion variant of the FMLP (Flexible/FIFO
Multiprocessor Locking Protocol) on Linux. This module should run on Vanilla
Linux, but is only usable by `SCHED_FIFO` tasks, possibly with a few other
small restrictions.


Usage
-----

First, compile the module by running `make`.  Next, load the module:
`sudo insmod kfmlp_locking_module.ko`.

More information will follow after I've finished implementing stuff.

