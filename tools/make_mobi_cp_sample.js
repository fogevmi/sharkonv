// Собирает tests/corpus/handmade-cp1251.mobi и handmade-cp1252.mobi побайтно, без кода Sharkonv.
// В заголовке обоих указана cp1252, как у старых русских MOBI. Текст, название и автор записаны:
//   * в первом — в cp1251 (много HTML-тегов: их латиница не должна перевесить кириллицу);
//   * во втором — в настоящей cp1252 (французская книга не должна стать кириллицей).
// Запуск: node tools/make_mobi_cp_sample.js
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
    throw new Error('no cp1251 for ' + ch);
  }));
}
const cp1252 = s => Buffer.from(s, 'latin1');

function mobi(file, enc, title, author, paras) {
  // каждое слово в своём теге: латиницы в разметке больше, чем кириллицы в тексте
  const tagged = p => p.split(' ').map(w => `<span class="word-with-long-class-name">${w}</span>`).join(' ');
  const html = `<html><head><title>x</title></head><body><h1>${paras[0]}</h1>` +
    paras.slice(1).map(p => `<p>${tagged(p)}</p>`).join('') + '</body></html>';
  const text = enc(html);
  const name = enc(title);
  const auth = enc(author);

  const exthRec = Buffer.alloc(8 + auth.length);
  exthRec.writeUInt32BE(100, 0);
  exthRec.writeUInt32BE(exthRec.length, 4);
  auth.copy(exthRec, 8);
  const exth = Buffer.alloc(12);
  exth.write('EXTH', 0, 'latin1');
  exth.writeUInt32BE(12 + exthRec.length, 4);
  exth.writeUInt32BE(1, 8);

  const M = 16, hlen = 232;
  const r0head = Buffer.alloc(M + hlen);
  r0head.writeUInt16BE(1, 0);                // без сжатия
  r0head.writeUInt32BE(text.length, 4);
  r0head.writeUInt16BE(1, 8);
  r0head.writeUInt16BE(4096, 10);
  r0head.write('MOBI', M, 'latin1');
  r0head.writeUInt32BE(hlen, M + 4);
  r0head.writeUInt32BE(1252, M + 12);        // кодировка из заголовка
  r0head.writeUInt32BE(6, M + 20);           // версия
  r0head.writeUInt32BE(M + hlen + exth.length + exthRec.length, M + 0x44);
  r0head.writeUInt32BE(name.length, M + 0x48);
  r0head.writeUInt32BE(0x40, M + 0x70);      // есть EXTH
  const r0 = Buffer.concat([r0head, exth, exthRec, name]);

  const head = Buffer.alloc(78 + 2 * 8 + 2);
  head.write(path.basename(file, '.mobi').slice(0, 31), 0, 'latin1');
  head.write('BOOKMOBI', 60, 'latin1');
  head.writeUInt16BE(2, 76);
  head.writeUInt32BE(head.length, 78);
  head.writeUInt32BE(head.length + r0.length, 86);
  fs.writeFileSync(path.join(corpus, file), Buffer.concat([head, r0, text]));
  fs.writeFileSync(path.join(corpus, file + '.paras'), paras.slice(1).join('\n') + '\n');
}

mobi('handmade-cp1251.mobi', cp1251, 'Русская книга', 'Лев Толстой', [
  'Глава первая',
  'Всё смешалось в доме, и ёлка стояла в углу.',
  'Второй абзац русской книги.',
]);
mobi('handmade-cp1252.mobi', cp1252, 'Livre français', 'Émile Zola', [
  'Chapitre premier',
  'Déjà vu à la crème brûlée, dit-elle.',
  'Fin du récit.',
]);
