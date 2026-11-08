/*
vr_hands.c - Half-Life: Convergence: the player's hands
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
#include "studio.h"
#include "entity_types.h"

// The hands are our own model, models/vr/hands.mdl, drawn as two entities the
// engine owns, wherever the tracked hands are (kept out of walls). The weapon is a separate model the right hand holds. Contract with
// the model:
//
// - the origin is where the hand grips, like our VR weapon models, so a held
//   VR weapon lines up with the hand without offsets;
// - a bodypart named "hands" with four submodels: 0 right bare, 1 right HEV,
//   2 left bare, 3 left HEV (HEV once the player has the suit);
// - animations found by name: "grip_<weapon>" while the right hand holds that
//   weapon (grip_9mmAR for v_9mmAR.mdl), else "grip_default"; an empty hand
//   plays "hand". In all of them blend 0 follows the trigger (index finger);
//   in "hand" the frame follows the grip (open to fist).
//
// The model is loaded with the map (VR_LoadHandModel), never while drawing.
static CVAR_DEFINE_AUTO( vr_hands, "1", FCVAR_ARCHIVE, "draw the player's hands (models/vr/hands.mdl)" );
static CVAR_DEFINE_AUTO( vr_debug_handsuit, "-1", 0, "developer: hands without (0) or with (1) the HEV suit; -1 = the player's real suit" );

#define VR_WEAPON_SUIT 31 // Half-Life's suit bit in the weapons bits
#define VR_GRIP_CACHE  32

static struct
{
	model_t     *model;
	cl_entity_t  ents[2];
	int          seq_hand;          // empty hand, -1 if the model has none
	int          seq_grip_default;  // holding a weapon without its own grip
	int          bodypart;          // the "hands" bodypart, -1 if missing
	int          body_base;
	int          body_count;

	struct
	{
		const model_t *weapon;
		int seq;
	} grips[VR_GRIP_CACHE];
	int num_grips;

	// what vr_info reports
	int          shown_seq[2];
} vrh;

void VR_HandsInit( void )
{
	Cvar_RegisterVariable( &vr_hands );
	Cvar_RegisterVariable( &vr_debug_handsuit );
}

static int VR_FindSequence( const studiohdr_t *hdr, const char *name )
{
	const mstudioseqdesc_t *seq = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex);

	for( int i = 0; i < hdr->numseq; i++ )
	{
		if( !Q_stricmp( seq[i].label, name ))
			return i;
	}

	return -1;
}

void VR_LoadHandModel( void )
{
	const char *path = "models/vr/hands.mdl";
	studiohdr_t *hdr;

	vrh.model = NULL;
	vrh.num_grips = 0;

	if( !VR_Active( ) || !FS_FileExists( path, false ))
		return;

	vrh.model = Mod_ForName( path, false, false );
	if( !vrh.model || vrh.model->type != mod_studio || !( hdr = Mod_StudioExtradata( vrh.model )) || hdr->numseq <= 0 )
	{
		Con_Printf( S_WARN "VR: %s isn't a usable studio model, no hands\n", path );
		vrh.model = NULL;
		return;
	}

	vrh.seq_hand = VR_FindSequence( hdr, "hand" );
	vrh.seq_grip_default = VR_FindSequence( hdr, "grip_default" );

	vrh.bodypart = -1;
	vrh.body_base = 1;
	vrh.body_count = 1;
	{
		const mstudiobodyparts_t *parts = (const mstudiobodyparts_t *)((const byte *)hdr + hdr->bodypartindex);

		for( int i = 0; i < hdr->numbodyparts; i++ )
		{
			if( !Q_stricmp( parts[i].name, "hands" ))
			{
				vrh.bodypart = i;
				vrh.body_base = Q_max( 1, parts[i].base );
				vrh.body_count = Q_max( 1, parts[i].nummodels );
				break;
			}
		}
	}

	Con_Reportf( "VR: hands from %s: %d animations, %s, %s, %s\n", path, hdr->numseq,
		vrh.seq_hand >= 0 ? "\"hand\"" : "no \"hand\"",
		vrh.seq_grip_default >= 0 ? "\"grip_default\"" : "no \"grip_default\"",
		vrh.bodypart >= 0 ? va( "%d hand bodies", vrh.body_count ) : "no \"hands\" bodypart" );
}

// the right hand's animation for holding this weapon (Half-Life's model, so
// grip_9mmAR stays right after our VR model is swapped in)
static int VR_GripSequence( const model_t *weapon )
{
	studiohdr_t *hdr = Mod_StudioExtradata( vrh.model );
	char name[64];
	const char *base;
	int i, seq;

	for( i = 0; i < vrh.num_grips; i++ )
	{
		if( vrh.grips[i].weapon == weapon )
			return vrh.grips[i].seq;
	}

	base = COM_FileWithoutPath( weapon->name );
	if( !Q_strnicmp( base, "v_", 2 ))
		base += 2;
	Q_snprintf( name, sizeof( name ), "grip_%s", base );
	COM_StripExtension( name );

	seq = hdr ? VR_FindSequence( hdr, name ) : -1;
	if( seq < 0 )
		seq = vrh.seq_grip_default;
	if( seq < 0 )
		seq = 0;

	if( vrh.num_grips < VR_GRIP_CACHE )
	{
		vrh.grips[vrh.num_grips].weapon = weapon;
		vrh.grips[vrh.num_grips].seq = seq;
		vrh.num_grips++;
	}

	return seq;
}

static qboolean VR_HasSuit( void )
{
	if( vr_debug_handsuit.value >= 0.0f )
		return vr_debug_handsuit.value != 0.0f;
	return FBitSet( cl.local.weapons, BIT( VR_WEAPON_SUIT )) != 0;
}

/*
VR_AddHands

Called at the end of VR_UpdateHands, before the eyes are drawn: puts both
hands where the tracked hands are and adds them to this frame's scene
(CL_EmitEntities already built it, R_ClearScene only runs next frame).
valid/origin/angles are the hands as drawn (origin kept out of walls,
angles in view convention); held is Half-Life's weapon model in the right
hand, or NULL.
*/
void VR_AddHands( const qboolean valid[2], const vec3_t origin[2], const vec3_t angles[2], const model_t *held )
{
	const qboolean suit = VR_HasSuit( );

	vrh.shown_seq[0] = vrh.shown_seq[1] = -1;

	if( !vrh.model || !vr_hands.value || cls.state != ca_active || cl.local.health <= 0 )
		return;

	for( int hand = 0; hand < 2; hand++ )
	{
		cl_entity_t *ent = &vrh.ents[hand];
		float trigger, squeeze;
		int seq, sub;

		if( !valid[hand] )
			continue;

		VR_HandFingers( hand, &trigger, &squeeze );

		if( hand == VR_HAND_RIGHT && held )
		{
			seq = VR_GripSequence( held );
			squeeze = 0.0f; // the grip animation already holds the gun
		}
		else seq = vrh.seq_hand >= 0 ? vrh.seq_hand : 0;

		memset( ent, 0, sizeof( *ent ));
		ent->model = vrh.model;
		ent->curstate.rendermode = kRenderNormal;
		ent->curstate.renderamt = 255;

		VectorCopy( origin[hand], ent->origin );
		VectorCopy( angles[hand], ent->angles );
		ent->angles[PITCH] = -ent->angles[PITCH]; // models use the opposite pitch sign
		VectorCopy( ent->origin, ent->curstate.origin );
		VectorCopy( ent->angles, ent->curstate.angles );
		VectorCopy( ent->origin, ent->latched.prevorigin );
		VectorCopy( ent->angles, ent->latched.prevangles );

		// a still pose: the frame and blend come from the fingers, not from time
		ent->curstate.sequence = seq;
		ent->latched.prevsequence = seq;
		ent->curstate.animtime = cl.time;
		ent->curstate.framerate = 0.0f;
		ent->curstate.frame = squeeze * 255.0f;
		ent->curstate.blending[0] = ent->latched.prevblending[0] = (byte)( trigger * 255.0f );

		sub = ( hand == VR_HAND_LEFT ? 2 : 0 ) + ( suit ? 1 : 0 );
		ent->curstate.body = ( vrh.bodypart >= 0 ) ? Q_min( sub, vrh.body_count - 1 ) * vrh.body_base : 0;

		CL_AddVisibleEntity( ent, ET_NORMAL );
		vrh.shown_seq[hand] = seq;
	}
}

void VR_HandsInfo( void )
{
	studiohdr_t *hdr;

	if( !vrh.model || !( hdr = Mod_StudioExtradata( vrh.model )))
	{
		Con_Printf( "VR: hands: no models/vr/hands.mdl\n" );
		return;
	}

	for( int hand = 0; hand < 2; hand++ )
	{
		const mstudioseqdesc_t *seq = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex);
		int s = vrh.shown_seq[hand];

		Con_Printf( "VR: %s hand: %s, body %d\n", hand == VR_HAND_LEFT ? "left" : "right",
			( s >= 0 && s < hdr->numseq ) ? seq[s].label : "not drawn", vrh.ents[hand].curstate.body );
	}
}

#endif // XASH_OPENXR
