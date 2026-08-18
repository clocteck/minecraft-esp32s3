#define CC_NO_UPDATER
#define CC_NO_DYNLIB
#define CC_NO_SOCKETS
#define CC_NO_THREADING
#define CC_NO_ENCRYPTION
#define CC_NO_CRASHHANDLER
#define CC_NO_OPEN
#define OVERRIDE_MEM_FUNCTIONS

#include "Stream.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Window.h"
#include "Utils.h"
#include "Errors.h"
#include "Options.h"
#include "Logger.h"
#include "holo_port.h"

#include <stdint.h>
#include <string.h>

const char *Platform_AppNameSuffix = " ESP32-S3";
cc_bool Platform_ReadonlyFilesystem;
cc_uint8 Platform_Flags = PLAT_FLAG_SINGLE_PROCESS | PLAT_FLAG_APP_EXIT;

#include "_PlatformBase.h"

static void *s_temp_mem;
static cc_result s_exit_code;

typedef union AllocationHeader AllocationHeader;
union AllocationHeader {
    struct {
        AllocationHeader *next;
        AllocationHeader *previous;
        cc_uint32 size;
    } links;
    cc_uint64 alignment;
};
static AllocationHeader *s_allocations;

/* The current host exports memcpy/memset/strlen, but not memmove. Keep this
 * implementation inside the module so overlap-safe ClassiCube copies never
 * depend on an unavailable dynamic symbol. */
void *memmove(void *dst, const void *src, size_t count)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    if (out > in && out < in + count) {
        while (count) { count--; out[count] = in[count]; }
    } else {
        size_t i;
        for (i = 0; i < count; i++) out[i] = in[i];
    }
    return dst;
}

static cc_uint32 checked_size(cc_uint32 count, cc_uint32 size)
{
    cc_uint32 total;
    if (!count || !size) return 1;
    total = count * size;
    return total / size == count ? total : 0;
}

void *TempMem_Alloc(int size)
{
    if (size <= 0 || size > 64 * 1024) return NULL;
    if (!s_temp_mem) s_temp_mem = Mem_TryAlloc(64 * 1024, 1);
    return s_temp_mem;
}

void *Mem_TryAlloc(cc_uint32 numElems, cc_uint32 elemsSize)
{
    const module_host_api_v2 *host = holo_port_host();
    cc_uint32 size = checked_size(numElems, elemsSize);
    AllocationHeader *header;
    if (!size || size > UINT32_MAX - sizeof(*header) || !host || !host->heap.malloc) return NULL;
    header = (AllocationHeader *)host->heap.malloc(
        sizeof(*header) + size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
    if (!header) return NULL;
    header->links.previous = NULL;
    header->links.next = s_allocations;
    header->links.size = size;
    if (s_allocations) s_allocations->links.previous = header;
    s_allocations = header;
    return header + 1;
}

void *Mem_TryAllocCleared(cc_uint32 numElems, cc_uint32 elemsSize)
{
    cc_uint32 size = checked_size(numElems, elemsSize);
    void *memory = size ? Mem_TryAlloc(1, size) : NULL;
    if (memory) Mem_Set(memory, 0, size);
    return memory;
}

void *Mem_TryRealloc(void *mem, cc_uint32 numElems, cc_uint32 elemsSize)
{
    cc_uint32 size = checked_size(numElems, elemsSize);
    AllocationHeader *old_header;
    void *replacement;
    if (!mem) return Mem_TryAlloc(1, size);
    if (!size) return NULL;
    old_header = ((AllocationHeader *)mem) - 1;
    replacement = Mem_TryAlloc(1, size);
    if (!replacement) return NULL;
    Mem_Copy(replacement, mem, min(size, old_header->links.size));
    Mem_Free(mem);
    return replacement;
}

void Mem_Free(void *mem)
{
    const module_host_api_v2 *host = holo_port_host();
    AllocationHeader *header;
    if (!mem || !host || !host->heap.free) return;
    header = ((AllocationHeader *)mem) - 1;
    if (header->links.previous) header->links.previous->links.next = header->links.next;
    else s_allocations = header->links.next;
    if (header->links.next) header->links.next->links.previous = header->links.previous;
    host->heap.free(header);
}

void Platform_HoloForceFree(void)
{
    const module_host_api_v2 *host = holo_port_host();
    AllocationHeader *header = s_allocations;
    if (!host || !host->heap.free) return;
    while (header) {
        AllocationHeader *next = header->links.next;
        host->heap.free(header);
        header = next;
    }
    s_allocations = NULL;
    s_temp_mem = NULL;
    holo_port_hot_vb_release();
}

void Platform_Log(const char *msg, int len)
{
    const module_host_api_v2 *host = holo_port_host();
    if (host && host->serial.write && msg && len > 0) {
        (void)host->serial.write(msg, (size_t)len);
        (void)host->serial.write("\n", 1);
    }
}

TimeMS DateTime_CurrentUTC(void)
{
    const module_host_api_v2 *host = holo_port_host();
    return UNIX_EPOCH_SECONDS + (host && host->time.millis ? host->time.millis() / 1000u : 0);
}

void DateTime_CurrentLocal(struct cc_datetime *t) { Mem_Set(t, 0, sizeof(*t)); }

cc_uint64 Stopwatch_Measure(void)
{
    const module_host_api_v2 *host = holo_port_host();
    return host && host->time.micros ? host->time.micros() : 0;
}

cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) { return end >= beg ? end - beg : 0; }

void Thread_Sleep(cc_uint32 milliseconds)
{
    const module_host_api_v2 *host = holo_port_host();
    if (host && host->task.delay) host->task.delay(milliseconds);
    else if (host && host->time.delay) host->time.delay(milliseconds);
}

void Platform_Init(void)
{
    Platform_ReadonlyFilesystem = false;
    s_exit_code = 0;
}

void Platform_Free(void)
{
    Platform_HoloForceFree();
}

cc_bool Platform_DescribeError(cc_result res, cc_string *dst)
{
    String_Format1(dst, "host error %i", &res);
    return true;
}

cc_result Process_StartGame2(const cc_string *args, int numArgs) { return 0; }

int Platform_GetCommandLineArgs(int argc, STRING_REF char **argv, cc_string *args)
{
    args[0] = String_FromReadonly("--singleplayer");
    return 1;
}

cc_result Platform_SetDefaultCurrentDirectory(void) { return 0; }

void Process_Exit(cc_result code)
{
    s_exit_code = code;
    holo_port_request_stop();
    Window_RequestClose();
}

const cc_result ReturnCode_FileShareViolation = MODULE_ERR_BUSY;
const cc_result ReturnCode_FileNotFound = MODULE_ERR_NOT_FOUND;
const cc_result ReturnCode_PathNotFound = MODULE_ERR_NOT_FOUND;
const cc_result ReturnCode_DirectoryExists = MODULE_ERR_BUSY;

void Platform_EncodePath(cc_filepath *dst, const cc_string *src)
{
    const char *root = holo_port_root();
    int i = 0, j;
    if (src->length > 0 && src->buffer[0] == '/') {
        for (j = 0; j < src->length && i + 1 < NATIVE_STR_LEN; j++) dst->buffer[i++] = src->buffer[j];
    } else {
        for (j = 0; root[j] && i + 1 < NATIVE_STR_LEN; j++) dst->buffer[i++] = root[j];
        if (i && dst->buffer[i - 1] != '/' && i + 1 < NATIVE_STR_LEN) dst->buffer[i++] = '/';
        for (j = 0; j < src->length && i + 1 < NATIVE_STR_LEN; j++) dst->buffer[i++] = src->buffer[j];
    }
    dst->buffer[i] = '\0';
}

void Platform_DecodePath(cc_string *dst, const cc_filepath *path) { String_AppendConst(dst, path->buffer); }
void Directory_GetCachePath(cc_string *path) { String_AppendConst(path, "cache"); }

cc_result Directory_Create2(const cc_filepath *path)
{
    const module_host_api_v2 *host = holo_port_host();
    if (!host || !host->sd.mkdir) return ERR_NOT_SUPPORTED;
    return host->sd.mkdir(path->buffer);
}

int File_Exists(const cc_filepath *path)
{
    const module_host_api_v2 *host = holo_port_host();
    return host && host->sd.exists && host->sd.exists(path->buffer) > 0;
}

cc_result Directory_Enum(const cc_string *dirPath, void *obj, Directory_EnumCallback callback)
{
    const module_host_api_v2 *host = holo_port_host();
    cc_filepath native;
    void *dir = NULL, *entry = NULL;
    int32_t result, is_dir;
    if (!host || !host->dir.open || !host->dir.open_next || !host->dir.close) return ERR_NOT_SUPPORTED;
    Platform_EncodePath(&native, dirPath);
    result = host->dir.open(native.buffer, &dir);
    if (result != MODULE_OK) return result;
    while (host->dir.open_next(dir, &entry) == MODULE_OK && entry) {
        const char *name = host->dir.name ? host->dir.name(entry) : "";
        cc_string path; char pathBuffer[NATIVE_STR_LEN];
        String_InitArray(path, pathBuffer);
        String_AppendString(&path, dirPath);
        if (path.length && path.buffer[path.length - 1] != '/') String_Append(&path, '/');
        String_AppendConst(&path, name);
        is_dir = 0;
        if (host->dir.is_dir) (void)host->dir.is_dir(entry, &is_dir);
        callback(&path, obj, is_dir != 0);
        host->dir.close(entry);
        entry = NULL;
    }
    host->dir.close(dir);
    return 0;
}

static void *file_ptr(cc_file file) { return (void *)(uintptr_t)(uint32_t)file; }

static cc_result file_open_mode(cc_file *file, const cc_filepath *path, uint32_t mode)
{
    const module_host_api_v2 *host = holo_port_host();
    void *raw = NULL;
    int32_t result;
    if (!host || !host->sd.open) return ERR_NOT_SUPPORTED;
    result = host->sd.open(path->buffer, mode, &raw);
    if (result == MODULE_OK) *file = (cc_file)(uint32_t)(uintptr_t)raw;
    return result;
}

cc_result File_Open(cc_file *file, const cc_filepath *path) { return file_open_mode(file, path, MODULE_FILE_READ); }
cc_result File_Create(cc_file *file, const cc_filepath *path) { return file_open_mode(file, path, MODULE_FILE_READ | MODULE_FILE_WRITE | MODULE_FILE_CREATE | MODULE_FILE_TRUNC); }
cc_result File_OpenOrCreate(cc_file *file, const cc_filepath *path)
{
    uint32_t mode = MODULE_FILE_READ | MODULE_FILE_WRITE;
    if (!File_Exists(path)) mode |= MODULE_FILE_CREATE;
    return file_open_mode(file, path, mode);
}

cc_result File_Read(cc_file file, void *data, cc_uint32 count, cc_uint32 *bytesRead)
{
    const module_host_api_v2 *host = holo_port_host();
    cc_uint8 *dst = (cc_uint8 *)data;
    cc_uint32 total = 0;
    int32_t result = MODULE_OK;
    if (!host || !host->file.read) { *bytesRead = 0; return ERR_NOT_SUPPORTED; }

    /* The host's CachedFile allocates an internal-RAM physical-I/O scratch
     * buffer as large as the request. Keep requests below the late-runtime
     * largest free block so screenshots and map saves remain reliable. */
    while (total < count) {
        size_t done = 0;
        cc_uint32 want = min(count - total, 512u);
        result = host->file.read(file_ptr(file), dst + total, want, &done);
        total += (cc_uint32)done;
        if (result != MODULE_OK || done < want) break;
    }
    *bytesRead = total;
    return result;
}

cc_result File_Write(cc_file file, const void *data, cc_uint32 count, cc_uint32 *bytesWrote)
{
    const module_host_api_v2 *host = holo_port_host();
    const cc_uint8 *src = (const cc_uint8 *)data;
    cc_uint32 total = 0;
    int32_t result = MODULE_OK;
    if (!host || !host->file.write) { *bytesWrote = 0; return ERR_NOT_SUPPORTED; }

    while (total < count) {
        size_t done = 0;
        cc_uint32 want = min(count - total, 512u);
        result = host->file.write(file_ptr(file), src + total, want, &done);
        total += (cc_uint32)done;
        if (result != MODULE_OK || done < want) break;
    }
    *bytesWrote = total;
    return result;
}

cc_result File_Close(cc_file file)
{
    const module_host_api_v2 *host = holo_port_host();
    return host && host->file.close ? host->file.close(file_ptr(file)) : ERR_NOT_SUPPORTED;
}

cc_result File_Seek(cc_file file, int offset, int seekType)
{
    const module_host_api_v2 *host = holo_port_host();
    return host && host->file.seek ? host->file.seek(file_ptr(file), offset, (uint32_t)seekType) : ERR_NOT_SUPPORTED;
}

cc_result File_Position(cc_file file, cc_uint32 *pos)
{
    const module_host_api_v2 *host = holo_port_host(); uint64_t value = 0;
    int32_t result = host && host->file.position ? host->file.position(file_ptr(file), &value) : ERR_NOT_SUPPORTED;
    *pos = (cc_uint32)value; return result;
}

cc_result File_Length(cc_file file, cc_uint32 *len)
{
    const module_host_api_v2 *host = holo_port_host(); uint64_t value = 0;
    int32_t result = host && host->file.size_bytes ? host->file.size_bytes(file_ptr(file), &value) : ERR_NOT_SUPPORTED;
    *len = (cc_uint32)value; return result;
}

void Platform_LoadSysFonts(void) { }
void CPU_FlushDataCache(void *start, int length) { }
void CPU_InvalidateDataCache(void *start, int length) { }

#include "main_impl.h"

int ClassiCube_HoloMain(void)
{
    int result;
    Platform_LogConst("[classicube.trace] setup begin");
    SetupProgram(0, NULL);
    Platform_LogConst("[classicube.trace] setup done, run begin");
    result = RunProgram(0, NULL);
    Platform_LogConst("[classicube.trace] run returned");
    Window_Free();
    Platform_Free();
    return s_exit_code ? s_exit_code : result;
}
