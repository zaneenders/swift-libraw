// swift-tools-version: 6.3
import PackageDescription

let package = Package(
    name: "swift-libraw",
    platforms: [
        .macOS(.v26),
    ],
    products: [
        .library(name: "Libraw", targets: ["Libraw"]),
    ],
    targets: [
        .target(
            name: "Clibraw",
            path: "Sources/Clibraw",
            exclude: [
                "libraw/samples",
                "libraw/RawSpeed",
                "libraw/RawSpeed3",
                "libraw/GoPro",
                "libraw/bin",
                "libraw/buildfiles",
                "libraw/doc",
                "libraw/m4",
                "libraw/object",
                "libraw/lib",
                "libraw/src/postprocessing/postprocessing_ph.cpp",
                "libraw/src/preprocessing/preprocessing_ph.cpp",
                "libraw/src/write/write_ph.cpp",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("libraw"),
                .define("LIBRAW_NOTHREADS"),
                .define("USE_ZLIB"),
            ],
            cxxSettings: [
                .unsafeFlags([
                    "-std=c++17",
                    "-Wno-deprecated-declarations",
                    "-Wno-shorten-64-to-32",
                    "-Wno-macro-redefined",
                ]),
            ],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedLibrary("m"),
            ]
        ),
        .target(
            name: "Libraw",
            dependencies: ["Clibraw"],
            path: "Sources/Libraw"
        ),
        .testTarget(
            name: "LibrawTests",
            dependencies: ["Libraw"],
            path: "Tests/LibrawTests"
        ),
    ]
)
