# SPDX-License-Identifier: GPL-2.0-only

AUTO_GVM_TARGETS += $(CONFIG_ARCH_QTI_VM)
AUTO_GVM_TARGETS += $(CONFIG_QTI_QUIN_GVM)

ifneq (, $(filter y, $(AUTO_GVM_TARGETS)))
CONFIG_QCOM_HGSL = m
endif # ifneq (, $(filter y, $(AUTO_GVM_TARGETS)))

KDIR := $(TOP)/kernel_platform/common

ifeq ($(HGSL_PATH),)
HGSL_PATH=$(src)
endif

$(info echo source path is $(src))

include $(HGSL_PATH)/config/gki_lemans.conf

ccflags-y += -I$(HGSL_PATH)  -I$(HGSL_PATH)/include -I$(HGSL_PATH)/include/linux -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
ccflags += -I$(HGSL_PATH)/include

LOCAL_CFLAGS :+ -Wno-unused-parameter -Wno-unused-variable 

LOCAL_EXPORT_C_INCLUDE_DIRS += $(LOCAL_PATH)/include

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

