ifeq ($(HGSL_MODULE_ROOT),)
CUR_MKFILE = $(abspath $(lastword $(MAKEFILE_LIST)))
HGSL_MODULE_ROOT = $(dir $(CUR_MKFILE))
endif

KBUILD_OPTIONS+=HGSL_PATH=$(HGSL_MODULE_ROOT)

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	rm -f *.cmd *.d *.mod *.o *.ko *.mod.c *.mod.o Module.symvers modules.order

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
