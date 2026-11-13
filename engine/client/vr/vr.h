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

// called at the start of every client frame, never blocks
void VR_PollEvents( void );

// frame loop, called from the Host_Main loop around COM_Frame:
// VR_WaitFrame blocks in xrWaitFrame until the headset wants the next frame
// and then calls xrBeginFrame, VR_EndFrame submits it with xrEndFrame.
// Both do nothing unless the session is running.
void VR_WaitFrame( void );
void VR_EndFrame( void );

// true while xrWaitFrame paces frames, the engine's own fps limiter must
// stay out of the way then. Only true once eye images are submitted:
// SteamVR doesn't throttle xrWaitFrame for frames without layers.
qboolean VR_PacesFrames( void );

// called by Host_RunFrame while a changelevel is queued: true holds it
// back for this frame, so the eyes can fade to black before the engine
// blocks in the load (vr_levelfade)
qboolean VR_HoldLevelChange( void );

// called by V_RenderView before the desktop pass: draws the 3D view into
// both eye swapchains through the renderer (RF_EXTERNAL_TARGET) and says
// what the desktop pass has to do
#define VR_EYES_NONE     0 // eyes not drawn, desktop pass runs normally
#define VR_EYES_RENDERED 1 // eyes drawn, desktop pass runs as RF_REPEAT_PASS (vr_mirror 1)
#define VR_EYES_MIRRORED 2 // eyes drawn and the left eye copied into the desktop view, skip the desktop pass
struct ref_viewpass_s;
int VR_RenderEyes( const struct ref_viewpass_s *rvp );

// for frames whose eyes were drawn: puts the head center's position and
// orientation (between the eyes, facing where the head faces) into rvp,
// so the sound listener follows the real head instead of the body camera
void VR_ListenerView( struct ref_viewpass_s *rvp );

// room scale, called by CL_CreateCmd and after the first prediction of a
// command: the request rides in usercmd_t.impact_position, pm_shared
// writes back how far the player really moved
struct usercmd_s;
void VR_RoomRequest( struct usercmd_s *cmd );
void VR_RoomApplied( const float *moved );

// controllers (vr_input.c), around the client's CL_CreateMove in CL_CreateCmd:
// VR_PreCreateMove syncs the actions and turns the view, VR_CreateMove adds
// buttons and stick movement to the command
void VR_PreCreateMove( float frametime );
void VR_CreateMove( struct usercmd_s *cmd );

// the engine's usercmd_t keeps Half-Life's impact_position[3] floats as
// raw 32-bit slots reserved[1..3]; the game DLLs see the same memory as floats
#define VR_CMD_ROOMMOVE( cmd ) ((float *)&( cmd )->reserved[1] )

// called by the renderer's buffer swap, before the desktop window swaps:
// if the eyes weren't rendered this frame (menus, loading), copies the
// finished desktop frame into them so 2D screens stay visible
void VR_CopyDesktopToEyes( void );

// called by CL_IsInGame: true while the headset has no focus (taken off,
// asleep, system menu open), which pauses singleplayer like the menu does
qboolean VR_PausesGame( void );

// called by Mod_LoadStudioModel for every studio model (vr_arms.c): marks the
// arms built into weapon models (STUDIO_NF_VR_ARM, STUDIO_VR_ARMS_SUBMODEL)
// so eye passes can leave them out (RF_VIEWMODEL_NOARMS)
struct studiohdr_s;
void VR_MarkViewmodelArms( const char *modelname, struct studiohdr_s *phdr );

// called by CL_RegisterResources while the map loads, before unused models
// are freed (vr_weaponmodels.c): loads our own models, the VR weapon model
// models/vr/v_X.mdl for every weapon v_X.mdl that has one, and the hands
void VR_LoadModels( void );

// extra ref_viewpass_t flags for the desktop pass of a frame whose eyes were drawn
int VR_DesktopPassFlags( void );

#endif // XASH_OPENXR

#endif // VR_H
