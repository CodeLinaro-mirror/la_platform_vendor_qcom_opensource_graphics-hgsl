# SPDX-License-Identifier: GPL-2.0-only

CONFIG_QCOM_HGSL = m

KDIR := $(TOP)/kernel_platform/common

ifeq ($(HGSL_PATH),)
HGSL_PATH=$(src)
endif

include $(HGSL_PATH)/config/hgsl.conf

ccflags-y += -I$(HGSL_PATH)  \
            -I$(HGSL_PATH)/include \
            -I$(HGSL_PATH)/include/linux \
            -Wno-unused-function \
            -Wno-unused-parameter \
            -Wno-unused-variable

obj-$(CONFIG_QCOM_HGSL) += qcom_hgsl.o

qcom_hgsl-y = hgsl.o \
            hgsl_gmugos.o \
            hgsl_hyp.o \
            hgsl_hyp_socket.o \
            hgsl_memory.o \
            hgsl_sync.o

qcom_hgsl-$(CONFIG_QCOM_HGSL_TCSR_SIGNAL) += hgsl_tcsr.o
qcom_hgsl-$(CONFIG_SYSFS) += hgsl_sysfs.o
qcom_hgsl-$(CONFIG_DEBUG_FS) += hgsl_debugfs.o
