// Records what the JavaScript logic in js/dejavuamp-core.js (the reference) does, so the C++ port
// can be held to the same answers. Writes core.golden next to this file: node
// tests/parity/gen_core_golden.js One line of JSON each: {"kind":K, ...}. Numbers that JSON cannot
// carry (NaN, Infinity) are written as the strings "NaN", "Infinity", "-Infinity".
//  fmt : {"fn":name,"in":x,"out":string|number} the small formatters and converters, one value at a
// time
//   time     : {"in":seconds,"remaining":bool,"out":string}
//   ticker   : {"title":..,"artist":..,"out":string}
// sanitize : {"raw":anything,"out":settings}
//   file     : {"text":string,"out":{ok,...}}                     parsePresetFile
//  save : {"settings":raw,"name":string,"out":{ok,error|settings}} savePreset on sanitize(settings)
//   delete   : {"settings":raw,"name":string,"out":settings}
// presets  : {"presets":[...],"bands":[...],"labels":[...],"specs":[...]}
const fs = require('fs'),
  path = require('path');
const C = require('../../js/dejavuamp-core.js');

let seed = 20261013;
const rnd = () => {
  seed |= 0;
  seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const pick = list => list[Math.floor(rnd() * list.length)];
const enc = v =>
  typeof v === 'number' && !Number.isFinite(v)
    ? Number.isNaN(v)
      ? 'NaN'
      : v > 0
        ? 'Infinity'
        : '-Infinity'
    : v;
const encDeep = v =>
  Array.isArray(v)
    ? v.map(encDeep)
    : v && typeof v === 'object'
      ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, encDeep(x)]))
      : enc(v);
const cases = [];
const add = c => cases.push(JSON.stringify(encDeep(c)));

const NUMS = [
  NaN,
  Infinity,
  -Infinity,
  0,
  -0,
  1,
  -1,
  0.04,
  0.05,
  -0.05,
  0.25,
  -0.25,
  0.75,
  1.15,
  -1.15,
  2.5,
  -2.5,
  3.5,
  12,
  -12,
  12.5,
  -12.5,
  13,
  -13,
  59.95,
  -59.95,
  -60,
  -60.04,
  -59.9,
  -61,
  -2,
  -0.1,
  0.1,
  5,
  50,
  99.5,
  100,
  101,
  -100,
  -101,
  1e6,
  -1e6,
  0.5,
  0.74,
  1.5,
  1.74,
  1.76,
  2,
  2.2,
  0.3,
  0.6,
];
for (let i = 0; i < 25; i++)
  NUMS.push(
    (rnd() - 0.5) * 140,
    Math.round((rnd() - 0.5) * 1400) / 10,
    Math.round(rnd() * 100) / 4,
  );
for (const x of NUMS) {
  add({ kind: 'fmt', fn: 'dbToGain', in: x, out: C.dbToGain(x) });
  add({ kind: 'fmt', fn: 'formatDb', in: x, out: C.formatDb(x) });
  if (Number.isFinite(x)) add({ kind: 'fmt', fn: 'signedDb', in: x, out: C.signedDb(x) });
  add({ kind: 'fmt', fn: 'balanceToPan', in: x, out: C.balanceToPan(x) });
  if (Number.isInteger(x))
    add({ kind: 'fmt', fn: 'formatBalance', in: x, out: C.formatBalance(x) });
}
for (const x of [...NUMS, 0.6, 0.62, 0.625, 0.875, 1.125, 1.375, 1.75, 1.875, 3, 100]) {
  // snapSpeed is only defined for numbers; the page passes the slider's number or a stored one that
  // went through number()
  if (Number.isFinite(x))
    add({ kind: 'fmt', fn: 'snapSpeed', in: x, out: C.sanitize({ speed: x }).speed });
}
for (const s of [
  NaN,
  Infinity,
  -Infinity,
  -1,
  -0.5,
  0,
  0.4,
  0.999,
  1,
  59.9,
  60,
  61,
  3599,
  3600,
  3661,
  36000,
  86399,
  86400,
  100000,
  359999.5,
  1e9,
  1e12,
]) {
  add({ kind: 'time', in: s, remaining: false, out: C.formatTime(s, false) });
  add({ kind: 'time', in: s, remaining: true, out: C.formatTime(s, true) });
}
const TEXTS = [
  '',
  'Song',
  'A Very Long Title '.repeat(20),
  'tab\there',
  'new\nline',
  '\u0001\u007f',
  ' lead and trail ',
  undefined,
  null,
  'x'.repeat(199),
  'x'.repeat(200),
  'x'.repeat(201),
];
for (const t of TEXTS)
  for (const a of TEXTS) add({ kind: 'ticker', title: t, artist: a, out: C.tickerText(t, a) });

const WEIRD = [
  undefined,
  null,
  true,
  false,
  0,
  1,
  -1,
  0.25,
  0.3,
  0.74,
  0.75,
  1.25,
  -2.5,
  5,
  12,
  12.5,
  13,
  -12.5,
  -13,
  50,
  99.5,
  100,
  101,
  -100,
  -101,
  -2,
  -60,
  -61,
  0.5,
  1.5,
  2,
  1e9,
  -1e9,
  'abc',
  '5',
  '',
  [],
  [1, 2],
  {},
  { a: 1 },
  'x'.repeat(40),
];
const val = () => pick(WEIRD);
const bandList = () =>
  Array.from({ length: pick([0, 3, 9, 10, 10, 10, 11, 14]) }, () =>
    rnd() < 0.2 ? val() : Math.round((rnd() - 0.5) * 60) / 4,
  );
const NAMES = [
  'Mine',
  'My Curve',
  'Flat',
  'Rock',
  'Voice',
  'rock',
  '  Padded  ',
  '<b>tag</b>',
  'a\tb',
  '',
  'x'.repeat(40),
  'Name With Spaces',
  'q',
];
const userList = () =>
  Array.from({ length: pick([0, 1, 2, 3, 5, 25]) }, () =>
    rnd() < 0.15
      ? val()
      : {
          name: rnd() < 0.1 ? val() : pick(NAMES),
          preamp: val(),
          bands: rnd() < 0.8 ? bandList() : val(),
        },
  );
function rawSettings() {
  const raw = {};
  if (rnd() < 0.8) raw.volumeDb = val();
  if (rnd() < 0.8) raw.balance = val();
  if (rnd() < 0.8) raw.mono = val();
  if (rnd() < 0.8)
    raw.speed = rnd() < 0.5 ? val() : pick([0.5, 0.6, 0.75, 1, 1.1, 1.25, 1.4, 1.5, 1.75, 2, 3]);
  if (rnd() < 0.8) raw.eqOn = val();
  if (rnd() < 0.8) raw.preamp = val();
  if (rnd() < 0.7) raw.bands = rnd() < 0.9 ? bandList() : val();
  if (rnd() < 0.8) raw.limiter = val();
  if (rnd() < 0.8) raw.timeMode = rnd() < 0.5 ? val() : pick(['elapsed', 'remaining', 'other']);
  if (rnd() < 0.7) raw.userPresets = rnd() < 0.9 ? userList() : val();
  if (rnd() < 0.15) {
    const p = pick(C.PRESETS);
    raw.preamp = p.preamp;
    raw.bands = p.bands.slice();
  } // exactly a built-in: the name must be found
  return raw;
}
for (const raw of [null, undefined, 5, 'x', [], {}, true])
  add({ kind: 'sanitize', raw, out: C.sanitize(raw) });
for (let i = 0; i < 150; i++) {
  const raw = rawSettings();
  add({ kind: 'sanitize', raw, out: C.sanitize(raw) });
}

const goodFile = o =>
  JSON.stringify({
    'dva-eq': 1,
    name: 'Mine',
    preamp: 1,
    bands: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ...o,
  });
const FILES = [
  goodFile(),
  goodFile({ name: '' }),
  goodFile({ name: '   ' }),
  goodFile({ name: '<x>' }),
  goodFile({ preamp: 'a' }),
  goodFile({ bands: [1, 2] }),
  goodFile({ bands: [1, 2, 3, 4, 5, 6, 7, 8, 9, 'x'] }),
  goodFile({ bands: [99, -99, 0.3, 0.74, 0.75, 1, 2, 3, 4, 5], preamp: 99 }),
  goodFile({ 'dva-eq': 2 }),
  goodFile({ 'dva-eq': undefined }),
  '{',
  '',
  'null',
  '[]',
  '5',
  '"x"',
  'x'.repeat(2049),
  goodFile({ name: 'x'.repeat(2000) }),
  JSON.stringify({ 'dva-eq': 1, name: 'NoBands' }),
  JSON.stringify({
    'dva-eq': 1,
    name: 'Nulls',
    bands: [null, null, null, null, null, null, null, null, null, null],
  }),
];
for (const t of FILES) add({ kind: 'file', text: t, out: C.parsePresetFile(t) });
for (let i = 0; i < 40; i++) {
  let t = goodFile({
    name: pick(NAMES),
    preamp: val(),
    bands:
      rnd() < 0.8
        ? Array.from({ length: 10 }, () =>
            rnd() < 0.15 ? val() : Math.round((rnd() - 0.5) * 60) / 4,
          )
        : val(),
  });
  if (rnd() < 0.3) {
    const at = Math.floor(rnd() * t.length);
    t =
      t.slice(0, at) +
      pick(['', '"', '}', ',', ' ', '\u0001']) +
      t.slice(at + Math.floor(rnd() * 3));
  }
  add({ kind: 'file', text: t, out: C.parsePresetFile(t) });
}

for (let i = 0; i < 50; i++) {
  const raw = rawSettings(),
    s = C.sanitize(raw),
    name = rnd() < 0.15 ? val() : pick(NAMES);
  add({ kind: 'save', settings: raw, name, out: C.savePreset(s, name) });
  const victim = pick([...NAMES, ...s.userPresets.map(p => p.name)]);
  add({ kind: 'delete', settings: raw, name: victim, out: C.deletePreset(s, victim) });
}
add({
  kind: 'presets',
  presets: C.PRESETS,
  bands: C.BANDS_HZ,
  labels: C.BAND_LABELS,
  specs: C.filterSpecs(),
});

fs.writeFileSync(path.join(__dirname, 'core.golden'), cases.join('\n') + '\n');
console.log(`wrote ${cases.length} cases`);
