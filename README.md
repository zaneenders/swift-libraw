# swift-libraw

A cross-platform Swift wrapper around [LibRaw](https://github.com/LibRaw/LibRaw),
the open-source RAW image decoder. LibRaw is vendored as a git submodule and
compiled directly by Swift Package Manager — no separate build step. The same
package builds on **Linux and macOS**.

It exposes a single `Libraw` class that opens a DNG/RAW file, applies a
photographic grade (exposure, white balance, contrast, saturation, vibrance,
shadows, highlights), and writes an 8-bit sRGB PNG.

## Layout

```
Sources/
  Clibraw/
    include/libraw_bridge.h   # public C API Swift imports
    include/module.modulemap
    shim.cpp                  # extern "C" impl calling LibRaw's C++ API
    stb_image_write.h         # vendored single-header PNG writer (public domain)
    libraw/                   # LibRaw submodule — compiled by SwiftPM directly
  Libraw/
    Libraw.swift              # Swift API
```

## Setup

```sh
git clone --recursive <this-repo>
cd swift-libraw
swift build            # compiles LibRaw + shim + Swift wrapper
swift test             # runs smoke tests
```

System dependencies: a C++ compiler (`g++`/`clang++`) and `zlib1g-dev`
(Linux) / `zlib` (Homebrew). SwiftPM compiles LibRaw's 83 `.cpp` sources as
part of the `Clibraw` target, so the symbols are linked automatically into
any package that depends on `swift-libraw`.

## Usage

```swift
import Libraw

let dev = Libraw()
try dev.open("/path/to/frame.dng")
dev.setGrade(.init(exposure: 1.25, temperature: 4300, tint: 5,
                   contrast: 1.05, saturation: 1.05, vibrance: 0.15,
                   shadows: 0.2, highlights: 0.8))
dev.setMaxWidth(3840)
try dev.developPNG(to: "/tmp/frame.png")
```

## Notes on the grade mapping

`LibrawGrade` mirrors the controls the macOS Core Image pipeline in
`gopro-timelapse` used, so the two paths produce comparable results:

- `exposure` is in stops, passed to LibRaw's `exp_shift`.
- `temperature`/`tint` are converted to RGB white-balance multipliers via a
  Kelvin curve; `<= 0` falls back to camera WB.
- `contrast`, `saturation`, `vibrance`, `shadows`, `highlights` are applied as
  sRGB-space pixel ops after demosaic. Approximate, not a match to Core
  Image's filters, but good enough to drive a deflickered timelapse ramp.

PNG output uses the public-domain `stb_image_write` so there are no extra
system image libraries to link.
