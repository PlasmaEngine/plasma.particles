import AVFoundation
import AppKit
// usage: swift frames.swift video.mp4 outdir t1 t2 ...
let a = CommandLine.arguments
let asset = AVURLAsset(url: URL(fileURLWithPath: a[1]))
let gen = AVAssetImageGenerator(asset: asset)
gen.requestedTimeToleranceBefore = .zero; gen.requestedTimeToleranceAfter = .zero
for t in a[3...] {
  let time = CMTime(seconds: Double(t)!, preferredTimescale: 600)
  if let img = try? gen.copyCGImage(at: time, actualTime: nil) {
    let rep = NSBitmapImageRep(cgImage: img)
    let data = rep.representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(a[2])/t\(t).png"))
    print("wrote t\(t)")
  } else { print("fail \(t)") }
}
