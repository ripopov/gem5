# CHI Protocol with different ISAs test

The purpose of this test is to ensure the CHI protocol functions across ARM, X86, and RISCV ISA targets with varying numbers of CPU cores. The tests can be run with the following command:

```shell
# In the "tests" directory
./main.py run --length=long -j`nproc` gem5/chi_protocol
```

## Checkpoint save/restore

`run_checkpoint_regression.py` covers taking and restoring a checkpoint of a
CHI system: the flush that writes dirty blocks back before the trace is
serialized, and the replay that reinstalls them on restore. It is not a
testlib test, because save and restore have to run in that order and because
it needs a `RUBY_PROTOCOL_CHI=y` binary, which the testlib builds are not.

```shell
scons build/RISCV/gem5.opt RUBY_PROTOCOL_CHI=y
sudo apt install gcc-riscv64-linux-gnu

tests/gem5/chi_protocol/run_checkpoint_regression.py build/RISCV/gem5.opt
```

For each core count it runs the workload straight through for a reference
checksum, then checkpoints at several ticks and restores each one, requiring
the same checksum back. A checkpoint whose cache trace came out empty is
rejected rather than passed: with nothing dirty, the flush and the replay are
both no-ops and the run would prove nothing. The workload
(`tests/test-progs/ruby-checkpoint/src`) exists to keep a buffer dirty in the
private caches for its whole run so that never happens by accident.

The restored run is not tick-identical to the reference one, and is not
checked to be. Taking a checkpoint flushes the hierarchy, and the replay
takes simulated time, so the resumed run starts later and with different
cache warmth. Only the workload result has to match.
