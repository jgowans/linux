KDIR ?= ../../../

default:
	$(MAKE) -C $(KDIR) M=$$PWD

install: default
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

clean:
	rm -f *.o *.ko Module.* modules.* *.mod.c
