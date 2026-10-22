// SquachWatch-CYD — photo frame mode implementation
#include "ui_photo.h"
#include "ui_desk.h"
#include "ui_clear.h"
#include "clock.h"
#include "settings.h"
#include "theme.h"
#include "fast_sprite.h"
#include "frame_push.h"
#include "sd_log.h"
#include <Arduino.h>
#include <FS.h>
using fs::File;
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if defined(S3_BOX_3) || defined(FREENOVE_S3) || defined(FREENOVE_S3_35)
    #include <SD_MMC.h>
    #define PHOTO_FS SD_MMC
#else
    #include <SD.h>
    #define PHOTO_FS SD
#endif

#if __has_include(<rom/tjpgd.h>)
    #include <rom/tjpgd.h>
    #define SQW_HAS_TJPGD 1
#elif __has_include(<esp32s3/rom/tjpgd.h>)
    #include <esp32s3/rom/tjpgd.h>
    #define SQW_HAS_TJPGD 1
#else
    #define SQW_HAS_TJPGD 0
#endif

static bool cardMounted() { return PHOTO_FS.cardType() != CARD_NONE; }

static inline uint16_t rgb332To565(uint8_t c) {
    const uint8_t r3 = (c >> 5) & 7, g3 = (c >> 2) & 7, b2 = c & 3;
    return (uint16_t)(((r3 * 31 / 7) << 11) | ((g3 * 63 / 7) << 5) | (b2 * 31 / 3));
}

static const char* const PHOTO_DIR = "/photos";
static const uint16_t    MAX_PHOTOS = 128;
static const size_t      MAX_NAME   = 48;

struct PhotoEntry {
    char name[MAX_NAME];
};

static PhotoEntry* s_list       = nullptr;
static uint16_t    s_count      = 0;
static uint16_t    s_index      = 0;
static int16_t     s_loadedIdx  = -1;
static int32_t     s_loadedW    = 0;
static int32_t     s_loadedH    = 0;
static bool        s_loadedFill = true;
static bool        s_photoValid = false;
static bool        s_wireActive = false;

static uint8_t*    s_photo8     = nullptr;
static uint16_t*   s_photo565   = nullptr;
static int32_t     s_bufPixels  = 0;

static uint32_t    s_lastStepAt = 0;
static uint32_t    s_hudUntil   = 0;
static uint32_t    s_pillUntil  = 0;

static const uint32_t HUD_SHOW_MS  = 4500;
static const uint32_t PILL_SHOW_MS = 2500;

static inline uint16_t rgb888ToWire565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t rgb565 = ((uint16_t)(r & 0xF8) << 8) |
                            ((uint16_t)(g & 0xFC) << 3) |
                            ((uint16_t)b >> 3);
    return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

static inline uint8_t rgb888To332(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    const uint8_t r3 = (uint8_t)((r * 7 + 127) / 255);
    const uint8_t g3 = (uint8_t)((g * 7 + 127) / 255);
    const uint8_t b2 = (uint8_t)((b * 3 + 127) / 255);
    return (uint8_t)((r3 << 5) | (g3 << 2) | b2);
}

static bool hasSupportedExt(const char* name) {
    if (!name || name[0] == '.' || name[0] == '\0') return false;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".bmp") == 0;
}

static bool isBmpFile(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".bmp") == 0;
}

// Writes a 320x240 24-bit sample BMP postcard into /photos when the directory
// is first created on the SD card, so Photo Frame Mode works out of the box.
static void writeSampleBmp(const char* path, int variant) {
    if (PHOTO_FS.exists(path)) return;
    File f = PHOTO_FS.open(path, FILE_WRITE);
    if (!f) return;

    const int32_t w = 320, h = 240;
    const uint32_t rowBytes = (uint32_t)w * 3u;
    const uint32_t imgBytes = rowBytes * (uint32_t)h;
    const uint32_t fileSize = 54u + imgBytes;

    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(fileSize);
    hdr[3] = (uint8_t)(fileSize >> 8);
    hdr[4] = (uint8_t)(fileSize >> 16);
    hdr[5] = (uint8_t)(fileSize >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)(w);       hdr[19] = (uint8_t)(w >> 8);
    // Negative height = top-down DIB
    const int32_t negH = -h;
    hdr[22] = (uint8_t)(negH);    hdr[23] = (uint8_t)(negH >> 8);
    hdr[24] = (uint8_t)(negH >> 16); hdr[25] = (uint8_t)(negH >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = (uint8_t)(imgBytes);
    hdr[35] = (uint8_t)(imgBytes >> 8);
    hdr[36] = (uint8_t)(imgBytes >> 16);
    hdr[37] = (uint8_t)(imgBytes >> 24);
    f.write(hdr, sizeof hdr);

    uint8_t* row = (uint8_t*)malloc(rowBytes);
    if (!row) { f.close(); return; }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0;
            if (variant == 0) {
                // Synthwave sunset over ridge mountains
                if (y < 155) {
                    const float t = (float)y / 155.0f;
                    r = (uint8_t)(28.0f + t * 220.0f);
                    g = (uint8_t)(12.0f + (t < 0.6f ? t * 90.0f : (1.2f - t) * 110.0f));
                    b = (uint8_t)(75.0f + (1.0f - t) * 115.0f);
                    // Sun disc with horizontal scan slices
                    const int dx = x - 160, dy = y - 92;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < 46 * 46) {
                        const bool stripe = (dy > 8) && (((dy - 8) % 7) < 2);
                        if (!stripe) {
                            const float st = (float)(y - 46) / 92.0f;
                            r = 255;
                            g = (uint8_t)(220.0f - st * 140.0f);
                            b = (uint8_t)(60.0f + st * 95.0f);
                        }
                    }
                } else {
                    // Perspective cyber grid floor
                    const int gy = y - 155;
                    r = (uint8_t)(18 + gy / 4);
                    g = 8;
                    b = (uint8_t)(38 + gy / 2);
                    const bool hLine = (gy == 0) || (gy == 6) || (gy == 14) || (gy == 25) || (gy == 40) || (gy == 60) || (gy == 82);
                    const int vx = (x - 160) * 28 / (gy + 12);
                    const bool vLine = ((vx + 200) % 14) == 0;
                    if (hLine || vLine) {
                        r = 235; g = 45; b = 190;
                    }
                }
                // Mountain silhouette around y=155
                const int m1 = 155 - (42 - abs(x - 75) * 42 / 95);
                const int m2 = 155 - (54 - abs(x - 230) * 54 / 110);
                const int m3 = 155 - (30 - abs(x - 155) * 30 / 70);
                int mTop = 155;
                if (m1 < mTop) mTop = m1;
                if (m2 < mTop) mTop = m2;
                if (m3 < mTop) mTop = m3;
                if (y >= mTop && y < 155) {
                    r = 16; g = 8; b = 32;
                }
            } else {
                // Aurora Borealis over pine forest
                if (y < 170) {
                    const float t = (float)y / 170.0f;
                    r = (uint8_t)(6.0f + t * 18.0f);
                    g = (uint8_t)(14.0f + t * 38.0f);
                    b = (uint8_t)(34.0f + t * 56.0f);
                    // Aurora ribbon waves
                    const int waveY = 58 + (int)(18.0f * sinf((float)x * 0.035f) + 9.0f * cosf((float)x * 0.08f));
                    const int dist = y - waveY;
                    if (dist > -32 && dist < 44) {
                        const float a = 1.0f - (float)abs(dist) / (dist < 0 ? 32.0f : 44.0f);
                        const int gAdd = (int)(a * 175.0f);
                        const int bAdd = (int)(a * 110.0f);
                        const int rAdd = (int)(a * a * 65.0f);
                        r = (uint8_t)min(255, (int)r + rAdd);
                        g = (uint8_t)min(255, (int)g + gAdd);
                        b = (uint8_t)min(255, (int)b + bAdd);
                    }
                    // Stars
                    if (((x * 73 + y * 151) % 397) == 0 && y < 130) {
                        r = 240; g = 250; b = 255;
                    }
                } else {
                    // Reflective lake at bottom
                    const int ly = y - 170;
                    r = 8;
                    g = (uint8_t)(32 + (ly % 5 == 0 ? 28 : 0));
                    b = (uint8_t)(48 + (ly % 5 == 0 ? 32 : 0));
                }
                // Treeline silhouette along y=170
                const int treeX = x % 18;
                const int treeH = 18 + ((x / 18) * 13) % 22;
                const int canopy = 170 - (treeH - abs(treeX - 9) * treeH / 10);
                if (y >= canopy && y < 170) {
                    r = 6; g = 14; b = 12;
                }
            }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        f.write(row, rowBytes);
    }
    free(row);
    f.close();
}

static bool ensureBuffers(int32_t w, int32_t h) {
    const int32_t total = w * h;
    if (s_photo8 && s_photo565 && s_bufPixels == total) return true;
    if (s_photo8)   { heap_caps_free(s_photo8);   s_photo8 = nullptr; }
    if (s_photo565) { heap_caps_free(s_photo565); s_photo565 = nullptr; }
    s_bufPixels = 0;

    s_photo8   = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_photo565 = (uint16_t*)heap_caps_malloc((size_t)total * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_photo8)   s_photo8   = (uint8_t*)malloc((size_t)total);
    if (!s_photo565) s_photo565 = (uint16_t*)malloc((size_t)total * sizeof(uint16_t));
    if (!s_photo8 || !s_photo565) {
        if (s_photo8)   { free(s_photo8);   s_photo8 = nullptr; }
        if (s_photo565) { free(s_photo565); s_photo565 = nullptr; }
        return false;
    }
    s_bufPixels = total;
    return true;
}

struct ScaleMap {
    int16_t* sx;
    int16_t* sy;
    int32_t  dstW;
    int32_t  dstH;
};

static bool buildScaleMap(ScaleMap& m, int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH, bool fill) {
    m.dstW = dstW;
    m.dstH = dstH;
    m.sx = (int16_t*)malloc((size_t)dstW * sizeof(int16_t));
    m.sy = (int16_t*)malloc((size_t)dstH * sizeof(int16_t));
    if (!m.sx || !m.sy) {
        if (m.sx) free(m.sx);
        if (m.sy) free(m.sy);
        m.sx = m.sy = nullptr;
        return false;
    }
    for (int32_t x = 0; x < dstW; x++) m.sx[x] = -1;
    for (int32_t y = 0; y < dstH; y++) m.sy[y] = -1;

    if (fill) {
        // Cover-crop: scale so image covers full dstW x dstH, centered
        int32_t cropW = srcW, cropH = srcH;
        if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
            cropW = (int32_t)(((int64_t)srcH * dstW) / dstH);
        } else {
            cropH = (int32_t)(((int64_t)srcW * dstH) / dstW);
        }
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        const int32_t cropX = (srcW - cropW) / 2;
        const int32_t cropY = (srcH - cropH) / 2;
        for (int32_t x = 0; x < dstW; x++) {
            int32_t s = cropX + (int32_t)(((int64_t)x * cropW) / dstW);
            if (s < 0) s = 0; else if (s >= srcW) s = srcW - 1;
            m.sx[x] = (int16_t)s;
        }
        for (int32_t y = 0; y < dstH; y++) {
            int32_t s = cropY + (int32_t)(((int64_t)y * cropH) / dstH);
            if (s < 0) s = 0; else if (s >= srcH) s = srcH - 1;
            m.sy[y] = (int16_t)s;
        }
    } else {
        // Fit whole image inside dstW x dstH with black letterbox/pillarbox bars
        int32_t fitW = dstW, fitH = dstH;
        if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
            fitH = (int32_t)(((int64_t)dstW * srcH) / srcW);
        } else {
            fitW = (int32_t)(((int64_t)dstH * srcW) / srcH);
        }
        if (fitW < 1) fitW = 1;
        if (fitH < 1) fitH = 1;
        const int32_t offX = (dstW - fitW) / 2;
        const int32_t offY = (dstH - fitH) / 2;
        for (int32_t x = 0; x < fitW; x++) {
            int32_t s = (int32_t)(((int64_t)x * srcW) / fitW);
            if (s < 0) s = 0; else if (s >= srcW) s = srcW - 1;
            if (offX + x >= 0 && offX + x < dstW) m.sx[offX + x] = (int16_t)s;
        }
        for (int32_t y = 0; y < fitH; y++) {
            int32_t s = (int32_t)(((int64_t)y * srcH) / fitH);
            if (s < 0) s = 0; else if (s >= srcH) s = srcH - 1;
            if (offY + y >= 0 && offY + y < dstH) m.sy[offY + y] = (int16_t)s;
        }
    }
    return true;
}

static void freeScaleMap(ScaleMap& m) {
    if (m.sx) { free(m.sx); m.sx = nullptr; }
    if (m.sy) { free(m.sy); m.sy = nullptr; }
}

// Converts the w x h RGB888 staging buffer into:
//   1. s_photo565 (true 16-bit RGB565 wire words)
//   2. s_photo8   (Floyd-Steinberg error-diffusion dithered RGB332)
static void finalizeRgbBuffer(const uint8_t* rgb, int32_t w, int32_t h) {
    // Ordered 2x2 Bayer dither + wire565 conversion: fast, zero heap allocation,
    // and completely free of serpentine scan artifacts.
    static const int8_t BAYER4[4] = { -12, 12, 18, -6 };
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* srcRow = rgb + (size_t)y * (size_t)w * 3u;
        uint8_t*       dst8   = s_photo8   + (size_t)y * (size_t)w;
        uint16_t*      dst565 = s_photo565 + (size_t)y * (size_t)w;
        const int      bRow   = (y & 1) << 1;
        for (int32_t x = 0; x < w; x++) {
            const uint8_t r = srcRow[x * 3 + 0];
            const uint8_t g = srcRow[x * 3 + 1];
            const uint8_t b = srcRow[x * 3 + 2];
            dst565[x] = rgb888ToWire565(r, g, b);
            const int d = BAYER4[bRow | (x & 1)];
            dst8[x]   = rgb888To332((int)r + d, (int)g + d, (int)b + (d * 2));
        }
    }
}

#if SQW_HAS_TJPGD
struct JpgCtx {
    File      file;
    uint8_t*  rgb;
    ScaleMap* map;
};

static UINT jpgInFunc(JDEC* jd, BYTE* buf, UINT nbyte) {
    JpgCtx* ctx = (JpgCtx*)jd->device;
    if (!buf) {
        ctx->file.seek(ctx->file.position() + nbyte);
        return nbyte;
    }
    return (UINT)ctx->file.read(buf, nbyte);
}

static UINT jpgOutFunc(JDEC* jd, void* bitmap, JRECT* rect) {
    JpgCtx* ctx = (JpgCtx*)jd->device;
    const ScaleMap& m = *ctx->map;
    const uint8_t* src = (const uint8_t*)bitmap;
    const int32_t bw = (int32_t)(rect->right - rect->left + 1);

    for (int32_t dy = 0; dy < m.dstH; dy++) {
        const int16_t sy = m.sy[dy];
        if (sy < (int16_t)rect->top || sy > (int16_t)rect->bottom) continue;
        const uint8_t* srcRow = src + (size_t)(sy - rect->top) * (size_t)bw * 3u;
        uint8_t* dstRow = ctx->rgb + (size_t)dy * (size_t)m.dstW * 3u;
        for (int32_t dx = 0; dx < m.dstW; dx++) {
            const int16_t sx = m.sx[dx];
            if (sx < (int16_t)rect->left || sx > (int16_t)rect->right) continue;
            const uint8_t* p = srcRow + (size_t)(sx - rect->left) * 3u;
            dstRow[dx * 3 + 0] = p[0];
            dstRow[dx * 3 + 1] = p[1];
            dstRow[dx * 3 + 2] = p[2];
        }
    }
    return 1;
}

static bool decodeJpgFile(File& f, uint8_t* rgb, int32_t dstW, int32_t dstH, bool fill) {
    const size_t poolSz = 4096;
    void* pool = malloc(poolSz);
    if (!pool) return false;

    JpgCtx ctx = { f, rgb, nullptr };
    JDEC jdec;
    if (jd_prepare(&jdec, jpgInFunc, pool, poolSz, &ctx) != JDR_OK || jdec.width == 0 || jdec.height == 0) {
        free(pool);
        return false;
    }

    uint8_t scale = 0;
    while (scale < 3 && (int32_t)(jdec.width >> (scale + 1)) >= dstW && (int32_t)(jdec.height >> (scale + 1)) >= dstH) {
        scale++;
    }
    const int32_t jw = max<int32_t>(1, (int32_t)(jdec.width >> scale));
    const int32_t jh = max<int32_t>(1, (int32_t)(jdec.height >> scale));

    ScaleMap map = {};
    if (!buildScaleMap(map, jw, jh, dstW, dstH, fill)) {
        free(pool);
        return false;
    }
    ctx.map = &map;
    const JRESULT rc = jd_decomp(&jdec, jpgOutFunc, scale);
    freeScaleMap(map);
    free(pool);
    return rc == JDR_OK;
}
#endif

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool decodeBmpFile(File& f, uint8_t* rgb, int32_t dstW, int32_t dstH, bool fill) {
    uint8_t hdr[54];
    if (f.read(hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') return false;
    const uint32_t dataOff = rd32(hdr + 10);
    const int32_t  srcW    = (int32_t)rd32(hdr + 18);
    int32_t        srcH    = (int32_t)rd32(hdr + 22);
    const uint16_t bpp     = rd16(hdr + 28);
    const uint32_t comp    = rd32(hdr + 30);
    if (srcW <= 0 || srcH == 0 || (bpp != 16 && bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) return false;

    const bool topDown = (srcH < 0);
    if (topDown) srcH = -srcH;

    ScaleMap map = {};
    if (!buildScaleMap(map, srcW, srcH, dstW, dstH, fill)) return false;

    const uint32_t bytesPerPx = bpp / 8u;
    const uint32_t rowStride  = ((uint32_t)srcW * bytesPerPx + 3u) & ~3u;
    uint8_t* srcRow = (uint8_t*)malloc(rowStride);
    if (!srcRow) { freeScaleMap(map); return false; }

    int32_t lastSy = -1;
    for (int32_t dy = 0; dy < dstH; dy++) {
        const int16_t sy = map.sy[dy];
        if (sy < 0) continue;
        if (sy != lastSy) {
            const uint32_t fileRow = topDown ? (uint32_t)sy : (uint32_t)(srcH - 1 - sy);
            f.seek(dataOff + fileRow * rowStride);
            if (f.read(srcRow, rowStride) != rowStride) break;
            lastSy = sy;
        }
        uint8_t* dstRow = rgb + (size_t)dy * (size_t)dstW * 3u;
        for (int32_t dx = 0; dx < dstW; dx++) {
            const int16_t sx = map.sx[dx];
            if (sx < 0) continue;
            const uint8_t* p = srcRow + (size_t)sx * bytesPerPx;
            uint8_t r = 0, g = 0, b = 0;
            if (bpp == 24 || bpp == 32) {
                b = p[0]; g = p[1]; r = p[2];
            } else {
                const uint16_t v = rd16(p);
                r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
                g = (uint8_t)(((v >> 5)  & 0x3F) * 255 / 63);
                b = (uint8_t)((v & 0x1F) * 255 / 31);
            }
            dstRow[dx * 3 + 0] = r;
            dstRow[dx * 3 + 1] = g;
            dstRow[dx * 3 + 2] = b;
        }
    }
    free(srcRow);
    freeScaleMap(map);
    return true;
}

static bool loadPhotoAtIndex(uint16_t idx, int32_t w, int32_t h, bool fill) {
    if (!cardMounted() || idx >= s_count || !s_list) return false;
    if (!ensureBuffers(w, h)) return false;

    const size_t rgbBytes = (size_t)w * (size_t)h * 3u;
    uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) rgb = (uint8_t*)malloc(rgbBytes);
    if (!rgb) return false;
    memset(rgb, 0, rgbBytes);

    char path[80];
    snprintf(path, sizeof path, "%s/%s", PHOTO_DIR, s_list[idx].name);
    File f = PHOTO_FS.open(path, FILE_READ);
    bool ok = false;
    if (f) {
        if (isBmpFile(s_list[idx].name)) {
            ok = decodeBmpFile(f, rgb, w, h, fill);
        }
#if SQW_HAS_TJPGD
        else {
            ok = decodeJpgFile(f, rgb, w, h, fill);
        }
#endif
        f.close();
    }
    if (ok) {
        finalizeRgbBuffer(rgb, w, h);
    }
    free(rgb);
    return ok;
}

void uiPhotoRescan() {
    s_count = 0;
    s_loadedIdx = -1;
    s_photoValid = false;
    if (!cardMounted()) return;

    if (!s_list) {
        s_list = (PhotoEntry*)heap_caps_malloc(sizeof(PhotoEntry) * MAX_PHOTOS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_list) s_list = (PhotoEntry*)malloc(sizeof(PhotoEntry) * MAX_PHOTOS);
        if (!s_list) return;
    }

    if (!PHOTO_FS.exists(PHOTO_DIR)) {
        PHOTO_FS.mkdir(PHOTO_DIR);
    }

    // First pass: check if any photos exist; if empty, generate two starter postcards
    for (int pass = 0; pass < 2; pass++) {
        s_count = 0;
        File dir = PHOTO_FS.open(PHOTO_DIR);
        if (!dir || !dir.isDirectory()) return;
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (!entry.isDirectory()) {
                const char* rawName = entry.name();
                const char* base = strrchr(rawName, '/');
                base = base ? (base + 1) : rawName;
                if (hasSupportedExt(base) && s_count < MAX_PHOTOS) {
                    snprintf(s_list[s_count].name, MAX_NAME, "%s", base);
                    s_count++;
                }
            }
            entry.close();
        }
        dir.close();
        if (s_count > 0 || pass == 1) break;
        writeSampleBmp("/photos/squachy-sunset.bmp", 0);
        writeSampleBmp("/photos/squachy-aurora.bmp", 1);
    }

    // Insertion sort alphabetically so slideshow order is predictable
    for (uint16_t i = 1; i < s_count; i++) {
        PhotoEntry tmp = s_list[i];
        int j = (int)i - 1;
        while (j >= 0 && strcasecmp(s_list[j].name, tmp.name) > 0) {
            s_list[j + 1] = s_list[j];
            j--;
        }
        s_list[j + 1] = tmp;
    }
    if (s_index >= s_count) s_index = 0;
}

uint16_t uiPhotoCount() {
    return s_count;
}

void uiPhotoInit(TFT_eSPI& t, uint32_t now) {
    (void)t;
    uiPhotoRescan();
    s_lastStepAt = now;
    s_hudUntil   = now + HUD_SHOW_MS;
    s_pillUntil  = now + HUD_SHOW_MS;
}

void uiPhotoStep(int dir, uint32_t now) {
    s_lastStepAt = now;
    s_pillUntil  = now + PILL_SHOW_MS;
    if (s_count <= 1) return;
    if (Settings::photoShuffle() && dir > 0 && s_count > 2) {
        const uint16_t next = (uint16_t)((s_index + 1 + (esp_random() % (s_count - 1))) % s_count);
        s_index = next;
    } else if (dir >= 0) {
        s_index = (uint16_t)((s_index + 1) % s_count);
    } else {
        s_index = (uint16_t)((s_index + s_count - 1) % s_count);
    }
    s_loadedIdx = -1;
}

bool uiPhotoHudUp(uint32_t now) {
    return s_count == 0 || (int32_t)(s_hudUntil - now) > 0;
}

void uiPhotoToggleHud(uint32_t now) {
    if (uiPhotoHudUp(now) && s_count > 0) {
        s_hudUntil  = now;
        s_pillUntil = now;
    } else {
        s_hudUntil  = now + HUD_SHOW_MS;
        s_pillUntil = now + HUD_SHOW_MS;
    }
}

static void gearRect(int screenW, int screenH, int& gx, int& gy, int& gs) {
    const Theme::ButtonBarGeom bar = Theme::computeButtonBar(screenW, screenH);
    gs = bar.h;
    gx = 4;
    gy = bar.y;
}

static void barRects(int screenW, int screenH, int& nx, int& ny, int& nw, int& nh, int& bx, int& bw) {
    const Theme::ButtonBarGeom bar = Theme::computeButtonBar(screenW, screenH);
    int gx, gy, gs;
    gearRect(screenW, screenH, gx, gy, gs);
    const int left = gx + gs + 4;
    const int gap  = 4;
    const int room = screenW - left - 4 - gap;
    bw = room * 36 / 100;
    nw = room - bw;
    nx = left;
    bx = left + nw + gap;
    ny = bar.y;
    nh = bar.h;
}

static void drawGearIcon(TFT_eSPI& t, int cx, int cy, int r, uint16_t fg, uint16_t bg) {
    static const int8_t DX[8] = {  10,  7,  0, -7, -10, -7,   0,  7 };
    static const int8_t DY[8] = {   0,  7, 10,  7,   0, -7, -10, -7 };
    for (int i = 0; i < 8; i++) {
        const int x = cx + (DX[i] * r + (DX[i] >= 0 ? 5 : -5)) / 10;
        const int y = cy + (DY[i] * r + (DY[i] >= 0 ? 5 : -5)) / 10;
        t.fillRect(x - 1, y - 1, 3, 3, fg);
    }
    t.fillCircle(cx, cy, r - 2, fg);
    t.fillCircle(cx, cy, r / 2 - 1, bg);
}

void uiPhotoTick(TFT_eSPI& t, uint32_t now, const DetectionEngine& eng) {
    (void)eng;
    const int32_t w = t.width(), h = t.height();
    const bool fill = Settings::photoFill();

    // Auto-advance slideshow timer
    const uint16_t ivSec = Settings::photoIntervalSec();
    if (ivSec > 0 && s_count > 1 && (uint32_t)(now - s_lastStepAt) >= (uint32_t)ivSec * 1000UL) {
        uiPhotoStep(1, now);
    }

    // Decode photo into PSRAM buffers when index, dimensions, or aspect mode changes
    if (s_count > 0 && (s_loadedIdx != (int16_t)s_index || s_loadedW != w || s_loadedH != h || s_loadedFill != fill)) {
        s_photoValid = loadPhotoAtIndex(s_index, w, h, fill);
        s_loadedIdx  = (int16_t)s_index;
        s_loadedW    = w;
        s_loadedH    = h;
        s_loadedFill = fill;
        FramePush::invalidate();
    }

    // 1. Copy decoded photo background into the frame sprite
    if (s_photoValid && s_photo8 && s_loadedW == w && s_loadedH == h) {
        FastSprite* fsp = (g_frameSprite && static_cast<TFT_eSPI*>(g_frameSprite) == &t &&
                           g_frameSprite->buf() && g_frameSprite->bufW() == w) ? g_frameSprite : nullptr;
        if (fsp) {
            // Straight into the 8-bit frame; rows outside this band come back null.
            for (int32_t y = 0; y < h; y++) {
                uint8_t* dst = const_cast<uint8_t*>(fsp->row8(y));
                if (dst) memcpy(dst, s_photo8 + (size_t)y * (size_t)w, (size_t)w);
            }
        } else {
            for (int32_t y = 0; y < h; y++) {
                const uint8_t* row = s_photo8 + (size_t)y * (size_t)w;
                for (int32_t x = 0; x < w; x++) {
                    t.drawPixel(x, y, rgb332To565(row[x]));
                }
            }
        }
    } else {
        t.fillRect(0, 0, w, h, Theme::BG);
        t.setTextSize(1);
        t.setTextColor(Theme::CYAN, Theme::BG);
        const char* h1 = cardMounted() ? "NO PHOTOS IN /photos" : "SD CARD NOT MOUNTED";
        t.setCursor((w - t.textWidth(h1)) / 2, h / 2 - 18);
        t.print(h1);
        t.setTextColor(Theme::WHITE, Theme::BG);
        const char* h2 = "Put .jpg or .bmp images in /photos";
        t.setCursor((w - t.textWidth(h2)) / 2, h / 2 - 2);
        t.print(h2);
        t.setTextColor(Theme::W95_SHADOW, Theme::BG);
        const char* h3 = "Tap center for controls or BACK";
        t.setCursor((w - t.textWidth(h3)) / 2, h / 2 + 12);
        t.print(h3);
    }

    // 2. Optional Corner Clock & Dock Battery Gauge Overlay
    if (Settings::photoClock()) {
        uiClearDrawBatteryGauge(t, now);
        bool pm = false;
        char tm[12];
        const bool twelveHour = !Settings::clock24Hour();
        Clock::formatTime(tm, sizeof tm, twelveHour, &pm);
        char clkBuf[16];
        if (twelveHour) snprintf(clkBuf, sizeof clkBuf, "%s %s", tm, pm ? "PM" : "AM");
        else            snprintf(clkBuf, sizeof clkBuf, "%s", tm);

        t.setTextSize(1);
        const int tw = t.textWidth(clkBuf);
        const int pw = tw + 10;
        const int ph = 15;
        const int px = w - pw - 4;
        const int py = 3;
        t.fillRoundRect(px, py, pw, ph, 4, Theme::BG);
        t.drawRoundRect(px, py, pw, ph, 4, Theme::W95_SHADOW);
        t.setTextColor(Clock::trusted() ? Theme::WHITE : Theme::W95_SHADOW, Theme::BG);
        t.setCursor(px + 5, py + 4);
        t.print(clkBuf);
    }

    // 3. Optional Compact Detection Alert Card Overlay
    const Theme::ButtonBarGeom bar = Theme::computeButtonBar(w, h);
    if (Settings::photoAlerts()) {
        uiDeskDrawAlertCard(t, bar.y, now, false, 4);
    }

    // 4. Top photo counter/filename pill (shown briefly on step or when HUD is up)
    const bool hudUp  = uiPhotoHudUp(now);
    const bool pillUp = hudUp || ((int32_t)(s_pillUntil - now) > 0);
    if (pillUp && s_count > 0 && s_list) {
        char info[64];
        snprintf(info, sizeof info, "%u/%u  %.22s", (unsigned)(s_index + 1), (unsigned)s_count, s_list[s_index].name);
        t.setTextSize(1);
        const int iw = t.textWidth(info) + 12;
        const int ih = 15;
        const int ix = (w - iw) / 2;
        const int iy = 3;
        t.fillRoundRect(ix, iy, iw, ih, 4, Theme::BG);
        t.drawRoundRect(ix, iy, iw, ih, 4, Theme::CYAN);
        t.setTextColor(Theme::CYAN, Theme::BG);
        t.setCursor(ix + 6, iy + 4);
        t.print(info);
    }

    // 5. Auto-hiding bottom HUD bar (Gear, Interval/Next, BACK)
    if (hudUp) {
        int nx, ny, nw, nh, bx, bw;
        barRects(w, h, nx, ny, nw, nh, bx, bw);
        char nextLbl[24];
        snprintf(nextLbl, sizeof nextLbl, "NEXT (%s)", Settings::photoIntervalName());
        Theme::drawButton(t, nx, ny, nw, nh, nextLbl, false);
        Theme::drawButton(t, bx, ny, bw, nh, "BACK", false);

        int gx, gy, gs;
        gearRect(w, h, gx, gy, gs);
        Theme::drawButton(t, gx, gy, gs, gs, "", false);
        drawGearIcon(t, gx + gs / 2, gy + gs / 2, gs / 2 - 5, Theme::CYAN, Theme::BG);
    }
}

bool uiPhotoHitBack(int x, int y, int screenW, int screenH, uint32_t now) {
    if (!uiPhotoHudUp(now)) return false;
    int nx, ny, nw, nh, bx, bw;
    barRects(screenW, screenH, nx, ny, nw, nh, bx, bw);
    return x >= bx && x <= bx + bw && y >= ny && y <= ny + nh;
}

bool uiPhotoHitSettings(int x, int y, int screenW, int screenH, uint32_t now) {
    if (!uiPhotoHudUp(now)) return false;
    int gx, gy, gs;
    gearRect(screenW, screenH, gx, gy, gs);
    return x >= gx && x <= gx + gs && y >= gy && y <= gy + gs;
}

bool uiPhotoHitNext(int x, int y, int screenW, int screenH, uint32_t now) {
    if (!uiPhotoHudUp(now)) return false;
    int nx, ny, nw, nh, bx, bw;
    barRects(screenW, screenH, nx, ny, nw, nh, bx, bw);
    return x >= nx && x <= nx + nw && y >= ny && y <= ny + nh;
}

int uiPhotoHitEdge(int x, int y, int screenW, int screenH) {
    (void)y; (void)screenH;
    const int edge = screenW * 22 / 100;
    if (x < edge) return -1;
    if (x >= screenW - edge) return 1;
    return 0;
}

uint8_t uiPhotoStops(int screenW, int screenH, uint32_t now, int16_t* xs, int16_t* ys, uint8_t cap) {
    (void)now;
    uint8_t n = 0;
    auto add = [&](int x, int y) { if (n < cap) { xs[n] = (int16_t)x; ys[n] = (int16_t)y; n++; } };
    int nx, ny, nw, nh, bx, bw;
    barRects(screenW, screenH, nx, ny, nw, nh, bx, bw);
    add(nx + nw / 2, ny + nh / 2);
    add(bx + bw / 2, ny + nh / 2);
    int gx, gy, gs;
    gearRect(screenW, screenH, gx, gy, gs);
    add(gx + gs / 2, gy + gs / 2);
    return n;
}

void uiPhotoSetWireActive(bool on) {
    if (s_wireActive != on) {
        s_wireActive = on;
        FramePush::invalidate();
    }
}

const uint16_t* uiPhotoWire565(const uint8_t*& match8, int32_t w, int32_t h) {
    if (!s_wireActive || !s_photoValid || !s_photo8 || !s_photo565 || s_loadedW != w || s_loadedH != h) {
        match8 = nullptr;
        return nullptr;
    }
    match8 = s_photo8;
    return s_photo565;
}

