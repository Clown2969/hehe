# SimpleFS - Toplevel Makefile

.PHONY: all kernel userspace clean

all: kernel userspace

kernel:
	$(MAKE) -C src

userspace:
	$(MAKE) -C userspace

clean:
	$(MAKE) -C src clean
	$(MAKE) -C userspace clean
