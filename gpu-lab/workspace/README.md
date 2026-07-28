# GPU lab workspace

This workspace contains the provided HIP samples and one script interface for
all three execution backends:

```sh
./scripts/run-all-samples.sh --backend simdojo
./scripts/run-all-samples.sh --backend gem5-kmd
./scripts/run-all-samples.sh --backend full-system
```

The backend is always explicit. Builds go to `work/apps/<backend>/`; runs go to
`work/runs/`. Use `run-sample.sh` for a provided sample and `run-app.sh` for any
executable below this workspace.

| Path | Purpose |
| --- | --- |
| `samples/` | HIP vector add, matmul, kernel chain, rainbow image, and their Makefile |
| `scripts/` | Image preparation, build, launch, and verification |
| `compose.yaml` | Internal two-container topology used by `gem5-kmd` |
| `images/`, `disk/`, `kernel/` | Packaged inputs for `full-system`; disk and kernel files may be symlinks |
| `work/` | Generated applications and run evidence |

See [Running the GPU lab](../RunningTheLab.md) for backend selection,
prerequisites, commands, output files, and troubleshooting.
