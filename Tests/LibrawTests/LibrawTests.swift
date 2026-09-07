import Testing
import Foundation
@testable import Libraw

@Suite("Libraw")
struct LibrawTests {
    @Test("version string is non-empty")
    func versionNotEmpty() {
        #expect(!Libraw.version.isEmpty)
    }

    @Test("grade bridge round-trips defaults")
    func gradeDefaults() {
        let g = LibrawGrade()
        let b = g.bridge()
        #expect(b.exposure == 0)
        #expect(b.temperature == 0)
        #expect(b.contrast == 1)
        #expect(b.saturation == 1)
        #expect(b.vibrance == 0)
        #expect(b.shadows == 0)
        #expect(b.highlights == 0)
    }

    @Test("16-bit RGB image preserves packed buffer metadata")
    func rgb16ImageMetadata() {
        let pixels = Data([0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a])
        let image = LibrawRGB16Image(width: 1, height: 1, pixels: pixels)
        #expect(image.width == 1)
        #expect(image.height == 1)
        #expect(image.pixels == pixels)
        #expect(image.pixels.count == image.width * image.height * 3 * 2)
    }

    @Test("numeric setters clamp extreme and non-finite inputs")
    func numericSetters() {
        let raw = Libraw()
        raw.setMaxWidth(-1)
        raw.setMaxWidth(Int.max)
        raw.setDenoise(-1)
        raw.setDenoise(0.65)
        raw.setDenoise(2)
        raw.setDenoise(.nan)
        raw.setDenoise(.infinity)
    }
}
