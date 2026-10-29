#!/usr/bin/env python3
"""Check instantiated eight-CU MI300X teaching resources, not parser defaults."""

import argparse
import configparser
import json
from pathlib import Path


def check(path):
    config = configparser.ConfigParser()
    config.read(path / "config.ini")

    def expect(section, key, value):
        actual = config[section][key]
        assert float(actual) == value, (section, key, actual, value)

    def period(section):
        return int(config[config[section]["clk_domain"]]["clock"])

    cus = [s for s in config if config[s].get("type") == "ComputeUnit"]
    assert len(cus) == 8, len(cus)
    shaders = [s for s in config if config[s].get("type") == "Shader"]
    assert len(shaders) == 1 and period(shaders[0]) == 476
    for section in config:
        if config[section].get("type") == "RocjitsuKmdServer":
            for key, value in dict(
                simd_count=32,
                simd_per_cu=4,
                max_waves_per_simd=8,
                wave_front_size=64,
                lds_size_kb=64,
                num_xcc=1,
            ).items():
                expect(section, key, value)
    for cu in cus:
        for key, value in dict(
            num_SIMDs=4, wf_size=64, n_wf=8, simd_width=16, issue_period=4
        ).items():
            expect(cu, key, value)
        # 1 ps tick resolution rounds the requested 2.1 GHz period to 476 ps.
        assert period(cu) == 476, (cu, period(cu))
        assert len(config[cu]["wavefronts"].split()) == 32
        for kind, capacity in [("vector", 512), ("scalar", 800)]:
            files = config[cu][kind + "_register_file"].split()
            assert len(files) == 4
            for file in files:
                expect(file, "num_regs", capacity)
        manager = config[cu]["register_manager"]
        for kind, capacity in [("vrf", 512), ("srf", 800)]:
            pools = config[manager][kind + "_pool_managers"].split()
            assert len(pools) == 4
            for pool in pools:
                expect(pool, "pool_size", capacity)
                expect(pool, "min_alloc", 16)
        lds = config[cu]["localDataStore"]
        expect(lds, "size", 65536)
        expect(lds, "banks", 32)
        expect(lds, "bankConflictPenalty", 1)
        assert period(lds) == period(cu)
        assert period(cu + ".ldsBus") == period(cu)

    groups = {}
    for prefix, count, size in [
        ("tcp", 8, 32768),
        ("sqc", 4, 65536),
        ("scalar", 4, 16384),
        ("tcc", 1, 1048576),
    ]:
        caches = [
            s
            for s in config
            if s.startswith("system.ruby." + prefix + "_cntrl")
            and s.endswith(".L2cache" if prefix == "tcc" else ".L1cache")
        ]
        assert len(caches) == count, (prefix, caches)
        for cache in caches:
            expect(cache, "size", size)
        groups[prefix] = dict(count=count, bytes_each=size)
    latency = {
        k: v
        for k, v in config[cus[0]].items()
        if any(
            part in k
            for part in ("latency", "length", "width", "period", "mfma")
        )
    }
    memory_timing = {}
    for section in config:
        if section in (
            "system.ruby.tcp_cntrl0",
            "system.ruby.sqc_cntrl0",
            "system.ruby.scalar_cntrl0",
            "system.ruby.tcc_cntrl0",
            "system.ruby.gpu_dir_cntrl0",
            "system.gpu_mem_ctrls.dram",
        ) or section in (
            "system.ruby.tcp_cntrl0.L1cache",
            "system.ruby.sqc_cntrl0.L1cache",
            "system.ruby.scalar_cntrl0.L1cache",
            "system.ruby.tcc_cntrl0.L2cache",
            "system.ruby.gpu_dir_cntrl0.L3CacheMemory",
        ):
            memory_timing[section] = {
                k: v
                for k, v in config[section].items()
                if "latency" in k
                or k
                in (
                    "tck",
                    "tcl",
                    "trcd",
                    "trp",
                    "tburst",
                    "size",
                    "assoc",
                    "device_bus_width",
                    "burst_length",
                )
            }
            if "clk_domain" in config[section]:
                memory_timing[section]["clock_period_ticks"] = period(section)
    return dict(
        run=path.name,
        cus=len(cus),
        gpu_period_ticks=period(cus[0]),
        vgpr_slots_per_simd=512,
        sgprs_per_simd=800,
        caches=groups,
        cu_timing_parameters=latency,
        memory_timing_parameters=memory_timing,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    results = [check(path) for path in args.runs]
    output = json.dumps(results, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
