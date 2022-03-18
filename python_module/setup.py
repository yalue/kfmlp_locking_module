from distutils.core import setup, Extension

kfmlp_control_module = Extension(
    "kfmlp_control",
    sources=["kfmlp_control.c"]
)
description = "Functions for interacting with the KFMLP kernel module."
setup(name="KFMLP Control", version="1.0", description=description,
    ext_modules=[kfmlp_control_module])

