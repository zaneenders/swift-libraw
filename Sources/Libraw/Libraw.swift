import Foundation
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

public final class Libraw: @unchecked Sendable {
    private let handle: OpaquePointer

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
        let rc = path.withCString { libraw_bridge_open_file(handle, $0) }
        if rc != 0 {
            let msg = libraw_bridge_last_error(handle).map { String(cString: $0) } ?? ""
            throw LibrawError.openFailed(rc, msg)
        }
    }

    public func setGrade(_ grade: LibrawGrade) {
        libraw_bridge_set_grade(handle, grade.bridge())
    }

    public func setMaxWidth(_ width: Int) {
        libraw_bridge_set_max_width(handle, UInt32(max(0, width)))
    }

    public func developPNG(to path: String) throws {
        let rc = path.withCString { libraw_bridge_develop_png(handle, $0) }
        if rc != 0 {
            let msg = libraw_bridge_last_error(handle).map { String(cString: $0) } ?? ""
            throw LibrawError.developFailed(rc, msg)
        }
    }

    public static var version: String {
        String(cString: libraw_bridge_version())
    }
}
