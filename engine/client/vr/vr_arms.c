/*
vr_arms.c - Half-Life: Convergence: finds the arms built into weapon models
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
#include "studio.h"

#if XASH_OPENXR

#include "vr.h"

// Half-Life's weapon models (v_*.mdl) have the player's arms built in. In VR
// the weapon sits in the tracked hand, so the renderer leaves them out
// (RF_VIEWMODEL_NOARMS). Nothing is changed on disk: the player's own files
// are marked in memory when they load.
//
// The arms always use their own textures. Most models keep them in submodels
// of their own; some (glock, MP5, shotgun, crowbar) put them in the gun's
// submodel as separate meshes. rubbergloveCHROME is also used for a few
// weapon parts (crossbow limbs, satchel, detonator), but only in models whose
// arms have a submodel of their own, so in those models only the arm-only
// submodels are left out.
static const char *const vr_arm_textures[] =
{
	"GLOVED_sleeve",
	"GLOVED_knuckle",
	"GLOVE_handpak",
	"rubbergloveCHROME",
	"xbow_sleeve",
	"HAND_ForeArm1",
	"PLAYER_ForeArm1",
	"PLAYER_Cuff1",
	"PLAYER_Chrome2",
};

static qboolean VR_IsArmTexture( const char *name )
{
	for( size_t i = 0; i < ARRAYSIZE( vr_arm_textures ); i++ )
	{
		size_t len = Q_strlen( vr_arm_textures[i] );

		// names keep their .bmp (GLOVE_handpak even has it twice)
		if( !Q_strnicmp( name, vr_arm_textures[i], len ) && ( name[len] == '\0' || name[len] == '.' ))
			return true;
	}

	return false;
}

void VR_MarkViewmodelArms( const char *modelname, studiohdr_t *phdr )
{
	mstudiotexture_t *ptexture;
	mstudiobodyparts_t *pbodypart;
	const short *pskinref;
	int marked = 0;

	ClearBits( phdr->flags, STUDIO_VR_ARMS_SUBMODEL );

	if( phdr->numtextures <= 0 || phdr->textureindex <= 0 || phdr->numskinref <= 0 || phdr->skinindex <= 0 )
		return;

	ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	for( int i = 0; i < phdr->numtextures; i++ )
		ClearBits( ptexture[i].flags, STUDIO_NF_VR_ARM );

	// only Half-Life's weapon models: scientists and guards keep their
	// hands, and our own VR models (models/vr/) have no arms built in
	if( Q_strnicmp( COM_FileWithoutPath( modelname ), "v_", 2 ) || !Q_strnicmp( modelname, "models/vr/", 10 ))
		return;

	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( VR_IsArmTexture( ptexture[i].name ))
		{
			SetBits( ptexture[i].flags, STUDIO_NF_VR_ARM );
			marked++;
		}
	}

	if( !marked )
		return;

	// skin family 0 decides, the others only swap colours
	pskinref = (const short *)((byte *)phdr + phdr->skinindex);
	pbodypart = (mstudiobodyparts_t *)((byte *)phdr + phdr->bodypartindex);

	for( int i = 0; i < phdr->numbodyparts; i++ )
	{
		mstudiomodel_t *psubmodel = (mstudiomodel_t *)((byte *)phdr + pbodypart[i].modelindex);

		for( int j = 0; j < pbodypart[i].nummodels; j++ )
		{
			mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)phdr + psubmodel[j].meshindex);
			qboolean arms_only = psubmodel[j].nummesh > 0;

			for( int k = 0; k < psubmodel[j].nummesh && arms_only; k++ )
			{
				int skinref = pmesh[k].skinref;
				int tex = ( skinref >= 0 && skinref < phdr->numskinref ) ? pskinref[skinref] : -1;

				if( tex < 0 || tex >= phdr->numtextures || !FBitSet( ptexture[tex].flags, STUDIO_NF_VR_ARM ))
					arms_only = false;
			}

			if( arms_only )
				SetBits( phdr->flags, STUDIO_VR_ARMS_SUBMODEL );
		}
	}

	Con_Reportf( "VR: %s: %d arm textures, arms %s\n", modelname, marked,
		FBitSet( phdr->flags, STUDIO_VR_ARMS_SUBMODEL ) ? "in submodels of their own" : "mixed into the weapon's meshes" );
}

#endif // XASH_OPENXR
