/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*
*	This product contains software technology licensed from Id
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#ifndef REF_PARAMS_H
#define REF_PARAMS_H

#include "xash3d_types.h"

typedef struct ref_params_s
{
	// output
	vec3_t		vieworg;
	vec3_t		viewangles;

	vec3_t		forward;
	vec3_t		right;
	vec3_t		up;

	// Client frametime;
	float		frametime;
	// Client time
	float		time;

	// Misc
	int		intermission;
	int		paused;
	int		spectator;
	int		onground;
	int		waterlevel;

	vec3_t		simvel;
	vec3_t		simorg;

	vec3_t		viewheight;
	float		idealpitch;

	vec3_t		cl_viewangles;
	int		health;
	vec3_t		crosshairangle;
	float		viewsize;

	vec3_t		punchangle;
	int		maxclients;
	int		viewentity;
	int		playernum;
	int		max_entities;
	int		demoplayback;
	int		hardware;
	int		smoothing;

	// Last issued usercmd
	struct usercmd_s	*cmd;

	// Movevars
	struct movevars_s	*movevars;

	int		viewport[4];	// the viewport coordinates x, y, width, height
	int		nextView;		// the renderer calls ClientDLL_CalcRefdef() and Renderview
					// so long in cycles until this value is 0 (multiple views)
	int		onlyClientDraw;	// if !=0 nothing is drawn by the engine except clientDraw functions
} ref_params_t;

// same as ref_params but for overview mode
typedef struct ref_overview_s
{
	vec3_t		origin;
	qboolean		rotated;

	float		xLeft;
	float		xRight;
	float		yTop;
	float		yBottom;
	float		zFar;
	float		zNear;
	float		flZoom;
} ref_overview_t;

// ref_viewpass_t->flags
#define RF_DRAW_WORLD	(1<<0)		// pass should draw the world (otherwise it's player menu model)
#define RF_DRAW_CUBEMAP	(1<<1)		// special 6x pass to render cubemap\skybox sides
#define RF_DRAW_OVERVIEW	(1<<2)		// overview mode is active
#define RF_ONLY_CLIENTDRAW	(1<<3)		// nothing is drawn by the engine except clientDraw functions
#define RF_EXTERNAL_TARGET	(1<<4)		// draw into the framebuffer the caller bound (VR eye): viewport is in its pixels, no scene target
#define RF_ASYMMETRIC_FOV	(1<<5)		// projection and culling use fov_tan instead of fov_x/fov_y (VR eye)
#define RF_REPEAT_PASS	(1<<6)		// another view of a frame that was already drawn once: no time step, no viewmodel events
#define RF_WORLD_VIEWMODEL	(1<<7)		// the viewmodel is placed in the world (VR hand): normal depth range, no squeezing in front of walls

// intermediate struct for viewpass (or just a single frame)
typedef struct ref_viewpass_s
{
	int		viewport[4];	// size of new viewport
	vec3_t		vieworigin;	// view origin
	vec3_t		viewangles;	// view angles
	int		viewentity;	// entitynum (P2: Savior uses this)
	float		fov_x, fov_y;	// vertical & horizontal FOV
	int		flags;		// if !=0 nothing is drawn by the engine except clientDraw functions

	// only valid with RF_ASYMMETRIC_FOV, kept last so code built against the
	// older struct still reads the fields above correctly
	float		fov_tan[4];	// tangents of the left, right, down and up half-angles (left and down negative)
} ref_viewpass_t;

#endif//REF_PARAMS_H
