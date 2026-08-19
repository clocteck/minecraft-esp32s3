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
    char data[896];
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
static uint32_t s_pose_world_vertices;
static uint32_t s_map_view_distance, s_map_render_distance, s_map_visible_chunks;
static uint32_t s_map_resident_chunks, s_map_pending_chunks;
static uint32_t s_map_failed_chunks, s_map_mesh_vertices;

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
    log_append_avg_ms(&line, " pre3d=", s_total.slots[HOLO_PERF_SCENE_PRE_MAP]);
    log_append_avg_ms(&line, " mid3d=", s_total.slots[HOLO_PERF_SCENE_MID_MAP]);
    log_append_avg_ms(&line, " post3d=", s_total.slots[HOLO_PERF_SCENE_POST_MAP]);
    log_append_avg_ms(&line, " held=", s_total.slots[HOLO_PERF_HELD_BLOCK]);
    log_append_avg_ms(&line, " sides=", s_total.slots[HOLO_PERF_MAP_SIDES]);
    log_append_avg_ms(&line, " shSel=", s_total.slots[HOLO_PERF_SHADOW_SELECTION]);
    log_append_avg_ms(&line, " build=", s_total.slots[HOLO_PERF_CHUNK_BUILD]);
    log_append_avg_ms(&line, " clear=", s_total.slots[HOLO_PERF_CLEAR]);
    log_append_avg_ms(&line, " scale=", s_total.slots[HOLO_PERF_UPSCALE]);
#ifdef CC_BUILD_ESP32S3_HOT_PROBES
    log_append_avg_ms(&line, " draw3d=", s_total.slots[HOLO_PERF_DRAW3D]);
    log_append_avg_ms(&line, " draw2d=", s_total.slots[HOLO_PERF_DRAW2D]);
#endif
    log_append_avg_ms(&line, " conv=", s_total.slots[HOLO_PERF_CONVERT]);
    log_append_avg_ms(&line, " dma=", s_total.slots[HOLO_PERF_DMA]);
    log_append_avg_ms(&line, " wait0=", s_total.slots[HOLO_PERF_ASYNC_WAIT]);
#ifdef CC_BUILD_ESP32S3_HOT_PROBES
    log_append_avg_ms(&line, " qCull=", s_total.slots[HOLO_PERF_MAP_CULL]);
    log_append_avg_ms(&line, " qXf=", s_total.slots[HOLO_PERF_MAP_XFORM]);
    log_append_avg_ms(&line, " qProj=", s_total.slots[HOLO_PERF_MAP_PROJECT]);
    log_append_avg_ms(&line, " qDraw=", s_total.slots[HOLO_PERF_MAP_DRAW]);
    log_append_avg_ms(&line, " qSet=", s_total.slots[HOLO_PERF_MAP_SETUP]);
    log_append_avg_ms(&line, " qRast=", s_total.slots[HOLO_PERF_MAP_RASTER]);
    log_append_avg_ms(&line, " qHiZ=", s_total.slots[HOLO_PERF_MAP_HIZ]);
    log_append_avg_ms(&line, " qClip=", s_total.slots[HOLO_PERF_MAP_CLIP]);
    log_append_avg_ms(&line, " qWait=", s_total.slots[HOLO_PERF_RASTER_WAIT]);
    log_append_avg_ms(&line, " qFree=", s_total.slots[HOLO_PERF_RASTER_FREE_WAIT]);
    log_append_avg_ms(&line, " qEnd=", s_total.slots[HOLO_PERF_RASTER_FLUSH_WAIT]);
#endif
    log_append_avg_ms(&line, " shadow=", s_total.slots[HOLO_PERF_ENTITY_SHADOW]);
    log_append_avg_ms(&line, " sel=", s_total.slots[HOLO_PERF_SELECTION]);
    holo_port_log(line.data);

    line.length = 0; line.data[0] = '\0';
    log_append_text(&line,
#ifdef CC_BUILD_ESP32S3_HOT_PROBES
        "[cc.perf.work]"
#else
        "[cc.perf.mem]"
#endif
    );
#ifdef CC_BUILD_ESP32S3_HOT_PROBES
    log_append_avg_count(&line, " quad=", s_total.quads);
    log_append_avg_count(&line, " tri=", s_total.triangles);
    log_append_avg_count(&line, " bboxpx=", s_total.bbox_pixels);
    log_append_avg_count(&line, " spanpx=", s_total.slots[HOLO_PERF_RASTER_SPAN_PIXELS]);
    log_append_avg_count(&line, " texHit=", s_total.slots[HOLO_PERF_TEXTURE_REUSE]);
    log_append_avg_count(&line, " texMiss=", s_total.slots[HOLO_PERF_TEXTURE_LOAD]);
    log_append_avg_count(&line, " tintHit=", s_total.slots[HOLO_PERF_TINT_REUSE]);
    log_append_avg_count(&line, " tintMiss=", s_total.slots[HOLO_PERF_TINT_LOAD]);
    log_append_avg_count(&line, " perspTri=", s_total.slots[HOLO_PERF_PERSPECTIVE_TRIANGLES]);
    log_append_avg_count(&line, " perspPx=", s_total.slots[HOLO_PERF_PERSPECTIVE_PIXELS]);
    log_append_avg_count(&line, " perspRc=", s_total.slots[HOLO_PERF_PERSPECTIVE_RECIPROCALS]);
    log_append_avg_count(&line, " rQuad=", s_total.slots[HOLO_PERF_RASTER_QUADS]);
    log_append_avg_count(&line, " oLutQ=", s_total.slots[HOLO_PERF_OPAQUE_LUT_QUADS]);
    log_append_avg_count(&line, " sprQ=", s_total.slots[HOLO_PERF_SPRITE_QUADS]);
    log_append_avg_count(&line, " sprCull=", s_total.slots[HOLO_PERF_SPRITE_CULL]);
    log_append_avg_count(&line, " uiFast=", s_total.slots[HOLO_PERF_UI_FAST_SPRITES]);
    log_append_avg_count(&line, " uiGen=", s_total.slots[HOLO_PERF_UI_GENERIC_SPRITES]);
    log_append_avg_count(&line, " uiPx=", s_total.slots[HOLO_PERF_UI_SPRITE_PIXELS]);
    log_append_avg_count(&line, " cellSkip=", s_total.slots[HOLO_PERF_HIZ_CELL_SKIP_PIXELS]);
    log_append_avg_count(&line, " frCull=", s_total.slots[HOLO_PERF_FRUSTUM_CULL]);
    log_append_avg_count(&line, " bfCull=", s_total.slots[HOLO_PERF_BACKFACE_CULL]);
    log_append_avg_count(&line, " tmpCull=", s_total.slots[HOLO_PERF_TEMPORAL_CULL]);
    log_append_avg_count(&line, " rangeH=", s_total.slots[HOLO_PERF_RANGE_CACHE_HIT]);
    log_append_avg_count(&line, " rangeM=", s_total.slots[HOLO_PERF_RANGE_CACHE_MISS]);
    log_append_avg_count(&line, " frGrp=", s_total.slots[HOLO_PERF_FRUSTUM_GROUP_CULL]);
    log_append_avg_count(&line, " hizQC=", s_total.slots[HOLO_PERF_CURRENT_HIZ_CULL]);
    log_append_avg_count(&line, " prevQC=", s_total.slots[HOLO_PERF_PREVIOUS_HIZ_CULL]);
    log_append_avg_count(&line, " prevBC=", s_total.slots[HOLO_PERF_PREVIOUS_BOUNDS_CULL]);
    log_append_avg_count(&line, " c0Job=", s_total.slots[HOLO_PERF_CORE0_RASTER_JOBS]);
    log_append_avg_count(&line, " c0Fb=", s_total.slots[HOLO_PERF_CORE0_RASTER_FALLBACKS]);
    log_append_avg_count(&line, " c0Fl=", s_total.slots[HOLO_PERF_CORE0_RASTER_FLUSHES]);
    log_append_avg_count(&line, " c0Bt=", s_total.slots[HOLO_PERF_CORE0_RASTER_BATCHES]);
    log_append_avg_count(&line, " mN=", s_total.slots[HOLO_PERF_MAP_MODE_NONE]);
    log_append_avg_count(&line, " mO=", s_total.slots[HOLO_PERF_MAP_MODE_OPAQUE]);
    log_append_avg_count(&line, " mD=", s_total.slots[HOLO_PERF_MAP_MODE_DEPTH]);
    log_append_avg_count(&line, " mH=", s_total.slots[HOLO_PERF_MAP_MODE_HALF]);
    log_append_avg_count(&line, " hCall=", s_total.slots[HOLO_PERF_HALF_RASTER_CALLS]);
    log_append_avg_count(&line, " c0T=", s_total.slots[HOLO_PERF_CORE0_TRANS_JOBS]);
    log_append_avg_count(&line, " clip0=", s_total.slots[HOLO_PERF_CLIP_ZERO_MASK]);
#endif
    log_append_avg_count(&line, " chunks=", s_total.chunk_builds);
    log_append_avg_count(&line, " dmaN=", s_total.dma_submits);
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
    log_append_text(&line, "[cc.perf.map] view=");
    log_append_u64(&line, s_map_view_distance);
    log_append_text(&line, " render=");
    log_append_u64(&line, s_map_render_distance);
    log_append_text(&line, " visible=");
    log_append_u64(&line, s_map_visible_chunks);
    log_append_text(&line, " mesh=");
    log_append_u64(&line, s_map_resident_chunks);
    log_append_text(&line, " pending=");
    log_append_u64(&line, s_map_pending_chunks);
    log_append_text(&line, " fail=");
    log_append_u64(&line, s_map_failed_chunks);
    log_append_text(&line, " meshV=");
    log_append_u64(&line, s_map_mesh_vertices);
    log_append_text(&line, " vbKB=");
    log_append_u64(&line, ((uint64_t)s_map_mesh_vertices * 12U + 1023U) / 1024U);
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
    log_append_text(&line, " worldV=");
    log_append_u64(&line, s_pose_world_vertices);
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
                        int32_t yaw100, int32_t pitch100, uint32_t world_vertices)
{
    s_pose_x100 = x100; s_pose_y100 = y100; s_pose_z100 = z100;
    s_pose_yaw100 = yaw100; s_pose_pitch100 = pitch100;
    s_pose_world_vertices = world_vertices;
}

void holo_perf_set_map_state(uint32_t view_distance, uint32_t render_distance,
                             uint32_t visible_chunks,
                             uint32_t resident_chunks, uint32_t pending_chunks,
                             uint32_t failed_chunks, uint32_t mesh_vertices)
{
    s_map_view_distance  = view_distance;
    s_map_render_distance = render_distance;
    s_map_visible_chunks = visible_chunks;
    s_map_resident_chunks = resident_chunks;
    s_map_pending_chunks = pending_chunks;
    s_map_failed_chunks  = failed_chunks;
    s_map_mesh_vertices  = mesh_vertices;
}
