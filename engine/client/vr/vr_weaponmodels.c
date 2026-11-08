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
static CVAR_DEFINE_AUTO( vr_weaponmodels, "1", FCVAR_ARCHIVE, "hold our own VR weapon models (models/vr/) instead of Half-Life's where they exist" );

#define VR_MAX_WEAPON_MODELS 32

static struct
{
	const model_t *original;
	model_t *vr;
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
		vr_num_weapon_models++;
		Con_Reportf( "VR: %s replaces %s\n", path, mod->name );
	}
}

void VR_LoadModels( void )
{
	VR_LoadWeaponModels( );
	VR_LoadHandModel( );
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
