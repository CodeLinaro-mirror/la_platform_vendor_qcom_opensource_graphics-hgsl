LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true

AUTO_GVM_TARGETS += $(CONFIG_ARCH_QTI_VM)
AUTO_GVM_TARGETS += $(CONFIG_QTI_QUIN_GVM)

ifneq (, $(filter y, $(AUTO_GVM_TARGETS)))
HGSL_SELECT := CONFIG_QCOM_HGSL=m

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# This makefile is only for DLKM
ifneq ($(findstring vendor,$(LOCAL_PATH)),)

DLKM_DIR   := device/qcom/common/dlkm

KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTIONS += $(HGSL_SELECT)
KBUILD_OPTIONS += MODNAME=qcom_hgsl

LOCAL_CFLAGS := -Wno-unused-parameter -Wno-unused-variable 

include $(CLEAR_VARS)
LOCAL_EXPORT_C_INCLUDE_DIRS += $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDE_DIRS += $(LOCAL_PATH)/include/uapi
LOCAL_EXPORT_C_INCLUDE_DIRS += $(LOCAL_PATH)/include/uapi/linux

# For incremental compilation
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := qcom_hgsl.ko
LOCAL_MODULE_KBUILD_NAME  := qcom_hgsl.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)

# Include qcom_hgsl.ko in the /vendor/lib/modules (vendor.img)
BOARD_VENDOR_KERNEL_MODULES += $(LOCAL_MODULE_PATH)/$(LOCAL_MODULE)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

endif # DLKM check
endif # ifneq (, $(filter y, $(AUTO_GVM_TARGETS)))
