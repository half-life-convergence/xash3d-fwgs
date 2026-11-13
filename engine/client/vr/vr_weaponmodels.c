/*
vr_weaponmodels.c - Half-Life: Convergence: our own VR weapon models
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

// Half-Life's weapon models were made for a fixed desktop camera: the arms
// are built in, faces the camera never saw are missing, and the animations
// move the whole gun. Our own VR models replace them one weapon at a time:
// for models/v_X.mdl, models/vr/v_X.mdl is held instead when it exists.
//
// A VR model has no arms, is complete from every side, has its origin where
// the hand grips it and only animates moving parts, so the fixes for the
// originals (arm hiding, pinning, two-sided drawing, vr_viewmodel_grip) are
// skipped for it. It must keep the original's animations in the same order,
// because the game plays them by number, and their events (muzzle flash
// 5001, sounds 5004). Weapons without one keep those fixes.
//
// VR models are loaded here, while the map loads, so swapping them in
// (VR_WeaponModel, from VR_UpdateHands) never touches the disk.
//
// Each VR model also gets a shape the off hand collides with and the points
// it can grab (VR_WeaponShape, used by VR_OffhandWeapon):
//
// - the shape is the model's hitboxes ($hbox), or one box per bone around
//   its vertices when it has none, at the idle pose;
// - grab points come from a text file next to the model, models/vr/v_X.txt,
//   one line each:
//
//   grab <name> <forward> <right> <up> <pitch> <yaw> <roll> <radius> <animation>
//
//   position in model units from the grip, angles turn the off hand relative
//   to the gun (0 0 0 points it the same way), radius is how close the hand
//   snaps from, animation is the pose from hands.mdl (falls back to
//   grab_default, then a closed "hand"). "//" starts a comment.
static CVAR_DEFINE_AUTO( vr_weaponmodels, "1", FCVAR_ARCHIVE, "hold our own VR weapon models (models/vr/) instead of Half-Life's where they exist" );

#define VR_MAX_WEAPON_MODELS 32
#define VR_MAX_SHAPE_BONES   128

static struct
{
	const model_t *original;
	model_t *vr;
	vr_weapon_shape_t shape;
} vr_weapon_models[VR_MAX_WEAPON_MODELS];
static int vr_num_weapon_models;

void VR_WeaponModelsInit( void )
{
	Cvar_RegisterVariable( &vr_weaponmodels );
}

qboolean VR_IsVRWeaponModel( const model_t *mod )
{
	return mod && !Q_strnicmp( mod->name, "models/vr/", 10 );
}

/*
VR_IdleBones

A model's skeleton at the first frame of its first animation (the idle),
in model space. False if it can't be read here (too many bones, the
animation in a separate file).
*/
qboolean VR_IdleBones( studiohdr_t *hdr, matrix3x4 *bones, int max )
{
	mstudioseqdesc_t *seq;
	mstudiobone_t *pbones;
	mstudioanim_t *panim;

	if( !hdr || hdr->numbones <= 0 || hdr->numbones > max || hdr->numseq <= 0 )
		return false;

	seq = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex);
	if( seq->seqgroup != 0 )
		return false; // animation in a separate file

	pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	panim = (mstudioanim_t *)((byte *)hdr + seq->animindex);

	// bones come parents first
	for( int i = 0; i < hdr->numbones; i++ )
	{
		matrix3x4 local;
		vec3_t pos;
		vec4_t q;

		R_StudioCalcBones( 0, 0.0f, &pbones[i], &panim[i], NULL, pos, q );
		Matrix3x4_FromOriginQuat( local, q, pos );

		if( pbones[i].parent < 0 || pbones[i].parent >= i )
			Matrix3x4_Copy( bones[i], local );
		else Matrix3x4_ConcatTransforms( bones[i], bones[pbones[i].parent], local );
	}

	return true;
}

// a box given in a bone's space, into the hand's axes (model space is
// forward, left, up; the hand's is forward, right, up)
static void VR_AddShapeBox( vr_weapon_shape_t *shape, const matrix3x4 bone, const vec3_t mins, const vec3_t maxs )
{
	vr_weapon_box_t *box;
	vec3_t mid;

	if( shape->num_boxes == VR_MAX_WEAPON_BOXES )
		return;

	box = &shape->boxes[shape->num_boxes++];
	VectorAverage( mins, maxs, mid );
	Matrix3x4_VectorTransform( bone, mid, box->center );
	box->center[1] = -box->center[1];

	for( int j = 0; j < 3; j++ )
	{
		box->axis[j][0] = bone[0][j];
		box->axis[j][1] = -bone[1][j];
		box->axis[j][2] = bone[2][j];
		VectorNormalize( box->axis[j] );
		box->half[j] = Q_max( 0.0f, ( maxs[j] - mins[j] ) * 0.5f );
	}
}

static void VR_BuildShape( model_t *mod, vr_weapon_shape_t *shape )
{
	static matrix3x4 bones[VR_MAX_SHAPE_BONES];
	studiohdr_t *hdr = Mod_StudioExtradata( mod );
	int i, j;

	shape->num_boxes = 0;
	shape->from_hitboxes = false;

	if( !VR_IdleBones( hdr, bones, VR_MAX_SHAPE_BONES ))
		return;

	if( hdr->numhitboxes > 0 )
	{
		const mstudiobbox_t *hb = (const mstudiobbox_t *)((byte *)hdr + hdr->hitboxindex);

		shape->from_hitboxes = true;
		for( i = 0; i < hdr->numhitboxes; i++ )
		{
			if( hb[i].bone >= 0 && hb[i].bone < hdr->numbones )
				VR_AddShapeBox( shape, bones[hb[i].bone], hb[i].bbmin, hb[i].bbmax );
		}
		return;
	}

	// no hitboxes: the vertices of the default bodies, one box per bone
	// (vertices are stored in their bone's space)
	for( int b = 0; b < hdr->numbones; b++ )
	{
		const mstudiobodyparts_t *parts = (const mstudiobodyparts_t *)((byte *)hdr + hdr->bodypartindex);
		vec3_t mins, maxs;
		qboolean any = false;

		for( i = 0; i < hdr->numbodyparts; i++ )
		{
			const mstudiomodel_t *sub;
			const vec3_t *verts;
			const byte *vertbone;

			if( parts[i].nummodels <= 0 )
				continue;

			sub = (const mstudiomodel_t *)((byte *)hdr + parts[i].modelindex);
			verts = (const vec3_t *)((byte *)hdr + sub->vertindex);
			vertbone = (const byte *)hdr + sub->vertinfoindex;

			for( j = 0; j < sub->numverts; j++ )
			{
				if( vertbone[j] != b )
					continue;
				if( !any )
				{
					VectorCopy( verts[j], mins );
					VectorCopy( verts[j], maxs );
					any = true;
				}
				else AddPointToBounds( verts[j], mins, maxs );
			}
		}

		if( any )
			VR_AddShapeBox( shape, bones[b], mins, maxs );
	}
}

// models/vr/v_X.txt next to models/vr/v_X.mdl, see the top of the file
static void VR_LoadGrabPoints( const char *modelpath, vr_weapon_shape_t *shape )
{
	char path[MAX_QPATH], token[64];
	char *data, *p;

	shape->num_grabs = 0;

	Q_strncpy( path, modelpath, sizeof( path ));
	COM_ReplaceExtension( path, ".txt", sizeof( path ));
	if( !FS_FileExists( path, false ) || !( data = (char *)FS_LoadFile( path, NULL, false )))
		return;

	p = data;
	while(( p = COM_ParseFile( p, token, sizeof( token ))) != NULL )
	{
		vr_grab_point_t *g;
		float v[7];

		if( Q_stricmp( token, "grab" ))
		{
			Con_Printf( S_WARN "VR: %s: unknown \"%s\", the rest is ignored\n", path, token );
			break;
		}

		if( shape->num_grabs == VR_MAX_GRAB_POINTS )
		{
			Con_Printf( S_WARN "VR: %s: more than %d grab points, the rest is ignored\n", path, VR_MAX_GRAB_POINTS );
			break;
		}

		g = &shape->grabs[shape->num_grabs];
		if( !( p = COM_ParseFile( p, g->name, sizeof( g->name ))))
			break;

		for( int i = 0; i < 7; i++ )
		{
			if( !( p = COM_ParseFile( p, token, sizeof( token ))))
				break;
			v[i] = Q_atof( token );
		}

		if( !p || !( p = COM_ParseFile( p, g->pose, sizeof( g->pose ))))
		{
			Con_Printf( S_WARN "VR: %s: grab point \"%s\" is incomplete\n", path, g->name );
			break;
		}

		// the file says forward right up, like the hand's axes
		VectorSet( g->origin, v[0], v[1], v[2] );
		VectorSet( g->angles, v[3], v[4], v[5] );
		g->radius = Q_max( 0.5f, v[6] );
		g->seq = VR_POSE_FIST;
		shape->num_grabs++;
	}

	Mem_Free( data );
}

void VR_LoadWeaponModels( void )
{
	char path[MAX_QPATH];

	vr_num_weapon_models = 0;

	if( !VR_Active( ))
		return;

	for( int i = 1; i < cl.nummodels && i < MAX_MODELS; i++ )
	{
		const model_t *mod = cl.models[i];
		const char *base;
		model_t *vrmod;

		if( !mod || mod->type != mod_studio )
			continue;

		base = COM_FileWithoutPath( mod->name );
		if( Q_strnicmp( base, "v_", 2 ))
			continue;

		Q_snprintf( path, sizeof( path ), "models/vr/%s", base );
		if( !FS_FileExists( path, false ) || !( vrmod = Mod_ForName( path, false, false )))
			continue;

		if( vrmod->type != mod_studio )
		{
			Con_Printf( S_WARN "VR: %s isn't a studio model, ignored\n", path );
			continue;
		}

		if( vr_num_weapon_models == VR_MAX_WEAPON_MODELS )
		{
			Con_Printf( S_WARN "VR: too many VR weapon models, %s left out\n", path );
			break;
		}

		vr_weapon_models[vr_num_weapon_models].original = mod;
		vr_weapon_models[vr_num_weapon_models].vr = vrmod;
		VR_BuildShape( vrmod, &vr_weapon_models[vr_num_weapon_models].shape );
		VR_LoadGrabPoints( path, &vr_weapon_models[vr_num_weapon_models].shape );
		Con_Reportf( "VR: %s replaces %s: %d boxes from its %s, %d grab points\n", path, mod->name,
			vr_weapon_models[vr_num_weapon_models].shape.num_boxes,
			vr_weapon_models[vr_num_weapon_models].shape.from_hitboxes ? "hitboxes" : "vertices",
			vr_weapon_models[vr_num_weapon_models].shape.num_grabs );
		vr_num_weapon_models++;
	}
}

// the grab points' hand animations, once both kinds of model are loaded
static void VR_ResolveGrabPoses( void )
{
	int fallback = VR_HandSequence( "grab_default" );

	for( int i = 0; i < vr_num_weapon_models; i++ )
	{
		vr_weapon_shape_t *shape = &vr_weapon_models[i].shape;

		for( int j = 0; j < shape->num_grabs; j++ )
		{
			int seq = VR_HandSequence( shape->grabs[j].pose );

			if( seq < 0 )
				seq = fallback;
			shape->grabs[j].seq = seq >= 0 ? seq : VR_POSE_FIST;
		}
	}
}

void VR_LoadModels( void )
{
	VR_LoadWeaponModels( );
	VR_LoadHandModel( );
	VR_ResolveGrabPoses( );
}

const vr_weapon_shape_t *VR_WeaponShape( const model_t *vrmodel )
{
	for( int i = 0; vrmodel && i < vr_num_weapon_models; i++ )
	{
		if( vr_weapon_models[i].vr == vrmodel )
			return &vr_weapon_models[i].shape;
	}

	return NULL;
}

model_t *VR_WeaponModel( const model_t *original )
{
	if( !original || !vr_weaponmodels.value )
		return NULL;

	for( int i = 0; i < vr_num_weapon_models; i++ )
	{
		if( vr_weapon_models[i].original == original )
			return vr_weapon_models[i].vr;
	}

	return NULL;
}

#endif // XASH_OPENXR
