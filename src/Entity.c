#include "Entity.h"
#include "ExtMath.h"
#include "World.h"
#include "Block.h"
#include "Event.h"
#include "Game.h"
#include "Camera.h"
#include "Platform.h"
#include "Funcs.h"
#include "Graphics.h"
#include "Lighting.h"
#include "Drawer2D.h"
#include "Particle.h"
#include "Http.h"
#include "Chat.h"
#include "Model.h"
#include "Input.h"
#include "Gui.h"
#include "Stream.h"
#include "Bitmap.h"
#include "Logger.h"

const char* const NameMode_Names[NAME_MODE_COUNT]   = { "None", "Hovered", "All", "AllHovered", "AllUnscaled" };
const char* const ShadowMode_Names[SHADOW_MODE_COUNT] = { "None", "SnapToBlock", "Circle", "CircleAll" };

/*########################################################################################################################*
*-----------------------------------------------------LocationUpdate------------------------------------------------------*
*#########################################################################################################################*/
float LocationUpdate_Clamp(float degrees) {
	while (degrees >= 360.0f) degrees -= 360.0f;
	while (degrees < 0.0f)    degrees += 360.0f;
	return degrees;
}

static struct LocationUpdate loc_empty;
void LocationUpdate_MakeOri(struct LocationUpdate* update, float yaw, float pitch) {
	*update = loc_empty;
	update->Flags = LOCATIONUPDATE_PITCH | LOCATIONUPDATE_YAW;
	update->Pitch = LocationUpdate_Clamp(pitch);
	update->Yaw   = LocationUpdate_Clamp(yaw);
}

void LocationUpdate_MakePos(struct LocationUpdate* update, Vec3 pos, cc_bool rel) {
	*update = loc_empty;
	update->Flags = LOCATIONUPDATE_POS;
	update->Pos   = pos;
	update->RelativePos = rel;
}

void LocationUpdate_MakePosAndOri(struct LocationUpdate* update, Vec3 pos, float yaw, float pitch, cc_bool rel) {
	*update = loc_empty;
	update->Flags = LOCATIONUPDATE_POS | LOCATIONUPDATE_PITCH | LOCATIONUPDATE_YAW;
	update->Pitch = LocationUpdate_Clamp(pitch);
	update->Yaw   = LocationUpdate_Clamp(yaw);
	update->Pos   = pos;
	update->RelativePos = rel;
}


/*########################################################################################################################*
*---------------------------------------------------------Entity----------------------------------------------------------*
*#########################################################################################################################*/
static PackedCol Entity_GetCol(struct Entity* e) {
	Vec3 eyePos = Entity_GetEyePosition(e);
	IVec3 pos; IVec3_Floor(&pos, &eyePos);
	return World_Contains(pos.X, pos.Y, pos.Z) ? Lighting_Col(pos.X, pos.Y, pos.Z) : Env.SunCol;
}

void Entity_Init(struct Entity* e) {
	static const String model = String_FromConst("humanoid");
	Vec3_Set(e->ModelScale, 1,1,1);
	e->uScale   = 1.0f;
	e->vScale   = 1.0f;
	e->StepSize = 0.5f;
	e->SkinNameRaw[0]    = '\0';
	e->DisplayNameRaw[0] = '\0';
	Entity_SetModel(e, &model);
}

Vec3 Entity_GetEyePosition(struct Entity* e) {
	Vec3 pos = e->Position; pos.Y += Entity_GetEyeHeight(e); return pos;
}

float Entity_GetEyeHeight(struct Entity* e) {
	return e->Model->GetEyeY(e) * e->ModelScale.Y;
}

void Entity_GetTransform(struct Entity* e, Vec3 pos, Vec3 scale, struct Matrix* m) {
	struct Matrix tmp;
	Matrix_Scale(m, scale.X, scale.Y, scale.Z);

	Matrix_RotateZ(&tmp, -e->RotZ * MATH_DEG2RAD);
	Matrix_MulBy(m, &tmp);
	Matrix_RotateX(&tmp, -e->RotX * MATH_DEG2RAD);
	Matrix_MulBy(m, &tmp);
	Matrix_RotateY(&tmp, -e->RotY * MATH_DEG2RAD);
	Matrix_MulBy(m, &tmp);
	Matrix_Translate(&tmp, pos.X, pos.Y, pos.Z);
	Matrix_MulBy(m, &tmp);
	/* return rotZ * rotX * rotY * scale * translate; */
}

void Entity_GetPickingBounds(struct Entity* e, struct AABB* bb) {
	AABB_Offset(bb, &e->ModelAABB, &e->Position);
}

void Entity_GetBounds(struct Entity* e, struct AABB* bb) {
	AABB_Make(bb, &e->Position, &e->Size);
}

static void Entity_ParseScale(struct Entity* e, const String* scale) {
	float value;
	if (!Convert_ParseFloat(scale, &value)) return;

	value = max(value, 0.001f);
	/* local player doesn't allow giant model scales */
	/* (can't climb stairs, extremely CPU intensive collisions) */
	if (e->ModelRestrictedScale) { value = min(value, e->Model->maxScale); }
	Vec3_Set(e->ModelScale, value,value,value);
}

static void Entity_SetBlockModel(struct Entity* e, const String* model) {
	static const String block = String_FromConst("block");
	int raw = Block_Parse(model);

	if (raw == -1) {
		/* use default humanoid model */
		e->Model      = Models.Human;
	} else {	
		e->ModelBlock = (BlockID)raw;
		e->Model      = Model_Get(&block);
	}
}

void Entity_SetModel(struct Entity* e, const String* model) {
	String name, scale, skin;
	Vec3_Set(e->ModelScale, 1,1,1);
	String_UNSAFE_Separate(model, '|', &name, &scale);

	/* 'giant' model kept for backwards compatibility */
	if (String_CaselessEqualsConst(&name, "giant")) {
		name = String_FromReadonly("humanoid");
		Vec3_Set(e->ModelScale, 2,2,2);
	}

	e->ModelBlock   = BLOCK_AIR;
	e->Model        = Model_Get(&name);
	e->MobTextureId = 0;
	if (!e->Model) Entity_SetBlockModel(e, &name);

	Entity_ParseScale(e, &scale);
	Entity_UpdateModelBounds(e);
	
	skin = String_FromRawArray(e->SkinNameRaw);
	if (Utils_IsUrlPrefix(&skin)) e->MobTextureId = e->TextureId;
}

void Entity_UpdateModelBounds(struct Entity* e) {
	struct Model* model = e->Model;
	model->GetCollisionSize(e);
	model->GetPickingBounds(e);

	Vec3_Mul3By(&e->Size,          &e->ModelScale);
	Vec3_Mul3By(&e->ModelAABB.Min, &e->ModelScale);
	Vec3_Mul3By(&e->ModelAABB.Max, &e->ModelScale);
}

cc_bool Entity_TouchesAny(struct AABB* bounds, Entity_TouchesCondition condition) {
	IVec3 bbMin, bbMax;
	BlockID block;
	struct AABB blockBB;
	Vec3 v;
	int x, y, z;

	IVec3_Floor(&bbMin, &bounds->Min);
	IVec3_Floor(&bbMax, &bounds->Max);

	bbMin.X = max(bbMin.X, 0); bbMax.X = min(bbMax.X, World.MaxX);
	bbMin.Y = max(bbMin.Y, 0); bbMax.Y = min(bbMax.Y, World.MaxY);
	bbMin.Z = max(bbMin.Z, 0); bbMax.Z = min(bbMax.Z, World.MaxZ);

	for (y = bbMin.Y; y <= bbMax.Y; y++) { v.Y = (float)y;
		for (z = bbMin.Z; z <= bbMax.Z; z++) { v.Z = (float)z;
			for (x = bbMin.X; x <= bbMax.X; x++) { v.X = (float)x;

				block = World_GetBlock(x, y, z);
				Vec3_Add(&blockBB.Min, &v, &Blocks.MinBB[block]);
				Vec3_Add(&blockBB.Max, &v, &Blocks.MaxBB[block]);

				if (!AABB_Intersects(&blockBB, bounds)) continue;
				if (condition(block)) return true;
			}
		}
	}
	return false;
}

static cc_bool IsRopeCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_CLIMB_ROPE; }
cc_bool Entity_TouchesAnyRope(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	bounds.Max.Y += 0.5f / 16.0f;
	return Entity_TouchesAny(&bounds, IsRopeCollide);
}

static const Vec3 entity_liqExpand = { 0.25f/16.0f, 0.0f/16.0f, 0.25f/16.0f };
static cc_bool IsLavaCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_LIQUID_LAVA; }
cc_bool Entity_TouchesAnyLava(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	AABB_Offset(&bounds, &bounds, &entity_liqExpand);
	return Entity_TouchesAny(&bounds, IsLavaCollide);
}

static cc_bool IsWaterCollide(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_LIQUID_WATER; }
cc_bool Entity_TouchesAnyWater(struct Entity* e) {
	struct AABB bounds; Entity_GetBounds(e, &bounds);
	AABB_Offset(&bounds, &bounds, &entity_liqExpand);
	return Entity_TouchesAny(&bounds, IsWaterCollide);
}



/*########################################################################################################################*
*-----------------------------------------------------Entity nametag------------------------------------------------------*
*#########################################################################################################################*/
#define NAME_IS_EMPTY -30000
#define NAME_OFFSET 3 /* offset of back layer of name above an entity */

static void Entity_MakeNameTexture(struct Entity* e) {
	String colorlessName; char colorlessBuffer[STRING_SIZE];
	BitmapCol shadowCol = BitmapCol_Make(80, 80, 80, 255);
	BitmapCol origWhiteCol;

	struct DrawTextArgs args;
	struct FontDesc font;
	cc_bool bitmapped;
	int width, height;
	String name;
	Bitmap bmp;

	/* Names are always drawn not using the system font */
	bitmapped = Drawer2D_BitmappedText;
	Drawer2D_BitmappedText = true;
	name = String_FromRawArray(e->DisplayNameRaw);

	Drawer2D_MakeFont(&font, 24, FONT_STYLE_NORMAL);
	DrawTextArgs_Make(&args, &name, &font, false);
	width = Drawer2D_TextWidth(&args);

	if (!width) {
		e->NameTex.ID = 0;
		e->NameTex.X  = NAME_IS_EMPTY;
	} else {
		String_InitArray(colorlessName, colorlessBuffer);
		width  += NAME_OFFSET; 
		height = Drawer2D_TextHeight(&args) + NAME_OFFSET;

		Bitmap_AllocateClearedPow2(&bmp, width, height);
		{
			origWhiteCol = Drawer2D_Cols['f'];

			Drawer2D_Cols['f'] = shadowCol;
			String_AppendColorless(&colorlessName, &name);
			args.text = colorlessName;
			Drawer2D_DrawText(&bmp, &args, NAME_OFFSET, NAME_OFFSET);

			Drawer2D_Cols['f'] = origWhiteCol;
			args.text = name;
			Drawer2D_DrawText(&bmp, &args, 0, 0);
		}
		Drawer2D_MakeTexture(&e->NameTex, &bmp, width, height);
		Mem_Free(bmp.Scan0);
	}
	Drawer2D_BitmappedText = bitmapped;
}

static void Entity_DrawName(struct Entity* e) {
	VertexP3fT2fC4b vertices[4];
	PackedCol col = PACKEDCOL_WHITE;

	struct Model* model;
	struct Matrix mat;
	Vec3 pos;
	float scale;
	Vec2 size;

	if (e->NameTex.X == NAME_IS_EMPTY) return;
	if (!e->NameTex.ID) Entity_MakeNameTexture(e);
	Gfx_BindTexture(e->NameTex.ID);

	model = e->Model;
	Vec3_TransformY(&pos, model->GetNameY(e), &e->Transform);

	scale  = model->nameScale * e->ModelScale.Y;
	scale  = scale > 1.0f ? (1.0f/70.0f) : (scale/70.0f);
	size.X = e->NameTex.Width * scale; size.Y = e->NameTex.Height * scale;

	if (Entities.NamesMode == NAME_MODE_ALL_UNSCALED && LocalPlayer_Instance.Hacks.CanSeeAllNames) {			
		Matrix_Mul(&mat, &Gfx.View, &Gfx.Projection); /* TODO: This mul is slow, avoid it */
		/* Get W component of transformed position */
		scale = pos.X * mat.Row0.W + pos.Y * mat.Row1.W + pos.Z * mat.Row2.W + mat.Row3.W;
		size.X *= scale * 0.2f; size.Y *= scale * 0.2f;
	}

	Particle_DoRender(&size, &pos, &e->NameTex.uv, col, vertices);
	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	Gfx_UpdateDynamicVb_IndexedTris(Gfx_texVb, vertices, 4);
}

/* Deletes the texture containing the entity's nametag */
CC_NOINLINE static void Entity_DeleteNameTex(struct Entity* e) {
	Gfx_DeleteTexture(&e->NameTex.ID);
	e->NameTex.X = 0; /* X is used as an 'empty name' flag */
}

void Entity_SetName(struct Entity* e, const String* name) {
	Entity_DeleteNameTex(e);
	String_CopyToRawArray(e->DisplayNameRaw, name);
	/* name texture redraw deferred until necessary */
}


/*########################################################################################################################*
*------------------------------------------------------Entity skins-------------------------------------------------------*
*#########################################################################################################################*/
static struct Entity* Entity_FirstOtherWithSameSkinAndFetchedSkin(struct Entity* except) {
	struct Entity* e;
	String skin, eSkin;
	int i;

	skin = String_FromRawArray(except->SkinNameRaw);
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i] || Entities.List[i] == except) continue;

		e     = Entities.List[i];
		eSkin = String_FromRawArray(e->SkinNameRaw);
		if (e->SkinFetchState && String_Equals(&skin, &eSkin)) return e;
	}
	return NULL;
}

/* Copies skin data from another entity */
static void Entity_CopySkin(struct Entity* dst, struct Entity* src) {
	String skin;
	dst->TextureId = src->TextureId;	
	dst->SkinType  = src->SkinType;
	dst->uScale    = src->uScale;
	dst->vScale    = src->vScale;

	/* Custom mob textures */
	dst->MobTextureId = 0;
	skin = String_FromRawArray(dst->SkinNameRaw);
	if (Utils_IsUrlPrefix(&skin)) dst->MobTextureId = dst->TextureId;
}

/* Resets skin data for the given entity */
static void Entity_ResetSkin(struct Entity* e) {
	e->uScale = 1.0f; e->vScale = 1.0f;
	e->MobTextureId = 0;
	e->TextureId    = 0;
	e->SkinType     = SKIN_64x32;
}

/* Copies or resets skin data for all entity with same skin */
static void Entity_SetSkinAll(struct Entity* source, cc_bool reset) {
	struct Entity* e;
	String skin, eSkin;
	int i;

	skin = String_FromRawArray(source->SkinNameRaw);
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;

		e     = Entities.List[i];
		eSkin = String_FromRawArray(e->SkinNameRaw);
		if (!String_Equals(&skin, &eSkin)) continue;

		if (reset) {
			Entity_ResetSkin(e);
		} else {
			Entity_CopySkin(e, source);
		}
		e->SkinFetchState = SKIN_FETCH_COMPLETED;
	}
}

/* Clears hat area from a skin bitmap if it's completely white or black,
   so skins edited with Microsoft Paint or similiar don't have a solid hat */
static void Entity_ClearHat(Bitmap* bmp, cc_uint8 skinType) {
	int sizeX  = (bmp->Width / 64) * 32;
	int yScale = skinType == SKIN_64x32 ? 32 : 64;
	int sizeY  = (bmp->Height / yScale) * 16;
	int x, y;

	/* determine if we actually need filtering */
	for (y = 0; y < sizeY; y++) {
		BitmapCol* row = Bitmap_GetRow(bmp, y) + sizeX;
		for (x = 0; x < sizeX; x++) {
			if (BitmapCol_A(row[x]) != 255) return;
		}
	}

	/* only perform filtering when the entire hat is opaque */
	for (y = 0; y < sizeY; y++) {
		BitmapCol* row = Bitmap_GetRow(bmp, y) + sizeX;
		for (x = 0; x < sizeX; x++) {
			BitmapCol c = row[x];
			if (c == BITMAPCOL_WHITE || c == BITMAPCOL_BLACK) row[x] = 0;
		}
	}
}

/* Ensures skin is a power of two size, resizing if needed. */
static void Entity_EnsurePow2(struct Entity* e, Bitmap* bmp) {
	cc_uint32 stride;
	int width, height;
	Bitmap scaled;
	int y;

	width  = Math_NextPowOf2(bmp->Width);
	height = Math_NextPowOf2(bmp->Height);
	if (width == bmp->Width && height == bmp->Height) return;

	Bitmap_Allocate(&scaled, width, height);
	e->uScale = (float)bmp->Width  / width;
	e->vScale = (float)bmp->Height / height;
	stride = bmp->Width * 4;

	for (y = 0; y < bmp->Height; y++) {
		BitmapCol* src = Bitmap_GetRow(bmp, y);
		BitmapCol* dst = Bitmap_GetRow(&scaled, y);
		Mem_Copy(dst, src, stride);
	}

	Mem_Free(bmp->Scan0);
	*bmp = scaled;
}

static void Entity_CheckSkin(struct Entity* e) {
	struct Entity* first;
	String url, skin;

	struct HttpRequest item;
	struct Stream mem;
	Bitmap bmp;
	cc_result res;

	/* Don't check skin if don't have to */
	if (!e->Model->usesSkin) return;
	if (e->SkinFetchState == SKIN_FETCH_COMPLETED) return;
	skin = String_FromRawArray(e->SkinNameRaw);

	if (!e->SkinFetchState) {
		first = Entity_FirstOtherWithSameSkinAndFetchedSkin(e);
		if (!first) {
			Http_AsyncGetSkin(&skin);
			e->SkinFetchState = SKIN_FETCH_DOWNLOADING;
		} else {
			Entity_CopySkin(e, first);
			e->SkinFetchState = SKIN_FETCH_COMPLETED;
			return;
		}
	}

	if (!Http_GetResult(&skin, &item)) return;
	if (!item.success) { Entity_SetSkinAll(e, true); return; }
	Stream_ReadonlyMemory(&mem, item.data, item.size);

	if ((res = Png_Decode(&bmp, &mem))) {
		url = String_FromRawArray(item.url);
		Logger_Warn2(res, "decoding", &url);
		Mem_Free(bmp.Scan0); return;
	}

	Gfx_DeleteTexture(&e->TextureId);
	Entity_SetSkinAll(e, true);
	Entity_EnsurePow2(e, &bmp);
	e->SkinType = Utils_CalcSkinType(&bmp);

	if (bmp.Width > Gfx.MaxTexWidth || bmp.Height > Gfx.MaxTexHeight) {
		Chat_Add1("&cSkin %s is too large", &skin);
	} else if (e->SkinType != SKIN_INVALID) {
		if (e->Model->usesHumanSkin) Entity_ClearHat(&bmp, e->SkinType);
		e->TextureId = Gfx_CreateTexture(&bmp, true, false);
		Entity_SetSkinAll(e, false);
	}
	Mem_Free(bmp.Scan0);
}

/* Returns true if no other entities are sharing this skin texture */
static cc_bool Entity_CanDeleteTexture(struct Entity* except) {
	int i;
	if (!except->TextureId) return false;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i] || Entities.List[i] == except)  continue;
		if (Entities.List[i]->TextureId == except->TextureId) return false;
	}
	return true;
}

CC_NOINLINE static void Entity_DeleteSkin(struct Entity* e) {
	if (Entity_CanDeleteTexture(e)) {
		Gfx_DeleteTexture(&e->TextureId);
	}

	Entity_ResetSkin(e);
	e->SkinFetchState = 0;
}

void Entity_SetSkin(struct Entity* e, const String* skin) {
	Entity_DeleteSkin(e);
	String_CopyToRawArray(e->SkinNameRaw, skin);
}


/*########################################################################################################################*
*--------------------------------------------------------Entities---------------------------------------------------------*
*#########################################################################################################################*/
struct _EntitiesData Entities;
static EntityID entities_closestId;

void Entities_Tick(struct ScheduledTask* task) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		Entities.List[i]->VTABLE->Tick(Entities.List[i], task->Interval);
	}
}

void Entities_RenderModels(double delta, float t) {
	int i;
	Gfx_SetTexturing(true);
	Gfx_SetAlphaTest(true);
	
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		Entities.List[i]->VTABLE->RenderModel(Entities.List[i], delta, t);
	}
	Gfx_SetTexturing(false);
	Gfx_SetAlphaTest(false);
}
	

void Entities_RenderNames(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	cc_bool hadFog;
	int i;

	if (Entities.NamesMode == NAME_MODE_NONE) return;
	entities_closestId = Entities_GetClosest(&p->Base);
	if (!p->Hacks.CanSeeAllNames || Entities.NamesMode != NAME_MODE_ALL) return;

	Gfx_SetTexturing(true);
	Gfx_SetAlphaTest(true);
	hadFog = Gfx_GetFog();
	if (hadFog) Gfx_SetFog(false);

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		if (i != entities_closestId || i == ENTITIES_SELF_ID) {
			Entities.List[i]->VTABLE->RenderName(Entities.List[i]);
		}
	}

	Gfx_SetTexturing(false);
	Gfx_SetAlphaTest(false);
	if (hadFog) Gfx_SetFog(true);
}

void Entities_RenderHoveredNames(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	cc_bool allNames, hadFog;
	int i;

	if (Entities.NamesMode == NAME_MODE_NONE) return;
	allNames = !(Entities.NamesMode == NAME_MODE_HOVERED || Entities.NamesMode == NAME_MODE_ALL) 
		&& p->Hacks.CanSeeAllNames;

	Gfx_SetTexturing(true);
	Gfx_SetAlphaTest(true);
	Gfx_SetDepthTest(false);
	hadFog = Gfx_GetFog();
	if (hadFog) Gfx_SetFog(false);

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		if ((i == entities_closestId || allNames) && i != ENTITIES_SELF_ID) {
			Entities.List[i]->VTABLE->RenderName(Entities.List[i]);
		}
	}

	Gfx_SetTexturing(false);
	Gfx_SetAlphaTest(false);
	Gfx_SetDepthTest(true);
	if (hadFog) Gfx_SetFog(true);
}

static void Entity_ContextLost(struct Entity* e) {
	Entity_DeleteNameTex(e);
}

static void Entities_ContextLost(void* obj) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		Entity_ContextLost(Entities.List[i]);
	}
	Gfx_DeleteTexture(&ShadowComponent_ShadowTex);
}

static void Entities_ContextRecreated(void* obj) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		/* name redraw is deferred until rendered */
	}
}

static void Entities_ChatFontChanged(void* obj) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		Entity_DeleteNameTex(Entities.List[i]);
		/* name redraw is deferred until rendered */
	}
}

void Entities_Remove(EntityID id) {
	Event_RaiseInt(&EntityEvents.Removed, id);
	Entities.List[id]->VTABLE->Despawn(Entities.List[id]);
	Entities.List[id] = NULL;
}

EntityID Entities_GetClosest(struct Entity* src) {
	Vec3 eyePos = Entity_GetEyePosition(src);
	Vec3 dir = Vec3_GetDirVector(src->Yaw * MATH_DEG2RAD, src->Pitch * MATH_DEG2RAD);
	float closestDist = MATH_POS_INF;
	EntityID targetId = ENTITIES_SELF_ID;

	float t0, t1;
	int i;

	for (i = 0; i < ENTITIES_SELF_ID; i++) { /* because we don't want to pick against local player */
		struct Entity* entity = Entities.List[i];
		if (!entity) continue;

		if (Intersection_RayIntersectsRotatedBox(eyePos, dir, entity, &t0, &t1) && t0 < closestDist) {
			closestDist = t0;
			targetId = (EntityID)i;
		}
	}
	return targetId;
}

void Entities_DrawShadows(void) {
	int i;
	if (Entities.ShadowsMode == SHADOW_MODE_NONE) return;
	ShadowComponent_BoundShadowTex = false;

	Gfx_SetAlphaArgBlend(true);
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaBlending(true);
	Gfx_SetTexturing(true);

	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	ShadowComponent_Draw(Entities.List[ENTITIES_SELF_ID]);

	if (Entities.ShadowsMode == SHADOW_MODE_CIRCLE_ALL) {	
		for (i = 0; i < ENTITIES_SELF_ID; i++) {
			if (!Entities.List[i]) continue;
			ShadowComponent_Draw(Entities.List[i]);
		}
	}

	Gfx_SetAlphaArgBlend(false);
	Gfx_SetDepthWrite(true);
	Gfx_SetAlphaBlending(false);
	Gfx_SetTexturing(false);
}


/*########################################################################################################################*
*--------------------------------------------------------TabList----------------------------------------------------------*
*#########################################################################################################################*/
struct _TabListData TabList;

/* Removes the names from the names buffer for the given id. */
static void TabList_Delete(EntityID id) {
	int i, index;
	index = TabList.NameOffsets[id];
	if (!index) return;

	StringsBuffer_Remove(&TabList._buffer, index - 1);
	StringsBuffer_Remove(&TabList._buffer, index - 2);
	StringsBuffer_Remove(&TabList._buffer, index - 3);

	/* Indices after this entry need to be shifted down */
	for (i = 0; i < TABLIST_MAX_NAMES; i++) {
		if (TabList.NameOffsets[i] > index) TabList.NameOffsets[i] -= 3;
	}
}

void TabList_Remove(EntityID id) {
	TabList_Delete(id);
	TabList.NameOffsets[id] = 0;
	TabList.GroupRanks[id]  = 0;
	Event_RaiseInt(&TabListEvents.Removed, id);
}

void TabList_Set(EntityID id, const String* player, const String* list, const String* group, cc_uint8 rank) {
	String oldPlayer, oldList, oldGroup;
	cc_uint8 oldRank;
	struct Event_Int* events;
	
	if (TabList.NameOffsets[id]) {
		oldPlayer = TabList_UNSAFE_GetPlayer(id);
		oldList   = TabList_UNSAFE_GetList(id);
		oldGroup  = TabList_UNSAFE_GetGroup(id);
		oldRank   = TabList.GroupRanks[id];

		/* Don't redraw the tab list if nothing changed. */
		if (String_Equals(player, &oldPlayer)  && String_Equals(list, &oldList)
			&& String_Equals(group, &oldGroup) && rank == oldRank) return;

		events = &TabListEvents.Changed;
	} else {
		events = &TabListEvents.Added;
	}
	TabList_Delete(id);

	StringsBuffer_Add(&TabList._buffer, player);
	StringsBuffer_Add(&TabList._buffer, list);
	StringsBuffer_Add(&TabList._buffer, group);

	TabList.NameOffsets[id] = TabList._buffer.count;
	TabList.GroupRanks[id]  = rank;
	Event_RaiseInt(events, id);
}

static void TabList_Free(void) { StringsBuffer_Clear(&TabList._buffer); }
static void TabList_Reset(void) {
	Mem_Set(TabList.NameOffsets, 0, sizeof(TabList.NameOffsets));
	Mem_Set(TabList.GroupRanks,  0, sizeof(TabList.GroupRanks));
	StringsBuffer_Clear(&TabList._buffer);
}

struct IGameComponent TabList_Component = {
	NULL,         /* Init  */
	TabList_Free, /* Free  */
	TabList_Reset /* Reset */
};


static void Player_Despawn(struct Entity* e) {
	Entity_DeleteSkin(e);
	Entity_ContextLost(e);
}


/*########################################################################################################################*
*------------------------------------------------------LocalPlayer--------------------------------------------------------*
*#########################################################################################################################*/
struct LocalPlayer LocalPlayer_Instance;
static cc_bool hackPermMsgs;
float LocalPlayer_JumpHeight(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	return (float)PhysicsComp_CalcMaxHeight(p->Physics.JumpVel);
}

void LocalPlayer_SetInterpPosition(float t) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (!(p->Hacks.WOMStyleHacks && p->Hacks.Noclip)) {
		Vec3_Lerp(&p->Base.Position, &p->Interp.Prev.Pos, &p->Interp.Next.Pos, t);
	}
	InterpComp_LerpAngles((struct InterpComp*)(&p->Interp), &p->Base, t);
}

static void LocalPlayer_HandleInput(float* xMoving, float* zMoving) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct HacksComp* hacks = &p->Hacks;

	if (Gui_GetInputGrab()) {
		p->Physics.Jumping = false; hacks->Speeding = false;
		hacks->FlyingUp    = false; hacks->FlyingDown = false;
	} else {
		if (KeyBind_IsPressed(KEYBIND_FORWARD)) *zMoving -= 0.98f;
		if (KeyBind_IsPressed(KEYBIND_BACK))    *zMoving += 0.98f;
		if (KeyBind_IsPressed(KEYBIND_LEFT))    *xMoving -= 0.98f;
		if (KeyBind_IsPressed(KEYBIND_RIGHT))   *xMoving += 0.98f;

		p->Physics.Jumping  = KeyBind_IsPressed(KEYBIND_JUMP);
		hacks->Speeding     = hacks->Enabled && KeyBind_IsPressed(KEYBIND_SPEED);
		hacks->HalfSpeeding = hacks->Enabled && KeyBind_IsPressed(KEYBIND_HALF_SPEED);
		hacks->FlyingUp     = KeyBind_IsPressed(KEYBIND_FLY_UP);
		hacks->FlyingDown   = KeyBind_IsPressed(KEYBIND_FLY_DOWN);

		if (hacks->WOMStyleHacks && hacks->Enabled && hacks->CanNoclip) {
			if (hacks->Noclip) {
				/* need the { } because it's a macro */
				Vec3_Set(p->Base.Velocity, 0,0,0);
			}
			hacks->Noclip = KeyBind_IsPressed(KEYBIND_NOCLIP);
		}
	}
}

static void LocalPlayer_SetLocation(struct Entity* e, struct LocationUpdate* update, cc_bool interpolate) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	LocalInterpComp_SetLocation(&p->Interp, update, interpolate);
}

static void LocalPlayer_Tick(struct Entity* e, double delta) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	struct HacksComp* hacks = &p->Hacks;
	float xMoving = 0, zMoving = 0;
	cc_bool wasOnGround;
	Vec3 headingVelocity;

	if (!World.Blocks) return;
	e->StepSize = hacks->FullBlockStep && hacks->Enabled && hacks->CanSpeed ? 1.0f : 0.5f;
	p->OldVelocity = e->Velocity;
	wasOnGround    = e->OnGround;

	LocalInterpComp_AdvanceState(&p->Interp);
	LocalPlayer_HandleInput(&xMoving, &zMoving);
	hacks->Floating = hacks->Noclip || hacks->Flying;
	if (!hacks->Floating && hacks->CanBePushed) PhysicsComp_DoEntityPush(e);

	/* Immediate stop in noclip mode */
	if (!hacks->NoclipSlide && (hacks->Noclip && xMoving == 0 && zMoving == 0)) {
		Vec3_Set(e->Velocity, 0,0,0);
	}

	PhysicsComp_UpdateVelocityState(&p->Physics);
	headingVelocity = Vec3_RotateY3(xMoving, 0, zMoving, e->Yaw * MATH_DEG2RAD);
	PhysicsComp_PhysicsTick(&p->Physics, headingVelocity);

	/* Fixes high jump, when holding down a movement key, jump, fly, then let go of fly key */
	if (p->Hacks.Floating) e->Velocity.Y = 0.0f;

	p->Interp.Next.Pos = e->Position; e->Position = p->Interp.Prev.Pos;
	AnimatedComp_Update(e, p->Interp.Prev.Pos, p->Interp.Next.Pos, delta);
	TiltComp_Update(&p->Tilt, delta);

	Entity_CheckSkin(&p->Base);
	SoundComp_Tick(wasOnGround);
}

static void LocalPlayer_RenderModel(struct Entity* e, double deltaTime, float t) {
	struct LocalPlayer* p = (struct LocalPlayer*)e;
	AnimatedComp_GetCurrent(e, t);
	TiltComp_GetCurrent(&p->Tilt, t);

	if (!Camera.Active->isThirdPerson) return;
	Model_Render(e->Model, e);
}

static void LocalPlayer_RenderName(struct Entity* e) {
	if (!Camera.Active->isThirdPerson) return;
	Entity_DrawName(e);
}

static void LocalPlayer_CheckJumpVelocity(void* obj) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (!HacksComp_CanJumpHigher(&p->Hacks)) {
		p->Physics.JumpVel = p->Physics.ServerJumpVel;
	}
}

static struct EntityVTABLE localPlayer_VTABLE = {
	LocalPlayer_Tick,        Player_Despawn,         LocalPlayer_SetLocation, Entity_GetCol,
	LocalPlayer_RenderModel, LocalPlayer_RenderName
};
static void LocalPlayer_Init(void) {
	struct LocalPlayer* p   = &LocalPlayer_Instance;
	struct HacksComp* hacks = &p->Hacks;

	Entity_Init(&p->Base);
	Entity_SetName(&p->Base, &Game_Username);
	Entity_SetSkin(&p->Base, &Game_Username);
	Event_RegisterVoid(&UserEvents.HackPermissionsChanged, NULL, LocalPlayer_CheckJumpVelocity);

	p->Collisions.Entity = &p->Base;
	HacksComp_Init(hacks);
	PhysicsComp_Init(&p->Physics, &p->Base);
	TiltComp_Init(&p->Tilt);

	p->Base.ModelRestrictedScale = true;
	p->ReachDistance = 5.0f;
	p->Physics.Hacks = &p->Hacks;
	p->Physics.Collisions = &p->Collisions;
	p->Base.VTABLE   = &localPlayer_VTABLE;

	hacks->Enabled = !Game_PureClassic && Options_GetBool(OPT_HACKS_ENABLED, true);
	/* p->Base.Health = 20; TODO: survival mode stuff */
	if (Game_ClassicMode) return;

	hacks->SpeedMultiplier = Options_GetFloat(OPT_SPEED_FACTOR, 0.1f, 50.0f, 10.0f);
	hacks->PushbackPlacing = Options_GetBool(OPT_PUSHBACK_PLACING, false);
	hacks->NoclipSlide     = Options_GetBool(OPT_NOCLIP_SLIDE, false);
	hacks->WOMStyleHacks   = Options_GetBool(OPT_WOM_STYLE_HACKS, false);
	hacks->FullBlockStep   = Options_GetBool(OPT_FULL_BLOCK_STEP, false);
	p->Physics.UserJumpVel = Options_GetFloat(OPT_JUMP_VELOCITY, 0.0f, 52.0f, 0.42f);
	p->Physics.JumpVel     = p->Physics.UserJumpVel;
	hackPermMsgs           = Options_GetBool(OPT_HACK_PERM_MSGS, true);
}

static void LocalPlayer_Reset(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	p->ReachDistance = 5.0f;
	Vec3_Set(p->Base.Velocity, 0,0,0);
	p->Physics.JumpVel       = 0.42f;
	p->Physics.ServerJumpVel = 0.42f;
	/* p->Base.Health = 20; TODO: survival mode stuff */
}

static void LocalPlayer_OnNewMap(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	Vec3_Set(p->Base.Velocity, 0,0,0);
	Vec3_Set(p->OldVelocity,   0,0,0);

	p->_warnedRespawn = false;
	p->_warnedFly     = false;
	p->_warnedNoclip  = false;
}

static cc_bool LocalPlayer_IsSolidCollide(BlockID b) { return Blocks.Collide[b] == COLLIDE_SOLID; }
static void LocalPlayer_DoRespawn(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct LocationUpdate update;
	struct AABB bb;
	Vec3 spawn = p->Spawn;
	IVec3 pos;
	BlockID block;
	float height, spawnY;
	int y;

	if (!World.Blocks) return;
	IVec3_Floor(&pos, &spawn);	

	/* Spawn player at highest solid position to match vanilla Minecraft classic */
	/* Only when player can noclip, since this can let you 'clip' to above solid blocks */
	if (p->Hacks.CanNoclip) {
		AABB_Make(&bb, &spawn, &p->Base.Size);
		for (y = pos.Y; y <= World.Height; y++) {
			spawnY = Respawn_HighestSolidY(&bb);

			if (spawnY == RESPAWN_NOT_FOUND) {
				block   = World_SafeGetBlock(pos.X, y, pos.Z);
				height  = Blocks.Collide[block] == COLLIDE_SOLID ? Blocks.MaxBB[block].Y : 0.0f;
				spawn.Y = y + height + ENTITY_ADJUSTMENT;
				break;
			}
			bb.Min.Y += 1.0f; bb.Max.Y += 1.0f;
		}
	}

	spawn.Y += 2.0f/16.0f;
	LocationUpdate_MakePosAndOri(&update, spawn, p->SpawnYaw, p->SpawnPitch, false);
	p->Base.VTABLE->SetLocation(&p->Base, &update, false);
	Vec3_Set(p->Base.Velocity, 0,0,0);

	/* Update onGround, otherwise if 'respawn' then 'space' is pressed, you still jump into the air if onGround was true before */
	Entity_GetBounds(&p->Base, &bb);
	bb.Min.Y -= 0.01f; bb.Max.Y = bb.Min.Y;
	p->Base.OnGround = Entity_TouchesAny(&bb, LocalPlayer_IsSolidCollide);
}

static cc_bool LocalPlayer_HandleRespawn(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (p->Hacks.CanRespawn) {
		LocalPlayer_DoRespawn();
		return true;
	} else if (!p->_warnedRespawn) {
		p->_warnedRespawn = true;
		if (hackPermMsgs) Chat_AddRaw("&cRespawning is currently disabled");
	}
	return false;
}

static cc_bool LocalPlayer_HandleSetSpawn(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (p->Hacks.CanRespawn) {

		if (!p->Hacks.CanNoclip && !p->Base.OnGround) {
			Chat_AddRaw("&cCannot set spawn midair when noclip is disabled");
			return false;
		}

		/* Spawn is normally centered to match vanilla Minecraft classic */
		if (!p->Hacks.CanNoclip) {
			p->Spawn   = p->Base.Position;
		} else {
			p->Spawn.X = Math_Floor(p->Base.Position.X) + 0.5f;
			p->Spawn.Y = p->Base.Position.Y;
			p->Spawn.Z = Math_Floor(p->Base.Position.Z) + 0.5f;
		}
		
		p->SpawnYaw   = p->Base.Yaw;
		p->SpawnPitch = p->Base.Pitch;
	}
	return LocalPlayer_HandleRespawn();
}

static cc_bool LocalPlayer_HandleFly(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (p->Hacks.CanFly && p->Hacks.Enabled) {
		p->Hacks.Flying = !p->Hacks.Flying;
		return true;
	} else if (!p->_warnedFly) {
		p->_warnedFly = true;
		if (hackPermMsgs) Chat_AddRaw("&cFlying is currently disabled");
	}
	return false;
}

static cc_bool LocalPlayer_HandleNoClip(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (p->Hacks.CanNoclip && p->Hacks.Enabled) {
		if (p->Hacks.WOMStyleHacks) return true; /* don't handle this here */
		if (p->Hacks.Noclip) p->Base.Velocity.Y = 0;

		p->Hacks.Noclip = !p->Hacks.Noclip;
		return true;
	} else if (!p->_warnedNoclip) {
		p->_warnedNoclip = true;
		if (hackPermMsgs) Chat_AddRaw("&cNoclip is currently disabled");
	}
	return false;
}

cc_bool LocalPlayer_HandlesKey(int key) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct HacksComp* hacks = &p->Hacks;
	struct PhysicsComp* physics = &p->Physics;
	int maxJumps;

	if (key == KeyBinds[KEYBIND_RESPAWN]) {
		return LocalPlayer_HandleRespawn();
	} else if (key == KeyBinds[KEYBIND_SET_SPAWN]) {
		return LocalPlayer_HandleSetSpawn();
	} else if (key == KeyBinds[KEYBIND_FLY]) {
		return LocalPlayer_HandleFly();
	} else if (key == KeyBinds[KEYBIND_NOCLIP]) {
		return LocalPlayer_HandleNoClip();
	} else if (key == KeyBinds[KEYBIND_JUMP] && !p->Base.OnGround && !(hacks->Flying || hacks->Noclip)) {
		maxJumps = hacks->CanDoubleJump && hacks->WOMStyleHacks ? 2 : 0;
		maxJumps = max(maxJumps, hacks->MaxJumps - 1);

		if (physics->MultiJumps < maxJumps) {
			PhysicsComp_DoNormalJump(physics);
			physics->MultiJumps++;
		}
		return true;
	}
	return false;
}


/*########################################################################################################################*
*-------------------------------------------------------NetPlayer---------------------------------------------------------*
*#########################################################################################################################*/
struct NetPlayer NetPlayers_List[ENTITIES_SELF_ID];

static void NetPlayer_SetLocation(struct Entity* e, struct LocationUpdate* update, cc_bool interpolate) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	NetInterpComp_SetLocation(&p->Interp, update, interpolate);
}

static void NetPlayer_Tick(struct Entity* e, double delta) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	Entity_CheckSkin(e);
	NetInterpComp_AdvanceState(&p->Interp);
	AnimatedComp_Update(e, p->Interp.Prev.Pos, p->Interp.Next.Pos, delta);
}

static void NetPlayer_RenderModel(struct Entity* e, double deltaTime, float t) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	Vec3_Lerp(&e->Position, &p->Interp.Prev.Pos, &p->Interp.Next.Pos, t);
	InterpComp_LerpAngles((struct InterpComp*)(&p->Interp), e, t);

	AnimatedComp_GetCurrent(e, t);
	p->ShouldRender = Model_ShouldRender(e);
	if (p->ShouldRender) Model_Render(e->Model, e);
}

static void NetPlayer_RenderName(struct Entity* e) {
	struct NetPlayer* p = (struct NetPlayer*)e;
	float distance;
	int threshold;
	if (!p->ShouldRender) return;

	distance  = Model_RenderDistance(e);
	threshold = Entities.NamesMode == NAME_MODE_ALL_UNSCALED ? 8192 * 8192 : 32 * 32;
	if (distance <= (float)threshold) Entity_DrawName(e);
}

struct EntityVTABLE netPlayer_VTABLE = {
	NetPlayer_Tick,        Player_Despawn,       NetPlayer_SetLocation, Entity_GetCol,
	NetPlayer_RenderModel, NetPlayer_RenderName
};
void NetPlayer_Init(struct NetPlayer* p) {
	Mem_Set(p, 0, sizeof(struct NetPlayer));
	Entity_Init(&p->Base);
	p->Base.VTABLE = &netPlayer_VTABLE;
}



/*########################################################################################################################*
*---------------------------------------------------Entities component----------------------------------------------------*
*#########################################################################################################################*/
static void Entities_Init(void) {
	Event_RegisterVoid(&GfxEvents.ContextLost,      NULL, Entities_ContextLost);
	Event_RegisterVoid(&GfxEvents.ContextRecreated, NULL, Entities_ContextRecreated);
	Event_RegisterVoid(&ChatEvents.FontChanged,     NULL, Entities_ChatFontChanged);

	Entities.NamesMode = Options_GetEnum(OPT_NAMES_MODE, NAME_MODE_HOVERED,
		NameMode_Names, Array_Elems(NameMode_Names));
	if (Game_ClassicMode) Entities.NamesMode = NAME_MODE_HOVERED;

	Entities.ShadowsMode = Options_GetEnum(OPT_ENTITY_SHADOW, SHADOW_MODE_NONE,
		ShadowMode_Names, Array_Elems(ShadowMode_Names));
	if (Game_ClassicMode) Entities.ShadowsMode = SHADOW_MODE_NONE;

	Entities.List[ENTITIES_SELF_ID] = &LocalPlayer_Instance.Base;
	LocalPlayer_Init();
}

static void Entities_Free(void) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (!Entities.List[i]) continue;
		Entities_Remove((EntityID)i);
	}

	Event_UnregisterVoid(&GfxEvents.ContextLost,      NULL, Entities_ContextLost);
	Event_UnregisterVoid(&GfxEvents.ContextRecreated, NULL, Entities_ContextRecreated);
	Event_UnregisterVoid(&ChatEvents.FontChanged,     NULL, Entities_ChatFontChanged);

	if (ShadowComponent_ShadowTex) {
		Gfx_DeleteTexture(&ShadowComponent_ShadowTex);
	}
}

struct IGameComponent Entities_Component = {
	Entities_Init,  /* Init  */
	Entities_Free,  /* Free  */
	LocalPlayer_Reset,    /* Reset */
	LocalPlayer_OnNewMap, /* OnNewMap */
};
