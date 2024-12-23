load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")

print("Loading : hgsl_defs.bzl")

qcom_hgsl_includes = [
    "include/uapi/linux/hgsl.h",
]

def hgsl_get_srcs():
    srcs = [
        "hgsl.c",
        "hgsl_gmugos.c",
        "hgsl_hyp.c",
        "hgsl_hyp_socket.c",
        "hgsl_memory.c",
        "hgsl_sync.c",
    ]

    srcs = srcs + native.glob(["*.h"]) + qcom_hgsl_includes

    return srcs

def define_target_variant_module(target, variant):
    tv = "{}_{}".format(target, variant)
    rule_name = "{}_qcom_hgsl".format(tv)
    kernel_build = "//msm-kernel:{}".format(tv)
    defconfig = "config/{}_gpuconf".format(tv)
    defconfig_hdr = "{}.h".format(defconfig)

    ddk_module(
        name = rule_name,
        out = "qcom_hgsl.ko",
        srcs = hgsl_get_srcs() + [ defconfig_hdr ],
        copts = [ "-include", defconfig_hdr ],
        defconfig = defconfig,
        kconfig = "Kconfig",
        conditional_srcs = {
            "CONFIG_QCOM_HGSL_TCSR_SIGNAL": { True: [ "hgsl_tcsr.c" ] },
            "CONFIG_SYSFS": { True: [ "hgsl_sysfs.c" ] },
            "CONFIG_DEBUG_FS": { True: [ "hgsl_debugfs.c" ] },
        },
        deps = [
            "//msm-kernel:all_headers" ],
        includes = ["include", "."],
        kernel_build = kernel_build,
        visibility = ["//visibility:private"]
    )

    copy_to_dist_dir(
        name = "{}_dist".format(rule_name),
        data = [rule_name, "hgsl_kernel_headers"] + qcom_hgsl_includes,
        dist_dir = "out/graphics-hgsl",
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )

    genrule(
    name = "copy_headers",
    srcs = glob(["path/to/your/headers/*.h"]),
    outs = ["hgsl_header_copied"],
    cmd = "cp $(SRCS) /usr/include/linux/ && touch $@",
    )

def define_target_module(target):
    define_target_variant_module(target, "gki")
    define_target_variant_module(target, "consolidate")
