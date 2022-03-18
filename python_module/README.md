A Python 3 C Library for the KFMLP Kernel Module API
====================================================

This library enables interacting with the k-FMLP locking module from Python
scripts.

Usage
-----

First, build and install this module by running `python setup.py install`.
Note that this requires Python 3. Afterwards, you can access the module using
`import kfmlp_control` in your python code. Information about specific
functions can be found using `help(kfmlp_control)` after it has been imported.
Note that all processes using the module must call
`kfmlp_control.reset_module_handle()` prior to using any other library
functions.

You can also run `python test_script.py`, which should spin up 20 concurrent
processes that contend for a lock, and (hopefully) thoroughly test all of the
ioctls in the library.

