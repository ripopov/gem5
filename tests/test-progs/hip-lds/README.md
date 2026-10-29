# LDS access footprint regression

Build with `hipcc --offload-arch=gfx942 -O2 -std=c++17 main.cpp -o hip-lds`.
Run the binary in a HIP-capable GPU simulation environment. Success prints
`hip_lds: PASS`; every returned word is checked. The kernel exercises immediate
and dual/ST64 offsets, B32/B64/B128 reads, B128 writes, partial execution masks,
and contended modifying atomics. Explicit waits protect inline-assembly loads.

The companion `src/gpu-compute/lds_access.test.cc` unit tests check bank service
phases and conflicts independently of kernel scheduling and cache traffic.
