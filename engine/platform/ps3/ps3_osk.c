/*
ps3_osk.c - PS3 native on-screen keyboard via sysutil/osk
Copyright (C) 2026 xashPS3 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// Ported from ioQuake3-PS3's code/input/ps3_osk.c, adapted to this engine's
// CL_CharEvent/Key_Event injection path instead of Q3's ui_ime_* cvar delivery.
#include "platform/platform.h"
#include "ps3_osk.h"
#include "client.h"
#include <string.h>
#include <sys/memory.h>
// Same -Werror=strict-prototypes issue as net/net.h elsewhere in this directory.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>
#pragma GCC diagnostic pop

// firmware docs recommend 4MB for the OSK container
#define OSK_CONTAINER_SIZE (4 * 1024 * 1024)
#define OSK_MAX_TEXT 256

typedef enum
{
	OSK_STATE_IDLE,
	OSK_STATE_OPEN,
	OSK_STATE_RUNNING,
	OSK_STATE_DONE,
	OSK_STATE_CLOSING
} ps3_osk_state_t;

static ps3_osk_state_t     ps3_osk_state = OSK_STATE_IDLE;
static sys_mem_container_t ps3_osk_container;
static qboolean            ps3_osk_container_valid = false;

static u16 ps3_osk_message[OSK_MAX_TEXT]; // UCS-2 prompt, fixed generic title
static u16 ps3_osk_result[OSK_MAX_TEXT];  // UCS-2 result buffer
static oskCallbackReturnParam ps3_osk_return;

static void PS3_OSK_AsciiToUcs2( const char *src, u16 *dst, int maxlen )
{
	int i;

	for( i = 0; i < maxlen - 1 && src[i] != '\0'; i++ )
		dst[i] = (u16)(unsigned char)src[i];
	dst[i] = 0;
}

// Delivers the result through the same char/key path as the built-in gamepad OSK (in_osk.c).
static void PS3_OSK_InjectResult( const u16 *str, int len )
{
	int i;

	for( i = 0; i < len; i++ )
	{
		int ch = (int)str[i];

		if( ch == 0 )
			break; // UCS-2 null terminator

		if( ch >= 32 && ch <= 126 )
			CL_CharEvent( ch );
	}
}

void PS3_OSK_Init( void )
{
	s32 ret;

	ps3_osk_state = OSK_STATE_IDLE;
	ps3_osk_container_valid = false;

	ret = sysMemContainerCreate( &ps3_osk_container, OSK_CONTAINER_SIZE );
	if( ret != 0 )
	{
		PS3_Printf( "PS3_OSK_Init: sysMemContainerCreate failed (0x%08x)\n", (unsigned int)ret );
		return;
	}

	ps3_osk_container_valid = true;
	PS3_Printf( "PS3_OSK_Init: OK, 4 MB container allocated\n" );
}

void PS3_OSK_Shutdown( void )
{
	if( ps3_osk_state != OSK_STATE_IDLE )
		oskAbort();

	if( ps3_osk_container_valid )
	{
		sysMemContainerDestroy( ps3_osk_container );
		ps3_osk_container_valid = false;
	}

	ps3_osk_state = OSK_STATE_IDLE;
}

void PS3_OSK_Open( void )
{
	oskParam param;
	oskInputFieldInfo input;
	s32 ret;

	PS3_Printf( "PS3_OSK_Open: called, state=%d container_valid=%d\n",
		(int)ps3_osk_state, (int)ps3_osk_container_valid );

	if( ps3_osk_state != OSK_STATE_IDLE )
	{
		PS3_Printf( "PS3_OSK_Open: skipped, already open (state=%d)\n", (int)ps3_osk_state );
		return;
	}
	if( !ps3_osk_container_valid )
	{
		PS3_Printf( "PS3_OSK_Open: skipped, no valid mem container (PS3_OSK_Init failed at boot?)\n" );
		return;
	}

	memset( ps3_osk_message, 0, sizeof( ps3_osk_message ));
	memset( ps3_osk_result, 0, sizeof( ps3_osk_result ));

	PS3_OSK_AsciiToUcs2( "Enter text", ps3_osk_message, OSK_MAX_TEXT );

	memset( &input, 0, sizeof( input ));
	input.message = ps3_osk_message;
	input.startText = ps3_osk_result;
	input.maxLength = OSK_MAX_TEXT - 1;

	memset( &param, 0, sizeof( param ));
	param.allowedPanels = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL | OSK_PANEL_TYPE_URL;
	param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET;
	param.controlPoint.x = 0.0f;
	param.controlPoint.y = 0.0f;
	param.prohibitFlags = 0;

	oskSetKeyLayoutOption( OSK_FULLKEY_PANEL );
	oskSetInitialInputDevice( OSK_DEVICE_PAD );
	oskDisableDimmer();

	ret = oskLoadAsync( ps3_osk_container, &param, &input );
	if( ret != 0 )
	{
		PS3_Printf( "PS3_OSK_Open: oskLoadAsync failed (0x%08x)\n", (unsigned int)ret );
		return;
	}

	ps3_osk_state = OSK_STATE_OPEN;
	PS3_Printf( "PS3_OSK_Open: oskLoadAsync ret=0, waiting for SYSUTIL_OSK_LOADED\n" );
}

void PS3_OSK_SysutilCallback( u64 status, u64 param )
{
	(void)param;

	PS3_Printf( "PS3_OSK_SysutilCallback: status=0x%04x state=%d\n",
		(unsigned int)status, (int)ps3_osk_state );

	switch( status )
	{
	case SYSUTIL_OSK_LOADED:
		if( ps3_osk_state == OSK_STATE_OPEN )
			ps3_osk_state = OSK_STATE_RUNNING;
		break;

	case SYSUTIL_OSK_DONE:
	case SYSUTIL_OSK_INPUT_CANCELED:
		if( ps3_osk_state == OSK_STATE_RUNNING )
			ps3_osk_state = OSK_STATE_DONE;
		break;

	case SYSUTIL_OSK_INPUT_ENTERED:
		break;

	case SYSUTIL_OSK_UNLOADED:
		if( ps3_osk_state == OSK_STATE_CLOSING )
		{
			if( ps3_osk_return.res == OSK_OK && ps3_osk_return.len > 0 )
				PS3_OSK_InjectResult( ps3_osk_return.str, ps3_osk_return.len );

			ps3_osk_state = OSK_STATE_IDLE;
		}
		break;

	default:
		break;
	}

	if( ps3_osk_state == OSK_STATE_DONE )
	{
		memset( &ps3_osk_return, 0, sizeof( ps3_osk_return ));
		ps3_osk_return.str = ps3_osk_result;
		ps3_osk_return.len = OSK_MAX_TEXT;

		oskUnloadAsync( &ps3_osk_return );
		ps3_osk_state = OSK_STATE_CLOSING;
	}
}

qboolean PS3_OSK_IsActive( void )
{
	return ( ps3_osk_state != OSK_STATE_IDLE );
}
