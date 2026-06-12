// Unit tests for the simulation engine embedded in NewSimpleNetwork_demo.html
// (single-XP, 4-peer CHI-channel demo). Extracts the DOM-free
// /*ENGINE-BEGIN*/../*ENGINE-END*/ block and verifies the credit semantics
// against CreditedLinkBuffer.md / NewSimpleNetwork.md.
import { readFileSync } from 'fs';
import assert from 'assert';

const html = readFileSync(
  new URL('../NewSimpleNetwork_demo.html', import.meta.url), 'utf8');
const m = html.match(/\/\*ENGINE-BEGIN\*\/([\s\S]*?)\/\*ENGINE-END\*\//);
assert(m, 'engine block not found in HTML');
(0, eval)(m[1]);
const E = globalThis.Engine;
assert(E && E.buildSim && E.defaultCfg, 'Engine not exported');

let passed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log('  ok  ' + name); }
  catch (e) {
    console.error('FAIL  ' + name + '\n      ' + e.message);
    process.exitCode = 1;
  }
}
function run(sim, n) { for (let i = 0; i < n; i++) sim.step(); }
// only W transmits, to E only
function soloCfg(mut) {
  const c = E.defaultCfg();
  for (const P of E.PEERS) c.peers[P].txGap = 0;
  c.peers.W.txGap = 1;
  c.peers.W.dests = { N: false, E: true, S: false, W: false };
  if (mut) mut(c);
  return c;
}

// ---------------------------------------------------------------- knee
test('throughput knee: rate == min(1, C/RTT_min) for C=1..6 (RTT_min=4)', () => {
  for (let C = 1; C <= 6; C++) {
    const cfg = soloCfg(c => {
      c.peers.W.txLink.credits = C;
      c.peers.E.rxLink.credits = 8;   // not the bottleneck
    });
    const sim = E.buildSim(cfg);
    run(sim, 120);
    const d0 = sim.handles.peers.E.sink.delivered;
    run(sim, 500);
    const rate = (sim.handles.peers.E.sink.delivered - d0) / 500;
    const want = Math.min(1, C / 4);
    assert(Math.abs(rate - want) < 0.02,
      `C=${C}: measured ${rate.toFixed(3)}, want ${want.toFixed(3)}`);
  }
});

test('knee respects t_link and t_creditReturn knobs', () => {
  // RTT_min = 3 + 1 + 2 = 6, C=3 -> 0.5
  const cfg = soloCfg(c => {
    c.peers.W.txLink = { credits: 3, tLink: 3, tCR: 2 };
    c.peers.E.rxLink.credits = 8;
  });
  const sim = E.buildSim(cfg);
  run(sim, 120);
  const d0 = sim.handles.peers.E.sink.delivered;
  run(sim, 600);
  const rate = (sim.handles.peers.E.sink.delivered - d0) / 600;
  assert(Math.abs(rate - 0.5) < 0.02, `measured ${rate.toFixed(3)}, want 0.500`);
});

// ---------------------------------------------------- latency arithmetic
test('end-to-end latency matches pipeline arithmetic (single message)', () => {
  // inject@s, arrive XP s+2, grant s+3, sendable s+4, send s+4,
  // arrive E s+6, consume s+7 -> latency 7
  const cfg = soloCfg(c => { c.peers.W.txGap = 100; });
  const sim = E.buildSim(cfg);
  run(sim, 30);
  const sink = sim.handles.peers.E.sink;
  assert(sink.delivered >= 1, 'message not delivered');
  assert.equal(sink.avgLatency, 7,
    `latency ${sink.avgLatency}, want 7 = t_link+dwell+t_pipe+t_link+sink dwell`);
});

// ------------------------------------------------- conservation / drain
test('credit conservation + no message loss after drain (all peers)', () => {
  const cfg = E.defaultCfg();
  const sim = E.buildSim(cfg);
  run(sim, 150);
  for (const P of E.PEERS) cfg.peers[P].txGap = 0;  // live stop
  run(sim, 200);
  assert(sim.totalInjected() > 100, 'too little traffic');
  assert.equal(sim.totalDelivered(), sim.totalInjected(), 'loss');
  assert.equal(sim.inTransit(), 0, 'messages stuck in transit');
  for (const l of sim.links) {
    assert.equal(l.credits, l.maxCredits, `${l.id} credits not restored`);
    assert.equal(l.creditReturns, l.grants, `${l.id} returns != grants`);
  }
});

// ---------------------------------------------------- disabled mode
test('credited=false: no stalls, no credit events, all delivered', () => {
  const cfg = E.defaultCfg();
  cfg.credited = false;
  for (const P of E.PEERS) cfg.peers[P].txGap = 1;   // saturate
  const sim = E.buildSim(cfg);
  run(sim, 200);
  for (const P of E.PEERS) cfg.peers[P].txGap = 0;
  run(sim, 150);
  for (const P of E.PEERS) {
    assert.equal(sim.handles.peers[P].producer.creditStallCycles, 0,
      P + ': producer stalled in disabled mode');
  }
  for (const l of sim.links) {
    assert.equal(l.creditReturns, 0);
    assert.equal(l.crInFlight.length, 0);
  }
  assert.equal(sim.totalDelivered(), sim.totalInjected());
});

// ---------------------------------------------------- backpressure
test('frozen RX starves its link, backs up senders, then recovers', () => {
  const cfg = E.defaultCfg();
  for (const P of E.PEERS) cfg.peers[P].txGap = 2;
  const sim = E.buildSim(cfg);
  run(sim, 30);
  cfg.peers.E.freeze = true;       // live freeze
  run(sim, 250);
  const { peers } = sim.handles;
  assert.equal(peers.E.rxLink.credits, 0, 'XP→E link not starved');
  assert(peers.E.rxLink.clb.length > 0, 'no msgs parked in frozen RX CLB');
  const stalls = E.PEERS.map(P => peers[P].producer.creditStallCycles)
                        .reduce((a, b) => a + b, 0);
  assert(stalls > 0, 'backpressure never reached the producers');
  // recover
  cfg.peers.E.freeze = false;
  for (const P of E.PEERS) cfg.peers[P].txGap = 0;
  run(sim, 400);
  for (const l of sim.links)
    assert.equal(l.credits, l.maxCredits, l.id + ' did not recover');
  assert.equal(sim.totalDelivered(), sim.totalInjected(), 'loss after recovery');
});

test('scripted periodic RX stall throttles that peer only', () => {
  const cfg = E.defaultCfg();
  cfg.peers.S.stallLen = 20;       // S.RX stalled 20 of every 40 cycles
  cfg.peers.S.stallPeriod = 40;
  const sim = E.buildSim(cfg);
  run(sim, 600);
  const { peers } = sim.handles;
  // S receives ~ what others receive but with much higher latency / or less;
  // at default load S is throttled to its duty cycle only if demand > 50%...
  // robust check: S latency must exceed N latency, and nothing deadlocks
  assert(peers.S.sink.avgLatency > peers.N.sink.avgLatency + 1,
    `S lat ${peers.S.sink.avgLatency.toFixed(1)} not > ` +
    `N lat ${peers.N.sink.avgLatency.toFixed(1)}`);
  assert(peers.N.sink.delivered > 100, 'other peers throttled too');
});

// ---------------------------------------------------- HoL elimination
test('HoL elimination: lower non-E latency, more deliveries vs head-only', () => {
  // Congested East (slow RX) + saturating injection: E-bound msgs sit at
  // input heads. HoL elimination serves non-E traffic past them.
  const mk = hol => {
    const cfg = E.defaultCfg();
    cfg.xp.hol = hol;
    cfg.xp.stagingDepth = 1;
    cfg.peers.E.rxGap = 4;
    for (const P of E.PEERS) {
      cfg.peers[P].txGap = 1;
      cfg.peers[P].txLink.credits = 8;
    }
    const sim = E.buildSim(cfg);
    run(sim, 600);
    const lat = ['N', 'S', 'W']
      .map(P => sim.handles.peers[P].sink.avgLatency).reduce((a, b) => a + b) / 3;
    const del = ['N', 'S', 'W']
      .map(P => sim.handles.peers[P].sink.delivered).reduce((a, b) => a + b);
    return { sim, lat, del };
  };
  const on = mk(true), off = mk(false);
  assert(on.sim.handles.xp.holSkips > 100, 'no HoL skips with hol=true');
  assert.equal(off.sim.handles.xp.holSkips, 0, 'HoL skips with head-only');
  assert(off.lat > 1.8 * on.lat,
    `non-E latency: hol=${on.lat.toFixed(1)}, head-only=${off.lat.toFixed(1)}`);
  assert(on.del > off.del + 50,
    `non-E deliveries: hol=${on.del}, head-only=${off.del}`);
  assert(off.sim.handles.xp.headBlockedCycles > 2 * on.sim.handles.xp.headBlockedCycles,
    'head-blocked cycles not dominated by head-only mode');
});

// ---------------------------------------------------- staging decoupling
test('deeper staging -> more non-E throughput under head-only + frozen E', () => {
  const at = depth => {
    const cfg = E.defaultCfg();
    cfg.xp.hol = false;
    cfg.xp.stagingDepth = depth;
    cfg.peers.E.freeze = true;
    for (const P of E.PEERS) cfg.peers[P].txGap = 2;
    const sim = E.buildSim(cfg);
    run(sim, 300);
    return ['N', 'S', 'W']
      .map(P => sim.handles.peers[P].sink.delivered).reduce((a, b) => a + b, 0);
  };
  const d1 = at(1), d3 = at(3);
  assert(d3 > d1, `non-E(depth3)=${d3} not > non-E(depth1)=${d1}`);
});

// ---------------------------------------------------- live parameters
test('txGap / rxGap / hol / stagingDepth apply live (no rebuild)', () => {
  const cfg = E.defaultCfg();
  const sim = E.buildSim(cfg);
  run(sim, 100);
  const inj1 = sim.totalInjected();           // gap 3 -> ~33/peer
  for (const P of E.PEERS) cfg.peers[P].txGap = 1;
  run(sim, 100);
  const inj2 = sim.totalInjected() - inj1;
  assert(inj2 > inj1 * 2, `live rate change ignored: ${inj1} then ${inj2}`);
  // staging depth live shrink: occupancy must obey the new bound
  cfg.xp.stagingDepth = 3;
  run(sim, 50);
  cfg.xp.stagingDepth = 1;
  run(sim, 100);
  for (const o of sim.handles.xp.outputs)
    assert(o.staging.length <= 1, 'staging exceeds live-shrunk depth');
  // hol live toggle: no crash, holSkips can still increase
  cfg.xp.hol = false;
  run(sim, 50);
  cfg.xp.hol = true;
  run(sim, 50);
  assert.equal(sim.cycle, 450);
});

// ---------------------------------------------------- fairness & sanity
test('balanced default config delivers roughly evenly to all peers', () => {
  const sim = E.buildSim(E.defaultCfg());
  run(sim, 600);
  const d = E.PEERS.map(P => sim.handles.peers[P].sink.delivered);
  const mean = d.reduce((a, b) => a + b, 0) / 4;
  assert(mean > 50, 'too little delivered: ' + d);
  for (const v of d)
    assert(Math.abs(v - mean) / mean < 0.25, 'unbalanced: ' + d);
});

test('chaotic config stays invariant-clean for 1000 cycles', () => {
  const cfg = E.defaultCfg();
  cfg.peers.N.stallLen = 7; cfg.peers.N.stallPeriod = 23;
  cfg.peers.S.stallLen = 13; cfg.peers.S.stallPeriod = 31;
  cfg.peers.W.rxGap = 3;
  cfg.peers.E.txLink.credits = 2;
  cfg.peers.W.rxLink.credits = 1;
  for (const P of E.PEERS) cfg.peers[P].txGap = 1;
  const sim = E.buildSim(cfg);
  for (let i = 0; i < 1000; i++) {
    sim.step();   // checkInvariant() throws on any violation
    if (i % 50 === 0) cfg.peers.E.freeze = !cfg.peers.E.freeze;
    if (i % 70 === 0) cfg.xp.stagingDepth = 1 + (i / 70) % 3;
    if (i % 90 === 0) cfg.xp.hol = !cfg.xp.hol;
  }
  assert(sim.totalDelivered() > 200, 'network seized up');
  assert(sim.totalInjected() >= sim.totalDelivered());
});

test('XP grants at most 1 msg per input and 4 per cycle (4 outputs)', () => {
  const cfg = E.defaultCfg();
  for (const P of E.PEERS) cfg.peers[P].txGap = 1;
  const sim = E.buildSim(cfg);
  let prev = 0;
  for (let i = 0; i < 300; i++) {
    sim.step();
    const g = sim.handles.xp.grantsTotal;
    assert(g - prev <= 4, 'more than 4 grants in one cycle');
    prev = g;
  }
});

test('destination masks are honoured', () => {
  const cfg = E.defaultCfg();
  for (const P of E.PEERS) cfg.peers[P].dests = { N: true, E: false, S: true, W: true };
  const sim = E.buildSim(cfg);
  run(sim, 300);
  assert.equal(sim.handles.peers.E.sink.delivered, 0, 'E received traffic');
  assert(sim.handles.peers.N.sink.delivered > 30, 'N starved');
});

test('layout: all 8 links have geometry, 5 nodes placed', () => {
  const sim = E.buildSim(E.defaultCfg());
  assert.equal(sim.links.length, 8);
  assert.equal(sim.layout.nodes.length, 5);
  for (const l of sim.links) assert(l.geom, l.id + ' missing geom');
  for (const n of sim.layout.nodes) assert(n.w > 0 && n.h > 0 && n.ref);
  // latency-tag anchors: every link gets one, on-canvas, valid alignment
  for (const l of sim.links) {
    const a = l.geom.lbl;
    assert(a, l.id + ' missing latency-label anchor');
    assert(a.x > 0 && a.x < 900 && a.y > 0 && a.y < 640,
           l.id + ' label anchor off-canvas');
    assert(['left', 'right', 'center'].includes(a.align),
           l.id + ' bad label alignment');
  }
});

console.log(passed + ' test group(s) passed' +
            (process.exitCode ? ', SOME FAILED' : ''));
