#ifndef LIBRAW_BRIDGE_H
#define LIBRAW_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libraw_processor libraw_processor;

typedef struct {
    double exposure;     /* stops, 0 = unchanged */
    double temperature;  /* Kelvin, <= 0 = use camera WB */
    double tint;         /* green(+) / magenta(-), 0 = unchanged */
    double contrast;     /* 1.0 = neutral */
    double saturation;   /* 1.0 = neutral */
    double vibrance;     /* 0 = neutral, range ~ -1..1 */
    double shadows;      /* -1..1, 0 = neutral (lift/crush blacks) */
    double highlights;   /* 0..1, 0 = neutral (recover brights) */
} libraw_grade;

/* Lifecycle */
libraw_processor* libraw_bridge_new(void);
void libraw_bridge_free(libraw_processor* p);

/* Open a RAW/DNG file. Returns 0 on success, LibRaw error code otherwise. */
int libraw_bridge_open_file(libraw_processor* p, const char* path);

/* Set grading parameters applied during develop. */
void libraw_bridge_set_grade(libraw_processor* p, libraw_grade grade);

/* Set maximum output width; 0 keeps native size. */
void libraw_bridge_set_max_width(libraw_processor* p, uint32_t width);

/* Develop the loaded RAW and write an 8-bit sRGB PNG to out_path.
   Returns 0 on success. */
int libraw_bridge_develop_png(libraw_processor* p, const char* out_path);

/* Last error message, or NULL if no error. Valid until next call. */
const char* libraw_bridge_last_error(libraw_processor* p);

/* LibRaw version string (static). */
const char* libraw_bridge_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRAW_BRIDGE_H */
