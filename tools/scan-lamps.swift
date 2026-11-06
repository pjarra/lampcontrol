// Discover AwoX/EGLO lamps by their advertisements and merge them into lamps.json.
//
//   swift tools/scan-lamps.swift [seconds=60] [path=lamps.json]
//
// Passive scan only: no connection, no writes to any lamp. Lamps advertise only
// about every 25 s, so keep the default duration. Known lamps keep their names;
// for new ones you are asked for a name (switch on only the new lamp if unsure).
// Entering an existing name replaces that lamp's ID, e.g. after re-provisioning.
import CoreBluetooth
import Foundation

let companyAwoX: UInt16 = 0x0160
let args = CommandLine.arguments
let seconds = args.count > 1 ? Double(args[1]) ?? 60 : 60
let path = args.count > 2 ? args[2] : "lamps.json"

struct Seen { var id: String; var address: Int?; var rssi: Int }

final class Scanner: NSObject, CBCentralManagerDelegate {
    var central: CBCentralManager!
    var seen = [String: Seen]()
    override init() { super.init(); central = CBCentralManager(delegate: self, queue: nil) }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        } else if c.state == .unauthorized {
            print("Bluetooth access denied: allow your terminal app in System Settings → Privacy & Security → Bluetooth."); exit(1)
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        guard let m = ad[CBAdvertisementDataManufacturerDataKey] as? Data, m.count >= 8 else { return }
        let bytes = [UInt8](m)
        guard UInt16(bytes[0]) | UInt16(bytes[1]) << 8 == companyAwoX else { return }
        // id: first six data bytes after the company ID, present in the primary advertisement.
        let id = bytes[2..<8].map { String(format: "%02x", $0) }.joined()
        var entry = seen[id] ?? Seen(id: id, address: nil, rssi: rssi.intValue)
        // Mesh address: little-endian at data offset 15-16, only in the scan response.
        if bytes.count >= 19 { entry.address = Int(bytes[17]) | Int(bytes[18]) << 8 }
        entry.rssi = rssi.intValue
        if seen[id] == nil { print("  found \(id)") }
        seen[id] = entry
    }
}

struct Lamp { var name: String; var id: String; var meshAddress: String }

func load() -> [Lamp] {
    guard let data = FileManager.default.contents(atPath: path),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let lamps = json["lamps"] as? [[String: Any]] else { return [] }
    return lamps.compactMap { l in
        guard let name = l["name"] as? String, let id = l["id"] as? String else { return nil }
        return Lamp(name: name, id: id.lowercased(), meshAddress: l["meshAddress"] as? String ?? "0x0000")
    }
}

func quote(_ s: String) -> String {
    let data = try! JSONSerialization.data(withJSONObject: [s], options: [])
    return String(String(data: data, encoding: .utf8)!.dropFirst().dropLast())
}

func save(_ lamps: [Lamp]) {
    let rows = lamps.map { "    {\"name\": \(quote($0.name)), \"id\": \"\($0.id)\", \"meshAddress\": \"\($0.meshAddress)\"}" }
    let text = """
    {
      "comment": "Written by tools/scan-lamps.swift. id = first 6 manufacturer-data bytes (company 0x0160) of the lamp's advertisement; meshAddress = from its scan response.",
      "lamps": [
    \(rows.joined(separator: ",\n"))
      ]
    }

    """
    try! text.write(toFile: path, atomically: true, encoding: .utf8)
}

print("Scanning \(Int(seconds)) s for AwoX lamps (passive)…")
let scanner = Scanner()
RunLoop.main.run(until: Date().addingTimeInterval(seconds))
scanner.central.stopScan()

var lamps = load()
let hex4 = { (a: Int) in String(format: "0x%04X", a) }
var changed = false
for s in scanner.seen.values.sorted(by: { $0.rssi > $1.rssi }) {
    let addr = s.address.map(hex4) ?? "unknown (no scan response seen)"
    if let i = lamps.firstIndex(where: { $0.id == s.id }) {
        print("known  \(lamps[i].name): id \(s.id), address \(addr), RSSI \(s.rssi)")
        if let a = s.address, hex4(a) != lamps[i].meshAddress { lamps[i].meshAddress = hex4(a); changed = true }
    } else {
        print("NEW    id \(s.id), address \(addr), RSSI \(s.rssi) (closer = higher)")
        guard s.address != nil else { print("       skipped: rerun the scan to also catch its mesh address"); continue }
        print("       Name for this lamp (empty = skip; an existing name replaces that lamp's entry): ", terminator: "")
        let name = (readLine() ?? "").trimmingCharacters(in: .whitespaces)
        if name.isEmpty { continue }
        // Re-provisioning (e.g. by the AwoX app) can give a lamp a new advertisement ID.
        if let i = lamps.firstIndex(where: { $0.name.lowercased() == name.lowercased() }) {
            print("       replacing \(lamps[i].name): id \(lamps[i].id) -> \(s.id)")
            lamps[i].id = s.id; lamps[i].meshAddress = hex4(s.address!)
        } else {
            lamps.append(Lamp(name: name, id: s.id, meshAddress: hex4(s.address!)))
        }
        changed = true
    }
}
for l in lamps where scanner.seen[l.id] == nil { print("absent \(l.name) (off, connected elsewhere, or not advertising during the scan)") }
if changed { save(lamps); print("Updated \(path). Commit and push it to update the published page.") }
else { print("No changes to \(path).") }
