#include "holo_port.h"

#include <string.h>

#define DMA_ROWS   16u
#define DMA_BYTES (CLASSICUBE_DISPLAY_WIDTH * DMA_ROWS * sizeof(uint16_t))
#define RENDER_CACHE_BYTES (CLASSICUBE_3D_WIDTH * CLASSICUBE_3D_HEIGHT)
#define COLOR_BAND_BYTES (CLASSICUBE_3D_WIDTH * CLASSICUBE_COLOR_BAND_ROWS * sizeof(uint16_t))
#define PORT_BUFFERS 6u
#define HOT_VB_ARENA_BYTES (12u * 1024u)

typedef struct HotVbBlock {
    uint32_t size;
    uint32_t used;
} HotVbBlock;

static const module_host_api_v2 *s_host;
static char s_root[CLASSICUBE_PATH_MAX] = "/sd/apps/classicube";
static char s_last_error[192];
static volatile HoloInputState s_input;
static volatile uint8_t s_stop_requested;
static volatile uint32_t s_frames;
static void *s_display_surface;
static void *s_dma_buffers[PORT_BUFFERS];
static size_t s_dma_buffer_bytes[PORT_BUFFERS];
static uint8_t *s_hot_vb_arena;
static uint32_t s_hot_vb_used;
static uint32_t s_hot_vb_count;
static uint32_t s_hot_vb_fallbacks;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || !dst_size) return;
    if (!src) src = "";
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int16_t clamp_axis(int32_t value)
{
    if (value < -32767) return -32767;
    if (value >  32767) return  32767;
    return (int16_t)value;
}

void holo_port_set_host(const module_host_api_v2 *host) { s_host = host; }
const module_host_api_v2 *holo_port_host(void) { return s_host; }

void holo_port_set_root(const char *root)
{
    size_t len;
    copy_text(s_root, sizeof(s_root), root && root[0] ? root : "/sd/apps/classicube");
    len = strlen(s_root);
    while (len > 1 && s_root[len - 1] == '/') s_root[--len] = '\0';
}

const char *holo_port_root(void) { return s_root; }

void holo_port_set_input(uint32_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry)
{
    HoloInputState next;
    next.buttons = buttons;
    next.lx = clamp_axis(lx); next.ly = clamp_axis(ly);
    next.rx = clamp_axis(rx); next.ry = clamp_axis(ry);
    s_input = next;
}

HoloInputState holo_port_input(void)
{
    HoloInputState value;
    value.buttons = s_input.buttons;
    value.lx = s_input.lx; value.ly = s_input.ly;
    value.rx = s_input.rx; value.ry = s_input.ry;
    return value;
}

void holo_port_reset_input(void) { holo_port_set_input(0, 0, 0, 0, 0); }
void holo_port_request_stop(void) { s_stop_requested = 1; }
void holo_port_clear_stop(void) { s_stop_requested = 0; }
bool holo_port_stop_requested(void) { return s_stop_requested != 0; }

bool holo_port_display_acquire(void)
{
    module_display_desc_t desc;
    module_display_caps_t caps;
    if (s_display_surface) return true;
    s_frames = 0;
    if (!s_host || !s_host->display.acquire || !s_host->display.pushImageDMA) {
        holo_port_set_last_error("display API unavailable");
        return false;
    }
    memset(&caps, 0, sizeof(caps));
    caps.size = sizeof(caps);
    if (s_host->display.get_caps && s_host->display.get_caps(&caps) == MODULE_OK &&
        (caps.width != CLASSICUBE_DISPLAY_WIDTH || caps.height != CLASSICUBE_DISPLAY_HEIGHT)) {
        holo_port_set_last_error("native 320x240 renderer requires a 320x240 display");
        return false;
    }
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.width = CLASSICUBE_DISPLAY_WIDTH;
    desc.height = CLASSICUBE_DISPLAY_HEIGHT;
    desc.pixel_format = MODULE_PIXEL_RGB565;
    if (s_host->display.acquire("classicube", &desc, &s_display_surface) != MODULE_OK) {
        s_display_surface = NULL;
        holo_port_set_last_error("display is busy or cannot be acquired");
        return false;
    }
    /* DMA output is mandatory. Reserve both blocks before optional renderer
     * caches so internal colour bands can never starve the display path. */
    if (!holo_port_buffer(DMA_BYTES, 0) || !holo_port_buffer(DMA_BYTES, 1)) {
        if (s_host->display.release) (void)s_host->display.release(s_display_surface);
        s_display_surface = NULL;
        holo_port_set_last_error("cannot reserve RGB565 DMA buffers");
        return false;
    }
    if (s_host->display.fillScreen) (void)s_host->display.fillScreen(s_display_surface, 0);
    return true;
}

void holo_port_display_release(void)
{
    unsigned i;
    /* Return the one contiguous renderer arena before LVGL recreates its
     * pair of internal draw buffers. This avoids heap fragmentation on exit. */
    holo_port_hot_vb_release();
    if (s_host && s_host->heap.free) {
        for (i = 0; i < PORT_BUFFERS; i++) {
            if (s_dma_buffers[i]) s_host->heap.free(s_dma_buffers[i]);
            s_dma_buffers[i] = NULL;
            s_dma_buffer_bytes[i] = 0;
        }
    }
    if (s_display_surface && s_host && s_host->display.release)
        (void)s_host->display.release(s_display_surface);
    s_display_surface = NULL;
}

void *holo_port_display_surface(void) { return s_display_surface; }

void *holo_port_buffer(size_t bytes, unsigned index)
{
    size_t capacity;
    uint32_t caps = MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT;
    if (index >= PORT_BUFFERS || !s_host || !s_host->heap.malloc) return NULL;
    if (index == 2) {
        capacity = RENDER_CACHE_BYTES;
    } else if (index >= 4) {
        capacity = COLOR_BAND_BYTES;
    } else {
        capacity = DMA_BYTES;
    }
    if (bytes > capacity) return NULL;
    if (s_dma_buffers[index]) {
        return bytes <= s_dma_buffer_bytes[index] ? s_dma_buffers[index] : NULL;
    }
    if (index < 2) caps |= MODULE_HEAP_DMA;
    s_dma_buffers[index] = s_host->heap.malloc(bytes, caps);
    if (s_dma_buffers[index]) s_dma_buffer_bytes[index] = bytes;
    if (!s_dma_buffers[index]) {
        if (index == 2) {
            holo_port_set_last_error("cannot allocate internal 3D depth buffer");
        } else if (index == 3) {
            holo_port_set_last_error("cannot allocate internal terrain texture cache");
        } else if (index >= 4) {
            holo_port_set_last_error("cannot allocate internal 3D colour band");
        } else {
            holo_port_set_last_error("cannot allocate 10 KiB internal DMA buffer");
        }
    }
    return s_dma_buffers[index];
}

static HotVbBlock *hot_vb_next(HotVbBlock *block)
{
    uintptr_t base = (uintptr_t)s_hot_vb_arena;
    uintptr_t here = (uintptr_t)block;
    uintptr_t next;
    if (!base || here < base || here + sizeof(*block) > base + HOT_VB_ARENA_BYTES) return NULL;
    if (block->size > base + HOT_VB_ARENA_BYTES - here - sizeof(*block)) return NULL;
    next = here + sizeof(*block) + block->size;
    return next + sizeof(*block) <= base + HOT_VB_ARENA_BYTES ? (HotVbBlock *)next : NULL;
}

bool holo_port_hot_vb_reserve(void)
{
    HotVbBlock *first;
    if (s_hot_vb_arena) return true;
    if (!s_host || !s_host->heap.malloc) return false;
    s_hot_vb_arena = (uint8_t *)s_host->heap.malloc(
        HOT_VB_ARENA_BYTES, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!s_hot_vb_arena) {
        s_hot_vb_fallbacks++;
        holo_port_log("[cc.mem] 12 KiB internal dynamic-VB arena unavailable; using PSRAM");
        return false;
    }
    first = (HotVbBlock *)s_hot_vb_arena;
    first->size = HOT_VB_ARENA_BYTES - sizeof(*first);
    first->used = 0;
    s_hot_vb_used = 0;
    s_hot_vb_count = 0;
    holo_port_log("[cc.mem] dynamic-VB arena=12288 internal contiguous; colder tail=PSRAM");
    return true;
}

void *holo_port_hot_vb_alloc(size_t bytes)
{
    HotVbBlock *block;
    uint32_t needed;
    if (!bytes || bytes > UINT32_MAX - 7u) return NULL;
    needed = ((uint32_t)bytes + 7u) & ~7u;
    if (!holo_port_hot_vb_reserve()) return NULL;

    block = (HotVbBlock *)s_hot_vb_arena;
    for (;;) {
        if (!block->used && block->size >= needed) {
            uint32_t remaining = block->size - needed;
            if (remaining >= sizeof(HotVbBlock) + 8u) {
                HotVbBlock *split = (HotVbBlock *)((uint8_t *)(block + 1) + needed);
                split->size = remaining - sizeof(HotVbBlock);
                split->used = 0;
                block->size = needed;
            }
            block->used = 1;
            s_hot_vb_used += block->size;
            s_hot_vb_count++;
            return block + 1;
        }
        block = hot_vb_next(block);
        if (!block) break;
    }
    s_hot_vb_fallbacks++;
    return NULL;
}

bool holo_port_hot_vb_free(void *memory)
{
    HotVbBlock *block, *previous = NULL;
    uintptr_t address = (uintptr_t)memory;
    uintptr_t base = (uintptr_t)s_hot_vb_arena;
    if (!memory || !base || address < base + sizeof(HotVbBlock) ||
        address >= base + HOT_VB_ARENA_BYTES) return false;

    block = (HotVbBlock *)s_hot_vb_arena;
    for (;;) {
        HotVbBlock *next;
        if ((void *)(block + 1) == memory) break;
        next = hot_vb_next(block);
        if (!next) return false;
        previous = block;
        block = next;
    }
    if (!block->used) return true;
    block->used = 0;
    if (block->size <= s_hot_vb_used) s_hot_vb_used -= block->size;
    if (s_hot_vb_count) s_hot_vb_count--;

    {
        HotVbBlock *next = hot_vb_next(block);
        if (next && !next->used) block->size += sizeof(*next) + next->size;
    }
    if (previous && !previous->used) {
        previous->size += sizeof(*block) + block->size;
    }
    return true;
}

void holo_port_hot_vb_release(void)
{
    if (s_hot_vb_arena && s_host && s_host->heap.free) s_host->heap.free(s_hot_vb_arena);
    s_hot_vb_arena = NULL;
    s_hot_vb_used = 0;
    s_hot_vb_count = 0;
    s_hot_vb_fallbacks = 0;
}

uint32_t holo_port_hot_vb_reserved(void) { return s_hot_vb_arena ? HOT_VB_ARENA_BYTES : 0; }
uint32_t holo_port_hot_vb_used(void) { return s_hot_vb_used; }
uint32_t holo_port_hot_vb_count(void) { return s_hot_vb_count; }
uint32_t holo_port_hot_vb_fallbacks(void) { return s_hot_vb_fallbacks; }

void holo_port_set_last_error(const char *message) { copy_text(s_last_error, sizeof(s_last_error), message); }
const char *holo_port_last_error(void) { return s_last_error; }

void holo_port_log(const char *message)
{
    if (s_host && s_host->serial.println) s_host->serial.println(message ? message : "");
}

void holo_port_frame_presented(void) { s_frames++; }
uint32_t holo_port_frame_count(void) { return s_frames; }
