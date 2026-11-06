(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const P = globalThis.AwoxProtocol;
  // Each lamp is controlled over its own direct connection with destination
  // 0x0000 (the connected lamp). Schlafzimmerschrank's advertised address 0xFFFE
  // is unusable and it does not relay mesh traffic, so the mesh is not relied on.
  const CONNECTED = P.TARGETS.connected;
  // Lamps come from lamps.json (written by tools/scan-lamps.swift). All lamps
  // advertise the same name, so the picker filters on manufacturer data
  // (company 0x0160): the first six data bytes are per-lamp and constant, and
  // unlike the mesh address (scan response only) they are already in the short
  // primary advertisement. A browser cannot read advertisements, hence the Mac tool.
  let LAMPS = [];
  function parseLamps(json) {
    return (json.lamps || []).map(entry => {
      const id = String(entry.id || '').toLowerCase(), address = Number(entry.meshAddress);
      if (!/^[0-9a-f]{12}$/.test(id) || !Number.isInteger(address) || address < 0 || address > 0xFFFF) throw new Error('bad lamp entry');
      return {
        id, name: String(entry.name || 'Lamp ' + id).slice(0, 40),
        adPrefix: id.match(/../g).map(h => parseInt(h, 16)),
        // 0xFFFE/0xFFFF (and 0) are unusable inside schedules; 0x0000 works there (verified).
        scheduleAddress: address >= 0xFFFE ? 0x0000 : address
      };
    });
  }
  const SCHEDULES = [
    {id: 'wake', title: 'Wake-up light', routine: P.ROUTINE.dawn, on: true, defaults: {time: '07:00', fade: 30, brightness: 100, mired: 250}},
    {id: 'evening', title: 'Evening: warm & dim', routine: P.ROUTINE.program, on: true, defaults: {time: '19:00', fade: 5, brightness: 30, mired: 370}},
    {id: 'off', title: 'Off', routine: P.ROUTINE.timer, on: false, defaults: {time: '23:00', fade: 1}}
  ];
  // Display order Monday first; values are mask bits (bit 0 = Sunday).
  const WEEKDAYS = [['Mo', 1], ['Tu', 2], ['We', 3], ['Th', 4], ['Fr', 5], ['Sa', 6], ['Su', 0]];
  const SLIDER_KINDS = ['brightness', 'temperature', 'color'];
  const hex4 = n => '0x' + n.toString(16).toUpperCase().padStart(4, '0');
  function storageGet(key) { try { return JSON.parse(localStorage.getItem(key)); } catch (_) { return null; } }
  function storageSet(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch (_) { /* optional */ } }
  const events = [];
  let bootOK = false;

  function log(text) {
    events.push(new Date().toISOString() + ' ' + text);
    if (events.length > 250) events.shift();
    $('log').value = events.join('\n');
    $('log').scrollTop = $('log').scrollHeight;
  }
  function pickerFilter(prefix) {
    return {filters: [{manufacturerData: [{companyIdentifier: 0x0160, dataPrefix: Uint8Array.from(prefix)}]}]};
  }
  function kelvin(mired) { return Math.round(1e6 / mired / 50) * 50; }
  // 0 % = warmest (454 mired), 100 % = coldest (153 mired).
  function whitePercent(mired) { return Math.round((454 - mired) / 3.01); }
  function readCredentials() {
    let name;
    try {
      name = P.credentials($('mesh-name').value);
      return [name, P.credentials($('mesh-password').value)];
    } catch (_) {
      if (name) name.fill(0);
      $('status').textContent = 'Enter both mesh credentials, each 1–16 UTF-8 bytes.';
      return null;
    }
  }
  function reportError(lamp, error) {
    // Browser error.message may contain device names; do not copy it into logs.
    const known = {
      NotFoundError: 'No lamp selected, or none matched. Try “Pick from full list”.',
      NotAllowedError: 'Bluetooth access was declined.',
      SecurityError: 'Bluetooth access was blocked. Use localhost or HTTPS.',
      NetworkError: 'Bluetooth communication failed. Reconnect.',
      NotSupportedError: 'This Bluetooth operation is unsupported by the browser.',
      TypeError: 'This browser does not support the filtered picker. Use “Pick from full list”.',
      InvalidStateError: 'Bluetooth is no longer connected. Reconnect.',
      AbortError: 'Bluetooth operation was cancelled.'
    };
    const text = lamp.name + ': ' + (error.safeCode ? error.safeCode + ': ' + error.message : known[error.name] || 'Operation failed. Reconnect.');
    log(text); $('status').textContent = text; syncAll();
  }

  function setupLamp(lamp) {
    const el = $('lamp-template').content.firstElementChild.cloneNode(true);
    const q = cls => el.querySelector('.' + cls);
    q('lamp-name').textContent = lamp.name;
    $('lamps').append(el);
    lamp.el = el;
    lamp.pending = new Map(); // latest unsent value per control
    lamp.sync = () => {
      const s = lamp.session, idle = !s.busy && !s.device;
      q('connect').disabled = q('connect-any').disabled = !bootOK || !idle;
      q('disconnect').disabled = !(s.busy || s.device);
      for (const c of ['on', 'off', 'brightness', 'temperature', 'hue', 'saturation']) q(c).disabled = !s.ready;
      for (const sch of lamp.schedules || []) sch.save.disabled = !s.ready;
      q('badge').textContent = s.busy && !s.ready ? 'Connecting…' : s.ready ? 'Connected' : 'Disconnected';
      if (!s.device) lamp.pending.clear();
    };
    lamp.session = new globalThis.AwoxTransport.MeshSession({
      log: text => log(lamp.name + ': ' + text), changed: () => { lamp.sync(); syncAll(); pump(lamp); }
    });
    const connect = request => {
      const creds = readCredentials();
      if (!creds) return;
      $('status').textContent = 'Choose ' + lamp.name + ' in the Bluetooth picker…';
      lamp.session.connect(creds[0], creds[1], request)
        .then(() => {
          $('status').textContent = lamp.name + ' connected.';
          // Schedules need the lamp clock; it stores a fixed UTC offset, so refresh on every connect (DST).
          lamp.lastActivity = Date.now();
          queue(lamp, 'clock', null);
        })
        .catch(error => reportError(lamp, error));
    };
    q('connect').addEventListener('click', () => connect(pickerFilter(lamp.adPrefix)));
    q('connect-any').addEventListener('click', () => connect({acceptAllDevices: true}));
    q('disconnect').addEventListener('click', () => { lamp.session.disconnect(); log(lamp.name + ': disconnected by user.'); });
    q('on').addEventListener('click', () => queue(lamp, 'power', true));
    q('off').addEventListener('click', () => queue(lamp, 'power', false));
    const showBrightness = () => { q('brightness-out').textContent = Math.round((q('brightness').value - 2) / 2.52) + ' %'; };
    const showTemperature = () => { q('temperature-out').textContent = kelvin(607 - q('temperature').value) + ' K'; };
    // Temperature slider runs warm → cold left to right: mired = 607 - position.
    q('brightness').addEventListener('input', () => { showBrightness(); queue(lamp, 'brightness', Number(q('brightness').value)); });
    q('temperature').addEventListener('input', () => { showTemperature(); queue(lamp, 'temperature', 607 - Number(q('temperature').value)); });
    const showColor = () => {
      q('color-out').textContent = Math.round(q('hue').value * 360 / 254) + '°';
      q('saturation-out').textContent = Math.round(q('saturation').value / 2.54) + ' %';
    };
    const sendColor = () => { showColor(); queue(lamp, 'color', [Number(q('hue').value), Number(q('saturation').value)]); };
    q('hue').addEventListener('input', sendColor);
    q('saturation').addEventListener('input', sendColor);
    showBrightness(); showTemperature(); showColor();

    // Schedules: one slot per routine type in the lamp.
    const box = q('schedules');
    lamp.schedules = SCHEDULES.map(def => setupSchedule(lamp, def, box));
  }

  function setupSchedule(lamp, def, box) {
    const el = $('schedule-template').content.firstElementChild.cloneNode(true);
    const q = cls => el.querySelector('.' + cls);
    q('sch-title').textContent = def.title;
    if (!def.on) for (const node of el.querySelectorAll('.sch-light')) node.hidden = true;
    const days = WEEKDAYS.map(([label, bit]) => {
      const wrap = document.createElement('label'), input = document.createElement('input');
      input.type = 'checkbox'; input.checked = true; input.dataset.bit = bit;
      wrap.append(input, ' ' + label); q('weekdays').append(wrap);
      return input;
    });
    const storeKey = 'schedule:' + lamp.id + ':' + def.id;
    const saved = storageGet(storeKey);
    const v = {enabled: false, weekdays: 0x7F, ...def.defaults, ...(saved || {})};
    q('sch-enabled').checked = !!v.enabled; q('sch-time').value = v.time; q('sch-fade').value = v.fade;
    // White slider runs warm → cold like the live one: mired = 607 - position; 0 % = warmest.
    const showWhite = () => {
      const position = Number(q('sch-mired').value);
      q('sch-mired-out').textContent = whitePercent(607 - position) + ' % · ' + kelvin(607 - position) + ' K';
    };
    if (def.on) {
      q('sch-brightness').value = v.brightness; q('sch-mired').value = String(607 - v.mired);
      q('sch-mired').addEventListener('input', showWhite); showWhite();
    }
    for (const d of days) d.checked = !!(v.weekdays & (1 << d.dataset.bit));
    const showSaved = s => {
      if (!s) { q('sch-saved').textContent = 'Not saved from this browser yet.'; return; }
      const dayList = WEEKDAYS.filter(([, bit]) => s.weekdays & (1 << bit)).map(([label]) => label).join(' ');
      q('sch-saved').textContent = (s.enabled ? 'Active: ' + s.time + ' · ' + (s.weekdays === 0x7F ? 'daily' : dayList) +
        (def.on ? ' · ' + s.brightness + ' % brightness · white ' + whitePercent(s.mired) + ' %' : '') + ' · ' + s.fade + ' min fade' : 'Saved as inactive') + ' · sent ' + new Date(s.sentAt).toLocaleString();
    };
    showSaved(saved);
    const save = q('sch-save');
    save.addEventListener('click', () => {
      const time = q('sch-time').value, weekdays = days.reduce((m, d) => d.checked ? m | (1 << d.dataset.bit) : m, 0);
      const fade = Math.round(Number(q('sch-fade').value));
      if (!/^\d\d:\d\d$/.test(time)) { $('status').textContent = def.title + ': enter a time.'; return; }
      if (!weekdays) { $('status').textContent = def.title + ': choose at least one day.'; return; }
      if (!(fade >= 0 && fade <= 63)) { $('status').textContent = def.title + ': fade must be 0–63 minutes.'; return; }
      const [hour, minute] = time.split(':').map(Number);
      const record = {enabled: q('sch-enabled').checked, time, weekdays, fade};
      const schedule = {address: lamp.scheduleAddress, routine: def.routine, enabled: record.enabled, hour, minute, weekdays, fade, on: def.on};
      if (def.on) {
        record.brightness = Math.min(100, Math.max(1, Math.round(Number(q('sch-brightness').value) || 100)));
        record.mired = 607 - Number(q('sch-mired').value);
        Object.assign(schedule, {brightness: record.brightness, mired: record.mired});
      } else schedule.keepLight = true;
      queue(lamp, 'clock', null);
      queue(lamp, 'schedule:' + def.routine, schedule);
      record.sentAt = Date.now();
      storageSet(storeKey, record); showSaved(record);
      $('status').textContent = lamp.name + ': ' + def.title + ' sent.';
    });
    box.append(el);
    return {save};
  }

  const COMMANDS = {
    power: on => ['power ' + (on ? 'ON' : 'OFF'), key => P.powerPacket(key, on, CONNECTED)],
    brightness: v => ['brightness ' + v, key => P.brightnessPacket(key, v, CONNECTED)],
    temperature: m => ['temperature ' + m + ' mired', key => P.temperaturePacket(key, m, CONNECTED)],
    color: ([h, s]) => ['colour hue ' + h + ' sat ' + s, key => P.colorPacket(key, h, s, CONNECTED)],
    clock: () => {
      const now = Math.floor(Date.now() / 1000), offset = -new Date().getTimezoneOffset() * 60;
      return ['clock (UTC offset ' + offset / 3600 + ' h)', key => P.clockPacket(key, now, offset)];
    },
    schedule: s => ['schedule routine ' + s.routine + ': ' + (s.enabled ? (s.on ? 'ON' : 'OFF') + ' at ' + s.hour + ':' + String(s.minute).padStart(2, '0') +
      ', weekdays 0x' + s.weekdays.toString(16) + ', fade ' + s.fade + ' min' + (s.keepLight ? ', light unchanged' : ', ' + s.brightness + ' %, ' + s.mired + ' mired')
      : 'disabled') + ', address ' + hex4(s.address), key => P.schedulePacket(key, s)]
  };
  // Coalesce: slider drags replace the pending value, so only the newest is sent.
  function queue(lamp, kind, value) {
    if (!lamp.session.ready) return;
    // Power supersedes pending slider moves; the latest colour mode wins.
    if (kind === 'power') for (const k of SLIDER_KINDS) lamp.pending.delete(k);
    if (kind === 'temperature') lamp.pending.delete('color');
    if (kind === 'color') lamp.pending.delete('temperature');
    lamp.pending.delete(kind); // re-insert so it is sent after earlier pending kinds
    lamp.pending.set(kind, value);
    pump(lamp);
  }
  async function pump(lamp) {
    const s = lamp.session;
    if (s.busy || !s.ready || !lamp.pending.size) return;
    const [kind, value] = lamp.pending.entries().next().value;
    lamp.pending.delete(kind);
    const [description, build] = COMMANDS[kind.split(':')[0]](value);
    lamp.lastActivity = Date.now();
    try { await s.send(description, build); } catch (error) { reportError(lamp, error); }
  }

  function syncAll() {
    const any = LAMPS.some(l => l.session?.ready);
    $('all-on').disabled = $('all-off').disabled = !any;
    $('mesh-name').disabled = $('mesh-password').disabled = LAMPS.some(l => l.session?.busy && !l.session.ready);
  }
  for (const [id, on] of [['all-on', true], ['all-off', false]]) {
    $(id).addEventListener('click', () => { for (const lamp of LAMPS) queue(lamp, 'power', on); });
  }
  $('copy').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('log').value); $('copy').textContent = 'Copied'; }
    catch (_) { $('log').focus(); $('log').select(); $('copy').textContent = 'Select & copy'; }
    setTimeout(() => { $('copy').textContent = 'Copy log'; }, 2500);
  });
  addEventListener('pagehide', () => {
    for (const lamp of LAMPS) lamp.session.disconnect();
    $('mesh-name').value = $('mesh-password').value = '';
  });
  // A connection left open (e.g. the Mac sleeping with the tab connected) can
  // wedge the lamp until it is power-cycled, so release connections eagerly.
  // Note: no Bluetooth write may run here; just close the links.
  function releaseAll(reason) {
    let released = false;
    for (const lamp of LAMPS) {
      if (lamp.session.device) { lamp.session.disconnect(); log(lamp.name + ': disconnected (' + reason + ').'); released = true; }
    }
    if (released) $('status').textContent = 'Disconnected (' + reason + '). Connect again when needed.';
  }
  const connecting = () => LAMPS.some(l => l.session.busy && !l.session.ready);
  document.addEventListener('visibilitychange', () => {
    // The Bluetooth picker can obscure the page on iOS, so never cancel a connect in progress.
    if (document.hidden && !connecting()) releaseAll('page hidden');
  });
  addEventListener('freeze', () => releaseAll('page frozen'));
  const IDLE_MS = 5 * 60 * 1000;
  setInterval(() => {
    for (const lamp of LAMPS) {
      const s = lamp.session;
      if (s.ready && !s.busy && !lamp.pending.size && Date.now() - (lamp.lastActivity || 0) > IDLE_MS) {
        s.disconnect(); log(lamp.name + ': disconnected after 5 min idle.');
        $('status').textContent = lamp.name + ' disconnected after 5 min idle. Connect again when needed.';
      }
    }
  }, 15000);

  (async () => {
    try {
      // Query string bypasses the CDN cache (max-age 600), so a freshly pushed list shows at once.
      const response = await fetch('lamps.json?t=' + Date.now(), {cache: 'no-store'});
      LAMPS = parseLamps(await response.json());
      LAMPS.forEach(setupLamp);
    } catch (_) {
      log('Could not load lamps.json.'); $('status').textContent = 'Could not load the lamp list (lamps.json).';
      return;
    }
    try {
      if (!globalThis.isSecureContext || !globalThis.crypto?.subtle) throw new Error('no secure context');
      await P.selfTest(); log('Crypto self-test PASS (dummy credentials).');
      bootOK = !!navigator.bluetooth;
      $('status').textContent = !bootOK ? 'This browser has no Web Bluetooth. Use Chrome or Bluefy.' :
        LAMPS.length ? 'Ready. Enter the mesh credentials, then connect a lamp.' : 'No lamps in lamps.json yet. Run tools/scan-lamps.swift on the Mac.';
    } catch (_) { log('Crypto self-test FAILED. Controls disabled.'); $('status').textContent = 'Crypto self-test failed.'; }
    for (const lamp of LAMPS) lamp.sync();
    syncAll();
  })();
})();
