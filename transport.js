(function (root) {
  'use strict';
  const P = typeof module !== 'undefined' && module.exports ? require('./protocol.js') : root.AwoxProtocol;
  const UUID = Object.freeze({
    service: '00010203-0405-0607-0809-0a0b0c0d1910',
    pair: '00010203-0405-0607-0809-0a0b0c0d1914',
    command: '00010203-0405-0607-0809-0a0b0c0d1912'
  });
  const fail = (code, message) => Object.assign(new Error(message), {safeCode: code});
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  const MIN_GAP = 350;
  function writer(characteristic) {
    const props = characteristic.properties;
    if (props.write && typeof characteristic.writeValueWithResponse === 'function') {
      return {mode: 'with response', send: bytes => characteristic.writeValueWithResponse(bytes)};
    }
    if (props.write && typeof characteristic.writeValue === 'function') {
      return {mode: 'legacy writeValue', send: bytes => characteristic.writeValue(bytes)};
    }
    if (props.writeWithoutResponse && typeof characteristic.writeValueWithoutResponse === 'function') {
      return {mode: 'without response', send: bytes => characteristic.writeValueWithoutResponse(bytes)};
    }
    if (props.writeWithoutResponse && typeof characteristic.writeValue === 'function') {
      return {mode: 'legacy writeValue (no response property)', send: bytes => characteristic.writeValue(bytes)};
    }
    throw fail('WRITE_UNAVAILABLE', 'No supported write method on this characteristic.');
  }
  class MeshSession {
    constructor({bluetooth = root.navigator?.bluetooth, log = () => {}, changed = () => {}, timeout = 20000, pause = delay} = {}) {
      Object.assign(this, {bluetooth, log, changed, timeout, pause});
      this.generation = 0; this.device = null; this.key = null; this.command = null;
      this.busy = false; this.ready = false; this.lastWrite = -Infinity;
    }
    assertCurrent(generation) {
      if (generation !== this.generation) throw fail('CANCELLED', 'Session ended. Connect again to continue.');
    }
    async bounded(operation, generation, stage) {
      let timer;
      try {
        const result = await Promise.race([
          operation,
          new Promise((_, reject) => { timer = setTimeout(() => reject(fail('TIMEOUT', stage + ' timed out. Disconnect and reconnect.')), this.timeout); })
        ]);
        this.assertCurrent(generation);
        return result;
      } finally { clearTimeout(timer); }
    }
    disconnect() {
      ++this.generation;
      const device = this.device;
      this.device = null; this.ready = false; this.busy = false; this.command = null;
      if (this.key) this.key.fill(0);
      this.key = null;
      if (device && this.disconnectListener) device.removeEventListener('gattserverdisconnected', this.disconnectListener);
      try { if (device?.gatt?.connected) device.gatt.disconnect(); } catch (_) { /* best effort */ }
      this.changed();
    }
    // knownDevice: a BluetoothDevice picked earlier (or from getDevices()); reconnects without the picker.
    async connect(name, password, request = {acceptAllDevices: true}, knownDevice = null) {
      if (this.busy || this.device) throw fail('BUSY', 'Disconnect the current session first.');
      if (!this.bluetooth) throw fail('NO_BLUETOOTH', 'Web Bluetooth is unavailable. Use Chrome (localhost/HTTPS) or Bluefy.');
      this.busy = true; this.changed();
      const generation = ++this.generation;
      let random;
      try {
        // requestDevice runs directly from the click handler, before any await.
        // Names are identical across lamps; callers may filter by advertisement data instead.
        const device = knownDevice || await this.bluetooth.requestDevice({...request, optionalServices: [UUID.service]});
        this.assertCurrent(generation);
        this.device = device;
        this.disconnectListener = () => {
          if (this.reconnecting) return; // intentional drop before the 0x0E retry
          this.log('BLE disconnected; session key cleared.'); this.disconnect();
        };
        device.addEventListener('gattserverdisconnected', this.disconnectListener);
        this.log((knownDevice ? 'Reconnecting to the known lamp.' : 'Device selected. Connecting to GATT.') + ' Device name/ID omitted from diagnostics.');
        if (!device.gatt) throw fail('NO_GATT', 'Selected device has no GATT server.');
        let commandWriter, response;
        // The chip alternates accept/reject (upstream hardware notes): a 0x0E right
        // after an accepted session is not a credentials problem. Like upstream,
        // drop the link, wait 5 s and try exactly once more with a fresh random.
        for (let round = 1; ; round++) {
          const connecting = device.gatt.connect();
          // If the browser resolves connect after cancellation/timeout, release it.
          connecting.then(() => { if (generation !== this.generation && device.gatt.connected) device.gatt.disconnect(); }, () => {});
          const server = await this.bounded(connecting, generation, 'GATT connection');
          const service = await this.bounded(server.getPrimaryService(UUID.service), generation, 'Service lookup');
          this.log('Mesh service …1910 found.');
          const pair = await this.bounded(service.getCharacteristic(UUID.pair), generation, 'Authentication characteristic lookup');
          const command = await this.bounded(service.getCharacteristic(UUID.command), generation, 'Command characteristic lookup');
          if (!pair.properties.read) throw fail('PAIR_NOT_READABLE', 'Authentication characteristic is not readable.');
          const pairWriter = writer(pair);
          commandWriter = writer(command);
          if (random) random.fill(0);
          random = root.crypto.getRandomValues(new Uint8Array(8));
          const packet = await P.pairPacket(name, password, random);
          this.assertCurrent(generation);
          this.log('Authentication round ' + round + ': writing 17 bytes, ' + pairWriter.mode + '. Payload hidden.');
          await this.bounded(pairWriter.send(packet), generation, 'Authentication write');
          let rejected = false;
          response = null;
          for (let attempt = 1; attempt <= 15; attempt++) {
            await this.pause(400); this.assertCurrent(generation);
            const view = await this.bounded(pair.readValue(), generation, 'Authentication read');
            const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
            const kind = bytes.length ? '0x' + bytes[0].toString(16).padStart(2,'0') : 'empty';
            this.log('Authentication reply ' + attempt + ': type=' + kind + ', length=' + bytes.length + '. Payload hidden.');
            if (bytes[0] === 0x0E) { rejected = true; break; }
            if (bytes[0] === 0x0D && bytes.length >= 9) { response = bytes.slice(1,9); break; }
          }
          if (response) break;
          if (!rejected) throw fail('AUTH_NO_REPLY', 'No authentication reply after 15 reads. The chip may be stuck: switch it off at the wall for 10 s.');
          if (round >= 2) throw fail('AUTH_REJECTED', 'Lamp rejected authentication twice (0x0E). Recheck the mesh credentials.');
          this.log('Rejected (0x0E), likely the lamp\'s alternating mode. Reconnecting in 5 s for one retry.');
          this.reconnecting = true;
          try { device.gatt.disconnect(); } catch (_) { /* best effort */ }
          await this.pause(5000);
          this.reconnecting = false;
          this.assertCurrent(generation);
        }
        const key = await P.sessionKey(name, password, random, response);
        if (generation !== this.generation) { key.fill(0); this.assertCurrent(generation); }
        if (!device.gatt.connected) { key.fill(0); throw fail('DISCONNECTED', 'Bluetooth disconnected during authentication.'); }
        this.key = key; this.command = commandWriter; this.ready = true;
        this.log('Authentication accepted (0x0D). No power command sent. Write mode: ' + commandWriter.mode + '.');
      } catch (error) {
        if (generation === this.generation) this.disconnect();
        throw error;
      } finally {
        this.reconnecting = false;
        name.fill(0); password.fill(0); if (random) random.fill(0);
        if (generation === this.generation) { this.busy = false; this.changed(); }
      }
    }
    // build(key) returns the 17-byte packet. Commands closer than MIN_GAP are
    // swallowed by the lamp (upstream hardware notes), so wait instead.
    // gap: minimum ms since the previous write (the app writes schedule pairs ~0.1 s apart).
    async send(description, build, gap = MIN_GAP) {
      if (this.busy) throw fail('BUSY', 'A Bluetooth operation is already running.');
      if (!this.ready || !this.device?.gatt?.connected || !this.key) throw fail('NOT_AUTHENTICATED', 'Connect and authenticate first.');
      this.busy = true; this.changed();
      const generation = this.generation;
      try {
        const wait = gap - (Date.now() - this.lastWrite);
        if (wait > 0) { await this.pause(wait); this.assertCurrent(generation); }
        const packet = await build(this.key);
        this.assertCurrent(generation);
        this.lastWrite = Date.now();
        this.log('Sending ' + description + ': ' + packet.length + ' bytes, ' + this.command.mode + '.');
        await this.bounded(this.command.send(packet), generation, 'Command write');
      } catch (error) {
        // A failed/timeout write may already have reached the lamp. Never retry it.
        if (generation === this.generation) this.disconnect();
        throw error;
      } finally {
        if (generation === this.generation) { this.busy = false; this.changed(); }
      }
    }
  }
  const api = {MeshSession, UUID, writer};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AwoxTransport = api;
})(globalThis);
