#include "module_abi.h"
#include "holo_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CLASSICUBE_EXPORT __attribute__((visibility("default"), used))
#define CLASSICUBE_NAME "classicube"
#define CLASSICUBE_VERSION "0.1.0"
#define CLASSICUBE_DESCRIPTION "Offline ClassiCube, 320x240 SoftFP with perspective-correct textures for ESP32-S3"
#define CLASSICUBE_STACK_BYTES (128u * 1024u)

int ClassiCube_HoloMain(void);
void Platform_HoloForceFree(void);

typedef struct ClassiCubeInstance {
    const module_host_api_v2 *host;
    void *task;
    volatile uint8_t running;
    volatile uint8_t stop_requested;
    volatile int32_t last_result;
    char module_path[MODULE_PATH_MAX];
    char root[CLASSICUBE_PATH_MAX];
} ClassiCubeInstance;

static module_host_api_v2 s_resolved_host;
static ClassiCubeInstance s_instance;
static const module_host_api_v2 *s_host;

static const module_manifest_t s_manifest = {
    MODULE_MANIFEST_MAGIC,
    MODULE_ABI_VERSION,
    sizeof(module_manifest_t),
    CLASSICUBE_NAME,
    CLASSICUBE_VERSION,
    CLASSICUBE_DESCRIPTION,
    0,
    MODULE_BOOTSTRAP_ABI_VERSION,
};

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || !dst_size) return;
    if (!src) src = "";
    while (i + 1 < dst_size && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void join_path(char *dst, size_t dst_size, const char *root, const char *child)
{
    size_t len;
    copy_text(dst, dst_size, root);
    len = strlen(dst);
    if (len && dst[len - 1] != '/' && len + 1 < dst_size) dst[len++] = '/';
    while (*child && len + 1 < dst_size) dst[len++] = *child++;
    dst[len] = '\0';
}

static int push_error(lua_State *L, const char *message)
{
    s_host->lua.pushnil(L);
    s_host->lua.pushstring(L, message ? message : "classicube failed");
    return 2;
}

static void set_string(lua_State *L, const char *key, const char *value)
{
    s_host->lua.pushstring(L, value ? value : "");
    s_host->lua.setfield(L, -2, key);
}

static void set_integer(lua_State *L, const char *key, int64_t value)
{
    s_host->lua.pushinteger(L, value);
    s_host->lua.setfield(L, -2, key);
}

static void set_boolean(lua_State *L, const char *key, bool value)
{
    s_host->lua.pushboolean(L, value ? 1 : 0);
    s_host->lua.setfield(L, -2, key);
}

static ClassiCubeInstance *instance_from_lua(lua_State *L)
{
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index) return NULL;
    return (ClassiCubeInstance *)s_host->lua.touserdata(L, s_host->lua.upvalue_index(1));
}

static void set_closure(lua_State *L, const char *key, module_lua_cfunction_t fn,
                        ClassiCubeInstance *instance)
{
    s_host->lua.pushlightuserdata(L, instance);
    s_host->lua.pushcclosure(L, fn, 1);
    s_host->lua.setfield(L, -2, key);
}

static bool read_table_string(lua_State *L, int table_index, const char *key,
                              char *dst, size_t dst_size)
{
    int top = s_host->lua.gettop(L);
    bool found = false;
    s_host->lua.getfield(L, table_index, key);
    if (s_host->lua.isstring(L, -1)) {
        copy_text(dst, dst_size, s_host->lua.tostring(L, -1));
        found = true;
    }
    s_host->lua.settop(L, top);
    return found;
}

static void classicube_task(void *arg)
{
    ClassiCubeInstance *instance = (ClassiCubeInstance *)arg;
    if (!instance) return;
    instance->running = 1;
    holo_port_log("[classicube.so] game task started (320x240 SoftFP, perspective spans)");
    instance->last_result = ClassiCube_HoloMain();
    holo_port_reset_input();
    holo_port_display_release();
    instance->running = 0;
    instance->task = NULL;
    holo_port_log("[classicube.so] game task stopped");
    if (instance->host && instance->host->task.remove) instance->host->task.remove(NULL);
    for (;;) {
        if (instance->host && instance->host->task.delay) instance->host->task.delay(1000);
    }
}

static int l_start(lua_State *L)
{
    ClassiCubeInstance *instance = instance_from_lua(L);
    char asset[CLASSICUBE_PATH_MAX + 32];
    char path[CLASSICUBE_PATH_MAX + 32];
    if (!instance) return push_error(L, "classicube.start: instance missing");
    if (instance->running || instance->task) return push_error(L, "classicube is already running");

    copy_text(instance->root, sizeof(instance->root), "/sd/apps/classicube");
    if (s_host->lua.gettop(L) >= 1 && s_host->lua.istable(L, 1))
        (void)read_table_string(L, 1, "root", instance->root, sizeof(instance->root));
    if (!s_host->sd.mounted || s_host->sd.mounted() <= 0)
        return push_error(L, "SD card is not mounted");
    join_path(asset, sizeof(asset), instance->root, "texpacks/default.zip");
    if (!s_host->sd.exists || s_host->sd.exists(asset) <= 0)
        return push_error(L, "texpacks/default.zip is missing from the ClassiCube package");

    if (s_host->sd.mkdir) {
        join_path(path, sizeof(path), instance->root, "maps"); (void)s_host->sd.mkdir(path);
        join_path(path, sizeof(path), instance->root, "screenshots"); (void)s_host->sd.mkdir(path);
        join_path(path, sizeof(path), instance->root, "cache"); (void)s_host->sd.mkdir(path);
    }
    holo_port_set_root(instance->root);
    holo_port_clear_stop();
    holo_port_reset_input();
    holo_port_set_last_error("");
    if (!holo_port_display_acquire()) return push_error(L, holo_port_last_error());

    instance->stop_requested = 0;
    instance->last_result = MODULE_OK;
    instance->running = 1;
    if (s_host->task.create_ex) {
        if (s_host->task.create_ex(CLASSICUBE_NAME, classicube_task, instance,
                                   CLASSICUBE_STACK_BYTES, 3u, 1,
                                   MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT,
                                   &instance->task) != MODULE_OK)
            instance->task = NULL;
    } else if (!s_host->task.create ||
               s_host->task.create(CLASSICUBE_NAME, classicube_task, instance,
                                   CLASSICUBE_STACK_BYTES, 3u, 1,
                                   &instance->task) != MODULE_OK) {
        instance->task = NULL;
    }
    if (!instance->task) {
        instance->running = 0;
        holo_port_display_release();
        return push_error(L, "failed to create PSRAM-backed ClassiCube task");
    }
    s_host->lua.pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L)
{
    ClassiCubeInstance *instance = instance_from_lua(L);
    if (!instance) return push_error(L, "classicube.stop: instance missing");
    instance->stop_requested = 1;
    holo_port_reset_input();
    holo_port_request_stop();
    s_host->lua.pushboolean(L, 1);
    return 1;
}

static int l_set_input(lua_State *L)
{
    uint32_t buttons = (uint32_t)s_host->lua.checkinteger(L, 1);
    int32_t lx = (int32_t)s_host->lua.checkinteger(L, 2);
    int32_t ly = (int32_t)s_host->lua.checkinteger(L, 3);
    int32_t rx = (int32_t)s_host->lua.checkinteger(L, 4);
    int32_t ry = (int32_t)s_host->lua.checkinteger(L, 5);
    holo_port_set_input(buttons, lx, ly, rx, ry);
    s_host->lua.pushboolean(L, 1);
    return 1;
}

static int l_set_input_mask(lua_State *L)
{
    HoloInputState current = holo_port_input();
    current.buttons = (uint32_t)s_host->lua.checkinteger(L, 1);
    holo_port_set_input(current.buttons, current.lx, current.ly, current.rx, current.ry);
    s_host->lua.pushboolean(L, 1);
    return 1;
}

static int l_info(lua_State *L)
{
    ClassiCubeInstance *instance = instance_from_lua(L);
    HoloInputState input;
    if (!instance) return push_error(L, "classicube.info: instance missing");
    input = holo_port_input();
    s_host->lua.createtable(L, 0, 16);
    set_string(L, "version", CLASSICUBE_VERSION);
    set_string(L, "description", CLASSICUBE_DESCRIPTION);
    set_string(L, "module_path", instance->module_path);
    set_string(L, "root", instance->root);
    set_boolean(L, "running", instance->running != 0);
    set_boolean(L, "stop_requested", instance->stop_requested != 0);
    set_integer(L, "last_result", instance->last_result);
    set_integer(L, "frames", holo_port_frame_count());
    set_integer(L, "width", CLASSICUBE_RENDER_WIDTH);
    set_integer(L, "height", CLASSICUBE_RENDER_HEIGHT);
    set_integer(L, "input_mask", input.buttons);
    set_string(L, "renderer", "SoftFP perspective/4 reciprocal-W Z32 RGB565 160x120x2");
    set_string(L, "last_error", holo_port_last_error());
    if (s_host->heap.free_size)
        set_integer(L, "psram_free", s_host->heap.free_size(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT));
    if (s_host->heap.largest_free_block)
        set_integer(L, "psram_largest", s_host->heap.largest_free_block(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT));
    return 1;
}

CLASSICUBE_EXPORT const module_manifest_t *module_query_v1(void) { return &s_manifest; }

CLASSICUBE_EXPORT int32_t module_create_v2(module_host_resolve_v2_fn resolve,
                                           void *resolve_ctx,
                                           const module_open_info_t *info,
                                           void **out_instance)
{
    int32_t result;
    if (!out_instance) return MODULE_ERR_INVALID_ARG;
    *out_instance = NULL;
    result = module_sdk_resolve_host_v2(resolve, resolve_ctx, &s_resolved_host);
    if (result != MODULE_OK) return result;
    s_host = &s_resolved_host;
    holo_port_set_host(s_host);
    memset(&s_instance, 0, sizeof(s_instance));
    s_instance.host = s_host;
    copy_text(s_instance.module_path, sizeof(s_instance.module_path), info ? info->path : "");
    copy_text(s_instance.root, sizeof(s_instance.root), "/sd/apps/classicube");
    *out_instance = &s_instance;
    holo_port_log("[classicube.so] created");
    return MODULE_OK;
}

CLASSICUBE_EXPORT int32_t module_luaopen_v1(void *instance_ptr, lua_State *L)
{
    ClassiCubeInstance *instance = (ClassiCubeInstance *)instance_ptr;
    if (!instance || !L || !s_host) return MODULE_ERR_INVALID_ARG;
    s_host->lua.createtable(L, 0, 28);
    set_string(L, "VERSION", CLASSICUBE_VERSION);
    set_string(L, "DESCRIPTION", CLASSICUBE_DESCRIPTION);
    set_integer(L, "BTN_UP", 1); set_integer(L, "BTN_DOWN", 2);
    set_integer(L, "BTN_LEFT", 4); set_integer(L, "BTN_RIGHT", 8);
    set_integer(L, "BTN_A", 16); set_integer(L, "BTN_B", 32);
    set_integer(L, "BTN_X", 64); set_integer(L, "BTN_Y", 128);
    set_integer(L, "BTN_L", 256); set_integer(L, "BTN_R", 512);
    set_integer(L, "BTN_L2", 1024); set_integer(L, "BTN_R2", 2048);
    set_integer(L, "BTN_SELECT", 4096); set_integer(L, "BTN_START", 8192);
    set_integer(L, "BTN_HOME", 32768);
    set_closure(L, "start", l_start, instance);
    set_closure(L, "stop", l_stop, instance);
    set_closure(L, "set_input", l_set_input, instance);
    set_closure(L, "set_input_mask", l_set_input_mask, instance);
    set_closure(L, "info", l_info, instance);
    return MODULE_OK;
}

CLASSICUBE_EXPORT void module_destroy_v1(void *instance_ptr)
{
    ClassiCubeInstance *instance = (ClassiCubeInstance *)instance_ptr;
    int i;
    if (!instance || !s_host) return;
    holo_port_reset_input();
    holo_port_request_stop();
    for (i = 0; instance->running && i < 300; i++) {
        if (s_host->task.delay) s_host->task.delay(10);
        else if (s_host->time.delay) s_host->time.delay(10);
    }
    if (instance->running && instance->task && s_host->task.remove) {
        s_host->task.remove(instance->task);
        instance->task = NULL;
        instance->running = 0;
        Platform_HoloForceFree();
        holo_port_display_release();
    }
    holo_port_log("[classicube.so] destroyed");
}
