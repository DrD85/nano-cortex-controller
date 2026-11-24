// Writes the Duo DAW's StartKit sounds for test_drums.c: compiled together with the DAW's DrumKit.swift sound code.
// Usage (run.sh does it): swiftc -O ref_kit.swift drumkit_ref.swift -o ref && ./ref out.bin
import Foundation
let roles = ["kick", "snare", "clap", "hat", "openhat", "lowtom", "hightom", "ride", "crash"]
var data = Data()
for role in roles {
    let (l, r) = RefKit.sound(role)
    var n = Int32(l.count)
    data.append(Data(bytes: &n, count: 4))
    var lr = [Float](); lr.reserveCapacity(2 * l.count)
    for i in 0..<l.count { lr.append(l[i]); lr.append(r[i]) }
    lr.withUnsafeBufferPointer { data.append(Data(buffer: $0)) }
}
try! data.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
