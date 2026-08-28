// generate_icon.swift
//
// One-time app icon generator for Akaizer S -- not part of the
// repeatable build (build_app_bundle.sh just copies the already-built
// Resources/AppIcon.icns). Run this again only if the icon design
// itself changes.
//
// Matches the app's own retro-accent identity (LCDReadoutView's fixed
// phosphor-green-on-dark palette) rather than a generic glyph: a
// waveform that visibly stretches from tight/compressed on the left to
// wide/sparse on the right -- the app's actual function, drawn.
//
// Usage:
//   rm -rf Resources/AppIcon.iconset
//   swift Scripts/generate_icon.swift Resources/AppIcon.iconset
//   iconutil -c icns Resources/AppIcon.iconset -o Resources/AppIcon.icns
//   rm -rf Resources/AppIcon.iconset   # intermediate output, not worth keeping once the .icns exists

import AppKit
import CoreGraphics
import CoreImage

let outputDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "./AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: outputDir, withIntermediateDirectories: true)

let canvasSize = 1024.0

// Palette -- exactly LCDReadoutView's fixed colours.
let bgTop = NSColor(srgbRed: 0.118, green: 0.153, blue: 0.129, alpha: 1.0)  // noticeably lighter than the LCD bg, for real gradient depth
let bgBottom = NSColor(srgbRed: 0.047, green: 0.078, blue: 0.063, alpha: 1.0) // #0C1410
let litGreen = NSColor(srgbRed: 0.56, green: 0.90, blue: 0.66, alpha: 1.0)    // #8FE5A8
let dimGreen = NSColor(srgbRed: 0.25, green: 0.42, blue: 0.30, alpha: 1.0)    // #3F6B4C

func drawIcon(size: CGFloat) -> NSImage {
    let image = NSImage(size: NSSize(width: size, height: size))
    image.lockFocus()
    guard let ctx = NSGraphicsContext.current?.cgContext else {
        image.unlockFocus()
        return image
    }

    // Background: rounded square at the conventional macOS icon
    // proportions (content inset ~4%, corner radius ~18% of canvas).
    let inset = size * 0.04
    let rect = CGRect(x: inset, y: inset, width: size - inset * 2, height: size - inset * 2)
    let cornerRadius = rect.width * 0.225
    let bgPath = CGPath(roundedRect: rect, cornerWidth: cornerRadius, cornerHeight: cornerRadius, transform: nil)

    ctx.saveGState()
    ctx.addPath(bgPath)
    ctx.clip()
    let colors = [bgTop.cgColor, bgBottom.cgColor] as CFArray
    if let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0, 1]) {
        ctx.drawLinearGradient(gradient, start: CGPoint(x: rect.midX, y: rect.maxY), end: CGPoint(x: rect.midX, y: rect.minY), options: [])
    }
    ctx.restoreGState()

    // The waveform glyph: a chirp that stretches -- tight/frequent on
    // the left, wide/sparse on the right, numerically integrating a
    // local frequency that decreases across x. This is the one thing
    // that should be instantly legible even at 16x16: a simple
    // horizontal squiggle whose density visibly changes left to right.
    let glyphLeft = rect.minX + rect.width * 0.16
    let glyphRight = rect.maxX - rect.width * 0.16
    let midY = rect.midY
    let amplitude = rect.height * 0.16
    let steps = 800
    var points: [CGPoint] = []
    var phase = 0.0
    let baseFreq = 52.0 // radians per unit t at the tightest (left) end -- enough for ~5-6 tight cycles before the stretch opens up
    for i in 0...steps {
        let t = Double(i) / Double(steps) // 0...1 across the glyph width
        let stretchFactor = 1.0 + t * t * t * 11.0 // slow start, accelerating hard -- 1x at left, 12x at right, so density contrast reads clearly even at 16x16
        let localFreq = baseFreq / stretchFactor
        phase += localFreq * (1.0 / Double(steps))
        let x = glyphLeft + CGFloat(t) * (glyphRight - glyphLeft)
        // Amplitude also eases in, so the very left edge isn't a jarring hard clip.
        let envelope = min(1.0, Double(i) / 12.0) * min(1.0, Double(steps - i) / 12.0 + 0.85)
        let y = midY + CGFloat(sin(phase)) * amplitude * CGFloat(envelope)
        points.append(CGPoint(x: x, y: y))
    }

    let path = CGMutablePath()
    path.move(to: points[0])
    for p in points.dropFirst() { path.addLine(to: p) }

    // Real phosphor glow: render the stroke alone into its own layer,
    // Gaussian-blur it, then composite that under a crisp top stroke.
    // (CoreImage here is a one-time dev-tooling script, not something
    // the shipped app links against -- doesn't touch the project's
    // zero-third-party-dependency stance.)
    guard let glowLayer = CGContext(
        data: nil, width: Int(size), height: Int(size), bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
    ) else {
        image.unlockFocus()
        return image
    }
    glowLayer.setLineCap(.round)
    glowLayer.setLineJoin(.round)
    glowLayer.addPath(path)
    glowLayer.setStrokeColor(litGreen.cgColor)
    glowLayer.setLineWidth(rect.width * (9.0 / 1024.0))
    glowLayer.strokePath()

    if let glowImage = glowLayer.makeImage() {
        let ciImage = CIImage(cgImage: glowImage)
        let blurred = ciImage
            .applyingFilter("CIGaussianBlur", parameters: [kCIInputRadiusKey: size * 0.02])
            .cropped(to: CGRect(x: 0, y: 0, width: size, height: size))
        let ciContext = CIContext()
        if let blurredCG = ciContext.createCGImage(blurred, from: blurred.extent) {
            ctx.saveGState()
            ctx.setAlpha(0.85)
            ctx.draw(blurredCG, in: CGRect(x: 0, y: 0, width: size, height: size))
            ctx.restoreGState()
        }
    }

    ctx.saveGState()
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    ctx.addPath(path)
    ctx.setStrokeColor(litGreen.cgColor)
    ctx.setLineWidth(rect.width * (7.0 / 1024.0))
    ctx.strokePath()
    ctx.restoreGState()

    image.unlockFocus()
    return image
}

func savePNG(_ image: NSImage, size: Int, to path: String) {
    let targetSize = NSSize(width: size, height: size)
    guard let resized = NSImage(size: targetSize, flipped: false, drawingHandler: { rect in
        image.draw(in: rect, from: .zero, operation: .copy, fraction: 1.0)
        return true
    }) as NSImage?,
    let tiff = resized.tiffRepresentation,
    let bitmap = NSBitmapImageRep(data: tiff),
    let png = bitmap.representation(using: .png, properties: [:]) else {
        print("Failed to render \(path)")
        return
    }
    try? png.write(to: URL(fileURLWithPath: path))
}

let master = drawIcon(size: canvasSize)

let targets: [(String, Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

for (name, size) in targets {
    savePNG(master, size: size, to: "\(outputDir)/\(name)")
}

print("Wrote \(targets.count) icon images to \(outputDir)")
