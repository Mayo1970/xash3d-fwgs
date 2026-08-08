/*
sys_win.c - posix system utils
Copyright (C) 2019 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <unistd.h> // fork
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h> // gettimeofday -- PS3 has no clock_gettime, see Platform_DoubleTime below
#include "platform/platform.h"
#include "menu_int.h"

#if !XASH_ANDROID && !XASH_NSWITCH && !XASH_PSVITA && !XASH_PS3
void Platform_ShellExecute( const char *path, const char *parms )
{
	const char *argv[] = { OPEN_COMMAND, path, NULL };
	pid_t id = fork();

	if( id == 0 )
	{
		execvp( OPEN_COMMAND, (char **)argv );
		fprintf( stderr, "error opening %s %s", OPEN_COMMAND, path );
		exit( 1 );
	}
}
#endif // !XASH_ANDROID && !XASH_NSWITCH && !XASH_PSVITA && !XASH_PS3

void Posix_Daemonize( void )
{
	if( Sys_CheckParm( "-daemonize" ))
	{
#if XASH_POSIX && defined(_POSIX_VERSION) && !defined(XASH_MOBILE_PLATFORM)
		pid_t daemon = fork();

		if( daemon < 0 )
		{
			Host_Error( "fork() failed: %s\n", strerror( errno ) );
		}

		if( daemon > 0 )
		{
			// parent
			Con_Reportf( "Child pid: %ld\n", (long)daemon );
			exit( 0 );
		}
		else
		{
			// don't be closed by parent
			if( setsid() < 0 )
			{
				Host_Error( "setsid() failed: %s\n", strerror( errno ) );
			}

			// set permissions
			umask( 0 );

			// engine will still use stdin/stdout,
			// so just redirect them to /dev/null
			close( STDIN_FILENO );
			close( STDOUT_FILENO );
			close( STDERR_FILENO );
			open("/dev/null", O_RDONLY); // becomes stdin
			open("/dev/null", O_RDWR); // stdout
			open("/dev/null", O_RDWR); // stderr

			// fallthrough
		}
#elif defined(XASH_MOBILE_PLATFORM)
		Sys_Error( "Can't run in background on mobile platforms!" );
#else
		Sys_Error( "Daemonize not supported on this platform!" );
#endif
	}

}

static void Posix_SigtermCallback( int signal )
{
	string reason;
	Q_snprintf( reason, sizeof( reason ), "caught signal %d", signal );
	Sys_Quit( reason );
}

void Posix_SetupSigtermHandling( void )
{
#if !XASH_PSVITA && !XASH_PS3
	struct sigaction act = { 0 };
	act.sa_handler = Posix_SigtermCallback;
	act.sa_flags = 0;
	sigaction( SIGTERM, &act, NULL );
#endif
	// PS3: PSL1GHT has no sigaction() at all (no symbol, same as psvita) --
	// there's no equivalent LV2 process-signal mechanism to wire up here.
}

#if XASH_TIMER == TIMER_POSIX
double Platform_DoubleTime( void )
{
#if XASH_PS3
	// PSL1GHT's libc has no clock_gettime()/CLOCK_MONOTONIC at all (same
	// Cygwin-only gating in sys/features.h that also hides nanosleep) --
	// gettimeofday() is the finest-grained clock actually available.
	struct timeval tv;
	gettimeofday( &tv, NULL );
	return (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
#else
	struct timespec ts;
#if XASH_IRIX
	clock_gettime( CLOCK_SGI_CYCLE, &ts );
#else
	clock_gettime( CLOCK_MONOTONIC, &ts );
#endif
	return (double) ts.tv_sec + (double) ts.tv_nsec/1000000000.0;
#endif
}

void Platform_Sleep( int msec )
{
	usleep( msec * 1000 );
}
#endif // XASH_TIMER == TIMER_POSIX

