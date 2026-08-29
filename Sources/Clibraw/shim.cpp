#include "libraw/libraw.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
    std::string error;
};

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

static inline unsigned char clamp8(double v) {
    return (unsigned char)std::clamp(v + 0.5, 0.0, 255.0);
}

/* Apply contrast/saturation/vibrance/shadows/highlights in sRGB space.
   Pixels are 8-bit, chans channels (3 or 4), row-major. */
static void applyGrade(unsigned char* data, int W, int H, int chans, const libraw_grade& g) {
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
        unsigned char* row = data + (size_t)y * stride;
        for (int x = 0; x < W; ++x) {
            unsigned char* px = row + (size_t)x * chans;
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

            /* Normalize to [0,1] for tone ops. */
            double rn = r / 255.0, gn = gg / 255.0, bn = b / 255.0;

            /* Shadows lift/crush: weight strongest in the darks. */
            if (shadows != 0) {
                double w = (1.0 - rn) * (1.0 - rn);
                rn = rn + shadows * w * (shadows > 0 ? (1.0 - rn) : rn);
                gn = gn + shadows * w * (shadows > 0 ? (1.0 - gn) : gn);
                bn = bn + shadows * w * (shadows > 0 ? (1.0 - bn) : bn);
            }

            /* Highlights recovery: weight strongest in the brights. */
            if (highlights > 0) {
                double w = rn * rn;
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

            px[0] = clamp8(rn * 255.0);
            px[1] = clamp8(gn * 255.0);
            px[2] = clamp8(bn * 255.0);
            if (hasAlpha) px[3] = px[3];
        }
    }
}

/* Simple bilinear downscale. */
static void bilinearScale(const unsigned char* src, int sw, int sh, int chans,
                          unsigned char* dst, int dw, int dh) {
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
                dst[((size_t)y * dw + x) * chans + c] = clamp8(v);
            }
        }
    }
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
    int ret = pp->raw.open_file(path);
    if (ret != LIBRAW_SUCCESS) {
        pp->error = "open_file failed: ";
        pp->error += libraw_strerror(ret);
    }
    return ret;
}

void libraw_bridge_set_grade(libraw_processor* p, libraw_grade g) {
    reinterpret_cast<Processor*>(p)->grade = g;
}

void libraw_bridge_set_max_width(libraw_processor* p, uint32_t w) {
    reinterpret_cast<Processor*>(p)->maxWidth = w;
}

int libraw_bridge_develop_png(libraw_processor* p, const char* out_path) {
    Processor* pp = reinterpret_cast<Processor*>(p);
    pp->error.clear();
    LibRaw& raw = pp->raw;
    libraw_output_params_t& params = raw.imgdata.params;

    params.output_color = 1;  /* sRGB */
    params.output_bps = 8;
    params.output_tiff = 0;
    params.user_flip = 0;     /* respect metadata orientation */

    /* Exposure shift in stops. */
    if (pp->grade.exposure != 0) {
        params.exp_correc = 1;
        params.exp_shift = pp->grade.exposure;
        params.exp_preser = 1;
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
    int chans = image->colors;
    if (chans < 3) chans = 3;

    std::vector<unsigned char> pixels(image->data, image->data + image->data_size);

    /* Post-demosaic color grading. */
    applyGrade(pixels.data(), W, H, chans, pp->grade);

    /* Downscale to max width if requested. */
    std::vector<unsigned char> scaled;
    unsigned char* finalPixels = pixels.data();
    int finalW = W, finalH = H;
    if (pp->maxWidth > 0 && W > (int)pp->maxWidth) {
        int newW = (int)pp->maxWidth;
        int newH = (int)((double)H * newW / W + 0.5);
        scaled.resize((size_t)newW * newH * chans);
        bilinearScale(pixels.data(), W, H, chans, scaled.data(), newW, newH);
        finalPixels = scaled.data();
        finalW = newW;
        finalH = newH;
    }

    int stride = finalW * chans;
    int ok = stbi_write_png(out_path, finalW, finalH, chans, finalPixels, stride);
    raw.dcraw_clear_mem(image);
    if (!ok) {
        pp->error = "stbi_write_png failed";
        return LIBRAW_UNSPECIFIED_ERROR;
    }
    return LIBRAW_SUCCESS;
}

const char* libraw_bridge_last_error(libraw_processor* p) {
    Processor* pp = reinterpret_cast<Processor*>(p);
    return pp->error.empty() ? nullptr : pp->error.c_str();
}

const char* libraw_bridge_version(void) {
    return LibRaw::version();
}

}  // extern "C"
