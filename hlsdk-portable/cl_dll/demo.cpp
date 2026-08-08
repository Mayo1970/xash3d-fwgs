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

#include "hud.h"
#include "cl_util.h"
#include "demo.h"
#include "demo_api.h"

int g_demosniper = 0;
int g_demosniperdamage = 0;
float g_demosniperorg[3];
float g_demosniperangles[3];
float g_demozoom;

// FIXME:  There should be buffer helper functions to avoid all of the *(int *)& crap.

extern "C" 
{
	void DLLEXPORT Demo_ReadBuffer( int size, unsigned char *buffer );
}

/*
=====================
Demo_WriteBuffer

Write some data to the demo stream
=====================
*/
void Demo_WriteBuffer( int type, int size, unsigned char *buffer )
{
	int pos = 0;
	unsigned char buf[32 * 1024];
	memcpy( &buf[pos], &type, sizeof( type ) );
	pos += sizeof(int);

	memcpy( &buf[pos], buffer, size );

	// Write full buffer out
	gEngfuncs.pDemoAPI->WriteBuffer( size + sizeof(int), buf );
}

/*
=====================
Demo_ReadBuffer

Engine wants us to parse some data from the demo stream
=====================
*/
void DLLEXPORT Demo_ReadBuffer( int size, unsigned char *buffer )
{
	int type;
	int i = 0;

	memcpy( &type, buffer, sizeof( type ) );
	i += sizeof(int);
	switch( type )
	{
	case TYPE_SNIPERDOT:
		memcpy( &g_demosniper, &buffer[i], sizeof( g_demosniper ) );
		i += sizeof(int);

		if( g_demosniper )
		{
			memcpy( &g_demosniperdamage, &buffer[i], sizeof( g_demosniperdamage ) );
			i += sizeof(int);

			memcpy( &g_demosniperangles[0], &buffer[i], sizeof( float ) );
			i += sizeof(float);
			memcpy( &g_demosniperangles[1], &buffer[i], sizeof( float ) );
			i += sizeof(float);
			memcpy( &g_demosniperangles[2], &buffer[i], sizeof( float ) );
			i += sizeof(float);
			memcpy( &g_demosniperorg[0], &buffer[i], sizeof( float ) );
			i += sizeof(float);
			memcpy( &g_demosniperorg[1], &buffer[i], sizeof( float ) );
			i += sizeof(float);
			memcpy( &g_demosniperorg[2], &buffer[i], sizeof( float ) );
			// i += sizeof(float);
		}
		break;
	case TYPE_ZOOM:
		memcpy( &g_demozoom, &buffer[i], sizeof( g_demozoom ) );
		// i += sizeof(float);
		break;
	default:
		gEngfuncs.Con_DPrintf( "Unknown demo buffer type, skipping.\n" );
		break;
	}
}
