#include "Core.h"
#if CC_GFX_BACKEND == CC_GFX_BACKEND_SOFTFP
#ifndef CC_BUILD_ESP32S3
#define CC_DYNAMIC_VBS_ARE_STATIC
#endif
#define OVERRIDE_BEGEND2D_FUNCTIONS
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Window.h"
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
static cc_uint32 depthCacheHits, depthCacheMisses, depthCacheWritebacks;
#elif defined CC_BUILD_SOFTFP_W_DEPTH8
typedef cc_uint8 DepthValue;
static DepthValue* depthBuffer;
static cc_bool depthBufferInternal;
#elif defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
typedef cc_uint32 DepthValue;
static DepthValue* depthBuffer;
#else
typedef int DepthValue;
static DepthValue* depthBuffer;
#endif
#ifdef CC_BUILD_SOFTFP_ASYNC_CLEAR
static cc_bool asyncPresenterClear, asyncDepthClear;
#if !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_W_DEPTH8 && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
static DepthValue* depthBuffers[2];
static unsigned depthBufferIndex;
#endif
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
#endif
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
#define TERRAIN_TILE_CACHE_SLOTS 8
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
static cc_uint16 terrainCoverage[SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS];
static DepthValue terrainHiZ4[SOFTFP_COVERAGE_COLS * SOFTFP_COVERAGE_ROWS];
static DepthValue terrainHiZ[SOFTFP_HIZ_COLS * SOFTFP_HIZ_ROWS];
static TerrainTintTable* terrainTintCache;
static cc_uint8 terrainTintCacheNext;
static cc_uint32 terrainTintCacheHits, terrainTintCacheMisses;
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
#define SOFTFP_MAP_BATCH_HINT ((DrawHints)0x10)
#endif
#ifdef CC_BUILD_ESP32S3
static cc_uint64 rasterBboxPixels, rasterSpanPixels;
static cc_uint64 rasterPerspectiveTriangles, rasterPerspectivePixels;
static cc_uint64 rasterPerspectiveReciprocals;
static cc_uint64 rasterFrustumCacheHits, rasterFrustumCacheMisses;
static cc_uint64 rasterTerrainQuads;
static cc_uint64 rasterSpriteQuads, rasterSpriteCull;
static cc_uint64 rasterHiZTests, rasterHiZCull;
static cc_uint64 rasterChunkHiZTests, rasterChunkHiZCull;
static cc_uint64 rasterGroupHiZTests, rasterGroupHiZCull;
static cc_uint64 rasterTemporalHiZHit, rasterTemporalHiZMiss, rasterTemporalHiZCull;
static cc_uint32 gfx_temporal_quad_key;
static cc_uint64 rasterProjectedVertices, rasterProjectionReuse;
/* These two diagnostics update on every visible pixel and cost about 0.7 ms
 * per frame on ESP32-S3. Define this only for short profiling builds. */
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
static cc_uint64 rasterFastCutoutPixels;
static cc_uint64 rasterFogPixels;
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
static cc_uint64 rasterQuadCullTotal;
#endif
#if defined CC_BUILD_SOFTFP_TEXEL_2X2 || defined CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
static cc_uint64 textureSampleReuse, textureSampleLoads;
#endif
#endif

static void* gfx_vertices;
static GfxResourceID white_square;

#ifdef CC_BUILD_SOFTFP_FOG4
static CC_INLINE SoftPixel AveragePixel565(SoftPixel a, SoftPixel b) {
    return (SoftPixel)((a & b) + (((a ^ b) & 0xF7DEu) >> 1));
}

static CC_INLINE SoftPixel ApplyFog4(SoftPixel source, int inverseW,
                                     cc_uint32* fogPixels) {
    SoftPixel halfFog, quarterFog, eighthFog;
    if (!gfx_fogEnabled || inverseW >= fogWStart) return source;
    if (fogPixels) (*fogPixels)++;

    /* Preserve the existing fog distances while halving the fog opacity
     * again: the four stages now blend 6.25/12.5/18.75/25% fog. */
    halfFog    = AveragePixel565(source, fogPixel);
    quarterFog = AveragePixel565(source, halfFog);
    eighthFog  = AveragePixel565(source, quarterFog);
    if (inverseW < fogWEnd) return quarterFog;
    if (inverseW < fogWHalf) return AveragePixel565(quarterFog, eighthFog);
    if (inverseW < fogWQuarter) return eighthFog;
    return AveragePixel565(source, eighthFog);
}
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
#define APPLY_FOG4(source, inverseW, counter) \
    ApplyFog4((source), (inverseW), &(counter))
#else
#define APPLY_FOG4(source, inverseW, counter) \
    ApplyFog4((source), (inverseW), NULL)
#endif
#endif

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
    depthCacheHits = depthCacheMisses = depthCacheWritebacks = 0;
}

static DepthValue* DepthCacheGetTile(int tile, int* lineIndex) {
    int line = tile & (DEPTH_CACHE_LINES - 1);
    int writeback = 0;
    DepthValue* pixels = depthCachePixels + line * DEPTH_TILE_PIXELS;
    cc_uint16 oldTag = depthCacheTags[line];

    if (oldTag == (cc_uint16)tile) {
        depthCacheHits++;
        *lineIndex = line;
        return pixels;
    }
    if (oldTag != 0xFFFFu && depthCacheDirty[line]) {
        Mem_Copy(depthBuffer + oldTag * DEPTH_TILE_PIXELS,
                 pixels, DEPTH_TILE_PIXELS * sizeof(DepthValue));
        depthBackingValid[oldTag] = 1;
        writeback = 1;
    }
    if (depthBackingValid[tile]) {
        Mem_Copy(pixels, depthBuffer + tile * DEPTH_TILE_PIXELS,
                 DEPTH_TILE_PIXELS * sizeof(DepthValue));
    } else {
        FillDepth16(pixels, DEPTH_CLEAR_VALUE, DEPTH_TILE_PIXELS);
    }
    depthCacheTags[line]  = (cc_uint16)tile;
    depthCacheDirty[line] = 0;
    depthCacheMisses++;
    if (writeback) depthCacheWritebacks++;
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
    /* Claim the fixed-resolution Z8 block before component VBs can fragment
     * the internal heap. Keep the same raster resolution on fallback. */
    raster_width  = CLASSICUBE_3D_WIDTH;
    raster_height = CLASSICUBE_3D_HEIGHT;
    if (!holo_port_buffer(raster_width * raster_height * sizeof(DepthValue), 2)) {
        Platform_LogConst("[cc.gfx] WARN: 192x144 Z8 internal reserve unavailable; using PSRAM at same resolution");
    }
#endif
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    {
        const size_t tileBytes = TERRAIN_TILE_CACHE_SLOTS * TERRAIN_TILE_PIXELS * sizeof(cc_uint16);
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
        cc_uint8* cacheMemory = (cc_uint8*)holo_port_buffer(
            tileBytes + TERRAIN_TINT_CACHE_SLOTS * sizeof(TerrainTintTable), 3);
        terrainTileCacheMemory = cacheMemory;
        terrainTintCache = cacheMemory ? (TerrainTintTable*)(cacheMemory + tileBytes) : NULL;
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
}

static void DestroyBuffers(void) {
    Window_FreeFramebuffer(&fb_bmp);
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    Mem_Free(rasterColorBuffer);
    rasterColorBuffer = NULL;
#endif
#ifdef CC_BUILD_SOFTFP_W_DEPTH8
    if (depthBuffer && !depthBufferInternal) Mem_Free(depthBuffer);
    depthBufferInternal = false;
    asyncPresenterClear = asyncDepthClear = false;
#elif defined CC_BUILD_SOFTFP_ASYNC_CLEAR && !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
    Mem_Free(depthBuffers[0]);
    Mem_Free(depthBuffers[1]);
    depthBuffers[0] = depthBuffers[1] = NULL;
    depthBufferIndex = 0;
    asyncPresenterClear = asyncDepthClear = false;
#else
    Mem_Free(depthBuffer);
#endif
    depthBuffer = NULL;
#ifdef CC_BUILD_SOFTFP_Z16_TILES
    depthCachePixels = NULL;
#endif
}

void Gfx_Free(void) { 
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
static int texWidthMask, texHeightMask;
static int texSinglePixel;
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
static CCTexture* terrainTileCacheOwners[TERRAIN_TILE_CACHE_SLOTS];
static cc_uint16 terrainTileCacheIndices[TERRAIN_TILE_CACHE_SLOTS];
static cc_uint8 terrainTileCacheNext;

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

    slot = terrainTileCacheNext++ & (TERRAIN_TILE_CACHE_SLOTS - 1);
    Mem_Copy(pixels + slot * TERRAIN_TILE_PIXELS,
             curTexPixels + tileIndex * TERRAIN_TILE_PIXELS,
             TERRAIN_TILE_PIXELS * sizeof(SoftTexel));
    terrainTileCacheOwners[slot]  = curTexture;
    terrainTileCacheIndices[slot] = (cc_uint16)tileIndex;
    textureSampleLoads++;
    return pixels + slot * TERRAIN_TILE_PIXELS;
}

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
static const TerrainTintTable* TerrainTintCache_Get(int tintR, int tintG, int tintB) {
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
        
void Gfx_DeleteTexture(GfxResourceID* texId) {
    GfxResourceID data = *texId;
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
void Gfx_Begin2D(int width, int height) {
#ifdef SOFTFP_LOWRES_COLOR_STAGE
	cc_uint64 upscaleMark = Stopwatch_Measure();
	int y;
	for (y = 0; y < raster_height; y++) {
		const SoftPixel* src = rasterColorBuffer + y * raster_width;
		cc_uint32 rowh = raster_to_color_rowh[y];
		SoftPixel* dst = colorBuffer + (rowh & ~RASTER_GROUP_DOUBLE_Y);

		if (raster_width == 192 && fb_width == 320) {
			cc_uint32* out = (cc_uint32*)dst;
			cc_uint32* out2 = (rowh & RASTER_GROUP_DOUBLE_Y) ?
				(cc_uint32*)(dst + cb_stride) : NULL;
			int x;
			for (x = 0; x < 192; x += 6, out += 5) {
				cc_uint32 c0 = src[x + 0], c1 = src[x + 1], c2 = src[x + 2];
				cc_uint32 c3 = src[x + 3], c4 = src[x + 4], c5 = src[x + 5];
				cc_uint32 p0 = c0 | (c1 << 16), p1 = c1 | (c2 << 16);
				cc_uint32 p2 = c2 | (c3 << 16), p3 = c4 | (c4 << 16);
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
    target = rasterColorBuffer;
    width  = raster_width;
    height = raster_height;
    stride = raster_width;
#elif defined CC_BUILD_SOFTFP_ASYNC_CLEAR && defined CC_BUILD_SOFTFP_FB16
    if (asyncPresenterClear &&
        holo_port_presenter_take_precleared_color(colorBuffer, (cc_uint16)fill)) return;
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

static void ClearDepthBuffer(void) {
#ifdef CC_BUILD_ESP32S3
    rasterBboxPixels = rasterSpanPixels = 0;
    rasterPerspectiveTriangles = rasterPerspectivePixels = 0;
    rasterPerspectiveReciprocals = 0;
    rasterFrustumCacheHits = rasterFrustumCacheMisses = 0;
    rasterTerrainQuads = 0;
    rasterSpriteQuads = rasterSpriteCull = 0;
    rasterHiZTests = rasterHiZCull = 0;
    rasterChunkHiZTests = rasterChunkHiZCull = 0;
    rasterGroupHiZTests = rasterGroupHiZCull = 0;
    rasterTemporalHiZHit = rasterTemporalHiZMiss = rasterTemporalHiZCull = 0;
    gfx_temporal_quad_key = 0;
    rasterProjectedVertices = rasterProjectionReuse = 0;
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    rasterFastCutoutPixels = 0;
    rasterFogPixels = 0;
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    rasterQuadCullTotal = 0;
#endif
#if defined CC_BUILD_SOFTFP_TEXEL_2X2 || defined CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    textureSampleReuse = textureSampleLoads = 0;
#endif
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    terrainTintCacheHits = terrainTintCacheMisses = 0;
#endif
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
    Mem_Set(terrainCoverage, 0, sizeof(terrainCoverage));
    Mem_Set(terrainHiZ4, DEPTH_CLEAR_VALUE, sizeof(terrainHiZ4));
    Mem_Set(terrainHiZ, DEPTH_CLEAR_VALUE, sizeof(terrainHiZ));
#endif
#else
#ifdef CC_BUILD_SOFTFP_Z32_FASTCLEAR
    Mem_Set(depthBuffer, 0xFF, db_stride * db_height * sizeof(DepthValue));
#else
#ifdef CC_BUILD_SOFTFP_ASYNC_CLEAR
    if (asyncDepthClear) return;
#endif
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
typedef struct SoftFPMapCullData {
    SoftFPMapQuadBounds aggregate;
    cc_uint32 cacheKey;
    cc_uint32 quadCount;
    cc_uint32 bitBytes;
    SoftFPMapQuadBounds quads[1];
} SoftFPMapCullData;
#define SOFTFP_TEMPORAL_HIZ_SIZE 4096u
#define SOFTFP_TEMPORAL_HIZ_WAYS 4u
#define SOFTFP_TEMPORAL_HIZ_SETS (SOFTFP_TEMPORAL_HIZ_SIZE / SOFTFP_TEMPORAL_HIZ_WAYS)
typedef struct SoftFPTemporalHiZEntry {
    cc_uint32 key;
    cc_uint32 rect;
    cc_uint32 depth;
} SoftFPTemporalHiZEntry;
static SoftFPTemporalHiZEntry temporalHiZ[SOFTFP_TEMPORAL_HIZ_SIZE];
static cc_uint8 temporalHiZNext[SOFTFP_TEMPORAL_HIZ_SETS];
static SoftFPMapCullData* gfx_mapCullData;
static const SoftFPMapQuadBounds* gfx_mapCullBounds;
static SoftFPClipPlane mapCullLocalPlanes[NUM_PLANES];
static int gfx_map_origin_x, gfx_map_origin_y, gfx_map_origin_z;
static cc_uint32 gfx_mvp_cache_key = 1;
static cc_uint32 gfx_map_generation = 1;

static cc_uint8* SoftFP_CullKnownBits(SoftFPMapCullData* data) {
    return (cc_uint8*)(data->quads + data->quadCount);
}

static cc_uint8* SoftFP_CullResultBits(SoftFPMapCullData* data) {
    return SoftFP_CullKnownBits(data) + data->bitBytes;
}

static CC_INLINE cc_uint32 SoftFP_TemporalQuadKey(int firstVertex) {
    cc_uint32 key;
    cc_uint32 quadIndex;
    if (!gfx_mapCullData || firstVertex < 0) return 0;
    quadIndex = (cc_uint32)firstVertex >> 2;
    if (quadIndex >= gfx_mapCullData->quadCount) return 0;
    key  = (cc_uint32)(uintptr_t)gfx_mapCullData;
    key ^= quadIndex * 0x9E3779B1u;
    key ^= gfx_mvp_cache_key * 0x85EBCA6Bu;
    key ^= gfx_map_generation * 0xC2B2AE35u;
    key ^= key >> 16; key *= 0x7FEB352Du;
    key ^= key >> 15; key *= 0x846CA68Bu;
    key ^= key >> 16;
    return key ? key : 1;
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
    int quadCount = verticesCount >> 2;
    const int quantStep = 256 >> 3;
    cc_uint32 bytes;
    int i;

    (void)originX; (void)originY; (void)originZ;

    if (!src || quadCount <= 0) return NULL;
    data = NULL;
    {
        cc_uint32 bitBytes = ((cc_uint32)quadCount + 7u) >> 3;
        bytes = (cc_uint32)offsetof(SoftFPMapCullData, quads) +
                (cc_uint32)quadCount * (cc_uint32)sizeof(data->quads[0]) +
                bitBytes * 2u;
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
    data->quadCount = (cc_uint32)quadCount;
    data->bitBytes = ((cc_uint32)quadCount + 7u) >> 3;
    Mem_Set(SoftFP_CullKnownBits(data), 0, data->bitBytes);
    bounds = data->quads;
    data->aggregate.minX = data->aggregate.minY = data->aggregate.minZ = 0xFF;
    data->aggregate.maxX = data->aggregate.maxY = data->aggregate.maxZ = 0;

    for (i = 0; i < quadCount; i++, src += 4) {
        int minX = min(src[0].x, src[2].x), maxX = max(src[0].x, src[2].x);
        int minY = min(src[0].y, src[2].y), maxY = max(src[0].y, src[2].y);
        int minZ = min(src[0].z, src[2].z), maxZ = max(src[0].z, src[2].z);
        int qMinX, qMaxX, qMinY, qMaxY, qMinZ, qMaxZ;

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
        data->aggregate.minX = min(data->aggregate.minX, bounds[i].minX);
        data->aggregate.minY = min(data->aggregate.minY, bounds[i].minY);
        data->aggregate.minZ = min(data->aggregate.minZ, bounds[i].minZ);
        data->aggregate.maxX = max(data->aggregate.maxX, bounds[i].maxX);
        data->aggregate.maxY = max(data->aggregate.maxY, bounds[i].maxY);
        data->aggregate.maxZ = max(data->aggregate.maxZ, bounds[i].maxZ);
    }
    gfx_map_generation++;
    if (!gfx_map_generation) gfx_map_generation = 1;
    return data;
}

void Gfx_DeleteMapCullBounds(void** bounds) {
    void* data = *bounds;
    if (gfx_mapCullData == data) {
        gfx_mapCullData = NULL;
        gfx_mapCullBounds = NULL;
    }
    if (data) {
        gfx_map_generation++;
        if (!gfx_map_generation) gfx_map_generation = 1;
    }
    if (data && !holo_port_hot_vb_free(data)) Mem_Free(data);
    *bounds = NULL;
}

void Gfx_BindMapCullBounds(void* bounds, int originX, int originY, int originZ) {
    cc_uint8 planes;
    gfx_mapCullData = (SoftFPMapCullData*)bounds;
    gfx_mapCullBounds = gfx_mapCullData ? gfx_mapCullData->quads : NULL;
    gfx_map_origin_x = originX;
    gfx_map_origin_y = originY;
    gfx_map_origin_z = originZ;
    if (!bounds) return;

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
        m->row1.w - m->row1.z * 0.25f,
        m->row2.w - m->row2.z * 0.25f,
        m->row3.w - m->row3.z * 0.25f,
        m->row4.w - m->row4.z * 0.25f);
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
static CC_NOINLINE cc_bool SoftFP_MapQuadOutsideFrustum(int firstVertex, cc_uint8 planeMask) {
    const PackedTerrainVertex* src =
        (const PackedTerrainVertex*)gfx_vertices + firstVertex;
    float minXf, minYf, minZf, maxXf, maxYf, maxZf;
    cc_uint8 originalMask = planeMask;
    cc_bool outside = false;
    cc_uint8* knownBits = NULL;
    cc_uint8* resultBits = NULL;
    cc_uint32 quadIndex = (cc_uint32)firstVertex >> 2;
    cc_uint32 cacheByte = quadIndex >> 3;
    cc_uint8 cacheBit = (cc_uint8)(1u << (quadIndex & 7u));
    int i;

    if (gfx_mapCullData && quadIndex < gfx_mapCullData->quadCount) {
        cc_uint32 key = gfx_mvp_cache_key ^ ((cc_uint32)originalMask * 0x9E3779B1u);
        knownBits  = SoftFP_CullKnownBits(gfx_mapCullData);
        resultBits = SoftFP_CullResultBits(gfx_mapCullData);
        if (gfx_mapCullData->cacheKey != key) {
            gfx_mapCullData->cacheKey = key;
            Mem_Set(knownBits, 0, gfx_mapCullData->bitBytes);
        }
        if (knownBits[cacheByte] & cacheBit) {
            rasterFrustumCacheHits++;
            return (resultBits[cacheByte] & cacheBit) != 0;
        }
        rasterFrustumCacheMisses++;
    }

    if (gfx_mapCullBounds) {
        const SoftFPMapQuadBounds* bounds = &gfx_mapCullBounds[quadIndex];
        while (planeMask) {
            const float rejectMargin = -1.0f / 1024.0f;
            const SoftFPClipPlane* plane;
            float px, py, pz, support;

            i = __builtin_ctz((unsigned)planeMask);
            planeMask &= (cc_uint8)(planeMask - 1u);
            plane = &mapCullLocalPlanes[i];
            px = plane->x < 0.0f ? bounds->minX : bounds->maxX;
            py = plane->y < 0.0f ? bounds->minY : bounds->maxY;
            pz = plane->z < 0.0f ? bounds->minZ : bounds->maxZ;
            support = px * plane->x + py * plane->y + pz * plane->z + plane->w;
            if (support < rejectMargin) { outside = true; break; }
        }
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
        const SoftFPClipPlane* plane;
        float px, py, pz, support;

        i = __builtin_ctz((unsigned)planeMask);
        planeMask &= (cc_uint8)(planeMask - 1u);
        plane = &quadCullPlanes[i];
        px = plane->x < 0.0f ? minXf : maxXf;
        py = plane->y < 0.0f ? minYf : maxYf;
        pz = plane->z < 0.0f ? minZf : maxZf;
        support = px * plane->x + py * plane->y + pz * plane->z + plane->w;
        if (support < rejectMargin) { outside = true; break; }
    }

cache_result:
    if (knownBits) {
        if (outside) resultBits[cacheByte] |= cacheBit;
        else         resultBits[cacheByte] &= (cc_uint8)~cacheBit;
        knownBits[cacheByte] |= cacheBit;
    }
    return outside;
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

    minX = max(minX, 0); maxX = min(maxX, fb_maxX);
    minY = max(minY, 0); maxY = min(maxY, fb_maxY);

    int x, y;
    for (y = minY; y <= maxY; y++) 
    {
        int texY = fast ? (begTY + (y - minY)) : (((begTY + delTY * (y - minY) / height)) & texHeightMask);
        for (x = minX; x <= maxX; x++) 
        {
            int texX = fast ? (begTX + (x - minX)) : (((begTX + delTX * (x - minX) / width)) & texWidthMask);
            int texIndex = texY * curTexWidth + texX;

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
    rasterColorBuffer[rasterY * raster_width + rasterX] = color;
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
    SoftPixel* dst = rasterColorBuffer + rasterY * raster_width + rasterX;
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
static CC_INLINE DepthValue EncodeWDepth8(int inverseW) {
    cc_uint32 scaled = (cc_uint32)inverseW >> 8;
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
#endif

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
static CC_INLINE void SoftFP_RecordTerrainCoverage(int x, int y) {
    int coverageX = x >> SOFTFP_COVERAGE_SHIFT;
    int coverageY = y >> SOFTFP_COVERAGE_SHIFT;
    int coverageIndex = coverageY * SOFTFP_COVERAGE_COLS + coverageX;
    cc_uint16 mask = terrainCoverage[coverageIndex];
    int bit, hizX, hizY, coverageBase;
    int scanX, scanY;
    DepthValue maxDepth;

    if (mask == 0xFFFFu) return;
    bit = ((y & 3) << 2) | (x & 3);
    mask = (cc_uint16)(mask | (cc_uint16)(1u << bit));
    terrainCoverage[coverageIndex] = mask;
    if (mask != 0xFFFFu) return;

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
    hizX = x >> SOFTFP_HIZ_SHIFT;
    hizY = y >> SOFTFP_HIZ_SHIFT;
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

static CC_INLINE cc_bool SoftFP_RectOccluded(int minX, int minY, int maxX, int maxY,
                                             DepthValue nearestDepth) {
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
        const DepthValue* parentRow = terrainHiZ + tileY * SOFTFP_HIZ_COLS;
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
                const DepthValue* childRow = terrainHiZ4 + childY * SOFTFP_COVERAGE_COLS;
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

static CC_INLINE cc_bool SoftFP_TemporalRectOccluded(cc_uint32 key) {
    SoftFPTemporalHiZEntry* entry;
    cc_uint32 rect;
    cc_uint32 base, way;
    int minX, minY, maxX, maxY;
    if (!key) return false;
    base = ((cc_uint32)key & (SOFTFP_TEMPORAL_HIZ_SETS - 1u)) * SOFTFP_TEMPORAL_HIZ_WAYS;
    entry = NULL;
    for (way = 0; way < SOFTFP_TEMPORAL_HIZ_WAYS; way++) {
        if (temporalHiZ[base + way].key == key) {
            entry = &temporalHiZ[base + way];
            break;
        }
    }
    if (!entry) {
        rasterTemporalHiZMiss++;
        return false;
    }
    rasterTemporalHiZHit++;
    rect = entry->rect;
    minX = rect & 0xFF; minY = (rect >> 8) & 0xFF;
    maxX = (rect >> 16) & 0xFF; maxY = rect >> 24;
    if (!SoftFP_RectOccluded(minX, minY, maxX, maxY, (DepthValue)entry->depth)) return false;
    rasterTemporalHiZCull++;
    return true;
}

static CC_INLINE void SoftFP_TemporalStoreRect(cc_uint32 key,
                                                int minX, int minY, int maxX, int maxY,
                                                DepthValue nearestDepth) {
    SoftFPTemporalHiZEntry* entry;
    cc_uint32 set, base, way;
    if (!key) return;
    set = (cc_uint32)key & (SOFTFP_TEMPORAL_HIZ_SETS - 1u);
    base = set * SOFTFP_TEMPORAL_HIZ_WAYS;
    entry = NULL;
    for (way = 0; way < SOFTFP_TEMPORAL_HIZ_WAYS; way++) {
        if (temporalHiZ[base + way].key == key || !temporalHiZ[base + way].key) {
            entry = &temporalHiZ[base + way];
            break;
        }
    }
    if (!entry) {
        way = temporalHiZNext[set] & (SOFTFP_TEMPORAL_HIZ_WAYS - 1u);
        temporalHiZNext[set] = (cc_uint8)(way + 1u);
        entry = &temporalHiZ[base + way];
    }
    entry->rect = (cc_uint32)minX | ((cc_uint32)minY << 8) |
                  ((cc_uint32)maxX << 16) | ((cc_uint32)maxY << 24);
    entry->depth = nearestDepth;
    entry->key = key;
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
    return nearestDepth && SoftFP_RectOccluded(minX, minY, maxX, maxY, nearestDepth);
}
#endif

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
cc_bool Gfx_IsBoundMapOccluded(void) {
#if defined SOFTFP_FAST_TERRAIN_KERNEL && defined CC_BUILD_SOFTFP_FLOAT_XFORM
    if (!gfx_mapCullData || !depthTest) return false;
    rasterChunkHiZTests++;
    if (!SoftFP_LocalBoundsOccluded(&gfx_mapCullData->aggregate)) return false;
    rasterChunkHiZCull++;
    return true;
#else
    return false;
#endif
}

cc_bool Gfx_IsBoundMapRangeOccluded(int firstVertex, int verticesCount) {
#if defined SOFTFP_FAST_TERRAIN_KERNEL && defined CC_BUILD_SOFTFP_FLOAT_XFORM
    SoftFPMapQuadBounds aggregate;
    cc_uint32 firstQuad, quadCount, endQuad, i;
    if (!gfx_mapCullData || !gfx_mapCullBounds || !depthTest || verticesCount <= 0) return false;
    firstQuad = (cc_uint32)firstVertex >> 2;
    quadCount = (cc_uint32)verticesCount >> 2;
    endQuad = firstQuad + quadCount;
    if (!quadCount || endQuad > gfx_mapCullData->quadCount) return false;

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

    rasterGroupHiZTests++;
    if (!SoftFP_LocalBoundsOccluded(&aggregate)) return false;
    rasterGroupHiZCull++;
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
static CC_INLINE cc_bool SoftFP_FastTerrainState(void) {
    return gfx_format == VERTEX_FORMAT_TEXTURED && !texSinglePixel &&
           colWrite && depthTest && depthWrite && gfx_alphaTest &&
           !gfx_alphaBlend;
}

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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    cc_uint32 fastCutoutPixelsLocal = 0;
#ifdef CC_BUILD_SOFTFP_FOG4
    cc_uint32 fogPixelsLocal = 0;
#endif
#endif

    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
    rasterBboxPixels += (cc_uint64)(maxX - minX + 1) *
                        (cc_uint64)(maxY - minY + 1);

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
                fastColor = APPLY_FOG4(fastColor, w_interp, fogPixelsLocal);
#endif
                StoreRasterGroup(x, y, fastColor);
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
                fastCutoutPixelsLocal++;
#endif
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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    rasterFastCutoutPixels += fastCutoutPixelsLocal;
#ifdef CC_BUILD_SOFTFP_FOG4
    rasterFogPixels += fogPixelsLocal;
#endif
#endif
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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    cc_uint32 fastCutoutPixelsLocal = 0;
#ifdef CC_BUILD_SOFTFP_FOG4
    cc_uint32 fogPixelsLocal = 0;
#endif
#endif

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
    rasterBboxPixels += (cc_uint64)(maxX - minX + 1) *
                        (cc_uint64)(maxY - minY + 1);
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
                    fastColor = APPLY_FOG4(fastColor, w_interp, fogPixelsLocal);
#endif
                    StoreRasterGroup(x, y, fastColor);
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
                    fastCutoutPixelsLocal++;
#endif
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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    rasterFastCutoutPixels += fastCutoutPixelsLocal;
#ifdef CC_BUILD_SOFTFP_FOG4
    rasterFogPixels += fogPixelsLocal;
#endif
#endif
}
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
    int z0 = V0->z, z1 = V1->z, z2 = V2->z;
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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    cc_uint32 fastCutoutPixelsLocal = 0;
#endif
#endif
#if defined CC_BUILD_SOFTFP_FOG4 && defined CC_BUILD_SOFTFP_PIXEL_PROBES
    cc_uint32 fogPixelsLocal = 0;
#endif
    PackedCol color = V0->c;
    cc_bool texturing = gfx_format == VERTEX_FORMAT_TEXTURED;
#ifdef CC_BUILD_SOFTFP_FAST_CUTOUT
    cc_bool fastCutout = false;
#endif

    if (maxX < 0 || minX > rasterMaxX || maxY < 0 || minY > rasterMaxY) return;
    minX = max(minX, 0); maxX = min(maxX, rasterMaxX);
    minY = max(minY, 0); maxY = min(maxY, rasterMaxY);
#ifdef CC_BUILD_ESP32S3
    rasterBboxPixels += (cc_uint64)(maxX - minX + 1) *
                        (cc_uint64)(maxY - minY + 1);
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

#ifdef CC_BUILD_SOFTFP_FAST_CUTOUT
    /* Normal terrain is textured A1 cutout with depth read/write and no
     * blending. Select that state once per triangle so its inner loop avoids
     * all of the generic colour/depth/blend state branches. */
    fastCutout = texturing && colWrite && depthTest && depthWrite &&
                 gfx_alphaTest && !gfx_alphaBlend && a1 >= 0x80;
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

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        {
            int perspectiveU = 0, perspectiveV = 0;
            int perspectiveStepU = 0, perspectiveStepV = 0;
            int perspectiveEndU = 0, perspectiveEndV = 0;
            int reciprocalEnd = 0, perspectiveRemain = 0;
            cc_bool perspectiveValid = false;
#endif

        for (x = spanMinX; x <= spanMaxX; x++) {
            int reciprocalW = 0;
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
                    fastColor = APPLY_FOG4(fastColor, w_interp, fogPixelsLocal);
#endif
                    StoreRasterGroup(x, y, fastColor);
                }
#else
                {
                    SoftPixel fastColor = SoftPixel_FromTexelTint(fastTexel, r1, g1, b1);
#ifdef CC_BUILD_SOFTFP_FOG4
                    fastColor = APPLY_FOG4(fastColor, w_interp, fogPixelsLocal);
#endif
                    colorBuffer[fastCbIndex] = fastColor;
                }
#endif
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_PIXEL_PROBES
                fastCutoutPixelsLocal++;
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
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    rasterFastCutoutPixels += fastCutoutPixelsLocal;
#ifdef CC_BUILD_SOFTFP_FOG4
    rasterFogPixels += fogPixelsLocal;
#endif
#endif
#endif
}

static CC_INLINE void DrawTriangle3D(VertexFixed* V0, VertexFixed* V1,
                                     VertexFixed* V2, int area, int factor,
                                     int perspectiveShift) {
#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    if (SoftFP_FastTerrainState() && PackedCol_A(V0->c) >= 0x80) {
        DrawTriangleFastTerrain(V0, V1, V2, area, factor, perspectiveShift);
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
#ifdef CC_BUILD_ESP32S3
    int perspectiveShift = -1;
#else
    int perspectiveShift = forceAffine ? -1 : 2;
#endif
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    int minW = INT_MAX;
#endif
    if (polyCount < 3) return;
#ifdef CC_BUILD_ESP32S3
    rasterProjectedVertices += (cc_uint32)polyCount;
    rasterProjectionReuse += (cc_uint32)((polyCount - 2) * 3 - polyCount);
#endif

    /* Project each unique polygon vertex once. Fast map quads stay quads;
     * other polygons are emitted as a triangle fan.
     * The old path repeated the reciprocal-W viewport transform six times
     * per quad, and repeated another six reciprocals when culling was active. */
    for (i = 0; i < polyCount; i++) {
        if (!ViewportVertex3D(&inVerts[i])) return;
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
        if (inVerts[i].w < minW) minW = inVerts[i].w;
#endif
    }

#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
    if (optimizeMap && !forceAffine) perspectiveShift = 0;
#endif

#ifdef SOFTFP_FAST_TERRAIN_KERNEL
    if (optimizeMap && polyCount == 4 && SoftFP_FastTerrainState() &&
        PackedCol_A(inVerts[0].c) >= 0x80) {
        VertexFixed* A = &inVerts[0];
        VertexFixed* B = &inVerts[1];
        VertexFixed* C = &inVerts[2];
        VertexFixed* D = &inVerts[3];

        rasterHiZTests++;
        if (SoftFP_TerrainQuadOccluded(inVerts)) {
            rasterHiZCull++;
            return;
        }

        int area = SoftFP_RasterEdge(
            SOFTFP_RASTER_COORD(A->x), SOFTFP_RASTER_COORD(A->y),
            SOFTFP_RASTER_COORD(B->x), SOFTFP_RASTER_COORD(B->y),
            SOFTFP_RASTER_COORD(C->x), SOFTFP_RASTER_COORD(C->y));
        int area2 = SoftFP_RasterEdge(
            SOFTFP_RASTER_COORD(A->x), SOFTFP_RASTER_COORD(A->y),
            SOFTFP_RASTER_COORD(C->x), SOFTFP_RASTER_COORD(C->y),
            SOFTFP_RASTER_COORD(D->x), SOFTFP_RASTER_COORD(D->y));

        /* Only take the direct path for a consistently wound convex quad.
         * Degenerate and unusual model-like inputs retain the proven fan. */
        if (ABS(area) > SOFTFP_RASTER_FP_ONE &&
            ABS(area2) > SOFTFP_RASTER_FP_ONE &&
            ((area ^ area2) >= 0)) {
            int factor;
            if (faceCulling && area > 0) return;
            factor = FixedReciprocalFast(area);
#ifdef CC_BUILD_SOFTFP_PERSPECTIVE4
            if (!perspectiveShift) {
                perspectiveShift = SoftFP_SelectPerspectiveShift(A, B, C, factor, minW);
            }
#endif
            DrawQuadFastTerrain(inVerts, area, factor, perspectiveShift);
#ifdef CC_BUILD_ESP32S3
            rasterTerrainQuads++;
#endif
            return;
        }
    }
#endif

#ifdef CC_BUILD_ESP32S3
    holo_perf_count_triangles((uint32_t)(polyCount - 2));
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
        result = (int64_t)v->w - (v->z >> 2); //hacked
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
    int i;

    if (!planeMask) {
        ProcessClippedTriangleAndDraw(quad, 4, optimizeMap, forceAffine);
        return;
    }

    for (i = 0; i < 4; i++) {
        cc_uint32 code = VertexClipOutcode(&quad[i], planeMask);
        clipOr  |= code;
        clipAnd &= code;
    }
    if (clipAnd) return;
    if (!clipOr) {
        ProcessClippedTriangleAndDraw(quad, 4, optimizeMap, forceAffine);
        return;
    }

    int polyCount = ClipQuadToFrustumFixed(quad, buf1, buf2, &outPoly, clipOr);
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
    cc_uint32 quadCullLocal = 0;
    cc_uint32 spriteCullLocal = 0;
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
            gfx_temporal_quad_key = 0;
            if ((hints & SOFTFP_MAP_BATCH_HINT) && mapClipPlanes &&
                SoftFP_MapQuadOutsideFrustum(j, mapClipPlanes)) {
#ifdef CC_BUILD_ESP32S3
                quadCullLocal++;
                if (hints & DRAW_HINT_SPRITE) spriteCullLocal++;
#endif
                continue;
            }
            if (optimizeMap) {
                gfx_temporal_quad_key = SoftFP_TemporalQuadKey(j);
                if (SoftFP_TemporalRectOccluded(gfx_temporal_quad_key)) {
                    gfx_temporal_quad_key = 0;
                    continue;
                }
            }
#endif
            TransformVertex3D(j + 0, &vertices[0], optimizeMap);
            TransformVertex3D(j + 1, &vertices[1], optimizeMap);
            TransformVertex3D(j + 2, &vertices[2], optimizeMap);
            TransformVertex3D(j + 3, &vertices[3], optimizeMap);
            
            DrawClippedFixed(vertices, clipPlaneMask, optimizeMap, forceAffine);
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
            gfx_temporal_quad_key = 0;
#endif
        }
		}

		if (tex_offseting && !optimizeMap) UnshiftTextureCoords(verticesCount);
    }
#ifdef CC_BUILD_ESP32S3
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    rasterQuadCullTotal += quadCullLocal;
    rasterSpriteCull += spriteCullLocal;
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
#ifdef CC_BUILD_SOFTFP_Z16_TILES
    holo_perf_add(HOLO_PERF_DEPTH_TILE_HIT, depthCacheHits);
    holo_perf_add(HOLO_PERF_DEPTH_TILE_MISS, depthCacheMisses);
    holo_perf_add(HOLO_PERF_DEPTH_TILE_WRITEBACK, depthCacheWritebacks);
#endif
#ifdef CC_BUILD_ESP32S3
    holo_perf_count_bbox_pixels(rasterBboxPixels);
    holo_perf_add(HOLO_PERF_RASTER_SPAN_PIXELS, rasterSpanPixels);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_TRIANGLES, rasterPerspectiveTriangles);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_PIXELS, rasterPerspectivePixels);
    holo_perf_add(HOLO_PERF_PERSPECTIVE_RECIPROCALS, rasterPerspectiveReciprocals);
    holo_perf_add(HOLO_PERF_FRUSTUM_CACHE_HIT, rasterFrustumCacheHits);
    holo_perf_add(HOLO_PERF_FRUSTUM_CACHE_MISS, rasterFrustumCacheMisses);
    holo_perf_add(HOLO_PERF_RASTER_QUADS, rasterTerrainQuads);
    holo_perf_add(HOLO_PERF_SPRITE_QUADS, rasterSpriteQuads);
    holo_perf_add(HOLO_PERF_SPRITE_CULL, rasterSpriteCull);
    holo_perf_add(HOLO_PERF_HIZ_TESTS, rasterHiZTests);
    holo_perf_add(HOLO_PERF_HIZ_CULL, rasterHiZCull);
    holo_perf_add(HOLO_PERF_CHUNK_HIZ_TESTS, rasterChunkHiZTests);
    holo_perf_add(HOLO_PERF_CHUNK_HIZ_CULL, rasterChunkHiZCull);
    holo_perf_add(HOLO_PERF_GROUP_HIZ_TESTS, rasterGroupHiZTests);
    holo_perf_add(HOLO_PERF_GROUP_HIZ_CULL, rasterGroupHiZCull);
    holo_perf_add(HOLO_PERF_TEMPORAL_HIZ_HIT, rasterTemporalHiZHit);
    holo_perf_add(HOLO_PERF_TEMPORAL_HIZ_MISS, rasterTemporalHiZMiss);
    holo_perf_add(HOLO_PERF_TEMPORAL_HIZ_CULL, rasterTemporalHiZCull);
    holo_perf_add(HOLO_PERF_PROJECTED_VERTICES, rasterProjectedVertices);
    holo_perf_add(HOLO_PERF_PROJECTION_REUSE, rasterProjectionReuse);
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    holo_perf_add(HOLO_PERF_FAST_CUTOUT_PIXELS, rasterFastCutoutPixels);
    holo_perf_add(HOLO_PERF_FOG_PIXELS, rasterFogPixels);
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    holo_perf_add(HOLO_PERF_QUAD_CULL, rasterQuadCullTotal);
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
#if defined CC_BUILD_SOFTFP_ASYNC_CLEAR && defined CC_BUILD_SOFTFP_FB16 && !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
    if (asyncPresenterClear) {
        holo_port_presenter_schedule_clears(
            (cc_uint16)SoftPixel_FromBitmap(clearColor),
            asyncDepthClear ? (cc_uint32*)depthBuffer : NULL,
            asyncDepthClear ? (size_t)db_stride * db_height : 0,
            0x7FFFFFFFu);
    }
#endif
    Window_DrawFramebuffer(r, &fb_bmp);
#if defined CC_BUILD_SOFTFP_ASYNC_CLEAR && !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_W_DEPTH8 && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
    if (asyncDepthClear) {
        depthBufferIndex ^= 1u;
        depthBuffer = depthBuffers[depthBufferIndex];
    }
#endif
    /* The Holo backend may hand the completed buffer to its Core 0 presenter. */
    colorBuffer = (SoftPixel*)fb_bmp.scan0;
}

void Gfx_SetVSync(cc_bool vsync) {
    gfx_vsync = vsync;
}

void Gfx_OnWindowResize(int width, int height) {
    if (depthBuffer) DestroyBuffers();

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
    rasterColorBuffer = (SoftPixel*)Mem_Alloc(
        raster_width, raster_height * sizeof(SoftPixel), "192x144 staged color buffer");
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
    asyncPresenterClear = holo_port_presenter_async_available();
#ifdef SOFTFP_LOWRES_COLOR_STAGE
    /* The low-resolution stage is cleared on Core 1 and overwrites the whole
     * native framebuffer once before UI, so clearing the native buffer on the
     * presenter would only add redundant Core 0 traffic. */
    asyncPresenterClear = false;
#endif
    asyncDepthClear = false;
#elif defined CC_BUILD_SOFTFP_ASYNC_CLEAR && !defined CC_BUILD_SOFTFP_Z16_TILES && !defined CC_BUILD_SOFTFP_Z32_FASTCLEAR
    depthBuffers[0] = Mem_Alloc(db_stride * db_height, sizeof(DepthValue), "depth buffer A");
    depthBuffers[1] = NULL;
    depthBufferIndex = 0;
    depthBuffer = depthBuffers[0];
    asyncPresenterClear = holo_port_presenter_async_available();
    if (asyncPresenterClear) {
        depthBuffers[1] = Mem_TryAlloc(db_stride * db_height, sizeof(DepthValue));
        if (depthBuffers[1]) {
            FillDepth32(depthBuffers[0], db_stride * db_height);
            FillDepth32(depthBuffers[1], db_stride * db_height);
            asyncDepthClear = true;
        }
    }
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
    Platform_LogConst("[cc.gfx] texture=PS1-style affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
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
    Platform_LogConst("[cc.gfx] texture=PS1-style affine A1RGB555 framebuffer=RGB565 alpha=fixed50 extras=off");
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
#elif defined CC_BUILD_SOFTFP_ASYNC_CLEAR
    if (asyncDepthClear) {
#ifdef CC_BUILD_SOFTFP_W_DEPTH
        Platform_LogConst("[cc.gfx] depth=reciprocal-W Z32 double-buffer clear=Core0");
#else
        Platform_LogConst("[cc.gfx] depth=Z32 original-order double-buffer clear=Core0");
#endif
    } else {
#ifdef CC_BUILD_SOFTFP_W_DEPTH
        Platform_LogConst("[cc.gfx] depth=reciprocal-W Z32 single-buffer clear=Core1");
#else
        Platform_LogConst("[cc.gfx] depth=Z32 original-order single-buffer clear=Core1");
#endif
    }
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
    Platform_LogConst("[cc.gfx] color=192x144 PSRAM stage upscale=packed-single-pass-before-UI");
#endif
#endif
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
    Platform_LogConst("[cc.gfx] frontend=map-only quad-raster AABB cull-cache packed-local/8 Hi-Z4/8+groups+temporal");
#endif
    Platform_LogConst("[cc.gfx] terrain=PS1-style affine, full-rate sampling");
#ifdef CC_BUILD_SOFTFP_TERRAIN_TILE_CACHE
    if (terrainTileCacheMemory) {
        Platform_LogConst("[cc.gfx] terrain-texture-cache=8x16x16 A1RGB555 internal");
    } else {
        Platform_LogConst("[cc.gfx] terrain-texture-cache unavailable; direct PSRAM sampling");
    }
#endif
#ifdef CC_BUILD_SOFTFP_FOG4
    Platform_LogConst("[cc.gfx] fog=4-level quarter-strength");
#endif
#ifdef CC_BUILD_ESP32S3
#ifdef CC_BUILD_SOFTFP_PIXEL_PROBES
    Platform_LogConst("[cc.gfx] probes=pipeline+pixel");
#else
    Platform_LogConst("[cc.gfx] probes=pipeline (cutPx/fogPx disabled)");
#endif
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
