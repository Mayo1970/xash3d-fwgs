/*
in_ps3.c - PS3 (PSL1GHT) input
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

#include "platform/platform.h"
#include "input.h"
#include "keydefs.h"
#include "client.h"
#include "ps3_osk.h"
#include <string.h>
// io/pad.h declares foo() instead of foo(void); silence -Werror=strict-prototypes for it only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <io/pad.h>
#pragma GCC diagnostic pop

static qboolean ps3_pad_initialized;
static int ps3_active_port;

#define NUM_PS3_DIGITAL_BUTTONS 12
static int ps3_btn_prev[NUM_PS3_DIGITAL_BUTTONS];

// Matches joy_sdl2.c's g_button_mapping order for consistent on-screen prompts.
// L2/R2 excluded -- fed as an analog trigger axis further down, same as SDL2.
static const int ps3_key_map[NUM_PS3_DIGITAL_BUTTONS] =
{
	K_A_BUTTON, K_B_BUTTON, K_X_BUTTON, K_Y_BUTTON,
	K_BACK_BUTTON, K_START_BUTTON,
	K_LSTICK, K_RSTICK,
	K_L1_BUTTON, K_R1_BUTTON,
	K_DPAD_UP, K_DPAD_DOWN,
};

void PS3_InputInit( void )
{
	int port;

	// MAX_PADS (127) is io/pad.h's virtual/LDD-pad cap, not the physical
	// port count -- real DS3/Sixaxis ports are MAX_PORT_NUM (7).
	ioPadInit( MAX_PORT_NUM );

	// PRE_L2/PRE_R2 read 0 unless pressure mode is explicitly turned on.
	for( port = 0; port < MAX_PORT_NUM; port++ )
		ioPadSetPressMode( port, PAD_PRESS_MODE_ON );

	memset( ps3_btn_prev, 0, sizeof( ps3_btn_prev ));
	ps3_active_port = 0;
	ps3_pad_initialized = true;
}

void PS3_InputShutdown( void )
{
	ioPadEnd();
	ps3_pad_initialized = false;
}

// Sticky: keeps current port if still connected, else picks the first connected one.
// Runs every frame -- BT sleep/USB replug can re-enumerate a pad onto a different port.
static void PS3_SelectActivePort( const padInfo *info )
{
	int i;

	if( info->status[ps3_active_port] )
		return;

	for( i = 0; i < MAX_PORT_NUM; i++ )
	{
		if( info->status[i] )
		{
			ps3_active_port = i;
			return;
		}
	}
}

// Raw stick axis is 0..255, center ~128. Scale to the engine's signed ~-32768..32767 range.
static short PS3_ScaleStick( unsigned int raw )
{
	int value = ((int)raw - 128) * 256;

	if( value > 32767 ) value = 32767;
	if( value < -32768 ) value = -32768;

	return (short)value;
}

// Raw trigger pressure is 0..255. Scale to the unsigned 0..32767 range
// Joy_ProcessTrigger()'s threshold compare expects (no centering, unlike sticks).
static short PS3_ScaleTrigger( unsigned int raw )
{
	int value = (int)raw * 128;

	if( value > 32767 ) value = 32767;

	return (short)value;
}

// Ports can (dis)connect anytime, so all MAX_PORT_NUM ports are rescanned every frame.
// ioPadGetData's data isn't zero-filled on "no new input" despite its doc comment (per
// ioQuake3-PS3 on real hardware) -- never branch on padData.len, just reuse the buffer.
void Platform_RunEvents( void )
{
	static padData data;
	padInfo info;
	qboolean btn_cur[NUM_PS3_DIGITAL_BUTTONS];
	int i;

	// Must run every frame regardless of pad state -- this is the only place
	// XMB "close application" (SYSUTIL_EXIT_GAME) ever gets drained. Calls
	// Sys_Quit() itself once XMB requests it (see sys_ps3.c).
	PS3_CheckExitRequested();

	if( !ps3_pad_initialized )
		return;

	ioPadGetInfo( &info );
	PS3_SelectActivePort( &info );

	if( !info.status[ps3_active_port] )
		return;

	ioPadGetData( ps3_active_port, &data );

	btn_cur[0]  = data.BTN_CROSS;
	btn_cur[1]  = data.BTN_CIRCLE;
	btn_cur[2]  = data.BTN_SQUARE;
	btn_cur[3]  = data.BTN_TRIANGLE;
	btn_cur[4]  = data.BTN_SELECT;
	btn_cur[5]  = data.BTN_START;
	btn_cur[6]  = data.BTN_L3;
	btn_cur[7]  = data.BTN_R3;
	btn_cur[8]  = data.BTN_L1;
	btn_cur[9]  = data.BTN_R1;
	btn_cur[10] = data.BTN_UP;
	btn_cur[11] = data.BTN_DOWN;

	for( i = 0; i < NUM_PS3_DIGITAL_BUTTONS; i++ )
	{
		if( btn_cur[i] == ps3_btn_prev[i] )
			continue;

		Key_Event( ps3_key_map[i], btn_cur[i] );
		ps3_btn_prev[i] = btn_cur[i];
	}

	// D-pad left/right double as menu-navigation-only via the analog side
	// axis's Joy_HatMotionEvent path below; still emit real key events for
	// them here so they work identically to up/down when not in a menu.
	{
		static qboolean left_prev, right_prev;

		if( data.BTN_LEFT != left_prev )
		{
			Key_Event( K_DPAD_LEFT, data.BTN_LEFT );
			left_prev = data.BTN_LEFT;
		}
		if( data.BTN_RIGHT != right_prev )
		{
			Key_Event( K_DPAD_RIGHT, data.BTN_RIGHT );
			right_prev = data.BTN_RIGHT;
		}
	}

	Joy_AxisMotionEvent( JOY_AXIS_SIDE,  PS3_ScaleStick( data.ANA_L_H ));
	Joy_AxisMotionEvent( JOY_AXIS_FWD,   PS3_ScaleStick( data.ANA_L_V ));
	Joy_AxisMotionEvent( JOY_AXIS_YAW,   PS3_ScaleStick( data.ANA_R_H ));
	Joy_AxisMotionEvent( JOY_AXIS_PITCH, PS3_ScaleStick( data.ANA_R_V ));
	Joy_AxisMotionEvent( JOY_AXIS_LT,    PS3_ScaleTrigger( data.PRE_L2 ));
	Joy_AxisMotionEvent( JOY_AXIS_RT,    PS3_ScaleTrigger( data.PRE_R2 ));
}

void Platform_MouseMove( float *x, float *y )
{
	// PS3 has no mouse. Right-stick look goes through JOY_AXIS_YAW/PITCH ->
	// Joy_FinalizeMove instead of mouse-delta emulation, so this stays dead.
	if( x ) *x = 0.0f;
	if( y ) *y = 0.0f;
}

void Platform_EnableTextInput( qboolean enable )
{
	// Only reached when osk_enable is 0 (in_keys.c routes elsewhere otherwise).
	// No "disable" needed: the OSK closes itself and PS3_OSK_Open no-ops if already active.
	PS3_Printf( "Platform_EnableTextInput: enable=%d\n", (int)enable );

	if( enable )
		PS3_OSK_Open();
}
