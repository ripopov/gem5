# ROCr and rocjitsu source

This directory owns the pinned `rocm-systems` submodule used by the GPU lab.
The active application-side path is rocjitsu's ordinary `/dev/kfd` and DRM
interposer; there is no repository-specific ROCr model-ABI library here.

The relevant sources are:

```text
rocm-systems/
  projects/rocr-runtime/       pinned ROCr and libhsakmt
  emulation/rocjitsu/          interposer, KMD server, and simulator
```

rocjitsu owns the KMD RPC schema, transport, `RemoteDriver`,
`SimulatedDriver`, generated topology, reusable server, and device-backend
interface. The gem5 service links the installed, versioned C API from
`rocjitsu/kmd/server.h`; rocjitsu must not include gem5 headers, depend on
generated SimObjects, or invoke SCons.

Build the complete rocjitsu project and run its tests through the service
orchestration target:

```sh
make -C src/dev/hsakmt-service rocjitsu-tests
```

Build only the embeddable KMD server used by gem5 with:

```sh
make -C src/dev/hsakmt-service install-kmd
```

The SDK Docker build compiles ROCr from
`rocm-systems/projects/rocr-runtime` and full rocjitsu from
`rocm-systems/emulation/rocjitsu`. The runner build compiles the server-only
rocjitsu target and installs `librocjitsu_kmd_server.so` alongside
`gem5.opt`.

This is a Git submodule. Changes under `rocm-systems/` require a commit in
that repository before the outer gem5 repository can record the new
submodule revision.
