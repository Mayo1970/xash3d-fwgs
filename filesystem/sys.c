/*
sys.c - stringlist, directory walk, raw OS file I/O for filesystem
Copyright (C) 2003-2006 Mathieu Olivier
Copyright (C) 2000-2007 DarkPlaces contributors
Copyright (C) 2007 Uncle Mike
Copyright (C) 2015-2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#define _GNU_SOURCE 1

#include "build.h"
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#if XASH_WIN32
	#include <direct.h>
	#include <io.h>
	#include "utflib.h"
#elif XASH_DOS4GW
	#include <direct.h>
#else
	#include <dirent.h>
#endif

#if HAVE_MEMFD_CREATE
	#include <sys/mman.h>
#endif

#include <stdio.h>
#include "port.h"
#include "crtlib.h"
#include "filesystem.h"
#include "filesystem_internal.h"
#include "common/com_strings.h"

#if !defined( O_BINARY )
	#define O_BINARY 0
#endif

#if !defined( O_TEXT )
	#define O_TEXT 0
#endif

#if !defined( MFD_NOEXEC_SEAL )
	#define MFD_NOEXEC_SEAL 8U
#endif

#if !defined( S_ISREG )
	#define S_ISREG( m ) ( FBitSet( m, S_IFMT ) == S_IFREG )
#endif

#if !defined( S_ISDIR )
	#define S_ISDIR( m ) ( FBitSet( m, S_IFMT ) == S_IFDIR )
#endif

#if !XASH_PSVITA && !XASH_NSWITCH
	#define HAVE_DUP
#endif

#if XASH_PS3
// LV2 filesystem syscalls take absolute paths only -- there is no real
// per-process working directory on PS3. chdir() itself dispatches through a
// real syscall and reports success, but it doesn't establish any state that
// opendir()/stat()/open() actually consult: opendir(".") reliably fails
// ENOENT on real hardware after a "successful" chdir(fs_rootdir) (confirmed
// on real hardware, not guessed -- see AGENTS.md goal-stack #3). Every
// relative path the generic FS layer builds assuming chdir() worked has to
// be resolved against fs_rootdir by hand instead, mirroring how every other
// console port in this workspace (e.g. ioQuake3-PS4's Sys_Cwd()) treats
// "no real cwd" as a hard platform fact, not a bug to paper over.
const char *PS3_ResolvePath( const char *in, char *out, size_t outsize )
{
	if( in[0] == '/' )
	{
		Q_strncpy( out, in, outsize );
		return out;
	}

	if( in[0] == '.' && in[1] == '/' )
		in += 2;

	if( in[0] == '\0' )
		Q_strncpy( out, fs_rootdir, outsize );
	else
		Q_snprintf( out, outsize, "%s/%s", fs_rootdir, in );

	return out;
}

fs_prof_t fs_prof;

double FS_Prof_Time( void )
{
	// same clock Platform_DoubleTime() uses on this platform -- PSL1GHT's libc
	// exposes no clock_gettime()
	struct timeval tv;
	gettimeofday( &tv, NULL );
	return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

// every stat() in this file funnels through here so the profiler sees all of
// them, and so the PS3_ResolvePath() prefixing lives in exactly one place
static int FS_SysStat( const char *path, struct stat *buf )
{
	char ps3_path[MAX_SYSPATH];
	double start = FS_Prof_Time();
	int ret;

	path = PS3_ResolvePath( path, ps3_path, sizeof( ps3_path ));
	ret = stat( path, buf );

	fs_prof.stat_calls++;
	fs_prof.stat_msec += FS_Prof_Time() - start;

	return ret;
}

/*
==================
PS3_ProbeDirentType

After the dir-cache work the dominant FS cost is FS_FindFile_DIR's per-lookup
FS_SysFileExists(), whose only job beyond "does this exist" is the S_ISREG test
-- and readdir() already answers that in d_type. Trusting it requires two facts
this platform is not documented for: that LV2 fills the field in at all rather
than answering DT_UNKNOWN, and that it agrees with stat().

Probe one real directory once per boot and print the verdict. Runs outside the
listdirectory() timing and calls stat() directly instead of FS_SysStat(), so it
cannot skew the counters it exists to justify. `resolved` has already been
through PS3_ResolvePath().
==================
*/
static void PS3_ProbeDirentType( const char *resolved )
{
	uint32_t reg = 0, isdir = 0, unknown = 0, other = 0, mismatch = 0, unstattable = 0;
	struct dirent *entry;
	DIR *dir = opendir( resolved );

	if( !dir )
		return;

	while(( entry = readdir( dir )))
	{
		char probe[MAX_SYSPATH];
		struct stat st;
		qboolean claims_dir = entry->d_type == DT_DIR;
		qboolean claims_reg = entry->d_type == DT_REG;

		switch( entry->d_type )
		{
		case DT_REG: reg++; break;
		case DT_DIR: isdir++; break;
		case DT_UNKNOWN: unknown++; break;
		default: other++; break;
		}

		Q_snprintf( probe, sizeof( probe ), "%s/%s", resolved, entry->d_name );

		if( stat( probe, &st ) < 0 )
		{
			unstattable++;
			continue;
		}

		if( claims_dir != ( S_ISDIR( st.st_mode ) != 0 ) || claims_reg != ( S_ISREG( st.st_mode ) != 0 ))
		{
			if( mismatch < 4 ) // name a few, don't flood the log
			{
				Con_Printf( "[fs/dtype] mismatch: %s d_type=%d stat=%s\n", entry->d_name, (int)entry->d_type,
					S_ISDIR( st.st_mode ) ? "dir" : S_ISREG( st.st_mode ) ? "reg" : "other" );
			}
			mismatch++;
		}
	}

	closedir( dir );

	Con_Printf( "[fs/dtype] probe %s: reg %u dir %u unknown %u other %u | %u mismatch(es), %u unstattable\n",
		resolved, reg, isdir, unknown, other, mismatch, unstattable );
	Con_Printf( "[fs/dtype] verdict for this directory: d_type %s\n",
		unknown || other || mismatch ? "NOT trustworthy" : "matches stat on every entry" );
}
#endif // XASH_PS3

/*
=============================================================================

FILEMATCH COMMON SYSTEM

=============================================================================
*/
void stringlistinit( stringlist_t *list )
{
	memset( list, 0, sizeof( *list ));
}

void stringlistfreecontents( stringlist_t *list )
{
	for( int i = 0; i < list->numstrings; i++ )
	{
		if( list->strings[i] )
			Mem_Free( list->strings[i] );
		list->strings[i] = NULL;
	}

	if( list->strings )
		Mem_Free( list->strings );

	if( list->types )
		Mem_Free( list->types );

	list->numstrings = 0;
	list->maxstrings = 0;
	list->strings = NULL;
	list->types = NULL;
}

void stringlistappendtyped( stringlist_t *list, const char *text, byte type )
{
	size_t textlen;

	if( !Q_strcmp( text, "." ) || !Q_strcmp( text, ".." ))
		return; // ignore the virtual directories

	if( list->numstrings >= list->maxstrings )
	{
		list->maxstrings += 4096;
		list->strings = Mem_Realloc( fs_mempool, list->strings, list->maxstrings * sizeof( *list->strings ));
		list->types = Mem_Realloc( fs_mempool, list->types, list->maxstrings * sizeof( *list->types ));
	}

	textlen = Q_strlen( text ) + 1;
	list->strings[list->numstrings] = Mem_Calloc( fs_mempool, textlen );
	memcpy( list->strings[list->numstrings], text, textlen );
	list->types[list->numstrings] = type;
	list->numstrings++;
}

void stringlistappend( stringlist_t *list, const char *text )
{
	stringlistappendtyped( list, text, FS_ENT_UNKNOWN );
}

void stringlistsort( stringlist_t *list )
{
	// this is a selection sort (finds the best entry for each slot)
	for( int i = 0; i < list->numstrings - 1; i++ )
	{
		for( int j = i + 1; j < list->numstrings; j++ )
		{
			if( Q_strcmp( list->strings[i], list->strings[j] ) > 0 )
			{
				char *temp = list->strings[i];
				list->strings[i] = list->strings[j];
				list->strings[j] = temp;

				// types[] is parallel to strings[]; leaving it behind here would
				// silently hand every entry its neighbour's type
				if( list->types )
				{
					byte temptype = list->types[i];
					list->types[i] = list->types[j];
					list->types[j] = temptype;
				}
			}
		}
	}
}

// convert names to lowercase because dos doesn't care, but pattern matching code often does
MAYBE_UNUSED static void listlowercase( stringlist_t *list )
{
	for( int i = 0; i < list->numstrings; i++ )
	{
		for( char *c = list->strings[i]; *c; c++ )
			*c = Q_tolower( *c );
	}
}

void listdirectory( stringlist_t *list, const char *path, qboolean dirs_only )
{
#if XASH_WIN32
	char pattern[4096];
	Q_snprintf( pattern, sizeof( pattern ), "%s/*", path );

	// ask for the directory listing handle
	struct _finddata_t n_file = { 0 };
	intptr_t hFile = _findfirst( pattern, &n_file );
	if( hFile == -1 )
		return;

	// start a new chain with the the first name
	stringlistappend( list, n_file.name );

	// iterate through the directory
	while( _findnext( hFile, &n_file ) == 0 )
	{
		if( dirs_only && !FBitSet( n_file.attrib, _A_SUBDIR ))
			continue;

		stringlistappend( list, n_file.name );
	}

	_findclose( hFile );
#else // !XASH_WIN32
#if XASH_PS3
	static qboolean prof_dtype_probed = false;
	char ps3_path[MAX_SYSPATH];
	double prof_start = FS_Prof_Time();
	uint32_t prof_entries = 0;

	path = PS3_ResolvePath( path, ps3_path, sizeof( ps3_path ));
	fs_prof.listdir_calls++;
#endif
	DIR *dir = opendir( path );

	if( !dir )
	{
#if XASH_PS3
		fs_prof.listdir_msec += FS_Prof_Time() - prof_start;
#endif
		return;
	}

	// iterate through the directory
	struct dirent *entry;
	while(( entry = readdir( dir )))
	{
#if XASH_PS3
		prof_entries++;

		// free tally: no syscall, and it covers every directory in the session
		// rather than the single one PS3_ProbeDirentType() cross-checks
		switch( entry->d_type )
		{
		case DT_REG: fs_prof.dtype_reg++; break;
		case DT_DIR: fs_prof.dtype_dir++; break;
		case DT_UNKNOWN: fs_prof.dtype_unknown++; break;
		default: fs_prof.dtype_other++; break;
		}
#endif
#if HAVE_DIRENT_D_TYPE
		if( dirs_only && entry->d_type != DT_DIR && entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN )
			continue;

		// DT_LNK deliberately stays UNKNOWN: d_type describes the link, not its
		// target, so only a stat() can say whether it resolves to a regular file
		stringlistappendtyped( list, entry->d_name,
			entry->d_type == DT_REG ? FS_ENT_FILE :
			entry->d_type == DT_DIR ? FS_ENT_DIR : FS_ENT_UNKNOWN );
#else
		stringlistappend( list, entry->d_name );
#endif // HAVE_DIRENT_D_TYPE
	}

	closedir( dir );
#if XASH_PS3
	fs_prof.listdir_entries += prof_entries;
	fs_prof.listdir_msec += FS_Prof_Time() - prof_start;

	// cross-check d_type against stat once, on the first directory with enough
	// entries to be worth the syscalls. Deliberately after the timing above
	if( !prof_dtype_probed && prof_entries >= 8 )
	{
		prof_dtype_probed = true;
		PS3_ProbeDirentType( path );
	}
#endif
#endif // !XASH_WIN32

#if XASH_DOS4GW
	// convert names to lowercase because 8.3 always in CAPS
	listlowercase( list );
#endif // XASH_DOS4GW
}

/*
====================
FS_PathToWideChar

Converts input UTF-8 string to wide char string.
====================
*/
MAYBE_UNUSED static const wchar_t *FS_PathToWideChar( const char *path )
{
#if XASH_WIN32
	static wchar_t pathBuffer[MAX_PATH];
	MultiByteToWideChar( CP_UTF8, 0, path, -1, pathBuffer, MAX_PATH );
	return pathBuffer;
#endif
	return L"";
}

/*
============
FS_CreatePath

Only used for FS_Open.
============
*/
void FS_CreatePath( char *path )
{
	char *walk = path;
#if XASH_PS3
	char ps3_path[MAX_SYSPATH];
	walk = (char *)PS3_ResolvePath( path, ps3_path, sizeof( ps3_path ));
#endif

	for( char *ofs = walk + 1; *ofs; ofs++ )
	{
		if( *ofs == '/' || *ofs == '\\' )
		{
			// create the directory
			char save = *ofs;
			*ofs = 0;
#if XASH_WIN32
			_mkdir( walk ); // use _wmkdir maybe?
#else // !XASH_WIN32
			mkdir( walk, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH );
#endif // !XASH_WIN32
			*ofs = save;
		}
	}
}

/*
====================
FS_SysFileTime

Internal function used to determine filetime
====================
*/
int FS_SysFileTime( const char *filename )
{
#if XASH_WIN32
	struct _stat buf;
	if( _wstat( FS_PathToWideChar( filename ), &buf ) < 0 )
#else
	struct stat buf;
#if XASH_PS3
	if( FS_SysStat( filename, &buf ) < 0 )
#else
	if( stat( filename, &buf ) < 0 )
#endif
#endif
		return -1;

	return buf.st_mtime;
}

/*
==================
FS_SysFileExists

Look for a file in the filesystem only
==================
*/
qboolean FS_SysFileExists( const char *path )
{
#if XASH_WIN32
	struct _stat buf;
	if( _wstat( FS_PathToWideChar( path ), &buf ) < 0 )
#else // !XASH_WIN32
	struct stat buf;
#if XASH_PS3
	if( FS_SysStat( path, &buf ) < 0 )
#else
	if( stat( path, &buf ) < 0 )
#endif
#endif // !XASH_WIN32
		return false;

	return S_ISREG( buf.st_mode );
}

/*
==================
FS_SysFolderExists

Look for a existing folder
==================
*/
qboolean FS_SysFolderExists( const char *path )
{
#if XASH_WIN32
	struct _stat buf;
	if( _wstat( FS_PathToWideChar( path ), &buf ) < 0 )
#else
	struct stat buf;
#if XASH_PS3
	if( FS_SysStat( path, &buf ) < 0 )
#else
	if( stat( path, &buf ) < 0 )
#endif
#endif
		return false;

	return S_ISDIR( buf.st_mode );
}

/*
==============
FS_SysFileOrFolderExists

Check if filesystem entry exists at all, don't mind the type
==============
*/
qboolean FS_SysFileOrFolderExists( const char *path )
{
#if XASH_WIN32
	struct _stat buf;
	return _wstat( FS_PathToWideChar( path ), &buf ) >= 0;
#else
	struct stat buf;
#if XASH_PS3
	return FS_SysStat( path, &buf ) >= 0;
#else
	return stat( path, &buf ) >= 0;
#endif
#endif
}

/*
====================
FS_SysOpen

Internal function used to create a file_t and open the relevant non-packed file on disk
====================
*/
file_t *FS_SysOpen( const char *filepath, const char *mode )
{
	file_t *file;
	int mod, opt, fd = -1;
	qboolean memfile = false;
	uint ind;

	// Parse the mode string
	switch( mode[0] )
	{
	case 'r': // read
		mod = O_RDONLY;
		opt = 0;
		break;
	case 'w': // write
		mod = O_WRONLY;
		opt = O_CREAT | O_TRUNC;
		break;
	case 'a': // append
		mod = O_WRONLY;
		opt = O_CREAT | O_APPEND;
		break;
	case 'e': // edit
		mod = O_WRONLY;
		opt = O_CREAT;
		break;
	default:
		return NULL;
	}

	for( ind = 1; mode[ind] != '\0'; ind++ )
	{
		switch( mode[ind] )
		{
		case '+':
			mod = O_RDWR;
			break;
		case 'b':
			opt |= O_BINARY;
			break;
		default:
			break;
		}
	}

	// the 'm' flag let's user to create temporary file in memory
	// through so-called "anonymous files"
	if( Q_strchr( mode, 'm' ))
	{
#if HAVE_MEMFD_CREATE
		fd = memfd_create( filepath, MFD_CLOEXEC | MFD_NOEXEC_SEAL );

		// through fcntl() and MFD_ALLOW_SEALING we could enforce
		// read-write flags but we don't really care about them yet
		if( fd < 0 )
			Con_Printf( S_WARN "%s: can't create anonymous file %s: %s\n", __func__, filepath, strerror( errno ));
		else
			memfile = true;
#endif
		// if it's unsupported, we can open it on disk
	}

	if( fd < 0 )
	{
#if XASH_WIN32
		fd = _wopen( FS_PathToWideChar( filepath ), mod | opt, 0666 );
#else // !XASH_WIN32
#if XASH_PS3
		char ps3_path[MAX_SYSPATH];
		double prof_start = FS_Prof_Time();

		filepath = PS3_ResolvePath( filepath, ps3_path, sizeof( ps3_path ));
#endif
		fd = open( filepath, mod | opt, 0666 );
#if XASH_PS3
		fs_prof.open_calls++;
		fs_prof.open_msec += FS_Prof_Time() - prof_start;
#endif
#endif // !XASH_WIN32
	}

	if( fd < 0 )
	{
		if( errno != ENOENT )
			Con_Printf( S_ERROR "%s: can't open file %s: %s\n", __func__, filepath, strerror( errno ));

		return NULL;
	}

	file = (file_t *)Mem_Calloc( fs_mempool, sizeof( *file ));
	file->filetime = memfile ? 0 : FS_SysFileTime( filepath );
	file->ungetc = EOF;
	file->handle = fd;

	if( !memfile )
		FS_BackupFileName( file, filepath, mod | opt );

	file->searchpath = NULL;
	file->real_length = lseek( file->handle, 0, SEEK_END );

	// uncomment do disable write
	//if( opt & O_CREAT )
	//	return NULL;

	// For files opened in append mode, we start at the end of the file
	if( opt & O_APPEND )
		file->position = file->real_length;
	else
		lseek( file->handle, 0, SEEK_SET );

	return file;
}

/*
====================
FS_OpenHandle

====================
*/
file_t *FS_OpenHandle( searchpath_t *searchpath, int handle, fs_offset_t offset, fs_offset_t len )
{
	file_t *file = (file_t *)Mem_Calloc( fs_mempool, sizeof( file_t ));

#ifdef XASH_REDUCE_FD
	file->backup_position = offset;
	file->backup_path = copystring( syspath );
	file->backup_options = O_RDONLY|O_BINARY;
	file->handle = -1;
#else // !XASH_REDUCE_FD
#ifdef HAVE_DUP
	file->handle = dup( handle );
#else // !HAVE_DUP
	file->handle = open( searchpath->filename, O_RDONLY|O_BINARY );
#endif // !HAVE_DUP

	if( file->handle < 0 )
	{
		Con_Printf( S_ERROR "%s: couldn't create fd for %s:0x%lx: %s\n", __func__, searchpath->filename, (long)offset, strerror( errno ));
		Mem_Free( file );
		return NULL;
	}

	if( lseek( file->handle, offset, SEEK_SET ) == -1 )
	{
		Mem_Free( file );
		return NULL;
	}
#endif // !XASH_REDUCE_FD

	file->real_length = len;
	file->offset = offset;
	file->position = 0;
	file->ungetc = EOF;
	file->searchpath = searchpath;

	return file;
}

/*
==================
FS_SetCurrentDirectory

Sets current directory, path should be in UTF-8 encoding
TODO: make this non-fatal
==================
*/
int FS_SetCurrentDirectory( const char *path )
{
#if XASH_WIN32
	if( !SetCurrentDirectoryW( FS_PathToWideChar( path )))
	{
		const DWORD fm_flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK;
		DWORD errorcode;
		wchar_t wide_buf[1024];
		char buf[1024];

		FormatMessageW( fm_flags, NULL, GetLastError(), 0, wide_buf, sizeof( wide_buf ) / sizeof( wide_buf[0] ), NULL );
		Q_UTF16ToUTF8( buf, sizeof( buf ), wide_buf, sizeof( wide_buf ) / sizeof( wide_buf[0] ));

		Sys_Error( "Changing directory to %s failed: %s\n", path, buf );
		return false;
	}
#elif XASH_POSIX
	if( chdir( path ) < 0 )
	{
		Sys_Error( "Changing directory to %s failed: %s\n", path, strerror( errno ));
		return false;
	}
#else
	// it may be fine for some systems to skip chdir
	Con_Printf( "%s: not implemented, ignoring...\n", __func__ );
	return true;
#endif

	Con_Printf( "%s is working directory now\n", path );
	return true;
}
