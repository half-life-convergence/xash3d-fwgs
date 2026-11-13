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

// vr_weaponmodels.c
//
// The shape of a VR weapon model and the points the off hand can grab, in
// the hand's axes (forward, right, up) with the grip at 0, the idle pose
// (sequence 0, frame 0). Worked out while the map loads.
#define VR_MAX_WEAPON_BOXES 16
#define VR_MAX_GRAB_POINTS  4
#define VR_POSE_FIST        -2 // grab point pose missing from hands.mdl: closed "hand"

typedef struct
{
	vec3_t center;
	vec3_t axis[3]; // the box's own axes, unit length
	vec3_t half;    // half the size along each of them
} vr_weapon_box_t;

typedef struct
{
	char   name[32];
	vec3_t origin;   // where the off hand's grip goes
	vec3_t angles;   // the off hand's angles relative to the gun, pitch yaw roll
	float  radius;   // the hand snaps from this close
	char   pose[32]; // hands.mdl animation
	int    seq;      // pose resolved, or VR_POSE_FIST
} vr_grab_point_t;

typedef struct
{
	int             num_boxes;
	vr_weapon_box_t boxes[VR_MAX_WEAPON_BOXES];
	qboolean        from_hitboxes; // else one box per bone from the vertices
	int             num_grabs;
	vr_grab_point_t grabs[VR_MAX_GRAB_POINTS];
} vr_weapon_shape_t;

void VR_WeaponModelsInit( void );                // registers its cvar, from VR_Init
model_t *VR_WeaponModel( const model_t *original ); // our VR model held instead of original, or NULL
qboolean VR_IsVRWeaponModel( const model_t *mod );  // one of ours (models/vr/), needs none of the fixes for Half-Life's
const vr_weapon_shape_t *VR_WeaponShape( const model_t *vrmodel ); // for one of our models, NULL if it has no shape
qboolean VR_IdleBones( studiohdr_t *hdr, matrix3x4 *bones, int max ); // a model's skeleton at sequence 0 frame 0, in model space
void VR_LoadWeaponModels( void );

// vr_hands.c
void VR_HandsInit( void );      // registers its cvars, from VR_Init
void VR_LoadHandModel( void );  // with the map, from VR_LoadModels
int VR_HandSequence( const char *name ); // a hands.mdl animation by name, -1 if missing
void VR_AddHands( const qboolean valid[2], const vec3_t origin[2], const vec3_t angles[2], const model_t *held, const int pose[2] ); // end of VR_UpdateHands; pose: -1 = from the fingers, else a grab pose
void VR_HandsInfo( void );      // for vr_info

// vr_input.c
void VR_HandFingers( int hand, float *trigger, float *squeeze ); // 0 to 1, for the hand poses

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
