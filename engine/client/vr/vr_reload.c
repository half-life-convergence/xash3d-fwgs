/*
vr_reload.c - Half-Life: Convergence: reloading by hand
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
#include "entity_types.h"
#include "const.h" // IN_ buttons

// Reloading by hand: the left hand reaches over a shoulder and squeezes grip
// to pull out ammo for the gun in the right hand (a haptic tick says the hand
// is in the right place), then brings it to the gun's magazine well. There
// the engine presses reload for the game: ammo counts, timing, animation and
// sounds stay the game's own, the hands only decide when the reload happens.
//
// With vr_manualreload 1 (the default) the game doesn't reload on its own
// when the clip runs dry, and the controller's reload button does nothing
// (it's kept for the magazine eject). The game DLLs read vr_manualreload
// themselves (VR_ReloadAllowed in hlsdk). vr_manualreload 0 brings back the
// button and the automatic reload; the hand reload works in both.
//
// Which weapons: Half-Life's with a clip, from the table below (keyed by the
// view model the game asks for). Where the magazine goes is in the hand's
// axes (forward, right, up) from the grip; our own VR weapon models can move
// it with a "magwell <forward> <right> <up> <radius>" line in models/vr/v_X.txt.
// The ammo in the hand is Half-Life's own pickup model, or ours from
// models/vr/ under the same name. Everything is loaded with the map.
static CVAR_DEFINE_AUTO( vr_manualreload, "1", FCVAR_ARCHIVE, "reload by reaching over the shoulder for ammo and putting it in the gun; 0 = the reload button and automatic reloads (the hand reload still works)" );

#define VR_SHOULDER_BACK    1.0f  // game units behind the head center the zone starts
#define VR_SHOULDER_DOWN    12.0f // ... down to this far below the head center (~30 cm)
#define VR_SHOULDER_REACH   18.0f // ... and this far out to the side or back (~45 cm)
#define VR_RELOAD_HOLD      0.6f  // grip squeezed this far takes the ammo (like VR_GRAB_HOLD)
#define VR_RELOAD_LET_GO    0.4f  // ... below this drops it
#define VR_RELOAD_PRESS     1.0f  // seconds reload is held at most, waiting for the game to start it
#define VR_RELOAD_PRESS_BLIND 0.3f // ... when the game sends no weapon data (cl_lw 0)

typedef struct
{
	const char *viewmodel; // Half-Life's, the one the game sets
	const char *ammo;      // in the hand
	int         maxclip;   // the game's, so a full gun can't be reloaded
	vec3_t      magwell;   // forward right up from the grip
	float       radius;
} vr_reload_weapon_t;

// first guesses for Half-Life's models, to be tuned in the headset
static const vr_reload_weapon_t vr_reload_weapons[] =
{
	{ "v_9mmhandgun.mdl", "w_9mmclip.mdl",       17, { -1.0f, 0.0f, -4.0f }, 4.0f }, // into the grip from below
	{ "v_9mmAR.mdl",      "w_9mmARclip.mdl",     50, {  5.0f, 0.0f, -3.0f }, 4.0f }, // in front of the trigger guard
	{ "v_357.mdl",        "w_357ammo.mdl",        6, {  3.0f, 0.0f,  2.0f }, 4.0f }, // the cylinder
	{ "v_shotgun.mdl",    "w_shotshell.mdl",      8, {  8.0f, 0.0f, -2.0f }, 4.0f }, // the loading port underneath
	{ "v_crossbow.mdl",   "w_crossbow_clip.mdl",  5, {  6.0f, 0.0f,  3.0f }, 5.0f }, // on top
	{ "v_rpg.mdl",        "w_rpgammo.mdl",        1, { -6.0f, 0.0f,  2.0f }, 6.0f }, // the back of the tube
};

static struct
{
	model_t      *ammo[ARRAYSIZE( vr_reload_weapons )];
	int           seq_hold; // hands.mdl "hold_ammo", or VR_POSE_FIST

	const vr_reload_weapon_t *holding; // the ammo in the left hand is for this weapon, NULL = none
	int           holding_index;
	qboolean      in_zone;
	qboolean      squeeze_down; // the left grip was already squeezed, so a new squeeze is needed
	double        press_start;  // reload is pressed for the game since then
	double        press_until;
	cl_entity_t   ent;

	// what vr_info reports
	float         magwell_dist;
} vrr;

void VR_ReloadInit( void )
{
	Cvar_RegisterVariable( &vr_manualreload );
	vrr.seq_hold = VR_POSE_FIST;
}

qboolean VR_ManualReload( void )
{
	return vr_manualreload.value != 0.0f;
}

void VR_LoadReloadModels( void )
{
	char path[MAX_QPATH];

	memset( vrr.ammo, 0, sizeof( vrr.ammo ));
	vrr.holding = NULL;
	vrr.press_until = 0.0;

	if( !VR_Active( ))
		return;

	for( size_t i = 0; i < ARRAYSIZE( vr_reload_weapons ); i++ )
	{
		Q_snprintf( path, sizeof( path ), "models/vr/%s", vr_reload_weapons[i].ammo );
		if( !FS_FileExists( path, false ))
			Q_snprintf( path, sizeof( path ), "models/%s", vr_reload_weapons[i].ammo );
		if( FS_FileExists( path, false ))
			vrr.ammo[i] = Mod_ForName( path, false, false );
		if( vrr.ammo[i] && vrr.ammo[i]->type != mod_studio )
			vrr.ammo[i] = NULL;
	}

	vrr.seq_hold = VR_HandSequence( "hold_ammo" );
	if( vrr.seq_hold < 0 )
		vrr.seq_hold = VR_POSE_FIST;
}

static int VR_ReloadWeaponIndex( const model_t *weapon )
{
	const char *base;

	if( !weapon )
		return -1;

	base = COM_FileWithoutPath( weapon->name );
	for( size_t i = 0; i < ARRAYSIZE( vr_reload_weapons ); i++ )
	{
		if( !Q_stricmp( base, vr_reload_weapons[i].viewmodel ))
			return (int)i;
	}
	return -1;
}

// the game's own state of the gun in hand, from the last server frame;
// false when it doesn't send any (cl_lw 0)
static qboolean VR_GunState( const weapon_data_t **wd, int *reserve )
{
	const frame_t *frame = &cl.frames[cl.parsecountmod];
	int id = frame->clientdata.m_iId;

	if( !cl_lw.value || id <= 0 || id >= MAX_LOCAL_WEAPONS )
		return false;

	*wd = &frame->weapondata[id];
	*reserve = (int)frame->clientdata.vuser4[1];
	return true;
}

// there's room in the gun and ammo to put in it
static qboolean VR_ReloadNeeded( const vr_reload_weapon_t *w )
{
	const weapon_data_t *wd;
	int reserve;

	if( !VR_GunState( &wd, &reserve ))
		return true; // the game decides
	if( wd->m_fInReload || wd->m_fInSpecialReload )
		return false;
	return reserve > 0 && wd->m_iClip >= 0 && wd->m_iClip < w->maxclip;
}

/*
VR_ShoulderZone

Over either shoulder: behind the head center, not far below it, within
reach. Measured in the head's yaw only, so looking up or down doesn't
move the zone.
*/
static qboolean VR_ShoulderZone( const vec3_t hand, const vec3_t head, float head_yaw )
{
	vec3_t rel;
	float back, side, h = DEG2RAD( head_yaw );

	VectorSubtract( hand, head, rel );
	back = -( rel[0] * cosf( h ) + rel[1] * sinf( h ));
	side = rel[0] * sinf( h ) - rel[1] * cosf( h );

	return back > VR_SHOULDER_BACK && rel[2] > -VR_SHOULDER_DOWN
		&& back * back + side * side < VR_SHOULDER_REACH * VR_SHOULDER_REACH;
}

/*
VR_UpdateReload

From VR_UpdateHands, after both hands are placed and before the left one
meets the gun (VR_OffhandWeapon, which leaves a hand holding ammo alone).
weapon is Half-Life's model the game set, drawn our VR model or the same.
gun_origin/gun_angles is the right hand as drawn, off_tracked where the
left controller is.
*/
void VR_UpdateReload( const model_t *weapon, const model_t *drawn, qboolean gun_valid, const vec3_t gun_origin, const vec3_t gun_angles,
	qboolean off_valid, const vec3_t off_tracked, const vec3_t head_origin, float head_yaw )
{
	const vr_weapon_shape_t *shape;
	int index = VR_ReloadWeaponIndex( weapon );
	const vr_reload_weapon_t *w = index >= 0 ? &vr_reload_weapons[index] : NULL;
	float trigger, squeeze, radius;
	vec3_t f, r, u, magwell, ofs, spot;
	qboolean zone, usable;

	vrr.magwell_dist = -1.0f;

	usable = w && gun_valid && off_valid && cls.state == ca_active && cls.key_dest == key_game && cl.local.health > 0;
	if( !usable )
	{
		vrr.holding = NULL;
		vrr.in_zone = false;
		vrr.squeeze_down = true;
		return;
	}

	VR_HandFingers( VR_HAND_LEFT, &trigger, &squeeze );

	if( vrr.holding && ( vrr.holding != w || squeeze < VR_RELOAD_LET_GO ))
		vrr.holding = NULL; // dropped, or the weapon was switched

	// take ammo from over the shoulder with a fresh squeeze
	zone = VR_ShoulderZone( off_tracked, head_origin, head_yaw ) && VR_ReloadNeeded( w );
	if( zone && !vrr.in_zone && !vrr.holding )
		VR_Haptic( VR_HAND_LEFT, 0.02f, 0.3f ); // a tick: ammo is here
	vrr.in_zone = zone;

	if( !vrr.holding && zone && !vrr.squeeze_down && squeeze >= VR_RELOAD_HOLD )
	{
		vrr.holding = w;
		vrr.holding_index = index;
		VR_Haptic( VR_HAND_LEFT, 0.05f, 0.6f );
	}

	if( squeeze >= VR_RELOAD_HOLD )
		vrr.squeeze_down = true;
	else if( squeeze < VR_RELOAD_LET_GO )
		vrr.squeeze_down = false;

	if( !vrr.holding )
		return;

	// to the magazine well
	VectorCopy( w->magwell, magwell );
	radius = w->radius;
	if(( shape = VR_WeaponShape( drawn )) && shape->has_magwell )
	{
		VectorCopy( shape->magwell, magwell );
		radius = shape->magwell_radius;
	}

	AngleVectors( gun_angles, f, r, u );
	VectorScale( f, magwell[0], ofs );
	VectorMA( ofs, magwell[1], r, ofs );
	VectorMA( ofs, magwell[2], u, ofs );
	VectorAdd( gun_origin, ofs, spot );

	vrr.magwell_dist = VectorDistance( off_tracked, spot );
	if( vrr.magwell_dist < radius )
	{
		vrr.holding = NULL;
		vrr.press_start = host.realtime;
		vrr.press_until = host.realtime + ( cl_lw.value ? VR_RELOAD_PRESS : VR_RELOAD_PRESS_BLIND );
		VR_Haptic( VR_HAND_LEFT, 0.08f, 1.0f );
		VR_Haptic( VR_HAND_RIGHT, 0.08f, 0.8f );
		Con_Reportf( "VR: ammo in %s, reloading\n", weapon->name );
	}
}

qboolean VR_ReloadHolding( void )
{
	return vrr.holding != NULL;
}

int VR_ReloadHandPose( void )
{
	return vrr.seq_hold;
}

/*
VR_ReloadCommand

From VR_CreateMove: while the ammo has just gone in, the command presses
reload (and not fire, or the game's weapon code never gets to the reload)
until the game has started it.
*/
void VR_ReloadCommand( usercmd_t *cmd )
{
	const weapon_data_t *wd;
	int reserve;

	if( host.realtime >= vrr.press_until )
		return;

	if( VR_GunState( &wd, &reserve ) && host.realtime > vrr.press_start + 0.05
		&& ( wd->m_fInReload || wd->m_fInSpecialReload ))
	{
		vrr.press_until = 0.0; // started
		return;
	}

	SetBits( cmd->buttons, IN_RELOAD );
	ClearBits( cmd->buttons, IN_ATTACK|IN_ATTACK2 );
}

/*
VR_AddHeldAmmo

After VR_AddHands: the ammo in the left hand, at its grip.
*/
void VR_AddHeldAmmo( const vec3_t origin, const vec3_t angles )
{
	cl_entity_t *ent = &vrr.ent;
	model_t *mod;

	if( !vrr.holding || !( mod = vrr.ammo[vrr.holding_index] ) || cls.state != ca_active )
		return;

	memset( ent, 0, sizeof( *ent ));
	ent->model = mod;
	ent->curstate.rendermode = kRenderNormal;
	ent->curstate.renderamt = 255;

	VectorCopy( origin, ent->origin );
	VectorCopy( angles, ent->angles );
	ent->angles[PITCH] = -ent->angles[PITCH]; // models use the opposite pitch sign
	VectorCopy( ent->origin, ent->curstate.origin );
	VectorCopy( ent->angles, ent->curstate.angles );
	VectorCopy( ent->origin, ent->latched.prevorigin );
	VectorCopy( ent->angles, ent->latched.prevangles );
	ent->curstate.animtime = cl.time;

	CL_AddVisibleEntity( ent, ET_NORMAL );
}

void VR_ReloadInfo( void )
{
	const weapon_data_t *wd;
	int reserve;

	if( VR_GunState( &wd, &reserve ))
		Con_Printf( "VR: gun: clip %d, reserve %d%s\n", wd->m_iClip, reserve,
			( wd->m_fInReload || wd->m_fInSpecialReload ) ? ", reloading" : "" );

	if( vrr.holding )
		Con_Printf( "VR: reload: left hand holds %s for %s, %.1f units from the magazine well\n",
			vrr.ammo[vrr.holding_index] ? vrr.ammo[vrr.holding_index]->name : vrr.holding->ammo,
			vrr.holding->viewmodel, vrr.magwell_dist );
	else if( vrr.in_zone )
		Con_Printf( "VR: reload: left hand over the shoulder, ammo in reach\n" );
	else Con_Printf( "VR: reload: %s%s\n", vr_manualreload.value ? "by hand" : "button and automatic, or by hand",
		host.realtime < vrr.press_until ? ", pressing reload" : "" );
}

#endif // XASH_OPENXR
