#!/usr/bin/env python3
"""Run the final register-width and pipeline-depth comparisons serially."""

import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    study = parser.add_mutually_exclusive_group()
    study.add_argument(
        "--publication",
        action="store_true",
        help="compare late publication with existing midpoint controls",
    )
    study.add_argument(
        "--fragments",
        action="store_true",
        help="let the compiler schedule LDS fragment loads freely",
    )
    study.add_argument(
        "--wave-tiles",
        action="store_true",
        help="compare rectangular MFMA tiles within each wave",
    )
    study.add_argument(
        "--larger",
        action="store_true",
        help="test deeper K without shrinking the output grid",
    )
    study.add_argument(
        "--producer-controls",
        action="store_true",
        help="separate producer specialization from double buffering",
    )
    study.add_argument(
        "--lookahead",
        action="store_true",
        help="compare two-tile global prefetch on the small grids",
    )
    study.add_argument(
        "--small-registers",
        action="store_true",
        help="tune cache-only MFMA wave geometry on compact grids",
    )
    study.add_argument(
        "--native",
        action="store_true",
        help="compare finalists with accumulator-resident loops",
    )
    study.add_argument(
        "--native-layouts",
        action="store_true",
        help="compare padding and XOR on native compact LDS tiles",
    )
    study.add_argument(
        "--native-fragments",
        action="store_true",
        help="prefetch current LDS operands ahead of computation",
    )
    study.add_argument(
        "--native-deep",
        action="store_true",
        help="test eight deeper K tiles and stronger cache prefetch",
    )
    study.add_argument(
        "--native-large-tile",
        action="store_true",
        help="test full-LDS tiles and quantify the runtime tradeoff",
    )
    study.add_argument(
        "--final-shape",
        action="store_true",
        help="finish cache-prefetch and LDS-layout tuning at 128x256x128",
    )
    args = parser.parse_args()
    tune = Path(__file__).with_name("tune.py")
    variants = []
    for label, shape in (("k256", (128, 128, 256)), ("k512", (64, 128, 512))):
        for rn, rm, unroll in ((8, 4, 4), (8, 4, 8), (4, 8, 4), (4, 4, 4)):
            variants.append(
                (
                    f"{label}-reg-r{rm}n{rn}u{unroll}",
                    "regtiles",
                    [
                        f"REG_N={rn}",
                        f"REG_M={rm}",
                        f"K_UNROLL={unroll}",
                        "PREFETCH=0",
                    ],
                    shape,
                )
            )
        for bk in (16, 32):
            for pipeline in (1, 2):
                variants.append(
                    (
                        f"{label}-lds64-k{bk}p{pipeline}",
                        "lds",
                        [f"TILE_K={bk}", f"PIPELINE={pipeline}"],
                        shape,
                    )
                )
        for kind in ("mfma-lds", "mfma-regtiles"):
            for pipeline in ((1, 2) if kind == "mfma-lds" else (0, 1)):
                variants.append(
                    (
                        f"{label}-{kind}-p{pipeline}",
                        kind,
                        [f"PIPELINE={pipeline}"],
                        shape,
                    )
                )
    if args.publication:
        variants = [
            (
                f"{label}-lds-publish{position}",
                "lds",
                [f"PUBLISH_K={position}"],
                shape,
            )
            for label, shape in (
                ("k128", (128, 256, 128)),
                ("k256", (128, 128, 256)),
                ("k512", (64, 128, 512)),
            )
            for position in (12, 15)
        ]
    elif args.fragments:
        variants = [
            (
                f"{label}-lds-free-fragments-p{pipeline}",
                "lds",
                ["SCHEDULE_FRAGMENTS=0", f"PIPELINE={pipeline}"],
                shape,
            )
            for label, shape in (
                ("k128", (128, 256, 128)),
                ("k256", (128, 128, 256)),
            )
            for pipeline in (1, 2)
        ]
    elif args.wave_tiles:
        variants = [
            (
                f"mfma-wave{wm}x{wn}-{kind}-p{pipeline}",
                f"mfma-{kind}",
                [f"WAVE_M={wm}", f"WAVE_N={wn}", f"PIPELINE={pipeline}"],
                (128, 256, 128),
            )
            for wm, wn in ((16, 64), (64, 16))
            for kind, pipeline in (("lds", 1), ("lds", 2), ("regtiles", 0))
        ]
    elif args.larger:
        variants = [
            (f"large-k{k}-{label}", kind, definitions, (128, 256, k))
            for k in (256, 512)
            for label, kind, definitions in (
                ("single", "reference-lds", ["PIPELINE=1"]),
                ("double", "reference-lds", []),
                ("producer", "reference-lds", ["PRODUCERS=64"]),
                ("reg", "reference-regtiles", []),
            )
        ]
    elif args.final_shape:
        shape = (128, 256, 128)
        native = ["ACC_BARRIER=0", "STORE_VGPR=1", "TILE_M=64", "TILE_N=64"]
        variants = [
            (
                f"finalshape-reg-bk{bk}-p{pipeline}",
                "reference-regtiles",
                native
                + [
                    "WAVE_M=32",
                    "WAVE_N=32",
                    f"TILE_K={bk}",
                    f"PIPELINE={pipeline}",
                ],
                shape,
            )
            for bk, pipeline in ((8, 0), (8, 1), (16, 1), (32, 0), (32, 1))
        ]
        variants += [
            (
                f"finalshape-lds-{layout}-p{pipeline}",
                "reference-lds",
                native
                + [
                    "WAVE_M=64",
                    "WAVE_N=16",
                    "TILE_K=16",
                    "PREFETCH_LDS=2",
                    definition,
                    f"PIPELINE={pipeline}",
                ],
                shape,
            )
            for layout, definition in (
                ("pad16", "LDS_PAD=16"),
                ("xor", "SWIZZLE=1"),
            )
            for pipeline in (1, 2)
        ]
    elif args.native_large_tile:
        shape = (128, 256, 512)
        native = ["ACC_BARRIER=0", "STORE_VGPR=1"]
        tile = [
            "TILE_M=64",
            "TILE_N=64",
            "TILE_K=64",
            "WAVE_M=64",
            "WAVE_N=16",
        ]
        variants = [
            (
                f"native-large-f{fragments}-p{pipeline}",
                "reference-lds",
                native
                + tile
                + [f"PREFETCH_LDS={fragments}", f"PIPELINE={pipeline}"],
                shape,
            )
            for fragments in (0, 1)
            for pipeline in (1, 2)
        ]
        variants += [
            (
                f"native-large-{label}-p{pipeline}",
                "reference-lds",
                native + tile + [definition, f"PIPELINE={pipeline}"],
                shape,
            )
            for label, definition in (
                ("lookahead2", "GLOBAL_LOOKAHEAD=2"),
                ("producer128", "PRODUCERS=128"),
            )
            for pipeline in (1, 2)
        ]
        variants += [
            (
                f"native-large-reg-{label}",
                "reference-regtiles",
                native + definitions,
                shape,
            )
            for label, definitions in (
                (
                    "wide",
                    [
                        "TILE_M=64",
                        "TILE_N=64",
                        "TILE_K=16",
                        "WAVE_M=32",
                        "WAVE_N=32",
                        "PIPELINE=0",
                    ],
                ),
                (
                    "small",
                    [
                        "TILE_M=32",
                        "TILE_N=32",
                        "TILE_K=8",
                        "WAVE_M=16",
                        "WAVE_N=16",
                        "PIPELINE=0",
                    ],
                ),
                (
                    "prefetch",
                    [
                        "TILE_M=32",
                        "TILE_N=32",
                        "TILE_K=8",
                        "WAVE_M=16",
                        "WAVE_N=16",
                        "PIPELINE=1",
                    ],
                ),
            )
        ]
    elif args.native_deep:
        shape = (64, 128, 512)
        native = ["ACC_BARRIER=0", "STORE_VGPR=1", "TILE_M=32", "TILE_N=32"]
        variants = [
            (
                f"native-k512-bk64-f{fragments}-p{pipeline}",
                "reference-lds",
                native
                + [
                    "WAVE_M=32",
                    "WAVE_N=16",
                    "TILE_K=64",
                    f"PREFETCH_LDS={fragments}",
                    f"PIPELINE={pipeline}",
                ],
                shape,
            )
            for fragments in (0, 1)
            for pipeline in (1, 2)
        ]
        variants += [
            (
                f"native-k512-bk64-f{fragments}-p{pipeline}-lookahead2",
                "reference-lds",
                native
                + [
                    "WAVE_M=32",
                    "WAVE_N=16",
                    "TILE_K=64",
                    f"PIPELINE={pipeline}",
                    "GLOBAL_LOOKAHEAD=2",
                    f"PREFETCH_LDS={fragments}",
                ],
                shape,
            )
            for fragments in (0, 1)
            for pipeline in (1, 2)
        ]
        variants += [
            (
                f"native-k512-reg-bk{bk}-p{pipeline}",
                "reference-regtiles",
                native
                + [
                    "WAVE_M=16",
                    "WAVE_N=16",
                    f"TILE_K={bk}",
                    f"PIPELINE={pipeline}",
                ],
                shape,
            )
            for bk, pipeline in (
                (8, 1),
                (16, 1),
                (32, 0),
                (32, 1),
                (64, 0),
                (64, 1),
            )
        ]
    elif args.native_fragments:
        variants = [
            (
                f"native-k{k}-fragments-p{pipeline}",
                "reference-lds",
                [
                    "ACC_BARRIER=0",
                    "STORE_VGPR=1",
                    "TILE_M=32",
                    "TILE_N=32",
                    "WAVE_M=32",
                    "WAVE_N=16",
                    f"PIPELINE={pipeline}",
                    "PREFETCH_LDS=1",
                ],
                (64 if k == 512 else 128, 128, k),
            )
            for k in (256, 512)
            for pipeline in (1, 2)
        ]
    elif args.native_layouts:
        variants = [
            (
                f"native-k{k}-{layout}-p{pipeline}",
                "reference-lds",
                [
                    "ACC_BARRIER=0",
                    "STORE_VGPR=1",
                    "TILE_M=32",
                    "TILE_N=32",
                    "WAVE_M=32",
                    "WAVE_N=16",
                    f"PIPELINE={pipeline}",
                    definition,
                ],
                (64 if k == 512 else 128, 128, k),
            )
            for k in (256, 512)
            for layout, definition in (
                ("pad16", "LDS_PAD=16"),
                ("xor", "SWIZZLE=1"),
            )
            for pipeline in (1, 2)
        ]
    elif args.native:
        variants = []
        native = ["ACC_BARRIER=0", "STORE_VGPR=1"]
        for k, shape in (
            (128, (128, 256, 128)),
            (256, (128, 128, 256)),
            (512, (64, 128, 512)),
        ):
            tile = (
                []
                if k == 128
                else ["TILE_M=32", "TILE_N=32", "WAVE_M=32", "WAVE_N=16"]
            )
            for label, control in (
                ("single", ["PIPELINE=1"]),
                ("double", ["PIPELINE=2"]),
                ("producer", ["PIPELINE=2", "PRODUCERS=64"]),
                ("single-lookahead2", ["PIPELINE=1", "GLOBAL_LOOKAHEAD=2"]),
            ):
                variants.append(
                    (
                        f"native-k{k}-{label}",
                        "reference-lds",
                        native + tile + control,
                        shape,
                    )
                )
            variants.append(
                (f"native-k{k}-reg", "reference-regtiles", native, shape)
            )
            if k != 128:
                variants.append(
                    (
                        f"native-k{k}-reg-small",
                        "reference-regtiles",
                        native
                        + [
                            "TILE_M=32",
                            "TILE_N=32",
                            "WAVE_M=16",
                            "WAVE_N=16",
                            "TILE_K=8",
                        ],
                        shape,
                    )
                )
    elif args.small_registers:
        variants = [
            (
                f"small-k{k}-reg-wave{wm}x{wn}-bk{bk}",
                "reference-regtiles",
                [
                    "TILE_M=32",
                    "TILE_N=32",
                    f"WAVE_M={wm}",
                    f"WAVE_N={wn}",
                    f"TILE_K={bk}",
                    "PIPELINE=0",
                ],
                (64 if k == 512 else 128, 128, k),
            )
            for k in (256, 512)
            for wm, wn in ((16, 16), (32, 16), (32, 32))
            for bk in (8, 16)
        ]
    elif args.producer_controls or args.lookahead:
        small = ["TILE_M=32", "TILE_N=32", "WAVE_M=32", "WAVE_N=16"]
        suffix = "-lookahead2" if args.lookahead else ""
        if args.lookahead:
            small += ["GLOBAL_LOOKAHEAD=2"]
        variants = [
            (
                f"small-k{k}-prod{producers}-p{pipeline}{suffix}",
                "reference-lds",
                small + [f"PRODUCERS={producers}", f"PIPELINE={pipeline}"],
                (64 if k == 512 else 128, 128, k),
            )
            for k in (256, 512)
            for producers in (0, 64)
            for pipeline in (1, 2)
        ]
        variants += (
            []
            if args.lookahead
            else [
                (
                    f"large-k{k}-producer-single",
                    "reference-lds",
                    ["PRODUCERS=64", "PIPELINE=1"],
                    (128, 256, k),
                )
                for k in (256, 512)
            ]
        )
    for name, kind, definitions, shape in variants:
        # Preserve the original ablations when reference defaults improve.
        # Native studies explicitly override the accumulator-copy controls.
        if kind.startswith(("reference-", "mfma-")):
            defaults = {"ACC_BARRIER": 1, "STORE_VGPR": 0}
            if kind == "reference-lds":
                defaults.update(LDS_PAD=0, PREFETCH_LDS=0)
            specified = {value.split("=", 1)[0] for value in definitions}
            definitions = definitions + [
                f"{key}={value}"
                for key, value in defaults.items()
                if key not in specified
            ]
        command = [
            sys.executable,
            str(tune),
            name,
            kind,
            "--shape",
            *map(str, shape),
        ]
        for definition in definitions:
            command += ["--define", definition]
        if args.build_only:
            command += ["--build-only"]
        print(f"START {name}", flush=True)
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
