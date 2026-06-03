# DDR4-2400 4Gb x8 Single-Channel Single-Rank 4GiB Study Requirements

## Purpose

This document defines the requirements for an apples-to-apples comparison of
three memory backends:

- gem5 native `MemCtrl` with a `DRAMInterface`
- DRAMSys
- DRAMSim3

The comparison must use the same DDR4 device-under-test definition, the same
memory timing assumptions, the same controller/request-side clock assumptions,
the same traffic, and the same metric units. Results must report latency in
nanoseconds and bandwidth in GB/sec.

The reference memory specification for this study is:

`ext/dramsys/DRAMSys/configs/memspec/JEDEC_4Gb_DDR4-2400_8bit_A.json`

## Device Under Test

The device under test is:

`DDR4-2400 4Gb x8, single channel, single rank, 4GiB`

This means the tested memory system is one DDR4 memory channel made from one
rank of eight x8 DDR4 devices. Each device has 4 gigabits of storage. Eight
devices are used together to form one 64-bit channel. Total capacity is 4GiB.

## DDR/DRAM Terminology

`DDR4` is the DRAM generation. DDR means double data rate: data transfers on
both edges of the data clock.

`2400` means 2400 mega-transfers per second on each data pin. Because DDR
transfers data on both clock edges, the command clock is 1200 MHz and the
clock period is approximately 0.833 ns.

`4Gb` is the density of one DRAM chip. It means 4 gigabits per device, not
4 gigabytes. 4 gigabits equals 512 MiB per device.

`x8` is the data width of one DRAM device. One chip supplies 8 data bits per
data transfer.

`single channel` means there is one independent memory channel and one 64-bit
data bus. The study must not compare a single-channel backend against a
dual-channel or interleaved multi-channel backend.

`single rank` means there is one group of DRAM devices selected together by
the controller. For this device, one rank contains eight x8 devices. Eight
devices times 8 bits per device gives a 64-bit data bus.

`4GiB` is the total channel capacity:

```text
8 devices/rank * 512 MiB/device * 1 rank/channel = 4096 MiB = 4 GiB
```

`BL8` means burst length 8. A single full-width burst transfers:

```text
8 transfers * 64 bits/transfer = 512 bits = 64 bytes
```

This is why 64-byte traffic blocks are the natural request size for this
study.

## Canonical Memory Configuration

The DRAMSys JEDEC DDR4-2400 4Gb x8 memspec is the source of truth.

### Required Geometry

| Parameter | Required value |
| --- | --- |
| Memory type | DDR4 |
| Data rate | 2400 MT/sec |
| Command clock | 1200 MHz |
| tCK | 0.833 ns |
| Channels | 1 |
| Ranks per channel | 1 |
| Devices per rank | 8 |
| Device density | 4Gb = 512 MiB |
| Device width | x8 |
| Channel data width | 64 bits |
| Total capacity | 4GiB |
| Burst length | 8 |
| Full burst size | 64 bytes |
| Bank groups per rank | 4 |
| Banks per rank | 16 |
| Banks per bank group | 4 |
| Rows | 32768 |
| Columns | 1024 |

### Required Timing Parameters

All backends must use the same logical timing values. Cycle values are from
the DRAMSys reference memspec. Nanosecond values use `tCK = 0.833 ns`.

| Timing | Cycles | Approx. ns |
| --- | ---: | ---: |
| tCK | n/a | 0.833 |
| CL / RL | 16 | 13.328 |
| WL / CWL | 16 | 13.328 |
| RCD | 16 | 13.328 |
| RP | 16 | 13.328 |
| RAS | 39 | 32.487 |
| RC | 55 | 45.815 |
| CCD_S | 4 | 3.332 |
| CCD_L | 6 | 4.998 |
| RRD_S | 4 | 3.332 |
| RRD_L | 6 | 4.998 |
| FAW | 26 | 21.658 |
| RFC1 | 312 | 259.896 |
| RFC2 | 192 | 159.936 |
| RFC4 | 132 | 109.956 |
| REFI | 9360 | 7796.880 |
| WR | 18 | 14.994 |
| WTR_S | 3 | 2.499 |
| WTR_L | 9 | 7.497 |
| RTP | 12 | 9.996 |
| XP | 8 | 6.664 |
| XS | 324 | 269.892 |
| XPDLL | 325 | 270.725 |
| XSDLL | 512 | 426.496 |
| CKE | 6 | 4.998 |
| CKESR | 7 | 5.831 |
| RTRS | 1 | 0.833 |

If a backend cannot model a timing parameter directly, the study must document
the missing parameter and whether it was folded into another timing parameter.
Silent mismatches are not acceptable.

## Backend Configuration Requirements

### DRAMSys

DRAMSys must use the reference memspec:

```text
ext/dramsys/DRAMSys/configs/memspec/JEDEC_4Gb_DDR4-2400_8bit_A.json
```

The gem5-facing DRAMSys wrapper should be a dedicated DDR4-2400 config, for
example:

```text
ext/dramsys/gem5_configs/ddr4-2400-4gb-x8-gem5-se.json
```

It must use:

- the DDR4-2400 4Gb x8 memspec above
- the same address mapping for every run
- the same memory controller config for every run
- `simconfig/gem5_se.json` or an equivalent storage-backed config if SE-mode
  functional/debug accesses are required
- one channel and one 4GiB address range

### DRAMSim3

DRAMSim3 should start from:

```text
ext/dramsim3/DRAMsim3/configs/DDR4_4Gb_x8_2400_2.ini
```

That file is the closest stock DRAMSim3 DDR4-2400 4Gb x8 timing match, but it
is not identical as shipped. The study requires a derived config, for example:

```text
ext/dramsim3/DRAMsim3/configs/DDR4_4Gb_x8_2400_1rank_4GiB.ini
```

Required DRAMSim3 edits relative to `DDR4_4Gb_x8_2400_2.ini`:

```ini
[timing]
tCK = 0.833
CWL = 16
tRTP = 12

[system]
channel_size = 4096
channels = 1
bus_width = 64
```

Rationale:

- `channel_size = 4096` is required so DRAMSim3 derives one rank for this
  4Gb x8 geometry. The stock file uses `8192`, which creates two ranks.
- `CWL = 16` matches the DRAMSys reference `WL = 16`.
- `tRTP = 12` matches the DRAMSys reference. The stock DRAMSim3 file uses 9.
- `tCK = 0.833` avoids a small clock mismatch with the DRAMSys reference.

Any retained DRAMSim3 parameters that do not have a DRAMSys equivalent, such
as `trans_queue_size`, must be listed in the run metadata.

### gem5 Native MemCtrl

The study must not use gem5's stock `DDR4_2400_8x8` as-is. Stock
`DDR4_2400_8x8` models 8Gb devices, two ranks, and 16GiB/channel. The study
requires a custom 4Gb, one-rank variant, for example:

```python
class DDR4_2400_8x8_4GiB(DRAMInterface):
    ...
```

The custom gem5 interface must use:

- `device_size = "512MiB"`
- `device_bus_width = 8`
- `devices_per_rank = 8`
- `ranks_per_channel = 1`
- `burst_length = 8`
- `bank_groups_per_rank = 4`
- `banks_per_rank = 16`
- `device_rowbuffer_size = "1KiB"`
- timing values converted from the DRAMSys reference table above

The gem5 `MemCtrl` instance must use the same externally visible memory range:

```text
start = 0
size = 4GiB
```

The native gem5 controller must use an FR-FCFS style scheduler if available,
open-page behavior, and queue limits chosen to match the DRAMSys and DRAMSim3
comparison as closely as gem5 exposes them. Any queue or scheduling policy
that cannot be made identical must be recorded in the run metadata.

## Controller and System Clock Requirements

The DRAM device clock is not enough for an apples-to-apples comparison. The
controller and request-side clock domains must also be normalized.

Each backend run must record and hold constant:

- memory device clock: DDR4-2400, `tCK = 0.833 ns`
- controller clock domain
- requestor clock domain
- system clock domain
- bridge or wrapper clock domain, if present
- cache/Ruby/CHI clock domain, if present

Recommended baseline:

```text
memory data rate: 2400 MT/sec
memory command clock: 1200 MHz
memory tCK: 0.833 ns
requestor/controller/system clock: same value across all backends
```

If a backend uses event scheduling rather than an explicit controller clock,
the study must still report the effective clock or tick granularity used for
request injection, command scheduling, and response timing.

## Address Mapping Requirements

All backends must use equivalent address mapping, or the mismatch must be
called out as a known limitation.

The address mapping must distribute address bits consistently across:

- byte offset
- column
- row
- bank
- bank group
- rank
- channel

For this single-channel, single-rank study, channel and rank selection should
not affect the traffic distribution. Bank, bank-group, row, and column mapping
still matter and must be aligned.

## Controller Policy Requirements

The following controller policies must be matched as closely as the backends
allow:

| Policy | Required setting |
| --- | --- |
| Scheduler | FR-FCFS or closest available equivalent |
| Row buffer policy | Open page |
| Command queue organization | Bankwise/per-bank where available |
| Command queue size | 8 entries where available |
| Request/transaction queue size | Match or record explicitly |
| Refresh policy | All-bank/rank-level behavior aligned or recorded |
| Power-down policy | Disabled unless all backends enable equivalent behavior |
| Write/read switching policy | Match or record thresholds and burst limits |

If a policy cannot be matched exactly, the run is still useful, but the report
must label it as a model comparison rather than a strict device-only
comparison.

## Traffic Requirements

The traffic source must be identical for all backends.

Required traffic controls:

- same number of requestors
- same requestor clock
- same traffic pattern
- same read/write mix
- same offered bandwidth points
- same block size, preferably 64 bytes
- same address range
- same random seed for random traffic
- same duration and warmup policy
- same outstanding request limits
- same backpressure behavior, or a documented difference

Recommended default:

```text
requestors: 1
block size: 64 bytes
traffic range: 256MiB or larger, held constant
memory range: 4GiB
patterns: linear-read, linear-write, linear-mixed, random-read, random-mixed
read/write mix for mixed traffic: 50 percent reads
```

If the experiment includes CHI, Ruby, caches, bridges, or interconnects, those
components must be identical across all backend runs. To isolate the memory
backend, direct TrafficGen-to-memory-controller tests are preferred.

## Metric Requirements

Latency and bandwidth must be measured at the same observation point for all
backends. The primary metrics must be requestor-visible, response-based
metrics, not backend-local counters.

### Latency

Latency must be reported in nanoseconds.

For each completed request:

```text
latency_ns = response_time_ns - request_accept_time_ns
```

Average latency must be computed from completed requests only:

```text
avg_latency_ns =
    total_completed_request_latency_ns / completed_request_count
```

If read and write requests are both present, report:

- read average latency in ns
- write average latency in ns
- combined average latency in ns, weighted by completed request count

### Bandwidth

Bandwidth must be reported in GB/sec.

Use decimal GB:

```text
1 GB = 1,000,000,000 bytes
```

For achieved bandwidth:

```text
achieved_GB_per_sec =
    completed_bytes / simulated_seconds / 1,000,000,000
```

For offered bandwidth:

```text
offered_GB_per_sec =
    requested_injection_bytes_per_second / 1,000,000,000
```

If existing scripts accept or print GiB/sec, the report must either convert to
GB/sec or include both columns with explicit names:

```text
offered_GBps
offered_GiBps
achieved_GBps
achieved_GiBps
```

The primary plots and summary tables for this study must use GB/sec.

## Required Output Schema

Each result row must include at least:

```text
backend
config_name
traffic_pattern
read_percent
offered_GBps
achieved_GBps
avg_latency_ns
read_avg_latency_ns
write_avg_latency_ns
completed_requests
completed_bytes
sim_seconds
retry_count_or_backpressure_indicator
```

Each run must also record enough metadata to reproduce the comparison:

```text
gem5_binary
config_script
backend
memory_config_file
controller_clock
requestor_clock
system_clock
memory_tCK
memory_size
traffic_address_range
block_size
num_requestors
random_seed
duration
warmup
```

## Validation Checklist

Before accepting results, verify:

- All backends expose exactly one 4GiB memory range.
- All backends use one channel and one rank.
- All backends use 8 x8 devices per rank.
- All backends use DDR4-2400 timing with `tCK = 0.833 ns`.
- DRAMSim3 does not accidentally use `channel_size = 8192`.
- gem5 native does not accidentally use stock `DDR4_2400_8x8`.
- DRAMSys uses the DDR4-2400 memspec, not the DDR4-1866 memspec.
- Controller, requestor, and system clocks are identical across backends.
- Address mapping is equivalent or explicitly documented as different.
- Scheduler, queue, row policy, refresh, and power-down settings are matched
  or explicitly documented as different.
- Latency is reported in ns.
- Bandwidth is reported in decimal GB/sec.
- Offered bandwidth and achieved bandwidth are not conflated.
- All plots and tables include units in axis labels and column names.

## Acceptance Criteria

The comparison is acceptable only if:

- the device geometry matches across all three backends
- the main timing table matches across all three backends, or every mismatch
  is documented and justified
- controller/requestor/system clocks are matched
- the same traffic reaches each backend
- latency is response-based and reported in ns
- bandwidth is completion-based and reported in GB/sec
- the report clearly distinguishes strict configuration equality from
  unavoidable simulator model differences

The expected conclusion should not claim that the simulators are identical.
The goal is to compare three backend models under a shared DDR4-2400 4Gb x8
single-channel single-rank 4GiB configuration.
