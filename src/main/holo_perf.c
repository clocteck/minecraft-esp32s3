#pragma GCC optimize("O2")

#include "holo_perf.h"
#include "holo_port.h"

#include <stddef.h>
#include <stdint.h>

#define HOLO_PERF_REPORT_US 4000000ULL

typedef struct HoloPerfCounters {
    uint64_t slots[HOLO_PERF_SLOT_COUNT];
    uint64_t quads;
    uint64_t triangles;
    uint64_t bbox_pixels;
    uint64_t chunk_builds;
    uint64_t dma_submits;
} HoloPerfCounters;

typedef struct HoloLogBuilder {
    char data[672];
    size_t length;
} HoloLogBuilder;

static HoloPerfCounters s_frame;
static HoloPerfCounters s_total;
static uint64_t s_frame_start_us;
static uint64_t s_window_start_us;
static uint64_t s_frame_total_us;
static uint64_t s_frame_max_us;
static uint32_t s_frames;
static uint8_t s_frame_active;
static int32_t s_pose_x100, s_pose_y100, s_pose_z100;
static int32_t s_pose_yaw100, s_pose_pitch100;
static uint32_t s_pose_hud_vertices;

static void counters_clear(HoloPerfCounters *counters)
{
    uint32_t i;
    for (i = 0; i < HOLO_PERF_SLOT_COUNT; i++) counters->slots[i] = 0;
    counters->quads = 0;
    counters->triangles = 0;
    counters->bbox_pixels = 0;
    counters->chunk_builds = 0;
    counters->dma_submits = 0;
}

static void counters_accumulate(HoloPerfCounters *dst, const HoloPerfCounters *src)
{
    uint32_t i;
    for (i = 0; i < HOLO_PERF_SLOT_COUNT; i++) dst->slots[i] += src->slots[i];
    dst->quads += src->quads;
    dst->triangles += src->triangles;
    dst->bbox_pixels += src->bbox_pixels;
    dst->chunk_builds += src->chunk_builds;
    dst->dma_submits += src->dma_submits;
}

static void log_append_char(HoloLogBuilder *builder, char value)
{
    if (builder->length + 1 >= sizeof(builder->data)) return;
    builder->data[builder->length++] = value;
    builder->data[builder->length] = '\0';
}

static void log_append_text(HoloLogBuilder *builder, const char *text)
{
    if (!text) return;
    while (*text && builder->length + 1 < sizeof(builder->data)) {
        builder->data[builder->length++] = *text++;
    }
    builder->data[builder->length] = '\0';
}

static void log_append_u64(HoloLogBuilder *builder, uint64_t value)
{
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count) log_append_char(builder, digits[--count]);
}

static void log_append_fixed2(HoloLogBuilder *builder, uint64_t hundredths)
{
    log_append_u64(builder, hundredths / 100U);
    log_append_char(builder, '.');
    log_append_char(builder, (char)('0' + (hundredths / 10U) % 10U));
    log_append_char(builder, (char)('0' + hundredths % 10U));
}

static void log_append_signed_fixed2(HoloLogBuilder *builder, int32_t hundredths)
{
    uint32_t magnitude;
    if (hundredths < 0) {
        log_append_char(builder, '-');
        magnitude = (uint32_t)(-(int64_t)hundredths);
    } else {
        magnitude = (uint32_t)hundredths;
    }
    log_append_fixed2(builder, magnitude);
}

static void log_append_avg_ms(HoloLogBuilder *builder, const char *name, uint64_t total_us)
{
    uint64_t average_us = s_frames ? total_us / s_frames : 0;
    log_append_text(builder, name);
    log_append_u64(builder, average_us / 1000U);
    log_append_char(builder, '.');
    log_append_char(builder, (char)('0' + (average_us % 1000U) / 100U));
}

static void log_append_avg_count(HoloLogBuilder *builder, const char *name, uint64_t total)
{
    log_append_text(builder, name);
    log_append_u64(builder, s_frames ? total / s_frames : 0);
}

static void report_window(uint64_t now_us)
{
    const module_host_api_v2 *host = holo_port_host();
    uint64_t window_us = now_us - s_window_start_us;
    uint64_t fps100 = window_us ? ((uint64_t)s_frames * 100000000ULL) / window_us : 0;
    uint64_t avg_frame = s_frames ? s_frame_total_us / s_frames : 0;
    HoloLogBuilder line = { { 0 }, 0 };
    size_t internal_free = 0, internal_largest = 0, psram_free = 0;

    log_append_text(&line, "[cc.perf] n=");
    log_append_u64(&line, s_frames);
    log_append_text(&line, " fps=");
    log_append_fixed2(&line, fps100);
    log_append_text(&line, " frame=");
    log_append_u64(&line, avg_frame / 1000U);
    log_append_char(&line, '.');
    log_append_char(&line, (char)('0' + (avg_frame % 1000U) / 100U));
    log_append_text(&line, " max=");
    log_append_u64(&line, s_frame_max_us / 1000U);
    log_append_avg_ms(&line, " logic=", s_total.slots[HOLO_PERF_LOGIC]);
    log_append_avg_ms(&line, " 3d=", s_total.slots[HOLO_PERF_RENDER3D]);
    log_append_avg_ms(&line, " gui=", s_total.slots[HOLO_PERF_GUI2D]);
    log_append_avg_ms(&line, " present=", s_total.slots[HOLO_PERF_PRESENT]);
    holo_port_log(line.data);

    line.length = 0; line.data[0] = '\0';
    log_append_text(&line, "[cc.perf.gfx]");
    log_append_avg_ms(&line, " mapupd=", s_total.slots[HOLO_PERF_MAP_UPDATE]);
    log_append_avg_ms(&line, " opaque=", s_total.slots[HOLO_PERF_MAP_OPAQUE]);
    log_append_avg_ms(&line, " trans=", s_total.slots[HOLO_PERF_MAP_TRANSLUCENT]);
    log_append_avg_ms(&line, " build=", s_total.slots[HOLO_PERF_CHUNK_BUILD]);
    log_append_avg_ms(&line, " clear=", s_total.slots[HOLO_PERF_CLEAR]);
    log_append_avg_ms(&line, " scale=", s_total.slots[HOLO_PERF_UPSCALE]);
    log_append_avg_ms(&line, " draw3d=", s_total.slots[HOLO_PERF_DRAW3D]);
    log_append_avg_ms(&line, " draw2d=", s_total.slots[HOLO_PERF_DRAW2D]);
    log_append_avg_ms(&line, " conv=", s_total.slots[HOLO_PERF_CONVERT]);
    log_append_avg_ms(&line, " dma=", s_total.slots[HOLO_PERF_DMA]);
    log_append_avg_ms(&line, " clear0=", s_total.slots[HOLO_PERF_ASYNC_CLEAR]);
    log_append_avg_ms(&line, " wait0=", s_total.slots[HOLO_PERF_ASYNC_WAIT]);
    holo_port_log(line.data);

    line.length = 0; line.data[0] = '\0';
    log_append_text(&line, "[cc.perf.work]");
    log_append_avg_count(&line, " quad=", s_total.quads);
    log_append_avg_count(&line, " tri=", s_total.triangles);
    log_append_avg_count(&line, " bboxpx=", s_total.bbox_pixels);
    log_append_avg_count(&line, " chunks=", s_total.chunk_builds);
    log_append_avg_count(&line, " dmaN=", s_total.dma_submits);
    log_append_avg_count(&line, " zHit=", s_total.slots[HOLO_PERF_DEPTH_TILE_HIT]);
    log_append_avg_count(&line, " zMiss=", s_total.slots[HOLO_PERF_DEPTH_TILE_MISS]);
    log_append_avg_count(&line, " zWB=", s_total.slots[HOLO_PERF_DEPTH_TILE_WRITEBACK]);
    log_append_avg_count(&line, " spanpx=", s_total.slots[HOLO_PERF_RASTER_SPAN_PIXELS]);
    log_append_avg_count(&line, " texHit=", s_total.slots[HOLO_PERF_TEXTURE_REUSE]);
    log_append_avg_count(&line, " texMiss=", s_total.slots[HOLO_PERF_TEXTURE_LOAD]);
    log_append_avg_count(&line, " tintHit=", s_total.slots[HOLO_PERF_TINT_REUSE]);
    log_append_avg_count(&line, " tintMiss=", s_total.slots[HOLO_PERF_TINT_LOAD]);
    log_append_avg_count(&line, " perspTri=", s_total.slots[HOLO_PERF_PERSPECTIVE_TRIANGLES]);
    log_append_avg_count(&line, " perspPx=", s_total.slots[HOLO_PERF_PERSPECTIVE_PIXELS]);
    log_append_avg_count(&line, " perspRc=", s_total.slots[HOLO_PERF_PERSPECTIVE_RECIPROCALS]);
    log_append_avg_count(&line, " coarseG=", s_total.slots[HOLO_PERF_COARSE_GROUPS]);
    log_append_avg_count(&line, " coarsePx=", s_total.slots[HOLO_PERF_COARSE_PIXELS]);
    log_append_avg_count(&line, " fcHit=", s_total.slots[HOLO_PERF_FRUSTUM_CACHE_HIT]);
    log_append_avg_count(&line, " fcMiss=", s_total.slots[HOLO_PERF_FRUSTUM_CACHE_MISS]);
    log_append_avg_count(&line, " rQuad=", s_total.slots[HOLO_PERF_RASTER_QUADS]);
    log_append_avg_count(&line, " sprQ=", s_total.slots[HOLO_PERF_SPRITE_QUADS]);
    log_append_avg_count(&line, " sprCull=", s_total.slots[HOLO_PERF_SPRITE_CULL]);
    log_append_avg_count(&line, " hizQ=", s_total.slots[HOLO_PERF_HIZ_TESTS]);
    log_append_avg_count(&line, " hizCull=", s_total.slots[HOLO_PERF_HIZ_CULL]);
    log_append_avg_count(&line, " chzQ=", s_total.slots[HOLO_PERF_CHUNK_HIZ_TESTS]);
    log_append_avg_count(&line, " chzCull=", s_total.slots[HOLO_PERF_CHUNK_HIZ_CULL]);
    log_append_avg_count(&line, " projV=", s_total.slots[HOLO_PERF_PROJECTED_VERTICES]);
    log_append_avg_count(&line, " projReuse=", s_total.slots[HOLO_PERF_PROJECTION_REUSE]);
    log_append_avg_count(&line, " cutPx=", s_total.slots[HOLO_PERF_FAST_CUTOUT_PIXELS]);
    log_append_avg_count(&line, " fogPx=", s_total.slots[HOLO_PERF_FOG_PIXELS]);
    log_append_avg_count(&line, " cull=", s_total.slots[HOLO_PERF_QUAD_CULL]);
    if (host && host->heap.free_size) {
        internal_free = host->heap.free_size(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
        psram_free = host->heap.free_size(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
    }
    if (host && host->heap.largest_free_block) {
        internal_largest = host->heap.largest_free_block(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    }
    log_append_text(&line, " iramFree="); log_append_u64(&line, internal_free);
    log_append_text(&line, " iramMax=");  log_append_u64(&line, internal_largest);
    log_append_text(&line, " hotRam=");
    log_append_u64(&line, holo_port_hot_vb_reserved());
    log_append_text(&line, " hotUsed=");
    log_append_u64(&line, holo_port_hot_vb_used());
    log_append_text(&line, " hotN=");
    log_append_u64(&line, holo_port_hot_vb_count());
    log_append_text(&line, " hotFb=");
    log_append_u64(&line, holo_port_hot_vb_fallbacks());
    log_append_text(&line, " psramFree="); log_append_u64(&line, psram_free);
    holo_port_log(line.data);

    line.length = 0; line.data[0] = '\0';
    log_append_text(&line, "[cc.perf.pose] x=");
    log_append_signed_fixed2(&line, s_pose_x100);
    log_append_text(&line, " y=");
    log_append_signed_fixed2(&line, s_pose_y100);
    log_append_text(&line, " z=");
    log_append_signed_fixed2(&line, s_pose_z100);
    log_append_text(&line, " yaw=");
    log_append_signed_fixed2(&line, s_pose_yaw100);
    log_append_text(&line, " pitch=");
    log_append_signed_fixed2(&line, s_pose_pitch100);
    log_append_text(&line, " hud=");
    log_append_u64(&line, s_pose_hud_vertices);
    holo_port_log(line.data);
}

uint64_t holo_perf_now_us(void)
{
    const module_host_api_v2 *host = holo_port_host();
    return host && host->time.micros ? host->time.micros() : 0;
}

void holo_perf_frame_begin(void)
{
    uint64_t now_us = holo_perf_now_us();
    counters_clear(&s_frame);
    if (!s_window_start_us) s_window_start_us = now_us;
    s_frame_start_us = now_us;
    s_frame_active = 1;
}

void holo_perf_frame_cancel(void)
{
    s_frame_active = 0;
}

void holo_perf_frame_end(void)
{
    uint64_t now_us, elapsed_us;
    if (!s_frame_active) return;
    now_us = holo_perf_now_us();
    elapsed_us = now_us - s_frame_start_us;
    counters_accumulate(&s_total, &s_frame);
    s_frame_total_us += elapsed_us;
    if (elapsed_us > s_frame_max_us) s_frame_max_us = elapsed_us;
    s_frames++;
    s_frame_active = 0;

    if (now_us - s_window_start_us < HOLO_PERF_REPORT_US) return;
    report_window(now_us);
    counters_clear(&s_total);
    s_frame_total_us = 0;
    s_frame_max_us = 0;
    s_frames = 0;
    s_window_start_us = now_us;
}

void holo_perf_add(HoloPerfSlot slot, uint64_t elapsed_us)
{
    if (!s_frame_active || (uint32_t)slot >= HOLO_PERF_SLOT_COUNT) return;
    s_frame.slots[slot] += elapsed_us;
}

void holo_perf_count_quads(uint32_t count) { if (s_frame_active) s_frame.quads += count; }
void holo_perf_count_triangles(uint32_t count) { if (s_frame_active) s_frame.triangles += count; }
void holo_perf_count_bbox_pixels(uint64_t count) { if (s_frame_active) s_frame.bbox_pixels += count; }
void holo_perf_count_chunk_build(void) { if (s_frame_active) s_frame.chunk_builds++; }
void holo_perf_count_dma_submit(void) { if (s_frame_active) s_frame.dma_submits++; }
void holo_perf_set_pose(int32_t x100, int32_t y100, int32_t z100,
                        int32_t yaw100, int32_t pitch100, uint32_t hud_vertices)
{
    s_pose_x100 = x100; s_pose_y100 = y100; s_pose_z100 = z100;
    s_pose_yaw100 = yaw100; s_pose_pitch100 = pitch100;
    s_pose_hud_vertices = hud_vertices;
}
