/*
launcher.c - direct xash3d launcher
Copyright (C) 2015 Mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#if XASH_ENABLE_MAIN
#include "build.h"
#include "common.h"
#include "platform/platform.h"

#if XASH_SDLMAIN
#include <SDL.h>
#endif

#ifndef XASH_GAMEDIR
#define XASH_GAMEDIR "valve" // !!! Replace with your default (base) game directory !!!
#endif

static int  szArgc;
static char **szArgv;

static void Sys_ChangeGame( const char *progname )
{
	// stub
}

int main( int argc, char **argv )
{
	int ret;

#if XASH_PS3 && defined( XASH_PS3_GAME )
	// The XMB launches EBOOT.BIN with no arguments, so the mod directory picked
	// at build time (the "flavor") has to be injected here. It deliberately does
	// NOT go through XASH_GAMEDIR: that macro is the engine basedir, and
	// FS_InitStdio only adds the base game hierarchy when basedir differs from
	// the gamedir -- setting both to the mod would drop valve/ out of the search
	// path and take every shared asset with it. This mirrors `xash -game bshift`
	// on PC. Nothing is discarded by overwriting argv, since there is nothing to
	// discard on this platform; argv[0] is preserved for anything that reads it.
	static char *ps3_argv[3];

	ps3_argv[0] = argc > 0 ? argv[0] : (char *)"EBOOT.BIN";
	ps3_argv[1] = (char *)"-game";
	ps3_argv[2] = (char *)XASH_PS3_GAME;

	szArgc = 3;
	szArgv = ps3_argv;
#elif XASH_PSVITA
	// inject -dev -console into args if required
	szArgc = PSVita_GetArgv( argc, argv, &szArgv );
#elif XASH_IOS
	IOS_LaunchDialog();
	szArgc = IOS_GetArgs( &szArgv );
#else
	szArgc = argc;
	szArgv = argv;
#endif // XASH_PSVITA
	ret = Host_Main( szArgc, szArgv, XASH_GAMEDIR, 0, Sys_ChangeGame );

	return ret;
}
#endif // XASH_ENABLE_MAIN
