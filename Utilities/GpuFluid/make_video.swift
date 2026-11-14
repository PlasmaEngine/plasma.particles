// Turns the numbered frames of a Player recording into an H.264 video. macOS only, needs no extra tools.
//
//   swift make_video.swift <out.mp4> <frames per second> <frame folder> [<frame folder> ...]
//
// Several folders are played one after the other. A file named 'title.txt' in a folder puts its first line on the
// frames of that folder, in the lower left corner. A second line 'repeat N' shows every frame of the folder N times,
// for frames that were captured slower than the video plays. Frames of another shape than the video are fitted into
// it with black bars.
//
// VIDEO_WIDTH and VIDEO_BITRATE in the environment set the width in pixels (the height follows 16:9) and the bits per
// second. Without them the video has the size of the first frame.

import AVFoundation
import AppKit
import CoreGraphics
import Foundation

let arguments = CommandLine.arguments
guard arguments.count >= 4, let fps = Int32(arguments[2]) else {
  print("usage: make_video.swift <out.mp4> <frames per second> <frame folder> [<frame folder> ...]")
  exit(1)
}

let outUrl = URL(fileURLWithPath: arguments[1])
try? FileManager.default.removeItem(at: outUrl)

struct Clip {
  let frames: [URL]
  let title: String?
  let repeats: Int
}

var clips: [Clip] = []
for folder in arguments[3...] {
  let url = URL(fileURLWithPath: folder)
  let files = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? []
  let frames = files.filter { ["jpg", "png"].contains($0.pathExtension.lowercased()) }.sorted { $0.lastPathComponent < $1.lastPathComponent }

  let lines = ((try? String(contentsOf: url.appendingPathComponent("title.txt"), encoding: .utf8)) ?? "").split(separator: "\n").map(String.init)
  let repeats = lines.dropFirst().compactMap { $0.hasPrefix("repeat ") ? Int($0.dropFirst(7)) : nil }.first ?? 1

  if !frames.isEmpty { clips.append(Clip(frames: frames, title: lines.first, repeats: max(repeats, 1))) }
}

guard let firstFrame = clips.first?.frames.first, let firstImage = NSImage(contentsOf: firstFrame)?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
  print("no frames found")
  exit(1)
}

// H.264 wants even sizes
let environment = ProcessInfo.processInfo.environment
let wantedWidth = environment["VIDEO_WIDTH"].flatMap { Int($0) }
let width = (wantedWidth ?? firstImage.width) / 2 * 2
let height = (wantedWidth != nil ? width * 9 / 16 : firstImage.height) / 2 * 2
let bitrate = environment["VIDEO_BITRATE"].flatMap { Int($0) } ?? width * height * 6

let writer = try AVAssetWriter(outputURL: outUrl, fileType: .mp4)
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
  AVVideoCodecKey: AVVideoCodecType.h264,
  AVVideoWidthKey: width,
  AVVideoHeightKey: height,
  AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: bitrate],
])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
  kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB,
  kCVPixelBufferWidthKey as String: width,
  kCVPixelBufferHeightKey as String: height,
])

writer.add(input)
writer.startWriting()
writer.startSession(atSourceTime: .zero)

func draw(_ image: CGImage, title: String?, into buffer: CVPixelBuffer) {
  CVPixelBufferLockBaseAddress(buffer, [])
  defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

  guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
    bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)
  else { return }

  // the whole frame, as large as it fits
  context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
  context.fill(CGRect(x: 0, y: 0, width: width, height: height))

  let scale = min(CGFloat(width) / CGFloat(image.width), CGFloat(height) / CGFloat(image.height))
  let fitted = CGSize(width: CGFloat(image.width) * scale, height: CGFloat(image.height) * scale)
  context.interpolationQuality = .high
  context.draw(image, in: CGRect(x: (CGFloat(width) - fitted.width) / 2, y: (CGFloat(height) - fitted.height) / 2, width: fitted.width, height: fitted.height))

  guard let title = title else { return }

  let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: CGFloat(height) / 30, weight: .medium), .foregroundColor: NSColor.white]
  let text = NSAttributedString(string: title, attributes: attributes)
  let size = text.size()
  let margin = CGFloat(height) / 40

  context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 0.55))
  context.fill(CGRect(x: margin, y: margin, width: size.width + margin, height: size.height + margin / 2))

  NSGraphicsContext.saveGraphicsState()
  NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
  text.draw(at: CGPoint(x: margin * 1.5, y: margin * 1.25))
  NSGraphicsContext.restoreGraphicsState()
}

var frameIndex: Int64 = 0
for clip in clips {
  for frame in clip.frames {
    guard let image = NSImage(contentsOf: frame)?.cgImage(forProposedRect: nil, context: nil, hints: nil) else { continue }

    var buffer: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &buffer)
    guard let pixelBuffer = buffer else { continue }

    draw(image, title: clip.title, into: pixelBuffer)

    for _ in 0..<clip.repeats {
      while !input.isReadyForMoreMediaData { usleep(2000) }
      adaptor.append(pixelBuffer, withPresentationTime: CMTime(value: frameIndex, timescale: fps))
      frameIndex += 1
    }
  }
}

input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()

print("\(frameIndex) frames, \(width)x\(height), \(Double(frameIndex) / Double(fps)) s -> \(outUrl.path)")
