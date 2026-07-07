#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int recovery_skia_init(void);
void recovery_skia_shutdown(void);

int recovery_skia_width(void);
int recovery_skia_height(void);
int recovery_skia_row_bytes(void);
void *recovery_skia_pixels(void);

void recovery_skia_present(void);
void recovery_skia_present_external_rgba(const void *pixels, int row_bytes, int width, int height);
void recovery_skia_log_marker(const char *message);
void recovery_skia_log_touch_packet(int phase, int slot, int event_x, int event_y,
                                    int physical_x, int physical_y, float dpr,
                                    float logical_x, float logical_y);

typedef struct RecoverySkiaTouchEvent {
    int phase;
    int slot;
    int x;
    int y;
} RecoverySkiaTouchEvent;

int recovery_skia_poll_touch(RecoverySkiaTouchEvent *event);

#ifdef __cplusplus
}
#endif
