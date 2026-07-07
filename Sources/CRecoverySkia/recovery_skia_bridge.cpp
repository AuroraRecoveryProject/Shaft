#include "recovery_skia_bridge.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <linux/input.h>
#include <linux/types.h>
#include <vector>

struct GRSurface {
    int width;
    int height;
    int row_bytes;
    int pixel_bytes;
    unsigned char *data;
    uint32_t format;
};

extern GRSurface *gr_draw;

int gr_init(void);
void gr_exit(void);
int gr_fb_width(void);
int gr_fb_height(void);
void gr_flip(void);
int ev_init(void);
void ev_exit(void);
int ev_get(struct input_event *ev, int timeout_ms);

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

constexpr uint32_t DRM_FORMAT_RGBA8888 = fourcc('R', 'A', '2', '4');
constexpr uint32_t DRM_FORMAT_RGBX8888 = fourcc('R', 'X', '2', '4');
constexpr uint32_t DRM_FORMAT_ABGR8888 = fourcc('A', 'B', '2', '4');
constexpr uint32_t DRM_FORMAT_XBGR8888 = fourcc('X', 'B', '2', '4');
constexpr uint32_t DRM_FORMAT_ARGB8888 = fourcc('A', 'R', '2', '4');
constexpr uint32_t DRM_FORMAT_XRGB8888 = fourcc('X', 'R', '2', '4');
constexpr uint32_t DRM_FORMAT_BGRA8888 = fourcc('B', 'A', '2', '4');
constexpr uint32_t DRM_FORMAT_BGRX8888 = fourcc('B', 'X', '2', '4');

std::vector<uint8_t> rgba_buffer;
int width = 0;
int height = 0;
int row_bytes = 0;
bool initialized = false;
bool input_initialized = false;
bool logged_present = false;
bool slot_active[16] = {};
bool legacy_touch_active = false;
int legacy_last_x = 0;
int legacy_last_y = 0;

int64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec / 1000);
}

bool profile_enabled() {
    static bool enabled = std::getenv("SHAFT_RECOVERY_PROFILE") != nullptr;
    return enabled;
}

enum class TouchTransform {
    autoRotate,
    none,
    rotate90,
    rotate270,
};

TouchTransform touch_transform = TouchTransform::autoRotate;

constexpr int kTouchPhaseDown = 1;
constexpr int kTouchPhaseMove = 2;
constexpr int kTouchPhaseUp = 3;
constexpr int kMinuitwrpMtDownMoveBase = 0x6000;
constexpr int kMinuitwrpMtUpBase = 0x6100;
constexpr int kMinuitwrpMaxMtSlots = 10;
constexpr int kLegacySlot = 0;
constexpr int kTouchPollBudget = 64;

int packed_x(int value) {
    return (value >> 16) & 0xffff;
}

int packed_y(int value) {
    return value & 0xffff;
}

TouchTransform parse_touch_transform() {
    const char *raw = std::getenv("SHAFT_RECOVERY_TOUCH_TRANSFORM");
    if (raw == nullptr || raw[0] == '\0' || std::strcmp(raw, "auto") == 0) {
        return TouchTransform::autoRotate;
    }
    if (std::strcmp(raw, "none") == 0) {
        return TouchTransform::none;
    }
    if (std::strcmp(raw, "rotate90") == 0 || std::strcmp(raw, "cw") == 0) {
        return TouchTransform::rotate90;
    }
    if (std::strcmp(raw, "rotate270") == 0 || std::strcmp(raw, "ccw") == 0) {
        return TouchTransform::rotate270;
    }
    std::fprintf(stderr, "[shaft-recovery-skia] unknown SHAFT_RECOVERY_TOUCH_TRANSFORM=%s; using auto\n", raw);
    return TouchTransform::autoRotate;
}

TouchTransform effective_touch_transform() {
    if (touch_transform != TouchTransform::autoRotate) {
        return touch_transform;
    }
    return width > height ? TouchTransform::rotate90 : TouchTransform::none;
}

void transform_touch_coordinates(int *x, int *y) {
    if (x == nullptr || y == nullptr || width <= 0 || height <= 0) {
        return;
    }

    const int raw_x = *x;
    const int raw_y = *y;
    switch (effective_touch_transform()) {
    case TouchTransform::none:
        break;
    case TouchTransform::rotate90:
        // minuitwrp has already scaled the touch samples into a portrait-shaped
        // physical coordinate space. Rotate that into the landscape framebuffer.
        *x = raw_y;
        *y = height - 1 - raw_x;
        break;
    case TouchTransform::rotate270:
        *x = width - 1 - raw_y;
        *y = raw_x;
        break;
    case TouchTransform::autoRotate:
        break;
    }

    if (*x < 0) {
        *x = 0;
    } else if (*x >= width) {
        *x = width - 1;
    }
    if (*y < 0) {
        *y = 0;
    } else if (*y >= height) {
        *y = height - 1;
    }
}

int fill_touch_event(RecoverySkiaTouchEvent *event, int phase, int slot, int x, int y) {
    if (event == nullptr) {
        return 0;
    }
    const int raw_x = x;
    const int raw_y = y;
    transform_touch_coordinates(&x, &y);
    event->phase = phase;
    event->slot = slot;
    event->x = x;
    event->y = y;
    (void)raw_x;
    (void)raw_y;
    return 1;
}

void copy_rgba_to_8888(uint8_t *dst, const uint8_t *src, int pixels, uint32_t format) {
    for (int x = 0; x < pixels; ++x) {
        const uint8_t r = src[x * 4 + 0];
        const uint8_t g = src[x * 4 + 1];
        const uint8_t b = src[x * 4 + 2];
        const uint8_t a = src[x * 4 + 3];
        uint8_t *out = dst + x * 4;

        // DRM fourcc names describe the 32-bit word; the CPU byte order here is little-endian.
        switch (format) {
        case DRM_FORMAT_RGBA8888:
            out[0] = a;
            out[1] = b;
            out[2] = g;
            out[3] = r;
            break;
        case DRM_FORMAT_RGBX8888:
            out[0] = 0xff;
            out[1] = b;
            out[2] = g;
            out[3] = r;
            break;
        case DRM_FORMAT_ABGR8888:
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
            break;
        case DRM_FORMAT_XBGR8888:
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = 0xff;
            break;
        case DRM_FORMAT_ARGB8888:
            out[0] = b;
            out[1] = g;
            out[2] = r;
            out[3] = a;
            break;
        case DRM_FORMAT_XRGB8888:
            out[0] = b;
            out[1] = g;
            out[2] = r;
            out[3] = 0xff;
            break;
        case DRM_FORMAT_BGRA8888:
            out[0] = a;
            out[1] = r;
            out[2] = g;
            out[3] = b;
            break;
        case DRM_FORMAT_BGRX8888:
            out[0] = 0xff;
            out[1] = r;
            out[2] = g;
            out[3] = b;
            break;
        default:
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
            break;
        }
    }
}

void copy_rgba_to_565(uint8_t *dst, const uint8_t *src, int pixels) {
    auto *out = reinterpret_cast<uint16_t *>(dst);
    for (int x = 0; x < pixels; ++x) {
        const uint8_t r = src[x * 4 + 0];
        const uint8_t g = src[x * 4 + 1];
        const uint8_t b = src[x * 4 + 2];
        out[x] = static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
}

void copy_rgba_buffer_to_gr_draw(const uint8_t *buffer, int src_width, int src_height, int src_row_bytes) {
    if (gr_draw == nullptr || gr_draw->data == nullptr || buffer == nullptr) {
        return;
    }

    const int copy_width = src_width < gr_draw->width ? src_width : gr_draw->width;
    const int copy_height = src_height < gr_draw->height ? src_height : gr_draw->height;
    const int dst_pixel_bytes = gr_draw->pixel_bytes;

    for (int y = 0; y < copy_height; ++y) {
        const uint8_t *src = buffer + y * src_row_bytes;
        uint8_t *dst = gr_draw->data + y * gr_draw->row_bytes;

        if (dst_pixel_bytes == 4) {
            copy_rgba_to_8888(dst, src, copy_width, gr_draw->format);
        } else if (dst_pixel_bytes == 2) {
            copy_rgba_to_565(dst, src, copy_width);
        } else {
            const size_t bytes = src_row_bytes < gr_draw->row_bytes ? src_row_bytes : gr_draw->row_bytes;
            std::memcpy(dst, src, bytes);
        }
    }
}

void copy_to_gr_draw() {
    if (rgba_buffer.empty()) {
        return;
    }
    copy_rgba_buffer_to_gr_draw(rgba_buffer.data(), width, height, row_bytes);
}

bool can_present_rgba_direct(const void *buffer, int src_row_bytes, int src_width, int src_height) {
    if (gr_draw == nullptr || buffer == nullptr || gr_draw->pixel_bytes != 4) {
        return false;
    }
    if (src_width != gr_draw->width || src_height != gr_draw->height || src_row_bytes != gr_draw->row_bytes) {
        return false;
    }
    // The recovery devices tested here report format=0x1 while accepting RGBA
    // bytes in memory. Known ABGR/XBGR fourcc scanout layouts are also RGBA in
    // little-endian byte order, with alpha either used or ignored.
    return gr_draw->format == 0x1 ||
           gr_draw->format == DRM_FORMAT_ABGR8888 ||
           gr_draw->format == DRM_FORMAT_XBGR8888;
}

} // namespace

int recovery_skia_init(void) {
    if (!initialized && gr_init() != 0) {
        return -1;
    }
    initialized = true;
    if (!input_initialized) {
        if (ev_init() == 0) {
            input_initialized = true;
        } else {
            std::fprintf(stderr, "[shaft-recovery-skia] ev_init failed; touch disabled\n");
        }
    }
    width = gr_draw != nullptr ? gr_draw->width : gr_fb_width();
    height = gr_draw != nullptr ? gr_draw->height : gr_fb_height();
    row_bytes = width * 4;
    if (width <= 0 || height <= 0 || row_bytes <= 0) {
        return -1;
    }
    touch_transform = parse_touch_transform();
    std::fprintf(stderr, "[shaft-recovery-skia] init fb=%dx%d row=%d draw=%p\n", width, height, row_bytes, gr_draw);
    std::fprintf(stderr, "[shaft-recovery-skia] touch transform=%s\n",
        effective_touch_transform() == TouchTransform::none ? "none" :
        effective_touch_transform() == TouchTransform::rotate90 ? "rotate90" :
        effective_touch_transform() == TouchTransform::rotate270 ? "rotate270" : "auto");
    if (gr_draw != nullptr) {
        std::fprintf(stderr, "[shaft-recovery-skia] draw=%dx%d row=%d pixel=%d format=0x%x data=%p\n",
            gr_draw->width, gr_draw->height, gr_draw->row_bytes, gr_draw->pixel_bytes, gr_draw->format, gr_draw->data);
    }
    rgba_buffer.assign(static_cast<size_t>(row_bytes) * static_cast<size_t>(height), 0);
    return 0;
}

void recovery_skia_shutdown(void) {
    rgba_buffer.clear();
    width = 0;
    height = 0;
    row_bytes = 0;
    std::memset(slot_active, 0, sizeof(slot_active));
    legacy_touch_active = false;
    legacy_last_x = 0;
    legacy_last_y = 0;
    if (input_initialized) {
        ev_exit();
        input_initialized = false;
    }
    if (initialized) {
        gr_exit();
        initialized = false;
    }
}

int recovery_skia_width(void) {
    return width;
}

int recovery_skia_height(void) {
    return height;
}

int recovery_skia_row_bytes(void) {
    return row_bytes;
}

void *recovery_skia_pixels(void) {
    return rgba_buffer.empty() ? nullptr : rgba_buffer.data();
}

void recovery_skia_present(void) {
    if (!logged_present) {
        std::fprintf(stderr, "[shaft-recovery-skia] present begin\n");
        logged_present = true;
    }
    const bool profile = profile_enabled();
    const int64_t t0 = profile ? now_us() : 0;
    if (can_present_rgba_direct(rgba_buffer.data(), row_bytes, width, height)) {
        unsigned char *saved_data = gr_draw->data;
        gr_draw->data = rgba_buffer.data();
        gr_flip();
        gr_draw->data = saved_data;
        if (profile) {
            const int64_t t1 = now_us();
            std::fprintf(
                stderr,
                "[shaft-recovery-skia:profile] present copy_us=0 flip_us=%lld total_us=%lld direct=1\n",
                static_cast<long long>(t1 - t0),
                static_cast<long long>(t1 - t0)
            );
        }
        return;
    }
    copy_to_gr_draw();
    const int64_t t1 = profile ? now_us() : 0;
    gr_flip();
    if (profile) {
        const int64_t t2 = now_us();
        std::fprintf(
            stderr,
            "[shaft-recovery-skia:profile] present copy_us=%lld flip_us=%lld total_us=%lld direct=0\n",
            static_cast<long long>(t1 - t0),
            static_cast<long long>(t2 - t1),
            static_cast<long long>(t2 - t0)
        );
    }
}

void recovery_skia_present_external_rgba(const void *pixels, int src_row_bytes, int src_width, int src_height) {
    if (!logged_present) {
        std::fprintf(stderr, "[shaft-recovery-skia] present begin\n");
        logged_present = true;
    }
    const bool profile = profile_enabled();
    const int64_t t0 = profile ? now_us() : 0;
    if (can_present_rgba_direct(pixels, src_row_bytes, src_width, src_height)) {
        unsigned char *saved_data = gr_draw->data;
        gr_draw->data = const_cast<unsigned char *>(static_cast<const unsigned char *>(pixels));
        gr_flip();
        gr_draw->data = saved_data;
        if (profile) {
            const int64_t t1 = now_us();
            std::fprintf(
                stderr,
                "[shaft-recovery-skia:profile] present copy_us=0 flip_us=%lld total_us=%lld direct=1\n",
                static_cast<long long>(t1 - t0),
                static_cast<long long>(t1 - t0)
            );
        }
        return;
    }
    copy_rgba_buffer_to_gr_draw(
        static_cast<const uint8_t *>(pixels),
        src_width,
        src_height,
        src_row_bytes
    );
    const int64_t t1 = profile ? now_us() : 0;
    gr_flip();
    if (profile) {
        const int64_t t2 = now_us();
        std::fprintf(
            stderr,
            "[shaft-recovery-skia:profile] present copy_us=%lld flip_us=%lld total_us=%lld direct=0\n",
            static_cast<long long>(t1 - t0),
            static_cast<long long>(t2 - t1),
            static_cast<long long>(t2 - t0)
        );
    }
}

void recovery_skia_log_marker(const char *message) {
    std::fprintf(stderr, "%s\n", message == nullptr ? "" : message);
}

void recovery_skia_log_touch_packet(int phase, int slot, int event_x, int event_y,
                                    int physical_x, int physical_y, float dpr,
                                    float logical_x, float logical_y) {
    (void)phase;
    (void)slot;
    (void)event_x;
    (void)event_y;
    (void)physical_x;
    (void)physical_y;
    (void)dpr;
    (void)logical_x;
    (void)logical_y;
}

int recovery_skia_poll_touch(RecoverySkiaTouchEvent *event) {
    if (!input_initialized || event == nullptr) {
        return 0;
    }

    struct input_event ev;
    for (int attempt = 0; attempt < kTouchPollBudget; ++attempt) {
        const int status = ev_get(&ev, 0);
        if (status == -2) {
            return 0;
        }
        if (status != 0) {
            continue;
        }

        if (ev.type == EV_ABS) {
            if (ev.code >= kMinuitwrpMtDownMoveBase &&
                ev.code < kMinuitwrpMtDownMoveBase + kMinuitwrpMaxMtSlots) {
                const int slot = ev.code - kMinuitwrpMtDownMoveBase;
                const int phase = slot_active[slot] ? kTouchPhaseMove : kTouchPhaseDown;
                slot_active[slot] = true;
                return fill_touch_event(event, phase, slot, packed_x(ev.value), packed_y(ev.value));
            }

            if (ev.code >= kMinuitwrpMtUpBase &&
                ev.code < kMinuitwrpMtUpBase + kMinuitwrpMaxMtSlots) {
                const int slot = ev.code - kMinuitwrpMtUpBase;
                slot_active[slot] = false;
                return fill_touch_event(event, kTouchPhaseUp, slot, packed_x(ev.value), packed_y(ev.value));
            }

            // Legacy minuitwrp path: code 1 carries packed screen coordinates,
            // code 0 is used by the bundled touch demo as release.
            if (ev.code == 1) {
                const int phase = legacy_touch_active ? kTouchPhaseMove : kTouchPhaseDown;
                legacy_touch_active = true;
                slot_active[kLegacySlot] = true;
                legacy_last_x = packed_x(ev.value);
                legacy_last_y = packed_y(ev.value);
                return fill_touch_event(event, phase, kLegacySlot, legacy_last_x, legacy_last_y);
            }
            if (ev.code == 0 && legacy_touch_active) {
                legacy_touch_active = false;
                slot_active[kLegacySlot] = false;
                return fill_touch_event(event, kTouchPhaseUp, kLegacySlot, legacy_last_x, legacy_last_y);
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0 && legacy_touch_active) {
            legacy_touch_active = false;
            slot_active[kLegacySlot] = false;
            return fill_touch_event(event, kTouchPhaseUp, kLegacySlot, legacy_last_x, legacy_last_y);
        }
    }
    return 0;
}
