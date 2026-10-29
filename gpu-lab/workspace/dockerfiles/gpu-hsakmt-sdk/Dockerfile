# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer; redistributions in binary
# form must reproduce the above copyright notice, this list of conditions and
# the following disclaimer in the documentation and/or other materials
# provided with the distribution; neither the name of the copyright holders
# nor the names of its contributors may be used to endorse or promote products
# derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# syntax=docker/dockerfile:1.7

ARG UBUNTU_IMAGE=ubuntu@sha256:651ba3fe3a830441e3deaf70fafac40d808a6bd2800a6f2c43130055159f23e6

FROM ${UBUNTU_IMAGE} AS rocr-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=20

# ROCr's current gfx12 trap-handler sources require Clang 22. Device bitcode
# comes from Ubuntu's pinned ROCm 7.1/LLVM 21 package.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        clang-22 \
        cmake \
        libclang-22-dev \
        libcurl4-openssl-dev \
        libdrm-dev \
        libedit-dev \
        libelf-dev \
        libnuma-dev \
        lld-22 \
        ninja-build \
        llvm-22-dev \
        pkg-config \
        rocm-device-libs-21=7.1.1+dfsg-0ubuntu1 \
        xxd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/rocr-runtime
COPY --from=rocm-systems projects/rocr-runtime .

RUN grep -Fq 'get_version("1.21.0")' CMakeLists.txt \
    && cmake -S . -B /build-rocr -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEXPORT_TO_USER_PACKAGE_REGISTRY=OFF \
        -DClang_DIR=/usr/lib/llvm-22/lib/cmake/clang \
        -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
    && HIP_DEVICE_LIB_PATH=/usr/lib/llvm-21/lib/clang/21/amdgcn/bitcode \
        cmake --build /build-rocr --parallel "${BUILD_JOBS}" \
    && install -D -m 0755 \
        /build-rocr/rocr/lib/libhsa-runtime64.so.1.21.0 \
        /opt/rocr/lib/libhsa-runtime64.so.1.21.0 \
    && ln -s libhsa-runtime64.so.1.21.0 \
        /opt/rocr/lib/libhsa-runtime64.so.1 \
    && ln -s libhsa-runtime64.so.1 \
        /opt/rocr/lib/libhsa-runtime64.so \
    && install -D -m 0644 LICENSE.txt \
        /opt/rocr/share/licenses/rocr-runtime/LICENSE.txt \
    && strip --strip-unneeded /opt/rocr/lib/libhsa-runtime64.so.1.21.0

FROM ${UBUNTU_IMAGE} AS rocjitsu-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=20

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libdrm-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/rocjitsu
COPY --from=rocm-systems emulation/rocjitsu .

RUN grep -Fq 'project(rocm_jit_suite' CMakeLists.txt \
    && cmake -S . -B /build-rocjitsu -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS=-Wno-error=free-nonheap-object \
        -DCMAKE_INSTALL_PREFIX=/opt/rocjitsu \
        -DROCJITSU_HOST_CPU_BASELINE=default \
        -DBUILD_TESTING=OFF \
        -DRJ_LOG_GROUPS=OFF \
        -DFETCHCONTENT_BASE_DIR=/tmp/rocjitsu-deps \
    && cmake --build /build-rocjitsu --parallel "${BUILD_JOBS}" \
    && cmake --install /build-rocjitsu --strip \
    && install -D -m 0644 LICENSE.md \
        /opt/rocjitsu/share/licenses/rocjitsu/LICENSE.md

FROM ${UBUNTU_IMAGE} AS sdk

ARG DEBIAN_FRONTEND=noninteractive
ARG GEM5_SOURCE_REVISION=unknown
ARG ROCR_REVISION=unknown
ARG ROCJITSU_REVISION=unknown
ARG HIPCC_VERSION=7.1.1+dfsg-0ubuntu1
ARG HIP_RUNTIME_VERSION=7.1.0-0ubuntu2

LABEL org.opencontainers.image.source="https://github.com/gem5/gem5" \
      org.opencontainers.image.description="HIP 7.1 SDK with pinned ROCr 1.21 and rocjitsu attach client" \
      org.opencontainers.image.licenses="BSD-3-Clause" \
      org.gem5.hsakmt.sdk.revision="${GEM5_SOURCE_REVISION}" \
      org.gem5.hsakmt.rocr.revision="${ROCR_REVISION}" \
      org.gem5.rocjitsu.revision="${ROCJITSU_REVISION}" \
      org.gem5.hsakmt.rocr.version="1.21.0" \
      org.gem5.rocjitsu.version="0.3.0"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        hipcc="${HIPCC_VERSION}" \
        libamdhip64-dev="${HIP_RUNTIME_VERSION}" \
        make \
        ninja-build \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=rocr-builder /opt/rocr /opt/rocr
COPY --from=rocjitsu-builder /opt/rocjitsu /opt/rocjitsu
COPY --chmod=0755 rocjitsu-run /usr/local/bin/rocjitsu-run

ENV ROCJITSU_CONFIG_DIR=/opt/rocjitsu/share/rocjitsu/configs \
    LD_LIBRARY_PATH=/opt/rocr/lib \
    PATH=/opt/rocjitsu/bin:${PATH}

WORKDIR /workspace

RUN test "$(dpkg --print-architecture)" = "$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')" \
    && test -x /usr/bin/hipcc \
    && test -f /opt/rocr/lib/libhsa-runtime64.so.1.21.0 \
    && test -x /usr/local/bin/rocjitsu-run \
    && test -x /opt/rocjitsu/bin/rocjitsu \
    && test -f /opt/rocjitsu/lib/librocjitsu.so \
    && test -f "${ROCJITSU_CONFIG_DIR}/gfx942_cdna3_kmd.json" \
    && rocjitsu --version \
    && rocjitsu-pretranslate.py --help > /dev/null
