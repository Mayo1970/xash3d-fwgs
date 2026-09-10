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

static float    ps3_cursor_x, ps3_cursor_y;
static double   ps3_cursor_last_move;
static qboolean ps3_cursor_valid;
// Set from Platform_SetCursorType() -- true whenever the engine's native
// menu OR a VGUI1 panel (team select, class select, MOTD, scoreboard, ...)
// currently wants a visible/movable cursor. VGUI1 panels open over live
// gameplay, not through key_dest, so key_dest==key_menu alone misses them.
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

// Bind a key only if the player has not already bound it. keynum is a plain
// lowercase ASCII char. Called after config.cfg has executed (see
// Platform_RunEvents), so an explicit user bind -- or an unbindall that leaves
// the key clear on purpose -- always wins.
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

// The engine's keynames[] table only ships the classic arrow-key layout. A PS3
// has no easy way to open the controls menu with a keyboard attached, so seed a
// standard WASD + action layout here. Mod-specific keys are added per gamedir.
static void PS3_SeedKeyboardBinds( void )
{
	const char *game = FS_Gamedir();
	qboolean is_cs = ( !Q_stricmp( game, "cstrike" ) || !Q_stricmp( game, "czero" ));
	qboolean is_tfc = !Q_stricmp( game, "tfc" );

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

		// CS gamepad layout. L2/R2 reach the engine as the K_JOY1/K_JOY2
		// trigger axis, not K_L2_BUTTON/K_R2_BUTTON.
		PS3_ModRebind( K_JOY1,      "+attack2",       "+speed"      ); // L2: walk
		PS3_ModRebind( K_RSTICK,    "impulse 100",    "+attack2"    ); // R3
		PS3_ModRebind( K_LSTICK,    "+speed",         "nightvision" ); // L3
		PS3_ModRebind( K_DPAD_UP,   "weapon_crowbar", "buy"         );
		PS3_ModRebind( K_DPAD_DOWN, "impulse 201",    "chooseteam"  );
		PS3_ModRebind( K_DPAD_LEFT, "lastinv",        "invprev"     ); // previous weapon
		// D-pad Right stays invnext; L1 stays lastinv (engine defaults).
	}

	if( is_tfc )
	{
		// L1 primes/throws the hand grenade, R1 the class grenade. Weapon-next
		// stays on D-pad Right. +gren1/+gren2 are unknown to the engine and get
		// forwarded to the TFC server DLL.
		PS3_ModRebind( K_L1_BUTTON, "lastinv", "+gren1" );
		PS3_ModRebind( K_R1_BUTTON, "invnext", "+gren2" );
	}
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

	memset( ps3_btn_prev, 0, sizeof( ps3_btn_prev ));
	ps3_active_port = 0;
	ps3_pad_initialized = true;

	KB_Init();
	Mouse_Init();
}

void PS3_InputShutdown( void )
{
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

// Cursor shows in the native menu or whenever a VGUI1 panel wants one, and
// only for a few seconds after the last stick motion -- so d-pad navigation
// is never fighting a stale pointer.
static qboolean PS3_CursorVisible( void )
{
	return joy_cursor.value != 0.0f
		&& ( cls.key_dest == key_menu || ps3_wants_cursor )
		&& ps3_cursor_valid
		&& ( host.realtime - ps3_cursor_last_move ) < 3.0;
}

// Called by the engine (via Platform_SetCursorType, declared in platform.h)
// whenever the native menu or a VGUI1 panel changes its desired cursor --
// dc_none means "nothing wants a cursor right now."
void GAME_EXPORT Platform_SetCursorType( VGUI_DefaultCursor type )
{
	ps3_wants_cursor = ( type != dc_none );
	// TFC-5 diagnostic: engine dedupes (only calls this on real transitions),
	// so this is not spammy. Confirms whether VGUI1 ever actually requests a
	// cursor at all on real hardware -- remove once the MOTD/team-select
	// interaction bug is closed.
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

// Integrate both sticks into the virtual cursor position -- native menu or
// any VGUI1 panel that currently wants a cursor (see ps3_wants_cursor).
// Left and right stick both move it, so their input adds up if pushed together.
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

// ===========================================================================
// USB keyboard + USB mouse (raw PSL1GHT HID, ported from ioQuake3-PS3)
// ===========================================================================

// Raw USB HID usage code (0x04-0x65) -> engine keynum (0 = unmapped).
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

	// Modifier keys. In RAW code type they arrive in the keycode array as
	// usages 0xE0-0xE7, so they go through the same edge-diff path as every
	// other key -- do NOT derive them from data.mkey, which comes back as
	// garbage in this driver mode and fired CTRL (=+attack) on every press.
	[0xE0]=K_CTRL, [0xE1]=K_SHIFT, [0xE2]=K_ALT,
	[0xE4]=K_CTRL, [0xE5]=K_SHIFT, [0xE6]=K_ALT,
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

// State slots: 0..255 are raw HID usage codes, 256..383 are printable ASCII
// character keys (slot 256+ch). The keyboard driver stays in its INPUTCHAR
// default (calling ioKbSetReadMode corrupts the shared io subsystem and also
// breaks the pad), and in that mode letters/digits/punctuation come back as
// ASCII while special keys come back as raw usages -- so both are handled.
#define KB_SLOT_CHAR   256
#define KB_NUM_SLOTS   384

static KbInfo   s_kb_info;
static qboolean s_kb_connected;
static int      s_kb_port = -1;
static byte     s_kb_cur[KB_NUM_SLOTS];
static byte     s_kb_prev[KB_NUM_SLOTS];
static double   s_kb_last_seen[KB_NUM_SLOTS];

// Resolve a state slot to an engine keynum. 0 = no key event for this slot.
static int KB_SlotToKey( int slot )
{
	if( slot >= KB_SLOT_CHAR )
	{
		int ch = slot - KB_SLOT_CHAR;
		if( ch >= 'A' && ch <= 'Z' ) ch += 'a' - 'A'; // engine wants lowercase
		if( ch == 8 ) return K_BACKSPACE;             // ASCII BS -> engine 127
		return ch;                                    // 9/13/27 already match K_TAB/K_ENTER/K_ESCAPE
	}
	return s_raw_to_key[slot];
}

static void KB_ReleaseAll( void )
{
	int k;

	for( k = 1; k < KB_NUM_SLOTS; k++ )
	{
		if( s_kb_prev[k] && KB_SlotToKey( k ))
			Key_Event( KB_SlotToKey( k ), false );
	}
	memset( s_kb_prev, 0, sizeof( s_kb_prev ));
}

static void KB_Init( void )
{
	int rc = ioKbInit( MAX_KB_PORT_NUM );
	Con_Printf( "[ps3kb] ioKbInit -> %d\n", rc );
	memset( s_kb_cur, 0, sizeof( s_kb_cur ));
	memset( s_kb_prev, 0, sizeof( s_kb_prev ));
	memset( s_kb_last_seen, 0, sizeof( s_kb_last_seen ));
	s_kb_connected = false;
	s_kb_port = -1;
}

static void KB_Shutdown( void )
{
	ioKbEnd();
}

// syncOnly: keep edge state current without emitting events (OSK owns input).
static void KB_Frame( qboolean syncOnly )
{
	KbData data;
	int i, k, found_port, rc;
	double now;
	qboolean dbg = ( kb_debug.value != 0.0f );
	static double s_next_status_log;

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

		// Same sequence as ioQuake3-PS3 (proven on hardware): CodeType(RAW)
		// first, then ReadMode(PACKET). PACKET+RAW makes every key -- letters
		// included -- arrive as a raw HID usage in data.keycode[]; without
		// PACKET the driver falls back to per-character ASCII and the raw
		// table below misses A-Z/0-9.
		rc_ct = ioKbSetCodeType( found_port, KB_CODETYPE_RAW );
		rc_rm = ioKbSetReadMode( found_port, KB_RMODE_PACKET );
		ioKbClearBuf( found_port );
		Con_Printf( "[ps3kb] port %d attached, CodeType(RAW)->%d ReadMode(PACKET)->%d\n",
			found_port, rc_ct, rc_rm );

		memset( s_kb_cur, 0, sizeof( s_kb_cur ));
		memset( s_kb_prev, 0, sizeof( s_kb_prev ));
		memset( s_kb_last_seen, 0, sizeof( s_kb_last_seen ));
		s_kb_port = found_port;
		s_kb_connected = true;
	}

	rc = ioKbRead( s_kb_port, &data );
	if( rc != 0 )
	{
		if( dbg && host.realtime > s_next_status_log )
		{
			Con_Printf( "[ps3kb] ioKbRead port %d -> %d\n", s_kb_port, rc );
			s_next_status_log = host.realtime + 1.0;
		}
		return;
	}

	if( dbg && data.nb_keycode != 0 )
		Con_Printf( "[ps3kb] nb_keycode=%d kc0=0x%04x kc1=0x%04x\n",
			(int)data.nb_keycode, (unsigned)data.keycode[0], (unsigned)data.keycode[1] );

	now = host.realtime;

	if( data.nb_keycode > 0 )
	{
		// New snapshot: rebuild s_kb_cur from the packet. Absent keys = released.
		memset( s_kb_cur, 0, sizeof( s_kb_cur ));
		for( i = 0; i < data.nb_keycode && i < MAX_KEYCODES; i++ )
		{
			u16 kc = data.keycode[i];
			u16 v  = kc & ~((u16)( KB_RAWDAT | KB_KEYPAD ));
			int slot = -1;

			if( kc & KB_RAWDAT )
				slot = v; // special key -> raw HID usage
			else if( v == 0x08 || v == 0x09 || v == 0x0D || v == 0x1B )
				slot = KB_SLOT_CHAR + v; // ASCII BS/TAB/ENTER/ESC (ambiguous with letter usages)
			else if(( v >= '0' && v <= '9' ) || ( v >= 'A' && v <= 'Z' ) || ( v >= 'a' && v <= 'z' ))
				slot = KB_SLOT_CHAR + v; // alphanumeric ASCII character key
			else if( v >= 0x04 && v <= 0x65 && s_raw_to_key[v] )
				slot = v; // known raw HID usage delivered without RAWDAT (e.g. SPACE)
			else if( v >= 0x20 && v <= 0x7E )
				slot = KB_SLOT_CHAR + v; // other printable ASCII (punctuation)

			if( slot > 0 && slot < KB_NUM_SLOTS )
			{
				s_kb_cur[slot] = 1;
				s_kb_last_seen[slot] = now;
			}
		}
	}
	else
	{
		// nb_keycode == 0 means both "no keys" and "read failed" -- the SDK
		// won't say which, so a 2s idle timeout is the only reliable release.
		for( i = 1; i < KB_NUM_SLOTS; i++ )
		{
			if( s_kb_cur[i] && ( now - s_kb_last_seen[i] ) > 2.0 )
				s_kb_cur[i] = 0;
		}
	}

	if( !syncOnly )
	{
		qboolean shift = s_kb_cur[0xE1] || s_kb_cur[0xE5];

		for( k = 1; k < KB_NUM_SLOTS; k++ )
		{
			int key;
			if( s_kb_cur[k] == s_kb_prev[k] )
				continue;
			key = KB_SlotToKey( k );
			if( !key )
				continue;

			if( dbg )
				Con_Printf( "[ps3kb] slot 0x%x -> Key_Event(%d '%c', %d)\n",
					k, key, ( key >= 32 && key < 127 ) ? key : '.', s_kb_cur[k] );

			Key_Event( key, s_kb_cur[k] );

			// Char events: ASCII slots carry the character already; raw
			// printable keynums are their own lowercase ASCII.
			if( s_kb_cur[k] && key >= 32 && key < 127 )
			{
				int ch = key;
				if( k >= KB_SLOT_CHAR )
					ch = ( k - KB_SLOT_CHAR ); // pre-shifted by the driver
				else if( shift )
					ch = KB_ShiftChar( ch );
				CL_CharEvent( ch );
			}
		}
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

// Ports can (dis)connect anytime, so all MAX_PORT_NUM ports are rescanned every frame.
// ioPadGetData's data isn't zero-filled on "no new input" despite its doc comment (per
// ioQuake3-PS3 on real hardware) -- never branch on padData.len, just reuse the buffer.
void Platform_RunEvents( void )
{
	static padData data;
	padInfo info;
	qboolean btn_cur[NUM_PS3_DIGITAL_BUTTONS];
	qboolean osk;
	int i;

	// Must run every frame regardless of pad state -- this is the only place
	// XMB "close application" (SYSUTIL_EXIT_GAME) ever gets drained. Calls
	// Sys_Quit() itself once XMB requests it (see sys_ps3.c).
	PS3_CheckExitRequested();

	if( !ps3_pad_initialized )
		return;

	// Seed WASD + action binds once, after config.cfg has run, filling only
	// keys the player's config left empty (survives its leading unbindall).
	if( !s_kb_binds_seeded && host.config_executed )
	{
		PS3_SeedKeyboardBinds();
		s_kb_binds_seeded = true;
	}

	// Poll USB keyboard/mouse every frame. While the OSK owns input, keep
	// their edge state in sync but emit nothing.
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

		// While the menu cursor is up, CROSS acts as a left click on the
		// hovered item instead of "activate focused item". Latched on press
		// so the matching release always uses the same key.
		if( i == 0 )
		{
			if( btn_cur[0] )
			{
				ps3_cross_is_mouse = PS3_CursorVisible();
				// TFC-5 diagnostic: only on press, confirms whether CROSS
				// is landing as a click (K_MOUSE1) or a gameplay button
				// (K_A_BUTTON), and the cursor state that decided it.
				// Remove once the MOTD/team-select interaction bug is closed.
				Con_Printf( "[ps3cursor] CROSS press: cross_is_mouse=%d wants_cursor=%d key_dest=%d cursor=(%.0f,%.0f)\n",
					(int)ps3_cross_is_mouse, (int)ps3_wants_cursor, (int)cls.key_dest, ps3_cursor_x, ps3_cursor_y );
			}

			// Key_Event(K_MOUSE1, ...) only ever reaches VGui_KeyEvent() --
			// VGUI1's own Panel/CommandButton click activation is driven by
			// App::internalMousePressed, which only fires from the engine's
			// separate IN_MouseEvent() -> VGui_MouseEvent() path (see
			// engine/client/input/input.c). Routing a simulated click
			// through Key_Event was the real reason CROSS moved the cursor
			// onto "OK" but never activated it.
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

	// In the menu with the cursor on, the stick moves the pointer only
	// (PS3_UpdateMenuCursor reads the raw axes). Feed 0 to the nav axes so
	// Joy_ProcessStick does not also step the highlighted item.
	if( joy_cursor.value != 0.0f && cls.key_dest == key_menu )
	{
		Joy_AxisMotionEvent( JOY_AXIS_SIDE, 0 );
		Joy_AxisMotionEvent( JOY_AXIS_FWD,  0 );
	}
	else
	{
		Joy_AxisMotionEvent( JOY_AXIS_SIDE, PS3_ScaleStick( data.ANA_L_H ));
		Joy_AxisMotionEvent( JOY_AXIS_FWD,  PS3_ScaleStick( data.ANA_L_V ));
	}
	Joy_AxisMotionEvent( JOY_AXIS_YAW,   PS3_ScaleStick( data.ANA_R_H ));
	Joy_AxisMotionEvent( JOY_AXIS_PITCH, PS3_ScaleStick( data.ANA_R_V ));
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
