About
=====

This directory contains a Python module that exposes the functions provided by
the GPU locking module in python code.  It was initially intended to be used to
set deadlines between "frames" of processing in PyTorch applications.

Prerequisites
-------------

This module requires the installation of the kernel module defined in the
parent directory, and therefore shares all of the same prerequisites.

Installation
------------

From within this directory, run `python setup.py install`.

Usage
-----

In your python code, run `import gpu_lock_control`. More information can be
seen via `help(gpu_lock_control)`.

