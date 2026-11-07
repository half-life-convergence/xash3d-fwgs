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
static PFN_xrGetActionStateVector2f            pxrGetActionStateVector2f;
static PFN_xrGetCurrentInteractionProfile      pxrGetCurrentInteractionProfile;

static CVAR_DEFINE_AUTO( vr_turnmode, "0", FCVAR_ARCHIVE, "right stick turning: 0 = snap (comfortable), 1 = smooth" );
static CVAR_DEFINE_AUTO( vr_snapturn, "45", FCVAR_ARCHIVE, "degrees per snap turn" );
static CVAR_DEFINE_AUTO( vr_turnspeed, "120", FCVAR_ARCHIVE, "smooth turning speed, degrees per second" );
static CVAR_DEFINE_AUTO( vr_debug_sticks, "0 0 0 0", 0, "developer: pretend the sticks are at \"left_x left_y right_x right_y\", to test without controllers" );

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
};

typedef struct
{
	vr_action_t action;
	const char *path;
} vr_binding_t;

// Suggested bindings. SteamVR adapts them to any controller it knows,
// including ones without a profile here (the Steam Frame's), and players
// can rebind everything in SteamVR's own controller binding screen.
static const vr_binding_t vr_bindings_index[] =
{
	{ VR_ACT_MOVE, "/user/hand/left/input/thumbstick" },
	{ VR_ACT_TURN, "/user/hand/right/input/thumbstick" },
	{ VR_ACT_FIRE, "/user/hand/right/input/trigger/click" },
	{ VR_ACT_ALTFIRE, "/user/hand/left/input/trigger/click" },
	{ VR_ACT_JUMP, "/user/hand/right/input/a/click" },
	{ VR_ACT_RELOAD, "/user/hand/right/input/b/click" },
	{ VR_ACT_USE, "/user/hand/left/input/a/click" },
	{ VR_ACT_CROUCH, "/user/hand/left/input/b/click" },
	{ VR_ACT_MENU, "/user/hand/left/input/thumbstick/click" },
};

static const vr_binding_t vr_bindings_touch[] =
{
	{ VR_ACT_MOVE, "/user/hand/left/input/thumbstick" },
	{ VR_ACT_TURN, "/user/hand/right/input/thumbstick" },
	{ VR_ACT_FIRE, "/user/hand/right/input/trigger/value" },
	{ VR_ACT_ALTFIRE, "/user/hand/left/input/trigger/value" },
	{ VR_ACT_JUMP, "/user/hand/right/input/a/click" },
	{ VR_ACT_RELOAD, "/user/hand/right/input/b/click" },
	{ VR_ACT_USE, "/user/hand/left/input/x/click" },
	{ VR_ACT_CROUCH, "/user/hand/left/input/y/click" },
	{ VR_ACT_MENU, "/user/hand/left/input/menu/click" },
};

static const vr_binding_t vr_bindings_simple[] =
{
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

	// this frame's state, read by VR_PreCreateMove
	int          buttons; // IN_ bits held
	qboolean     menu;
	XrVector2f   move;
	XrVector2f   turn;

	// edge tracking
	qboolean     menu_was_down;
	qboolean     snap_armed; // the turn stick went back to center since the last snap
	qboolean     flick_armed; // same for weapon flicks
} vri;

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
		&& VR_LOAD_PROC( xrGetActionStateVector2f )
		&& VR_LOAD_PROC( xrGetCurrentInteractionProfile );
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
		XrActionSuggestedBinding suggested[VR_ACT_COUNT];
		XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		XrResult result;

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
	Cvar_RegisterVariable( &vr_debug_sticks );

	if( !VR_LoadInputProcs( ))
		return false;

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

	vri.snap_armed = vri.flick_armed = true;
	vri.initialized = true;
	Con_Printf( "VR: controller actions ready (%d actions)\n", VR_ACT_COUNT );
	return true;
}

void VR_ShutdownInput( void )
{
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

static void VR_ReadActions( void )
{
	XrActiveActionSet active = { 0 };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	float sticks[4] = { 0 };
	int i;

	vri.buttons = 0;
	vri.menu = false;
	vri.move.x = vri.move.y = vri.turn.x = vri.turn.y = 0.0f;

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
			vri.move = VR_Vector( VR_ACT_MOVE );
			vri.turn = VR_Vector( VR_ACT_TURN );
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
buttons and the move stick. The stick moves where the head faces: the
head's yaw in the play space is folded in, so "forward" is wherever the
player looks, whichever way the game body points.
*/
void VR_CreateMove( usercmd_t *cmd )
{
	float mx, my, h, dx, dy, fwd, side;

	if( !vri.initialized || !VR_Active( ))
		return;

	cmd->buttons |= vri.buttons;

	mx = VR_Deadzone( vri.move.x );
	my = VR_Deadzone( vri.move.y );
	if( mx == 0.0f && my == 0.0f )
		return;

	// head forward = (cos h, sin h), head right = (sin h, -cos h) in the
	// play space, which is the player's frame (X forward, Y left)
	h = DEG2RAD( VR_HeadYaw( ));
	dx = my * cosf( h ) + mx * sinf( h );
	dy = my * sinf( h ) - mx * cosf( h );

	fwd = Cvar_VariableValue( "cl_forwardspeed" );
	side = Cvar_VariableValue( "cl_sidespeed" );
	if( fwd <= 0.0f ) fwd = 400.0f;
	if( side <= 0.0f ) side = 400.0f;

	cmd->forwardmove += dx * fwd;
	cmd->sidemove += -dy * side; // sidemove is positive to the right
}

#endif // XASH_OPENXR
