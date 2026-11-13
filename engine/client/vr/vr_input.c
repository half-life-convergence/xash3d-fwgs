/*
vr_input.c - OpenXR controller actions for Half-Life: Convergence
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
#include "const.h" // IN_ buttons

static PFN_xrStringToPath                      pxrStringToPath;
static PFN_xrPathToString                      pxrPathToString;
static PFN_xrCreateActionSet                   pxrCreateActionSet;
static PFN_xrDestroyActionSet                  pxrDestroyActionSet;
static PFN_xrCreateAction                      pxrCreateAction;
static PFN_xrSuggestInteractionProfileBindings pxrSuggestInteractionProfileBindings;
static PFN_xrAttachSessionActionSets           pxrAttachSessionActionSets;
static PFN_xrSyncActions                       pxrSyncActions;
static PFN_xrGetActionStateBoolean             pxrGetActionStateBoolean;
static PFN_xrGetActionStateFloat               pxrGetActionStateFloat;
static PFN_xrGetActionStateVector2f            pxrGetActionStateVector2f;
static PFN_xrGetCurrentInteractionProfile      pxrGetCurrentInteractionProfile;
static PFN_xrCreateActionSpace                 pxrCreateActionSpace;
static PFN_xrLocateSpace                       pxrLocateSpace;
static PFN_xrDestroySpace                      pxrDestroySpace;

static CVAR_DEFINE_AUTO( vr_turnmode, "0", FCVAR_ARCHIVE, "right stick turning: 0 = snap (comfortable), 1 = smooth" );
static CVAR_DEFINE_AUTO( vr_snapturn, "45", FCVAR_ARCHIVE, "degrees per snap turn" );
static CVAR_DEFINE_AUTO( vr_turnspeed, "120", FCVAR_ARCHIVE, "smooth turning speed, degrees per second" );
static CVAR_DEFINE_AUTO( vr_movespeed, "4", FCVAR_ARCHIVE, "stick movement speed at full tilt in metres per second (the original game runs at ~8); 0 = the original full speed" );
static CVAR_DEFINE_AUTO( vr_autocrouch, "1", FCVAR_ARCHIVE, "jumping ducks the player until landing, like a crouch jump, so ledges and vents need only the jump button" );
static CVAR_DEFINE_AUTO( vr_crouchtoggle, "1", FCVAR_ARCHIVE, "the crouch button: 1 = press to toggle ducking, 0 = hold to duck" );
static CVAR_DEFINE_AUTO( vr_movedir, "0", FCVAR_ARCHIVE, "the move stick goes where: 0 = the head looks, 1 = the left hand points" );
static CVAR_DEFINE_AUTO( vr_debug_sticks, "0 0 0 0", 0, "developer: pretend the sticks are at \"left_x left_y right_x right_y\", to test without controllers" );
static CVAR_DEFINE_AUTO( vr_debug_fingers, "0 0 0 0", 0, "developer: pretend the triggers and grips are at \"left_trigger left_grip right_trigger right_grip\" (0 to 1), to test the hand poses without controllers" );
static CVAR_DEFINE_AUTO( vr_debug_hands, "0", 0, "developer: \"1 [yaw pitch]\" fakes both hands held in front of the head (45 cm forward, 20 cm down), the right one turned by yaw/pitch degrees, to test without controllers" );

// Hand aim for the game DLLs. Singleplayer runs client and server in this
// process, and both DLLs can read engine cvars, so these carry the gun's
// start point without a protocol or interface change. Written by the engine
// every command, do not set by hand.
static CVAR_DEFINE_AUTO( vr_aim_active, "0", 0, "VR: 1 while shots start at the tracked hand (set by the engine)" );
static CVAR_DEFINE_AUTO( vr_aim_x, "0", 0, "VR: gun start point relative to the player origin (set by the engine)" );
static CVAR_DEFINE_AUTO( vr_aim_y, "0", 0, "VR: gun start point relative to the player origin (set by the engine)" );
static CVAR_DEFINE_AUTO( vr_aim_z, "0", 0, "VR: gun start point relative to the player origin (set by the engine)" );

typedef enum
{
	VR_ACT_MOVE = 0, // vector2f
	VR_ACT_TURN, // vector2f: x turns, y flicks through weapons
	VR_ACT_FIRE,
	VR_ACT_ALTFIRE,
	VR_ACT_JUMP,
	VR_ACT_CROUCH,
	VR_ACT_USE,
	VR_ACT_RELOAD,
	VR_ACT_MENU,
	VR_ACT_FLASHLIGHT, // off hand: toggles the flashlight (impulse 100)
	VR_ACT_FINGER_TRIGGER, // float, per hand: how far the trigger is pulled, for the hand model
	VR_ACT_FINGER_SQUEEZE, // float, per hand: how hard the grip is squeezed, for the hand model
	VR_ACT_GRIP_POSE, // pose, per hand: where the hand holds things
	VR_ACT_AIM_POSE, // pose, per hand: the pointing ray
	VR_ACT_COUNT
} vr_action_t;

typedef struct
{
	const char *name; // lowercase, OpenXR rules
	const char *title; // shown in SteamVR's binding UI
	XrActionType type;
	int button; // IN_ bit for boolean actions that are plain buttons
} vr_action_def_t;

static const vr_action_def_t vr_action_defs[VR_ACT_COUNT] =
{
	{ "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, 0 },
	{ "turn", "Turn / weapon select", XR_ACTION_TYPE_VECTOR2F_INPUT, 0 },
	{ "fire", "Fire", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_ATTACK },
	{ "altfire", "Alt fire", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_ATTACK2 },
	{ "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_JUMP },
	{ "crouch", "Crouch", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_DUCK },
	{ "use", "Use", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_USE },
	{ "reload", "Reload", XR_ACTION_TYPE_BOOLEAN_INPUT, IN_RELOAD },
	{ "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0 },
	{ "flashlight", "Flashlight", XR_ACTION_TYPE_BOOLEAN_INPUT, 0 },
	{ "finger_trigger", "Index finger (hand pose)", XR_ACTION_TYPE_FLOAT_INPUT, 0 },
	{ "finger_squeeze", "Hand closing (hand pose)", XR_ACTION_TYPE_FLOAT_INPUT, 0 },
	{ "hand_grip", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT, 0 },
	{ "hand_aim", "Hand aim pose", XR_ACTION_TYPE_POSE_INPUT, 0 },
};

// the pose and finger actions are per hand
#define VR_ACTION_PER_HAND( i ) ( vr_action_defs[i].type == XR_ACTION_TYPE_POSE_INPUT || vr_action_defs[i].type == XR_ACTION_TYPE_FLOAT_INPUT )

typedef struct
{
	vr_action_t action;
	const char *path;
} vr_binding_t;

// Suggested bindings. SteamVR adapts them to any controller it knows,
// including ones without a profile here (the Steam Frame's), and players
// can rebind everything in SteamVR's own controller binding screen.
#define VR_HAND_POSES \
	{ VR_ACT_GRIP_POSE, "/user/hand/left/input/grip/pose" }, \
	{ VR_ACT_GRIP_POSE, "/user/hand/right/input/grip/pose" }, \
	{ VR_ACT_AIM_POSE, "/user/hand/left/input/aim/pose" }, \
	{ VR_ACT_AIM_POSE, "/user/hand/right/input/aim/pose" }

// both trigger and squeeze report a value on the Index and Touch profiles
#define VR_HAND_FINGERS 	{ VR_ACT_FINGER_TRIGGER, "/user/hand/left/input/trigger/value" }, 	{ VR_ACT_FINGER_TRIGGER, "/user/hand/right/input/trigger/value" }, 	{ VR_ACT_FINGER_SQUEEZE, "/user/hand/left/input/squeeze/value" }, 	{ VR_ACT_FINGER_SQUEEZE, "/user/hand/right/input/squeeze/value" }

static const vr_binding_t vr_bindings_index[] =
{
	VR_HAND_POSES,
	VR_HAND_FINGERS,
	{ VR_ACT_MOVE, "/user/hand/left/input/thumbstick" },
	{ VR_ACT_TURN, "/user/hand/right/input/thumbstick" },
	{ VR_ACT_FIRE, "/user/hand/right/input/trigger/click" },
	{ VR_ACT_ALTFIRE, "/user/hand/left/input/trigger/click" },
	{ VR_ACT_JUMP, "/user/hand/right/input/a/click" },
	{ VR_ACT_RELOAD, "/user/hand/right/input/b/click" },
	{ VR_ACT_USE, "/user/hand/left/input/a/click" },
	{ VR_ACT_CROUCH, "/user/hand/left/input/b/click" },
	{ VR_ACT_MENU, "/user/hand/left/input/thumbstick/click" },
	{ VR_ACT_FLASHLIGHT, "/user/hand/left/input/trackpad/force" }, // pressing the trackpad, the runtime turns the force into a click
};

static const vr_binding_t vr_bindings_touch[] =
{
	VR_HAND_POSES,
	VR_HAND_FINGERS,
	{ VR_ACT_MOVE, "/user/hand/left/input/thumbstick" },
	{ VR_ACT_TURN, "/user/hand/right/input/thumbstick" },
	{ VR_ACT_FIRE, "/user/hand/right/input/trigger/value" },
	{ VR_ACT_ALTFIRE, "/user/hand/left/input/trigger/value" },
	{ VR_ACT_JUMP, "/user/hand/right/input/a/click" },
	{ VR_ACT_RELOAD, "/user/hand/right/input/b/click" },
	{ VR_ACT_USE, "/user/hand/left/input/x/click" },
	{ VR_ACT_CROUCH, "/user/hand/left/input/y/click" },
	{ VR_ACT_MENU, "/user/hand/left/input/menu/click" },
	{ VR_ACT_FLASHLIGHT, "/user/hand/left/input/thumbstick/click" },
};

static const vr_binding_t vr_bindings_simple[] =
{
	VR_HAND_POSES,
	{ VR_ACT_FIRE, "/user/hand/right/input/select/click" },
	{ VR_ACT_USE, "/user/hand/left/input/select/click" },
	{ VR_ACT_MENU, "/user/hand/left/input/menu/click" },
};

static const struct
{
	const char *profile;
	const vr_binding_t *bindings;
	size_t count;
} vr_profiles[] =
{
	{ "/interaction_profiles/valve/index_controller", vr_bindings_index, ARRAYSIZE( vr_bindings_index ) },
	{ "/interaction_profiles/oculus/touch_controller", vr_bindings_touch, ARRAYSIZE( vr_bindings_touch ) },
	{ "/interaction_profiles/khr/simple_controller", vr_bindings_simple, ARRAYSIZE( vr_bindings_simple ) },
};

static struct
{
	qboolean     initialized;
	XrActionSet  set;
	XrAction     actions[VR_ACT_COUNT];
	XrPath       hand_paths[2]; // left, right
	XrSpace      hand_spaces[2][2]; // [grip, aim][left, right]

	// this frame's state, read by VR_PreCreateMove
	int          buttons; // IN_ bits held
	qboolean     menu;
	qboolean     flashlight;
	XrVector2f   move;
	XrVector2f   turn;
	float        fingers[2][2]; // [left, right][trigger, squeeze], 0 to 1

	// edge tracking
	qboolean     menu_was_down;
	qboolean     flashlight_was_down;
	qboolean     flashlight_debug_press; // vr_debug_flashlightpress
	qboolean     flashlight_pending; // pressed, VR_CreateMove sends impulse 100
	qboolean     headlamp_pull; // the left trigger was pulled at the head flashlight's switch and is still held
	qboolean     snap_armed; // the turn stick went back to center since the last snap
	qboolean     flick_armed; // same for weapon flicks
	qboolean     crouch_was_down;
	qboolean     crouch_latched; // vr_crouchtoggle: ducked by the button
	qboolean     crouch_debug_press; // vr_debug_crouchpress
	qboolean     jump_ducking; // vr_autocrouch: a jump is under way
	qboolean     jump_left_ground; // ... and the player has left the ground
	double       jump_time;
} vri;

static void VR_DebugCrouchPress_f( void )
{
	vri.crouch_debug_press = true;
}

static void VR_DebugFlashlightPress_f( void )
{
	vri.flashlight_debug_press = true;
}

static qboolean VR_LoadInputProcs( void )
{
	return VR_LOAD_PROC( xrStringToPath )
		&& VR_LOAD_PROC( xrPathToString )
		&& VR_LOAD_PROC( xrCreateActionSet )
		&& VR_LOAD_PROC( xrDestroyActionSet )
		&& VR_LOAD_PROC( xrCreateAction )
		&& VR_LOAD_PROC( xrSuggestInteractionProfileBindings )
		&& VR_LOAD_PROC( xrAttachSessionActionSets )
		&& VR_LOAD_PROC( xrSyncActions )
		&& VR_LOAD_PROC( xrGetActionStateBoolean )
		&& VR_LOAD_PROC( xrGetActionStateFloat )
		&& VR_LOAD_PROC( xrGetActionStateVector2f )
		&& VR_LOAD_PROC( xrGetCurrentInteractionProfile )
		&& VR_LOAD_PROC( xrCreateActionSpace )
		&& VR_LOAD_PROC( xrLocateSpace )
		&& VR_LOAD_PROC( xrDestroySpace );
}

static XrPath VR_Path( const char *str )
{
	XrPath path = XR_NULL_PATH;
	pxrStringToPath( VR_GetInstance( ), str, &path );
	return path;
}

static void VR_SuggestBindings( void )
{
	size_t p, b;

	for( p = 0; p < ARRAYSIZE( vr_profiles ); p++ )
	{
		XrActionSuggestedBinding suggested[32];
		XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		XrResult result;

		if( vr_profiles[p].count > ARRAYSIZE( suggested ))
			continue;

		for( b = 0; b < vr_profiles[p].count; b++ )
		{
			suggested[b].action = vri.actions[vr_profiles[p].bindings[b].action];
			suggested[b].binding = VR_Path( vr_profiles[p].bindings[b].path );
		}

		info.interactionProfile = VR_Path( vr_profiles[p].profile );
		info.suggestedBindings = suggested;
		info.countSuggestedBindings = (uint32_t)vr_profiles[p].count;

		// a runtime that doesn't know a profile or path refuses just that profile
		result = pxrSuggestInteractionProfileBindings( VR_GetInstance( ), &info );
		if( XR_FAILED( result ))
			Con_Printf( S_WARN "VR: bindings for %s refused: %s\n", vr_profiles[p].profile, VR_ResultString( result ));
		else Con_Reportf( "VR: suggested bindings for %s\n", vr_profiles[p].profile );
	}
}

/*
VR_InitInput

One action set with the card's actions, suggested bindings for the
common controllers, attached to the session. Attaching can only happen
once per session, so every action exists from the start.
*/
qboolean VR_InitInput( void )
{
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	XrResult result;
	int i;

	Cvar_RegisterVariable( &vr_turnmode );
	Cvar_RegisterVariable( &vr_snapturn );
	Cvar_RegisterVariable( &vr_turnspeed );
	Cvar_RegisterVariable( &vr_movespeed );
	Cvar_RegisterVariable( &vr_movedir );
	Cvar_RegisterVariable( &vr_crouchtoggle );
	Cvar_RegisterVariable( &vr_autocrouch );
	Cmd_AddCommand( "vr_debug_crouchpress", VR_DebugCrouchPress_f, "developer: press the crouch button once, to test without controllers" );
	Cmd_AddCommand( "vr_debug_flashlightpress", VR_DebugFlashlightPress_f, "developer: press the flashlight button once, to test without controllers" );
	Cvar_RegisterVariable( &vr_debug_sticks );
	Cvar_RegisterVariable( &vr_debug_hands );
	Cvar_RegisterVariable( &vr_debug_fingers );
	Cvar_RegisterVariable( &vr_aim_active );
	Cvar_RegisterVariable( &vr_aim_x );
	Cvar_RegisterVariable( &vr_aim_y );
	Cvar_RegisterVariable( &vr_aim_z );

	if( !VR_LoadInputProcs( ))
		return false;

	vri.hand_paths[0] = VR_Path( "/user/hand/left" );
	vri.hand_paths[1] = VR_Path( "/user/hand/right" );

	Q_strncpy( set_info.actionSetName, "gameplay", sizeof( set_info.actionSetName ));
	Q_strncpy( set_info.localizedActionSetName, "Gameplay", sizeof( set_info.localizedActionSetName ));
	result = pxrCreateActionSet( VR_GetInstance( ), &set_info, &vri.set );
	if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrCreateActionSet failed: %s\n", VR_ResultString( result ));
		return false;
	}

	for( i = 0; i < VR_ACT_COUNT; i++ )
	{
		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };

		Q_strncpy( info.actionName, vr_action_defs[i].name, sizeof( info.actionName ));
		Q_strncpy( info.localizedActionName, vr_action_defs[i].title, sizeof( info.localizedActionName ));
		info.actionType = vr_action_defs[i].type;
		if( VR_ACTION_PER_HAND( i ))
		{
			info.countSubactionPaths = 2;
			info.subactionPaths = vri.hand_paths;
		}

		result = pxrCreateAction( vri.set, &info, &vri.actions[i] );
		if( XR_FAILED( result ))
		{
			Con_Printf( S_ERROR "VR: xrCreateAction %s failed: %s\n", vr_action_defs[i].name, VR_ResultString( result ));
			return false;
		}
	}

	VR_SuggestBindings( );

	attach.countActionSets = 1;
	attach.actionSets = &vri.set;
	result = pxrAttachSessionActionSets( VR_GetSession( ), &attach );
	if( XR_FAILED( result ))
	{
		Con_Printf( S_ERROR "VR: xrAttachSessionActionSets failed: %s\n", VR_ResultString( result ));
		return false;
	}

	// one space per pose and hand, located against the world space every frame
	for( i = 0; i < 2; i++ )
	{
		int hand;

		for( hand = 0; hand < 2; hand++ )
		{
			XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };

			info.action = vri.actions[i ? VR_ACT_AIM_POSE : VR_ACT_GRIP_POSE];
			info.subactionPath = vri.hand_paths[hand];
			info.poseInActionSpace.orientation.w = 1.0f;

			result = pxrCreateActionSpace( VR_GetSession( ), &info, &vri.hand_spaces[i][hand] );
			if( XR_FAILED( result ))
			{
				Con_Printf( S_ERROR "VR: xrCreateActionSpace failed: %s\n", VR_ResultString( result ));
				return false;
			}
		}
	}

	vri.snap_armed = vri.flick_armed = true;
	vri.initialized = true;
	Con_Printf( "VR: controller actions ready (%d actions)\n", VR_ACT_COUNT );
	return true;
}

/*
VR_GetHandPose

where a hand is at the given time in the given space: which = 0 for the
grip pose (where the hand holds things), 1 for the aim pose (the pointing
ray). False when the hand isn't tracked.
*/
qboolean VR_GetHandPose( int hand, int which, XrTime time, XrSpace base, XrPosef *out )
{
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	if( !vri.initialized || hand < 0 || hand > 1 || which < 0 || which > 1 )
		return false;

	if( XR_FAILED( pxrLocateSpace( vri.hand_spaces[which][hand], base, time, &loc )))
		return false;

	if(( loc.locationFlags & valid ) != valid )
		return false;

	*out = loc.pose;
	return true;
}

qboolean VR_DebugHands( float *yaw, float *pitch )
{
	float on = 0.0f, y = 0.0f, p = 0.0f;

	sscanf( vr_debug_hands.string, "%f %f %f", &on, &y, &p );
	if( yaw ) *yaw = y;
	if( pitch ) *pitch = p;
	return on != 0.0f;
}

void VR_ShutdownInput( void )
{
	int i, hand;

	for( i = 0; i < 2; i++ )
	{
		for( hand = 0; hand < 2; hand++ )
		{
			if( vri.hand_spaces[i][hand] != XR_NULL_HANDLE && pxrDestroySpace )
				pxrDestroySpace( vri.hand_spaces[i][hand] );
		}
	}

	// destroying the set destroys its actions too
	if( vri.set != XR_NULL_HANDLE && pxrDestroyActionSet )
		pxrDestroyActionSet( vri.set );

	memset( &vri, 0, sizeof( vri ));
}

void VR_InputProfileChanged( void )
{
	static const char *hands[2] = { "/user/hand/left", "/user/hand/right" };
	int i;

	if( !vri.initialized )
		return;

	for( i = 0; i < 2; i++ )
	{
		XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
		char name[XR_MAX_PATH_LENGTH] = "none";
		uint32_t len = 0;

		if( XR_SUCCEEDED( pxrGetCurrentInteractionProfile( VR_GetSession( ), VR_Path( hands[i] ), &state ))
			&& state.interactionProfile != XR_NULL_PATH )
			pxrPathToString( VR_GetInstance( ), state.interactionProfile, sizeof( name ), &len, name );

		Con_Printf( "VR: %s hand uses %s\n", i ? "right" : "left", name );
	}
}

static qboolean VR_Bool( vr_action_t act )
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

	info.action = vri.actions[act];
	return XR_SUCCEEDED( pxrGetActionStateBoolean( VR_GetSession( ), &info, &state ))
		&& state.isActive && state.currentState;
}

static XrVector2f VR_Vector( vr_action_t act )
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrVector2f zero = { 0.0f, 0.0f };

	info.action = vri.actions[act];
	if( XR_SUCCEEDED( pxrGetActionStateVector2f( VR_GetSession( ), &info, &state )) && state.isActive )
		return state.currentState;
	return zero;
}

static float VR_HandFloat( vr_action_t act, int hand )
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };

	info.action = vri.actions[act];
	info.subactionPath = vri.hand_paths[hand];
	if( XR_SUCCEEDED( pxrGetActionStateFloat( VR_GetSession( ), &info, &state )) && state.isActive )
		return bound( 0.0f, state.currentState, 1.0f );
	return 0.0f;
}

/*
VR_HandFingers

How far a hand's trigger is pulled and its grip squeezed, 0 to 1, for
the hand model's pose. vr_debug_fingers overrides it without controllers.
*/
void VR_HandFingers( int hand, float *trigger, float *squeeze )
{
	float f[4] = { 0 };

	*trigger = *squeeze = 0.0f;
	if( hand < 0 || hand > 1 )
		return;

	if( sscanf( vr_debug_fingers.string, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3] ) == 4
		&& ( f[0] || f[1] || f[2] || f[3] ))
	{
		*trigger = bound( 0.0f, f[hand * 2], 1.0f );
		*squeeze = bound( 0.0f, f[hand * 2 + 1], 1.0f );
		return;
	}

	*trigger = vri.fingers[hand][0];
	*squeeze = vri.fingers[hand][1];
}

static void VR_ReadActions( void )
{
	XrActiveActionSet active = { 0 };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	float sticks[4] = { 0 };
	int i;

	vri.buttons = 0;
	vri.menu = false;
	vri.flashlight = false;
	vri.move.x = vri.move.y = vri.turn.x = vri.turn.y = 0.0f;
	memset( vri.fingers, 0, sizeof( vri.fingers ));

	// actions only report while the session has input focus
	if( VR_SessionFocused( ))
	{
		active.actionSet = vri.set;
		active.subactionPath = XR_NULL_PATH;
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;

		if( XR_SUCCEEDED( pxrSyncActions( VR_GetSession( ), &sync )))
		{
			for( i = 0; i < VR_ACT_COUNT; i++ )
			{
				if( vr_action_defs[i].button && VR_Bool( i ))
					vri.buttons |= vr_action_defs[i].button;
			}

			vri.menu = VR_Bool( VR_ACT_MENU );
			vri.flashlight = VR_Bool( VR_ACT_FLASHLIGHT );
			vri.move = VR_Vector( VR_ACT_MOVE );
			vri.turn = VR_Vector( VR_ACT_TURN );

			for( i = 0; i < 2; i++ )
			{
				vri.fingers[i][0] = VR_HandFloat( VR_ACT_FINGER_TRIGGER, i );
				vri.fingers[i][1] = VR_HandFloat( VR_ACT_FINGER_SQUEEZE, i );
			}
		}
	}

	// developer override for testing without controllers
	if( sscanf( vr_debug_sticks.string, "%f %f %f %f", &sticks[0], &sticks[1], &sticks[2], &sticks[3] ) == 4
		&& ( sticks[0] || sticks[1] || sticks[2] || sticks[3] ))
	{
		vri.move.x = sticks[0];
		vri.move.y = sticks[1];
		vri.turn.x = sticks[2];
		vri.turn.y = sticks[3];
	}
}

static float VR_Deadzone( float v )
{
	const float dz = 0.15f;

	if( fabsf( v ) < dz )
		return 0.0f;
	return ( v - ( v > 0 ? dz : -dz )) / ( 1.0f - dz );
}

/*
VR_PreCreateMove

Before the client builds the command: read the controllers, then turn the
view (the client reads cl.viewangles in CL_CreateMove) and handle the
one-shot inputs, weapon flicks and the menu button.
*/
void VR_PreCreateMove( float frametime )
{
	float tx;

	if( !vri.initialized || !VR_Active( ))
		return;

	VR_ReadActions( );

	// menu toggles on press
	if( vri.menu && !vri.menu_was_down )
		Cbuf_AddText( "togglemenu\n" );
	vri.menu_was_down = vri.menu;

	// the flashlight toggles on press, in the game only; VR_CreateMove puts
	// impulse 100 in this frame's command, and the game does the rest
	// (sound, battery, the light), as with the keyboard
	if( vri.flashlight_debug_press )
	{
		vri.flashlight = true;
		vri.flashlight_debug_press = false;
	}
	if( vri.flashlight && !vri.flashlight_was_down && cls.key_dest == key_game )
		vri.flashlight_pending = true;
	vri.flashlight_was_down = vri.flashlight;

	// the head flashlight (vr_flashlight 0) also switches like a headlamp:
	// the left trigger pulled beside the left temple. The trigger is alt fire
	// too, so alt fire waits while the hand is there or that pull is held.
	{
		qboolean at_lamp = VR_HeadlampZone( );
		float trigger, squeeze;

		VR_HandFingers( VR_HAND_LEFT, &trigger, &squeeze );

		if( vri.headlamp_pull )
		{
			if( trigger < 0.3f )
				vri.headlamp_pull = false;
		}
		else if( at_lamp && trigger > 0.7f )
		{
			vri.headlamp_pull = true;
			if( cls.key_dest == key_game )
				vri.flashlight_pending = true;
		}

		if( at_lamp || vri.headlamp_pull )
			ClearBits( vri.buttons, IN_ATTACK2 );
	}

	tx = VR_Deadzone( vri.turn.x );

	// stick right turns right, which lowers the yaw
	if( vr_turnmode.value )
	{
		cl.viewangles[YAW] -= tx * vr_turnspeed.value * frametime;
	}
	else
	{
		if( vri.snap_armed && fabsf( vri.turn.x ) > 0.7f )
		{
			cl.viewangles[YAW] -= ( vri.turn.x > 0.0f ? 1.0f : -1.0f ) * vr_snapturn.value;
			vri.snap_armed = false;
		}
		else if( fabsf( vri.turn.x ) < 0.3f )
			vri.snap_armed = true;
	}
	cl.viewangles[YAW] = anglemod( cl.viewangles[YAW] );

	// flick the turn stick up or down to change weapons, only when it's
	// clearly vertical so a diagonal turn doesn't switch weapons too
	if( vri.flick_armed && fabsf( vri.turn.y ) > 0.7f && fabsf( vri.turn.y ) > 2.0f * fabsf( vri.turn.x ))
	{
		Cbuf_AddText( vri.turn.y > 0.0f ? "invprev\n" : "invnext\n" );
		vri.flick_armed = false;
	}
	else if( fabsf( vri.turn.y ) < 0.3f )
		vri.flick_armed = true;
}

/*
VR_CreateMove

After the client built the command from the keyboard: add the held
buttons and the move stick. The stick moves where the head faces, or
with vr_movedir 1 where the left hand points: that yaw in the play space
is folded in, so "forward" is wherever the player looks (or points),
whichever way the game body points. Full tilt is vr_movespeed metres per
second; starts and stops stay as instant as in the original, since slow
ramps are worse for comfort than sudden ones.
*/
static void VR_SetAimCvar( convar_t *var, float value )
{
	if( var->value != value )
		Cvar_DirectSetValue( var, value );
}

/*
VR_AimFromHand

While the weapon is in the tracked right hand, shots must go where it
points. The command's view angles are what every weapon fires along
(pev->v_angle on the server, the event angles on the client) and the VR
eyes no longer use them, so they become the hand's aim. Movement in the
command is relative to those angles, so it is turned from the body yaw
into the new yaw, and walking keeps going where it went. The start point
is published through the vr_aim_* cvars for GetGunPosition and
EV_GetGunPosition.
*/
static void VR_AimFromHand( usercmd_t *cmd )
{
	vec3_t origin, angles;
	float old_yaw, new_yaw, delta, s, c, f, r;

	if( !VR_RightHandAim( origin, angles ))
	{
		VR_SetAimCvar( &vr_aim_active, 0.0f );
		return;
	}

	old_yaw = cmd->viewangles[YAW];
	new_yaw = angles[YAW];

	// same world direction, expressed in the new yaw: rotate (forward, right)
	// by the yaw difference (yaw grows to the left, right is -left)
	delta = DEG2RAD( new_yaw - old_yaw );
	s = sinf( delta );
	c = cosf( delta );
	f = cmd->forwardmove;
	r = cmd->sidemove;
	cmd->forwardmove = f * c - r * s;
	cmd->sidemove = f * s + r * c;

	VectorCopy( angles, cmd->viewangles );

	VR_SetAimCvar( &vr_aim_x, origin[0] - cl.simorg[0] );
	VR_SetAimCvar( &vr_aim_y, origin[1] - cl.simorg[1] );
	VR_SetAimCvar( &vr_aim_z, origin[2] - cl.simorg[2] );
	VR_SetAimCvar( &vr_aim_active, 1.0f );
}

/*
VR_CrouchButtons

The held buttons, with ducking worked out: with vr_crouchtoggle the
crouch button toggles a latched duck instead of holding it, so long vent
crawls don't need a button held down. Crouching for real (VR_PhysicalCrouch)
ducks too; either one keeps the player ducked. Player movement only
stands up when there's room, so letting go in a vent stays ducked until
the player is out.

With vr_autocrouch, a jump (from the controller or the keyboard) ducks
from the moment the player leaves the ground until he lands, like a
crouch jump. Ducking in the air is instant in player movement, and the
view doesn't move when the legs tuck up (see VR_UpdateStairs).
*/
static int VR_CrouchButtons( int cmdbuttons )
{
	int buttons = vri.buttons;
	qboolean onground = cl.local.onground != -1;
	qboolean down = FBitSet( buttons, IN_DUCK ) || vri.crouch_debug_press;

	vri.crouch_debug_press = false;

	if( cls.state != ca_active || cl.local.health <= 0 )
		vri.crouch_latched = false; // dead, loading: start the next life standing

	if( vr_crouchtoggle.value )
	{
		ClearBits( buttons, IN_DUCK );
		if( down && !vri.crouch_was_down )
		{
			vri.crouch_latched = !vri.crouch_latched;
			Con_Reportf( "VR: crouch %s\n", vri.crouch_latched ? "on" : "off" );
		}
		if( vri.crouch_latched )
			SetBits( buttons, IN_DUCK );
	}
	else vri.crouch_latched = false;

	vri.crouch_was_down = down;

	if( VR_PhysicalCrouch( ))
		SetBits( buttons, IN_DUCK );

	if( vr_autocrouch.value && cls.state == ca_active )
	{
		if( FBitSet( buttons | cmdbuttons, IN_JUMP ) && onground && !vri.jump_ducking )
		{
			vri.jump_ducking = true;
			vri.jump_left_ground = false;
			vri.jump_time = host.realtime;
		}

		if( vri.jump_ducking )
		{
			if( !onground )
				vri.jump_left_ground = true;
			else if( vri.jump_left_ground || host.realtime - vri.jump_time > 0.3 )
				vri.jump_ducking = false; // landed, or the jump never happened (a ceiling)
		}

		if( vri.jump_ducking && vri.jump_left_ground )
			SetBits( buttons, IN_DUCK );
	}
	else vri.jump_ducking = false;

	return buttons;
}

void VR_CreateMove( usercmd_t *cmd )
{
	float mx, my, h, dx, dy, fwd, side, yaw;

	if( !vri.initialized || !VR_Active( ))
		return;

	cmd->buttons |= VR_CrouchButtons( cmd->buttons );

	// the flashlight button, unless the command already carries an impulse
	// (it waits for the next one then)
	if( vri.flashlight_pending && !cmd->impulse )
	{
		cmd->impulse = 100;
		vri.flashlight_pending = false;
	}

	mx = VR_Deadzone( vri.move.x );
	my = VR_Deadzone( vri.move.y );
	if( mx != 0.0f || my != 0.0f )
	{
		// head forward = (cos h, sin h), head right = (sin h, -cos h) in the
		// play space, which is the player's frame (X forward, Y left)
		if( !vr_movedir.value || !VR_HandYaw( VR_HAND_LEFT, &yaw ))
			yaw = VR_HeadYaw( ); // and when the left hand isn't tracked
		h = DEG2RAD( yaw );
		dx = my * cosf( h ) + mx * sinf( h );
		dy = my * sinf( h ) - mx * cosf( h );

		if( vr_movespeed.value > 0.0f )
		{
			// player movement caps the speed at sv_maxspeed (320), so anything
			// below that comes out exactly; the keyboard keeps its own speeds
			fwd = side = vr_movespeed.value * Q_max( 1.0f, Cvar_VariableValue( "vr_worldscale" ));
		}
		else
		{
			fwd = Cvar_VariableValue( "cl_forwardspeed" );
			side = Cvar_VariableValue( "cl_sidespeed" );
			if( fwd <= 0.0f ) fwd = 400.0f;
			if( side <= 0.0f ) side = 400.0f;
		}

		cmd->forwardmove += dx * fwd;
		cmd->sidemove += -dy * side; // sidemove is positive to the right
	}

	// last: it turns all of the command's movement, keyboard included
	VR_AimFromHand( cmd );
}

#endif // XASH_OPENXR
