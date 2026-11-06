/* AwoX non-Telink subset; adapted from awox-mesh, MIT (see LICENSE.txt).
 * Source commit: 3a46539588b96a2dd705e1cee13a238cabce58e7.
 * Only authentication and power to the two addresses read from the lamps' advertisements are exposed.
 */
(function (root) {
  'use strict';
  // Mesh addresses read passively from each lamp's scan-response manufacturer data,
  // plus 0x0000 (classic Telink: the directly connected lamp) and 0xFFFF (broadcast).
  const TARGETS = Object.freeze({standleuchte: 0xC7B4, schlafzimmerschrank: 0xFFFE, connected: 0x0000, all: 0xFFFF});
  const ALLOWED = new Set(Object.values(TARGETS));
  const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
  function pad16(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length > 16) throw new Error('Expected at most 16 bytes');
    const padded = new Uint8Array(16);
    padded.set(bytes);
    return padded;
  }
  function credentials(text) {
    const value = new TextEncoder().encode(text); // No trimming or silent truncation.
    if (!value.length || value.length > 16) throw new Error('Credentials must contain 1–16 UTF-8 bytes');
    return value;
  }
  function combine(name, password) {
    const n = pad16(name), p = pad16(password);
    for (let i = 0; i < 16; i++) n[i] ^= p[i];
    p.fill(0);
    return n;
  }
  async function attEncrypt(key, value) {
    const k = pad16(key).reverse(), v = pad16(value).reverse();
    try {
      // Web Crypto has no ECB. The FIRST CBC block with IV=0 is exactly AES(K,P).
      // Web Crypto adds a full padding block; discard it, then reverse the first.
      // This equivalence is only used for this single-block vendor protocol.
      const imported = await root.crypto.subtle.importKey('raw', k, 'AES-CBC', false, ['encrypt']);
      const result = await root.crypto.subtle.encrypt({name: 'AES-CBC', iv: new Uint8Array(16)}, imported, v);
      return new Uint8Array(result).slice(0, 16).reverse();
    } finally { k.fill(0); v.fill(0); }
  }
  function crc8(data) {
    let crc = 0;
    for (const byte of data) {
      crc ^= byte;
      for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0x6C : 0);
    }
    return crc;
  }
  async function pairPacket(name, password, random) {
    if (random.length !== 8) throw new Error('Expected 8 random bytes');
    const combined = combine(name, password);
    try {
      const packet = new Uint8Array(17);
      packet[0] = 0x0C;
      packet.set(random, 1);
      packet.set((await attEncrypt(random, combined)).slice(0, 8), 9);
      return packet;
    } finally { combined.fill(0); }
  }
  async function sessionKey(name, password, random, response) {
    if (random.length !== 8 || response.length !== 8) throw new Error('Expected 8-byte random values');
    const combined = combine(name, password), value = new Uint8Array(16);
    value.set(random); value.set(response, 8);
    try { return await attEncrypt(combined, value); }
    finally { combined.fill(0); value.fill(0); }
  }
  // Opcodes on the wire, high byte first (upstream OP_* tuples reversed).
  const OP = Object.freeze({power: [6, 0], brightness: [8, 0], color: [0, 3]});
  const RANGE = Object.freeze({brightness: [2, 254], mired: [153, 454], byte: [0, 254], candle: [1, 254]});
  const EPOCH_2000 = 946684800;
  // Schedules carry a mesh address in the body: the lamp's own address
  // (0xC7B4 verified), or 0x0000 where it is unusable (0xFFFE lamp verified).
  const scheduleAddressOK = a => Number.isInteger(a) && a >= 0 && a <= 0xFFFD;
  // Verified 2026-11-05: timer (2) switches off, program (3) switches on with
  // brightness/white, dawn (7) fades up. Presence simulator (1) is excluded.
  const ROUTINE = Object.freeze({timer: 2, program: 3, dawn: 7});
  const SCHEDULE_ROUTINES = new Set(Object.values(ROUTINE));
  function intIn(value, [low, high], what) {
    if (!Number.isInteger(value) || value < low || value > high) throw new Error(what + ' out of range');
    return value;
  }
  function commandBlock(target, opcode, data) {
    if (!ALLOWED.has(target)) throw new Error('Address not in the allowed lamp list');
    // [CRC][length][destination BE][type=1][opcode hi lo][data][zero pad]
    const fields = Uint8Array.of(data.length + 5, target >>> 8, target & 255, 1, ...opcode, ...data);
    if (fields.length > 15) throw new Error('Command too long for one block');
    const block = new Uint8Array(16);
    block[0] = crc8(fields); block.set(fields, 1);
    return block;
  }
  function powerBlock(on, target) {
    if (typeof on !== 'boolean') throw new Error('Power must be a boolean');
    return commandBlock(target, OP.power, [on ? 1 : 0]);
  }
  function brightnessBlock(value, target) {
    // [4][value][transition 02 00][0][0][0], value 2..254
    return commandBlock(target, OP.brightness, [4, intIn(value, RANGE.brightness, 'Brightness'), 2, 0, 0, 0, 0]);
  }
  function temperatureBlock(mired, target) {
    // [10][mired LE][transition 02 00], mired 153 (cold) .. 454 (warm)
    intIn(mired, RANGE.mired, 'Temperature');
    return commandBlock(target, OP.color, [10, mired & 255, mired >>> 8, 2, 0]);
  }
  function colorBlock(hue, saturation, target) {
    // [6][hue 0..254][saturation 0..254][transition 02 00]
    return commandBlock(target, OP.color, [6, intIn(hue, RANGE.byte, 'Hue'), intIn(saturation, RANGE.byte, 'Saturation'), 2, 0]);
  }
  function candleBlock(level, target) {
    // [0x10][level][1]; shares the brightness opcode. Upstream only ever sends level 1.
    return commandBlock(target, OP.brightness, [0x10, intIn(level, RANGE.candle, 'Candle level'), 1]);
  }
  // Weekday mask bit 0 = Sunday (verified on hardware), same as Date.getDay().
  // Query frames (wire flag 0x02) have no destination header: [CRC][length][payload][zero pad].
  function queryBlock(payload) {
    const fields = Uint8Array.of(payload.length, ...payload);
    if (fields.length > 15) throw new Error('Query too long for one block');
    const block = new Uint8Array(16);
    block[0] = crc8(fields); block.set(fields, 1);
    return block;
  }
  const le32 = v => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  function clockBlock(unixSeconds, utcOffsetSeconds) {
    // [0x30][seconds since 2000-01-01 UTC, LE32][UTC offset seconds, LE32 two's complement]
    const since2000 = intIn(unixSeconds - EPOCH_2000, [0, 0xFFFFFFFF], 'Clock');
    intIn(utcOffsetSeconds, [-14 * 3600, 14 * 3600], 'UTC offset');
    return queryBlock([0x30, ...le32(since2000), ...le32(utcOffsetSeconds >>> 0)]);
  }
  function scheduleBlock(s) {
    // 12-byte schedule body after opcode 0x33; layout in upstream docs/protocol.md.
    if (!scheduleAddressOK(s.address)) throw new Error('Schedule address not allowed');
    if (!SCHEDULE_ROUTINES.has(s.routine)) throw new Error('Routine not enabled in this build');
    const body = [
      s.address & 255, s.address >>> 8,
      s.routine | (s.enabled ? 0x80 : 0),
      1 | (intIn(s.minute, [0, 59], 'Minute') << 2),
      intIn(s.hour, [0, 23], 'Hour'),
      intIn(s.weekdays, [1, 0x7F], 'Weekdays') | 0x80, // recurring
      intIn(s.fade, [0, 63], 'Fade'),
      // Lighting mode: white temperature (0), or with keepLight "neither" (2) = leave brightness/colour unchanged.
      (s.on ? 1 : 0) | (s.keepLight ? 1 << 2 : 0),
      s.keepLight ? 0xFF : intIn(s.brightness, [0, 100], 'Schedule brightness'),
      ...(s.keepLight ? [0xFF, 0xFF] : [intIn(s.mired, RANGE.mired, 'Temperature')].flatMap(m => [m & 255, m >>> 8])),
      1 // single schedule: current=1, total=0 (total=1 is silently dropped)
    ];
    return queryBlock([0x33, ...body]);
  }
  async function packet(key, block, flag = 0) {
    if (key.length !== 16) throw new Error('Expected 16-byte session key');
    try {
      const out = new Uint8Array(17); // command flag 0x00: no allowed address has high byte 0x80
      out[0] = flag;
      out.set(await attEncrypt(key, block), 1);
      return out;
    } finally { block.fill(0); }
  }
  const powerPacket = (key, on, target) => packet(key, powerBlock(on, target));
  const brightnessPacket = (key, value, target) => packet(key, brightnessBlock(value, target));
  const temperaturePacket = (key, mired, target) => packet(key, temperatureBlock(mired, target));
  const colorPacket = (key, hue, saturation, target) => packet(key, colorBlock(hue, saturation, target));
  const candlePacket = (key, level, target) => packet(key, candleBlock(level, target));
  const clockPacket = (key, unixSeconds, utcOffsetSeconds) => packet(key, clockBlock(unixSeconds, utcOffsetSeconds), 2);
  const schedulePacket = (key, schedule) => packet(key, scheduleBlock(schedule), 2);
  async function selfTest() {
    // Public dummy credentials only. Generated independently by upstream Python.
    const n = credentials('meshname'), p = credentials('meshpass');
    const r = Uint8Array.from([0,1,2,3,4,5,6,7]), s = Uint8Array.from([8,9,10,11,12,13,14,15]);
    const pair = await pairPacket(n,p,r), key = await sessionKey(n,p,r,s);
    // Expected values generated by upstream Python (awox_mesh.protocol).
    const expect = [
      [powerPacket(key,true,0xC7B4), '00665bcbf60bf2f3bdc1fcc25cc02694d5'],
      [powerPacket(key,false,0xC7B4), '002b8370754a97bd6d237ae8ac7ca1025f'],
      [powerPacket(key,true,0x0000), '0007d56316f5a019812579b222244cecbe'],
      [powerPacket(key,false,0x0000), '00fae8d7a2ce2b9c4a871b43d416491741'],
      [powerPacket(key,true,0xFFFF), '0053b9cd569a2d118cf0383637ec7fd4e3'],
      [brightnessPacket(key,2,0x0000), '00a924c0808a65a525321a7b65393c4647'],
      [brightnessPacket(key,128,0x0000), '00e4ff57e7faa2289f3c7f3659c92a1158'],
      [brightnessPacket(key,254,0x0000), '00085f6889dde08618db891fed09853baa'],
      [brightnessPacket(key,128,0xC7B4), '0006e2606bc6aafa1241690df377715202'],
      [temperaturePacket(key,153,0x0000), '00dbecc3f7b654ee875bd9a600b37f9435'],
      [temperaturePacket(key,300,0x0000), '0048532982640b23181c7a8d3ac98f8008'],
      [temperaturePacket(key,454,0x0000), '00439645e05deb8e6478a46b53f67552d5'],
      [temperaturePacket(key,300,0xC7B4), '00ae2df0c4b33896e62754b6b91afae7f7'],
      [colorPacket(key,0,254,0x0000), '00d64b2eba0099284596911251b907a394'],
      [colorPacket(key,127,200,0x0000), '00a5951a8483bed18aa724701c28357535'],
      [candlePacket(key,1,0x0000), '0000edf43732ad9292bc01bcc21771b934'],
      [candlePacket(key,50,0x0000), '00b11b62fa988cc40cc1cb978083c49ef0'],
      [clockPacket(key,EPOCH_2000 + 838000000,3600), '027e3bb6e32f19d3b13522774521ba94d3'],
      [clockPacket(key,EPOCH_2000 + 838000000,-18000), '02a1a06c418c95ddc63dbd52e7088d029f'],
      ...[[0xC7B4, '021a0f783083c6e49ec84a06249bd775bf'], [0x0000, '025b63e84d3d42f0cca0b423f640e550fa']]
        .map(([address, expected]) => [schedulePacket(key, {address, routine: 7, enabled: true, hour: 7, minute: 30,
          weekdays: 0x7F, fade: 45, on: true, brightness: 90, mired: 153}), expected]),
      [schedulePacket(key, {address: 0xC7B4, routine: 7, enabled: false, hour: 23, minute: 59,
        weekdays: 0x1F, fade: 1, on: true, brightness: 100, mired: 454}), '0295a7e5e94852f27768b38adb05ecf429'],
      [schedulePacket(key, {address: 0xC7B4, routine: 2, enabled: true, hour: 22, minute: 15,
        weekdays: 0x7F, fade: 1, on: false, keepLight: true}), '02357ea502a0a208fd578b0f397605b4de'],
      [schedulePacket(key, {address: 0xC7B4, routine: 3, enabled: true, hour: 19, minute: 0,
        weekdays: 0x7F, fade: 1, on: true, brightness: 30, mired: 370}), '025a087de12359e40426e87fdc51db76a0'],
      [schedulePacket(key, {address: 0xC7B4, routine: 7, enabled: true, hour: 7, minute: 0,
        weekdays: 0x08, fade: 1, on: true, brightness: 100, mired: 153}), '029c80f0687a180b933c6aac6e1c4d8b9f']
    ];
    const results = await Promise.all(expect.map(([promise]) => promise));
    if (hex(pair) !== '0c00010203040506077f4e2d01cbd9af89' ||
        hex(key) !== '03a53246e89ea73e0a5d38ae66d370d6' ||
        results.some((bytes, i) => hex(bytes) !== expect[i][1])) {
      throw new Error('Cryptography self-test failed');
    }
    key.fill(0);
    return true;
  }
  const api = {TARGETS, RANGE, ROUTINE, hex, credentials, attEncrypt, crc8, pairPacket, sessionKey, powerBlock, brightnessBlock, temperatureBlock,
    colorBlock, candleBlock, queryBlock, clockBlock, scheduleBlock,
    powerPacket, brightnessPacket, temperaturePacket, colorPacket, candlePacket, clockPacket, schedulePacket, selfTest};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AwoxProtocol = api;
})(globalThis);
