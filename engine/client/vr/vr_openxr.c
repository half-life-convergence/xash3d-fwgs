/*
vr_openxr.c - OpenXR instance and system setup
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

#include "common.h"
#include "client.h"

#if XASH_OPENXR

#include "vr_local.h"
#include "platform/platform.h"
#include "ref_common.h" // refState, the window size
#include "const.h" // kRenderTransTexture
#include "pm_local.h" // PM_CL_TraceLine flags
#include "sound.h" // snd.origin, the listener, for vr_info
#include "mod_local.h" // Mod_StudioExtradata, for the weapon's muzzle
#include "studio.h" // studiohdr_t
#if XASH_SDL == 2
#include <SDL.h> // SDL_GL_SetSwapInterval for the desktop mirror window
#endif

#define VR_OPENGL_EXTENSION "XR_KHR_opengl_enable"

static PFN_xrGetInstanceProcAddr                  pxrGetInstanceProcAddr;
static PFN_xrEnumerateInstanceExtensionProperties pxrEnumerateInstanceExtensionProperties;
static PFN_xrCreateInstance                       pxrCreateInstance;
static PFN_xrDestroyInstance                      pxrDestroyInstance;
static PFN_xrGetInstanceProperties                pxrGetInstanceProperties;
static PFN_xrGetSystem                            pxrGetSystem;
static PFN_xrGetSystemProperties                  pxrGetSystemProperties;
static PFN_xrEnumerateViewConfigurationViews      pxrEnumerateViewConfigurationViews;
static PFN_xrGetOpenGLGraphicsRequirementsKHR     pxrGetOpenGLGraphicsRequirementsKHR;
static PFN_xrCreateSession                        pxrCreateSession;
static PFN_xrDestroySession                       pxrDestroySession;
static PFN_xrEnumerateReferenceSpaces             pxrEnumerateReferenceSpaces;
static PFN_xrCreateReferenceSpace                 pxrCreateReferenceSpace;
static PFN_xrDestroySpace                         pxrDestroySpace;
static PFN_xrPollEvent                            pxrPollEvent;
static PFN_xrEnumerateEnvironmentBlendModes       pxrEnumerateEnvironmentBlendModes;
static PFN_xrBeginSession                         pxrBeginSession;
static PFN_xrEndSession                           pxrEndSession;
static PFN_xrWaitFrame                            pxrWaitFrame;
static PFN_xrBeginFrame                           pxrBeginFrame;
static PFN_xrEndFrame                             pxrEndFrame;
static PFN_xrLocateViews                          pxrLocateViews;
static PFN_xrEnumerateSwapchainFormats            pxrEnumerateSwapchainFormats;
static PFN_xrCreateSwapchain                      pxrCreateSwapchain;
static PFN_xrDestroySwapchain                     pxrDestroySwapchain;
static PFN_xrEnumerateSwapchainImages             pxrEnumerateSwapchainImages;
static PFN_xrAcquireSwapchainImage                pxrAcquireSwapchainImage;
static PFN_xrWaitSwapchainImage                   pxrWaitSwapchainImage;
static PFN_xrReleaseSwapchainImage                pxrReleaseSwapchainImage;

// the few GL entry points the VR layer needs itself, loaded through
// GL_GetProcAddress so the engine doesn't depend on GL headers
#ifndef APIENTRY
#define APIENTRY
#endif
typedef unsigned int vr_GLuint;
typedef unsigned int vr_GLenum;
typedef int          vr_GLint;
typedef unsigned char vr_GLboolean;

#define VR_GL_TEXTURE_2D                0x0DE1
#define VR_GL_SCISSOR_TEST              0x0C11
#define VR_GL_LINEAR                    0x2601
#define VR_GL_COLOR_BUFFER_BIT          0x4000
#define VR_GL_RGBA8                     0x8058
#define VR_GL_SRGB8_ALPHA8              0x8C43
#define VR_GL_DRAW_FRAMEBUFFER_BINDING  0x8CA6
#define VR_GL_READ_FRAMEBUFFER          0x8CA8
#define VR_GL_DRAW_FRAMEBUFFER          0x8CA9
#define VR_GL_READ_FRAMEBUFFER_BINDING  0x8CAA
#define VR_GL_FRAMEBUFFER_COMPLETE      0x8CD5
#define VR_GL_COLOR_ATTACHMENT0         0x8CE0
#define VR_GL_FRAMEBUFFER_SRGB          0x8DB9
#define VR_GL_FRAMEBUFFER               0x8D40
#define VR_GL_RENDERBUFFER              0x8D41
#define VR_GL_DEPTH24_STENCIL8          0x88F0
#define VR_GL_DEPTH_STENCIL_ATTACHMENT  0x821A
#define VR_GL_ALPHA_TEST                0x0BC0
#define VR_GL_MAX_SAMPLES               0x8D57
#define VR_GL_NEAREST                   0x2600

static struct
{
	void (APIENTRY *GenFramebuffers)( int n, vr_GLuint *ids );
	void (APIENTRY *DeleteFramebuffers)( int n, const vr_GLuint *ids );
	void (APIENTRY *BindFramebuffer)( vr_GLenum target, vr_GLuint id );
	void (APIENTRY *FramebufferTexture2D)( vr_GLenum target, vr_GLenum attachment, vr_GLenum textarget, vr_GLuint texture, vr_GLint level );
	vr_GLenum (APIENTRY *CheckFramebufferStatus)( vr_GLenum target );
	void (APIENTRY *BlitFramebuffer)( vr_GLint sx0, vr_GLint sy0, vr_GLint sx1, vr_GLint sy1, vr_GLint dx0, vr_GLint dy0, vr_GLint dx1, vr_GLint dy1, unsigned int mask, vr_GLenum filter );
	void (APIENTRY *GetIntegerv)( vr_GLenum pname, vr_GLint *data );
	vr_GLboolean (APIENTRY *IsEnabled)( vr_GLenum cap );
	void (APIENTRY *Enable)( vr_GLenum cap );
	void (APIENTRY *Disable)( vr_GLenum cap );
	void (APIENTRY *GenRenderbuffers)( int n, vr_GLuint *ids );
	void (APIENTRY *DeleteRenderbuffers)( int n, const vr_GLuint *ids );
	void (APIENTRY *BindRenderbuffer)( vr_GLenum target, vr_GLuint id );
	void (APIENTRY *RenderbufferStorage)( vr_GLenum target, vr_GLenum format, int width, int height );
	void (APIENTRY *FramebufferRenderbuffer)( vr_GLenum target, vr_GLenum attachment, vr_GLenum rbtarget, vr_GLuint rb );
	void (APIENTRY *ClearColor)( float r, float g, float b, float a );
	void (APIENTRY *Clear)( unsigned int mask );
	void (APIENTRY *Viewport)( vr_GLint x, vr_GLint y, int width, int height );
	void (APIENTRY *RenderbufferStorageMultisample)( vr_GLenum target, int samples, vr_GLenum format, int width, int height );
} vrgl;

static const dllfunc_t openxr_loader_funcs[] =
{
	{ "xrGetInstanceProcAddr", (void **)&pxrGetInstanceProcAddr },
};

static dll_info_t openxr_loader_info =
{
#if XASH_WIN32
	.name = "openxr_loader.dll",
#elif XASH_APPLE
	.name = "libopenxr_loader.dylib",
#else
	.name = "libopenxr_loader.so.1",
#endif
	.fcts = openxr_loader_funcs,
	.num_fcts = ARRAYSIZE( openxr_loader_funcs ),
	.crash = false,
};

static struct
{
	qboolean   initialized;
	qboolean   has_opengl;
	XrInstance instance;
	XrSystemId system;
	uint32_t   eye_width; // recommended per-eye render size
	uint32_t   eye_height;

	XrSession      session;
	XrSessionState session_state;
	XrSpace        space; // world origin, STAGE when the runtime has it, else LOCAL
	XrReferenceSpaceType space_type;
	XrEnvironmentBlendMode blend_mode;

	// frame loop, only runs between xrBeginSession and xrEndSession
	qboolean running;
	qboolean frame_begun; // xrBeginFrame was called, xrEndFrame is owed
	XrTime   display_time; // predicted display time of the current frame
	XrDuration display_period;
	qboolean should_render;
	qboolean submits_layers; // set once eye images are submitted, see VR_UpdatePacing
	qboolean paced; // xrWaitFrame is the frame limiter right now
	// one color swapchain per eye
	XrSwapchain swapchain[2];
	XrSwapchainImageOpenGLKHR *images[2];
	uint32_t    num_images[2];
	int64_t     swapchain_format;
	vr_GLuint   fbo; // draw target for rendering/copying into swapchain images
	vr_GLuint   depth; // depth+stencil renderbuffer shared by both eyes, stays attached to fbo
	vr_GLuint   msaa_fbo; // with vr_msaa, the eyes are drawn here and resolved into the swapchain image
	vr_GLuint   msaa_color;
	vr_GLuint   msaa_depth;
	int         samples; // 0 = no MSAA
	uint32_t    rec_width, rec_height; // the runtime's recommended eye size, before vr_renderscale
	uint32_t    max_width, max_height; // the runtime's swapchain size limit

	XrView   views[2]; // eye poses and fovs at display_time
	qboolean views_valid;
	qboolean logged_fov;
	qboolean eyes_written; // both eye images were filled this frame

	// room scale: the real head walks away from the player's collision box,
	// the box follows through player movement when the way is clear
	vec3_t   room_gap; // horizontal offset from the box to the head, game units, world axes
	vec3_t   head_prev; // last head center, game units, play space axes
	qboolean head_prev_valid;
	float    fade; // head-in-wall fade, 0..1
	float    head_yaw; // degrees, where the head faces in the play space, for head-relative stick movement
	float    feet_z; // the box bottom with stairs smoothed, see VR_UpdateStairs
	float    air_tuck; // ducking in the air lifted the box bottom by this, the view stays put
	int      prev_hull; // last frame's cl.local.usehull, -1 = none
	qboolean feet_valid;
	float    stair_lag_max; // largest smoothing so far, for vr_info
	float    head_height; // the head center above the play space floor, game units (vr_heightoffset included)
	float    duck_lower; // the view is lowered this much while ducked, see VR_UpdateDuckView

	// hands in the world, this frame: grip position, aim direction.
	// hand_origin is where the hand is drawn, kept out of walls; the
	// controller itself is at hand_tracked (see VR_CollideHand)
	qboolean hand_valid[2];
	vec3_t   hand_tracked[2];
	float    hand_play_yaw[2]; // where the aim points in the play space, degrees, for vr_movedir
	qboolean hand_prev_valid[2]; // hand_origin holds last frame's drawn hand
	qboolean hand_separated[2]; // held back by a wall, easing back to the tracked hand
	qboolean hand_blocked[2]; // touching a wall this frame
	qboolean hand_at_head[2]; // pushed out of the player's head this frame
	model_t  *muzzle_model; // the weapon model muzzle_local belongs to
	vec3_t   muzzle_local; // the end of the weapon relative to the hand: forward, right, up
	qboolean muzzle_measured; // muzzle_local comes from the model's shape
	vec3_t   hand_origin[2];
	vec3_t   hand_angles[2];

	// the off hand against the weapon in the right hand, see VR_OffhandWeapon
	int      hand_pose[2]; // hands.mdl pose for VR_AddHands, -1 = from the fingers
	const model_t *grab_model; // the weapon model grab_point belongs to
	int      grab_point; // the grab point the left hand holds, -1 = none
	int      grab_near; // the grab point the left hand is reaching for, -1 = none
	qboolean offhand_on_gun; // the left hand was pushed out of the weapon this frame
	qboolean offhand_prev_valid;
	vec3_t   offhand_prev; // last frame's drawn left hand in the gun's axes

	// the head center in the world, this frame, for the sound listener
	vec3_t   head_origin;
	vec3_t   head_angles;

	// loading panel: while the game loads, the headset gets only a
	// world-locked quad with the loading screen on it, see VR_UpdateTransition
	XrSwapchain panel_swapchain;
	XrSwapchainImageOpenGLKHR *panel_images;
	uint32_t panel_num_images;
	qboolean panel_valid; // drawn for the current loading
	XrPosef  panel_pose;
	int      transition; // VR_TRANSITION_*
	double   transition_time; // when the current phase began
	double   transition_last; // last update, for the fade speed
	int      hold_frames; // frames rendered since loading ended
	float    load_fade; // fade-in after loading, 0..1, drawn like the wall fade
	qboolean game_paused; // the session lost focus, the game is paused
	uint32_t frames_panel; // frames submitted with the loading panel

	uint32_t frames; // frames submitted since the session began
	uint32_t frames_rendered; // ... of which the renderer drew the eyes
	uint32_t frames_copied; // ... of which the eyes got the desktop copy
	double   begin_time;
	double   wait_total; // seconds spent blocked in xrWaitFrame
	double   wait_max;
	qboolean warned_wait; // print a failing xrWaitFrame only once
} vr;

// Half-Life units are close to inches: the player is 72 units tall and
// his eyes are 64 units above his feet
static CVAR_DEFINE_AUTO( vr_worldscale, "39.37", FCVAR_ARCHIVE, "game units per real metre for head tracking" );
static CVAR_DEFINE_AUTO( vr_heightoffset, "0", FCVAR_ARCHIVE, "metres added to the tracked head height (seated play, SteamVR null driver which sits on the floor)" );
static CVAR_DEFINE_AUTO( vr_mirror, "0", FCVAR_ARCHIVE, "desktop window in VR: 0 = copy of the left eye (fast), 1 = full desktop render (third world pass, for streaming)" );
static CVAR_DEFINE_AUTO( vr_roomscale, "1", FCVAR_ARCHIVE, "walking in the real room moves the player" );
static CVAR_DEFINE_AUTO( vr_renderscale, "1.0", FCVAR_ARCHIVE, "eye resolution relative to what the headset recommends, 0.5 to 2" );
static CVAR_DEFINE_AUTO( vr_msaa, "4", FCVAR_ARCHIVE, "multisample anti-aliasing samples for the eyes, 0 = off" );
static CVAR_DEFINE_AUTO( vr_handweapon, "1", FCVAR_ARCHIVE, "the weapon follows the right hand instead of the camera" );
static CVAR_DEFINE_AUTO( vr_viewmodel_grip, "12 6 -9", FCVAR_ARCHIVE, "where the hand holds the weapon model, \"forward right up\" in model units from its origin" );
static CVAR_DEFINE_AUTO( vr_debug_headmove, "0 0", 0, "developer: move the tracked head by \"forward left\" metres in the play space, to test room scale without a headset" );
static CVAR_DEFINE_AUTO( vr_crouchheight, "1.2", FCVAR_ARCHIVE, "the player ducks when the real head is lower than this many metres (after vr_heightoffset); 0 = no physical crouching" );
static CVAR_DEFINE_AUTO( vr_stairsmooth, "1", FCVAR_ARCHIVE, "smooth the view up and down stairs and steps instead of popping" );
static CVAR_DEFINE_AUTO( vr_hidearms, "1", FCVAR_ARCHIVE, "leave out the arms built into Half-Life's weapon models while the weapon is in the hand" );
static CVAR_DEFINE_AUTO( vr_pinweapon, "1", FCVAR_ARCHIVE, "keep the weapon still in the hand while its animations play (reloads, draws); read by the client's studio renderer" );
static CVAR_DEFINE_AUTO( vr_twosided, "1", FCVAR_ARCHIVE, "draw the weapon in the hand without back-face culling, so faces Half-Life left out of the model show the far side instead of a hole" );
static int VR_ViewmodelFlags( void );
static CVAR_DEFINE_AUTO( vr_handcollide, "1", FCVAR_ARCHIVE, "hands and the weapon stop at walls instead of going through them" );
static CVAR_DEFINE_AUTO( vr_weaponcollide, "1", FCVAR_ARCHIVE, "the off hand touches the weapon in the other hand instead of going through it (our VR weapon models)" );
static CVAR_DEFINE_AUTO( vr_grab_needgrip, "1", FCVAR_ARCHIVE, "the off hand takes hold of a weapon's grab point only while squeezing grip; 0 = as soon as it's close" );
static CVAR_DEFINE_AUTO( vr_debug_offhand, "", 0, "developer: with vr_debug_hands, put the left hand at \"forward right up\" game units from the right hand, in the gun's axes" );
static CVAR_DEFINE_AUTO( vr_weapon_length, "12", FCVAR_ARCHIVE, "for weapon models without a muzzle point that aren't in the built-in table: how far they reach past the hand along the aim, game units" );
static CVAR_DEFINE_AUTO( vr_loadfade, "0.5", FCVAR_ARCHIVE, "seconds the view takes to fade in after loading, 0 = no fade" );
static CVAR_DEFINE_AUTO( vr_debug_unfocused, "0", 0, "developer: act as if the headset lost focus (headset off, system menu open), to test pausing without a headset" );

// stair smoothing, see VR_UpdateStairs
#define VR_STAIR_RATE 12.0f // how fast the view catches up, 1/s (~13 units behind running up stairs at 4 m/s)
#define VR_STEP_MAX 18.0f // never more than a step behind (player movement's step height)
#define VR_STEP_SNAP 24.0f // a bigger jump in height is a teleport or respawn, follow it at once
#define VR_TUCK_RATE 4.0f // how fast the view lets go of a mid-air duck, 1/s (about half of it in a normal jump)

// ducking, see VR_UpdateDuckView
#define VR_DUCK_EYE 30.0f // the game's ducked eye height above the feet: duck hull bottom 18 + view offset 12
#define VR_DUCK_EYE_STAND 64.0f // standing: hull bottom 36 + view offset 28; vr_calibrate maps the head here
#define VR_DUCK_RATE 10.0f // how fast the view lowers and rises, 1/s

// hand collision, see VR_CollideHand
#define VR_HAND_RADIUS 1.5f // game units (~4 cm) kept between a hand and a surface
#define VR_HAND_RESET 12.0f // drawn and tracked hand further apart (~30 cm): start over from the eyes
#define VR_HAND_EASE 20.0f // how fast a freed hand catches up, 1/s
#define VR_OFFHAND_RADIUS 1.5f // game units (~4 cm): the off hand's palm against the weapon
#define VR_GRAB_BREAK 6.0f // game units (~15 cm) past the snap radius: the controller pulls the hand off a grab point
#define VR_GRAB_HOLD 0.6f // grip squeezed this far takes hold of a grab point
#define VR_GRAB_LET_GO 0.4f // ... and below this lets go
#define VR_BARREL_MARGIN 2.0f // kept between the muzzle and a wall
#define VR_HEAD_RADIUS_GUN 7.0f // the weapon stays this far from the head center (~18 cm, room for the barrel's thickness)
#define VR_HEAD_RADIUS_HAND 4.0f // an empty hand (~10 cm)
#define VR_GUN_REAR 4.0f // the weapon reaches this far behind the grip, for the head check

// loading transitions, see VR_UpdateTransition
#define VR_TRANSITION_NONE    0 // playing, the eyes are submitted
#define VR_TRANSITION_LOADING 1 // the game is loading, only the panel is submitted
#define VR_TRANSITION_HOLD    2 // loaded, the panel stays until the first frames run smoothly
#define VR_TRANSITION_FADEIN  3 // the eyes are back, fading in from black

#define VR_PANEL_WIDTH  1280 // loading panel image, 16:9 like most desktop windows
#define VR_PANEL_HEIGHT 720
#define VR_PANEL_METRES 1.6f // panel width in the room
#define VR_PANEL_DISTANCE 2.0f // in front of the head
#define VR_PANEL_TEXT 0.5f // LOADING image width, part of the panel's width
#define VR_HOLD_FRAMES 5 // after loading, rendered frames before the world is shown...
#define VR_HOLD_TIME 0.25 // ... and seconds, so the first hitches stay hidden

// the player's collision box never follows the head more than this per
// movement command, a lost tracking frame can't throw him across the room
#define VR_ROOM_MAX_STEP 16.0f

static const char *VR_SessionStateString( XrSessionState state )
{
	switch( state )
	{
#define VR_STATE_CASE( name, value ) case name: return #name;
	XR_LIST_ENUM_XrSessionState( VR_STATE_CASE )
#undef VR_STATE_CASE
	default: return "unknown";
	}
}

// works without an instance, unlike xrResultToString
const char *VR_ResultString( XrResult result )
{
	static char buf[64];

	switch( result )
	{
#define VR_RESULT_CASE( name, value ) case name: return #name;
	XR_LIST_ENUM_XrResult( VR_RESULT_CASE )
#undef VR_RESULT_CASE
	default: break;
	}

	Q_snprintf( buf, sizeof( buf ), "XrResult %d", (int)result );
	return buf;
}

qboolean VR_LoadProc( XrInstance instance, const char *name, PFN_xrVoidFunction *func )
{
	XrResult result = pxrGetInstanceProcAddr( instance, name, func );

	if( XR_FAILED( result ) || !*func )
	{
		Con_Printf( S_ERROR "VR: can't get %s: %s\n", name, VR_ResultString( result ));
		return false;
	}

	return true;
}

#define VR_GET_PROC( instance, name ) VR_LoadProc(( instance ), #name, (PFN_xrVoidFunction *)&p##name )

static qboolean VR_HasExtension( const char *name )
{
	XrExtensionProperties *props;
	uint32_t count = 0, i;
	qboolean found = false;

	if( XR_FAILED( pxrEnumerateInstanceExtensionProperties( NULL, 0, &count, NULL )) || !count )
		return false;

	props = Mem_Calloc( host.mempool, sizeof( *props ) * count );
	for( i = 0; i < count; i++ )
		props[i].type = XR_TYPE_EXTENSION_PROPERTIES;

	if( XR_SUCCEEDED( pxrEnumerateInstanceExtensionProperties( NULL, count, &count, props )))
	{
		for( i = 0; i < count; i++ )
		{
			if( !Q_strcmp( props[i].extensionName, name ))
			{
				found = true;
				break;
			}
		}
	}

	Mem_Free( props );
	return found;
}

static qboolean VR_CreateInstance( void )
{
	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
	const char *extensions[1];
	XrResult result;

	if( !VR_GET_PROC( XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties )
		|| !VR_GET_PROC( XR_NULL_HANDLE, xrCreateInstance ))
		return false;

	vr.has_opengl = VR_HasExtension( VR_OPENGL_EXTENSION );
	if( !vr.has_opengl )
	{
		// without it there is no way to hand our GL frames to the runtime
		Con_Printf( S_ERROR "VR: OpenXR runtime does not support %s\n", VR_OPENGL_EXTENSION );
		return false;
	}

	extensions[0] = VR_OPENGL_EXTENSION;

	Q_strncpy( info.applicationInfo.applicationName, "Half-Life: Convergence", sizeof( info.applicationInfo.applicationName ));
	info.applicationInfo.applicationVersion = 1;
	Q_strncpy( info.applicationInfo.engineName, XASH_ENGINE_NAME, sizeof( info.applicationInfo.engineName ));
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0; // widest runtime support, we need nothing from 1.1 yet
	info.enabledExtensionCount = ARRAYSIZE( extensions );
	info.enabledExtensionNames = extensions;

	result = pxrCreateInstance( &info, &vr.instance );
	if( XR_FAILED( result ))
	{
		vr.instance = XR_NULL_HANDLE;
		Con_Printf( S_ERROR "VR: xrCreateInstance failed: %s\n", VR_ResultString( result ));
		if( result == XR_ERROR_RUNTIME_FAILURE )
			Con_Printf( "VR: SteamVR reports this when no headset is connected\n" );
		return false;
	}

	if( !VR_GET_PROC( vr.instance, xrDestroyInstance )
		|| !VR_GET_PROC( vr.instance, xrGetInstanceProperties )
		|| !VR_GET_PROC( vr.instance, xrGetSystem )
		|| !VR_GET_PROC( vr.instance, xrGetSystemProperties )
		|| !VR_GET_PROC( vr.instance, xrEnumerateViewConfigurationViews )
		|| !VR_GET_PROC( vr.instance, xrGetOpenGLGraphicsRequirementsKHR )
		|| !VR_GET_PROC( vr.instance, xrCreateSession )
		|| !VR_GET_PROC( vr.instance, xrDestroySession )
		|| !VR_GET_PROC( vr.instance, xrEnumerateReferenceSpaces )
		|| !VR_GET_PROC( vr.instance, xrCreateReferenceSpace )
		|| !VR_GET_PROC( vr.instance, xrDestroySpace )
		|| !VR_GET_PROC( vr.instance, xrPollEvent )
		|| !VR_GET_PROC( vr.instance, xrEnumerateEnvironmentBlendModes )
		|| !VR_GET_PROC( vr.instance, xrBeginSession )
		|| !VR_GET_PROC( vr.instance, xrEndSession )
		|| !VR_GET_PROC( vr.instance, xrWaitFrame )
		|| !VR_GET_PROC( vr.instance, xrBeginFrame )
		|| !VR_GET_PROC( vr.instance, xrEndFrame )
		|| !VR_GET_PROC( vr.instance, xrLocateViews )
		|| !VR_GET_PROC( vr.instance, xrEnumerateSwapchainFormats )
		|| !VR_GET_PROC( vr.instance, xrCreateSwapchain )
		|| !VR_GET_PROC( vr.instance, xrDestroySwapchain )
		|| !VR_GET_PROC( vr.instance, xrEnumerateSwapchainImages )
		|| !VR_GET_PROC( vr.instance, xrAcquireSwapchainImage )
		|| !VR_GET_PROC( vr.instance, xrWaitSwapchainImage )
		|| !VR_GET_PROC( vr.instance, xrReleaseSwapchainImage ))
		return false;

	if( XR_SUCCEEDED( pxrGetInstanceProperties( vr.instance, &props )))
	{
		Con_Printf( "VR: OpenXR runtime: %s %u.%u.%u\n", props.runtimeName,
			XR_VERSION_MAJOR( props.runtimeVersion ),
			XR_VERSION_MINOR( props.runtimeVersion ),
			XR_VERSION_PATCH( props.runtimeVersion ));
	}

	return true;
}

static qboolean VR_GetSystem( void )
{
	XrSystemGetInfo info = { XR_TYPE_SYSTEM_GET_INFO };
	XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
	XrViewConfigurationView views[2];
	uint32_t count = 0;
	XrResult result;

	info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	result = pxrGetSystem( vr.instance, &info, &vr.system );

	if( result == XR_ERROR_FORM_FACTOR_UNAVAILABLE )
	{
		Con_Printf( S_WARN "VR: no headset available, is it connected and is SteamVR running?\n" );
		return false;
	}
	else if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrGetSystem failed: %s\n", VR_ResultString( result ));
		return false;
	}

	if( XR_SUCCEEDED( pxrGetSystemProperties( vr.instance, vr.system, &props )))
	{
		Con_Printf( "VR: headset: %s (max swapchain %ux%u, %u layers)\n", props.systemName,
			props.graphicsProperties.maxSwapchainImageWidth,
			props.graphicsProperties.maxSwapchainImageHeight,
			props.graphicsProperties.maxLayerCount );
		vr.max_width = props.graphicsProperties.maxSwapchainImageWidth;
		vr.max_height = props.graphicsProperties.maxSwapchainImageHeight;
	}

	if( !vr.max_width || !vr.max_height )
		vr.max_width = vr.max_height = 4096;

	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	views[0].next = views[1].next = NULL;

	result = pxrEnumerateViewConfigurationViews( vr.instance, vr.system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, ARRAYSIZE( views ), &count, views );

	if( XR_FAILED( result ) || count != 2 )
	{
		Con_Printf( S_ERROR "VR: headset has no stereo view configuration: %s\n", VR_ResultString( result ));
		return false;
	}

	vr.rec_width = vr.eye_width = views[0].recommendedImageRectWidth;
	vr.rec_height = vr.eye_height = views[0].recommendedImageRectHeight;
	Con_Printf( "VR: recommended eye size %ux%u\n", vr.eye_width, vr.eye_height );

	{
		// runtimes list blend modes in their preferred order, a VR headset
		// gives OPAQUE first; xrEndFrame needs a valid one even with no layers
		XrEnvironmentBlendMode modes[8];
		uint32_t num_modes = 0;

		vr.blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if( XR_SUCCEEDED( pxrEnumerateEnvironmentBlendModes( vr.instance, vr.system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, ARRAYSIZE( modes ), &num_modes, modes )) && num_modes > 0 )
			vr.blend_mode = modes[0];
	}

	return true;
}

static qboolean VR_CreateSession( void )
{
	XrGraphicsRequirementsOpenGLKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	XrResult result;

	// the runtime refuses to create a session before this is called
	result = pxrGetOpenGLGraphicsRequirementsKHR( vr.instance, vr.system, &reqs );
	if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrGetOpenGLGraphicsRequirementsKHR failed: %s\n", VR_ResultString( result ));
		return false;
	}

	Con_Reportf( "VR: runtime wants OpenGL %u.%u to %u.%u\n",
		XR_VERSION_MAJOR( reqs.minApiVersionSupported ), XR_VERSION_MINOR( reqs.minApiVersionSupported ),
		XR_VERSION_MAJOR( reqs.maxApiVersionSupported ), XR_VERSION_MINOR( reqs.maxApiVersionSupported ));

#if XASH_WIN32
	{
		// VID_Init has made the engine's GL context current on this thread,
		// it lives until R_Shutdown, which runs after VR_Shutdown
		XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };

		binding.hDC = wglGetCurrentDC();
		binding.hGLRC = wglGetCurrentContext();

		if( !binding.hDC || !binding.hGLRC )
		{
			Con_Printf( S_ERROR "VR: no current OpenGL context, VR needs the GL renderer\n" );
			return false;
		}

		info.next = &binding;
		info.systemId = vr.system;
		result = pxrCreateSession( vr.instance, &info, &vr.session );
	}
#else
	Con_Printf( S_ERROR "VR: OpenGL session binding is only implemented for Windows yet\n" );
	return false;
#endif

	if( XR_FAILED( result ))
	{
		vr.session = XR_NULL_HANDLE;
		Con_Printf( S_ERROR "VR: xrCreateSession failed: %s\n", VR_ResultString( result ));
		return false;
	}

	vr.session_state = XR_SESSION_STATE_UNKNOWN;
	return true;
}

static qboolean VR_CreateSpace( void )
{
	XrReferenceSpaceCreateInfo info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrReferenceSpaceType types[8];
	uint32_t count = 0, i;
	XrResult result;

	// STAGE has its origin on the floor in the middle of the play area,
	// which is what room scale wants; LOCAL (origin at the starting head
	// position) is the fallback every runtime must have
	vr.space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if( XR_SUCCEEDED( pxrEnumerateReferenceSpaces( vr.session, ARRAYSIZE( types ), &count, types )))
	{
		for( i = 0; i < count; i++ )
		{
			if( types[i] == XR_REFERENCE_SPACE_TYPE_STAGE )
				vr.space_type = XR_REFERENCE_SPACE_TYPE_STAGE;
		}
	}

	info.referenceSpaceType = vr.space_type;
	info.poseInReferenceSpace.orientation.w = 1.0f; // identity

	result = pxrCreateReferenceSpace( vr.session, &info, &vr.space );
	if( XR_FAILED( result ))
	{
		vr.space = XR_NULL_HANDLE;
		Con_Printf( S_ERROR "VR: xrCreateReferenceSpace failed: %s\n", VR_ResultString( result ));
		return false;
	}

	Con_Printf( "VR: session created, %s reference space\n",
		vr.space_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL" );
	return true;
}

static qboolean VR_LoadGL( void )
{
	static const struct { const char *name; void **func; } funcs[] =
	{
		{ "glGenFramebuffers", (void **)&vrgl.GenFramebuffers },
		{ "glDeleteFramebuffers", (void **)&vrgl.DeleteFramebuffers },
		{ "glBindFramebuffer", (void **)&vrgl.BindFramebuffer },
		{ "glFramebufferTexture2D", (void **)&vrgl.FramebufferTexture2D },
		{ "glCheckFramebufferStatus", (void **)&vrgl.CheckFramebufferStatus },
		{ "glBlitFramebuffer", (void **)&vrgl.BlitFramebuffer },
		{ "glGetIntegerv", (void **)&vrgl.GetIntegerv },
		{ "glIsEnabled", (void **)&vrgl.IsEnabled },
		{ "glEnable", (void **)&vrgl.Enable },
		{ "glDisable", (void **)&vrgl.Disable },
		{ "glGenRenderbuffers", (void **)&vrgl.GenRenderbuffers },
		{ "glDeleteRenderbuffers", (void **)&vrgl.DeleteRenderbuffers },
		{ "glBindRenderbuffer", (void **)&vrgl.BindRenderbuffer },
		{ "glRenderbufferStorage", (void **)&vrgl.RenderbufferStorage },
		{ "glFramebufferRenderbuffer", (void **)&vrgl.FramebufferRenderbuffer },
		{ "glClearColor", (void **)&vrgl.ClearColor },
		{ "glClear", (void **)&vrgl.Clear },
		{ "glViewport", (void **)&vrgl.Viewport },
		{ "glRenderbufferStorageMultisample", (void **)&vrgl.RenderbufferStorageMultisample },
	};
	size_t i;

	for( i = 0; i < ARRAYSIZE( funcs ); i++ )
	{
		*funcs[i].func = GL_GetProcAddress( funcs[i].name );
		if( !*funcs[i].func )
		{
			Con_Printf( S_ERROR "VR: OpenGL function %s is missing\n", funcs[i].name );
			return false;
		}
	}

	return true;
}

static int64_t VR_PickSwapchainFormat( void )
{
	// the engine draws gamma-encoded colors without GL_FRAMEBUFFER_SRGB, so
	// an sRGB swapchain stores them as-is and the runtime decodes them right
	static const int64_t wanted[] = { VR_GL_SRGB8_ALPHA8, VR_GL_RGBA8 };
	int64_t formats[64];
	uint32_t count = 0, i, j;

	if( XR_FAILED( pxrEnumerateSwapchainFormats( vr.session, ARRAYSIZE( formats ), &count, formats )))
		return 0;

	for( j = 0; j < ARRAYSIZE( wanted ); j++ )
	{
		for( i = 0; i < count; i++ )
		{
			if( formats[i] == wanted[j] )
				return wanted[j];
		}
	}

	return 0;
}

static vr_GLuint VR_CreateRenderbuffer( vr_GLenum format, int samples )
{
	vr_GLuint rb = 0;

	vrgl.GenRenderbuffers( 1, &rb );
	vrgl.BindRenderbuffer( VR_GL_RENDERBUFFER, rb );
	if( samples > 1 )
		vrgl.RenderbufferStorageMultisample( VR_GL_RENDERBUFFER, samples, format, vr.eye_width, vr.eye_height );
	else vrgl.RenderbufferStorage( VR_GL_RENDERBUFFER, format, vr.eye_width, vr.eye_height );
	vrgl.BindRenderbuffer( VR_GL_RENDERBUFFER, 0 );

	return rb;
}

static void VR_DestroyEyeTargets( void )
{
	vr_GLuint rbs[3] = { vr.depth, vr.msaa_color, vr.msaa_depth };
	int eye, i;

	for( i = 0; i < 3; i++ )
	{
		if( rbs[i] && vrgl.DeleteRenderbuffers )
			vrgl.DeleteRenderbuffers( 1, &rbs[i] );
	}
	vr.depth = vr.msaa_color = vr.msaa_depth = 0;
	vr.samples = 0;

	for( eye = 0; eye < 2; eye++ )
	{
		if( vr.swapchain[eye] != XR_NULL_HANDLE )
			pxrDestroySwapchain( vr.swapchain[eye] );
		vr.swapchain[eye] = XR_NULL_HANDLE;

		if( vr.images[eye] )
			Mem_Free( vr.images[eye] );
		vr.images[eye] = NULL;
		vr.num_images[eye] = 0;
	}
}

/*
VR_CreateEyeTargets

The eye swapchains at the recommended size times vr_renderscale, and
what the eyes are drawn into: with vr_msaa samples a multisampled color
and depth pair that is resolved into the swapchain image afterwards,
otherwise a plain depth buffer next to the swapchain image.
*/
static qboolean VR_CreateEyeTargets( void )
{
	float scale = bound( 0.5f, vr_renderscale.value, 2.0f );
	vr_GLint old = 0, max_samples = 0;
	int eye;

	vr.eye_width = Q_min( (uint32_t)Q_rint( vr.rec_width * scale ), vr.max_width );
	vr.eye_height = Q_min( (uint32_t)Q_rint( vr.rec_height * scale ), vr.max_height );

	for( eye = 0; eye < 2; eye++ )
	{
		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		XrResult result;
		uint32_t i;

		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.format = vr.swapchain_format;
		info.sampleCount = 1;
		info.width = vr.eye_width;
		info.height = vr.eye_height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;

		result = pxrCreateSwapchain( vr.session, &info, &vr.swapchain[eye] );
		if( XR_FAILED( result ))
		{
			vr.swapchain[eye] = XR_NULL_HANDLE;
			Con_Printf( S_ERROR "VR: xrCreateSwapchain failed: %s\n", VR_ResultString( result ));
			return false;
		}

		pxrEnumerateSwapchainImages( vr.swapchain[eye], 0, &vr.num_images[eye], NULL );
		vr.images[eye] = Mem_Calloc( host.mempool, sizeof( *vr.images[eye] ) * vr.num_images[eye] );
		for( i = 0; i < vr.num_images[eye]; i++ )
			vr.images[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;

		result = pxrEnumerateSwapchainImages( vr.swapchain[eye], vr.num_images[eye], &vr.num_images[eye],
			(XrSwapchainImageBaseHeader *)vr.images[eye] );
		if( XR_FAILED( result ))
		{
			Con_Printf( S_ERROR "VR: xrEnumerateSwapchainImages failed: %s\n", VR_ResultString( result ));
			return false;
		}
	}

	vrgl.GetIntegerv( VR_GL_DRAW_FRAMEBUFFER_BINDING, &old );

	// the eyes are drawn one after another, so one set of buffers serves both
	vr.depth = VR_CreateRenderbuffer( VR_GL_DEPTH24_STENCIL8, 0 );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
	vrgl.FramebufferRenderbuffer( VR_GL_FRAMEBUFFER, VR_GL_DEPTH_STENCIL_ATTACHMENT, VR_GL_RENDERBUFFER, vr.depth );

	vrgl.GetIntegerv( VR_GL_MAX_SAMPLES, &max_samples );
	vr.samples = bound( 0, (int)vr_msaa.value, max_samples );
	if( vr.samples > 1 )
	{
		// RGBA8 holds the same gamma-encoded bytes the sRGB swapchain stores,
		// the resolve blit copies them unchanged with FRAMEBUFFER_SRGB off
		vr.msaa_color = VR_CreateRenderbuffer( VR_GL_RGBA8, vr.samples );
		vr.msaa_depth = VR_CreateRenderbuffer( VR_GL_DEPTH24_STENCIL8, vr.samples );
		vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.msaa_fbo );
		vrgl.FramebufferRenderbuffer( VR_GL_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_RENDERBUFFER, vr.msaa_color );
		vrgl.FramebufferRenderbuffer( VR_GL_FRAMEBUFFER, VR_GL_DEPTH_STENCIL_ATTACHMENT, VR_GL_RENDERBUFFER, vr.msaa_depth );

		if( vrgl.CheckFramebufferStatus( VR_GL_FRAMEBUFFER ) != VR_GL_FRAMEBUFFER_COMPLETE )
		{
			Con_Printf( S_WARN "VR: %dx MSAA eye buffers are incomplete, continuing without MSAA\n", vr.samples );
			vrgl.DeleteRenderbuffers( 1, &vr.msaa_color );
			vrgl.DeleteRenderbuffers( 1, &vr.msaa_depth );
			vr.msaa_color = vr.msaa_depth = 0;
			vr.samples = 0;
		}
	}
	else vr.samples = 0;

	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, old );

	Con_Printf( "VR: eye swapchains %ux%u (%.2fx), %s, %u images each, %s\n", vr.eye_width, vr.eye_height, scale,
		vr.swapchain_format == VR_GL_SRGB8_ALPHA8 ? "SRGB8_ALPHA8" : "RGBA8", vr.num_images[0],
		vr.samples ? va( "%dx MSAA", vr.samples ) : "no MSAA" );
	return true;
}

/*
VR_CreatePanel

The loading panel's swapchain, made once per session. It's small and
doesn't depend on vr_renderscale or vr_msaa. Without it loading still
works, the headset just gets no layers then.
*/
static void VR_CreatePanel( void )
{
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	XrResult result;
	uint32_t i;

	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format = vr.swapchain_format;
	info.sampleCount = 1;
	info.width = VR_PANEL_WIDTH;
	info.height = VR_PANEL_HEIGHT;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;

	result = pxrCreateSwapchain( vr.session, &info, &vr.panel_swapchain );
	if( XR_FAILED( result ))
	{
		vr.panel_swapchain = XR_NULL_HANDLE;
		Con_Printf( S_WARN "VR: can't create the loading panel: %s\n", VR_ResultString( result ));
		return;
	}

	pxrEnumerateSwapchainImages( vr.panel_swapchain, 0, &vr.panel_num_images, NULL );
	vr.panel_images = Mem_Calloc( host.mempool, sizeof( *vr.panel_images ) * vr.panel_num_images );
	for( i = 0; i < vr.panel_num_images; i++ )
		vr.panel_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;

	result = pxrEnumerateSwapchainImages( vr.panel_swapchain, vr.panel_num_images, &vr.panel_num_images,
		(XrSwapchainImageBaseHeader *)vr.panel_images );
	if( XR_FAILED( result ))
	{
		Con_Printf( S_WARN "VR: can't get the loading panel images: %s\n", VR_ResultString( result ));
		pxrDestroySwapchain( vr.panel_swapchain );
		vr.panel_swapchain = XR_NULL_HANDLE;
		Mem_Free( vr.panel_images );
		vr.panel_images = NULL;
		vr.panel_num_images = 0;
	}
}

static void VR_DestroyPanel( void )
{
	if( vr.panel_swapchain != XR_NULL_HANDLE )
		pxrDestroySwapchain( vr.panel_swapchain );
	vr.panel_swapchain = XR_NULL_HANDLE;

	if( vr.panel_images )
		Mem_Free( vr.panel_images );
	vr.panel_images = NULL;
	vr.panel_num_images = 0;
	vr.panel_valid = false;
}

static qboolean VR_CreateSwapchains( void )
{
	if( !VR_LoadGL( ))
		return false;

	vr.swapchain_format = VR_PickSwapchainFormat( );
	if( !vr.swapchain_format )
	{
		Con_Printf( S_ERROR "VR: runtime offers no RGBA8 or SRGB8_ALPHA8 swapchain format\n" );
		return false;
	}

	vrgl.GenFramebuffers( 1, &vr.fbo );
	vrgl.GenFramebuffers( 1, &vr.msaa_fbo );

	ClearBits( vr_renderscale.flags, FCVAR_CHANGED );
	ClearBits( vr_msaa.flags, FCVAR_CHANGED );
	VR_CreatePanel( );
	return VR_CreateEyeTargets( );
}

static void VR_DestroySwapchains( void )
{
	VR_DestroyEyeTargets( );
	VR_DestroyPanel( );

	if( vr.fbo && vrgl.DeleteFramebuffers )
		vrgl.DeleteFramebuffers( 1, &vr.fbo );
	if( vr.msaa_fbo && vrgl.DeleteFramebuffers )
		vrgl.DeleteFramebuffers( 1, &vr.msaa_fbo );
	vr.fbo = vr.msaa_fbo = 0;
}

/*
VR_CheckEyeTargets

vr_renderscale and vr_msaa can change at any time (the config is read
after VR starts), the eye targets are rebuilt between frames when they do
*/
static void VR_CheckEyeTargets( void )
{
	if( !FBitSet( vr_renderscale.flags|vr_msaa.flags, FCVAR_CHANGED ))
		return;

	ClearBits( vr_renderscale.flags, FCVAR_CHANGED );
	ClearBits( vr_msaa.flags, FCVAR_CHANGED );

	VR_DestroyEyeTargets( );
	if( !VR_CreateEyeTargets( ))
	{
		Con_Printf( S_ERROR "VR: can't rebuild the eye swapchains, VR disabled\n" );
		VR_Shutdown( );
	}
}

static void VR_LocateViews( void )
{
	XrViewLocateInfo info = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState state = { XR_TYPE_VIEW_STATE };
	uint32_t count = 0;

	vr.views[0].type = vr.views[1].type = XR_TYPE_VIEW;
	vr.views[0].next = vr.views[1].next = NULL;

	info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	info.displayTime = vr.display_time;
	info.space = vr.space;

	vr.views_valid = XR_SUCCEEDED( pxrLocateViews( vr.session, &info, &state, 2, &count, vr.views ))
		&& count == 2
		&& FBitSet( state.viewStateFlags, XR_VIEW_STATE_POSITION_VALID_BIT )
		&& FBitSet( state.viewStateFlags, XR_VIEW_STATE_ORIENTATION_VALID_BIT );

	if( vr.views_valid && !vr.logged_fov )
	{
		int eye;

		// the per-eye angles are fixed by the headset's lenses, log them once
		for( eye = 0; eye < 2; eye++ )
		{
			const XrFovf *f = &vr.views[eye].fov;
			const XrVector3f *p = &vr.views[eye].pose.position;
			Con_Printf( "VR: %s eye fov: left %.1f, right %.1f, down %.1f, up %.1f degrees\n",
				eye ? "right" : "left",
				f->angleLeft * ( 180.0f / M_PI_F ), f->angleRight * ( 180.0f / M_PI_F ),
				f->angleDown * ( 180.0f / M_PI_F ), f->angleUp * ( 180.0f / M_PI_F ));
			Con_Printf( "VR: %s eye at %.3f %.3f %.3f m in the %s space\n", eye ? "right" : "left",
				p->x, p->y, p->z, vr.space_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL" );
		}

		{
			// canted displays rotate the eyes against each other; sprites and
			// particles face each eye separately then, which can shimmer
			const XrQuaternionf *a = &vr.views[0].pose.orientation, *b = &vr.views[1].pose.orientation;
			float dot = fabsf( a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w );
			Con_Printf( "VR: eyes are canted %.2f degrees against each other\n",
				2.0f * acosf( Q_min( dot, 1.0f )) * ( 180.0f / M_PI_F ));
		}
		vr.logged_fov = true;
	}
}

/*
OpenXR spaces are right-handed with +X right, +Y up and -Z forward,
Half-Life has +X forward, +Y left and +Z up
*/
static void VR_VectorToGame( const XrVector3f *in, vec3_t out )
{
	out[0] = -in->z;
	out[1] = -in->x;
	out[2] = in->y;
}

static void VR_RotateVector( const XrQuaternionf *q, const XrVector3f *v, XrVector3f *out )
{
	// v' = v + 2w(q x v) + 2(q x (q x v))
	XrVector3f t, u;

	t.x = 2.0f * ( q->y * v->z - q->z * v->y );
	t.y = 2.0f * ( q->z * v->x - q->x * v->z );
	t.z = 2.0f * ( q->x * v->y - q->y * v->x );

	u.x = q->y * t.z - q->z * t.y;
	u.y = q->z * t.x - q->x * t.z;
	u.z = q->x * t.y - q->y * t.x;

	out->x = v->x + q->w * t.x + u.x;
	out->y = v->y + q->w * t.y + u.y;
	out->z = v->z + q->w * t.z + u.z;
}

static void VR_YawVector( const vec3_t in, float sy, float cy, vec3_t out )
{
	out[0] = in[0] * cy - in[1] * sy;
	out[1] = in[0] * sy + in[1] * cy;
	out[2] = in[2];
}

// eye or head position in game units and play space axes (not yet turned
// by the player's yaw), with the height offset and debug head move added
static void VR_PlayPosition( const XrVector3f *pos, vec3_t out )
{
	float scale = Q_max( 1.0f, vr_worldscale.value );
	float move[2] = { 0.0f, 0.0f };

	VR_VectorToGame( pos, out );
	out[2] += vr_heightoffset.value;

	if( sscanf( vr_debug_headmove.string, "%f %f", &move[0], &move[1] ) == 2 )
	{
		out[0] += move[0];
		out[1] += move[1];
	}

	VectorScale( out, scale, out );
}

static void VR_HeadCenter( vec3_t out )
{
	vec3_t l, r;

	VR_PlayPosition( &vr.views[0].pose.position, l );
	VR_PlayPosition( &vr.views[1].pose.position, r );
	VectorAverage( l, r, out );
}

// bottom of the player's collision box
static float VR_BoxBottom( void )
{
	return cl.simorg[2] + (( cl.local.usehull == 1 ) ? -18.0f : -36.0f ); // ducked or standing hull bottom
}

/*
VR_UpdateStairs

Once per rendered frame. Player movement lifts the body up each stair
(up to 18 units) in a single frame; the flat game hides that with view
smoothing in V_CalcRefdef, but in VR the whole world would pop under the
real head. So the height the eyes and hands are placed from follows the
box bottom smoothly while walking on the ground, never more than one
step behind. Jumps, falls, swimming and big jumps in height (teleports,
respawns) are followed exactly.

Ducking in the air keeps the origin and tucks the legs up, which lifts the
box bottom by 18 units; on the ground ducking lowers the origin instead
and the bottom stays. So a duck or unduck in the air is carried in
vr.air_tuck, and the view doesn't jump (auto-crouch ducks on every jump).
It fades during the flight, and on landing the rest is dropped and the
stair smoothing takes the view to the floor, so landing with tucked legs
doesn't dip the view by the whole 18 units.
*/
static void VR_UpdateStairs( void )
{
	float raw, diff;
	int hull = cl.local.usehull;

	if( cls.state != ca_active || cl.local.onground != -1 )
		vr.air_tuck = 0.0f;
	else
	{
		if( vr.prev_hull >= 0 && hull != vr.prev_hull )
			vr.air_tuck += ( hull == 1 ) ? -18.0f : 18.0f; // the bottom moved, the body didn't

		// fade it out during the flight, so a tucked landing doesn't dip the
		// view 18 units below standing height before the player stands up
		vr.air_tuck *= expf( -(float)host.realframetime * VR_TUCK_RATE );
	}
	vr.prev_hull = ( cls.state == ca_active ) ? hull : -1;

	raw = VR_BoxBottom( ) + vr.air_tuck;
	diff = raw - vr.feet_z;

	if( !vr_stairsmooth.value || cls.state != ca_active || !vr.feet_valid
		|| cl.local.onground == -1 || cl.local.waterlevel >= 2 || fabsf( diff ) > VR_STEP_SNAP )
	{
		vr.feet_z = raw;
		vr.feet_valid = cls.state == ca_active;
		return;
	}

	vr.feet_z += diff * ( 1.0f - expf( -(float)host.realframetime * VR_STAIR_RATE ));
	vr.feet_z = bound( raw - VR_STEP_MAX, vr.feet_z, raw + VR_STEP_MAX );
	vr.stair_lag_max = Q_max( vr.stair_lag_max, fabsf( raw - vr.feet_z ));
}

/*
VR_UpdateDuckView

Once per rendered frame, after the head is located. A ducked player is
only 36 units tall (~0.9 m), less than a real crouching head, so in a
vent the head would poke into the ceiling and the view would fade.
While ducked the view is lowered, smoothly, until the eyes are no higher
than the game's own ducked eye height; standing up raises it again. A
player who crouches really low isn't lowered any further.
*/
static void VR_UpdateDuckView( float head_height )
{
	float target = 0.0f;

	vr.head_height = head_height;

	// not in the air: auto-crouch ducks every jump, and a jump shouldn't dip the view
	if( cls.state == ca_active && cl.local.usehull == 1 && cl.local.onground != -1 )
		target = Q_max( 0.0f, head_height - VR_DUCK_EYE );

	vr.duck_lower += ( target - vr.duck_lower ) * ( 1.0f - expf( -(float)host.realframetime * VR_DUCK_RATE ));
	if( fabsf( target - vr.duck_lower ) < 0.05f )
		vr.duck_lower = target;
}

qboolean VR_PhysicalCrouch( void )
{
	if( !vr.running || !vr.views_valid || vr_crouchheight.value <= 0.0f || cls.state != ca_active )
		return false;

	return vr.head_height < vr_crouchheight.value * Q_max( 1.0f, vr_worldscale.value );
}

/*
VR_Calibrate_f

Stand (or sit) normally and run it: sets vr_heightoffset so the current
head height becomes the game's standing eye height. That fits the game
to the player's real height, and is how seated play works: sit, and the
view still starts at standing height. Physical crouching (vr_crouchheight)
is measured after the offset, so it follows along.
*/
static void VR_Calibrate_f( void )
{
	float scale = Q_max( 1.0f, vr_worldscale.value );
	float real, offset;

	if( !vr.running || !vr.views_valid )
	{
		Con_Printf( "VR: the headset isn't tracking, can't calibrate\n" );
		return;
	}

	real = ( vr.views[0].pose.position.y + vr.views[1].pose.position.y ) * 0.5f; // metres above the play space floor
	offset = VR_DUCK_EYE_STAND / scale - real;
	Cvar_SetValue( "vr_heightoffset", offset );
	Con_Printf( "VR: head at %.2f m, vr_heightoffset set to %.2f m so it's at the game's eye height (%.2f m)\n",
		real, offset, VR_DUCK_EYE_STAND / scale );
}

// where the STAGE floor is: the bottom of the collision box, with steps
// smoothed, lowered while ducked so the head fits the ducked box
static void VR_PlayerFeet( vec3_t out )
{
	VectorCopy( cl.simorg, out );
	out[2] = ( vr.feet_valid ? vr.feet_z : VR_BoxBottom( )) - vr.duck_lower;
}

static void VR_ResetRoom( void )
{
	VectorClear( vr.room_gap );
	vr.head_prev_valid = false;
	vr.fade = 0.0f;
}

static void VR_Recenter_f( void )
{
	VR_ResetRoom( );
	Con_Printf( "VR: recentered, the player is under the head again\n" );
}

/*
VR_UpdateRoom

Once per rendered frame: the head's horizontal movement since the last
frame, turned into world axes with the current yaw, is added to the gap
between the player's collision box and the head. Using the current yaw
for both positions means turning with the mouse or stick never shows up
as head movement. The box catches up through VR_RoomRequest.
*/
static void VR_UpdateRoom( float yaw_degrees )
{
	float yaw = DEG2RAD( yaw_degrees );
	vec3_t head, delta, world;

	if( cls.state != ca_active || !vr_roomscale.value || vr.space_type != XR_REFERENCE_SPACE_TYPE_STAGE )
	{
		VR_ResetRoom( );
		return;
	}

	VR_HeadCenter( head );

	if( vr.head_prev_valid )
	{
		VectorSubtract( head, vr.head_prev, delta );
		delta[2] = 0.0f;
		VR_YawVector( delta, sinf( yaw ), cosf( yaw ), world );
		VectorAdd( vr.room_gap, world, vr.room_gap );
	}

	VectorCopy( head, vr.head_prev );
	vr.head_prev_valid = true;
}

/*
VR_UpdateFade

Head-in-wall fade: trace from the collision box's eye point to the real
head. If something solid is in between, the head is in or past a wall,
and the view fades to black over the first few units of depth, so the
player can't look through walls by leaning or walking into them.
*/
static void VR_UpdateFade( const vec3_t head_world )
{
	vec3_t start, dir;
	pmtrace_t tr;
	float len, depth;

	vr.fade = 0.0f;

	if( cls.state != ca_active || !cl.worldmodel )
		return;

	VectorCopy( cl.simorg, start );
	start[2] += cl.viewheight[2];

	VectorSubtract( head_world, start, dir );
	len = VectorLength( dir );
	if( len < 1.0f )
		return;

	// solid geometry only (point hull, studio models skipped); not
	// PM_CL_TraceLine, whose third argument is a mode, so PM_STUDIO_IGNORE
	// there would trace every visible entity
	tr = CL_TraceLine( start, (float *)head_world, PM_STUDIO_IGNORE );
	if( tr.fraction >= 1.0f && !tr.allsolid )
		return;

	depth = tr.allsolid ? len : ( 1.0f - tr.fraction ) * len;
	vr.fade = bound( 0.0f, depth / 6.0f, 1.0f ); // black after 6 units (~15 cm) into the wall
}

static void VR_DrawFade( void )
{
	// the head-in-wall fade and the fade-in after loading share one black overlay
	float fade = Q_max( vr.fade, vr.load_fade );

	if( fade <= 0.0f )
		return;

	// the renderer's 2D mode maps window coordinates onto the viewport,
	// so the window rectangle covers the whole eye image
	ref.dllFuncs.R_Set2DMode( true );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo ); // 2D mode may bind a screen target
	vrgl.Viewport( 0, 0, vr.eye_width, vr.eye_height );
	vrgl.Disable( VR_GL_ALPHA_TEST );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, 0, refState.width, refState.height, 0, 0, 0, (byte)( fade * 255.0f ));
	ref.dllFuncs.R_Set2DMode( false );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
}

/*
VR_RoomRequest

Called by CL_CreateCmd: asks player movement to move the collision box
under the head. The request travels in usercmd_t.impact_position, which
the netcode already sends and Half-Life never uses; the game's
pm_shared moves the box with full collision and writes back how far it
really got (see VR_RoomApplied). Only while the client predicts
movement, otherwise that answer never arrives and the box would move
twice.
*/
void VR_RoomRequest( usercmd_t *cmd )
{
	float len;
	vec3_t step;

	if( !vr.running || !vr_roomscale.value || cls.state != ca_active || cl_nopred.value || cl.intermission )
		return;

	VectorCopy( vr.room_gap, step );
	step[2] = 0.0f;
	len = VectorLength( step );
	if( len < 0.125f ) // below the netcode's 1/8 unit precision
		return;

	if( len > VR_ROOM_MAX_STEP )
		VectorScale( step, VR_ROOM_MAX_STEP / len, step );

	VR_CMD_ROOMMOVE( cmd )[0] = step[0];
	VR_CMD_ROOMMOVE( cmd )[1] = step[1];
	VR_CMD_ROOMMOVE( cmd )[2] = 0.0f;
}

/*
VR_RoomApplied

Called after a movement command was predicted for the first time, with
pm_shared's answer: how far the box really moved toward the head.
*/
void VR_RoomApplied( const float *moved )
{
	if( !vr.running )
		return;

	vr.room_gap[0] -= moved[0];
	vr.room_gap[1] -= moved[1];
}

/*
VR_EyeView

Places one eye in the game: the player's feet (predicted origin, no view
bob) plus the room-scale gap to the head, plus the eye's offset from the
head center and its height above the STAGE floor, scaled to game units
and turned by the player's yaw. The angles come from the headset alone,
so there is no view bob, punch angle or roll in the eyes. Each eye has
its own pose, so the real eye distance gives the stereo offset.
*/
static void VR_EyeView( const ref_viewpass_t *rvp, const XrPosef *pose, const vec3_t head, vec3_t origin, vec3_t angles )
{
	static const XrVector3f xr_forward = { 0.0f, 0.0f, -1.0f };
	static const XrVector3f xr_right = { 1.0f, 0.0f, 0.0f };
	static const XrVector3f xr_up = { 0.0f, 1.0f, 0.0f };
	float yaw = DEG2RAD( rvp->viewangles[YAW] );
	float sy = sinf( yaw ), cy = cosf( yaw );
	vec3_t base, offset, forward, right, up, v;
	XrVector3f axis;

	if( vr.space_type == XR_REFERENCE_SPACE_TYPE_STAGE )
	{
		// STAGE height is measured from the real floor, so start at the feet;
		// horizontally the head is wherever room scale says it is
		VR_PlayerFeet( base );
		base[0] += vr.room_gap[0];
		base[1] += vr.room_gap[1];

		VR_PlayPosition( &pose->position, v );
		v[0] -= head[0]; // eye relative to the head center, height stays absolute
		v[1] -= head[1];
	}
	else
	{
		// LOCAL starts at the head, keep the game's eye height
		VectorCopy( rvp->vieworigin, base );
		VR_PlayPosition( &pose->position, v );
	}

	VR_YawVector( v, sy, cy, offset );
	VectorAdd( base, offset, origin );

	VR_RotateVector( &pose->orientation, &xr_forward, &axis );
	VR_VectorToGame( &axis, v );
	VR_YawVector( v, sy, cy, forward );

	VR_RotateVector( &pose->orientation, &xr_right, &axis );
	VR_VectorToGame( &axis, v );
	VR_YawVector( v, sy, cy, right );

	VR_RotateVector( &pose->orientation, &xr_up, &axis );
	VR_VectorToGame( &axis, v );
	VR_YawVector( v, sy, cy, up );

	VectorsAngles( forward, right, up, angles );
}

/*
VR_FakeHandPose

vr_debug_hands: a hand held level in front of the head, 20 cm to its side,
45 cm forward and 20 cm down (about 24 degrees below the eye, inside even
a narrow view), pointing where the head faces
*/
static void VR_FakeHandPose( int hand, XrPosef *out )
{
	static const XrVector3f xr_forward = { 0.0f, 0.0f, -1.0f };
	XrVector3f head, f, axis;
	float len, side = ( hand == VR_HAND_RIGHT ) ? 0.2f : -0.2f, theta;

	head.x = ( vr.views[0].pose.position.x + vr.views[1].pose.position.x ) * 0.5f;
	head.y = ( vr.views[0].pose.position.y + vr.views[1].pose.position.y ) * 0.5f;
	head.z = ( vr.views[0].pose.position.z + vr.views[1].pose.position.z ) * 0.5f;

	VR_RotateVector( &vr.views[0].pose.orientation, &xr_forward, &axis );
	f.x = axis.x; f.y = 0.0f; f.z = axis.z;
	len = sqrtf( f.x * f.x + f.z * f.z );
	if( len < 0.001f ) { f.x = 0.0f; f.z = -1.0f; len = 1.0f; }
	f.x /= len; f.z /= len;

	// right = forward x up = ( -f.z, 0, f.x )
	out->position.x = head.x + side * -f.z + 0.45f * f.x;
	out->position.y = head.y - 0.2f;
	out->position.z = head.z + side * f.x + 0.45f * f.z;

	// yaw only, around +Y, so that -Z turns into f
	theta = atan2f( -f.x, -f.z );
	out->orientation.x = 0.0f;
	out->orientation.y = sinf( theta * 0.5f );
	out->orientation.z = 0.0f;
	out->orientation.w = cosf( theta * 0.5f );
}

/*
VR_HandTrace

Point trace against the world and brush entities (doors, lifts, trains),
ignoring studio models so hands don't snag on NPCs. On a hit, end is the
hit point moved off the surface by the hand's radius; returns the
surface normal in normal, or false if nothing was hit.
*/
static qboolean VR_HandTrace( const vec3_t start, vec3_t end, vec3_t normal )
{
	pmtrace_t tr = CL_TraceLine( (float *)start, end, PM_STUDIO_IGNORE );

	if( tr.fraction >= 1.0f && !tr.startsolid && !tr.allsolid )
		return false;

	VectorCopy( tr.plane.normal, normal );
	VectorMA( tr.endpos, VR_HAND_RADIUS, normal, end );
	return true;
}

/*
VR_MuzzleTable

Fallback when a weapon model's shape can't be read (no vertices, its idle
animation stored in a separate file): how far it reaches past the hand
along the aim, in game units. Others fall back to vr_weapon_length.
*/
static const struct
{
	const char *model;
	float length;
} vr_muzzle_table[] =
{
	{ "v_crowbar", 16.0f },
	{ "v_crossbow", 20.0f },
	{ "v_rpg", 20.0f },
	{ "v_hgun", 10.0f },
	{ "v_grenade", 4.0f },
	{ "v_satchel", 6.0f },
	{ "v_tripmine", 8.0f },
	{ "v_squeak", 6.0f },
};

/*
VR_WeaponTip

The point of a weapon model that reaches furthest forward (the end of
the barrel), in model space, from its real shape: the skeleton is posed
at the first frame of the first animation (the idle) and every vertex of
the default body is moved onto its bone. Neither the muzzle attachment
(often partway down the barrel, missing on the crossbow and RPG) nor the
model's bounding boxes (set by hand per animation) are reliable enough.
Runs once per weapon model, when it's first held.
*/
#define VR_MAX_TIP_BONES 128
static qboolean VR_WeaponTip( model_t *model, vec3_t tip )
{
	studiohdr_t *hdr = Mod_StudioExtradata( model );
	static matrix3x4 bones[VR_MAX_TIP_BONES];
	mstudiobodyparts_t *parts;
	qboolean found = false;
	int i, j;

	if( !VR_IdleBones( hdr, bones, VR_MAX_TIP_BONES ))
		return false;

	parts = (mstudiobodyparts_t *)((byte *)hdr + hdr->bodypartindex);
	for( i = 0; i < hdr->numbodyparts; i++ )
	{
		mstudiomodel_t *sub;
		const vec3_t *verts;
		const byte *vertbone;

		if( parts[i].nummodels <= 0 )
			continue;

		sub = (mstudiomodel_t *)((byte *)hdr + parts[i].modelindex); // the default body
		verts = (const vec3_t *)((byte *)hdr + sub->vertindex);
		vertbone = (const byte *)hdr + sub->vertinfoindex;

		for( j = 0; j < sub->numverts; j++ )
		{
			vec3_t v;

			if( vertbone[j] >= hdr->numbones )
				continue;

			Matrix3x4_VectorTransform( bones[vertbone[j]], verts[j], v );
			if( !found || v[0] > tip[0] )
			{
				VectorCopy( v, tip );
				found = true;
			}
		}
	}

	return found;
}

/*
VR_MeasureMuzzle

Where the end of the weapon is relative to the hand, in the hand's axes
(forward, right, up), for the barrel and head checks. Worked out once per
weapon model from its real shape (VR_WeaponTip); models whose shape
can't be read use VR_MuzzleTable or vr_weapon_length straight along the
aim.
*/
static void VR_MeasureMuzzle( void )
{
	cl_entity_t *view = &clgame.viewent;
	float g[3] = { 0.0f, 0.0f, 0.0f };
	vec3_t tip;
	size_t i;

	if( !view->model || view->model->type != mod_studio )
	{
		vr.muzzle_model = NULL;
		return;
	}

	if( view->model == vr.muzzle_model && !FBitSet( vr_viewmodel_grip.flags, FCVAR_CHANGED ))
		return;

	ClearBits( vr_viewmodel_grip.flags, FCVAR_CHANGED );
	vr.muzzle_model = view->model;
	VectorClear( vr.muzzle_local );

	if( VR_WeaponTip( view->model, tip ))
	{
		// model space is forward, left, up; vr_viewmodel_grip is forward, right, up
		if( !VR_IsVRWeaponModel( view->model ))
			sscanf( vr_viewmodel_grip.string, "%f %f %f", &g[0], &g[1], &g[2] );
		vr.muzzle_local[0] = tip[0] - g[0];
		vr.muzzle_local[1] = -tip[1] - g[1];
		vr.muzzle_local[2] = tip[2] - g[2];
		vr.muzzle_measured = true;
		return;
	}

	vr.muzzle_measured = false;
	vr.muzzle_local[0] = Q_max( 0.0f, vr_weapon_length.value );
	for( i = 0; i < ARRAYSIZE( vr_muzzle_table ); i++ )
	{
		if( Q_stristr( view->model->name, vr_muzzle_table[i].model ))
			vr.muzzle_local[0] = vr_muzzle_table[i].length;
	}
}

// the muzzle's offset from a hand at angles, in world axes
static void VR_MuzzleOffset( const vec3_t angles, vec3_t out )
{
	vec3_t f, r, u;

	AngleVectors( angles, f, r, u );
	VectorScale( f, vr.muzzle_local[0], out );
	VectorMA( out, vr.muzzle_local[1], r, out );
	VectorMA( out, vr.muzzle_local[2], u, out );
}

/*
VR_HeadPush

Keeps a hand, or the weapon from just behind the grip to the muzzle, at
least a head radius away from the head center, so it can't be pushed
into the player's own face. Moves pos straight away from the head;
returns true if it had to.
*/
static qboolean VR_HeadPush( int hand, qboolean weapon, vec3_t pos )
{
	vec3_t a, b, ab, c, away, f;
	float t, d, radius = weapon ? VR_HEAD_RADIUS_GUN : VR_HEAD_RADIUS_HAND;

	VectorCopy( pos, a );
	VectorCopy( pos, b );
	if( weapon )
	{
		vec3_t muzzle;

		AngleVectors( vr.hand_angles[hand], f, NULL, NULL );
		VectorMA( pos, -VR_GUN_REAR, f, a );
		VR_MuzzleOffset( vr.hand_angles[hand], muzzle );
		VectorAdd( pos, muzzle, b );
	}

	// the point of a-b closest to the head
	VectorSubtract( b, a, ab );
	t = DotProduct( ab, ab );
	t = t > 0.0001f ? bound( 0.0f, ( DotProduct( vr.head_origin, ab ) - DotProduct( a, ab )) / t, 1.0f ) : 0.0f;
	VectorMA( a, t, ab, c );

	VectorSubtract( c, vr.head_origin, away );
	d = VectorLength( away );
	if( d >= radius )
		return false;

	if( d < 0.01f )
	{
		// dead center: push the hand away from the head instead
		VectorSubtract( pos, vr.head_origin, away );
		d = VectorLength( away );
		if( d < 0.01f )
		{
			AngleVectors( vr.head_angles, away, NULL, NULL );
			d = 1.0f;
		}
		VectorScale( away, 1.0f / d, away );
		VectorMA( pos, radius, away, pos );
		return true;
	}

	VectorMA( pos, ( radius - d ) / d, away, pos );
	return true;
}

/*
VR_CollideHand

Keeps the drawn hand out of walls (simplified Alyx style) and out of the
player's own head. The tracked position is vr.hand_tracked; what
everything else uses (the weapon model, where shots start, vr_info) is
vr.hand_origin, the drawn position:

- the target is the tracked position, pushed out of the head first.
- the hand moves from where it was drawn last frame toward the target;
  a wall stops it, and the rest of the move slides along the wall once,
  so a hand pressed into a wall can still move sideways.
- the gun hand also traces from the grip to the end of the barrel (from
  the weapon model's shape, see VR_MeasureMuzzle): if the barrel would go into a
  wall, the hand is pushed back along that line, so the muzzle stops at
  the surface and shots never start behind a wall. If that pushes the
  gun into the head, walls win.
- once the target is clear again, the drawn hand eases back to it
  instead of snapping.
- the move starts at the player's eyes instead (the collision box, which
  is never inside a wall) when there was no hand last frame, or the hands
  are more than VR_HAND_RESET apart: a snap turn, a teleport, a level
  change, or a door that closed on the hand.

Only the position changes; the hand keeps the tracked rotation.
*/
static void VR_CollideHand( int hand, qboolean weapon )
{
	vec3_t target, anchor, end, normal, rest;
	pmtrace_t tr;
	qboolean blocked = false;
	float d;

	vr.hand_blocked[hand] = false;
	vr.hand_at_head[hand] = false;

	if( !vr_handcollide.value || cls.state != ca_active || !cl.worldmodel )
	{
		VectorCopy( vr.hand_tracked[hand], vr.hand_origin[hand] );
		vr.hand_prev_valid[hand] = false;
		vr.hand_separated[hand] = false;
		return;
	}

	VectorCopy( vr.hand_tracked[hand], target );
	vr.hand_at_head[hand] = VR_HeadPush( hand, weapon, target );

	// where the move starts: last frame's drawn hand if it's still usable
	if( vr.hand_prev_valid[hand] && VectorDistance( vr.hand_origin[hand], target ) < VR_HAND_RESET
		&& !CL_TraceLine( vr.hand_origin[hand], vr.hand_origin[hand], PM_STUDIO_IGNORE ).startsolid )
	{
		VectorCopy( vr.hand_origin[hand], anchor );
	}
	else
	{
		VectorCopy( cl.simorg, anchor );
		anchor[2] += cl.viewheight[2];
		vr.hand_separated[hand] = false;
	}

	VectorCopy( target, end );
	if( VR_HandTrace( anchor, end, normal ))
	{
		// slide the rest of the move along the wall, once
		blocked = true;
		VectorSubtract( target, end, rest );
		d = DotProduct( rest, normal );
		VectorMA( rest, -d, normal, rest );
		VectorCopy( end, anchor );
		VectorAdd( anchor, rest, end );
		VR_HandTrace( anchor, end, normal );
	}

	if( weapon )
	{
		// the barrel: push the hand back along grip -> muzzle until the muzzle is out
		vec3_t offset, muzzle, dir;
		float len;

		VR_MuzzleOffset( vr.hand_angles[hand], offset );
		len = VectorLength( offset );

		if( len > 0.1f )
		{
			VectorScale( offset, 1.0f / len, dir );
			VectorMA( end, len + VR_BARREL_MARGIN, dir, muzzle );
			tr = CL_TraceLine( end, muzzle, PM_STUDIO_IGNORE );

			if( tr.fraction < 1.0f && !tr.startsolid )
			{
				vec3_t back;

				blocked = true;
				VectorMA( end, -( 1.0f - tr.fraction ) * ( len + VR_BARREL_MARGIN ), dir, back );
				VectorCopy( end, anchor );
				VR_HandTrace( anchor, back, normal ); // never through a wall behind the hand
				VectorCopy( back, end );

				// pushed back into the face? move it out again, but walls win
				VectorCopy( end, back );
				if( VR_HeadPush( hand, weapon, back ))
				{
					VectorCopy( end, anchor );
					VR_HandTrace( anchor, back, normal );
					VectorCopy( back, end );
					vr.hand_at_head[hand] = true;
				}
			}
		}
	}

	if( blocked )
	{
		vr.hand_separated[hand] = true;
		vr.hand_blocked[hand] = true;
	}
	else if( vr.hand_separated[hand] )
	{
		// clear again: ease back to the target over a few frames; the path
		// toward it was just traced clear
		float k = 1.0f - expf( -(float)host.realframetime * VR_HAND_EASE );

		VectorSubtract( target, vr.hand_origin[hand], rest );
		VectorMA( vr.hand_origin[hand], k, rest, end );

		if( VectorDistance( end, target ) < 0.25f )
		{
			VectorCopy( target, end );
			vr.hand_separated[hand] = false;
		}
	}

	VectorCopy( end, vr.hand_origin[hand] );
	vr.hand_prev_valid[hand] = true;
}

// a hand-axes vector (forward, right, up) into world axes and back
static void VR_GunToWorld( const vec3_t f, const vec3_t r, const vec3_t u, const vec3_t in, vec3_t out )
{
	VectorScale( f, in[0], out );
	VectorMA( out, in[1], r, out );
	VectorMA( out, in[2], u, out );
}

static void VR_WorldToGun( const vec3_t f, const vec3_t r, const vec3_t u, const vec3_t in, vec3_t out )
{
	vec3_t t;

	VectorSet( t, DotProduct( in, f ), DotProduct( in, r ), DotProduct( in, u ));
	VectorCopy( t, out );
}

/*
VR_PushOutOfBox

Keeps a sphere of VR_OFFHAND_RADIUS at pos (gun axes) out of one box.
With the center outside the box it moves straight away from the nearest
point; with the center inside, it leaves through the face prev (last
frame's position) was beyond, so a hand pushed through a thin part
doesn't pop out the far side. Returns true if it moved pos.
*/
static qboolean VR_PushOutOfBox( const vr_weapon_box_t *box, vec3_t pos, const vec3_t prev, qboolean prev_valid )
{
	vec3_t d, p, q, pp;
	float dist, best = -1e9f;
	int j, axis = 0;
	qboolean inside = true;

	VectorSubtract( pos, box->center, d );
	for( j = 0; j < 3; j++ )
	{
		p[j] = DotProduct( d, box->axis[j] );
		q[j] = bound( -box->half[j], p[j], box->half[j] );
		if( q[j] != p[j] )
			inside = false;
	}

	if( !inside )
	{
		VectorSubtract( p, q, d );
		dist = VectorLength( d );
		if( dist >= VR_OFFHAND_RADIUS )
			return false;
		VectorMA( q, VR_OFFHAND_RADIUS / dist, d, p );
	}
	else
	{
		// the face the hand came through, or the nearest one
		if( prev_valid )
		{
			VectorSubtract( prev, box->center, d );
			for( j = 0; j < 3; j++ )
				pp[j] = DotProduct( d, box->axis[j] );
		}
		else VectorCopy( p, pp );

		for( j = 0; j < 3; j++ )
		{
			float out = fabsf( pp[j] ) - box->half[j];

			if( out > best )
			{
				best = out;
				axis = j;
			}
		}

		p[axis] = ( pp[axis] < 0.0f ? -1.0f : 1.0f ) * ( box->half[axis] + VR_OFFHAND_RADIUS );
	}

	VectorCopy( box->center, pos );
	for( j = 0; j < 3; j++ )
		VectorMA( pos, p[j], box->axis[j], pos );
	return true;
}

/*
VR_OffhandWeapon

After both hands are placed: the left hand and the weapon in the right
hand, for our VR weapon models (VR_WeaponShape; Half-Life's own models
include their arms, so they're left alone).

- On a grab point: the left hand sits on it, turned and posed as the
  grab point says, and moves with the gun. It takes hold when the
  controller comes within the grab point's radius while grip is squeezed
  (or right away with vr_grab_needgrip 0), and lets go when grip is
  released or the controller is pulled VR_GRAB_BREAK further away.
- Reaching for a grab point (the controller within its radius): no
  collision, so the hand can get there.
- Otherwise the palm is kept out of the gun's boxes (VR_PushOutOfBox),
  with walls winning over the gun.

Moving the drawn hand marks it separated, so VR_CollideHand eases it back
to the controller next frame instead of snapping.
*/
static void VR_OffhandWeapon( void )
{
	const int off = VR_HAND_LEFT, gun = VR_HAND_RIGHT;
	const model_t *model = clgame.viewent.model;
	const vr_weapon_shape_t *shape;
	vec3_t f, r, u, rel, tracked, pos, world, normal;
	float trigger, squeeze, best;
	qboolean moved = false;
	int i, iter;

	vr.hand_pose[off] = vr.hand_pose[gun] = -1;
	vr.grab_near = -1;
	vr.offhand_on_gun = false;

	if( !vr_weaponcollide.value || !vr_handweapon.value || !vr.hand_valid[off] || !vr.hand_valid[gun]
		|| !Cvar_VariableValue( "r_drawviewmodel" ) || !( shape = VR_WeaponShape( model )))
	{
		vr.grab_point = -1;
		vr.grab_model = NULL;
		vr.offhand_prev_valid = false;
		return;
	}

	if( model != vr.grab_model )
	{
		vr.grab_model = model;
		vr.grab_point = -1;
		vr.offhand_prev_valid = false;
	}

	AngleVectors( vr.hand_angles[gun], f, r, u );
	VectorSubtract( vr.hand_tracked[off], vr.hand_origin[gun], rel );
	VR_WorldToGun( f, r, u, rel, tracked );
	VR_HandFingers( off, &trigger, &squeeze );

	// let go?
	if( vr.grab_point >= 0 )
	{
		const vr_grab_point_t *g = &shape->grabs[vr.grab_point];

		if( VectorDistance( tracked, g->origin ) > g->radius + VR_GRAB_BREAK
			|| ( vr_grab_needgrip.value && squeeze < VR_GRAB_LET_GO ))
			vr.grab_point = -1;
	}

	// the nearest grab point in reach
	best = 1e9f;
	for( i = 0; i < shape->num_grabs; i++ )
	{
		float d = VectorDistance( tracked, shape->grabs[i].origin );

		if( d < shape->grabs[i].radius && d < best )
		{
			best = d;
			vr.grab_near = i;
		}
	}

	if( vr.grab_point < 0 && vr.grab_near >= 0 && ( !vr_grab_needgrip.value || squeeze >= VR_GRAB_HOLD ))
		vr.grab_point = vr.grab_near;

	if( vr.grab_point >= 0 )
	{
		const vr_grab_point_t *g = &shape->grabs[vr.grab_point];
		vec3_t lf, lr, lu, wf, wr, wu;

		VR_GunToWorld( f, r, u, g->origin, world );
		VectorAdd( vr.hand_origin[gun], world, vr.hand_origin[off] );

		// the grab point's angles are relative to the gun
		AngleVectors( g->angles, lf, lr, lu );
		VR_GunToWorld( f, r, u, lf, wf );
		VR_GunToWorld( f, r, u, lr, wr );
		VR_GunToWorld( f, r, u, lu, wu );
		VectorsAngles( wf, wr, wu, vr.hand_angles[off] );

		vr.hand_pose[off] = g->seq;
		vr.hand_separated[off] = true;
		VectorCopy( g->origin, vr.offhand_prev );
		vr.offhand_prev_valid = true;
		return;
	}

	VectorSubtract( vr.hand_origin[off], vr.hand_origin[gun], rel );
	VR_WorldToGun( f, r, u, rel, pos );

	if( vr.grab_near < 0 )
	{
		// twice, for a hand between two boxes
		for( iter = 0; iter < 2; iter++ )
		{
			for( i = 0; i < shape->num_boxes; i++ )
			{
				if( VR_PushOutOfBox( &shape->boxes[i], pos, vr.offhand_prev, vr.offhand_prev_valid ))
					moved = true;
			}
		}
	}

	if( moved )
	{
		// walls win: from where the wall check left the hand to the pushed place
		VR_GunToWorld( f, r, u, pos, world );
		VectorAdd( vr.hand_origin[gun], world, world );
		VR_HandTrace( vr.hand_origin[off], world, normal );
		VectorCopy( world, vr.hand_origin[off] );

		vr.hand_separated[off] = true;
		vr.offhand_on_gun = true;

		VectorSubtract( vr.hand_origin[off], vr.hand_origin[gun], rel );
		VR_WorldToGun( f, r, u, rel, pos );
	}

	VectorCopy( pos, vr.offhand_prev );
	vr.offhand_prev_valid = true;
}

/*
VR_UpdateHands

Both hands in the world for this frame, placed exactly like the eyes
(player's feet + room gap + pose relative to the head, turned by the
player's yaw): the grip pose gives the position, the aim pose the
direction. Then the weapon model goes into the right hand: its origin is
moved so that the point given by vr_viewmodel_grip lands in the hand.
That replaces the client's camera-relative placement, view bob and sway
included.
*/
static void VR_UpdateHands( const ref_viewpass_t *rvp, const vec3_t head )
{
	int hand;
	const model_t *held = NULL; // Half-Life's weapon model in the right hand, for the hand's grip

	// hold our own VR model instead of Half-Life's where there is one;
	// V_SetupViewModel sets the original again every frame
	if( vr_handweapon.value && vr.hand_valid[VR_HAND_RIGHT] )
	{
		model_t *vrmodel = VR_WeaponModel( clgame.viewent.model );

		held = clgame.viewent.model;
		if( vrmodel )
			clgame.viewent.model = vrmodel;
	}

	VR_MeasureMuzzle( ); // once per weapon model

	// the right hand first, so vr_debug_offhand can place the left one from it
	for( int n = 0; n < 2; n++ )
	{
		XrPosef grip, aim;
		vec3_t dummy;
		float dyaw, dpitch;

		hand = n ? VR_HAND_LEFT : VR_HAND_RIGHT;

		if( VR_DebugHands( &dyaw, &dpitch ))
		{
			VR_FakeHandPose( hand, &grip );
			aim = grip;

			if( hand == VR_HAND_RIGHT && ( dyaw || dpitch ))
			{
				// turn the fake aim: yaw around up, then pitch around the
				// aim's own right axis (positive pitch tilts it up, OpenXR style)
				XrQuaternionf qy, qp, t;
				const XrQuaternionf *q = &aim.orientation;

				qy.x = 0.0f; qy.y = sinf( DEG2RAD( dyaw ) * 0.5f ); qy.z = 0.0f; qy.w = cosf( DEG2RAD( dyaw ) * 0.5f );
				qp.x = sinf( DEG2RAD( dpitch ) * 0.5f ); qp.y = 0.0f; qp.z = 0.0f; qp.w = cosf( DEG2RAD( dpitch ) * 0.5f );

				// t = qy * q
				t.w = qy.w * q->w - qy.x * q->x - qy.y * q->y - qy.z * q->z;
				t.x = qy.w * q->x + qy.x * q->w + qy.y * q->z - qy.z * q->y;
				t.y = qy.w * q->y - qy.x * q->z + qy.y * q->w + qy.z * q->x;
				t.z = qy.w * q->z + qy.x * q->y - qy.y * q->x + qy.z * q->w;
				// aim = t * qp (local pitch)
				aim.orientation.w = t.w * qp.w - t.x * qp.x - t.y * qp.y - t.z * qp.z;
				aim.orientation.x = t.w * qp.x + t.x * qp.w + t.y * qp.z - t.z * qp.y;
				aim.orientation.y = t.w * qp.y - t.x * qp.z + t.y * qp.w + t.z * qp.x;
				aim.orientation.z = t.w * qp.z + t.x * qp.y - t.y * qp.x + t.z * qp.w;
			}
		}
		else if( !VR_GetHandPose( hand, VR_POSE_GRIP, vr.display_time, vr.space, &grip )
			|| !VR_GetHandPose( hand, VR_POSE_AIM, vr.display_time, vr.space, &aim ))
		{
			vr.hand_valid[hand] = false;
			vr.hand_prev_valid[hand] = false;
			continue;
		}

		VR_EyeView( rvp, &grip, head, vr.hand_tracked[hand], dummy );
		VR_EyeView( rvp, &aim, head, dummy, vr.hand_angles[hand] );
		vr.hand_play_yaw[hand] = vr.hand_angles[hand][YAW] - rvp->viewangles[YAW];
		vr.hand_valid[hand] = true;

		if( hand == VR_HAND_LEFT && vr.hand_valid[VR_HAND_RIGHT] && VR_DebugHands( NULL, NULL ) && vr_debug_offhand.string[0] )
		{
			vec3_t o = { 0.0f, 0.0f, 0.0f }, f, r, u, ofs;

			sscanf( vr_debug_offhand.string, "%f %f %f", &o[0], &o[1], &o[2] );
			AngleVectors( vr.hand_angles[VR_HAND_RIGHT], f, r, u );
			VR_GunToWorld( f, r, u, o, ofs );
			VectorAdd( vr.hand_tracked[VR_HAND_RIGHT], ofs, vr.hand_tracked[hand] );
		}

		VR_CollideHand( hand, hand == VR_HAND_RIGHT && vr_handweapon.value && clgame.viewent.model );
	}

	VR_OffhandWeapon( );

	if( vr_handweapon.value && vr.hand_valid[VR_HAND_RIGHT] )
	{
		cl_entity_t *view = &clgame.viewent;
		vec3_t f, r, u;
		float g[3] = { 0.0f, 0.0f, 0.0f };

		// our VR models have their origin where the hand grips them
		if( !VR_IsVRWeaponModel( view->model ))
			sscanf( vr_viewmodel_grip.string, "%f %f %f", &g[0], &g[1], &g[2] );
		AngleVectors( vr.hand_angles[VR_HAND_RIGHT], f, r, u );

		VectorCopy( vr.hand_origin[VR_HAND_RIGHT], view->origin );
		VectorMA( view->origin, -g[0], f, view->origin );
		VectorMA( view->origin, -g[1], r, view->origin );
		VectorMA( view->origin, -g[2], u, view->origin );

		// models use the opposite pitch sign from view angles (the renderer's
		// "stupid quake bug" flip, V_CalcGunAngle negates it the same way)
		VectorCopy( vr.hand_angles[VR_HAND_RIGHT], view->angles );
		view->angles[PITCH] = -view->angles[PITCH];

		// the client (V_CalcGunAngle, V_CalcRefdef with cl_viewbob) also puts its
		// bobbing camera transform into the interpolation fields, and parts of
		// the drawing use those: overwrite them too, or walking tilts the gun
		VectorCopy( view->origin, view->curstate.origin );
		VectorCopy( view->origin, view->latched.prevorigin );
		VectorCopy( view->angles, view->curstate.angles );
		VectorCopy( view->angles, view->latched.prevangles );
	}

	// the hands go where the tracked hands are; the right one grips the weapon
	if( !vr_handweapon.value || !vr.hand_valid[VR_HAND_RIGHT] || !Cvar_VariableValue( "r_drawviewmodel" ))
		held = NULL;
	VR_AddHands( vr.hand_valid, (const vec3_t *)vr.hand_origin, (const vec3_t *)vr.hand_angles, held, vr.hand_pose );
}

/*
VR_MirrorEye

Copies the eye image bound to vr.fbo into the desktop's 3D view
rectangle of the framebuffer the renderer was drawing to, center-cropped
to the view's aspect, so the desktop window shows what the headset shows
without drawing the world a third time. The HUD and 2D draw over it as
usual afterwards.
*/
static void VR_MirrorEye( const ref_viewpass_t *rvp, vr_GLuint desktop_fbo )
{
	int dst_x0 = rvp->viewport[0];
	int dst_y0 = refState.height - ( rvp->viewport[1] + rvp->viewport[3] ); // viewport y counts from the top
	int dst_w = rvp->viewport[2], dst_h = rvp->viewport[3];
	int src_w = vr.eye_width, src_h = vr.eye_height;
	qboolean srgb;

	if( dst_w <= 0 || dst_h <= 0 )
		return;

	if( (double)dst_w / dst_h > (double)src_w / src_h )
		src_h = (int)((double)src_w * dst_h / dst_w ); // view is wider: full eye width
	else
		src_w = (int)((double)src_h * dst_w / dst_h ); // view is taller: full eye height

	srgb = vrgl.IsEnabled( VR_GL_FRAMEBUFFER_SRGB );
	if( srgb ) vrgl.Disable( VR_GL_FRAMEBUFFER_SRGB );

	vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, vr.fbo );
	vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, desktop_fbo );
	vrgl.BlitFramebuffer(
		( vr.eye_width - src_w ) / 2, ( vr.eye_height - src_h ) / 2,
		( vr.eye_width + src_w ) / 2, ( vr.eye_height + src_h ) / 2,
		dst_x0, dst_y0, dst_x0 + dst_w, dst_y0 + dst_h,
		VR_GL_COLOR_BUFFER_BIT, VR_GL_LINEAR );

	if( srgb ) vrgl.Enable( VR_GL_FRAMEBUFFER_SRGB );
}

/*
VR_RenderEyes

Draws the 3D view into both eye swapchain images, called by V_RenderView
right before the desktop pass so the renderer ends the frame in the
desktop view's state. The views are located again here, as late as
possible, so the eyes and the submitted layer use the freshest head pose.

The left eye is the frame's main pass: it steps time and runs viewmodel
events. The right eye, and the desktop pass if it still runs, are
RF_REPEAT_PASS. With vr_mirror 0 the left eye is also copied into the
desktop view and the desktop pass is skipped.
*/
int VR_RenderEyes( const ref_viewpass_t *rvp )
{
	vr_GLint old_draw = 0, old_read = 0;
	qboolean scissor, stepped = false, mirrored = false;
	vec3_t head, head_world;
	int eye;

	if( !vr.frame_begun || !vr.should_render || !vr.fbo )
		return VR_EYES_NONE;

	VR_LocateViews( );
	if( !vr.views_valid )
		return VR_EYES_NONE;

	VR_UpdateRoom( rvp->viewangles[YAW] );
	VR_UpdateStairs( );
	VR_HeadCenter( head );
	VR_UpdateDuckView( head[2] );
	{
		// where the head faces in the play space, the sticks move relative to it
		static const XrVector3f xr_forward = { 0.0f, 0.0f, -1.0f };
		XrVector3f axis;
		vec3_t fwd;

		VR_RotateVector( &vr.views[0].pose.orientation, &xr_forward, &axis );
		VR_VectorToGame( &axis, fwd );
		if( fwd[0] != 0.0f || fwd[1] != 0.0f )
			vr.head_yaw = RAD2DEG( atan2f( fwd[1], fwd[0] ));
	}
	{
		// the head center: the sound listener sits here, and hands keep out of it;
		// the eye positions averaged, the eye orientations blended (equal on
		// most headsets, canted displays tilt them apart a little)
		const XrQuaternionf *a = &vr.views[0].pose.orientation, *b = &vr.views[1].pose.orientation;
		float sign = ( a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w ) < 0.0f ? -1.0f : 1.0f;
		XrPosef center;
		float len;

		center.position.x = ( vr.views[0].pose.position.x + vr.views[1].pose.position.x ) * 0.5f;
		center.position.y = ( vr.views[0].pose.position.y + vr.views[1].pose.position.y ) * 0.5f;
		center.position.z = ( vr.views[0].pose.position.z + vr.views[1].pose.position.z ) * 0.5f;
		center.orientation.x = a->x + sign * b->x;
		center.orientation.y = a->y + sign * b->y;
		center.orientation.z = a->z + sign * b->z;
		center.orientation.w = a->w + sign * b->w;
		len = sqrtf( center.orientation.x * center.orientation.x + center.orientation.y * center.orientation.y
			+ center.orientation.z * center.orientation.z + center.orientation.w * center.orientation.w );
		if( len > 0.0001f )
		{
			center.orientation.x /= len;
			center.orientation.y /= len;
			center.orientation.z /= len;
			center.orientation.w /= len;
		}
		else center.orientation = *a;

		VR_EyeView( rvp, &center, head, vr.head_origin, vr.head_angles );
	}
	VR_UpdateHands( rvp, head ); // after the head center, the hands keep out of the head
	{
		// the head's place in the world, for the wall check
		float yaw = DEG2RAD( rvp->viewangles[YAW] );
		vec3_t feet, turned, local = { 0.0f, 0.0f, head[2] };

		VR_PlayerFeet( feet );
		VR_YawVector( local, sinf( yaw ), cosf( yaw ), turned );
		VectorAdd( feet, turned, head_world );
		head_world[0] += vr.room_gap[0];
		head_world[1] += vr.room_gap[1];
	}
	if( vr.space_type == XR_REFERENCE_SPACE_TYPE_STAGE )
		VR_UpdateFade( head_world );
	else vr.fade = 0.0f;

	vrgl.GetIntegerv( VR_GL_DRAW_FRAMEBUFFER_BINDING, &old_draw );
	vrgl.GetIntegerv( VR_GL_READ_FRAMEBUFFER_BINDING, &old_read );
	scissor = vrgl.IsEnabled( VR_GL_SCISSOR_TEST );
	if( scissor ) vrgl.Disable( VR_GL_SCISSOR_TEST );

	for( eye = 0; eye < 2; eye++ )
	{
		XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		const XrFovf *fov = &vr.views[eye].fov;
		ref_viewpass_t eye_rvp = *rvp;
		uint32_t index;

		if( XR_FAILED( pxrAcquireSwapchainImage( vr.swapchain[eye], &acquire, &index )))
			break;

		wait.timeout = XR_INFINITE_DURATION;
		if( XR_FAILED( pxrWaitSwapchainImage( vr.swapchain[eye], &wait )))
			break;

		vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
		vrgl.FramebufferTexture2D( VR_GL_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, vr.images[eye][index].image, 0 );

		if( vrgl.CheckFramebufferStatus( VR_GL_FRAMEBUFFER ) == VR_GL_FRAMEBUFFER_COMPLETE )
		{
			// with MSAA the eye is drawn into the multisampled buffers and
			// resolved into the swapchain image afterwards
			if( vr.samples )
				vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.msaa_fbo );

			// the renderer only clears depth, the image starts out undefined
			vrgl.ClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
			vrgl.Clear( VR_GL_COLOR_BUFFER_BIT );

			eye_rvp.viewport[0] = eye_rvp.viewport[1] = 0;
			eye_rvp.viewport[2] = vr.eye_width;
			eye_rvp.viewport[3] = vr.eye_height;
			// fov_x/fov_y stay a symmetric cover for code that only knows those,
			// projection and culling use the exact per-eye angles
			eye_rvp.fov_x = ( fov->angleRight - fov->angleLeft ) * ( 180.0f / M_PI_F );
			eye_rvp.fov_y = ( fov->angleUp - fov->angleDown ) * ( 180.0f / M_PI_F );
			eye_rvp.fov_tan[0] = tanf( fov->angleLeft );
			eye_rvp.fov_tan[1] = tanf( fov->angleRight );
			eye_rvp.fov_tan[2] = tanf( fov->angleDown );
			eye_rvp.fov_tan[3] = tanf( fov->angleUp );
			SetBits( eye_rvp.flags, RF_EXTERNAL_TARGET|RF_ASYMMETRIC_FOV );
			if( vr_handweapon.value && vr.hand_valid[VR_HAND_RIGHT] && vr_handcollide.value )
				SetBits( eye_rvp.flags, RF_WORLD_VIEWMODEL ); // the gun is in the hand and kept out of walls
			SetBits( eye_rvp.flags, VR_ViewmodelFlags( ));
			if( stepped )
				SetBits( eye_rvp.flags, RF_REPEAT_PASS );

			VR_EyeView( rvp, &vr.views[eye].pose, head, eye_rvp.vieworigin, eye_rvp.viewangles );

			GL_RenderFrame( &eye_rvp );

			if( vr.samples )
			{
				qboolean srgb = vrgl.IsEnabled( VR_GL_FRAMEBUFFER_SRGB );

				if( srgb ) vrgl.Disable( VR_GL_FRAMEBUFFER_SRGB );
				vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, vr.msaa_fbo );
				vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, vr.fbo );
				vrgl.BlitFramebuffer( 0, 0, vr.eye_width, vr.eye_height, 0, 0, vr.eye_width, vr.eye_height,
					VR_GL_COLOR_BUFFER_BIT, VR_GL_NEAREST );
				if( srgb ) vrgl.Enable( VR_GL_FRAMEBUFFER_SRGB );
				vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
			}

			VR_DrawFade( );
			stepped = true;

			if( eye == 0 && !vr_mirror.value )
			{
				VR_MirrorEye( rvp, (vr_GLuint)old_draw );
				mirrored = true;
			}
		}

		vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo ); // the renderer may have changed it
		vrgl.FramebufferTexture2D( VR_GL_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, 0, 0 );
		pxrReleaseSwapchainImage( vr.swapchain[eye], &release );

		if( eye == 1 )
		{
			vr.eyes_written = true;
			vr.frames_rendered++;
		}
	}

	vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, old_draw );
	vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, old_read );
	if( scissor ) vrgl.Enable( VR_GL_SCISSOR_TEST );

	if( mirrored )
		return VR_EYES_MIRRORED;
	return stepped ? VR_EYES_RENDERED : VR_EYES_NONE;
}

/*
VR_CopyDesktopToEyes

Called right before the desktop window swaps. When the eyes weren't
rendered this frame (menu, console, loading), each eye gets the finished
desktop frame instead, center-cropped to the eye's aspect ratio, so the
2D screens stay visible in the headset.
*/
/*
VR_GameLoading

True from the moment the server decides to load (a changelevel, a new
game, a save being loaded, death reloading the last save) until the
client is back in the game with the loading plaque gone. The engine
blocks for seconds somewhere in between.
*/
static qboolean VR_GameLoading( void )
{
	if( GameState->curstate != STATE_RUNFRAME || GameState->nextstate != STATE_RUNFRAME )
		return true; // set during the server frame that triggers the load
	if( cls.disable_screen )
		return true; // loading plaque up
	if( cls.state > ca_disconnected && cls.state < ca_active )
		return true; // connecting to the new map, precaching
	return false;
}

/*
VR_ViewmodelFlags

The weapon is in the tracked hand: its built-in arms would float behind
it, and it can be turned to show sides the desktop camera never saw.
*/
static int VR_ViewmodelFlags( void )
{
	int flags = 0;

	if( !vr_handweapon.value || !vr.hand_valid[VR_HAND_RIGHT] )
		return 0;

	// our VR models have no arms and no holes
	if( VR_IsVRWeaponModel( clgame.viewent.model ))
		return 0;

	if( vr_hidearms.value )
		SetBits( flags, RF_VIEWMODEL_NOARMS );
	if( vr_twosided.value )
		SetBits( flags, RF_VIEWMODEL_TWOSIDED );

	return flags;
}

int VR_DesktopPassFlags( void )
{
	return RF_REPEAT_PASS | VR_ViewmodelFlags( );
}

/*
VR_PausesGame

Called through CL_IsInGame: singleplayer stops simulating and game sounds
pause while it returns true, exactly like with the menu open. True while
the session isn't focused: the headset is off or asleep, or the runtime's
system menu is up. The game resumes by itself when focus comes back.
*/
qboolean VR_PausesGame( void )
{
	if( !vr.initialized || vr.session == XR_NULL_HANDLE || cls.state != ca_active )
		return false;

	if( vr_debug_unfocused.value )
		return true;

	return vr.session_state != XR_SESSION_STATE_FOCUSED;
}

/*
VR_DrawLoadingScreen

Black with the game's LOADING image in the middle at VR_PANEL_TEXT of
the width, aspect kept. Runs in the renderer's 2D mode with a viewport of
vw x vh pixels; 2D mode works in window units, so sizes are worked out
in viewport pixels and turned into window units.
*/
static void VR_DrawLoadingScreen( int vw, int vh )
{
	int iw = 0, ih = 0;
	float pw, ph, sx = (float)refState.width / vw, sy = (float)refState.height / vh;

	vrgl.ClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	vrgl.Clear( VR_GL_COLOR_BUFFER_BIT );
	vrgl.Disable( VR_GL_ALPHA_TEST );

	if( cls.loadingBar <= 0 )
		return;

	R_GetTextureParms( &iw, &ih, cls.loadingBar );
	if( iw <= 0 || ih <= 0 )
		return;

	pw = vw * VR_PANEL_TEXT;
	ph = pw * ih / iw;
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
	ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
	ref.dllFuncs.R_DrawStretchPic(( vw - pw ) * 0.5f * sx, ( vh - ph ) * 0.5f * sy, pw * sx, ph * sy, 0, 0, 1, 1, cls.loadingBar );
}

/*
VR_DrawDesktopLoading

While the headset shows the loading panel, the desktop window shows the
same: in VR it's normally a copy of the left eye, which would stay on the
last world frame and never show the game's loading screen.
*/
static void VR_DrawDesktopLoading( void )
{
	qboolean scissor = vrgl.IsEnabled( VR_GL_SCISSOR_TEST );

	if( refState.width <= 0 || refState.height <= 0 )
		return;

	if( scissor ) vrgl.Disable( VR_GL_SCISSOR_TEST ); // clears obey the scissor
	ref.dllFuncs.R_Set2DMode( true ); // binds the screen and sets the window viewport
	VR_DrawLoadingScreen( refState.width, refState.height );
	ref.dllFuncs.R_Set2DMode( false );
	if( scissor ) vrgl.Enable( VR_GL_SCISSOR_TEST );
}

/*
VR_DrawPanel

Draws the loading panel: black with the game's LOADING image in the
middle. Called at the desktop swap while the game is loading, once per
load. The desktop's own plaque isn't copied, it's the last world frame
with LOADING on top, which floats in the dark like a photo in VR.
*/
static void VR_DrawPanel( void )
{
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	vr_GLint old_draw = 0, old_read = 0;
	qboolean scissor, srgb;
	uint32_t index;

	if( vr.panel_valid || vr.panel_swapchain == XR_NULL_HANDLE || !vr.fbo || refState.width <= 0 || refState.height <= 0 )
		return;

	if( XR_FAILED( pxrAcquireSwapchainImage( vr.panel_swapchain, &acquire, &index )))
		return;

	wait.timeout = XR_INFINITE_DURATION;
	if( XR_FAILED( pxrWaitSwapchainImage( vr.panel_swapchain, &wait )))
		return;

	vrgl.GetIntegerv( VR_GL_DRAW_FRAMEBUFFER_BINDING, &old_draw );
	vrgl.GetIntegerv( VR_GL_READ_FRAMEBUFFER_BINDING, &old_read );
	scissor = vrgl.IsEnabled( VR_GL_SCISSOR_TEST );
	srgb = vrgl.IsEnabled( VR_GL_FRAMEBUFFER_SRGB );
	if( scissor ) vrgl.Disable( VR_GL_SCISSOR_TEST );
	if( srgb ) vrgl.Disable( VR_GL_FRAMEBUFFER_SRGB );

	// the renderer's 2D mode maps window coordinates onto the viewport
	ref.dllFuncs.R_Set2DMode( true );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
	vrgl.FramebufferTexture2D( VR_GL_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, vr.panel_images[index].image, 0 );

	if( vrgl.CheckFramebufferStatus( VR_GL_FRAMEBUFFER ) == VR_GL_FRAMEBUFFER_COMPLETE )
	{
		vrgl.Viewport( 0, 0, VR_PANEL_WIDTH, VR_PANEL_HEIGHT );
		VR_DrawLoadingScreen( VR_PANEL_WIDTH, VR_PANEL_HEIGHT );
		vr.panel_valid = true;
	}

	ref.dllFuncs.R_Set2DMode( false );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo );
	vrgl.FramebufferTexture2D( VR_GL_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, 0, 0 );
	pxrReleaseSwapchainImage( vr.panel_swapchain, &release );

	vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, old_draw );
	vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, old_read );
	vrgl.Viewport( 0, 0, refState.width, refState.height );
	if( scissor ) vrgl.Enable( VR_GL_SCISSOR_TEST );
	if( srgb ) vrgl.Enable( VR_GL_FRAMEBUFFER_SRGB );
}

void VR_CopyDesktopToEyes( void )
{
	vr_GLint old_draw = 0, old_read = 0;
	qboolean scissor, srgb;
	int src_w, src_h, src_x, eye;

	if( vr.running && ( VR_GameLoading( ) || vr.transition == VR_TRANSITION_LOADING || vr.transition == VR_TRANSITION_HOLD ))
	{
		// while the headset shows the loading panel (loading, and the hold
		// after it) the desktop shows the same, never the head-locked copy
		VR_DrawDesktopLoading( );
		VR_DrawPanel( );
		return;
	}

	if( !vr.frame_begun || !vr.should_render || !vr.views_valid || !vr.fbo || vr.eyes_written )
		return;

	if( refState.width <= 0 || refState.height <= 0 )
		return;

	// largest window rect with the eye's aspect, centered
	src_h = refState.height;
	src_w = (int)((double)src_h * vr.eye_width / vr.eye_height );
	if( src_w > refState.width )
	{
		src_w = refState.width;
		src_h = (int)((double)src_w * vr.eye_height / vr.eye_width );
	}
	src_x = ( refState.width - src_w ) / 2;

	vrgl.GetIntegerv( VR_GL_DRAW_FRAMEBUFFER_BINDING, &old_draw );
	vrgl.GetIntegerv( VR_GL_READ_FRAMEBUFFER_BINDING, &old_read );
	scissor = vrgl.IsEnabled( VR_GL_SCISSOR_TEST );
	srgb = vrgl.IsEnabled( VR_GL_FRAMEBUFFER_SRGB );
	if( scissor ) vrgl.Disable( VR_GL_SCISSOR_TEST ); // blits obey the scissor
	if( srgb ) vrgl.Disable( VR_GL_FRAMEBUFFER_SRGB ); // copy bytes as they are

	vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, 0 ); // the window's back buffer

	for( eye = 0; eye < 2; eye++ )
	{
		XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		uint32_t index;

		if( XR_FAILED( pxrAcquireSwapchainImage( vr.swapchain[eye], &acquire, &index )))
			break;

		wait.timeout = XR_INFINITE_DURATION;
		if( XR_FAILED( pxrWaitSwapchainImage( vr.swapchain[eye], &wait )))
			break; // nothing to release, the wait didn't give us the image

		vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, vr.fbo );
		vrgl.FramebufferTexture2D( VR_GL_DRAW_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, vr.images[eye][index].image, 0 );

		if( vrgl.CheckFramebufferStatus( VR_GL_DRAW_FRAMEBUFFER ) == VR_GL_FRAMEBUFFER_COMPLETE )
		{
			vrgl.BlitFramebuffer( src_x, 0, src_x + src_w, src_h,
				0, 0, vr.eye_width, vr.eye_height, VR_GL_COLOR_BUFFER_BIT, VR_GL_LINEAR );
		}

		vrgl.FramebufferTexture2D( VR_GL_DRAW_FRAMEBUFFER, VR_GL_COLOR_ATTACHMENT0, VR_GL_TEXTURE_2D, 0, 0 );
		pxrReleaseSwapchainImage( vr.swapchain[eye], &release );

		if( eye == 1 )
		{
			vr.eyes_written = true;
			vr.frames_copied++;
		}
	}

	vrgl.BindFramebuffer( VR_GL_DRAW_FRAMEBUFFER, old_draw );
	vrgl.BindFramebuffer( VR_GL_READ_FRAMEBUFFER, old_read );
	if( scissor ) vrgl.Enable( VR_GL_SCISSOR_TEST );
	if( srgb ) vrgl.Enable( VR_GL_FRAMEBUFFER_SRGB );
}

static void VR_PrintFrameStats( void )
{
	double elapsed = Platform_DoubleTime() - vr.begin_time;

	Con_Printf( "VR: %u frames in %.1f s (%.1f fps), headset period %.2f ms\n",
		vr.frames, elapsed, elapsed > 0.0 ? vr.frames / elapsed : 0.0,
		vr.display_period / 1000000.0 );
	Con_Printf( "VR: xrWaitFrame blocked %.2f ms per frame on average, %.2f ms at most\n",
		vr.frames ? vr.wait_total * 1000.0 / vr.frames : 0.0, vr.wait_max * 1000.0 );
	Con_Printf( "VR: eyes rendered in %u frames, desktop copy in %u, empty %u\n",
		vr.frames_rendered, vr.frames_copied, vr.frames - vr.frames_rendered - vr.frames_copied );
	Con_Printf( "VR: loading panel submitted in %u frames\n", vr.frames_panel );
}

// the desktop mirror window must not wait for vsync while VR runs, the
// headset's xrWaitFrame is the only thing allowed to block a frame
static void VR_SetDesktopVsync( qboolean allow )
{
	if( allow )
	{
		SetBits( gl_vsync.flags, FCVAR_CHANGED ); // the platform reapplies gl_vsync next frame
		return;
	}

#if XASH_SDL == 2
	if( SDL_GL_SetSwapInterval( 0 ) < 0 )
		Con_Printf( S_WARN "VR: can't disable desktop vsync: %s\n", SDL_GetError( ));
#endif
}

static void VR_SubmitFrame( void );

static void VR_BeginSession( void )
{
	XrSessionBeginInfo info = { XR_TYPE_SESSION_BEGIN_INFO };
	XrResult result;

	info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	result = pxrBeginSession( vr.session, &info );

	if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrBeginSession failed: %s\n", VR_ResultString( result ));
		return;
	}

	vr.running = true;
	vr.frames = vr.frames_rendered = vr.frames_copied = vr.frames_panel = 0;
	vr.wait_total = vr.wait_max = 0.0;
	vr.begin_time = Platform_DoubleTime();
	Con_Printf( "VR: session running\n" );
}

// SteamVR's xrWaitFrame does not block while a session submits frames with
// no layers (measured 0.07 ms per frame, the engine ran at ~130 fps against
// a 90 Hz headset), so the headset only takes over pacing once eye images
// are submitted. Until then the engine's own limiter and vsync stay in charge.
static void VR_UpdatePacing( void )
{
	qboolean paced = vr.running && vr.submits_layers;

	if( paced == vr.paced )
		return;

	vr.paced = paced;
	VR_SetDesktopVsync( !paced );
	Con_Reportf( "VR: frame pacing by %s\n", paced ? "the headset" : "the engine" );
}

static void VR_EndSession( void )
{
	if( !vr.running )
		return;

	VR_SubmitFrame( ); // a frame begun this host frame still has to be submitted
	pxrEndSession( vr.session );
	vr.running = false;
	VR_UpdatePacing( );
	Con_Printf( "VR: session stopped\n" );
	VR_PrintFrameStats( );
}

static void VR_DestroySession( void )
{
	if( vr.running )
	{
		// destroying a running session is legal, just restore the desktop
		VR_PrintFrameStats( );
		vr.running = false;
		vr.frame_begun = false;
		VR_UpdatePacing( );
	}

	VR_DestroySwapchains( );
	VR_ShutdownInput( );

	if( vr.space != XR_NULL_HANDLE )
		pxrDestroySpace( vr.space );

	if( vr.session != XR_NULL_HANDLE )
		pxrDestroySession( vr.session );

	vr.space = XR_NULL_HANDLE;
	vr.session = XR_NULL_HANDLE;
	vr.session_state = XR_SESSION_STATE_UNKNOWN;
}

static void VR_HandleSessionState( const XrEventDataSessionStateChanged *ev )
{
	vr.session_state = ev->state;
	Con_Reportf( "VR: session state %s\n", VR_SessionStateString( ev->state ));

	switch( ev->state )
	{
	case XR_SESSION_STATE_READY:
		VR_BeginSession( );
		break;
	case XR_SESSION_STATE_STOPPING:
		VR_EndSession( );
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		// the runtime is going away or asked us to quit VR, go back to desktop
		Con_Printf( "VR: session ended by the runtime (%s), VR disabled\n", VR_SessionStateString( ev->state ));
		VR_Shutdown( );
		break;
	default:
		break;
	}
}

void VR_PollEvents( void )
{
	XrEventDataBuffer ev;

	if( !vr.initialized )
		return;

	// non-blocking, drains whatever the runtime queued since last frame
	while( vr.instance != XR_NULL_HANDLE )
	{
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		ev.next = NULL;

		if( pxrPollEvent( vr.instance, &ev ) != XR_SUCCESS )
			break;

		switch( ev.type )
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			VR_HandleSessionState( (const XrEventDataSessionStateChanged *)&ev );
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			Con_Printf( "VR: OpenXR runtime is shutting down, VR disabled\n" );
			VR_Shutdown( );
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			VR_InputProfileChanged( );
			break;
		case XR_TYPE_EVENT_DATA_EVENTS_LOST:
			Con_Printf( S_WARN "VR: lost %u OpenXR events\n", ((const XrEventDataEventsLost *)&ev )->lostEventCount );
			break;
		default:
			break;
		}
	}
}

/*
VR_PlacePanel

Puts the loading panel 2 m in front of where the head faces, level, at
eye height, turned toward the head. It stays at that spot in the room,
so looking around while the game is frozen behaves like the real world.
*/
static void VR_PlacePanel( void )
{
	static const XrVector3f xr_forward = { 0.0f, 0.0f, -1.0f };
	XrVector3f head = { 0.0f, 1.6f, 0.0f }, f = { 0.0f, 0.0f, -1.0f }, axis;
	float len, theta;

	if( vr.views_valid )
	{
		head.x = ( vr.views[0].pose.position.x + vr.views[1].pose.position.x ) * 0.5f;
		head.y = ( vr.views[0].pose.position.y + vr.views[1].pose.position.y ) * 0.5f;
		head.z = ( vr.views[0].pose.position.z + vr.views[1].pose.position.z ) * 0.5f;

		VR_RotateVector( &vr.views[0].pose.orientation, &xr_forward, &axis );
		len = sqrtf( axis.x * axis.x + axis.z * axis.z );
		if( len > 0.001f )
		{
			f.x = axis.x / len;
			f.z = axis.z / len;
		}
	}

	vr.panel_pose.position.x = head.x + f.x * VR_PANEL_DISTANCE;
	vr.panel_pose.position.y = head.y;
	vr.panel_pose.position.z = head.z + f.z * VR_PANEL_DISTANCE;

	// a quad faces along its +Z: turn around Y so -Z points away from the head
	theta = atan2f( -f.x, -f.z );
	vr.panel_pose.orientation.x = 0.0f;
	vr.panel_pose.orientation.y = sinf( theta * 0.5f );
	vr.panel_pose.orientation.z = 0.0f;
	vr.panel_pose.orientation.w = cosf( theta * 0.5f );
}

static void VR_SetTransition( int transition, double now )
{
	vr.transition = transition;
	vr.transition_time = now;
	vr.hold_frames = 0;
}

/*
VR_UpdateTransition

Once per submitted frame, decides what the headset gets:

NONE     playing: the eyes.
LOADING  from the frame that triggers a load until the game is back:
         only the loading panel (or nothing, if it couldn't be drawn).
         The last frame before the engine blocks is a panel frame, so
         while the engine is frozen the runtime keeps drawing a
         world-locked panel with fresh head tracking, never a stuck world.
HOLD     loaded: the eyes are drawn again but the panel stays for a few
         frames, so the hitches of the first frames stay hidden.
FADEIN   the eyes are submitted, fading in from black over vr_loadfade.

Starting a load cuts straight to the panel: the server has already
switched levels by then, there's no frame left to fade out in.
*/
static void VR_UpdateTransition( void )
{
	double now = Platform_DoubleTime(), dt = now - vr.transition_last;
	qboolean loading = VR_GameLoading( ), paused = VR_PausesGame( );

	vr.transition_last = now;

	if( paused != vr.game_paused )
	{
		vr.game_paused = paused;
		Con_Printf( "VR: %s\n", paused ? "the headset lost focus, game paused" : "the headset has focus again, game resumed" );
	}

	if( loading && vr.transition != VR_TRANSITION_LOADING )
	{
		if( vr.transition == VR_TRANSITION_NONE )
			VR_PlacePanel( );
		Con_Printf( "VR: loading, the headset shows the loading panel\n" );
		VR_SetTransition( VR_TRANSITION_LOADING, now );
		vr.load_fade = 0.0f;
		return;
	}

	switch( vr.transition )
	{
	case VR_TRANSITION_LOADING:
		if( !loading )
		{
			Con_Printf( "VR: loaded in %.2f s\n", now - vr.transition_time );
			VR_SetTransition( VR_TRANSITION_HOLD, now );
		}
		break;
	case VR_TRANSITION_HOLD:
		if( vr.eyes_written )
			vr.hold_frames++;

		if( vr.hold_frames >= VR_HOLD_FRAMES && now - vr.transition_time >= VR_HOLD_TIME )
		{
			VR_SetTransition( VR_TRANSITION_FADEIN, now );
			vr.load_fade = vr_loadfade.value > 0.0f ? 1.0f : 0.0f;
		}
		break;
	case VR_TRANSITION_FADEIN:
		if( vr_loadfade.value > 0.0f )
			vr.load_fade -= dt / vr_loadfade.value;
		else vr.load_fade = 0.0f;

		if( vr.load_fade <= 0.0f )
		{
			vr.load_fade = 0.0f;
			vr.panel_valid = false; // the next load draws it again
			VR_SetTransition( VR_TRANSITION_NONE, now );
		}
		break;
	default:
		break;
	}
}

static void VR_SubmitFrame( void )
{
	XrFrameEndInfo info = { XR_TYPE_FRAME_END_INFO };
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerProjectionView views[2];
	const XrCompositionLayerBaseHeader *layers[1];
	XrResult result;
	int eye;

	if( !vr.frame_begun )
		return;

	VR_UpdateTransition( );

	info.displayTime = vr.display_time;
	info.environmentBlendMode = vr.blend_mode;
	info.layerCount = 0;
	info.layers = NULL;

	if( vr.transition == VR_TRANSITION_LOADING || vr.transition == VR_TRANSITION_HOLD )
	{
		// opaque quad on black, placed in the room; if it couldn't be drawn
		// the frame goes out empty
		if( vr.panel_valid )
		{
			quad.space = vr.space;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = vr.panel_swapchain;
			quad.subImage.imageRect.extent.width = VR_PANEL_WIDTH;
			quad.subImage.imageRect.extent.height = VR_PANEL_HEIGHT;
			quad.pose = vr.panel_pose;
			quad.size.width = VR_PANEL_METRES;
			quad.size.height = VR_PANEL_METRES * VR_PANEL_HEIGHT / VR_PANEL_WIDTH;
			layers[0] = (const XrCompositionLayerBaseHeader *)&quad;

			info.layerCount = 1;
			info.layers = layers;
			vr.frames_panel++;
		}
	}
	// a frame where the eyes weren't filled (menus before the first copy,
	// shouldRender false) goes out with no layers
	else if( vr.eyes_written )
	{
		for( eye = 0; eye < 2; eye++ )
		{
			memset( &views[eye], 0, sizeof( views[eye] ));
			views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			views[eye].pose = vr.views[eye].pose;
			views[eye].fov = vr.views[eye].fov;
			views[eye].subImage.swapchain = vr.swapchain[eye];
			views[eye].subImage.imageRect.extent.width = vr.eye_width;
			views[eye].subImage.imageRect.extent.height = vr.eye_height;
		}

		layer.space = vr.space;
		layer.viewCount = 2;
		layer.views = views;
		layers[0] = (const XrCompositionLayerBaseHeader *)&layer;

		info.layerCount = 1;
		info.layers = layers;
		vr.submits_layers = true; // from now on the headset paces frames
	}

	vr.eyes_written = false;

	result = pxrEndFrame( vr.session, &info );
	vr.frame_begun = false;

	if( XR_FAILED( result ))
		Con_Printf( S_ERROR "VR: xrEndFrame failed: %s\n", VR_ResultString( result ));
	else vr.frames++;
}

void VR_WaitFrame( void )
{
	XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
	XrFrameState state = { XR_TYPE_FRAME_STATE };
	XrResult result;

	VR_UpdatePacing( );

	if( !vr.running )
		return;

	// a Host_Error/longjmp skipped last frame's VR_EndFrame, close it now
	// so every xrWaitFrame stays paired with one xrBeginFrame/xrEndFrame
	VR_SubmitFrame( );

	// no frame is open here, the only safe moment to rebuild the eye swapchains
	VR_CheckEyeTargets( );
	if( !vr.running )
		return;

	// blocks until the runtime wants the next frame; this is the frame limiter
	{
		double t = Platform_DoubleTime();
		result = pxrWaitFrame( vr.session, &wait_info, &state );
		t = Platform_DoubleTime() - t;
		vr.wait_total += t;
		vr.wait_max = Q_max( vr.wait_max, t );
	}
	if( XR_FAILED( result ))
	{
		if( !vr.warned_wait )
			Con_Printf( S_ERROR "VR: xrWaitFrame failed: %s\n", VR_ResultString( result ));
		vr.warned_wait = true;
		return;
	}

	vr.display_time = state.predictedDisplayTime;
	vr.display_period = state.predictedDisplayPeriod;
	vr.should_render = state.shouldRender;

	// XR_FRAME_DISCARDED is a success code, the frame must still be ended
	result = pxrBeginFrame( vr.session, &begin_info );
	if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrBeginFrame failed: %s\n", VR_ResultString( result ));
		return;
	}

	vr.frame_begun = true;
	vr.eyes_written = false;
	VR_LocateViews( );
}

void VR_EndFrame( void )
{
	VR_SubmitFrame( );
}

qboolean VR_PacesFrames( void )
{
	return vr.paced;
}

static void VR_Info_f( void )
{
	if( !vr.initialized )
	{
		Con_Printf( "VR: not active (start the engine with -vr)\n" );
		return;
	}

	Con_Printf( "VR: active, eye size %ux%u, session %s, %s space\n", vr.eye_width, vr.eye_height,
		VR_SessionStateString( vr.session_state ),
		vr.space_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL" );

	if( vr.running )
		VR_PrintFrameStats( );

	{
		static const char *names[] = { "playing", "loading (panel)", "loaded, holding the panel", "fading in" };
		Con_Printf( "VR: %s, game %s\n", names[bound( 0, vr.transition, 3 )], vr.game_paused ? "paused (no focus)" : "running" );
	}

	Con_Printf( "VR: room scale %s, player at %.1f %.1f %.1f yaw %.0f, head %.1f units away, wall fade %.2f\n",
		vr_roomscale.value ? "on" : "off", cl.simorg[0], cl.simorg[1], cl.simorg[2], cl.viewangles[YAW],
		sqrtf( vr.room_gap[0] * vr.room_gap[0] + vr.room_gap[1] * vr.room_gap[1] ), vr.fade );
	Con_Printf( "VR: view floor %.1f, box bottom %.1f (stairs smoothed by up to %.1f units so far)\n",
		vr.feet_valid ? vr.feet_z : VR_BoxBottom( ), VR_BoxBottom( ), vr.stair_lag_max );
	Con_Printf( "VR: head %.2f m above the floor (crouch below %.2f m), %s, %s, view lowered %.1f units\n",
		vr.head_height / Q_max( 1.0f, vr_worldscale.value ), vr_crouchheight.value,
		cl.local.usehull == 1 ? "ducked" : "standing", cl.local.onground != -1 ? "on the ground" : "in the air", vr.duck_lower );
	Con_Printf( "VR: moving at %.0f units/s (%.1f m/s) horizontally\n", sqrtf( cl.simvel[0] * cl.simvel[0] + cl.simvel[1] * cl.simvel[1] ),
		sqrtf( cl.simvel[0] * cl.simvel[0] + cl.simvel[1] * cl.simvel[1] ) / Q_max( 1.0f, vr_worldscale.value ));

	{
		int hand;

		for( hand = 0; hand < 2; hand++ )
		{
			if( vr.hand_valid[hand] )
				Con_Printf( "VR: %s hand at %.1f %.1f %.1f, aiming %.0f %.0f %.0f, %s, %.1f units from the controller\n", hand ? "right" : "left",
					vr.hand_origin[hand][0], vr.hand_origin[hand][1], vr.hand_origin[hand][2],
					vr.hand_angles[hand][0], vr.hand_angles[hand][1], vr.hand_angles[hand][2],
					vr.hand_blocked[hand] ? "against a wall" : vr.hand_at_head[hand] ? "kept out of the head" : vr.hand_separated[hand] ? "easing back" : "free",
					VectorDistance( vr.hand_origin[hand], vr.hand_tracked[hand] ));
			else Con_Printf( "VR: %s hand not tracked\n", hand ? "right" : "left" );
		}
		Con_Printf( "VR: head at %.1f %.1f %.1f facing %.0f %.0f %.0f, sound listener at %.1f %.1f %.1f\n",
			vr.head_origin[0], vr.head_origin[1], vr.head_origin[2],
			vr.head_angles[0], vr.head_angles[1], vr.head_angles[2],
			snd.origin[0], snd.origin[1], snd.origin[2] );
		if( vr.muzzle_model )
			Con_Printf( "VR: %s muzzle %.1f %.1f %.1f from the hand (%s)\n", vr.muzzle_model->name,
				vr.muzzle_local[0], vr.muzzle_local[1], vr.muzzle_local[2], vr.muzzle_measured ? "from the model's shape" : "length table" );
		Con_Printf( "VR: weapon model at %.1f %.1f %.1f\n", clgame.viewent.origin[0], clgame.viewent.origin[1], clgame.viewent.origin[2] );
		Con_Printf( "VR: last command aims %.0f %.0f, body yaw %.0f, gun start %s %.1f %.1f %.1f from the player\n",
			cl.cmd.viewangles[PITCH], cl.cmd.viewangles[YAW], cl.viewangles[YAW],
			Cvar_VariableValue( "vr_aim_active" ) ? "at the hand," : "at the eyes,",
			Cvar_VariableValue( "vr_aim_x" ), Cvar_VariableValue( "vr_aim_y" ), Cvar_VariableValue( "vr_aim_z" ));
		if( vr.grab_model )
		{
			const vr_weapon_shape_t *shape = VR_WeaponShape( vr.grab_model );
			vec3_t f, r, u, rel, c, d;

			// both hands in the gun's axes (forward, right, up from the grip)
			AngleVectors( vr.hand_angles[VR_HAND_RIGHT], f, r, u );
			VectorSubtract( vr.hand_tracked[VR_HAND_LEFT], vr.hand_origin[VR_HAND_RIGHT], rel );
			VR_WorldToGun( f, r, u, rel, c );
			VectorSubtract( vr.hand_origin[VR_HAND_LEFT], vr.hand_origin[VR_HAND_RIGHT], rel );
			VR_WorldToGun( f, r, u, rel, d );
			Con_Printf( "VR: from the gun's grip, the left controller is at %.1f %.1f %.1f and the left hand drawn at %.1f %.1f %.1f (forward right up)\n",
				c[0], c[1], c[2], d[0], d[1], d[2] );

			if( shape && vr.grab_point >= 0 && vr.grab_point < shape->num_grabs )
				Con_Printf( "VR: left hand holds grab point \"%s\" of %s\n", shape->grabs[vr.grab_point].name, vr.grab_model->name );
			else if( shape && vr.grab_near >= 0 && vr.grab_near < shape->num_grabs )
				Con_Printf( "VR: left hand reaching for grab point \"%s\" of %s\n", shape->grabs[vr.grab_near].name, vr.grab_model->name );
			else Con_Printf( "VR: left hand %s %s (%d boxes)\n", vr.offhand_on_gun ? "against" : "clear of",
				vr.grab_model->name, shape ? shape->num_boxes : 0 );
		}
		VR_HandsInfo( );
	}
}

XrInstance VR_GetInstance( void )
{
	return vr.instance;
}

XrSession VR_GetSession( void )
{
	return vr.session;
}

qboolean VR_SessionFocused( void )
{
	return vr.running && vr.session_state == XR_SESSION_STATE_FOCUSED;
}

float VR_HeadYaw( void )
{
	return vr.head_yaw;
}

qboolean VR_HandYaw( int hand, float *yaw )
{
	if( !vr.running || hand < 0 || hand > 1 || !vr.hand_valid[hand] )
		return false;

	*yaw = vr.hand_play_yaw[hand];
	return true;
}

void VR_ListenerView( struct ref_viewpass_s *rvp )
{
	// only called for frames whose eyes were drawn, so the head is fresh
	VectorCopy( vr.head_origin, rvp->vieworigin );
	VectorCopy( vr.head_angles, rvp->viewangles );
}

qboolean VR_RightHandAim( vec3_t origin, vec3_t angles )
{
	if( !vr.running || !vr_handweapon.value || !vr.hand_valid[VR_HAND_RIGHT] || cls.state != ca_active )
		return false;

	VectorCopy( vr.hand_origin[VR_HAND_RIGHT], origin );
	VectorCopy( vr.hand_angles[VR_HAND_RIGHT], angles );
	return true;
}

qboolean VR_Active( void )
{
	return vr.initialized;
}

void VR_Init( void )
{
	Cmd_AddCommand( "vr_info", VR_Info_f, "print OpenXR runtime and headset state" );
	Cvar_RegisterVariable( &vr_worldscale );
	Cvar_RegisterVariable( &vr_heightoffset );
	Cvar_RegisterVariable( &vr_mirror );
	Cvar_RegisterVariable( &vr_roomscale );
	Cvar_RegisterVariable( &vr_renderscale );
	Cvar_RegisterVariable( &vr_msaa );
	Cvar_RegisterVariable( &vr_handweapon );
	Cvar_RegisterVariable( &vr_viewmodel_grip );
	Cvar_RegisterVariable( &vr_debug_headmove );
	Cvar_RegisterVariable( &vr_crouchheight );
	Cvar_RegisterVariable( &vr_stairsmooth );
	Cvar_RegisterVariable( &vr_handcollide );
	Cvar_RegisterVariable( &vr_weaponcollide );
	Cvar_RegisterVariable( &vr_grab_needgrip );
	Cvar_RegisterVariable( &vr_debug_offhand );
	Cvar_RegisterVariable( &vr_hidearms );
	Cvar_RegisterVariable( &vr_pinweapon );
	Cvar_RegisterVariable( &vr_twosided );
	VR_WeaponModelsInit( );
	VR_HandsInit( );
	Cvar_RegisterVariable( &vr_weapon_length );
	Cvar_RegisterVariable( &vr_loadfade );
	Cvar_RegisterVariable( &vr_debug_unfocused );
	Cmd_AddCommand( "vr_recenter", VR_Recenter_f, "put the player back under the real head" );
	Cmd_AddCommand( "vr_calibrate", VR_Calibrate_f, "stand or sit normally and run it: sets vr_heightoffset so your head is at the game's eye height" );

	if( !Sys_CheckParm( "-vr" ))
		return;

	if( !Sys_LoadLibrary( &openxr_loader_info ))
	{
		Con_Printf( S_ERROR "VR: can't load %s, VR disabled\n", openxr_loader_info.name );
		return;
	}

	if( !VR_CreateInstance( ) || !VR_GetSystem( ) || !VR_CreateSession( ) || !VR_CreateSpace( ) || !VR_CreateSwapchains( ) || !VR_InitInput( ))
	{
		VR_Shutdown( );
		return;
	}

	vr.initialized = true;
	Con_Printf( "VR: OpenXR ready\n" );
}

void VR_Shutdown( void )
{
	if( vr.instance != XR_NULL_HANDLE )
		VR_DestroySession( );

	if( vr.instance != XR_NULL_HANDLE && pxrDestroyInstance )
		pxrDestroyInstance( vr.instance );

	Sys_FreeLibrary( &openxr_loader_info );
	memset( &vr, 0, sizeof( vr ));
}

#endif // XASH_OPENXR
