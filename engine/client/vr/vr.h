/*
vr.h - Half-Life: Convergence VR layer (OpenXR)
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

#ifndef VR_H
#define VR_H

#if XASH_OPENXR

// called from CL_Init after VID_Init and from CL_Shutdown before R_Shutdown
// VR only starts when the engine was launched with -vr, so a VR-enabled
// build behaves exactly like the desktop build otherwise
void VR_Init( void );
void VR_Shutdown( void );
qboolean VR_Active( void );

#endif // XASH_OPENXR

#endif // VR_H
