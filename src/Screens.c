#include "Screens.h"
#include "Widgets.h"
#include "Game.h"
#include "Event.h"
#include "Platform.h"
#include "Inventory.h"
#include "Drawer2D.h"
#include "Graphics.h"
#include "Funcs.h"
#include "TexturePack.h"
#include "Model.h"
#include "Generator.h"
#include "Server.h"
#include "Chat.h"
#include "ExtMath.h"
#include "Window.h"
#include "Camera.h"
#include "Http.h"
#include "Block.h"
#include "Menus.h"
#include "World.h"

#define CHAT_MAX_STATUS Array_Elems(Chat_Status)
#define CHAT_MAX_BOTTOMRIGHT Array_Elems(Chat_BottomRight)
#define CHAT_MAX_CLIENTSTATUS Array_Elems(Chat_ClientStatus)

int Screen_FInput(void* s, int key)           { return false; }
int Screen_FKeyPress(void* s, char keyChar)   { return false; }
int Screen_FText(void* s, const String* str)  { return false; }
int Screen_FMouseScroll(void* s, float delta) { return false; }
int Screen_FPointer(void* s, int id, int x, int y) { return false; }

int Screen_TInput(void* s, int key)           { return true; }
int Screen_TKeyPress(void* s, char keyChar)   { return true; }
int Screen_TText(void* s, const String* str)  { return true; }
int Screen_TMouseScroll(void* s, float delta) { return true; }
int Screen_TPointer(void* s, int id, int x, int y) { return true; }

void Screen_NullFunc(void* screen) { }
void Screen_NullUpdate(void* screen, double delta) { }
int  Screen_InputDown(void* screen, int key) { return key < KEY_F1 || key > KEY_F35; }

CC_NOINLINE static cc_bool IsOnlyHudActive(void) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui_ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->grabsInput && s != (struct Screen*)Gui_Chat) return false;
	}
	return true;
}

void Screen_RenderWidgets(void* screen, double delta) {
	struct Screen* s = (struct Screen*)screen;
	struct Widget** widgets = s->widgets;
	int i;

	for (i = 0; i < s->numWidgets; i++) {
		if (!widgets[i]) continue;
		Elem_Render(widgets[i], delta);
	}
}

void Screen_Render2Widgets(void* screen, double delta) {
	struct Screen* s = (struct Screen*)screen;
	struct Widget** widgets = s->widgets;
	int i, offset = 0;

	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	Gfx_BindDynamicVb(s->vb);

	for (i = 0; i < s->numWidgets; i++) {
		if (!widgets[i]) continue;
		offset = Widget_Render2(widgets[i], offset);
	}
}

void Screen_Layout(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	struct Widget** widgets = s->widgets;
	int i;

	for (i = 0; i < s->numWidgets; i++) {
		if (!widgets[i]) continue;
		Widget_Layout(widgets[i]);
	}
}

void Screen_ContextLost(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	struct Widget** widgets = s->widgets;
	int i;
	Gfx_DeleteDynamicVb(&s->vb);

	for (i = 0; i < s->numWidgets; i++) {
		if (!widgets[i]) continue;
		Elem_Free(widgets[i]);
	}
}

void Screen_CreateVb(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	s->vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_P3FT2FC4B, s->maxVertices);
}

void Screen_BuildMesh(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	struct Widget** widgets = s->widgets;
	VertexP3fT2fC4b* data;
	VertexP3fT2fC4b** ptr;
	int i;

	data = (VertexP3fT2fC4b*)Gfx_LockDynamicVb(s->vb, VERTEX_FORMAT_P3FT2FC4B, s->maxVertices);
	ptr  = &data;

	for (i = 0; i < s->numWidgets; i++) {
		if (!widgets[i]) continue;
		Widget_BuildMesh(widgets[i], ptr);
	}
	Gfx_UnlockDynamicVb(s->vb);
}


/*########################################################################################################################*
*--------------------------------------------------------HUDScreen--------------------------------------------------------*
*#########################################################################################################################*/
static struct HUDScreen {
	Screen_Body
	struct FontDesc font;
	struct TextWidget line1, line2;
	struct TextAtlas posAtlas;
	double accumulator;
	int frames, fps;
	cc_bool speed, halfSpeed, noclip, fly, canSpeed;
	int lastFov;
	struct HotbarWidget hotbar;
} HUDScreen_Instance;

static void HUDScreen_MakeText(struct HUDScreen* s, String* status) {
	int indices, ping;
	s->fps = (int)(s->frames / s->accumulator);
	String_Format1(status, "%i fps, ", &s->fps);

	if (Game_ClassicMode) {
		String_Format1(status, "%i chunk updates", &Game.ChunkUpdates);
	} else {
		if (Game.ChunkUpdates) {
			String_Format1(status, "%i chunks/s, ", &Game.ChunkUpdates);
		}

		indices = ICOUNT(Game_Vertices);
		String_Format1(status, "%i vertices", &indices);

		ping = Ping_AveragePingMS();
		if (ping) String_Format1(status, ", ping %i ms", &ping);
	}
}

static void HUDScreen_DrawPosition(struct HUDScreen* s) {
	VertexP3fT2fC4b vertices[4 * 64];
	VertexP3fT2fC4b* ptr = vertices;
	PackedCol col = PACKEDCOL_WHITE;

	struct TextAtlas* atlas = &s->posAtlas;
	struct Texture tex;
	IVec3 pos;
	int count;	

	/* Make "Position: " prefix */
	tex = atlas->tex; 
	tex.X = 2; tex.Width = atlas->offset;
	Gfx_Make2DQuad(&tex, col, &ptr);

	IVec3_Floor(&pos, &LocalPlayer_Instance.Base.Position);
	atlas->curX = atlas->offset + 2;

	/* Make (X, Y, Z) suffix */
	TextAtlas_Add(atlas, 13, &ptr);
	TextAtlas_AddInt(atlas, pos.X, &ptr);
	TextAtlas_Add(atlas, 11, &ptr);
	TextAtlas_AddInt(atlas, pos.Y, &ptr);
	TextAtlas_Add(atlas, 11, &ptr);
	TextAtlas_AddInt(atlas, pos.Z, &ptr);
	TextAtlas_Add(atlas, 14, &ptr);

	Gfx_BindTexture(atlas->tex.ID);
	/* TODO: Do we need to use a separate VB here? */
	count = (int)(ptr - vertices);
	Gfx_UpdateDynamicVb_IndexedTris(Models.Vb, vertices, count);
}

static cc_bool HUDScreen_HacksChanged(struct HUDScreen* s) {
	struct HacksComp* hacks = &LocalPlayer_Instance.Hacks;
	return hacks->Speeding != s->speed || hacks->HalfSpeeding != s->halfSpeed || hacks->Flying != s->fly
		|| hacks->Noclip != s->noclip  || Game_Fov != s->lastFov || hacks->CanSpeed != s->canSpeed;
}

static void HUDScreen_UpdateHackState(struct HUDScreen* s) {
	String status; char statusBuffer[STRING_SIZE * 2];
	struct HacksComp* hacks;
	cc_bool speeding;

	hacks = &LocalPlayer_Instance.Hacks;
	s->speed = hacks->Speeding; s->halfSpeed = hacks->HalfSpeeding; s->fly = hacks->Flying;
	s->noclip = hacks->Noclip;  s->lastFov = Game_Fov; s->canSpeed = hacks->CanSpeed;

	String_InitArray(status, statusBuffer);
	if (Game_Fov != Game_DefaultFov) {
		String_Format1(&status, "Zoom fov %i  ", &Game_Fov);
	}
	speeding = (hacks->Speeding || hacks->HalfSpeeding) && hacks->CanSpeed;

	if (hacks->Flying) String_AppendConst(&status, "Fly ON   ");
	if (speeding)      String_AppendConst(&status, "Speed ON   ");
	if (hacks->Noclip) String_AppendConst(&status, "Noclip ON   ");

	TextWidget_Set(&s->line2, &status, &s->font);
}

static void HUDScreen_Update(struct HUDScreen* s, double delta) {
	String status; char statusBuffer[STRING_SIZE * 2];

	s->frames++;
	s->accumulator += delta;
	if (s->accumulator < 1.0) return;

	String_InitArray(status, statusBuffer);
	HUDScreen_MakeText(s, &status);

	TextWidget_Set(&s->line1, &status, &s->font);
	s->accumulator = 0.0;
	s->frames = 0;
	Game.ChunkUpdates = 0;
}

static void HUDScreen_ContextLost(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	Font_Free(&s->font);
	TextAtlas_Free(&s->posAtlas);
	Elem_TryFree(&s->line1);
	Elem_TryFree(&s->line2);
}

static void HUDScreen_ContextRecreated(void* screen) {	
	static const String chars  = String_FromConst("0123456789-, ()");
	static const String prefix = String_FromConst("Position: ");

	struct HUDScreen* s      = (struct HUDScreen*)screen;
	struct TextWidget* line1 = &s->line1;
	struct TextWidget* line2 = &s->line2;
	int y;

	Widget_Layout(&s->hotbar);
	Drawer2D_MakeFont(&s->font, 16, FONT_STYLE_NORMAL);
	Font_ReducePadding(&s->font, 4);
	
	y = 2;
	TextWidget_Make(line1, ANCHOR_MIN, ANCHOR_MIN, 2, y);
	HUDScreen_Update(s, 1.0);

	y += line1->height;
	TextAtlas_Make(&s->posAtlas, &chars, &s->font, &prefix);
	s->posAtlas.tex.Y = y;

	y += s->posAtlas.tex.Height;
	TextWidget_Make(line2, ANCHOR_MIN, ANCHOR_MIN, 2, 0);
	/* We can't pass y to TextWidget_Make because that DPI scales it */
	line2->yOffset = y;

	if (Game_ClassicMode) {
		/* Swap around so 0.30 version is at top */
		line2->yOffset = 2;
		line1->yOffset = s->posAtlas.tex.Y;
		TextWidget_SetConst(line2, "0.30", &s->font);

		Widget_Layout(line1);
		Widget_Layout(line2);
	} else {
		HUDScreen_UpdateHackState(s);
	}
}

static void HUDScreen_BuildMesh(void* screen) { }

static void HUDScreen_Layout(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	Widget_Layout(&s->hotbar);
}

static int HUDScreen_KeyDown(void* screen, int key) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	return Elem_HandlesKeyDown(&s->hotbar, key);
}

static int HUDScreen_KeyUp(void* screen, int key) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	return Elem_HandlesKeyUp(&s->hotbar, key);
}

static int HUDscreen_PointerDown(void* screen, int id, int x, int y) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
#ifdef CC_BUILD_TOUCH
	if (Input_TouchMode || Gui_GetInputGrab()) {
		return Elem_HandlesPointerDown(&s->hotbar, id, x, y);
	}
#else
	if (Gui_GetInputGrab()) return Elem_HandlesPointerDown(&s->hotbar, id, x, y);
#endif
	return false;
}

static void HUDScreen_Init(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	HotbarWidget_Create(&s->hotbar);
}

static void HUDScreen_Render(void* screen, double delta) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	HUDScreen_Update(s, delta);
	if (Game_HideGui) return;

	/* TODO: If Game_ShowFps is off and not classic mode, we should just return here */
	Gfx_SetTexturing(true);
	if (Gui_ShowFPS) Elem_Render(&s->line1, delta);

	if (Game_ClassicMode) {
		Elem_Render(&s->line2, delta);
	} else if (IsOnlyHudActive() && Gui_ShowFPS) {
		if (HUDScreen_HacksChanged(s)) { HUDScreen_UpdateHackState(s); }
		HUDScreen_DrawPosition(s);
		Elem_Render(&s->line2, delta);
	}

	if (!Gui_GetBlocksWorld()) Elem_Render(&s->hotbar, delta);
	Gfx_SetTexturing(false);
}

static const struct ScreenVTABLE HUDScreen_VTABLE = {
	HUDScreen_Init,        Screen_NullUpdate,   Screen_NullFunc,  
	HUDScreen_Render,      HUDScreen_BuildMesh,
	HUDScreen_KeyDown,     HUDScreen_KeyUp,     Screen_FKeyPress, Screen_FText,
	HUDscreen_PointerDown, Screen_FPointer,     Screen_FPointer,  Screen_FMouseScroll,
	HUDScreen_Layout,      HUDScreen_ContextLost, HUDScreen_ContextRecreated
};
void HUDScreen_Show(void) {
	struct HUDScreen* s = &HUDScreen_Instance;
	s->VTABLE = &HUDScreen_VTABLE;
	Gui_HUD   = s;
	Gui_Replace((struct Screen*)s, GUI_PRIORITY_HUD);
}

struct Widget* HUDScreen_GetHotbar(void) {
	return (struct Widget*)&HUDScreen_Instance.hotbar;
}


/*########################################################################################################################*
*--------------------------------------------------------ChatScreen-------------------------------------------------------*
*#########################################################################################################################*/
static struct ChatScreen {
	Screen_Body
	/* player list state */
	struct PlayerListWidget playerList;
	struct FontDesc playerFont;
	cc_bool showingList;
	/* chat state */
	float chatAcc;
	cc_bool suppressNextPress;
	int chatIndex;
	int lastDownloadStatus;
	struct FontDesc chatFont, announcementFont;
	struct TextWidget announcement;
	struct ChatInputWidget input;
	struct TextGroupWidget status, bottomRight, chat, clientStatus;
	struct SpecialInputWidget altText;
#ifdef CC_BUILD_TOUCH
	struct ButtonWidget send, cancel;
#endif

	struct Texture statusTextures[CHAT_MAX_STATUS];
	struct Texture bottomRightTextures[CHAT_MAX_BOTTOMRIGHT];
	struct Texture clientStatusTextures[CHAT_MAX_CLIENTSTATUS];
	struct Texture chatTextures[TEXTGROUPWIDGET_MAX_LINES];
} ChatScreen_Instance;
#define CH_EXTENT 16

static void ChatScreen_UpdateChatYOffsets(struct ChatScreen* s) {
	int pad, y;

	/* Determining chat Y requires us to know hotbar's position */
	/* But HUD is lower priority, so it gets laid out AFTER chat */
	/* Hence use this hack to resize HUD first */
	HUDScreen_Layout(Gui_HUD);
		
	y = min(s->input.base.y, Gui_HUD->hotbar.y);
	y -= s->input.base.yOffset; /* add some padding */
	s->altText.yOffset = Window_Height - y;
	Widget_Layout(&s->altText);

	pad = s->altText.active ? 5 : 10;
	s->clientStatus.yOffset = Window_Height - s->altText.y + pad;
	Widget_Layout(&s->clientStatus);
	s->chat.yOffset = s->clientStatus.yOffset + s->clientStatus.height;
	Widget_Layout(&s->chat);
}

static String ChatScreen_GetChat(void* obj, int i) {
	i += ChatScreen_Instance.chatIndex;

	if (i >= 0 && i < Chat_Log.count) {
		return StringsBuffer_UNSAFE_Get(&Chat_Log, i);
	}
	return String_Empty;
}

static String ChatScreen_GetStatus(void* obj, int i)       { return Chat_Status[i]; }
static String ChatScreen_GetBottomRight(void* obj, int i)  { return Chat_BottomRight[2 - i]; }
static String ChatScreen_GetClientStatus(void* obj, int i) { return Chat_ClientStatus[i]; }

static void ChatScreen_FreeChatFonts(struct ChatScreen* s) {
	Font_Free(&s->chatFont);
	Font_Free(&s->announcementFont);
}

static cc_bool ChatScreen_ChatUpdateFont(struct ChatScreen* s) {
	int size = (int)(8  * Game_GetChatScale());
	Math_Clamp(size, 8, 60);

	/* don't recreate font if possible */
	if (size == s->chatFont.size) return false;
	ChatScreen_FreeChatFonts(s);
	Drawer2D_MakeFont(&s->chatFont, size, FONT_STYLE_NORMAL);

	size = (int)(16 * Game_GetChatScale());
	Math_Clamp(size, 8, 60);
	Drawer2D_MakeFont(&s->announcementFont, size, FONT_STYLE_NORMAL);

	ChatInputWidget_SetFont(&s->input,        &s->chatFont);
	TextGroupWidget_SetFont(&s->status,       &s->chatFont);
	TextGroupWidget_SetFont(&s->bottomRight,  &s->chatFont);
	TextGroupWidget_SetFont(&s->chat,         &s->chatFont);
	TextGroupWidget_SetFont(&s->clientStatus, &s->chatFont);
	return true;
}

static void ChatScreen_ChatUpdateLayout(struct ChatScreen* s) {
	int yOffset = Gui_HUD->hotbar.height + 15;
	Widget_SetLocation(&s->input.base,   ANCHOR_MIN, ANCHOR_MAX,  5, 5);
	Widget_SetLocation(&s->altText,      ANCHOR_MIN, ANCHOR_MAX,  5, 5);
	Widget_SetLocation(&s->status,       ANCHOR_MAX, ANCHOR_MIN,  0, 0);
	Widget_SetLocation(&s->bottomRight,  ANCHOR_MAX, ANCHOR_MAX,  0, yOffset);
	Widget_SetLocation(&s->chat,         ANCHOR_MIN, ANCHOR_MAX, 10, 0);
	Widget_SetLocation(&s->clientStatus, ANCHOR_MIN, ANCHOR_MAX, 10, 0);
	ChatScreen_UpdateChatYOffsets(s);
}

static void ChatScreen_Redraw(struct ChatScreen* s) {
	TextGroupWidget_RedrawAll(&s->chat);
	TextWidget_Set(&s->announcement, &Chat_Announcement, &s->announcementFont);
	TextGroupWidget_RedrawAll(&s->status);
	TextGroupWidget_RedrawAll(&s->bottomRight);
	TextGroupWidget_RedrawAll(&s->clientStatus);

	if (s->grabsInput) InputWidget_UpdateText(&s->input.base);
	SpecialInputWidget_Redraw(&s->altText);
}

static int ChatScreen_ClampChatIndex(int index) {
	int maxIndex = Chat_Log.count - Gui_Chatlines;
	int minIndex = min(0, maxIndex);
	Math_Clamp(index, minIndex, maxIndex);
	return index;
}

static void ChatScreen_ScrollChatBy(struct ChatScreen* s, int delta) {
	int newIndex = ChatScreen_ClampChatIndex(s->chatIndex + delta);
	delta = newIndex - s->chatIndex;

	while (delta) {
		if (delta < 0) {
			/* scrolling up to oldest */
			s->chatIndex--; delta++;
			TextGroupWidget_ShiftDown(&s->chat);
		} else {
			/* scrolling down to newest */
			s->chatIndex++; delta--;
			TextGroupWidget_ShiftUp(&s->chat);
		}
	}
}

static void ChatScreen_EnterChatInput(struct ChatScreen* s, cc_bool close) {
	struct InputWidget* input;
	int defaultIndex;

	s->grabsInput = false;
	Camera_CheckFocus();
	Window_CloseKeyboard();
	if (close) InputWidget_Clear(&s->input.base);

	input = &s->input.base;
	input->OnPressedEnter(input);
	SpecialInputWidget_SetActive(&s->altText, false);
	ChatScreen_UpdateChatYOffsets(s);

	/* Reset chat when user has scrolled up in chat history */
	defaultIndex = Chat_Log.count - Gui_Chatlines;
	if (s->chatIndex != defaultIndex) {
		s->chatIndex = defaultIndex;
		TextGroupWidget_RedrawAll(&s->chat);
	}
}

static void ChatScreen_UpdateTexpackStatus(struct ChatScreen* s) {
	static const String texPack = String_FromConst("texturePack");
	struct HttpRequest request;
	int progress;
	cc_bool hasRequest;
	String identifier;
	
	hasRequest = Http_GetCurrent(&request, &progress);
	identifier = String_FromRawArray(request.id);	

	/* Is terrain/texture pack currently being downloaded? */
	if (!hasRequest || !String_Equals(&identifier, &texPack)) {
		if (s->status.textures[0].ID) {
			Chat_Status[0].length = 0;
			TextGroupWidget_Redraw(&s->status, 0);
		}
		s->lastDownloadStatus = Int32_MinValue;
		return;
	}

	if (progress == s->lastDownloadStatus) return;
	s->lastDownloadStatus = progress;
	Chat_Status[0].length = 0;

	if (progress == ASYNC_PROGRESS_MAKING_REQUEST) {
		String_AppendConst(&Chat_Status[0], "&eRetrieving texture pack..");
	} else if (progress == ASYNC_PROGRESS_FETCHING_DATA) {
		String_AppendConst(&Chat_Status[0], "&eDownloading texture pack");
	} else if (progress >= 0 && progress <= 100) {
		String_Format1(&Chat_Status[0], "&eDownloading texture pack (&7%i&e%%)", &progress);
	}
	TextGroupWidget_Redraw(&s->status, 0);
}

static void ChatScreen_ColCodeChanged(void* screen, int code) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	double caretAcc;
	if (Gfx.LostContext) return;

	SpecialInputWidget_UpdateCols(&s->altText);
	TextGroupWidget_RedrawAllWithCol(&s->chat,         code);
	TextGroupWidget_RedrawAllWithCol(&s->status,       code);
	TextGroupWidget_RedrawAllWithCol(&s->bottomRight,  code);
	TextGroupWidget_RedrawAllWithCol(&s->clientStatus, code);

	/* Some servers have plugins that redefine colours constantly */
	/* Preserve caret accumulator so caret blinking stays consistent */
	caretAcc = s->input.base.caretAccumulator;
	InputWidget_UpdateText(&s->input.base);
	s->input.base.caretAccumulator = caretAcc;
}

static void ChatScreen_ChatReceived(void* screen, const String* msg, int type) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (Gfx.LostContext) return;

	if (type == MSG_TYPE_NORMAL) {
		s->chatIndex++;
		if (!Gui_Chatlines) return;
		TextGroupWidget_ShiftUp(&s->chat);
	} else if (type >= MSG_TYPE_STATUS_1 && type <= MSG_TYPE_STATUS_3) {
		/* Status[0] is for texture pack downloading message */
		TextGroupWidget_Redraw(&s->status, 1 + (type - MSG_TYPE_STATUS_1));
	} else if (type >= MSG_TYPE_BOTTOMRIGHT_1 && type <= MSG_TYPE_BOTTOMRIGHT_3) {
		/* Bottom3 is top most line, so need to redraw index 0 */
		TextGroupWidget_Redraw(&s->bottomRight, 2 - (type - MSG_TYPE_BOTTOMRIGHT_1));
	} else if (type == MSG_TYPE_ANNOUNCEMENT) {
		TextWidget_Set(&s->announcement, msg, &s->announcementFont);
	} else if (type >= MSG_TYPE_CLIENTSTATUS_1 && type <= MSG_TYPE_CLIENTSTATUS_2) {
		TextGroupWidget_Redraw(&s->clientStatus, type - MSG_TYPE_CLIENTSTATUS_1);
		ChatScreen_UpdateChatYOffsets(s);
	}
}

static void ChatScreen_DrawCrosshairs(void) {
	static struct Texture tex = { 0, Tex_Rect(0,0,0,0), Tex_UV(0.0f,0.0f, 15/256.0f,15/256.0f) };
	int extent;
	if (!Gui_IconsTex) return;

	extent = (int)(CH_EXTENT * Game_Scale(Window_Height / 480.0f));
	tex.ID = Gui_IconsTex;
	tex.X  = (Window_Width  / 2) - extent;
	tex.Y  = (Window_Height / 2) - extent;

	tex.Width  = extent * 2;
	tex.Height = extent * 2;
	Texture_Render(&tex);
}

static void ChatScreen_DrawChatBackground(struct ChatScreen* s) {
	int usedHeight = TextGroupWidget_UsedHeight(&s->chat);
	int x = s->chat.x;
	int y = s->chat.y + s->chat.height - usedHeight;

	int width  = max(s->clientStatus.width, s->chat.width);
	int height = usedHeight + s->clientStatus.height;

	if (height > 0) {
		PackedCol backCol = PackedCol_Make(0, 0, 0, 127);
		Gfx_Draw2DFlat(x - 5, y - 5, width + 10, height + 10, backCol);
	}
}

static void ChatScreen_DrawChat(struct ChatScreen* s, double delta) {
	struct Texture tex;
	TimeMS now;
	int i, logIdx;

	ChatScreen_UpdateTexpackStatus(s);
	if (!Game_PureClassic) { Elem_Render(&s->status, delta); }
	Elem_Render(&s->bottomRight, delta);
	Elem_Render(&s->clientStatus, delta);

	now = DateTime_CurrentUTC_MS();
	if (s->grabsInput) {
		Elem_Render(&s->chat, delta);
	} else {
		/* Only render recent chat */
		for (i = 0; i < s->chat.lines; i++) {
			tex    = s->chat.textures[i];
			logIdx = s->chatIndex + i;
			if (!tex.ID) continue;

			if (logIdx < 0 || logIdx >= Chat_Log.count) continue;
			if (Chat_LogTime[logIdx] + (10 * 1000) >= now) Texture_Render(&tex);
		}
	}

	/* Destroy announcement texture before even rendering it at all, */
	/* otherwise changing texture pack shows announcement for one frame */
	if (s->announcement.tex.ID && now > Chat_AnnouncementReceived + (5 * 1000)) {
		Elem_TryFree(&s->announcement);
	}
	Elem_Render(&s->announcement, delta);

	if (s->grabsInput) {
		Elem_Render(&s->input.base, delta);
		if (s->altText.active) {
			Elem_Render(&s->altText, delta);
		}

#ifdef CC_BUILD_TOUCH
		if (!Input_TouchMode) return;
		Elem_Render(&s->send,   delta);
		Elem_Render(&s->cancel, delta);
#endif
	}
}

static void ChatScreen_TabEntryAdded(void* screen, int id) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (s->showingList) PlayerListWidget_Add(&s->playerList, id);
}

static void ChatScreen_TabEntryChanged(void* screen, int id) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (s->showingList) PlayerListWidget_Update(&s->playerList, id);
}

static void ChatScreen_TabEntryRemoved(void* screen, int id) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (s->showingList) PlayerListWidget_Remove(&s->playerList, id);
}

static void ChatScreen_ContextLost(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	Font_Free(&s->playerFont);
	ChatScreen_FreeChatFonts(s);
	if (s->showingList) Elem_Free(&s->playerList);

	Elem_TryFree(&s->chat);
	Elem_TryFree(&s->input.base);
	Elem_TryFree(&s->altText);
	Elem_TryFree(&s->status);
	Elem_TryFree(&s->bottomRight);
	Elem_TryFree(&s->clientStatus);
	Elem_TryFree(&s->announcement);

#ifdef CC_BUILD_TOUCH
	Elem_TryFree(&s->send);
	Elem_TryFree(&s->cancel);
#endif
}

static void ChatScreen_RemakePlayerList(struct ChatScreen* s) {
	cc_bool classic = Gui_ClassicTabList || !Server.SupportsExtPlayerList;
	PlayerListWidget_Create(&s->playerList, &s->playerFont, classic);
	s->showingList  = true;
	Widget_Layout(&s->playerList);
}

static void ChatScreen_ContextRecreated(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int size = Drawer2D_BitmappedText ? 16 : 11;
	struct FontDesc font;
	Drawer2D_MakeFont(&s->playerFont, size, FONT_STYLE_NORMAL);
	ChatScreen_ChatUpdateFont(s);

	ChatScreen_Redraw(s);
	ChatScreen_ChatUpdateLayout(s);
	if (s->showingList) ChatScreen_RemakePlayerList(s);

#ifdef CC_BUILD_TOUCH
	if (!Input_TouchMode) return;
	Drawer2D_MakeFont(&font, 16, FONT_STYLE_BOLD);
	ButtonWidget_SetConst(&s->send,   "Send",   &font);
	ButtonWidget_SetConst(&s->cancel, "Cancel", &font);
	Font_Free(&font);
#endif
}

static void ChatScreen_BuildMesh(void* screen) { }

static void ChatScreen_Layout(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;

	if (ChatScreen_ChatUpdateFont(s)) ChatScreen_Redraw(s);
	ChatScreen_ChatUpdateLayout(s);
	if (s->showingList) Widget_Layout(&s->playerList);

#ifdef CC_BUILD_TOUCH
	if (!Input_TouchMode) return;
	Widget_Layout(&s->send);
	Widget_Layout(&s->cancel);
#endif
}

static int ChatScreen_KeyPress(void* screen, char keyChar) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (!s->grabsInput) return false;

	if (s->suppressNextPress) {
		s->suppressNextPress = false;
		return false;
	}

	InputWidget_Append(&s->input.base, keyChar);
	ChatScreen_UpdateChatYOffsets(s);
	return true;
}

static int ChatScreen_TextChanged(void* screen, const String* str) {
#ifdef CC_BUILD_TOUCH
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (!s->grabsInput) return false;

	InputWidget_SetText(&s->input.base, str);
	ChatScreen_UpdateChatYOffsets(s);
#endif
	return true;
}

static int ChatScreen_KeyDown(void* screen, int key) {
	static const String slash = String_FromConst("/");
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int playerListKey   = KeyBinds[KEYBIND_PLAYER_LIST];
	cc_bool handlesList = playerListKey != KEY_TAB || !Gui_TabAutocomplete || !s->grabsInput;

	if (key == playerListKey && handlesList) {
		if (!s->showingList && !Server.IsSinglePlayer) {
			ChatScreen_RemakePlayerList(s);
		}
		return true;
	}

	s->suppressNextPress = false;
	/* Handle chat text input */
	if (s->grabsInput) {
#ifdef CC_BUILD_WEB
		/* See reason for this in HandleInputUp */
		if (key == KeyBinds[KEYBIND_SEND_CHAT] || key == KEY_KP_ENTER) {
			ChatScreen_EnterChatInput(s, false);
#else
		if (key == KeyBinds[KEYBIND_SEND_CHAT] || key == KEY_KP_ENTER || key == KEY_ESCAPE) {
			ChatScreen_EnterChatInput(s, key == KEY_ESCAPE);
#endif
		} else if (key == KEY_PAGEUP) {
			ChatScreen_ScrollChatBy(s, -Gui_Chatlines);
		} else if (key == KEY_PAGEDOWN) {
			ChatScreen_ScrollChatBy(s, +Gui_Chatlines);
		} else {
			Elem_HandlesKeyDown(&s->input.base, key);
			ChatScreen_UpdateChatYOffsets(s);
		}
		return key < KEY_F1 || key > KEY_F35;
	}

	if (key == KeyBinds[KEYBIND_CHAT]) {
		ChatScreen_OpenInput(&String_Empty);
	} else if (key == KEY_SLASH) {
		ChatScreen_OpenInput(&slash);
	} else if (key == KeyBinds[KEYBIND_INVENTORY]) {
		InventoryScreen_Show();
	} else {
		return false;
	}
	return true;
}

static int ChatScreen_KeyUp(void* screen, int key) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (key == KeyBinds[KEYBIND_PLAYER_LIST] && s->showingList) {
		s->showingList = false;
		Elem_Free(&s->playerList);
		return true;
	}

	if (!s->grabsInput) return false;
#ifdef CC_BUILD_WEB
	/* See reason for this in HandleInputUp */
	if (key == KEY_ESCAPE) ChatScreen_EnterChatInput(s, true);
#endif

	if (Server.SupportsFullCP437 && key == KeyBinds[KEYBIND_EXT_INPUT]) {
		if (!Window_Focused) return true;
		SpecialInputWidget_SetActive(&s->altText, !s->altText.active);
		ChatScreen_UpdateChatYOffsets(s);
	}
	return true;
}

static int ChatScreen_MouseScroll(void* screen, float delta) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int steps;
	if (!s->grabsInput) return false;

	steps = Utils_AccumulateWheelDelta(&s->chatAcc, delta);
	ChatScreen_ScrollChatBy(s, -steps);
	return true;
}

static int ChatScreen_PointerDown(void* screen, int id, int x, int y) {
	String text; char textBuffer[STRING_SIZE * 4];
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int height, chatY;

	if (!s->grabsInput) return false;

	/* player clicks on name in tab list */
	/* TODO: Move to PlayerListWidget */
	if (s->showingList) {
		String_InitArray(text, textBuffer);
		PlayerListWidget_GetNameAt(&s->playerList, x, y, &text);

		if (text.length) {
			String_Append(&text, ' ');
			ChatScreen_AppendInput(&text);
			return true;
		}
	}

	if (Game_HideGui) return false;

#ifdef CC_BUILD_TOUCH
	if (Widget_Contains(&s->send, x, y)) {
		ChatScreen_EnterChatInput(s, false); return true;
	}
	if (Widget_Contains(&s->cancel, x, y)) {
		ChatScreen_EnterChatInput(s, true); return true;
	}
#endif

	if (!Widget_Contains(&s->chat, x, y)) {
		if (s->altText.active && Widget_Contains(&s->altText, x, y)) {
			Elem_HandlesPointerDown(&s->altText, id, x, y);
			ChatScreen_UpdateChatYOffsets(s);
			return true;
		}
		Elem_HandlesPointerDown(&s->input.base, id, x, y);
		return true;
	}

	height = TextGroupWidget_UsedHeight(&s->chat);
	chatY  = s->chat.y + s->chat.height - height;
	if (!Gui_Contains(s->chat.x, chatY, s->chat.width, height, x, y)) return false;

	String_InitArray(text, textBuffer);
	TextGroupWidget_GetSelected(&s->chat, &text, x, y);
	if (!text.length) return false;

	if (Utils_IsUrlPrefix(&text)) {
		UrlWarningOverlay_Show(&text);
	} else if (Gui_ClickableChat) {
		ChatScreen_AppendInput(&text);
	}
	return true;
}

static void ChatScreen_Init(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	ChatInputWidget_Create(&s->input);
	SpecialInputWidget_Create(&s->altText, &s->chatFont, &s->input.base);

	TextGroupWidget_Create(&s->status, CHAT_MAX_STATUS,
							s->statusTextures, ChatScreen_GetStatus);
	TextGroupWidget_Create(&s->bottomRight, CHAT_MAX_BOTTOMRIGHT, 
							s->bottomRightTextures, ChatScreen_GetBottomRight);
	TextGroupWidget_Create(&s->chat, Gui_Chatlines,
							s->chatTextures, ChatScreen_GetChat);
	TextGroupWidget_Create(&s->clientStatus, CHAT_MAX_CLIENTSTATUS,
							s->clientStatusTextures, ChatScreen_GetClientStatus);
	TextWidget_Make(&s->announcement, ANCHOR_CENTRE, ANCHOR_CENTRE, 0, -Window_Height / 4);

	s->status.collapsible[0]       = true; /* Texture pack download status */
	s->clientStatus.collapsible[0] = true;
	s->clientStatus.collapsible[1] = true;

	s->chat.underlineUrls = !Game_ClassicMode;
	s->chatIndex = Chat_Log.count - Gui_Chatlines;

	Event_RegisterChat(&ChatEvents.ChatReceived,  s, ChatScreen_ChatReceived);
	Event_RegisterInt(&ChatEvents.ColCodeChanged, s, ChatScreen_ColCodeChanged);
	Event_RegisterInt(&TabListEvents.Added,       s, ChatScreen_TabEntryAdded);
	Event_RegisterInt(&TabListEvents.Changed,     s, ChatScreen_TabEntryChanged);
	Event_RegisterInt(&TabListEvents.Removed,     s, ChatScreen_TabEntryRemoved);

#ifdef CC_BUILD_TOUCH
	if (!Input_TouchMode) return;
	ButtonWidget_Make(&s->send,   100, NULL, ANCHOR_MAX, ANCHOR_MIN, 10, 10);
	ButtonWidget_Make(&s->cancel, 100, NULL, ANCHOR_MAX, ANCHOR_MIN, 10, 60);
#endif
}

static void ChatScreen_Render(void* screen, double delta) {
	struct ChatScreen* s = (struct ChatScreen*)screen;

	if (Game_HideGui && s->grabsInput) {
		Gfx_SetTexturing(true);
		Elem_Render(&s->input.base, delta);
		Gfx_SetTexturing(false);
	}
	if (Game_HideGui) return;

	if (!s->showingList && !Gui_GetBlocksWorld()) {
		Gfx_SetTexturing(true);
		ChatScreen_DrawCrosshairs();
		Gfx_SetTexturing(false);
	}
	if (s->grabsInput && !Game_PureClassic) {
		ChatScreen_DrawChatBackground(s);
	}

	Gfx_SetTexturing(true);
	ChatScreen_DrawChat(s, delta);

	if (s->showingList && IsOnlyHudActive()) {
		s->playerList.active = s->grabsInput;
		Elem_Render(&s->playerList, delta);
		/* NOTE: Should usually be caught by KeyUp, but just in case. */
		if (!KeyBind_IsPressed(KEYBIND_PLAYER_LIST)) {
			s->showingList = false;
			Elem_Free(&s->playerList);
		}
	}
	Gfx_SetTexturing(false);
}

static void ChatScreen_Free(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	s->showingList = false;

	Event_UnregisterChat(&ChatEvents.ChatReceived,  s, ChatScreen_ChatReceived);
	Event_UnregisterInt(&ChatEvents.ColCodeChanged, s, ChatScreen_ColCodeChanged);
	Event_UnregisterInt(&TabListEvents.Added,       s, ChatScreen_TabEntryAdded);
	Event_UnregisterInt(&TabListEvents.Changed,     s, ChatScreen_TabEntryChanged);
	Event_UnregisterInt(&TabListEvents.Removed,     s, ChatScreen_TabEntryRemoved);
}

static const struct ScreenVTABLE ChatScreen_VTABLE = {
	ChatScreen_Init,        Screen_NullUpdate, ChatScreen_Free,    
	ChatScreen_Render,      ChatScreen_BuildMesh,
	ChatScreen_KeyDown,     ChatScreen_KeyUp,  ChatScreen_KeyPress, ChatScreen_TextChanged,
	ChatScreen_PointerDown, Screen_FPointer,   Screen_FPointer,     ChatScreen_MouseScroll,
	ChatScreen_Layout, ChatScreen_ContextLost, ChatScreen_ContextRecreated
};
void ChatScreen_Show(void) {
	struct ChatScreen* s   = &ChatScreen_Instance;
	s->lastDownloadStatus = Int32_MinValue;

	s->VTABLE = &ChatScreen_VTABLE;
	Gui_Chat  = s;
	Gui_Replace((struct Screen*)s, GUI_PRIORITY_CHAT);
}

void ChatScreen_OpenInput(const String* text) {
	struct ChatScreen* s = &ChatScreen_Instance;
	s->suppressNextPress = true;
	s->grabsInput        = true;
	Camera_CheckFocus();
	Window_OpenKeyboard();
	Window_SetKeyboardText(text);

	String_Copy(&s->input.base.text, text);
	InputWidget_UpdateText(&s->input.base);
}

void ChatScreen_AppendInput(const String* text) {
	struct ChatScreen* s = &ChatScreen_Instance;
	InputWidget_AppendText(&s->input.base, text);
	ChatScreen_UpdateChatYOffsets(s);
}

void ChatScreen_SetChatlines(int lines) {
	struct ChatScreen* s = &ChatScreen_Instance;
	Elem_Free(&s->chat);
	s->chatIndex += s->chat.lines - lines;
	s->chat.lines = lines;
	TextGroupWidget_RedrawAll(&s->chat);
}


/*########################################################################################################################*
*-----------------------------------------------------InventoryScreen-----------------------------------------------------*
*#########################################################################################################################*/
static struct InventoryScreen {
	Screen_Body
	struct FontDesc font;
	struct TableWidget table;
	cc_bool releasedInv, deferredSelect;
} InventoryScreen_Instance;

static void InventoryScreen_OnBlockChanged(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	TableWidget_OnInventoryChanged(&s->table);
}

static void InventoryScreen_ContextLost(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Font_Free(&s->font);
	Elem_TryFree(&s->table);
}

static void InventoryScreen_ContextRecreated(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Drawer2D_MakeFont(&s->font, 16, FONT_STYLE_NORMAL);
	TableWidget_Recreate(&s->table);
}

static void InventoryScreen_BuildMesh(void* screen) { }

static void InventoryScreen_MoveToSelected(struct InventoryScreen* s) {
	struct TableWidget* table = &s->table;
	TableWidget_SetBlockTo(table, Inventory_SelectedBlock);
	TableWidget_Recreate(table);

	s->deferredSelect = false;
	/* User is holding invalid block */
	if (table->selectedIndex == -1) {
		TableWidget_MakeDescTex(table, Inventory_SelectedBlock);
	}
}

static void InventoryScreen_Init(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	
	TableWidget_Create(&s->table);
	s->table.font         = &s->font;
	s->table.blocksPerRow = Game_PureClassic ? 9 : 10;
	TableWidget_RecreateBlocks(&s->table);

	/* Can't immediately move to selected here, because cursor grabbed  */
	/* status might be toggled after InventoryScreen_Init() is called. */
	/* That causes the cursor to be moved back to the middle of the window. */
	s->deferredSelect = true;

	Event_RegisterVoid(&BlockEvents.PermissionsChanged, s, InventoryScreen_OnBlockChanged);
	Event_RegisterVoid(&BlockEvents.BlockDefChanged,    s, InventoryScreen_OnBlockChanged);
}

static void InventoryScreen_Render(void* screen, double delta) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	if (s->deferredSelect) InventoryScreen_MoveToSelected(s);
	Elem_Render(&s->table, delta);
}

static void InventoryScreen_Layout(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Widget_Layout(&s->table);
}

static void InventoryScreen_Free(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Event_UnregisterVoid(&BlockEvents.PermissionsChanged, s, InventoryScreen_OnBlockChanged);
	Event_UnregisterVoid(&BlockEvents.BlockDefChanged,    s, InventoryScreen_OnBlockChanged);
}

static int InventoryScreen_KeyDown(void* screen, int key) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	struct TableWidget* table = &s->table;

	if (key == KeyBinds[KEYBIND_INVENTORY] && s->releasedInv) {
		Gui_Remove((struct Screen*)s);
	} else if (key == KEY_ENTER && table->selectedIndex != -1) {
		Inventory_SetSelectedBlock(table->blocks[table->selectedIndex]);
		Gui_Remove((struct Screen*)s);
	} else if (Elem_HandlesKeyDown(table, key)) {
	} else {
		return Elem_HandlesKeyDown(&Gui_HUD->hotbar, key);
	}
	return true;
}

static int InventoryScreen_KeyUp(void* screen, int key) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;

	if (key == KeyBinds[KEYBIND_INVENTORY]) {
		s->releasedInv = true; return true;
	}
	return Elem_HandlesKeyUp(&Gui_HUD->hotbar, key);
}

static int InventoryScreen_PointerDown(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	struct TableWidget* table = &s->table;
	cc_bool handled, hotbar;

	if (table->scroll.draggingId == id) return true;
	if (HUDscreen_PointerDown(Gui_HUD, id, x, y)) return true;
	handled = Elem_HandlesPointerDown(table, id, x, y);

	if (!handled || table->pendingClose) {
		hotbar = Key_IsControlPressed() || Key_IsShiftPressed();
		if (!hotbar) Gui_Remove(screen);
	}
	return true;
}

static int InventoryScreen_PointerUp(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	return Elem_HandlesPointerUp(&s->table, id, x, y);
}

static int InventoryScreen_PointerMove(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	return Elem_HandlesPointerMove(&s->table, id, x, y);
}

static int InventoryScreen_MouseScroll(void* screen, float delta) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;

	cc_bool hotbar = Key_IsAltPressed() || Key_IsControlPressed() || Key_IsShiftPressed();
	if (hotbar) return false;
	return Elem_HandlesMouseScroll(&s->table, delta);
}

static const struct ScreenVTABLE InventoryScreen_VTABLE = {
	InventoryScreen_Init,        Screen_NullUpdate,         InventoryScreen_Free, 
	InventoryScreen_Render,      InventoryScreen_BuildMesh,
	InventoryScreen_KeyDown,     InventoryScreen_KeyUp,     Screen_TKeyPress,            Screen_TText,
	InventoryScreen_PointerDown, InventoryScreen_PointerUp, InventoryScreen_PointerMove, InventoryScreen_MouseScroll,
	InventoryScreen_Layout,  InventoryScreen_ContextLost, InventoryScreen_ContextRecreated
};
void InventoryScreen_Show(void) {
	struct InventoryScreen* s = &InventoryScreen_Instance;
	s->grabsInput = true;
	s->closable   = true;

	s->VTABLE = &InventoryScreen_VTABLE;
	Gui_Replace((struct Screen*)s, GUI_PRIORITY_INVENTORY);
}


/*########################################################################################################################*
*------------------------------------------------------LoadingScreen------------------------------------------------------*
*#########################################################################################################################*/
static struct LoadingScreen {
	Screen_Body
	struct FontDesc font;
	float progress;
	
	struct TextWidget title, message;
	String titleStr, messageStr;
	const char* lastState;

	char _titleBuffer[STRING_SIZE];
	char _messageBuffer[STRING_SIZE];
} LoadingScreen;

static void LoadingScreen_SetTitle(struct LoadingScreen* s) {
	TextWidget_Set(&s->title, &s->titleStr, &s->font);
}
static void LoadingScreen_SetMessage(struct LoadingScreen* s) {
	TextWidget_Set(&s->message, &s->messageStr, &s->font);
}

static void LoadingScreen_MapLoading(void* screen, float progress) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	s->progress = progress;
}

static void LoadingScreen_Layout(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	if (!s->title.VTABLE) return;
	Widget_Layout(&s->title);
	Widget_Layout(&s->message);
}

static void LoadingScreen_ContextLost(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Font_Free(&s->font);
	if (!s->title.VTABLE) return;

	Elem_Free(&s->title);
	Elem_Free(&s->message);
}

static void LoadingScreen_ContextRecreated(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Drawer2D_MakeFont(&s->font, 16, FONT_STYLE_NORMAL);
	LoadingScreen_SetTitle(s);
	LoadingScreen_SetMessage(s);
}

static void LoadingScreen_BuildMesh(void* screen) { }

static void LoadingScreen_UpdateBackgroundVB(VertexP3fT2fC4b* vertices, int count, int atlasIndex, cc_bool* bound) {
	if (!(*bound)) {
		*bound = true;
		Gfx_BindTexture(Atlas1D.TexIds[atlasIndex]);
	}

	Gfx_SetVertexFormat(VERTEX_FORMAT_P3FT2FC4B);
	/* TODO: Do we need to use a separate VB here? */
	Gfx_UpdateDynamicVb_IndexedTris(Models.Vb, vertices, count);
}

#define LOADING_TILE_SIZE 64
static void LoadingScreen_DrawBackground(void) {
	VertexP3fT2fC4b vertices[144];
	VertexP3fT2fC4b* ptr = vertices;
	PackedCol col = PackedCol_Make(64, 64, 64, 255);

	struct Texture tex;
	TextureLoc loc;
	int count = 0, atlasIndex, y;
	cc_bool bound = false;

	loc    = Block_Tex(BLOCK_DIRT, FACE_YMAX);
	tex.ID = 0;
	Tex_SetRect(tex, 0,0, Window_Width,LOADING_TILE_SIZE);
	tex.uv    = Atlas1D_TexRec(loc, 1, &atlasIndex);
	tex.uv.U2 = (float)Window_Width / LOADING_TILE_SIZE;
	
	for (y = 0; y < Window_Height; y += LOADING_TILE_SIZE) {
		tex.Y = y;
		Gfx_Make2DQuad(&tex, col, &ptr);
		count += 4;

		if (count < Array_Elems(vertices)) continue;
		LoadingScreen_UpdateBackgroundVB(vertices, count, atlasIndex, &bound);
		count = 0;
		ptr = vertices;
	}

	if (!count) return;
	LoadingScreen_UpdateBackgroundVB(vertices, count, atlasIndex, &bound);
}

static void LoadingScreen_Init(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;

	TextWidget_Make(&s->title,   ANCHOR_CENTRE, ANCHOR_CENTRE, 0, -31);
	TextWidget_Make(&s->message, ANCHOR_CENTRE, ANCHOR_CENTRE, 0,  17);

	Gfx_SetFog(false);
	Event_RegisterFloat(&WorldEvents.Loading, s, LoadingScreen_MapLoading);
}

#define PROG_BAR_WIDTH 200
#define PROG_BAR_HEIGHT 4
static void LoadingScreen_Render(void* screen, double delta) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	PackedCol backCol = PackedCol_Make(128, 128, 128, 255);
	PackedCol progCol = PackedCol_Make(128, 255, 128, 255);
	int progWidth;
	int x, y;

	Gfx_SetTexturing(true);
	LoadingScreen_DrawBackground();

	Elem_Render(&s->title,   delta);
	Elem_Render(&s->message, delta);
	Gfx_SetTexturing(false);

	x = Gui_CalcPos(ANCHOR_CENTRE,  0, PROG_BAR_WIDTH,  Window_Width);
	y = Gui_CalcPos(ANCHOR_CENTRE, 34, PROG_BAR_HEIGHT, Window_Height);
	progWidth = (int)(PROG_BAR_WIDTH * s->progress);

	Gfx_Draw2DFlat(x, y, PROG_BAR_WIDTH, PROG_BAR_HEIGHT, backCol);
	Gfx_Draw2DFlat(x, y, progWidth,      PROG_BAR_HEIGHT, progCol);
}

static void LoadingScreen_Free(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Event_UnregisterFloat(&WorldEvents.Loading, s, LoadingScreen_MapLoading);
}

CC_NOINLINE static void LoadingScreen_ShowCommon(const String* title, const String* message) {
	struct LoadingScreen* s = &LoadingScreen;
	s->lastState = NULL;
	s->progress  = 0.0f;

	String_InitArray(s->titleStr,   s->_titleBuffer);
	String_AppendString(&s->titleStr,   title);
	String_InitArray(s->messageStr, s->_messageBuffer);
	String_AppendString(&s->messageStr, message);
	
	s->grabsInput  = true;
	s->blocksWorld = true;
	Gui_Replace((struct Screen*)s, 
		Game_ClassicMode ? GUI_PRIORITY_OLDLOADING : GUI_PRIORITY_LOADING);
}

static const struct ScreenVTABLE LoadingScreen_VTABLE = {
	LoadingScreen_Init,   Screen_NullUpdate, LoadingScreen_Free, 
	LoadingScreen_Render, LoadingScreen_BuildMesh,
	Screen_TInput,        Screen_TInput,     Screen_TKeyPress,   Screen_TText,
	Screen_TPointer,      Screen_TPointer,   Screen_TPointer,    Screen_TMouseScroll,
	LoadingScreen_Layout, LoadingScreen_ContextLost, LoadingScreen_ContextRecreated
};
void LoadingScreen_Show(const String* title, const String* message) {
	LoadingScreen.VTABLE = &LoadingScreen_VTABLE;
	LoadingScreen_ShowCommon(title, message);
}
struct Screen* LoadingScreen_UNSAFE_RawPointer = (struct Screen*)&LoadingScreen;


/*########################################################################################################################*
*--------------------------------------------------GeneratingMapScreen----------------------------------------------------*
*#########################################################################################################################*/
static void GeneratingScreen_Init(void* screen) {
	Gen_Done = false;
	LoadingScreen_Init(screen);
	Event_RaiseVoid(&WorldEvents.NewMap);

	Gen_Blocks = (BlockRaw*)Mem_TryAlloc(World.Volume, 1);
	if (!Gen_Blocks) {
		Window_ShowDialog("Out of memory", "Not enough free memory to generate a map that large.\nTry a smaller size.");
		Gen_Done = true;
	} else if (Gen_Vanilla) {
		Thread_Start(NotchyGen_Generate, true);
	} else {
		Thread_Start(FlatgrassGen_Generate, true);
	}
}

static void GeneratingScreen_EndGeneration(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct LocationUpdate update;
	float x, z;

	Gui_Remove((struct Screen*)&LoadingScreen);
	Gen_Done = false;

	if (!Gen_Blocks) { Chat_AddRaw("&cFailed to generate the map."); return; }
	World_SetNewMap(Gen_Blocks, World.Width, World.Height, World.Length);
	Gen_Blocks = NULL;

	x = (World.Width / 2) + 0.5f; z = (World.Length / 2) + 0.5f;
	p->Spawn = Respawn_FindSpawnPosition(x, z, p->Base.Size);

	LocationUpdate_MakePosAndOri(&update, p->Spawn, 0.0f, 0.0f, false);
	p->Base.VTABLE->SetLocation(&p->Base, &update, false);

	Camera.CurrentPos = Camera.Active->GetPosition(0.0f);
	Event_RaiseVoid(&WorldEvents.MapLoaded);
}

static void GeneratingScreen_Render(void* screen, double delta) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	const volatile char* state;

	LoadingScreen_Render(s, delta);
	if (Gen_Done) { GeneratingScreen_EndGeneration(); return; }

	state       = Gen_CurrentState;
	s->progress = Gen_CurrentProgress;
	if (state == s->lastState) return;

	s->messageStr.length = 0;
	String_AppendConst(&s->messageStr, (const char*)state);
	LoadingScreen_SetMessage(s);
}

static const struct ScreenVTABLE GeneratingScreen_VTABLE = {
	GeneratingScreen_Init,   Screen_NullUpdate, LoadingScreen_Free,
	GeneratingScreen_Render, LoadingScreen_BuildMesh,
	Screen_TInput,           Screen_TInput,     Screen_TKeyPress,   Screen_TText,
	Screen_TPointer,         Screen_TPointer,   Screen_FPointer,    Screen_TMouseScroll,
	LoadingScreen_Layout, LoadingScreen_ContextLost, LoadingScreen_ContextRecreated
};
void GeneratingScreen_Show(void) {
	static const String title   = String_FromConst("Generating level");
	static const String message = String_FromConst("Generating..");

	LoadingScreen.VTABLE = &GeneratingScreen_VTABLE;
	LoadingScreen_ShowCommon(&title, &message);
}


/*########################################################################################################################*
*----------------------------------------------------DisconnectScreen-----------------------------------------------------*
*#########################################################################################################################*/
static struct DisconnectScreen {
	Screen_Body
	TimeMS initTime;
	cc_bool canReconnect, lastActive;
	int lastSecsLeft;
	struct ButtonWidget reconnect;

	struct FontDesc titleFont, messageFont;
	struct TextWidget title, message;
	char _titleBuffer[STRING_SIZE];
	char _messageBuffer[STRING_SIZE];
	String titleStr, messageStr;
} DisconnectScreen;

static struct Widget* disconnect_widgets[3] = {
	(struct Widget*)&DisconnectScreen.title, 
	(struct Widget*)&DisconnectScreen.message,
	(struct Widget*)&DisconnectScreen.reconnect
};
#define DISCONNECT_MAX_VERTICES (2 * TEXTWIDGET_MAX + BUTTONWIDGET_MAX)
#define DISCONNECT_DELAY_MS 5000

static void DisconnectScreen_ReconnectMessage(struct DisconnectScreen* s, String* msg) {
	if (s->canReconnect) {
		int elapsedMS = (int)(DateTime_CurrentUTC_MS() - s->initTime);
		int secsLeft = (DISCONNECT_DELAY_MS - elapsedMS) / MILLIS_PER_SEC;

		if (secsLeft > 0) {
			String_Format1(msg, "Reconnect in %i", &secsLeft);
		} else {
			String_AppendConst(msg, "Reconnect");
		}
	}
}

static void DisconnectScreen_UpdateDelayLeft(struct DisconnectScreen* s, double delta) {
	String msg; char msgBuffer[STRING_SIZE];
	int elapsedMS, secsLeft;

	elapsedMS = (int)(DateTime_CurrentUTC_MS() - s->initTime);
	secsLeft  = (DISCONNECT_DELAY_MS - elapsedMS) / MILLIS_PER_SEC;
	if (secsLeft < 0) secsLeft = 0;
	if (s->lastSecsLeft == secsLeft && s->reconnect.active == s->lastActive) return;

	String_InitArray(msg, msgBuffer);
	DisconnectScreen_ReconnectMessage(s, &msg);
	ButtonWidget_Set(&s->reconnect, &msg, &s->titleFont);

	s->reconnect.disabled = secsLeft != 0;
	s->lastSecsLeft = secsLeft;
	s->lastActive   = s->reconnect.active;
	s->dirty        = true;
}

static void DisconnectScreen_ContextLost(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	Font_Free(&s->titleFont);
	Font_Free(&s->messageFont);
	Screen_ContextLost(screen);
}

static void DisconnectScreen_ContextRecreated(void* screen) {
	String msg; char msgBuffer[STRING_SIZE];
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	Screen_CreateVb(screen);

	Drawer2D_MakeFont(&s->titleFont,   16, FONT_STYLE_BOLD);
	Drawer2D_MakeFont(&s->messageFont, 16, FONT_STYLE_NORMAL);
	TextWidget_Set(&s->title,   &s->titleStr,   &s->titleFont);
	TextWidget_Set(&s->message, &s->messageStr, &s->messageFont);

	String_InitArray(msg, msgBuffer);
	DisconnectScreen_ReconnectMessage(s, &msg);
	ButtonWidget_Set(&s->reconnect, &msg, &s->titleFont);
}

static void DisconnectScreen_Init(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;

	TextWidget_Make(&s->title,   ANCHOR_CENTRE, ANCHOR_CENTRE, 0, -30);
	TextWidget_Make(&s->message, ANCHOR_CENTRE, ANCHOR_CENTRE, 0,  10);

	ButtonWidget_Make(&s->reconnect, 300, NULL, 
					ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 80);
	s->reconnect.disabled = !s->canReconnect;
	s->maxVertices  = DISCONNECT_MAX_VERTICES;

	/* NOTE: changing VSync can't be done within frame, causes crash on some GPUs */
	Gfx_SetFpsLimit(Game_FpsLimit == FPS_LIMIT_VSYNC, 1000 / 5.0f);

	s->initTime     = DateTime_CurrentUTC_MS();
	s->lastSecsLeft = DISCONNECT_DELAY_MS / MILLIS_PER_SEC;
	s->widgets      = disconnect_widgets;
	s->numWidgets   = s->canReconnect ? 3 : 2;
}

static void DisconnectScreen_Update(void* screen, double delta) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	if (s->canReconnect) DisconnectScreen_UpdateDelayLeft(s, delta);
}

static void DisconnectScreen_Render(void* screen, double delta) {
	PackedCol top    = PackedCol_Make(64, 32, 32, 255);
	PackedCol bottom = PackedCol_Make(80, 16, 16, 255);
	Gfx_Draw2DGradient(0, 0, Window_Width, Window_Height, top, bottom);

	Gfx_SetTexturing(true);
	Screen_Render2Widgets(screen, delta);
	Gfx_SetTexturing(false);
}

static void DisconnectScreen_Free(void* screen) { Game_SetFpsLimit(Game_FpsLimit); }

static int DisconnectScreen_PointerDown(void* screen, int id, int x, int y) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	struct ButtonWidget* w     = &s->reconnect;

	if (!w->disabled && Widget_Contains(w, x, y)) {
		Gui_Remove((struct Screen*)s);
		Gui_ShowDefault();
		Server.BeginConnect();
	}
	return true;
}

static int DisconnectScreen_PointerMove(void* screen, int idx, int x, int y) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	struct ButtonWidget* w     = &s->reconnect;

	w->active = !w->disabled && Widget_Contains(w, x, y);
	return true;
}

static const struct ScreenVTABLE DisconnectScreen_VTABLE = {
	DisconnectScreen_Init,        DisconnectScreen_Update, DisconnectScreen_Free,
	DisconnectScreen_Render,      Screen_BuildMesh,
	Screen_InputDown,             Screen_TInput,           Screen_TKeyPress,             Screen_TText,
	DisconnectScreen_PointerDown, Screen_TPointer,         DisconnectScreen_PointerMove, Screen_TMouseScroll,
	Screen_Layout,                DisconnectScreen_ContextLost, DisconnectScreen_ContextRecreated
};
void DisconnectScreen_Show(const String* title, const String* message) {
	static const String kick = String_FromConst("Kicked ");
	static const String ban  = String_FromConst("Banned ");
	String why; char whyBuffer[STRING_SIZE];
	struct DisconnectScreen* s = &DisconnectScreen;

	s->grabsInput  = true;
	s->blocksWorld = true;

	String_InitArray(s->titleStr,   s->_titleBuffer);
	String_AppendString(&s->titleStr,   title);
	String_InitArray(s->messageStr, s->_messageBuffer);
	String_AppendString(&s->messageStr, message);

	String_InitArray(why, whyBuffer);
	String_AppendColorless(&why, message);
	
	s->canReconnect = !(String_CaselessStarts(&why, &kick) || String_CaselessStarts(&why, &ban));
	s->VTABLE       = &DisconnectScreen_VTABLE;

	/* Get rid of all other menus instead of just hiding to reduce GPU usage */
	while (Gui_ScreensCount) Gui_Remove(Gui_Screens[0]);
	Gui_Replace((struct Screen*)s, GUI_PRIORITY_DISCONNECT);
}


/*########################################################################################################################*
*--------------------------------------------------------TouchScreen------------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_TOUCH
static struct TouchScreen {
	Screen_Body
	cc_uint8 binds[7];
	struct FontDesc font;
	struct ButtonWidget btns[7];
} TouchScreen;

static struct Widget* touch_widgets[7] = {
	(struct Widget*)&TouchScreen.btns[0], (struct Widget*)&TouchScreen.btns[1],
	(struct Widget*)&TouchScreen.btns[2], (struct Widget*)&TouchScreen.btns[3],
	(struct Widget*)&TouchScreen.btns[4], (struct Widget*)&TouchScreen.btns[5],
	(struct Widget*)&TouchScreen.btns[6],
};
#define TOUCH_MAX_VERTICES (7 * BUTTONWIDGET_MAX)

static const struct TouchBindDesc {
	const char* text;
	cc_uint8 bind, width;
	cc_int16 xOffset, yOffset;
} touchDescs[7] = {
	{ "<",    KEYBIND_LEFT,     40,  10,  90 },
	{ ">",    KEYBIND_RIGHT,    40, 150,  90 },
	{ "^",    KEYBIND_FORWARD,  40,  80,  50 },
	{ "\\/",  KEYBIND_BACK,     40,  80, 130 },
	{ "Jump", KEYBIND_JUMP,    100,  50,  50 },
	{ "",     KEYBIND_COUNT,   100,  50,  90 },
	{ "More", KEYBIND_COUNT,   100,  50, 130 },
};

static void TouchScreen_ContextLost(void* screen) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	Font_Free(&s->font);
	Screen_ContextLost(screen);
}

static void TouchScreen_UpdateModeText(void* screen) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	ButtonWidget_SetConst(&s->btns[5], Input_Placing ? "Place" : "Delete", &s->font);
}

static void TouchScreen_ModeClick(void* s, void* w) { 
	Input_Placing = !Input_Placing; 
	TouchScreen_UpdateModeText(s);
}
static void TouchScreen_MoreClick(void* s, void* w) { TouchMoreScreen_Show(); }

static void TouchScreen_ContextRecreated(void* screen) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	const struct TouchBindDesc* desc;
	int i;
	Screen_CreateVb(screen);
	Drawer2D_MakeFont(&s->font, 16, FONT_STYLE_BOLD);

	for (i = 0; i < s->numWidgets; i++) {
		desc = &touchDescs[i];
		ButtonWidget_SetConst(&s->btns[i], desc->text, &s->font);
	}
	TouchScreen_UpdateModeText(s);
	/* TODO: this is pretty nasty hacky. rewrite! */
}

static void TouchScreen_Render(void* screen, double delta) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	if (Gui_GetInputGrab()) return;

	Gfx_SetTexturing(true);
	Screen_Render2Widgets(screen, delta);
	Gfx_SetTexturing(false);
}

static int TouchScreen_PointerDown(void* screen, int id, int x, int y) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	int i;
	//Chat_Add1("POINTER DOWN: %i", &id);
	if (Gui_GetInputGrab()) return false;

	for (i = 0; i < s->numWidgets; i++) {
		if (!Widget_Contains(&s->btns[i], x, y)) continue;

		if (s->binds[i] < KEYBIND_COUNT) {
			Input_SetPressed(KeyBinds[s->binds[i]], true);
		} else {
			s->btns[i].MenuClick(screen, &s->btns[i]);
		}
		s->btns[i].active |= id;
		return true;
	}
	return false;
}

static int TouchScreen_PointerUp(void* screen, int id, int x, int y) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	int i;
	//Chat_Add1("POINTER UP: %i", &id);

	for (i = 0; i < s->numWidgets; i++) {
		if (!(s->btns[i].active & id)) continue;

		if (s->binds[i] < KEYBIND_COUNT) {
			Input_SetPressed(KeyBinds[s->binds[i]], false);
		}
		s->btns[i].active &= ~id;
		return true;
	}
	return false;
}

static void TouchScreen_Init(void* screen) {
	struct TouchScreen* s = (struct TouchScreen*)screen;
	const struct TouchBindDesc* desc;
	int i;

	s->widgets     = touch_widgets;
	s->numWidgets  = Array_Elems(touch_widgets);
	s->maxVertices = TOUCH_MAX_VERTICES;

	for (i = 0; i < s->numWidgets; i++) {
		desc = &touchDescs[i];
		ButtonWidget_Make(&s->btns[i], desc->width, NULL, i < 4 ? ANCHOR_MIN : ANCHOR_MAX, 
			ANCHOR_MIN, desc->xOffset, desc->yOffset);
		s->binds[i] = desc->bind;
	}

	s->btns[5].MenuClick = TouchScreen_ModeClick;
	s->btns[6].MenuClick = TouchScreen_MoreClick;
}

static const struct ScreenVTABLE TouchScreen_VTABLE = {
	TouchScreen_Init,        Screen_NullUpdate,     Screen_NullFunc,
	TouchScreen_Render,      Screen_BuildMesh,
	Screen_FInput,           Screen_FInput,         Screen_FKeyPress, Screen_FText,
	TouchScreen_PointerDown, TouchScreen_PointerUp, Screen_FPointer,  Screen_FMouseScroll,
	Screen_Layout,           TouchScreen_ContextLost, TouchScreen_ContextRecreated
};
void TouchScreen_Show(void) {
	struct TouchScreen* s = &TouchScreen;
	s->VTABLE = &TouchScreen_VTABLE;

	if (!Input_TouchMode) return;
	Gui_Replace((struct Screen*)s, GUI_PRIORITY_TOUCH);
}
#endif
