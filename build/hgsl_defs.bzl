load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load(":build/target_variants.bzl", "get_all_la_variants")

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
        "hgsl_events.c",
        "hgsl_drawobj.c",
        "hgsl_dispatch.c",
    ]

    srcs = srcs + native.glob(["*.h"]) + qcom_hgsl_includes

    return srcs

def define_target_variant_module(target, variant):
    tv = "{}_{}".format(target, variant)
    rule_name = "{}_qcom_hgsl".format(tv)
    kernel_build = select({
        "//build/kernel/kleaf:socrepo_true": "//soc-repo:{}_base_kernel".format(tv),
        "//build/kernel/kleaf:socrepo_false": "//msm-kernel:{}".format(tv),
    })
    ddk_deps = select({
        "//build/kernel/kleaf:socrepo_true": [
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/soc/qcom/hab/msm_hab".format(tv),
            "//soc-repo:{}/drivers/soc/qcom/secure_buffer".format(tv),
        ],
        "//build/kernel/kleaf:socrepo_false": ["//msm-kernel:all_headers"],
    })
    defconfig = "config/{}_hgslconf".format(target)

    ddk_module(
        name = rule_name,
        out = "qcom_hgsl.ko",
        srcs = hgsl_get_srcs(),
        copts = ["-Wno-format", "-Wno-incompatible-function-pointer-types"],
        defconfig = defconfig,
        kconfig = "Kconfig",
        conditional_srcs = {
            "CONFIG_QCOM_HGSL_TCSR_SIGNAL": {True: ["hgsl_tcsr.c"]},
            "CONFIG_SYSFS": {True: ["hgsl_sysfs.c"]},
            "CONFIG_DEBUG_FS": {True: ["hgsl_debugfs.c"]},
        },
        deps = ddk_deps,
        includes = ["include", "."],
        kernel_build = kernel_build,
        visibility = ["//visibility:private"],
    )

    copy_to_dist_dir(
        name = "{}_dist".format(rule_name),
        data = [rule_name],
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )

def define_target_module(target):
    for target, variant in get_all_la_variants():
        define_target_variant_module(target, variant)
