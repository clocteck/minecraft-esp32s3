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
    HOLO_PERF_CHUNK_BUILD,
    HOLO_PERF_CLEAR,
    HOLO_PERF_UPSCALE,
    HOLO_PERF_DRAW3D,
    HOLO_PERF_DRAW2D,
    HOLO_PERF_CONVERT,
    HOLO_PERF_DMA,
    HOLO_PERF_ASYNC_CLEAR,
    HOLO_PERF_ASYNC_WAIT,
    HOLO_PERF_DEPTH_TILE_HIT,
    HOLO_PERF_DEPTH_TILE_MISS,
    HOLO_PERF_DEPTH_TILE_WRITEBACK,
    HOLO_PERF_RASTER_SPAN_PIXELS,
    HOLO_PERF_TEXTURE_REUSE,
    HOLO_PERF_TEXTURE_LOAD,
    HOLO_PERF_TINT_REUSE,
    HOLO_PERF_TINT_LOAD,
    HOLO_PERF_PERSPECTIVE_TRIANGLES,
    HOLO_PERF_PERSPECTIVE_PIXELS,
    HOLO_PERF_PERSPECTIVE_RECIPROCALS,
    HOLO_PERF_COARSE_GROUPS,
    HOLO_PERF_COARSE_PIXELS,
    HOLO_PERF_FRUSTUM_CACHE_HIT,
    HOLO_PERF_FRUSTUM_CACHE_MISS,
    HOLO_PERF_RASTER_QUADS,
    HOLO_PERF_SPRITE_QUADS,
    HOLO_PERF_SPRITE_CULL,
    HOLO_PERF_HIZ_TESTS,
    HOLO_PERF_HIZ_CULL,
    HOLO_PERF_CHUNK_HIZ_TESTS,
    HOLO_PERF_CHUNK_HIZ_CULL,
    HOLO_PERF_PROJECTED_VERTICES,
    HOLO_PERF_PROJECTION_REUSE,
    HOLO_PERF_FAST_CUTOUT_PIXELS,
    HOLO_PERF_FOG_PIXELS,
    HOLO_PERF_QUAD_CULL,
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
                        int32_t yaw100, int32_t pitch100, uint32_t hud_vertices);

#endif
