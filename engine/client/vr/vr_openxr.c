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

#include "vr.h"
#include "platform/platform.h"
#include "ref_common.h" // refState, the window size
#if XASH_SDL == 2
#include <SDL.h> // SDL_GL_SetSwapInterval for the desktop mirror window
#endif

// the loader is opened in runtime, so only xrGetInstanceProcAddr is linked
// by name and everything else is fetched through it
#define XR_NO_PROTOTYPES 1
#if XASH_WIN32
#include <windows.h>
#include <unknwn.h> // IUnknown, used by openxr_platform.h on win32
#define XR_USE_PLATFORM_WIN32 1
#endif
#define XR_USE_GRAPHICS_API_OPENGL 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

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
	vr_GLuint   fbo; // draw target for copying into swapchain images

	XrView   views[2]; // eye poses and fovs at display_time
	qboolean views_valid;
	qboolean eyes_written; // both eye images were filled this frame

	uint32_t frames; // frames submitted since the session began
	double   begin_time;
	double   wait_total; // seconds spent blocked in xrWaitFrame
	double   wait_max;
	qboolean warned_wait; // print a failing xrWaitFrame only once
} vr;

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
static const char *VR_ResultString( XrResult result )
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

static qboolean VR_GetProc( XrInstance instance, const char *name, PFN_xrVoidFunction *func )
{
	XrResult result = pxrGetInstanceProcAddr( instance, name, func );

	if( XR_FAILED( result ) || !*func )
	{
		Con_Printf( S_ERROR "VR: can't get %s: %s\n", name, VR_ResultString( result ));
		return false;
	}

	return true;
}

#define VR_GET_PROC( instance, name ) VR_GetProc(( instance ), #name, (PFN_xrVoidFunction *)&p##name )

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
	}

	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	views[0].next = views[1].next = NULL;

	result = pxrEnumerateViewConfigurationViews( vr.instance, vr.system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, ARRAYSIZE( views ), &count, views );

	if( XR_FAILED( result ) || count != 2 )
	{
		Con_Printf( S_ERROR "VR: headset has no stereo view configuration: %s\n", VR_ResultString( result ));
		return false;
	}

	vr.eye_width = views[0].recommendedImageRectWidth;
	vr.eye_height = views[0].recommendedImageRectHeight;
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

static qboolean VR_CreateSwapchains( void )
{
	int eye;

	if( !VR_LoadGL( ))
		return false;

	vr.swapchain_format = VR_PickSwapchainFormat( );
	if( !vr.swapchain_format )
	{
		Con_Printf( S_ERROR "VR: runtime offers no RGBA8 or SRGB8_ALPHA8 swapchain format\n" );
		return false;
	}

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

	vrgl.GenFramebuffers( 1, &vr.fbo );

	Con_Printf( "VR: eye swapchains %ux%u, %s, %u images each\n", vr.eye_width, vr.eye_height,
		vr.swapchain_format == VR_GL_SRGB8_ALPHA8 ? "SRGB8_ALPHA8" : "RGBA8", vr.num_images[0] );
	return true;
}

static void VR_DestroySwapchains( void )
{
	int eye;

	if( vr.fbo && vrgl.DeleteFramebuffers )
		vrgl.DeleteFramebuffers( 1, &vr.fbo );
	vr.fbo = 0;

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
}

/*
VR_CopyDesktopToEyes

Called right before the desktop window swaps. For now each eye gets the
finished desktop frame, center-cropped to the eye's aspect ratio, so the
whole swapchain -> projection layer -> pacing path works before the
renderer learns to draw each eye itself.
*/
void VR_CopyDesktopToEyes( void )
{
	vr_GLint old_draw = 0, old_read = 0;
	qboolean scissor, srgb;
	int src_w, src_h, src_x, eye;

	if( !vr.frame_begun || !vr.should_render || !vr.views_valid || !vr.fbo )
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
			vr.eyes_written = true;
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
	vr.frames = 0;
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
}

qboolean VR_Active( void )
{
	return vr.initialized;
}

void VR_Init( void )
{
	Cmd_AddCommand( "vr_info", VR_Info_f, "print OpenXR runtime and headset state" );

	if( !Sys_CheckParm( "-vr" ))
		return;

	if( !Sys_LoadLibrary( &openxr_loader_info ))
	{
		Con_Printf( S_ERROR "VR: can't load %s, VR disabled\n", openxr_loader_info.name );
		return;
	}

	if( !VR_CreateInstance( ) || !VR_GetSystem( ) || !VR_CreateSession( ) || !VR_CreateSpace( ) || !VR_CreateSwapchains( ))
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
