# Garnet Latency And Buffering

This note describes the Garnet model used by the CHI testbench. It is not a
transaction-latency report; its purpose is to show which buffers, latencies,
credits, and arbitration points exist in one directed router-to-router hop when
the 4x4 mesh runs with:

```text
--network=garnet
--per-vnet-links=true       # default in the testbench driver
--vcs-per-vnet=8            # default in the testbench driver
configs/example/noc_config/rbook_4x4.py
```

The driver sizes `--link-width-bits` so a CHI data packet is one Garnet flit.
The C++ method is still named `flitisizeMessage()`, but in this configuration it
creates exactly one `HEAD_TAIL` flit per unicast packet. This note therefore
treats packet and flit as the same object.

The testbench tiles connect directly to mesh routers with one ExtLink. The
endpoint link, NetworkInterface, and controller queues are separate from the
router-to-router hop described here.

## Every one-hop parameter

These are the values that matter for one internal Garnet hop in this testbench.

```text
4x4 mesh routers

   0 --- 1 === 2 --- 3
   |     |     |     |
   4 --- 5 === 6 --- 7
  ===   ===   ===   ===
   8 --- 9 ===10 ---11
   |     |     |     |
  12 ---13 ===14 ---15

--- normal mesh link: router_link_latency = 2 cycles
=== cluster-boundary link: cross_link_latency = 7 cycles
```

`cross_link_latency` replaces `router_link_latency` for the directed links
listed in `cross_links`. It is not added on top of `router_link_latency`.

| Parameter | Value | Where it is used |
| --- | ---: | --- |
| `router_latency` | 4 cy | `GarnetRouter.latency`; a flit waits `latency - 1` cycles in the source router input VC, then takes switch allocation/traversal and reaches the output link on the next cycle. |
| `router_link_latency` | 2 cy | Normal directed router-to-router `NetworkLink.link_latency`. |
| `cross_link_latency` | 7 cy | Directed cluster-boundary router-to-router `NetworkLink.link_latency`. |
| `number_of_virtual_networks` | 4 | CHI REQ, SNP, RSP, and DAT channels. |
| `per_vnet_links` | true | `CustomMesh` creates one physical directed `GarnetIntLink` per vnet per mesh direction. |
| `vcs_per_vnet` | 8 | Eight VCs, and therefore eight single-packet link-credit slots, per CHI channel on each input/output port. |
| `buffers_per_ctrl_vc` | 1 credit | Initial credit count for REQ, SNP, and RSP VCs. |
| `buffers_per_data_vc` | 1 credit | Initial credit count for DAT VCs. |
| `ni_flit_size` | 40 B | One flit holds a 32 B CHI data beat plus the 8 B Ruby control header. |
| CDC / SerDes bridges | off | No `NetworkBridge` latency is enabled for these mesh links. |

Derived structural costs for a ready flit with no arbitration or credit wait:

| Hop | Cost | Meaning |
| --- | ---: | --- |
| Normal internal hop | `4 + 2 = 6 cy` | Source router pipeline plus normal mesh link. |
| Cross internal hop | `4 + 7 = 11 cy` | Source router pipeline plus cluster-boundary mesh link. |

That cost is measured from the cycle when Router A consumes the flit from its
input link to the cycle when Router B consumes it from the A-to-B link. Router
B's own pipeline starts after this hop.

The important Garnet buffers and credit state for one directed A-to-B link are:

| Storage / state | Depth in this testbench | Backpressure check |
| --- | ---: | --- |
| A input VC buffers | 8 VCs per vnet, credit-limited to 1 flit each | A flit cannot leave until switch allocation wins and downstream credit exists. |
| A `OutputUnit.outBuffer` | transient `flitBuffer` | Fed by crossbar, drained by the output link one flit/cycle. |
| A-to-B `NetworkLink.linkBuffer` | delayed in-flight flits | Models link latency; not the finite credit-limited buffer. |
| B input VC buffers | 8 VCs per vnet, credit-limited to 1 flit each | The finite slots represented by A's output VC credits. |
| B-to-A `CreditLink` | delayed credit objects | Returns a free VC/credit to A after B releases the input VC. |

`router_buffer_size` is a SimpleNetwork parameter. It does not size Garnet
router buffers.

## How a single flit hop works in Garnet

Garnet models a router-to-router hop as an input-VC router pipeline, a one-flit
per-cycle output link, and an explicit credit link in the reverse direction.

```text
Legend:
  ╔═════╗  credit-counted VC storage; consumes a CHI-channel LCredit slot
  ║░░░░░║  delayed in-flight link storage; carries time, not finite credits
  ┌─────┐  logic/arbitration stage
  ◁═════  reverse credit path

One directed internal link, one CHI channel: Router A → Router B

╭── Router A ────────────────────────────────────────────────────────────────╮
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: A input VC                                                   ║   │
│ ║ one flit, already assigned to this vnet and input VC                 ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
│                                   │ route_compute() on HEAD_TAIL           │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: router pipeline wait                                          │   │
│ │ flit becomes ready for SA after router_latency - 1 = 3 cycles        │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: SwitchAllocator                                               │   │
│ │ require a free B input VC for this vnet; allocate it                 │   │
│ │ decrement A's credit for that downstream VC                          │   │
│ │ at most one input wins each output port per cycle                    │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: CrossbarSwitch / OutputUnit                                   │   │
│ │ move winning flit to A output queue; output link wakes next cycle    │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
╰────────────────────────────────────────────────────────────────────────────╯
                                    │ link latency L
                                    ▼
╭── A→B NetworkLink ─────────────────────────────────────────────────────────╮
│ ║░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░║   │
│ ║ DELAY: flit in flight; ready at Router B after L cycles              ║   │
│ ║ normal L = 2 cycles, cross-cluster L = 7 cycles                      ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
╰────────────────────────────────────────────────────────────────────────────╯
                                    ▼
╭── Router B ────────────────────────────────────────────────────────────────╮
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: B input VC                                                   ║   │
│ ║ the slot selected by A's output VC allocation                        ║   │
│ ║ credit remains unavailable to A while this flit is here              ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
│                                   │ later, when B wins SA                  │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: B SwitchAllocator                                             │   │
│ │ on HEAD_TAIL, free B input VC and enqueue a credit back to A         │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
╰────────────────────────────────────────────────────────────────────────────╯
                                    ◁════════ B→A CreditLink, latency L
```

The forward latency of the hop is:

```text
Router A input-link consume
  + (router_latency - 1) cycles waiting in A input VC
  + 1 cycle for switch traversal / output-link launch
  + L cycles on A→B NetworkLink
= router_latency + L
```

So the two router-to-router cases are:

```text
normal mesh link: 4 + 2 = 6 cycles
cross mesh link:  4 + 7 = 11 cycles
```

## Switch allocation arbitration

Garnet switch allocation is two-stage and not fully work-conserving:

```text
SA-I:  each InputUnit records at most one VC/output request
SA-II: each output grants at most one requesting InputUnit
```

One `InputUnit` can read at most one flit per cycle. If it chooses a VC for East
in SA-I but loses East in SA-II, it sends nothing that cycle; Garnet does not
retry that input with another ready VC for North. A VC that wins SA-II is removed
from its input VC, assigned the downstream VC, has that downstream credit
consumed, and is queued to the crossbar/output unit.

With `per_vnet_links=True`, different CHI channels from the same neighbor are
different physical input links, hence different `InputUnit`s. Each can send one
flit per cycle if it wins its own output arbitration.

## Credits and CHI LCredits

With `per_vnet_links=True`, each CHI channel has its own physical mesh links:

```text
REQ vnet 0:  A.req_link ─────────▶ B.req_link_in
SNP vnet 1:  A.snp_link ─────────▶ B.snp_link_in
RSP vnet 2:  A.rsp_link ─────────▶ B.rsp_link_in
DAT vnet 3:  A.dat_link ─────────▶ B.dat_link_in
```

This is the Garnet approximation of an RTL router with LCredits on each CHI
channel. The exact mapping is:

| CHI idea | Garnet object in this setup |
| --- | --- |
| CHI channel (`REQ`, `SNP`, `RSP`, `DAT`) | one vnet-specific physical link |
| CHI LCredit count for that channel | count of free downstream Garnet VC slots |
| one consumed LCredit | one allocated downstream VC with its credit consumed |
| one returned LCredit | one returned Garnet credit/free signal |

A Garnet VC is not itself the credit packet. It is the virtual lane/allocation
identity. The credit is the availability state for that downstream VC. Because
this testbench uses one-flit packets and one credit per VC, an idle downstream
VC behaves exactly like one available CHI LCredit slot.

For one direction and one channel, Router A has eight output VC states. Each
output VC mirrors one possible input VC at Router B.

```text
A output VC state for channel c          B input VC slots for channel c

  VC0 credit ─────────────────────────▶  ╔═══╗ slot VC0
  VC1 credit ─────────────────────────▶  ╔═══╗ slot VC1
  VC2 credit ─────────────────────────▶  ╔═══╗ slot VC2
  VC3 credit ─────────────────────────▶  ╔═══╗ slot VC3
  VC4 credit ─────────────────────────▶  ╔═══╗ slot VC4
  VC5 credit ─────────────────────────▶  ╔═══╗ slot VC5
  VC6 credit ─────────────────────────▶  ╔═══╗ slot VC6
  VC7 credit ─────────────────────────▶  ╔═══╗ slot VC7

  8 idle downstream VCs = 8 LCredit-like packet slots for this channel.
  A may launch up to 8 single-flit packets before it must wait for B to
  release one of those input VCs.
```

For `HEAD_TAIL` flits, `SwitchAllocator` only needs a free downstream VC. It
then decrements that VC's credit count and sends the flit. When B later forwards
or ejects the flit, B sends a credit with the free signal set, and A marks that
output VC idle again. In CHI terms: A consumed one LCredit, then B returned it.

This is close to CHI LCredit flow control: it has one credit pool per CHI
channel and direction, explicit credit objects on a reverse `CreditLink`, and
source stalls when no downstream VC credit is available. It is not exact RTL:
Garnet does not model separate CHI channel micro-architecture, CHI credit
packetization, or multi-flit wormhole occupancy for these packets.

Because every packet is one flit, there is no wormhole routing case to reason
about. A packet never occupies a sequence of body flits behind a head flit.
Single-packet concurrency comes from `vcs_per_vnet`, not from per-VC flit depth;
increasing `buffers_per_data_vc` above 1 would not create more single-packet
LCredit slots in this setup.

## What happens when B is blocked

For one directed A-to-B link, one vnet, and B unable to forward downstream, the
finite credit-counted storage is eight packets:

```text
A can allocate B input VCs until all 8 VC credits are consumed:

  B input VC slots for this channel
  ╔═══╦═══╦═══╦═══╦═══╦═══╦═══╦═══╗
  ║ 1 ║ 2 ║ 3 ║ 4 ║ 5 ║ 6 ║ 7 ║ 8 ║
  ╚═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╝

  A's ninth packet for this channel and direction cannot win SA.
```

The ninth packet remains in A's input VC. Since that input VC is still occupied,
A does not send a free credit to the previous hop for that flit either. This is
how backpressure propagates upstream.

The delayed `NetworkLink.linkBuffer` can contain flits in flight, especially on
the 7-cycle cross links, but it is not the finite backpressure queue. The finite
limit seen by A is the number of downstream VC credits.

## Normal links versus cross links

Increasing `cross_link_latency` has two effects in this Garnet configuration:

```text
1. It increases forward delay:
     structural cross-hop cost = router_latency + cross_link_latency
                               = 4 + 7 = 11 cycles

2. It increases credit round-trip time:
     a free credit cannot return until the flit reaches B, waits in B's
     router pipeline, wins B's switch, and traverses the reverse CreditLink.
```

For this testbench's `router_latency = 4`, the minimum free-credit turnaround
after A grants the flit is:

```text
1 cycle to launch A→B link
+ L cycles forward link
+ (router_latency - 1) cycles in B input VC before B SA
+ 1 cycle to launch B→A credit link
+ L cycles reverse credit link
= 2L + router_latency + 1

normal L = 2:  2*2 + 4 + 1 = 9 cycles
cross  L = 7:  2*7 + 4 + 1 = 19 cycles
```

If B is blocked, the turnaround is longer. Cross-link latency therefore does
not increase the number of LCredit slots; it only keeps consumed credits away
from A for more cycles.
