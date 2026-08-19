#include "Core.h"
#if CC_GFX_BACKEND == CC_GFX_BACKEND_SOFTFP
#ifndef CC_BUILD_ESP32S3
#define CC_DYNAMIC_VBS_ARE_STATIC
#endif
#define OVERRIDE_BEGEND2D_FUNCTIONS
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Window.h"
#include "Camera.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#ifdef CC_BUILD_ESP32S3
#include "holo_perf.h"
#include "holo_port.h"
#include "Platform.h"
#if defined(__GNUC__)
#pragma GCC optimize ("O3")
#endif
#endif


// 16.16 fixed point
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (FP_ONE >> 1)
#define FP_MASK ((1 << FP_SHIFT) - 1)

#define ABS(x) ((x) < 0 ? -(x) : (x))

#define FloatToFixed(x) ((int)((x) * FP_ONE))
#define FixedToFloat(x) ((float)(x) / FP_ONE)
#define FixedToInt(x) ((x) >> FP_SHIFT)
#define IntToFixed(x) ((x) << FP_SHIFT)

#define FixedMul(a, b) (((int64_t)(a) * (b)) >> FP_SHIFT)

#define FixedDiv(a, b) (((int64_t)(a) << FP_SHIFT) / (b))

enum { PLANE_LEFT=0, PLANE_RIGHT, PLANE_BOTTOM, PLANE_TOP, PLANE_NEAR, PLANE_FAR };
#define NUM_PLANES 6

static int FixedReciprocal(int x) {
    if (x == 0) return 0;
    int64_t res = ((int64_t)FP_ONE << FP_SHIFT) / (int64_t)x; // (1<<32)/x -> fixed reciprocal
    if (res > INT_MAX) return INT_MAX;
    if (res < INT_MIN) return INT_MIN;
    return (int)res;
}

static inline int FixedReciprocalFast(int x) {
    uint32_t magnitude;
    uint32_t quotient;

    if (x == 0) return 0;
    magnitude = x < 0 ? (0u - (uint32_t)x) : (uint32_t)x;
    if (magnitude == 1u) return x > 0 ? INT_MAX : INT_MIN;
    quotient  = UINT32_MAX / magnitude;
    /* floor(2^32 / magnitude) exceeds floor((2^32 - 1) / magnitude)
     * exactly when magnitude divides 2^32, i.e. when it is a power of two. */
    if (!(magnitude & (magnitude - 1u))) quotient++;

    if (x > 0) return quotient > (uint32_t)INT_MAX ? INT_MAX : (int)quotient;
    if (quotient >= 0x80000000u) return INT_MIN;
    return -(int)quotient;
}

static cc_bool faceCulling;
static int fb_width, fb_height; 
static struct Bitmap fb_bmp;
static int vp_hwidth_fp, vp_hheight_fp;
static int fb_maxX, fb_maxY;

#ifdef CC_BUILD_SOFTFP_FB16
typedef cc_uint16 SoftPixel;

static CC_INLINE SoftPixel SoftPixel_Make(int r, int g, int b) {
    return (SoftPixel)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static CC_INLINE int SoftPixel_R(SoftPixel color) {
    int value = (color >> 11) & 0x1F;
    return (value << 3) | (value >> 2);
}
static CC_INLINE int SoftPixel_G(SoftPixel color) {
    int value = (color >> 5) & 0x3F;
    return (value << 2) | (value >> 4);
}
static CC_INLINE int SoftPixel_B(SoftPixel color) {
    int value = color & 0x1F;
    return (value << 3) | (value >> 2);
}
static CC_INLINE SoftPixel SoftPixel_FromBitmap(BitmapCol color) {
    return SoftPixel_Make(BitmapCol_R(color), BitmapCol_G(color), BitmapCol_B(color));
}
static CC_INLINE BitmapCol SoftPixel_ToBitmap(SoftPixel color) {
    return BitmapCol_Make(SoftPixel_R(color), SoftPixel_G(color), SoftPixel_B(color), 0xFF);
}
#else
typedef BitmapCol SoftPixel;
#define SoftPixel_Make(r, g, b) BitmapCol_Make((r), (g), (b), 0xFF)
#define SoftPixel_R(c) BitmapCol_R(c)
#define SoftPixel_G(c) BitmapCol_G(c)
#define SoftPixel_B(c) BitmapCol_B(c)
static CC_INLINE SoftPixel SoftPixel_FromBitmap(BitmapCol color) { return color; }
static CC_INLINE BitmapCol SoftPixel_ToBitmap(SoftPixel color) { return color; }
#endif

static SoftPixel* colorBuffer;
static BitmapCol clearColor;
static cc_bool colWrite = true;
static int cb_stride;
#ifdef CC_BUILD_SOFTFP_FOG4
static SoftPixel fogPixel;
static int fogWStart, fogWQuarter, fogWHalf, fogWEnd;
#endif

#ifdef CC_BUILD_SOFTFP_Z16_TILES
#define DEPTH_CLEAR_VALUE 0xFFFFu
#elif defined CC_BUILD_SOFTFP_W_DEPTH8
#define DEPTH_CLEAR_VALUE 0xFFu
#endif

#ifdef CC_BUILD_SOFTFP_Z16_TILES
typedef cc_uint16 DepthValue;
#define DEPTH_TILE_SHIFT        4
#define DEPTH_TILE_SIZE         (1 << DEPTH_TILE_SHIFT)
#define DEPTH_TILE_PIXELS       (DEPTH_TILE_SIZE * DEPTH_TILE_SIZE)
#define DEPTH_CACHE_LINES       64
#define DEPTH_TILE_COLS         ((CLASSICUBE_RENDER_WIDTH  + DEPTH_TILE_SIZE - 1) / DEPTH_TILE_SIZE)
#define DEPTH_TILE_ROWS         ((CLASSICUBE_RENDER_HEIGHT + DEPTH_TILE_SIZE - 1) / DEPTH_TILE_SIZE)
#define DEPTH_TILE_COUNT        (DEPTH_TILE_COLS * DEPTH_TILE_ROWS)
static DepthValue* depthBuffer;
static DepthValue* depthCachePixels;
static cc_uint16 depthCacheTags[DEPTH_CACHE_LINES];
static cc_uint8 depthCacheDirty[DEPTH_CACHE_LINES];
static cc_uint8 depthBackingValid[DEPTH_TILE_COUNT];
#elif defined CC_BUILD_SOFTFP_W_DEPTH8
typedef cc_uint8 DepthValue;
#define SOFTFP_W_DEPTH8_LUT_SIZE 2048u
static DepthValue* depthBuffer;
static cc_bool depthBufferInternal;
static DepthValue* wDepth8EncodeLut;
#elif defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
typedef cc_uint32 DepthValue;
static DepthValue* depthBuffer;
#else
typedef int DepthValue;
static DepthValue* depthBuffer;
#endif
static cc_bool depthTest  = true;
static cc_bool depthWrite = true;
static int db_stride, db_height;
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
static int raster_width  = CLASSICUBE_3D_WIDTH;
static int raster_height = CLASSICUBE_3D_HEIGHT;
static int vp_raster_hwidth_fp, vp_raster_hheight_fp;
#define RASTER_GROUP_DOUBLE_X 0x8000u
#define RASTER_GROUP_DOUBLE_Y 0x80000000u
static cc_uint16 raster_to_color_xw[CLASSICUBE_3D_WIDTH];
static cc_uint32 raster_to_color_rowh[CLASSICUBE_3D_HEIGHT];
#define SOFTFP_GROUPED_RASTER
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_FB16
#define SOFTFP_LOWRES_COLOR_STAGE
static SoftPixel* rasterColorBuffer;
static SoftPixel* rasterColorInternal[2];
static SoftPixel* rasterColorFallback[2];
static SoftPixel* rasterColorRowsFallback[CLASSICUBE_3D_HEIGHT];
static SoftPixel** rasterColorRows = rasterColorRowsFallback;
#define SOFTFP_COLOR_ROW(y) rasterColorRows[(y)]
#endif
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
#define TERRAIN_TILE_CACHE_SLOTS 24
#define TERRAIN_TILE_SIZE 16
#define TERRAIN_TILE_PIXELS (TERRAIN_TILE_SIZE * TERRAIN_TILE_SIZE)
static void* terrainTileCacheMemory;
#endif
#if defined CC_BUILD_SOFTFP_FAST_CUTOUT && defined CC_BUILD_SOFTFP_PERSPECTIVE4 && \
    defined CC_BUILD_SOFTFP_W_DEPTH8 && defined CC_BUILD_SOFTFP_LOWRES_RASTER && \
    defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
#if (CLASSICUBE_3D_WIDTH & 7) || (CLASSICUBE_3D_HEIGHT & 7)
#error "Terrain Hi-Z requires the fixed 3D dimensions to be divisible by 8"
#endif
#define SOFTFP_FAST_TERRAIN_KERNEL
#define SOFTFP_COVERAGE_SHIFT 2
#define SOFTFP_HIZ_SHIFT 3
#define SOFTFP_COVERAGE_COLS (CLASSICUBE_3D_WIDTH  >> SOFTFP_COVERAGE_SHIFT)
#define SOFTFP_COVERAGE_ROWS (CLASSICUBE_3D_HEIGHT >> SOFTFP_COVERAGE_SHIFT)
#define SOFTFP_HIZ_COLS      (CLASSICUBE_3D_WIDTH  >> SOFTFP_HIZ_SHIFT)
#define SOFTFP_HIZ_ROWS      (CLASSICUBE_3D_HEIGHT >> SOFTFP_HIZ_SHIFT)
#define TERRAIN_TINT_CACHE_SLOTS 8
typedef struct TerrainTintTable {
    cc_uint32 key;
    SoftPixel r[32], g[32], b[32];
} TerrainTintTable;
static cc_uint16 terrainCoverageFallback[SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS];
static cc_uint16* terrainCoverage = terrainCoverageFallback;
static DepthValue terrainHiZ4Buffers[2][SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS];
static DepthValue terrainHiZBuffers[2][SOFTFP_HIZ_COLS * SOFTFP_HIZ_ROWS];
static DepthValue* terrainHiZ4 = terrainHiZ4Buffers[0];
static DepthValue* terrainHiZ = terrainHiZBuffers[0];
static const DepthValue* previousTerrainHiZ4 = terrainHiZ4Buffers[1];
static const DepthValue* previousTerrainHiZ = terrainHiZBuffers[1];
static cc_uint8 terrainHiZBufferIndex;
static cc_bool previousTerrainHiZValid;
static cc_uint32 previousTerrainMvpKey, previousTerrainMapGeneration;
static cc_uint32 terrainFrameMvpKey;
static TerrainTintTable* terrainTintCache;
static cc_uint8 terrainTintCacheNext;
static cc_uint32 terrainTintCacheHits, terrainTintCacheMisses;
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
#define SOFTFP_MAP_BATCH_HINT ((DrawHints)0x10)
#endif
#ifdef CC_BUILD_ESP32S3
/* These counters reset every frame and never approach 32-bit range. Keeping
 * them 32-bit avoids thousands of emulated 64-bit increments in culling and
 * raster hot paths; holo_perf widens them only once at frame end. */
static cc_uint32 rasterBboxPixels, rasterSpanPixels;
static cc_uint32 rasterPerspectiveTriangles, rasterPerspectivePixels;
static cc_uint32 rasterPerspectiveReciprocals;
static cc_uint32 rasterTriangleCount;
static cc_uint32 rasterTerrainQuads;
static cc_uint32 rasterOpaqueLutQuads;
static cc_uint32 rasterHiZCellSkipPixels;
static cc_uint32 rasterSpriteQuads, rasterSpriteCull;
static cc_uint32 rasterUiFastSprites, rasterUiGenericSprites, rasterUiSpritePixels;
static cc_uint32 rasterFrustumCull, rasterBackfaceCull, rasterTemporalCull;
static cc_uint32 rasterRangeCacheHits, rasterRangeCacheMisses;
static cc_uint32 rasterFrustumGroupCull;
static cc_uint32 rasterMapCullCycles, rasterMapXformCycles, rasterMapProjectCycles;
static cc_uint32 rasterMapDrawCycles;
static cc_uint32 rasterMapSetupCycles, rasterMapRasterCycles;
static cc_uint32 rasterMapHiZCycles, rasterMapClipCycles, rasterWorkerWaitCycles;
static cc_uint32 rasterWorkerFreeWaitCycles, rasterWorkerFlushWaitCycles;
static cc_uint32 rasterCurrentHiZCull;
static cc_uint32 rasterPreviousHiZCull;
static cc_uint32 rasterPreviousBoundsCull;
static cc_uint32 rasterClipZeroMask;
static cc_uint32 rasterMapModeCounts[4];
static cc_uint32 rasterHalfBlendCalls;
static cc_uint32 softfpRasterTransJobsFrame;
static cc_uint32 gfx_temporal_quad_key;
#if defined CC_BUILD_SOFTFP_TEXEL_2X2 || defined CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
static cc_uint32 textureSampleReuse, textureSampleLoads;
#endif
#endif

static void* gfx_vertices;
static GfxResourceID white_square;

#ifdef CC_BUILD_SOFTFP_FOG4
static CC_INLINE SoftPixel AveragePixel565(SoftPixel a, SoftPixel b) {
    return (SoftPixel)((a & b) + (((a ^ b) & 0xF7DEu) >> 1));
}

static CC_INLINE SoftPixel ApplyFog4(SoftPixel source, int inverseW) {
    SoftPixel halfFog, quarterFog, eighthFog;
    if (!gfx_fogEnabled || inverseW >= fogWStart) return source;

    /* Preserve the existing fog distances while halving the fog opacity
     * again: the four stages now blend 6.25/12.5/18.75/25% fog. */
    halfFog    = AveragePixel565(source, fogPixel);
    quarterFog = AveragePixel565(source, halfFog);
    if (inverseW < fogWEnd) return quarterFog;
    eighthFog  = AveragePixel565(source, quarterFog);
    if (inverseW < fogWHalf) return AveragePixel565(quarterFog, eighthFog);
    if (inverseW < fogWQuarter) return eighthFog;
    return AveragePixel565(source, eighthFog);
}
#endif

/* The ESP32 terrain kernel selects one of the same four weak fog strengths per
 * block face. This keeps the distant haze while removing distance comparisons
 * and repeated RGB565 averaging from every opaque pixel. */
static CC_INLINE int SoftFP_SelectTerrainFogLevel(int inverseW) {
#ifdef CC_BUILD_SOFTFP_FOG4
    if (!gfx_fogEnabled || inverseW >= fogWStart) return 0;
    if (inverseW < fogWEnd)     return 4;
    if (inverseW < fogWHalf)    return 3;
    if (inverseW < fogWQuarter) return 2;
    return 1;
#else
    (void)inverseW;
    return 0;
#endif
}

static CC_INLINE SoftPixel SoftFP_ApplyTerrainFogLevel(SoftPixel source, int level) {
#ifdef CC_BUILD_SOFTFP_FOG4
    SoftPixel halfFog, quarterFog, eighthFog;
    if (!level) return source;
    halfFog    = AveragePixel565(source, fogPixel);
    quarterFog = AveragePixel565(source, halfFog);
    if (level == 4) return quarterFog;
    eighthFog  = AveragePixel565(source, quarterFog);
    if (level == 3) return AveragePixel565(quarterFog, eighthFog);
    if (level == 2) return eighthFog;
    return AveragePixel565(source, eighthFog);
#else
    (void)level;
    return source;
#endif
}

#ifdef CC_BUILD_SOFTFP_Z16_TILES
static void FillDepth16(DepthValue* dst, DepthValue value, int count) {
    cc_uint32 pair = (cc_uint32)value | ((cc_uint32)value << 16);
    cc_uint32* out = (cc_uint32*)dst;
    int pairs = count >> 1;
    int i;
    for (i = 0; i < pairs; i++) out[i] = pair;
    if (count & 1) dst[count - 1] = value;
}
#endif

#ifdef CC_BUILD_SOFTFP_Z16_TILES
static void DepthCacheReset(void) {
    int i;
    if (!depthCachePixels) return;
    for (i = 0; i < DEPTH_CACHE_LINES; i++) {
        depthCacheTags[i]  = 0xFFFFu;
        depthCacheDirty[i] = 0;
    }
    Mem_Set(depthBackingValid, 0, sizeof(depthBackingValid));
}

static DepthValue* DepthCacheGetTile(int tile, int* lineIndex) {
    int line = tile & (DEPTH_CACHE_LINES - 1);
    DepthValue* pixels = depthCachePixels + line * DEPTH_TILE_PIXELS;
    cc_uint16 oldTag = depthCacheTags[line];

    if (oldTag == (cc_uint16)tile) {
        *lineIndex = line;
        return pixels;
    }
    if (oldTag != 0xFFFFu && depthCacheDirty[line]) {
        Mem_Copy(depthBuffer + oldTag * DEPTH_TILE_PIXELS,
                 pixels, DEPTH_TILE_PIXELS * sizeof(DepthValue));
        depthBackingValid[oldTag] = 1;
    }
    if (depthBackingValid[tile]) {
        Mem_Copy(pixels, depthBuffer + tile * DEPTH_TILE_PIXELS,
                 DEPTH_TILE_PIXELS * sizeof(DepthValue));
    } else {
        FillDepth16(pixels, DEPTH_CLEAR_VALUE, DEPTH_TILE_PIXELS);
    }
    depthCacheTags[line]  = (cc_uint16)tile;
    depthCacheDirty[line] = 0;
    *lineIndex = line;
    return pixels;
}

static DepthValue* DepthGetRow(int y, int x, int* nextTileX, int* lineIndex, int* xMask) {
    if (depthCachePixels) {
        int tileX = x >> DEPTH_TILE_SHIFT;
        int tileY = y >> DEPTH_TILE_SHIFT;
        int tile  = tileY * DEPTH_TILE_COLS + tileX;
        DepthValue* tilePixels = DepthCacheGetTile(tile, lineIndex);
        *nextTileX = (tileX + 1) << DEPTH_TILE_SHIFT;
        *xMask = DEPTH_TILE_SIZE - 1;
        return tilePixels + (y & (DEPTH_TILE_SIZE - 1)) * DEPTH_TILE_SIZE;
    }
    *nextTileX = fb_width;
    *lineIndex = -1;
    *xMask = -1;
    return depthBuffer + y * db_stride;
}

static CC_INLINE void DepthMarkDirty(int lineIndex) {
    if (lineIndex >= 0) depthCacheDirty[lineIndex] = 1;
}
#endif

typedef struct FixedMatrix {
    int m[4][4];
} FixedMatrix;

typedef struct VertexFixed {
    int x, y, z, w;
    int u, v;
    PackedCol c;
} VertexFixed;

#if defined CC_BUILD_ESP32S3 && defined SOFTFP_FAST_TERRAIN_KERNEL
/* The module task stack lives in PSRAM. Keep the frequently revisited polygon
 * edge arrays in the small internal renderer cache instead of spilling them to
 * that external stack for every terrain scanline. Rendering is single-threaded
 * on Core 1, so one shared scratch block is sufficient. */
typedef struct TerrainRasterScratch {
    VertexFixed* verts[4];
    int xEdge[4], yEdge[4];
    int edgeStart[4], edgeStepX[4], edgeStepY[4];
    int minX, minY, maxX, maxY, vertexCount;
    int rowW, rowU, rowV;
    int stepW, stepU, stepV;
    int rowStepW, rowStepU, rowStepV;
    int tintR, tintG, tintB;
    const TerrainTintTable* tintTable;
    cc_uint16* samplePixels;
    int sampleWidth, sampleWidthMask, sampleHeightMask;
    cc_uint8 shadedOpaque, fogLevel, depthLutSafe, workerMode;
    cc_uint8 blendDepthWrite;
    cc_uint8 translucentRowMode;
    signed char perspectiveShift;
} TerrainRasterScratch;
static TerrainRasterScratch terrainRasterScratchFallback;
static TerrainRasterScratch* terrainRasterScratch = &terrainRasterScratchFallback;
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
#define SOFTFP_RASTER_STACK_BYTES (6u * 1024u)
#define SOFTFP_RASTER_BATCH_JOBS 32u
#define SOFTFP_RASTER_BATCH_BUFFERS 4u
#define SOFTFP_RASTER_JOB_OPAQUE 1u
#define SOFTFP_RASTER_JOB_AFFINE 2u
#define SOFTFP_RASTER_JOB_TRANSLUCENT 3u
#define SOFTFP_RASTER_JOB_PERSPECTIVE 4u
static TerrainRasterScratch softfpRasterJobs[SOFTFP_RASTER_BATCH_BUFFERS]
                                                [SOFTFP_RASTER_BATCH_JOBS];
static cc_uint8 softfpRasterFillIndex;
static cc_uint8 softfpRasterFillCount[SOFTFP_RASTER_BATCH_BUFFERS];
static cc_uint8 softfpRasterFillClaimed;
static cc_uint32 softfpRasterWriteSeq;
static cc_uint32 softfpRasterCompletedSeq;
static volatile cc_uint8 softfpRasterStop;
static cc_uint32 softfpRasterCacheRefs[SOFTFP_RASTER_BATCH_BUFFERS];
static void* volatile softfpRasterTask;
static module_sync_handle_t softfpRasterReady;
static module_sync_handle_t softfpRasterIdle;
static module_sync_handle_t softfpRasterFree;
static module_sync_handle_t softfpRasterStopped;
static cc_uint32 softfpRasterJobsFrame;
static cc_uint32 softfpRasterBatchesFrame;
static cc_uint32 softfpRasterFallbacksFrame;
static cc_uint32 softfpRasterFlushesFrame;
static void RasterizeTerrainOpaqueLutDirect(TerrainRasterScratch* scratch);
static void RasterizeTerrainAffineDirect(TerrainRasterScratch* scratch);
static void RasterizeTerrainHalfBlendDirect(TerrainRasterScratch* scratch);
static void RasterizeTerrainPerspectiveDirect(TerrainRasterScratch* scratch);
static void SoftFP_RasterWorkerFlush(void);
static cc_bool SoftFP_RasterWorkerStart(void);
static void SoftFP_RasterWorkerStop(void);
#endif
#endif

static int tex_offseting;
static int texOffsetX_fp, texOffsetY_fp; // Fixed point texture offsets
#ifdef CC_BUILD_SOFTFP_FLOAT_XFORM
static struct Matrix _view_float, _proj_float, _mvp_float;
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
typedef struct SoftFPClipPlane {
    float x, y, z, w;
} SoftFPClipPlane;
static SoftFPClipPlane quadCullPlanes[NUM_PLANES];
static cc_uint8 mapClipPlanes = 0x3F;
#endif
#else
static FixedMatrix _view_fp, _proj_fp, _mvp_fp;
#endif

static void Gfx_RestoreState(void) {
    InitDefaultResources();

    struct Bitmap bmp;
    BitmapCol pixels[1] = { BITMAPCOLOR_WHITE };
    Bitmap_Init(bmp, 1, 1, pixels);
    white_square = Gfx_CreateTexture(&bmp, 0, false);
}

static void Gfx_FreeState(void) {
    FreeDefaultResources();
    Gfx_DeleteTexture(&white_square);
}

void Gfx_Create(void) {
    Gfx.MaxTexWidth  = 4096;
    Gfx.MaxTexHeight = 4096;
    Gfx.Created      = true;
    Gfx.BackendType  = CC_GFX_BACKEND_SOFTGPU;
    Gfx.Limitations  = GFX_LIMIT_MINIMAL;
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_W_DEPTH8
    /* Claim the fixed-resolution Z8 block and its 2 KiB encoder table before
     * component VBs/colour bands can fragment the internal heap. The direct
     * opaque kernel relies on this table, so losing it is a much larger cost
     * than the table's size would suggest. Keep raster resolution unchanged
     * if either reservation falls back. */
    raster_width  = CLASSICUBE_3D_WIDTH;
    raster_height = CLASSICUBE_3D_HEIGHT;
    if (!holo_port_buffer(raster_width * raster_height * sizeof(DepthValue), 2)) {
        Platform_LogConst("[cc.gfx] WARN: 192x144 Z8 internal reserve unavailable; using PSRAM at same resolution");
    }
    (void)holo_port_buffer(SOFTFP_W_DEPTH8_LUT_SIZE * sizeof(DepthValue), 6);
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    {
        const size_t tileBytes = TERRAIN_TILE_CACHE_SLOTS * TERRAIN_TILE_PIXELS * sizeof(cc_uint16);
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
        const size_t tintBytes = TERRAIN_TINT_CACHE_SLOTS * sizeof(TerrainTintTable);
#if defined CC_BUILD_ESP32S3 && defined SOFTFP_LOWRES_COLOR_STAGE
        const size_t coverageBytes = SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS * sizeof(cc_uint16);
        const size_t scratchBytes  = sizeof(TerrainRasterScratch);
        const size_t rowTableBytes = CLASSICUBE_3D_HEIGHT * sizeof(SoftPixel*);
#endif
        cc_uint8* cacheMemory = (cc_uint8*)holo_port_buffer(
            tileBytes + tintBytes
#if defined CC_BUILD_ESP32S3 && defined SOFTFP_LOWRES_COLOR_STAGE
            + coverageBytes + scratchBytes + rowTableBytes
#endif
            , 3);
        terrainTileCacheMemory = cacheMemory;
        terrainTintCache = cacheMemory ? (TerrainTintTable*)(cacheMemory + tileBytes) : NULL;
#if defined CC_BUILD_ESP32S3 && defined SOFTFP_LOWRES_COLOR_STAGE
        if (cacheMemory) {
            cc_uint8* hotData = cacheMemory + tileBytes + tintBytes;
            terrainCoverage    = (cc_uint16*)hotData;
            hotData            += coverageBytes;
            terrainRasterScratch = (TerrainRasterScratch*)hotData;
            hotData            += scratchBytes;
            rasterColorRows    = (SoftPixel**)hotData;
        } else {
            terrainCoverage     = terrainCoverageFallback;
            terrainRasterScratch = &terrainRasterScratchFallback;
            rasterColorRows     = rasterColorRowsFallback;
        }
#endif
        terrainTintCacheNext = 0;
        terrainTintCacheHits = terrainTintCacheMisses = 0;
        if (terrainTintCache) {
            Mem_Set(terrainTintCache, 0xFF,
                    TERRAIN_TINT_CACHE_SLOTS * sizeof(TerrainTintTable));
            Platform_LogConst("[cc.gfx] tint LUT=8x196 internal");
        }
#else
        terrainTileCacheMemory = holo_port_buffer(tileBytes, 3);
#endif
    }
#endif
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    rasterColorInternal[0] = (SoftPixel*)holo_port_buffer(
        CLASSICUBE_3D_WIDTH * CLASSICUBE_COLOR_BAND_ROWS * sizeof(SoftPixel), 4);
    rasterColorInternal[1] = (SoftPixel*)holo_port_buffer(
        CLASSICUBE_3D_WIDTH * CLASSICUBE_COLOR_BAND_ROWS * sizeof(SoftPixel), 5);
#endif
}

#ifdef CC_BUILD_ESP32S3
static void SoftFP_HotbarCacheFree(void);
#endif

static void DestroyBuffers(void) {
    Window_FreeFramebuffer(&fb_bmp);
#ifdef CC_BUILD_ESP32S3
    SoftFP_HotbarCacheFree();
#endif
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    Mem_Free(rasterColorBuffer);
    Mem_Free(rasterColorFallback[0]);
    Mem_Free(rasterColorFallback[1]);
    rasterColorBuffer = NULL;
    rasterColorFallback[0] = rasterColorFallback[1] = NULL;
#endif
#ifdef CC_BUILD_SOFTFP_W_DEPTH8
    if (depthBuffer && !depthBufferInternal) Mem_Free(depthBuffer);
    depthBufferInternal = false;
#else
    Mem_Free(depthBuffer);
#endif
    depthBuffer = NULL;
#ifdef CC_BUILD_SOFTFP_Z16_TILES
    depthCachePixels = NULL;
#endif
}

void Gfx_Free(void) { 
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    SoftFP_RasterWorkerStop();
#endif
    Gfx_FreeState();
    DestroyBuffers();
}

#ifdef CC_BUILD_SOFTFP_TEX16
typedef cc_uint16 SoftTexel;
#define SOFT_TEXEL_A_MASK 0x8000u
#define SoftTexel_R(c) ((((c) >> 10) & 0x1Fu) << 3 | (((c) >> 10) & 0x1Fu) >> 2)
#define SoftTexel_G(c) ((((c) >>  5) & 0x1Fu) << 3 | (((c) >>  5) & 0x1Fu) >> 2)
#define SoftTexel_B(c) ((((c)      ) & 0x1Fu) << 3 | (((c)      ) & 0x1Fu) >> 2)
#define SoftTexel_A(c) ((c) & SOFT_TEXEL_A_MASK ? 255 : 0)

static CC_INLINE SoftTexel SoftTexel_FromBitmap(BitmapCol color) {
    return (SoftTexel)((BitmapCol_A(color) ? SOFT_TEXEL_A_MASK : 0) |
                       ((BitmapCol_R(color) >> 3) << 10) |
                       ((BitmapCol_G(color) >> 3) <<  5) |
                        (BitmapCol_B(color) >> 3));
}

static CC_INLINE BitmapCol SoftTexel_ToBitmap(SoftTexel color) {
    return BitmapCol_Make(SoftTexel_R(color), SoftTexel_G(color),
                          SoftTexel_B(color), SoftTexel_A(color));
}
#else
typedef BitmapCol SoftTexel;
#define SoftTexel_R(c) BitmapCol_R(c)
#define SoftTexel_G(c) BitmapCol_G(c)
#define SoftTexel_B(c) BitmapCol_B(c)
#define SoftTexel_A(c) BitmapCol_A(c)
static CC_INLINE SoftTexel SoftTexel_FromBitmap(BitmapCol color) { return color; }
static CC_INLINE BitmapCol SoftTexel_ToBitmap(SoftTexel color) { return color; }
#endif

#if defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
static CC_INLINE SoftPixel SoftPixel_FromTexel(SoftTexel texture) {
    /* Exact A1RGB555 -> RGB565 equivalent of ToBitmap + FromBitmap. Green's
     * high bit is duplicated into the sixth RGB565 green bit. */
    return (SoftPixel)(((texture & 0x7C00u) << 1) |
                       ((texture & 0x03E0u) << 1) |
                       ((texture & 0x0200u) >> 4) |
                        (texture & 0x001Fu));
}

static CC_INLINE SoftPixel SoftPixel_FromTexelTint(SoftTexel texture,
                                                   int tintR, int tintG, int tintB) {
    int r5 = (texture >> 10) & 0x1F;
    int g5 = (texture >>  5) & 0x1F;
    int b5 =  texture        & 0x1F;
    int r  = (tintR * ((r5 << 3) | (r5 >> 2))) >> 11;
    int g  = (tintG * ((g5 << 3) | (g5 >> 2))) >> 10;
    int b  = (tintB * ((b5 << 3) | (b5 >> 2))) >> 11;
    return (SoftPixel)((r << 11) | (g << 5) | b);
}
#endif

typedef struct CCTexture {
    unsigned short width, height;
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    SoftTexel* hotPixels;
#endif
    SoftTexel pixels[];
} CCTexture;

static CCTexture* curTexture;
static SoftTexel* curTexPixels;
static int curTexWidth, curTexHeight;
#ifdef CC_BUILD_ESP32S3
#define SOFTFP_HOTBAR_CACHE_ENTRIES 2048u
#define SOFTFP_HOTBAR_CACHE_TILE_WORDS 4u
typedef struct SoftFPHotbarCacheEntry {
    cc_uint32 index;
    SoftPixel color;
} SoftFPHotbarCacheEntry;
static SoftFPHotbarCacheEntry* hotbarCacheEntries;
static cc_uint32 hotbarCacheCount;
static cc_uint32 hotbarCacheTiles[SOFTFP_HOTBAR_CACHE_TILE_WORDS];
static CCTexture* hotbarCacheTexture;
static cc_bool hotbarCacheValid, hotbarCacheCapturing;
static cc_bool hotbarCacheOverflow, hotbarCacheMixedTextures, hotbarCacheLogged;

static void SoftFP_HotbarCacheFree(void) {
    Mem_Free(hotbarCacheEntries);
    hotbarCacheEntries = NULL;
    hotbarCacheCount = 0;
    hotbarCacheValid = hotbarCacheCapturing = false;
}
#endif
static int texWidthMask, texHeightMask;
static int texSinglePixel;
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
static CCTexture* terrainTileCacheOwners[TERRAIN_TILE_CACHE_SLOTS];
static cc_uint16 terrainTileCacheIndices[TERRAIN_TILE_CACHE_SLOTS];
static cc_uint8 terrainTileCacheNext;
static CC_INLINE int TerrainTileCache_NextSlot(void) {
    int slot = terrainTileCacheNext;
    terrainTileCacheNext++;
    if (terrainTileCacheNext >= TERRAIN_TILE_CACHE_SLOTS) terrainTileCacheNext = 0;
    return slot;
}
#if defined CC_BUILD_ESP32S3 && defined SOFTFP_FAST_TERRAIN_KERNEL
/* Opaque terrain tiles are shaded once into the existing internal cache.
 * The hot raster loop can then fetch one final RGB565 value instead of an
 * A1RGB555 texel followed by three tint-table lookups for every pixel. Cutout
 * tiles remain cached in their original format so their alpha mask is exact. */
static cc_uint32 terrainTileCacheTintKeys[TERRAIN_TILE_CACHE_SLOTS];
static cc_uint8 terrainTileCacheOpaque[TERRAIN_TILE_CACHE_SLOTS];
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
static int TerrainTileCache_SlotForPixels(const cc_uint16* pixels) {
    cc_uintptr base = (cc_uintptr)terrainTileCacheMemory;
    cc_uintptr address = (cc_uintptr)pixels;
    const cc_uintptr slotBytes = TERRAIN_TILE_PIXELS * sizeof(cc_uint16);
    cc_uintptr offset;
    if (!base || address < base || address >= base + TERRAIN_TILE_CACHE_SLOTS * slotBytes) return -1;
    offset = address - base;
    if (offset % slotBytes) return -1;
    return (int)(offset / slotBytes);
}

static int TerrainTileCache_SelectSlot(void) {
    cc_uint32 refs = 0;
    int i, slot;
    /* Jobs keep direct pointers into cache slots. Prefer any slot no queued or
     * running Core 0 job still references, preserving pipeline overlap across
     * cache misses. Only a completely busy cache requires a full barrier. */
    for (i = 0; i < SOFTFP_RASTER_BATCH_BUFFERS; i++) {
        refs |= __atomic_load_n(&softfpRasterCacheRefs[i], __ATOMIC_ACQUIRE);
    }
    for (i = 0; i < TERRAIN_TILE_CACHE_SLOTS; i++) {
        slot = TerrainTileCache_NextSlot();
        if (!(refs & (1u << slot))) return slot;
    }
    SoftFP_RasterWorkerFlush();
    refs = 0;
    for (i = 0; i < SOFTFP_RASTER_BATCH_BUFFERS; i++) {
        refs |= __atomic_load_n(&softfpRasterCacheRefs[i], __ATOMIC_ACQUIRE);
    }
    for (i = 0; i < TERRAIN_TILE_CACHE_SLOTS; i++) {
        slot = TerrainTileCache_NextSlot();
        if (!(refs & (1u << slot))) return slot;
    }
    return -1;
}
#endif
#endif

static void TerrainTileCache_Invalidate(CCTexture* texture, int firstTile, int lastTile) {
    int i;
    for (i = 0; i < TERRAIN_TILE_CACHE_SLOTS; i++) {
        if ((!texture || terrainTileCacheOwners[i] == texture) &&
            terrainTileCacheIndices[i] >= firstTile &&
            terrainTileCacheIndices[i] <= lastTile) {
            terrainTileCacheOwners[i] = NULL;
        }
    }
}

static SoftTexel* TerrainTileCache_Get(int tileIndex) {
    SoftTexel* pixels = (SoftTexel*)terrainTileCacheMemory;
    int i, slot;
    if (!pixels) return NULL;

    for (i = 0; i < TERRAIN_TILE_CACHE_SLOTS; i++) {
        if (terrainTileCacheOwners[i] == curTexture &&
            terrainTileCacheIndices[i] == tileIndex) {
            textureSampleReuse++;
            return pixels + i * TERRAIN_TILE_PIXELS;
        }
    }

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    slot = TerrainTileCache_SelectSlot();
    if (slot < 0) return NULL;
#else
    slot = TerrainTileCache_NextSlot();
#endif
    Mem_Copy(pixels + slot * TERRAIN_TILE_PIXELS,
             curTexPixels + tileIndex * TERRAIN_TILE_PIXELS,
             TERRAIN_TILE_PIXELS * sizeof(SoftTexel));
    terrainTileCacheOwners[slot]  = curTexture;
    terrainTileCacheIndices[slot] = (cc_uint16)tileIndex;
    textureSampleLoads++;
    return pixels + slot * TERRAIN_TILE_PIXELS;
}

#if defined CC_BUILD_ESP32S3 && defined SOFTFP_FAST_TERRAIN_KERNEL
static CC_INLINE SoftPixel SoftPixel_FromTintTable(SoftTexel texture,
                                                    const TerrainTintTable* table);
static CC_NOINLINE SoftPixel* TerrainTileCache_GetShaded(int tileIndex,
                                                         const TerrainTintTable* tintTable,
                                                         cc_uint32 tintKey,
                                                         cc_bool* shadedOpaque) {
    SoftTexel* pixels = (SoftTexel*)terrainTileCacheMemory;
    int i, slot;
    cc_bool opaque = true;
    if (shadedOpaque) *shadedOpaque = false;
    if (!pixels) return NULL;

    for (i = 0; i < TERRAIN_TILE_CACHE_SLOTS; i++) {
        if (terrainTileCacheOwners[i] != curTexture ||
            terrainTileCacheIndices[i] != tileIndex) continue;
        if (terrainTileCacheOpaque[i] &&
            terrainTileCacheTintKeys[i] != tintKey) continue;
        textureSampleReuse++;
        if (shadedOpaque) *shadedOpaque = terrainTileCacheOpaque[i] != 0;
        return (SoftPixel*)(pixels + i * TERRAIN_TILE_PIXELS);
    }

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    slot = TerrainTileCache_SelectSlot();
    if (slot < 0) return NULL;
#else
    slot = TerrainTileCache_NextSlot();
#endif
    Mem_Copy(pixels + slot * TERRAIN_TILE_PIXELS,
             curTexPixels + tileIndex * TERRAIN_TILE_PIXELS,
             TERRAIN_TILE_PIXELS * sizeof(SoftTexel));
    for (i = 0; i < TERRAIN_TILE_PIXELS; i++) {
        if (!(pixels[slot * TERRAIN_TILE_PIXELS + i] & SOFT_TEXEL_A_MASK)) {
            opaque = false;
            break;
        }
    }
    if (opaque && tintTable) {
        SoftPixel* shaded = (SoftPixel*)(pixels + slot * TERRAIN_TILE_PIXELS);
        for (i = 0; i < TERRAIN_TILE_PIXELS; i++) {
            shaded[i] = SoftPixel_FromTintTable(
                pixels[slot * TERRAIN_TILE_PIXELS + i], tintTable);
        }
    } else {
        opaque = false;
    }
    terrainTileCacheOwners[slot]   = curTexture;
    terrainTileCacheIndices[slot]  = (cc_uint16)tileIndex;
    terrainTileCacheTintKeys[slot] = tintKey;
    terrainTileCacheOpaque[slot]   = opaque;
    textureSampleLoads++;
    if (shadedOpaque) *shadedOpaque = opaque;
    return (SoftPixel*)(pixels + slot * TERRAIN_TILE_PIXELS);
}

#endif

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
static CC_NOINLINE const TerrainTintTable* TerrainTintCache_Get(int tintR, int tintG, int tintB) {
    cc_uint32 key = ((cc_uint32)tintR << 16) | ((cc_uint32)tintG << 8) | (cc_uint32)tintB;
    TerrainTintTable* table;
    int i, slot;

    if (!terrainTintCache) return NULL;
    for (i = 0; i < TERRAIN_TINT_CACHE_SLOTS; i++) {
        if (terrainTintCache[i].key == key) {
            terrainTintCacheHits++;
            return &terrainTintCache[i];
        }
    }

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    /* Queued jobs retain this table pointer. A rare lighting-key miss may
     * replace a slot only after all earlier raster commands have consumed it. */
    SoftFP_RasterWorkerFlush();
#endif
    slot = terrainTintCacheNext++ & (TERRAIN_TINT_CACHE_SLOTS - 1);
    table = &terrainTintCache[slot];
    table->key = key;
    for (i = 0; i < 32; i++) {
        int expanded = (i << 3) | (i >> 2);
        table->r[i] = (SoftPixel)(((tintR * expanded) >> 11) << 11);
        table->g[i] = (SoftPixel)(((tintG * expanded) >> 10) << 5);
        table->b[i] = (SoftPixel)( (tintB * expanded) >> 11);
    }
    terrainTintCacheMisses++;
    return table;
}

static CC_INLINE SoftPixel SoftPixel_FromTintTable(SoftTexel texture,
                                                    const TerrainTintTable* table) {
    return (SoftPixel)(table->r[(texture >> 10) & 0x1F] |
                       table->g[(texture >>  5) & 0x1F] |
                       table->b[ texture        & 0x1F]);
}
#endif
#endif
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
static CCTexture* hotAtlasOwner;
#define HOT_ATLAS_PIXELS (32 * 1024)
#endif
        
void Gfx_BindTexture(GfxResourceID texId) {
    if (!texId) texId = white_square;
    CCTexture* tex = texId;

    curTexture   = tex;
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    curTexPixels = tex->hotPixels ? tex->hotPixels : tex->pixels;
#else
    curTexPixels = tex->pixels;
#endif
    curTexWidth  = tex->width;
    curTexHeight = tex->height;

    texWidthMask   = (1 << Math_ilog2(tex->width))  - 1;
    texHeightMask  = (1 << Math_ilog2(tex->height)) - 1;

    texSinglePixel = curTexWidth == 1;
}

#ifdef CC_BUILD_ESP32S3
void Gfx_HotbarCacheInvalidate(void) { hotbarCacheValid = false; }

cc_bool Gfx_HotbarCacheReplay(void) {
    cc_uint32 i;
    if (!hotbarCacheValid || hotbarCacheCapturing || !hotbarCacheEntries) return false;
    for (i = 0; i < hotbarCacheCount; i++) {
        const SoftFPHotbarCacheEntry* entry = &hotbarCacheEntries[i];
        colorBuffer[entry->index] = entry->color;
    }
    return true;
}

cc_bool Gfx_HotbarCacheBegin(void) {
    if (hotbarCacheValid || hotbarCacheCapturing) return false;
    if (!hotbarCacheEntries) {
        hotbarCacheEntries = (SoftFPHotbarCacheEntry*)Mem_TryAlloc(
            SOFTFP_HOTBAR_CACHE_ENTRIES, sizeof(SoftFPHotbarCacheEntry));
        if (!hotbarCacheEntries) return false;
    }
    hotbarCacheCount = 0;
    hotbarCacheTexture = NULL;
    hotbarCacheOverflow = false;
    hotbarCacheMixedTextures = false;
    Mem_Set(hotbarCacheTiles, 0, sizeof(hotbarCacheTiles));
    hotbarCacheCapturing = true;
    return true;
}

void Gfx_HotbarCacheEnd(void) {
    int value;
    if (!hotbarCacheCapturing) return;
    hotbarCacheCapturing = false;
    hotbarCacheValid = hotbarCacheCount != 0 && !hotbarCacheOverflow;
    if (!hotbarCacheLogged) {
        hotbarCacheLogged = true;
        value = (int)hotbarCacheCount;
        Platform_Log1("[cc.gfx] hotbar cache opaque writes=%i", &value);
        if (hotbarCacheOverflow) {
            Platform_LogConst("[cc.gfx] hotbar cache overflow; using normal raster");
        }
    }
}

static CC_INLINE void SoftFP_HotbarCacheCapture(cc_uint32 index,
                                                 SoftPixel color, int texY) {
    cc_uint32 tile;
    SoftFPHotbarCacheEntry* entry;
    if (!hotbarCacheCapturing) return;
    if (hotbarCacheCount >= SOFTFP_HOTBAR_CACHE_ENTRIES) {
        hotbarCacheOverflow = true;
        return;
    }
    entry = &hotbarCacheEntries[hotbarCacheCount++];
    entry->index = index;
    entry->color = color;

    if (!hotbarCacheTexture) hotbarCacheTexture = curTexture;
    else if (hotbarCacheTexture != curTexture) hotbarCacheMixedTextures = true;
    if (hotbarCacheMixedTextures || curTexture != hotbarCacheTexture ||
        curTexWidth != TERRAIN_TILE_SIZE || texY < 0) return;
    tile = (cc_uint32)texY / TERRAIN_TILE_SIZE;
    if (tile < SOFTFP_HOTBAR_CACHE_TILE_WORDS * 32u) {
        hotbarCacheTiles[tile >> 5] |= 1u << (tile & 31u);
    }
}

static void SoftFP_HotbarCacheTextureUpdated(CCTexture* texture,
                                              int firstTile, int lastTile) {
    int tile;
    if (!hotbarCacheValid) return;
    if (hotbarCacheMixedTextures) { hotbarCacheValid = false; return; }
    if (texture != hotbarCacheTexture) return;
    if (texture->width != TERRAIN_TILE_SIZE) {
        hotbarCacheValid = false; return;
    }
    firstTile = max(firstTile, 0);
    lastTile  = min(lastTile, (int)(SOFTFP_HOTBAR_CACHE_TILE_WORDS * 32u - 1u));
    for (tile = firstTile; tile <= lastTile; tile++) {
        if (hotbarCacheTiles[(cc_uint32)tile >> 5] &
            (1u << ((cc_uint32)tile & 31u))) {
            hotbarCacheValid = false;
            return;
        }
    }
}
#endif
        
void Gfx_DeleteTexture(GfxResourceID* texId) {
    GfxResourceID data = *texId;
#ifdef CC_BUILD_ESP32S3
    if (hotbarCacheValid && (hotbarCacheMixedTextures ||
        (CCTexture*)data == hotbarCacheTexture)) hotbarCacheValid = false;
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    if (data) TerrainTileCache_Invalidate((CCTexture*)data, 0, INT_MAX);
#endif
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    if ((CCTexture*)data == hotAtlasOwner) hotAtlasOwner = NULL;
#endif
    if (data) Mem_Free(data);
    *texId = NULL;
}
        
GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, int rowWidth, cc_uint8 flags, cc_bool mipmaps) {
	int x, y;
    CCTexture* tex = (CCTexture*)Mem_TryAlloc(1,
        sizeof(CCTexture) + bmp->width * bmp->height * sizeof(SoftTexel));
	if (!tex) return NULL;

    tex->width  = bmp->width;
    tex->height = bmp->height;
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    tex->hotPixels = NULL;
#endif

    for (y = 0; y < bmp->height; y++) {
        BitmapCol* src = bmp->scan0 + y * rowWidth;
        SoftTexel* dst = tex->pixels + y * bmp->width;
        for (x = 0; x < bmp->width; x++) dst[x] = SoftTexel_FromBitmap(src[x]);
    }
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    if (!hotAtlasOwner && bmp->width == 16 && bmp->height >= 1024 &&
        bmp->width * bmp->height <= HOT_ATLAS_PIXELS) {
        tex->hotPixels = (SoftTexel*)holo_port_buffer(
            (size_t)bmp->width * bmp->height * sizeof(SoftTexel), 2);
        if (tex->hotPixels) {
            Mem_Copy(tex->hotPixels, tex->pixels,
                     (size_t)bmp->width * bmp->height * sizeof(SoftTexel));
            hotAtlasOwner = tex;
            Platform_LogConst("[cc.gfx] terrain atlas cached in 64 KiB internal RAM");
        } else {
            Platform_LogConst("[cc.gfx] terrain atlas internal cache unavailable; using PSRAM");
        }
    }
#endif
    return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
    CCTexture* tex = (CCTexture*)texId;
	int xx, yy;
#ifdef CC_BUILD_ESP32S3
    SoftFP_HotbarCacheTextureUpdated(tex, y / TERRAIN_TILE_SIZE,
        (y + part->height - 1) / TERRAIN_TILE_SIZE);
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    if (tex->width == TERRAIN_TILE_SIZE) {
        int firstTile = y / TERRAIN_TILE_SIZE;
        int lastTile  = (y + part->height - 1) / TERRAIN_TILE_SIZE;
        TerrainTileCache_Invalidate(tex, firstTile, lastTile);
    } else {
        TerrainTileCache_Invalidate(tex, 0, INT_MAX);
    }
#endif
    for (yy = 0; yy < part->height; yy++) {
        BitmapCol* src = part->scan0 + yy * rowWidth;
        SoftTexel* dst = tex->pixels + x + (y + yy) * tex->width;
        for (xx = 0; xx < part->width; xx++) dst[xx] = SoftTexel_FromBitmap(src[xx]);
#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
        if (tex->hotPixels) {
            Mem_Copy(tex->hotPixels + x + (y + yy) * tex->width,
                     dst, part->width * sizeof(SoftTexel));
        }
#endif
    }
}

void Gfx_EnableMipmaps(void)  { }
void Gfx_DisableMipmaps(void) { }


/*########################################################################################################################*
*--------------------------------------------------------2D drawing-------------------------------------------------------*
*#########################################################################################################################*/
#ifdef SOFTFP_LOWRES_COLOR_STAGE
typedef struct SoftFPUpscaleJob {
	int firstRow, lastRow;
} SoftFPUpscaleJob;

static void ScaleRasterRows(void* arg) {
	const SoftFPUpscaleJob* job = (const SoftFPUpscaleJob*)arg;
	int y;
	for (y = job->firstRow; y < job->lastRow; y++) {
		const SoftPixel* src = SOFTFP_COLOR_ROW(y);
		cc_uint32 rowh = raster_to_color_rowh[y];
		SoftPixel* dst = colorBuffer + (rowh & ~RASTER_GROUP_DOUBLE_Y);

		if (raster_width == 192 && fb_width == 320) {
			const cc_uint32* in = (const cc_uint32*)src;
			cc_uint32* out = (cc_uint32*)dst;
			cc_uint32* out2 = (rowh & RASTER_GROUP_DOUBLE_Y) ?
				(cc_uint32*)(dst + cb_stride) : NULL;
			int x;
			for (x = 0; x < 192; x += 6, in += 3, out += 5) {
				cc_uint32 p0 = in[0], p2 = in[1], c45 = in[2];
				cc_uint32 c1 = p0 >> 16, c2 = p2 & 0xFFFFu;
				cc_uint32 c4 = c45 & 0xFFFFu, c5 = c45 >> 16;
				cc_uint32 p1 = c1 | (c2 << 16), p3 = c4 | (c4 << 16);
				cc_uint32 p4 = c5 | (c5 << 16);
				out[0] = p0; out[1] = p1; out[2] = p2; out[3] = p3; out[4] = p4;
				if (out2) {
					out2[0] = p0; out2[1] = p1; out2[2] = p2; out2[3] = p3; out2[4] = p4;
					out2 += 5;
				}
			}
		} else {
			int x;
			for (x = 0; x < raster_width; x++) {
				cc_uint16 xw = raster_to_color_xw[x];
				int out = xw & ~RASTER_GROUP_DOUBLE_X;
				dst[out] = src[x];
				if (xw & RASTER_GROUP_DOUBLE_X) dst[out + 1] = src[x];
			}
		}
		if ((rowh & RASTER_GROUP_DOUBLE_Y) && !(raster_width == 192 && fb_width == 320)) {
			Mem_Copy(dst + cb_stride, dst, fb_width * sizeof(SoftPixel));
		}
	}
}

#endif

void Gfx_Begin2D(int width, int height) {
#ifdef SOFTFP_LOWRES_COLOR_STAGE
	cc_uint64 upscaleMark = Stopwatch_Measure();
	SoftFPUpscaleJob job;
	job.firstRow = 0;
	job.lastRow  = raster_height;
	ScaleRasterRows(&job);
	holo_perf_add(HOLO_PERF_UPSCALE,
		Stopwatch_ElapsedMicroseconds(upscaleMark, Stopwatch_Measure()));
#endif
	gfx_rendering2D = true;

	Gfx_SetDepthTest(false);
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaBlending(true);
}

void Gfx_End2D(void) {
	gfx_rendering2D = false;

	Gfx_SetDepthTest(true);
	Gfx_SetDepthWrite(true);
	Gfx_SetAlphaBlending(false);
}


/*########################################################################################################################*
*------------------------------------------------------State management---------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFog(cc_bool enabled)    { gfx_fogEnabled = enabled; }
void Gfx_SetFogCol(PackedCol col)   {
#ifdef CC_BUILD_SOFTFP_FOG4
    fogPixel = SoftPixel_FromBitmap(col);
#endif
}
void Gfx_SetFogDensity(float value) { }
void Gfx_SetFogEnd(float value)     {
#ifdef CC_BUILD_SOFTFP_FOG4
    if (value < 1.0f) value = 1.0f;
    fogWStart   = FloatToFixed(1.0f / (value * 0.55f));
    fogWQuarter = FloatToFixed(1.0f / (value * 0.70f));
    fogWHalf    = FloatToFixed(1.0f / (value * 0.85f));
    fogWEnd     = FloatToFixed(1.0f / value);
#endif
}
void Gfx_SetFogMode(FogFunc func)   { }

void Gfx_SetFaceCulling(cc_bool enabled) {
    faceCulling = enabled;
}

static void SetAlphaTest(cc_bool enabled) {
}

static void SetAlphaBlend(cc_bool enabled) {
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

static void ClearColorBuffer(void) {
    SoftPixel* target = colorBuffer;
    int width = fb_width, height = fb_height, stride = cb_stride;
    int i, x, y, size;
    SoftPixel fill = SoftPixel_FromBitmap(clearColor);

#ifdef SOFTFP_LOWRES_COLOR_STAGE
    /* The low-resolution stage is contiguous in either internal RAM or its
     * PSRAM fallback. */
    for (y = 0; y < raster_height; y++) {
        target = SOFTFP_COLOR_ROW(y);
        for (x = 0; x + 3 < raster_width; x += 4) {
            target[x + 0] = fill; target[x + 1] = fill;
            target[x + 2] = fill; target[x + 3] = fill;
        }
        for (; x < raster_width; x++) target[x] = fill;
    }
    return;
#endif
    size = width * height;
    if (stride == width) {
        for (i = 0; i + 3 < size; i += 4) {
            target[i + 0] = fill;
            target[i + 1] = fill;
            target[i + 2] = fill;
            target[i + 3] = fill;
        }
        for (; i < size; i++) target[i] = fill;
    } else {
        for (y = 0; y < height; y++) {
            i = y * stride;
            for (x = 0; x < width; x++) {
                target[i + x] = fill;
            }
        }
    }
}

#if !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_W_DEPTH8 && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
static void FillDepth32(DepthValue* dst, int count) {
    int i = 0;
    const DepthValue maxDepth = 0x7FFFFFFF;
    for (; i + 8 <= count; i += 8) {
        dst[i + 0] = maxDepth; dst[i + 1] = maxDepth;
        dst[i + 2] = maxDepth; dst[i + 3] = maxDepth;
        dst[i + 4] = maxDepth; dst[i + 5] = maxDepth;
        dst[i + 6] = maxDepth; dst[i + 7] = maxDepth;
    }
    for (; i < count; i++) dst[i] = maxDepth;
}
#endif

#ifdef CC_BUILD_ESP32S3
static void ResetRasterFrameCounters(void) {
    rasterBboxPixels = rasterSpanPixels = 0;
    rasterPerspectiveTriangles = rasterPerspectivePixels = 0;
    rasterPerspectiveReciprocals = 0;
    rasterTriangleCount = 0;
    rasterTerrainQuads = 0;
    rasterOpaqueLutQuads = 0;
    rasterHiZCellSkipPixels = 0;
    rasterSpriteQuads = rasterSpriteCull = 0;
    rasterUiFastSprites = rasterUiGenericSprites = rasterUiSpritePixels = 0;
    rasterFrustumCull = rasterBackfaceCull = rasterTemporalCull = 0;
    rasterRangeCacheHits = rasterRangeCacheMisses = 0;
    rasterFrustumGroupCull = 0;
    rasterMapCullCycles = rasterMapXformCycles = rasterMapProjectCycles = 0;
    rasterMapDrawCycles = 0;
    rasterMapSetupCycles = rasterMapRasterCycles = 0;
    rasterMapHiZCycles = rasterMapClipCycles = rasterWorkerWaitCycles = 0;
    rasterWorkerFreeWaitCycles = rasterWorkerFlushWaitCycles = 0;
    rasterCurrentHiZCull = 0;
    rasterPreviousHiZCull = 0;
    rasterPreviousBoundsCull = 0;
    rasterClipZeroMask = 0;
    Mem_Set(rasterMapModeCounts, 0, sizeof(rasterMapModeCounts));
    rasterHalfBlendCalls = 0;
    softfpRasterTransJobsFrame = 0;
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    softfpRasterJobsFrame = 0;
    softfpRasterBatchesFrame = 0;
    softfpRasterFallbacksFrame = 0;
    softfpRasterFlushesFrame = 0;
#endif
    gfx_temporal_quad_key = 0;
#if defined CC_BUILD_SOFTFP_TEXEL_2X2 || defined CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    textureSampleReuse = textureSampleLoads = 0;
#endif
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    terrainTintCacheHits = terrainTintCacheMisses = 0;
#endif
}
#endif

static void ClearDepthBuffer(void) {
#ifdef CC_BUILD_ESP32S3
    ResetRasterFrameCounters();
#endif
#ifdef CC_BUILD_SOFTFP_Z16_TILES
    if (depthCachePixels) {
        DepthCacheReset();
    } else {
        FillDepth16(depthBuffer, DEPTH_CLEAR_VALUE, db_stride * db_height);
    }
#elif defined CC_BUILD_SOFTFP_W_DEPTH8
    Mem_Set(depthBuffer, DEPTH_CLEAR_VALUE, db_stride * db_height);
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    Mem_Set(terrainCoverage, 0,
            SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS * sizeof(*terrainCoverage));
    Mem_Set(terrainHiZ4, DEPTH_CLEAR_VALUE,
            SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS * sizeof(*terrainHiZ4));
    Mem_Set(terrainHiZ, DEPTH_CLEAR_VALUE,
            SOFTFP_HIZ_COLS * SOFTFP_HIZ_ROWS * sizeof(*terrainHiZ));
#endif
#else
#ifdef CC_BUILD_SOFTFP_Z32_FASTCLEAR
    Mem_Set(depthBuffer, 0xFF, db_stride * db_height * sizeof(DepthValue));
#else
    FillDepth32(depthBuffer, db_stride * db_height);
#endif
#endif
}

void Gfx_ClearBuffers(GfxBuffers buffers) {
#ifdef CC_BUILD_ESP32S3
    cc_uint64 perfMark = Stopwatch_Measure();
#endif
    if (buffers & GFX_BUFFER_COLOR) ClearColorBuffer();
    if (buffers & GFX_BUFFER_DEPTH) ClearDepthBuffer();
#ifdef CC_BUILD_ESP32S3
    holo_perf_add(HOLO_PERF_CLEAR, Stopwatch_ElapsedMicroseconds(perfMark, Stopwatch_Measure()));
#endif
}

void Gfx_ClearColor(PackedCol color) {
    int R = PackedCol_R(color);
    int G = PackedCol_G(color);
    int B = PackedCol_B(color);
    int A = PackedCol_A(color);

    clearColor = BitmapCol_Make(R, G, B, A);
}

void Gfx_SetDepthTest(cc_bool enabled) {
    depthTest = enabled;
}

void Gfx_SetDepthWrite(cc_bool enabled) {
    depthWrite = enabled;
}

static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
    // TODO
}

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {
    colWrite = !depthOnly;
}

/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
    return (void*)1;
}

void Gfx_BindIb(GfxResourceID ib) { }
void Gfx_DeleteIb(GfxResourceID* ib) { }

/*########################################################################################################################*
*-------------------------------------------------------Vertex buffers----------------------------------------------------*
*#########################################################################################################################*/
// Preprocess vertex buffers into fixed point values
struct FPVertexColoured { 
	int x, y, z;
	BitmapCol c;
};

struct FPVertexTextured { 
	int x, y, z;
	BitmapCol c;
	int u, v;
};

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
/* Map vertices are immutable after chunk construction. Store positions in
 * chunk-local Q8, repeating U in Q12, normalised V in Q16, and tint in RGB565.
 * This is exactly 12 bytes instead of the generic fixed-point vertex's 24. */
typedef struct PackedTerrainVertex {
    cc_int16 x, y, z;
    cc_uint16 u, v;
    cc_uint16 color;
} PackedTerrainVertex;
typedef char PackedTerrainVertex_SizeCheck[sizeof(PackedTerrainVertex) == 12 ? 1 : -1];

typedef struct SoftFPMapQuadBounds {
    cc_uint8 minX, minY, minZ;
    cc_uint8 maxX, maxY, maxZ;
} SoftFPMapQuadBounds;
#define SOFTFP_RANGE_CACHE_SIZE 8u
typedef struct SoftFPMapRangeCache {
    cc_uint16 firstQuad, quadCount;
    SoftFPMapQuadBounds aggregate;
} SoftFPMapRangeCache;
typedef struct SoftFPMapCullData {
    SoftFPMapQuadBounds aggregate;
    cc_uint32 cacheKey;
    cc_uint32 temporalKey;
    cc_uint32 quadCount;
    cc_uint32 groupCount;
    cc_uint32 bitBytes;
    cc_uint32 groupBitBytes;
    void* temporal;
    cc_uint8 rangeNext;
    SoftFPMapRangeCache ranges[SOFTFP_RANGE_CACHE_SIZE];
    SoftFPMapQuadBounds quads[1];
} SoftFPMapCullData;
static SoftFPMapCullData* gfx_mapCullData;
static const SoftFPMapQuadBounds* gfx_mapCullBounds;
static SoftFPClipPlane mapCullLocalPlanes[NUM_PLANES];
static int gfx_map_origin_x, gfx_map_origin_y, gfx_map_origin_z;
static int gfx_map_camera_x_q8, gfx_map_camera_y_q8, gfx_map_camera_z_q8;
static cc_uint8* gfx_map_group_known;
static cc_uint8* gfx_map_group_result;
static cc_uint8* gfx_map_quad_known;
static cc_uint8* gfx_map_quad_result;
static struct Matrix mapMvpPacked;
static cc_uint32 gfx_mvp_cache_key = 1;
static cc_uint32 gfx_map_generation = 1;

static SoftFPMapQuadBounds* SoftFP_CullGroupBounds(SoftFPMapCullData* data) {
    return data->quads + data->quadCount;
}

static cc_uint16* SoftFP_CullFaces(SoftFPMapCullData* data) {
    return (cc_uint16*)(SoftFP_CullGroupBounds(data) + data->groupCount);
}

static cc_uint8* SoftFP_CullGroupKnownBits(SoftFPMapCullData* data) {
    return (cc_uint8*)(SoftFP_CullFaces(data) + data->quadCount);
}

static cc_uint8* SoftFP_CullGroupResultBits(SoftFPMapCullData* data) {
    return SoftFP_CullGroupKnownBits(data) + data->groupBitBytes;
}

static cc_uint8* SoftFP_CullKnownBits(SoftFPMapCullData* data) {
    return SoftFP_CullGroupResultBits(data) + data->groupBitBytes;
}

#ifdef CC_BUILD_ESP32S3
static CC_INLINE cc_uint32 SoftFP_ReadCycles(void) {
    cc_uint32 cycles;
    __asm__ __volatile__("rsr.ccount %0" : "=a"(cycles));
    return cycles;
}
#endif

static cc_uint8* SoftFP_CullResultBits(SoftFPMapCullData* data) {
    return SoftFP_CullKnownBits(data) + data->bitBytes;
}

static cc_uint32* SoftFP_TemporalRects(SoftFPMapCullData* data) {
    return (cc_uint32*)data->temporal;
}

static DepthValue* SoftFP_TemporalDepths(SoftFPMapCullData* data) {
    return (DepthValue*)(SoftFP_TemporalRects(data) + data->quadCount);
}

static cc_uint8* SoftFP_TemporalValidBits(SoftFPMapCullData* data) {
    return (cc_uint8*)(SoftFP_TemporalDepths(data) + data->quadCount);
}

static cc_uint8* SoftFP_CullClipMasks(SoftFPMapCullData* data) {
    return SoftFP_TemporalValidBits(data) + data->bitBytes;
}

static CC_INLINE cc_uint32 SoftFP_TemporalQuadKey(int firstVertex) {
    cc_uint32 quadIndex;
    if (!gfx_mapCullData || !gfx_mapCullData->temporal || firstVertex < 0) return 0;
    quadIndex = (cc_uint32)firstVertex >> 2;
    if (quadIndex >= gfx_mapCullData->quadCount) return 0;
    return quadIndex + 1u;
}

static CC_INLINE cc_uint16 SoftFP_PackTerrainColor(PackedCol color) {
    return (cc_uint16)(((PackedCol_R(color) & 0xF8) << 8) |
                       ((PackedCol_G(color) & 0xFC) << 3) |
                        (PackedCol_B(color) >> 3));
}

static CC_INLINE PackedCol SoftFP_UnpackTerrainColor(cc_uint16 color) {
    int r5 = (color >> 11) & 0x1F;
    int g6 = (color >>  5) & 0x3F;
    int b5 =  color        & 0x1F;
    return PackedCol_Make((r5 << 3) | (r5 >> 2),
                          (g6 << 2) | (g6 >> 4),
                          (b5 << 3) | (b5 >> 2), 0xFF);
}

void* Gfx_CreateMapCullBounds(GfxResourceID vb, int verticesCount,
                              int originX, int originY, int originZ) {
    const PackedTerrainVertex* src = (const PackedTerrainVertex*)vb;
    SoftFPMapCullData* data;
    SoftFPMapQuadBounds* bounds;
    SoftFPMapQuadBounds* groups;
    cc_uint16* faces;
    int quadCount = verticesCount >> 2;
    int groupCount = (quadCount + 7) >> 3;
    const int quantStep = 256 >> 3;
    cc_uint32 bytes;
    int i;

    (void)originX; (void)originY; (void)originZ;

    if (!src || quadCount <= 0) return NULL;
    data = NULL;
    {
        cc_uint32 bitBytes = ((cc_uint32)quadCount + 7u) >> 3;
        cc_uint32 groupBitBytes = ((cc_uint32)groupCount + 7u) >> 3;
        bytes = (cc_uint32)offsetof(SoftFPMapCullData, quads) +
                (cc_uint32)quadCount * (cc_uint32)sizeof(data->quads[0]) +
                (cc_uint32)groupCount * (cc_uint32)sizeof(data->quads[0]) +
                (cc_uint32)quadCount * (cc_uint32)sizeof(cc_uint16) +
                groupBitBytes * 2u + bitBytes * 2u;
    }
    /* Reuse otherwise idle space in the already-reserved internal hot arena,
     * while retaining a small tail for late dynamic UI/model vertex buffers. */
    if (holo_port_hot_vb_reserved() >=
        holo_port_hot_vb_used() + bytes + 512u) {
        data = (SoftFPMapCullData*)holo_port_hot_vb_alloc(bytes);
    }
    if (!data) data = (SoftFPMapCullData*)Mem_TryAlloc(1, bytes);
    if (!data) return NULL;
    data->cacheKey = 0;
    data->temporalKey = 0;
    data->temporal = NULL;
    data->quadCount = (cc_uint32)quadCount;
    data->groupCount = (cc_uint32)groupCount;
    data->bitBytes = ((cc_uint32)quadCount + 7u) >> 3;
    data->groupBitBytes = ((cc_uint32)groupCount + 7u) >> 3;
    data->rangeNext = 0;
    Mem_Set(data->ranges, 0, sizeof(data->ranges));
    Mem_Set(SoftFP_CullGroupKnownBits(data), 0, data->groupBitBytes);
    Mem_Set(SoftFP_CullKnownBits(data), 0, data->bitBytes);
    bounds = data->quads;
    groups = SoftFP_CullGroupBounds(data);
    faces  = SoftFP_CullFaces(data);
    data->aggregate.minX = data->aggregate.minY = data->aggregate.minZ = 0xFF;
    data->aggregate.maxX = data->aggregate.maxY = data->aggregate.maxZ = 0;

    for (i = 0; i < quadCount; i++, src += 4) {
        int minX = min(src[0].x, src[2].x), maxX = max(src[0].x, src[2].x);
        int minY = min(src[0].y, src[2].y), maxY = max(src[0].y, src[2].y);
        int minZ = min(src[0].z, src[2].z), maxZ = max(src[0].z, src[2].z);
        int qMinX, qMaxX, qMinY, qMaxY, qMinZ, qMaxZ;
        int e1x = src[1].x - src[0].x, e1y = src[1].y - src[0].y;
        int e1z = src[1].z - src[0].z;
        int e2x = src[2].x - src[0].x, e2y = src[2].y - src[0].y;
        int e2z = src[2].z - src[0].z;
        int nx = e1y * e2z - e1z * e2y;
        int ny = e1z * e2x - e1x * e2z;
        int nz = e1x * e2y - e1y * e2x;
        int axis = 0, plane = 0, positive = 0;

        /* Chunk geometry must stay inside its 16^3 local cube. Reject an
         * unusual buffer as a whole and retain the vertex-derived fallback. */
        if (minX < 0 || maxX > 16 * 256 ||
            minY < 0 || maxY > 16 * 256 ||
            minZ < 0 || maxZ > 16 * 256) {
            if (!holo_port_hot_vb_free(data)) Mem_Free(data);
            return NULL;
        }

        qMinX = minX / quantStep;
        qMaxX = (maxX + quantStep - 1) / quantStep;
        qMinY = minY / quantStep;
        qMaxY = (maxY + quantStep - 1) / quantStep;
        qMinZ = minZ / quantStep;
        qMaxZ = (maxZ + quantStep - 1) / quantStep;
        bounds[i].minX = (cc_uint8)qMinX; bounds[i].maxX = (cc_uint8)qMaxX;
        bounds[i].minY = (cc_uint8)qMinY; bounds[i].maxY = (cc_uint8)qMaxY;
        bounds[i].minZ = (cc_uint8)qMinZ; bounds[i].maxZ = (cc_uint8)qMaxZ;
        /* Bits 15..14 encode axis (0=unusual, 1=X, 2=Y, 3=Z), bit 13
         * encodes a positive normal, and bits 12..0 store the exact Q8 plane. */
        if (nx && !ny && !nz) { axis = 1; plane = src[0].x; positive = nx > 0; }
        if (ny && !nx && !nz) { axis = 2; plane = src[0].y; positive = ny > 0; }
        if (nz && !nx && !ny) { axis = 3; plane = src[0].z; positive = nz > 0; }
        faces[i] = axis && plane >= 0 && plane <= 0x1FFF ?
            (cc_uint16)((axis << 14) | (positive ? 0x2000 : 0) | plane) : 0;
        data->aggregate.minX = min(data->aggregate.minX, bounds[i].minX);
        data->aggregate.minY = min(data->aggregate.minY, bounds[i].minY);
        data->aggregate.minZ = min(data->aggregate.minZ, bounds[i].minZ);
        data->aggregate.maxX = max(data->aggregate.maxX, bounds[i].maxX);
        data->aggregate.maxY = max(data->aggregate.maxY, bounds[i].maxY);
        data->aggregate.maxZ = max(data->aggregate.maxZ, bounds[i].maxZ);
        if (!(i & 7)) {
            groups[i >> 3] = bounds[i];
        } else {
            SoftFPMapQuadBounds* group = &groups[i >> 3];
            group->minX = min(group->minX, bounds[i].minX);
            group->minY = min(group->minY, bounds[i].minY);
            group->minZ = min(group->minZ, bounds[i].minZ);
            group->maxX = max(group->maxX, bounds[i].maxX);
            group->maxY = max(group->maxY, bounds[i].maxY);
            group->maxZ = max(group->maxZ, bounds[i].maxZ);
        }
    }
    {
        cc_uint32 temporalBytes = (cc_uint32)quadCount *
            ((cc_uint32)sizeof(cc_uint32) + (cc_uint32)sizeof(DepthValue)) +
            data->bitBytes + (cc_uint32)quadCount;
        data->temporal = Mem_TryAlloc(1, temporalBytes);
        if (data->temporal) {
            Mem_Set(SoftFP_TemporalValidBits(data), 0, data->bitBytes);
            Mem_Set(SoftFP_CullClipMasks(data), 0x3F, (cc_uint32)quadCount);
        }
    }
    gfx_map_generation++;
    if (!gfx_map_generation) gfx_map_generation = 1;
    return data;
}

void Gfx_DeleteMapCullBounds(void** bounds) {
    SoftFPMapCullData* data = (SoftFPMapCullData*)*bounds;
    if (gfx_mapCullData == data) {
        gfx_mapCullData = NULL;
        gfx_mapCullBounds = NULL;
        gfx_map_group_known = gfx_map_group_result = NULL;
        gfx_map_quad_known  = gfx_map_quad_result  = NULL;
    }
    if (data) {
        if (data->temporal) Mem_Free(data->temporal);
        gfx_map_generation++;
        if (!gfx_map_generation) gfx_map_generation = 1;
    }
    if (data && !holo_port_hot_vb_free(data)) Mem_Free(data);
    *bounds = NULL;
}

void Gfx_BindMapCullBounds(void* bounds, int originX, int originY, int originZ) {
    const struct Matrix* m = &_mvp_float;
    cc_uint8 planes;
    gfx_mapCullData = (SoftFPMapCullData*)bounds;
    gfx_mapCullBounds = gfx_mapCullData ? gfx_mapCullData->quads : NULL;
    gfx_map_origin_x = originX;
    gfx_map_origin_y = originY;
    gfx_map_origin_z = originZ;
    gfx_map_camera_x_q8 = (int)((Camera.CurrentPos.x - originX) * 256.0f);
    gfx_map_camera_y_q8 = (int)((Camera.CurrentPos.y - originY) * 256.0f);
    gfx_map_camera_z_q8 = (int)((Camera.CurrentPos.z - originZ) * 256.0f);
    gfx_map_group_known  = gfx_mapCullData ? SoftFP_CullGroupKnownBits(gfx_mapCullData) : NULL;
    gfx_map_group_result = gfx_mapCullData ? SoftFP_CullGroupResultBits(gfx_mapCullData) : NULL;
    gfx_map_quad_known   = gfx_mapCullData ? SoftFP_CullKnownBits(gfx_mapCullData) : NULL;
    gfx_map_quad_result  = gfx_mapCullData ? SoftFP_CullResultBits(gfx_mapCullData) : NULL;
    if (gfx_mapCullData) {
        cc_uint32 key = gfx_mvp_cache_key ^ ((cc_uint32)mapClipPlanes * 0x9E3779B1u);
        if (gfx_mapCullData->cacheKey != key) {
            gfx_mapCullData->cacheKey = key;
            Mem_Set(gfx_map_group_known, 0, gfx_mapCullData->groupBitBytes);
            Mem_Set(gfx_map_quad_known, 0, gfx_mapCullData->bitBytes);
        }
    }
    terrainFrameMvpKey = gfx_mvp_cache_key;
    /* Packed positions are chunk-local Q8. Fold the chunk origin into the MVP
     * once, and scale the three basis rows once, so a terrain quad can share
     * the transform of its two rectangle edges. */
    mapMvpPacked.row1.x = m->row1.x * (1.0f / 256.0f);
    mapMvpPacked.row1.y = m->row1.y * (1.0f / 256.0f);
    mapMvpPacked.row1.z = m->row1.z * (1.0f / 256.0f);
    mapMvpPacked.row1.w = m->row1.w * (1.0f / 256.0f);
    mapMvpPacked.row2.x = m->row2.x * (1.0f / 256.0f);
    mapMvpPacked.row2.y = m->row2.y * (1.0f / 256.0f);
    mapMvpPacked.row2.z = m->row2.z * (1.0f / 256.0f);
    mapMvpPacked.row2.w = m->row2.w * (1.0f / 256.0f);
    mapMvpPacked.row3.x = m->row3.x * (1.0f / 256.0f);
    mapMvpPacked.row3.y = m->row3.y * (1.0f / 256.0f);
    mapMvpPacked.row3.z = m->row3.z * (1.0f / 256.0f);
    mapMvpPacked.row3.w = m->row3.w * (1.0f / 256.0f);
    mapMvpPacked.row4.x = originX * m->row1.x + originY * m->row2.x +
                          originZ * m->row3.x + m->row4.x;
    mapMvpPacked.row4.y = originX * m->row1.y + originY * m->row2.y +
                          originZ * m->row3.y + m->row4.y;
    mapMvpPacked.row4.z = originX * m->row1.z + originY * m->row2.z +
                          originZ * m->row3.z + m->row4.z;
    mapMvpPacked.row4.w = originX * m->row1.w + originY * m->row2.w +
                          originZ * m->row3.w + m->row4.w;
    if (!bounds) return;
    if (gfx_mapCullData->temporal &&
        gfx_mapCullData->temporalKey != gfx_mvp_cache_key) {
        gfx_mapCullData->temporalKey = gfx_mvp_cache_key;
        Mem_Set(SoftFP_TemporalValidBits(gfx_mapCullData), 0,
                gfx_mapCullData->bitBytes);
    }

    /* Convert world-space clip planes to the packed chunk-local 1/8 grid
     * once per bound buffer bind. Most chunks are fully inside the frustum,
     * so only prepare the planes which can actually clip this chunk. */
    planes = mapClipPlanes;
    while (planes) {
        int i = __builtin_ctz((unsigned)planes);
        const SoftFPClipPlane* src = &quadCullPlanes[i];
        SoftFPClipPlane* dst = &mapCullLocalPlanes[i];
        planes &= (cc_uint8)(planes - 1u);
        dst->x = src->x * 0.125f;
        dst->y = src->y * 0.125f;
        dst->z = src->z * 0.125f;
        dst->w = src->w + originX * src->x + originY * src->y + originZ * src->z;
    }
}
#endif

static VertexFormat buf_fmt;
static int buf_count;

static void PreprocessTexturedVertices(void* vertices) {
	struct FPVertexTextured* dst = vertices;
	struct VertexTextured* src   = vertices;

	for (int i = 0; i < buf_count; i++, src++, dst++)
	{
		dst->x = FloatToFixed(src->x);
		dst->y = FloatToFixed(src->y);
		dst->z = FloatToFixed(src->z);
		dst->u = FloatToFixed(src->U);
		dst->v = FloatToFixed(src->V);
		dst->c = src->Col;
	}
}

static void PreprocessColouredVertices(void* vertices) {
	struct FPVertexColoured* dst = vertices;
	struct VertexColoured* src   = vertices;

	for (int i = 0; i < buf_count; i++, src++, dst++)
	{
		dst->x = FloatToFixed(src->x);
		dst->y = FloatToFixed(src->y);
		dst->z = FloatToFixed(src->z);
		dst->c = src->Col;
	}
}

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
void Gfx_PackMapVb(GfxResourceID* vb, int verticesCount,
                   int originX, int originY, int originZ) {
    struct FPVertexTextured* src;
    PackedTerrainVertex* dst;
    GfxResourceID compact;
    int originXFixed = originX * FP_ONE;
    int originYFixed = originY * FP_ONE;
    int originZFixed = originZ * FP_ONE;
    int i;

    if (!vb || !*vb || verticesCount <= 0) return;
    src = (struct FPVertexTextured*)*vb;
    dst = (PackedTerrainVertex*)*vb;

    /* Destination elements are half the source size, therefore forward
     * in-place conversion never overwrites a source vertex not yet read. */
    for (i = 0; i < verticesCount; i++) {
        int localX = src[i].x - originXFixed;
        int localY = src[i].y - originYFixed;
        int localZ = src[i].z - originZFixed;
        int u = src[i].u >> 4;
        int v = src[i].v;

        Math_Clamp(u, 0, 0xFFFF);
        Math_Clamp(v, 0, 0xFFFF);

        dst[i].x = (cc_int16)(localX / 256);
        dst[i].y = (cc_int16)(localY / 256);
        dst[i].z = (cc_int16)(localZ / 256);
        dst[i].u = (cc_uint16)u;
        dst[i].v = (cc_uint16)v;
        dst[i].color = SoftFP_PackTerrainColor(src[i].c);
    }

    /* Shrinking can temporarily fail under extreme fragmentation. The old
     * allocation is still valid and already contains the packed stream, so
     * rendering remains correct even when this particular chunk saves no RAM. */
    compact = Mem_TryRealloc(*vb, (cc_uint32)verticesCount,
                             (cc_uint32)sizeof(PackedTerrainVertex));
    if (compact) *vb = compact;
}
#endif

static GfxResourceID Gfx_AllocStaticVb(VertexFormat fmt, int count) {
    return Mem_TryAlloc(count, strideSizes[fmt]);
}

#ifdef CC_BUILD_ESP32S3
static GfxResourceID Gfx_AllocDynamicVb(VertexFormat fmt, int maxVertices) {
    cc_uint32 bytes = (cc_uint32)maxVertices * (cc_uint32)strideSizes[fmt];
    void *memory = holo_port_hot_vb_alloc(bytes);
    return memory ? memory : Mem_TryAlloc(1, bytes);
}

void Gfx_BindDynamicVb(GfxResourceID vb) { Gfx_BindVb(vb); }
void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
    return Gfx_LockVb(vb, fmt, count);
}
void Gfx_UnlockDynamicVb(GfxResourceID vb) { Gfx_UnlockVb(vb); Gfx_BindVb(vb); }
void Gfx_DeleteDynamicVb(GfxResourceID* vb) {
    GfxResourceID data = *vb;
    if (data && !holo_port_hot_vb_free(data)) Mem_Free(data);
    *vb = NULL;
}
#endif

void Gfx_BindVb(GfxResourceID vb) { gfx_vertices = vb; }

void Gfx_DeleteVb(GfxResourceID* vb) {
    GfxResourceID data = *vb;
    if (data) Mem_Free(data);

    *vb = 0;
}

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
    buf_fmt   = fmt;
    buf_count = count;
	return vb;
}

void Gfx_UnlockVb(GfxResourceID vb) { 
    if (buf_fmt == VERTEX_FORMAT_TEXTURED) {
        PreprocessTexturedVertices(vb);
    } else {
        PreprocessColouredVertices(vb);
    }
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/

#ifndef CC_BUILD_SOFTFP_FLOAT_XFORM
static void MatrixToFixed(FixedMatrix* dst, const struct Matrix* src) {
    dst->m[0][0] = FloatToFixed(src->row1.x); dst->m[0][1] = FloatToFixed(src->row1.y);
    dst->m[0][2] = FloatToFixed(src->row1.z); dst->m[0][3] = FloatToFixed(src->row1.w);
    
    dst->m[1][0] = FloatToFixed(src->row2.x); dst->m[1][1] = FloatToFixed(src->row2.y);
    dst->m[1][2] = FloatToFixed(src->row2.z); dst->m[1][3] = FloatToFixed(src->row2.w);
    
    dst->m[2][0] = FloatToFixed(src->row3.x); dst->m[2][1] = FloatToFixed(src->row3.y);
    dst->m[2][2] = FloatToFixed(src->row3.z); dst->m[2][3] = FloatToFixed(src->row3.w);
    
    dst->m[3][0] = FloatToFixed(src->row4.x); dst->m[3][1] = FloatToFixed(src->row4.y);
    dst->m[3][2] = FloatToFixed(src->row4.z); dst->m[3][3] = FloatToFixed(src->row4.w);
}

static void MatrixMulFixed(FixedMatrix* dst, const FixedMatrix* a, const FixedMatrix* b) {
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int64_t sum = 0;
            for (k = 0; k < 4; k++) {
                sum += (int64_t)a->m[i][k] * b->m[k][j];
            }
            dst->m[i][j] = (int)(sum >> FP_SHIFT);
        }
    }
}
#endif

#if defined CC_BUILD_SOFTFP_FLOAT_XFORM && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
static cc_uint32 SoftFP_MatrixCacheKey(const struct Matrix* matrix) {
    const cc_uint32* words = (const cc_uint32*)matrix;
    cc_uint32 hash = 2166136261u;
    int i;
    for (i = 0; i < 16; i++) hash = (hash ^ words[i]) * 16777619u;
    return hash ? hash : 1u;
}

static CC_NOINLINE void SoftFP_SetQuadCullPlane(int index, float x, float y, float z, float w) {
    SoftFPClipPlane* plane = &quadCullPlanes[index];
    plane->x = x; plane->y = y; plane->z = z; plane->w = w;
}

static void SoftFP_UpdateQuadCullPlanes(void) {
    const struct Matrix* m = &_mvp_float;
    SoftFP_SetQuadCullPlane(PLANE_LEFT,
        m->row1.x + m->row1.w, m->row2.x + m->row2.w,
        m->row3.x + m->row3.w, m->row4.x + m->row4.w);
    SoftFP_SetQuadCullPlane(PLANE_RIGHT,
        m->row1.w - m->row1.x, m->row2.w - m->row2.x,
        m->row3.w - m->row3.x, m->row4.w - m->row4.x);
    SoftFP_SetQuadCullPlane(PLANE_BOTTOM,
        m->row1.y + m->row1.w, m->row2.y + m->row2.w,
        m->row3.y + m->row3.w, m->row4.y + m->row4.w);
    SoftFP_SetQuadCullPlane(PLANE_TOP,
        m->row1.w - m->row1.y, m->row2.w - m->row2.y,
        m->row3.w - m->row3.y, m->row4.w - m->row4.y);
    SoftFP_SetQuadCullPlane(PLANE_NEAR,
        m->row1.z, m->row2.z, m->row3.z, m->row4.z);
    SoftFP_SetQuadCullPlane(PLANE_FAR,
        m->row1.w - m->row1.z,
        m->row2.w - m->row2.z,
        m->row3.w - m->row3.z,
        m->row4.w - m->row4.z);
}

cc_uint8 Gfx_GetAABBClipPlanes(float x, float y, float z,
                               float halfX, float halfY, float halfZ) {
    cc_uint8 mask = 0;
    int i;

    for (i = 0; i < NUM_PLANES; i++) {
        const SoftFPClipPlane* plane = &quadCullPlanes[i];
        float distance = x * plane->x + y * plane->y + z * plane->z + plane->w;
        float radius = Math_AbsF(plane->x) * halfX +
                       Math_AbsF(plane->y) * halfY +
                       Math_AbsF(plane->z) * halfZ;
        /* Only planes intersecting this chunk can reject one of its quads.
         * Keep a small inside margin so boundary chunks take the exact path. */
        if (distance <= radius + (1.0f / 1024.0f)) mask |= (cc_uint8)(1u << i);
    }
    return mask;
}

void Gfx_SetMapClipPlanes(cc_uint8 planes) {
    mapClipPlanes = planes & 0x3F;
}

#endif

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
#ifdef CC_BUILD_SOFTFP_FLOAT_XFORM
    if (type == MATRIX_VIEW) _view_float = *matrix;
    if (type == MATRIX_PROJ) _proj_float = *matrix;
    Matrix_Mul(&_mvp_float, &_view_float, &_proj_float);
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    gfx_mvp_cache_key = SoftFP_MatrixCacheKey(&_mvp_float);
    SoftFP_UpdateQuadCullPlanes();
#endif
#else
    if (type == MATRIX_VIEW) {
        MatrixToFixed(&_view_fp, matrix);
    }
    if (type == MATRIX_PROJ) {
        MatrixToFixed(&_proj_fp, matrix);
    }

    MatrixMulFixed(&_mvp_fp, &_view_fp, &_proj_fp);
#endif
}

void Gfx_LoadMVP(const struct Matrix* view, const struct Matrix* proj, struct Matrix* mvp) {
#ifdef CC_BUILD_SOFTFP_FLOAT_XFORM
    _view_float = *view;
    _proj_float = *proj;
    Matrix_Mul(&_mvp_float, view, proj);
    *mvp = _mvp_float;
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    gfx_mvp_cache_key = SoftFP_MatrixCacheKey(&_mvp_float);
    SoftFP_UpdateQuadCullPlanes();
#endif
#else
    MatrixToFixed(&_view_fp, view);
    MatrixToFixed(&_proj_fp, proj);

    MatrixMulFixed(&_mvp_fp, &_view_fp, &_proj_fp);
    
    Matrix_Mul(mvp, view, proj);
#endif
}

void Gfx_EnableTextureOffset(float x, float y) {
	tex_offseting = true;
    texOffsetX_fp = FloatToFixed(x);
    texOffsetY_fp = FloatToFixed(y);
}

void Gfx_DisableTextureOffset(void) {
	tex_offseting = false;
}

static CC_NOINLINE void ShiftTextureCoords(int count) {
	for (int i = 0; i < count; i++) 
	{
		struct FPVertexTextured* v = (struct FPVertexTextured*)gfx_vertices + i;
		v->u += texOffsetX_fp; // TODO avoid overflow
		v->v += texOffsetY_fp;
	}
}

static CC_NOINLINE void UnshiftTextureCoords(int count) {
	for (int i = 0; i < count; i++) 
	{
		struct FPVertexTextured* v = (struct FPVertexTextured*)gfx_vertices + i;
		v->u -= texOffsetX_fp; // TODO avoid overflow
		v->v -= texOffsetY_fp;
	}
}

void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
    *matrix = Matrix_Identity;

    matrix->row1.x =  2.0f / width;
    matrix->row2.y = -2.0f / height;
    matrix->row3.z =  1.0f / (zNear - zFar);

    matrix->row4.x = -1.0f;
    matrix->row4.y =  1.0f;
    matrix->row4.z = zNear / (zNear - zFar);
}

static float Cotangent(float x) { return Math_CosF(x) / Math_SinF(x); }

void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
    float zNear = 0.1f;

    float c = Cotangent(0.5f * fov);
    *matrix = Matrix_Identity;

    matrix->row1.x =  c / aspect;
    matrix->row2.y =  c;
    matrix->row3.z = zFar / (zNear - zFar);
    matrix->row3.w = -1.0f;
    matrix->row4.z = (zNear * zFar) / (zNear - zFar);
    matrix->row4.w =  0.0f;
}


/*########################################################################################################################*
*---------------------------------------------------------Rendering-------------------------------------------------------*
*#########################################################################################################################*/
static void TransformVertex2D(int index, VertexFixed* vertex) {
    struct FPVertexColoured* v_col;
    struct FPVertexTextured* v_tex;
    
    if (gfx_format != VERTEX_FORMAT_TEXTURED) {
        v_col = (struct FPVertexColoured*)gfx_vertices + index;
        vertex->x = v_col->x;
        vertex->y = v_col->y;
        vertex->u = 0;
        vertex->v = 0;
        vertex->c = v_col->c;
    } else {
        v_tex = (struct FPVertexTextured*)gfx_vertices + index;
        vertex->x = v_tex->x;
        vertex->y = v_tex->y;
        vertex->u = v_tex->u;
        vertex->v = v_tex->v;
        vertex->c = v_tex->c;
    }
}

#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
/* Gfx_DrawIndexedTris_T2fC4b is the map-renderer-only entry point. Chunk
 * quads store opposite corners at vertices 0 and 2, so their complete AABB
 * can be formed without loading and comparing all four vertices. Do not use
 * this for dynamic/model quads: camera-facing rectangles can have a diagonal
 * whose component-wise bounds do not contain their other two vertices. */
static CC_INLINE cc_bool SoftFP_MapBoundsOutsideFrustum(
    const SoftFPMapQuadBounds* bounds, cc_uint8 planeMask,
    cc_uint8* reducedClipMask) {
    cc_uint8 uncertain = 0;
    while (planeMask) {
        const float rejectMargin = -1.0f / 1024.0f;
        const float insideMargin =  1.0f / 1024.0f;
        const SoftFPClipPlane* plane;
        float px, py, pz, nx, ny, nz, support;
        int i = __builtin_ctz((unsigned)planeMask);
        planeMask &= (cc_uint8)(planeMask - 1u);
        plane = &mapCullLocalPlanes[i];
        px = plane->x < 0.0f ? bounds->minX : bounds->maxX;
        py = plane->y < 0.0f ? bounds->minY : bounds->maxY;
        pz = plane->z < 0.0f ? bounds->minZ : bounds->maxZ;
        support = px * plane->x + py * plane->y + pz * plane->z + plane->w;
        if (support < rejectMargin) return true;
        if (!reducedClipMask) continue;

        nx = plane->x < 0.0f ? bounds->maxX : bounds->minX;
        ny = plane->y < 0.0f ? bounds->maxY : bounds->minY;
        nz = plane->z < 0.0f ? bounds->maxZ : bounds->minZ;
        support = nx * plane->x + ny * plane->y + nz * plane->z + plane->w;
        if (support < insideMargin) uncertain |= (cc_uint8)(1u << i);
    }
    if (reducedClipMask) *reducedClipMask = uncertain;
    return false;
}

static CC_INLINE cc_bool SoftFP_MapGroupOutsideFrustum(
    int firstVertex, cc_uint8 planeMask) {
    cc_uint32 quadIndex, groupIndex, cacheByte;
    cc_uint8 cacheBit;
    SoftFPMapQuadBounds* groups;
    cc_bool outside;
    if (!gfx_mapCullData || firstVertex < 0) return false;
    quadIndex = (cc_uint32)firstVertex >> 2;
    groupIndex = quadIndex >> 3;
    if (groupIndex >= gfx_mapCullData->groupCount) return false;
    cacheByte = groupIndex >> 3;
    cacheBit  = (cc_uint8)(1u << (groupIndex & 7u));
    if (gfx_map_group_known[cacheByte] & cacheBit) {
        return (gfx_map_group_result[cacheByte] & cacheBit) != 0;
    }
    groups = SoftFP_CullGroupBounds(gfx_mapCullData);
    outside = SoftFP_MapBoundsOutsideFrustum(&groups[groupIndex], planeMask, NULL);
    if (outside) gfx_map_group_result[cacheByte] |= cacheBit;
    else         gfx_map_group_result[cacheByte] &= (cc_uint8)~cacheBit;
    gfx_map_group_known[cacheByte] |= cacheBit;
    return outside;
}

static CC_INLINE cc_bool SoftFP_MapQuadBackfacing(int firstVertex) {
    cc_uint32 quadIndex;
    cc_uint16 face;
    int axis, plane, camera;
    const int margin = 2;
    if (!gfx_mapCullData || firstVertex < 0) return false;
    quadIndex = (cc_uint32)firstVertex >> 2;
    if (quadIndex >= gfx_mapCullData->quadCount) return false;
    face = SoftFP_CullFaces(gfx_mapCullData)[quadIndex];
    axis = face >> 14;
    if (!axis) return false;
    plane = face & 0x1FFF;
    camera = axis == 1 ? gfx_map_camera_x_q8 :
             axis == 2 ? gfx_map_camera_y_q8 : gfx_map_camera_z_q8;
    /* Keep faces within 2/256 block of the eye plane on the proven path. */
    if (face & 0x2000) return camera < plane - margin;
    return camera > plane + margin;
}

static CC_NOINLINE cc_bool SoftFP_MapQuadOutsideFrustumSlow(int firstVertex,
                                                            cc_uint8 planeMask,
                                                            cc_uint8* reducedClipMask) {
    const PackedTerrainVertex* src =
        (const PackedTerrainVertex*)gfx_vertices + firstVertex;
    float minXf, minYf, minZf, maxXf, maxYf, maxZf;
    cc_bool outside = false;
    cc_uint32 quadIndex = (cc_uint32)firstVertex >> 2;
    cc_uint32 cacheByte = quadIndex >> 3;
    cc_uint8 cacheBit = (cc_uint8)(1u << (quadIndex & 7u));
    cc_uint8 reduced = 0;
    int i;

    if (gfx_mapCullBounds) {
        const SoftFPMapQuadBounds* bounds = &gfx_mapCullBounds[quadIndex];
        outside = SoftFP_MapBoundsOutsideFrustum(bounds, planeMask, &reduced);
        goto cache_result;
    } else {
        int minX = min(src[0].x, src[2].x), maxX = max(src[0].x, src[2].x);
        int minY = min(src[0].y, src[2].y), maxY = max(src[0].y, src[2].y);
        int minZ = min(src[0].z, src[2].z), maxZ = max(src[0].z, src[2].z);
        const float q8Scale = 1.0f / 256.0f;

        minXf = gfx_map_origin_x + minX * q8Scale;
        maxXf = gfx_map_origin_x + maxX * q8Scale;
        minYf = gfx_map_origin_y + minY * q8Scale;
        maxYf = gfx_map_origin_y + maxY * q8Scale;
        minZf = gfx_map_origin_z + minZ * q8Scale;
        maxZf = gfx_map_origin_z + maxZ * q8Scale;
    }

    /* The margin covers different float grouping versus the original
     * component transform. Any boundary quad continues down that exact path. */
    while (planeMask) {
        const float rejectMargin = -1.0f / 1024.0f;
        const float insideMargin =  1.0f / 1024.0f;
        const SoftFPClipPlane* plane;
        float px, py, pz, nx, ny, nz, support;
        cc_uint8 planeBit;

        i = __builtin_ctz((unsigned)planeMask);
        planeBit = (cc_uint8)(1u << i);
        planeMask &= (cc_uint8)(planeMask - 1u);
        plane = &quadCullPlanes[i];
        px = plane->x < 0.0f ? minXf : maxXf;
        py = plane->y < 0.0f ? minYf : maxYf;
        pz = plane->z < 0.0f ? minZf : maxZf;
        support = px * plane->x + py * plane->y + pz * plane->z + plane->w;
        if (support < rejectMargin) { outside = true; break; }
        nx = plane->x < 0.0f ? maxXf : minXf;
        ny = plane->y < 0.0f ? maxYf : minYf;
        nz = plane->z < 0.0f ? maxZf : minZf;
        support = nx * plane->x + ny * plane->y + nz * plane->z + plane->w;
        if (support < insideMargin) reduced |= planeBit;
    }

cache_result:
    if (gfx_map_quad_known && gfx_mapCullData &&
        quadIndex < gfx_mapCullData->quadCount) {
        if (outside) gfx_map_quad_result[cacheByte] |= cacheBit;
        else         gfx_map_quad_result[cacheByte] &= (cc_uint8)~cacheBit;
        gfx_map_quad_known[cacheByte] |= cacheBit;
        if (gfx_mapCullData->temporal) {
            SoftFP_CullClipMasks(gfx_mapCullData)[quadIndex] = reduced;
        }
    }
    if (!outside && reducedClipMask) *reducedClipMask = reduced;
    return outside;
}

static CC_INLINE cc_bool SoftFP_MapQuadOutsideFrustum(int firstVertex,
                                                       cc_uint8 planeMask,
                                                       cc_uint8* reducedClipMask) {
    cc_uint32 quadIndex, cacheByte;
    cc_uint8 cacheBit;
    if (gfx_map_quad_known && gfx_mapCullData && firstVertex >= 0) {
        quadIndex = (cc_uint32)firstVertex >> 2;
        if (quadIndex < gfx_mapCullData->quadCount) {
            cacheByte = quadIndex >> 3;
            cacheBit  = (cc_uint8)(1u << (quadIndex & 7u));
            if (gfx_map_quad_known[cacheByte] & cacheBit) {
                cc_bool outside = (gfx_map_quad_result[cacheByte] & cacheBit) != 0;
                if (!outside && reducedClipMask) {
                    *reducedClipMask = gfx_mapCullData->temporal ?
                        SoftFP_CullClipMasks(gfx_mapCullData)[quadIndex] : planeMask;
                }
                return outside;
            }
        }
    }
    return SoftFP_MapQuadOutsideFrustumSlow(firstVertex, planeMask,
                                             reducedClipMask);
}
#endif

static int TransformVertex3D(int index, VertexFixed* vertex, cc_bool packedMap) {
    int pos_x, pos_y, pos_z;
    struct FPVertexColoured* v_col;
    struct FPVertexTextured* v_tex;

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    if (packedMap) {
        const PackedTerrainVertex* packed =
            (const PackedTerrainVertex*)gfx_vertices + index;
        vertex->x = gfx_map_origin_x * FP_ONE + packed->x * 256;
        vertex->y = gfx_map_origin_y * FP_ONE + packed->y * 256;
        vertex->z = gfx_map_origin_z * FP_ONE + packed->z * 256;
        vertex->u = ((int)packed->u << 4) + (tex_offseting ? texOffsetX_fp : 0);
        vertex->v = (int)packed->v + (tex_offseting ? texOffsetY_fp : 0);
        vertex->c = SoftFP_UnpackTerrainColor(packed->color);
    } else
#endif
    if (gfx_format != VERTEX_FORMAT_TEXTURED) {
        v_col = (struct FPVertexColoured*)gfx_vertices + index;
        vertex->x = v_col->x;
        vertex->y = v_col->y;
		vertex->z = v_col->z;
        vertex->u = 0;
        vertex->v = 0;
        vertex->c = v_col->c;
    } else {
        v_tex = (struct FPVertexTextured*)gfx_vertices + index;
        vertex->x = v_tex->x;
        vertex->y = v_tex->y;
        vertex->z = v_tex->z;
        vertex->u = v_tex->u;
        vertex->v = v_tex->v;
        vertex->c = v_tex->c;
    }

	pos_x = vertex->x;
	pos_y = vertex->y;
	pos_z = vertex->z;

    if (ABS(pos_x) > (1 << 28) || ABS(pos_y) > (1 << 28) || ABS(pos_z) > (1 << 28)) {
        return 0;
    }

#ifdef CC_BUILD_SOFTFP_FLOAT_XFORM
    {
        float x = FixedToFloat(pos_x), y = FixedToFloat(pos_y), z = FixedToFloat(pos_z);
        float x_temp = x * _mvp_float.row1.x + y * _mvp_float.row2.x +
                       z * _mvp_float.row3.x + _mvp_float.row4.x;
        float y_temp = x * _mvp_float.row1.y + y * _mvp_float.row2.y +
                       z * _mvp_float.row3.y + _mvp_float.row4.y;
        float z_temp = x * _mvp_float.row1.z + y * _mvp_float.row2.z +
                       z * _mvp_float.row3.z + _mvp_float.row4.z;
        float w_temp = x * _mvp_float.row1.w + y * _mvp_float.row2.w +
                       z * _mvp_float.row3.w + _mvp_float.row4.w;
        vertex->x = FloatToFixed(x_temp); vertex->y = FloatToFixed(y_temp);
        vertex->z = FloatToFixed(z_temp); vertex->w = FloatToFixed(w_temp);
    }
#else
    int64_t x_temp = (int64_t)pos_x * _mvp_fp.m[0][0] + (int64_t)pos_y * _mvp_fp.m[1][0] + 
                     (int64_t)pos_z * _mvp_fp.m[2][0] + ((int64_t)_mvp_fp.m[3][0] << FP_SHIFT);
    int64_t y_temp = (int64_t)pos_x * _mvp_fp.m[0][1] + (int64_t)pos_y * _mvp_fp.m[1][1] + 
                     (int64_t)pos_z * _mvp_fp.m[2][1] + ((int64_t)_mvp_fp.m[3][1] << FP_SHIFT);
    int64_t z_temp = (int64_t)pos_x * _mvp_fp.m[0][2] + (int64_t)pos_y * _mvp_fp.m[1][2] + 
                     (int64_t)pos_z * _mvp_fp.m[2][2] + ((int64_t)_mvp_fp.m[3][2] << FP_SHIFT);
    int64_t w_temp = (int64_t)pos_x * _mvp_fp.m[0][3] + (int64_t)pos_y * _mvp_fp.m[1][3] + 
                     (int64_t)pos_z * _mvp_fp.m[2][3] + ((int64_t)_mvp_fp.m[3][3] << FP_SHIFT);

    x_temp >>= FP_SHIFT;
    y_temp >>= FP_SHIFT;
    z_temp >>= FP_SHIFT;
    w_temp >>= FP_SHIFT;

    vertex->x = (x_temp > INT_MAX) ? INT_MAX : (x_temp < INT_MIN) ? INT_MIN : (int)x_temp;
    vertex->y = (y_temp > INT_MAX) ? INT_MAX : (y_temp < INT_MIN) ? INT_MIN : (int)y_temp;
    vertex->z = (z_temp > INT_MAX) ? INT_MAX : (z_temp < INT_MIN) ? INT_MIN : (int)z_temp;
    vertex->w = (w_temp > INT_MAX) ? INT_MAX : (w_temp < INT_MIN) ? INT_MIN : (int)w_temp;
#endif

    return 1;
}

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
static CC_INLINE void SoftFP_LoadPackedMapAttrs(const PackedTerrainVertex* packed,
                                                 VertexFixed* vertex) {
    vertex->u = ((int)packed->u << 4) + (tex_offseting ? texOffsetX_fp : 0);
    vertex->v = (int)packed->v + (tex_offseting ? texOffsetY_fp : 0);
    vertex->c = SoftFP_UnpackTerrainColor(packed->color);
}

static CC_INLINE void SoftFP_TransformPackedDelta(int dx, int dy, int dz,
    float* outX, float* outY, float* outZ, float* outW) {
    const struct Matrix* m = &mapMvpPacked;
    if (!dy && !dz) {
        *outX = dx * m->row1.x; *outY = dx * m->row1.y;
        *outZ = dx * m->row1.z; *outW = dx * m->row1.w;
    } else if (!dx && !dz) {
        *outX = dy * m->row2.x; *outY = dy * m->row2.y;
        *outZ = dy * m->row2.z; *outW = dy * m->row2.w;
    } else if (!dx && !dy) {
        *outX = dz * m->row3.x; *outY = dz * m->row3.y;
        *outZ = dz * m->row3.z; *outW = dz * m->row3.w;
    } else {
        *outX = dx * m->row1.x + dy * m->row2.x + dz * m->row3.x;
        *outY = dx * m->row1.y + dy * m->row2.y + dz * m->row3.y;
        *outZ = dx * m->row1.z + dy * m->row2.z + dz * m->row3.z;
        *outW = dx * m->row1.w + dy * m->row2.w + dz * m->row3.w;
    }
}

static void TransformMapQuad3D(int firstVertex, VertexFixed vertices[4]) {
    const PackedTerrainVertex* p =
        (const PackedTerrainVertex*)gfx_vertices + firstVertex;
    const struct Matrix* m = &mapMvpPacked;
    float x0 = p[0].x * m->row1.x + p[0].y * m->row2.x +
               p[0].z * m->row3.x + m->row4.x;
    float y0 = p[0].x * m->row1.y + p[0].y * m->row2.y +
               p[0].z * m->row3.y + m->row4.y;
    float z0 = p[0].x * m->row1.z + p[0].y * m->row2.z +
               p[0].z * m->row3.z + m->row4.z;
    float w0 = p[0].x * m->row1.w + p[0].y * m->row2.w +
               p[0].z * m->row3.w + m->row4.w;
    float d1x, d1y, d1z, d1w, d3x, d3y, d3z, d3w;
    cc_bool parallelogram =
        p[2].x == p[1].x + p[3].x - p[0].x &&
        p[2].y == p[1].y + p[3].y - p[0].y &&
        p[2].z == p[1].z + p[3].z - p[0].z;
    int i;

    SoftFP_TransformPackedDelta(p[1].x - p[0].x, p[1].y - p[0].y,
                                p[1].z - p[0].z, &d1x, &d1y, &d1z, &d1w);
    SoftFP_TransformPackedDelta(p[3].x - p[0].x, p[3].y - p[0].y,
                                p[3].z - p[0].z, &d3x, &d3y, &d3z, &d3w);

    vertices[0].x = FloatToFixed(x0);       vertices[0].y = FloatToFixed(y0);
    vertices[0].z = FloatToFixed(z0);       vertices[0].w = FloatToFixed(w0);
    vertices[1].x = FloatToFixed(x0 + d1x); vertices[1].y = FloatToFixed(y0 + d1y);
    vertices[1].z = FloatToFixed(z0 + d1z); vertices[1].w = FloatToFixed(w0 + d1w);
    vertices[3].x = FloatToFixed(x0 + d3x); vertices[3].y = FloatToFixed(y0 + d3y);
    vertices[3].z = FloatToFixed(z0 + d3z); vertices[3].w = FloatToFixed(w0 + d3w);
    if (parallelogram) {
        vertices[2].x = FloatToFixed(x0 + d1x + d3x);
        vertices[2].y = FloatToFixed(y0 + d1y + d3y);
        vertices[2].z = FloatToFixed(z0 + d1z + d3z);
        vertices[2].w = FloatToFixed(w0 + d1w + d3w);
    } else {
        vertices[2].x = FloatToFixed(p[2].x * m->row1.x + p[2].y * m->row2.x +
                                     p[2].z * m->row3.x + m->row4.x);
        vertices[2].y = FloatToFixed(p[2].x * m->row1.y + p[2].y * m->row2.y +
                                     p[2].z * m->row3.y + m->row4.y);
        vertices[2].z = FloatToFixed(p[2].x * m->row1.z + p[2].y * m->row2.z +
                                     p[2].z * m->row3.z + m->row4.z);
        vertices[2].w = FloatToFixed(p[2].x * m->row1.w + p[2].y * m->row2.w +
                                     p[2].z * m->row3.w + m->row4.w);
    }
    for (i = 0; i < 4; i++) SoftFP_LoadPackedMapAttrs(&p[i], &vertices[i]);
}
#endif

static cc_bool ViewportVertex3D(VertexFixed* vertex) {
    if (vertex->w == 0) return false;
    if (ABS(vertex->w) < 64) return false; // Too small w
    
    int invW = FixedReciprocalFast(vertex->w);
    
    if (ABS(invW) > (FP_ONE << 10)) return false; // 1024x magnification limit

    int64_t x_ndc = ((int64_t)vertex->x * invW) >> FP_SHIFT;
    int64_t y_ndc = ((int64_t)vertex->y * invW) >> FP_SHIFT;
    int64_t z_ndc = ((int64_t)vertex->z * invW) >> FP_SHIFT;
    
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
    /* Rasterise only the 3D scene at 192x144 and expand its samples through
     * the precomputed map. Keep 2D/UI native at 320x240. */
    int64_t screen_x = vp_raster_hwidth_fp  + ((x_ndc * vp_raster_hwidth_fp)  >> FP_SHIFT);
    int64_t screen_y = vp_raster_hheight_fp - ((y_ndc * vp_raster_hheight_fp) >> FP_SHIFT);
#else
    int64_t screen_x = vp_hwidth_fp  + ((x_ndc * vp_hwidth_fp)  >> FP_SHIFT);
    int64_t screen_y = vp_hheight_fp - ((y_ndc * vp_hheight_fp) >> FP_SHIFT);
#endif
    
    // Clamp
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
    if (screen_x < -(raster_width  << FP_SHIFT) || screen_x > (raster_width  << (FP_SHIFT + 1))) return false;
    if (screen_y < -(raster_height << FP_SHIFT) || screen_y > (raster_height << (FP_SHIFT + 1))) return false;
#else
    if (screen_x < -(fb_width  << FP_SHIFT) || screen_x > (fb_width  << (FP_SHIFT + 1))) return false;
    if (screen_y < -(fb_height << FP_SHIFT) || screen_y > (fb_height << (FP_SHIFT + 1))) return false;
#endif
    
    vertex->x = (int)screen_x;
    vertex->y = (int)screen_y;
    vertex->z = (z_ndc > INT_MAX) ? INT_MAX : (z_ndc < INT_MIN) ? INT_MIN : (int)z_ndc;
    vertex->w = invW;

#if !defined CC_BUILD_SOFTFP_AFFINE && !defined CC_BUILD_SOFTFP_PERSPECTIVE4
    vertex->u = FixedMul(vertex->u, invW);
    vertex->v = FixedMul(vertex->v, invW);
#endif
    
    return true;
}

static void DrawSprite2D(VertexFixed* V0, VertexFixed* V1, VertexFixed* V2) {
    PackedCol vColor = V0->c;
    int minX = FixedToInt(V0->x);
    int minY = FixedToInt(V0->y);
    int maxX = FixedToInt(V1->x);
    int maxY = FixedToInt(V2->y);

    if (maxX < 0 || minX > fb_maxX) return;
    if (maxY < 0 || minY > fb_maxY) return;

    int begTX = FixedMul(V0->u, IntToFixed(curTexWidth)) >> FP_SHIFT;
    int begTY = FixedMul(V0->v, IntToFixed(curTexHeight)) >> FP_SHIFT;
    int delTX = (FixedMul(V1->u, IntToFixed(curTexWidth)) >> FP_SHIFT) - begTX;
    int delTY = (FixedMul(V2->v, IntToFixed(curTexHeight)) >> FP_SHIFT) - begTY;

    int width = maxX - minX, height = maxY - minY;
    if (width == 0) width = 1;
    if (height == 0) height = 1;

    int fast = delTX == width && delTY == height && 
               (begTX + delTX < curTexWidth) && 
               (begTY + delTY < curTexHeight);
    cc_bool incremental = !fast && delTX >= 0 && delTY >= 0;
    int texXStep = incremental ? delTX / width : 0;
    int texXRem  = incremental ? delTX % width : 0;
    int texYStep = incremental ? delTY / height : 0;
    int texYRem  = incremental ? delTY % height : 0;
    int texYDda = begTY, texYError = 0;

    minX = max(minX, 0); maxX = min(maxX, fb_maxX);
    minY = max(minY, 0); maxY = min(maxY, fb_maxY);

    int x, y;
#if defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
    int tintR = PackedCol_R(vColor);
    int tintG = PackedCol_G(vColor);
    int tintB = PackedCol_B(vColor);
    rasterUiSpritePixels += (cc_uint32)(maxX - minX + 1) *
                            (cc_uint32)(maxY - minY + 1);
    if (fast && begTX >= 0 && begTY >= 0) {
        int span = maxX - minX + 1;
        rasterUiFastSprites++;
        for (y = minY; y <= maxY; y++) {
            const SoftTexel* src = curTexPixels +
                (begTY + y - minY) * curTexWidth + begTX;
            SoftPixel* dst = colorBuffer + y * cb_stride + minX;
            if (vColor == PACKEDCOL_WHITE) {
                for (x = 0; x < span; x++) {
                    SoftTexel texel = src[x];
                    if (!gfx_alphaBlend || (texel & SOFT_TEXEL_A_MASK)) {
                        dst[x] = SoftPixel_FromTexel(texel);
                    }
                }
            } else {
                for (x = 0; x < span; x++) {
                    SoftTexel texel = src[x];
                    if (!gfx_alphaBlend || (texel & SOFT_TEXEL_A_MASK)) {
                        dst[x] = SoftPixel_FromTexelTint(texel, tintR, tintG, tintB);
                    }
                }
            }
        }
        return;
    }
    rasterUiGenericSprites++;
#endif
    for (y = minY; y <= maxY; y++) 
    {
        int texY;
        int texXDda = begTX, texXError = 0;
        if (fast) {
            texY = begTY + (y - minY);
        } else if (incremental) {
            texY = texYDda & texHeightMask;
            texYDda += texYStep;
            texYError += texYRem;
            if (texYError >= height) { texYDda++; texYError -= height; }
        } else {
            texY = (begTY + delTY * (y - minY) / height) & texHeightMask;
        }
        {
        int texRow = texY * curTexWidth;
        for (x = minX; x <= maxX; x++) 
        {
            int texX;
            if (fast) {
                texX = begTX + (x - minX);
            } else if (incremental) {
                texX = texXDda & texWidthMask;
                texXDda += texXStep;
                texXError += texXRem;
                if (texXError >= width) { texXDda++; texXError -= width; }
            } else {
                texX = (begTX + delTX * (x - minX) / width) & texWidthMask;
            }
            int texIndex = texRow + texX;

#if defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
            SoftTexel texel = curTexPixels[texIndex];
            SoftPixel color;
            if (gfx_alphaBlend && !(texel & SOFT_TEXEL_A_MASK)) continue;
            color = vColor == PACKEDCOL_WHITE ? SoftPixel_FromTexel(texel) :
                SoftPixel_FromTexelTint(texel, tintR, tintG, tintB);
            colorBuffer[y * cb_stride + x] = color;
#else
            BitmapCol color = SoftTexel_ToBitmap(curTexPixels[texIndex]);
            int R, G, B, A;

            A = BitmapCol_A(color);
            if (gfx_alphaBlend && A == 0) continue;
            int cb_index = y * cb_stride + x;

            if (gfx_alphaBlend && A < 0xFE) {
                SoftPixel dst = colorBuffer[cb_index];
                int dstR = SoftPixel_R(dst);
                int dstG = SoftPixel_G(dst);
                int dstB = SoftPixel_B(dst);

                R = BitmapCol_R(color);
                G = BitmapCol_G(color);
                B = BitmapCol_B(color);

#ifdef CC_BUILD_SOFTFP_HALF_ALPHA
                R = (R + dstR) >> 1;
                G = (G + dstG) >> 1;
                B = (B + dstB) >> 1;
#else
                R = (R * A + dstR * (255 - A)) >> 8;
                G = (G * A + dstG * (255 - A)) >> 8;
                B = (B * A + dstB * (255 - A)) >> 8;
#endif
                color = BitmapCol_Make(R, G, B, 0xFF);
            }

            if (vColor != PACKEDCOL_WHITE) {
                int r1 = PackedCol_R(vColor), r2 = BitmapCol_R(color);
                R = ( r1 * r2 ) >> 8;
                int g1 = PackedCol_G(vColor), g2 = BitmapCol_G(color);
                G = ( g1 * g2 ) >> 8;
                int b1 = PackedCol_B(vColor), b2 = BitmapCol_B(color);
                B = ( b1 * b2 ) >> 8;

                color = BitmapCol_Make(R, G, B, 0xFF);
            }

            colorBuffer[cb_index] = SoftPixel_FromBitmap(color);
#endif
        }
        }
    }
}

#define edgeFunctionFixed(ax,ay, bx,by, cx,cy) \
    (FixedMul((bx) - (ax), (cy) - (ay)) - FixedMul((by) - (ay), (cx) - (ax)))

/* Near-clipped geometry can extend well outside the viewport before coverage
 * bounds are clamped, overflowing signed 32-bit 16.16 edge products. Retain
 * 1/4096-pixel subpixel precision for ESP32-S3 3D setup so those polygons stay
 * representable. Other backends keep their original 16.16 setup precision. */
#ifdef CC_BUILD_ESP32S3
#define SOFTFP_RASTER_FP_SHIFT 12
#define SOFTFP_RASTER_COORD(value) ((value) >> (FP_SHIFT - SOFTFP_RASTER_FP_SHIFT))
#else
#define SOFTFP_RASTER_FP_SHIFT FP_SHIFT
#define SOFTFP_RASTER_COORD(value) (value)
#endif
#define SOFTFP_RASTER_FP_ONE (1 << SOFTFP_RASTER_FP_SHIFT)

/* Fixed-point clipping can leave adjacent polygons a tiny fraction of a
 * raster pixel apart even when their original world-space edge was shared.
 * Thirty-two barycentric LSBs conservatively cover only those borderline
 * samples (less than 0.1 pixel at this viewport) without visibly growing
 * silhouettes or adding a per-pixel cost. */
#ifdef CC_BUILD_ESP32S3
#define SOFTFP_COVERAGE_GUARD 32
#else
#define SOFTFP_COVERAGE_GUARD 0
#endif

static CC_INLINE int SoftFP_RasterEdge(int ax, int ay, int bx, int by,
                                       int cx, int cy) {
    int64_t cross = ((int64_t)bx - ax) * ((int64_t)cy - ay) -
                    ((int64_t)by - ay) * ((int64_t)cx - ax);

    /* Quantise the complete cross product, not its two products separately.
     * Separate arithmetic shifts can round both directions of a shared edge
     * outwards by one unit, so neither adjacent triangle owns the pixel and
     * the clear colour leaks through as a dotted seam.  A single floor shift
     * keeps pixel-step increments exact and makes reversed shared edges
     * complementary (their quantised values sum to either 0 or -1). */
    return (int)(cross >> SOFTFP_RASTER_FP_SHIFT);
}


#define MultiplyColors(vColor, tColor) \
    a1 = PackedCol_A(vColor); \
    a2 = BitmapCol_A(tColor); \
    A  = ( a1 * a2 ) >> 8;    \
\
    r1 = PackedCol_R(vColor); \
    r2 = BitmapCol_R(tColor); \
    R  = ( r1 * r2 ) >> 8;    \
\
    g1 = PackedCol_G(vColor); \
    g2 = BitmapCol_G(tColor); \
    G  = ( g1 * g2 ) >> 8;    \
\
    b1 = PackedCol_B(vColor); \
    b2 = BitmapCol_B(tColor); \
    B  = ( b1 * b2 ) >> 8;    \

/*
static void DrawTriangle3D(VertexFixed* V0, VertexFixed* V1, VertexFixed* V2) {
    int x0_fp = V0->x, y0_fp = V0->y;
    int x1_fp = V1->x, y1_fp = V1->y;
    int x2_fp = V2->x, y2_fp = V2->y;
    int minX = FixedToInt(min(x0_fp, min(x1_fp, x2_fp)));
    int minY = FixedToInt(min(y0_fp, min(y1_fp, y2_fp)));
    int maxX = FixedToInt(max(x0_fp, max(x1_fp, x2_fp)));
    int maxY = FixedToInt(max(y0_fp, max(y1_fp, y2_fp)));

    if (maxX < 0 || minX > fb_maxX) return;
    if (maxY < 0 || minY > fb_maxY) return;

    minX = max(minX, 0); maxX = min(maxX, fb_maxX);
    minY = max(minY, 0); maxY = min(maxY, fb_maxY);

    int area = edgeFunctionFixed(x0_fp, y0_fp, x1_fp, y1_fp, x2_fp, y2_fp);
    if (area == 0) return;

    int factor = FixedReciprocal(area);

    int w0 = V0->w, w1 = V1->w, w2 = V2->w;
    if (w0 <= 0 && w1 <= 0 && w2 <= 0) return;

    int z0 = V0->z, z1 = V1->z, z2 = V2->z;
    PackedCol color = V0->c;

    int u0 = FixedMul(V0->u, IntToFixed(curTexWidth));
    int v0 = FixedMul(V0->v, IntToFixed(curTexHeight));
    int u1 = FixedMul(V1->u, IntToFixed(curTexWidth));
    int v1 = FixedMul(V1->v, IntToFixed(curTexHeight));
    int u2 = FixedMul(V2->u, IntToFixed(curTexWidth));
    int v2 = FixedMul(V2->v, IntToFixed(curTexHeight));

    int dx01  = y0_fp - y1_fp, dy01 = x1_fp - x0_fp;
    int dx12  = y1_fp - y2_fp, dy12 = x2_fp - x1_fp;
    int dx20  = y2_fp - y0_fp, dy20 = x0_fp - x2_fp;

    int minX_fp = IntToFixed(minX) + FP_HALF;
    int minY_fp = IntToFixed(minY) + FP_HALF;

    int bc0_start = edgeFunctionFixed(x1_fp, y1_fp, x2_fp, y2_fp, minX_fp, minY_fp);
    int bc1_start = edgeFunctionFixed(x2_fp, y2_fp, x0_fp, y0_fp, minX_fp, minY_fp);
    int bc2_start = edgeFunctionFixed(x0_fp, y0_fp, x1_fp, y1_fp, minX_fp, minY_fp);


    int R, G, B, A, x, y;
    int a1, r1, g1, b1;
    int a2, r2, g2, b2;
    cc_bool texturing = gfx_format == VERTEX_FORMAT_TEXTURED;

    if (!texturing) {
        R = PackedCol_R(color);
        G = PackedCol_G(color);
        B = PackedCol_B(color);
        A = PackedCol_A(color);
    } else if (texSinglePixel) {
        int rawY0 = FixedDiv(v0, w0);
        int rawY1 = FixedDiv(v1, w1);
        int rawY = min(rawY0, rawY1);
        int texY = (FixedToInt(rawY) + 1) & texHeightMask;
        MultiplyColors(color, curTexPixels[texY * curTexWidth]);
        texturing = false;
    }

    for (y = minY; y <= maxY; y++, bc0_start += dy12, bc1_start += dy20, bc2_start += dy01) 
    {
        int bc0 = bc0_start;
        int bc1 = bc1_start;
        int bc2 = bc2_start;

        for (x = minX; x <= maxX; x++, bc0 += dx12, bc1 += dx20, bc2 += dx01) 
        {
            int ic0 = FixedMul(bc0, factor);
            int ic1 = FixedMul(bc1, factor);
            int ic2 = FixedMul(bc2, factor);
            if (ic0 < 0 || ic1 < 0 || ic2 < 0) continue;
            int db_index = y * db_stride + x;

            int w_interp = FixedMul(ic0, w0) + FixedMul(ic1, w1) + FixedMul(ic2, w2);
            if (w_interp == 0) continue;
            int w = FixedReciprocal(w_interp);
            int z_interp = FixedMul(ic0, z0) + FixedMul(ic1, z1) + FixedMul(ic2, z2);
            int z = FixedMul(z_interp, w);

            if (depthTest && (z < 0 || z > depthBuffer[db_index])) continue;
            if (!colWrite) {
                if (depthWrite) depthBuffer[db_index] = z;
                continue;
            }

            if (texturing) {
                int u_interp = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
                int v_interp = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);
                int u = FixedMul(u_interp, w);
                int v = FixedMul(v_interp, w);
                
                int texX = FixedToInt(u) & texWidthMask;
                int texY = FixedToInt(v) & texHeightMask;

                int texIndex = texY * curTexWidth + texX;
                BitmapCol tColor = curTexPixels[texIndex];

                MultiplyColors(color, tColor);
            }

            if (gfx_alphaTest && A < 0x80) continue;
            if (depthWrite) depthBuffer[db_index] = z;
            int cb_index = y * cb_stride + x;
            
            if (!gfx_alphaBlend) {
                colorBuffer[cb_index] = BitmapCol_Make(R, G, B, 0xFF);
                continue;
            }

            BitmapCol dst = colorBuffer[cb_index];
            int dstR = BitmapCol_R(dst);
            int dstG = BitmapCol_G(dst);
            int dstB = BitmapCol_B(dst);

            int finR = (R * A + dstR * (255 - A)) >> 8;
            int finG = (G * A + dstG * (255 - A)) >> 8;
            int finB = (B * A + dstB * (255 - A)) >> 8;
            colorBuffer[cb_index] = BitmapCol_Make(finR, finG, finB, 0xFF);
        }
    }
}
*/

static inline void MultiplyColorsInline(PackedCol vColor, SoftTexel tColor,
                                        int *outR, int *outG, int *outB, int *outA) {
    int a1 = PackedCol_A(vColor);
    int r1 = PackedCol_R(vColor);
    int g1 = PackedCol_G(vColor);
    int b1 = PackedCol_B(vColor);

    int a2 = SoftTexel_A(tColor);
    int r2 = SoftTexel_R(tColor);
    int g2 = SoftTexel_G(tColor);
    int b2 = SoftTexel_B(tColor);

    *outA = (a1 * a2) >> 8;
    *outR = (r1 * r2) >> 8;
    *outG = (g1 * g2) >> 8;
    *outB = (b1 * b2) >> 8;
}

static CC_INLINE SoftPixel BlendSoftPixel(int srcR, int srcG, int srcB,
                                          int srcA, SoftPixel dst) {
#ifdef CC_BUILD_SOFTFP_HALF_ALPHA
    SoftPixel src = SoftPixel_Make(srcR, srcG, srcB);
    return (SoftPixel)((src & dst) + (((src ^ dst) & 0xF7DEu) >> 1));
#else
    int outR = (srcR * srcA + SoftPixel_R(dst) * (255 - srcA)) >> 8;
    int outG = (srcG * srcA + SoftPixel_G(dst) * (255 - srcA)) >> 8;
    int outB = (srcB * srcA + SoftPixel_B(dst) * (255 - srcA)) >> 8;
    return SoftPixel_Make(outR, outG, outB);
#endif
}

#ifdef SOFTFP_GROUPED_RASTER
static CC_INLINE void StoreRasterGroup(int rasterX, int rasterY, SoftPixel color) {
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    SOFTFP_COLOR_ROW(rasterY)[rasterX] = color;
#else
    cc_uint32 rowh = raster_to_color_rowh[rasterY];
    cc_uint16 xw = raster_to_color_xw[rasterX];
    SoftPixel* dst = colorBuffer + (rowh & ~RASTER_GROUP_DOUBLE_Y) +
                     (xw & ~RASTER_GROUP_DOUBLE_X);

    /* 192x144 -> 320x240 expands one raster sample to at most 2x2 output
     * pixels. Width/height flags share the two precomputed address loads. */
    dst[0] = color;
    if (xw & RASTER_GROUP_DOUBLE_X) dst[1] = color;
    if (rowh & RASTER_GROUP_DOUBLE_Y) {
        dst += cb_stride;
        dst[0] = color;
        if (xw & RASTER_GROUP_DOUBLE_X) dst[1] = color;
    }
#endif
}

static CC_INLINE void BlendRasterGroup(int rasterX, int rasterY,
    int srcR, int srcG, int srcB, int srcA) {
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    SoftPixel* dst = SOFTFP_COLOR_ROW(rasterY) + rasterX;
    *dst = BlendSoftPixel(srcR, srcG, srcB, srcA, *dst);
#else
    cc_uint32 rowh = raster_to_color_rowh[rasterY];
    cc_uint16 xw = raster_to_color_xw[rasterX];
    SoftPixel* dst = colorBuffer + (rowh & ~RASTER_GROUP_DOUBLE_Y) +
                     (xw & ~RASTER_GROUP_DOUBLE_X);

    dst[0] = BlendSoftPixel(srcR, srcG, srcB, srcA, dst[0]);
    if (xw & RASTER_GROUP_DOUBLE_X) {
        dst[1] = BlendSoftPixel(srcR, srcG, srcB, srcA, dst[1]);
    }
    if (rowh & RASTER_GROUP_DOUBLE_Y) {
        dst += cb_stride;
        dst[0] = BlendSoftPixel(srcR, srcG, srcB, srcA, dst[0]);
        if (xw & RASTER_GROUP_DOUBLE_X) {
            dst[1] = BlendSoftPixel(srcR, srcG, srcB, srcA, dst[1]);
        }
    }
#endif
}
#endif

#ifdef CC_BUILD_SOFTFP_W_DEPTH8
static CC_INLINE DepthValue EncodeWDepth8Scaled(cc_uint32 scaled) {
    cc_uint32 quantised;

    /* Keep one-code-per-step reciprocal precision from infinity through two
     * blocks, then use a five-bit mantissa so nearby geometry does not all
     * collapse into the same Z8 value. Larger inverse-W remains nearer. */
    if (scaled <= 127u) {
        quantised = scaled;
    } else {
        int exponent = 31 - __builtin_clz(scaled);
        cc_uint32 mantissa = (scaled - (1u << exponent)) >> (exponent - 5);
        quantised = 128u + (cc_uint32)(exponent - 7) * 32u + mantissa;
        if (quantised > DEPTH_CLEAR_VALUE) quantised = DEPTH_CLEAR_VALUE;
    }
    return (DepthValue)(DEPTH_CLEAR_VALUE - quantised);
}

static CC_INLINE DepthValue EncodeWDepth8(int inverseW) {
    cc_uint32 scaled = (cc_uint32)inverseW >> 8;
    if (wDepth8EncodeLut && scaled < SOFTFP_W_DEPTH8_LUT_SIZE) {
        return wDepth8EncodeLut[scaled];
    }
    return EncodeWDepth8Scaled(scaled);
}
#endif

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
static CC_NOINLINE void SoftFP_PromoteTerrainCoverage(int coverageX,
                                                       int coverageY) {
    int coverageIndex = coverageY * SOFTFP_COVERAGE_COLS + coverageX;
    int hizX, hizY, coverageBase;
    int scanX, scanY;
    DepthValue maxDepth;

    /* Build the 4x4 leaf once it becomes fully covered. Its maximum stored
     * depth is an upper bound forever: later successful depth writes can only
     * move pixels nearer. */
    maxDepth = 0;
    for (scanY = coverageY << SOFTFP_COVERAGE_SHIFT;
         scanY < (coverageY + 1) << SOFTFP_COVERAGE_SHIFT; scanY++) {
        const DepthValue* row = depthBuffer + scanY * db_stride +
                                (coverageX << SOFTFP_COVERAGE_SHIFT);
        for (scanX = 0; scanX < (1 << SOFTFP_COVERAGE_SHIFT); scanX++) {
            if (row[scanX] > maxDepth) maxDepth = row[scanX];
        }
    }
    terrainHiZ4[coverageIndex] = maxDepth;

    /* Promote four complete leaves into one cheap 8x8 parent. */
    hizX = coverageX >> 1;
    hizY = coverageY >> 1;
    coverageBase = (hizY << 1) * SOFTFP_COVERAGE_COLS + (hizX << 1);
    if (terrainCoverage[coverageBase] != 0xFFFFu ||
        terrainCoverage[coverageBase + 1] != 0xFFFFu ||
        terrainCoverage[coverageBase + SOFTFP_COVERAGE_COLS] != 0xFFFFu ||
        terrainCoverage[coverageBase + SOFTFP_COVERAGE_COLS + 1] != 0xFFFFu) return;

    maxDepth = max(terrainHiZ4[coverageBase], terrainHiZ4[coverageBase + 1]);
    maxDepth = max(maxDepth, terrainHiZ4[coverageBase + SOFTFP_COVERAGE_COLS]);
    maxDepth = max(maxDepth, terrainHiZ4[coverageBase + SOFTFP_COVERAGE_COLS + 1]);
    terrainHiZ[hizY * SOFTFP_HIZ_COLS + hizX] = maxDepth;
}

static CC_INLINE void SoftFP_RecordTerrainCoverageRow(cc_uint16* coverageRow,
                                                       int coverageY, int x,
                                                       cc_uint16 rowBitBase) {
    int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
    cc_uint16 mask = coverageRow[coverageX];
    if (mask == 0xFFFFu) return;
    mask = (cc_uint16)(mask | (cc_uint16)(rowBitBase << (x & 3)));
    coverageRow[coverageX] = mask;
    if (mask == 0xFFFFu) SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
}

static CC_INLINE void SoftFP_RecordTerrainCoverage(int x, int y) {
    int coverageY = y >> SOFTFP_COVERAGE_SHIFT;
    cc_uint16* row = terrainCoverage + coverageY * SOFTFP_COVERAGE_COLS;
    cc_uint16 rowBitBase = (cc_uint16)(1u << ((y & 3) << 2));
    SoftFP_RecordTerrainCoverageRow(row, coverageY, x, rowBitBase);
}

static CC_INLINE cc_bool SoftFP_RectOccludedFrom(
    const DepthValue* parentHiZ, const DepthValue* childHiZ,
    int minX, int minY, int maxX, int maxY, DepthValue nearestDepth) {
    int leafMinX = minX >> SOFTFP_COVERAGE_SHIFT;
    int leafMinY = minY >> SOFTFP_COVERAGE_SHIFT;
    int leafMaxX = maxX >> SOFTFP_COVERAGE_SHIFT;
    int leafMaxY = maxY >> SOFTFP_COVERAGE_SHIFT;
    int tileMinX = minX >> SOFTFP_HIZ_SHIFT;
    int tileMinY = minY >> SOFTFP_HIZ_SHIFT;
    int tileMaxX = maxX >> SOFTFP_HIZ_SHIFT;
    int tileMaxY = maxY >> SOFTFP_HIZ_SHIFT;
    int tileX, tileY;

    for (tileY = tileMinY; tileY <= tileMaxY; tileY++) {
        const DepthValue* parentRow = parentHiZ + tileY * SOFTFP_HIZ_COLS;
        for (tileX = tileMinX; tileX <= tileMaxX; tileX++) {
            int childMinX, childMinY, childMaxX, childMaxY, childX, childY;
            if (parentRow[tileX] < nearestDepth) continue;

            /* A partially covered 8x8 parent may still fully cover the 4x4
             * leaves touched by a small quad. Fall back only for that parent. */
            childMinX = max(leafMinX, tileX << 1);
            childMinY = max(leafMinY, tileY << 1);
            childMaxX = min(leafMaxX, (tileX << 1) + 1);
            childMaxY = min(leafMaxY, (tileY << 1) + 1);
            for (childY = childMinY; childY <= childMaxY; childY++) {
                const DepthValue* childRow = childHiZ + childY * SOFTFP_COVERAGE_COLS;
                for (childX = childMinX; childX <= childMaxX; childX++) {
                    /* Equal Z8 values retain normal draw order for coplanar
                     * surfaces and quantisation ties. */
                    if (childRow[childX] >= nearestDepth) return false;
                }
            }
        }
    }
    return true;
}

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_LOWRES_RASTER
/* Opaque map geometry uses affine texture coordinates and reciprocal-W depth,
 * so its post-clip Z value is never consumed. The full-screen 192x144 viewport
 * also has integer half extents (96,72), which makes the second pair of 16.16
 * multiplies exactly equivalent to cheap 32-bit multiplies. Keep the generic
 * path as a fallback for any unusual viewport. */
static CC_INLINE cc_bool ViewportMapOpaqueVertex3D(VertexFixed* vertex) {
    int invW, xNdc, yNdc, halfW, halfH;
    int64_t xNdc64, yNdc64;

    if (vertex->w == 0 || ABS(vertex->w) < 64) return false;
    if (vp_raster_hwidth_fp != (raster_width << (FP_SHIFT - 1)) ||
        vp_raster_hheight_fp != (raster_height << (FP_SHIFT - 1))) {
        return ViewportVertex3D(vertex);
    }

    invW = FixedReciprocalFast(vertex->w);
    if (ABS(invW) > (FP_ONE << 10)) return false;
    xNdc64 = ((int64_t)vertex->x * invW) >> FP_SHIFT;
    yNdc64 = ((int64_t)vertex->y * invW) >> FP_SHIFT;

    /* These are the exact NDC limits implied by the generic path's
     * [-width, 2*width] guard for a centred full-screen viewport. Besides
     * preserving rejection behaviour, they prove the following products fit
     * signed 32-bit arithmetic. */
    if (xNdc64 < -3LL * FP_ONE || xNdc64 > 3LL * FP_ONE ||
        yNdc64 < -3LL * FP_ONE || yNdc64 > 3LL * FP_ONE) return false;

    xNdc = (int)xNdc64; yNdc = (int)yNdc64;
    halfW = raster_width >> 1; halfH = raster_height >> 1;
    vertex->x = vp_raster_hwidth_fp  + xNdc * halfW;
    vertex->y = vp_raster_hheight_fp - yNdc * halfH;
    vertex->w = invW;
    return true;
}
#endif

static CC_INLINE cc_bool SoftFP_RectOccluded(int minX, int minY, int maxX, int maxY,
                                             DepthValue nearestDepth) {
    return SoftFP_RectOccludedFrom(terrainHiZ, terrainHiZ4,
        minX, minY, maxX, maxY, nearestDepth);
}

static CC_INLINE cc_bool SoftFP_TemporalRectOccluded(cc_uint32 key) {
    SoftFPMapCullData* data = gfx_mapCullData;
    cc_uint32 quadIndex, cacheByte;
    cc_uint8 cacheBit;
    cc_uint32 rect;
    int minX, minY, maxX, maxY;
    if (!key || !data || !data->temporal ||
        data->temporalKey != gfx_mvp_cache_key) return false;
    quadIndex = key - 1u;
    if (quadIndex >= data->quadCount) return false;
    cacheByte = quadIndex >> 3;
    cacheBit = (cc_uint8)(1u << (quadIndex & 7u));
    if (!(SoftFP_TemporalValidBits(data)[cacheByte] & cacheBit)) return false;
    rect = SoftFP_TemporalRects(data)[quadIndex];
    minX = rect & 0xFF; minY = (rect >> 8) & 0xFF;
    maxX = (rect >> 16) & 0xFF; maxY = rect >> 24;
    if (previousTerrainHiZValid &&
        previousTerrainMvpKey == gfx_mvp_cache_key &&
        previousTerrainMapGeneration == gfx_map_generation) {
        cc_bool occluded = SoftFP_RectOccludedFrom(
            previousTerrainHiZ, previousTerrainHiZ4,
            minX, minY, maxX, maxY, SoftFP_TemporalDepths(data)[quadIndex]);
        if (occluded) rasterPreviousHiZCull++;
        return occluded;
    }
    return SoftFP_RectOccluded(minX, minY, maxX, maxY,
        SoftFP_TemporalDepths(data)[quadIndex]);
}

static CC_INLINE void SoftFP_TemporalStoreRect(cc_uint32 key,
                                                int minX, int minY, int maxX, int maxY,
                                                DepthValue nearestDepth) {
    SoftFPMapCullData* data = gfx_mapCullData;
    cc_uint32 quadIndex, cacheByte;
    cc_uint8 cacheBit;
    if (!key || !data || !data->temporal ||
        data->temporalKey != gfx_mvp_cache_key) return;
    quadIndex = key - 1u;
    if (quadIndex >= data->quadCount) return;
    cacheByte = quadIndex >> 3;
    cacheBit = (cc_uint8)(1u << (quadIndex & 7u));
    SoftFP_TemporalRects(data)[quadIndex] =
        (cc_uint32)minX | ((cc_uint32)minY << 8) |
        ((cc_uint32)maxX << 16) | ((cc_uint32)maxY << 24);
    SoftFP_TemporalDepths(data)[quadIndex] = nearestDepth;
    SoftFP_TemporalValidBits(data)[cacheByte] |= cacheBit;
}

static CC_INLINE cc_bool SoftFP_TerrainQuadOccluded(const VertexFixed quad[4]) {
    int minX = FixedToInt(min(min(quad[0].x, quad[1].x), min(quad[2].x, quad[3].x)));
    int minY = FixedToInt(min(min(quad[0].y, quad[1].y), min(quad[2].y, quad[3].y)));
    int maxX = FixedToInt(max(max(quad[0].x, quad[1].x), max(quad[2].x, quad[3].x)));
    int maxY = FixedToInt(max(max(quad[0].y, quad[1].y), max(quad[2].y, quad[3].y)));
    int rasterMaxX = (fb_maxX * raster_width) / fb_width;
    int rasterMaxY = (fb_maxY * raster_height) / fb_height;
    int nearestW = max(max(quad[0].w, quad[1].w), max(quad[2].w, quad[3].w));
    DepthValue nearestDepth;

    if (nearestW <= 0 || maxX < 0 || minX > rasterMaxX ||
        maxY < 0 || minY > rasterMaxY) return false;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    nearestDepth = EncodeWDepth8(nearestW);
    if (!nearestDepth) return false;
    SoftFP_TemporalStoreRect(gfx_temporal_quad_key,
                             minX, minY, maxX, maxY, nearestDepth);
    return SoftFP_RectOccluded(minX, minY, maxX, maxY, nearestDepth);
}
#endif

#if defined SOFTFP_FAST_TERRAIN_KERNEL && defined CC_BUILD_SOFTFP_FLOAT_XFORM
static cc_bool SoftFP_LocalBoundsOccluded(const SoftFPMapQuadBounds* bounds) {
    const struct Matrix* m = &_mvp_float;
    float xs[2], ys[2], zs[2];
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    int nearestW = 0;
    int rasterMaxX = (fb_maxX * raster_width) / fb_width;
    int rasterMaxY = (fb_maxY * raster_height) / fb_height;
    int nx, ny, nz, ix, iy, iz;
    DepthValue nearestDepth;

    xs[0] = gfx_map_origin_x + bounds->minX * 0.125f;
    xs[1] = gfx_map_origin_x + bounds->maxX * 0.125f;
    ys[0] = gfx_map_origin_y + bounds->minY * 0.125f;
    ys[1] = gfx_map_origin_y + bounds->maxY * 0.125f;
    zs[0] = gfx_map_origin_z + bounds->minZ * 0.125f;
    zs[1] = gfx_map_origin_z + bounds->maxZ * 0.125f;
    nx = bounds->minX == bounds->maxX ? 1 : 2;
    ny = bounds->minY == bounds->maxY ? 1 : 2;
    nz = bounds->minZ == bounds->maxZ ? 1 : 2;

    for (iz = 0; iz < nz; iz++) {
        for (iy = 0; iy < ny; iy++) {
            for (ix = 0; ix < nx; ix++) {
                float x = xs[ix], y = ys[iy], z = zs[iz];
                float clipX = x * m->row1.x + y * m->row2.x + z * m->row3.x + m->row4.x;
                float clipY = x * m->row1.y + y * m->row2.y + z * m->row3.y + m->row4.y;
                float clipZ = x * m->row1.z + y * m->row2.z + z * m->row3.z + m->row4.z;
                float clipW = x * m->row1.w + y * m->row2.w + z * m->row3.w + m->row4.w;
                int w, invW, xNdc, yNdc, screenX, screenY;

                /* Never query a bound which intersects the near plane. */
                if (clipZ < 0.0f || clipW <= 0.0f) return false;
                w = FloatToFixed(clipW);
                if (w < 64) return false;
                invW = FixedReciprocalFast(w);
                if (invW <= 0 || invW > (FP_ONE << 10)) return false;
                xNdc = FixedMul(FloatToFixed(clipX), invW);
                yNdc = FixedMul(FloatToFixed(clipY), invW);
                screenX = vp_raster_hwidth_fp + FixedMul(xNdc, vp_raster_hwidth_fp);
                screenY = vp_raster_hheight_fp - FixedMul(yNdc, vp_raster_hheight_fp);
                screenX = FixedToInt(screenX); screenY = FixedToInt(screenY);
                minX = min(minX, screenX); maxX = max(maxX, screenX);
                minY = min(minY, screenY); maxY = max(maxY, screenY);
                nearestW = max(nearestW, invW);
            }
        }
    }

    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return false;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    nearestDepth = EncodeWDepth8(nearestW);
    if (!nearestDepth) return false;
    if (previousTerrainHiZValid &&
        previousTerrainMvpKey == gfx_mvp_cache_key &&
        previousTerrainMapGeneration == gfx_map_generation) {
        cc_bool occluded = SoftFP_RectOccludedFrom(
            previousTerrainHiZ, previousTerrainHiZ4,
            minX, minY, maxX, maxY, nearestDepth);
        if (occluded) rasterPreviousBoundsCull++;
        return occluded;
    }
    return SoftFP_RectOccluded(minX, minY, maxX, maxY, nearestDepth);
}

#endif

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
cc_bool Gfx_IsBoundMapOccluded(void) {
#if defined SOFTFP_FAST_TERRAIN_KERNEL && defined CC_BUILD_SOFTFP_FLOAT_XFORM
    if (!gfx_mapCullData || !depthTest) return false;
    if (!SoftFP_LocalBoundsOccluded(&gfx_mapCullData->aggregate)) return false;
    return true;
#else
    return false;
#endif
}

cc_bool Gfx_IsBoundMapRangeOccluded(int firstVertex, int verticesCount) {
#if defined SOFTFP_FAST_TERRAIN_KERNEL && defined CC_BUILD_SOFTFP_FLOAT_XFORM
    SoftFPMapQuadBounds aggregate;
    const SoftFPMapQuadBounds* queryBounds = &aggregate;
    SoftFPMapRangeCache* cache = NULL;
    cc_uint32 firstQuad, quadCount, endQuad, i;
    if (!gfx_mapCullData || !gfx_mapCullBounds || !depthTest || verticesCount <= 0) return false;
    firstQuad = (cc_uint32)firstVertex >> 2;
    quadCount = (cc_uint32)verticesCount >> 2;
    endQuad = firstQuad + quadCount;
    if (!quadCount || endQuad > gfx_mapCullData->quadCount) return false;

    if (firstQuad <= 0xFFFFu && quadCount <= 0xFFFFu) {
        for (i = 0; i < SOFTFP_RANGE_CACHE_SIZE; i++) {
            SoftFPMapRangeCache* entry = &gfx_mapCullData->ranges[i];
            if (entry->quadCount == quadCount && entry->firstQuad == firstQuad) {
                cache = entry;
                queryBounds = &entry->aggregate;
                rasterRangeCacheHits++;
                break;
            }
        }
    }

    if (!cache) {
        aggregate = gfx_mapCullBounds[firstQuad];
        for (i = firstQuad + 1; i < endQuad; i++) {
            const SoftFPMapQuadBounds* bounds = &gfx_mapCullBounds[i];
            aggregate.minX = min(aggregate.minX, bounds->minX);
            aggregate.minY = min(aggregate.minY, bounds->minY);
            aggregate.minZ = min(aggregate.minZ, bounds->minZ);
            aggregate.maxX = max(aggregate.maxX, bounds->maxX);
            aggregate.maxY = max(aggregate.maxY, bounds->maxY);
            aggregate.maxZ = max(aggregate.maxZ, bounds->maxZ);
        }
        rasterRangeCacheMisses++;
        if (firstQuad <= 0xFFFFu && quadCount <= 0xFFFFu) {
            cache = &gfx_mapCullData->ranges[
                gfx_mapCullData->rangeNext++ & (SOFTFP_RANGE_CACHE_SIZE - 1u)];
            cache->firstQuad = (cc_uint16)firstQuad;
            cache->quadCount = (cc_uint16)quadCount;
            cache->aggregate = aggregate;
            queryBounds = &cache->aggregate;
        }
    }

    if (!SoftFP_LocalBoundsOccluded(queryBounds)) return false;
    return true;
#else
    (void)firstVertex; (void)verticesCount;
    return false;
#endif
}
#endif

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
static CC_INLINE int SoftFP_SelectPerspectiveShift(const VertexFixed* v0,
                                                   const VertexFixed* v1,
                                                   const VertexFixed* v2,
                                                   int factor, int minW) {
    int y0 = SOFTFP_RASTER_COORD(v0->y);
    int y1 = SOFTFP_RASTER_COORD(v1->y);
    int y2 = SOFTFP_RASTER_COORD(v2->y);
    int dx01 = y0 - y1;
    int dx12 = y1 - y2;
    int dx20 = y2 - y0;
    int stepIc0 = FixedMul(dx12, factor);
    int stepIc1 = FixedMul(dx20, factor);
    int stepIc2 = FixedMul(dx01, factor);
    int stepW = FixedMul(stepIc0, v0->w) +
                FixedMul(stepIc1, v1->w) + FixedMul(stepIc2, v2->w);
    cc_uint64 delta = stepW < 0 ? (cc_uint64)(-(int64_t)stepW) : (cc_uint64)stepW;

    /* Keep the maximum reciprocal-W change across a correction interval
     * below a conservative fraction of the polygon's farthest reciprocal-W.
     * Returning a shift keeps all intervals power-of-two and screen aligned. */
    if (minW > 0 && delta * 32u <= ((cc_uint64)(cc_uint32)minW >> 7)) return 5;
    if (minW > 0 && delta * 16u <= ((cc_uint64)(cc_uint32)minW >> 7)) return 4;
    if (minW > 0 && delta *  8u <= ((cc_uint64)(cc_uint32)minW >> 5)) return 3;
    return 2;
}

#ifdef CC_BUILD_ESP32S3
static CC_INLINE int SoftFP_SelectTerrainPerspectiveShift(const VertexFixed* verts,
                                                          int count) {
    int minX = verts[0].x, maxX = verts[0].x;
    int minY = verts[0].y, maxY = verts[0].y;
    int minW = verts[0].w, maxW = verts[0].w;
    int i, width, height;

    for (i = 1; i < count; i++) {
        minX = min(minX, verts[i].x); maxX = max(maxX, verts[i].x);
        minY = min(minY, verts[i].y); maxY = max(maxY, verts[i].y);
        minW = min(minW, verts[i].w); maxW = max(maxW, verts[i].w);
    }
    width  = FixedToInt(maxX) - FixedToInt(minX) + 1;
    height = FixedToInt(maxY) - FixedToInt(minY) + 1;

    /* Only large, strongly oblique faces expose the affine triangle seam or
     * visibly jump between texels during a tiny camera movement. Keep every
     * other terrain face on the branch-free affine kernel. Eight-pixel,
     * screen-aligned correction spans preserve full-rate texel sampling while
     * adding only two reciprocal evaluations per corrected span. */
    if (minW <= 0 || width < 20 || height < 10) return -1;
    if (width * height < 320) return -1;
    if (maxW - minW <= (maxW >> 2)) return -1;
    return 3;
}
#endif
#endif

static CC_INLINE cc_bool ClipSpanEdge(int value, int step, int* lower, int* upper) {
    if (step > 0) {
        if (value < 0) {
            cc_uint32 needed = 0u - (cc_uint32)value;
            cc_uint32 offset = 1u + (needed - 1u) / (cc_uint32)step;
            if (offset > (cc_uint32)*upper) return false;
            if ((int)offset > *lower) *lower = (int)offset;
        }
    } else if (step < 0) {
        cc_uint32 divisor;
        cc_uint32 offset;
        if (value < 0) return false;
        divisor = 0u - (cc_uint32)step;
        offset  = (cc_uint32)value / divisor;
        if (offset < (cc_uint32)*lower) return false;
        if ((int)offset < *upper) *upper = (int)offset;
    } else if (value < 0) {
        return false;
    }
    return *lower <= *upper;
}

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
#define SOFTFP_MAP_MODE_NONE        0
#define SOFTFP_MAP_MODE_OPAQUE      1
#define SOFTFP_MAP_MODE_DEPTH_ONLY  2
#define SOFTFP_MAP_MODE_HALF_BLEND  3

static CC_INLINE cc_bool SoftFP_FastTerrainState(void) {
    return gfx_format == VERTEX_FORMAT_TEXTURED && !texSinglePixel &&
           colWrite && depthTest && depthWrite && gfx_alphaTest &&
           !gfx_alphaBlend;
}

static CC_INLINE int SoftFP_FastMapMode(void) {
    if (SoftFP_FastTerrainState()) return SOFTFP_MAP_MODE_OPAQUE;
    if (gfx_format != VERTEX_FORMAT_TEXTURED || texSinglePixel || !depthTest) {
        return SOFTFP_MAP_MODE_NONE;
    }
    if (!colWrite && depthWrite && !gfx_alphaBlend) {
        return SOFTFP_MAP_MODE_DEPTH_ONLY;
    }
#if defined CC_BUILD_SOFTFP_HALF_ALPHA && defined CC_BUILD_SOFTFP_FB16
    if (colWrite && gfx_alphaBlend && !gfx_alphaTest) {
        return SOFTFP_MAP_MODE_HALF_BLEND;
    }
#endif
    return SOFTFP_MAP_MODE_NONE;
}

#ifdef CC_BUILD_ESP32S3
static CC_NOINLINE void RasterizeTerrainOpaqueLutDirect(TerrainRasterScratch* scratch) {
    cc_uint32 kernelStart = SoftFP_ReadCycles();
    const DepthValue* depthLut = wDepth8EncodeLut;
    const SoftPixel* shaded = (const SoftPixel*)scratch->samplePixels;
    int rowW = scratch->rowW, rowU = scratch->rowU, rowV = scratch->rowV;
    cc_uint32 spanPixelsLocal = 0;
    cc_uint32 cellSkipPixelsLocal = 0;
    int i, x, y;

    for (y = scratch->minY; y <= scratch->maxY; y++) {
        int spanLower = 0, spanUpper = scratch->maxX - scratch->minX;
        cc_bool rowVisible = true;
        for (i = 0; i < scratch->vertexCount; i++) {
            if (!ClipSpanEdge(scratch->edgeStart[i], scratch->edgeStepX[i],
                              &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int spanMinX = scratch->minX + spanLower;
            int spanMaxX = scratch->minX + spanUpper;
            int wInterp = rowW + scratch->stepW * spanLower;
            int uInterp = rowU + scratch->stepU * spanLower;
            int vInterp = rowV + scratch->stepV * spanLower;
            int coverageY = y >> SOFTFP_COVERAGE_SHIFT;
            cc_uint16* coverageRow = terrainCoverage +
                coverageY * SOFTFP_COVERAGE_COLS;
            cc_uint16 coverageBitBase =
                (cc_uint16)(1u << ((y & 3) << 2));
            DepthValue* depth = depthBuffer + y * db_stride + spanMinX;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
            SoftPixel* colorDst = SOFTFP_COLOR_ROW(y) + spanMinX;
#endif
            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
            x = spanMinX;
#ifdef CC_BUILD_SOFTFP_FOG4
            if (!scratch->fogLevel) {
#else
            {
#endif
                while (x <= spanMaxX) {
                    int cellEnd = min(spanMaxX, x | 3);
                    int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
                    cc_uint16 coverageOld = coverageRow[coverageX];
                    cc_uint16 coverageMask = coverageOld;

                    /* A partially visible quad can cross small spans already
                     * hidden by nearer terrain. A promoted 4x4 value is a
                     * conservative maximum depth, so this exact linear-W
                     * endpoint test can reject the complete span safely. */
                    if (coverageOld == 0xFFFFu && wInterp > 0) {
                        int pixels = cellEnd - x + 1;
                        int wEnd = wInterp + scratch->stepW * (pixels - 1);
                        if (wEnd > 0) {
                            int nearestW = max(wInterp, wEnd);
                            DepthValue nearest = depthLut[(cc_uint32)nearestW >> 8];
                            DepthValue covered = terrainHiZ4[
                                coverageY * SOFTFP_COVERAGE_COLS + coverageX];
                            if (covered <= nearest) {
                                x       += pixels;
                                depth   += pixels;
                                wInterp += scratch->stepW * pixels;
                                uInterp += scratch->stepU * pixels;
                                vInterp += scratch->stepV * pixels;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                                colorDst += pixels;
#endif
                                cellSkipPixelsLocal += (cc_uint32)pixels;
                                continue;
                            }
                        }
                    }
                    for (; x <= cellEnd; x++, depth++) {
                        DepthValue z;
                        int texIndex;
                        if (wInterp <= 0) goto next_opaque_lut_near_pixel;
                        z = depthLut[(cc_uint32)wInterp >> 8];
                        if (z >= *depth) goto next_opaque_lut_near_pixel;
                        texIndex = ((FixedToInt(vInterp) & (TERRAIN_TILE_SIZE - 1)) << 4) |
                                   (FixedToInt(uInterp) & (TERRAIN_TILE_SIZE - 1));
                        *depth = z;
                        if (coverageMask != 0xFFFFu) {
                            coverageMask = (cc_uint16)(coverageMask |
                                (cc_uint16)(coverageBitBase << (x & 3)));
                        }
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        *colorDst = shaded[texIndex];
#else
                        StoreRasterGroup(x, y, shaded[texIndex]);
#endif

next_opaque_lut_near_pixel:
                        wInterp += scratch->stepW;
                        uInterp += scratch->stepU;
                        vInterp += scratch->stepV;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        colorDst++;
#endif
                    }
                    if (coverageMask != coverageOld) {
                        coverageRow[coverageX] = coverageMask;
                        if (coverageMask == 0xFFFFu) {
                            SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
                        }
                    }
                }
#ifdef CC_BUILD_SOFTFP_FOG4
            } else {
            while (x <= spanMaxX) {
                int cellEnd = min(spanMaxX, x | 3);
                int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
                cc_uint16 coverageOld = coverageRow[coverageX];
                cc_uint16 coverageMask = coverageOld;
                for (; x <= cellEnd; x++, depth++) {
                    DepthValue z;
                    SoftPixel color;
                    if (wInterp <= 0) goto next_opaque_lut_pixel;
                    z = depthLut[(cc_uint32)wInterp >> 8];
                    if (z >= *depth) goto next_opaque_lut_pixel;
                    int texIndex = ((FixedToInt(vInterp) & (TERRAIN_TILE_SIZE - 1)) << 4) |
                                   (FixedToInt(uInterp) & (TERRAIN_TILE_SIZE - 1));
                    color = SoftFP_ApplyTerrainFogLevel(
                        shaded[texIndex], scratch->fogLevel);
                    *depth = z;
                    if (coverageMask != 0xFFFFu) {
                        coverageMask = (cc_uint16)(coverageMask |
                            (cc_uint16)(coverageBitBase << (x & 3)));
                    }
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                    *colorDst = color;
#else
                    StoreRasterGroup(x, y, color);
#endif

next_opaque_lut_pixel:
                    wInterp += scratch->stepW;
                    uInterp += scratch->stepU;
                    vInterp += scratch->stepV;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                    colorDst++;
#endif
                }
                if (coverageMask != coverageOld) {
                    coverageRow[coverageX] = coverageMask;
                    if (coverageMask == 0xFFFFu) {
                        SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
                    }
                }
            }
#endif
            }
        }
        rowW += scratch->rowStepW;
        rowU += scratch->rowStepU;
        rowV += scratch->rowStepV;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i];
        }
    }
    __atomic_fetch_add(&rasterSpanPixels, spanPixelsLocal, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rasterHiZCellSkipPixels, cellSkipPixelsLocal, __ATOMIC_RELAXED);
    rasterMapRasterCycles += SoftFP_ReadCycles() - kernelStart;
}

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
static void SoftFP_RasterWorkerRun(TerrainRasterScratch* scratch) {
    if (scratch->workerMode == SOFTFP_RASTER_JOB_PERSPECTIVE) {
        RasterizeTerrainPerspectiveDirect(scratch);
    } else if (scratch->workerMode == SOFTFP_RASTER_JOB_AFFINE) {
        RasterizeTerrainAffineDirect(scratch);
    } else if (scratch->workerMode == SOFTFP_RASTER_JOB_TRANSLUCENT) {
        RasterizeTerrainHalfBlendDirect(scratch);
    } else {
        RasterizeTerrainOpaqueLutDirect(scratch);
    }
}

static cc_bool SoftFP_RasterWorkerWaitUntil(cc_uint32 target) {
    const module_host_api_v2* host = holo_port_host();
    while (__atomic_load_n(&softfpRasterCompletedSeq, __ATOMIC_ACQUIRE) < target) {
        cc_uint32 waitStart = SoftFP_ReadCycles();
        int32_t result;
        if (!host || !host->sync.take || !softfpRasterIdle) return false;
        result = host->sync.take(softfpRasterIdle, MODULE_WAIT_FOREVER);
        {
            cc_uint32 elapsed = SoftFP_ReadCycles() - waitStart;
            rasterWorkerWaitCycles += elapsed;
            rasterWorkerFlushWaitCycles += elapsed;
        }
        if (result != MODULE_OK) return false;
    }
    return true;
}

static cc_bool SoftFP_RasterWorkerSubmitPending(void) {
    const module_host_api_v2* host = holo_port_host();
    unsigned index = softfpRasterFillIndex;
    unsigned count = softfpRasterFillCount[index];
    cc_uint32 previousTarget;
    unsigned i;
    if (!softfpRasterFillClaimed || !count) return true;

    if (!softfpRasterTask || !host || !host->sync.take || !host->sync.give) {
        for (i = 0; i < count; i++) {
            SoftFP_RasterWorkerRun(&softfpRasterJobs[index][i]);
        }
        __atomic_store_n(&softfpRasterCacheRefs[index], 0u, __ATOMIC_RELEASE);
        softfpRasterFallbacksFrame += count;
        softfpRasterFillCount[index] = 0;
        softfpRasterFillClaimed = 0;
        return true;
    }

    /* Publish complete command buffers into an ordered SPSC ring. Core 1 can
     * continue preparing later batches while Core 0 drains earlier ones; the
     * worker still consumes every job in original draw order. */
    previousTarget = softfpRasterWriteSeq;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&softfpRasterWriteSeq, previousTarget + 1u, __ATOMIC_RELEASE);
    if (host->sync.give(softfpRasterReady) != MODULE_OK) {
        __atomic_store_n(&softfpRasterWriteSeq, previousTarget, __ATOMIC_RELEASE);
        if (!SoftFP_RasterWorkerWaitUntil(previousTarget)) return false;
        for (i = 0; i < count; i++) {
            SoftFP_RasterWorkerRun(&softfpRasterJobs[index][i]);
        }
        __atomic_store_n(&softfpRasterCacheRefs[index], 0u, __ATOMIC_RELEASE);
        softfpRasterFallbacksFrame += count;
        softfpRasterFillCount[index] = 0;
        softfpRasterFillClaimed = 0;
        (void)host->sync.give(softfpRasterFree);
        return true;
    }

    softfpRasterJobsFrame += count;
    softfpRasterBatchesFrame++;
    softfpRasterFillClaimed = 0;
    return true;
}

static void SoftFP_RasterWorkerFlush(void) {
    (void)SoftFP_RasterWorkerSubmitPending();
    if (!softfpRasterTask) return;
    if (__atomic_load_n(&softfpRasterCompletedSeq, __ATOMIC_ACQUIRE) >=
        __atomic_load_n(&softfpRasterWriteSeq, __ATOMIC_ACQUIRE)) return;
    softfpRasterFlushesFrame++;
    (void)SoftFP_RasterWorkerWaitUntil(
        __atomic_load_n(&softfpRasterWriteSeq, __ATOMIC_ACQUIRE));
}

void Gfx_FlushRasterWorker(void) { SoftFP_RasterWorkerFlush(); }

static void SoftFP_RasterWorkerTask(void* arg) {
    const module_host_api_v2* host = holo_port_host();
    (void)arg;
    while (host && host->sync.take &&
           host->sync.take(softfpRasterReady, MODULE_WAIT_FOREVER) == MODULE_OK) {
        cc_uint32 completed;
        unsigned index, count, i;
        if (softfpRasterStop) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        completed = __atomic_load_n(&softfpRasterCompletedSeq, __ATOMIC_RELAXED);
        index = completed & (SOFTFP_RASTER_BATCH_BUFFERS - 1u);
        count = softfpRasterFillCount[index];
        for (i = 0; i < count; i++) {
            SoftFP_RasterWorkerRun(&softfpRasterJobs[index][i]);
        }
        __atomic_store_n(&softfpRasterCacheRefs[index], 0u, __ATOMIC_RELEASE);
        softfpRasterFillCount[index] = 0;
        __atomic_store_n(&softfpRasterCompletedSeq, completed + 1u, __ATOMIC_RELEASE);
        (void)host->sync.give(softfpRasterFree);
        (void)host->sync.give(softfpRasterIdle);
    }
    softfpRasterTask = NULL;
    if (host && host->sync.give && softfpRasterStopped) {
        (void)host->sync.give(softfpRasterStopped);
    }
    if (host && host->task.remove) host->task.remove(NULL);
    for (;;) {
        if (host && host->task.delay) host->task.delay(1000);
    }
}

static cc_bool SoftFP_RasterWorkerStart(void) {
    const module_host_api_v2* host = holo_port_host();
    void* task = NULL;
    int32_t result;
    if (softfpRasterTask) return true;
    if (!host || !host->sync.create_counting || !host->sync.take ||
        !host->sync.give || !host->sync.destroy || !host->task.create_ex) return false;
    if (host->sync.create_counting(1, 0, &softfpRasterIdle) != MODULE_OK) return false;
    if (host->sync.create_counting(SOFTFP_RASTER_BATCH_BUFFERS, 0,
                                   &softfpRasterReady) != MODULE_OK) goto fail;
    if (host->sync.create_counting(SOFTFP_RASTER_BATCH_BUFFERS,
                                   SOFTFP_RASTER_BATCH_BUFFERS,
                                   &softfpRasterFree) != MODULE_OK) goto fail;
    if (host->sync.create_counting(1, 0, &softfpRasterStopped) != MODULE_OK) goto fail;
    softfpRasterFillIndex = 0;
    softfpRasterFillClaimed = 0;
    Mem_Set(softfpRasterFillCount, 0, sizeof(softfpRasterFillCount));
    Mem_Set(softfpRasterCacheRefs, 0, sizeof(softfpRasterCacheRefs));
    __atomic_store_n(&softfpRasterWriteSeq, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&softfpRasterCompletedSeq, 0u, __ATOMIC_RELEASE);
    softfpRasterStop = 0;
    result = host->task.create_ex("cc_raster", SoftFP_RasterWorkerTask, NULL,
                                 SOFTFP_RASTER_STACK_BYTES, 4u, 0,
                                 MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT, &task);
    if (result != MODULE_OK || !task) goto fail;
    softfpRasterTask = task;
    Platform_LogConst("[cc.raster] Core0 ordered ring=4 batch32 prio4 stack=6144 PSRAM exact-Z8");
    return true;

fail:
    if (host->sync.destroy) {
        if (softfpRasterStopped) host->sync.destroy(softfpRasterStopped);
        if (softfpRasterFree)    host->sync.destroy(softfpRasterFree);
        if (softfpRasterReady)   host->sync.destroy(softfpRasterReady);
        if (softfpRasterIdle)    host->sync.destroy(softfpRasterIdle);
    }
    softfpRasterStopped = softfpRasterFree = softfpRasterReady = softfpRasterIdle = NULL;
    Platform_LogConst("[cc.raster] Core0 worker unavailable; single-core fallback");
    return false;
}

static void SoftFP_RasterWorkerStop(void) {
    const module_host_api_v2* host = holo_port_host();
    if (!host) return;
    if (softfpRasterTask) {
        SoftFP_RasterWorkerFlush();
        softfpRasterStop = 1;
        if (host->sync.give) (void)host->sync.give(softfpRasterReady);
        if (host->sync.take && softfpRasterStopped) {
            (void)host->sync.take(softfpRasterStopped, 2000);
        }
    }
    softfpRasterTask = NULL;
    if (host->sync.destroy) {
        if (softfpRasterStopped) host->sync.destroy(softfpRasterStopped);
        if (softfpRasterFree)    host->sync.destroy(softfpRasterFree);
        if (softfpRasterReady)   host->sync.destroy(softfpRasterReady);
        if (softfpRasterIdle)    host->sync.destroy(softfpRasterIdle);
    }
    softfpRasterStopped = softfpRasterFree = softfpRasterReady = softfpRasterIdle = NULL;
    softfpRasterStop = 0;
    softfpRasterFillIndex = 0;
    softfpRasterFillClaimed = 0;
    Mem_Set(softfpRasterFillCount, 0, sizeof(softfpRasterFillCount));
    Mem_Set(softfpRasterCacheRefs, 0, sizeof(softfpRasterCacheRefs));
    __atomic_store_n(&softfpRasterWriteSeq, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&softfpRasterCompletedSeq, 0u, __ATOMIC_RELEASE);
}

static cc_bool SoftFP_RasterWorkerQueue(const TerrainRasterScratch* scratch) {
    const module_host_api_v2* host = holo_port_host();
    TerrainRasterScratch* job;
    int cacheSlot;
    unsigned index, count;
    if (!softfpRasterTask || !host || !host->sync.take || !softfpRasterFree) return false;
claim_buffer:
    if (!softfpRasterFillClaimed) {
        cc_uint32 waitStart = SoftFP_ReadCycles();
        int32_t waitResult = host->sync.take(softfpRasterFree, MODULE_WAIT_FOREVER);
        {
            cc_uint32 elapsed = SoftFP_ReadCycles() - waitStart;
            rasterWorkerWaitCycles += elapsed;
            rasterWorkerFreeWaitCycles += elapsed;
        }
        if (waitResult != MODULE_OK) return false;
        index = __atomic_load_n(&softfpRasterWriteSeq, __ATOMIC_ACQUIRE) &
                (SOFTFP_RASTER_BATCH_BUFFERS - 1u);
        softfpRasterFillIndex = (cc_uint8)index;
        softfpRasterFillCount[index] = 0;
        __atomic_store_n(&softfpRasterCacheRefs[index], 0u, __ATOMIC_RELEASE);
        softfpRasterFillClaimed = 1;
    }
    index = softfpRasterFillIndex;
    count = softfpRasterFillCount[index];
    if (count >= SOFTFP_RASTER_BATCH_JOBS) {
        if (!SoftFP_RasterWorkerSubmitPending()) return false;
        goto claim_buffer;
    }
    job = &softfpRasterJobs[index][count];
    *job = *scratch;
    cacheSlot = TerrainTileCache_SlotForPixels(job->samplePixels);
    if (cacheSlot >= 0) {
        __atomic_fetch_or(&softfpRasterCacheRefs[index], 1u << cacheSlot, __ATOMIC_ACQ_REL);
    }
    softfpRasterFillCount[index] = (cc_uint8)(count + 1u);
    if (count + 1u == SOFTFP_RASTER_BATCH_JOBS) {
        return SoftFP_RasterWorkerSubmitPending();
    }
    return true;
}

static CC_NOINLINE void RasterizeTerrainOpaqueLut(TerrainRasterScratch* scratch) {
    scratch->workerMode = SOFTFP_RASTER_JOB_OPAQUE;
    if (!softfpRasterTask) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainOpaqueLutDirect(scratch);
        return;
    }
    if (!SoftFP_RasterWorkerQueue(scratch)) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainOpaqueLutDirect(scratch);
    }
}
#else
void Gfx_FlushRasterWorker(void) { }
#define RasterizeTerrainOpaqueLut RasterizeTerrainOpaqueLutDirect
#endif

static CC_NOINLINE void RasterizeTerrainAffineDirect(TerrainRasterScratch* scratch) {
    int rowW = scratch->rowW, rowU = scratch->rowU, rowV = scratch->rowV;
    cc_uint32 spanPixelsLocal = 0;
    int i, x, y;

    for (y = scratch->minY; y <= scratch->maxY; y++) {
        int spanLower = 0, spanUpper = scratch->maxX - scratch->minX;
        cc_bool rowVisible = true;
        for (i = 0; i < scratch->vertexCount; i++) {
            if (!ClipSpanEdge(scratch->edgeStart[i], scratch->edgeStepX[i],
                              &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int spanMinX = scratch->minX + spanLower;
            int spanMaxX = scratch->minX + spanUpper;
            int wInterp = rowW + scratch->stepW * spanLower;
            int uInterp = rowU + scratch->stepU * spanLower;
            int vInterp = rowV + scratch->stepV * spanLower;
            int coverageY = y >> SOFTFP_COVERAGE_SHIFT;
            cc_uint16* coverageRow = terrainCoverage +
                coverageY * SOFTFP_COVERAGE_COLS;
            cc_uint16 coverageBitBase =
                (cc_uint16)(1u << ((y & 3) << 2));
            DepthValue* depth = depthBuffer + y * db_stride + spanMinX;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
            SoftPixel* colorDst = SOFTFP_COLOR_ROW(y) + spanMinX;
#endif
            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);

            if (scratch->shadedOpaque) {
                const SoftPixel* shaded = (const SoftPixel*)scratch->samplePixels;
                x = spanMinX;
                while (x <= spanMaxX) {
                    int cellEnd = min(spanMaxX, x | 3);
                    int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
                    cc_uint16 coverageOld = coverageRow[coverageX];
                    cc_uint16 coverageMask = coverageOld;
                    for (; x <= cellEnd; x++, depth++) {
                        DepthValue z;
                        int texIndex;
                        SoftPixel color;
                        if (wInterp <= 0) goto next_opaque_terrain_pixel;
                        z = EncodeWDepth8(wInterp);
                        if (z >= *depth) goto next_opaque_terrain_pixel;
                        texIndex = ((FixedToInt(vInterp) & (TERRAIN_TILE_SIZE - 1)) << 4) |
                                    (FixedToInt(uInterp) & (TERRAIN_TILE_SIZE - 1));
                        color = SoftFP_ApplyTerrainFogLevel(
                            shaded[texIndex], scratch->fogLevel);
                        *depth = z;
                        if (coverageMask != 0xFFFFu) {
                            coverageMask = (cc_uint16)(coverageMask |
                                (cc_uint16)(coverageBitBase << (x & 3)));
                        }
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        *colorDst = color;
#else
                        StoreRasterGroup(x, y, color);
#endif

next_opaque_terrain_pixel:
                        wInterp += scratch->stepW;
                        uInterp += scratch->stepU;
                        vInterp += scratch->stepV;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        colorDst++;
#endif
                    }
                    if (coverageMask != coverageOld) {
                        coverageRow[coverageX] = coverageMask;
                        if (coverageMask == 0xFFFFu) {
                            SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
                        }
                    }
                }
            } else {
                x = spanMinX;
                while (x <= spanMaxX) {
                    int cellEnd = min(spanMaxX, x | 3);
                    int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
                    cc_uint16 coverageOld = coverageRow[coverageX];
                    cc_uint16 coverageMask = coverageOld;
                    for (; x <= cellEnd; x++, depth++) {
                        DepthValue z;
                        int texX, texY, texIndex;
                        SoftTexel texel;
                        SoftPixel color;
                        if (wInterp <= 0) goto next_cutout_terrain_pixel;
                        z = EncodeWDepth8(wInterp);
                        if (z >= *depth) goto next_cutout_terrain_pixel;
                        texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                        texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                        texIndex = texY * scratch->sampleWidth + texX;
                        texel = ((SoftTexel*)scratch->samplePixels)[texIndex];
                        if (!(texel & SOFT_TEXEL_A_MASK)) goto next_cutout_terrain_pixel;
                        color = scratch->tintTable ?
                            SoftPixel_FromTintTable(texel, scratch->tintTable) :
                            SoftPixel_FromTexelTint(texel, scratch->tintR,
                                                    scratch->tintG, scratch->tintB);
                        color = SoftFP_ApplyTerrainFogLevel(color, scratch->fogLevel);
                        *depth = z;
                        if (coverageMask != 0xFFFFu) {
                            coverageMask = (cc_uint16)(coverageMask |
                                (cc_uint16)(coverageBitBase << (x & 3)));
                        }
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        *colorDst = color;
#else
                        StoreRasterGroup(x, y, color);
#endif

next_cutout_terrain_pixel:
                        wInterp += scratch->stepW;
                        uInterp += scratch->stepU;
                        vInterp += scratch->stepV;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        colorDst++;
#endif
                    }
                    if (coverageMask != coverageOld) {
                        coverageRow[coverageX] = coverageMask;
                        if (coverageMask == 0xFFFFu) {
                            SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
                        }
                    }
                }
            }
        }
        rowW += scratch->rowStepW;
        rowU += scratch->rowStepU;
        rowV += scratch->rowStepV;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i];
        }
    }
    rasterSpanPixels += spanPixelsLocal;
}

/* Perspective correction is deliberately a separate worker kernel. The
 * overwhelmingly common affine path therefore keeps exactly the same hot
 * loop and register pressure. This kernel is selected only for large opaque
 * oblique faces and evaluates U/V at screen-aligned eight-pixel boundaries. */
static CC_NOINLINE void RasterizeTerrainPerspectiveDirect(TerrainRasterScratch* scratch) {
    const DepthValue* depthLut = wDepth8EncodeLut;
    const SoftPixel* shaded = (const SoftPixel*)scratch->samplePixels;
    const int shift = scratch->perspectiveShift;
    const int interval = 1 << shift;
    int rowW = scratch->rowW, rowU = scratch->rowU, rowV = scratch->rowV;
    cc_uint32 spanPixelsLocal = 0, reciprocalLocal = 0;
    cc_uint32 cellSkipPixelsLocal = 0;
    int i, x, y;

    for (y = scratch->minY; y <= scratch->maxY; y++) {
        int spanLower = 0, spanUpper = scratch->maxX - scratch->minX;
        cc_bool rowVisible = true;
        for (i = 0; i < scratch->vertexCount; i++) {
            if (!ClipSpanEdge(scratch->edgeStart[i], scratch->edgeStepX[i],
                              &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int spanMinX = scratch->minX + spanLower;
            int spanMaxX = scratch->minX + spanUpper;
            int wInterp = rowW + scratch->stepW * spanLower;
            int uOverW = rowU + scratch->stepU * spanLower;
            int vOverW = rowV + scratch->stepV * spanLower;
            int perspectiveU = 0, perspectiveV = 0;
            int perspectiveStepU = 0, perspectiveStepV = 0;
            int perspectiveEndU = 0, perspectiveEndV = 0;
            int reciprocalEnd = 0, perspectiveRemain = 0;
            cc_bool perspectiveValid = false;
            int coverageY = y >> SOFTFP_COVERAGE_SHIFT;
            cc_uint16* coverageRow = terrainCoverage +
                coverageY * SOFTFP_COVERAGE_COLS;
            cc_uint16 coverageBitBase =
                (cc_uint16)(1u << ((y & 3) << 2));
            DepthValue* depth = depthBuffer + y * db_stride + spanMinX;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
            SoftPixel* colorDst = SOFTFP_COLOR_ROW(y) + spanMinX;
#endif
            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);

            x = spanMinX;
            while (x <= spanMaxX) {
                int cellEnd = min(spanMaxX, x | 3);
                int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
                cc_uint16 coverageOld = coverageRow[coverageX];
                cc_uint16 coverageMask = coverageOld;

                if (!perspectiveRemain) {
                    int offset = x & (interval - 1);
                    int segment = min(interval - offset, spanMaxX - x + 1);
                    int baseW = wInterp - scratch->stepW * offset;
                    int endW = baseW + scratch->stepW * interval;
                    int baseUOverW = uOverW - scratch->stepU * offset;
                    int baseVOverW = vOverW - scratch->stepV * offset;
                    int endUOverW = baseUOverW + scratch->stepU * interval;
                    int endVOverW = baseVOverW + scratch->stepV * interval;
                    int baseReciprocal, endReciprocal;
                    int baseU, baseV, endU, endV;

                    if (baseW <= 0) {
                        offset = 0;
                        segment = min(interval, spanMaxX - x + 1);
                        baseW = wInterp;
                        endW = baseW + scratch->stepW * interval;
                        baseUOverW = uOverW;
                        baseVOverW = vOverW;
                        endUOverW = baseUOverW + scratch->stepU * interval;
                        endVOverW = baseVOverW + scratch->stepV * interval;
                    }

                    if (!offset && perspectiveValid) {
                        baseReciprocal = reciprocalEnd;
                        baseU = perspectiveEndU;
                        baseV = perspectiveEndV;
                    } else {
                        baseReciprocal = FixedReciprocalFast(baseW);
                        baseU = FixedMul(baseUOverW, baseReciprocal);
                        baseV = FixedMul(baseVOverW, baseReciprocal);
                        reciprocalLocal++;
                    }
                    if (endW > 0) {
                        endReciprocal = FixedReciprocalFast(endW);
                        reciprocalLocal++;
                    } else {
                        endReciprocal = baseReciprocal;
                    }
                    endU = FixedMul(endUOverW, endReciprocal);
                    endV = FixedMul(endVOverW, endReciprocal);
                    perspectiveStepU = (endU - baseU) >> shift;
                    perspectiveStepV = (endV - baseV) >> shift;
                    perspectiveU = baseU + perspectiveStepU * offset;
                    perspectiveV = baseV + perspectiveStepV * offset;
                    perspectiveEndU = endU;
                    perspectiveEndV = endV;
                    reciprocalEnd = endReciprocal;
                    perspectiveRemain = segment;
                    perspectiveValid = true;
                }

                if (coverageOld == 0xFFFFu && wInterp > 0) {
                    int pixels = cellEnd - x + 1;
                    int wEnd = wInterp + scratch->stepW * (pixels - 1);
                    if (wEnd > 0) {
                        int nearestW = max(wInterp, wEnd);
                        DepthValue nearest = depthLut[(cc_uint32)nearestW >> 8];
                        DepthValue covered = terrainHiZ4[
                            coverageY * SOFTFP_COVERAGE_COLS + coverageX];
                        if (covered <= nearest) {
                            x += pixels;
                            depth += pixels;
                            wInterp += scratch->stepW * pixels;
                            uOverW += scratch->stepU * pixels;
                            vOverW += scratch->stepV * pixels;
                            perspectiveU += perspectiveStepU * pixels;
                            perspectiveV += perspectiveStepV * pixels;
                            perspectiveRemain -= pixels;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                            colorDst += pixels;
#endif
                            cellSkipPixelsLocal += (cc_uint32)pixels;
                            continue;
                        }
                    }
                }

                for (; x <= cellEnd; x++, depth++) {
                    int texIndex;
                    DepthValue z = depthLut[(cc_uint32)wInterp >> 8];
                    if (z < *depth) {
                        texIndex = ((FixedToInt(perspectiveV) & (TERRAIN_TILE_SIZE - 1)) << 4) |
                                   (FixedToInt(perspectiveU) & (TERRAIN_TILE_SIZE - 1));
                        *depth = z;
                        coverageMask = (cc_uint16)(coverageMask |
                            (cc_uint16)(coverageBitBase << (x & 3)));
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                        *colorDst = shaded[texIndex];
#else
                        StoreRasterGroup(x, y, shaded[texIndex]);
#endif
                    }

                    wInterp += scratch->stepW;
                    uOverW += scratch->stepU;
                    vOverW += scratch->stepV;
                    perspectiveU += perspectiveStepU;
                    perspectiveV += perspectiveStepV;
                    perspectiveRemain--;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                    colorDst++;
#endif
                }
                if (coverageMask != coverageOld) {
                    coverageRow[coverageX] = coverageMask;
                    if (coverageMask == 0xFFFFu) {
                        SoftFP_PromoteTerrainCoverage(coverageX, coverageY);
                    }
                }
            }
        }
        rowW += scratch->rowStepW;
        rowU += scratch->rowStepU;
        rowV += scratch->rowStepV;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i];
        }
    }
    __atomic_fetch_add(&rasterSpanPixels, spanPixelsLocal, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rasterPerspectiveTriangles, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rasterPerspectivePixels, spanPixelsLocal, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rasterPerspectiveReciprocals, reciprocalLocal, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rasterHiZCellSkipPixels, cellSkipPixelsLocal, __ATOMIC_RELAXED);
}

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
static CC_NOINLINE void RasterizeTerrainAffine(TerrainRasterScratch* scratch) {
    scratch->workerMode = SOFTFP_RASTER_JOB_AFFINE;
    if (!softfpRasterTask) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainAffineDirect(scratch);
        return;
    }
    if (!SoftFP_RasterWorkerQueue(scratch)) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainAffineDirect(scratch);
    }
}

static CC_NOINLINE void RasterizeTerrainPerspective(TerrainRasterScratch* scratch) {
    scratch->workerMode = SOFTFP_RASTER_JOB_PERSPECTIVE;
    if (!softfpRasterTask) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainPerspectiveDirect(scratch);
        return;
    }
    if (!SoftFP_RasterWorkerQueue(scratch)) {
        softfpRasterFallbacksFrame++;
        RasterizeTerrainPerspectiveDirect(scratch);
    }
}
#else
#define RasterizeTerrainAffine RasterizeTerrainAffineDirect
#define RasterizeTerrainPerspective RasterizeTerrainPerspectiveDirect
#endif

static CC_NOINLINE void RasterizeTerrainDepthOnly(TerrainRasterScratch* scratch) {
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    SoftFP_RasterWorkerFlush();
#endif
    int rowW = scratch->rowW;
    cc_uint32 spanPixelsLocal = 0;
    int i, x, y;

    for (y = scratch->minY; y <= scratch->maxY; y++) {
        int spanLower = 0, spanUpper = scratch->maxX - scratch->minX;
        cc_bool rowVisible = true;
        for (i = 0; i < scratch->vertexCount; i++) {
            if (!ClipSpanEdge(scratch->edgeStart[i], scratch->edgeStepX[i],
                              &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int spanMinX = scratch->minX + spanLower;
            int spanMaxX = scratch->minX + spanUpper;
            int wInterp = rowW + scratch->stepW * spanLower;
            DepthValue* depth = depthBuffer + y * db_stride + spanMinX;
            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);

            for (x = spanMinX; x <= spanMaxX; x++, depth++) {
                if (wInterp > 0) {
                    DepthValue z = EncodeWDepth8(wInterp);
                    if (z <= *depth) *depth = z;
                }
                wInterp += scratch->stepW;
            }
        }
        rowW += scratch->rowStepW;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i];
        }
    }
    rasterSpanPixels += spanPixelsLocal;
}

#ifdef SOFTFP_LOWRES_COLOR_STAGE
static CC_INLINE void SoftFP_Blend565HalfDepth(DepthValue* depth,
                                               SoftPixel* color,
                                               int inverseW,
                                               SoftPixel src) {
    DepthValue z;
    SoftPixel dst;
    if (inverseW <= 0) return;
    z = EncodeWDepth8(inverseW);
    if (z > *depth) return;
    dst = *color;
    /* Exact 50% average directly in RGB565. No channel unpack, multiply,
     * divide, or 32-bit temporary color is needed. */
    *color = (SoftPixel)((src & dst) + (((src ^ dst) & 0xF7DEu) >> 1));
    *depth = z;
}
#endif

static CC_NOINLINE void RasterizeTerrainHalfBlendDirect(TerrainRasterScratch* scratch) {
    int rowW = scratch->rowW, rowU = scratch->rowU, rowV = scratch->rowV;
    int rowAdvance = scratch->translucentRowMode ? 2 : 1;
    int firstY = scratch->minY;
    cc_uint32 spanPixelsLocal = 0;
    int i, x, y;
    __atomic_fetch_add(&rasterHalfBlendCalls, 1u, __ATOMIC_RELAXED);

    /* Mode 1 owns even rows and mode 2 owns odd rows. This lets Core 0 and
     * Core 1 rasterise the same translucent scene concurrently without ever
     * touching the same color/depth locations, so draw order and pixels stay
     * identical to the single-core checker pass. */
    if (scratch->translucentRowMode &&
        ((firstY & 1) != (scratch->translucentRowMode - 1))) {
        firstY++;
        rowW += scratch->rowStepW;
        rowU += scratch->rowStepU;
        rowV += scratch->rowStepV;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i];
        }
    }

    for (y = firstY; y <= scratch->maxY; y += rowAdvance) {
        int spanLower = 0, spanUpper = scratch->maxX - scratch->minX;
        cc_bool rowVisible = true;
        for (i = 0; i < scratch->vertexCount; i++) {
            if (!ClipSpanEdge(scratch->edgeStart[i], scratch->edgeStepX[i],
                              &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int spanMinX = scratch->minX + spanLower;
            int spanMaxX = scratch->minX + spanUpper;
            int wInterp = rowW + scratch->stepW * spanLower;
            int uInterp = rowU + scratch->stepU * spanLower;
            int vInterp = rowV + scratch->stepV * spanLower;
            DepthValue* depth = depthBuffer + y * db_stride + spanMinX;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
            SoftPixel* colorDst = SOFTFP_COLOR_ROW(y) + spanMinX;
#endif
            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);

            if (scratch->blendDepthWrite) {
                int stepW2 = scratch->stepW << 1;
                int stepU2 = scratch->stepU << 1;
                int stepV2 = scratch->stepV << 1;
#ifdef SOFTFP_LOWRES_COLOR_STAGE

                /* Smooth every pixel, but quantise affine texture sampling to
                 * one texel per horizontal pair. Water keeps a true RGB565
                 * 50% blend while U/V address and texture/tint work are halved.
                 * Reciprocal depth remains exact for each pixel. */
                if (scratch->shadedOpaque) {
                    const SoftPixel* pixels = (const SoftPixel*)scratch->samplePixels;
                    for (x = spanMinX; x + 1 <= spanMaxX; x += 2, depth += 2,
                         colorDst += 2) {
                        int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                        int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                        SoftPixel src = pixels[texY * scratch->sampleWidth + texX];
                        SoftFP_Blend565HalfDepth(depth + 0, colorDst + 0,
                                                wInterp, src);
                        SoftFP_Blend565HalfDepth(depth + 1, colorDst + 1,
                                                wInterp + scratch->stepW, src);
                        wInterp += stepW2;
                        uInterp += stepU2;
                        vInterp += stepV2;
                    }
                    if (x <= spanMaxX) {
                        int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                        int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                        SoftPixel src = pixels[texY * scratch->sampleWidth + texX];
                        SoftFP_Blend565HalfDepth(depth, colorDst, wInterp, src);
                    }
                } else {
                    for (x = spanMinX; x + 1 <= spanMaxX; x += 2, depth += 2,
                         colorDst += 2) {
                        int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                        int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                        int texIndex = texY * scratch->sampleWidth + texX;
                        SoftTexel texel = ((SoftTexel*)scratch->samplePixels)[texIndex];
                        if (texel & SOFT_TEXEL_A_MASK) {
                            SoftPixel src = scratch->tintTable ?
                                SoftPixel_FromTintTable(texel, scratch->tintTable) :
                                SoftPixel_FromTexelTint(texel, scratch->tintR,
                                                        scratch->tintG, scratch->tintB);
                            SoftFP_Blend565HalfDepth(depth + 0, colorDst + 0,
                                                    wInterp, src);
                            SoftFP_Blend565HalfDepth(depth + 1, colorDst + 1,
                                                    wInterp + scratch->stepW, src);
                        }
                        wInterp += stepW2;
                        uInterp += stepU2;
                        vInterp += stepV2;
                    }
                    if (x <= spanMaxX) {
                        int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                        int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                        int texIndex = texY * scratch->sampleWidth + texX;
                        SoftTexel texel = ((SoftTexel*)scratch->samplePixels)[texIndex];
                        if (texel & SOFT_TEXEL_A_MASK) {
                            SoftPixel src = scratch->tintTable ?
                                SoftPixel_FromTintTable(texel, scratch->tintTable) :
                                SoftPixel_FromTexelTint(texel, scratch->tintR,
                                                        scratch->tintG, scratch->tintB);
                            SoftFP_Blend565HalfDepth(depth, colorDst, wInterp, src);
                        }
                    }
                }
#else
                for (x = spanMinX; x <= spanMaxX; x++, depth++) {
                    if (wInterp > 0) {
                        DepthValue z = EncodeWDepth8(wInterp);
                        if (z <= *depth) {
                            int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                            int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                            int texIndex = texY * scratch->sampleWidth + texX;
                            SoftTexel texel = ((SoftTexel*)scratch->samplePixels)[texIndex];
                            if (scratch->shadedOpaque || (texel & SOFT_TEXEL_A_MASK)) {
                                SoftPixel src = scratch->shadedOpaque ?
                                    ((SoftPixel*)scratch->samplePixels)[texIndex] :
                                    (scratch->tintTable ?
                                        SoftPixel_FromTintTable(texel, scratch->tintTable) :
                                        SoftPixel_FromTexelTint(texel, scratch->tintR,
                                                                scratch->tintG, scratch->tintB));
                                BlendRasterGroup(x, y, SoftPixel_R(src), SoftPixel_G(src),
                                                 SoftPixel_B(src), 128);
                                *depth = z;
                            }
                        }
                    }
                    wInterp += scratch->stepW;
                    uInterp += scratch->stepU;
                    vInterp += scratch->stepV;
                }
#endif
            } else {
                for (x = spanMinX; x <= spanMaxX; x++, depth++) {
                    if (wInterp > 0) {
                        DepthValue z = EncodeWDepth8(wInterp);
                        if (z <= *depth) {
                            int texX = FixedToInt(uInterp) & scratch->sampleWidthMask;
                            int texY = FixedToInt(vInterp) & scratch->sampleHeightMask;
                            int texIndex = texY * scratch->sampleWidth + texX;
                            SoftTexel texel = ((SoftTexel*)scratch->samplePixels)[texIndex];
                            if (scratch->shadedOpaque || (texel & SOFT_TEXEL_A_MASK)) {
                                SoftPixel src = scratch->shadedOpaque ?
                                    ((SoftPixel*)scratch->samplePixels)[texIndex] :
                                    (scratch->tintTable ?
                                        SoftPixel_FromTintTable(texel, scratch->tintTable) :
                                        SoftPixel_FromTexelTint(texel, scratch->tintR,
                                                                scratch->tintG, scratch->tintB));
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                                SoftPixel dst = *colorDst;
                                *colorDst = (SoftPixel)((src & dst) +
                                    (((src ^ dst) & 0xF7DEu) >> 1));
#else
                                BlendRasterGroup(x, y, SoftPixel_R(src), SoftPixel_G(src),
                                                 SoftPixel_B(src), 128);
#endif
                            }
                        }
                    }
                    wInterp += scratch->stepW;
                    uInterp += scratch->stepU;
                    vInterp += scratch->stepV;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
                    colorDst++;
#endif
                }
            }
        }
        rowW += scratch->rowStepW * rowAdvance;
        rowU += scratch->rowStepU * rowAdvance;
        rowV += scratch->rowStepV * rowAdvance;
        for (i = 0; i < scratch->vertexCount; i++) {
            scratch->edgeStart[i] += scratch->edgeStepY[i] * rowAdvance;
        }
    }
    __atomic_fetch_add(&rasterSpanPixels, spanPixelsLocal, __ATOMIC_RELAXED);
}

#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
static CC_NOINLINE void RasterizeTerrainHalfBlend(TerrainRasterScratch* scratch) {
    TerrainRasterScratch directScratch;
    scratch->workerMode = SOFTFP_RASTER_JOB_TRANSLUCENT;
    if (!softfpRasterTask) {
        softfpRasterFallbacksFrame++;
        scratch->translucentRowMode = 0;
        RasterizeTerrainHalfBlendDirect(scratch);
        return;
    }

    /* Core 0 gets even rows through the FIFO. Core 1 immediately renders the
     * odd rows from its private scratch copy while the worker drains the prior
     * batch. The buffers are disjoint by construction, avoiding atomics in the
     * hot pixel loop and preserving the exact 50% checker pattern. */
    scratch->translucentRowMode = 1;
    if (!SoftFP_RasterWorkerQueue(scratch)) {
        softfpRasterFallbacksFrame++;
        scratch->translucentRowMode = 0;
        RasterizeTerrainHalfBlendDirect(scratch);
        return;
    }
    softfpRasterTransJobsFrame++;

    directScratch = *scratch;
    directScratch.translucentRowMode = 2;
    RasterizeTerrainHalfBlendDirect(&directScratch);

    /* Eight half-row jobs start quickly while amortising semaphore traffic. */
    if (softfpRasterFillCount[softfpRasterFillIndex] >= 8u) {
        (void)SoftFP_RasterWorkerSubmitPending();
    }
}
#else
#define RasterizeTerrainHalfBlend RasterizeTerrainHalfBlendDirect
#endif

/* ESP32-S3 always uses affine 3D texturing. Keeping the adaptive perspective
 * machinery in the hot function still forced a 336-432 byte stack frame and
 * several per-pixel branches, even though every perspective counter was zero.
 * This shared triangle/quad kernel keeps only the affine state. It also steps
 * W/U/V planes between rows instead of rebuilding them with 64-bit multiplies
 * on every scanline. Large functions deliberately remain outside .mod_iram. */
static CC_NOINLINE void DrawPolygonFastTerrainAffine(VertexFixed* V0,
                                                       VertexFixed* V1,
                                                       VertexFixed* V2,
                                                       VertexFixed* V3,
                                                       int vertexCount,
                                                       int area, int factor,
                                                       int mapMode,
                                                       int perspectiveShift) {
    cc_uint32 setupStart = SoftFP_ReadCycles();
    TerrainRasterScratch* scratch = terrainRasterScratch;
    int minX, minY, maxX, maxY;
    int rasterMaxX = (fb_maxX * raster_width) / fb_width;
    int rasterMaxY = (fb_maxY * raster_height) / fb_height;
    int orientation = area < 0 ? -1 : 1;
    int w0 = V0->w, w1 = V1->w, w2 = V2->w;
    int u0, u1, u2, v0, v1, v2;
    int sx0, sx1, sx2, sy0, sy1, sy2;
    int rowW, rowU, rowV, stepW, stepU, stepV;
    int rowStepW, rowStepU, rowStepV;
    int r1 = PackedCol_R(V0->c);
    int g1 = PackedCol_G(V0->c);
    int b1 = PackedCol_B(V0->c);
    int minFogW, maxFogW, fogLevel;
    cc_uint32 tintKey = ((cc_uint32)r1 << 16) |
                        ((cc_uint32)g1 << 8) | (cc_uint32)b1;
    const TerrainTintTable* tintTable = NULL;
    cc_uint16* samplePixels = (cc_uint16*)curTexPixels;
    int sampleHeightMask = texHeightMask;
    cc_bool shadedOpaque = false;
    int sampleX, sampleY, bc0, bc1, bc2;
    int ic0, ic1, ic2;
    int i;

    scratch->verts[0] = V0; scratch->verts[1] = V1;
    scratch->verts[2] = V2; scratch->verts[3] = V3;
    minX = maxX = V0->x;
    minY = maxY = V0->y;
    for (i = 0; i < vertexCount; i++) {
        int next = i + 1 == vertexCount ? 0 : i + 1;
        scratch->xEdge[i] = SOFTFP_RASTER_COORD(scratch->verts[i]->x);
        scratch->yEdge[i] = SOFTFP_RASTER_COORD(scratch->verts[i]->y);
        minX = min(minX, scratch->verts[i]->x); maxX = max(maxX, scratch->verts[i]->x);
        minY = min(minY, scratch->verts[i]->y); maxY = max(maxY, scratch->verts[i]->y);
        scratch->edgeStepX[i] = orientation *
            (scratch->yEdge[i] - SOFTFP_RASTER_COORD(scratch->verts[next]->y));
        scratch->edgeStepY[i] = orientation *
            (SOFTFP_RASTER_COORD(scratch->verts[next]->x) - scratch->xEdge[i]);
    }
    minX = FixedToInt(minX); minY = FixedToInt(minY);
    maxX = FixedToInt(maxX); maxY = FixedToInt(maxY);
    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    rasterBboxPixels += (cc_uint32)(maxX - minX + 1) *
                        (cc_uint32)(maxY - minY + 1);
    if (w0 <= 0 && w1 <= 0 && w2 <= 0 && (!V3 || V3->w <= 0)) return;

    minFogW = min(w0, min(w1, w2));
    maxFogW = max(w0, max(w1, w2));
    if (V3) {
        minFogW = min(minFogW, V3->w);
        maxFogW = max(maxFogW, V3->w);
    }
    fogLevel = mapMode == SOFTFP_MAP_MODE_OPAQUE ?
        SoftFP_SelectTerrainFogLevel((minFogW >> 1) + (maxFogW >> 1)) : 0;

    if (mapMode != SOFTFP_MAP_MODE_DEPTH_ONLY) {
        tintTable = TerrainTintCache_Get(r1, g1, b1);
    }

    u0 = (int)((cc_uint32)V0->u * (cc_uint32)curTexWidth);
    v0 = (int)((cc_uint32)V0->v * (cc_uint32)curTexHeight);
    u1 = (int)((cc_uint32)V1->u * (cc_uint32)curTexWidth);
    v1 = (int)((cc_uint32)V1->v * (cc_uint32)curTexHeight);
    u2 = (int)((cc_uint32)V2->u * (cc_uint32)curTexWidth);
    v2 = (int)((cc_uint32)V2->v * (cc_uint32)curTexHeight);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    if (perspectiveShift >= 0) {
        u0 = FixedMul(u0, w0); v0 = FixedMul(v0, w0);
        u1 = FixedMul(u1, w1); v1 = FixedMul(v1, w1);
        u2 = FixedMul(u2, w2); v2 = FixedMul(v2, w2);
    }
#else
    perspectiveShift = -1;
#endif

#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    if (mapMode != SOFTFP_MAP_MODE_DEPTH_ONLY) {
    if (curTexWidth == TERRAIN_TILE_SIZE && curTexHeight >= TERRAIN_TILE_SIZE &&
        !(curTexHeight & (TERRAIN_TILE_SIZE - 1))) {
        int tileIndex = -1;
        cc_bool oneTile = true;
        for (i = 0; i < vertexCount; i++) {
            int scaledV = (int)((cc_uint32)scratch->verts[i]->v * (cc_uint32)curTexHeight);
            int candidate = (FixedToInt(scaledV) & texHeightMask) / TERRAIN_TILE_SIZE;
            if (tileIndex < 0) tileIndex = candidate;
            else if (candidate != tileIndex) { oneTile = false; break; }
        }
        if (oneTile) {
            SoftPixel* cached = TerrainTileCache_GetShaded(
                tileIndex, tintTable, tintKey, &shadedOpaque);
            if (cached) {
                samplePixels = (cc_uint16*)cached;
                sampleHeightMask = TERRAIN_TILE_SIZE - 1;
            }
        }
    }
    }
#endif

    sampleX = (minX << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
    sampleY = (minY << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
    bc0 = SoftFP_RasterEdge(scratch->xEdge[1], scratch->yEdge[1],
                            scratch->xEdge[2], scratch->yEdge[2], sampleX, sampleY);
    bc1 = SoftFP_RasterEdge(scratch->xEdge[2], scratch->yEdge[2],
                            scratch->xEdge[0], scratch->yEdge[0], sampleX, sampleY);
    bc2 = SoftFP_RasterEdge(scratch->xEdge[0], scratch->yEdge[0],
                            scratch->xEdge[1], scratch->yEdge[1], sampleX, sampleY);
    for (i = 0; i < vertexCount; i++) {
        int next = i + 1 == vertexCount ? 0 : i + 1;
        scratch->edgeStart[i] = orientation * SoftFP_RasterEdge(
            scratch->xEdge[i], scratch->yEdge[i],
            scratch->xEdge[next], scratch->yEdge[next], sampleX, sampleY);
    }

    sx0 = FixedMul(scratch->yEdge[1] - scratch->yEdge[2], factor);
    sx1 = FixedMul(scratch->yEdge[2] - scratch->yEdge[0], factor);
    sx2 = FixedMul(scratch->yEdge[0] - scratch->yEdge[1], factor);
    sy0 = FixedMul(scratch->xEdge[2] - scratch->xEdge[1], factor);
    sy1 = FixedMul(scratch->xEdge[0] - scratch->xEdge[2], factor);
    sy2 = FixedMul(scratch->xEdge[1] - scratch->xEdge[0], factor);
    ic0 = FixedMul(bc0, factor);
    ic1 = FixedMul(bc1, factor);
    ic2 = FixedMul(bc2, factor);

    rowW = FixedMul(ic0, w0) + FixedMul(ic1, w1) + FixedMul(ic2, w2);
    rowU = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
    rowV = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);
    stepW = FixedMul(sx0, w0) + FixedMul(sx1, w1) + FixedMul(sx2, w2);
    stepU = FixedMul(sx0, u0) + FixedMul(sx1, u1) + FixedMul(sx2, u2);
    stepV = FixedMul(sx0, v0) + FixedMul(sx1, v1) + FixedMul(sx2, v2);
    rowStepW = FixedMul(sy0, w0) + FixedMul(sy1, w1) + FixedMul(sy2, w2);
    rowStepU = FixedMul(sy0, u0) + FixedMul(sy1, u1) + FixedMul(sy2, u2);
    rowStepV = FixedMul(sy0, v0) + FixedMul(sy1, v1) + FixedMul(sy2, v2);

    scratch->minX = minX; scratch->minY = minY;
    scratch->maxX = maxX; scratch->maxY = maxY;
    scratch->vertexCount = vertexCount;
    scratch->rowW = rowW; scratch->rowU = rowU; scratch->rowV = rowV;
    scratch->stepW = stepW; scratch->stepU = stepU; scratch->stepV = stepV;
    scratch->rowStepW = rowStepW;
    scratch->rowStepU = rowStepU;
    scratch->rowStepV = rowStepV;
    scratch->tintR = r1; scratch->tintG = g1; scratch->tintB = b1;
    scratch->tintTable = tintTable;
    scratch->samplePixels = samplePixels;
    scratch->sampleWidth = curTexWidth;
    scratch->sampleWidthMask = texWidthMask;
    scratch->sampleHeightMask = sampleHeightMask;
    scratch->shadedOpaque = shadedOpaque;
    scratch->fogLevel = (cc_uint8)fogLevel;
    scratch->blendDepthWrite = (cc_uint8)depthWrite;
    scratch->translucentRowMode = 0;
    scratch->perspectiveShift = (signed char)perspectiveShift;
    scratch->depthLutSafe = wDepth8EncodeLut && minFogW > 0 &&
        (cc_uint32)maxFogW <= ((SOFTFP_W_DEPTH8_LUT_SIZE - 32u) << 8);
    rasterMapSetupCycles += SoftFP_ReadCycles() - setupStart;
    if (mapMode == SOFTFP_MAP_MODE_DEPTH_ONLY) {
        RasterizeTerrainDepthOnly(scratch);
    } else if (mapMode == SOFTFP_MAP_MODE_HALF_BLEND) {
        RasterizeTerrainHalfBlend(scratch);
    } else if (perspectiveShift >= 0 && scratch->shadedOpaque &&
               scratch->depthLutSafe && mapMode == SOFTFP_MAP_MODE_OPAQUE) {
        RasterizeTerrainPerspective(scratch);
    } else if (scratch->shadedOpaque && scratch->depthLutSafe) {
        rasterOpaqueLutQuads++;
        RasterizeTerrainOpaqueLut(scratch);
    } else {
        RasterizeTerrainAffine(scratch);
    }
}
#endif

#ifndef CC_BUILD_ESP32S3
static CC_NOINLINE void DrawTriangleFastTerrain(VertexFixed* V0, VertexFixed* V1,
                                                VertexFixed* V2, int area, int factor,
                                                int perspectiveShift) {
    int x0_fp = V0->x, y0_fp = V0->y;
    int x1_fp = V1->x, y1_fp = V1->y;
    int x2_fp = V2->x, y2_fp = V2->y;
    int x0_edge = SOFTFP_RASTER_COORD(x0_fp), y0_edge = SOFTFP_RASTER_COORD(y0_fp);
    int x1_edge = SOFTFP_RASTER_COORD(x1_fp), y1_edge = SOFTFP_RASTER_COORD(y1_fp);
    int x2_edge = SOFTFP_RASTER_COORD(x2_fp), y2_edge = SOFTFP_RASTER_COORD(y2_fp);
    int minX = FixedToInt(min(x0_fp, min(x1_fp, x2_fp)));
    int minY = FixedToInt(min(y0_fp, min(y1_fp, y2_fp)));
    int maxX = FixedToInt(max(x0_fp, max(x1_fp, x2_fp)));
    int maxY = FixedToInt(max(y0_fp, max(y1_fp, y2_fp)));
    int rasterMaxX = (fb_maxX * raster_width) / fb_width;
    int rasterMaxY = (fb_maxY * raster_height) / fb_height;
    int w0 = V0->w, w1 = V1->w, w2 = V2->w;
    int u0, v0, u1, v1, u2, v2;
    int dx01, dy01, dx12, dy12, dx20, dy20;
    int bc0_start, bc1_start, bc2_start;
    int step_ic0_per_x, step_ic1_per_x, step_ic2_per_x;
    int step_w, step_u, step_v;
    int x, y;
    int r1 = PackedCol_R(V0->c);
    int g1 = PackedCol_G(V0->c);
    int b1 = PackedCol_B(V0->c);
    const TerrainTintTable* tintTable;
    cc_bool perspective;
    int perspectiveInterval = perspectiveShift >= 0 ? 1 << perspectiveShift : 1;
    cc_uint32 spanPixelsLocal = 0;
    cc_uint32 perspectiveTrianglesLocal = 0, perspectivePixelsLocal = 0;
    cc_uint32 perspectiveReciprocalsLocal = 0;

    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    rasterBboxPixels += (cc_uint32)(maxX - minX + 1) *
                        (cc_uint32)(maxY - minY + 1);

    if (area == 0 || (w0 <= 0 && w1 <= 0 && w2 <= 0)) return;
    tintTable = TerrainTintCache_Get(r1, g1, b1);
    /* Multiplying a 16.16 coordinate by an integer texture dimension is
     * exactly the same as FixedMul(coord, dimension << 16). Keep the product
     * unsigned so the low 32-bit wrap behaviour also remains defined. */
    u0 = (int)((cc_uint32)V0->u * (cc_uint32)curTexWidth);
    v0 = (int)((cc_uint32)V0->v * (cc_uint32)curTexHeight);
    u1 = (int)((cc_uint32)V1->u * (cc_uint32)curTexWidth);
    v1 = (int)((cc_uint32)V1->v * (cc_uint32)curTexHeight);
    u2 = (int)((cc_uint32)V2->u * (cc_uint32)curTexWidth);
    v2 = (int)((cc_uint32)V2->v * (cc_uint32)curTexHeight);

    dx01 = y0_edge - y1_edge; dy01 = x1_edge - x0_edge;
    dx12 = y1_edge - y2_edge; dy12 = x2_edge - x1_edge;
    dx20 = y2_edge - y0_edge; dy20 = x0_edge - x2_edge;
    {
        int sampleX = (minX << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        int sampleY = (minY << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        bc0_start = SoftFP_RasterEdge(x1_edge, y1_edge, x2_edge, y2_edge, sampleX, sampleY);
        bc1_start = SoftFP_RasterEdge(x2_edge, y2_edge, x0_edge, y0_edge, sampleX, sampleY);
        bc2_start = SoftFP_RasterEdge(x0_edge, y0_edge, x1_edge, y1_edge, sampleX, sampleY);
    }

    {
        int wMin = min(w0, min(w1, w2));
        int wMax = max(w0, max(w1, w2));
        perspective = perspectiveShift >= 0 && wMin > 0 && wMax != wMin;
    }
    if (perspective) {
        u0 = FixedMul(u0, w0); v0 = FixedMul(v0, w0);
        u1 = FixedMul(u1, w1); v1 = FixedMul(v1, w1);
        u2 = FixedMul(u2, w2); v2 = FixedMul(v2, w2);
        perspectiveTrianglesLocal++;
    }

    step_ic0_per_x = FixedMul(dx12, factor);
    step_ic1_per_x = FixedMul(dx20, factor);
    step_ic2_per_x = FixedMul(dx01, factor);
    step_w = FixedMul(step_ic0_per_x, w0) +
             FixedMul(step_ic1_per_x, w1) + FixedMul(step_ic2_per_x, w2);
    step_u = FixedMul(step_ic0_per_x, u0) +
             FixedMul(step_ic1_per_x, u1) + FixedMul(step_ic2_per_x, u2);
    step_v = FixedMul(step_ic0_per_x, v0) +
             FixedMul(step_ic1_per_x, v1) + FixedMul(step_ic2_per_x, v2);

    for (y = minY; y <= maxY;
         y++, bc0_start += dy12, bc1_start += dy20, bc2_start += dy01) {
        int ic0 = FixedMul(bc0_start, factor);
        int ic1 = FixedMul(bc1_start, factor);
        int ic2 = FixedMul(bc2_start, factor);
        int rowIc0 = ic0, rowIc1 = ic1, rowIc2 = ic2;
        int spanLower = 0, spanUpper = maxX - minX;
        int spanOffset, spanMinX, spanMaxX;
        int w_interp;
        int u_interp, v_interp;
        int perspectiveU = 0, perspectiveV = 0;
        int perspectiveStepU = 0, perspectiveStepV = 0;
        int perspectiveEndU = 0, perspectiveEndV = 0;
        int reciprocalEnd = 0, perspectiveRemain = 0;
        cc_bool perspectiveValid = false;
        DepthValue* fastDepth;

        if (!ClipSpanEdge(rowIc0 + SOFTFP_COVERAGE_GUARD,
                          step_ic0_per_x, &spanLower, &spanUpper) ||
            !ClipSpanEdge(rowIc1 + SOFTFP_COVERAGE_GUARD,
                          step_ic1_per_x, &spanLower, &spanUpper) ||
            !ClipSpanEdge(rowIc2 + SOFTFP_COVERAGE_GUARD,
                          step_ic2_per_x, &spanLower, &spanUpper)) continue;

        spanMinX = minX + spanLower;
        spanMaxX = minX + spanUpper;
        spanOffset = spanLower;
        ic0 = rowIc0 + step_ic0_per_x * spanOffset;
        ic1 = rowIc1 + step_ic1_per_x * spanOffset;
        ic2 = rowIc2 + step_ic2_per_x * spanOffset;
        spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
        if (perspective) {
            perspectivePixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
        }

        w_interp = FixedMul(ic0, w0) + FixedMul(ic1, w1) + FixedMul(ic2, w2);
        u_interp = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
        v_interp = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);

        fastDepth = depthBuffer + y * db_stride + spanMinX;

        for (x = spanMinX; x <= spanMaxX; x++, fastDepth++) {
            DepthValue fastZ;

            if (w_interp <= 0) goto next_split_terrain_pixel;
            {
                int fastU, fastV;
                int fastTexX, fastTexY;
                SoftTexel fastTexel;
                SoftPixel fastColor;

                fastZ = EncodeWDepth8(w_interp);
                if (fastZ > *fastDepth) goto next_split_terrain_pixel;

                /* Defer perspective span setup until after the depth test.
                 * Fully occluded correction groups then avoid both reciprocal
                 * calculations without changing any sampled pixel. */
                if (perspective && !perspectiveRemain) {
                    int offset = x & (perspectiveInterval - 1);
                    int segment = min(perspectiveInterval - offset, spanMaxX - x + 1);
                    int baseW = w_interp - step_w * offset;
                    int endW = baseW + step_w * perspectiveInterval;
                    int baseReciprocal, endReciprocal;
                    int baseU, baseV, endU, endV;
                    int baseUOverW = u_interp - step_u * offset;
                    int baseVOverW = v_interp - step_v * offset;
                    int endUOverW = baseUOverW + step_u * perspectiveInterval;
                    int endVOverW = baseVOverW + step_v * perspectiveInterval;

                    if (baseW <= 0) {
                        offset = 0;
                        segment = min(perspectiveInterval, spanMaxX - x + 1);
                        baseW = w_interp;
                        endW = baseW + step_w * perspectiveInterval;
                        baseUOverW = u_interp;
                        baseVOverW = v_interp;
                        endUOverW = baseUOverW + step_u * perspectiveInterval;
                        endVOverW = baseVOverW + step_v * perspectiveInterval;
                    }

                    if (!offset && perspectiveValid) {
                        baseReciprocal = reciprocalEnd;
                        baseU = perspectiveEndU;
                        baseV = perspectiveEndV;
                    } else {
                        baseReciprocal = FixedReciprocalFast(baseW);
                        perspectiveReciprocalsLocal++;
                        baseU = FixedMul(baseUOverW, baseReciprocal);
                        baseV = FixedMul(baseVOverW, baseReciprocal);
                    }
                    if (endW > 0) {
                        endReciprocal = FixedReciprocalFast(endW);
                        perspectiveReciprocalsLocal++;
                    } else {
                        endReciprocal = baseReciprocal;
                    }
                    endU = FixedMul(endUOverW, endReciprocal);
                    endV = FixedMul(endVOverW, endReciprocal);

                    perspectiveStepU = (endU - baseU) >> perspectiveShift;
                    perspectiveStepV = (endV - baseV) >> perspectiveShift;
                    perspectiveU = baseU + perspectiveStepU * offset;
                    perspectiveV = baseV + perspectiveStepV * offset;
                    perspectiveEndU = endU;
                    perspectiveEndV = endV;
                    reciprocalEnd = endReciprocal;
                    perspectiveRemain = segment;
                    perspectiveValid = true;
                }
                fastU = perspective ? perspectiveU : u_interp;
                fastV = perspective ? perspectiveV : v_interp;

                fastTexX = FixedToInt(fastU) & texWidthMask;
                fastTexY = FixedToInt(fastV) & texHeightMask;
                fastTexel = curTexPixels[fastTexY * curTexWidth + fastTexX];
                if (!(fastTexel & SOFT_TEXEL_A_MASK)) goto next_split_terrain_pixel;

                *fastDepth = fastZ;
                SoftFP_RecordTerrainCoverage(x, y);
                fastColor = tintTable ? SoftPixel_FromTintTable(fastTexel, tintTable) :
                                        SoftPixel_FromTexelTint(fastTexel, r1, g1, b1);
#ifdef CC_BUILD_SOFTFP_FOG4
                fastColor = ApplyFog4(fastColor, w_interp);
#endif
                StoreRasterGroup(x, y, fastColor);
            }

next_split_terrain_pixel:
            w_interp += step_w;
            if (perspective && perspectiveRemain) {
                perspectiveU += perspectiveStepU;
                perspectiveV += perspectiveStepV;
                perspectiveRemain--;
            }
            u_interp += step_u;
            v_interp += step_v;
        }
    }

    rasterSpanPixels += spanPixelsLocal;
    rasterPerspectiveTriangles += perspectiveTrianglesLocal;
    rasterPerspectivePixels += perspectivePixelsLocal;
    rasterPerspectiveReciprocals += perspectiveReciprocalsLocal;
}

static CC_NOINLINE void DrawQuadFastTerrain(VertexFixed quad[4], int area,
                                            int factor, int perspectiveShift) {
    int x_fp[4], y_fp[4], x_edge[4], y_edge[4];
    int edgeStart[4], edgeStepX[4], edgeStepY[4];
    int minX, minY, maxX, maxY, rasterMaxX, rasterMaxY;
    int w0 = quad[0].w, w1 = quad[1].w, w2 = quad[2].w, w3 = quad[3].w;
    int u0, v0, u1, v1, u2, v2, v3;
    int dx01, dy01, dx12, dy12, dx20, dy20;
    int bc0_start, bc1_start, bc2_start;
    int step_ic0_per_x, step_ic1_per_x, step_ic2_per_x;
    int step_w, step_u, step_v;
    int orientation = area < 0 ? -1 : 1;
    int perspectiveInterval = perspectiveShift >= 0 ? 1 << perspectiveShift : 1;
    int r1 = PackedCol_R(quad[0].c);
    int g1 = PackedCol_G(quad[0].c);
    int b1 = PackedCol_B(quad[0].c);
    const TerrainTintTable* tintTable;
    int x, y, i;
    SoftTexel* quadTexPixels = curTexPixels;
    int quadTexHeightMask = texHeightMask;
    cc_bool perspective;
    cc_uint32 spanPixelsLocal = 0;
    cc_uint32 perspectivePrimitivesLocal = 0, perspectivePixelsLocal = 0;
    cc_uint32 perspectiveReciprocalsLocal = 0;

    for (i = 0; i < 4; i++) {
        x_fp[i] = quad[i].x; y_fp[i] = quad[i].y;
        x_edge[i] = SOFTFP_RASTER_COORD(x_fp[i]);
        y_edge[i] = SOFTFP_RASTER_COORD(y_fp[i]);
    }
    minX = FixedToInt(min(min(x_fp[0], x_fp[1]), min(x_fp[2], x_fp[3])));
    minY = FixedToInt(min(min(y_fp[0], y_fp[1]), min(y_fp[2], y_fp[3])));
    maxX = FixedToInt(max(max(x_fp[0], x_fp[1]), max(x_fp[2], x_fp[3])));
    maxY = FixedToInt(max(max(y_fp[0], y_fp[1]), max(y_fp[2], y_fp[3])));
    rasterMaxX = (fb_maxX * raster_width) / fb_width;
    rasterMaxY = (fb_maxY * raster_height) / fb_height;
    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    rasterBboxPixels += (cc_uint32)(maxX - minX + 1) *
                        (cc_uint32)(maxY - minY + 1);
    if (w0 <= 0 && w1 <= 0 && w2 <= 0 && w3 <= 0) return;
    tintTable = TerrainTintCache_Get(r1, g1, b1);

    u0 = (int)((cc_uint32)quad[0].u * (cc_uint32)curTexWidth);
    v0 = (int)((cc_uint32)quad[0].v * (cc_uint32)curTexHeight);
    u1 = (int)((cc_uint32)quad[1].u * (cc_uint32)curTexWidth);
    v1 = (int)((cc_uint32)quad[1].v * (cc_uint32)curTexHeight);
    u2 = (int)((cc_uint32)quad[2].u * (cc_uint32)curTexWidth);
    v2 = (int)((cc_uint32)quad[2].v * (cc_uint32)curTexHeight);
    v3 = (int)((cc_uint32)quad[3].v * (cc_uint32)curTexHeight);
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    if (curTexWidth == TERRAIN_TILE_SIZE && curTexHeight >= TERRAIN_TILE_SIZE &&
        !(curTexHeight & (TERRAIN_TILE_SIZE - 1))) {
        int tile0 = (FixedToInt(v0) & texHeightMask) / TERRAIN_TILE_SIZE;
        int tile1 = (FixedToInt(v1) & texHeightMask) / TERRAIN_TILE_SIZE;
        int tile2 = (FixedToInt(v2) & texHeightMask) / TERRAIN_TILE_SIZE;
        int tile3 = (FixedToInt(v3) & texHeightMask) / TERRAIN_TILE_SIZE;
        if (tile0 == tile1 && tile0 == tile2 && tile0 == tile3) {
            SoftTexel* cached = TerrainTileCache_Get(tile0);
            if (cached) {
                quadTexPixels = cached;
                quadTexHeightMask = TERRAIN_TILE_SIZE - 1;
            }
        }
    }
#endif

    dx01 = y_edge[0] - y_edge[1]; dy01 = x_edge[1] - x_edge[0];
    dx12 = y_edge[1] - y_edge[2]; dy12 = x_edge[2] - x_edge[1];
    dx20 = y_edge[2] - y_edge[0]; dy20 = x_edge[0] - x_edge[2];
    {
        int sampleX = (minX << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        int sampleY = (minY << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        bc0_start = SoftFP_RasterEdge(x_edge[1], y_edge[1], x_edge[2], y_edge[2], sampleX, sampleY);
        bc1_start = SoftFP_RasterEdge(x_edge[2], y_edge[2], x_edge[0], y_edge[0], sampleX, sampleY);
        bc2_start = SoftFP_RasterEdge(x_edge[0], y_edge[0], x_edge[1], y_edge[1], sampleX, sampleY);
        for (i = 0; i < 4; i++) {
            int next = (i + 1) & 3;
            edgeStart[i] = orientation * SoftFP_RasterEdge(
                x_edge[i], y_edge[i], x_edge[next], y_edge[next], sampleX, sampleY);
            edgeStepX[i] = orientation * (y_edge[i] - y_edge[next]);
            edgeStepY[i] = orientation * (x_edge[next] - x_edge[i]);
        }
    }

    {
        int wMin = min(min(w0, w1), min(w2, w3));
        int wMax = max(max(w0, w1), max(w2, w3));
        perspective = perspectiveShift >= 0 && wMin > 0 && wMax != wMin;
    }
    if (perspective) {
        u0 = FixedMul(u0, w0); v0 = FixedMul(v0, w0);
        u1 = FixedMul(u1, w1); v1 = FixedMul(v1, w1);
        u2 = FixedMul(u2, w2); v2 = FixedMul(v2, w2);
        /* Keep the existing counter in triangle-equivalent units. */
        perspectivePrimitivesLocal = 2;
    }

    step_ic0_per_x = FixedMul(dx12, factor);
    step_ic1_per_x = FixedMul(dx20, factor);
    step_ic2_per_x = FixedMul(dx01, factor);
    step_w = FixedMul(step_ic0_per_x, w0) +
             FixedMul(step_ic1_per_x, w1) + FixedMul(step_ic2_per_x, w2);
    step_u = FixedMul(step_ic0_per_x, u0) +
             FixedMul(step_ic1_per_x, u1) + FixedMul(step_ic2_per_x, u2);
    step_v = FixedMul(step_ic0_per_x, v0) +
             FixedMul(step_ic1_per_x, v1) + FixedMul(step_ic2_per_x, v2);

    for (y = minY; y <= maxY; y++) {
        int spanLower = 0, spanUpper = maxX - minX;
        cc_bool rowVisible = true;

        for (i = 0; i < 4; i++) {
            if (!ClipSpanEdge(edgeStart[i], edgeStepX[i], &spanLower, &spanUpper)) {
                rowVisible = false; break;
            }
        }
        if (rowVisible) {
            int ic0 = FixedMul(bc0_start, factor) + step_ic0_per_x * spanLower;
            int ic1 = FixedMul(bc1_start, factor) + step_ic1_per_x * spanLower;
            int ic2 = FixedMul(bc2_start, factor) + step_ic2_per_x * spanLower;
            int spanMinX = minX + spanLower;
            int spanMaxX = minX + spanUpper;
            int w_interp = FixedMul(ic0, w0) + FixedMul(ic1, w1) + FixedMul(ic2, w2);
            int u_interp = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
            int v_interp = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);
            int perspectiveU = 0, perspectiveV = 0;
            int perspectiveStepU = 0, perspectiveStepV = 0;
            int perspectiveEndU = 0, perspectiveEndV = 0;
            int reciprocalEnd = 0, perspectiveRemain = 0;
            cc_bool perspectiveValid = false;
            DepthValue* fastDepth = depthBuffer + y * db_stride + spanMinX;

            spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
            if (perspective) perspectivePixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);

            for (x = spanMinX; x <= spanMaxX; x++, fastDepth++) {
                DepthValue fastZ;
                if (w_interp <= 0) goto next_quad_terrain_pixel;
                {
                    int fastU, fastV, fastTexX, fastTexY;
                    SoftTexel fastTexel;
                    SoftPixel fastColor;

                    fastZ = EncodeWDepth8(w_interp);
                    if (fastZ > *fastDepth) goto next_quad_terrain_pixel;
                    if (perspective && !perspectiveRemain) {
                        int offset = x & (perspectiveInterval - 1);
                        int segment = min(perspectiveInterval - offset, spanMaxX - x + 1);
                        int baseW = w_interp - step_w * offset;
                        int endW = baseW + step_w * perspectiveInterval;
                        int baseReciprocal, endReciprocal;
                        int baseU, baseV, endU, endV;
                        int baseUOverW = u_interp - step_u * offset;
                        int baseVOverW = v_interp - step_v * offset;
                        int endUOverW = baseUOverW + step_u * perspectiveInterval;
                        int endVOverW = baseVOverW + step_v * perspectiveInterval;

                        if (baseW <= 0) {
                            offset = 0;
                            segment = min(perspectiveInterval, spanMaxX - x + 1);
                            baseW = w_interp;
                            endW = baseW + step_w * perspectiveInterval;
                            baseUOverW = u_interp;
                            baseVOverW = v_interp;
                            endUOverW = baseUOverW + step_u * perspectiveInterval;
                            endVOverW = baseVOverW + step_v * perspectiveInterval;
                        }
                        if (!offset && perspectiveValid) {
                            baseReciprocal = reciprocalEnd;
                            baseU = perspectiveEndU; baseV = perspectiveEndV;
                        } else {
                            baseReciprocal = FixedReciprocalFast(baseW);
                            perspectiveReciprocalsLocal++;
                            baseU = FixedMul(baseUOverW, baseReciprocal);
                            baseV = FixedMul(baseVOverW, baseReciprocal);
                        }
                        if (endW > 0) {
                            endReciprocal = FixedReciprocalFast(endW);
                            perspectiveReciprocalsLocal++;
                        } else {
                            endReciprocal = baseReciprocal;
                        }
                        endU = FixedMul(endUOverW, endReciprocal);
                        endV = FixedMul(endVOverW, endReciprocal);
                        perspectiveStepU = (endU - baseU) >> perspectiveShift;
                        perspectiveStepV = (endV - baseV) >> perspectiveShift;
                        perspectiveU = baseU + perspectiveStepU * offset;
                        perspectiveV = baseV + perspectiveStepV * offset;
                        perspectiveEndU = endU; perspectiveEndV = endV;
                        reciprocalEnd = endReciprocal;
                        perspectiveRemain = segment;
                        perspectiveValid = true;
                    }
                    fastU = perspective ? perspectiveU : u_interp;
                    fastV = perspective ? perspectiveV : v_interp;
                    fastTexX = FixedToInt(fastU) & texWidthMask;
                    fastTexY = FixedToInt(fastV) & quadTexHeightMask;
                    fastTexel = quadTexPixels[fastTexY * curTexWidth + fastTexX];
                    if (!(fastTexel & SOFT_TEXEL_A_MASK)) goto next_quad_terrain_pixel;
                    *fastDepth = fastZ;
                    SoftFP_RecordTerrainCoverage(x, y);
                    fastColor = tintTable ? SoftPixel_FromTintTable(fastTexel, tintTable) :
                                            SoftPixel_FromTexelTint(fastTexel, r1, g1, b1);
#ifdef CC_BUILD_SOFTFP_FOG4
                    fastColor = ApplyFog4(fastColor, w_interp);
#endif
                    StoreRasterGroup(x, y, fastColor);
                }

next_quad_terrain_pixel:
                w_interp += step_w;
                if (perspective && perspectiveRemain) {
                    perspectiveU += perspectiveStepU;
                    perspectiveV += perspectiveStepV;
                    perspectiveRemain--;
                }
                u_interp += step_u;
                v_interp += step_v;
            }
        }

        bc0_start += dy12; bc1_start += dy20; bc2_start += dy01;
        for (i = 0; i < 4; i++) edgeStart[i] += edgeStepY[i];
    }

    rasterSpanPixels += spanPixelsLocal;
    rasterPerspectiveTriangles += perspectivePrimitivesLocal;
    rasterPerspectivePixels += perspectivePixelsLocal;
    rasterPerspectiveReciprocals += perspectiveReciprocalsLocal;
}
#endif
#endif

static void DrawTriangle3DGeneric(VertexFixed* V0, VertexFixed* V1,
                                  VertexFixed* V2, int area, int factor,
                                  int perspectiveShift) {
    int x0_fp = V0->x, y0_fp = V0->y;
    int x1_fp = V1->x, y1_fp = V1->y;
    int x2_fp = V2->x, y2_fp = V2->y;
    int x0_edge = SOFTFP_RASTER_COORD(x0_fp), y0_edge = SOFTFP_RASTER_COORD(y0_fp);
    int x1_edge = SOFTFP_RASTER_COORD(x1_fp), y1_edge = SOFTFP_RASTER_COORD(y1_fp);
    int x2_edge = SOFTFP_RASTER_COORD(x2_fp), y2_edge = SOFTFP_RASTER_COORD(y2_fp);
    int minX = FixedToInt(min(x0_fp, min(x1_fp, x2_fp)));
    int minY = FixedToInt(min(y0_fp, min(y1_fp, y2_fp)));
    int maxX = FixedToInt(max(x0_fp, max(x1_fp, x2_fp)));
    int maxY = FixedToInt(max(y0_fp, max(y1_fp, y2_fp)));
    int x, y;
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
    int rasterMaxX = (fb_maxX * raster_width) / fb_width;
    int rasterMaxY = (fb_maxY * raster_height) / fb_height;
#else
    int rasterMaxX = fb_maxX, rasterMaxY = fb_maxY;
#endif
    int w0 = V0->w, w1 = V1->w, w2 = V2->w;
#ifndef CC_BUILD_SOFTFP_W_DEPTH
    int z0 = V0->z, z1 = V1->z, z2 = V2->z;
#endif
    int u0, v0, u1, v1, u2, v2;
    int dx01, dy01, dx12, dy12, dx20, dy20;
    int bc0_start, bc1_start, bc2_start;
    int step_ic0_per_x, step_ic1_per_x, step_ic2_per_x;
#ifndef CC_BUILD_SOFTFP_W_DEPTH
    int step_z;
#endif
#if !defined CC_BUILD_SOFTFP_AFFINE || defined CC_BUILD_SOFTFP_PERSPECTIVE4 || !defined CC_BUILD_SOFTFP_Z16_TILES
    int step_w;
#endif
    int step_u, step_v;
    int R = 0, G = 0, B = 0, A = 0, a1 = 0, r1 = 0, g1 = 0, b1 = 0;
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    cc_bool perspective = false;
    int perspectiveInterval = perspectiveShift >= 0 ? 1 << perspectiveShift : 1;
#endif
#ifdef CC_BUILD_SOFTFP_TEXEL_2X2
    cc_uint32 texReuseLocal = 0, texLoadsLocal = 0;
#endif
#ifdef CC_BUILD_ESP32S3
    cc_uint32 spanPixelsLocal = 0;
    cc_uint32 perspectiveTrianglesLocal = 0, perspectivePixelsLocal = 0;
    cc_uint32 perspectiveReciprocalsLocal = 0;
#endif
    PackedCol color = V0->c;
    cc_bool texturing = gfx_format == VERTEX_FORMAT_TEXTURED;
#if defined CC_BUILD_SOFTFP_FAST_CUTOUT && !defined SOFTFP_FAST_TERRAIN_KERNEL
    cc_bool fastCutout = false;
#endif
#ifdef CC_BUILD_ESP32S3
    cc_bool fastNoDepthTextured = false;
#if defined CC_BUILD_SOFTFP_W_DEPTH8 && defined SOFTFP_LOWRES_COLOR_STAGE
    cc_bool fastSolidDepthBlend = false;
    SoftPixel fastSolidPixel = 0;
#endif
#endif

    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
#ifdef CC_BUILD_ESP32S3
    rasterBboxPixels += (cc_uint32)(maxX - minX + 1) *
                        (cc_uint32)(maxY - minY + 1);
#endif

    if (area == 0 || (w0 <= 0 && w1 <= 0 && w2 <= 0)) return;
    u0 = FixedMul(V0->u, IntToFixed(curTexWidth));
    v0 = FixedMul(V0->v, IntToFixed(curTexHeight));
    u1 = FixedMul(V1->u, IntToFixed(curTexWidth));
    v1 = FixedMul(V1->v, IntToFixed(curTexHeight));
    u2 = FixedMul(V2->u, IntToFixed(curTexWidth));
    v2 = FixedMul(V2->v, IntToFixed(curTexHeight));

    dx01 = y0_edge - y1_edge; dy01 = x1_edge - x0_edge;
    dx12 = y1_edge - y2_edge; dy12 = x2_edge - x1_edge;
    dx20 = y2_edge - y0_edge; dy20 = x0_edge - x2_edge;
    {
        int sampleX = (minX << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        int sampleY = (minY << SOFTFP_RASTER_FP_SHIFT) + (SOFTFP_RASTER_FP_ONE >> 1);
        bc0_start = SoftFP_RasterEdge(x1_edge, y1_edge, x2_edge, y2_edge, sampleX, sampleY);
        bc1_start = SoftFP_RasterEdge(x2_edge, y2_edge, x0_edge, y0_edge, sampleX, sampleY);
        bc2_start = SoftFP_RasterEdge(x0_edge, y0_edge, x1_edge, y1_edge, sampleX, sampleY);
    }

    if (!texturing) {
        R = PackedCol_R(color); G = PackedCol_G(color);
        B = PackedCol_B(color); A = PackedCol_A(color);
    } else if (texSinglePixel) {
#if defined CC_BUILD_SOFTFP_AFFINE || defined CC_BUILD_SOFTFP_PERSPECTIVE4
        int rawY = min(v0, v1);
#else
        int rawY = min(FixedDiv(v0, w0), FixedDiv(v1, w1));
#endif
        int texY = (FixedToInt(rawY) + 1) & texHeightMask;
        MultiplyColorsInline(color, curTexPixels[texY * curTexWidth], &R, &G, &B, &A);
        texturing = false;
    } else {
        a1 = PackedCol_A(color); r1 = PackedCol_R(color);
        g1 = PackedCol_G(color); b1 = PackedCol_B(color);
    }

#if defined CC_BUILD_SOFTFP_FAST_CUTOUT && !defined SOFTFP_FAST_TERRAIN_KERNEL
    /* Normal terrain is textured A1 cutout with depth read/write and no
     * blending. Select that state once per triangle so its inner loop avoids
     * all of the generic colour/depth/blend state branches. */
    fastCutout = texturing && colWrite && depthTest && depthWrite &&
                 gfx_alphaTest && !gfx_alphaBlend && a1 >= 0x80;
#endif
#ifdef CC_BUILD_ESP32S3
    /* Crafti-style state-specialised kernel for the held 3D block/arm pass.
     * That pass is rendered last with depth testing and writing disabled, so
     * avoid encoding Z, touching the depth buffer, and checking generic blend
     * state for every pixel. Geometry, affine UVs and RGB565 shading stay the
     * same as the general path. */
    fastNoDepthTextured = texturing && colWrite && !depthTest && !depthWrite &&
                          !gfx_alphaBlend && a1 >= 0x80;
#if defined CC_BUILD_SOFTFP_W_DEPTH8 && defined SOFTFP_LOWRES_COLOR_STAGE
    /* Selection outlines are constant-colour translucent geometry. Keep the
     * exact triangle coverage, W-depth test and RGB565 blend, but select that
     * fixed state once per triangle instead of rechecking all generic texture,
     * alpha and depth-write branches for every covered pixel. */
    fastSolidDepthBlend = !texturing && colWrite && depthTest && !depthWrite &&
                          gfx_alphaBlend && !gfx_alphaTest && A != 0;
    if (fastSolidDepthBlend) fastSolidPixel = SoftPixel_Make(R, G, B);
#endif
#endif

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    if (texturing) {
        int wMin = min(w0, min(w1, w2));
        int wMax = max(w0, max(w1, w2));

        /* Affine interpolation is exact when reciprocal-W is constant.
         * Every face with real depth variation instead uses screen-aligned
         * perspective spans, so both triangles of a block face sample the
         * same texture mapping and cannot expose a diagonal half-face seam. */
        perspective = perspectiveShift >= 0 && wMin > 0 && wMax != wMin;
        if (perspective) {
            u0 = FixedMul(u0, w0); v0 = FixedMul(v0, w0);
            u1 = FixedMul(u1, w1); v1 = FixedMul(v1, w1);
            u2 = FixedMul(u2, w2); v2 = FixedMul(v2, w2);
#ifdef CC_BUILD_ESP32S3
            perspectiveTrianglesLocal++;
#endif
        }
    }
#endif

    step_ic0_per_x = FixedMul(dx12, factor);
    step_ic1_per_x = FixedMul(dx20, factor);
    step_ic2_per_x = FixedMul(dx01, factor);
#ifndef CC_BUILD_SOFTFP_W_DEPTH
    step_z = FixedMul(step_ic0_per_x, z0) +
             FixedMul(step_ic1_per_x, z1) + FixedMul(step_ic2_per_x, z2);
#endif
#if !defined CC_BUILD_SOFTFP_AFFINE || defined CC_BUILD_SOFTFP_PERSPECTIVE4 || !defined CC_BUILD_SOFTFP_Z16_TILES
    step_w = FixedMul(step_ic0_per_x, w0) +
             FixedMul(step_ic1_per_x, w1) + FixedMul(step_ic2_per_x, w2);
#endif
    if (texturing) {
        step_u = FixedMul(step_ic0_per_x, u0) +
                 FixedMul(step_ic1_per_x, u1) + FixedMul(step_ic2_per_x, u2);
        step_v = FixedMul(step_ic0_per_x, v0) +
                 FixedMul(step_ic1_per_x, v1) + FixedMul(step_ic2_per_x, v2);
    }

    for (y = minY; y <= maxY; y++, bc0_start += dy12, bc1_start += dy20, bc2_start += dy01) {
        int ic0 = FixedMul(bc0_start, factor);
        int ic1 = FixedMul(bc1_start, factor);
        int ic2 = FixedMul(bc2_start, factor);
        int rowIc0 = ic0, rowIc1 = ic1, rowIc2 = ic2;
        int spanLower = 0, spanUpper = maxX - minX;
        int spanOffset, spanMinX, spanMaxX;
#ifdef CC_BUILD_SOFTFP_TEXEL_2X2
        int lastTexIndex = -1;
        SoftTexel lastTexel = 0;
#ifdef CC_BUILD_SOFTFP_FB16
        SoftPixel lastOpaquePixel = 0;
#endif
#endif

        if (!ClipSpanEdge(rowIc0 + SOFTFP_COVERAGE_GUARD,
                          step_ic0_per_x, &spanLower, &spanUpper) ||
            !ClipSpanEdge(rowIc1 + SOFTFP_COVERAGE_GUARD,
                          step_ic1_per_x, &spanLower, &spanUpper) ||
            !ClipSpanEdge(rowIc2 + SOFTFP_COVERAGE_GUARD,
                          step_ic2_per_x, &spanLower, &spanUpper)) continue;

        spanMinX   = minX + spanLower;
        spanMaxX   = minX + spanUpper;
        spanOffset   = spanLower;
        ic0 = rowIc0 + step_ic0_per_x * spanOffset;
        ic1 = rowIc1 + step_ic1_per_x * spanOffset;
        ic2 = rowIc2 + step_ic2_per_x * spanOffset;
#ifdef CC_BUILD_ESP32S3
        spanPixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        if (perspective) {
            perspectivePixelsLocal += (cc_uint32)(spanMaxX - spanMinX + 1);
        }
#endif
#endif

#ifndef CC_BUILD_SOFTFP_W_DEPTH
        int z_interp = FixedMul(ic0, z0) + FixedMul(ic1, z1) + FixedMul(ic2, z2);
#endif
        int u_interp = 0, v_interp = 0;
#if !defined CC_BUILD_SOFTFP_AFFINE || defined CC_BUILD_SOFTFP_PERSPECTIVE4 || !defined CC_BUILD_SOFTFP_Z16_TILES
        int w_interp = FixedMul(ic0, w0) + FixedMul(ic1, w1) + FixedMul(ic2, w2);
#endif
#ifdef CC_BUILD_SOFTFP_Z16_TILES
        DepthValue* depthRow = NULL;
        int depthNextX = spanMinX, depthLineIndex = -1, depthXMask = -1;
#endif

        if (texturing) {
            u_interp = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
            v_interp = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);
        }

#ifdef CC_BUILD_ESP32S3
        if (fastNoDepthTextured) {
            int noDepthU = u_interp, noDepthV = v_interp;
            int noDepthW = w_interp;
            if (gfx_alphaTest) {
                for (x = spanMinX; x <= spanMaxX; x++) {
                    if (noDepthW > 0) {
                        int texX = FixedToInt(noDepthU) & texWidthMask;
                        int texY = FixedToInt(noDepthV) & texHeightMask;
                        SoftTexel texel = curTexPixels[texY * curTexWidth + texX];
                        if (texel & SOFT_TEXEL_A_MASK) {
                            StoreRasterGroup(x, y,
                                SoftPixel_FromTexelTint(texel, r1, g1, b1));
                        }
                    }
                    noDepthW += step_w;
                    noDepthU += step_u;
                    noDepthV += step_v;
                }
            } else {
                for (x = spanMinX; x <= spanMaxX; x++) {
                    if (noDepthW > 0) {
                        int texX = FixedToInt(noDepthU) & texWidthMask;
                        int texY = FixedToInt(noDepthV) & texHeightMask;
                        SoftTexel texel = curTexPixels[texY * curTexWidth + texX];
                        StoreRasterGroup(x, y,
                            SoftPixel_FromTexelTint(texel, r1, g1, b1));
                    }
                    noDepthW += step_w;
                    noDepthU += step_u;
                    noDepthV += step_v;
                }
            }
            continue;
        }
#if defined CC_BUILD_SOFTFP_W_DEPTH8 && defined SOFTFP_LOWRES_COLOR_STAGE
        if (fastSolidDepthBlend) {
            int solidW = w_interp;
            DepthValue* solidDepth = depthBuffer + y * db_stride + spanMinX;
            SoftPixel* solidDst = SOFTFP_COLOR_ROW(y) + spanMinX;
            for (x = spanMinX; x <= spanMaxX;
                 x++, solidDepth++, solidDst++, solidW += step_w) {
                DepthValue solidZ;
                if (solidW <= 0) continue;
                solidZ = EncodeWDepth8(solidW);
                if (solidZ > *solidDepth) continue;
#ifdef CC_BUILD_SOFTFP_HALF_ALPHA
                {
                    SoftPixel dst = *solidDst;
                    *solidDst = (SoftPixel)((fastSolidPixel & dst) +
                        (((fastSolidPixel ^ dst) & 0xF7DEu) >> 1));
                }
#else
                *solidDst = BlendSoftPixel(R, G, B, A, *solidDst);
#endif
            }
            continue;
        }
#endif
#endif

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        {
            int perspectiveU = 0, perspectiveV = 0;
            int perspectiveStepU = 0, perspectiveStepV = 0;
            int perspectiveEndU = 0, perspectiveEndV = 0;
            int reciprocalEnd = 0, perspectiveRemain = 0;
            cc_bool perspectiveValid = false;
#endif

        for (x = spanMinX; x <= spanMaxX; x++) {
#if !defined CC_BUILD_SOFTFP_PERSPECTIVE4 && !defined CC_BUILD_SOFTFP_W_DEPTH && \
    (!defined CC_BUILD_SOFTFP_AFFINE || !defined CC_BUILD_SOFTFP_Z16_TILES)
            int reciprocalW = 0;
#endif
#ifdef CC_BUILD_SOFTFP_Z16_TILES
            DepthValue z;
            int depthX = 0;
#else
#if defined CC_BUILD_SOFTFP_Z32_FASTCLEAR || defined CC_BUILD_SOFTFP_W_DEPTH8
            DepthValue z;
#ifdef CC_BUILD_SOFTFP_Z32_FASTCLEAR
            int zSigned, db_index = y * db_stride + x;
#else
            int db_index = y * db_stride + x;
#endif
#else
            int z, db_index = y * db_stride + x;
#endif
#endif

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
            if (w_interp <= 0) goto next_pixel_3d;
            if (perspective && !perspectiveRemain) {
                int offset = x & (perspectiveInterval - 1);
                int segment = min(perspectiveInterval - offset, spanMaxX - x + 1);
                int baseW = w_interp - step_w * offset;
                int endW  = baseW + step_w * perspectiveInterval;
                int baseReciprocal, endReciprocal;
                int baseU, baseV, endU, endV;
                int baseUOverW = u_interp - step_u * offset;
                int baseVOverW = v_interp - step_v * offset;
                int endUOverW  = baseUOverW + step_u * perspectiveInterval;
                int endVOverW  = baseVOverW + step_v * perspectiveInterval;

                /* An aligned point can fall just outside a clipped triangle.
                 * If it crosses the near plane, restart the short segment at
                 * the first covered pixel instead. */
                if (baseW <= 0) {
                    offset = 0;
                    segment = min(perspectiveInterval, spanMaxX - x + 1);
                    baseW = w_interp;
                    endW  = baseW + step_w * perspectiveInterval;
                    baseUOverW = u_interp;
                    baseVOverW = v_interp;
                    endUOverW  = baseUOverW + step_u * perspectiveInterval;
                    endVOverW  = baseVOverW + step_v * perspectiveInterval;
                }

                /* Align correction spans to screen X rather than
                 * each triangle's left edge. Coplanar triangles then use the
                 * same reciprocal approximation and cannot form a diagonal
                 * texture seam where a quad was split into two triangles. */
                if (!offset && perspectiveValid) {
                    baseReciprocal = reciprocalEnd;
                    baseU = perspectiveEndU;
                    baseV = perspectiveEndV;
                } else {
                    baseReciprocal = FixedReciprocalFast(baseW);
#ifdef CC_BUILD_ESP32S3
                    perspectiveReciprocalsLocal++;
#endif
                    baseU = FixedMul(baseUOverW, baseReciprocal);
                    baseV = FixedMul(baseVOverW, baseReciprocal);
                }
                if (endW > 0) {
                    endReciprocal = FixedReciprocalFast(endW);
#ifdef CC_BUILD_ESP32S3
                    perspectiveReciprocalsLocal++;
#endif
                } else {
                    endReciprocal = baseReciprocal;
                }
                endU = FixedMul(endUOverW, endReciprocal);
                endV = FixedMul(endVOverW, endReciprocal);

                /* Standard PS1-era span correction: perform perspective
                 * divides only at adaptive boundaries, then affine-step U/V
                 * within the short span. This removes two 64-bit multiplies
                 * from every corrected pixel. */
                perspectiveStepU = (endU - baseU) >> perspectiveShift;
                perspectiveStepV = (endV - baseV) >> perspectiveShift;
                perspectiveU = baseU + perspectiveStepU * offset;
                perspectiveV = baseV + perspectiveStepV * offset;
                perspectiveEndU = endU;
                perspectiveEndV = endV;
                reciprocalEnd = endReciprocal;
                perspectiveRemain = segment;
                perspectiveValid  = true;
            }
#elif !defined CC_BUILD_SOFTFP_W_DEPTH && (!defined CC_BUILD_SOFTFP_AFFINE || !defined CC_BUILD_SOFTFP_Z16_TILES)
            if (w_interp == 0) goto next_pixel_3d;
            reciprocalW = FixedReciprocalFast(w_interp);
#elif defined CC_BUILD_SOFTFP_W_DEPTH
            if (w_interp <= 0) goto next_pixel_3d;
#endif
#ifdef CC_BUILD_SOFTFP_Z16_TILES
            if (z_interp < 0) goto next_pixel_3d;
            z = z_interp >= (int)DEPTH_CLEAR_VALUE ? (DepthValue)(DEPTH_CLEAR_VALUE - 1) : (DepthValue)z_interp;
            if ((depthTest || depthWrite) && (!depthRow || x >= depthNextX)) {
                depthRow = DepthGetRow(y, x, &depthNextX, &depthLineIndex, &depthXMask);
            }
            depthX = x & depthXMask;
            if (depthTest && z > depthRow[depthX]) goto next_pixel_3d;
#else
#ifdef CC_BUILD_SOFTFP_W_DEPTH8
            z = EncodeWDepth8(w_interp);
#elif defined CC_BUILD_SOFTFP_W_DEPTH
            /* For this perspective projection, interpolated 1/W is strictly
             * monotonic with camera depth. Invert it so the existing smaller-
             * is-nearer comparison remains unchanged, without a pixel divide. */
            z = INT_MAX - w_interp;
#elif defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
            zSigned = FixedMul(z_interp, reciprocalW);
            if (zSigned < 0) goto next_pixel_3d;
            z = (DepthValue)zSigned;
#else
            z = FixedMul(z_interp, reciprocalW);
            if (z < 0) goto next_pixel_3d;
#endif
            if (depthTest && z > depthBuffer[db_index]) goto next_pixel_3d;
#endif

#if defined CC_BUILD_SOFTFP_FAST_CUTOUT && !defined SOFTFP_FAST_TERRAIN_KERNEL
            if (fastCutout) {
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
                int fastU = perspective ? perspectiveU : u_interp;
                int fastV = perspective ? perspectiveV : v_interp;
#elif defined CC_BUILD_SOFTFP_AFFINE
                int fastU = u_interp, fastV = v_interp;
#else
                int fastU = FixedMul(u_interp, reciprocalW);
                int fastV = FixedMul(v_interp, reciprocalW);
#endif
                int fastTexX = FixedToInt(fastU) & texWidthMask;
                int fastTexY = FixedToInt(fastV) & texHeightMask;
                SoftTexel fastTexel = curTexPixels[fastTexY * curTexWidth + fastTexX];
#ifndef SOFTFP_GROUPED_RASTER
                int fastCbIndex = y * cb_stride + x;
#endif

                if (!(fastTexel & SOFT_TEXEL_A_MASK)) goto next_pixel_3d;
                depthBuffer[db_index] = z;
#ifdef SOFTFP_GROUPED_RASTER
                {
                    SoftPixel fastColor = SoftPixel_FromTexelTint(fastTexel, r1, g1, b1);
#ifdef CC_BUILD_SOFTFP_FOG4
                    fastColor = ApplyFog4(fastColor, w_interp);
#endif
                    StoreRasterGroup(x, y, fastColor);
                }
#else
                {
                    SoftPixel fastColor = SoftPixel_FromTexelTint(fastTexel, r1, g1, b1);
#ifdef CC_BUILD_SOFTFP_FOG4
                    fastColor = ApplyFog4(fastColor, w_interp);
#endif
                    colorBuffer[fastCbIndex] = fastColor;
                }
#endif
                goto next_pixel_3d;
            }
#endif

            if (!colWrite) {
                if (depthWrite) {
#ifdef CC_BUILD_SOFTFP_Z16_TILES
                    depthRow[depthX] = z; DepthMarkDirty(depthLineIndex);
#else
                    depthBuffer[db_index] = z;
#endif
                }
                goto next_pixel_3d;
            }

            {
                int Rloc = R, Gloc = G, Bloc = B, Aloc = A;
#ifndef SOFTFP_GROUPED_RASTER
                int cb_index = y * cb_stride + x;
#endif
                if (texturing) {
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
                    int u = perspective ? perspectiveU : u_interp;
                    int v = perspective ? perspectiveV : v_interp;
#elif defined CC_BUILD_SOFTFP_AFFINE
                    int u = u_interp, v = v_interp;
#else
                    int u = FixedMul(u_interp, reciprocalW);
                    int v = FixedMul(v_interp, reciprocalW);
#endif
                    int texX = FixedToInt(u) & texWidthMask;
                    int texY = FixedToInt(v) & texHeightMask;
                    int texIndex;
                    SoftTexel tColor;
#ifdef CC_BUILD_SOFTFP_TEXEL_2X2
                    cc_bool texelReused;
                    texX &= ~1; texY &= ~1;
                    texIndex = texY * curTexWidth + texX;
                    texelReused = texIndex == lastTexIndex;
                    if (texelReused) {
                        tColor = lastTexel;
                        texReuseLocal++;
                    } else {
                        tColor = curTexPixels[texIndex];
                        lastTexIndex = texIndex;
                        lastTexel = tColor;
                        texLoadsLocal++;
                    }
#else
                    texIndex = texY * curTexWidth + texX;
                    tColor = curTexPixels[texIndex];
#endif
#ifdef CC_BUILD_SOFTFP_TEX16
                    Aloc = (tColor & SOFT_TEXEL_A_MASK) ? a1 : 0;
                    if (gfx_alphaTest && Aloc < 0x80) goto next_pixel_3d;
                    if (gfx_alphaBlend && Aloc == 0) goto next_pixel_3d;
#ifdef CC_BUILD_SOFTFP_FB16
                    if (!gfx_alphaBlend) {
                        if (depthWrite) {
#ifdef CC_BUILD_SOFTFP_Z16_TILES
                            depthRow[depthX] = z; DepthMarkDirty(depthLineIndex);
#else
                            depthBuffer[db_index] = z;
#endif
                        }
#ifdef CC_BUILD_SOFTFP_TEXEL_2X2
                        if (!texelReused) lastOpaquePixel = SoftPixel_FromTexelTint(tColor, r1, g1, b1);
#ifdef SOFTFP_GROUPED_RASTER
                        StoreRasterGroup(x, y, lastOpaquePixel);
#else
                        colorBuffer[cb_index] = lastOpaquePixel;
#endif
#else
#ifdef SOFTFP_GROUPED_RASTER
                        {
                            SoftPixel opaque = SoftPixel_FromTexelTint(tColor, r1, g1, b1);
                            StoreRasterGroup(x, y, opaque);
                        }
#else
                        colorBuffer[cb_index] = SoftPixel_FromTexelTint(tColor, r1, g1, b1);
#endif
#endif
                        goto next_pixel_3d;
                    }
#endif
#else
                    Aloc = (a1 * SoftTexel_A(tColor)) >> 8;
#endif
                    Rloc = (r1 * SoftTexel_R(tColor)) >> 8;
                    Gloc = (g1 * SoftTexel_G(tColor)) >> 8;
                    Bloc = (b1 * SoftTexel_B(tColor)) >> 8;
                }
                if (gfx_alphaTest && Aloc < 0x80) goto next_pixel_3d;
                if (gfx_alphaBlend && Aloc == 0) goto next_pixel_3d;

                if (depthWrite) {
#ifdef CC_BUILD_SOFTFP_Z16_TILES
                    depthRow[depthX] = z; DepthMarkDirty(depthLineIndex);
#else
                    depthBuffer[db_index] = z;
#endif
                }
                if (!gfx_alphaBlend) {
#ifdef SOFTFP_GROUPED_RASTER
                    SoftPixel opaque = SoftPixel_Make(Rloc, Gloc, Bloc);
                    StoreRasterGroup(x, y, opaque);
#else
                    colorBuffer[cb_index] = SoftPixel_Make(Rloc, Gloc, Bloc);
#endif
                } else {
#ifdef SOFTFP_GROUPED_RASTER
                    BlendRasterGroup(x, y, Rloc, Gloc, Bloc, Aloc);
#else
                    colorBuffer[cb_index] = BlendSoftPixel(
                        Rloc, Gloc, Bloc, Aloc, colorBuffer[cb_index]);
#endif
                }
            }

next_pixel_3d:
#ifndef CC_BUILD_SOFTFP_W_DEPTH
            z_interp += step_z;
#endif
#if !defined CC_BUILD_SOFTFP_AFFINE || defined CC_BUILD_SOFTFP_PERSPECTIVE4 || !defined CC_BUILD_SOFTFP_Z16_TILES
            w_interp += step_w;
#endif
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
            if (perspective && perspectiveRemain) {
                perspectiveU += perspectiveStepU;
                perspectiveV += perspectiveStepV;
                perspectiveRemain--;
            }
#endif
            if (texturing) {
                u_interp += step_u;
                v_interp += step_v;
            }
        }
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        }
#endif
    }
#ifdef CC_BUILD_SOFTFP_TEXEL_2X2
    textureSampleReuse += texReuseLocal;
    textureSampleLoads += texLoadsLocal;
#endif
#ifdef CC_BUILD_ESP32S3
    rasterSpanPixels += spanPixelsLocal;
    rasterPerspectiveTriangles += perspectiveTrianglesLocal;
    rasterPerspectivePixels += perspectivePixelsLocal;
    rasterPerspectiveReciprocals += perspectiveReciprocalsLocal;
#endif
}

static CC_INLINE void DrawTriangle3D(VertexFixed* V0, VertexFixed* V1,
                                     VertexFixed* V2, int area, int factor,
                                     int perspectiveShift) {
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    if (SoftFP_FastTerrainState() && PackedCol_A(V0->c) >= 0x80) {
#ifdef CC_BUILD_ESP32S3
        DrawPolygonFastTerrainAffine(V0, V1, V2, NULL, 3, area, factor,
                                     SOFTFP_MAP_MODE_OPAQUE, perspectiveShift);
#else
        DrawTriangleFastTerrain(V0, V1, V2, area, factor, perspectiveShift);
#endif
        return;
    }
#endif
    DrawTriangle3DGeneric(V0, V1, V2, area, factor, perspectiveShift);
}


static void DrawTriangle2D(VertexFixed* V0, VertexFixed* V1, VertexFixed* V2) {
    int x0_fp = V0->x, y0_fp = V0->y;
    int x1_fp = V1->x, y1_fp = V1->y;
    int x2_fp = V2->x, y2_fp = V2->y;

    int minX = FixedToInt(min(x0_fp, min(x1_fp, x2_fp)));
    int minY = FixedToInt(min(y0_fp, min(y1_fp, y2_fp)));
    int maxX = FixedToInt(max(x0_fp, max(x1_fp, x2_fp)));
    int maxY = FixedToInt(max(y0_fp, max(y1_fp, y2_fp)));

    if (maxX < 0 || minX > fb_maxX) return;
    if (maxY < 0 || minY > fb_maxY) return;

    minX = max(minX, 0); maxX = min(maxX, fb_maxX);
    minY = max(minY, 0); maxY = min(maxY, fb_maxY);

    int u0 = FixedMul(V0->u, IntToFixed(curTexWidth));
    int v0 = FixedMul(V0->v, IntToFixed(curTexHeight));
    int u1 = FixedMul(V1->u, IntToFixed(curTexWidth));
    int v1 = FixedMul(V1->v, IntToFixed(curTexHeight));
    int u2 = FixedMul(V2->u, IntToFixed(curTexWidth));
    int v2 = FixedMul(V2->v, IntToFixed(curTexHeight));
    PackedCol color = V0->c;
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
    cc_bool fastA1Textured = gfx_format == VERTEX_FORMAT_TEXTURED &&
                             gfx_alphaBlend && PackedCol_A(color) == 255;
    int fastTintR = PackedCol_R(color), fastTintG = PackedCol_G(color);
    int fastTintB = PackedCol_B(color);
#endif

    int area = edgeFunctionFixed(x0_fp,y0_fp, x1_fp,y1_fp, x2_fp,y2_fp);
    if (area == 0) return;
    int factor = FixedReciprocalFast(area);

    int dx01  = y0_fp - y1_fp, dy01 = x1_fp - x0_fp;
    int dx12  = y1_fp - y2_fp, dy12 = x2_fp - x1_fp;
    int dx20  = y2_fp - y0_fp, dy20 = x0_fp - x2_fp;

    int minX_fp = IntToFixed(minX) + FP_HALF;
    int minY_fp = IntToFixed(minY) + FP_HALF;

    int bc0_start = edgeFunctionFixed(x1_fp,y1_fp, x2_fp,y2_fp, minX_fp,minY_fp);
    int bc1_start = edgeFunctionFixed(x2_fp,y2_fp, x0_fp,y0_fp, minX_fp,minY_fp);
    int bc2_start = edgeFunctionFixed(x0_fp,y0_fp, x1_fp,y1_fp, minX_fp,minY_fp);

    int x, y;
    for (y = minY; y <= maxY; y++, bc0_start += dy12, bc1_start += dy20, bc2_start += dy01) 
    {
        int bc0 = bc0_start;
        int bc1 = bc1_start;
        int bc2 = bc2_start;

        for (x = minX; x <= maxX; x++, bc0 += dx12, bc1 += dx20, bc2 += dx01) 
        {
            int ic0 = FixedMul(bc0, factor);
            int ic1 = FixedMul(bc1, factor);
            int ic2 = FixedMul(bc2, factor);

            if (ic0 < 0 || ic1 < 0 || ic2 < 0) continue;
            int cb_index = y * cb_stride + x;

            int R, G, B, A;
            if (gfx_format == VERTEX_FORMAT_TEXTURED) {
                int u = FixedMul(ic0, u0) + FixedMul(ic1, u1) + FixedMul(ic2, u2);
                int v = FixedMul(ic0, v0) + FixedMul(ic1, v1) + FixedMul(ic2, v2);
                int texX = FixedToInt(u) & texWidthMask;
                int texY = FixedToInt(v) & texHeightMask;
                int texIndex = texY * curTexWidth + texX;

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_TEX16 && defined CC_BUILD_SOFTFP_FB16
                if (fastA1Textured) {
                    SoftTexel texel = curTexPixels[texIndex];
                    SoftPixel output;
                    /* A1 texture alpha with vertex alpha 255 becomes either
                     * 0 or 254 in the generic path. The latter is copied, not
                     * blended. This RGB565 expression is bit-identical to its
                     * 8-bit expand, tint, and repack sequence. */
                    if (!(texel & SOFT_TEXEL_A_MASK)) continue;
                    output = SoftPixel_FromTexelTint(
                        texel, fastTintR, fastTintG, fastTintB);
                    colorBuffer[cb_index] = output;
                    SoftFP_HotbarCacheCapture((cc_uint32)cb_index, output, texY);
                    continue;
                }
#endif
                BitmapCol tColor = SoftTexel_ToBitmap(curTexPixels[texIndex]);
                int a1 = PackedCol_A(color), a2 = BitmapCol_A(tColor);
                A = ( a1 * a2 ) >> 8;
                int r1 = PackedCol_R(color), r2 = BitmapCol_R(tColor);
                R = ( r1 * r2 ) >> 8;
                int g1 = PackedCol_G(color), g2 = BitmapCol_G(tColor);
                G = ( g1 * g2 ) >> 8;
                int b1 = PackedCol_B(color), b2 = BitmapCol_B(tColor);
                B = ( b1 * b2 ) >> 8;
            } else {
                R = PackedCol_R(color);
                G = PackedCol_G(color);
                B = PackedCol_B(color);
                A = PackedCol_A(color);
            }

            if (gfx_alphaTest && A < 0x80) continue;
            if (gfx_alphaBlend && A == 0)  continue;

            if (gfx_alphaBlend && A < 0xFE) {
                SoftPixel dst = colorBuffer[cb_index];
                int dstR = SoftPixel_R(dst);
                int dstG = SoftPixel_G(dst);
                int dstB = SoftPixel_B(dst);

#ifdef CC_BUILD_SOFTFP_HALF_ALPHA
                R = (R + dstR) >> 1;
                G = (G + dstG) >> 1;
                B = (B + dstB) >> 1;
#else
                R = (R * A + dstR * (255 - A)) >> 8;
                G = (G * A + dstG * (255 - A)) >> 8;
                B = (B * A + dstB * (255 - A)) >> 8;
#endif
            }

            colorBuffer[cb_index] = SoftPixel_Make(R, G, B);
        }
    }
}


static void ProcessClippedTriangleAndDraw(VertexFixed* inVerts, int polyCount,
                                          cc_bool optimizeMap, cc_bool forceAffine) {
    int i;
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    int fastMapMode = optimizeMap ? SoftFP_FastMapMode() : SOFTFP_MAP_MODE_NONE;
    if (optimizeMap && (cc_uint32)fastMapMode < 4u) {
        rasterMapModeCounts[fastMapMode]++;
    }
#endif
#ifdef CC_BUILD_ESP32S3
    int perspectiveShift = -1;
    cc_uint32 projectStart = optimizeMap ? SoftFP_ReadCycles() : 0;
#else
    int perspectiveShift = forceAffine ? -1 : 2;
#endif
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    int minW = INT_MAX;
#endif
    if (polyCount < 3) return;

    /* Project each unique polygon vertex once. Depth-only map quads can stay
     * quads; textured polygons are emitted as a triangle fan.
     * The old path repeated the reciprocal-W viewport transform six times
     * per quad, and repeated another six reciprocals when culling was active. */
    for (i = 0; i < polyCount; i++) {
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_LOWRES_RASTER && defined SOFTFP_FAST_TERRAIN_KERNEL
        if (fastMapMode == SOFTFP_MAP_MODE_OPAQUE) {
            if (!ViewportMapOpaqueVertex3D(&inVerts[i])) return;
        } else
#endif
        if (!ViewportVertex3D(&inVerts[i])) return;
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        if (inVerts[i].w < minW) minW = inVerts[i].w;
#endif
    }
#ifdef CC_BUILD_ESP32S3
    if (optimizeMap) rasterMapProjectCycles += SoftFP_ReadCycles() - projectStart;
#endif

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
#ifdef CC_BUILD_ESP32S3
    if (optimizeMap && fastMapMode == SOFTFP_MAP_MODE_OPAQUE && !forceAffine) {
        perspectiveShift = SoftFP_SelectTerrainPerspectiveShift(inVerts, polyCount);
    }
#else
    if (optimizeMap && !forceAffine) perspectiveShift = 0;
#endif
#endif

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    {
    if (polyCount == 4 && fastMapMode != SOFTFP_MAP_MODE_NONE &&
        (fastMapMode != SOFTFP_MAP_MODE_OPAQUE ||
         PackedCol_A(inVerts[0].c) >= 0x80)) {
        VertexFixed* A = &inVerts[0];
        VertexFixed* B = &inVerts[1];
        VertexFixed* C = &inVerts[2];
        VertexFixed* D = &inVerts[3];

        {
        cc_uint32 hizStart = SoftFP_ReadCycles();
        cc_bool occluded = SoftFP_TerrainQuadOccluded(inVerts);
        rasterMapHiZCycles += SoftFP_ReadCycles() - hizStart;
        if (occluded) {
            rasterCurrentHiZCull++;
            return;
        }
        }

        int area = SoftFP_RasterEdge(
            SOFTFP_RASTER_COORD(A->x), SOFTFP_RASTER_COORD(A->y),
            SOFTFP_RASTER_COORD(B->x), SOFTFP_RASTER_COORD(B->y),
            SOFTFP_RASTER_COORD(C->x), SOFTFP_RASTER_COORD(C->y));
        int area2 = SoftFP_RasterEdge(
            SOFTFP_RASTER_COORD(A->x), SOFTFP_RASTER_COORD(A->y),
            SOFTFP_RASTER_COORD(C->x), SOFTFP_RASTER_COORD(C->y),
            SOFTFP_RASTER_COORD(D->x), SOFTFP_RASTER_COORD(D->y));

        /* Coverage and reciprocal depth form one plane across a block face, so
         * an untextured depth-only quad can use the direct polygon path. Raw
         * affine U/V do not: after perspective projection the fourth corner
         * of a trapezoid generally differs from the plane fitted through the
         * first three corners. Textured quads must therefore use the two
         * triangles below so V3's U/V participates in the second half. */
        if (ABS(area) > SOFTFP_RASTER_FP_ONE &&
            ABS(area2) > SOFTFP_RASTER_FP_ONE &&
            ((area ^ area2) >= 0)) {
            if (faceCulling && area > 0) return;
#ifdef CC_BUILD_ESP32S3
            rasterTerrainQuads++;
            if (fastMapMode == SOFTFP_MAP_MODE_DEPTH_ONLY ||
                fastMapMode == SOFTFP_MAP_MODE_HALF_BLEND) {
                int factor = FixedReciprocalFast(area);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
                if (!perspectiveShift) {
                    perspectiveShift = SoftFP_SelectPerspectiveShift(A, B, C, factor, minW);
                }
#endif
                DrawPolygonFastTerrainAffine(A, B, C, D, 4, area, factor,
                                             fastMapMode, -1);
                return;
            }
#else
            {
                int factor = FixedReciprocalFast(area);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
                if (!perspectiveShift) {
                    perspectiveShift = SoftFP_SelectPerspectiveShift(A, B, C, factor, minW);
                }
#endif
                DrawQuadFastTerrain(inVerts, area, factor, perspectiveShift);
                return;
            }
#endif
        }
    }
    }
#endif

#ifdef CC_BUILD_ESP32S3
    rasterTriangleCount += (cc_uint32)(polyCount - 2);
#endif

    for (i = 1; i + 1 < polyCount; i++) {
        VertexFixed* A = &inVerts[0];
        VertexFixed* B = &inVerts[i];
        VertexFixed* C = &inVerts[i + 1];
        int area = SoftFP_RasterEdge(
            SOFTFP_RASTER_COORD(A->x), SOFTFP_RASTER_COORD(A->y),
            SOFTFP_RASTER_COORD(B->x), SOFTFP_RASTER_COORD(B->y),
            SOFTFP_RASTER_COORD(C->x), SOFTFP_RASTER_COORD(C->y));
        int factor;

        /* Viewport Y is inverted, therefore its winding sign is the opposite
         * of the pre-viewport NDC sign used by the original culling path. */
        if (faceCulling && area > 0) continue;
        if (ABS(area) <= SOFTFP_RASTER_FP_ONE) continue;

        factor = FixedReciprocalFast(area);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        /* One interval is shared by every triangle produced from this quad.
         * Combined with screen-X alignment this preserves a seam-free mapping. */
        if (!perspectiveShift) {
            perspectiveShift = SoftFP_SelectPerspectiveShift(A, B, C, factor, minW);
        }
#endif
#ifdef CC_BUILD_ESP32S3
        /* Near-camera stretched water quads are commonly clipped into 3-6
         * vertices. Their triangle fan used to fall back to the generic
         * full-pixel blender, which dominated the translucent pass and used a
         * different shared-edge rule from intact quads. Keep every resulting
         * triangle in the same affine, screen-door, exact-Z8 terrain kernel. */
        if (fastMapMode == SOFTFP_MAP_MODE_HALF_BLEND) {
            DrawPolygonFastTerrainAffine(A, B, C, NULL, 3, area, factor,
                                         fastMapMode, -1);
            continue;
        }
#endif
        DrawTriangle3D(A, B, C, area, factor, perspectiveShift);
    }
}


static int PlaneDistFixed(const VertexFixed* v, int plane) {
    int64_t result;
    switch (plane) {
    case PLANE_LEFT:
        result = (int64_t)v->x + v->w;
        break;
    case PLANE_RIGHT:
        result = (int64_t)v->w - v->x;
        break;
    case PLANE_BOTTOM:
        result = (int64_t)v->y + v->w;
        break;
    case PLANE_TOP:
        result = (int64_t)v->w - v->y;
        break;
    case PLANE_NEAR:
        result = v->z;
        break;
    case PLANE_FAR:
        result = (int64_t)v->w - v->z;
        break;
    default:
        return 0;
    }
    
    // Clamp to valid range
    if (result > INT_MAX) return INT_MAX;
    if (result < INT_MIN) return INT_MIN;
    return (int)result;
}

static void LerpClipFixed(VertexFixed* out, const VertexFixed* a, const VertexFixed* b, int t) {
    if (t < 0) t = 0;
    if (t > FP_ONE) t = FP_ONE;
    
    int invt = FP_ONE - t;
    
    int64_t x_interp = ((int64_t)invt * a->x + (int64_t)t * b->x) >> FP_SHIFT;
    int64_t y_interp = ((int64_t)invt * a->y + (int64_t)t * b->y) >> FP_SHIFT;
    int64_t z_interp = ((int64_t)invt * a->z + (int64_t)t * b->z) >> FP_SHIFT;
    int64_t w_interp = ((int64_t)invt * a->w + (int64_t)t * b->w) >> FP_SHIFT;
    int64_t u_interp = ((int64_t)invt * a->u + (int64_t)t * b->u) >> FP_SHIFT;
    int64_t v_interp = ((int64_t)invt * a->v + (int64_t)t * b->v) >> FP_SHIFT;
    
    // Clamp results
    out->x = (x_interp > INT_MAX) ? INT_MAX : (x_interp < INT_MIN) ? INT_MIN : (int)x_interp;
    out->y = (y_interp > INT_MAX) ? INT_MAX : (y_interp < INT_MIN) ? INT_MIN : (int)y_interp;
    out->z = (z_interp > INT_MAX) ? INT_MAX : (z_interp < INT_MIN) ? INT_MIN : (int)z_interp;
    out->w = (w_interp > INT_MAX) ? INT_MAX : (w_interp < INT_MIN) ? INT_MIN : (int)w_interp;
    out->u = (u_interp > INT_MAX) ? INT_MAX : (u_interp < INT_MIN) ? INT_MIN : (int)u_interp;
    out->v = (v_interp > INT_MAX) ? INT_MAX : (v_interp < INT_MIN) ? INT_MIN : (int)v_interp;
    
    out->c = (t < FP_HALF) ? a->c : b->c;
}

static int SafeFixedDiv(int numerator, int denominator) {
    if (denominator == 0) return FP_HALF;
    if (ABS(denominator) < 16) return FP_HALF; // Avoid extreme divisions
    
    int64_t result = ((int64_t)numerator << FP_SHIFT) / denominator;
    
    if (result > FP_ONE) return FP_ONE;
    if (result < 0) return 0;
    return (int)result;
}

static int ClipPolygonPlaneFixed(const VertexFixed* in, int inCount, VertexFixed* out, int plane) {
    int outCount = 0;
    
	// https://en.wikipedia.org/wiki/Sutherland%E2%80%93Hodgman_algorithm
    for (int i = 0; i < inCount; i++) 
	{
        const VertexFixed* cur  = &in[i];
        const VertexFixed* next = &in[(i + 1) % inCount];
        
        int dCur  = PlaneDistFixed(cur,  plane);
        int dNext = PlaneDistFixed(next, plane);
        cc_bool curIn  = dCur >= 0;
        cc_bool nextIn = dNext >= 0;
        
        if (curIn && nextIn) {
            // Both inside
            out[outCount++] = *next;
        } else if (curIn && !nextIn) {
            // Exiting
            int denom = dCur - dNext;
            int t = SafeFixedDiv(dCur, denom);
            
            VertexFixed intersection;
            LerpClipFixed(&intersection, cur, next, t);
            out[outCount++] = intersection;
        } else if (!curIn && nextIn) {
            // Entering
            int denom = dCur - dNext;
            int t = SafeFixedDiv(dCur, denom);
            
            VertexFixed intersection;
            LerpClipFixed(&intersection, cur, next, t);
            out[outCount++] = intersection;
            out[outCount++] = *next;
        }
        // Both outside
        
        if (outCount >= 15) break; // Safety limit
    }
    
    return outCount;
}

static int ClipQuadToFrustumFixed(const VertexFixed quad[4], VertexFixed buf1[16], VertexFixed buf2[16],
                                  VertexFixed** outPoly, cc_uint32 planeMask) {
    VertexFixed* src = buf1;
    VertexFixed* dst = buf2;

    for (int i = 0; i < 4; i++) src[i] = quad[i];
    int count = 4;

    // check w zero
    for (int i = 0; i < 4; i++) if (src[i].w == 0) return 0;

    for (int plane = 0; plane < NUM_PLANES; plane++) 
	{
        if (!(planeMask & (1u << plane))) continue;
        int newCount = ClipPolygonPlaneFixed(src, count, dst, plane);
        if (newCount == 0) return 0;

        // swap lists
        VertexFixed* tmp = src; src = dst; dst = tmp;
        count = newCount;
        if (count > 15) count = 15; // TODO prob not needed?
    }

    *outPoly = src;
    return count;
}

static CC_INLINE cc_uint32 VertexClipOutcode(const VertexFixed* vertex,
                                             cc_uint32 planeMask) {
    cc_uint32 code = 0;
    while (planeMask) {
        int plane = __builtin_ctz(planeMask);
        planeMask &= planeMask - 1u;
        if (PlaneDistFixed(vertex, plane) < 0) code |= 1u << plane;
    }
    return code;
}

static void DrawClippedFixed(VertexFixed quad[4], cc_uint32 planeMask,
                             cc_bool optimizeMap, cc_bool forceAffine) {
	VertexFixed buf1[16], buf2[16];
    VertexFixed* outPoly;
    cc_uint32 clipOr = 0, clipAnd = planeMask;
    cc_uint32 clipStart;
    int i;

    if (!planeMask) {
        ProcessClippedTriangleAndDraw(quad, 4, optimizeMap, forceAffine);
        return;
    }
	clipStart = SoftFP_ReadCycles();

    for (i = 0; i < 4; i++) {
        cc_uint32 code = VertexClipOutcode(&quad[i], planeMask);
        clipOr  |= code;
        clipAnd &= code;
    }
    if (clipAnd) {
		rasterMapClipCycles += SoftFP_ReadCycles() - clipStart;
		return;
	}
    if (!clipOr) {
		rasterMapClipCycles += SoftFP_ReadCycles() - clipStart;
        ProcessClippedTriangleAndDraw(quad, 4, optimizeMap, forceAffine);
        return;
    }

    int polyCount = ClipQuadToFrustumFixed(quad, buf1, buf2, &outPoly, clipOr);
	rasterMapClipCycles += SoftFP_ReadCycles() - clipStart;
    if (polyCount > 0) {
        ProcessClippedTriangleAndDraw(outPoly, polyCount, optimizeMap, forceAffine);
    }
}

void DrawQuadsFixed(int startVertex, int verticesCount, DrawHints hints) {
    VertexFixed vertices[4];
    int i, j = startVertex;
#ifdef CC_BUILD_ESP32S3
    cc_uint64 perfMark = Stopwatch_Measure();
    cc_bool perfWas2D = gfx_rendering2D;
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    cc_uint32 spriteCullLocal = 0;
    cc_uint32 frustumCullLocal = 0, temporalCullLocal = 0;
    cc_uint32 backfaceCullLocal = 0;
    cc_uint32 frustumGroupCullLocal = 0;
#endif
    holo_perf_count_quads((uint32_t)(verticesCount / 4));
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    if (!gfx_rendering2D && (hints & SOFTFP_MAP_BATCH_HINT) &&
        (hints & DRAW_HINT_SPRITE)) {
        rasterSpriteQuads += (cc_uint32)(verticesCount / 4);
    }
#endif
#endif

    if (gfx_rendering2D && (hints & (DRAW_HINT_SPRITE|DRAW_HINT_RECT))) {
        for (i = 0; i < verticesCount / 4; i++, j += 4) {
            TransformVertex2D(j + 0, &vertices[0]);
            TransformVertex2D(j + 1, &vertices[1]);
            TransformVertex2D(j + 2, &vertices[2]);
            DrawSprite2D(&vertices[0], &vertices[1], &vertices[2]);
        }
    } else if (gfx_rendering2D) {
        for (i = 0; i < verticesCount / 4; i++, j += 4) {
            TransformVertex2D(j + 0, &vertices[0]);
            TransformVertex2D(j + 1, &vertices[1]);
            TransformVertex2D(j + 2, &vertices[2]);
            TransformVertex2D(j + 3, &vertices[3]);
            DrawTriangle2D(&vertices[0], &vertices[2], &vertices[1]);
            DrawTriangle2D(&vertices[2], &vertices[0], &vertices[3]);
        }
    } else {
		cc_uint32 clipPlaneMask = (1u << NUM_PLANES) - 1u;
		cc_bool optimizeMap = false;
		cc_bool forceAffine = (hints & DRAW_HINT_AFFINE) != 0;
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
		optimizeMap = (hints & SOFTFP_MAP_BATCH_HINT) != 0;
		if (optimizeMap) {
			clipPlaneMask = (hints & DRAW_HINT_NOCLIP) ? 0u : mapClipPlanes;
		}
#endif
		/* Packed map buffers are immutable; their U/V offset is applied while
		 * decoding each vertex instead of rewriting the compressed stream. */
		if (tex_offseting && !optimizeMap) ShiftTextureCoords(verticesCount);
		{
			int step = 4;
			if (hints & DRAW_HINT_REVERSE) {
				j = startVertex + verticesCount - 4;
				step = -4;
			}

        for (i = 0; i < verticesCount / 4; i++, j += step) {
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
            cc_uint8 quadClipPlaneMask = (cc_uint8)clipPlaneMask;
#endif
#ifdef CC_BUILD_ESP32S3
            cc_uint32 mapPhaseStart = optimizeMap ? SoftFP_ReadCycles() : 0;
            cc_uint32 mapPhaseMark;
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
            if (optimizeMap && mapClipPlanes &&
                i + 8 <= verticesCount / 4) {
                cc_uint32 quadIndex = (cc_uint32)j >> 2;
                cc_bool aligned = step > 0 ? !(quadIndex & 7u) :
                                             (quadIndex & 7u) == 7u;
                if (aligned) {
                    if (SoftFP_MapGroupOutsideFrustum(j, mapClipPlanes)) {
                        if (hints & DRAW_HINT_SPRITE) spriteCullLocal += 8;
                        frustumCullLocal += 8;
                        frustumGroupCullLocal += 8;
                        i += 7;
                        j += step * 7;
#ifdef CC_BUILD_ESP32S3
                        rasterMapCullCycles += SoftFP_ReadCycles() - mapPhaseStart;
#endif
                        continue;
                    }
                }
            }
            gfx_temporal_quad_key = 0;
            if (optimizeMap && faceCulling && !(hints & DRAW_HINT_SPRITE) &&
                SoftFP_MapQuadBackfacing(j)) {
                backfaceCullLocal++;
#ifdef CC_BUILD_ESP32S3
                rasterMapCullCycles += SoftFP_ReadCycles() - mapPhaseStart;
#endif
                continue;
            }
            if ((hints & SOFTFP_MAP_BATCH_HINT) && mapClipPlanes &&
                SoftFP_MapQuadOutsideFrustum(j, mapClipPlanes,
                                             &quadClipPlaneMask)) {
#ifdef CC_BUILD_ESP32S3
                if (hints & DRAW_HINT_SPRITE) spriteCullLocal++;
                frustumCullLocal++;
                rasterMapCullCycles += SoftFP_ReadCycles() - mapPhaseStart;
#endif
                continue;
            }
            if (optimizeMap) {
                gfx_temporal_quad_key = SoftFP_TemporalQuadKey(j);
                if (SoftFP_TemporalRectOccluded(gfx_temporal_quad_key)) {
                    gfx_temporal_quad_key = 0;
                    temporalCullLocal++;
#ifdef CC_BUILD_ESP32S3
                    rasterMapCullCycles += SoftFP_ReadCycles() - mapPhaseStart;
#endif
                    continue;
                }
            }
#ifdef CC_BUILD_ESP32S3
            if (optimizeMap && !quadClipPlaneMask) rasterClipZeroMask++;
#endif
#endif
#ifdef CC_BUILD_ESP32S3
            if (optimizeMap) {
                mapPhaseMark = SoftFP_ReadCycles();
                rasterMapCullCycles += mapPhaseMark - mapPhaseStart;
                mapPhaseStart = mapPhaseMark;
            }
#endif
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
            if (optimizeMap) {
                TransformMapQuad3D(j, vertices);
            } else
#endif
            {
                TransformVertex3D(j + 0, &vertices[0], optimizeMap);
                TransformVertex3D(j + 1, &vertices[1], optimizeMap);
                TransformVertex3D(j + 2, &vertices[2], optimizeMap);
                TransformVertex3D(j + 3, &vertices[3], optimizeMap);
            }
#ifdef CC_BUILD_ESP32S3
            if (optimizeMap) {
                mapPhaseMark = SoftFP_ReadCycles();
                rasterMapXformCycles += mapPhaseMark - mapPhaseStart;
                mapPhaseStart = mapPhaseMark;
            }
#endif
            DrawClippedFixed(vertices,
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
                             optimizeMap ? quadClipPlaneMask : clipPlaneMask,
#else
                             clipPlaneMask,
#endif
                             optimizeMap, forceAffine);
#ifdef CC_BUILD_ESP32S3
            if (optimizeMap) {
                rasterMapDrawCycles += SoftFP_ReadCycles() - mapPhaseStart;
            }
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
            gfx_temporal_quad_key = 0;
#endif
        }
		}

		if (tex_offseting && !optimizeMap) UnshiftTextureCoords(verticesCount);
    }
#ifdef CC_BUILD_ESP32S3
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    rasterSpriteCull += spriteCullLocal;
    rasterFrustumCull += frustumCullLocal;
    rasterBackfaceCull += backfaceCullLocal;
    rasterTemporalCull += temporalCullLocal;
    rasterFrustumGroupCull += frustumGroupCullLocal;
#endif
    holo_perf_add(perfWas2D ? HOLO_PERF_DRAW2D : HOLO_PERF_DRAW3D,
                  Stopwatch_ElapsedMicroseconds(perfMark, Stopwatch_Measure()));
#endif
}
void Gfx_SetVertexFormat(VertexFormat fmt) {
    gfx_format = fmt;
    gfx_stride = strideSizes[fmt];
}

void Gfx_DrawVb_Lines(int verticesCount) { } /* TODO */

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex, DrawHints hints) {
    DrawQuadsFixed(startVertex, verticesCount, hints);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
    DrawQuadsFixed(0, verticesCount, DRAW_HINT_NONE);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex, DrawHints hints) {
    /* Private bit 0x10 identifies the map-renderer-only fast cull path. */
    DrawQuadsFixed(startVertex, verticesCount, (DrawHints)(hints | SOFTFP_MAP_BATCH_HINT));
}

/*########################################################################################################################*
*---------------------------------------------------------Other/Misc------------------------------------------------------*
*#########################################################################################################################*/
static BitmapCol* CB_GetRow(struct Bitmap* bmp, int y, void* ctx) {
#ifdef CC_BUILD_SOFTFP_FB16
    static BitmapCol row[CLASSICUBE_RENDER_WIDTH];
    SoftPixel* src = colorBuffer + cb_stride * y;
    int x;
    for (x = 0; x < fb_width; x++) row[x] = SoftPixel_ToBitmap(src[x]);
    return row;
#else
    return colorBuffer + cb_stride * y;
#endif
}

cc_result Gfx_TakeScreenshot(struct Stream* output) {
    struct Bitmap bmp;
    Bitmap_Init(bmp, fb_width, fb_height, NULL);
    return Png_Encode(&bmp, output, CB_GetRow, false, NULL);
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }
cc_bool Gfx_GetUIOptions(struct MenuOptionsScreen* s) { return false; }

void Gfx_BeginFrame(void) { }

void Gfx_EndFrame(void) {
    Rect2D r = { 0, 0, fb_width, fb_height };
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    SoftFP_RasterWorkerFlush();
#endif
#ifdef CC_BUILD_ESP32S3
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    /* The just-finished hierarchy becomes an immutable previous-frame
     * occluder. The other 2.1 KiB buffer is selected for the next clear/build,
     * so this costs no per-frame copy. */
    previousTerrainHiZ4 = terrainHiZ4;
    previousTerrainHiZ  = terrainHiZ;
    previousTerrainMvpKey = terrainFrameMvpKey;
    previousTerrainMapGeneration = gfx_map_generation;
    previousTerrainHiZValid = rasterTerrainQuads != 0;
    terrainHiZBufferIndex ^= 1u;
    terrainHiZ4 = terrainHiZ4Buffers[terrainHiZBufferIndex];
    terrainHiZ  = terrainHiZBuffers[terrainHiZBufferIndex];
#endif
    holo_perf_count_triangles(rasterTriangleCount);
    holo_perf_count_bbox_pixels(rasterBboxPixels);
    holo_perf_add(HOLO_PERF_RASTER_SPAN_PIXELS, rasterSpanPixels);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_TRIANGLES, rasterPerspectiveTriangles);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_PIXELS, rasterPerspectivePixels);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_RECIPROCALS, rasterPerspectiveReciprocals);
    holo_perf_add(HOLO_PERF_RASTER_QUADS, rasterTerrainQuads);
    holo_perf_add(HOLO_PERF_OPAQUE_LUT_QUADS, rasterOpaqueLutQuads);
    holo_perf_add(HOLO_PERF_HIZ_CELL_SKIP_PIXELS, rasterHiZCellSkipPixels);
    holo_perf_add(HOLO_PERF_SPRITE_QUADS, rasterSpriteQuads);
    holo_perf_add(HOLO_PERF_SPRITE_CULL, rasterSpriteCull);
    holo_perf_add(HOLO_PERF_UI_FAST_SPRITES, rasterUiFastSprites);
    holo_perf_add(HOLO_PERF_UI_GENERIC_SPRITES, rasterUiGenericSprites);
    holo_perf_add(HOLO_PERF_UI_SPRITE_PIXELS, rasterUiSpritePixels);
    holo_perf_add(HOLO_PERF_FRUSTUM_CULL, rasterFrustumCull);
    holo_perf_add(HOLO_PERF_BACKFACE_CULL, rasterBackfaceCull);
    holo_perf_add(HOLO_PERF_TEMPORAL_CULL, rasterTemporalCull);
    holo_perf_add(HOLO_PERF_RANGE_CACHE_HIT, rasterRangeCacheHits);
    holo_perf_add(HOLO_PERF_RANGE_CACHE_MISS, rasterRangeCacheMisses);
    holo_perf_add(HOLO_PERF_FRUSTUM_GROUP_CULL, rasterFrustumGroupCull);
    /* ESP32-S3 is fixed at 240 MHz in this firmware. CCOUNT reads are one
     * instruction and let the serial probe split map work without timing each
     * quad through the host API. */
    holo_perf_add(HOLO_PERF_MAP_CULL, rasterMapCullCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_XFORM, rasterMapXformCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_PROJECT, rasterMapProjectCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_DRAW, rasterMapDrawCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_SETUP, rasterMapSetupCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_RASTER, rasterMapRasterCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_HIZ, rasterMapHiZCycles / 240u);
    holo_perf_add(HOLO_PERF_MAP_CLIP, rasterMapClipCycles / 240u);
    holo_perf_add(HOLO_PERF_RASTER_WAIT, rasterWorkerWaitCycles / 240u);
    holo_perf_add(HOLO_PERF_RASTER_FREE_WAIT, rasterWorkerFreeWaitCycles / 240u);
    holo_perf_add(HOLO_PERF_RASTER_FLUSH_WAIT, rasterWorkerFlushWaitCycles / 240u);
    holo_perf_add(HOLO_PERF_CURRENT_HIZ_CULL, rasterCurrentHiZCull);
    holo_perf_add(HOLO_PERF_PREVIOUS_HIZ_CULL, rasterPreviousHiZCull);
    holo_perf_add(HOLO_PERF_PREVIOUS_BOUNDS_CULL, rasterPreviousBoundsCull);
    holo_perf_add(HOLO_PERF_MAP_MODE_NONE, rasterMapModeCounts[SOFTFP_MAP_MODE_NONE]);
    holo_perf_add(HOLO_PERF_MAP_MODE_OPAQUE, rasterMapModeCounts[SOFTFP_MAP_MODE_OPAQUE]);
    holo_perf_add(HOLO_PERF_MAP_MODE_DEPTH, rasterMapModeCounts[SOFTFP_MAP_MODE_DEPTH_ONLY]);
    holo_perf_add(HOLO_PERF_MAP_MODE_HALF, rasterMapModeCounts[SOFTFP_MAP_MODE_HALF_BLEND]);
    holo_perf_add(HOLO_PERF_HALF_RASTER_CALLS, rasterHalfBlendCalls);
    holo_perf_add(HOLO_PERF_CORE0_TRANS_JOBS, softfpRasterTransJobsFrame);
    holo_perf_add(HOLO_PERF_CLIP_ZERO_MASK, rasterClipZeroMask);
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    holo_perf_add(HOLO_PERF_CORE0_RASTER_JOBS, softfpRasterJobsFrame);
    holo_perf_add(HOLO_PERF_CORE0_RASTER_FALLBACKS, softfpRasterFallbacksFrame);
    holo_perf_add(HOLO_PERF_CORE0_RASTER_FLUSHES, softfpRasterFlushesFrame);
    holo_perf_add(HOLO_PERF_CORE0_RASTER_BATCHES, softfpRasterBatchesFrame);
#endif
#if defined CC_BUILD_SOFTFP_TEXEL_2X2 || defined CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    holo_perf_add(HOLO_PERF_TEXTURE_REUSE, textureSampleReuse);
    holo_perf_add(HOLO_PERF_TEXTURE_LOAD, textureSampleLoads);
#endif
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    holo_perf_add(HOLO_PERF_TINT_REUSE, terrainTintCacheHits);
    holo_perf_add(HOLO_PERF_TINT_LOAD, terrainTintCacheMisses);
#endif
#endif
    Window_DrawFramebuffer(r, &fb_bmp);
    /* The Holo backend may hand the completed buffer to its Core 0 presenter. */
    colorBuffer = (SoftPixel*)fb_bmp.scan0;
}

void Gfx_SetVSync(cc_bool vsync) {
    gfx_vsync = vsync;
}

void Gfx_OnWindowResize(int width, int height) {
    if (depthBuffer) {
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
        SoftFP_RasterWorkerStop();
#endif
        DestroyBuffers();
    }

    fb_width   = width;
    fb_height  = height;

#ifdef CC_BUILD_ESP32S3
    /* Reserve one contiguous block before the presenter task/DMA buffers.
     * Suballocation keeps hot VBs internal without fragmenting the host heap. */
    (void)holo_port_hot_vb_reserve();
#endif

#ifdef CC_BUILD_SOFTFP_HOT_ATLAS
    /* Reserve the contiguous atlas store before the Core 0 presenter stack
     * and DMA blocks fragment internal RAM. The atlas is populated later. */
    (void)holo_port_buffer(HOT_ATLAS_PIXELS * sizeof(SoftTexel), 2);
#endif
    Window_AllocFramebuffer(&fb_bmp, width, height);
    colorBuffer = (SoftPixel*)fb_bmp.scan0;
    cb_stride   = fb_bmp.width;
#ifdef SOFTFP_LOWRES_COLOR_STAGE
#ifdef CC_BUILD_SOFTFP_CORE0_RASTER
    (void)SoftFP_RasterWorkerStart();
#endif
#ifdef CC_BUILD_SOFTFP_W_DEPTH8
    wDepth8EncodeLut = (DepthValue*)holo_port_buffer(
        SOFTFP_W_DEPTH8_LUT_SIZE * sizeof(DepthValue), 6);
    if (wDepth8EncodeLut) {
        cc_uint32 lutIndex;
        for (lutIndex = 0; lutIndex < SOFTFP_W_DEPTH8_LUT_SIZE; lutIndex++) {
            wDepth8EncodeLut[lutIndex] = EncodeWDepth8Scaled(lutIndex);
        }
        Platform_LogConst("[cc.gfx] Z8 encode LUT=2048 internal");
    } else {
        Platform_LogConst("[cc.gfx] Z8 encode LUT unavailable; scalar encode");
    }
#endif
    rasterColorBuffer = (SoftPixel*)Mem_Alloc(
        raster_width, CLASSICUBE_COLOR_PSRAM_ROWS * sizeof(SoftPixel),
        "192x72 staged color buffer");
    if (!rasterColorInternal[0]) {
        rasterColorFallback[0] = (SoftPixel*)Mem_Alloc(
            raster_width, CLASSICUBE_COLOR_BAND_ROWS * sizeof(SoftPixel),
            "192x36 staged color fallback A");
    }
    if (!rasterColorInternal[1]) {
        rasterColorFallback[1] = (SoftPixel*)Mem_Alloc(
            raster_width, CLASSICUBE_COLOR_BAND_ROWS * sizeof(SoftPixel),
            "192x36 staged color fallback B");
    }
    {
        int row;
        for (row = 0; row < raster_height; row++) {
            if (row < CLASSICUBE_COLOR_PSRAM_ROWS) {
                rasterColorRows[row] = rasterColorBuffer + row * raster_width;
            } else {
                int localRow = row - CLASSICUBE_COLOR_PSRAM_ROWS;
                int band = localRow / CLASSICUBE_COLOR_BAND_ROWS;
                SoftPixel* pixels = rasterColorInternal[band] ?
                    rasterColorInternal[band] : rasterColorFallback[band];
                rasterColorRows[row] = pixels +
                    (localRow % CLASSICUBE_COLOR_BAND_ROWS) * raster_width;
            }
        }
        if (rasterColorInternal[0] && rasterColorInternal[1]) {
            Platform_LogConst("[cc.gfx] color=lower72 internal=27648 + top72 PSRAM=27648");
        } else if (rasterColorInternal[0] || rasterColorInternal[1]) {
            Platform_LogConst("[cc.gfx] color=36 rows internal=13824 + 108 rows PSRAM=41472");
        } else {
            Platform_LogConst("[cc.gfx] color bands internal unavailable; 192x144 RGB565 in PSRAM");
        }
    }
#endif

#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
    int i;
    db_stride = raster_width;
    db_height = raster_height;
    for (i = 0; i < raster_width; i++) {
        int x0 = (i * width) / raster_width;
        int x1 = ((i + 1) * width) / raster_width;
        raster_to_color_xw[i] = (cc_uint16)x0 |
            ((x1 - x0) == 2 ? RASTER_GROUP_DOUBLE_X : 0);
    }
    for (i = 0; i < raster_height; i++) {
        int y0 = (i * height) / raster_height;
        int y1 = ((i + 1) * height) / raster_height;
        raster_to_color_rowh[i] = (cc_uint32)(y0 * cb_stride) |
            ((y1 - y0) == 2 ? RASTER_GROUP_DOUBLE_Y : 0);
    }
#else
    db_stride = width;
    db_height = height;
#endif

#ifdef CC_BUILD_SOFTFP_W_DEPTH8
    depthBuffer = (DepthValue*)holo_port_buffer(
        db_stride * db_height * sizeof(DepthValue), 2);
    depthBufferInternal = depthBuffer != NULL;
    if (!depthBuffer) {
#ifdef CC_BUILD_ESP32S3
        Platform_LogConst("[cc.gfx] depth internal unavailable; allocating 192x144 Z8 in PSRAM");
#endif
        depthBuffer = Mem_Alloc(db_stride * db_height, sizeof(DepthValue), "depth buffer");
    }
    Mem_Set(depthBuffer, DEPTH_CLEAR_VALUE, db_stride * db_height);
#else
    depthBuffer = Mem_Alloc(db_stride * db_height, sizeof(DepthValue), "depth buffer");
#endif
#ifdef CC_BUILD_SOFTFP_Z16_TILES
    depthCachePixels = (DepthValue*)holo_port_buffer(
        DEPTH_CACHE_LINES * DEPTH_TILE_PIXELS * sizeof(DepthValue), 2);
    DepthCacheReset();
    if (depthCachePixels) {
        Platform_LogConst("[cc.gfx] depth=Z16 ndc tile=16x16 cache=64 internal=32768");
    } else {
        Platform_LogConst("[cc.gfx] depth=Z16 linear PSRAM fallback");
    }
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
#ifdef CC_BUILD_ESP32S3
    Platform_LogConst("[cc.gfx] texture=PS1-affine + selective perspective/8 full-rate A1RGB555 framebuffer=RGB565");
#else
    Platform_LogConst("[cc.gfx] texture=perspective-correct adaptive/4/8/16/32 A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#else
    Platform_LogConst("[cc.gfx] texture=affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#elif defined CC_BUILD_SOFTFP_W_DEPTH8
    if (depthBufferInternal) {
        Platform_LogConst("[cc.gfx] depth=reciprocal-W Z8 hybrid-linear-log internal=27648");
    } else {
        Platform_LogConst("[cc.gfx] depth=reciprocal-W Z8 PSRAM fallback");
    }
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
#ifdef CC_BUILD_ESP32S3
    Platform_LogConst("[cc.gfx] texture=PS1-affine + selective perspective/8 full-rate A1RGB555 framebuffer=RGB565");
#else
    Platform_LogConst("[cc.gfx] texture=perspective-correct adaptive/4/8/16/32 A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#else
    Platform_LogConst("[cc.gfx] texture=affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#elif defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
    Platform_LogConst("[cc.gfx] depth=Z32 original-order fast-clear");
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    Platform_LogConst("[cc.gfx] texture=perspective-correct adaptive/4/8/16/32 A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#else
    Platform_LogConst("[cc.gfx] texture=affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#else
#ifdef CC_BUILD_SOFTFP_W_DEPTH
    Platform_LogConst("[cc.gfx] depth=reciprocal-W Z32 single-buffer clear=Core1");
#else
    Platform_LogConst("[cc.gfx] depth=Z32 original-order single-buffer clear=Core1");
#endif
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    Platform_LogConst("[cc.gfx] texture=perspective-correct adaptive/4/8/16/32 A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#else
    Platform_LogConst("[cc.gfx] texture=affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
#endif
#endif
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
#ifdef CC_BUILD_SOFTFP_W_DEPTH8
    Platform_LogConst("[cc.gfx] raster=3D-192x144 Z8 scale5/3 UI=320x240");
#else
    Platform_LogConst("[cc.gfx] raster=3D-lowres Z32 scaled UI=320x240");
#endif
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    Platform_LogConst("[cc.gfx] color=192x144 RGB565 lower72-hot split upscale=packed-single-pass-before-UI");
#endif
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
	Platform_LogConst("[cc.gfx] frontend=map-only quad-raster AABB cull-cache packed-local/8 Hi-Z4/8+groups+temporal");
#endif
    Platform_LogConst("[cc.gfx] terrain=PS1-style affine, full-rate sampling");
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
	if (terrainTileCacheMemory) {
		Platform_LogConst("[cc.gfx] terrain-texture-cache=24x16x16 referenced-slot-safe internal");
    } else {
        Platform_LogConst("[cc.gfx] terrain-texture-cache unavailable; direct PSRAM sampling");
    }
#endif
#ifdef CC_BUILD_SOFTFP_FOG4
    Platform_LogConst("[cc.gfx] fog=4-level quarter-strength");
#else
    Platform_LogConst("[cc.gfx] fog=off (clear view, no per-pixel fog)");
#endif
#ifdef CC_BUILD_ESP32S3
    Platform_LogConst("[cc.gfx] probes=pipeline");
#endif

    Gfx_SetViewport(0, 0, width, height);
    Gfx_SetScissor (0, 0, width, height);
}

void Gfx_SetViewport(int x, int y, int w, int h) {
    vp_hwidth_fp  = FloatToFixed(w / 2.0f);
    vp_hheight_fp = FloatToFixed(h / 2.0f);
#ifdef CC_BUILD_SOFTFP_LOWRES_RASTER
    vp_raster_hwidth_fp = (int)(((int64_t)vp_hwidth_fp * raster_width) / fb_width);
    vp_raster_hheight_fp = (int)(((int64_t)vp_hheight_fp * raster_height) / fb_height);
#endif
}

void Gfx_SetScissor (int x, int y, int w, int h) {
    fb_maxX = x + w - 1;
    fb_maxY = y + h - 1;
}

void Gfx_GetApiInfo(cc_string* info) {
    int pointerSize = sizeof(void*) * 8;
    String_Format1(info, "-- Using software fixed-point (%i bit) --\n", &pointerSize);
    PrintMaxTextureInfo(info);
}

cc_bool Gfx_TryRestoreContext(void) { return true; }
#endif
