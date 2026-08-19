#ifndef CLASSICUBE_HOLO_PERF_H
#define CLASSICUBE_HOLO_PERF_H

#include <stdint.h>

typedef enum HoloPerfSlot {
    HOLO_PERF_LOGIC = 0,
    HOLO_PERF_RENDER3D,
    HOLO_PERF_GUI2D,
    HOLO_PERF_PRESENT,
    HOLO_PERF_MAP_UPDATE,
    HOLO_PERF_MAP_OPAQUE,
    HOLO_PERF_MAP_TRANSLUCENT,
    HOLO_PERF_SCENE_PRE_MAP,
    HOLO_PERF_SCENE_MID_MAP,
    HOLO_PERF_SCENE_POST_MAP,
    HOLO_PERF_HELD_BLOCK,
    HOLO_PERF_MAP_SIDES,
    HOLO_PERF_SHADOW_SELECTION,
    HOLO_PERF_CHUNK_BUILD,
    HOLO_PERF_CLEAR,
    HOLO_PERF_UPSCALE,
    HOLO_PERF_DRAW3D,
    HOLO_PERF_DRAW2D,
    HOLO_PERF_CONVERT,
    HOLO_PERF_DMA,
    HOLO_PERF_ASYNC_WAIT,
    HOLO_PERF_RASTER_SPAN_PIXELS,
    HOLO_PERF_TEXTURE_REUSE,
    HOLO_PERF_TEXTURE_LOAD,
    HOLO_PERF_TINT_REUSE,
    HOLO_PERF_TINT_LOAD,
    HOLO_PERF_PERSPECTIVE_TRIANGLES,
    HOLO_PERF_PERSPECTIVE_PIXELS,
    HOLO_PERF_PERSPECTIVE_RECIPROCALS,
    HOLO_PERF_RASTER_QUADS,
    HOLO_PERF_OPAQUE_LUT_QUADS,
    HOLO_PERF_SPRITE_QUADS,
    HOLO_PERF_SPRITE_CULL,
    HOLO_PERF_UI_FAST_SPRITES,
    HOLO_PERF_UI_GENERIC_SPRITES,
    HOLO_PERF_UI_SPRITE_PIXELS,
    HOLO_PERF_HIZ_CELL_SKIP_PIXELS,
    HOLO_PERF_FRUSTUM_CULL,
    HOLO_PERF_BACKFACE_CULL,
    HOLO_PERF_TEMPORAL_CULL,
    HOLO_PERF_RANGE_CACHE_HIT,
    HOLO_PERF_RANGE_CACHE_MISS,
    HOLO_PERF_FRUSTUM_GROUP_CULL,
    HOLO_PERF_CORE0_RASTER_JOBS,
    HOLO_PERF_CORE0_RASTER_FALLBACKS,
    HOLO_PERF_CORE0_RASTER_FLUSHES,
    HOLO_PERF_CORE0_RASTER_BATCHES,
    HOLO_PERF_MAP_CULL,
    HOLO_PERF_MAP_XFORM,
    HOLO_PERF_MAP_PROJECT,
    HOLO_PERF_MAP_DRAW,
    HOLO_PERF_MAP_SETUP,
    HOLO_PERF_MAP_RASTER,
    HOLO_PERF_MAP_HIZ,
    HOLO_PERF_MAP_CLIP,
    HOLO_PERF_RASTER_WAIT,
    HOLO_PERF_RASTER_FREE_WAIT,
    HOLO_PERF_RASTER_FLUSH_WAIT,
    HOLO_PERF_ENTITY_SHADOW,
    HOLO_PERF_SELECTION,
    HOLO_PERF_CURRENT_HIZ_CULL,
    HOLO_PERF_PREVIOUS_HIZ_CULL,
    HOLO_PERF_PREVIOUS_BOUNDS_CULL,
    HOLO_PERF_MAP_MODE_NONE,
    HOLO_PERF_MAP_MODE_OPAQUE,
    HOLO_PERF_MAP_MODE_DEPTH,
    HOLO_PERF_MAP_MODE_HALF,
    HOLO_PERF_HALF_RASTER_CALLS,
    HOLO_PERF_CORE0_TRANS_JOBS,
    HOLO_PERF_CLIP_ZERO_MASK,
    HOLO_PERF_SLOT_COUNT
} HoloPerfSlot;

uint64_t holo_perf_now_us(void);
void holo_perf_frame_begin(void);
void holo_perf_frame_cancel(void);
void holo_perf_frame_end(void);
void holo_perf_add(HoloPerfSlot slot, uint64_t elapsed_us);
void holo_perf_count_quads(uint32_t count);
void holo_perf_count_triangles(uint32_t count);
void holo_perf_count_bbox_pixels(uint64_t count);
void holo_perf_count_chunk_build(void);
void holo_perf_count_dma_submit(void);
void holo_perf_set_pose(int32_t x100, int32_t y100, int32_t z100,
                        int32_t yaw100, int32_t pitch100, uint32_t world_vertices);
void holo_perf_set_map_state(uint32_t view_distance, uint32_t render_distance,
                             uint32_t visible_chunks,
                             uint32_t resident_chunks, uint32_t pending_chunks,
                             uint32_t failed_chunks, uint32_t mesh_vertices);

#endif
