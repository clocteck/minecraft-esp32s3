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

bool holo_port_hot_vb_reserve(void);
void *holo_port_hot_vb_alloc(size_t bytes);
bool holo_port_hot_vb_free(void *memory);
void holo_port_hot_vb_release(void);
uint32_t holo_port_hot_vb_reserved(void);
uint32_t holo_port_hot_vb_used(void);
uint32_t holo_port_hot_vb_count(void);
uint32_t holo_port_hot_vb_fallbacks(void);

bool holo_port_presenter_async_available(void);
bool holo_port_presenter_take_precleared_color(const void *pixels, uint16_t color);
void holo_port_presenter_schedule_clears(uint16_t color, uint32_t *depth,
                                         size_t depth_words, uint32_t depth_value);

void holo_port_set_last_error(const char *message);
const char *holo_port_last_error(void);
void holo_port_log(const char *message);

void holo_port_frame_presented(void);
uint32_t holo_port_frame_count(void);

#endif
