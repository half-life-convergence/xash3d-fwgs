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

	// hands in the world, this frame: grip position, aim direction
	qboolean hand_valid[2];
	vec3_t   hand_origin[2];
	vec3_t   hand_angles[2];

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
	return VR_CreateEyeTargets( );
}

static void VR_DestroySwapchains( void )
{
	VR_DestroyEyeTargets( );

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

// bottom of the player's collision box, where the STAGE floor is
static void VR_PlayerFeet( vec3_t out )
{
	VectorCopy( cl.simorg, out );
	out[2] += ( cl.local.usehull == 1 ) ? -18.0f : -36.0f; // ducked or standing hull bottom
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
	pmtrace_t *tr;
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

	tr = PM_CL_TraceLine( start, (float *)head_world, PM_STUDIO_IGNORE, 2, -1 ); // point hull
	if( tr->fraction >= 1.0f && !tr->allsolid )
		return;

	depth = tr->allsolid ? len : ( 1.0f - tr->fraction ) * len;
	vr.fade = bound( 0.0f, depth / 6.0f, 1.0f ); // black after 6 units (~15 cm) into the wall
}

static void VR_DrawFade( void )
{
	if( vr.fade <= 0.0f )
		return;

	// the renderer's 2D mode maps window coordinates onto the viewport,
	// so the window rectangle covers the whole eye image
	ref.dllFuncs.R_Set2DMode( true );
	vrgl.BindFramebuffer( VR_GL_FRAMEBUFFER, vr.fbo ); // 2D mode may bind a screen target
	vrgl.Viewport( 0, 0, vr.eye_width, vr.eye_height );
	vrgl.Disable( VR_GL_ALPHA_TEST );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, 0, refState.width, refState.height, 0, 0, 0, (byte)( vr.fade * 255.0f ));
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

	for( hand = 0; hand < 2; hand++ )
	{
		XrPosef grip, aim;
		vec3_t dummy;

		if( VR_DebugHands( ))
		{
			VR_FakeHandPose( hand, &grip );
			aim = grip;
		}
		else if( !VR_GetHandPose( hand, VR_POSE_GRIP, vr.display_time, vr.space, &grip )
			|| !VR_GetHandPose( hand, VR_POSE_AIM, vr.display_time, vr.space, &aim ))
		{
			vr.hand_valid[hand] = false;
			continue;
		}

		VR_EyeView( rvp, &grip, head, vr.hand_origin[hand], dummy );
		VR_EyeView( rvp, &aim, head, dummy, vr.hand_angles[hand] );
		vr.hand_valid[hand] = true;
	}

	if( vr_handweapon.value && vr.hand_valid[VR_HAND_RIGHT] )
	{
		cl_entity_t *view = &clgame.viewent;
		vec3_t f, r, u;
		float g[3] = { 0.0f, 0.0f, 0.0f };

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
	VR_HeadCenter( head );
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
	VR_UpdateHands( rvp, head );
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
void VR_CopyDesktopToEyes( void )
{
	vr_GLint old_draw = 0, old_read = 0;
	qboolean scissor, srgb;
	int src_w, src_h, src_x, eye;

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
	vr.frames = vr.frames_rendered = vr.frames_copied = 0;
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

static void VR_SubmitFrame( void )
{
	XrFrameEndInfo info = { XR_TYPE_FRAME_END_INFO };
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView views[2];
	const XrCompositionLayerBaseHeader *layers[1];
	XrResult result;
	int eye;

	if( !vr.frame_begun )
		return;

	info.displayTime = vr.display_time;
	info.environmentBlendMode = vr.blend_mode;
	info.layerCount = 0;
	info.layers = NULL;

	// a frame where the eyes weren't filled (loading, shouldRender false)
	// goes out with no layers; the runtime keeps showing its last image
	if( vr.eyes_written )
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

	Con_Printf( "VR: room scale %s, player at %.1f %.1f %.1f yaw %.0f, head %.1f units away, wall fade %.2f\n",
		vr_roomscale.value ? "on" : "off", cl.simorg[0], cl.simorg[1], cl.simorg[2], cl.viewangles[YAW],
		sqrtf( vr.room_gap[0] * vr.room_gap[0] + vr.room_gap[1] * vr.room_gap[1] ), vr.fade );

	{
		int hand;

		for( hand = 0; hand < 2; hand++ )
		{
			if( vr.hand_valid[hand] )
				Con_Printf( "VR: %s hand at %.1f %.1f %.1f, aiming %.0f %.0f %.0f\n", hand ? "right" : "left",
					vr.hand_origin[hand][0], vr.hand_origin[hand][1], vr.hand_origin[hand][2],
					vr.hand_angles[hand][0], vr.hand_angles[hand][1], vr.hand_angles[hand][2] );
			else Con_Printf( "VR: %s hand not tracked\n", hand ? "right" : "left" );
		}
		Con_Printf( "VR: weapon model at %.1f %.1f %.1f\n", clgame.viewent.origin[0], clgame.viewent.origin[1], clgame.viewent.origin[2] );
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
	Cmd_AddCommand( "vr_recenter", VR_Recenter_f, "put the player back under the real head" );

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
