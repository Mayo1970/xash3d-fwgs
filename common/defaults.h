/*
defaults.h - set up default configuration
Copyright (C) 2016 Mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef DEFAULTS_H
#define DEFAULTS_H

#include "backends.h"
#include "build.h"

/*
===================================================================

SETUP BACKENDS DEFINITIONS

===================================================================
*/
//
// when compiling client, we need to pick video, audio and input implementations
//
#if !XASH_DEDICATED // when compiling client, we need to pick video, audio and input implementations
	#if XASH_SDL // we are building with SDL
		#define XASH_VIDEO     VIDEO_SDL
		#define XASH_INPUT     INPUT_SDL
		#define XASH_SOUND     SOUND_SDL
	#elif XASH_LINUX // we are building for Linux without SDL, only framebuffer is supported for now
		#define XASH_VIDEO     VIDEO_FBDEV
		#define XASH_INPUT     INPUT_EVDEV
		#define XASH_SOUND     SOUND_ALSA
		#define XASH_USE_EVDEV 1
	#elif XASH_DOS4GW
		#define XASH_VIDEO     VIDEO_DOS
		#define XASH_REDUCE_FD 1 // usually only 10-20 fds available
	#elif XASH_PSP
		#define XASH_VIDEO     VIDEO_PSP
		#define XASH_INPUT     INPUT_PSP
		#define XASH_SOUND     SOUND_PSP
		#define XASH_REDUCE_FD 1
		#define XASH_NO_TOUCH  1
		#define XASH_NO_ZIP    1
	#elif XASH_PS3
		#define XASH_VIDEO     VIDEO_PS3
		#define XASH_INPUT     INPUT_PS3
		#define XASH_SOUND     SOUND_PS3
	#endif
#endif // !XASH_DEDICATED

//
// select messagebox implementation
//
#ifndef XASH_MESSAGEBOX
	#if XASH_SDL >= 2 && !XASH_NSWITCH // SDL2 messageboxes are not available on NSW
		#define XASH_MESSAGEBOX MSGBOX_SDL
	#elif XASH_WIN32
		#define XASH_MESSAGEBOX MSGBOX_WIN32
	#elif XASH_NSWITCH
		#define XASH_MESSAGEBOX MSGBOX_NSWITCH
	#elif XASH_PSP
		#define XASH_MESSAGEBOX MSGBOX_PSP
	#else // !XASH_WIN32
		#define XASH_MESSAGEBOX MSGBOX_STDERR
	#endif // !XASH_WIN32
#endif // XASH_MESSAGEBOX

//
// no timer - no xash
//
#ifndef XASH_TIMER
	#if XASH_SDL >= 2
		#define XASH_TIMER TIMER_SDL
	#elif XASH_WIN32
		#define XASH_TIMER TIMER_WIN32
	#elif XASH_DOS4GW
		#define XASH_TIMER TIMER_DOS
	#elif XASH_PSP
		#define XASH_TIMER TIMER_PSP
	#else // !XASH_WIN32
		#define XASH_TIMER TIMER_POSIX
	#endif // !XASH_WIN32
#endif

//
// determine movie playback backend
//
#ifndef XASH_AVI
	#if HAVE_FFMPEG
		#define XASH_AVI AVI_FFMPEG
	#elif XASH_PS3
		// no ffmpeg in ps3toolchain: stock Half-Life media is Cinepak/MSRLE
		// in a plain RIFF AVI container, which the native backend decodes
		#define XASH_AVI AVI_NATIVE
	#else
		#define XASH_AVI AVI_NULL
	#endif
#endif

#if XASH_PSP
	#define XASH_PSP LIB_PSP
#elif defined( XASH_STATIC_LIBS )
	#define XASH_LIB LIB_STATIC
	#define XASH_INTERNAL_GAMELIBS
	#define XASH_ALLOW_SAVERESTORE_OFFSETS
#elif XASH_WIN32
	#define XASH_LIB LIB_WIN32
#elif XASH_POSIX
	#define XASH_LIB LIB_POSIX
#endif

//
// fallback to NULL
//
#ifndef XASH_VIDEO
	#define XASH_VIDEO VIDEO_NULL
#endif // XASH_VIDEO

#ifndef XASH_SOUND
	#define XASH_SOUND SOUND_NULL
#endif // XASH_SOUND

#ifndef XASH_INPUT
	#define XASH_INPUT INPUT_NULL
#endif // XASH_INPUT

/*
=========================================================================

Default build-depended cvar and constant values

=========================================================================
*/

// Platform overrides
#if XASH_WIN32
	// set up windowed by default on Windows to avoid problems with
	// Xbox Game Bar
	#define DEFAULT_FULLSCREEN   "0"
#elif XASH_NSWITCH
	#define DEFAULT_TOUCH_ENABLE "1"
	#define DEFAULT_M_IGNORE     "1"
	#define DEFAULT_MODE_WIDTH   1280
	#define DEFAULT_MODE_HEIGHT  720
	#define DEFAULT_ALLOWCONSOLE 1
#elif XASH_PSVITA
	#define DEFAULT_TOUCH_ENABLE "1"
	#define DEFAULT_M_IGNORE     "1"
	#define DEFAULT_MODE_WIDTH   960
	#define DEFAULT_MODE_HEIGHT  544
	#define DEFAULT_ALLOWCONSOLE 1
#elif XASH_PS3
	// resolution is fixed by the TV, not requested by the app; 720p is the
	// safe default fill-rate target (see the PS3 skill's RSX guidance)
	#define DEFAULT_MODE_WIDTH   1280
	#define DEFAULT_MODE_HEIGHT  720
	#define DEFAULT_ALLOWCONSOLE 1
	// A listen server sizes svs.packet_entities as
	//   maxclients * SV_UPDATE_BACKUP * NUM_PACKET_ENTITIES * sizeof( entity_state_t )
	// which at the stock MAX_CLIENTS of 32 is 32 * 64 * 256 * 340 = exactly
	// 170 MiB -- upstream says as much itself at netchan.h:79. Z_Realloc needs
	// that as ONE contiguous run, out of a ~190 MB XDR budget that already has
	// ~105 MB in it after a singleplayer session, so it reliably dies in
	// _Mem_Alloc. At 4 the same expression is 22.3 MB.
	//
	// This is a real capacity limit, not just an allocator dodge: the in-order
	// PPE has no headroom to simulate 32 players at 60fps either. Raising it is
	// a one-line change, but read PS3_ProbeMemory's largest-contiguous-block
	// line from a real hardware log first -- do not guess a bigger number.
	#define DEFAULT_MAX_LISTEN_CLIENTS 4
#elif XASH_ANDROID
	#define DEFAULT_TOUCH_ENABLE "1"
#elif XASH_MOBILE_PLATFORM
	#define DEFAULT_TOUCH_ENABLE "1"
	#define DEFAULT_M_IGNORE     "1"
#endif // !XASH_MOBILE_PLATFORM && !XASH_NSWITCH

// Defaults
#ifndef DEFAULT_TOUCH_ENABLE
	#define DEFAULT_TOUCH_ENABLE "0"
#endif // DEFAULT_TOUCH_ENABLE

#ifndef DEFAULT_M_IGNORE
	#define DEFAULT_M_IGNORE "0"
#endif // DEFAULT_M_IGNORE

#ifndef DEFAULT_JOY_DEADZONE
	#define DEFAULT_JOY_DEADZONE "4096"
#endif // DEFAULT_JOY_DEADZONE

#ifndef DEFAULT_DEV
	#define DEFAULT_DEV 0
#endif // DEFAULT_DEV

#ifndef DEFAULT_ALLOWCONSOLE
	#define DEFAULT_ALLOWCONSOLE 0
#endif // DEFAULT_ALLOWCONSOLE

#ifndef DEFAULT_FULLSCREEN
	#define DEFAULT_FULLSCREEN "2" // must be a string
#endif // DEFAULT_FULLSCREEN

#ifndef DEFAULT_MAX_EDICTS
	#define DEFAULT_MAX_EDICTS 1200 // was 900 before HL25
#endif // DEFAULT_MAX_EDICTS

#ifndef DEFAULT_MAX_LISTEN_CLIENTS
	// expands macro-to-macro; MAX_CLIENTS (xash3d_types.h) only has to be
	// visible at the use site, not here
	#define DEFAULT_MAX_LISTEN_CLIENTS MAX_CLIENTS
#endif // DEFAULT_MAX_LISTEN_CLIENTS

#ifndef DEFAULT_CL_LW
	#if XASH_PS3
		// Client-side weapon prediction is off by default on PS3.
		//
		// Upstream keeps the server game DLL and the client game DLL in
		// separate shared objects, so each gets its own compile of the
		// shared dlls/*.cpp weapon classes: the client's copy resolves
		// PRECACHE_MODEL and friends through the client's g_engfuncs (the
		// harmless stubs HUD_InitClientWeapons installs), the server's copy
		// through the real engine table. PSL1GHT has no dlopen, so this port
		// statically links both modules into one executable -- and while
		// objcopy -G keeps the two *function bodies* separate, only a single
		// vtable per weapon class survives the final link. Confirmed on the
		// real build: two CGlock::Spawn/Precache bodies, one _ZTV6CGlock,
		// and that vtable dispatches into the SERVER-compiled copies.
		//
		// The consequence is that HUD_PrepEntity's g_Glock.Spawn() ran server
		// weapon code against the real engine table, reaching pfnPrecacheModel
		// -> SV_ModelIndex/Mod_ForName (a disk model load) in the middle of
		// client prediction -- a hard console lock on this no-MMU platform,
		// on the first frame the client reached ca_active. See AGENTS.md
		// goal 10.
		//
		// cl_lw 0 makes HUD_PostRunCmd take its else branch, so
		// HUD_WeaponsPostThink -- and therefore HUD_InitClientWeapons and all
		// client-side weapon Spawn calls -- never run. Movement prediction
		// (pfnPlayerMove) is unaffected. The server stays authoritative for
		// weapons, which on a loopback single-player listen server costs
		// essentially nothing.
		#define DEFAULT_CL_LW "0"
		// Not FCVAR_ARCHIVE on PS3, and read-only: this is a platform
		// limitation, not a user preference, and an archived config.cfg from
		// an earlier build would otherwise set it straight back to 1 after
		// the default is registered -- which is exactly what happened on the
		// first attempt at this fix.
		#define DEFAULT_CL_LW_FLAGS ( FCVAR_READ_ONLY | FCVAR_USERINFO )
	#else
		#define DEFAULT_CL_LW "1"
		#define DEFAULT_CL_LW_FLAGS ( FCVAR_ARCHIVE | FCVAR_USERINFO )
	#endif
#endif // DEFAULT_CL_LW


#ifndef DEFAULT_ACCELERATED_RENDERER
	#if XASH_PSP
		#define DEFAULT_ACCELERATED_RENDERER "gu"
	#elif XASH_MOBILE_PLATFORM
		#define DEFAULT_ACCELERATED_RENDERER "gles1"
	#elif XASH_IOS
		#define DEFAULT_ACCELERATED_RENDERER "gles2"
	#else // !XASH_MOBILE_PLATFORM
		#define DEFAULT_ACCELERATED_RENDERER "gl"
	#endif // !XASH_MOBILE_PLATFORM
#endif // DEFAULT_ACCELERATED_RENDERER

#ifndef DEFAULT_SOFTWARE_RENDERER
	#define DEFAULT_SOFTWARE_RENDERER "soft" // mittorn's ref_soft
#endif // DEFAULT_SOFTWARE_RENDERER

#endif // DEFAULTS_H
