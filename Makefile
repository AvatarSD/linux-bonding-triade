# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build for the Triade ring driver.
#
#   make            # build triade.ko against the running kernel
#   make clean
#   sudo insmod triade.ko   /   sudo rmmod triade

KDIR ?= /lib/modules/$(shell uname -r)/build

# This is a native x86_64 build. Override any CROSS_COMPILE/ARCH/CC leaking in
# from the login profile. CC defaults to plain `gcc` (matches the distro
# kernel on most boxes); the dev workstation overrides it via `make CC=gcc-13`
# (or whatever matches its running kernel) when needed.
ARCH ?= x86_64
CROSS_COMPILE :=
CC ?= gcc
KBUILD_ARGS := ARCH=$(ARCH) CROSS_COMPILE= CC=$(CC)

obj-m += triade.o
triade-objs := src/triade_main.o src/triade_device.o src/triade_slave.o \
	       src/triade_forward.o src/triade_framereg.o src/triade_localreg.o \
	       src/triade_super.o src/triade_sched.o src/triade_debugfs.o

# Let the build find headers in src/ relative to each object.
ccflags-y := -I$(src)/src

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

.PHONY: all clean
