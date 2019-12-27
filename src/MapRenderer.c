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

int MapRenderer_ChunksX, MapRenderer_ChunksY, MapRenderer_ChunksZ;
int MapRenderer_1DUsedCount, MapRenderer_ChunksCount;
int MapRenderer_MaxUpdates;
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

/* Buffer for all chunk parts. There are (MapRenderer_ChunksCount * Atlas1D_Count) * 2 parts in the buffer,
 with parts for 'normal' buffer being in lower half. */
static struct ChunkPartInfo* partsBuffer_Raw;

struct ChunkInfo* MapRenderer_GetChunk(int cx, int cy, int cz) {
	return &mapChunks[MapRenderer_Pack(cx, cy, cz)];
}

void ChunkInfo_Reset(struct ChunkInfo* chunk, int x, int y, int z) {
	chunk->CentreX = x + HALF_CHUNK_SIZE; chunk->CentreY = y + HALF_CHUNK_SIZE; 
	chunk->CentreZ = z + HALF_CHUNK_SIZE;
#ifndef CC_BUILD_GL11
	chunk->Vb = 0;
#endif

	chunk->Visible = true;        chunk->Empty = false;
	chunk->PendingDelete = false; chunk->AllAir = false;
	chunk->DrawXMin = false; chunk->DrawXMax = false; chunk->DrawZMin = false;
	chunk->DrawZMax = false; chunk->DrawYMin = false; chunk->DrawYMax = false;

	chunk->NormalParts      = NULL;
	chunk->TranslucentParts = NULL;
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
static void MapRenderer_CheckWeather(double delta) {
	IVec3 pos;
	BlockID block;
	cc_bool outside;
	IVec3_Floor(&pos, &Camera.CurrentPos);

	block   = World_SafeGetBlock(pos.X, pos.Y, pos.Z);
	outside = pos.Y < 0 || !World_ContainsXZ(pos.X, pos.Z);
	inTranslucent = Blocks.Draw[block] == DRAW_TRANSLUCENT || (pos.Y < Env.EdgeHeight && outside);

	/* If we are under water, render weather before to blend properly */
	if (!inTranslucent || Env.Weather == WEATHER_SUNNY) return;
	Gfx_SetAlphaBlending(true);
	EnvRenderer_RenderWeather(delta);
	Gfx_SetAlphaBlending(false);
}

#ifdef CC_BUILD_GL11
#define MapRenderer_DrawFace(face, ign)    Gfx_DrawIndexedVb_TrisT2fC4b(part.Vbs[face], 0);
#define MapRenderer_DrawFaces(f1, f2, ign) MapRenderer_DrawFace(f1, ign); MapRenderer_DrawFace(f2, ign);
#else
#define MapRenderer_DrawFace(face, offset)    Gfx_DrawIndexedVb_TrisT2fC4b(part.Counts[face], offset);
#define MapRenderer_DrawFaces(f1, f2, offset) Gfx_DrawIndexedVb_TrisT2fC4b(part.Counts[f1] + part.Counts[f2], offset);
#endif

#define MapRenderer_DrawNormalFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	Gfx_SetFaceCulling(true); \
	MapRenderer_DrawFaces(minFace, maxFace, offset); \
	Gfx_SetFaceCulling(false); \
	Game_Vertices += (part.Counts[minFace] + part.Counts[maxFace]); \
} else if (drawMin) { \
	MapRenderer_DrawFace(minFace, offset); \
	Game_Vertices += part.Counts[minFace]; \
} else if (drawMax) { \
	MapRenderer_DrawFace(maxFace, offset + part.Counts[minFace]); \
	Game_Vertices += part.Counts[maxFace]; \
}

static void MapRenderer_RenderNormalBatch(int batch) {
	int batchOffset = MapRenderer_ChunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset, count;

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->NormalParts) continue;

		part = info->NormalParts[batchOffset];
		if (part.Offset < 0) continue;
		hasNormParts[batch] = true;

#ifndef CC_BUILD_GL11
		Gfx_BindVb(info->Vb);
#endif

		offset  = part.Offset + part.SpriteCount;
		drawMin = info->DrawXMin && part.Counts[FACE_XMIN];
		drawMax = info->DrawXMax && part.Counts[FACE_XMAX];
		MapRenderer_DrawNormalFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.Counts[FACE_XMIN] + part.Counts[FACE_XMAX];
		drawMin = info->DrawZMin && part.Counts[FACE_ZMIN];
		drawMax = info->DrawZMax && part.Counts[FACE_ZMAX];
		MapRenderer_DrawNormalFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.Counts[FACE_ZMIN] + part.Counts[FACE_ZMAX];
		drawMin = info->DrawYMin && part.Counts[FACE_YMIN];
		drawMax = info->DrawYMax && part.Counts[FACE_YMAX];
		MapRenderer_DrawNormalFaces(FACE_YMIN, FACE_YMAX);

		if (!part.SpriteCount) continue;
		offset = part.Offset;
		count  = part.SpriteCount >> 2; /* 4 per sprite */

		Gfx_SetFaceCulling(true);
		/* TODO: fix to not render them all */
#ifdef CC_BUILD_GL11
		Gfx_DrawIndexedVb_TrisT2fC4b(part.Vbs[FACE_COUNT], 0);
		Game_Vertices += count * 4;
		Gfx_SetFaceCulling(false);
		continue;
#endif
		if (info->DrawXMax || info->DrawZMin) {
			Gfx_DrawIndexedVb_TrisT2fC4b(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->DrawXMin || info->DrawZMax) {
			Gfx_DrawIndexedVb_TrisT2fC4b(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->DrawXMin || info->DrawZMin) {
			Gfx_DrawIndexedVb_TrisT2fC4b(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->DrawXMax || info->DrawZMax) {
			Gfx_DrawIndexedVb_TrisT2fC4b(count, offset); Game_Vertices += count;
		}
		Gfx_SetFaceCulling(false);
	}
}

void MapRenderer_RenderNormal(double delta) {
	int batch;
	if (!mapChunks) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	Gfx_SetTexturing(true);
	Gfx_SetAlphaTest(true);
	
	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		if (normPartsCount[batch] <= 0) continue;
		if (hasNormParts[batch] || checkNormParts[batch]) {
			Gfx_BindTexture(Atlas1D.TexIds[batch]);
			MapRenderer_RenderNormalBatch(batch);
			checkNormParts[batch] = false;
		}
	}
	Gfx_DisableMipmaps();

	MapRenderer_CheckWeather(delta);
	Gfx_SetAlphaTest(false);
	Gfx_SetTexturing(false);
#if DEBUG_OCCLUSION
	DebugPickedPos();
#endif
}

#define MapRenderer_DrawTranslucentFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	MapRenderer_DrawFaces(minFace, maxFace, offset); \
	Game_Vertices += (part.Counts[minFace] + part.Counts[maxFace]); \
} else if (drawMin) { \
	MapRenderer_DrawFace(minFace, offset); \
	Game_Vertices += part.Counts[minFace]; \
} else if (drawMax) { \
	MapRenderer_DrawFace(maxFace, offset + part.Counts[minFace]); \
	Game_Vertices += part.Counts[maxFace]; \
}

static void MapRenderer_RenderTranslucentBatch(int batch) {
	int batchOffset = MapRenderer_ChunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset;

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->TranslucentParts) continue;

		part = info->TranslucentParts[batchOffset];
		if (part.Offset < 0) continue;
		hasTranParts[batch] = true;

#ifndef CC_BUILD_GL11
		Gfx_BindVb(info->Vb);
#endif

		offset  = part.Offset;
		drawMin = (inTranslucent || info->DrawXMin) && part.Counts[FACE_XMIN];
		drawMax = (inTranslucent || info->DrawXMax) && part.Counts[FACE_XMAX];
		MapRenderer_DrawTranslucentFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.Counts[FACE_XMIN] + part.Counts[FACE_XMAX];
		drawMin = (inTranslucent || info->DrawZMin) && part.Counts[FACE_ZMIN];
		drawMax = (inTranslucent || info->DrawZMax) && part.Counts[FACE_ZMAX];
		MapRenderer_DrawTranslucentFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.Counts[FACE_ZMIN] + part.Counts[FACE_ZMAX];
		drawMin = (inTranslucent || info->DrawYMin) && part.Counts[FACE_YMIN];
		drawMax = (inTranslucent || info->DrawYMax) && part.Counts[FACE_YMAX];
		MapRenderer_DrawTranslucentFaces(FACE_YMIN, FACE_YMAX);
	}
}

void MapRenderer_RenderTranslucent(double delta) {
	int vertices, batch;
	if (!mapChunks) return;

	/* First fill depth buffer */
	vertices = Game_Vertices;
	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	Gfx_SetTexturing(false);
	Gfx_SetAlphaBlending(false);
	Gfx_SetColWriteMask(false, false, false, false);

	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		if (tranPartsCount[batch] <= 0) continue;
		if (hasTranParts[batch] || checkTranParts[batch]) {
			MapRenderer_RenderTranslucentBatch(batch);
			checkTranParts[batch] = false;
		}
	}
	Game_Vertices = vertices;

	/* Then actually draw the transluscent blocks */
	Gfx_SetAlphaBlending(true);
	Gfx_SetTexturing(true);
	Gfx_SetColWriteMask(true, true, true, true);
	Gfx_SetDepthWrite(false); /* already calculated depth values in depth pass */

	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		if (tranPartsCount[batch] <= 0) continue;
		if (!hasTranParts[batch]) continue;
		Gfx_BindTexture(Atlas1D.TexIds[batch]);
		MapRenderer_RenderTranslucentBatch(batch);
	}
	Gfx_DisableMipmaps();

	Gfx_SetDepthWrite(true);
	/* If we weren't under water, render weather after to blend properly */
	if (!inTranslucent && Env.Weather != WEATHER_SUNNY) {
		Gfx_SetAlphaTest(true);
		EnvRenderer_RenderWeather(delta);
		Gfx_SetAlphaTest(false);
	}
	Gfx_SetAlphaBlending(false);
	Gfx_SetTexturing(false);
}


/*########################################################################################################################*
*----------------------------------------------------Chunks mangagement---------------------------------------------------*
*#########################################################################################################################*/
static void MapRenderer_FreeParts(void) {
	Mem_Free(partsBuffer_Raw);
	partsBuffer_Raw              = NULL;
	MapRenderer_PartsNormal      = NULL;
	MapRenderer_PartsTranslucent = NULL;
}

static void MapRenderer_FreeChunks(void) {
	Mem_Free(mapChunks);
	Mem_Free(sortedChunks);
	Mem_Free(renderChunks);
	Mem_Free(distances);

	mapChunks    = NULL;
	sortedChunks = NULL;
	renderChunks = NULL;
	distances    = NULL;
}

static void MapRenderer_AllocateParts(void) {
	cc_uint32 count  = MapRenderer_ChunksCount * MapRenderer_1DUsedCount;
	partsBuffer_Raw = (struct ChunkPartInfo*)Mem_AllocCleared(count * 2, sizeof(struct ChunkPartInfo), "chunk parts");

	MapRenderer_PartsNormal      = partsBuffer_Raw;
	MapRenderer_PartsTranslucent = partsBuffer_Raw + count;
}

static void MapRenderer_AllocateChunks(void) {
	mapChunks    = (struct ChunkInfo*) Mem_Alloc(MapRenderer_ChunksCount, sizeof(struct ChunkInfo),  "chunk info");
	sortedChunks = (struct ChunkInfo**)Mem_Alloc(MapRenderer_ChunksCount, sizeof(struct ChunkInfo*), "sorted chunk info");
	renderChunks = (struct ChunkInfo**)Mem_Alloc(MapRenderer_ChunksCount, sizeof(struct ChunkInfo*), "render chunk info");
	distances    = (cc_uint32*)Mem_Alloc(MapRenderer_ChunksCount, 4, "chunk distances");
}

static void MapRenderer_ResetPartFlags(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		checkNormParts[i] = true;
		hasNormParts[i]   = false;
		checkTranParts[i] = true;
		hasTranParts[i]   = false;
	}
}

static void MapRenderer_ResetPartCounts(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		normPartsCount[i] = 0;
		tranPartsCount[i] = 0;
	}
}

static void MapRenderer_InitChunks(void) {
	int x, y, z, index = 0;
	for (z = 0; z < World.Length; z += CHUNK_SIZE) {
		for (y = 0; y < World.Height; y += CHUNK_SIZE) {
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				ChunkInfo_Reset(&mapChunks[index], x, y, z);
				sortedChunks[index] = &mapChunks[index];
				renderChunks[index] = &mapChunks[index];
				distances[index]    = 0;
				index++;
			}
		}
	}
}

static void MapRenderer_ResetChunks(void) {
	int x, y, z, index = 0;
	for (z = 0; z < World.Length; z += CHUNK_SIZE) {
		for (y = 0; y < World.Height; y += CHUNK_SIZE) {
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				ChunkInfo_Reset(&mapChunks[index], x, y, z);
				index++;
			}
		}
	}
}

static void MapRenderer_DeleteChunks(void) {
	int i;
	if (!mapChunks) return;

	for (i = 0; i < MapRenderer_ChunksCount; i++) {
		MapRenderer_DeleteChunk(&mapChunks[i]);
	}
	MapRenderer_ResetPartCounts();
}

void MapRenderer_Refresh(void) {
	int oldCount;
	chunkPos = IVec3_MaxValue();

	if (mapChunks && World.Blocks) {
		MapRenderer_DeleteChunks();
		MapRenderer_ResetChunks();

		oldCount = MapRenderer_1DUsedCount;
		MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
		/* Need to reallocate parts array in this case */
		if (MapRenderer_1DUsedCount != oldCount) {
			MapRenderer_FreeParts();
			MapRenderer_AllocateParts();
		}
	}
	MapRenderer_ResetPartCounts();
}

void MapRenderer_RefreshBorders(int maxHeight) {
	int cx, cy, cz;
	cc_bool onBorder;

	chunkPos = IVec3_MaxValue();
	if (!mapChunks || !World.Blocks) return;

	for (cz = 0; cz < MapRenderer_ChunksZ; cz++) {
		for (cy = 0; cy < MapRenderer_ChunksY; cy++) {
			for (cx = 0; cx < MapRenderer_ChunksX; cx++) {
				onBorder = cx == 0 || cz == 0 || cx == (MapRenderer_ChunksX - 1) || cz == (MapRenderer_ChunksZ - 1);

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
#define CHUNK_TARGET_TIME ((1.0/30) + 0.01)
static int chunksTarget = 12;
static Vec3 lastCamPos;
static float lastYaw, lastPitch;
/* Max distance from camera that chunks are rendered within */
/* This may differ from the view distance configured by the user */
static int renderDistSquared;
/* Max distance from camera that chunks are built within */
/* Chunks past this distance are automatically unloaded */
static int buildDistSquared;

static int MapRenderer_AdjustDist(int dist) {
	if (dist < CHUNK_SIZE) dist = CHUNK_SIZE;
	dist = Utils_AdjViewDist(dist);
	return (dist + 24) * (dist + 24);
}

static void MapRenderer_CalcViewDists(void) {
	buildDistSquared  = MapRenderer_AdjustDist(Game_UserViewDistance);
	renderDistSquared = MapRenderer_AdjustDist(Game_ViewDistance);
}

static int MapRenderer_UpdateChunksAndVisibility(int* chunkUpdates) {
	int renderDistSqr = renderDistSquared;
	int buildDistSqr  = buildDistSquared;

	struct ChunkInfo* info;
	int i, j = 0, distSqr;
	cc_bool noData;

	for (i = 0; i < MapRenderer_ChunksCount; i++) {
		info = sortedChunks[i];
		if (info->Empty) continue;

		distSqr = distances[i];
		noData  = !info->NormalParts && !info->TranslucentParts;
		
		/* Auto unload chunks far away chunks */
		if (!noData && distSqr >= buildDistSqr + 32 * 16) {
			MapRenderer_DeleteChunk(info); continue;
		}
		noData |= info->PendingDelete;

		if (noData && distSqr <= buildDistSqr && *chunkUpdates < chunksTarget) {
			MapRenderer_DeleteChunk(info);
			MapRenderer_BuildChunk(info, chunkUpdates);
		}

		info->Visible = distSqr <= renderDistSqr &&
			FrustumCulling_SphereInFrustum(info->CentreX, info->CentreY, info->CentreZ, 14); /* 14 ~ sqrt(3 * 8^2) */
		if (info->Visible && !info->Empty) { renderChunks[j] = info; j++; }
	}
	return j;
}

static int MapRenderer_UpdateChunksStill(int* chunkUpdates) {
	int renderDistSqr = renderDistSquared;
	int buildDistSqr  = buildDistSquared;

	struct ChunkInfo* info;
	int i, j = 0, distSqr;
	cc_bool noData;

	for (i = 0; i < MapRenderer_ChunksCount; i++) {
		info = sortedChunks[i];
		if (info->Empty) continue;

		distSqr = distances[i];
		noData  = !info->NormalParts && !info->TranslucentParts;

		/* Auto unload chunks far away chunks */
		if (!noData && distSqr >= buildDistSqr + 32 * 16) {
			MapRenderer_DeleteChunk(info); continue;
		}
		noData |= info->PendingDelete;

		if (noData && distSqr <= buildDistSqr && *chunkUpdates < chunksTarget) {
			MapRenderer_DeleteChunk(info);
			MapRenderer_BuildChunk(info, chunkUpdates);

			/* only need to update the visibility of chunks in range. */
			info->Visible = distSqr <= renderDistSqr &&
				FrustumCulling_SphereInFrustum(info->CentreX, info->CentreY, info->CentreZ, 14); /* 14 ~ sqrt(3 * 8^2) */
			if (info->Visible && !info->Empty) { renderChunks[j] = info; j++; }
		} else if (info->Visible) {
			renderChunks[j] = info; j++;
		}
	}
	return j;
}

static void MapRenderer_UpdateChunks(double delta) {
	struct LocalPlayer* p;
	cc_bool samePos;
	int chunkUpdates = 0;

	/* Build more chunks if 30 FPS or over, otherwise slowdown */
	chunksTarget += delta < CHUNK_TARGET_TIME ? 1 : -1; 
	Math_Clamp(chunksTarget, 4, MapRenderer_MaxUpdates);

	p = &LocalPlayer_Instance;
	samePos = Vec3_Equals(&Camera.CurrentPos, &lastCamPos)
		&& p->Base.Pitch == lastPitch && p->Base.Yaw == lastYaw;

	renderChunksCount = samePos ?
		MapRenderer_UpdateChunksStill(&chunkUpdates) :
		MapRenderer_UpdateChunksAndVisibility(&chunkUpdates);

	lastCamPos = Camera.CurrentPos;
	lastPitch  = p->Base.Pitch;
	lastYaw    = p->Base.Yaw;

	if (!samePos || chunkUpdates) {
		MapRenderer_ResetPartFlags();
	}
}

static void MapRenderer_QuickSort(int left, int right) {
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
		QuickSort_Recurse(MapRenderer_QuickSort)
	}
}

static void MapRenderer_UpdateSortOrder(void) {
	struct ChunkInfo* info;
	IVec3 pos;
	int i, dx, dy, dz;

	/* pos is centre coordinate of chunk camera is in */
	IVec3_Floor(&pos, &Camera.CurrentPos);
	pos.X = (pos.X & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.Y = (pos.Y & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.Z = (pos.Z & ~CHUNK_MASK) + HALF_CHUNK_SIZE;

	/* If in same chunk, don't need to recalculate sort order */
	if (pos.X == chunkPos.X && pos.Y == chunkPos.Y && pos.Z == chunkPos.Z) return;
	chunkPos = pos;
	if (!MapRenderer_ChunksCount) return;

	for (i = 0; i < MapRenderer_ChunksCount; i++) {
		info = sortedChunks[i];
		/* Calculate distance to chunk centre */
		dx = info->CentreX - pos.X; dy = info->CentreY - pos.Y; dz = info->CentreZ - pos.Z;
		distances[i] = dx * dx + dy * dy + dz * dz;

		/* Consider these 3 chunks: */
		/* |       X-1      |        X        |       X+1      | */
		/* |################|########@########|################| */
		/* Assume the player is standing at @, then DrawXMin/XMax is calculated as this */
		/*    X-1: DrawXMin = false, DrawXMax = true  */
		/*    X  : DrawXMin = true,  DrawXMax = true  */
		/*    X+1: DraWXMin = true,  DrawXMax = false */

		info->DrawXMin = dx >= 0; info->DrawXMax = dx <= 0;
		info->DrawZMin = dz >= 0; info->DrawZMax = dz <= 0;
		info->DrawYMin = dy >= 0; info->DrawYMax = dy <= 0;
	}

	MapRenderer_QuickSort(0, MapRenderer_ChunksCount - 1);
	MapRenderer_ResetPartFlags();
	/*SimpleOcclusionCulling();*/
}

void MapRenderer_Update(double delta) {
	if (!mapChunks) return;
	MapRenderer_UpdateSortOrder();
	MapRenderer_UpdateChunks(delta);
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
void MapRenderer_RefreshChunk(int cx, int cy, int cz) {
	struct ChunkInfo* info;
	if (cx < 0 || cy < 0 || cz < 0 || cx >= MapRenderer_ChunksX 
		|| cy >= MapRenderer_ChunksY || cz >= MapRenderer_ChunksZ) return;

	info = &mapChunks[MapRenderer_Pack(cx, cy, cz)];
	if (info->AllAir) return; /* do not recreate chunks completely air */
	info->Empty         = false;
	info->PendingDelete = true;
}

void MapRenderer_DeleteChunk(struct ChunkInfo* info) {
	struct ChunkPartInfo* ptr;
	int i;
#ifdef CC_BUILD_GL11
	int j;
#else
	Gfx_DeleteVb(&info->Vb);
#endif

	info->Empty = false; info->AllAir = false;
#ifdef OCCLUSION
	info.OcclusionFlags = 0;
	info.OccludedFlags = 0;
#endif

	if (info->NormalParts) {
		ptr = info->NormalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += MapRenderer_ChunksCount) {
			if (ptr->Offset < 0) continue; 
			normPartsCount[i]--;
#ifdef CC_BUILD_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->Vbs[j]);
#endif
		}
		info->NormalParts = NULL;
	}

	if (info->TranslucentParts) {
		ptr = info->TranslucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += MapRenderer_ChunksCount) {
			if (ptr->Offset < 0) continue;
			tranPartsCount[i]--;
#ifdef CC_BUILD_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->Vbs[j]);
#endif
		}
		info->TranslucentParts = NULL;
	}
}

void MapRenderer_BuildChunk(struct ChunkInfo* info, int* chunkUpdates) {
	struct ChunkPartInfo* ptr;
	int i;

	Game.ChunkUpdates++;
	(*chunkUpdates)++;
	info->PendingDelete = false;
	Builder_MakeChunk(info);

	if (!info->NormalParts && !info->TranslucentParts) {
		info->Empty = true; return;
	}
	
	if (info->NormalParts) {
		ptr = info->NormalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += MapRenderer_ChunksCount) {
			if (ptr->Offset >= 0) normPartsCount[i]++;
		}
	}

	if (info->TranslucentParts) {
		ptr = info->TranslucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += MapRenderer_ChunksCount) {
			if (ptr->Offset >= 0) tranPartsCount[i]++;
		}
	}
}

static void MapRenderer_EnvVariableChanged(void* obj, int envVar) {
	if (envVar == ENV_VAR_SUN_COL || envVar == ENV_VAR_SHADOW_COL) {
		MapRenderer_Refresh();
	} else if (envVar == ENV_VAR_EDGE_HEIGHT || envVar == ENV_VAR_SIDES_OFFSET) {
		int oldClip        = Builder_EdgeLevel;
		Builder_SidesLevel = max(0, Env_SidesHeight);
		Builder_EdgeLevel  = max(0, Env.EdgeHeight);

		/* Only need to refresh chunks on map borders up to highest edge level.*/
		MapRenderer_RefreshBorders(max(oldClip, Builder_EdgeLevel));
	}
}

static void MapRenderer_TerrainAtlasChanged(void* obj) {
	static int tilesPerAtlas;
	/* e.g. If old atlas was 256x256 and new is 256x256, don't need to refresh */
	if (MapRenderer_1DUsedCount && tilesPerAtlas != Atlas1D.TilesPerAtlas) {
		MapRenderer_Refresh();
	}

	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	tilesPerAtlas = Atlas1D.TilesPerAtlas;
	MapRenderer_ResetPartFlags();
}

static void MapRenderer_BlockDefinitionChanged(void* obj) {
	MapRenderer_Refresh();
	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	MapRenderer_ResetPartFlags();
}

static void MapRenderer_RecalcVisibility(void* obj) {
	lastCamPos = Vec3_BigPos();
	MapRenderer_CalcViewDists();
}
static void MapRenderer_DeleteChunks_(void* obj) { MapRenderer_DeleteChunks(); }
static void MapRenderer_Refresh_(void* obj)      { MapRenderer_Refresh(); }

static void MapRenderer_OnNewMap(void) {
	Game.ChunkUpdates = 0;
	MapRenderer_DeleteChunks();
	MapRenderer_ResetPartCounts();

	chunkPos = IVec3_MaxValue();
	MapRenderer_FreeChunks();
	MapRenderer_FreeParts();
}

static void MapRenderer_OnNewMapLoaded(void) {
	int count;
	MapRenderer_ChunksX = (World.Width  + CHUNK_MAX) >> CHUNK_SHIFT;
	MapRenderer_ChunksY = (World.Height + CHUNK_MAX) >> CHUNK_SHIFT;
	MapRenderer_ChunksZ = (World.Length + CHUNK_MAX) >> CHUNK_SHIFT;

	count = MapRenderer_ChunksX * MapRenderer_ChunksY * MapRenderer_ChunksZ;
	/* TODO: Only perform reallocation when map volume has changed */
	/*if (MapRenderer_ChunksCount != count) { */
		MapRenderer_ChunksCount = count;
		MapRenderer_FreeChunks();
		MapRenderer_FreeParts();
		MapRenderer_AllocateChunks();
		MapRenderer_AllocateParts();
	/*}*/

	MapRenderer_InitChunks();
	Builder_OnNewMapLoaded();
	lastCamPos = Vec3_BigPos();
}

static void MapRenderer_Init(void) {
	Event_RegisterVoid(&TextureEvents.AtlasChanged,  NULL, MapRenderer_TerrainAtlasChanged);
	Event_RegisterInt(&WorldEvents.EnvVarChanged,    NULL, MapRenderer_EnvVariableChanged);
	Event_RegisterVoid(&BlockEvents.BlockDefChanged, NULL, MapRenderer_BlockDefinitionChanged);

	Event_RegisterVoid(&GfxEvents.ViewDistanceChanged, NULL, MapRenderer_RecalcVisibility);
	Event_RegisterVoid(&GfxEvents.ProjectionChanged,   NULL, MapRenderer_RecalcVisibility);
	Event_RegisterVoid(&GfxEvents.ContextLost,         NULL, MapRenderer_DeleteChunks_);
	Event_RegisterVoid(&GfxEvents.ContextRecreated,    NULL, MapRenderer_Refresh_);

	/* This = 87 fixes map being invisible when no textures */
	MapRenderer_1DUsedCount = 87; /* Atlas1D_UsedAtlasesCount(); */
	chunkPos   = IVec3_MaxValue();
	MapRenderer_MaxUpdates = Options_GetInt(OPT_MAX_CHUNK_UPDATES, 4, 1024, 30);

	Builder_Init();
	Builder_ApplyActive();
	MapRenderer_CalcViewDists();
}

static void MapRenderer_Free(void) {
	Event_UnregisterVoid(&TextureEvents.AtlasChanged,  NULL, MapRenderer_TerrainAtlasChanged);
	Event_UnregisterInt(&WorldEvents.EnvVarChanged,    NULL, MapRenderer_EnvVariableChanged);
	Event_UnregisterVoid(&BlockEvents.BlockDefChanged, NULL, MapRenderer_BlockDefinitionChanged);

	Event_UnregisterVoid(&GfxEvents.ViewDistanceChanged, NULL, MapRenderer_RecalcVisibility);
	Event_UnregisterVoid(&GfxEvents.ProjectionChanged,   NULL, MapRenderer_RecalcVisibility);
	Event_UnregisterVoid(&GfxEvents.ContextLost,         NULL, MapRenderer_DeleteChunks_);
	Event_UnregisterVoid(&GfxEvents.ContextRecreated,    NULL, MapRenderer_Refresh_);

	MapRenderer_OnNewMap();
}

struct IGameComponent MapRenderer_Component = {
	MapRenderer_Init, /* Init  */
	MapRenderer_Free, /* Free  */
	MapRenderer_OnNewMap, /* Reset */
	MapRenderer_OnNewMap, /* OnNewMap */
	MapRenderer_OnNewMapLoaded /* OnNewMapLoaded */
};
