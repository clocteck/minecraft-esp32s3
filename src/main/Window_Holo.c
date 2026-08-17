#include "Window.h"
#include "Platform.h"
#include "Input.h"
#include "Event.h"
#include "Graphics.h"
#include "String_.h"
#include "Funcs.h"
#include "Bitmap.h"
#include "Errors.h"
#include "holo_port.h"
#include "holo_perf.h"

#include <stdint.h>
#include <string.h>

#define BTN_UP     1u
#define BTN_DOWN   2u
#define BTN_LEFT   4u
#define BTN_RIGHT  8u
#define BTN_A      16u
#define BTN_B      32u
#define BTN_X      64u
#define BTN_Y      128u
#define BTN_L      256u
#define BTN_R      512u
#define BTN_L2     1024u
#define BTN_R2     2048u
#define BTN_SELECT 4096u
#define BTN_START  8192u
#define PRESENT_STACK_BYTES (12u * 1024u)
#define RIGHT_STICK_LOOK_MULTIPLIER 6.0f

struct _DisplayData DisplayInfo;
struct cc_window WindowInfo;
struct cc_window Window_Alt;
static cc_bool s_close_raised;
static const module_host_api_v2 *s_window_host;
#ifdef CC_BUILD_SOFTFP_FB16
typedef uint16_t HoloFramebufferPixel;
#else
typedef BitmapCol HoloFramebufferPixel;
#endif
static HoloFramebufferPixel *s_framebuffers[2];
static unsigned s_render_buffer;
static void * volatile s_present_task;
static module_sync_handle_t s_present_ready;
static module_sync_handle_t s_present_done;
static HoloFramebufferPixel * volatile s_present_pixels;
static volatile uint8_t s_present_stop;
static volatile uint8_t s_present_stats_ready;
static uint64_t s_present_convert_us;
static uint64_t s_present_dma_us;
static uint64_t s_present_clear_us;
static uint32_t s_present_dma_submits;
static uint16_t s_precleared_color[2];
static uint8_t s_precleared_valid[2];
static volatile uint8_t s_pending_color_clear;
static uint16_t s_pending_color;
static uint32_t * volatile s_pending_depth_clear;
static size_t s_pending_depth_words;
static uint32_t s_pending_depth_value;
static uint8_t s_present_color_clear;
static uint16_t s_present_color;
static uint32_t *s_present_depth_clear;
static size_t s_present_depth_words;
static uint32_t s_present_depth_value;

static cc_bool PresentFramebuffer(HoloFramebufferPixel *pixels, uint64_t *convert_us,
                                  uint64_t *dma_us, uint32_t *dma_submits);

static void Fill16(uint16_t *dst, size_t count, uint16_t value)
{
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        dst[i + 0] = value; dst[i + 1] = value;
        dst[i + 2] = value; dst[i + 3] = value;
        dst[i + 4] = value; dst[i + 5] = value;
        dst[i + 6] = value; dst[i + 7] = value;
    }
    for (; i < count; i++) dst[i] = value;
}

static void Fill32(uint32_t *dst, size_t count, uint32_t value)
{
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        dst[i + 0] = value; dst[i + 1] = value;
        dst[i + 2] = value; dst[i + 3] = value;
        dst[i + 4] = value; dst[i + 5] = value;
        dst[i + 6] = value; dst[i + 7] = value;
    }
    for (; i < count; i++) dst[i] = value;
}

static void ConsumePendingClears(void)
{
    s_present_color_clear = s_pending_color_clear;
    s_present_color = s_pending_color;
    s_present_depth_clear = s_pending_depth_clear;
    s_present_depth_words = s_pending_depth_words;
    s_present_depth_value = s_pending_depth_value;
    s_pending_color_clear = 0;
    s_pending_depth_clear = NULL;
    s_pending_depth_words = 0;
}

static uint64_t ClearPresentedBuffers(HoloFramebufferPixel *pixels)
{
    uint64_t started = holo_perf_now_us();
    unsigned i;
    if (s_present_color_clear && pixels) {
        Fill16((uint16_t *)pixels,
               CLASSICUBE_RENDER_WIDTH * CLASSICUBE_RENDER_HEIGHT,
               s_present_color);
        for (i = 0; i < 2; i++) {
            if (pixels != s_framebuffers[i]) continue;
            s_precleared_color[i] = s_present_color;
            s_precleared_valid[i] = 1;
            break;
        }
    }
    if (s_present_depth_clear && s_present_depth_words) {
        Fill32(s_present_depth_clear, s_present_depth_words,
               s_present_depth_value);
    }
    s_present_color_clear = 0;
    s_present_depth_clear = NULL;
    s_present_depth_words = 0;
    return holo_perf_now_us() - started;
}

bool holo_port_presenter_async_available(void)
{
    return s_present_task != NULL;
}

bool holo_port_presenter_take_precleared_color(const void *pixels, uint16_t color)
{
    unsigned i;
    if (!s_present_task || !pixels) return false;
    for (i = 0; i < 2; i++) {
        bool match;
        if (pixels != s_framebuffers[i]) continue;
        match = s_precleared_valid[i] && s_precleared_color[i] == color;
        s_precleared_valid[i] = 0;
        return match;
    }
    return false;
}

void holo_port_presenter_schedule_clears(uint16_t color, uint32_t *depth,
                                         size_t depth_words, uint32_t depth_value)
{
    s_pending_color = color;
    s_pending_color_clear = 1;
    s_pending_depth_clear = depth;
    s_pending_depth_words = depth_words;
    s_pending_depth_value = depth_value;
}

static void PresentWorker(void *arg)
{
    const module_host_api_v2 *host = s_window_host;
    (void)arg;
    while (host && host->sync.take &&
           host->sync.take(s_present_ready, MODULE_WAIT_FOREVER) == MODULE_OK) {
        uint64_t convert_us = 0, dma_us = 0;
        uint32_t dma_submits = 0;
        HoloFramebufferPixel *pixels;

        if (s_present_stop) break;
        pixels = s_present_pixels;
        (void)PresentFramebuffer(pixels, &convert_us, &dma_us, &dma_submits);
        s_present_clear_us = ClearPresentedBuffers(pixels);
        s_present_convert_us = convert_us;
        s_present_dma_us = dma_us;
        s_present_dma_submits = dma_submits;
        s_present_stats_ready = 1;
        s_present_pixels = NULL;
        (void)host->sync.give(s_present_done);
    }

    s_present_task = NULL;
    if (host && host->sync.give) (void)host->sync.give(s_present_done);
    if (host && host->task.remove) host->task.remove(NULL);
    for (;;) {
        if (host && host->task.delay) host->task.delay(1000);
    }
}

static cc_bool StartPresentWorker(void)
{
    const module_host_api_v2 *host = s_window_host;
    void *task = NULL;
    int32_t result = MODULE_ERR_UNSUPPORTED;
    if (!host || !host->sync.create_counting || !host->sync.take ||
        !host->sync.give || !host->sync.destroy) return false;

    if (host->sync.create_counting(1, 0, &s_present_ready) != MODULE_OK) return false;
    if (host->sync.create_counting(1, 1, &s_present_done) != MODULE_OK) {
        host->sync.destroy(s_present_ready); s_present_ready = NULL;
        return false;
    }

    s_present_stop = 0;
    s_present_stats_ready = 0;
    s_present_pixels = NULL;
    s_present_clear_us = 0;
    s_pending_color_clear = 0;
    s_pending_depth_clear = NULL;
    s_pending_depth_words = 0;
    s_present_color_clear = 0;
    s_present_depth_clear = NULL;
    s_present_depth_words = 0;
    memset(s_precleared_valid, 0, sizeof(s_precleared_valid));
    if (host->task.create_ex) {
        result = host->task.create_ex("cc_present", PresentWorker, NULL,
                                     PRESENT_STACK_BYTES, 3u, 0,
                                     MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT,
                                     &task);
    } else if (host->task.create) {
        result = host->task.create("cc_present", PresentWorker, NULL,
                                  PRESENT_STACK_BYTES, 3u, 0, &task);
    }
    if (result != MODULE_OK || !task) {
        host->sync.destroy(s_present_done);  s_present_done = NULL;
        host->sync.destroy(s_present_ready); s_present_ready = NULL;
        return false;
    }
    s_present_task = task;
    return true;
}

static void Window_HoloShutdownPresent(void)
{
    const module_host_api_v2 *host = s_window_host;
    if (s_present_task && host && host->sync.take && host->sync.give) {
        (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
        s_present_stop = 1;
        (void)host->sync.give(s_present_ready);
        (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
    }
    if (host && host->sync.destroy) {
        if (s_present_done)  host->sync.destroy(s_present_done);
        if (s_present_ready) host->sync.destroy(s_present_ready);
    }
    s_present_done = NULL;
    s_present_ready = NULL;
    s_present_task = NULL;
    s_present_pixels = NULL;
    s_present_stop = 0;
    s_present_stats_ready = 0;
    s_pending_color_clear = 0;
    s_pending_depth_clear = NULL;
    s_pending_depth_words = 0;
    s_present_color_clear = 0;
    s_present_depth_clear = NULL;
    s_present_depth_words = 0;
    memset(s_precleared_valid, 0, sizeof(s_precleared_valid));
}

void Window_PreInit(void) { }

void Window_Init(void)
{
    s_window_host = holo_port_host();
    DisplayInfo.Width = CLASSICUBE_RENDER_WIDTH;
    DisplayInfo.Height = CLASSICUBE_RENDER_HEIGHT;
    DisplayInfo.Depth = 16;
    /* ClassiCube's menus target a 640x480 reference surface. 0.5 keeps
     * their physical size correct on the native 320x240 framebuffer. */
    DisplayInfo.ScaleX = CLASSICUBE_UI_DPI_SCALE;
    DisplayInfo.ScaleY = CLASSICUBE_UI_DPI_SCALE;
    DisplayInfo.CursorVisible = false;
    Window_Main.Width = CLASSICUBE_RENDER_WIDTH;
    Window_Main.Height = CLASSICUBE_RENDER_HEIGHT;
    Window_Main.Focused = true;
    Window_Main.Exists = true;
    Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
    Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;
    s_close_raised = false;
}

void Window_Free(void) { }
void Window_Create2D(int width, int height) { Window_SetSize(width, height); Window_Main.Is3D = false; }
void Window_Create3D(int width, int height) { Window_SetSize(width, height); Window_Main.Is3D = true; }
void Window_Destroy(void) { Window_Main.Exists = false; }
void Window_SetTitle(const cc_string *title) { }
void Clipboard_GetText(cc_string *value) { }
void Clipboard_SetText(const cc_string *value) { }
int Window_GetWindowState(void) { return WINDOW_STATE_FULLSCREEN; }
cc_result Window_EnterFullscreen(void) { return 0; }
cc_result Window_ExitFullscreen(void) { return 0; }
int Window_IsObscured(void) { return 0; }
void Window_Show(void) { }

void Window_SetSize(int width, int height)
{
    Window_Main.Width = CLASSICUBE_RENDER_WIDTH;
    Window_Main.Height = CLASSICUBE_RENDER_HEIGHT;
}

void Window_RequestClose(void)
{
    if (s_close_raised) return;
    s_close_raised = true;
    Event_RaiseVoid(&WindowEvents.Closing);
}

void Window_ProcessEvents(float delta)
{
    if (holo_port_stop_requested()) Window_RequestClose();
}

void Cursor_SetPosition(int x, int y) { }
void Window_EnableRawMouse(void) { Input.RawMode = true; }
void Window_DisableRawMouse(void) { Input.RawMode = false; }
void Window_UpdateRawMouse(void) { }

static const BindMapping pad_defaults[BIND_COUNT] = {
    [BIND_FORWARD]      = { CCPAD_UP, 0 },
    [BIND_BACK]         = { CCPAD_DOWN, 0 },
    [BIND_LEFT]         = { CCPAD_LEFT, 0 },
    [BIND_RIGHT]        = { CCPAD_RIGHT, 0 },
    [BIND_JUMP]         = { CCPAD_1, 0 },
    [BIND_INVENTORY]    = { CCPAD_3, 0 },
    [BIND_DELETE_BLOCK] = { CCPAD_L, 0 },
    [BIND_PICK_BLOCK]   = { CCPAD_4, 0 },
    [BIND_PLACE_BLOCK]  = { CCPAD_R, 0 },
    [BIND_HOTBAR_LEFT]  = { CCPAD_ZL, 0 },
    [BIND_HOTBAR_RIGHT] = { CCPAD_ZR, 0 },
};

void Gamepads_PreInit(void) { }
void Gamepads_Init(void) {
    /* Keep the existing 2x sensitivity step. Raw in-game camera movement is
     * multiplied separately so menu navigation retains its current speed. */
    Gamepad_AxisSensitivity[PAD_AXIS_RIGHT] = AXIS_SENSI_HIGH;
    Platform_LogConst("[cc.input] right-stick look=6x-original menu=unchanged");
}

void Gamepads_Process(float delta)
{
    HoloInputState input = holo_port_input();
    uint32_t b = input.buttons;
    int port = Gamepad_Connect(0x45535033, pad_defaults);
    Gamepad_SetButton(port, CCPAD_UP,     b & BTN_UP);
    Gamepad_SetButton(port, CCPAD_DOWN,   b & BTN_DOWN);
    Gamepad_SetButton(port, CCPAD_LEFT,   b & BTN_LEFT);
    Gamepad_SetButton(port, CCPAD_RIGHT,  b & BTN_RIGHT);
    Gamepad_SetButton(port, CCPAD_1,      b & BTN_A);
    Gamepad_SetButton(port, CCPAD_2,      b & BTN_B);
    Gamepad_SetButton(port, CCPAD_3,      b & BTN_X);
    Gamepad_SetButton(port, CCPAD_4,      b & BTN_Y);
    Gamepad_SetButton(port, CCPAD_L,      b & BTN_L);
    Gamepad_SetButton(port, CCPAD_R,      b & BTN_R);
    Gamepad_SetButton(port, CCPAD_ZL,     b & BTN_L2);
    Gamepad_SetButton(port, CCPAD_ZR,     b & BTN_R2);
    Gamepad_SetButton(port, CCPAD_SELECT, b & BTN_SELECT);
    Gamepad_SetButton(port, CCPAD_START,  b & BTN_START);
    Gamepad_SetAxis(port, PAD_AXIS_LEFT, input.lx / 32767.0f, input.ly / 32767.0f, delta);
    Gamepad_SetAxis(port, PAD_AXIS_RIGHT, input.rx / 32767.0f, input.ry / 32767.0f,
                    Input.RawMode ? delta * RIGHT_STICK_LOOK_MULTIPLIER : delta);
}

void Window_AllocFramebuffer(struct Bitmap *bmp, int width, int height)
{
    bmp->width = CLASSICUBE_RENDER_WIDTH;
    bmp->height = CLASSICUBE_RENDER_HEIGHT;
    s_render_buffer = 0;
    memset(s_precleared_valid, 0, sizeof(s_precleared_valid));
    s_framebuffers[0] = (HoloFramebufferPixel *)Mem_Alloc(
        bmp->width, bmp->height * sizeof(HoloFramebufferPixel), "320x240 framebuffer A");
    s_framebuffers[1] = (HoloFramebufferPixel *)Mem_Alloc(
        bmp->width, bmp->height * sizeof(HoloFramebufferPixel), "320x240 framebuffer B");
    bmp->scan0 = (BitmapCol*)s_framebuffers[0];
    if (!s_framebuffers[1] || !StartPresentWorker()) {
        if (s_framebuffers[1]) Mem_Free(s_framebuffers[1]);
        s_framebuffers[1] = NULL;
        holo_port_log("[cc.present] async presenter unavailable; using synchronous output");
    }
}

static uint16_t to_rgb565(BitmapCol color)
{
    return (uint16_t)(((color >> 8) & 0xF800u) |
                      ((color >> 5) & 0x07E0u) |
                      ((color >> 3) & 0x001Fu));
}

static cc_bool PresentFramebuffer(HoloFramebufferPixel *pixels, uint64_t *convert_us,
                                  uint64_t *dma_us, uint32_t *dma_submits)
{
    const module_host_api_v2 *host = s_window_host;
    void *surface = holo_port_display_surface();
    uint16_t *dma[2];
    int y;
    uint64_t started;
    uint64_t convert_total = 0, dma_total = 0;
    uint32_t submit_total = 0;
    if (convert_us) *convert_us = 0;
    if (dma_us) *dma_us = 0;
    if (dma_submits) *dma_submits = 0;
    if (!host || !surface || !pixels || !host->display.startWrite ||
        !host->display.pushImageDMA || !host->display.endWrite) return false;
    dma[0] = holo_port_dma_buffer(CLASSICUBE_DISPLAY_WIDTH * 16, 0);
    dma[1] = holo_port_dma_buffer(CLASSICUBE_DISPLAY_WIDTH * 16, 1);
    if (!dma[0] || !dma[1]) return false;

    started = holo_perf_now_us();
    if (host->display.startWrite(surface) != MODULE_OK) {
        holo_port_set_last_error("RGB565 DMA stream start failed");
        return false;
    }
    dma_total += holo_perf_now_us() - started;

    for (y = 0; y < CLASSICUBE_RENDER_HEIGHT; y += 16) {
        uint16_t *block = dma[(y / 16) & 1];
        int rows = min(16, CLASSICUBE_RENDER_HEIGHT - y);
        int yy, x;
        started = holo_perf_now_us();
#ifdef CC_BUILD_SOFTFP_FB16
        memcpy(block, pixels + y * CLASSICUBE_RENDER_WIDTH,
               (size_t)rows * CLASSICUBE_DISPLAY_WIDTH * sizeof(uint16_t));
#else
        for (yy = 0; yy < rows; yy++) {
            BitmapCol *src = pixels + (y + yy) * CLASSICUBE_RENDER_WIDTH;
            uint32_t *dst = (uint32_t *)(block + yy * CLASSICUBE_DISPLAY_WIDTH);
            for (x = 0; x < CLASSICUBE_RENDER_WIDTH; x += 2) {
                uint32_t lo = to_rgb565(src[x]);
                uint32_t hi = to_rgb565(src[x + 1]);
                dst[x >> 1] = lo | (hi << 16);
            }
        }
#endif
        convert_total += holo_perf_now_us() - started;
        started = holo_perf_now_us();
        if (host->display.pushImageDMA(surface, 0, (int16_t)y, CLASSICUBE_DISPLAY_WIDTH,
                                       (uint16_t)rows, block) != MODULE_OK) {
            (void)host->display.endWrite(surface);
            holo_port_set_last_error("RGB565 DMA push failed");
            return false;
        }
        dma_total += holo_perf_now_us() - started;
        submit_total++;
    }
    started = holo_perf_now_us();
    if (host->display.endWrite(surface) != MODULE_OK) {
        holo_port_set_last_error("RGB565 DMA completion failed");
        return false;
    }
    dma_total += holo_perf_now_us() - started;
    if (convert_us) *convert_us = convert_total;
    if (dma_us) *dma_us = dma_total;
    if (dma_submits) *dma_submits = submit_total;
    holo_port_frame_presented();
    return true;
}

static void RecordPresentStats(uint64_t convert_us, uint64_t dma_us,
                               uint64_t clear_us, uint32_t dma_submits)
{
    holo_perf_add(HOLO_PERF_CONVERT, convert_us);
    holo_perf_add(HOLO_PERF_DMA, dma_us);
    holo_perf_add(HOLO_PERF_ASYNC_CLEAR, clear_us);
    while (dma_submits--) holo_perf_count_dma_submit();
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap *bmp)
{
    const module_host_api_v2 *host = s_window_host;
    uint64_t convert_us = 0, dma_us = 0, clear_us = 0;
    uint64_t wait_started;
    uint32_t dma_submits = 0;
    cc_bool clears_prepared = false;
    (void)r;
    if (!bmp || !bmp->scan0) return;

    if (s_present_task && s_present_done && s_present_ready && host &&
        host->sync.take && host->sync.give) {
        HoloFramebufferPixel *presented = (HoloFramebufferPixel*)bmp->scan0;
        unsigned next_buffer = s_render_buffer ^ 1u;

        wait_started = holo_perf_now_us();
        (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
        holo_perf_add(HOLO_PERF_ASYNC_WAIT, holo_perf_now_us() - wait_started);
        if (s_present_stats_ready) {
            RecordPresentStats(s_present_convert_us, s_present_dma_us,
                               s_present_clear_us, s_present_dma_submits);
            s_present_stats_ready = 0;
        }
        ConsumePendingClears();
        clears_prepared = true;
        s_present_pixels = presented;
        if (host->sync.give(s_present_ready) == MODULE_OK) {
            s_render_buffer = next_buffer;
            bmp->scan0 = (BitmapCol*)s_framebuffers[s_render_buffer];
            /* Let IDLE1 run every frame so the task watchdog is serviced. */
            if (host->task.delay) host->task.delay(1);
            return;
        }
        s_present_pixels = NULL;
        (void)host->sync.give(s_present_done);
    }

    if (!clears_prepared) ConsumePendingClears();
    (void)PresentFramebuffer((HoloFramebufferPixel*)bmp->scan0,
                             &convert_us, &dma_us, &dma_submits);
    clear_us = ClearPresentedBuffers((HoloFramebufferPixel*)bmp->scan0);
    RecordPresentStats(convert_us, dma_us, clear_us, dma_submits);
}

void Window_FreeFramebuffer(struct Bitmap *bmp)
{
    Window_HoloShutdownPresent();
    if (s_framebuffers[0]) Mem_Free(s_framebuffers[0]);
    if (s_framebuffers[1]) Mem_Free(s_framebuffers[1]);
    s_framebuffers[0] = NULL;
    s_framebuffers[1] = NULL;
    if (bmp) { bmp->scan0 = NULL; bmp->width = bmp->height = 0; }
}

void OnscreenKeyboard_Open(struct OpenKeyboardArgs *args) { }
void OnscreenKeyboard_SetText(const cc_string *text) { }
void OnscreenKeyboard_Close(void) { }
void Window_LockLandscapeOrientation(cc_bool lock) { }

void Window_ShowDialog(const char *title, const char *msg)
{
    Platform_LogConst(title ? title : "ClassiCube");
    Platform_LogConst(msg ? msg : "");
    holo_port_set_last_error(msg ? msg : title);
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs *args) { return ERR_NOT_SUPPORTED; }
cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs *args) { return ERR_NOT_SUPPORTED; }
