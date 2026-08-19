#ifndef CLASSICUBE_HOLO_PORT_H
#define CLASSICUBE_HOLO_PORT_H

#include "module_abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLASSICUBE_RENDER_WIDTH   320
#define CLASSICUBE_RENDER_HEIGHT  240
#define CLASSICUBE_3D_WIDTH       192
#define CLASSICUBE_3D_HEIGHT      144
#define CLASSICUBE_DMA_ROWS        10
#define CLASSICUBE_DISPLAY_WIDTH  320
#define CLASSICUBE_DISPLAY_HEIGHT 240
#define CLASSICUBE_UI_DPI_SCALE   0.50f
#define CLASSICUBE_PATH_MAX      256

typedef struct HoloInputState {
    uint32_t buttons;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
} HoloInputState;

/* Fills one native-width RGB565 DMA band. The callback runs on the Core 0
 * presenter task, so context must remain immutable until the next frame is
 * accepted (or until shutdown). */
typedef void (*HoloPresentBandFn)(void *context, uint16_t *dst,
                                  int dst_y, int rows);

void holo_port_set_host(const module_host_api_v2 *host);
const module_host_api_v2 *holo_port_host(void);

void holo_port_set_root(const char *root);
const char *holo_port_root(void);

void holo_port_set_input(uint32_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
HoloInputState holo_port_input(void);
void holo_port_reset_input(void);

void holo_port_request_stop(void);
void holo_port_clear_stop(void);
bool holo_port_stop_requested(void);

bool holo_port_display_acquire(void);
void holo_port_display_release(void);
void *holo_port_display_surface(void);
void *holo_port_buffer(size_t bytes, unsigned index);
void *holo_port_color_band(unsigned index);
unsigned holo_port_color_band_rows(void);

/* Queues a frame whose 320x240 output is produced a 10-row DMA band at a time.
 * This is used by the low-resolution renderer to fuse upscale and HUD overlay
 * without materialising a full native-resolution framebuffer in PSRAM. */
bool holo_window_present_composed(void *context, HoloPresentBandFn compose);
/* Waits until the queued composed frame no longer references renderer-owned
 * textures/caches. Intended for rare resource rebuild/destruction paths. */
void holo_window_wait_present(void);

bool holo_port_hot_vb_reserve(void);
void *holo_port_hot_vb_alloc(size_t bytes);
bool holo_port_hot_vb_free(void *memory);
void holo_port_hot_vb_release(void);
uint32_t holo_port_hot_vb_reserved(void);
uint32_t holo_port_hot_vb_used(void);
uint32_t holo_port_hot_vb_count(void);
uint32_t holo_port_hot_vb_fallbacks(void);

void holo_port_set_last_error(const char *message);
const char *holo_port_last_error(void);
void holo_port_log(const char *message);

void holo_port_frame_presented(void);
uint32_t holo_port_frame_count(void);

#endif
