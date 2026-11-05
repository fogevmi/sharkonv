// Собирает tests/corpus/handmade.pdb и handmade-raw.pdb побайтно, по описанию формата PalmDOC
// (MobileRead Wiki, чтение Calibre), без кода Sharkonv. Проверяет то, чего нет в файлах от Calibre:
//   * handmade.pdb: cp1251, две сжатые записи со всеми видами кодов PalmDOC (литерал, 1-8 сырых
//     байтов, повтор, «пробел + буква»), имя базы кириллицей, запись-закладка после текста;
//   * handmade-raw.pdb: cp1252 без сжатия, французский текст, запись-закладка.
// Запуск: node tools/make_pdb_sample.js
const fs = require('fs');
const path = require('path');
const corpus = path.join(__dirname, '..', 'tests', 'corpus');

function cp1251(s) {
  return Buffer.from([...s].map(ch => {
    const c = ch.codePointAt(0);
    if (c < 0x80) return c;
    if (c >= 0x410 && c <= 0x44F) return c - 0x410 + 0xC0;
    if (c === 0x401) return 0xA8;
    if (c === 0x451) return 0xB8;
    if (c === 0x2014) return 0x97;
    throw new Error('no cp1251 for ' + ch);
  }));
}
function cp1252(s) { return Buffer.from(s, 'latin1'); }

// Сжатие без поиска совпадений: вся проверка кодов задаётся явным списком кусков.
function pieces(list) {
  const out = [];
  for (const p of list) {
    if (p.lit) out.push(...p.lit);                                   // 0x09-0x7F как есть
    if (p.raw) out.push(p.raw.length, ...p.raw);                     // 1-8 сырых байтов
    if (p.rep) {                                                     // повтор: 2 байта
      const v = 0x8000 | (p.rep[0] << 3) | (p.rep[1] - 3);
      out.push(v >> 8, v & 255);
    }
    if (p.sp) out.push(p.sp.charCodeAt(0) ^ 0x80);                   // «пробел + буква»
  }
  return Buffer.from(out);
}

function palmdb(name, records) {
  const head = Buffer.alloc(78 + records.length * 8 + 2);
  Buffer.from(name).copy(head, 0, 0, 31);
  head.write('TEXtREAd', 60, 'latin1');
  head.writeUInt16BE(records.length, 76);
  let off = head.length;
  records.forEach((r, i) => {
    head.writeUInt32BE(off, 78 + i * 8);
    head.writeUInt32BE(i, 82 + i * 8);
    off += r.length;
  });
  return Buffer.concat([head, ...records]);
}

function record0(compression, textLen, count) {
  const r = Buffer.alloc(16);
  r.writeUInt16BE(compression, 0);
  r.writeUInt32BE(textLen, 4);
  r.writeUInt16BE(count, 8);
  r.writeUInt16BE(4096, 10);
  return r;
}

// закладка: имя и смещение. Если её принять за текст, в книге появится это слово.
const bookmark = Buffer.concat([Buffer.from('BOOKMARK-NOT-TEXT'.padEnd(16, '\0').slice(0, 16)), Buffer.from([0, 0, 0, 0])]);

// ------- handmade.pdb -------
const first = 'Глава 1\n\nПервый абзац книги.\n\n';
const second = 'Глава 2\n\nЭто ёлка и абзац, абзац, абзац. go go\n';
const a = cp1251('Глава 1\n\nПервый абзац книги.\n\n');
const rec1 = pieces([{ raw: [...a] .slice(0, 8) }, { raw: [...a].slice(8, 16) }, { raw: [...a].slice(16, 24) }, { raw: [...a].slice(24) }]);
const b1 = cp1251('Глава 2\n\nЭто ёлка и ');
const b2 = cp1251('абзац,');
const rec2 = pieces([
  { raw: [...b1].slice(0, 8) }, { raw: [...b1].slice(8, 16) }, { raw: [...b1].slice(16) },
  { raw: [...b2] },
  { lit: [0x20] },                         // пробел литералом
  { rep: [7, 6] },                         // «абзац,» — повтор на 7 назад, 6 байт
  { lit: [0x20] },
  { rep: [7, 5] },                         // «абзац»
  { lit: [...Buffer.from('.')] },
  { sp: 'g' }, { lit: [...Buffer.from('o')] },   // « go» кодом «пробел + буква»
  { sp: 'g' }, { lit: [...Buffer.from('o')] },
  { lit: [0x0A] },
]);
const text1 = Buffer.concat([cp1251(first), cp1251(second)]);
fs.writeFileSync(path.join(corpus, 'handmade.pdb'), palmdb(cp1251('Книга Палм'), [record0(2, text1.length, 2), rec1, rec2, bookmark]));
fs.writeFileSync(path.join(corpus, 'handmade-pdb.paras'), 'Первый абзац книги.\nЭто ёлка и абзац, абзац, абзац. go go\n');

// ------- handmade-raw.pdb -------
const raw = cp1252('Chapitre 1\n\nDéjà vu à la crème brûlée, dit-elle.\n\nFin du récit.\n');
fs.writeFileSync(path.join(corpus, 'handmade-raw.pdb'), palmdb(Buffer.from('Livre Palm'), [record0(1, raw.length, 1), raw, bookmark]));
fs.writeFileSync(path.join(corpus, 'handmade-raw-pdb.paras'), 'Chapitre 1\nDéjà vu à la crème brûlée, dit-elle.\nFin du récit.\n');
