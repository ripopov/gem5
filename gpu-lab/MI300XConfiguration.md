# MI300X configuration audit

Audited against the linked AMD documentation on 2026-09-15.

The lab retains eight CUs and a reduced memory system. Its per-CU register,
wavefront and cache capacities now follow MI300X documentation. The shared L2
is deliberately scaled to **1 MiB**, rather than pretending that eight CUs
constitute a complete XCD. This is a resource-matched teaching configuration,
not a cycle-calibrated model of an entire MI300X.

## Resources and clock

The changes apply to both gem5 launchers through
`configs/example/gpufs/amd/MI300X.py`. Explicit command-line overrides still
work. Other GPU configuration entry points keep their existing defaults.

| Resource | Previous teaching default | Revised default |
| --- | ---: | ---: |
| Modeled CUs | 8 | 8 |
| SIMDs per CU / lanes per wave | 4 / 64 | 4 / 64 |
| Wave slots per SIMD / CU | 10 / 40 | **8 / 32** |
| Vector-register slots per SIMD | 2,048 | **512** |
| Scalar registers per SIMD | 2,048 | **800** |
| Vector / scalar allocation units | 4 / 4 | **16 / 16** |
| LDS per CU / banks / bank word | 64 KiB / 32 / 4 B | unchanged |
| Vector L1 per CU | 16 KiB | **32 KiB** |
| Instruction cache | 32 KiB per 4 CUs | **64 KiB per 2 CUs** |
| Scalar cache | 32 KiB per 4 CUs | **16 KiB per 2 CUs** |
| Shared L2 | 256 KiB | **1 MiB**, scaled |
| Shader, CU and LDS clock | 1 GHz system clock | **2.1 GHz** requested |
| Register allocator (`--reg-alloc-policy`) | `simple` | **`dynamic`** |

The register and cache capacities follow AMD's
[hardware specifications](https://rocm.docs.amd.com/en/docs-6.3.3/reference/gpu-arch-specs.html).
One vector slot contains 64 32-bit lane values: 512 slots per SIMD across four
SIMDs gives 512 KiB per CU. Four pools of 800 scalar registers give 12.5 KiB.
AMD's [rocminfo example](https://rocm.docs.amd.com/projects/rocminfo/en/docs-6.2.0/how-to/use-rocminfo.html)
reports 32 maximum waves per CU for MI300X. The earlier 40-slot setting was an
older generic GPU default, not the MI300X limit.

AMD's [occupancy guidance](https://rocm.docs.amd.com/projects/ai-ecosystem/en/latest/optimization/workload-optimization.html)
specifies 512 vector slots per SIMD allocated in groups of 16. The scalar
allocation unit follows GPR_ALLOC in the
[MI300 ISA reference](https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-mi300-cdna3-instruction-set-architecture.pdf),
section 5.8. The configuration previously wired the scalar pool's allocation
unit to the vector option; both allocator policies now use the scalar option.

The register allocator decides how a workgroup's waves are placed on the
four SIMDs. The `simple` allocator hands a SIMD's whole register file to one
dispatch at a time, so it never co-schedules workgroups on a SIMD and, when
only one SIMD is free, stacks every wave of a new workgroup onto it. The
`dynamic` allocator places waves by the free capacity of each SIMD, which is
what the register sizes above are meant to bound. It is the lab default; the
GEMM study measured its effect and the earlier allocator checks validated it.
Pass `--reg-alloc-policy=simple` after `--` to restore the upstream default.

The **2.1 GHz** setting is AMD's published
[peak engine clock](https://www.amd.com/en/products/accelerators/instinct/mi300/mi300x.html),
used here as a fixed operating point, not a promise of sustained hardware boost.
At the simulator's 1 ps tick resolution its period rounds to 476 ticks.
Previously `--gpu-clock` reached cache/TLB components but the shader inherited
the system clock. The shader now owns a GPU clock domain, inherited by its CUs,
LDS and bridges. The CPU/system and Ruby fabric retain separate clock domains.

## Scaling the shared cache and memory system

AMD documents 4 MiB of L2 per XCD with 38 active CUs in its
[MI300 architecture description](https://rocm.docs.amd.com/en/docs-6.3.3/conceptual/gpu-arch/mi300.html).
Proportional scaling gives `4 MiB × 8 / 38 = 862.3 KiB`. The agreed 1 MiB
power-of-two size is approximately 19% above that ratio. It is an intentional
teaching approximation, not an MI300X partition mode. One TCC controller serves
that cache; it does not recreate all XCD cache slices or their bandwidth.
Changing CU count alone does not automatically resize L2.

The memory system remains reduced: one modeled HBM interface has a 16 GB/s
peak, versus the full device's published 5.3 TB/s across HBM3 stacks. The lab's
existing interface is `HBM_1000_4H_1x128`, not a calibrated HBM3 stack model.
The KMD launcher exposes 1 GiB of device memory; the full-system configuration
uses 16 GiB for its supported guest layout. Neither represents the full
192 GiB MI300X. The Ruby directory also contains an L3 cache object; its
existence must not be interpreted as a modeled MI300X Infinity Cache topology.
The full-system guest continues to report a larger CU topology than the eight
modeled CUs. The KMD service reports the configured SIMD and wave-slot counts.

These are explicit platform reductions, not per-CU resource specifications.
They mean HBM bandwidth can still limit an eight-CU workload. L2 capacity
misses alone are not evidence of kernel HBM bandwidth or latency.

## Timing audit: published values versus model assumptions

AMD publishes selected instruction cycle counts, capacity and peak throughput.
The cited documents do not provide a complete set of cache, interconnect,
load-to-use and DRAM timings from which every gem5 latency can be calibrated.
Unpublished values have therefore not been replaced with invented MI300X
numbers.

| Component | Configured behavior | Interpretation |
| --- | --- | --- |
| FP32 MFMA 16×16×4 | 32 GPU cycles; `mfma_scale=1` | Matches ISA table 28; models matrix-unit issue occupancy |
| Other supported gfx942 MFMA shapes | Existing gfx942 cycle table | Retained ISA-based instruction counts |
| Vector issue / SP and DP bypass | 4 / 4 / 4 GPU cycles | Existing pipeline model; not a complete dependency-latency calibration |
| CU request / response path | 50 / 50 GPU cycles | Uncalibrated pipeline delays, additional to cache/fabric timing |
| Scalar request / response | 50 / 0 GPU cycles | Response setting is unused by the current scalar path |
| Memory timestamp instruction | 41 GPU cycles | Inherited microbenchmark-derived default, not established MI300X timing |
| LDS bank serialization | 32 four-byte banks; one cycle per extra phase | Documented banking with approximate issue/return pipelines |
| LDS register-transfer parameter | 1 GPU cycle | Model assumption, not total LDS load latency |
| Vector L1 tag/data settings | 4 / 4 controller cycles | Model settings; not a measured end-to-end L1 hit latency |
| Scalar/instruction cache arrays / controller issue | 1 / 80 controller cycles | Separate inherited array and controller settings |
| L2 tag/data arrays | 2 / 8 controller cycles | Model assumptions |
| L2 response / GPU-to-directory | 16 / 120 controller cycles | Model assumptions |
| L3 tag/data settings | 15 / 20 controller cycles | Generic Ruby directory-cache assumptions |
| L1 / L2 TLB hits | 1 / 69 TLB cycles | Model assumptions; 32-entry L1 and 4,096-entry L2 |
| TLB miss-stage parameters | 750 / 750 TLB cycles | Inherited settings, not an MI300X page-walk latency specification |
| HBM tRCD / tCL / tRP / tBURST | 15 / 15 / 15 / 4 ns | Existing generic HBM interface; not MI300X HBM3 timings |

The LDS accounting covers bank words, active masks, read broadcasts and wide
instruction phases. Its service time also retains a lower bound derived from
the CU's global-register transfer widths, and its issue/return paths impose
additional limits. Thus 32 banks do not prove sustained 128 B/cycle in every
instruction stream. Matching capacities does not remove these timing-model
limitations. The scalar ALU, memory pipelines, queue capacities, cache
associativities, bank ports and TLB timings remain model assumptions unless
separately established by hardware measurement.

## Validation and using the revised configuration

Rebuild the runner/runtime images after changing the shared configuration
modules. Overlaying only `mi300.py` onto an old image is insufficient: the new
MI300X defaults module and scalar-cache option must also be present. The local
audit images use the previously validated simulator binary with updated Python
configuration files.

From `gpu-lab/workspace`, validate the actual generated `config.ini` with:

```sh
python3 experiments/mi300x-audit/check_config.py work/runs/RUN_DIRECTORY
```

The checker verifies all eight CUs, register pools and allocation units, wave
slots, LDS capacities and clock domains, and every vector/scalar/instruction/L2
cache. It checks instantiated objects, not just the intended parser defaults.
All six samples pass on each backend: **18 application checks**. Every one
of the 12 timing-backend runs also passes the instantiated-resource checker,
executes the expected kernel count and retires all submitted AQL packets.
Every KMD run drains its resources. The
[validation record](workspace/experiments/mi300x-audit/validation.json) includes
run IDs, actual configuration values, source hashes and immutable image IDs.
Both validated images were checked to contain the same configuration bytes
as the delivered source.
The separate allocator checks also exercise a 1.5 GHz clock override with
8-vector-register and 16-scalar-register allocation units, under both simple
and dynamic policies. Both pass correctness, AQL retirement and cleanup;
see [override results](workspace/experiments/mi300x-audit/overrides.json).
Repository hooks pass. The assignment PDF was regenerated and visually checked;
the architecture page passes offline desktop/mobile, keyboard/backend, journey,
memory, reduced-motion, no-JavaScript and local-link checks.

Earlier numbers in [EightCuEvaluation.md](EightCuEvaluation.md) and the
previous GEMM record
([results.json](workspace/experiments/gemm-study/results.json)) remain
historical measurements of their recorded configurations. They must not be
presented as performance measurements of these revised resources and clocks.
The current [GEMM study](GemmOptimization.md) remeasures the samples on the
revised configuration with a larger shape, because the old one fits
comfortably within the enlarged L2.

The locally built images used for this audit are selected with:

```sh
export HSAKMT_SDK_IMAGE=gem5-gpu-hsakmt-sdk:migration
export HSAKMT_RUNNER_IMAGE=gem5-gpu-hsakmt-runner:mi300x-8cu
export FULL_SYSTEM_IMAGE=gem5-gpufs-runtime:mi300x-8cu
./scripts/run-all-samples.sh --backend simdojo
./scripts/run-all-samples.sh --backend gem5-kmd
./scripts/run-all-samples.sh --backend full-system
```

The image names are local tags; no registry download is required. They contain
the prior validated LDS/accumulator/queue fixes as well as this configuration.
Older image tags do not acquire these changes automatically.
