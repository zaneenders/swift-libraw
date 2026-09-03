import Foundation
import Synchronization
import Clibraw

public enum LibrawError: Error, CustomStringConvertible {
    case openFailed(Int32, String)
    case developFailed(Int32, String)
    case message(String)

    public var description: String {
        switch self {
        case let .openFailed(code, msg): "libraw open failed (\(code)): \(msg)"
        case let .developFailed(code, msg): "libraw develop failed (\(code)): \(msg)"
        case let .message(text): text
        }
    }
}

public struct LibrawGrade: Sendable {
    public var exposure: Double = 0
    public var temperature: Double = 0
    public var tint: Double = 0
    public var contrast: Double = 1
    public var saturation: Double = 1
    public var vibrance: Double = 0
    public var shadows: Double = 0
    public var highlights: Double = 0

    public init(
        exposure: Double = 0,
        temperature: Double = 0,
        tint: Double = 0,
        contrast: Double = 1,
        saturation: Double = 1,
        vibrance: Double = 0,
        shadows: Double = 0,
        highlights: Double = 0
    ) {
        self.exposure = exposure
        self.temperature = temperature
        self.tint = tint
        self.contrast = contrast
        self.saturation = saturation
        self.vibrance = vibrance
        self.shadows = shadows
        self.highlights = highlights
    }

    func bridge() -> libraw_grade {
        libraw_grade(
            exposure: exposure,
            temperature: temperature,
            tint: tint,
            contrast: contrast,
            saturation: saturation,
            vibrance: vibrance,
            shadows: shadows,
            highlights: highlights
        )
    }
}

/// A tightly packed 8-bit sRGB image suitable for ffmpeg's `rgb24` input.
public struct LibrawRGBImage: Sendable {
    public let width: Int
    public let height: Int
    public let pixels: Data

    public init(width: Int, height: Int, pixels: Data) {
        self.width = width
        self.height = height
        self.pixels = pixels
    }
}

public final class Libraw: @unchecked Sendable {
    private let handle: OpaquePointer
    private let mutex = Mutex<Void>(())

    public init() {
        guard let h = libraw_bridge_new() else {
            fatalError("libraw_bridge_new returned nil")
        }
        self.handle = h
    }

    deinit {
        libraw_bridge_free(handle)
    }

    public func open(_ path: String) throws {
        try mutex.withLock { _ in
            let rc = path.withCString { libraw_bridge_open_file(handle, $0) }
            if rc != 0 {
                let msg = libraw_bridge_last_error(handle).map { String(cString: $0) } ?? ""
                throw LibrawError.openFailed(rc, msg)
            }
        }
    }

    public func setGrade(_ grade: LibrawGrade) {
        mutex.withLock { _ in libraw_bridge_set_grade(handle, grade.bridge()) }
    }

    public func setMaxWidth(_ width: Int) {
        let clamped = min(max(0, width), Int(UInt32.max))
        mutex.withLock { _ in libraw_bridge_set_max_width(handle, UInt32(clamped)) }
    }

    /// Edge-aware chroma noise reduction. Strength is clamped to `0...1`.
    public func setDenoise(_ strength: Double) {
        let finiteStrength = strength.isFinite ? strength : 0
        mutex.withLock { _ in
            libraw_bridge_set_denoise(handle, min(1, max(0, finiteStrength)))
        }
    }

    public func developPNG(to path: String) throws {
        try mutex.withLock { _ in
            let rc = path.withCString { libraw_bridge_develop_png(handle, $0) }
            if rc != 0 {
                let msg = libraw_bridge_last_error(handle).map { String(cString: $0) } ?? ""
                throw LibrawError.developFailed(rc, msg)
            }
        }
    }

    /// Develop into tightly packed, 8-bit sRGB pixels without PNG encoding.
    public func developRGB() throws -> LibrawRGBImage {
        try mutex.withLock { _ in
            var image = libraw_rgb_image()
            let rc = libraw_bridge_develop_rgb(handle, &image)
            if rc != 0 {
                let msg = libraw_bridge_last_error(handle).map { String(cString: $0) } ?? ""
                throw LibrawError.developFailed(rc, msg)
            }
            guard let bytes = image.data else {
                throw LibrawError.message("libraw returned an empty RGB image")
            }
            defer { libraw_bridge_free_rgb(bytes) }
            return LibrawRGBImage(
                width: Int(image.width),
                height: Int(image.height),
                pixels: Data(bytes: bytes, count: image.size)
            )
        }
    }

    public static var version: String {
        String(cString: libraw_bridge_version())
    }
}
