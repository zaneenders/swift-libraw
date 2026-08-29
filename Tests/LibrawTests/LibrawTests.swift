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
}
