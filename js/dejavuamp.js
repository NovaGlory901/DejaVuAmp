// DejaVuAmp: a player rack. A player window and an equalizer window over the audio graph
// preamp -> 10-band equalizer -> optional mono fold -> balance -> clip guard -> analyser -> volume.
// Needs window.DejaVuAmpCore (dejavuamp-core.js); takes the <audio> element and a container, so any
// page can mount it. Text goes in with textContent; colors come from the theme variables.
(function () {
  const Core = window.DejaVuAmpCore;

  function create(opts) {
    const { audio, mount, storage = null } = opts;
    let user = opts.username || 'guest';
    const calls = Object.assign(
      { onPrev() {}, onNext() {}, onToggle() {}, isPlaying: () => !audio.paused, onChange() {} },
      opts.calls || {},
    );
    let applied = null,
      routedMono = false,
      settings = Core.loadSettings(storage, user),
      saveTimer = null,
      ctx = null,
      g = null,
      vis = 'bars',
      rafId = 0,
      destroyed = false;
    const reduce = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
    const els = {};

    function ensureGraph() {
      // must first run inside a click: browsers only start audio then
      if (!ctx) {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (AC) {
          try {
            // 'playback' asks for the biggest output buffer: nothing here needs low latency (the
            // song comes from an <audio> element), and a phone that is also drawing visuals on the
            // same thread otherwise lets the small buffer run dry, which is heard as crackle (worst
            // on the phone's own speaker)
            const c = new AC({ latencyHint: 'playback' });
            const src = c.createMediaElementSource(audio);
            const pre = c.createGain();
            const bands = Core.filterSpecs().map(s => {
              const f = c.createBiquadFilter();
              f.type = s.type;
              f.frequency.value = s.frequency;
              f.Q.value = s.Q;
              return f;
            });
            const mono = c.createGain();
            mono.channelCount = 1;
            mono.channelCountMode = 'explicit';
            mono.channelInterpretation = 'speakers';
            const pan = c.createStereoPanner ? c.createStereoPanner() : null;
            const limit = c.createDynamicsCompressor();
            const analyser = c.createAnalyser();
            analyser.fftSize = 2048;
            analyser.smoothingTimeConstant = 0.7;
            const out = c.createGain();
            src.connect(pre);
            let last = pre;
            bands.forEach(f => {
              last.connect(f);
              last = f;
            });
            g = { c, pre, bands, mono, pan, limit, analyser, out, last, tail: pan || limit };
            if (pan) pan.connect(limit);
            limit.connect(analyser);
            analyser.connect(out);
            out.connect(c.destination);
            ctx = c;
            audio.volume = 1;
            route();
          } catch (e) {
            ctx = null;
            g = null;
          }
        }
        els.note.textContent = g
          ? ''
          : 'This browser could not set up the equalizer, balance or visuals. Volume and speed still work.';
        apply();
      }
      if (ctx && ctx.state === 'suspended') ctx.resume().catch(() => {});
    }
    function route() {
      if (!g) return;
      routedMono = settings.mono;
      g.last.disconnect();
      g.mono.disconnect();
      if (settings.mono) {
        g.last.connect(g.mono);
        g.mono.connect(g.tail);
      } else g.last.connect(g.tail);
    }
    function apply() {
      const gain = Core.dbToGain(settings.volumeDb);
      applied = {
        volume: gain,
        preampDb: Core.effectivePreamp(settings),
        bands: Core.effectiveBands(settings).slice(),
        pan: Core.balanceToPan(settings.balance),
        mono: settings.mono,
        limiter: settings.limiter,
        speed: settings.speed,
      }; // what the graph was last told (see inspect)
      if (g) {
        const t = ctx.currentTime,
          set = (param, v) => param.setTargetAtTime(v, t, 0.02);
        set(g.out.gain, gain);
        set(g.pre.gain, Math.pow(10, Core.effectivePreamp(settings) / 20));
        const eq = Core.effectiveBands(settings);
        g.bands.forEach((f, i) => set(f.gain, eq[i]));
        if (g.pan) set(g.pan.pan, Core.balanceToPan(settings.balance));
        // the clip guard: a hard-knee compressor just under full scale; "off" makes it do nothing
        g.limit.threshold.value = settings.limiter ? -2 : 0;
        g.limit.knee.value = 0;
        g.limit.ratio.value = settings.limiter ? 20 : 1;
        g.limit.attack.value = 0.003;
        g.limit.release.value = 0.2;
        if (routedMono !== settings.mono) {
          routedMono = settings.mono;
          route();
        }
      } else audio.volume = gain;
      audio.defaultPlaybackRate = settings.speed;
      audio.playbackRate = settings.speed;
      audio.preservesPitch = audio.mozPreservesPitch = audio.webkitPreservesPitch = true;
    }

    function change(patch, fromUser = true) {
      settings = Core.sanitize({ ...settings, ...patch });
      apply();
      sync();
      clearTimeout(saveTimer);
      saveTimer = setTimeout(() => {
        if (storage) Core.saveSettings(storage, user, settings);
      }, 300);
      if (fromUser) calls.onChange(settings);
    }

    const el = (tag, cls, text) => {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text !== undefined) e.textContent = text;
      return e;
    };
    const button = (cls, text, label, onclick) => {
      const b = el('button', cls, text);
      b.type = 'button';
      if (label) b.setAttribute('aria-label', label);
      b.addEventListener('click', onclick);
      return b;
    };
    // Transport symbols are drawn, not typed: phones show characters like the play and pause signs
    // from their colour emoji font, which ignores the theme.
    const ICONS = {
      play: 'M7 4.5v15l13-7.5z',
      pause: 'M6 4h4.5v16H6zM13.5 4H18v16h-4.5z',
      prev: 'M5.5 5h2.5v14H5.5zM19 5v14L9 12z',
      next: 'M16 5h2.5v14H16zM5 5v14l10-7z',
    };
    const icon = name => {
      const NS = 'http://www.w3.org/2000/svg',
        svg = document.createElementNS(NS, 'svg'),
        path = document.createElementNS(NS, 'path');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('class', 'da-ico');
      svg.setAttribute('aria-hidden', 'true');
      path.setAttribute('d', ICONS[name]);
      svg.append(path);
      return svg;
    };
    const iconButton = (name, label, onclick) => {
      const b = button('da-btn', undefined, label, onclick);
      b.append(icon(name));
      return b;
    };
    const toggle = (text, label, key) => {
      const b = button('da-tog', text, label, () => change({ [key]: !settings[key] }));
      b.dataset.key = key;
      return b;
    };
    const range = (min, max, step, label, oninput) => {
      const r = document.createElement('input');
      r.type = 'range';
      r.min = min;
      r.max = max;
      r.step = step;
      r.setAttribute('aria-label', label);
      r.addEventListener('input', () => oninput(Number(r.value)));
      return r;
    };

    const root = el('div', 'da');
    root.setAttribute('role', 'group');
    root.setAttribute('aria-label', 'DejaVuAmp player and equalizer');
    const main = el('div', 'da-win da-main');
    main.append(el('div', 'da-title', 'DejaVuAmp'));
    const display = el('div', 'da-display');
    els.time = button(
      'da-lcd da-time',
      '00:00',
      'Time. Press to switch between elapsed and remaining',
      () => change({ timeMode: settings.timeMode === 'elapsed' ? 'remaining' : 'elapsed' }),
    );
    els.canvas = document.createElement('canvas');
    els.canvas.className = 'da-vis';
    els.canvas.width = 152;
    els.canvas.height = 44;
    els.canvas.setAttribute('role', 'img');
    els.canvas.setAttribute('aria-label', 'Live sound display. Press to change it');
    els.canvas.tabIndex = 0;
    const cycleVis = () => {
      vis = vis === 'bars' ? 'scope' : vis === 'scope' ? 'off' : 'bars';
      draw(true);
    };
    els.canvas.addEventListener('click', cycleVis);
    els.canvas.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        cycleVis();
      }
    });
    els.ticker = el('div', 'da-ticker');
    els.tickerText = el('span', undefined, 'DejaVuAmp');
    els.ticker.append(els.tickerText);
    els.info = el('div', 'da-info');
    display.append(els.time, els.canvas, els.ticker, els.info);
    els.seek = range(0, 1000, 1, 'Position in the song', v => {
      if (Number.isFinite(audio.duration) && audio.duration > 0)
        audio.currentTime = (audio.duration * v) / 1000;
    });
    els.seek.className = 'da-seek';
    const transport = el('div', 'da-transport');
    els.play = iconButton('play', 'Play or pause', () => calls.onToggle());
    transport.append(
      iconButton('prev', 'Previous song', () => calls.onPrev()),
      els.play,
      iconButton('next', 'Next song', () => calls.onNext()),
    );
    els.vol = range(-60, 0, 0.5, 'Volume in decibels', v => change({ volumeDb: v }));
    els.vol.className = 'da-vol';
    els.volText = el('span', 'da-val');
    els.bal = range(-100, 100, 1, 'Balance, left to right', v => change({ balance: v }));
    els.bal.className = 'da-bal';
    els.balText = el('span', 'da-val');
    els.bal.addEventListener('dblclick', () => change({ balance: 0 }));
    const sliders = el('div', 'da-sliders');
    const lab = (t, input, val) => {
      const l = el('label', 'da-lab');
      l.append(el('span', undefined, t), input, val);
      return l;
    };
    sliders.append(lab('vol', els.vol, els.volText), lab('bal', els.bal, els.balText));
    const switches = el('div', 'da-switches');
    switches.append(
      toggle('MONO', 'Fold the sound to mono', 'mono'),
      toggle('EQ', 'Equalizer on or off', 'eqOn'),
      toggle('LIMIT', 'Prevent clipping on or off', 'limiter'),
    );
    els.speed = document.createElement('select');
    els.speed.setAttribute('aria-label', 'Playback speed');
    Core.SPEEDS.forEach(v => {
      const o = el('option', undefined, v + 'x');
      o.value = String(v);
      els.speed.append(o);
    });
    els.speed.addEventListener('change', () => change({ speed: Number(els.speed.value) }));
    switches.append(els.speed);
    main.append(display, els.seek, el('div', 'da-row'));
    const row = main.lastChild;
    row.append(transport, sliders, switches);

    const eqw = el('div', 'da-win da-eq');
    eqw.append(el('div', 'da-title', 'Equalizer'));
    const bar = el('div', 'da-eqbar');
    els.preset = document.createElement('select');
    els.preset.setAttribute('aria-label', 'Equalizer preset');
    els.preset.addEventListener('change', () => {
      const p =
        Core.presetByName(els.preset.value) ||
        settings.userPresets.find(x => x.name === els.preset.value);
      if (p) change({ preamp: p.preamp, bands: p.bands });
    });
    els.name = document.createElement('input');
    els.name.type = 'text';
    els.name.maxLength = Core.MAX_NAME;
    els.name.placeholder = 'name this curve';
    els.name.setAttribute('aria-label', 'Name for a new preset');
    els.save = button('da-btn da-small', 'Save', 'Save the current curve as a preset', () => {
      const r = Core.savePreset(settings, els.name.value);
      if (!r.ok) {
        say(r.error, true);
        return;
      }
      settings = r.settings;
      els.name.value = '';
      change({});
      say('Saved.', false);
    });
    els.del = button('da-btn da-small', 'Delete', 'Delete this saved preset', () => {
      if (settings.preset && settings.userPresets.some(p => p.name === settings.preset)) {
        settings = Core.deletePreset(settings, settings.preset);
        change({});
      }
    });
    els.exp = button('da-btn da-small', 'Export', 'Export the current curve to a file', () => {
      const a = document.createElement('a'),
        name = settings.preset || 'my curve';
      a.href = URL.createObjectURL(
        new Blob([Core.exportPreset({ name, preamp: settings.preamp, bands: settings.bands })], {
          type: 'application/json',
        }),
      );
      a.download = name.toLowerCase().replace(/[^a-z0-9]+/g, '-') + '.dva-eq.json';
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });
    els.file = document.createElement('input');
    els.file.type = 'file';
    els.file.accept = '.json,application/json';
    els.file.hidden = true;
    els.file.setAttribute('aria-label', 'Preset file');
    els.file.addEventListener('change', async () => {
      const f = els.file.files[0];
      els.file.value = '';
      if (!f) return;
      if (f.size > Core.MAX_FILE) {
        say('That file is too big to be a preset.', true);
        return;
      }
      const r = Core.parsePresetFile(await f.text());
      if (!r.ok) {
        say(r.error, true);
        return;
      }
      const saved = Core.savePreset(
        { ...settings, preamp: r.preset.preamp, bands: r.preset.bands },
        r.preset.name,
      );
      if (!saved.ok) {
        say(saved.error, true);
        return;
      }
      settings = saved.settings;
      change({});
      say(`Added "${r.preset.name}".`, false);
    });
    els.imp = button('da-btn da-small', 'Import', 'Import a preset file', () => els.file.click());
    els.reset = button('da-btn da-small', 'Reset', 'Reset the equalizer to flat', () =>
      change({ preamp: 0, bands: new Array(10).fill(0) }),
    );
    bar.append(els.preset, els.name, els.save, els.del, els.exp, els.imp, els.file, els.reset);
    els.say = el('p', 'da-say');
    els.say.setAttribute('role', 'status');
    const say = (t, bad) => {
      els.say.textContent = t;
      els.say.className = 'da-say ' + (bad ? 'bad' : 'ok');
    };
    const sliderBox = el('div', 'da-bands');
    const column = (labelText, input) => {
      const c = el('div', 'da-col');
      input.classList.add('da-v');
      c.append(el('span', 'da-db'), input, el('span', 'da-hz', labelText));
      return c;
    };
    const vslider = (label, oninput) => {
      const r = range(-Core.EQ_LIMIT, Core.EQ_LIMIT, 0.5, label, oninput);
      r.setAttribute('orient', 'vertical');
      return r;
    };
    els.pre = vslider('Preamp in decibels', v => change({ preamp: v }));
    const preCol = column('PRE', els.pre);
    sliderBox.append(preCol);
    const scale = el('div', 'da-scale');
    ['+12', '0', '-12'].forEach(t => scale.append(el('span', undefined, t)));
    sliderBox.append(scale);
    els.bandIn = Core.BANDS_HZ.map((hz, i) => {
      const r = vslider(`${Core.BAND_LABELS[i]} hertz in decibels`, v => {
        const b = settings.bands.slice();
        b[i] = v;
        change({ bands: b });
      });
      sliderBox.append(column(Core.BAND_LABELS[i], r));
      return r;
    });
    els.note = el('p', 'da-note');
    eqw.append(bar, els.say, sliderBox);
    root.append(main, eqw, els.note);
    mount.replaceChildren(root);

    function sync() {
      els.vol.value = settings.volumeDb;
      els.volText.textContent = Core.formatDb(settings.volumeDb);
      els.bal.value = settings.balance;
      els.balText.textContent = Core.formatBalance(settings.balance);
      els.speed.value = String(settings.speed);
      root
        .querySelectorAll('.da-tog')
        .forEach(b => b.setAttribute('aria-pressed', String(!!settings[b.dataset.key])));
      els.pre.value = settings.preamp;
      setDb(els.pre, settings.preamp);
      els.bandIn.forEach((r, i) => {
        r.value = settings.bands[i];
        setDb(r, settings.bands[i]);
      });
      root.classList.toggle('da-eq-off', !settings.eqOn);
      els.preset.replaceChildren();
      if (!settings.preset) {
        const o = el('option', undefined, '— custom —');
        o.value = '';
        o.disabled = true;
        els.preset.append(o);
      }
      const group = (label, list) => {
        if (!list.length) return;
        const og = document.createElement('optgroup');
        og.label = label;
        list.forEach(p => {
          const o = el('option', undefined, p.name);
          o.value = p.name;
          og.append(o);
        });
        els.preset.append(og);
      };
      group('Presets', Core.PRESETS);
      group('Yours', settings.userPresets);
      els.preset.value = settings.preset || '';
      els.del.hidden = !(
        settings.preset && settings.userPresets.some(p => p.name === settings.preset)
      );
      els.info.textContent = `${settings.mono ? 'mono' : 'stereo'} · ${settings.speed}x`;
      tick();
    }
    function setDb(input, v) {
      input.setAttribute('aria-valuetext', Core.signedDb(v));
      input.parentNode.querySelector('.da-db').textContent = (v > 0 ? '+' : '') + v;
    }

    function tick() {
      const remaining = settings.timeMode === 'remaining';
      const t = remaining ? audio.duration - audio.currentTime : audio.currentTime;
      els.time.textContent = Core.formatTime(t, remaining);
      if (Number.isFinite(audio.duration) && audio.duration > 0) {
        els.seek.disabled = false;
        if (document.activeElement !== els.seek)
          els.seek.value = Math.round((audio.currentTime / audio.duration) * 1000);
      } else {
        els.seek.disabled = true;
        els.seek.value = 0;
      }
      const shown = calls.isPlaying() ? 'pause' : 'play';
      if (els.play.dataset.shown !== shown) {
        els.play.dataset.shown = shown;
        els.play.replaceChildren(icon(shown));
      }
    }
    audio.addEventListener('timeupdate', tick);
    ['play', 'pause', 'loadedmetadata', 'durationchange', 'emptied'].forEach(ev =>
      audio.addEventListener(ev, tick),
    );

    function setNowPlaying(info) {
      const text = Core.tickerText(info && info.title, info && info.artist);
      els.tickerText.textContent = text;
      requestAnimationFrame(() => {
        const over = els.tickerText.scrollWidth - els.ticker.clientWidth;
        els.ticker.classList.toggle('da-scroll', over > 0 && !reduce);
        els.tickerText.style.setProperty('--da-over', over > 0 ? -over - 12 + 'px' : '0px');
        els.tickerText.title = text;
      });
    }

    const BARS = 24,
      peaks = new Array(BARS).fill(0);
    let colors = null,
      colorsAt = 0,
      lastDraw = 0;
    function themeColors() {
      const now = performance.now();
      if (!colors || now - colorsAt > 1000) {
        const cs = getComputedStyle(root);
        colors = {
          bg: cs.getPropertyValue('--bg-deep').trim() || '#000',
          a: cs.getPropertyValue('--cyan').trim() || '#8af3ff',
          b: cs.getPropertyValue('--pink').trim() || '#ff9de6',
          dim: cs.getPropertyValue('--border').trim() || '#445',
        };
        colorsAt = now;
      }
      return colors;
    }
    function draw(force) {
      const cv = els.canvas,
        c2 = cv.getContext('2d'),
        col = themeColors(),
        W = cv.width,
        H = cv.height;
      c2.fillStyle = col.bg;
      c2.fillRect(0, 0, W, H);
      if (vis === 'off' || !g) {
        c2.fillStyle = col.dim;
        c2.fillRect(0, H - 2, W, 2);
        return;
      }
      const an = g.analyser;
      if (vis === 'bars') {
        const bytes = new Uint8Array(an.frequencyBinCount);
        an.getByteFrequencyData(bytes);
        const binHz = ctx.sampleRate / 2 / bytes.length,
          lo = 40,
          hi = 16000,
          bw = W / BARS;
        for (let i = 0; i < BARS; i++) {
          const f0 = lo * Math.pow(hi / lo, i / BARS),
            f1 = lo * Math.pow(hi / lo, (i + 1) / BARS);
          const a = Math.floor(f0 / binHz),
            b = Math.max(a + 1, Math.ceil(f1 / binHz));
          let sum = 0;
          for (let k = a; k < b && k < bytes.length; k++) sum += bytes[k];
          const v = sum / (b - a) / 255,
            h = Math.round(v * (H - 2));
          peaks[i] = Math.max(h, peaks[i] - 0.8);
          const grad = c2.createLinearGradient(0, H, 0, 0);
          grad.addColorStop(0, col.a);
          grad.addColorStop(1, col.b);
          c2.fillStyle = grad;
          c2.fillRect(i * bw + 1, H - h, bw - 2, h);
          c2.fillStyle = col.b;
          c2.fillRect(i * bw + 1, H - peaks[i] - 2, bw - 2, 2);
        }
      } else {
        const data = new Uint8Array(an.fftSize);
        an.getByteTimeDomainData(data);
        c2.strokeStyle = col.a;
        c2.lineWidth = 1.5;
        c2.beginPath();
        for (let x = 0; x < W; x++) {
          const y = (data[Math.floor((x / W) * data.length)] / 255) * H;
          if (x === 0) c2.moveTo(x, y);
          else c2.lineTo(x, y);
        }
        c2.stroke();
      }
    }
    function loop(now) {
      if (destroyed) return;
      rafId = requestAnimationFrame(loop);
      if (!mount.offsetParent || document.hidden) return; // only while the rack is on screen
      if (now - lastDraw < (reduce ? 120 : 33)) return;
      lastDraw = now;
      draw();
    }
    rafId = requestAnimationFrame(loop);

    sync();
    setNowPlaying(null);
    const api = {
      ensureGraph,
      apply,
      change,
      setNowPlaying,
      tick,
      settings: () => settings,
      analyser: () => (g ? g.analyser : null),
      context: () => ctx,
      // For tests and diagnostics: the graph's shape and what it was last told to do. (Web Audio
      // ramps its values, so the targets are what matter.)
      inspect: () => ({
        built: !!g,
        bandTypes: g ? g.bands.map(f => f.type) : [],
        hasPanner: !!(g && g.pan),
        analyserFft: g ? g.analyser.fftSize : 0,
        applied,
      }),
      load(name) {
        user = name || 'guest';
        settings = Core.loadSettings(storage, user);
        apply();
        sync();
      },
      destroy() {
        destroyed = true;
        cancelAnimationFrame(rafId);
      },
    };
    return api;
  }

  window.DejaVuAmp = { create };
})();
