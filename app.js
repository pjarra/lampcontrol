(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const P = globalThis.AwoxProtocol;
  // Two routes to a lamp: its own direct connection (destination 0x0000 = the
  // connected lamp; needed for schedules and the clock, whose frames carry no
  // destination), or the shared mesh connection to any lamp, addressed by the
  // lamp's mesh address. The mesh route needs no per-lamp picker, which matters
  // in Bluefy: it cannot filter by advertisement data, and all lamps share a name.
  const CONNECTED = P.TARGETS.connected;
  const MESH_KINDS = new Set(['power', 'brightness', 'temperature', 'color']);
  // Experimental: schedule frames through the mesh, with the target lamp's mesh address
  // in the schedule body (the app uses 0x0000 over a direct connection). Clock and time
  // zone frames carry no address and stay direct-only; the lamps keep their clock.
  const MESH_ROUTABLE = new Set([...MESH_KINDS, 'schedule']);
  const MESH = {name: 'Mesh', device: null, pending: new Map()};
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
        adPrefix: id.match(/../g).map(h => parseInt(h, 16)), meshAddress: address,
        // 0x0000 / 0xFFFE: lamp without a usable mesh address (not properly provisioned).
        meshOK: address !== 0 && P.targetOK(address)
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
  // Chrome matches dataPrefix against the data after the company ID. Some browsers (Bluefy on
  // iOS, where CoreBluetooth reports the company bytes as part of the data) may not; the extra
  // variants cover that and a byte-swapped company ID. Filters are OR-ed, so in Chrome only
  // the first can match.
  function pickerFilter(prefix) {
    const variants = [[0x0160, prefix], [0x0160, [0x60, 0x01, ...prefix]], [0x6001, prefix], [0x6001, [0x60, 0x01, ...prefix]]];
    return {filters: variants.map(([companyIdentifier, bytes]) =>
      ({manufacturerData: [{companyIdentifier, dataPrefix: Uint8Array.from(bytes)}]}))};
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
      $('status').textContent = 'Enter both mesh credentials (each 1–16 UTF-8 bytes); tick “Remember” to keep them on this device.';
      log('No mesh credentials entered; connection not started.');
      $('credentials').open = true; $('credentials').scrollIntoView({block: 'start'});
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
    // error.name is a standard DOMException/Error type and safe to show; error.message may not be.
    const kind = typeof error?.name === 'string' ? error.name.slice(0, 40) : typeof error;
    // Bluefy rejects a closed or empty picker with a bare number. Its picker cannot filter by
    // advertisement data, so a lamp's own Connect shows nothing there: point to the mesh card.
    if (kind === 'number' || kind === 'NotFoundError') {
      const text = lamp === MESH ? 'Mesh: no lamp selected. Try “Pick from full list” in the top card.'
        : lamp.name + ': no lamp selected. If the list was empty (e.g. in Bluefy), use “All lamps, one connection” above instead.';
      log(text); $('status').textContent = text; syncAll();
      return;
    }
    const text = lamp.name + ': ' + (error?.safeCode ? error.safeCode + ': ' + error.message :
      (known[kind] || 'Operation failed. Reconnect.') + ' [' + kind + ']');
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
      const s = lamp.session, idle = !s.busy && !s.device, direct = bootOK && (s.ready || !!lamp.device);
      const viaMesh = bootOK && lamp.meshOK && !!(MESH.session?.ready || MESH.device);
      q('connect').disabled = q('connect-any').disabled = !bootOK || !idle;
      q('connect').textContent = lamp.device ? 'Pick again' : 'Connect';
      q('disconnect').disabled = !(s.busy || s.device);
      for (const c of ['on', 'off', 'brightness', 'temperature', 'hue', 'saturation']) q(c).disabled = !(direct || viaMesh);
      for (const sch of lamp.schedules || []) {
        sch.save.disabled = !(direct || viaMesh);
        sch.save.textContent = direct ? 'Save to lamp' : 'Save via mesh (experimental)';
      }
      q('badge').textContent = s.busy && !s.ready ? 'Connecting…' : s.ready ? 'Connected' :
        MESH.session?.ready && lamp.meshOK ? 'Via mesh' : lamp.device || viaMesh ? 'Ready' :
        lamp.meshOK ? 'Not set up' : 'Needs own connection';
    };
    lamp.session = new globalThis.AwoxTransport.MeshSession({
      log: text => log(lamp.name + ': ' + text), changed: () => { lamp.sync(); syncAll(); pumpAll(); }
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
    lamp.state = {on: null, mode: null, touched: false};
    // Scene entry from the controls: b = brightness 2..254, t = mired, c = [hue, saturation].
    lamp.snapshot = () => ({id: lamp.id, name: lamp.name, on: lamp.state.on ?? true, b: Number(q('brightness').value),
      ...(lamp.state.mode === 'color' ? {c: [Number(q('hue').value), Number(q('saturation').value)]} : {t: 607 - Number(q('temperature').value)})});
    lamp.applyUI = s => {
      if (s.b) { q('brightness').value = s.b; showBrightness(); }
      if (s.t) { q('temperature').value = 607 - s.t; showTemperature(); }
      if (s.c) { q('hue').value = s.c[0]; q('saturation').value = s.c[1]; showColor(); }
    };

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
    power: (on, dest) => ['power ' + (on ? 'ON' : 'OFF'), key => P.powerPacket(key, on, dest)],
    brightness: (v, dest) => ['brightness ' + v, key => P.brightnessPacket(key, v, dest)],
    temperature: (m, dest) => ['temperature ' + m + ' mired', key => P.temperaturePacket(key, m, dest)],
    color: ([h, s], dest) => ['colour hue ' + h + ' sat ' + s, key => P.colorPacket(key, h, s, dest)],
    timezone: () => {
      const rule = posixTZ();
      return rule ? ['time zone ' + rule, key => P.timezonePacket(key, Math.floor(Date.now() / 1000), rule)] : null;
    },
    clock: () => {
      const now = Math.floor(Date.now() / 1000), offset = -new Date().getTimezoneOffset() * 60;
      return ['clock (UTC offset ' + offset / 3600 + ' h)', key => P.clockPacket(key, now, offset)];
    },
    schedule: (s, dest) => (s = dest === CONNECTED ? s : {...s, address: dest}, ['schedule routine ' + s.routine + (s.offPart ? ' (off-part)' : '') + ': ' + (s.enabled ? (s.offPart ? 'OFF' : s.on ? 'ON' : 'OFF') +
      ' at ' + s.hour + ':' + String(s.minute).padStart(2, '0') + ', weekdays 0x' + s.weekdays.toString(16) +
      (s.offPart ? '' : ', fade ' + s.fade + ' min' + (s.keepLight ? ', light unchanged' : ', ' + s.brightness + ' %, ' + s.mired + ' mired'))
      : 'disabled') + ', parts 0x' + (s.parts ?? 1).toString(16).padStart(2, '0') + ', address 0x' + s.address.toString(16).padStart(4, '0'),
      key => P.schedulePacket(key, s), s.offPart ? 100 : undefined]) // like the app: off-part right after its on-part
  };
  // Coalesce: slider drags replace the pending value, so only the newest is sent.
  // Route for a lamp's next command: own connection, else the mesh (control commands only).
  function route(lamp, kind) {
    if (lamp.session.ready) return {session: lamp.session, dest: CONNECTED, label: lamp.name};
    if (MESH_ROUTABLE.has(kind.split(':')[0]) && lamp.meshOK && MESH.session.ready) {
      return {session: MESH.session, dest: lamp.meshAddress, label: lamp.name + ' via mesh'};
    }
    return null;
  }
  function queue(lamp, kind, value) {
    if (!bootOK) return;
    if (!route(lamp, kind)) {
      // Not connected: keep the command and connect in the background (no picker needed
      // for a known device); it is sent once logged in.
      const meshCapable = MESH_ROUTABLE.has(kind.split(':')[0]) && lamp.meshOK;
      if (lamp.device) { if (!lamp.session.busy) lamp.reconnect(); }
      else if (meshCapable && MESH.device) { if (!MESH.session.busy) MESH.reconnect(); }
      else if (!(meshCapable && MESH.session.busy) && !lamp.session.busy) return;
    }
    // What the page last set, for "Save current as scene" (lamps cannot be read back).
    if (kind === 'power') lamp.state.on = value;
    if (kind === 'temperature') lamp.state.mode = 'white';
    if (kind === 'color') lamp.state.mode = 'color';
    if (MESH_KINDS.has(kind)) lamp.state.touched = true;
    // Power supersedes pending slider moves; the latest colour mode wins.
    if (kind === 'power') for (const k of SLIDER_KINDS) lamp.pending.delete(k);
    if (kind === 'temperature') lamp.pending.delete('color');
    if (kind === 'color') lamp.pending.delete('temperature');
    lamp.pending.delete(kind); // re-insert so it is sent after earlier pending kinds
    lamp.pending.set(kind, value);
    pump(lamp);
  }
  async function pump(lamp) {
    if (!lamp.pending.size) return;
    const [kind, value] = lamp.pending.entries().next().value;
    const r = route(lamp, kind);
    if (!r || r.session.busy) return; // sent when a connection becomes ready or free
    lamp.pending.delete(kind);
    const command = COMMANDS[kind.split(':')[0]](value, r.dest);
    if (!command) { pump(lamp); return; } // e.g. no time zone rule for this zone
    const [description, build, gap] = command;
    lamp.lastActivity = MESH.lastActivity = Date.now();
    try { await r.session.send(description + (r.dest === CONNECTED ? '' : ' to ' + r.label), build, gap); }
    catch (error) { reportError(r.session === MESH.session ? MESH : lamp, error); }
  }
  function pumpAll() { for (const lamp of LAMPS) pump(lamp); }

  function syncAll() {
    const meshUsable = !!(MESH.session?.ready || MESH.device);
    const any = bootOK && LAMPS.some(l => l.session?.ready || l.device || (l.meshOK && meshUsable));
    $('all-on').disabled = $('all-off').disabled = !any;
    $('mesh-name').disabled = $('mesh-password').disabled = [...LAMPS, MESH].some(l => l.session?.busy && !l.session.ready);
    if (MESH.session) {
      const s = MESH.session;
      $('mesh-connect').disabled = $('mesh-any').disabled = !bootOK || s.busy || !!s.device;
      $('mesh-connect').textContent = MESH.device ? 'Pick again' : 'Connect';
      $('mesh-disconnect').disabled = !(s.busy || s.device);
      $('mesh-badge').textContent = s.busy && !s.ready ? 'Connecting…' : s.ready ? 'Connected' : MESH.device ? 'Ready' : 'Not set up';
    }
  }
  // Mesh connection: any lamp of the mesh. The picker filters by name, which is the
  // mesh name on these lamps (a factory-reset lamp advertises "unpaired" instead).
  MESH.session = new globalThis.AwoxTransport.MeshSession({
    log: text => log('Mesh: ' + text), changed: () => { for (const l of LAMPS) l.sync(); syncAll(); pumpAll(); }
  });
  function connectMesh(request, known = null) {
    const creds = readCredentials();
    if (!creds) { for (const l of LAMPS) l.pending.clear(); $('credentials').open = true; return; }
    $('status').textContent = known ? 'Connecting to the mesh…' : 'Choose any lamp in the Bluetooth picker…';
    MESH.session.connect(creds[0], creds[1], request, known)
      .then(() => {
        MESH.device = MESH.session.device; MESH.lastActivity = Date.now();
        storageSet('device:mesh', MESH.device.id);
        rememberCredentials();
        $('status').textContent = 'Mesh connected: lamps with a mesh address are controlled through it.';
        syncAll();
      })
      .catch(error => { for (const l of LAMPS) if (!l.session.ready) l.pending.clear(); reportError(MESH, error); });
  }
  MESH.reconnect = () => { if (MESH.device && !MESH.session.busy && !MESH.session.device) connectMesh(null, MESH.device); };
  $('mesh-connect').addEventListener('click', () => {
    // Advertised name = mesh name, but iOS/macOS often show the cached GATT name
    // (EGLO_ZM_RGB_TW) instead, so accept either; any lamp of the mesh will do.
    const name = $('mesh-name').value;
    connectMesh({filters: [...(name ? [{name}] : []), {namePrefix: 'EGLO'}]});
  });
  $('mesh-any').addEventListener('click', () => connectMesh({acceptAllDevices: true}));
  $('mesh-disconnect').addEventListener('click', () => { MESH.session.disconnect(); log('Mesh: disconnected by user.'); });
  for (const [id, on] of [['all-on', true], ['all-off', false]]) {
    $(id).addEventListener('click', () => { for (const lamp of LAMPS) queue(lamp, 'power', on); });
  }
  // ---- Scenes: named settings for several lamps, kept in this browser; a link carries
  // a whole scene (to another device, or as a home-screen bookmark).
  const SCENES_KEY = 'scenes';
  const inRange = (v, [lo, hi]) => Number.isInteger(v) && v >= lo && v <= hi;
  function validScene(s) {
    // Also used for scenes from links, which are untrusted input.
    if (!s || typeof s.name !== 'string' || !s.name.trim() || s.name.length > 30 || !Array.isArray(s.lamps) || !s.lamps.length || s.lamps.length > 20) return null;
    const lamps = [];
    for (const l of s.lamps) {
      if (!l || !/^[0-9a-f]{12}$/.test(l.id) || typeof l.on !== 'boolean') return null;
      const entry = {id: l.id, name: String(l.name || '').slice(0, 40), on: l.on};
      if (l.on) {
        if (!inRange(l.b, P.RANGE.brightness)) return null;
        entry.b = l.b;
        if (l.c) { if (!Array.isArray(l.c) || !l.c.every(v => inRange(v, P.RANGE.byte))) return null; entry.c = [l.c[0], l.c[1]]; }
        else if (inRange(l.t, P.RANGE.mired)) entry.t = l.t; else return null;
      }
      lamps.push(entry);
    }
    return {name: s.name.trim(), lamps};
  }
  let scenes = (Array.isArray(storageGet(SCENES_KEY)) ? storageGet(SCENES_KEY) : []).map(validScene).filter(Boolean);
  const saveScenes = () => storageSet(SCENES_KEY, scenes);
  function encodeScene(s) {
    const bytes = new TextEncoder().encode(JSON.stringify(s));
    return btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  function decodeScene(text) {
    try {
      const bin = atob(text.replace(/-/g, '+').replace(/_/g, '/'));
      return validScene(JSON.parse(new TextDecoder().decode(Uint8Array.from(bin, ch => ch.charCodeAt(0)))));
    } catch (_) { return null; }
  }
  const sceneURL = s => location.origin + location.pathname + '?scene=' + encodeScene(s);
  const describe = l => !l.on ? 'off' : Math.round((l.b - 2) / 2.52) + ' %, ' +
    (l.c ? 'colour ' + Math.round(l.c[0] * 360 / 254) + '°' : 'white ' + whitePercent(l.t) + ' %');

  function applyScene(scene) {
    const targets = scene.lamps.map(sl => [LAMPS.find(l => l.id === sl.id) || LAMPS.find(l => sl.name && l.name === sl.name), sl])
      .filter(([lamp]) => lamp);
    const unreachable = lamp => !(lamp.session.ready || lamp.device || (lamp.meshOK && (MESH.session.ready || MESH.device || MESH.session.busy)));
    // Nothing connected or known yet: open the mesh picker from this tap, then send.
    if (targets.some(([lamp]) => unreachable(lamp) && lamp.meshOK)) $('mesh-connect').click();
    for (const [lamp, sl] of targets) {
      queue(lamp, 'power', sl.on);
      if (sl.on) { queue(lamp, 'brightness', sl.b); if (sl.c) queue(lamp, 'color', sl.c); else queue(lamp, 'temperature', sl.t); }
      lamp.applyUI(sl);
    }
    const missing = scene.lamps.length - targets.length;
    $('status').textContent = 'Scene “' + scene.name + '” sent' + (missing ? '; ' + missing + ' lamp(s) of it are not in this lamp list.' : '.');
  }

  function renderScenes() {
    const list = $('scene-list');
    list.replaceChildren();
    $('scene-empty').hidden = scenes.length > 0;
    scenes.forEach((scene, index) => {
      const row = document.createElement('div'); row.className = 'scene';
      const apply = document.createElement('button'); apply.className = 'scene-apply'; apply.textContent = scene.name;
      apply.title = scene.lamps.map(l => (l.name || l.id) + ': ' + describe(l)).join('\n');
      apply.addEventListener('click', () => applyScene(scene));
      const link = document.createElement('button'); link.className = 'secondary compact'; link.textContent = 'Link';
      link.addEventListener('click', async () => {
        const url = sceneURL(scene);
        try { if (navigator.share) { await navigator.share({title: scene.name, url}); return; } } catch (_) { /* cancelled: fall back */ }
        try { await navigator.clipboard.writeText(url); link.textContent = 'Copied'; setTimeout(() => { link.textContent = 'Link'; }, 2000); }
        catch (_) { $('scene-url').hidden = false; $('scene-url').value = url; $('scene-url').select(); }
      });
      const del = document.createElement('button'); del.className = 'secondary compact'; del.textContent = 'Delete';
      del.addEventListener('click', () => {
        if (del.textContent !== 'Sure?') { del.textContent = 'Sure?'; setTimeout(() => { del.textContent = 'Delete'; }, 3000); return; }
        scenes.splice(index, 1); saveScenes(); renderScenes();
      });
      row.append(apply, link, del); list.append(row);
    });
  }
  $('scene-new').addEventListener('click', () => {
    const box = $('scene-lamps');
    box.replaceChildren();
    for (const lamp of LAMPS) {
      const label = document.createElement('label'), check = document.createElement('input');
      check.type = 'checkbox'; check.checked = lamp.state.touched; check.dataset.id = lamp.id;
      label.append(check, ' ' + lamp.name + ' · ' + describe(lamp.snapshot()));
      box.append(label);
    }
    $('scene-editor').hidden = false; $('scene-name').focus();
  });
  $('scene-cancel').addEventListener('click', () => { $('scene-editor').hidden = true; });
  $('scene-save').addEventListener('click', () => {
    const ids = [...$('scene-lamps').querySelectorAll('input:checked')].map(c => c.dataset.id);
    const scene = validScene({name: $('scene-name').value, lamps: LAMPS.filter(l => ids.includes(l.id)).map(l => l.snapshot())});
    if (!scene) { $('status').textContent = 'Give the scene a name (up to 30 characters) and include at least one lamp.'; return; }
    scenes = scenes.filter(s => s.name !== scene.name).concat(scene);
    saveScenes(); renderScenes();
    $('scene-editor').hidden = true; $('scene-name').value = '';
    $('status').textContent = 'Scene “' + scene.name + '” saved in this browser.';
  });
  // A scene from a link (?scene=…): offer to apply or keep it.
  const incoming = decodeScene(new URLSearchParams(location.search).get('scene') || '');
  if (incoming) {
    $('incoming-name').textContent = incoming.name; $('scene-incoming').hidden = false;
    $('incoming-apply').addEventListener('click', () => applyScene(incoming));
    $('incoming-save').addEventListener('click', () => {
      scenes = scenes.filter(s => s.name !== incoming.name).concat(incoming); saveScenes(); renderScenes();
      $('status').textContent = 'Scene “' + incoming.name + '” saved in this browser.';
    });
  }

  $('copy').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('log').value); $('copy').textContent = 'Copied'; }
    catch (_) { $('log').focus(); $('log').select(); $('copy').textContent = 'Select & copy'; }
    setTimeout(() => { $('copy').textContent = 'Copy log'; }, 2500);
  });
  // Restored from the back/forward cache after pagehide cleared the fields.
  addEventListener('pageshow', event => { if (event.persisted) loadCredentials(); });
  addEventListener('pagehide', () => {
    for (const lamp of [...LAMPS, MESH]) lamp.session.disconnect();
    $('mesh-name').value = $('mesh-password').value = '';
  });
  // A connection left open (e.g. the Mac sleeping with the tab connected) can
  // wedge the lamp until it is power-cycled, so release connections eagerly.
  // Note: no Bluetooth write may run here; just close the links.
  function releaseAll(reason) {
    let released = false;
    for (const lamp of [...LAMPS, MESH]) {
      if (lamp.session.device) { lamp.session.disconnect(); log(lamp.name + ': disconnected (' + reason + ').'); released = true; }
    }
    if (released) $('status').textContent = 'Disconnected (' + reason + '). Connect again when needed.';
  }
  const connecting = () => [...LAMPS, MESH].some(l => l.session.busy && !l.session.ready);
  document.addEventListener('visibilitychange', () => {
    // The Bluetooth picker can obscure the page on iOS, so never cancel a connect in progress.
    if (document.hidden && !connecting()) releaseAll('page hidden');
  });
  addEventListener('freeze', () => releaseAll('page frozen'));
  const IDLE_MS = 5 * 60 * 1000;
  setInterval(() => {
    for (const lamp of [...LAMPS, MESH]) {
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
        const meshId = storageGet('device:mesh');
        MESH.device = devices.find(d => meshId && d.id === meshId) || null;
        if (MESH.device) log('Mesh: remembered from an earlier visit.');
      } catch (_) { log('getDevices() failed; lamps need the picker once per visit.'); }
    } else if (bootOK) log('This browser has no getDevices(); pick each lamp once per visit.');
    for (const lamp of LAMPS) lamp.sync();
    syncAll();
    $('scene-new').disabled = !bootOK || !LAMPS.length;
    renderScenes();
  })();
  syncAll();
})();
