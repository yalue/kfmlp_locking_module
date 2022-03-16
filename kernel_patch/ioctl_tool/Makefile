.PHONY: all clean

CFLAGS := -Wall -Werror -O3 -g -fPIC

all: ioctl_tool

amdkfd_ioctl_helper.o: amdkfd_ioctl_helper.c amdkfd_ioctl_helper.h
	gcc $(CFLAGS) -c -o amdkfd_ioctl_helper.o amdkfd_ioctl_helper.c

ioctl_tool: ioctl_tool.c amdkfd_ioctl_helper.o
	gcc $(CFLAGS) -o ioctl_tool ioctl_tool.c amdkfd_ioctl_helper.o

hip_tester.o: hip_tester.cpp amdkfd_ioctl_helper.h
	hipcc $(CFLAGS) -c -o hip_tester.o hip_tester.cpp

hip_tester: hip_tester.o amdkfd_ioctl_helper.o
	hipcc $(CFLAGS) -o hip_tester hip_tester.o amdkfd_ioctl_helper.o

clean:
	rm -f ioctl_tool
	rm -f hip_tester
	rm -f *.o

