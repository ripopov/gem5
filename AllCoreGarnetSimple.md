# All-core `memset`: why SimpleNetwork beats Garnet (and how to close the gap)

Why does the **all-core** `memset` run a full ~21 % faster on SimpleNetwork than on
Garnet, when the two networks are configured to the *same* per-hop latency and nearly
tie in the single-core case?

```
# Garnet  (vcs_per_vnet=8, buffers_per_data_vc=1, router_latency=4, router_link_latency=2)
--scenario=memset --active-cores=all --network=garnet --num-outstanding-reqs=32
  system.cpu00 memset ROI BW: 19.833711 Bytes/Clock   (all 16 cores identical)

# SimpleNetwork  (router_buffer_size=8)
--scenario=memset --active-cores=all --network=simple --num-outstanding-reqs=32
  system.cpu00 memset ROI BW: 23.941606 Bytes/Clock   (all 16 cores identical)
```

**Bottom line up front.** Under all-to-all load the workload is **throughput-bound on
the saturated center-mesh links**, and Garnet's *realistic* router (a 2-stage separable
round-robin **SwitchAllocator** + credit flow control) drains those links only ~83–91 %
as efficiently as SimpleNetwork's *idealized* **PerfectSwitch** (near-maximal matching,
no arbitration loss). The deficit shows up entirely on the **data-return leg**, because
each 64 B line is **2 data beats** — so the data vnet carries 2× the flits and saturates
first. **No realistic Garnet latency/buffer/VC knob closes the gap** (they all plateau at
~21.8 B/cy); the only way to reach SimpleNetwork's number is to relieve the bottleneck
itself — e.g. **`data_width = 64` (1 beat/line)** lifts Garnet to **25.9 B/cy**, past
SimpleNetwork.

All experiments use the testbench at its current tuned config; nothing is committed.

---

## 1. Localizing the difference: it is the data-return leg

Per-transaction phase split for rnf0 (820 ROI samples, reconstructed from
`ProtocolTrace`; addresses are disjoint per core so the split is exact — §A):

| Phase | Garnet (cy) | Simple (cy) | Δ |
|------|---:|---:|---:|
| A. Request out (rnf0 → HNF) | 35.3 | 31.7 | **+3.6** |
| B. HNF L3 hit (tag 2 + data 10) | 12.0 | 12.0 | 0 |
| **C. Data return (HNF → rnf0)** | **50.4** | **37.2** | **+13.2** |
| **Total** `outTransLatHist.SendReadUnique` mean | **97.6** | **80.8** | **+16.8** |

(Both legs reconstructed over exactly 820 ROI transactions per core; the totals match
`stats.txt` to the digit: Garnet `…SendReadUnique::mean = 97.64`, Simple `= 80.84`.) The
HNF leg is identical (it's the L3, network-independent); **most** of the gap is on the
**data-return** leg (+13.2 cy), with a smaller request-leg contribution (+3.6 cy).

Why mostly the data leg? **A 64 B line is 2 data beats but only 1 request message**
(`data_width = 32` → 2 × 40 B beats; `cntrl_msg_size = 8`). The data vnet therefore
carries **twice** the flit load of the request vnet, so it saturates first, and the gap
scales with that load: **+3.6 cy on the lighter request vnet vs. +13.2 cy on the heavier
data vnet.** The difference is **congestion latency on the data vnet**, not a fixed
per-hop cost.

---

## 2. It is throughput-bound, not latency-bound

Little's Law (`BW = outstanding / latency`) is an identity and cannot by itself tell us
which side is the cause. The discriminating test is an **outstanding-request sweep** — if
adding concurrency does not raise BW, the network is at its throughput ceiling:

| `--num-outstanding-reqs` | Garnet BW | Simple BW |
|---:|---:|---:|
| 32 | 19.83 | **23.94** |
| 64 | 20.06 | 23.17 |
| 128 | 19.83 | 19.71 |

**Garnet is flat at ~20 B/cy** regardless of concurrency → it sits at a hard throughput
ceiling. SimpleNetwork **peaks at 23.94 @ 32** and then *degrades* with overload
(classic congestion collapse). Both are throughput-bound under all-to-all; the question
is simply **why Garnet's ceiling (~20) is below SimpleNetwork's (~24)** — a ratio of
**0.83**.

The hot resource is the mesh bisection: the four **center routers (5, 6, 9, 10)** have
the highest crossbar activity, as expected for uniform all-to-all traffic on a 4×4 mesh.
Both networks load those links identically (1 flit/cy/link in each); the difference is
how efficiently each *drains* them at saturation.

---

## 3. No realistic Garnet knob closes the gap

If the deficit were latency or buffering, the per-hop / buffer / VC knobs would recover
it. They do not — every one plateaus well below SimpleNetwork's 23.94:

| Garnet change (from default vcs8/buf1/rl4/lk2 = 19.83) | BW | note |
|---|---:|---|
| `vcs_per_vnet` 8 → 16 | 20.96 | more virtual lanes; small gain |
| `vcs_per_vnet` 8 → 32 | 20.97 | saturates — VCs plateau |
| `buffers_per_data_vc` 1 → 4 → 8 | 19.83 | **inert** (single-flit data packets, §5) |
| `router_latency` 4 → 3 | 20.51 | lower per-hop pipeline |
| `router_latency` 4 → 2 | 21.21 | still capped |
| `vcs=16` + `router_latency=2` + `router_link_latency=1` | 21.55 | combined |
| `vcs=16` + `router_latency=1` + `router_link_latency=1` | **21.84** | **aggressive — best realistic, still < 23.94** |

Even with the router pipeline cut to 1 cy, the credit round-trip halved, and double the
VCs, Garnet tops out at **~21.8 B/cy (≈91 % of SimpleNetwork)**. The residual is **not**
latency or buffering — it is **switching efficiency at saturation**.

---

## 4. The root cause: idealized PerfectSwitch vs. realistic SwitchAllocator

The two networks differ fundamentally in how a switch moves flits when many inputs
compete for the same outputs:

* **SimpleNetwork — `PerfectSwitch`** (`src/mem/ruby/network/simple/PerfectSwitch.cc`).
  As the name says, it is an *idealized* switch: each wakeup it sweeps all vnets and
  input ports and moves **every** routable head message whose destination port has a
  free slot. Effectively it achieves a **near-maximal input→output matching every
  cycle**, with a deep FIFO (`buffer_size = 8`) and a `Throttle` that drains 1 flit/cy
  smoothly. There is no arbitration *loss*: if an output can be used this cycle, some
  waiting message uses it.

* **Garnet — 2-stage separable `SwitchAllocator`**
  (`src/mem/ruby/network/garnet/SwitchAllocator.cc:91-278`). A realistic router:
  - **SA-i** (`arbitrate_inports`): each input port independently picks **one** input VC
    (round-robin) whose output VC is free and has a **credit**.
  - **SA-o** (`arbitrate_outports`): each output port independently grants **one** of the
    requesting inputs (round-robin).

  A separable round-robin allocator does **not** compute a maximal matching — two inputs
  can pick the *same* output in SA-i (only one wins SA-o, the other input's slot is
  wasted that cycle), and an output with no winner this round goes **idle even if a
  different input had a flit for it**. Combined with **credit flow control** (a flit also
  needs a downstream buffer credit, `send_allowed`, `SwitchAllocator.cc:294-349`), this
  leaves a fraction of crossbar/link cycles idle under saturation.

That fraction is the gap. A separable round-robin allocator typically sustains ~80–90 %
of ideal switch throughput, and **20 / 24 ≈ 0.83** lands squarely in that range. More VCs
raise the *SA-i* selection probability (hence 19.83 → 21) but cannot fix the *SA-o*
output-matching loss, which is why VCs plateau. Lower `router_latency` shortens the
pipeline but does nothing for matching efficiency, so it plateaus too. **PerfectSwitch is
a model, not a tunable router — there is no Garnet parameter that makes its allocator
ideal.**

### 4a. Verifying the two matching-loss mechanisms (against the code)

The two specific loss mechanisms above are not hand-waving; they are direct, provable
consequences of three facts in `SwitchAllocator.cc`:

**Fact A — SA-i issues exactly *one* request per input port, then stops.**
`arbitrate_inports` (`:116-146`) scans an input's VCs round-robin and, on the **first**
grantable VC, does:
```cpp
m_port_requests[inport] = outport;   // :135  one scalar outport per input
m_vc_winners[inport]    = invc;      // :136
break;                                // :138  stop — no second request from this input
```
`m_port_requests[inport]` is a **scalar**, not a per-output vector — an input physically
cannot request two outputs in a cycle.

**Fact B — SA-o grants exactly one input per output** (`arbitrate_outports`, `:169-...`):
each output round-robins over inputs and grants the first whose `m_port_requests[inport]`
equals it, then `break`s.

**Fact C — the allocation is single-pass.** `wakeup()` (`:92-99`) calls
`arbitrate_inports()` **once** and `arbitrate_outports()` **once**; there is no outer
loop. (A real iSLIP runs multiple iterations so unmatched inputs/outputs re-pair; Garnet
does not.)

From A + B + C the two claims follow exactly:

1. **Commit-then-lose.** An input commits (Fact A) to the *one* output its first
   grantable VC routes to. If it loses SA-o (Fact B) it sends **nothing** this cycle —
   and because there is no second iteration (Fact C) it never gets to offer a *different*
   VC's flit to a different, free output. Its turn is gone; `clear_request_vector()`
   (`:97`) wipes the request at cycle end.
2. **Idle output.** Symmetrically, a free output receives a request only if some input's
   single committed `m_port_requests` happens to equal it. If every input that holds a
   flit for output *Y* committed to a different output in SA-i, *Y* gets **no request and
   goes idle**, even though a flit for it was buffered.

**Worked 2×2 example** (router with inputs W, S and outputs E, N):

* Input **W** holds VC0→E (head in round-robin order) and VC1→N; input **S** holds VC0→E.
* **SA-i:** W's round-robin lands on VC0 first → W requests **E**. S requests **E**. (W's
  VC1→N is never offered — Fact A `break`.)
* **SA-o:** E grants one of them, say W. **N receives no request → idle** (claim 2). If
  instead E had granted S, then **W sends nothing** (claim 1) despite VC1→N being ready.

A maximal matching would have paired **W→N and S→E** (2 flits this cycle). The single-pass
separable allocator moves **1**. That wasted output slot, repeated under saturation, is
the deficit.

### 4b. Direct evidence: the bottleneck link is idle while flits wait

If the claims are real, Garnet's *busiest* link must run **below 100 %** even though the
workload is throughput-bound (demand exceeds capacity — proven by the flat outstanding
sweep, §2). It does. The busiest mesh links are the four center bisection edges; each
carries an **identical 1856 data flits** in both networks (same workload, same XY routing).
Dividing by each run's own ROI window (`simTicks`; §A.1):

| | busiest data link | ROI window | **utilization** |
|---|---:|---:|---:|
| **Garnet** | 1856 flits | 2646 cy | **0.701** |
| **Simple** | 1856 flits | 2192 cy | **0.847** |

Garnet leaves its bottleneck bisection link **idle ~30 % of cycles** (vs Simple's ~15 %)
**while flits are queued for it** — exactly the wasted-output-slot signature of §4a. And
the utilization ratio **0.847 / 0.701 = 1.208** equals the bandwidth ratio
**23.94 / 19.83 = 1.207** to three digits: *the entire BW gap is the bottleneck link's
extra idle time under Garnet's allocator.* (Note both are < 100 %: load is not perfectly
concentrated on one edge, and there is a fill/drain ramp inside the ROI; what matters is
the **relative** idle, which tracks the BW gap exactly.)

Finally, the credit-vs-matching split: at the current `vcs=8` the credit round-trip still
contributes (a single VC sustains ~1 flit/6 cy, so ~6 VCs are needed just to keep one link
full — see the steep low-VC curve, §3a below). Past `vcs≈8` credits are ample, yet Garnet
still cannot exceed ~21 B/cy → the residual **is** the SA matching loss verified here.

### 3a (addendum) — the VC sweep shows credit-round-trip + HoL recovery

| `vcs_per_vnet` | 1 | 2 | 4 | 8 | 16 | Simple (no VCs) |
|---|---:|---:|---:|---:|---:|---:|
| BW (B/cy) | 1.99 | 4.74 | 10.85 | 19.83 | 20.96 | **23.94** |

VCs lift Garnet 10× (1.99→20) — they are *essential* because each single-flit VC can only
hold one packet and must wait a full **credit round-trip** (~6 cy) before reuse, so you
need ~6 parallel VCs just to feed one 1-flit/cy link, plus VCs break input head-of-line
blocking. SimpleNetwork reaches **23.94 with a single FIFO per vnet and zero VCs**, because
its work-conserving `Throttle` (no credit handshake) on a deep buffer keeps the link fed
automatically. Garnet's VCs are a *remedy* for penalties (credit round-trip, HoL) that the
idealized PerfectSwitch never pays; once those are paid down (vcs≥8), the §4a matching loss
is what remains between ~21 and Simple's ~24.

---

## 5. Why `buffers_per_data_vc` is inert (and a caution)

Setting `buffers_per_data_vc` to 1, 4, or 8 gives **byte-identical** BW (19.83). Data
packets are **single-flit** (`ni_flit_size = 40 B` = one 40 B beat). A VC is held by one
packet from HEAD to TAIL; a single-flit packet occupies exactly **one** buffer slot and
frees it on the same flit, so depth > 1 is never used. Buffer depth would only matter for
**multi-flit** packets (smaller flit width or larger beats). For this workload the only
buffering lever that does anything is the **number of VCs**, and even that plateaus
(§3/§4).

---

## 6. How to actually match (and beat) SimpleNetwork

Since the deficit is the data vnet saturating the center links under Garnet's
less-efficient allocator, the effective fix is to **relieve that bottleneck** rather than
chase latency. The line is 2 data beats only because `data_width = 32`. Making it **1
beat** (`data_width = 64`, so each 64 B line is a single 72 B flit) halves the data-vnet
flit count on the saturated links:

| Garnet config | BW | vs Simple 23.94 |
|---|---:|---|
| default (`data_width=32`, 2 beats), vcs=8 | 19.83 | −17 % |
| best realistic latency/VC tuning | 21.84 | −9 % |
| **`data_width=64` (1 beat/line), vcs=8** | **25.89** | **+8 %** |
| `data_width=64`, vcs=16 | 25.97 | +8 % |

With 1-beat data, Garnet **exceeds** SimpleNetwork (25.9 vs 23.94) — the allocator
inefficiency no longer binds because the data vnet is no longer the saturated resource.
(`data_width` also widens the data link, so this both halves the beat count *and*
provisions more per-link data bandwidth; either way it removes the bottleneck that
exposed Garnet's allocator.) Note this is a **protocol/NoC** change that would also speed
up SimpleNetwork — it is the way to make *Garnet* reach the target number, not an
apples-to-apples re-tune.

**Summary of levers**

| Goal | Change | Result |
|------|--------|--------|
| Close the gap with realistic NoC knobs | `vcs↑`, `router_latency↓`, `router_link_latency↓` | **caps at ~21.8 (91 %)** — cannot fully match |
| Match / exceed SimpleNetwork | `data_width = 64` (1 data beat/line) | **25.9 B/cy (>23.94)** |
| (Inert) | `buffers_per_data_vc` | no effect (single-flit data) |

The honest conclusion: **at identical router latency, Garnet's realistic switch is
structurally ~10–17 % less efficient than the idealized PerfectSwitch on a saturated
all-to-all mesh.** You either accept that (it is the *more* faithful hardware model) or
remove the saturation (fewer/ wider data beats).

---

## Appendix A — Methodology

### A.1 Runs

All via `util/run_with_timeout.sh`, each into its own `-d m5out/...` dir. Baselines with
no debug flags; the phase split (§1) from `--debug-flags=ProtocolTrace --debug-file=trace.gz`.
ROI windows (assigned per run from each run's own `simTicks`/`finalTick` in `stats.txt`,
**not** from the interleaved parallel stdout): **Garnet** `[12189500, 13512500]`
(`simTicks = 1323000` = 2646 cy), **Simple** `[11517500, 12613500]` (`simTicks = 1096000`
= 2192 cy). The 2646/2192 cy windows match the per-core `over N clocks` in each BW line.

### A.2 Phase split (§1)

Addresses are **disjoint per core** (rnf0 uses 0x0/0x100/0x1000…, rnf1 uses 0x10000+),
verified from the trace, so a version-0 (rnf0) `SendReadUnique` for address X can be
matched unambiguously to the HNF (version ≥16) `ReadUnique_PoC` / `SendCompData` for the
same X. For each rnf0 transaction:

* **request leg** = `ReadUnique_PoC`(HNF) − `SendReadUnique`(rnf0)
* **HNF leg** = `SendCompData`(HNF) − `ReadUnique_PoC`(HNF)
* **data return** = `total` − request − HNF, where **total** = 2nd `CompData_UD_PD`(rnf0)
  − `SendReadUnique`(rnf0)

The reconstructed `total` mean reproduces `stats.txt`
`outTransLatHist.SendReadUnique::mean` exactly (97.6 / 80.8 cy), validating the split.

### A.3 Throughput-vs-latency test (§2)

Sweep `--num-outstanding-reqs ∈ {32, 64, 128}`. BW flat ⇒ throughput-bound; BW rising ⇒
latency-bound. Garnet flat at ~20 ⇒ throughput-bound at its ceiling.

### A.4 Parameter sweeps (§3, §6)

* `vcs_per_vnet`: CLI `--vcs-per-vnet=N` (read at `create_network`,
  `configs/network/Network.py:180`).
* `buffers_per_data_vc`: runtime `--param 'system.ruby.network.buffers_per_data_vc=N'`
  (verified in the run's `config.ini`).
* `router_latency`, `router_link_latency`, `data_width`: live in `NoC_Params`; swept via
  **temporary copies** of `configs/example/noc_config/rbook_4x4.py` passed with
  `--chi-config=/tmp/noc_*.py` (the real config file is left untouched). `NoC_Params` is
  copied into `options` at `CHI.py:246` and consumed by `CustomMesh.makeTopology`; the
  driver auto-sizes `link_width_bits` for single-flit data packets
  (`_ensure_single_flit_garnet`).

### A.5 Files / source references

* Idealized switch: `src/mem/ruby/network/simple/PerfectSwitch.cc:152-300`,
  `Throttle.cc:139-334`.
* Realistic allocator: `src/mem/ruby/network/garnet/SwitchAllocator.cc:91-349`
  (SA-i/SA-o, `send_allowed` credit check), `Router.cc:71-97`,
  `NetworkInterface.cc:204-220,396-423`.
* Config wiring: `configs/ruby/CHI.py:242-248`, `configs/network/Network.py:180`,
  `configs/topologies/CustomMesh.py`, `configs/example/noc_config/rbook_4x4.py`.
* Run dirs: `m5out/allcore-{garnet,simple}-*` (baselines, stats),
  `m5out/ac-{garnet,simple}-pt-*` (ProtocolTrace), plus the sweep dirs
  `m5out/{os,sw,grl,cmb,dw}-*`.
</content>
