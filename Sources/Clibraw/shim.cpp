#include "libraw/libraw.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_W_NO_FAILURE 0

extern "C" {

#include "include/libraw_bridge.h"

}

namespace {

struct Processor {
    LibRaw raw;
    libraw_grade grade{};
    uint32_t maxWidth = 0;
    double denoise = 0;
    std::string path;
    bool needsReload = false;
    std::string error;
};

static double finiteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

static libraw_grade sanitizeGrade(libraw_grade grade) {
    grade.exposure = finiteOr(grade.exposure, 0.0);
    grade.temperature = finiteOr(grade.temperature, 0.0);
    grade.tint = finiteOr(grade.tint, 0.0);
    grade.contrast = finiteOr(grade.contrast, 1.0);
    grade.saturation = finiteOr(grade.saturation, 1.0);
    grade.vibrance = finiteOr(grade.vibrance, 0.0);
    grade.shadows = finiteOr(grade.shadows, 0.0);
    grade.highlights = finiteOr(grade.highlights, 0.0);
    return grade;
}

/* Tanner Helland's Kelvin -> RGB approximation, returned normalized to [0,1]. */
static void kelvinToRGB(double K, double& r, double& g, double& b) {
    double t = K / 100.0;
    double red, green, blue;
    if (t <= 66) {
        red = 255.0;
        green = 99.4708025861 * std::log(t) - 161.1195681661;
        blue = (t <= 19) ? 0.0 : (138.5177312231 * std::log(t - 10) - 305.0447927307);
    } else {
        red = 329.698727446 * std::pow(t - 60, -0.1332047592);
        green = 288.1221695283 * std::pow(t - 60, -0.0755148492);
        blue = 255.0;
    }
    r = std::clamp(red / 255.0, 0.0, 1.0);
    g = std::clamp(green / 255.0, 0.0, 1.0);
    b = std::clamp(blue / 255.0, 0.0, 1.0);
}

template <typename Sample>
static inline Sample clampSample(double value, double maximum) {
    return (Sample)std::clamp(value + 0.5, 0.0, maximum);
}

/* Strong chroma denoise in YCbCr. Severe night RAW noise perturbs measured
   luma too, so an edge gate preserves the colored speckle. A separable box
   filter removes low-frequency chroma efficiently while retaining each
   pixel's original luminance, preserving stars and structural detail. */
template <typename Sample>
static void denoiseChroma(Sample* data, int W, int H, int chans, double strength,
                          double maximum) {
    strength = std::clamp(strength, 0.0, 1.0);
    if (strength <= 0 || W < 3 || H < 3 || chans < 3) return;

    const size_t count = (size_t)W * H;
    const int radius = 1 + (int)std::round(strength * 6.0);
    const double blend = std::min(1.0, 0.4 + strength);
    std::vector<float> chroma(count * 2);
    std::vector<float> horizontal(count * 2);

    for (size_t p = 0; p < count; ++p) {
        const size_t i = p * chans;
        const double r = data[i], g = data[i + 1], b = data[i + 2];
        const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        chroma[p * 2] = (float)(b - y);
        chroma[p * 2 + 1] = (float)(r - y);
    }

    for (int y = 0; y < H; ++y) {
        double cb = 0, cr = 0;
        int samples = 0;
        for (int sx = 0; sx <= std::min(radius, W - 1); ++sx) {
            const size_t p = ((size_t)y * W + sx) * 2;
            cb += chroma[p]; cr += chroma[p + 1]; ++samples;
        }
        for (int x = 0; x < W; ++x) {
            const size_t out = ((size_t)y * W + x) * 2;
            horizontal[out] = (float)(cb / samples);
            horizontal[out + 1] = (float)(cr / samples);
            const int removeX = x - radius;
            if (removeX >= 0) {
                const size_t p = ((size_t)y * W + removeX) * 2;
                cb -= chroma[p]; cr -= chroma[p + 1]; --samples;
            }
            const int addX = x + radius + 1;
            if (addX < W) {
                const size_t p = ((size_t)y * W + addX) * 2;
                cb += chroma[p]; cr += chroma[p + 1]; ++samples;
            }
        }
    }

    for (int x = 0; x < W; ++x) {
        double cb = 0, cr = 0;
        int samples = 0;
        for (int sy = 0; sy <= std::min(radius, H - 1); ++sy) {
            const size_t p = ((size_t)sy * W + x) * 2;
            cb += horizontal[p]; cr += horizontal[p + 1]; ++samples;
        }
        for (int y = 0; y < H; ++y) {
            const size_t p = (size_t)y * W + x;
            const size_t i = p * chans;
            const double r = data[i], g = data[i + 1], b = data[i + 2];
            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            const double filteredCb = cb / samples;
            const double filteredCr = cr / samples;
            const double outputCb = chroma[p * 2] * (1.0 - blend) + filteredCb * blend;
            const double outputCr = chroma[p * 2 + 1] * (1.0 - blend) + filteredCr * blend;
            data[i] = clampSample<Sample>(luma + outputCr, maximum);
            data[i + 2] = clampSample<Sample>(luma + outputCb, maximum);
            data[i + 1] = clampSample<Sample>(
                (luma - 0.2126 * data[i] - 0.0722 * data[i + 2]) / 0.7152,
                maximum);

            const int removeY = y - radius;
            if (removeY >= 0) {
                const size_t q = ((size_t)removeY * W + x) * 2;
                cb -= horizontal[q]; cr -= horizontal[q + 1]; --samples;
            }
            const int addY = y + radius + 1;
            if (addY < H) {
                const size_t q = ((size_t)addY * W + x) * 2;
                cb += horizontal[q]; cr += horizontal[q + 1]; ++samples;
            }
        }
    }
}

/* Dark-adaptive luminance denoise. It is intentionally weaker than chroma
   filtering so edges and stars remain visible while high-ISO grain settles. */
template <typename Sample>
static void denoiseLuma(Sample* data, int W, int H, int chans, double strength,
                        double maximum) {
    if (strength <= 0 || W < 3 || H < 3 || chans < 3) return;
    const size_t count = (size_t)W * H;
    const int radius = 1 + (int)std::round(strength * 3.0);
    std::vector<float> luma(count), horizontal(count);

    for (size_t p = 0; p < count; ++p) {
        const size_t i = p * chans;
        luma[p] = (float)(0.2126 * data[i] + 0.7152 * data[i + 1] + 0.0722 * data[i + 2]);
    }

    for (int y = 0; y < H; ++y) {
        double sum = 0;
        int samples = 0;
        for (int sx = 0; sx <= std::min(radius, W - 1); ++sx) {
            sum += luma[(size_t)y * W + sx]; ++samples;
        }
        for (int x = 0; x < W; ++x) {
            horizontal[(size_t)y * W + x] = (float)(sum / samples);
            const int removeX = x - radius;
            if (removeX >= 0) { sum -= luma[(size_t)y * W + removeX]; --samples; }
            const int addX = x + radius + 1;
            if (addX < W) { sum += luma[(size_t)y * W + addX]; ++samples; }
        }
    }

    for (int x = 0; x < W; ++x) {
        double sum = 0;
        int samples = 0;
        for (int sy = 0; sy <= std::min(radius, H - 1); ++sy) {
            sum += horizontal[(size_t)sy * W + x]; ++samples;
        }
        for (int y = 0; y < H; ++y) {
            const size_t p = (size_t)y * W + x;
            const size_t i = p * chans;
            const double originalY = luma[p];
            const double filteredY = sum / samples;
            const double darkness = std::clamp(
                1.0 - originalY / (maximum * (180.0 / 255.0)), 0.15, 1.0);
            const double blend = strength * 0.8 * darkness;
            const double outputY = originalY * (1.0 - blend) + filteredY * blend;
            const double delta = outputY - originalY;
            data[i] = clampSample<Sample>(data[i] + delta, maximum);
            data[i + 1] = clampSample<Sample>(data[i + 1] + delta, maximum);
            data[i + 2] = clampSample<Sample>(data[i + 2] + delta, maximum);

            const int removeY = y - radius;
            if (removeY >= 0) { sum -= horizontal[(size_t)removeY * W + x]; --samples; }
            const int addY = y + radius + 1;
            if (addY < H) { sum += horizontal[(size_t)addY * W + x]; ++samples; }
        }
    }
}

/* Apply contrast/saturation/vibrance/shadows/highlights in sRGB space.
   Samples use the supplied maximum, with chans channels in row-major order. */
template <typename Sample>
static void applyGrade(Sample* data, int W, int H, int chans, const libraw_grade& g,
                       double maximum) {
    if (g.contrast == 1 && g.saturation == 1 && g.vibrance == 0 &&
        g.shadows == 0 && g.highlights == 0) {
        return;
    }
    const double contrast = g.contrast;
    const double sat = g.saturation;
    const double vib = g.vibrance;
    const double shadows = g.shadows;
    const double highlights = g.highlights;
    const bool hasAlpha = (chans == 4);
    const size_t stride = (size_t)W * chans;

    for (int y = 0; y < H; ++y) {
        Sample* row = data + (size_t)y * stride;
        for (int x = 0; x < W; ++x) {
            Sample* px = row + (size_t)x * chans;
            double r = px[0], gg = px[1], b = px[2];
            double l = 0.2126 * r + 0.7152 * gg + 0.0722 * b;

            /* Saturation: push channels away from luminance. */
            if (sat != 1) {
                r = l + (r - l) * sat;
                gg = l + (gg - l) * sat;
                b = l + (b - l) * sat;
            }

            /* Vibrance: scale by how unsaturated the pixel is.
               s_cur in [0,1] is a rough saturation measure. */
            if (vib != 0) {
                double maxc = std::max({r, gg, b});
                double minc = std::min({r, gg, b});
                double s_cur = (maxc <= 0) ? 0 : (maxc - minc) / maxc;
                double v = vib * (1.0 - s_cur);
                r = l + (r - l) * (1.0 + v);
                gg = l + (gg - l) * (1.0 + v);
                b = l + (b - l) * (1.0 + v);
            }

            /* Normalize to [0,1] for tone ops. Use luminance—not the red
               channel—to weight tonal masks, otherwise saturated blue/green
               pixels receive a very different grade from equally bright red
               pixels and visibly shift hue. */
            double rn = r / maximum, gn = gg / maximum, bn = b / maximum;
            const double toneLuma = std::clamp(
                0.2126 * rn + 0.7152 * gn + 0.0722 * bn, 0.0, 1.0);

            /* Shadows lift/crush: weight strongest in the darks. */
            if (shadows != 0) {
                double w = (1.0 - toneLuma) * (1.0 - toneLuma);
                rn = rn + shadows * w * (shadows > 0 ? (1.0 - rn) : rn);
                gn = gn + shadows * w * (shadows > 0 ? (1.0 - gn) : gn);
                bn = bn + shadows * w * (shadows > 0 ? (1.0 - bn) : bn);
            }

            /* Highlights recovery: weight strongest in the brights. */
            if (highlights > 0) {
                double w = toneLuma * toneLuma;
                rn = rn - highlights * w * rn;
                gn = gn - highlights * w * gn;
                bn = bn - highlights * w * bn;
            }

            /* Contrast around 0.5. */
            if (contrast != 1) {
                rn = (rn - 0.5) * contrast + 0.5;
                gn = (gn - 0.5) * contrast + 0.5;
                bn = (bn - 0.5) * contrast + 0.5;
            }

            px[0] = clampSample<Sample>(rn * maximum, maximum);
            px[1] = clampSample<Sample>(gn * maximum, maximum);
            px[2] = clampSample<Sample>(bn * maximum, maximum);
            if (hasAlpha) px[3] = px[3];
        }
    }
}

/* Simple bilinear downscale. */
template <typename Sample>
static void bilinearScale(const Sample* src, int sw, int sh, int chans,
                          Sample* dst, int dw, int dh, double maximum) {
    const double sx = (double)sw / dw;
    const double sy = (double)sh / dh;
    for (int y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)std::floor(fy);
        int y1 = y0 + 1;
        double wy = fy - y0;
        y0 = std::clamp(y0, 0, sh - 1);
        y1 = std::clamp(y1, 0, sh - 1);
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)std::floor(fx);
            int x1 = x0 + 1;
            double wx = fx - x0;
            x0 = std::clamp(x0, 0, sw - 1);
            x1 = std::clamp(x1, 0, sw - 1);
            for (int c = 0; c < chans; ++c) {
                double v00 = src[((size_t)y0 * sw + x0) * chans + c];
                double v01 = src[((size_t)y0 * sw + x1) * chans + c];
                double v10 = src[((size_t)y1 * sw + x0) * chans + c];
                double v11 = src[((size_t)y1 * sw + x1) * chans + c];
                double v = (v00 * (1 - wx) + v01 * wx) * (1 - wy) +
                           (v10 * (1 - wx) + v11 * wx) * wy;
                dst[((size_t)y * dw + x) * chans + c] =
                    clampSample<Sample>(v, maximum);
            }
        }
    }
}


template <typename Sample>
static int developRGB(Processor* pp, std::vector<Sample>& output,
                      int& outputWidth, int& outputHeight, int outputBits) {
    pp->error.clear();
    output.clear();
    outputWidth = 0;
    outputHeight = 0;

    if (pp->path.empty()) {
        pp->error = "develop failed: no RAW file is open";
        return LIBRAW_INPUT_CLOSED;
    }

    /* unpack()/dcraw_process() are a one-shot sequence in LibRaw. Reopen the
       saved path before a subsequent development so PNG/RGB output and grade
       retries work on the same processor instance. */
    if (pp->needsReload) {
        int ret = pp->raw.open_file(pp->path.c_str());
        if (ret != LIBRAW_SUCCESS) {
            pp->error = "reopen failed: ";
            pp->error += libraw_strerror(ret);
            return ret;
        }
        pp->needsReload = false;
    }

    /* Once unpack starts, retry through a clean reopen even when processing
       fails partway through. */
    pp->needsReload = true;

    LibRaw& raw = pp->raw;
    libraw_output_params_t& params = raw.imgdata.params;

    params.output_color = 1;  /* sRGB */
    params.output_bps = outputBits;
    params.output_tiff = 0;
    params.user_flip = -1;    /* respect metadata orientation */
    /* Do not let dcraw normalize every dark frame toward full brightness.
       That behavior lifts the night noise floor by several stops and causes
       frame-to-frame exposure pumping. Exposure belongs to the ramp. */
    params.no_auto_bright = 1;

    /* LibRaw expects a linear multiplier, while the public API is in EV stops.
       Passing EV directly made 0.5 EV darken by one stop and sent negative EV
       values outside LibRaw's valid range, which can create severe channel
       clipping and green/magenta flashes. */
    if (pp->grade.exposure != 0) {
        params.exp_correc = 1;
        params.exp_shift = std::pow(2.0, std::clamp(pp->grade.exposure, -2.0, 3.0));
        params.exp_preser = pp->grade.exposure > 0 ? 1.0 : 0.0;
    } else {
        params.exp_correc = 0;
        params.exp_shift = 1.0;
        params.exp_preser = 0.0;
    }

    /* White balance: Kelvin + tint, else camera WB. */
    if (pp->grade.temperature > 0) {
        double r, g, b;
        kelvinToRGB(pp->grade.temperature, r, g, b);
        /* Tint: positive -> green, negative -> magenta. */
        double tintShift = pp->grade.tint / 100.0;
        g *= (1.0 + tintShift);
        double mx = std::max({r, g, b});
        if (mx > 0) { r /= mx; g /= mx; b /= mx; }
        params.use_camera_wb = 0;
        params.use_auto_wb = 0;
        params.user_mul[0] = r;
        params.user_mul[1] = g;
        params.user_mul[2] = b;
        params.user_mul[3] = g;
    } else {
        params.use_camera_wb = 1;
    }

    /* Unpack the raw data (required before processing). */
    int ret = raw.unpack();
    if (ret != LIBRAW_SUCCESS) {
        pp->error = "unpack failed: ";
        pp->error += libraw_strerror(ret);
        return ret;
    }

    ret = raw.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        pp->error = "dcraw_process failed: ";
        pp->error += libraw_strerror(ret);
        return ret;
    }

    libraw_processed_image_t* image = raw.dcraw_make_mem_image(&ret);
    if (!image || ret != LIBRAW_SUCCESS) {
        pp->error = "dcraw_make_mem_image failed: ";
        pp->error += libraw_strerror(ret);
        if (image) raw.dcraw_clear_mem(image);
        return ret ? ret : LIBRAW_UNSPECIFIED_ERROR;
    }

    int W = image->width;
    int H = image->height;
    int sourceChans = image->colors;
    if (image->type != LIBRAW_IMAGE_BITMAP || image->bits != outputBits ||
        sizeof(Sample) * 8 != (size_t)outputBits || W <= 0 || H <= 0 ||
        (sourceChans != 1 && sourceChans < 3)) {
        pp->error = "LibRaw returned an unsupported image format";
        raw.dcraw_clear_mem(image);
        return LIBRAW_UNSUPPORTED_THUMBNAIL;
    }

    const size_t pixelCount = (size_t)W * (size_t)H;
    if (pixelCount > SIZE_MAX / (size_t)sourceChans ||
        pixelCount * (size_t)sourceChans > SIZE_MAX / sizeof(Sample) ||
        image->data_size < pixelCount * (size_t)sourceChans * sizeof(Sample) ||
        pixelCount > SIZE_MAX / 3) {
        pp->error = "LibRaw returned an invalid image size";
        raw.dcraw_clear_mem(image);
        return LIBRAW_DATA_ERROR;
    }

    /* Normalize all LibRaw bitmap variants to packed RGB. In particular,
       monochrome RAWs return one channel; merely pretending they have three
       causes out-of-bounds reads in grading and denoising. */
    const int chans = 3;
    const double maximum = (double)((1ULL << outputBits) - 1);
    auto sampleAt = [&](size_t index) {
        Sample value;
        std::memcpy(&value, image->data + index * sizeof(Sample), sizeof(Sample));
        return value;
    };
    std::vector<Sample> pixels(pixelCount * chans);
    if (sourceChans == 1) {
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const Sample value = sampleAt(pixel);
            pixels[pixel * 3] = value;
            pixels[pixel * 3 + 1] = value;
            pixels[pixel * 3 + 2] = value;
        }
    } else {
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            pixels[pixel * 3] = sampleAt(pixel * sourceChans);
            pixels[pixel * 3 + 1] = sampleAt(pixel * sourceChans + 1);
            pixels[pixel * 3 + 2] = sampleAt(pixel * sourceChans + 2);
        }
    }

    /* Remove color speckle and dark-region grain before grading amplifies it. */
    denoiseChroma(pixels.data(), W, H, chans, pp->denoise, maximum);
    denoiseLuma(pixels.data(), W, H, chans, pp->denoise, maximum);

    /* Post-demosaic color grading. */
    applyGrade(pixels.data(), W, H, chans, pp->grade, maximum);

    /* Downscale to max width if requested. */
    std::vector<Sample> scaled;
    Sample* finalPixels = pixels.data();
    int finalW = W, finalH = H;
    if (pp->maxWidth > 0 && W > (int)pp->maxWidth) {
        int newW = (int)pp->maxWidth;
        int newH = (int)((double)H * newW / W + 0.5);
        scaled.resize((size_t)newW * newH * chans);
        bilinearScale(pixels.data(), W, H, chans, scaled.data(), newW, newH, maximum);
        finalPixels = scaled.data();
        finalW = newW;
        finalH = newH;
    }

    // ffmpeg's rgb24 input requires exactly three tightly packed channels.
    const size_t finalPixelCount = (size_t)finalW * (size_t)finalH;
    output.assign(finalPixels, finalPixels + finalPixelCount * 3);
    outputWidth = finalW;
    outputHeight = finalH;
    raw.dcraw_clear_mem(image);
    return LIBRAW_SUCCESS;
}
}  // namespace

extern "C" {

libraw_processor* libraw_bridge_new(void) {
    return reinterpret_cast<libraw_processor*>(new Processor);
}

void libraw_bridge_free(libraw_processor* p) {
    delete reinterpret_cast<Processor*>(p);
}

int libraw_bridge_open_file(libraw_processor* p, const char* path) {
    Processor* pp = reinterpret_cast<Processor*>(p);
    pp->error.clear();
    if (!path) {
        pp->error = "open_file failed: null path";
        return LIBRAW_IO_ERROR;
    }
    int ret = pp->raw.open_file(path);
    if (ret != LIBRAW_SUCCESS) {
        pp->path.clear();
        pp->needsReload = false;
        pp->error = "open_file failed: ";
        pp->error += libraw_strerror(ret);
    } else {
        pp->path = path;
        pp->needsReload = false;
    }
    return ret;
}

void libraw_bridge_set_grade(libraw_processor* p, libraw_grade g) {
    reinterpret_cast<Processor*>(p)->grade = sanitizeGrade(g);
}

void libraw_bridge_set_max_width(libraw_processor* p, uint32_t w) {
    reinterpret_cast<Processor*>(p)->maxWidth = w;
}

void libraw_bridge_set_denoise(libraw_processor* p, double strength) {
    reinterpret_cast<Processor*>(p)->denoise =
        std::clamp(finiteOr(strength, 0.0), 0.0, 1.0);
}





int libraw_bridge_develop_png(libraw_processor* p, const char* out_path) {
    Processor* pp = reinterpret_cast<Processor*>(p);
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
    int ret = developRGB(pp, pixels, width, height, 8);
    if (ret != LIBRAW_SUCCESS) return ret;
    int ok = stbi_write_png(out_path, width, height, 3, pixels.data(), width * 3);
    if (!ok) {
        pp->error = "stbi_write_png failed";
        return LIBRAW_UNSPECIFIED_ERROR;
    }
    return LIBRAW_SUCCESS;
}

int libraw_bridge_develop_rgb(libraw_processor* p, libraw_rgb_image* out_image) {
    if (!out_image) return LIBRAW_UNSPECIFIED_ERROR;
    *out_image = {};
    Processor* pp = reinterpret_cast<Processor*>(p);
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
    int ret = developRGB(pp, pixels, width, height, 8);
    if (ret != LIBRAW_SUCCESS) return ret;

    uint8_t* data = static_cast<uint8_t*>(std::malloc(pixels.size()));
    if (!data) {
        pp->error = "unable to allocate RGB output";
        return LIBRAW_UNSUFFICIENT_MEMORY;
    }
    std::memcpy(data, pixels.data(), pixels.size());
    out_image->data = data;
    out_image->size = pixels.size();
    out_image->width = (uint32_t)width;
    out_image->height = (uint32_t)height;
    out_image->channels = 3;
    return LIBRAW_SUCCESS;
}


int libraw_bridge_develop_rgb16(libraw_processor* p, libraw_rgb16_image* out_image) {
    if (!out_image) return LIBRAW_UNSPECIFIED_ERROR;
    *out_image = {};
    Processor* pp = reinterpret_cast<Processor*>(p);
    std::vector<uint16_t> pixels;
    int width = 0, height = 0;
    int ret = developRGB(pp, pixels, width, height, 16);
    if (ret != LIBRAW_SUCCESS) return ret;

    if (pixels.size() > SIZE_MAX / sizeof(uint16_t)) {
        pp->error = "16-bit RGB output is too large";
        return LIBRAW_TOO_BIG;
    }
    const size_t byteCount = pixels.size() * sizeof(uint16_t);
    uint8_t* data = static_cast<uint8_t*>(std::malloc(byteCount));
    if (!data) {
        pp->error = "unable to allocate 16-bit RGB output";
        return LIBRAW_UNSUFFICIENT_MEMORY;
    }
    // Serialize explicitly as little-endian so the public buffer always matches
    // FFmpeg's rgb48le format, including on big-endian hosts.
    for (size_t index = 0; index < pixels.size(); ++index) {
        data[index * 2] = (uint8_t)(pixels[index] & 0xff);
        data[index * 2 + 1] = (uint8_t)(pixels[index] >> 8);
    }
    out_image->data = data;
    out_image->size = byteCount;
    out_image->width = (uint32_t)width;
    out_image->height = (uint32_t)height;
    out_image->channels = 3;
    return LIBRAW_SUCCESS;
}

void libraw_bridge_free_rgb(uint8_t* data) {
    std::free(data);
}

const char* libraw_bridge_last_error(libraw_processor* p) {
    Processor* pp = reinterpret_cast<Processor*>(p);
    return pp->error.empty() ? nullptr : pp->error.c_str();
}

const char* libraw_bridge_version(void) {
    return LibRaw::version();
}

}  // extern "C"
