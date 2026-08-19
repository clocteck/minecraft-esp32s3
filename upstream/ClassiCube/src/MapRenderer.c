#include "MapRenderer.h"
#include "Block.h"
#include "Builder.h"
#include "Camera.h"
#include "Entity.h"
#include "EnvRenderer.h"
#include "Event.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "Platform.h"
#include "TexturePack.h"
#include "Utils.h"
#include "World.h"
#include "Options.h"
#ifdef CC_BUILD_ESP32S3
#include "holo_perf.h"
#endif

int MapRenderer_1DUsedCount;
struct ChunkPartInfo* MapRenderer_PartsNormal;
struct ChunkPartInfo* MapRenderer_PartsTranslucent;

static cc_bool inTranslucent;
static IVec3 chunkPos;

/* The number of non-empty Normal/Translucent ChunkPartInfos (across entire world) for each 1D atlas batch. */
/* 1D atlas batches that do not have any ChunkPartInfos can be entirely skipped. */
static int normPartsCount[ATLAS1D_MAX_ATLASES], tranPartsCount[ATLAS1D_MAX_ATLASES];
/* Whether there are any visible Normal/Translucent ChunkPartInfos for each 1D atlas batch. */
/* 1D atlas batches that do not have any visible ChunkPartInfos can be skipped. */
static cc_bool hasNormParts[ATLAS1D_MAX_ATLASES], hasTranParts[ATLAS1D_MAX_ATLASES];
/* Whether renderer should check if there are any visible Normal/Translucent ChunkPartInfos for each 1D atlas batch. */
static cc_bool checkNormParts[ATLAS1D_MAX_ATLASES], checkTranParts[ATLAS1D_MAX_ATLASES];

/* Render info for all chunks in the world. Unsorted. */
static struct ChunkInfo* mapChunks;
/* Pointers to render info for all chunks in the world, sorted by distance from the camera. */
static struct ChunkInfo** sortedChunks;
/* Pointers to render info for all chunks in the world, sorted by distance from the camera. */
/* Only chunks that can be rendered (i.e. not empty and are visible) are included in this.  */
static struct ChunkInfo** renderChunks;
/* Number of actually used pointers in the renderChunks array. Entries past this are ignored and skipped. */
static int renderChunksCount;
/* Distance of each chunk from the camera. */
static cc_uint32* distances;
/* Maximum number of chunk updates that can be performed in one frame. */
static int maxChunkUpdates;
/* Cached number of chunks in the world */
static int chunksCount;
/* Set by block/environment refreshes. Once all in-range dirty chunks have
 * rebuilt, a stationary camera can reuse renderChunks without scanning the
 * entire world every frame. */
static cc_bool chunksNeedScan;

static void ChunkInfo_Init(struct ChunkInfo* chunk, int x, int y, int z) {
	chunk->centreX = x + HALF_CHUNK_SIZE; chunk->centreY = y + HALF_CHUNK_SIZE; 
	chunk->centreZ = z + HALF_CHUNK_SIZE;
#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
	chunk->vb = 0;
#endif

	chunk->visible  = true;  
	chunk->empty    = false;
	chunk->allAir   = false;
	chunk->noData   = true;
	chunk->dirty    = true;
	chunk->skipClip = false;
	chunk->buildFailed = false;
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
	chunk->softClipPlanes = 0x3F;
	chunk->softCullBounds = NULL;
	chunk->meshVertices   = 0;
#endif

	chunk->drawXMin = false; chunk->drawXMax = false; chunk->drawZMin = false;
	chunk->drawZMax = false; chunk->drawYMin = false; chunk->drawYMax = false;

	chunk->normalParts      = NULL;
	chunk->translucentParts = NULL;
}

static CC_INLINE void ChunkInfo_Refresh(struct ChunkInfo* chunk) {
	if (chunk->allAir) return; /* do not recreate chunks completely air */

	chunk->empty = false;
	chunk->dirty = true;
	chunk->buildFailed = false;
	chunksNeedScan = true;
}

/* Index of maximum used 1D atlas + 1 */
CC_NOINLINE static int MapRenderer_UsedAtlases(void) {
	TextureLoc maxLoc = 0;
	int i;

	for (i = 0; i < Array_Elems(Blocks.Textures); i++) {
		maxLoc = max(maxLoc, Blocks.Textures[i]);
	}
	return Atlas1D_Index(maxLoc) + 1;
}


/*########################################################################################################################*
*-------------------------------------------------------Map rendering-----------------------------------------------------*
*#########################################################################################################################*/
static void CheckWeather(float delta) {
	IVec3 pos;
	BlockID block;
	cc_bool outside;
	IVec3_Floor(&pos, &Camera.CurrentPos);

	block   = World_SafeGetBlock(pos.x, pos.y, pos.z);
	outside = pos.y < 0 || !World_ContainsXZ(pos.x, pos.z);
	inTranslucent = Blocks.Draw[block] == DRAW_TRANSLUCENT || (pos.y < Env.EdgeHeight && outside);

	/* If we are under water, render weather before to blend properly */
	if (!inTranslucent || Env.Weather == WEATHER_SUNNY) return;
	Gfx_SetAlphaBlending(true);
	EnvRenderer_RenderWeather(delta);
	Gfx_SetAlphaBlending(false);
}

#ifdef CC_CLIPPING_FLAGS
	#define DrawBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, info->skipClip ? DRAW_HINT_NOCLIP : 0)
	#define DrawBatchHint(count, start, hint) Gfx_DrawIndexedTris_T2fC4b(count, start, \
		(DrawHints)((info->skipClip ? DRAW_HINT_NOCLIP : 0) | (hint)))
	#define DrawSpriteBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, \
		(DrawHints)((info->skipClip ? DRAW_HINT_NOCLIP : 0) | DRAW_HINT_SPRITE | lodHint))
#else
	#define DrawBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, 0)
	#define DrawBatchHint(count, start, hint) Gfx_DrawIndexedTris_T2fC4b(count, start, hint)
	#define DrawSpriteBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, \
		(DrawHints)(DRAW_HINT_SPRITE | lodHint))
#endif

#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
	#define DrawFace(face, ign)    Gfx_BindVb(part.vbs[face]); DrawBatch(0, 0);
	#define DrawFaces(f1, f2, ign) DrawFace(f1, ign); DrawFace(f2, ign);
#else
	#define DrawFace(face, offset)    DrawBatch(part.counts[face], offset);
	#define DrawFaceHint(face, offset, hint) DrawBatchHint(part.counts[face], offset, hint);
	#define DrawFaces(f1, f2, offset) DrawBatch(part.counts[f1] + part.counts[f2], offset);
#endif

#if defined CC_BUILD_ESP32S3 && CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
#define DrawNormalFaceVisible(face, faceOffset, faceHint) do { \
	int faceCount = part.counts[face]; \
	if (!Gfx_IsBoundMapRangeOccluded((faceOffset), faceCount)) { \
		DrawFaceHint((face), (faceOffset), (faceHint)); \
		Game_Vertices += faceCount; \
	} \
} while (0)
#define DrawNormalFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	Gfx_SetFaceCulling(true); \
	DrawNormalFaceVisible(minFace, offset, lodHint); \
	DrawNormalFaceVisible(maxFace, offset + part.counts[minFace], (DrawHints)(lodHint | DRAW_HINT_REVERSE)); \
	Gfx_SetFaceCulling(false); \
} else if (drawMin) { \
	DrawNormalFaceVisible(minFace, offset, lodHint); \
} else if (drawMax) { \
	DrawNormalFaceVisible(maxFace, offset + part.counts[minFace], (DrawHints)(lodHint | DRAW_HINT_REVERSE)); \
}
#else
#define DrawNormalFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	Gfx_SetFaceCulling(true); \
	DrawFaces(minFace, maxFace, offset); \
	Gfx_SetFaceCulling(false); \
	Game_Vertices += (part.counts[minFace] + part.counts[maxFace]); \
} else if (drawMin) { \
	DrawFace(minFace, offset); \
	Game_Vertices += part.counts[minFace]; \
} else if (drawMax) { \
	DrawFace(maxFace, offset + part.counts[minFace]); \
	Game_Vertices += part.counts[maxFace]; \
}
#endif

#ifdef CC_BUILD_ESP32S3
static cc_bool ChunkUsesFarLod(const struct ChunkInfo* info) {
	float dx = Math_AbsF(Camera.CurrentPos.x - info->centreX) - HALF_CHUNK_SIZE;
	float dz = Math_AbsF(Camera.CurrentPos.z - info->centreZ) - HALF_CHUNK_SIZE;
	float fullDetail = min((float)Game_ViewDistance, 28.0f);
	if (dx < 0.0f) dx = 0.0f;
	if (dz < 0.0f) dz = 0.0f;
	/* Geometry stays complete throughout the effective view. Only tiny crossed
	 * vegetation past 28 blocks uses the far-detail policy below. */
	return dx * dx + dz * dz > fullDetail * fullDetail;
}
#endif

static void RenderNormalBatch(int batch) {
	int batchOffset = chunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset, count;
#ifdef CC_BUILD_ESP32S3
	cc_bool farLod;
	DrawHints lodHint;
#else
	DrawHints lodHint = DRAW_HINT_NONE;
#endif

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->normalParts) continue;

		part = info->normalParts[batchOffset];
		if (part.offset < 0) continue;
		hasNormParts[batch] = true;
#ifdef CC_BUILD_ESP32S3
		farLod = ChunkUsesFarLod(info);
		/* Nearby large oblique faces may opt into sparse 8-pixel perspective
		 * correction. The far ring stays purely affine. */
		lodHint = farLod ? DRAW_HINT_AFFINE : DRAW_HINT_NONE;
#endif

#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
		Gfx_SetMapClipPlanes(info->softClipPlanes);
		Gfx_BindMapCullBounds(info->softCullBounds,
			info->centreX - 8, info->centreY - 8, info->centreZ - 8);
		if (Gfx_IsBoundMapOccluded()) continue;
#endif
#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
		Gfx_BindVb_Textured(info->vb);
#endif

#if defined CC_BUILD_ESP32S3 && CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
		{
			int axisOrder[3] = { 0, 1, 2 }; /* X, Z, Y */
			int axisDistance[3];
			int axisOffset[3];
			int a, b;

			/* The chunk list is already near-to-far, but faces inside each chunk
			 * used to be fixed X/Z/Y. Draw the nearest face axis first so a camera
			 * above the terrain establishes top-surface depth before side walls. */
			axisDistance[0] = Math_AbsI((int)Camera.CurrentPos.x - info->centreX);
			axisDistance[1] = Math_AbsI((int)Camera.CurrentPos.z - info->centreZ);
			axisDistance[2] = Math_AbsI((int)Camera.CurrentPos.y - info->centreY);
			axisOffset[0] = part.offset + part.spriteCount;
			axisOffset[1] = axisOffset[0] + part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
			axisOffset[2] = axisOffset[1] + part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];

			for (a = 0; a < 2; a++) {
				for (b = a + 1; b < 3; b++) {
					if (axisDistance[axisOrder[b]] > axisDistance[axisOrder[a]]) {
						int swap = axisOrder[a]; axisOrder[a] = axisOrder[b]; axisOrder[b] = swap;
					}
				}
			}

			for (a = 0; a < 3; a++) {
				switch (axisOrder[a]) {
				case 0:
					offset  = axisOffset[0];
					drawMin = info->drawXMin && part.counts[FACE_XMIN];
					drawMax = info->drawXMax && part.counts[FACE_XMAX];
					DrawNormalFaces(FACE_XMIN, FACE_XMAX);
					break;
				case 1:
					offset  = axisOffset[1];
					drawMin = info->drawZMin && part.counts[FACE_ZMIN];
					drawMax = info->drawZMax && part.counts[FACE_ZMAX];
					DrawNormalFaces(FACE_ZMIN, FACE_ZMAX);
					break;
				default:
					offset  = axisOffset[2];
					drawMin = info->drawYMin && part.counts[FACE_YMIN];
					drawMax = info->drawYMax && part.counts[FACE_YMAX];
					DrawNormalFaces(FACE_YMIN, FACE_YMAX);
					break;
				}
			}
		}
#else
		offset  = part.offset + part.spriteCount;
		drawMin = info->drawXMin && part.counts[FACE_XMIN];
		drawMax = info->drawXMax && part.counts[FACE_XMAX];
		DrawNormalFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
		drawMin = info->drawZMin && part.counts[FACE_ZMIN];
		drawMax = info->drawZMax && part.counts[FACE_ZMAX];
		DrawNormalFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
		drawMin = info->drawYMin && part.counts[FACE_YMIN];
		drawMax = info->drawYMax && part.counts[FACE_YMAX];
		DrawNormalFaces(FACE_YMIN, FACE_YMAX);
#endif

		if (!part.spriteCount) continue;
#ifdef CC_BUILD_ESP32S3
		/* Past the 28-block full-detail radius, omit tiny crossed-plane
		 * vegetation while keeping all solid terrain geometry. */
		if (farLod) continue;
#endif
		offset = part.offset;
		count  = part.spriteCount >> 2; /* 4 per sprite */

		Gfx_SetFaceCulling(true);
		/* TODO: fix to not render them all */
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
		Gfx_BindVb(part.vbs[FACE_COUNT]);
		DrawBatch(0, 0);
		Game_Vertices += count * 4;
		Gfx_SetFaceCulling(false);
		continue;
#endif
#ifdef CC_BUILD_ESP32S3
		{
			/* Each plant contains both windings of two crossed planes. A chunk's
			 * old axis-OR test submits three (and sometimes all four) windings,
			 * leaving the rasterizer to reject the extras. Select one winding
			 * whenever every sprite in this 16x16 chunk is provably on the same
			 * side of the diagonal plane; retain both near a diagonal boundary. */
			float relX = Camera.CurrentPos.x - info->centreX;
			float relZ = Camera.CurrentPos.z - info->centreZ;
			float diagonal = relX - relZ;
			float antiDiagonal = relX + relZ;

			if (diagonal >= -16.0f) {
				DrawSpriteBatch(count, offset); Game_Vertices += count;
			}
			if (diagonal <= 16.0f) {
				DrawSpriteBatch(count, offset + count); Game_Vertices += count;
			}
			offset += count * 2;

			if (antiDiagonal <= 16.0f) {
				DrawSpriteBatch(count, offset); Game_Vertices += count;
			}
			if (antiDiagonal >= -16.0f) {
				DrawSpriteBatch(count, offset + count); Game_Vertices += count;
			}
		}
#else
		if (info->drawXMax || info->drawZMin) {
			DrawSpriteBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMin || info->drawZMax) {
			DrawSpriteBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMin || info->drawZMin) {
			DrawSpriteBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMax || info->drawZMax) {
			DrawSpriteBatch(count, offset); Game_Vertices += count;
		}
#endif
		Gfx_SetFaceCulling(false);
	}
}

void MapRenderer_RenderNormal(float delta) {
	int batch;
	if (!mapChunks) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaTest(true);
	
	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (normPartsCount[batch] <= 0) continue;
		if (hasNormParts[batch] || checkNormParts[batch]) {
			Atlas1D_Bind(batch);
			RenderNormalBatch(batch);
			checkNormParts[batch] = false;
		}
	}
	Gfx_DisableMipmaps();

	CheckWeather(delta);
	Gfx_SetAlphaTest(false);
#if DEBUG_OCCLUSION
	DebugPickedPos();
#endif
}

#if defined CC_BUILD_ESP32S3 && CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
#define DrawTranslucentFaces(minFace, maxFace) \
if (drawMin) { \
	DrawFaceHint(minFace, offset, lodHint); \
	Game_Vertices += part.counts[minFace]; \
} \
if (drawMax) { \
	DrawFaceHint(maxFace, offset + part.counts[minFace], lodHint); \
	Game_Vertices += part.counts[maxFace]; \
}
#else
#define DrawTranslucentFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	DrawFaces(minFace, maxFace, offset); \
	Game_Vertices += (part.counts[minFace] + part.counts[maxFace]); \
} else if (drawMin) { \
	DrawFace(minFace, offset); \
	Game_Vertices += part.counts[minFace]; \
} else if (drawMax) { \
	DrawFace(maxFace, offset + part.counts[minFace]); \
	Game_Vertices += part.counts[maxFace]; \
}
#endif

static void RenderTranslucentBatch(int batch) {
	int batchOffset = chunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset;
#ifdef CC_BUILD_ESP32S3
	DrawHints lodHint;
#else
	DrawHints lodHint = DRAW_HINT_NONE;
#endif

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->translucentParts) continue;

		part = info->translucentParts[batchOffset];
		if (part.offset < 0) continue;
		hasTranParts[batch] = true;
#ifdef CC_BUILD_ESP32S3
		lodHint = DRAW_HINT_AFFINE;
#endif

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
		Gfx_BindVb_Textured(info->vb);
#endif
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
		Gfx_SetMapClipPlanes(info->softClipPlanes);
		Gfx_BindMapCullBounds(info->softCullBounds,
			info->centreX - 8, info->centreY - 8, info->centreZ - 8);
#endif

		offset  = part.offset;
		drawMin = (inTranslucent || info->drawXMin) && part.counts[FACE_XMIN];
		drawMax = (inTranslucent || info->drawXMax) && part.counts[FACE_XMAX];
		DrawTranslucentFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
		drawMin = (inTranslucent || info->drawZMin) && part.counts[FACE_ZMIN];
		drawMax = (inTranslucent || info->drawZMax) && part.counts[FACE_ZMAX];
		DrawTranslucentFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
		drawMin = (inTranslucent || info->drawYMin) && part.counts[FACE_YMIN];
		drawMax = (inTranslucent || info->drawYMax) && part.counts[FACE_YMAX];
		DrawTranslucentFaces(FACE_YMIN, FACE_YMAX);
	}
}

void MapRenderer_RenderTranslucent(float delta) {
	int batch;
#ifndef CC_BUILD_ESP32S3
	int vertices;
#endif
	if (!mapChunks) return;

#ifndef CC_BUILD_ESP32S3
	/* First fill depth buffer */
	vertices = Game_Vertices;
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaBlending(false);
	Gfx_DepthOnlyRendering(true);

	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (tranPartsCount[batch] <= 0) continue;
		if (hasTranParts[batch] || checkTranParts[batch]) {
			RenderTranslucentBatch(batch);
			checkTranParts[batch] = false;
		}
	}
	Game_Vertices = vertices;
#else
	/* renderChunks is sorted near-to-far. On the software ESP32-S3 backend,
	 * blend the nearest translucent surface and commit its depth in one pass.
	 * The old generic path transformed and rasterised every water/glass face
	 * twice (depth prepass, then colour), despite retaining only the nearest
	 * translucent layer. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
#endif

	/* Then actually draw the transluscent blocks */
	Gfx_SetAlphaBlending(true);
	Gfx_DepthOnlyRendering(false);
#ifndef CC_BUILD_ESP32S3
	Gfx_SetDepthWrite(false); /* already calculated depth values in depth pass */
#else
	Gfx_SetDepthWrite(true);  /* combined colour + nearest translucent depth */
#endif

	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (tranPartsCount[batch] <= 0) continue;
#ifdef CC_BUILD_ESP32S3
		if (hasTranParts[batch] || checkTranParts[batch]) {
			Atlas1D_Bind(batch);
			RenderTranslucentBatch(batch);
			checkTranParts[batch] = false;
		}
#else
		if (!hasTranParts[batch]) continue;

		Atlas1D_Bind(batch);
		RenderTranslucentBatch(batch);
#endif
	}
	Gfx_DisableMipmaps();

#ifndef CC_BUILD_ESP32S3
	Gfx_SetDepthWrite(true);
#endif
	/* If we weren't under water, render weather after to blend properly */
	if (!inTranslucent && Env.Weather != WEATHER_SUNNY) {
		Gfx_SetAlphaTest(true);
		EnvRenderer_RenderWeather(delta);
		Gfx_SetAlphaTest(false);
	}
	Gfx_SetAlphaBlending(false);
#ifdef CC_BUILD_ESP32S3
	/* Complete queued translucent FIFO jobs before selection/held/UI writes. */
	Gfx_FlushRasterWorker();
#endif
}


/*########################################################################################################################*
*---------------------------------------------------Chunk functionality---------------------------------------------------*
*#########################################################################################################################*/
/* Deletes vertex buffer associated with the given chunk and updates internal state */
static void DeleteChunk(struct ChunkInfo* chunk) {
	struct ChunkPartInfo* ptr;
	int i;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
	int j;
#else
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
	Gfx_DeleteMapCullBounds(&chunk->softCullBounds);
	chunk->meshVertices = 0;
#endif
	Gfx_DeleteVb(&chunk->vb);
#endif

	chunk->empty  = false; 
	chunk->allAir = false;
	chunk->noData = true;
	chunk->dirty  = true;
	chunk->buildFailed = false;

#ifdef OCCLUSION
	chunk.OcclusionFlags = 0;
	chunk.OccludedFlags = 0;
#endif

	if (chunk->normalParts) {
		ptr = chunk->normalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset < 0) continue; 
			normPartsCount[i]--;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->vbs[j]);
#endif
		}
		chunk->normalParts = NULL;
	}

	if (chunk->translucentParts) {
		ptr = chunk->translucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset < 0) continue;
			tranPartsCount[i]--;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->vbs[j]);
#endif
		}
		chunk->translucentParts = NULL;
	}
}

/* Builds the mesh (hence vertex buffer) for the given chunk, and updates internal state */
static void BuildChunk(struct ChunkInfo* chunk, int* chunkUpdates) {
	struct ChunkPartInfo* ptr;
	int i;
#ifdef CC_BUILD_ESP32S3
	cc_uint64 perfMark;
	cc_bool buildResult;
#endif

	Game.ChunkUpdates++;
	(*chunkUpdates)++;
#ifdef CC_BUILD_ESP32S3
	perfMark = Stopwatch_Measure();
	buildResult = Builder_MakeChunk(chunk);
	holo_perf_add(HOLO_PERF_CHUNK_BUILD, Stopwatch_ElapsedMicroseconds(perfMark, Stopwatch_Measure()));
	holo_perf_count_chunk_build();
	if (!buildResult) {
		chunk->buildFailed = true;
		return;
	}
#else
	if (!Builder_MakeChunk(chunk)) return;
#endif

	chunk->dirty  = false;
	chunk->buildFailed = false;
	chunk->noData = !chunk->normalParts && !chunk->translucentParts;
	chunk->empty  = chunk->noData;
	if (chunk->empty) return;
	
	if (chunk->normalParts) {
		ptr = chunk->normalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset >= 0) normPartsCount[i]++;
		}
	}

	if (chunk->translucentParts) {
		ptr = chunk->translucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset >= 0) tranPartsCount[i]++;
		}
	}
}


/*########################################################################################################################*
*----------------------------------------------------Chunks mangagement---------------------------------------------------*
*#########################################################################################################################*/
static void FreeParts(void) {
	Mem_Free(MapRenderer_PartsNormal);
	MapRenderer_PartsNormal      = NULL;
	MapRenderer_PartsTranslucent = NULL;
}

static void FreeChunks(void) {
	Mem_Free(mapChunks);
	Mem_Free(sortedChunks);
	Mem_Free(renderChunks);
	Mem_Free(distances);

	mapChunks    = NULL;
	sortedChunks = NULL;
	renderChunks = NULL;
	distances    = NULL;
	chunksNeedScan = false;
}

static void AllocateParts(void) {
	struct ChunkPartInfo* ptr;
	cc_uint32 count = chunksCount * MapRenderer_1DUsedCount;

	ptr = (struct ChunkPartInfo*)Mem_AllocCleared(count * 2, sizeof(struct ChunkPartInfo), "chunk parts");
	MapRenderer_PartsNormal      = ptr;
	MapRenderer_PartsTranslucent = ptr + count;
}

static void AllocateChunks(void) {
	mapChunks    = (struct ChunkInfo*) Mem_Alloc(chunksCount, sizeof(struct ChunkInfo),  "chunk info");
	sortedChunks = (struct ChunkInfo**)Mem_Alloc(chunksCount, sizeof(struct ChunkInfo*), "sorted chunk info");
	renderChunks = (struct ChunkInfo**)Mem_Alloc(chunksCount, sizeof(struct ChunkInfo*), "render chunk info");
	distances    = (cc_uint32*)Mem_Alloc(chunksCount, 4, "chunk distances");
}

static void ResetPartFlags(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		checkNormParts[i] = true;
		hasNormParts[i]   = false;
		checkTranParts[i] = true;
		hasTranParts[i]   = false;
	}
}

static void ResetPartCounts(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		normPartsCount[i] = 0;
		tranPartsCount[i] = 0;
	}
}

static void InitChunks(void) {
	int x, y, z, index = 0;
	chunksNeedScan = true;
	for (z = 0; z < World.Length; z += CHUNK_SIZE) {
		for (y = 0; y < World.Height; y += CHUNK_SIZE) {
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				ChunkInfo_Init(&mapChunks[index], x, y, z);
				sortedChunks[index] = &mapChunks[index];
				renderChunks[index] = &mapChunks[index];
				distances[index]    = 0;
				index++;
			}
		}
	}
}

static void RefreshChunks(void) {
	int i;
	if (!mapChunks) return;

	for (i = 0; i < chunksCount; i++) 
	{
		ChunkInfo_Refresh(&mapChunks[i]);
	}
}

static void DeleteChunks(void) {
	int i;
	if (!mapChunks) return;

	for (i = 0; i < chunksCount; i++) 
	{
		DeleteChunk(&mapChunks[i]);
	}
	ResetPartCounts();
	chunksNeedScan = true;
}

void MapRenderer_Refresh(void) {
	int oldCount;
	chunkPos = IVec3_MaxValue();

	if (mapChunks && World.Blocks) {
		DeleteChunks();

		oldCount = MapRenderer_1DUsedCount;
		MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
		/* Need to reallocate parts array in this case */
		if (MapRenderer_1DUsedCount != oldCount) {
			FreeParts();
			AllocateParts();
		}
	}
	ResetPartCounts();
}

/* Refreshes chunks on the border of the map whose y is less than 'maxHeight'. */
static void RefreshBorderChunks(int maxHeight) {
	int cx, cy, cz;
	cc_bool onBorder;

	chunkPos = IVec3_MaxValue();
	if (!mapChunks || !World.Blocks) return;

	for (cz = 0; cz < World.ChunksZ; cz++) {
		for (cy = 0; cy < World.ChunksY; cy++) {
			for (cx = 0; cx < World.ChunksX; cx++) {
				onBorder = cx == 0 || cz == 0 || cx == (World.ChunksX - 1) || cz == (World.ChunksZ - 1);

				if (onBorder && (cy * CHUNK_SIZE) < maxHeight) {
					MapRenderer_RefreshChunk(cx, cy, cz);
				}
			}
		}
	}
}


/*########################################################################################################################*
*--------------------------------------------------Chunks updating/sorting------------------------------------------------*
*#########################################################################################################################*/
#define CHUNK_TARGET_TIME ((1.0f/30) + 0.01f)
static int chunksTarget = 12;
static Vec3 lastCamPos;
static float lastYaw, lastPitch;
/* Max distance from camera that chunks are rendered within */
/* This may differ from the view distance configured by the user */
static int renderDistSquared;
/* Max distance from camera that chunks are built within */
/* Chunks past this distance are automatically unloaded */
static int buildDistSquared;
#ifdef CC_BUILD_ESP32S3
#define ESP_DYNAMIC_MIN_VIEW      20
#define ESP_PRELOAD_RING_DISTANCE CHUNK_SIZE
#define ESP_UNLOAD_RING_DISTANCE  CHUNK_SIZE
/* Meshes one chunk-ring beyond the visible radius avoid turn/movement holes.
 * A second ring is the eviction hysteresis, while voxel data remains in the
 * fixed classic map independently of these mesh distances. */
static int unloadDistSquared;
static int dynamicRenderDistance;
static cc_uint8 dynamicCriticalFrames, dynamicSlowFrames, dynamicFastFrames;

static float ChunkAabbDistSquared(const struct ChunkInfo* chunk) {
	float dx = Math_AbsF(Camera.CurrentPos.x - chunk->centreX) - HALF_CHUNK_SIZE;
	float dy = Math_AbsF(Camera.CurrentPos.y - chunk->centreY) - HALF_CHUNK_SIZE;
	float dz = Math_AbsF(Camera.CurrentPos.z - chunk->centreZ) - HALF_CHUNK_SIZE;
	if (dx < 0.0f) dx = 0.0f;
	if (dy < 0.0f) dy = 0.0f;
	if (dz < 0.0f) dz = 0.0f;
	return dx * dx + dy * dy + dz * dz;
}

static float ChunkHorizontalCentreDistSquared(const struct ChunkInfo* chunk) {
	float dx = Camera.CurrentPos.x - chunk->centreX;
	float dz = Camera.CurrentPos.z - chunk->centreZ;
	return dx * dx + dz * dz;
}
#endif

static int AdjustDist(int dist) {
	if (dist < CHUNK_SIZE) dist = CHUNK_SIZE;
	dist = Utils_AdjViewDist(dist);
	return (dist + 24) * (dist + 24);
}

int MapRenderer_GetEffectiveViewDistance(void) {
#ifdef CC_BUILD_ESP32S3
	return dynamicRenderDistance ? dynamicRenderDistance : Game_ViewDistance;
#else
	return Game_ViewDistance;
#endif
}

static void CalcViewDists(void) {
#ifdef CC_BUILD_ESP32S3
	int buildDist  = max(Game_UserViewDistance, CHUNK_SIZE) + ESP_PRELOAD_RING_DISTANCE;
	int unloadDist = buildDist + ESP_UNLOAD_RING_DISTANCE;
	/* Liquid fog temporarily lowers Game_ViewDistance (lava reaches 1). Keep
	 * that intentional cap while submerged, but do not let it become the
	 * adaptive controller's persistent state after leaving the liquid. */
	if (!dynamicRenderDistance) {
		dynamicRenderDistance = Game_ViewDistance;
	} else if (Game_ViewDistance <= ESP_DYNAMIC_MIN_VIEW) {
		dynamicRenderDistance = Game_ViewDistance;
	} else if (dynamicRenderDistance < ESP_DYNAMIC_MIN_VIEW) {
		dynamicRenderDistance = min(Game_ViewDistance, ESP_DYNAMIC_MIN_VIEW);
	} else if (dynamicRenderDistance > Game_ViewDistance) {
		dynamicRenderDistance = Game_ViewDistance;
	}
	renderDistSquared = dynamicRenderDistance * dynamicRenderDistance;
	buildDistSquared  = buildDist  * buildDist;
	unloadDistSquared = unloadDist * unloadDist;
#else
	buildDistSquared  = AdjustDist(Game_UserViewDistance);
	renderDistSquared = AdjustDist(Game_ViewDistance);
#endif
}

static cc_bool ChunkPastUnloadDistance(const struct ChunkInfo* chunk, int sortedDistSqr) {
#ifdef CC_BUILD_ESP32S3
	return ChunkHorizontalCentreDistSquared(chunk) >= unloadDistSquared;
#else
	(void)chunk;
	return sortedDistSqr >= buildDistSquared + 32 * 16;
#endif
}

static cc_bool ChunkWithinBuildDistance(const struct ChunkInfo* chunk, int sortedDistSqr) {
#ifdef CC_BUILD_ESP32S3
	return ChunkHorizontalCentreDistSquared(chunk) <= buildDistSquared;
#else
	(void)chunk;
	return sortedDistSqr <= buildDistSquared;
#endif
}

static cc_bool ChunkWithinRenderDistance(const struct ChunkInfo* chunk, int sortedDistSqr) {
#ifdef CC_BUILD_ESP32S3
	return ChunkAabbDistSquared(chunk) <= renderDistSquared;
#else
	(void)chunk;
	return sortedDistSqr <= renderDistSquared;
#endif
}

static int UpdateChunksAndVisibility(int* chunkUpdates, cc_bool* pendingBuilds) {
	struct ChunkInfo* chunk;
	int i, j = 0, distSqr, res;

	for (i = 0; i < chunksCount; i++) 
	{
		chunk = sortedChunks[i];
		if (chunk->empty) continue;
		distSqr = distances[i];

		/* Auto unload chunks far away chunks */
		if (!chunk->noData && ChunkPastUnloadDistance(chunk, distSqr)) {
			DeleteChunk(chunk); continue;
		}

		if (chunk->dirty && ChunkWithinBuildDistance(chunk, distSqr)) {
			if (*chunkUpdates < chunksTarget) {
				DeleteChunk(chunk);
				BuildChunk(chunk, chunkUpdates);
			}
			if (chunk->dirty && !chunk->buildFailed) *pendingBuilds = true;
		}

		if (!ChunkWithinRenderDistance(chunk, distSqr)) {
			chunk->visible = false;
		} else {
			res = Frustum_TestAABB(chunk->centreX, chunk->centreY, chunk->centreZ, 8, 8, 8);
			chunk->visible  = res != FRUSTUM_OUTSIDE;
			chunk->skipClip = Gfx_CanSphereSkipClipping(chunk->centreX, chunk->centreY, chunk->centreZ, 14);
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
			chunk->softClipPlanes = Gfx_GetAABBClipPlanes(
				chunk->centreX, chunk->centreY, chunk->centreZ, 8, 8, 8);
#endif
		}

		if (chunk->visible && !chunk->empty) { renderChunks[j] = chunk; j++; }
	}
	return j;
}

static int UpdateChunksStill(int* chunkUpdates, cc_bool* pendingBuilds) {
	struct ChunkInfo* chunk;
	int i, j = 0, distSqr, res;

	for (i = 0; i < chunksCount; i++) 
	{
		chunk = sortedChunks[i];
		if (chunk->empty) continue;
		distSqr = distances[i];

		/* Auto unload chunks far away chunks */
		if (!chunk->noData && ChunkPastUnloadDistance(chunk, distSqr)) {
			DeleteChunk(chunk); continue;
		}

		if (chunk->dirty && ChunkWithinBuildDistance(chunk, distSqr) && !chunk->buildFailed) {
			if (*chunkUpdates < chunksTarget) {
				DeleteChunk(chunk);
				BuildChunk(chunk, chunkUpdates);
			}
			if (chunk->dirty && !chunk->buildFailed) *pendingBuilds = true;

			/* only need to update the visibility of chunks in range. */
			if (!ChunkWithinRenderDistance(chunk, distSqr)) {
				chunk->visible  = false;
			} else {
				res = Frustum_TestAABB(chunk->centreX, chunk->centreY, chunk->centreZ, 8, 8, 8);
				chunk->visible  = res != FRUSTUM_OUTSIDE;
				chunk->skipClip = Gfx_CanSphereSkipClipping(chunk->centreX, chunk->centreY, chunk->centreZ, 14);
#if defined CC_BUILD_ESP32S3 && defined CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
				chunk->softClipPlanes = Gfx_GetAABBClipPlanes(
					chunk->centreX, chunk->centreY, chunk->centreZ, 8, 8, 8);
#endif
			}

			if (chunk->visible && !chunk->empty) { renderChunks[j] = chunk; j++; }
		} else if (chunk->visible) {
			renderChunks[j] = chunk; j++;
		}
	}
	return j;
}

#ifdef CC_BUILD_ESP32S3
static void UpdateMapPerfState(void) {
	cc_uint32 resident = 0, pending = 0, failed = 0, meshVertices = 0;
	int i;
	for (i = 0; i < chunksCount; i++) {
		const struct ChunkInfo* chunk = &mapChunks[i];
		if (!chunk->noData) resident++;
		if (chunk->dirty && ChunkWithinBuildDistance(chunk, 0)) pending++;
		if (chunk->buildFailed) failed++;
#ifdef CC_BUILD_SOFTFP_QUAD_FRUSTUM_CULL
		meshVertices += chunk->meshVertices;
#endif
	}
	holo_perf_set_map_state((cc_uint32)Game_ViewDistance,
		(cc_uint32)dynamicRenderDistance,
		(cc_uint32)renderChunksCount, resident, pending, failed, meshVertices);
}

static void UpdateDynamicRenderDistance(float delta, int chunkUpdates) {
	int oldDistance = dynamicRenderDistance;
	/* The configured distance is the ceiling. Adaptation may reduce it as low
	 * as 20 blocks, but never below either that floor or a smaller user value. */
	int minDistance = min(Game_ViewDistance, ESP_DYNAMIC_MIN_VIEW);
	int maxDistance = Game_ViewDistance;
	int logValues[2];

	if (Game_ViewDistance <= ESP_DYNAMIC_MIN_VIEW) {
		dynamicRenderDistance = Game_ViewDistance;
		renderDistSquared = dynamicRenderDistance * dynamicRenderDistance;
		dynamicCriticalFrames = dynamicSlowFrames = dynamicFastFrames = 0;
		return;
	}

	/* Chunk build spikes are transient and already rate limited. Count only
	 * completed render-heavy frames, while ignoring pauses and long stalls. */
	if (chunkUpdates || chunksNeedScan || delta <= 0.0f || delta > 0.20f) return;

	if (delta > (1.0f / 9.0f) && dynamicRenderDistance > minDistance) {
		/* Critical frames also count towards the gentler under-12 FPS path,
		 * unless the two-frame critical path fires first. */
		dynamicFastFrames = 0;
		dynamicSlowFrames++;
		if (++dynamicCriticalFrames >= 2) {
			dynamicRenderDistance = max(minDistance, dynamicRenderDistance - 2);
			dynamicCriticalFrames = dynamicSlowFrames = 0;
		} else if (dynamicSlowFrames >= 8) {
			dynamicRenderDistance = max(minDistance, dynamicRenderDistance - 1);
			dynamicCriticalFrames = dynamicSlowFrames = 0;
		}
	} else if (delta > (1.0f / 12.0f) && dynamicRenderDistance > minDistance) {
		dynamicCriticalFrames = dynamicFastFrames = 0;
		if (++dynamicSlowFrames >= 8) {
			dynamicRenderDistance = max(minDistance, dynamicRenderDistance - 1);
			dynamicSlowFrames = 0;
		}
	} else if (delta < (1.0f / 16.0f) && dynamicRenderDistance < maxDistance) {
		dynamicCriticalFrames = dynamicSlowFrames = 0;
		if (++dynamicFastFrames >= 20) {
			dynamicRenderDistance++;
			dynamicFastFrames = 0;
		}
	} else {
		dynamicCriticalFrames = dynamicSlowFrames = dynamicFastFrames = 0;
	}
	if (dynamicRenderDistance == oldDistance) return;

	renderDistSquared = dynamicRenderDistance * dynamicRenderDistance;
	/* Force the full visibility path next frame; a stationary-build scan only
	 * updates dirty chunks and cannot apply a new radius to every cached mesh. */
	lastCamPos = Vec3_BigPos();
	chunksNeedScan = true;
	logValues[0] = Game_ViewDistance;
	logValues[1] = dynamicRenderDistance;
	Platform_Log2("[cc.view] max=%i effective=%i", &logValues[0], &logValues[1]);
}
#endif

static void UpdateChunks(float delta) {
	struct LocalPlayer* p;
	cc_bool samePos;
	cc_bool pendingBuilds = false;
	int chunkUpdates = 0;

	/* Build more chunks if 30 FPS or over, otherwise slowdown */
	chunksTarget += delta < CHUNK_TARGET_TIME ? 1 : -1; 
	Math_Clamp(chunksTarget, 4, maxChunkUpdates);
	p = Entities.CurPlayer;
	samePos = Vec3_Equals(&Camera.CurrentPos, &lastCamPos)
		&& p->Base.Pitch == lastPitch && p->Base.Yaw == lastYaw;
	if (samePos && !chunksNeedScan) {
#ifdef CC_BUILD_ESP32S3
		/* Keep sampling after input stops. A costly view reached by rotating once
		 * must still be able to shrink its effective radius while held still. */
		UpdateDynamicRenderDistance(delta, 0);
#endif
		return;
	}

	renderChunksCount = samePos ?
		UpdateChunksStill(&chunkUpdates, &pendingBuilds) :
		UpdateChunksAndVisibility(&chunkUpdates, &pendingBuilds);
	chunksNeedScan = pendingBuilds;
#ifdef CC_BUILD_ESP32S3
	UpdateMapPerfState();
#endif

	lastCamPos = Camera.CurrentPos;
	lastPitch  = p->Base.Pitch;
	lastYaw    = p->Base.Yaw;

	if (!samePos || chunkUpdates) ResetPartFlags();
#ifdef CC_BUILD_ESP32S3
	UpdateDynamicRenderDistance(delta, chunkUpdates);
#endif
}

static void SortMapChunks(int left, int right) {
	struct ChunkInfo** values = sortedChunks; struct ChunkInfo* value;
	cc_uint32* keys = distances; cc_uint32 key;

	while (left < right) {
		int i = left, j = right;
		cc_uint32 pivot = keys[(i + j) >> 1];

		/* partition the list */
		while (i <= j) {
			while (pivot > keys[i]) i++;
			while (pivot < keys[j]) j--;
			QuickSort_Swap_KV_Maybe();
		}
		/* recurse into the smaller subset */
		QuickSort_Recurse(SortMapChunks)
	}
}

static void UpdateSortOrder(void) {
	struct ChunkInfo* info;
	IVec3 pos;
	int i, dx, dy, dz;

	/* pos is centre coordinate of chunk camera is in */
	IVec3_Floor(&pos, &Camera.CurrentPos);
	pos.x = (pos.x & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.y = (pos.y & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.z = (pos.z & ~CHUNK_MASK) + HALF_CHUNK_SIZE;

	/* If in same chunk, don't need to recalculate sort order */
	if (pos.x == chunkPos.x && pos.y == chunkPos.y && pos.z == chunkPos.z) return;
	chunkPos = pos;
	if (!chunksCount) return;

	for (i = 0; i < chunksCount; i++) {
		info = sortedChunks[i];
		/* Calculate distance to chunk centre */
		dx = info->centreX - pos.x; dy = info->centreY - pos.y; dz = info->centreZ - pos.z;
		distances[i] = dx * dx + dy * dy + dz * dz;

		/* Consider these 3 chunks: */
		/* |       X-1      |        X        |       X+1      | */
		/* |################|########@########|################| */
		/* Assume the player is standing at @, then DrawXMin/XMax is calculated as this */
		/*    X-1: DrawXMin = false, DrawXMax = true  */
		/*    X  : DrawXMin = true,  DrawXMax = true  */
		/*    X+1: DrawXMin = true,  DrawXMax = false */

		info->drawXMin = dx >= 0; info->drawXMax = dx <= 0;
		info->drawZMin = dz >= 0; info->drawZMax = dz <= 0;
		info->drawYMin = dy >= 0; info->drawYMax = dy <= 0;
	}

	SortMapChunks(0, chunksCount - 1);
	ResetPartFlags();
	/*SimpleOcclusionCulling();*/
}

void MapRenderer_Update(float delta) {
	if (!mapChunks) return;
	UpdateSortOrder();
	UpdateChunks(delta);
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
void MapRenderer_RefreshChunk(int cx, int cy, int cz) {
	struct ChunkInfo* chunk;
	if (cx < 0 || cy < 0 || cz < 0 || cx >= World.ChunksX || cy >= World.ChunksY || cz >= World.ChunksZ) return;

	chunk = &mapChunks[World_ChunkPack(cx, cy, cz)];
	ChunkInfo_Refresh(chunk);
}

void MapRenderer_OnBlockChanged(int x, int y, int z, BlockID block) {
	int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT, cz = z >> CHUNK_SHIFT;
	struct ChunkInfo* chunk;

	chunk = &mapChunks[World_ChunkPack(cx, cy, cz)];
	chunk->allAir &= Blocks.Draw[block] == DRAW_GAS;
	/* TODO: Don't lookup twice, refresh directly using chunk pointer */
	ChunkInfo_Refresh(chunk);
}

static void OnEnvVariableChanged(void* obj, int envVar) {
	if (envVar == ENV_VAR_SUN_COLOR || envVar == ENV_VAR_SHADOW_COLOR) {
		RefreshChunks();
	} else if (envVar == ENV_VAR_EDGE_HEIGHT || envVar == ENV_VAR_SIDES_OFFSET) {
		int oldClip        = Builder_EdgeLevel;
		Builder_SidesLevel = max(0, Env_SidesHeight);
		Builder_EdgeLevel  = max(0, Env.EdgeHeight);

		/* Only need to refresh chunks on map borders up to highest edge level.*/
		RefreshBorderChunks(max(oldClip, Builder_EdgeLevel));
	}
}

static void OnTerrainAtlasChanged(void* obj) {
	static int tilesPerAtlas;
	/* e.g. If old atlas was 256x256 and new is 256x256, don't need to refresh */
	if (MapRenderer_1DUsedCount && tilesPerAtlas != Atlas1D.TilesPerAtlas) {
		MapRenderer_Refresh();
	}

	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	tilesPerAtlas = Atlas1D.TilesPerAtlas;
	ResetPartFlags();
}

static void OnBlockDefinitionChanged(void* obj) {
	MapRenderer_Refresh();
	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	ResetPartFlags();
}

static void OnVisibilityChanged(void* obj) {
	lastCamPos = Vec3_BigPos();
	CalcViewDists();
}
static void DeleteChunks_(void* obj) { DeleteChunks(); }
static void Refresh_(void* obj)      { MapRenderer_Refresh(); }

static void OnNewMap(void) {
	Game.ChunkUpdates = 0;
	DeleteChunks();
	ResetPartCounts();

	chunkPos = IVec3_MaxValue();
	FreeChunks();
	FreeParts();
}

static void OnNewMapLoaded(void) {
	chunksCount = World.ChunksCount;
	/* TODO: Only perform reallocation when map volume has changed */
	/*if (chunksCount != World.ChunksCount) { */
		/* chunksCount = World.ChunksCount; */
		FreeChunks();
		FreeParts();
		AllocateChunks();
		AllocateParts();
	/*}*/

	InitChunks();
	lastCamPos = Vec3_BigPos();
}

static void OnInit(void) {
	Event_Register_(&TextureEvents.AtlasChanged,  NULL, OnTerrainAtlasChanged);
	Event_Register_(&WorldEvents.EnvVarChanged,   NULL, OnEnvVariableChanged);
	Event_Register_(&BlockEvents.BlockDefChanged, NULL, OnBlockDefinitionChanged);

	Event_Register_(&GfxEvents.ViewDistanceChanged, NULL, OnVisibilityChanged);
	Event_Register_(&GfxEvents.ProjectionChanged,   NULL, OnVisibilityChanged);
	Event_Register_(&GfxEvents.ContextLost,         NULL, DeleteChunks_);
	Event_Register_(&GfxEvents.ContextRecreated,    NULL, Refresh_);

	/* This = 87 fixes map being invisible when no textures */
	MapRenderer_1DUsedCount = 87; /* Atlas1D_UsedAtlasesCount(); */
	chunkPos   = IVec3_MaxValue();
	maxChunkUpdates = Options_GetInt(OPT_MAX_CHUNK_UPDATES, 4, 1024, 30);
	CalcViewDists();
#ifdef CC_BUILD_ESP32S3
	Platform_LogConst("[cc.gfx] translucent=single-pass RGB565-smooth50 pair-sample depth-write split-rows");
#endif
}

struct IGameComponent MapRenderer_Component = {
	OnInit, /* Init */
	OnNewMap, /* Free */
	OnNewMap, /* Reset */
	OnNewMap, /* OnNewMap */
	OnNewMapLoaded /* OnNewMapLoaded */
};
