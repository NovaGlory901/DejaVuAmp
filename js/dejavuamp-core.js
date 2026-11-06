// DejaVuAmp's logic: volume in decibels, the 10-band equalizer and its presets, balance, remembered
// settings and the player window's formatters. No page access or audio objects, so it runs under
// Node as well as in the browser. In the browser it is window.DejaVuAmpCore.
(function (root) {
  const DB_MIN = -60,
    DB_MAX = 0;
  const SPEEDS = [0.5, 0.75, 1, 1.25, 1.5, 2];
  const BANDS_HZ = [60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000];
  const BAND_LABELS = ['60', '170', '310', '600', '1K', '3K', '6K', '12K', '14K', '16K'];
  const EQ_LIMIT = 12; // decibels either way, on every band and on the preamp
  const MAX_USER_PRESETS = 20,
    MAX_NAME = 24,
    MAX_FILE = 2048;
  const VERSION = 1;

  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const number = (v, fallback) => (typeof v === 'number' && Number.isFinite(v) ? v : fallback);
  const half = v => Math.round(v * 2) / 2;

  // Volume is set in decibels (how loudness is actually heard); the audio graph wants a gain
  // factor.
  const dbToGain = db =>
    // anything odd means silence
    !Number.isFinite(db) || db <= DB_MIN ? 0 : Math.pow(10, clamp(db, DB_MIN, DB_MAX) / 20);
  const formatDb = db =>
    db <= DB_MIN ? 'muted' : Math.abs(db) < 0.05 ? '0.0 dB' : db.toFixed(1) + ' dB';
  const signedDb = db => (db > 0 ? '+' : '') + db.toFixed(1) + ' dB';
  const snapSpeed = s =>
    SPEEDS.reduce((best, x) => (Math.abs(x - s) < Math.abs(best - s) ? x : best), 1);

  const balanceToPan = b => clamp(number(b, 0), -100, 100) / 100;
  const formatBalance = b => (b === 0 ? 'center' : b < 0 ? `${-b}% left` : `${b}% right`);

  const flat = () => new Array(BANDS_HZ.length).fill(0);
  const P = (name, preamp, bands) => Object.freeze({ name, preamp, bands: Object.freeze(bands) });
  const PRESETS = Object.freeze([
    P('Flat', 0, flat()),
    P('Rock', -2, [5, 4, 3, 1, -1, -1.5, 0.5, 3, 4, 5]),
    P('Pop', -1, [-1.5, 1.5, 4, 5, 3.5, 0, -1.5, -1.5, -1, -1]),
    P('Dance', -2, [6, 4.5, 1.5, 0, 0, -3, -4, -4, 0, 0]),
    P('Club', -1, [0, 0, 4, 3, 3, 3, 2, 0, 0, 0]),
    P('Techno', -2, [5, 4, 0.5, -3, -3, 0, 4.5, 5.5, 5.5, 5]),
    P('Full bass', -4, [7, 7, 7, 4, 1, -2.5, -5, -6, -6.5, -6.5]),
    P('Full treble', -3, [-6.5, -6.5, -6.5, -2.5, 1.5, 7, 9.5, 9.5, 9.5, 10]),
    P('Full bass & treble', -4, [5, 4, 0, -5, -3.5, 1, 5, 7, 8, 8]),
    P('Soft', -2, [3, 1, 0, -1, 0, 3, 5, 6, 7, 8]),
    P('Soft rock', -1, [2.5, 2.5, 1.5, 0, -2, -2.5, -1.5, 0, 1.5, 5.5]),
    P('Classical', 0, [0, 0, 0, 0, 0, 0, -6, -6, -6, -8]),
    P('Live', -2, [-3, 0, 2, 3, 3, 3, 2, 1, 1, 1]),
    P('Large hall', -3, [6, 6, 3, 3, 0, -3, -3, -3, 0, 0]),
    P('Party', -2, [4.5, 4.5, 0, 0, 0, 0, 0, 0, 4.5, 4.5]),
    P('Reggae', -1, [0, 0, -0.5, -4.5, 0, 4, 4, 0, 0, 0]),
    P('Ska', -2, [-1.5, -3, -3, -0.5, 2.5, 3.5, 5.5, 6, 7, 6]),
    P('Headphones', -2, [3, 6.5, 3, -2, -1.5, 1, 3, 6, 8, 9]),
    P('Voice', -1, [-3, -2, 0, 3, 5, 5, 4, 2, 0, -2]),
  ]);
  const presetByName = name => PRESETS.find(p => p.name === name) || null;

  const cleanName = raw =>
    (typeof raw === 'string' ? raw : '')
      .replace(/[\u0000-\u001f\u007f<>]/g, '')
      .trim()
      .slice(0, MAX_NAME);
  const cleanBands = raw =>
    BANDS_HZ.map((_, i) =>
      clamp(half(number(Array.isArray(raw) ? raw[i] : 0, 0)), -EQ_LIMIT, EQ_LIMIT),
    );
  const cleanPreamp = raw => clamp(half(number(raw, 0)), -EQ_LIMIT, EQ_LIMIT);

  const DEFAULTS = Object.freeze({
    volumeDb: -2,
    balance: 0,
    mono: false,
    speed: 1,
    eqOn: true,
    preamp: 0,
    bands: flat(),
    limiter: true,
    timeMode: 'elapsed',
    userPresets: [],
  });

  // Anything read back from the browser's storage is untrusted: take only what is valid, fill in
  // the rest.
  function sanitize(raw) {
    raw = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
    const user = (Array.isArray(raw.userPresets) ? raw.userPresets : [])
      .map(p => {
        const name = p && typeof p === 'object' ? cleanName(p.name) : '';
        return name && !presetByName(name)
          ? { name, preamp: cleanPreamp(p.preamp), bands: cleanBands(p.bands) }
          : null;
      })
      .filter(Boolean)
      .filter((p, i, all) => all.findIndex(q => q.name === p.name) === i)
      .slice(0, MAX_USER_PRESETS);
    const preamp = cleanPreamp(raw.preamp),
      bands = cleanBands(raw.bands);
    return {
      volumeDb: clamp(
        Math.round(number(raw.volumeDb, DEFAULTS.volumeDb) * 10) / 10,
        DB_MIN,
        DB_MAX,
      ),
      balance: clamp(Math.round(number(raw.balance, 0)), -100, 100),
      mono: typeof raw.mono === 'boolean' ? raw.mono : DEFAULTS.mono,
      speed: snapSpeed(number(raw.speed, DEFAULTS.speed)),
      eqOn: typeof raw.eqOn === 'boolean' ? raw.eqOn : DEFAULTS.eqOn,
      preamp,
      bands,
      // worked out, never stored: the name of the preset the sliders match, or null
      preset: matchPreset(preamp, bands, user),
      limiter: typeof raw.limiter === 'boolean' ? raw.limiter : DEFAULTS.limiter,
      timeMode: raw.timeMode === 'remaining' ? 'remaining' : 'elapsed',
      userPresets: user,
    };
  }

  // Settings kept per person on this browser, so two people sharing a computer keep their own.
  const storageKey = username =>
    'dejavuamp.v' + VERSION + '.' + String(username || 'guest').slice(0, 64);

  function loadSettings(storage, username) {
    try {
      const own = storage.getItem(storageKey(username));
      if (own !== null) return sanitize(JSON.parse(own));
    } catch (e) {
      /* no storage, or broken JSON: the defaults */
    }
    return sanitize(null);
  }
  function saveSettings(storage, username, settings) {
    try {
      storage.setItem(storageKey(username), JSON.stringify(sanitize(settings)));
      return true;
    } catch (e) {
      return false;
    }
  }

  // What the audio graph should build for the ten bands: the lowest and highest are shelves, the
  // rest are bells.
  function filterSpecs() {
    return BANDS_HZ.map((hz, i) => ({
      type: i === 0 ? 'lowshelf' : i === BANDS_HZ.length - 1 ? 'highshelf' : 'peaking',
      frequency: hz,
      Q: 1.4,
    }));
  }
  // The gain each band should have right now (all zero when the equalizer is switched off).
  const effectiveBands = s => (s.eqOn ? s.bands : flat());
  const effectivePreamp = s => (s.eqOn ? s.preamp : 0);
  function matchPreset(preamp, bands, user = []) {
    const same = p => p.preamp === preamp && p.bands.every((g, i) => g === bands[i]);
    const hit = PRESETS.find(same) || user.find(same);
    return hit ? hit.name : null;
  }

  // Saves the current curve as a named preset (replacing one of the same name). Returns { ok,
  // error, settings }.
  function savePreset(settings, rawName) {
    const name = cleanName(rawName);
    if (!name)
      return { ok: false, error: `Give the preset a name (up to ${MAX_NAME} characters).` };
    if (presetByName(name))
      return { ok: false, error: `"${name}" is a built-in preset. Pick another name.` };
    const rest = settings.userPresets.filter(p => p.name !== name);
    if (rest.length >= MAX_USER_PRESETS)
      return { ok: false, error: `You can keep ${MAX_USER_PRESETS} presets. Delete one first.` };
    return {
      ok: true,
      settings: sanitize({
        ...settings,
        userPresets: [...rest, { name, preamp: settings.preamp, bands: settings.bands }],
      }),
    };
  }
  const deletePreset = (settings, name) =>
    sanitize({ ...settings, userPresets: settings.userPresets.filter(p => p.name !== name) });

  const exportPreset = p =>
    JSON.stringify({ 'dva-eq': 1, name: p.name, preamp: p.preamp, bands: p.bands }, null, 2);
  // Reads the text of a preset file. Returns { ok, error, preset } and changes nothing.
  function parsePresetFile(text) {
    if (typeof text !== 'string' || text.length > MAX_FILE)
      return { ok: false, error: 'That file is too big to be an equalizer preset.' };
    let raw;
    try {
      raw = JSON.parse(text);
    } catch (e) {
      return { ok: false, error: 'That file is not valid JSON.' };
    }
    if (!raw || raw['dva-eq'] !== 1)
      return { ok: false, error: 'That file is not a DejaVuAmp preset (it has no "dva-eq": 1).' };
    const name = cleanName(raw.name);
    if (!name)
      return { ok: false, error: `The preset needs a name of up to ${MAX_NAME} characters.` };
    if (
      !Array.isArray(raw.bands) ||
      raw.bands.length !== BANDS_HZ.length ||
      !raw.bands.every(v => typeof v === 'number' && Number.isFinite(v))
    ) {
      return {
        ok: false,
        error: `The preset needs exactly ${BANDS_HZ.length} band values, all numbers.`,
      };
    }
    return {
      ok: true,
      preset: { name, preamp: cleanPreamp(raw.preamp), bands: cleanBands(raw.bands) },
    };
  }

  function formatTime(seconds, remaining) {
    if (!Number.isFinite(seconds) || seconds < 0) return remaining ? '-:--' : '--:--';
    const s = Math.floor(seconds),
      h = Math.floor(s / 3600),
      m = Math.floor(s / 60) % 60,
      sec = s % 60,
      two = n => String(n).padStart(2, '0');
    return (remaining ? '-' : '') + (h ? `${h}:${two(m)}:${two(sec)}` : `${two(m)}:${two(sec)}`);
  }
  const tickerText = (title, artist) =>
    [artist, title]
      .filter(Boolean)
      .join(' - ')
      .replace(/[\u0000-\u001f\u007f]/g, ' ')
      .slice(0, 200) || 'DejaVuAmp';

  Object.assign(root, {
    DB_MIN,
    DB_MAX,
    SPEEDS,
    BANDS_HZ,
    BAND_LABELS,
    EQ_LIMIT,
    MAX_USER_PRESETS,
    MAX_NAME,
    MAX_FILE,
    PRESETS,
    DEFAULTS,
    clamp,
    dbToGain,
    formatDb,
    signedDb,
    balanceToPan,
    formatBalance,
    presetByName,
    sanitize,
    storageKey,
    loadSettings,
    saveSettings,
    filterSpecs,
    effectiveBands,
    effectivePreamp,
    matchPreset,
    savePreset,
    deletePreset,
    exportPreset,
    parsePresetFile,
    formatTime,
    tickerText,
  });
})(typeof module !== 'undefined' && module.exports ? module.exports : (window.DejaVuAmpCore = {}));
