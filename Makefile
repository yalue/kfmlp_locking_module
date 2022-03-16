obj-m += gpu_locking_module.o

gpu_locking_module-y := module_entry.o binheap.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

