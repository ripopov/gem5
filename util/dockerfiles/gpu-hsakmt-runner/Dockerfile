# syntax=docker/dockerfile:1.7

# Multi-platform Ubuntu 26.04 manifest shared with gpu-hsakmt-sdk. The gem5
# target ISA is X86/VEGA; the container host ISA may be AMD64 or ARM64.
ARG UBUNTU_IMAGE=ubuntu@sha256:651ba3fe3a830441e3deaf70fafac40d808a6bd2800a6f2c43130055159f23e6

FROM ${UBUNTU_IMAGE} AS rocjitsu-kmd-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=20

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libdrm-dev \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/rocjitsu
COPY --from=rocjitsu-src . .

# The runner consumes only the transport/KFD policy library. This build mode
# deliberately avoids rocjitsu's simulator, FlatBuffers, GoogleTest, and all
# FetchContent network access.
RUN cmake -S . -B /build-rocjitsu-kmd -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/rocjitsu-kmd \
        -DBUILD_TESTING=OFF \
        -DRJ_KMD_SERVER_ONLY=ON \
    && cmake --build /build-rocjitsu-kmd \
        --target rocjitsu_kmd_server \
        --parallel "${BUILD_JOBS}" \
    && cmake --install /build-rocjitsu-kmd \
    && strip --strip-unneeded \
        /opt/rocjitsu-kmd/lib/librocjitsu_kmd_server.so.1.0.0

FROM ${UBUNTU_IMAGE} AS gem5-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=20

# Keep the builder deliberately smaller than the interactive gem5
# devcontainer. Optional protobuf, HDF5, GUI, documentation, and editor
# packages are not needed by the HSAKMT GPUFS runner.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        git \
        libcapstone-dev \
        libelf-dev \
        libpng-dev \
        m4 \
        pkg-config \
        python3-dev \
        scons \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/gem5
COPY --from=gem5-src . .
COPY --from=rocjitsu-kmd-builder /opt/rocjitsu-kmd /opt/rocjitsu-kmd

# The named context is assembled from non-ignored source files by build.sh.
# These guards catch accidental use of the full, artifact-heavy worktree.
RUN test ! -e build/VEGA_X86/gem5.opt \
    && test ! -e gpu-lab/workspace/work \
    && test ! -e gpu-lab/workspace/images

RUN scons \
        build/VEGA_X86/gem5.opt \
        ROCJITSU_KMD_PREFIX=/opt/rocjitsu-kmd \
        --without-tcmalloc \
        -j "${BUILD_JOBS}" \
    && strip --strip-unneeded build/VEGA_X86/gem5.opt

FROM ${UBUNTU_IMAGE} AS runner

ARG DEBIAN_FRONTEND=noninteractive
ARG GEM5_SOURCE_REVISION=unknown
ARG ROCJITSU_REVISION=unknown

LABEL org.opencontainers.image.source="https://github.com/gem5/gem5" \
      org.opencontainers.image.description="Self-contained gem5 HSAKMT GPU runner" \
      org.opencontainers.image.licenses="BSD-3-Clause" \
      org.gem5.hsakmt.server.revision="${GEM5_SOURCE_REVISION}" \
      org.gem5.rocjitsu.revision="${ROCJITSU_REVISION}" \
      org.gem5.rocjitsu.kmd-server-abi="1"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libpng16-16t64 \
        libpython3.14 \
        python3-minimal \
        util-linux \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=gem5-builder \
    /src/gem5/build/VEGA_X86/gem5.opt \
    /gem5/build/VEGA_X86/gem5.opt
COPY --from=rocjitsu-kmd-builder \
    /opt/rocjitsu-kmd/lib \
    /opt/rocjitsu-kmd/lib

# hsakmt_server.py imports only these configuration families. gem5 source,
# object files, build metadata, tests, and development headers stay behind.
COPY --from=gem5-src configs/common /gem5/configs/common
COPY --from=gem5-src configs/example/gpufs /gem5/configs/example/gpufs
COPY --from=gem5-src configs/network /gem5/configs/network
COPY --from=gem5-src configs/ruby /gem5/configs/ruby
COPY --from=gem5-src configs/topologies /gem5/configs/topologies
COPY --chmod=0755 gem5-hsakmt-runner /usr/local/bin/gem5-hsakmt-runner

ENV HSAKMT_GEM5=/gem5/build/VEGA_X86/gem5.opt \
    HSAKMT_CONFIG=/gem5/configs/example/gpufs/hsakmt_server.py \
    ROCJITSU_RUNTIME_DIR=/run/rocjitsu \
    HSAKMT_RPC_SOCKET=/run/rocjitsu/daemon.sock \
    HSAKMT_OUTPUT_DIR=/outputs/current

WORKDIR /gem5
ENTRYPOINT ["/usr/local/bin/gem5-hsakmt-runner"]
CMD ["run"]

# Check arbitrary numeric runtime identities, dynamic dependencies, embedded
# Python imports, and the server-driven GPUFS graph during the image build.
RUN HSAKMT_RUN_UID=12345 \
        HSAKMT_RUN_GID=23456 \
        ROCJITSU_RUNTIME_DIR=/tmp/runner-user-check/rpc \
        HSAKMT_RPC_SOCKET=/tmp/runner-user-check/rpc/service.sock \
        HSAKMT_OUTPUT_DIR=/tmp/runner-user-check/output \
        /usr/local/bin/gem5-hsakmt-runner true \
    && rm -rf /tmp/runner-user-check \
    && ! ldd "${HSAKMT_GEM5}" | grep -Fq 'not found' \
    && ! ldd /opt/rocjitsu-kmd/lib/librocjitsu_kmd_server.so.1 \
        | grep -Fq 'not found' \
    && mkdir -p /tmp/runner-check \
    && "${HSAKMT_GEM5}" -d /tmp/runner-check "${HSAKMT_CONFIG}" \
        --num-compute-units=1 \
        --num-cpus=1 \
        --cpu-type=X86TimingSimpleCPU \
        --mem-size=2GiB \
        --dgpu-mem-size=1GiB \
        --gpu-device=MI300X \
        --hsakmt-mmio-bar-base=0xece00000 \
        --rocjitsu-runtime-root=/tmp/runner-check \
        --hsakmt-rpc-socket=/tmp/runner-check/service.sock \
    && rm -rf /tmp/runner-check
