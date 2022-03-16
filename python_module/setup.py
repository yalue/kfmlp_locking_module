from distutils.core import setup, Extension

gpu_lock_control_module = Extension(
    "gpu_lock_control",
    sources=["gpu_lock_control.c"]
)
description = "Functions for controlling the GPU locking kernel module."
setup(name="GPU-lock Control", version="1.0", description=description,
    ext_modules=[gpu_lock_control_module])

