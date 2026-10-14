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
#include <io/kb.h>
#include <io/mouse.h>
#pragma GCC diagnostic pop

static qboolean ps3_pad_initialized;
static int ps3_active_port;
static qboolean s_kb_binds_seeded;

static void KB_Init( void );
static void KB_Shutdown( void );
static void KB_Frame( qboolean syncOnly );
static void Mouse_Init( void );
static void Mouse_Shutdown( void );
static void Mouse_Frame( qboolean syncOnly );

// Set to 1 in the console to log USB keyboard reads (visible in-game console).
static CVAR_DEFINE_AUTO( kb_debug, "0", 0, "log PS3 USB keyboard reads" );

// Analog-stick menu cursor -- a virtual mouse for navigating mainui with the pad.
// While on, either stick drives the pointer instead of the menu hat.
static CVAR_DEFINE_AUTO( joy_cursor, "1", FCVAR_ARCHIVE, "analog-stick menu cursor (0 = stick steps menu items)" );
static CVAR_DEFINE_AUTO( joy_cursor_speed, "1400", FCVAR_ARCHIVE, "menu cursor speed, pixels/sec" );

// Bump when the engine's default pad layout changes; PS3_MigratePadBinds moves old configs once.
#define PS3_PAD_LAYOUT_VERSION 1
static CVAR_DEFINE_AUTO( joy_binds_version, "0", FCVAR_ARCHIVE, "pad layout version the saved binds were migrated to" );
static CVAR_DEFINE_AUTO( hud_scale_version, "0", FCVAR_ARCHIVE, "hud_scale default version the saved config was migrated to" );

static float    ps3_cursor_x, ps3_cursor_y;
static double   ps3_cursor_last_move;
static qboolean ps3_cursor_valid;
// Set by Platform_SetCursorType(): the menu or a VGUI1 panel wants a cursor. VGUI1 panels
// open over live gameplay, not through key_dest, so key_dest==key_menu alone misses them.
static qboolean ps3_wants_cursor;
static qboolean ps3_cross_is_mouse; // current CROSS press was emitted as K_MOUSE1

#define NUM_PS3_DIGITAL_BUTTONS 12
static int ps3_btn_prev[NUM_PS3_DIGITAL_BUTTONS];
// D-pad left/right edge state; file scope so the OSK gate can resync it.
static int ps3_dpad_left_prev, ps3_dpad_right_prev;

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

// Bind a key only if config.cfg left it empty. Runs after config.cfg (see
// Platform_RunEvents), so an explicit user bind always wins.
static void PS3_DefaultBind( int keynum, const char *cmd )
{
	const char *cur = Key_GetBinding( keynum );

	if( !cur || !cur[0] )
		Key_SetBinding( keynum, cmd );
}

// Replace a pad/key bind for a mod only when the player still has the engine's
// stock default on it (or nothing). A custom user bind always wins.
static void PS3_ModRebind( int keynum, const char *stock_default, const char *cmd )
{
	const char *cur = Key_GetBinding( keynum );

	if( !cur || !cur[0] || !Q_strcmp( cur, stock_default ))
		Key_SetBinding( keynum, cmd );
}

// Unlike PS3_ModRebind, an empty key stays empty: the player may have unbound it.
static void PS3_MigrateBind( int keynum, const char *old_default, const char *cmd )
{
	const char *cur = Key_GetBinding( keynum );

	if( cur && !Q_strcmp( cur, old_default ))
		Key_SetBinding( keynum, cmd );
}

// config.cfg saves every bind, so a new default layout only reaches existing players
// through this one-time move of keys that still hold the old default.
static void PS3_MigratePadBinds( void )
{
	if( joy_binds_version.value >= PS3_PAD_LAYOUT_VERSION )
		return;

	// v1: R1 was a second invnext and D-pad left a second lastinv; nothing had invprev
	PS3_MigrateBind( K_R1_BUTTON, "invnext", "" );
	PS3_MigrateBind( K_DPAD_LEFT, "lastinv", "invprev" );

	Cvar_DirectSetValue( &joy_binds_version, PS3_PAD_LAYOUT_VERSION );
}

// Per-gamedir pad layout over the engine defaults; also run by resetpadkeys (in_keys.c).
void PS3_ApplyModPadBinds( void )
{
	const char *game = FS_Gamedir();

	if( !Q_stricmp( game, "cstrike" ) || !Q_stricmp( game, "czero" ))
	{
		// L2/R2 reach the engine as the K_JOY1/K_JOY2 trigger axis, not K_L2_BUTTON/K_R2_BUTTON.
		// D-pad left/right (invprev/invnext) and L1 (lastinv) stay on the engine defaults.
		PS3_ModRebind( K_JOY1,      "+attack2",       "+speed"      ); // L2: walk
		PS3_ModRebind( K_RSTICK,    "impulse 100",    "+attack2"    ); // R3
		PS3_ModRebind( K_LSTICK,    "+speed",         "nightvision" ); // L3
		PS3_ModRebind( K_DPAD_UP,   "weapon_crowbar", "buy"         );
		PS3_ModRebind( K_DPAD_DOWN, "impulse 201",    "chooseteam"  );
	}
	else if( !Q_stricmp( game, "tfc" ))
	{
		// L1/R1 prime+throw grenades 1/2 (forwarded to the server DLL). R3 is the
		// class special (detpipe, rocket reload, ...) instead of the flashlight.
		PS3_ModRebind( K_L1_BUTTON, "lastinv", "+gren1" );
		PS3_ModRebind( K_R1_BUTTON, "", "+gren2" );
		PS3_ModRebind( K_RSTICK,    "impulse 100", "special" );

		// D-pad up opens the VGUI command menu (engineer build, spy disguise/feign) for the
		// cursor to pick from; nothing else on the pad reaches it
		PS3_ModRebind( K_DPAD_UP,   "weapon_crowbar", "+commandmenu" );
	}
	else if( !Q_stricmp( game, "decay" ))
	{
		// swap between Gina and Colette (PC binds G); only Decay's server handles changeplayer
		PS3_ModRebind( K_DPAD_UP,   "weapon_crowbar", "changeplayer" );
	}
}

// keynames[] only ships the arrow-key layout and a PS3 can't easily reach the controls
// menu with a keyboard, so seed WASD + actions here. Mod keys are added per gamedir.
static void PS3_SeedKeyboardBinds( void )
{
	const char *game = FS_Gamedir();
	qboolean is_cs = ( !Q_stricmp( game, "cstrike" ) || !Q_stricmp( game, "czero" ));

	PS3_DefaultBind( 'w', "+forward" );
	PS3_DefaultBind( 's', "+back" );
	PS3_DefaultBind( 'a', "+moveleft" );
	PS3_DefaultBind( 'd', "+moveright" );
	PS3_DefaultBind( 'e', "+use" );
	PS3_DefaultBind( 'r', "+reload" );
	PS3_DefaultBind( 'f', "impulse 100" );   // flashlight
	PS3_DefaultBind( 'q', "lastinv" );
	PS3_DefaultBind( 't', "messagemode" );
	PS3_DefaultBind( 'y', "messagemode2" );
	PS3_DefaultBind( '1', "slot1" );
	PS3_DefaultBind( '2', "slot2" );
	PS3_DefaultBind( '3', "slot3" );
	PS3_DefaultBind( '4', "slot4" );
	PS3_DefaultBind( '5', "slot5" );
	PS3_DefaultBind( '6', "slot6" );

	if( is_cs )
	{
		PS3_DefaultBind( 'b', "buy" );
		PS3_DefaultBind( 'm', "chooseteam" );
		PS3_DefaultBind( 'n', "nightvision" );
		PS3_DefaultBind( ',', "buyammo1" );
		PS3_DefaultBind( '.', "buyammo2" );
	}

	// migrate first, so the mod layout sees the current engine defaults
	PS3_MigratePadBinds();
	PS3_ApplyModPadBinds();
}

// hud_scale is archived, so its PS3 default (cl_main.c) reaches old configs only through this
// one-time move. Any other saved value is the player's own choice and stays.
static void PS3_MigrateHudScale( void )
{
	if( hud_scale_version.value >= 1.0f )
		return;

	// v1: the default went from Off ("0") to 1.5x for TV distance
	if( hud_scale.value == 0.0f )
		Cvar_DirectSet( &hud_scale, hud_scale.def_string );

	Cvar_DirectSetValue( &hud_scale_version, 1.0f );
}

// DS3 rumble. ioPadSetActDirect has no duration, so Platform_RunEvents stops the motors.
static double ps3_rumble_until; // Platform_DoubleTime() stop time, 0 = motors off
static int    ps3_rumble_port;

static void PS3_SetMotors( int port, int large, qboolean small_on )
{
	padActParam act;

	memset( &act, 0, sizeof( act ));
	act.large_motor = (u8)large;
	act.small_motor = small_on ? 1 : 0; // the DS3 small motor is on/off only
	ioPadSetActDirect( port, &act );
}

static void PS3_RumbleStop( void )
{
	if( ps3_rumble_until == 0.0 )
		return;

	PS3_SetMotors( ps3_rumble_port, 0, false );
	ps3_rumble_until = 0.0;
}

// Same contract as joy_sdl2.c: time in ms, a negative strength picks a random 50-100%.
void Platform_Vibrate2( float time, int low_freq, int high_freq, uint flags )
{
	if( !ps3_pad_initialized || time <= 0.0f )
		return;

	if( low_freq < 0 )
		low_freq = COM_RandomLong( 0x7FFF, 0xFFFF );

	if( high_freq < 0 )
		high_freq = COM_RandomLong( 0x7FFF, 0xFFFF );

	// a reconnect can move the active pad mid-rumble; stop the old one
	if( ps3_rumble_port != ps3_active_port )
		PS3_RumbleStop();

	PS3_SetMotors( ps3_active_port, bound( 0, low_freq >> 8, 255 ), high_freq > 0 );
	ps3_rumble_port = ps3_active_port;
	ps3_rumble_until = Platform_DoubleTime() + time / 1000.0;
}

void Platform_Vibrate( float life, char flags )
{
	Platform_Vibrate2( life, -1, -1, flags );
}

void PS3_InputInit( void )
{
	int port;

	// MAX_PADS (127) is io/pad.h's virtual/LDD-pad cap, not the physical
	// port count -- real DS3/Sixaxis ports are MAX_PORT_NUM (7).
	ioPadInit( MAX_PORT_NUM );

	// PRE_L2/PRE_R2 read 0 unless pressure mode is explicitly turned on.
	for( port = 0; port < MAX_PORT_NUM; port++ )
		ioPadSetPressMode( port, PAD_PRESS_MODE_ON );

	Cvar_RegisterVariable( &kb_debug );
	Cvar_RegisterVariable( &joy_cursor );
	Cvar_RegisterVariable( &joy_cursor_speed );
	Cvar_RegisterVariable( &joy_binds_version );
	Cvar_RegisterVariable( &hud_scale_version );

	memset( ps3_btn_prev, 0, sizeof( ps3_btn_prev ));
	ps3_active_port = 0;
	ps3_pad_initialized = true;

	KB_Init();
	Mouse_Init();
}

void PS3_InputShutdown( void )
{
	PS3_RumbleStop();
	ioPadEnd();
	KB_Shutdown();
	Mouse_Shutdown();
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

// Cursor shows in the menu or when a VGUI1 panel wants one, and only for a few seconds
// after the last stick motion, so d-pad navigation never fights a stale pointer.
static qboolean PS3_CursorVisible( void )
{
	return joy_cursor.value != 0.0f
		&& ( cls.key_dest == key_menu || ps3_wants_cursor )
		&& ps3_cursor_valid
		&& ( host.realtime - ps3_cursor_last_move ) < 3.0;
}

// Called when the menu or a VGUI1 panel changes its desired cursor; dc_none = nobody wants one.
void GAME_EXPORT Platform_SetCursorType( VGUI_DefaultCursor type )
{
	ps3_wants_cursor = ( type != dc_none );
	// TFC-5 diagnostic, not spammy (the engine only calls on transitions). Remove once
	// the MOTD/team-select interaction bug is closed.
	Con_Printf( "[ps3cursor] Platform_SetCursorType: type=%d wants_cursor=%d key_dest=%d\n",
		(int)type, (int)ps3_wants_cursor, (int)cls.key_dest );
}

// dz = deadzone, raw = 0..255 (center ~128). Returns -1..1, 0 inside the deadzone.
static float PS3_StickAxis( unsigned int raw, int dz )
{
	int d = (int)raw - 128;

	if( d >  dz ) return ( d - dz ) / (float)( 127 - dz );
	if( d < -dz ) return ( d + dz ) / (float)( 127 - dz );
	return 0.0f;
}

// Integrate both sticks into the virtual cursor (menu or a VGUI1 panel that wants one);
// their input adds up if both are pushed together.
static void PS3_UpdateMenuCursor( const padData *data )
{
	const int dz = 28;
	float sx, sy, move;

	if( joy_cursor.value == 0.0f || !( cls.key_dest == key_menu || ps3_wants_cursor ) )
		return;

	if( !ps3_cursor_valid && refState.width > 0 )
	{
		ps3_cursor_x = refState.width * 0.5f;
		ps3_cursor_y = refState.height * 0.5f;
		ps3_cursor_valid = true;
	}

	sx = PS3_StickAxis( data->ANA_L_H, dz ) + PS3_StickAxis( data->ANA_R_H, dz );
	sy = PS3_StickAxis( data->ANA_L_V, dz ) + PS3_StickAxis( data->ANA_R_V, dz );

	if( sx > 1.0f ) sx = 1.0f;
	if( sx < -1.0f ) sx = -1.0f;
	if( sy > 1.0f ) sy = 1.0f;
	if( sy < -1.0f ) sy = -1.0f;

	if( sx == 0.0f && sy == 0.0f )
		return;

	move = joy_cursor_speed.value * (float)host.realframetime;

	// square the magnitude, keep sign -- fine control near center
	ps3_cursor_x += sx * ( sx < 0 ? -sx : sx ) * move;
	ps3_cursor_y += sy * ( sy < 0 ? -sy : sy ) * move;

	if( ps3_cursor_x < 0.0f ) ps3_cursor_x = 0.0f;
	if( ps3_cursor_y < 0.0f ) ps3_cursor_y = 0.0f;
	if( ps3_cursor_x > refState.width  - 1 ) ps3_cursor_x = refState.width  - 1;
	if( ps3_cursor_y > refState.height - 1 ) ps3_cursor_y = refState.height - 1;

	ps3_cursor_last_move = host.realtime;
}

// Feeds IN_MouseMove(), which forwards to UI_MouseMove() every frame.
void GAME_EXPORT Platform_GetMousePos( int *x, int *y )
{
	if( !ps3_cursor_valid && refState.width > 0 )
	{
		ps3_cursor_x = refState.width * 0.5f;
		ps3_cursor_y = refState.height * 0.5f;
		ps3_cursor_valid = true;
	}

	if( x ) *x = (int)ps3_cursor_x;
	if( y ) *y = (int)ps3_cursor_y;
}

static void PS3_Fill( int x, int y, int w, int h, byte r, byte g, byte b )
{
	if( w <= 0 || h <= 0 )
		return;
	ref.dllFuncs.FillRGBA( kRenderNormal, x, y, w, h, r, g, b, 255 );
}

// Crosshair cursor, centered on the hotspot. Four arms around a center gap,
// white with a 1px black edge. Drawn from UI_UpdateMenu's redraw hook.
void PS3_MenuDrawCursor( void )
{
	int cx, cy, len, gap, t, pass;

	if( !PS3_CursorVisible( ))
		return;

	cx  = (int)ps3_cursor_x;
	cy  = (int)ps3_cursor_y;
	len = refState.height / 42;
	if( len < 6 ) len = 6;
	gap = 3;
	t   = 2;

	// pass 0: black outline (1px larger). pass 1: white fill.
	for( pass = 0; pass < 2; pass++ )
	{
		int e = pass ? 0 : 1;
		byte v = pass ? 255 : 0;

		PS3_Fill( cx - gap - len - e, cy - t / 2 - e, len + 2 * e, t + 2 * e, v, v, v ); // left
		PS3_Fill( cx + gap - e,       cy - t / 2 - e, len + 2 * e, t + 2 * e, v, v, v ); // right
		PS3_Fill( cx - t / 2 - e,     cy - gap - len - e, t + 2 * e, len + 2 * e, v, v, v ); // top
		PS3_Fill( cx - t / 2 - e,     cy + gap - e,       t + 2 * e, len + 2 * e, v, v, v ); // bottom
	}
}

// USB keyboard + USB mouse (raw PSL1GHT HID, ported from ioQuake3-PS3)

// Raw USB HID usage code -> engine keynum (0 = unmapped).
// Letters are always lowercase; shifted chars arrive via CL_CharEvent.
static const int s_raw_to_key[256] =
{
	[0x04]='a', [0x05]='b', [0x06]='c', [0x07]='d',
	[0x08]='e', [0x09]='f', [0x0A]='g', [0x0B]='h',
	[0x0C]='i', [0x0D]='j', [0x0E]='k', [0x0F]='l',
	[0x10]='m', [0x11]='n', [0x12]='o', [0x13]='p',
	[0x14]='q', [0x15]='r', [0x16]='s', [0x17]='t',
	[0x18]='u', [0x19]='v', [0x1A]='w', [0x1B]='x',
	[0x1C]='y', [0x1D]='z',
	[0x1E]='1', [0x1F]='2', [0x20]='3', [0x21]='4',
	[0x22]='5', [0x23]='6', [0x24]='7', [0x25]='8',
	[0x26]='9', [0x27]='0',
	[0x28]=K_ENTER,     [0x29]=K_ESCAPE,    [0x2A]=K_BACKSPACE,
	[0x2B]=K_TAB,       [0x2C]=K_SPACE,
	[0x2D]='-',  [0x2E]='=',  [0x2F]='[',  [0x30]=']',
	[0x31]='\\', [0x33]=';',  [0x34]='\'', [0x35]='`',
	[0x36]=',',  [0x37]='.',  [0x38]='/',
	[0x39]=K_CAPSLOCK,
	[0x3A]=K_F1,  [0x3B]=K_F2,  [0x3C]=K_F3,  [0x3D]=K_F4,
	[0x3E]=K_F5,  [0x3F]=K_F6,  [0x40]=K_F7,  [0x41]=K_F8,
	[0x42]=K_F9,  [0x43]=K_F10, [0x44]=K_F11, [0x45]=K_F12,
	[0x47]=K_SCROLLLOCK, [0x48]=K_PAUSE,
	[0x49]=K_INS,  [0x4A]=K_HOME, [0x4B]=K_PGUP,
	[0x4C]=K_DEL,  [0x4D]=K_END,  [0x4E]=K_PGDN,
	[0x4F]=K_RIGHTARROW, [0x50]=K_LEFTARROW,
	[0x51]=K_DOWNARROW,  [0x52]=K_UPARROW,
	[0x53]=K_KP_NUMLOCK,   [0x54]=K_KP_SLASH,     [0x55]=K_KP_MUL,
	[0x56]=K_KP_MINUS,     [0x57]=K_KP_PLUS,      [0x58]=K_KP_ENTER,
	[0x59]=K_KP_END,       [0x5A]=K_KP_DOWNARROW, [0x5B]=K_KP_PGDN,
	[0x5C]=K_KP_LEFTARROW, [0x5D]=K_KP_5,         [0x5E]=K_KP_RIGHTARROW,
	[0x5F]=K_KP_HOME,      [0x60]=K_KP_UPARROW,   [0x61]=K_KP_PGUP,
	[0x62]=K_KP_INS,       [0x63]=K_KP_DEL,

	// No Ctrl/Shift/Alt: HW reports them only in mkey, and a lone modifier sends no press packet
};

// US-101 shifted forms for the printable keynums the table above emits.
static int KB_ShiftChar( int c )
{
	if( c >= 'a' && c <= 'z' ) return c - 'a' + 'A';

	switch( c )
	{
	case '1': return '!';  case '2': return '@';  case '3': return '#';
	case '4': return '$';  case '5': return '%';  case '6': return '^';
	case '7': return '&';  case '8': return '*';  case '9': return '(';
	case '0': return ')';
	case '-': return '_';  case '=': return '+';
	case '[': return '{';  case ']': return '}';  case '\\': return '|';
	case ';': return ':';  case '\'': return '"'; case '`': return '~';
	case ',': return '<';  case '.': return '>';  case '/': return '?';
	default:  return c;
	}
}

#define KB_NUM_SLOTS   256 // one per raw HID usage

static KbInfo   s_kb_info;
static qboolean s_kb_connected;
static int      s_kb_port = -1;
static byte     s_kb_cur[KB_NUM_SLOTS];
static byte     s_kb_prev[KB_NUM_SLOTS];

static void KB_ReleaseAll( void )
{
	int k;

	for( k = 1; k < KB_NUM_SLOTS; k++ )
	{
		if( s_kb_prev[k] && s_raw_to_key[k] )
			Key_Event( s_raw_to_key[k], false );
	}
	memset( s_kb_prev, 0, sizeof( s_kb_prev ));
}

static void KB_Init( void )
{
	int rc = ioKbInit( MAX_KB_PORT_NUM );
	Con_Printf( "[ps3kb] ioKbInit -> %d\n", rc );
	memset( s_kb_cur, 0, sizeof( s_kb_cur ));
	memset( s_kb_prev, 0, sizeof( s_kb_prev ));
	s_kb_connected = false;
	s_kb_port = -1;
}

static void KB_Shutdown( void )
{
	ioKbEnd();
}

// syncOnly: the OSK owns input, so held keys are released and new ones ignored.
static void KB_Frame( qboolean syncOnly )
{
	KbData data;
	int i, k, found_port, rc;
	qboolean shift = false;
	qboolean dbg = ( kb_debug.value != 0.0f );
	static double s_next_status_log;
	static u32 s_last_mkey;

	if( ioKbGetInfo( &s_kb_info ) != 0 )
	{
		if( dbg && host.realtime > s_next_status_log )
		{
			Con_Printf( "[ps3kb] ioKbGetInfo failed\n" );
			s_next_status_log = host.realtime + 1.0;
		}
		return;
	}

	if( dbg && host.realtime > s_next_status_log )
	{
		Con_Printf( "[ps3kb] connected=%u status[0..2]=%u,%u,%u info=0x%x\n",
			(unsigned)s_kb_info.connected, s_kb_info.status[0], s_kb_info.status[1],
			s_kb_info.status[2], (unsigned)s_kb_info.info );
		s_next_status_log = host.realtime + 1.0;
	}

	if( s_kb_info.connected == 0 )
	{
		if( s_kb_connected )
		{
			if( !syncOnly )
				KB_ReleaseAll();
			else memset( s_kb_prev, 0, sizeof( s_kb_prev ));
			s_kb_connected = false;
			s_kb_port = -1;
		}
		return;
	}

	found_port = -1;
	for( i = 0; i < MAX_KB_PORT_NUM; i++ )
	{
		if( s_kb_info.status[i] ) { found_port = i; break; }
	}
	if( found_port < 0 )
		return;

	if( !s_kb_connected || found_port != s_kb_port )
	{
		int rc_ct, rc_rm;
		if( s_kb_connected && !syncOnly )
			KB_ReleaseAll();

		// ioQuake3-PS3's HW-proven order: CodeType(RAW), then ReadMode(PACKET), so every key arrives
		// as a raw usage; without PACKET the driver falls back to ASCII and the raw table misses A-Z/0-9
		rc_ct = ioKbSetCodeType( found_port, KB_CODETYPE_RAW );
		rc_rm = ioKbSetReadMode( found_port, KB_RMODE_PACKET );
		ioKbClearBuf( found_port );
		Con_Printf( "[ps3kb] port %d attached, CodeType(RAW)->%d ReadMode(PACKET)->%d\n",
			found_port, rc_ct, rc_rm );

		memset( s_kb_cur, 0, sizeof( s_kb_cur ));
		memset( s_kb_prev, 0, sizeof( s_kb_prev ));
		s_kb_port = found_port;
		s_kb_connected = true;
	}

	// zeroed: the driver skips fields on empty reads, and stale stack bytes once faked CTRL (+attack)
	memset( &data, 0, sizeof( data ));
	rc = ioKbRead( s_kb_port, &data );

	// The system owns the keyboard (OSK, XMB: info bit 0) and keeps its release packets,
	// so drop held keys; the read above still drains what queued meanwhile
	if( syncOnly || ( s_kb_info.info & 1 ))
	{
		KB_ReleaseAll();
		memset( s_kb_cur, 0, sizeof( s_kb_cur ));
		return;
	}

	if( rc != 0 )
	{
		if( dbg && host.realtime > s_next_status_log )
		{
			Con_Printf( "[ps3kb] ioKbRead port %d -> %d\n", s_kb_port, rc );
			s_next_status_log = host.realtime + 1.0;
		}
		return;
	}

	if( dbg && ( data.nb_keycode != 0 || data.mkey._KbMkeyU.mkeys != s_last_mkey ))
	{
		Con_Printf( "[ps3kb] nb_keycode=%d kc0=0x%04x kc1=0x%04x mkey=0x%x\n",
			(int)data.nb_keycode, (unsigned)data.keycode[0], (unsigned)data.keycode[1],
			(unsigned)data.mkey._KbMkeyU.mkeys );
		s_last_mkey = data.mkey._KbMkeyU.mkeys;
	}

	// PACKET mode sends a full snapshot only when the state changes (a release is a packet with
	// no keys), so nb_keycode == 0 means "no change" and held keys stay down
	if( data.nb_keycode > 0 )
	{
		memset( s_kb_cur, 0, sizeof( s_kb_cur ));
		for( i = 0; i < data.nb_keycode && i < MAX_KEYCODES; i++ )
		{
			// RAW mode sends plain usages; many equal an ASCII code (E = 0x08 = BS), so never decode as ASCII
			u16 raw = data.keycode[i] & ~((u16)( KB_RAWDAT | KB_KEYPAD ));

			if( raw > 0 && raw < KB_NUM_SLOTS )
				s_kb_cur[raw] = 1;
		}

		// mkey is valid only in a packet (empty reads return 0), and presses only change here
		shift = data.mkey._KbMkeyU._KbMkeyS.l_shift || data.mkey._KbMkeyU._KbMkeyS.r_shift;
	}

	for( k = 1; k < KB_NUM_SLOTS; k++ )
	{
		int key;
		if( s_kb_cur[k] == s_kb_prev[k] )
			continue;
		key = s_raw_to_key[k];
		if( !key )
			continue;

		if( dbg )
			Con_Printf( "[ps3kb] usage 0x%x -> Key_Event(%d '%c', %d)\n",
				k, key, ( key >= 32 && key < 127 ) ? key : '.', s_kb_cur[k] );

		Key_Event( key, s_kb_cur[k] );

		// printable keynums are their own lowercase ASCII
		if( s_kb_cur[k] && key >= 32 && key < 127 )
			CL_CharEvent( shift ? KB_ShiftChar( key ) : key );
	}

	memcpy( s_kb_prev, s_kb_cur, sizeof( s_kb_cur ));
}

// -- USB mouse --

static mouseInfo s_mouse_info;
static qboolean  s_mouse_connected;
static u8        s_mouse_btns_prev;
static int       s_mouse_dx, s_mouse_dy; // drained by Platform_MouseMove

static void Mouse_Init( void )
{
	ioMouseInit( 2 );
	s_mouse_connected = false;
	s_mouse_btns_prev = 0;
	s_mouse_dx = s_mouse_dy = 0;
}

static void Mouse_Shutdown( void )
{
	ioMouseEnd();
}

static void Mouse_Frame( qboolean syncOnly )
{
	mouseDataList list;
	u32 i;

	if( ioMouseGetInfo( &s_mouse_info ) != 0 )
		return;

	if( s_mouse_info.connected == 0 )
	{
		if( s_mouse_connected )
		{
			if( !syncOnly )
			{
				if( s_mouse_btns_prev & 0x01 ) IN_MouseEvent( 0, false );
				if( s_mouse_btns_prev & 0x02 ) IN_MouseEvent( 1, false );
				if( s_mouse_btns_prev & 0x04 ) IN_MouseEvent( 2, false );
			}
			s_mouse_btns_prev = 0;
			s_mouse_connected = false;
		}
		return;
	}

	if( !s_mouse_connected )
	{
		ioMouseClearBuf( 0 );
		s_mouse_btns_prev = 0;
		s_mouse_connected = true;
	}

	if( ioMouseGetDataList( 0, &list ) != 0 )
		return;

	for( i = 0; i < list.count && i < MOUSE_MAX_DATA_LIST; i++ )
	{
		mouseData *md = &list.list[i];
		u8 cur, diff;

		if( !md->update )
			continue;

		if( syncOnly )
		{
			s_mouse_btns_prev = md->buttons;
			continue;
		}

		if( cls.key_dest == key_menu || ps3_wants_cursor )
		{
			// Drive the shared virtual cursor so the menu/VGUI1 pointer follows the mouse.
			if( !ps3_cursor_valid && refState.width > 0 )
			{
				ps3_cursor_x = refState.width * 0.5f;
				ps3_cursor_y = refState.height * 0.5f;
				ps3_cursor_valid = true;
			}
			ps3_cursor_x += (float)md->x_axis;
			ps3_cursor_y += (float)md->y_axis;
			if( ps3_cursor_x < 0.0f ) ps3_cursor_x = 0.0f;
			if( ps3_cursor_y < 0.0f ) ps3_cursor_y = 0.0f;
			if( ps3_cursor_x > refState.width  - 1 ) ps3_cursor_x = refState.width  - 1;
			if( ps3_cursor_y > refState.height - 1 ) ps3_cursor_y = refState.height - 1;
			ps3_cursor_last_move = host.realtime;
		}
		else
		{
			s_mouse_dx += (int)md->x_axis;
			s_mouse_dy += (int)md->y_axis;
		}

		cur  = md->buttons;
		diff = cur ^ s_mouse_btns_prev;
		if( diff & 0x01 ) IN_MouseEvent( 0, ( cur & 0x01 ) != 0 );
		if( diff & 0x02 ) IN_MouseEvent( 1, ( cur & 0x02 ) != 0 );
		if( diff & 0x04 ) IN_MouseEvent( 2, ( cur & 0x04 ) != 0 );
		s_mouse_btns_prev = cur;

		if( md->wheel > 0 )      IN_MWheelEvent( 1 );
		else if( md->wheel < 0 ) IN_MWheelEvent( -1 );
	}
}

// All ports are rescanned every frame (they can (dis)connect anytime). ioPadGetData's data isn't
// zero-filled on "no new input" (ioQuake3-PS3, real HW), so never branch on padData.len.
void Platform_RunEvents( void )
{
	static padData data;
	padInfo info;
	qboolean btn_cur[NUM_PS3_DIGITAL_BUTTONS];
	qboolean osk;
	int i;

	// Every frame regardless of pad state: the only place XMB "close application"
	// (SYSUTIL_EXIT_GAME) is drained; calls Sys_Quit() itself (see sys_ps3.c).
	PS3_CheckExitRequested();

	if( !ps3_pad_initialized )
		return;

	if( ps3_rumble_until != 0.0 && Platform_DoubleTime() >= ps3_rumble_until )
		PS3_RumbleStop();

	// Once after config.cfg has run (so it survives its leading unbindall): seed binds into
	// keys it left empty, and move old saved settings to new PS3 defaults.
	if( !s_kb_binds_seeded && host.config_executed )
	{
		PS3_SeedKeyboardBinds();
		PS3_MigrateHudScale();
		s_kb_binds_seeded = true;
	}

	// Poll USB keyboard/mouse every frame. While the OSK owns input, the keyboard
	// releases held keys and the mouse keeps its edge state; neither emits a press.
	osk = PS3_OSK_IsActive();
	KB_Frame( osk );
	Mouse_Frame( osk );

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

	// OSK owns all input while open: resync pad edges, release any held axis
	// or synthetic click, emit nothing. Matches KB_Frame/Mouse_Frame above.
	if( osk )
	{
		if( ps3_cross_is_mouse && ps3_btn_prev[0] )
			IN_MouseEvent( 0, false );
		ps3_cross_is_mouse = false;

		for( i = 0; i < NUM_PS3_DIGITAL_BUTTONS; i++ )
			ps3_btn_prev[i] = btn_cur[i];
		ps3_dpad_left_prev  = data.BTN_LEFT;
		ps3_dpad_right_prev = data.BTN_RIGHT;

		Joy_AxisMotionEvent( JOY_AXIS_SIDE,  0 );
		Joy_AxisMotionEvent( JOY_AXIS_FWD,   0 );
		Joy_AxisMotionEvent( JOY_AXIS_YAW,   0 );
		Joy_AxisMotionEvent( JOY_AXIS_PITCH, 0 );
		Joy_AxisMotionEvent( JOY_AXIS_LT,    0 );
		Joy_AxisMotionEvent( JOY_AXIS_RT,    0 );
		return;
	}

	PS3_UpdateMenuCursor( &data );

	for( i = 0; i < NUM_PS3_DIGITAL_BUTTONS; i++ )
	{
		if( btn_cur[i] == ps3_btn_prev[i] )
			continue;

		// With the cursor up, CROSS clicks the hovered item instead of activating the focused
		// one. Latched on press so the matching release always uses the same key.
		if( i == 0 )
		{
			if( btn_cur[0] )
			{
				ps3_cross_is_mouse = PS3_CursorVisible();
				// TFC-5 diagnostic: logs whether CROSS lands as a click (K_MOUSE1) or K_A_BUTTON.
				// Remove once the MOTD/team-select interaction bug is closed.
				Con_Printf( "[ps3cursor] CROSS press: cross_is_mouse=%d wants_cursor=%d key_dest=%d cursor=(%.0f,%.0f)\n",
					(int)ps3_cross_is_mouse, (int)ps3_wants_cursor, (int)cls.key_dest, ps3_cursor_x, ps3_cursor_y );
			}

			// VGUI1 clicks fire only from IN_MouseEvent() -> VGui_MouseEvent(); Key_Event(K_MOUSE1)
			// reaches just VGui_KeyEvent(), which is why CROSS once hovered "OK" without clicking it
			if( ps3_cross_is_mouse )
				IN_MouseEvent( 0, btn_cur[0] );
			else
				Key_Event( K_A_BUTTON, btn_cur[0] );
		}
		else
		{
			Key_Event( ps3_key_map[i], btn_cur[i] );
		}

		ps3_btn_prev[i] = btn_cur[i];
	}

	// D-pad left/right as real key events, so they work the same in and out
	// of the menu. Menu discrete nav rides these, not the analog hat path.
	{
		if( data.BTN_LEFT != ps3_dpad_left_prev )
		{
			Key_Event( K_DPAD_LEFT, data.BTN_LEFT );
			ps3_dpad_left_prev = data.BTN_LEFT;
		}
		if( data.BTN_RIGHT != ps3_dpad_right_prev )
		{
			Key_Event( K_DPAD_RIGHT, data.BTN_RIGHT );
			ps3_dpad_right_prev = data.BTN_RIGHT;
		}
	}

	// While the cursor owns the stick (menu or VGUI1 panel; same test as PS3_UpdateMenuCursor), move
	// and look read 0. Gating on key_dest alone let VGUI1 panels turn the view while aiming at an entry.
	if( joy_cursor.value != 0.0f && ( cls.key_dest == key_menu || ps3_wants_cursor ))
	{
		Joy_AxisMotionEvent( JOY_AXIS_SIDE,  0 );
		Joy_AxisMotionEvent( JOY_AXIS_FWD,   0 );
		Joy_AxisMotionEvent( JOY_AXIS_YAW,   0 );
		Joy_AxisMotionEvent( JOY_AXIS_PITCH, 0 );
	}
	else
	{
		Joy_AxisMotionEvent( JOY_AXIS_SIDE,  PS3_ScaleStick( data.ANA_L_H ));
		Joy_AxisMotionEvent( JOY_AXIS_FWD,   PS3_ScaleStick( data.ANA_L_V ));
		Joy_AxisMotionEvent( JOY_AXIS_YAW,   PS3_ScaleStick( data.ANA_R_H ));
		Joy_AxisMotionEvent( JOY_AXIS_PITCH, PS3_ScaleStick( data.ANA_R_V ));
	}
	Joy_AxisMotionEvent( JOY_AXIS_LT,    PS3_ScaleTrigger( data.PRE_L2 ));
	Joy_AxisMotionEvent( JOY_AXIS_RT,    PS3_ScaleTrigger( data.PRE_R2 ));
}

void Platform_MouseMove( float *x, float *y )
{
	// Drain accumulated USB-mouse deltas for in-game look. When no mouse is
	// connected these stay 0 and right-stick look goes through JOY_AXIS_*.
	if( x ) *x = (float)s_mouse_dx;
	if( y ) *y = (float)s_mouse_dy;
	s_mouse_dx = 0;
	s_mouse_dy = 0;
}

void Platform_EnableTextInput( qboolean enable )
{
	// Only reached when osk_enable is 0 (in_keys.c routes elsewhere otherwise).
	// No "disable" needed: the OSK closes itself and PS3_OSK_Open no-ops if already active.
	PS3_Printf( "Platform_EnableTextInput: enable=%d\n", (int)enable );

	if( enable )
		PS3_OSK_Open();
}
