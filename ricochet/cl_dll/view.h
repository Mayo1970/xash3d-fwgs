/***
*
*	Copyright (c) 1999, 2000 Valve LLC. All rights reserved.
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
#if !defined ( VIEWH )
#define VIEWH
#pragma once

#if !defined(M_PI)
#define M_PI		3.14159265358979323846  // matches value in gcc v2 math.h
#endif

#if !defined(M_PI_F)
#define M_PI_F		(float)M_PI
#endif

void V_StartPitchDrift( void );
void V_StopPitchDrift( void );

#endif // !VIEWH
