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

// the loader is opened in runtime, so only xrGetInstanceProcAddr is linked
// by name and everything else is fetched through it
#define XR_NO_PROTOTYPES 1
#include <openxr/openxr.h>
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
} vr;

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
		|| !VR_GET_PROC( vr.instance, xrEnumerateViewConfigurationViews ))
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

	return true;
}

static void VR_Info_f( void )
{
	if( !vr.initialized )
	{
		Con_Printf( "VR: not active (start the engine with -vr)\n" );
		return;
	}

	Con_Printf( "VR: active, eye size %ux%u\n", vr.eye_width, vr.eye_height );
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

	if( !VR_CreateInstance( ) || !VR_GetSystem( ))
	{
		VR_Shutdown( );
		return;
	}

	vr.initialized = true;
	Con_Printf( "VR: OpenXR ready\n" );
}

void VR_Shutdown( void )
{
	if( vr.instance != XR_NULL_HANDLE && pxrDestroyInstance )
		pxrDestroyInstance( vr.instance );

	Sys_FreeLibrary( &openxr_loader_info );
	memset( &vr, 0, sizeof( vr ));
}

#endif // XASH_OPENXR
