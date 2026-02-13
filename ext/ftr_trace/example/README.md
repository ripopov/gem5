# Fake RISC-V simulator demo

This demo generates FTR traces from a fake multicore RISC-V simulator model.

- Each core has exactly 2 harts.
- All harts emit into one shared `.ftr` file, with separate fibers per hart.
- Each instruction is traced as a pipeline transaction.
- Load/store instructions create child memory-request transactions.

## Build

```bash
cmake -S . -B build -DFTR_TRACE_BUILD_EXAMPLES=ON
cmake --build build --target ftr_fake_riscv_sim
```

## Run

```bash
./build/example/ftr_fake_riscv_sim --cores 4 --instructions 128 --out-dir example/out
```

Example output files:

- `example/out/multicore.ftr`
