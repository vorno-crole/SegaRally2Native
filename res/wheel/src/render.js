const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ deviceScaleFactor: 1 });
  for (const f of process.argv.slice(2)) {
    const [name, w, h] = f.split(':');
    const svg = require('fs').readFileSync(name + '.svg', 'utf8');
    await p.setViewportSize({ width: +w, height: +h });
    await p.setContent(`<html><body style="margin:0;background:transparent">${svg}</body></html>`);
    await p.screenshot({ path: name + '.png', omitBackground: true, clip: { x: 0, y: 0, width: +w, height: +h } });
  }
  await b.close();
})();
