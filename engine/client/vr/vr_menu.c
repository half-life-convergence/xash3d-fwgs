/*
vr_menu.c - Half-Life: Convergence VR pause menu
Copyright (C) 2026 Half-Life: Convergence contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
The pause menu is a panel that floats in the room in front of the player,
its own OpenXR quad layer (vr_openxr.c owns the swapchain, draws into it
through VR_MenuDraw and submits it over the eyes). The controller's menu
button opens it in a game instead of the 2D menu; the game pauses (through
VR_PausesGame) and the world dims behind it.

It looks like Half-Life's weapon selection: everything is drawn additively
in the HUD's orange, the buttons are translucent orange boxes like the
weapon buckets, the one pointed at gets the HUD's selection frame, the
slot numbers beside them are the HUD's bucket sprites, and the text is the
HUD font (creditsfont) at whole-number scales. The quad is submitted with
premultiplied alpha over a mostly transparent backdrop, so the orange adds
light to the world behind it exactly like the HUD does on the desktop.

The right hand points (its aim ray onto the panel), the trigger clicks.
Nothing here loads anything while the menu is up: the sprites and sounds
come in with the map (VR_LoadMenuAssets).
*/

#include "common.h"
#include "client.h"

#if XASH_OPENXR

#include "vr_local.h"
#include "ref_common.h" // refState, R_GetTextureParms
#include "const.h" // kRenderTransAdd

// Half-Life's HUD colors (hud.h in the client: RGB_YELLOWISH, RGB_REDISH)
#define VR_MENU_R 255
#define VR_MENU_G 160
#define VR_MENU_B 0

// layout, in panel pixels (VR_MENU_WIDTH x VR_MENU_HEIGHT)
#define VR_MENU_TITLE_X  64
#define VR_MENU_TITLE_Y  40
#define VR_MENU_SLOT_X   64  // the bucket number
#define VR_MENU_SLOT_SIZE 40
#define VR_MENU_BOX_X    120 // the button boxes, the size of the HUD's selection frame
#define VR_MENU_BOX_W    340
#define VR_MENU_BOX_H    90
#define VR_MENU_BOX_GAP  12
#define VR_MENU_ITEMS_Y  136
#define VR_MENU_TEXT_PAD 28  // label from the box's left edge
#define VR_MENU_HELP_X   540 // the pointed item's description
#define VR_MENU_HELP_W   420
#define VR_MENU_HINT_Y   ( VR_MENU_HEIGHT - 56 )

#define VR_MENU_DIM       0.6f  // how dark the world gets behind the menu
#define VR_MENU_DIM_TIME  0.15f // seconds the dimming takes
#define VR_MENU_PRESS     0.75f // trigger pulled this far clicks
#define VR_MENU_RELEASE   0.35f // ... and must go back under this before the next click

typedef enum
{
	VR_MA_RESUME = 0,
	VR_MA_QUICKSAVE,
	VR_MA_QUICKLOAD,
	VR_MA_GAMEMENU,
	VR_MA_QUIT,
	VR_MA_QUIT_YES,
	VR_MA_QUIT_NO,
} vr_menu_action_t;

typedef struct
{
	const char *label;
	const char *help;
	vr_menu_action_t action;
} vr_menu_item_t;

typedef struct
{
	const char *title;
	const vr_menu_item_t *items;
	int count;
} vr_menu_page_t;

static const vr_menu_item_t vr_menu_main_items[] =
{
	{ "Resume", "Back to the game.", VR_MA_RESUME },
	{ "Quick save", "Save the game in the quick save slot.", VR_MA_QUICKSAVE },
	{ "Load quick save", "Go back to the last quick save.", VR_MA_QUICKLOAD },
	{ "Game menu", "Half-Life's own menu: new game, saves and options.", VR_MA_GAMEMENU },
	{ "Quit", "Leave Half-Life.", VR_MA_QUIT },
};

static const vr_menu_item_t vr_menu_quit_items[] =
{
	{ "Quit", "Progress since the last save is lost.", VR_MA_QUIT_YES },
	{ "Cancel", "Back to the menu.", VR_MA_QUIT_NO },
};

#define VR_PAGE_MAIN 0
#define VR_PAGE_QUIT 1

static const vr_menu_page_t vr_menu_pages[] =
{
	{ "Paused", vr_menu_main_items, ARRAYSIZE( vr_menu_main_items ) },
	{ "Quit Half-Life?", vr_menu_quit_items, ARRAYSIZE( vr_menu_quit_items ) },
};

// a rectangle of a sprite's first frame, in the sprite's pixels
typedef struct
{
	const char *path;
	int x, y, w, h;
} vr_menu_pic_def_t;

typedef struct
{
	model_t *sprite;
	int x, y, w, h;
} vr_menu_pic_t;

// the 25th anniversary update's sharp HUD first, the original 640 HUD
// (the same pictures at half the size) for older installs
static const vr_menu_pic_def_t vr_selection_defs[] =
{
	{ "sprites/1280/hud_selection.spr", 0, 0, 340, 90 },
	{ "sprites/640hud3.spr", 0, 180, 170, 45 },
};

static const vr_menu_pic_def_t vr_bucket_defs[5][2] =
{
	{ { "sprites/1280/hud_bucket1.spr", 0, 0, 40, 40 }, { "sprites/640hud7.spr", 168, 72, 20, 20 } },
	{ { "sprites/1280/hud_bucket2.spr", 0, 0, 40, 40 }, { "sprites/640hud7.spr", 188, 72, 20, 20 } },
	{ { "sprites/1280/hud_bucket3.spr", 0, 0, 40, 40 }, { "sprites/640hud7.spr", 208, 72, 20, 20 } },
	{ { "sprites/1280/hud_bucket4.spr", 0, 0, 40, 40 }, { "sprites/640hud7.spr", 168, 92, 20, 20 } },
	{ { "sprites/1280/hud_bucket5.spr", 0, 0, 40, 40 }, { "sprites/640hud7.spr", 188, 92, 20, 20 } },
};

// Half-Life's weapon selection sounds
#define VR_SND_OPEN   "common/wpn_hudon.wav"
#define VR_SND_CLOSE  "common/wpn_hudoff.wav"
#define VR_SND_MOVE   "common/wpn_moveselect.wav"
#define VR_SND_SELECT "common/wpn_select.wav"

static CVAR_DEFINE_AUTO( vr_menu, "1", FCVAR_ARCHIVE, "the controller's menu button opens the VR pause menu in a game; 0 = Half-Life's 2D menu" );
static CVAR_DEFINE_AUTO( vr_debug_menupoint, "", 0, "developer: point at \"x y\" panel pixels on the VR menu instead of with the right hand (the panel is 1024 x 704)" );

static struct
{
	qboolean open;
	int      page;
	int      hover; // item pointed at, -1 = none
	XrPosef  pose; // in the play space, set when the menu opens

	qboolean cursor_valid; // the pointer is on the panel
	float    cursor_x, cursor_y; // panel pixels from the top left

	qboolean trigger_down; // the right trigger is pulled (with hysteresis)
	qboolean hold_fire; // the menu closed with the trigger pulled: no shot until it's let go
	qboolean debug_click; // vr_debug_menuclick

	float    dim; // 0..1 of VR_MENU_DIM
	double   dim_time; // last update

	vr_menu_pic_t selection;
	vr_menu_pic_t buckets[5];

	float    sx, sy; // window units per panel pixel while drawing
	float    ox, oy; // window units where the panel's top left lands
} vrm;

static void VR_LoadMenuPic( vr_menu_pic_t *pic, const vr_menu_pic_def_t *defs, int count )
{
	int i;

	memset( pic, 0, sizeof( *pic ));

	for( i = 0; i < count; i++ )
	{
		model_t *spr;

		if( !FS_FileExists( defs[i].path, false ))
			continue;

		spr = CL_LoadClientSprite( defs[i].path );
		if( !spr )
			continue;

		pic->sprite = spr;
		pic->x = defs[i].x;
		pic->y = defs[i].y;
		pic->w = defs[i].w;
		pic->h = defs[i].h;
		return;
	}
}

void VR_LoadMenuAssets( void )
{
	int i;

	VR_LoadMenuPic( &vrm.selection, vr_selection_defs, ARRAYSIZE( vr_selection_defs ));
	for( i = 0; i < 5; i++ )
		VR_LoadMenuPic( &vrm.buckets[i], vr_bucket_defs[i], 2 );

	// loaded with the map's sounds, so playing them reads nothing
	S_RegisterSound( VR_SND_OPEN );
	S_RegisterSound( VR_SND_CLOSE );
	S_RegisterSound( VR_SND_MOVE );
	S_RegisterSound( VR_SND_SELECT );
}

// local sounds on the static channel play while the game is paused
static void VR_MenuSound( const char *name )
{
	S_StartLocalSound( name, VOL_NORM, true );
}

/*
VR_MenuAvailable

The menu belongs to a running singleplayer game: not in Half-Life's own
menu or console, not on the menu's background map, not while loading.
*/
static qboolean VR_MenuAvailable( void )
{
	return vr_menu.value && cls.state == ca_active && !cl.background && cl.maxclients <= 1
		&& cls.key_dest == key_game && !UI_IsVisible( ) && !cls.disable_screen;
}

qboolean VR_MenuOpen( void )
{
	return vrm.open;
}

static void VR_MenuSetOpen( qboolean open, qboolean sound )
{
	if( open == vrm.open )
		return;

	vrm.open = open;
	vrm.page = VR_PAGE_MAIN;
	vrm.hover = -1;
	vrm.cursor_valid = false;

	if( open )
	{
		VR_PoseAhead( VR_MENU_DISTANCE, VR_MENU_DROP, &vrm.pose );
		// a trigger already held doesn't click
		vrm.trigger_down = true;
	}
	else
	{
		// the trigger that clicked Resume mustn't fire the gun
		vrm.hold_fire = true;
	}

	if( sound )
		VR_MenuSound( open ? VR_SND_OPEN : VR_SND_CLOSE );
	Con_Reportf( "VR: pause menu %s\n", open ? "open" : "closed" );
}

/*
VR_MenuButton

The controller's menu button: opens the menu in a game, closes it when
it's up. Anywhere else (Half-Life's own menu, the console, not in a game)
it toggles Half-Life's 2D menu as before.
*/
void VR_MenuButton( void )
{
	if( vrm.open )
		VR_MenuSetOpen( false, true );
	else if( VR_MenuAvailable( ))
		VR_MenuSetOpen( true, true );
	else Cbuf_AddText( "togglemenu\n" );
}

static void VR_Menu_f( void )
{
	if( vrm.open )
		VR_MenuSetOpen( false, true );
	else if( VR_MenuAvailable( ))
		VR_MenuSetOpen( true, true );
	else Con_Printf( "VR: the pause menu only opens in a running singleplayer game\n" );
}

static void VR_DebugMenuClick_f( void )
{
	vrm.debug_click = true;
}

const XrPosef *VR_MenuPose( void )
{
	return &vrm.pose;
}

float VR_MenuDim( void )
{
	return vrm.dim * VR_MENU_DIM;
}

qboolean VR_MenuHoldsFire( void )
{
	return vrm.hold_fire;
}

static void VR_Rotate( const XrQuaternionf *q, float x, float y, float z, XrVector3f *out )
{
	// v + 2w(q x v) + 2 q x (q x v)
	float cx = q->y * z - q->z * y;
	float cy = q->z * x - q->x * z;
	float cz = q->x * y - q->y * x;

	out->x = x + 2.0f * ( q->w * cx + q->y * cz - q->z * cy );
	out->y = y + 2.0f * ( q->w * cy + q->z * cx - q->x * cz );
	out->z = z + 2.0f * ( q->w * cz + q->x * cy - q->y * cx );
}

static float VR_Dot( const XrVector3f *a, const XrVector3f *b )
{
	return a->x * b->x + a->y * b->y + a->z * b->z;
}

/*
VR_MenuPointer

Where the right hand's aim ray meets the panel, in panel pixels. The quad
faces along its +Z, its image's top is +Y.
*/
static qboolean VR_MenuPointer( float *px, float *py )
{
	XrPosef aim;
	XrVector3f dir, normal, right, up, to, hit;
	float denom, t, x, y, w, h;

	if( vr_debug_menupoint.string[0] )
		return sscanf( vr_debug_menupoint.string, "%f %f", px, py ) == 2;

	if( !VR_HandAimPose( VR_HAND_RIGHT, &aim ))
		return false;

	VR_Rotate( &aim.orientation, 0.0f, 0.0f, -1.0f, &dir );
	VR_Rotate( &vrm.pose.orientation, 0.0f, 0.0f, 1.0f, &normal );
	VR_Rotate( &vrm.pose.orientation, 1.0f, 0.0f, 0.0f, &right );
	VR_Rotate( &vrm.pose.orientation, 0.0f, 1.0f, 0.0f, &up );

	denom = VR_Dot( &dir, &normal );
	if( denom > -0.0001f )
		return false; // pointing away from the panel or along it

	to.x = vrm.pose.position.x - aim.position.x;
	to.y = vrm.pose.position.y - aim.position.y;
	to.z = vrm.pose.position.z - aim.position.z;
	t = VR_Dot( &to, &normal ) / denom;
	if( t < 0.0f )
		return false; // behind the hand

	hit.x = aim.position.x + dir.x * t - vrm.pose.position.x;
	hit.y = aim.position.y + dir.y * t - vrm.pose.position.y;
	hit.z = aim.position.z + dir.z * t - vrm.pose.position.z;
	x = VR_Dot( &hit, &right );
	y = VR_Dot( &hit, &up );

	w = VR_MENU_METRES;
	h = VR_MENU_METRES * VR_MENU_HEIGHT / VR_MENU_WIDTH;
	*px = ( x / w + 0.5f ) * VR_MENU_WIDTH;
	*py = ( 0.5f - y / h ) * VR_MENU_HEIGHT;
	return true;
}

static int VR_MenuItemY( int i )
{
	return VR_MENU_ITEMS_Y + i * ( VR_MENU_BOX_H + VR_MENU_BOX_GAP );
}

static int VR_MenuHitItem( float x, float y )
{
	const vr_menu_page_t *page = &vr_menu_pages[vrm.page];
	int i;

	// the slot number counts as part of its button
	if( x < VR_MENU_SLOT_X || x >= VR_MENU_BOX_X + VR_MENU_BOX_W )
		return -1;

	for( i = 0; i < page->count; i++ )
	{
		int top = VR_MenuItemY( i );

		if( y >= top && y < top + VR_MENU_BOX_H )
			return i;
	}

	return -1;
}

static void VR_MenuActivate( vr_menu_action_t action )
{
	switch( action )
	{
	case VR_MA_RESUME:
		VR_MenuSetOpen( false, false );
		break;
	case VR_MA_QUICKSAVE:
		// the game saves and carries on, like the quick save key
		VR_MenuSetOpen( false, false );
		Cbuf_AddText( "save quick\n" );
		break;
	case VR_MA_QUICKLOAD:
		VR_MenuSetOpen( false, false );
		Cbuf_AddText( "load quick\n" );
		break;
	case VR_MA_GAMEMENU:
		VR_MenuSetOpen( false, false );
		Cbuf_AddText( "togglemenu\n" );
		break;
	case VR_MA_QUIT:
		vrm.page = VR_PAGE_QUIT;
		vrm.hover = -1;
		break;
	case VR_MA_QUIT_YES:
		VR_MenuSetOpen( false, false );
		Cbuf_AddText( "quit\n" );
		break;
	case VR_MA_QUIT_NO:
		vrm.page = VR_PAGE_MAIN;
		vrm.hover = -1;
		break;
	}
}

/*
VR_MenuUpdate

Once a frame, after the controllers were read (VR_PreCreateMove): the
dimming, the pointer, hovering and clicking. Closes the menu by itself
when the game it belongs to goes away (a load, Half-Life's menu, the
console).
*/
void VR_MenuUpdate( void )
{
	double now = Platform_DoubleTime( );
	float dt = (float)bound( 0.0, now - vrm.dim_time, 0.1 );
	float trigger = 0.0f, squeeze = 0.0f;
	qboolean click = false;

	vrm.dim_time = now;

	VR_HandFingers( VR_HAND_RIGHT, &trigger, &squeeze );
	if( trigger < VR_MENU_RELEASE )
	{
		vrm.trigger_down = false;
		vrm.hold_fire = false;
	}
	else if( trigger > VR_MENU_PRESS && !vrm.trigger_down )
	{
		vrm.trigger_down = true;
		click = true;
	}

	if( vrm.open && !VR_MenuAvailable( ))
		VR_MenuSetOpen( false, false );

	// fade the world down and back up
	if( vrm.open )
		vrm.dim = Q_min( 1.0f, vrm.dim + dt / VR_MENU_DIM_TIME );
	else vrm.dim = Q_max( 0.0f, vrm.dim - dt / VR_MENU_DIM_TIME );

	if( !vrm.open )
	{
		vrm.debug_click = false;
		return;
	}

	if( vrm.debug_click )
	{
		click = true;
		vrm.debug_click = false;
	}

	vrm.cursor_valid = VR_MenuPointer( &vrm.cursor_x, &vrm.cursor_y )
		&& vrm.cursor_x >= 0.0f && vrm.cursor_x < VR_MENU_WIDTH && vrm.cursor_y >= 0.0f && vrm.cursor_y < VR_MENU_HEIGHT;

	{
		int hover = vrm.cursor_valid ? VR_MenuHitItem( vrm.cursor_x, vrm.cursor_y ) : -1;

		if( hover != vrm.hover )
		{
			vrm.hover = hover;
			if( hover >= 0 )
			{
				VR_MenuSound( VR_SND_MOVE );
				VR_Haptic( VR_HAND_RIGHT, 0.015f, 0.25f );
				Con_Reportf( "VR: menu pointing at %s\n", vr_menu_pages[vrm.page].items[hover].label );
			}
		}
	}

	if( click && vrm.hover >= 0 )
	{
		const vr_menu_item_t *item = &vr_menu_pages[vrm.page].items[vrm.hover];

		VR_MenuSound( VR_SND_SELECT );
		VR_Haptic( VR_HAND_RIGHT, 0.04f, 0.6f );
		Con_Reportf( "VR: menu: %s\n", item->label );
		VR_MenuActivate( item->action );
	}
}

/*
Drawing. Everything is placed in panel pixels and turned into the
renderer's 2D window units on the way out.
*/
static void VR_MenuFill( float x, float y, float w, float h, int a )
{
	ref.dllFuncs.FillRGBA( kRenderTransAdd, vrm.ox + x * vrm.sx, vrm.oy + y * vrm.sy, w * vrm.sx, h * vrm.sy, VR_MENU_R, VR_MENU_G, VR_MENU_B, a );
}

static void VR_MenuPic( const vr_menu_pic_t *pic, float x, float y, float w, float h, int brightness )
{
	int tex, fw = 0, fh = 0;

	if( !pic->sprite || !( tex = R_GetSpriteTexture( pic->sprite, 0 )))
		return;

	R_GetSpriteParms( &fw, &fh, NULL, 0, pic->sprite );
	if( fw <= 0 || fh <= 0 )
		return;

	ref.dllFuncs.GL_SetRenderMode( kRenderTransAdd );
	ref.dllFuncs.Color4ub( VR_MENU_R * brightness / 255, VR_MENU_G * brightness / 255, VR_MENU_B * brightness / 255, 255 );
	ref.dllFuncs.R_DrawStretchPic( vrm.ox + x * vrm.sx, vrm.oy + y * vrm.sy, w * vrm.sx, h * vrm.sy,
		(float)pic->x / fw, (float)pic->y / fh, (float)( pic->x + pic->w ) / fw, (float)( pic->y + pic->h ) / fh, tex );
}

// the HUD font's glyphs at their own size times scale, whole pixels
static int VR_MenuCharWidth( const cl_font_t *font, int c )
{
	c &= 255;
	if( c < ' ' || ( c == ' ' && !font->charWidths[' '] ))
		return 0;
	return font->fontRc[c].right - font->fontRc[c].left;
}

static int VR_MenuTextWidth( const char *s, int len, int scale )
{
	const cl_font_t *font = &cls.creditsFont;
	int i, w = 0;

	if( !font->valid )
		return 0;

	for( i = 0; s[i] && ( len < 0 || i < len ); i++ )
		w += VR_MenuCharWidth( font, (byte)s[i] ) * scale;
	return w;
}

static int VR_MenuLineHeight( int scale )
{
	const cl_font_t *font = &cls.creditsFont;

	if( !font->valid )
		return 0;
	return ( font->fontRc['A'].bottom - font->fontRc['A'].top ) * scale;
}

static void VR_MenuText( const char *s, int len, float x, float y, int scale, int brightness )
{
	const cl_font_t *font = &cls.creditsFont;
	int i, texw = 0, texh = 0;

	if( !font->valid )
		return;

	R_GetTextureParms( &texw, &texh, font->hFontTexture );
	if( texw <= 0 || texh <= 0 )
		return;

	ref.dllFuncs.GL_SetRenderMode( kRenderTransAdd );
	ref.dllFuncs.Color4ub( VR_MENU_R * brightness / 255, VR_MENU_G * brightness / 255, VR_MENU_B * brightness / 255, 255 );

	for( i = 0; s[i] && ( len < 0 || i < len ); i++ )
	{
		int c = (byte)s[i];
		const wrect_t *rc = &font->fontRc[c];
		int w = VR_MenuCharWidth( font, c );

		if( c > ' ' && w > 0 )
		{
			ref.dllFuncs.R_DrawStretchPic( vrm.ox + x * vrm.sx, vrm.oy + y * vrm.sy,
				w * scale * vrm.sx, ( rc->bottom - rc->top ) * scale * vrm.sy,
				(float)rc->left / texw, (float)rc->top / texh, (float)rc->right / texw, (float)rc->bottom / texh,
				font->hFontTexture );
		}
		x += w * scale;
	}
}

// word-wrapped into maxw pixels
static void VR_MenuWrapped( const char *s, float x, float y, int maxw, int scale, int brightness )
{
	int line = VR_MenuLineHeight( scale ) + 4 * scale;

	while( *s )
	{
		int len = 0, fit = 0;

		// take whole words while they fit
		while( s[len] )
		{
			int end = len;

			while( s[end] && s[end] != ' ' )
				end++;
			if( fit && VR_MenuTextWidth( s, end, scale ) > maxw )
				break;
			fit = end;
			len = s[end] ? end + 1 : end;
		}
		if( !fit )
			fit = len;

		VR_MenuText( s, fit, x, y, scale, brightness );
		y += line;
		s += len;
	}
}

/*
VR_MenuDraw

Draws the menu into a vw x vh viewport (the panel's swapchain, or the
desktop window) in the renderer's 2D mode. The caller cleared it to the
backdrop.
*/
void VR_MenuDraw( int vw, int vh )
{
	const vr_menu_page_t *page = &vr_menu_pages[vrm.page];
	int i, line = VR_MenuLineHeight( 2 );

	if( vw <= 0 || vh <= 0 )
		return;

	// panel pixels onto the viewport, aspect kept, centered (the panel's
	// swapchain fits exactly, the desktop window gets bars)
	{
		float fit = Q_min( (float)vw / VR_MENU_WIDTH, (float)vh / VR_MENU_HEIGHT );

		vrm.sx = fit * refState.width / vw;
		vrm.sy = fit * refState.height / vh;
		vrm.ox = ( vw - VR_MENU_WIDTH * fit ) * 0.5f * refState.width / vw;
		vrm.oy = ( vh - VR_MENU_HEIGHT * fit ) * 0.5f * refState.height / vh;
	}

	VR_MenuText( page->title, -1, VR_MENU_TITLE_X, VR_MENU_TITLE_Y, 3, 255 );

	for( i = 0; i < page->count; i++ )
	{
		const vr_menu_item_t *item = &page->items[i];
		qboolean hover = i == vrm.hover;
		int y = VR_MenuItemY( i );

		// the slot number, like the weapon buckets' top row
		if( i < 5 )
			VR_MenuPic( &vrm.buckets[i], VR_MENU_SLOT_X, y + ( VR_MENU_BOX_H - VR_MENU_SLOT_SIZE ) / 2,
				VR_MENU_SLOT_SIZE, VR_MENU_SLOT_SIZE, hover ? 255 : 192 );

		// the box: a weapon bucket's translucent fill, the selected
		// weapon's frame around the one pointed at
		VR_MenuFill( VR_MENU_BOX_X, y, VR_MENU_BOX_W, VR_MENU_BOX_H, hover ? 96 : 40 );
		if( hover )
			VR_MenuPic( &vrm.selection, VR_MENU_BOX_X, y, VR_MENU_BOX_W, VR_MENU_BOX_H, 255 );

		VR_MenuText( item->label, -1, VR_MENU_BOX_X + VR_MENU_TEXT_PAD, y + ( VR_MENU_BOX_H - line ) / 2, 2, hover ? 255 : 192 );
	}

	if( vrm.hover >= 0 )
		VR_MenuWrapped( page->items[vrm.hover].help, VR_MENU_HELP_X, VR_MenuItemY( vrm.hover ) + ( VR_MENU_BOX_H - line ) / 2,
			VR_MENU_HELP_W, 2, 192 );

	VR_MenuText( "Trigger: select    Menu: resume", -1, VR_MENU_TITLE_X, VR_MENU_HINT_Y, 2, 128 );

	// the pointer: a small cross like the HUD's crosshair
	if( vrm.cursor_valid )
	{
		VR_MenuFill( vrm.cursor_x - 9, vrm.cursor_y - 2, 18, 4, 255 );
		VR_MenuFill( vrm.cursor_x - 2, vrm.cursor_y - 9, 4, 18, 255 );
	}
}

void VR_MenuInfo( void )
{
	Con_Printf( "VR: pause menu %s%s, page %s, pointing at %s, cursor %s %.0f %.0f, dim %.2f%s\n",
		vrm.open ? "open" : "closed", VR_MenuAvailable( ) ? "" : " (not available here)",
		vr_menu_pages[vrm.page].title,
		vrm.hover >= 0 ? vr_menu_pages[vrm.page].items[vrm.hover].label : "nothing",
		vrm.cursor_valid ? "on the panel at" : "off the panel, last", vrm.cursor_x, vrm.cursor_y, VR_MenuDim( ),
		vrm.hold_fire ? ", holding fire until the trigger is let go" : "" );
	Con_Printf( "VR: menu art: selection frame %s, slot numbers %s\n",
		vrm.selection.sprite ? vrm.selection.sprite->name : "missing",
		vrm.buckets[0].sprite ? vrm.buckets[0].sprite->name : "missing" );
}

void VR_MenuInit( void )
{
	Cvar_RegisterVariable( &vr_menu );
	Cvar_RegisterVariable( &vr_debug_menupoint );
	Cmd_AddCommand( "vr_pausemenu", VR_Menu_f, "open or close the VR pause menu" );
	Cmd_AddCommand( "vr_debug_menuclick", VR_DebugMenuClick_f, "developer: click the VR menu once, as if the right trigger was pulled" );
}

#endif // XASH_OPENXR
