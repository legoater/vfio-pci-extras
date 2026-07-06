KDIR ?= /lib/modules/`uname -r`/build

all modules:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

modules_install install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install
