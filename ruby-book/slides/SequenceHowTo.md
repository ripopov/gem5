# How to Read CHI Transaction Diagrams

A reader's guide to the sequence diagrams in AMBA CHI spec section **B2.3 Transaction structure**.

The goal of this note is to give you a mental model so that when you open, say, *B2.3.1.2 Non-allocating Read* and stare at a knot of overlapping arrows labeled `CompData / RespSepData / DataSepResp`, you know exactly what the picture is saying and, more importantly, what it is *not* saying.

The trap most new readers fall into is treating each figure as one scenario.
It is not.
A single B2.3.x figure is a **superposition of every legal flow** for that transaction class.
Once you learn to decompose the superposition into alternatives, the diagrams become easy.

---

## 1. Start with the cast

Every B2.3 diagram has at most four actors.
Memorize them in this order — the diagrams almost always lay them out left-to-right in roughly this order.

| Short name | Full name | Role |
|------------|-----------|------|
| RN         | Request Node (Requester) | The agent that *issued* the transaction. An RN-F (fully coherent CPU cluster), RN-I (IO, non-caching), or RN-D (DVM-capable IO). |
| HN         | Home Node | The *Point of Coherence* and *Point of Serialization* for the line. Lives inside the interconnect. HN-F is the coherent home; HN-I is the non-coherent home. |
| SN         | Subordinate Node | Memory-side endpoint. SN-F for normal memory, SN-I for peripherals. Talks only to a Home. |
| Snoopee    | Peer RN-F being snooped | Another RN-F that holds or might hold a copy. Drawn as a separate lifeline even though architecturally it is just "another RN-F." |

Two rules make the cast behave predictably:

- "Requester" in B2.3 prose *always* means the agent that originally issued the transaction. It never slides to mean an intermediate agent that re-issues a secondary request (the spec is explicit: see the note just above B2.3.1).
- A Snoopee is not a separate node type. It is a role. A CPU that is a Requester for one line is a Snoopee for another line at the same instant.

If you ever see a figure lifeline and are not sure who it is, ask: *Is this the agent that issued the request the figure is titled with?*
If yes, it is the Requester.
Otherwise, it is Home, Subordinate, or Snoopee, in that order of likelihood.

---

## 2. Learn the channels before the messages

Messages in CHI do not float in free space.
Each one rides on a specific channel, and channels have fixed directions.
Once you have the channel map in your head, you can read an arrow's *channel* off its source and destination without squinting at labels.

```
                Requester (RN)                     Home (HN)                       Subordinate (SN)
                 ┌──────────┐                    ┌─────────┐                     ┌──────────┐
                 │          │  REQ (TXREQ→RXREQ) │         │  REQ (TXREQ→RXREQ)  │          │
                 │          │───────────────────▶│         │────────────────────▶│          │
                 │          │                    │         │                     │          │
                 │          │  RSP (CRSP)        │         │  RSP (CRSP)         │          │
                 │          │◀───────────────────│         │◀────────────────────│          │
                 │          │  DAT (RDAT)        │         │  DAT (RDAT)         │          │
                 │          │◀───────────────────│         │◀────────────────────│          │
                 │          │  DAT (WDAT)        │         │  DAT (WDAT)         │          │
                 │          │───────────────────▶│         │────────────────────▶│          │
                 │          │  RSP (SRSP, e.g.   │         │                     │          │
                 │          │   CompAck)         │         │                     │          │
                 │          │───────────────────▶│         │                     │          │
                 │          │  SNP (RXSNP)       │         │                     │          │
                 │          │◀───────────────────│         │                     │          │
                 └──────────┘                    └─────────┘                     └──────────┘
```

The five channel names you will see in B2.3 prose are the **shorthand** names, not the physical port names:

| Shorthand | Carries | At RN | At SN |
|-----------|---------|-------|-------|
| REQ  | Transaction requests                                                      | TXREQ (outbound) | RXREQ (inbound)  |
| SNP  | Snoop requests                                                            | RXSNP (inbound)  | —                |
| WDAT | Write data, atomic data, snoop data, forwarded data                       | TXDAT (outbound) | RXDAT (inbound)  |
| RDAT | Read data, atomic data                                                    | RXDAT (inbound)  | TXDAT (outbound) |
| CRSP | Completer → Requester responses (Comp, CompData, DBIDResp, ReadReceipt…)  | RXRSP (inbound)  | TXRSP (outbound) |
| SRSP | Snoop responses and CompAck (Requester → Home)                            | TXRSP (outbound) | —                |

Notice that `TXRSP` on an RN and `TXRSP` on an SN mean *different* shorthand channels (SRSP vs CRSP).
That's why the spec uses the shorthand everywhere in B2.3. If you are ever confused about whether a message is on "the response channel," check: *is it going towards the Requester (CRSP) or towards the Home (SRSP)?*

### Channel-color crutch

If you color-code the arrows as you read — REQ blue, SNP gold, RSP green, DAT violet — the diagrams stop looking tangled almost immediately. The same color palette is used in the gem5 CHI slides and in `gem5-chi.css`. Use it as a consistent crutch.

---

## 3. The single most important convention: Alternatives

This is the one idea that makes the diagrams hard for beginners and trivial for veterans.

**A B2.3 figure is not one transaction. It is a decision tree of every possible way that transaction class can complete, flattened onto one picture.**

The prose underneath the figure enumerates the branches, usually as numbered **Alternatives**:

- Alt 1: Combined response from Home.
- Alt 2: Separate data and response from Home.
- Alt 3: Combined response from Subordinate (DMT).
- Alt 4: Response from Home, data from Subordinate (DMT).
- Alt 5: Forwarding snoop (DCT) — often with sub-alternatives 5a, 5b, 5c, 5d.
- Alt 6: Sometimes reserved for a specific opcode (e.g. `MakeReadUnique`-only in B2.3.1.1).

A single real-world transaction follows **exactly one** of those alternatives.
Home picks which alternative based on where the data lives, whether a snoop is promising, and implementation choices.

Practical consequences for reading:

1. When you see the picture, do not try to trace every arrow at once. Pick one alternative number and mentally erase the others.
2. If an arrow is drawn dashed or is tagged "optional" in the prose, it exists in *some* alternatives and not others. Check which.
3. Alternatives can share messages. The initial REQ from Requester to Home is the same arrow in every alternative. That is why it is drawn once.

### "Arrows with multiple labels"

The spec states this explicitly (B2.3 preamble):

> In this section, a diagram with an arrow containing multiple message labels indicates any one of the messages can be sent in a flow.

So if you see one arrow labeled `CompData / RespSepData`, read it as:
*"In this step, exactly one of `CompData` or `RespSepData` is sent; which one depends on the chosen alternative."*

Not both. Not "first one, then the other." Exactly one, per the alternative in play.

The exception is when two arrows are drawn separately and the prose says both happen — e.g. `RespSepData` *and* `DataSepResp` in Alt 2. Then they are two distinct messages on the same channel pair, and both are sent.

### Dependencies are implicit unless the text says otherwise

B2.3 uses a minimal dependency convention (stated in the B2.3 preamble):

- An agent cannot send anything for a transaction until it has received the first message for that transaction. (First message in, before anything out.)
- After the first input, any output the agent produces may be sent in any order with respect to each other, *unless* the text explicitly mandates an order ("after", "before", "only after receiving…").
- Data transfers that span multiple DAT beats are drawn as a single arrow. If there is a dependency on data, it is on the *first* beat unless the text says otherwise.

This is why so few arrows in these figures are labeled with explicit "wait-for" conditions. Assume the minimal convention, and only track the explicit ones the prose calls out.

---

## 4. The verbs the spec uses

Four verbs appear constantly. They look synonymous; they are not.

| Verb       | Meaning                                                                                              |
|------------|------------------------------------------------------------------------------------------------------|
| Issues     | Used *only* for the first message of a transaction. "The Requester issues a ReadNoSnp request..."    |
| Sends      | Any message moving *away from* the Requester. Applies to Home and to forwarded messages.             |
| Returns    | Any message moving *towards* the Requester. `CompData`, `ReadReceipt`, and `Comp` are all returned.  |
| Provides   | Used specifically for a snoop response. A Snoopee "provides" `SnpResp`, `SnpRespData`, etc.          |

And three modal phrases that matter:

- *Optional* / *Permitted but not required*: the agent may or may not do this. Legal either way.
- *Not permitted*: doing this is non-compliant.
- *Must* / *is required to*: mandatory. A compliant implementation has no choice.

When an arrow in the figure has the word "Optional" next to it, it only appears in some alternatives or under some field settings (e.g. `ReadReceipt` only when `Order != 0`).

---

## 5. The fields that pick the flow

Most of the time, picking *which* alternative is taken is a decision Home makes dynamically based on state.
But several request-packet fields narrow the legal alternatives up front.
These are the ones that reliably affect transaction structure (the "Affects structure: Yes" entries in Table B2.2):

| Field        | What it controls                                                                                                   |
|--------------|--------------------------------------------------------------------------------------------------------------------|
| `Opcode`     | Primary selector. `ReadNoSnp` is non-allocating, `ReadShared` is allocating, etc.                                  |
| `Order`      | `00` = unordered, `10/11` = ordered (OWO or request-ordered). Ordered reads typically get a `ReadReceipt`.         |
| `ExpCompAck` | 1 ⇒ the Requester will send `CompAck` to close the loop. 0 ⇒ it will not, and Home must close the loop differently. |
| `Size`       | Selects full-line vs partial flows; restricts DMT/DCT for partial reads.                                           |
| `DoDWT`      | 1 ⇒ Requester allows Direct Write-data Transfer, so Home may forward data straight to SN.                          |
| `TagOp`      | Turns on tagged-memory data transfer rules.                                                                        |
| `AllowRetry` | Permits the Completer to answer `RetryAck`.                                                                        |
| `MultiReq`   | Combined with `NumReq` and `Size`, declares a multi-line transaction.                                              |

When you are decoding a diagram, it helps to fix these fields first. "Assume `ReadNoSnp`, `Order=00`, `ExpCompAck=0`, `Size=64B`" will cut most B2.3.1.2 alternatives in half.

The reverse direction matters too: **Table B2.6** in the spec tabulates which DMT/DCT combinations are legal for each `(Order, ExpCompAck)` pair. That table is the reason some "visible" alternatives in the figure are actually illegal for your chosen configuration.

---

## 6. The three hop-reduction families: DMT, DCT, DWT

Every figure in B2.3 exists in a world that offers three optional shortcuts for cutting out the Home as a middle hop.
If you know these three, you can classify any alternative at a glance.

```
                No shortcut (default)              DMT                             DCT                                 DWT
Read flow:

  RN ───REQ────▶ HN               RN ───REQ────▶ HN               RN ───REQ────▶ HN ───SNP────▶ Snoopee    (writes, not reads)
                 │                                │                                        │
                 ├─REQ─▶ SN                       ├─REQ────▶ SN                             └─CompData──▶ RN
                 │                                │                                        SnpRespFwded ──▶ HN
                 ◀─CompData─                      CompData─────▶ RN  (bypass HN)
  RN ◀─CompData─ HN

Write flow (DWT):

  RN ───REQ────▶ HN ───REQ(DoDWT=1)───▶ SN
  RN ───WriteData─────────────────────▶ SN   (data bypasses HN)
  RN ◀─Comp────────────────────────── HN
```

| Shortcut | Expands to               | Effect                                                                                                          |
|----------|--------------------------|-----------------------------------------------------------------------------------------------------------------|
| DMT      | Direct Memory Transfer   | Subordinate sends `CompData` (or `DataSepResp`) *directly* to the Requester, skipping Home on the return leg.   |
| DCT      | Direct Cache Transfer    | A Snoopee sends `CompData` *directly* to the Requester via `Snp*Fwd`. Snoopee still owes Home a snoop response. |
| DWT      | Direct Write-data Transfer | Requester sends write data *directly* to the Subordinate instead of through Home.                             |

When you read an Alternative number, it pays to ask: *Is this the non-shortcut flow, or does it invoke DMT, DCT, or DWT?*

- In B2.3.1.1 (Allocating Read), Alt 1 and 2 are Home-centric. Alt 3 and 4 are DMT. Alt 5 is DCT.
- In B2.3.1.2 (Non-allocating Read), the same pattern holds: Alt 3/4 are DMT, Alt 5 is DCT, Alt 1/2 keep Home in the middle.
- In B2.3.2.1 (Immediate Write), "Alt 1. DWT" and "Alt 2/3. Without DWT" are the two top-level branches.

---

## 7. How to read **B2.3.1.2 Non-allocating Read** end-to-end

Let's apply the machinery to the exact figure the question asked about.

### Step 1 — Identify the cast

Figure B2.2 (the figure inside B2.3.1.2) shows lifelines for Requester, Home, Subordinate, and Snoopee.
You will use all four only if Alt 5 (forwarding snoop) is taken.
For Alt 1–4 the Snoopee lifeline is dormant.

### Step 2 — Fix the opcode

The section header tells you the in-scope opcodes:

- `ReadNoSnp`
- `ReadOnce`
- `ReadOnceCleanInvalid`
- `ReadOnceMakeInvalid`

All of these are **non-allocating reads** — the Requester will not cache the line in a coherent state. That is why the figure is simpler than B2.3.1.1: there is no `CompAck` for coherence ordering unless `ExpCompAck=1`.

### Step 3 — Fix `Order` and `ExpCompAck`

The prose explicitly calls out that these two fields affect the flow. Legal combinations come from Table B2.6:

| `Order` | `ExpCompAck` | DMT | DCT | What it means                                                                                       |
|---------|--------------|-----|-----|-----------------------------------------------------------------------------------------------------|
| 00      | 0            | Y   | Y   | Unordered, no CompAck. Home does not need closing acknowledgement.                                  |
| 00      | 1            | Y   | Y   | Unordered, Requester chooses to send CompAck anyway (often to simplify Home's DMT bookkeeping).     |
| 01      | —            | —   | —   | Reserved / not permitted for these opcodes.                                                         |
| 10, 11  | 0            | N   | Y   | Ordered, no CompAck. DMT is illegal because Home needs another completion signal to order later requests. |
| 10, 11  | 1            | Y   | Y   | Ordered, CompAck closes the loop.                                                                  |

So before you look at an alternative, pick a row of that table. That immediately rules out several arrows.

### Step 4 — Walk one alternative at a time

Here is the full decomposition, in the order the spec lists them.

**Alt 1 — Combined response from Home.**
One REQ out, one `CompData` back. If `Order != 0`, a `ReadReceipt` may also come back before `CompData`. Simplest flow.
Used when Home already has the data (e.g. it's cached inside the interconnect).

**Alt 2 — Separate response and data from Home.**
Home returns `RespSepData` on CRSP and `DataSepResp` on RDAT, separately. The response carries the cache-state/permission information; the data can come later. Not legal with ordered-without-CompAck.

**Alt 3 — DMT, combined response from Subordinate.**
Home turns the read into a downstream `ReadNoSnp` to SN. Subordinate sends `CompData` *directly* to the Requester. Optionally SN returns a `ReadReceipt` to Home so Home can close its bookkeeping. Not legal with ordered-without-CompAck.

**Alt 4 — DMT, response from Home, data from Subordinate.**
Home sends `RespSepData` to Requester (fast, because Home knows permissions) and issues `ReadNoSnpSep` to SN. SN returns `DataSepResp` to the Requester and optionally `ReadReceipt` to Home. This is the latency-minimising flow when Home has the metadata but not the data.

**Alt 5 — DCT, forwarding snoop.**
Home requests a Snoopee via `Snp*Fwd` to return the data directly to the Requester.
Four sub-alternatives describe how the Snoopee may respond:

- 5a: Snoopee sends `CompData` → Requester *and* `SnpRespFwded` → Home (RSP only).
- 5b: Snoopee sends `CompData` → Requester *and* `SnpRespDataFwded` → Home (RSP + a data copy to Home).
- 5c: Snoopee refuses with `SnpResp` on RSP. Home must fall back to Alt 1/2/3/4.
- 5d: Snoopee refuses with `SnpRespData` / `SnpRespDataPtl` on DAT. Same fallback requirement.

Alts 5c and 5d are why the figure looks like DCT "can" happen but isn't guaranteed: the Snoopee has the right to punt.

**Alt 6** is not used in B2.3.1.2.

### Step 5 — Close the loop

If `ExpCompAck=1`, the Requester must end with `CompAck` to Home, but only after one of:

- a `CompData` arrived, or
- `RespSepData` arrived (for unordered requests — `DataSepResp` can follow later), or
- both `RespSepData` *and* at least one `DataSepResp` arrived (for ordered requests).

If `ExpCompAck=0`, no `CompAck` is sent. The transaction ends at the Requester when data and any required `ReadReceipt` have been received.

### Concrete reading: `ReadNoSnp`, `Order=0`, `ExpCompAck=0`, DMT

```
RN                               HN                           SN
 │        ReadNoSnp (REQ)        │                            │
 │ ──────────────────────────▶   │                            │
 │                               │      ReadNoSnp (REQ)       │
 │                               │ ─────────────────────────▶ │
 │                               │      ReadReceipt (CRSP)    │
 │                               │ ◀───────────────────────── │
 │             CompData (RDAT, direct from SN)                │
 │ ◀──────────────────────────────────────────────────────────│
 │                               │                            │
 (no CompAck — ExpCompAck = 0)
```

This is Alt 3. Now re-look at Figure B2.2 with all arrows *not* belonging to Alt 3 mentally greyed out. The figure should feel readable.

---

## 8. A portable checklist for reading any B2.3 figure

Every time you open a new transaction (say B2.3.2.1 or B2.3.4), run this checklist:

1. **Title check.** What opcode(s) does this figure cover? Copy them into your scratch notes.
2. **Cast check.** Which lifelines are drawn? Label each: Requester, Home, Subordinate, Snoopee.
3. **Field pinning.** What `Order`, `ExpCompAck`, `DoDWT`, `Size`, and `TagOp` are you assuming? The prose will list "The request contains the following fields which affect the transaction flow."
4. **Alternative enumeration.** List the alternatives the prose calls out. Count them. The figure should encode exactly that many legal paths.
5. **Classify by shortcut.** For each alternative, mark whether it uses DMT, DCT, DWT, or none.
6. **Pick one alternative.** Mentally erase the others. Trace the arrows in channel order: REQ → (optional SNP) → DAT → CRSP/SRSP → closing CompAck if any.
7. **Verify the closing condition.** Check `ExpCompAck` and `Order` to confirm the last message matches the rules in B2.7.3 and Table B2.6.
8. **Sanity-check dependencies.** For each agent that sends more than one message, confirm you are not assuming an ordering the spec does not require.

If step 4's count does not match the number of alternatives in the prose, you have miscounted arrows. If step 7 fails, the combination of fields you picked is not legal for this transaction.

---

## 9. Gotchas that confuse almost everyone

A short catalogue of the recurring traps, and how to avoid each one.

- **"Arrows with slashes are choices, not sequences."**
  `CompData / RespSepData` on one arrow means *one of the two* in the chosen alternative. Not "first one, then the other."

- **"The Requester never changes mid-transaction."**
  Even in DCT where Home bounces the request to a Snoopee, the Requester is still the original RN. Intermediate hops are not Requesters.

- **"`TXRSP` at RN and `TXRSP` at SN are different things."**
  Physical channel name collides, shorthand names do not. Use the shorthand (CRSP / SRSP) when talking about CHI flows.

- **"DMT disappears when `Order != 0` and `ExpCompAck = 0`."**
  Home needs a way to know the Subordinate's request won't be retried. With no `CompAck` and no `ReadReceipt`-ordered flow, DMT is simply illegal (Table B2.6). The figure still draws it; you must filter.

- **"Data is one arrow in B2.3, not one flit."**
  A `CompData` arrow represents up to four 128-bit (or 64-bit) beats on DAT. If a dependency matters, it is almost always on the *first* beat.

- **"Snoopees can refuse."**
  DCT alternatives always come with 5c/5d "failed, must use alternative" escape hatches. If you assume DCT succeeds, you are reading a best case.

- **"Retry is drawn separately for a reason."**
  Every B2.3 figure silently assumes `RetryAck` did not happen. Retry is only shown in B2.3.8 so the other figures stay readable. Real transactions can take a retry prefix before entering any of the B2.3.x flows, except for `PCrdReturn` and `PrefetchTgt`.

- **"Home-initiated transactions are independent transactions."**
  When Home fires off a snoop or a downstream `ReadNoSnp`, that is a *new* transaction with its own TxnID, not a continuation of the figure you are looking at. B2.3.9 documents them separately. In B2.3.1.x the outer request's arrows only show what crosses into the original transaction's flow.

---

## 10. If you remember one thing

> A B2.3 figure is a **superposition of alternatives**, not a single scenario.
> Fix the opcode, fix `Order` and `ExpCompAck`, pick exactly one alternative, classify it as plain / DMT / DCT / DWT, and trace its arrows in channel order (REQ → SNP? → DAT → CRSP/SRSP → CompAck?).
> Every arrow that does not belong to your chosen alternative is noise.

Once that reflex is in place, opening *B2.3.1.2 Non-allocating Read* becomes: "which of the six alternatives am I modelling today?" — and every other B2.3.x figure yields to the same question.
