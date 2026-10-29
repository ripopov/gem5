#!/usr/bin/env python3
"""Run persistent-GEMM study variants through tune.py, several at a time.

Exploration runs execute concurrently, so their launcher wall times are not
comparable; GPU-active cycles are simulated time and unaffected. Final
comparisons must be rerun serially with --jobs 1.
"""

import argparse
import concurrent.futures
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[1]
TUNE = WORKSPACE / "experiments/gemm-study/tune.py"

# Shapes: (label, (m, n, k), tile definitions). Every shape gives at least
# four output tiles to each of eight persistent workgroups.
SHAPES = {
    "s512x64-t64": ((256, 512, 64), ["TILE_M=64", "TILE_N=64"]),
    "s512x64-t32": ((256, 512, 64), ["TILE_M=64", "TILE_N=32"]),
    "s512x96-t64": ((256, 512, 96), ["TILE_M=64", "TILE_N=64"]),
    "s256x192-t32": ((256, 256, 192), ["TILE_M=64", "TILE_N=32"]),
    "s256x128-t32": ((256, 256, 128), ["TILE_M=64", "TILE_N=32"]),
    "s512x128-t64": ((256, 512, 128), ["TILE_M=64", "TILE_N=64"]),
}
LDS_WAVE = ["WAVE_M=64", "WAVE_N=16", "PREFETCH_LDS=2", "LDS_PAD=16"]
REG_WAVE = ["WAVE_M=32", "WAVE_N=32"]


def controls(shape_label):
    shape, tile = SHAPES[shape_label]
    variants = []
    lds = tile + LDS_WAVE
    # 1. conventional nonpersistent tiled GEMM, double buffered
    variants.append(
        (
            f"{shape_label}-nonpersist-p2",
            "reference-lds",
            lds + ["PERSISTENT=0", "PIPELINE=2"],
            shape,
        )
    )
    variants.append(
        (
            f"{shape_label}-nonpersist-p1",
            "reference-lds",
            lds + ["PERSISTENT=0", "PIPELINE=1"],
            shape,
        )
    )
    # 2. cooperative persistent, single and double buffer
    coop = ["PRODUCERS=0", "GROUPS=1"]
    variants.append(
        (
            f"{shape_label}-coop-s2",
            "reference-lds",
            lds + coop + ["STAGES=2"],
            shape,
        )
    )
    variants.append(
        (
            f"{shape_label}-coop-s1",
            "reference-lds",
            lds + coop + ["STAGES=1"],
            shape,
        )
    )
    # 5. cache/register only, nonpersistent and persistent
    reg = tile + REG_WAVE
    variants.append(
        (
            f"{shape_label}-reg-nonpersist-p0",
            "reference-regtiles",
            reg + ["PERSISTENT=0", "PIPELINE=0"],
            shape,
        )
    )
    variants.append(
        (
            f"{shape_label}-reg-nonpersist-p1",
            "reference-regtiles",
            reg + ["PERSISTENT=0", "PIPELINE=1"],
            shape,
        )
    )
    variants.append(
        (
            f"{shape_label}-reg-persist-p1",
            "reference-regtiles",
            reg + ["PIPELINE=1"],
            shape,
        )
    )
    return variants


def specialized(
    shape_label,
    producers=(64, 128),
    groups=(1, 2),
    stages=(2, 3, 4),
    prefetch=(1, 2),
):
    shape, tile = SHAPES[shape_label]
    variants = []
    for p in producers:
        for g in groups:
            for s in stages:
                for f in prefetch:
                    variants.append(
                        (
                            f"{shape_label}-spec-p{p}-g{g}-s{s}-f{f}",
                            "reference-lds",
                            tile
                            + LDS_WAVE
                            + [
                                f"PRODUCERS={p}",
                                f"GROUPS={g}",
                                f"STAGES={s}",
                                f"PREFETCH={f}",
                            ],
                            shape,
                        )
                    )
    return variants


def resident(shape_label, workgroups=(16, 24)):
    """Persistent controls with several resident workgroups per CU."""
    shape, tile = SHAPES[shape_label]
    variants = []
    for w in workgroups:
        lds = tile + LDS_WAVE + [f"WORKGROUPS={w}"]
        coop = ["PRODUCERS=0", "GROUPS=1"]
        variants.append(
            (
                f"{shape_label}-coop-s2-w{w}",
                "reference-lds",
                lds + coop + ["STAGES=2"],
                shape,
            )
        )
        variants.append(
            (
                f"{shape_label}-coop-s1-w{w}",
                "reference-lds",
                lds + coop + ["STAGES=1"],
                shape,
            )
        )
        variants.append(
            (
                f"{shape_label}-reg-persist-p1-w{w}",
                "reference-regtiles",
                tile + REG_WAVE + [f"WORKGROUPS={w}", "PIPELINE=1"],
                shape,
            )
        )
        for p, g, st, f in (
            (64, 1, 2, 1),
            (64, 2, 4, 1),
            (128, 1, 2, 1),
            (128, 2, 4, 1),
            (64, 1, 4, 2),
            (128, 2, 4, 2),
        ):
            variants.append(
                (
                    f"{shape_label}-spec-p{p}-g{g}-s{st}-f{f}-w{w}",
                    "reference-lds",
                    lds
                    + [
                        f"PRODUCERS={p}",
                        f"GROUPS={g}",
                        f"STAGES={st}",
                        f"PREFETCH={f}",
                    ],
                    shape,
                )
            )
    return variants


def deep(shape_label, workgroups=(8, 16)):
    """Deeper producer prefetch with more producer waves per stage."""
    shape, tile = SHAPES[shape_label]
    dims = {d.split("=")[0]: int(d.split("=")[1]) for d in tile}
    variants = []
    for w in workgroups:
        lds = tile + LDS_WAVE + [f"WORKGROUPS={w}"]
        for p, g, st, f in (
            (128, 1, 4, 3),
            (128, 2, 4, 3),
            (256, 1, 4, 3),
            (256, 2, 4, 3),
            (256, 1, 6, 4),
            (256, 2, 6, 4),
            (128, 1, 6, 4),
            (256, 1, 5, 4),
        ):
            # Every producer lane must own at least one float4 of each tile.
            if (dims["TILE_M"] * 16) % (p * 4) or (dims["TILE_N"] * 16) % (
                p * 4
            ):
                continue
            variants.append(
                (
                    f"{shape_label}-deep-p{p}-g{g}-s{st}-f{f}-w{w}",
                    "reference-lds",
                    lds
                    + [
                        f"PRODUCERS={p}",
                        f"GROUPS={g}",
                        f"STAGES={st}",
                        f"PREFETCH={f}",
                    ],
                    shape,
                )
            )
    return variants


def occupancy(shape_label):
    """Cap register allocation so several persistent workgroups co-reside."""
    shape, tile = SHAPES[shape_label]
    variants = []
    for w, eu in ((16, 4), (16, 5), (16, 6), (24, 6), (24, 8)):
        lds = tile + LDS_WAVE + [f"WORKGROUPS={w}", f"WAVES_PER_EU={eu}"]
        for p, g, st, f in (
            (128, 1, 4, 2),
            (128, 2, 4, 1),
            (256, 1, 4, 3),
            (256, 2, 4, 2),
            (64, 1, 3, 1),
        ):
            variants.append(
                (
                    f"{shape_label}-occ-p{p}-g{g}-s{st}-f{f}-w{w}-eu{eu}",
                    "reference-lds",
                    lds
                    + [
                        f"PRODUCERS={p}",
                        f"GROUPS={g}",
                        f"STAGES={st}",
                        f"PREFETCH={f}",
                    ],
                    shape,
                )
            )
        variants.append(
            (
                f"{shape_label}-occ-coop-s2-w{w}-eu{eu}",
                "reference-lds",
                lds + ["PRODUCERS=0", "GROUPS=1", "STAGES=2"],
                shape,
            )
        )
    for eu in (4, 6):
        variants.append(
            (
                f"{shape_label}-occ-nonpersist-p1-eu{eu}",
                "reference-lds",
                tile
                + LDS_WAVE
                + ["PERSISTENT=0", "PIPELINE=1", f"WAVES_PER_EU={eu}"],
                shape,
            )
        )
        variants.append(
            (
                f"{shape_label}-occ-reg-nonpersist-p1-eu{eu}",
                "reference-regtiles",
                tile
                + REG_WAVE
                + ["PERSISTENT=0", "PIPELINE=1", f"WAVES_PER_EU={eu}"],
                shape,
            )
        )
    return variants


def spin(shape_label):
    """Polling interval of the specialised pipeline's waiting waves."""
    shape, tile = SHAPES[shape_label]
    variants = []
    for w in (16, 24):
        for sleep in (0, 2, 4, 8):
            variants.append(
                (
                    f"{shape_label}-spin{sleep}-p128-g2-s4-f1-w{w}",
                    "reference-lds",
                    tile
                    + LDS_WAVE
                    + [
                        f"WORKGROUPS={w}",
                        "PRODUCERS=128",
                        "GROUPS=2",
                        "STAGES=4",
                        "PREFETCH=1",
                        f"SPIN_SLEEP={sleep}",
                    ],
                    shape,
                )
            )
    return variants


# Candidate final shapes: every persistent workgroup gets at least four
# output tiles and the naive sample stays near the two-to-three-minute budget.
SELECT = {
    # label: (shape, tile defines, workgroup counts)
    "f384x128-t32": (
        (256, 384, 128),
        ["TILE_M=64", "TILE_N=32", "WAVE_M=64", "WAVE_N=16"],
        (12, 16),
    ),
    "f512x128-t32": (
        (256, 512, 128),
        ["TILE_M=64", "TILE_N=32", "WAVE_M=64", "WAVE_N=16"],
        (16, 24),
    ),
    "f256x128-t16": (
        (256, 256, 128),
        ["TILE_M=32", "TILE_N=32", "WAVE_M=32", "WAVE_N=16"],
        (16, 24),
    ),
    "f256x128-t32": (
        (256, 256, 128),
        ["TILE_M=64", "TILE_N=32", "WAVE_M=64", "WAVE_N=16"],
        (8, 16),
    ),
    "f256x160-t32": (
        (256, 256, 160),
        ["TILE_M=64", "TILE_N=32", "WAVE_M=64", "WAVE_N=16"],
        (8, 16),
    ),
}
LDS_LAYOUT = ["PREFETCH_LDS=2", "LDS_PAD=16"]


def select(label):
    shape, tile, workgroups = SELECT[label]
    lds = tile + LDS_LAYOUT
    reg = [d for d in tile if d.startswith("TILE")] + REG_WAVE
    variants = [
        (
            f"{label}-nonpersist-p1",
            "reference-lds",
            lds + ["PERSISTENT=0", "PIPELINE=1"],
            shape,
        ),
        (
            f"{label}-nonpersist-p2",
            "reference-lds",
            lds + ["PERSISTENT=0", "PIPELINE=2"],
            shape,
        ),
        (
            f"{label}-reg-nonpersist-p1",
            "reference-regtiles",
            reg + ["PERSISTENT=0", "PIPELINE=1"],
            shape,
        ),
    ]
    for w in workgroups:
        variants += [
            (
                f"{label}-coop-s1-w{w}",
                "reference-lds",
                lds
                + ["PRODUCERS=0", "GROUPS=1", "STAGES=1", f"WORKGROUPS={w}"],
                shape,
            ),
            (
                f"{label}-coop-s2-w{w}",
                "reference-lds",
                lds
                + ["PRODUCERS=0", "GROUPS=1", "STAGES=2", f"WORKGROUPS={w}"],
                shape,
            ),
            (
                f"{label}-reg-persist-p1-w{w}",
                "reference-regtiles",
                reg + ["PIPELINE=1", f"WORKGROUPS={w}"],
                shape,
            ),
        ]
        for p, g, st, f in (
            (128, 2, 4, 1),
            (64, 2, 4, 1),
            (64, 1, 4, 2),
            (128, 1, 4, 2),
            (64, 2, 3, 1),
        ):
            if (int(tile[0].split("=")[1]) * 16) % (p * 4) or (
                int(tile[1].split("=")[1]) * 16
            ) % (p * 4):
                continue
            variants.append(
                (
                    f"{label}-spec-p{p}-g{g}-s{st}-f{f}-w{w}",
                    "reference-lds",
                    lds
                    + [
                        f"PRODUCERS={p}",
                        f"GROUPS={g}",
                        f"STAGES={st}",
                        f"PREFETCH={f}",
                        f"WORKGROUPS={w}",
                    ],
                    shape,
                )
            )
    return variants


def tune_a():
    """Tune the eight-workgroup specialised pipeline at 256x256x128."""
    shape = (256, 256, 128)
    base = [
        "TILE_M=64",
        "TILE_N=32",
        "WAVE_M=64",
        "WAVE_N=16",
        "PREFETCH_LDS=2",
        "LDS_PAD=16",
        "WORKGROUPS=8",
    ]
    configs = {
        "p128-g2-s6-f1": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=6",
            "PREFETCH=1",
        ],
        "p128-g2-s6-f2": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=6",
            "PREFETCH=2",
        ],
        "p128-g2-s4-f2": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=2",
        ],
        "p128-g2-s4-f1-order1": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=1",
            "ORDER=1",
        ],
        "p128-g2-s4-f1-spin2": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=1",
            "SPIN_SLEEP=2",
        ],
        "p128-g2-s4-f1-lds1": [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=1",
            "PREFETCH_LDS=1",
        ],
        "p128-g3-s6-f1": [
            "PRODUCERS=128",
            "GROUPS=3",
            "STAGES=6",
            "PREFETCH=1",
        ],
        "p64-g3-s6-f1": ["PRODUCERS=64", "GROUPS=3", "STAGES=6", "PREFETCH=1"],
        "p128-g1-s6-f3": [
            "PRODUCERS=128",
            "GROUPS=1",
            "STAGES=6",
            "PREFETCH=3",
        ],
        "p64-g2-s6-f2": ["PRODUCERS=64", "GROUPS=2", "STAGES=6", "PREFETCH=2"],
    }
    variants = [
        (f"tunea-{k}", "reference-lds", base + v, shape)
        for k, v in configs.items()
    ]
    variants.append(
        (
            "tunea-coop-s2-w8-order1",
            "reference-lds",
            base + ["PRODUCERS=0", "GROUPS=1", "STAGES=2", "ORDER=1"],
            shape,
        )
    )
    variants.append(
        (
            "tunea-nonpersist-p2-lds1",
            "reference-lds",
            base[:-1] + ["PERSISTENT=0", "PIPELINE=2", "PREFETCH_LDS=1"],
            shape,
        )
    )
    return variants


STUDIES = {
    "tuneA": tune_a,
    "select": lambda: [v for label in SELECT for v in select(label)],
    "spin-512x64": lambda: spin("s512x64-t64"),
    "occupancy-512x64": lambda: occupancy("s512x64-t64"),
    "occupancy-512x96": lambda: occupancy("s512x96-t64"),
    "occupancy-256x192": lambda: occupancy("s256x192-t32"),
    "deep-512x64": lambda: deep("s512x64-t64"),
    "deep-512x96": lambda: deep("s512x96-t64"),
    "deep-256x192": lambda: deep("s256x192-t32"),
    "resident-512x64": lambda: resident("s512x64-t64"),
    "resident-512x96": lambda: resident("s512x96-t64"),
    "resident-256x192": lambda: resident("s256x192-t32"),
    "resident-256x128": lambda: resident("s256x128-t32"),
    "controls": lambda: [v for s in SHAPES for v in controls(s)],
    "controls-fast": lambda: [
        v for s in SHAPES if s != "s512x128-t64" for v in controls(s)
    ],
    "spec-512x64": lambda: specialized("s512x64-t64"),
    "spec-512x96": lambda: specialized("s512x96-t64"),
    "spec-256x192": lambda: specialized("s256x192-t32"),
    "spec-256x128": lambda: specialized("s256x128-t32"),
    "spec-512x128": lambda: specialized("s512x128-t64"),
}


def run_variant(variant, args):
    name, kind, defines, shape = variant
    command = [
        sys.executable,
        str(TUNE),
        args.prefix + name,
        kind,
        "--shape",
        *map(str, shape),
        "--repeat",
        str(args.repeat),
    ]
    for definition in defines:
        command += ["--define", definition]
    if args.build_only:
        command.append("--build-only")
    for extra in args.gem5_arg:
        command.append("--gem5-arg=" + extra)
    for extra in args.config_arg:
        command.append("--config-arg=" + extra)
    log = WORKSPACE / "work/gemm-study" / (args.prefix + name)
    log.mkdir(parents=True, exist_ok=True)
    with (log / "explore.log").open("w") as stream:
        try:
            completed = subprocess.run(
                command,
                cwd=WORKSPACE,
                stdout=stream,
                stderr=stream,
                timeout=args.timeout,
            )
            code = completed.returncode
        except subprocess.TimeoutExpired:
            # A crashed simulator leaves the client waiting forever; remove
            # the run's containers so the pool can continue.
            stream.write("\nexplore: timeout, removing containers\n")
            containers = subprocess.run(
                [
                    "docker",
                    "ps",
                    "-aq",
                    "--filter",
                    f"name=gemm-{args.prefix}{name}-",
                ],
                capture_output=True,
                text=True,
            ).stdout.split()
            if containers:
                subprocess.run(
                    ["docker", "rm", "-f", *containers],
                    stdout=subprocess.DEVNULL,
                )
            code = "timeout"
    return name, code


def summarize(prefix):
    rows = []
    for directory in sorted(
        (WORKSPACE / "work/gemm-study").glob(prefix + "*")
    ):
        results = sorted(directory.glob("result-*.json"))
        if not results:
            continue
        cycles = []
        for path in results:
            record = json.loads(path.read_text())
            if record.get("gpu_active_cycles") is not None:
                cycles.append(record["gpu_active_cycles"])
        record = json.loads(results[-1].read_text())
        status = record.get("failure") or record.get("skipped") or "ok"
        resources = record.get("resources", {})
        rows.append(
            (
                directory.name,
                "x".join(map(str, record["shape"])),
                status,
                [round(c) for c in cycles],
                resources.get("next_free_vgpr"),
                resources.get("group_segment_fixed_size"),
            )
        )
    for row in rows:
        print(*row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("study", choices=sorted(STUDIES) + ["summary"])
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--prefix", default="px-")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--gem5-arg", action="append", default=[])
    parser.add_argument(
        "--config-arg",
        action="append",
        default=[],
        help="gem5 configuration option, e.g. --reg-alloc-policy=dynamic; "
        "the default simple allocator asserts when several workgroups "
        "share a CU (see the study notes)",
    )
    parser.add_argument("--only", help="substring filter on variant names")
    parser.add_argument(
        "--timeout",
        type=float,
        default=1500,
        help="seconds per variant before its run is killed",
    )
    args = parser.parse_args()
    if args.study == "summary":
        summarize(args.prefix)
        return
    variants = STUDIES[args.study]()
    if args.only:
        variants = [v for v in variants if args.only in v[0]]
    print(f"{len(variants)} variants, {args.jobs} concurrent", flush=True)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, code in pool.map(lambda v: run_variant(v, args), variants):
            print(
                ("ok   " if code == 0 else f"FAIL({code}) ") + name, flush=True
            )
            failures += code != 0
    # tune.py runs each variant sequentially, so an aborted run can leave a
    # dangling compose project behind its own client process.
    summarize(args.prefix)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
