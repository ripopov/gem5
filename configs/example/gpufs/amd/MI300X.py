"""MI300X per-CU resources for the reduced eight-CU teaching platform.

Sources and timing-model limitations: gpu-lab/MI300XConfiguration.md.
These are parser defaults so explicit experiment overrides still work.
"""


def setTeachingDefaults(parser):
    parser.set_defaults(
        num_compute_units=8,
        simds_per_cu=4,
        wf_size=64,
        wfs_per_simd=8,
        vreg_file_size=512,
        sreg_file_size=800,
        vreg_min_alloc=16,
        sreg_min_alloc=16,
        lds_size=65536,
        numLdsBanks=32,
        ldsBankConflictPenalty=1,
        tcp_size="32KiB",
        sqc_size="64KiB",
        scalar_cache_size="16KiB",
        cu_per_sqc=2,
        cu_per_scalar_cache=2,
        # 4 MiB * 8 / 38 active CUs, rounded to a power of two.
        tcc_size="1MiB",
        gpu_clock="2.1GHz",
        # The simple allocator owns a SIMD's register file per dispatch and
        # cannot co-schedule workgroups; the dynamic allocator places waves
        # by actual capacity, which the occupancy-sized register files need.
        reg_alloc_policy="dynamic",
    )
