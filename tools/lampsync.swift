// lampsync: read what the lamps really hold and write it as a Lamp Control settings file.
//
//   swift tools/lampsync.swift [--out lampcontrol-settings.json] [--lamps lamps.json]
//
// Connects to any lamp of the mesh, logs in with credentials typed at runtime (never stored or
// printed), and reads every lamp's schedule slots through the mesh. Only read queries are sent.
// The output uses the web page's settings-file format ("lampcontrol/1"): import it on any device
// (Settings file → Import), adjust if needed, then "Save all to lamp". Scenes are not included,
// so importing this file leaves the page's scenes untouched.
import CoreBluetooth
import CommonCrypto
import Foundation

// ---- Protocol (same as the web page and tools/readback.swift) --------------------------------
let serviceUUID = CBUUID(string: "00010203-0405-0607-0809-0a0b0c0d1910")
let statusUUID = CBUUID(string: "00010203-0405-0607-0809-0a0b0c0d1911")
let commandUUID = CBUUID(string: "00010203-0405-0607-0809-0a0b0c0d1912")
let pairUUID = CBUUID(string: "00010203-0405-0607-0809-0a0b0c0d1914")

func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined() }
func pad16(_ b: [UInt8]) -> [UInt8] { Array((b + [UInt8](repeating: 0, count: 16)).prefix(16)) }
func aes(_ key: [UInt8], _ value: [UInt8], encrypt: Bool) -> [UInt8] {
    let k = Array(pad16(key).reversed()), v = Array(pad16(value).reversed())
    var out = [UInt8](repeating: 0, count: 16 + kCCBlockSizeAES128), moved = 0
    let status = CCCrypt(CCOperation(encrypt ? kCCEncrypt : kCCDecrypt), CCAlgorithm(kCCAlgorithmAES128),
                         CCOptions(kCCOptionECBMode), k, 16, nil, v, 16, &out, out.count, &moved)
    precondition(status == kCCSuccess && moved == 16, "AES failed")
    return Array(out.prefix(16).reversed())
}
func crc8(_ data: [UInt8]) -> UInt8 {
    var crc: UInt8 = 0
    for byte in data { crc ^= byte; for _ in 0..<8 { crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x6C : crc >> 1 } }
    return crc
}
func combine(_ name: [UInt8], _ pass: [UInt8]) -> [UInt8] { zip(pad16(name), pad16(pass)).map { $0 ^ $1 } }
func pairPacket(_ combined: [UInt8], _ random: [UInt8]) -> [UInt8] { [0x0C] + random + Array(aes(random, combined, encrypt: true).prefix(8)) }
func queryPacket(_ key: [UInt8], _ payload: [UInt8]) -> [UInt8] {
    let fields = [UInt8(payload.count)] + payload
    return [0x02] + aes(key, [crc8(fields)] + fields, encrypt: true)
}
func selfTest() -> Bool {
    let c = combine(Array("meshname".utf8), Array("meshpass".utf8)), r: [UInt8] = Array(0...7), s: [UInt8] = Array(8...15)
    let key = aes(c, r + s, encrypt: true)
    return hex(pairPacket(c, r)) == "0c00010203040506077f4e2d01cbd9af89" && hex(key) == "03a53246e89ea73e0a5d38ae66d370d6"
        && hex(queryPacket(key, [0x0A, 0xFF, 0xFF])) == "02278f3e9880859d7ce4183806fe6aa70d"
}

// ---- Settings file (format "lampcontrol/1", as exported by the web page) ---------------------
struct Colour: Codable { let hue: Int; let saturation: Int }
struct Wakeup: Codable { let active: Bool; let time: String; let days: String; let fade: Int; let brightness: Int; let white: Int; let off: String? }
struct TimeEntry: Codable {
    let time: String; let days: String; let action: String
    let brightness: Int?; let white: Int?; let colour: Colour?; let fade: Int?; let active: Bool?
}
struct LampOut: Codable { let name: String; let id: String; let wakeup: Wakeup?; let times: [TimeEntry] }
struct SettingsFile: Codable { let format: String; let exported: String; let source: String; let lamps: [LampOut] }

// Ordered JSON (JSONEncoder does not keep field order), matching the web page's export layout.
indirect enum J { case s(String), i(Int), b(Bool), null, a([J]), o([(String, J)]) }
func render(_ v: J, _ indent: String = "") -> String {
    let inner = indent + "  "
    switch v {
    case .s(let s):
        let data = try! JSONSerialization.data(withJSONObject: [s], options: [.withoutEscapingSlashes])
        return String(String(data: data, encoding: .utf8)!.dropFirst().dropLast())
    case .i(let i): return String(i)
    case .b(let b): return b ? "true" : "false"
    case .null: return "null"
    case .a(let items): return items.isEmpty ? "[]" : "[\n" + items.map { inner + render($0, inner) }.joined(separator: ",\n") + "\n" + indent + "]"
    case .o(let pairs): return "{\n" + pairs.map { inner + render(.s($0.0)) + ": " + render($0.1, inner) }.joined(separator: ",\n") + "\n" + indent + "}"
    }
}
func json(_ e: TimeEntry) -> J {
    var p: [(String, J)] = [("time", .s(e.time)), ("days", .s(e.days)), ("action", .s(e.action))]
    if let b = e.brightness { p.append(("brightness", .i(b))) }
    if let c = e.colour { p.append(("colour", .o([("hue", .i(c.hue)), ("saturation", .i(c.saturation))]))) }
    if let w = e.white { p.append(("white", .i(w))) }
    if let f = e.fade { p.append(("fade", .i(f))) }
    if let a = e.active { p.append(("active", .b(a))) }
    return .o(p)
}
func json(_ w: Wakeup?) -> J {
    guard let w = w else { return .null }
    return .o([("active", .b(w.active)), ("time", .s(w.time)), ("days", .s(w.days)), ("fade", .i(w.fade)),
               ("brightness", .i(w.brightness)), ("white", .i(w.white)), ("off", w.off.map { .s($0) } ?? .null)])
}
func json(_ f: SettingsFile) -> J {
    .o([("format", .s(f.format)), ("exported", .s(f.exported)), ("source", .s(f.source)),
        ("lamps", .a(f.lamps.map { .o([("name", .s($0.name)), ("id", .s($0.id)), ("wakeup", json($0.wakeup)), ("times", .a($0.times.map(json)))]) }))])
}

let weekdayLabels: [(String, Int)] = [("Mo", 1), ("Tu", 2), ("We", 3), ("Th", 4), ("Fr", 5), ("Sa", 6), ("Su", 0)]
func daysText(_ mask: UInt8) -> String {
    if mask == 0x7F { return "daily" }
    if mask == 0x3E { return "Mo-Fr" }
    if mask == 0x41 { return "Sa-Su" }
    return weekdayLabels.filter { mask & (1 << $0.1) != 0 }.map { $0.0 }.joined(separator: ", ")
}
func kelvin(_ mired: Int) -> Int { mired > 0 ? Int((1e6 / Double(mired) / 50).rounded()) * 50 : 4000 }

/// One decoded schedule slot: body bytes [addr lo, addr hi, routine|enabled, minute<<2|1, hour, days|repeat, fade, mode, brightness, c1, c2, parts].
struct Slot {
    let b: [UInt8]
    var routine: Int { Int(b[2] & 0x7F) }
    var enabled: Bool { b[2] & 0x80 != 0 }
    var time: String { String(format: "%02d:%02d", Int(b[4] & 0x1F), Int(b[3] >> 2)) }
    var days: UInt8 { b[5] & 0x7F == 0 ? 0x7F : b[5] & 0x7F }
    var on: Bool { b[7] & 1 != 0 }
    /// Written by the page as "empty": disabled, 00:00, off, light unchanged.
    var cleared: Bool { !enabled && b[3] >> 2 == 0 && b[4] & 0x1F == 0 && !on }
    var entry: TimeEntry {
        if !on { return TimeEntry(time: time, days: daysText(days), action: "off", brightness: nil, white: nil, colour: nil, fade: nil, active: enabled ? nil : false) }
        let colour = b[7] & 2 != 0 ? Colour(hue: Int((Double(b[9]) * 360 / 254).rounded()), saturation: Int((Double(b[10]) / 2.54).rounded())) : nil
        return TimeEntry(time: time, days: daysText(days), action: "on", brightness: max(1, min(100, Int(b[8]))),
                         white: colour == nil ? kelvin(Int(b[9]) | Int(b[10]) << 8) : nil, colour: colour, fade: Int(b[6] & 0x3F),
                         active: enabled ? nil : false)
    }
}

struct LampInfo { let name: String; let id: String; let address: Int }
func loadLamps(_ path: String) -> [LampInfo] {
    guard let data = FileManager.default.contents(atPath: path),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let lamps = json["lamps"] as? [[String: Any]] else { print("Cannot read \(path)."); exit(1) }
    return lamps.compactMap { l in
        guard let name = l["name"] as? String, let id = l["id"] as? String, let a = l["meshAddress"] as? String,
              let address = Int(a.replacingOccurrences(of: "0x", with: ""), radix: 16) else { return nil }
        return LampInfo(name: name, id: id.lowercased(), address: address)
    }
}

// ---- Bluetooth session -------------------------------------------------------------------------
let readSlots = [2, 3, 4, 5, 6, 7, 8] + Array(9...16)
/// IDs written by the slot-limit experiment (never fired, maybe still stored); read once and reported.
let leftoverSlots = [32, 64, 127]

final class Sync: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var peripheral: CBPeripheral?
    var connectedId = ""
    var chars = [CBUUID: CBCharacteristic]()
    let lamps: [LampInfo], combined: [UInt8], outPath: String
    var random = [UInt8](repeating: 0, count: 8), key: [UInt8]? = nil
    var queue: [[UInt8]] = [], slots = [Int: [Int: Slot]](), power = [Int: Bool](), round = 0

    init(lamps: [LampInfo], combined: [UInt8], outPath: String) {
        self.lamps = lamps; self.combined = combined; self.outPath = outPath; super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn { print("Scanning for a lamp of the mesh…"); c.scanForPeripherals(withServices: nil) }
        else if c.state == .unauthorized { print("Bluetooth access denied for this terminal app."); exit(1) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        guard peripheral == nil, let m = ad[CBAdvertisementDataManufacturerDataKey] as? Data, m.count >= 8 else { return }
        let b = [UInt8](m), id = hex(Array(b[2..<8]))
        guard b[0] == 0x60, b[1] == 0x01, lamps.contains(where: { $0.id == id }) else { return }
        connectedId = id
        print("Connecting to \(lamps.first { $0.id == id }!.name) (RSSI \(rssi))…")
        peripheral = p; p.delegate = self; c.stopScan(); c.connect(p)
    }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) { p.discoverServices([serviceUUID]) }
    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) { print("Connect failed."); exit(1) }
    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        if key != nil { print("Disconnected early; writing what was read.") ; write(); }
        exit(key == nil ? 1 : 0)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == serviceUUID }) else { print("Mesh service not found."); exit(1) }
        p.discoverCharacteristics([statusUUID, commandUUID, pairUUID], for: s)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for ch in s.characteristics ?? [] { chars[ch.uuid] = ch }
        guard let status = chars[statusUUID], let pair = chars[pairUUID], chars[commandUUID] != nil else { print("Characteristics missing."); exit(1) }
        p.setNotifyValue(true, for: status) // fails (no CCCD), but macOS delivers the notifications anyway
        _ = SecRandomCopyBytes(kSecRandomDefault, 8, &random)
        p.writeValue(Data(pairPacket(combined, random)), for: pair, type: .withResponse)
    }
    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {}
    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if ch.uuid == pairUUID { DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { p.readValue(for: ch) } }
        else { sendNext(p) }
    }
    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        let v = [UInt8](ch.value ?? Data())
        if ch.uuid == pairUUID {
            guard v.first == 0x0D, v.count >= 9 else { print("Login rejected (check the credentials, close the AwoX app)."); exit(1) }
            key = aes(combined, random + Array(v[1...8]), encrypt: true)
            print("Logged in. Reading \(lamps.count) lamp(s), slots \(readSlots.map(String.init).joined(separator: ","))…")
            requestMissing()
        } else if ch.uuid == statusUUID, let key = key, v.count >= 17 {
            let b = aes(key, Array(v[1...16]), encrypt: false), from = Int(b[0]) | Int(b[1]) << 8
            if v[0] == 0x35 { let s = Slot(b: b); if readSlots.contains(s.routine) || leftoverSlots.contains(s.routine) { slots[from, default: [:]][s.routine] = s } }
            else if v[0] == 0x80 || v[0] == 0x82 || v[0] == 0x01 { power[from] = b[12] & 1 == 1 }
        }
    }
    /// The connected lamp answers reads addressed to 0x0000; the others need their mesh address.
    func address(_ lamp: LampInfo) -> Int { lamp.id == connectedId ? 0 : lamp.address }
    func requestMissing() {
        round += 1
        queue = [[0x0A, 0xFF, 0xFF]]
        for lamp in lamps where lamp.address != 0 || lamp.id == connectedId {
            for r in readSlots + (round == 1 ? leftoverSlots : []) where slots[lamp.address]?[r] == nil {
                let a = address(lamp); queue.append([0x35, UInt8(a & 255), UInt8(a >> 8), UInt8(r)])
            }
        }
        let wait = Double(queue.count) * 0.4 + 3
        if let p = peripheral { sendNext(p) }
        DispatchQueue.main.asyncAfter(deadline: .now() + wait) {
            let missing = self.lamps.reduce(0) { n, l in n + readSlots.filter { self.slots[l.address]?[$0] == nil }.count }
            if missing > 0 && self.round < 3 { print("  \(missing) slot(s) not answered yet, asking again…"); self.requestMissing() }
            else { self.write(); if let p = self.peripheral { self.central.cancelPeripheralConnection(p) }; exit(0) }
        }
    }
    func sendNext(_ p: CBPeripheral) {
        guard let key = key, let cmd = chars[commandUUID], !queue.isEmpty else { return }
        let q = queue.removeFirst()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { p.writeValue(Data(queryPacket(key, q)), for: cmd, type: .withResponse) }
    }

    func write() {
        var out = [LampOut]()
        for lamp in lamps {
            let s = slots[lamp.address] ?? [:]
            let state = power[lamp.address].map { $0 ? "on" : "off" } ?? "unknown"
            print("\(lamp.name): \(readSlots.filter { s[$0] != nil }.count)/\(readSlots.count) slots read, currently \(state)")
            for r in leftoverSlots { if let x = s[r], !x.cleared { print("  WARNING: experiment slot \(r) still holds \(x.enabled ? "an ACTIVE" : "an inactive") \(x.entry.action) at \(x.time) \(daysText(x.days))") } }
            var wake: Wakeup? = nil
            if let w = s[7], !w.cleared {
                let off = s[8].flatMap { $0.enabled ? $0.time : nil }
                wake = Wakeup(active: w.enabled, time: w.time, days: daysText(w.days), fade: Int(w.b[6] & 0x3F),
                              brightness: max(1, min(100, Int(w.b[8]))), white: kelvin(Int(w.b[9]) | Int(w.b[10]) << 8), off: off)
            }
            // Everything else becomes switching times, sorted by time (nightlight 5/6 from the AwoX app included).
            let times = [2, 3, 4, 5, 6] + Array(9...16)
            let entries = times.compactMap { s[$0] }.filter { !$0.cleared }.sorted { $0.time < $1.time }.map { $0.entry }
            for e in entries { print("  \(e.time) \(e.days) \(e.action)" + (e.brightness.map { " \($0) %" } ?? "") + (e.active == false ? " (inactive)" : "")) }
            if let w = wake { print("  wake-up \(w.time) \(w.days), fade \(w.fade) min" + (w.off.map { ", off \($0)" } ?? "") + (w.active ? "" : " (inactive)")) }
            out.append(LampOut(name: lamp.name, id: lamp.id, wakeup: wake, times: entries))
        }
        let iso = ISO8601DateFormatter().string(from: Date())
        let file = SettingsFile(format: "lampcontrol/1", exported: iso, source: "read from the lamps by tools/lampsync.swift", lamps: out)
        try! (render(json(file)) + "\n").write(toFile: outPath, atomically: true, encoding: .utf8)
        print("Wrote \(outPath). Import it in the page (Settings file → Import), then “Save all to lamp” after any edits.")
    }
}

// ---- Main ----------------------------------------------------------------------------------------
var args = Array(CommandLine.arguments.dropFirst())
func option(_ name: String, _ fallback: String) -> String {
    if let i = args.firstIndex(of: name), i + 1 < args.count { return args[i + 1] }
    return fallback
}
if args.first == "--selftest" { print(selfTest() ? "selftest PASS" : "selftest FAIL"); exit(selfTest() ? 0 : 1) }
guard selfTest() else { print("Crypto self-test failed; not connecting."); exit(1) }
let lamps = loadLamps(option("--lamps", "lamps.json"))
let outPath = option("--out", "lampcontrol-settings.json")
print("Mesh name: ", terminator: ""); let name = Array((readLine() ?? "").utf8)
let pass = Array(String(cString: getpass("Mesh password (hidden): ")).utf8)
guard (1...16).contains(name.count), (1...16).contains(pass.count) else { print("Credentials must be 1-16 bytes."); exit(1) }
let sync = Sync(lamps: lamps, combined: combine(name, pass), outPath: outPath)
RunLoop.main.run(until: Date().addingTimeInterval(180))
print("Timeout."); sync.write(); exit(1)
