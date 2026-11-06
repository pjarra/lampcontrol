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
        adPrefix: id.match(/../g).map(h => parseInt(h, 16)), meshAddress: address
      };
    });
  }
  const SCHEDULES = [
    {id: 'wake', title: 'Wake-up light', routine: P.ROUTINE.dawn, offRoutine: P.ROUTINE.dawnOff, on: true,
      defaults: {time: '07:00', fade: 30, brightness: 100, mired: 250, offEnabled: false, offTime: '08:30'}},
    // Evening used program (3) before the capture showed paired routines; it now
    // uses nightlight (5) + off-part (6), and each save disables program (3).
    {id: 'evening', title: 'Evening: warm & dim', routine: P.ROUTINE.nightlight, offRoutine: P.ROUTINE.nightlightOff, on: true,
      replaces: P.ROUTINE.program, defaults: {time: '19:00', fade: 5, brightness: 30, mired: 370, offEnabled: false, offTime: '23:00'}},
    {id: 'off', title: 'Off', routine: P.ROUTINE.timer, on: false, defaults: {time: '23:00', fade: 1}}
  ];
  // The AwoX app writes 0x0000 as the schedule address for every lamp (capture).
  const SCHEDULE_ADDRESS = 0x0000;
  // Display order Monday first; values are mask bits (bit 0 = Sunday).
  const WEEKDAYS = [['Mo', 1], ['Tu', 2], ['We', 3], ['Th', 4], ['Fr', 5], ['Sa', 6], ['Su', 0]];
  const SLIDER_KINDS = ['brightness', 'temperature', 'color'];
  function storageGet(key) { try { return JSON.parse(localStorage.getItem(key)); } catch (_) { return null; } }
  function storageSet(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch (_) { /* optional */ } }
  function storageRemove(key) { try { localStorage.removeItem(key); } catch (_) { /* optional */ } }
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
  // POSIX TZ rule for the lamp, like the AwoX app sends (e.g. CET-1CEST,M3.5.0/2,M10.5.0/3).
  // Only EU-style zones are derived; elsewhere null, and only the clock offset is sent.
  function posixTZ() {
    const year = new Date().getFullYear();
    const std = -new Date(year, 0, 1).getTimezoneOffset(), dst = -new Date(year, 6, 1).getTimezoneOffset();
    const zone = (Intl.DateTimeFormat().resolvedOptions().timeZone || '');
    const names = {0: ['WET', 'WEST'], 60: ['CET', 'CEST'], 120: ['EET', 'EEST']}[std];
    if (!zone.startsWith('Europe/') || dst !== std + 60 || !names) return null;
    const h = std / 60; // EU changes at 01:00 UTC
    return names[0] + (h ? '-' + h : '0') + names[1] + ',M3.5.0/' + (1 + h) + ',M10.5.0/' + (2 + h);
  }
  function kelvin(mired) { return Math.round(1e6 / mired / 50) * 50; }
  // 0 % = warmest (454 mired), 100 % = coldest (153 mired).
  function whitePercent(mired) { return Math.round((454 - mired) / 3.01); }
  // Optional, per device: credentials in this origin's localStorage (Bluefy/Chrome profile).
  const CRED_KEY = 'mesh-credentials';
  function showCredentialState() {
    const saved = !!storageGet(CRED_KEY);
    $('remember').checked = saved; $('forget').hidden = !saved;
    $('cred-state').textContent = saved ? '· saved on this device' : '';
  }
  function loadCredentials() {
    const saved = storageGet(CRED_KEY);
    if (saved && typeof saved.name === 'string' && typeof saved.password === 'string') {
      $('mesh-name').value = saved.name; $('mesh-password').value = saved.password;
      $('credentials').open = false;
    }
    showCredentialState();
  }
  function rememberCredentials() {
    if ($('remember').checked) storageSet(CRED_KEY, {name: $('mesh-name').value, password: $('mesh-password').value});
    else storageRemove(CRED_KEY);
    showCredentialState();
  }
  $('remember').addEventListener('change', () => { if (!$('remember').checked) rememberCredentials(); });
  $('forget').addEventListener('click', () => {
    storageRemove(CRED_KEY); $('mesh-name').value = $('mesh-password').value = '';
    $('credentials').open = true; showCredentialState();
  });
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
    // lamp.device: the BluetoothDevice picked once (or restored via getDevices()). With it,
    // tapping a control connects and logs in automatically; no picker needed.
    lamp.sync = () => {
      const s = lamp.session, idle = !s.busy && !s.device, usable = bootOK && (s.ready || !!lamp.device);
      q('connect').disabled = q('connect-any').disabled = !bootOK || !idle;
      q('connect').textContent = lamp.device ? 'Pick again' : 'Connect';
      q('disconnect').disabled = !(s.busy || s.device);
      for (const c of ['on', 'off', 'brightness', 'temperature', 'hue', 'saturation']) q(c).disabled = !usable;
      for (const sch of lamp.schedules || []) sch.save.disabled = !usable;
      q('badge').textContent = s.busy && !s.ready ? 'Connecting…' : s.ready ? 'Connected' : lamp.device ? 'Ready' : 'Not set up';
    };
    lamp.session = new globalThis.AwoxTransport.MeshSession({
      log: text => log(lamp.name + ': ' + text), changed: () => { lamp.sync(); syncAll(); pump(lamp); }
    });
    const connect = (request, known = null) => {
      const creds = readCredentials();
      if (!creds) { lamp.pending.clear(); $('credentials').open = true; return; }
      $('status').textContent = known ? 'Connecting to ' + lamp.name + '…' : 'Choose ' + lamp.name + ' in the Bluetooth picker…';
      lamp.session.connect(creds[0], creds[1], request, known)
        .then(() => {
          lamp.device = lamp.session.device;
          storageSet('device:' + lamp.id, lamp.device.id);
          rememberCredentials();
          lamp.sync();
          $('status').textContent = lamp.name + ' connected.';
          // Schedules need the lamp clock; it stores a fixed UTC offset, so refresh on every connect (DST).
          lamp.lastActivity = Date.now();
          queue(lamp, 'timezone', null);
          queue(lamp, 'clock', null);
        })
        .catch(error => { lamp.pending.clear(); reportError(lamp, error); });
    };
    lamp.reconnect = () => { if (lamp.device && !lamp.session.busy && !lamp.session.device) connect(null, lamp.device); };
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
    q('sch-offrow').hidden = !def.offRoutine;
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
    if (def.offRoutine) { q('sch-off-enabled').checked = !!v.offEnabled; q('sch-off-time').value = v.offTime; }
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
        (def.on ? ' · ' + s.brightness + ' % brightness · white ' + whitePercent(s.mired) + ' %' : '') + ' · ' + s.fade + ' min fade' +
        (s.offEnabled ? ' · off at ' + s.offTime : '') : 'Saved as inactive') + ' · sent ' + new Date(s.sentAt).toLocaleString();
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
      const schedule = {address: SCHEDULE_ADDRESS, routine: def.routine, enabled: record.enabled, hour, minute, weekdays, fade, on: def.on,
        parts: def.offRoutine ? P.PARTS.first : P.PARTS.single};
      let offPart = null;
      if (def.offRoutine) {
        record.offEnabled = q('sch-off-enabled').checked; record.offTime = q('sch-off-time').value;
        if (!/^\d\d:\d\d$/.test(record.offTime)) { $('status').textContent = def.title + ': enter an off time.'; return; }
        const [offHour, offMinute] = record.offTime.split(':').map(Number);
        // Same weekdays as the on-part, as the app does.
        offPart = {address: SCHEDULE_ADDRESS, routine: def.offRoutine, enabled: record.enabled && record.offEnabled,
          hour: offHour, minute: offMinute, weekdays, offPart: true, parts: P.PARTS.second};
      }
      if (def.on) {
        record.brightness = Math.min(100, Math.max(1, Math.round(Number(q('sch-brightness').value) || 100)));
        record.mired = 607 - Number(q('sch-mired').value);
        Object.assign(schedule, {brightness: record.brightness, mired: record.mired});
      } else schedule.keepLight = true;
      queue(lamp, 'timezone', null);
      queue(lamp, 'clock', null);
      queue(lamp, 'schedule:' + def.routine, schedule);
      if (offPart) queue(lamp, 'schedule:' + def.offRoutine, offPart);
      if (def.replaces) {
        queue(lamp, 'schedule:' + def.replaces, {address: SCHEDULE_ADDRESS, routine: def.replaces, enabled: false, hour, minute,
          weekdays: 0x7F, fade: 0, on: false, keepLight: true});
      }
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
    timezone: () => {
      const rule = posixTZ();
      return rule ? ['time zone ' + rule, key => P.timezonePacket(key, Math.floor(Date.now() / 1000), rule)] : null;
    },
    clock: () => {
      const now = Math.floor(Date.now() / 1000), offset = -new Date().getTimezoneOffset() * 60;
      return ['clock (UTC offset ' + offset / 3600 + ' h)', key => P.clockPacket(key, now, offset)];
    },
    schedule: s => ['schedule routine ' + s.routine + (s.offPart ? ' (off-part)' : '') + ': ' + (s.enabled ? (s.offPart ? 'OFF' : s.on ? 'ON' : 'OFF') +
      ' at ' + s.hour + ':' + String(s.minute).padStart(2, '0') + ', weekdays 0x' + s.weekdays.toString(16) +
      (s.offPart ? '' : ', fade ' + s.fade + ' min' + (s.keepLight ? ', light unchanged' : ', ' + s.brightness + ' %, ' + s.mired + ' mired'))
      : 'disabled') + ', parts 0x' + (s.parts ?? 1).toString(16).padStart(2, '0'), key => P.schedulePacket(key, s),
      s.offPart ? 100 : undefined] // like the app: off-part right after its on-part
  };
  // Coalesce: slider drags replace the pending value, so only the newest is sent.
  function queue(lamp, kind, value) {
    if (!lamp.session.ready) {
      // Known lamp: keep the command and connect in the background; it is sent once logged in.
      if (!lamp.device || !bootOK) return;
      if (!lamp.session.busy) lamp.reconnect();
    }
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
    const command = COMMANDS[kind.split(':')[0]](value);
    if (!command) { pump(lamp); return; } // e.g. no time zone rule for this zone
    const [description, build, gap] = command;
    lamp.lastActivity = Date.now();
    try { await s.send(description, build, gap); } catch (error) { reportError(lamp, error); }
  }

  function syncAll() {
    const any = bootOK && LAMPS.some(l => l.session?.ready || l.device);
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
  // Restored from the back/forward cache after pagehide cleared the fields.
  addEventListener('pageshow', event => { if (event.persisted) loadCredentials(); });
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

  // Version stamp: written into the meta tag by the publish workflow; version.json
  // holds the newest published one, so a stale tab can offer a reload.
  const BUILD = document.querySelector('meta[name="build"]').content;
  const published = BUILD !== '__BUILD__';
  $('build').textContent = published ? BUILD : 'local development';
  async function checkForUpdate() {
    if (!published) return;
    try {
      const response = await fetch('version.json?t=' + Date.now(), {cache: 'no-store'});
      const latest = (await response.json()).version;
      $('update').hidden = !latest || latest === BUILD;
    } catch (_) { /* offline or not yet deployed: keep quiet */ }
  }
  $('reload').addEventListener('click', () => location.reload());
  document.addEventListener('visibilitychange', () => { if (!document.hidden) checkForUpdate(); });
  checkForUpdate();

  (async () => {
    try {
      // Query string bypasses the CDN cache (max-age 600), so a freshly pushed list shows at once.
      const response = await fetch('lamps.json?t=' + Date.now(), {cache: 'no-store'});
      LAMPS = parseLamps(await response.json());
      LAMPS.forEach(setupLamp);
      loadCredentials();
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
    // Where supported (not in desktop Chrome by default), getDevices() returns lamps picked
    // in earlier visits, so even a reloaded page needs no picker.
    if (bootOK && navigator.bluetooth.getDevices) {
      try {
        const devices = await navigator.bluetooth.getDevices();
        for (const lamp of LAMPS) {
          const id = storageGet('device:' + lamp.id);
          lamp.device = devices.find(d => id && d.id === id) || null;
          if (lamp.device) log(lamp.name + ': remembered from an earlier visit.');
        }
      } catch (_) { log('getDevices() failed; lamps need the picker once per visit.'); }
    } else if (bootOK) log('This browser has no getDevices(); pick each lamp once per visit.');
    for (const lamp of LAMPS) lamp.sync();
    syncAll();
  })();
})();
