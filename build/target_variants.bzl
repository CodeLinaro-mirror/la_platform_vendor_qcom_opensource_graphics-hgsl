targets = [
    # keep sorted
    "autogvm",
]

la_variants = [
    # keep sorted
    "consolidate",
    "perf",
    "debug-defconfig",
    "defconfig",
]

def get_all_la_variants():
    tv = [(t, v) for t in targets for v in la_variants]

    return tv
