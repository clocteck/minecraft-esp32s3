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
#define PRESENT_STACK_BYTES (4u * 1024u)
#define RIGHT_STICK_LOOK_MULTIPLIER 6.0f

struct _DisplayData DisplayInfo;
struct cc_window WindowInfo;
struct cc_window Window_Alt;
static cc_bool s_close_raised;
static const module_host_api_v2 *s_window_host;
typedef uint16_t HoloFramebufferPixel;
static HoloFramebufferPixel *s_framebuffer;
static uint8_t s_core1_yield_count;
static void * volatile s_present_task;
static module_sync_handle_t s_present_ready;
static module_sync_handle_t s_present_done;
static void * volatile s_present_context;
static HoloPresentBandFn volatile s_present_compose;
static volatile uint8_t s_present_stop;
static volatile uint8_t s_present_stats_ready;
static uint64_t s_present_convert_us;
static uint64_t s_present_dma_us;
static uint32_t s_present_dma_submits;

static cc_bool PresentComposed(void *context, HoloPresentBandFn compose,
                               uint64_t *convert_us, uint64_t *dma_us,
                               uint32_t *dma_submits);
static void RecordPresentStats(uint64_t convert_us, uint64_t dma_us,
                               uint32_t dma_submits);

static void PresentWorker(void *arg)
{
    const module_host_api_v2 *host = s_window_host;
    (void)arg;
    while (host && host->sync.take &&
           host->sync.take(s_present_ready, MODULE_WAIT_FOREVER) == MODULE_OK) {
        uint64_t convert_us = 0, dma_us = 0;
        uint32_t dma_submits = 0;
        void *context;
        HoloPresentBandFn compose;

        if (s_present_stop) break;
        context = s_present_context;
        compose = s_present_compose;
        (void)PresentComposed(context, compose, &convert_us, &dma_us, &dma_submits);
        s_present_convert_us = convert_us;
        s_present_dma_us = dma_us;
        s_present_dma_submits = dma_submits;
        s_present_stats_ready = 1;
        s_present_context = NULL;
        s_present_compose = NULL;
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
    if (s_present_task) return true;
    if (!host || !host->sync.create_counting || !host->sync.take ||
        !host->sync.give || !host->sync.destroy) return false;

    if (host->sync.create_counting(1, 0, &s_present_ready) != MODULE_OK) return false;
    if (host->sync.create_counting(1, 1, &s_present_done) != MODULE_OK) {
        host->sync.destroy(s_present_ready); s_present_ready = NULL;
        return false;
    }

    s_present_stop = 0;
    s_present_stats_ready = 0;
    s_present_context = NULL;
    s_present_compose = NULL;
    if (host->task.create_ex) {
        result = host->task.create_ex("cc_present", PresentWorker, NULL,
                                     PRESENT_STACK_BYTES, 4u, 0,
                                     MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT,
                                     &task);
    } else if (host->task.create) {
        result = host->task.create("cc_present", PresentWorker, NULL,
                                  PRESENT_STACK_BYTES, 4u, 0, &task);
    }
    if (result != MODULE_OK || !task) {
        host->sync.destroy(s_present_done);  s_present_done = NULL;
        host->sync.destroy(s_present_ready); s_present_ready = NULL;
        return false;
    }
    s_present_task = task;
    s_core1_yield_count = 0;
    holo_port_log("[cc.present] worker=Core0 prio4 stack=4096 internal; Core1 idle yield=1ms/8 full frames");
    if (host->display.setAddrWindow && host->display.pushPixelsDMA) {
        holo_port_log("[cc.present] DMA=fixed 320x240 window + sequential pushPixelsDMA bands");
    } else {
        holo_port_log("[cc.present] DMA=per-band pushImageDMA compatibility fallback");
    }
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
    s_present_context = NULL;
    s_present_compose = NULL;
    s_present_stop = 0;
    s_present_stats_ready = 0;
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
    /* Start before renderer caches and dynamic VBs consume/fragment internal
     * RAM. The worker sleeps until the first completed framebuffer arrives. */
    (void)StartPresentWorker();
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
    Gamepad_SetAxis(port, PAD_AXIS_LEFT, input.lx / 32767.0f, -input.ly / 32767.0f, delta);
    Gamepad_SetAxis(port, PAD_AXIS_RIGHT, input.rx / 32767.0f, input.ry / 32767.0f,
                    Input.RawMode ? delta * RIGHT_STICK_LOOK_MULTIPLIER : delta);
}

void Window_AllocFramebuffer(struct Bitmap *bmp, int width, int height)
{
    bmp->width = CLASSICUBE_RENDER_WIDTH;
    bmp->height = CLASSICUBE_RENDER_HEIGHT;
    /* One native framebuffer is retained only for menus/compatibility. Normal
     * gameplay queues immutable 192x144 snapshots through the band composer. */
    s_framebuffer = (HoloFramebufferPixel *)Mem_Alloc(
        bmp->width, bmp->height * sizeof(HoloFramebufferPixel),
        "320x240 menu fallback framebuffer");
    bmp->scan0 = (BitmapCol*)s_framebuffer;
    if (!StartPresentWorker()) {
        holo_port_log("[cc.present] async presenter unavailable; composed output is synchronous");
    }
}

static void CopyNativeBand(void *context, uint16_t *dst, int dst_y, int rows)
{
    const HoloFramebufferPixel *pixels = (const HoloFramebufferPixel *)context;
    memcpy(dst, pixels + dst_y * CLASSICUBE_DISPLAY_WIDTH,
           (size_t)rows * CLASSICUBE_DISPLAY_WIDTH * sizeof(uint16_t));
}

static cc_bool PresentComposed(void *context, HoloPresentBandFn compose,
                               uint64_t *convert_us, uint64_t *dma_us,
                               uint32_t *dma_submits)
{
    const module_host_api_v2 *host = s_window_host;
    void *surface = holo_port_display_surface();
    uint16_t *dma[2];
    cc_bool continuous;
    int y;
    uint64_t started;
    uint64_t convert_total = 0, dma_total = 0;
    uint32_t submit_total = 0;
    if (convert_us) *convert_us = 0;
    if (dma_us) *dma_us = 0;
    if (dma_submits) *dma_submits = 0;
    if (!host || !surface || !context || !compose || !host->display.startWrite ||
        !host->display.endWrite) return false;
    continuous = host->display.setAddrWindow && host->display.pushPixelsDMA;
    if (!continuous && !host->display.pushImageDMA) return false;
    dma[0] = (uint16_t *)holo_port_buffer(
        CLASSICUBE_DISPLAY_WIDTH * CLASSICUBE_DMA_ROWS * sizeof(uint16_t), 0);
    dma[1] = (uint16_t *)holo_port_buffer(
        CLASSICUBE_DISPLAY_WIDTH * CLASSICUBE_DMA_ROWS * sizeof(uint16_t), 1);
    if (!dma[0] || !dma[1]) return false;

    started = holo_perf_now_us();
    if (host->display.startWrite(surface) != MODULE_OK) {
        holo_port_set_last_error("RGB565 DMA stream start failed");
        return false;
    }
    dma_total += holo_perf_now_us() - started;

    if (continuous) {
        started = holo_perf_now_us();
        if (host->display.setAddrWindow(surface, 0, 0, CLASSICUBE_DISPLAY_WIDTH,
                                        CLASSICUBE_RENDER_HEIGHT) != MODULE_OK) {
            (void)host->display.endWrite(surface);
            holo_port_set_last_error("RGB565 full-frame DMA window failed");
            return false;
        }
        dma_total += holo_perf_now_us() - started;
    }

    for (y = 0; y < CLASSICUBE_RENDER_HEIGHT; y += CLASSICUBE_DMA_ROWS) {
        uint16_t *block = dma[(y / CLASSICUBE_DMA_ROWS) & 1];
        int rows = min(CLASSICUBE_DMA_ROWS, CLASSICUBE_RENDER_HEIGHT - y);
        int32_t result;
        started = holo_perf_now_us();
        compose(context, block, y, rows);
        convert_total += holo_perf_now_us() - started;
        started = holo_perf_now_us();
        result = continuous
            ? host->display.pushPixelsDMA(surface, block,
                (size_t)CLASSICUBE_DISPLAY_WIDTH * (size_t)rows)
            : host->display.pushImageDMA(surface, 0, (int16_t)y,
                CLASSICUBE_DISPLAY_WIDTH, (uint16_t)rows, block);
        if (result != MODULE_OK) {
            (void)host->display.endWrite(surface);
            holo_port_set_last_error("RGB565 DMA band push failed");
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

static void RecordPendingPresentStats(void)
{
    if (!s_present_stats_ready) return;
    RecordPresentStats(s_present_convert_us, s_present_dma_us,
                       s_present_dma_submits);
    s_present_stats_ready = 0;
}

bool holo_window_present_composed(void *context, HoloPresentBandFn compose)
{
    const module_host_api_v2 *host = s_window_host;
    uint64_t convert_us = 0, dma_us = 0;
    uint64_t wait_started;
    uint32_t dma_submits = 0;

    if (!context || !compose) return false;
    if (s_present_task && s_present_done && s_present_ready && host &&
        host->sync.take && host->sync.give) {
        wait_started = holo_perf_now_us();
        (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
        holo_perf_add(HOLO_PERF_ASYNC_WAIT, holo_perf_now_us() - wait_started);
        RecordPendingPresentStats();
        s_present_context = context;
        s_present_compose = compose;
        if (host->sync.give(s_present_ready) == MODULE_OK) {
            if (++s_core1_yield_count >= 8) {
                s_core1_yield_count = 0;
                if (host->task.delay) host->task.delay(1);
            }
            return true;
        }
        s_present_context = NULL;
        s_present_compose = NULL;
        (void)host->sync.give(s_present_done);
    }

    if (!PresentComposed(context, compose, &convert_us, &dma_us, &dma_submits)) {
        return false;
    }
    RecordPresentStats(convert_us, dma_us, dma_submits);
    return true;
}

void holo_window_wait_present(void)
{
    const module_host_api_v2 *host = s_window_host;
    uint64_t wait_started;
    if (!s_present_task || !s_present_done || !host ||
        !host->sync.take || !host->sync.give) return;
    wait_started = holo_perf_now_us();
    (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
    holo_perf_add(HOLO_PERF_ASYNC_WAIT, holo_perf_now_us() - wait_started);
    RecordPendingPresentStats();
    (void)host->sync.give(s_present_done);
}

static void RecordPresentStats(uint64_t convert_us, uint64_t dma_us,
                               uint32_t dma_submits)
{
    holo_perf_add(HOLO_PERF_CONVERT, convert_us);
    holo_perf_add(HOLO_PERF_DMA, dma_us);
    while (dma_submits--) holo_perf_count_dma_submit();
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap *bmp)
{
    const module_host_api_v2 *host = s_window_host;
    uint64_t convert_us = 0, dma_us = 0;
    uint64_t wait_started;
    uint32_t dma_submits = 0;
    (void)r;
    if (!bmp || !bmp->scan0) return;

    /* The menu fallback owns a single native framebuffer. Wait for any prior
     * composed frame, then present it synchronously so it cannot be overwritten
     * while LCD DMA is still reading it. */
    if (s_present_task && s_present_done && host && host->sync.take && host->sync.give) {
        wait_started = holo_perf_now_us();
        (void)host->sync.take(s_present_done, MODULE_WAIT_FOREVER);
        holo_perf_add(HOLO_PERF_ASYNC_WAIT, holo_perf_now_us() - wait_started);
        RecordPendingPresentStats();
        (void)PresentComposed((HoloFramebufferPixel*)bmp->scan0, CopyNativeBand,
                              &convert_us, &dma_us, &dma_submits);
        RecordPresentStats(convert_us, dma_us, dma_submits);
        (void)host->sync.give(s_present_done);
        return;
    }

    (void)PresentComposed((HoloFramebufferPixel*)bmp->scan0, CopyNativeBand,
                          &convert_us, &dma_us, &dma_submits);
    RecordPresentStats(convert_us, dma_us, dma_submits);
}

void Window_FreeFramebuffer(struct Bitmap *bmp)
{
    Window_HoloShutdownPresent();
    if (s_framebuffer) Mem_Free(s_framebuffer);
    s_framebuffer = NULL;
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
