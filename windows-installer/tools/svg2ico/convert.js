// Renders an SVG to a multi-resolution Windows .ico.
// Used to turn windows-installer/favicon.svg into a shortcut icon
// (Inno Setup IconFilename needs .ico, not .svg).
const fs = require('fs');
const path = require('path');
const sharp = require('sharp');
const _p2i = require('png-to-ico');
const pngToIco = typeof _p2i === 'function' ? _p2i : _p2i.default;

const svg = path.resolve(__dirname, '../../favicon.svg');
const out = path.resolve(__dirname, '../../favicon.ico');
const sizes = [16, 24, 32, 48, 64, 128, 256];

(async () => {
  const src = fs.readFileSync(svg);
  const pngs = [];
  for (const s of sizes) {
    pngs.push(await sharp(src, { density: 384 }).resize(s, s, { fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 } }).png().toBuffer());
  }
  const ico = await pngToIco(pngs);
  fs.writeFileSync(out, ico);
  console.log('wrote ' + out + ' (' + ico.length + ' bytes, sizes ' + sizes.join('/') + ')');
})().catch(e => { console.error(e); process.exit(1); });
