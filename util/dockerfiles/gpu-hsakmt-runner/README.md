# gem5 HSAKMT runner image

This recipe builds the server side of the two-container HSAKMT workflow. The
final image contains a stripped `build/VEGA_X86/gem5.opt`,
`librocjitsu_kmd_server.so`, the Python configuration, and the runtime
entrypoint. It contains no ROCr application runtime, HIP toolchain, interposer,
full rocjitsu simulator, gem5 source tree, or host-built binary.

## Build

Build the matching SDK and runner pair from the repository root:

```sh
gpu-lab/workspace/scripts/build-hsakmt-images.sh
```

Or build only this image:

```sh
util/dockerfiles/gpu-hsakmt-runner/build.sh
```

Override the image, parallelism, or native platform when needed:

```sh
IMAGE=gem5-gpu-hsakmt-runner:test BUILD_JOBS=8 PLATFORM=linux/amd64 \
  util/dockerfiles/gpu-hsakmt-runner/build.sh
```

The helper stages tracked and non-ignored files in a temporary context. The
builder compiles rocjitsu's server-only CMake target into a private prefix,
then gives that prefix to SCons while linking `gem5.opt`. Only the installed
KMD library, executable, configs, and launcher cross into the final stage.

The image records the gem5 and rocjitsu revisions in OCI labels. The workspace
launcher requires a matching SDK platform and rocjitsu revision.

## Runtime

Use the checked-in Compose launcher:

```sh
cd gpu-lab/workspace
./scripts/run-sample.sh --backend gem5-kmd hip-vector-add
```

The entrypoint creates the shared runtime and output directories, removes only
the stale rocjitsu socket and generated topology trees, and drops to the
configured numeric UID and GID. It starts
`configs/example/gpufs/hsakmt_server.py`, publishes the KMD endpoint, and exits
after its one client disconnects with all service state drained.

The SDK and runner use `network_mode: none`. A named volume carries the Unix
socket, topology, and shared descriptors; host bind mounts carry application
artifacts and gem5 evidence.

The workspace verifier runs on the host-visible output after the containers
exit. It requires exact application output, modeled kernel counts,
submitted-versus-retired AQL equality, clean KMD teardown, and the rainbow BMP
digest. It rejects timeouts, nonzero exits, gem5 faults, reactor errors, and
stale doorbells; it is deliberately not packaged in the runtime image.

`VEGA_X86` describes the simulated target and can be built natively in an
AMD64 or ARM64 Linux container. The workflow does not use KVM.
