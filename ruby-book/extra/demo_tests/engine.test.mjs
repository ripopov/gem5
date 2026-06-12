// Unit tests for the simulation engine embedded in NewSimpleNetwork_demo.html.
// Extracts the /*ENGINE-BEGIN*/../*ENGINE-END*/ block (DOM-free) and verifies
// the credit semantics against CreditedLinkBuffer.md / NewSimpleNetwork.md.
import { readFileSync } from 'fs';
import assert from 'assert';

const html = readFileSync(
  new URL('../NewSimpleNetwork_demo.html', import.meta.url), 'utf8');
const m = html.match(/\/\*ENGINE-BEGIN\*\/([\s\S]*?)\/\*ENGINE-END\*\//);
assert(m, 'engine block not found in HTML');
(0, eval)(m[1]);
const E = globalThis.Engine;
assert(E && E.buildScenario, 'Engine not exported');

let passed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log('  ok  ' + name); }
  catch (e) {
    console.error('FAIL  ' + name + '\n      ' + e.message);
    process.exitCode = 1;
  }
}
function run(sim, n) { for (let i = 0; i < n; i++) sim.step(); }

// ---------------------------------------------------------------- knee
test('throughput knee: rate == min(1, C/RTT_min) for C=1..6 (RTT_min=4)', () => {
  for (let C = 1; C <= 6; C++) {
    const sim = E.buildScenario('knee', { credits: C, tLink: 2, tCR: 1 });
    run(sim, 120);
    const d0 = sim.handles.hn.delivered;
    run(sim, 500);
    const rate = (sim.handles.hn.delivered - d0) / 500;
    const want = Math.min(1, C / 4);
    assert(Math.abs(rate - want) < 0.02,
      `C=${C}: measured ${rate.toFixed(3)}, want ${want.toFixed(3)}`);
  }
});

test('knee respects t_link and t_creditReturn knobs', () => {
  // RTT_min = tLink + 1 + tCR = 3 + 1 + 2 = 6, C=3 -> 0.5
  const sim = E.buildScenario('knee', { credits: 3, tLink: 3, tCR: 2 });
  run(sim, 120);
  const d0 = sim.handles.hn.delivered;
  run(sim, 600);
  const rate = (sim.handles.hn.delivered - d0) / 600;
  assert(Math.abs(rate - 0.5) < 0.02, `measured ${rate.toFixed(3)}, want 0.500`);
});

// ------------------------------------------------- conservation / drain
test('credit conservation + no message loss after drain (lifecycle)', () => {
  const sim = E.buildScenario('lifecycle',
    { credited: true, credits: 3, tLink: 2, tCR: 1, staging: 2, gap: 3 });
  // inject for 90 cycles, then stop traffic and drain
  run(sim, 90);
  sim.handles.rn.gen = () => null;
  run(sim, 200);
  const { rn, hn, links } = sim.handles;
  assert(rn.injected > 20, 'expected some traffic, got ' + rn.injected);
  assert.equal(hn.delivered, rn.injected, 'delivered != injected');
  assert.equal(sim.inTransit(), 0, 'messages stuck in transit');
  for (const l of links) {
    assert.equal(l.credits, l.maxCredits, `${l.id} credits not restored`);
    assert.equal(l.creditReturns, l.grants, `${l.id} returns != grants`);
  }
});

test('per-cycle credit invariant holds under random-ish stop/go', () => {
  const sim = E.buildScenario('backpressure', { scripted: true, credits: 3 });
  run(sim, 400); // checkInvariant() throws inside step() on violation
  assert(sim.handles.hn.delivered > 0);
});

// ---------------------------------------------------- disabled mode
test('credits==0 (disabled): no stalls, no credit events, all delivered', () => {
  const sim = E.buildScenario('lifecycle',
    { credited: false, credits: 3, tLink: 2, tCR: 1, staging: 2, gap: 2 });
  run(sim, 150);
  sim.handles.rn.gen = () => null;
  run(sim, 100);
  const { rn, hn, links } = sim.handles;
  assert.equal(rn.creditStallCycles, 0, 'producer stalled in disabled mode');
  for (const l of links) {
    assert.equal(l.creditReturns, 0, 'credit event in disabled mode');
    assert.equal(l.crInFlight.length, 0);
  }
  assert.equal(hn.delivered, rn.injected);
});

// ---------------------------------------------------- latency model
test('end-to-end latency matches pipeline arithmetic (single message)', () => {
  const sim = E.buildScenario('lifecycle',
    { credited: true, credits: 3, tLink: 2, tCR: 1, staging: 2, gap: 50 });
  // first msg injected at cycle 1; 3 links x tLink=2, 2 XPs x (dwell1+pipe1),
  // sink dwell 1  =>  delivered at 1 + 3*2 + 2*2 + 1 = 12 -> latency 11
  run(sim, 30);
  const { hn } = sim.handles;
  assert(hn.delivered >= 1, 'first message not delivered');
  assert.equal(hn.avgLatency, 11,
    `latency ${hn.avgLatency}, want 11 (=3*t_link + 2*(dwell+pipe) + sink dwell)`);
});

// ---------------------------------------------------- backpressure
test('backpressure walks upstream hop-by-hop, then recovers', () => {
  const sim = E.buildScenario('backpressure', { scripted: true, credits: 6 });
  const { links, hn, rn } = sim.handles; // [L0,L1,L2,L3]
  const zeroAt = {};
  for (let i = 0; i < 130; i++) {
    sim.step();
    for (const l of links)
      if (l.credits === 0 && !(l.id in zeroAt)) zeroAt[l.id] = sim.cycle;
  }
  assert(hn.forceStall === false && sim.cycle === 130, 'script mismatch');
  for (const id of ['L0', 'L1', 'L2', 'L3'])
    assert(id in zeroAt, id + ' never starved');
  // credits >= RTT_min, so starvation comes only from the cycle-40 stall,
  // and must reach the tail link first, walking upstream with per-hop delay
  for (const id of ['L0', 'L1', 'L2', 'L3'])
    assert(zeroAt[id] > 40, id + ' starved before the stall: ' + zeroAt[id]);
  assert(zeroAt.L3 < zeroAt.L2 && zeroAt.L2 < zeroAt.L1 && zeroAt.L1 < zeroAt.L0,
    'stall did not walk tail->head: ' + JSON.stringify(zeroAt));
  // recovery: stop traffic, let everything drain
  rn.gen = () => null;
  run(sim, 300);
  for (const l of links)
    assert.equal(l.credits, l.maxCredits, l.id + ' did not recover');
  assert.equal(sim.totalDelivered(), sim.totalInjected(), 'loss after recovery');
});

// ---------------------------------------------------- HoL elimination
test('HoL elimination lets flow B drain past blocked flow A', () => {
  // East sink freezes at cycle 30; compare B deliveries AFTER the freeze
  const mk = hol => {
    const sim = E.buildScenario('hol', { hol, credits: 8, eGap: 8 });
    run(sim, 30);
    const b0 = sim.handles.sinkS.delivered;
    run(sim, 150);
    return { sim, dB: sim.handles.sinkS.delivered - b0 };
  };
  const on = mk(true), off = mk(false);
  assert(on.sim.handles.x0.holSkips > 5, 'no HoL skips with hol=true');
  assert.equal(off.sim.handles.x0.holSkips, 0, 'HoL skips with head-only');
  assert(on.dB > 3 * Math.max(1, off.dB) && on.dB - off.dB > 8,
    `post-freeze B(hol)=${on.dB} not >> B(head-only)=${off.dB}`);
  assert(off.sim.handles.x0.headBlockedCycles > 30,
    'head-only did not register head-blocked cycles');
});

// ---------------------------------------------------- staging decoupling
test('deeper staging -> more B throughput under head-only + bursty East', () => {
  const bAt = depth => {
    const sim = E.buildScenario('staging', { depth, hol: false });
    run(sim, 400);
    return sim.handles.sinkS.delivered;
  };
  const b1 = bAt(1), b2 = bAt(2), b3 = bAt(3);
  assert(b3 > b1, `B(depth3)=${b3} not > B(depth1)=${b1}`);
  assert(b2 >= b1, `B(depth2)=${b2} < B(depth1)=${b1}`);
});

// ---------------------------------------------------- structural checks
test('all scenarios build, run 300 cycles, and stay invariant-clean', () => {
  for (const sc of E.SCENARIOS) {
    const sim = E.buildScenario(sc.id, {});
    run(sim, 300);
    assert(sim.cycle === 300, sc.id);
    assert(sim.totalInjected() >=
           sim.totalDelivered(), sc.id + ': delivered > injected');
    for (const n of sim.layout.nodes)
      assert(n.ref && n.w > 0 && n.h > 0, sc.id + ': bad layout node');
    for (const l of sim.links)
      assert(l.geom, sc.id + ': link ' + l.id + ' has no geometry');
    assert(sim.statsFn().length > 0, sc.id + ': no stats');
  }
});

test('XP grants at most 1 msg per input and per output per cycle', () => {
  const sim = E.buildScenario('hol', { hol: true, credits: 8, eGap: 8 });
  let prev = 0;
  for (let i = 0; i < 200; i++) {
    sim.step();
    const g = sim.handles.x0.grantsTotal;
    assert(g - prev <= 2, 'more than 2 grants (2 outputs) in one cycle');
    prev = g;
  }
});

console.log(passed + ' test group(s) passed' +
            (process.exitCode ? ', SOME FAILED' : ''));
