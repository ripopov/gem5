// Browser verification for NewSimpleNetwork_demo.html (single-XP, 4-peer
// demo): loads the file in headless Chrome, exercises presets, config tabs,
// live & structural params, freeze buttons, chart selector and sim controls,
// asserts zero console/page errors, and takes screenshots.
import assert from 'assert';
import { mkdirSync } from 'fs';
import { createRequire } from 'module';
// puppeteer-core is installed globally (brew node):
const requireG = createRequire('/opt/homebrew/lib/node_modules/');
const puppeteer = requireG('puppeteer-core');

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const PAGE_URL = new URL('../NewSimpleNetwork_demo.html', import.meta.url).href;
const OUT = '/tmp/nsn_demo_shots';
mkdirSync(OUT, { recursive: true });
const sleep = ms => new Promise(r => setTimeout(r, ms));

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: 'new',
  args: ['--no-sandbox', '--force-device-scale-factor=1'],
});
const page = await browser.newPage();
await page.setViewport({ width: 1620, height: 1080 });

const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => {
  if (m.type() === 'error') errors.push('console: ' + m.text());
});

await page.goto(PAGE_URL, { waitUntil: 'load' });
await sleep(300);

const getCycle = () =>
  page.$eval('#cycleNum', el => +el.textContent.replace(/\D/g, ''));
const setSpeed = v => page.$eval('#speed', (el, val) => {
  el.value = val; el.dispatchEvent(new Event('input'));
}, v);

// ---------- basic controls ----------
assert.equal(await getCycle(), 0, 'initial cycle not 0');
await page.click('#btn-step');
await page.click('#btn-step10');
await sleep(100);
assert.equal(await getCycle(), 11, 'step controls broken');
await setSpeed(40);
await page.click('#btn-play');
await sleep(1300);
await page.click('#btn-play');
assert(await getCycle() > 40, 'play did not advance');
await page.click('#btn-reset');
await sleep(80);
assert.equal(await getCycle(), 0, 'reset broken');
console.log('controls ok');

// ---------- config tabs ----------
const tabs = await page.$$eval('#cfgTabs button', els => els.map(e => e.dataset.tab));
assert.deepEqual(tabs, ['xp', 'N', 'E', 'S', 'W'], 'tabs: ' + tabs);
for (const t of tabs) {
  await page.click(`#cfgTabs [data-tab="${t}"]`);
  await sleep(50);
  const nFields = await page.$$eval('#cfgBody .param', r => r.length);
  assert(nFields >= 3, t + ': too few fields (' + nFields + ')');
  if (t !== 'xp') {
    const nDests = await page.$$eval('#cfgBody .dests input', r => r.length);
    assert.equal(nDests, 3, t + ': dest checkboxes');
  }
}
console.log('config tabs ok');

// ---------- live param: txGap change must NOT reset the sim ----------
await page.click('#cfgTabs [data-tab="N"]');
await sleep(50);
await page.click('#btn-step10');
await sleep(150);
const cBefore = await getCycle();
assert(cBefore > 0);
await page.$eval('#cfgBody .param input[type=range]', el => {
  el.value = 1; el.dispatchEvent(new Event('input'));   // N txGap -> 1 (live)
});
await sleep(80);
assert.equal(await getCycle(), cBefore, 'live param reset the sim');

// ---------- structural param: credits change MUST reset the sim ----------
const ranges = await page.$$('#cfgBody .param input[type=range]');
// fields: txGap, rxGap, stallLen, stallPeriod, tx credits, ...
await ranges[4].evaluate(el => {
  el.value = 2; el.dispatchEvent(new Event('input'));   // N->XP credits (⟳)
});
await sleep(80);
assert.equal(await getCycle(), 0, 'structural param did not reset');
console.log('live vs structural params ok');

// ---------- freeze buttons ----------
await page.click('#btn-step10');
await page.click('#freezes [data-freeze="E"]');
await sleep(50);
let onCls = await page.$eval('#freezes [data-freeze="E"]',
                             el => el.className.includes('on'));
assert(onCls, 'freeze button not active');
await page.click('#btn-play');
await sleep(700);
await page.click('#btn-play');
const rxStalled = await page.$eval('#stats', el => el.innerHTML.includes('RX⛔'));
assert(rxStalled, 'frozen RX not flagged in stats');
await page.click('#freezes [data-freeze="E"]');
await sleep(150);
onCls = await page.$eval('#freezes [data-freeze="E"]',
                         el => el.className.includes('on'));
assert(!onCls, 'freeze button did not toggle off');
console.log('freeze buttons ok');

// ---------- chart selector ----------
const chartOpts = await page.$$eval('#chartSel option', o => o.map(x => x.value));
assert.deepEqual(chartOpts, ['rx', 'txcr', 'rxcr', 'stg'], 'chart options');
for (const o of chartOpts) {
  await page.select('#chartSel', o);
  await sleep(120);
}
console.log('chart selector ok');

// ---------- presets: each applies, runs clean, screenshot ----------
const presets = await page.$$eval('#presets button', b => b.map(x => x.dataset.preset));
assert.equal(presets.length, 5, 'expected 5 presets');
for (const p of presets) {
  await page.click(`#presets [data-preset="${p}"]`);
  await sleep(80);
  assert.equal(await getCycle(), 0, p + ': preset did not rebuild');
  await setSpeed(40);
  await page.click('#btn-play');
  await sleep(1500);
  await page.click('#btn-play');
  const c = await getCycle();
  assert(c > 40, p + `: did not run (cycle ${c})`);
  const statRows = await page.$$eval('#stats tr', r => r.length);
  assert(statRows >= 8, p + ': stats table incomplete');
  const logLines = await page.$$eval('#log div', r => r.length);
  assert(logLines > 3, p + ': event log empty');
  await page.screenshot({ path: `${OUT}/${p}.png` });
}
console.log('presets ok');

// ---------- knee preset numeric sanity (read from stats) ----------
await page.click('#presets [data-preset="knee"]');
await setSpeed(40);
await page.click('#btn-play');
await sleep(4000);
await page.click('#btn-play');
const eRecv = await page.$eval('#stats', el => {
  const row = [...el.querySelectorAll('tr')].find(r => r.textContent.includes('East'));
  return +row.children[2].textContent;
});
const cyc = await getCycle();
const rate = eRecv / cyc;
assert(rate > 0.15 && rate < 0.32,
  `knee preset rate ${rate.toFixed(3)} not ~0.25 (C=1/RTT=4)`);
console.log(`knee preset measured ${rate.toFixed(3)} ≈ 0.25 ok`);

// ---------- error banner hidden, canvas non-blank ----------
const errBanner = await page.$eval('#err', el => getComputedStyle(el).display);
assert.equal(errBanner, 'none', 'engine error banner visible');
const px = await page.$eval('#view', cv => {
  const x = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data;
  const s = new Set();
  for (let i = 0; i < x.length; i += 4001) s.add(`${x[i]},${x[i+1]},${x[i+2]}`);
  return s.size;
});
assert(px > 3, 'main canvas appears blank');

await browser.close();
if (errors.length) {
  console.error('JS errors:\n' + errors.join('\n'));
  process.exit(1);
}
console.log(`browser verification passed; 0 JS errors; screenshots in ${OUT}/`);
