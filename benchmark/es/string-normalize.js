'use strict';

const common = require('../common.js');
const assert = require('assert');

// Roughly 10k UTF-16 code units of representative text per language.
const samples = {
  latin: 'Le cœur déçu mais l\'âme plutôt naïve, Louÿs rêva de crapaüter ' +
         'en canoë au delà des îles, près du mälström où brûlent les novæ. ',
  hangul: '한국어 문서를 처리하는 속도는 정규화 방식에 따라 크게 달라집니다. ',
  arabic: 'إن الحياة مليئة بالتجارب التي تجعلنا أقوى وأكثر حكمة مع مرور الوقت. ',
  hindi: 'यह एक परीक्षण वाक्य है जिसमें संयुक्त अक्षर और मात्राएँ शामिल हैं। ',
  japanese: '日本語のテキストを正規化する処理の速度を測定するための文章です。 ',
};

for (const key of Object.keys(samples)) {
  let s = samples[key];
  while (s.length < 10000) s += s;
  samples[key] = s.slice(0, 10000);
}

const bench = common.createBenchmark(main, {
  form: ['NFC', 'NFD', 'NFKC', 'NFKD'],
  lang: Object.keys(samples),
  // 'normalized': the input is already in the requested form, the common case;
  // 'unnormalized': the input is in a different form, so work has to happen.
  state: ['normalized', 'unnormalized'],
  n: [1e4],
});

function main({ form, lang, state, n }) {
  const base = samples[lang];
  const input = state === 'normalized' ?
    base.normalize(form) :
    base.normalize(form === 'NFC' || form === 'NFKC' ? 'NFD' : 'NFC');
  // Force a flat string.
  const str = input.slice(0);

  let length = 0;
  bench.start();
  for (let i = 0; i < n; i++) {
    length += str.normalize(form).length;
  }
  bench.end(n);
  assert.ok(length > 0);
}
