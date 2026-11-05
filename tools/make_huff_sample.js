// Переупаковывает MOBI (PalmDOC) в HUFF/CDIC — сжатие, которое Calibre читает, но писать не умеет.
// Нужен образец, проверяемый чужим чтением (Calibre) и нашим. Код без кода Sharkonv, по описанию
// формата MobileRead Wiki: таблица HUFF на 256 кодов длиной 8 бит и один словарь CDIC, в котором
// есть обычные фразы и одна сжатая (флаг 0) — так проверяется и рекурсивная распаковка.
// Запуск: node make_huff_sample.js вход.mobi выход.mobi
const fs = require('fs');
const [, , inPath, outPath] = process.argv;
const b = fs.readFileSync(inPath);

const n = b.readUInt16BE(76);
const offs = [];
for (let i = 0; i < n; i++) offs.push(b.readUInt32BE(78 + 8 * i));
offs.push(b.length);
const records = [];
for (let i = 0; i < n; i++) records.push(Buffer.from(b.subarray(offs[i], offs[i + 1])));

const r0 = records[0], M = 16;
const comp = r0.readUInt16BE(0), textRecs = r0.readUInt16BE(8), recSize = r0.readUInt16BE(10), textLen = r0.readUInt32BE(4);
const extra = r0.readUInt16BE(M + 0xE2);
if (comp !== 2) throw new Error('need PalmDOC input, got ' + comp);

function palmdoc(d) {
  const out = [];
  for (let i = 0; i < d.length;) {
    const c = d[i++];
    if (c === 0) out.push(0);
    else if (c <= 8) { for (let k = 0; k < c; k++) out.push(d[i++]); }
    else if (c < 0x80) out.push(c);
    else if (c < 0xC0) {
      const v = ((c << 8) | d[i++]) & 0x3FFF, dist = v >> 3, len = (v & 7) + 3;
      for (let k = 0; k < len; k++) out.push(out[out.length - dist]);
    } else { out.push(32, c ^ 0x80); }
  }
  return out;
}
function trailing(rec) {
  let e = rec.length;
  for (let bit = 15; bit > 0; bit--) {
    if (extra & (1 << bit)) {
      let v = 0, k = 0;
      for (let p = e - 1; k < 4 && p >= 0; p--, k++) { v |= (rec[p] & 0x7F) << (7 * k); if (rec[p] & 0x80) break; }
      e -= v;
    }
  }
  if (extra & 1) e -= (rec[e - 1] & 3) + 1;
  return e;
}
let text = [];
for (let i = 1; i <= textRecs; i++) text = text.concat(palmdoc(records[i].subarray(0, trailing(records[i]))));
const plain = Buffer.from(text);
if (plain.length !== textLen) throw new Error(`unpacked ${plain.length}, header says ${textLen}`);

// Фразы словаря: номер фразы r соответствует коду 255 - r (формула из распаковщика).
const used = [...new Set(plain)];
const phrases = [];           // { data, literal }
const indexOf = new Map();    // байт -> номер фразы
for (const c of used) { indexOf.set(c, phrases.length); phrases.push({ data: Buffer.from([c]), literal: true }); }
if (phrases.length > 252) throw new Error('too many distinct bytes');
const pack = idx => idx.map(i => 255 - i);
// Частая пара байт — обычная фраза; пара пар — сжатая фраза, распаковывается рекурсивно
const counts = new Map();
for (let i = 0; i + 1 < plain.length; i++) { const k = plain[i] * 256 + plain[i + 1]; counts.set(k, (counts.get(k) || 0) + 1); }
const best = [...counts.entries()].sort((a, b) => b[1] - a[1])[0][0];
const pairA = best >> 8, pairB = best & 255;
const pairIdx = phrases.length;
phrases.push({ data: Buffer.from([pairA, pairB]), literal: true });
const nestIdx = phrases.length;
phrases.push({ data: Buffer.from(pack([pairIdx, pairIdx])), literal: false });
while (phrases.length < 256) phrases.push({ data: Buffer.from([0]), literal: true });

function encode(d) {
  const out = [];
  for (let i = 0; i < d.length;) {
    if (i + 3 < d.length && d[i] === pairA && d[i + 1] === pairB && d[i + 2] === pairA && d[i + 3] === pairB) { out.push(255 - nestIdx); i += 4; }
    else if (i + 1 < d.length && d[i] === pairA && d[i + 1] === pairB) { out.push(255 - pairIdx); i += 2; }
    else { out.push(255 - indexOf.get(d[i])); i++; }
  }
  return Buffer.from(out);
}

const huff = Buffer.alloc(0x18 + 1024 + 256);
huff.write('HUFF', 0, 'latin1');
huff.writeUInt32BE(0x18, 4);
huff.writeUInt32BE(0x18, 8);
huff.writeUInt32BE(0x18 + 1024, 12);
// codelen = 8, term = 1, maxcode = 255; вторая таблица для кодов до 8 бит не используется
for (let i = 0; i < 256; i++) huff.writeUInt32BE((255 << 8) | 0x80 | 8, 0x18 + 4 * i);

const head = Buffer.alloc(16 + 2 * 256);
head.write('CDIC', 0, 'latin1');
head.writeUInt32BE(16, 4);
head.writeUInt32BE(256, 8);
head.writeUInt32BE(8, 12);
const bodies = [];
let pos = 2 * 256;
phrases.forEach((p, i) => {
  head.writeUInt16BE(pos, 16 + 2 * i);
  const e = Buffer.alloc(2 + p.data.length);
  e.writeUInt16BE(p.data.length | (p.literal ? 0x8000 : 0), 0);
  p.data.copy(e, 2);
  bodies.push(e);
  pos += e.length;
});
const cdic = Buffer.concat([head, ...bodies]);

// Текст режем заново по recSize; записей столько же, если длина текста не менялась
const out = [Buffer.from(r0)];
for (let i = 0; i < textRecs; i++) out.push(encode(plain.subarray(i * recSize, (i + 1) * recSize)));
for (let i = textRecs + 1; i < n; i++) out.push(records[i]);
const huffAt = out.length;
out.push(huff, cdic);

out[0].writeUInt16BE(17480, 0);
out[0].writeUInt32BE(0, M + 0xE0);      // без служебных байтов в конце записей
out[0].writeUInt32BE(huffAt, M + 0x60);
out[0].writeUInt32BE(2, M + 0x64);

const count = out.length, hdr = 78 + 8 * count + 2;
const file = Buffer.alloc(hdr);
b.copy(file, 0, 0, 78);
file.writeUInt16BE(count, 76);
let at = hdr;
out.forEach((r, i) => { file.writeUInt32BE(at, 78 + 8 * i); file.writeUInt32BE(i * 2, 82 + 8 * i); at += r.length; });
fs.writeFileSync(outPath, Buffer.concat([file, ...out]));
console.log(`OK ${outPath}: text ${plain.length} bytes, ${textRecs} records, ${count} records in all`);
