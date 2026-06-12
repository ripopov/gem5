// Browser verification for NewSimpleNetwork_demo.html: loads the file in
// headless Chrome, exercises every scenario tab, controls, params and
// actions, asserts zero console/page errors, and screenshots each scenario.
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
await page.setViewport({ width: 1340, height: 1000 });

const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => {
  if (m.type() === 'error') errors.push('console: ' + m.text());
});

await page.goto(PAGE_URL, { waitUntil: 'load' });
await sleep(300);

const getCycle = () => page.$eval('#cycleNum', el => +el.textContent.replace(/\D/g, ''));
const scenarios = await page.$$eval('#tabs .tab', els => els.map(e => e.dataset.sc));
assert.equal(scenarios.length, 5, 'expected 5 scenario tabs, got ' + scenarios.length);
console.log('scenarios:', scenarios.join(', '));

for (const sc of scenarios) {
  await page.click(`#tabs .tab[data-sc="${sc}"]`);
  await sleep(120);
  assert.equal(await getCycle(), 0, sc + ': cycle not 0 after select');

  // step controls
  await page.click('#btn-step');
  await page.click('#btn-step10');
  await sleep(120);
  assert.equal(await getCycle(), 11, sc + ': step controls broken');

  // play for a bit at high speed
  await page.$eval('#speed', el => {
    el.value = 40; el.dispatchEvent(new Event('input'));
  });
  await page.click('#btn-play');
  await sleep(1500);
  await page.click('#btn-play'); // pause
  const c = await getCycle();
  assert(c > 30, sc + `: play did not advance (cycle ${c})`);

  // scenario actions (toggle on and off)
  for (const h of await page.$$('#actions button')) { await h.click(); await sleep(60); await h.click(); }

  // tweak first range param (rebuilds sim)
  const ranges = await page.$$('#params input[type=range]');
  if (ranges.length) {
    await ranges[0].evaluate(el => {
      el.value = el.max; el.dispatchEvent(new Event('input'));
    });
    await sleep(120);
    assert.equal(await getCycle(), 0, sc + ': param change did not rebuild');
    await ranges[0].evaluate(el => {
      el.dataset && null; // no-op
    });
  }
  // run again after rebuild and screenshot mid-flight
  await page.click('#btn-play');
  await sleep(1200);
  await page.click('#btn-play');
  assert(await getCycle() > 20, sc + ': post-rebuild run failed');
  // ensure stats and chart rendered
  const statRows = await page.$$eval('#stats tr', r => r.length);
  assert(statRows >= 3, sc + ': stats table empty');
  const logLines = await page.$$eval('#log div', r => r.length);
  assert(logLines > 3, sc + ': event log empty');
  await page.screenshot({ path: `${OUT}/${sc}.png` });

  // reset
  await page.click('#btn-reset');
  await sleep(100);
  assert.equal(await getCycle(), 0, sc + ': reset broken');
}

// error banner must be hidden everywhere
const errBanner = await page.$eval('#err', el => getComputedStyle(el).display);
assert.equal(errBanner, 'none', 'engine error banner is visible');

// canvas must actually contain drawn pixels (non-uniform)
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
console.log(`all ${scenarios.length} scenarios pass in-browser; ` +
            `0 JS errors; screenshots in ${OUT}/`);
