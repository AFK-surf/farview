// Generates macOS AppIcon.icns (and optional PNG previews) from a square source PNG.
//
// Usage:
//   swift tools/make_mac_icon.swift <input.png> <output.icns> [--preview <preview.png>]
//   swift tools/make_mac_icon.swift --png <input.png> <output.png> [size]
//
// Follows Apple's macOS icon grid on a 1024x1024 canvas: 824x824 rounded square
// centered at (100, 100) with corner radius 185 and subtle drop shadow.
import AppKit
import Foundation

let args = Array(CommandLine.arguments.dropFirst())
var pngOnly = false
var previewPath: String? = nil
var positional: [String] = []

var i = 0
while i < args.count {
  switch args[i] {
  case "--png":
    pngOnly = true
    i += 1
  case "--preview":
    guard i + 1 < args.count else {
      fputs("make_mac_icon: --preview requires a path\n", stderr)
      exit(1)
    }
    previewPath = args[i + 1]
    i += 2
  default:
    positional.append(args[i])
    i += 1
  }
}

guard positional.count >= 2 else {
  fputs("usage: swift tools/make_mac_icon.swift [--png] <input.png> <output> [--preview <png>]\n", stderr)
  exit(1)
}

let inputPath = positional[0]
let outputPath = positional[1]

guard let srcImage = NSImage(contentsOfFile: inputPath) else {
  fputs("make_mac_icon: failed to load \(inputPath)\n", stderr)
  exit(1)
}

func renderIcon(size: Int, fullBleed: Bool) -> Data {
  let rep = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: size,
    pixelsHigh: size,
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
  )!
  NSGraphicsContext.saveGraphicsState()
  let ctx = NSGraphicsContext(bitmapImageRep: rep)!
  ctx.imageInterpolation = .high
  NSGraphicsContext.current = ctx

  let k = CGFloat(size) / 1024.0
  let edge: CGFloat = fullBleed ? 32.0 : 100.0
  let side: CGFloat = fullBleed ? 960.0 : 824.0
  let radius: CGFloat = fullBleed ? 192.0 : 185.0
  let rect = NSRect(x: edge * k, y: edge * k, width: side * k, height: side * k)
  let shape = NSBezierPath(roundedRect: rect, xRadius: radius * k, yRadius: radius * k)

  if !fullBleed {
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowOffset = NSSize(width: 0, height: -10.0 * k)
    shadow.shadowBlurRadius = 24.0 * k
    shadow.shadowColor = NSColor(white: 0, alpha: 0.28)
    shadow.set()
    NSColor.black.setFill()
    shape.fill()
    NSGraphicsContext.restoreGraphicsState()
  }

  NSGraphicsContext.saveGraphicsState()
  shape.addClip()
  srcImage.draw(
    in: rect,
    from: NSRect(origin: .zero, size: srcImage.size),
    operation: .sourceOver,
    fraction: 1.0
  )
  NSGraphicsContext.restoreGraphicsState()

  NSGraphicsContext.restoreGraphicsState()
  return rep.representation(using: .png, properties: [:])!
}

if pngOnly {
  let px = positional.count >= 3 ? (Int(positional[2]) ?? 512) : 512
  do {
    try renderIcon(size: px, fullBleed: true).write(to: URL(fileURLWithPath: outputPath))
  } catch {
    fputs("make_mac_icon: failed to write \(outputPath): \(error)\n", stderr)
    exit(1)
  }
  exit(0)
}

if let preview = previewPath {
  do {
    try renderIcon(size: 1024, fullBleed: false).write(to: URL(fileURLWithPath: preview))
  } catch {
    fputs("make_mac_icon: failed to write preview \(preview): \(error)\n", stderr)
    exit(1)
  }
}

let tmpSet = URL(fileURLWithPath: NSTemporaryDirectory())
  .appendingPathComponent("farview-\(ProcessInfo.processInfo.processIdentifier).iconset")
try? FileManager.default.removeItem(at: tmpSet)
try FileManager.default.createDirectory(at: tmpSet, withIntermediateDirectories: true)

for base in [16, 32, 128, 256, 512] {
  try renderIcon(size: base, fullBleed: false)
    .write(to: tmpSet.appendingPathComponent("icon_\(base)x\(base).png"))
  try renderIcon(size: base * 2, fullBleed: false)
    .write(to: tmpSet.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}

let proc = Process()
proc.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
proc.arguments = ["-c", "icns", tmpSet.path, "-o", outputPath]
try proc.run()
proc.waitUntilExit()
try? FileManager.default.removeItem(at: tmpSet)
exit(proc.terminationStatus)
