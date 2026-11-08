/*
vr_patch.c - Half-Life: Convergence: patch models for the held weapon
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

// Half-Life's weapon models leave out the faces the desktop camera never saw.
// A patch model of our own fills them in: for models/v_X.mdl it is
// models/vr/v_X.mdl, built on the same bone names. The client's studio
// renderer draws it right after the weapon, with the weapon's bones copied
// by name (like the weapon models players hold in multiplayer).
//
// Patches are loaded here, while the map loads, so drawing never touches the
// disk. vr_patches lists the weapon models that have one; the client only asks
// for those, and gets them from the model cache.
CVAR_DEFINE_AUTO( vr_patches, "", FCVAR_READ_ONLY, "weapon models with a VR patch model loaded for this map (set by the engine, read by the client)" );

void VR_LoadPatchModels( void )
{
	char list[MAX_VA_STRING];
	char path[MAX_QPATH];
	int count = 0;

	list[0] = '\0';

	if( VR_Active( ))
	{
		for( int i = 1; i < cl.nummodels && i < MAX_MODELS; i++ )
		{
			const model_t *mod = cl.models[i];
			const char *base;

			if( !mod || mod->type != mod_studio )
				continue;

			base = COM_FileWithoutPath( mod->name );
			if( Q_strnicmp( base, "v_", 2 ))
				continue;

			Q_snprintf( path, sizeof( path ), "models/vr/%s", base );
			if( !FS_FileExists( path, false ) || !Mod_ForName( path, false, false ))
				continue;

			if( Q_strlen( list ) + Q_strlen( base ) + 2 >= sizeof( list ))
			{
				Con_Printf( S_WARN "VR: too many patch models, %s left out\n", base );
				continue;
			}

			if( list[0] )
				Q_strncat( list, " ", sizeof( list ));
			Q_strncat( list, base, sizeof( list ));
			count++;
		}
	}

	Cvar_DirectFullSet( &vr_patches, list, vr_patches.flags ); // Cvar_DirectSet refuses read-only cvars

	if( count )
		Con_Reportf( "VR: %d weapon patch models: %s\n", count, list );
}

#endif // XASH_OPENXR
