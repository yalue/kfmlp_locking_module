IOCTL Tool
==========

This directory contains code for a simple command-line utility I made to test
the AMDKFD ioctls I hack into the kernel.


Compilation
-----------

You will need to be running a patched version of the Linux kernel, following
the instructions in the parent directory of this project (containing the kernel
patches).

Second, just navigate to this directory and run `make`.


Usage
-----

After compiling the tool, run it using `./ioctl_tool`. Run `./ioctl_tool -h`
to print a list of command-line options to stdout.

