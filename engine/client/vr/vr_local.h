/*
vr_local.h - Half-Life: Convergence VR layer, shared between its own files
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

#ifndef VR_LOCAL_H
#define VR_LOCAL_H

#if XASH_OPENXR

#include "vr.h"

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

//
// vr_openxr.c
//
XrInstance VR_GetInstance( void );
XrSession VR_GetSession( void );
qboolean VR_SessionFocused( void );
qboolean VR_LoadProc( XrInstance instance, const char *name, PFN_xrVoidFunction *func );
const char *VR_ResultString( XrResult result );
float VR_HeadYaw( void ); // degrees, where the head faces in the play space, game convention
qboolean VR_HandYaw( int hand, float *yaw ); // degrees, where a hand points in the play space; false if not tracked
qboolean VR_PhysicalCrouch( void ); // the real head is below vr_crouchheight
qboolean VR_RightHandAim( vec3_t origin, vec3_t angles ); // the weapon hand in the world (grip position, aim angles in view convention), false if not tracked or vr_handweapon 0

extern convar_t vr_patches; // vr_patch.c, registered by VR_Init

#define VR_LOAD_PROC( name ) VR_LoadProc( VR_GetInstance( ), #name, (PFN_xrVoidFunction *)&p##name )

//
// vr_input.c
//
qboolean VR_InitInput( void ); // after the session exists, attaches the action set
void VR_ShutdownInput( void );
void VR_InputProfileChanged( void );
qboolean VR_GetHandPose( int hand, int which, XrTime time, XrSpace base, XrPosef *out ); // hand 0 left 1 right, which 0 grip 1 aim
qboolean VR_DebugHands( float *yaw, float *pitch ); // vr_debug_hands "1 [yaw pitch]": fake hands without controllers, the right one turned by yaw/pitch

#define VR_HAND_LEFT  0
#define VR_HAND_RIGHT 1
#define VR_POSE_GRIP  0
#define VR_POSE_AIM   1

#endif // XASH_OPENXR

#endif // VR_LOCAL_H
