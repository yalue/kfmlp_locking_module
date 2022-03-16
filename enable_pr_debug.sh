#!/bin/bash
#
# This script will enable the pr_debug statements in the module. It's in a
# script mostly so I don't forget the syntax.

echo -n "module kfmlp_locking_module +pflm" > /sys/kernel/debug/dynamic_debug/control

# To disable:
# echo -n "module kfmlp_locking_module -p" > /sys/kernel/debug/dynamic_debug/control

