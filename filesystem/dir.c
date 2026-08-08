/*
dir.c - caseinsensitive directory operations
Copyright (C) 2022 Alibek Omarov, Velaron
Copyright (C) 2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "build.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#if XASH_POSIX
#include <unistd.h>
#if !XASH_PSVITA && !XASH_PS3
#include <sys/ioctl.h>
#endif
#endif
#if XASH_LINUX
#include <linux/fs.h>
#ifndef FS_CASEFOLD_FL // for compatibility with older distros
#define FS_CASEFOLD_FL 0x40000000
#endif // FS_CASEFOLD_FL
#endif // XASH_LINUX

#include "port.h"
#include "filesystem_internal.h"
#include "crtlib.h"
#include "xash3d_mathlib.h"
#include "common/com_strings.h"

enum
{
	DIRENTRY_EMPTY_DIRECTORY = 0, // don't care if it's not directory or it's empty
	DIRENTRY_NOT_SCANNED = -1,
	DIRENTRY_CASEINSENSITIVE = -2, // directory is already caseinsensitive, just copy whatever is left
};

typedef struct dir_s
{
	string name;
	int numentries;
	qboolean listed; // this listing is known to still match the filesystem
	byte type; // FS_ENT_*, as reported by the listing this entry came from
	struct dir_s *entries; // sorted
} dir_t;

// A dir_t marked `listed` has a listing that cannot have gone stale, which
// lets us skip two things the stock code does unconditionally and which cost
// 12s of LV2 syscalls per map load on PS3:
//   - re-stat()ing every path component to confirm a cached entry still exists
//   - re-listdirectory()ing the whole directory whenever a lookup misses
// Both exist to catch files appearing or vanishing behind our back; on an
// install we are the only writer to, that can only happen when we write, and
// FS_InvalidateDirCache() clears the flag on exactly the directories we wrote.
//
// Consoles run against a read-only install that nothing outside this process
// can modify. Desktop keeps the stock defensive rescans, where developers do
// edit assets underneath a running engine.
#if XASH_PS3 || XASH_PSVITA || XASH_NSWITCH
#define FS_TRUST_DIR_CACHE 1
#else
#define FS_TRUST_DIR_CACHE 0
#endif

#if XASH_PS3
// /dev_hdd0 is a Sony-proprietary VFS and its case behaviour is not documented
// for homebrew, so probe it once rather than assume. List a directory, take a
// real entry, flip its case and stat that. If it resolves, the OS is already
// doing the case fixing for us and the whole dir cache below is dead weight --
// exactly the position XASH_PSVITA and XASH_NSWITCH are already in.
// Returns 1 insensitive, 0 sensitive, -1 inconclusive (nothing to flip).
static int PS3_ProbeCaseInsensitive( const char *dir )
{
	stringlist_t list;
	int result = -1;

	stringlistinit( &list );
	listdirectory( &list, dir, false );

	for( int i = 0; i < list.numstrings; i++ )
	{
		char probe[MAX_SYSPATH];
		qboolean flipped = false;
		size_t prefix;

		prefix = Q_strlen( dir );
		if( prefix + Q_strlen( list.strings[i] ) + 1 >= sizeof( probe ))
			continue; // would truncate, and a truncated probe answers nothing

		Q_strncpy( probe, dir, sizeof( probe ));
		Q_strncpy( &probe[prefix], list.strings[i], sizeof( probe ) - prefix );

		for( char *c = &probe[prefix]; *c; c++ )
		{
			char flip = *c >= 'a' && *c <= 'z' ? Q_toupper( *c ) : Q_tolower( *c );

			if( flip != *c )
			{
				*c = flip;
				flipped = true;
			}
		}

		if( !flipped )
			continue; // no letters in this name, it tells us nothing

		result = FS_SysFileOrFolderExists( probe ) ? 1 : 0;
		break;
	}

	stringlistfreecontents( &list );
	return result;
}
#endif // XASH_PS3

static qboolean Platform_GetDirectoryCaseSensitivity( const char *dir )
{
#if XASH_WIN32 || XASH_PSVITA || XASH_NSWITCH
	return false;
#elif XASH_PS3
	static int caseinsensitive = -1;

	if( caseinsensitive < 0 )
	{
		int probed = PS3_ProbeCaseInsensitive( dir );

		if( probed < 0 )
			return true; // inconclusive here, stay case-fixing and retry on the next directory

		caseinsensitive = probed;
		Con_Printf( "%s: filesystem probed as case-%ssensitive\n", __func__, caseinsensitive ? "IN" : "" );
	}

	return !caseinsensitive;
#elif XASH_ANDROID
	// on Android, doing code below causes crash in MediaProviderGoogle.apk!libfuse_jni.so
	// which in turn makes vold (Android's Volume Daemon) to umount /storage/emulated/0
	// and because you can't unmount a filesystem when there is file descriptors open
	// it has no other choice but to terminate and then kill our program
	return true;
#elif XASH_LINUX && defined( FS_IOC_GETFLAGS )
	int flags = 0;
	int fd = open( dir, O_RDONLY | O_NONBLOCK );

	if( fd < 0 )
		return true;

	if( ioctl( fd, FS_IOC_GETFLAGS, &flags ) < 0 )
	{
		close( fd );
		return true;
	}

	close( fd );

	return !FBitSet( flags, FS_CASEFOLD_FL );
#else
	return true;
#endif
}

static int FS_SortDirEntries( const void *_a, const void *_b )
{
	const dir_t *a = _a;
	const dir_t *b = _b;
	return Q_stricmp( a->name, b->name );
}

static void FS_FreeDirEntries( dir_t *dir )
{
	if( dir->entries )
	{
		for( int i = 0; i < dir->numentries; i++ )
			FS_FreeDirEntries( &dir->entries[i] );
		dir->entries = NULL;
	}

	dir->numentries = DIRENTRY_NOT_SCANNED;
	dir->listed = false;
}

static void FS_InitDirEntries( dir_t *dir, const stringlist_t *list )
{
	dir->numentries = list->numstrings;
	dir->listed = true;
	dir->entries = Mem_Malloc( fs_mempool, sizeof( dir_t ) * dir->numentries );

	for( int i = 0; i < list->numstrings; i++ )
	{
		dir_t *entry = &dir->entries[i];

		Q_strncpy( entry->name, list->strings[i], sizeof( entry->name ));
		entry->numentries = DIRENTRY_NOT_SCANNED;
		entry->listed = false; // never listed
		entry->type = list->types ? list->types[i] : FS_ENT_UNKNOWN;
		entry->entries = NULL;
	}

	qsort( dir->entries, dir->numentries, sizeof( dir->entries[0] ), FS_SortDirEntries );
}

static void FS_PopulateDirEntries( dir_t *dir, const char *path )
{
	stringlist_t list;

#if XASH_PS3
	fs_prof.populate_calls++;
#endif

	dir->listed = true;

	if( !FS_SysFolderExists( path ))
	{
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		dir->entries = NULL;
		return;
	}

	if( !Platform_GetDirectoryCaseSensitivity( path ))
	{
		dir->numentries = DIRENTRY_CASEINSENSITIVE;
		dir->entries = NULL;
		return;
	}

	stringlistinit( &list );
	listdirectory( &list, path, false );
	if( !list.numstrings )
	{
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		dir->entries = NULL;
	}
	else
	{
		FS_InitDirEntries( dir, &list );
	}
	stringlistfreecontents( &list );
}

static int FS_FindDirEntry( dir_t *dir, const char *name )
{
	// look for the file (binary search)
	int left = 0;
	int right = dir->numentries - 1;

	while( left <= right )
	{
		int middle = (left + right) / 2;
		int diff = Q_stricmp( dir->entries[middle].name, name );

		// found it
		if( !diff )
			return middle;

		// if we're too far in the list
		if( diff > 0 )
			right = middle - 1;
		else left = middle + 1;
	}
	return -1;
}

static void FS_MergeDirEntries( dir_t *dir, const stringlist_t *list )
{
	dir_t temp;

	// glorified realloc for sorted dir entries
	// make new array and copy old entries with same name and subentries
	// everything else get freed

	FS_InitDirEntries( &temp, list );

	for( int i = 0; i < dir->numentries; i++ )
	{
		dir_t *oldentry = &dir->entries[i];
		dir_t *newentry;
		int j;

		// don't care about directories without subentries
		if( oldentry->entries == NULL )
			continue;

		// try to find this directory in new tree
		j = FS_FindDirEntry( &temp, oldentry->name );

		// not found, free memory
		if( j < 0 )
		{
			FS_FreeDirEntries( oldentry );
			continue;
		}

		// found directory, move all entries
		newentry = &temp.entries[j];

		newentry->numentries = oldentry->numentries;
		newentry->listed = oldentry->listed; // the moved listing is as fresh as it was
		newentry->entries = oldentry->entries;
	}

	// now we can free old tree and replace it with temporary
	// do not add null check there! If we hit it, it's probably a logic error!
	Mem_Free( dir->entries );
	dir->numentries = temp.numentries;
	dir->listed = temp.listed;
	dir->entries = temp.entries;
}

static int FS_MaybeUpdateDirEntries( dir_t *dir, const char *path, const char *entryname )
{
	stringlist_t list;
	int ret;

#if XASH_PS3
	fs_prof.rescan_calls++;
#endif

	stringlistinit( &list );
	listdirectory( &list, path, false );

	// we just observed this directory first-hand, whatever we conclude below
	dir->listed = true;

	if( list.numstrings == 0 ) // empty directory
	{
		FS_FreeDirEntries( dir );
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		ret = -1;
	}
	else if( dir->numentries <= DIRENTRY_EMPTY_DIRECTORY ) // not initialized or was empty
	{
		FS_InitDirEntries( dir, &list );
		ret = FS_FindDirEntry( dir, entryname );
	}
	else if( list.numstrings != dir->numentries ) // quick update
	{
		FS_MergeDirEntries( dir, &list );
		ret = FS_FindDirEntry( dir, entryname );
	}
	else
	{
		// do heavy compare if directory now have an entry we need
		int i;

		for( i = 0; i < list.numstrings; i++ )
		{
			if( !Q_stricmp( list.strings[i], entryname ))
				break;
		}

		if( i != list.numstrings )
		{
			FS_MergeDirEntries( dir, &list );
			ret = FS_FindDirEntry( dir, entryname );
		}
		else ret = -1;
	}

	stringlistfreecontents( &list );
	return ret;
}

/*
=============
FS_RefreshDirEntries

Re-read a directory whose listing predates our last write, keeping any already
scanned subdirectories. Called once per stale directory instead of letting
every subsequent lookup pay a defensive rescan.
=============
*/
static void FS_RefreshDirEntries( dir_t *dir, const char *path )
{
	stringlist_t list;

#if XASH_PS3
	fs_prof.refresh_calls++;
#endif

	stringlistinit( &list );
	listdirectory( &list, path, false );

	if( list.numstrings == 0 )
	{
		FS_FreeDirEntries( dir );
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
	}
	else if( dir->numentries <= DIRENTRY_EMPTY_DIRECTORY )
		FS_InitDirEntries( dir, &list );
	else
		FS_MergeDirEntries( dir, &list );

	dir->listed = true;

	stringlistfreecontents( &list );
}

/*
=============
FS_InvalidateDirCache

Clear the `listed` flag on the directories along path, so the next lookup
re-lists exactly those and nothing else. Walks the cached tree only -- no
syscalls -- which is the whole point: a savegame must not cost a re-listing of
every directory the engine has ever touched.
=============
*/
void FS_InvalidateDirCache( dir_t *dir, const char *path )
{
	if( !dir )
		return;

	dir->listed = false;

	// stop before the last component: that one is the file being written, not
	// a directory whose listing we cache
	for( const char *prev = path, *next = Q_strchrnul( prev, '/' );
		  next[0] != '\0';
		  prev = next + 1, next = Q_strchrnul( prev, '/' ))
	{
		char entryname[MAX_SYSPATH];
		int ret;

		if( dir->numentries <= DIRENTRY_EMPTY_DIRECTORY )
			return; // nothing cached below here, so nothing to invalidate

		Q_strncpy( entryname, prev, next - prev + 1 );

		if(( ret = FS_FindDirEntry( dir, entryname )) < 0 )
			return; // this directory isn't in the cache at all

		dir = &dir->entries[ret];
		dir->listed = false;
	}
}

static inline qboolean FS_AppendToPath( char *dst, size_t *pi, const size_t len, const char *src, const char *path, const char *err )
{
	size_t i = *pi;

	i += Q_strncpy( &dst[i], src, len - i );
	*pi = i;

	if( i >= len )
	{
		Con_Printf( S_ERROR "%s: overflow while appending %s (%s)\n", __func__, path, err );
		return false;
	}
	return true;
}

/*
=============
FS_FixFileCaseNode

As FS_FixFileCase, but also hands back the cache node the path resolved to, so
a caller that wants the directory's listing can reuse the one the cache already
holds instead of calling listdirectory() again. *node is left NULL whenever the
walk couldn't track the path (parent escape, caseinsensitive slam) -- those
callers must fall back to a real listing.
=============
*/
static qboolean FS_FixFileCaseNode( dir_t *dir, const char *path, char *dst, const size_t len, qboolean createpath, dir_t **node )
{
	size_t i = 0;

	if( node )
		*node = NULL;

	if( !FS_AppendToPath( dst, &i, len, dir->name, path, "init" ))
		return false;

	// nothing to fix
	if( COM_StringEmpty( path ))
	{
		if( node && FS_TRUST_DIR_CACHE )
			*node = dir;
		return true;
	}

	// we can't and shouldn't track parent directories to not track the whole filesystem
	// exit early for this case
	// FIXME: track the path to catch other cases
	if( !Q_strncmp( path, "..", 2 ) && ( path[2] == '\0' || path[2] == '/' ))
	{
		if( !FS_AppendToPath( dst, &i, len, path, path, "escape to parent directory" ))
			return false;

		// check file existense
		return createpath ? true : FS_SysFileOrFolderExists( dst );
	}

	// the listing the final component was resolved out of is first-hand, so its
	// recorded FS_ENT_* type can be trusted by the caller. Stays false if we had
	// to fall back on stat()ing our way there, where we learned nothing new
	qboolean trusted = false;

	for( const char *prev = path, *next = Q_strchrnul( prev, '/' );
		  ;
		  prev = next + 1, next = Q_strchrnul( prev, '/' ))
	{
		qboolean uptodate = false; // do not run second scan if we're just updated our directory list
		qboolean current; // nothing has been written since this listing was taken
		size_t temp;
		char entryname[MAX_SYSPATH];
		int ret;

		if( dir->numentries == DIRENTRY_NOT_SCANNED )
		{
			// read directory first time
			FS_PopulateDirEntries( dir, dst );
			uptodate = true;
		}
		else if( FS_TRUST_DIR_CACHE && dir->numentries != DIRENTRY_CASEINSENSITIVE && !dir->listed )
		{
			FS_RefreshDirEntries( dir, dst );
			uptodate = true;
		}

		current = FS_TRUST_DIR_CACHE && dir->listed;

		// this subdirectory is case insensitive, just slam everything that's left
		if( dir->numentries == DIRENTRY_CASEINSENSITIVE )
		{
			if( !FS_AppendToPath( dst, &i, len, prev, path, "caseinsensitive entry" ))
				return false;

			// check file existense
			return createpath ? true : FS_SysFileOrFolderExists( dst );
		}

		// get our entry name
		Q_strncpy( entryname, prev, next - prev + 1 );

		// didn't found, but does it exists in FS?
		if(( ret = FS_FindDirEntry( dir, entryname )) < 0 )
		{
			// if the listing is still current the miss is authoritative -- a
			// rescan would re-read the whole directory only to conclude the
			// same thing, which is the dominant cost of every failed lookup
			// if we're creating files or folders, we don't care if path doesn't exist
			// so copy everything that's left and exit without an error
			if( uptodate || current || ( ret = FS_MaybeUpdateDirEntries( dir, dst, entryname )) < 0 )
				return createpath ? FS_AppendToPath( dst, &i, len, prev, path, "create path" ) : false;

			uptodate = true;
		}

		dir = &dir->entries[ret];
		temp = i;
		if( !FS_AppendToPath( dst, &temp, len, dir->name, path, "case fix" ))
			return false;

		// the entry name came out of a real readdir(); if nothing has been
		// written since, re-stat()ing it can only confirm what we know. This
		// stat runs once per path component per lookup and was the whole cost
		// of an otherwise-cached hit on PS3
		if( !uptodate && !current && !FS_SysFileOrFolderExists( dst )) // file not found, rescan...
		{
			dst[i] = 0; // strip failed part

			// if we're creating files or folders, we don't care if path doesn't exist
			// so copy everything that's left and exit without an error
			if(( ret = FS_MaybeUpdateDirEntries( dir, dst, entryname )) < 0 )
				return createpath ? FS_AppendToPath( dst, &i, len, prev, path, "create path rescan" ) : false;

			dir = &dir->entries[ret];
			if( !FS_AppendToPath( dst, &temp, len, dir->name, path, "case fix rescan" ))
				return false;
		}
		trusted = uptodate || current;
		i = temp;

		// end of string, found file, return
		if( next[0] == '\0' || ( next[0] == '/' && next[1] == '\0' ))
			break;

		if( !FS_AppendToPath( dst, &i, len, "/", path, "path separator" ))
			return false;
	}

	if( node && FS_TRUST_DIR_CACHE && trusted )
		*node = dir;

	return true;
}

qboolean FS_FixFileCase( dir_t *dir, const char *path, char *dst, const size_t len, qboolean createpath )
{
	return FS_FixFileCaseNode( dir, path, dst, len, createpath, NULL );
}

static void FS_Close_DIR( searchpath_t *search )
{
	FS_FreeDirEntries( search->dir );
	Mem_Free( search->dir );
}

static void FS_PrintInfo_DIR( searchpath_t *search, char *dst, size_t size )
{
	Q_strncpy( dst, search->filename, size );
}

static int FS_FindFile_DIR( searchpath_t *search, const char *path, char *fixedname, size_t len )
{
	char netpath[MAX_SYSPATH];
	dir_t *node;

	if( !FS_FixFileCaseNode( search->dir, path, netpath, sizeof( netpath ), false, &node ))
		return -1;

	// the name came out of a readdir() on a listing nothing has written to since,
	// and that readdir() also told us whether it's a file. So the stat() below can
	// only confirm what we already know -- and it ran once per searchpath per
	// lookup, which is what the dir cache left as the dominant FS cost.
	// FS_ENT_UNKNOWN (no d_type, or a symlink) still falls through to the stat.
	if( node && node->type != FS_ENT_UNKNOWN )
	{
		// FS_SysFileExists() is stat + S_ISREG, so a directory is NOT a hit here
		if( node->type != FS_ENT_FILE )
			return -1;

		if( fixedname )
			Q_strncpy( fixedname, netpath + Q_strlen( search->filename ), len );
		return 0;
	}

	if( FS_SysFileExists( netpath ))
	{
		// return fixed case file name only local for that searchpath
		if( fixedname )
			Q_strncpy( fixedname, netpath + Q_strlen( search->filename ), len );
		return 0;
	}

	return -1;
}

static void FS_Search_DIR( searchpath_t *search, stringlist_t *list, const char *pattern, int caseinsensitive )
{
	string netpath, temp;
	stringlist_t dirlist;
	const char *slash = Q_strrchr( pattern, '/' );
	const char *backslash = Q_strrchr( pattern, '\\' );
	const char *colon = Q_strrchr( pattern, ':' );
	const char *separator = Q_max( slash, backslash );
	int basepathlength, dirlistindex, numentries;
	char *basepath;
	dir_t *cached;

	separator = Q_max( separator, colon );

	basepathlength = separator ? (separator + 1 - pattern) : 0;
	basepath = Mem_Calloc( fs_mempool, basepathlength + 1 );
	if( basepathlength ) memcpy( basepath, pattern, basepathlength );
	basepath[basepathlength] = '\0';

	if( !FS_FixFileCaseNode( search->dir, basepath, netpath, sizeof( netpath ), false, &cached ))
	{
		Mem_Free( basepath );
		return;
	}

	// every FS_Search walks every searchpath, so the stock unconditional
	// listdirectory() here re-lists the same directories once per pattern per
	// searchpath -- the listings the dir cache is already holding. Reuse them
	// where the cache is trusted; the names came out of a real readdir() and
	// FS_InvalidateDirCache() marks anything we wrote since as unlisted.
	if( !FS_TRUST_DIR_CACHE )
		cached = NULL;

	if( cached )
	{
		if( cached->numentries == DIRENTRY_NOT_SCANNED )
			FS_PopulateDirEntries( cached, netpath );
		else if( cached->numentries != DIRENTRY_CASEINSENSITIVE && !cached->listed )
			FS_RefreshDirEntries( cached, netpath );

		// DIRENTRY_CASEINSENSITIVE: nothing is cached for this directory
		if( cached->numentries < DIRENTRY_EMPTY_DIRECTORY )
			cached = NULL;
	}

	stringlistinit( &dirlist );

	if( !cached )
		listdirectory( &dirlist, netpath, false );

	numentries = cached ? cached->numentries : dirlist.numstrings;

	Q_strncpy( temp, basepath, sizeof( temp ));

	for( dirlistindex = 0; dirlistindex < numentries; dirlistindex++ )
	{
		const char *entryname = cached ? cached->entries[dirlistindex].name : dirlist.strings[dirlistindex];

		Q_strncpy( &temp[basepathlength], entryname, sizeof( temp ) - basepathlength );

		if( matchpattern( temp, (char *)pattern, true ) )
		{
			int resultlistindex;

			for( resultlistindex = 0; resultlistindex < list->numstrings; resultlistindex++ )
			{
				if( !Q_strcmp( list->strings[resultlistindex], temp ) )
					break;
			}

			if( resultlistindex == list->numstrings )
				stringlistappend( list, temp );
		}
	}

	stringlistfreecontents( &dirlist );

	Mem_Free( basepath );
}

static int FS_FileTime_DIR( searchpath_t *search, const char *filename )
{
	char path[MAX_SYSPATH];

	Q_snprintf( path, sizeof( path ), "%s%s", search->filename, filename );
	return FS_SysFileTime( path );
}

static file_t *FS_OpenFile_DIR( searchpath_t *search, const char *filename, const char *mode, int pack_ind )
{
	char path[MAX_SYSPATH];
	file_t *f;

	Q_snprintf( path, sizeof( path ), "%s%s", search->filename, filename );
	f = FS_SysOpen( path, mode );
	if( !f )
		return NULL;

	f->searchpath = search;

	return f;
}

void FS_InitDirectorySearchpath( searchpath_t *search, const char *path, int flags )
{
	memset( search, 0, sizeof( searchpath_t ));

	Q_strncpy( search->filename, path, sizeof( search->filename ) - 1 );
	COM_PathSlashFix( search->filename );

	if( !Q_stricmp( COM_FileExtension( path ), "pk3dir" ))
		search->type = SEARCHPATH_PK3DIR;
	else search->type = SEARCHPATH_PLAIN;
	search->flags = flags;
	search->pfnPrintInfo = FS_PrintInfo_DIR;
	search->pfnClose = FS_Close_DIR;
	search->pfnOpenFile = FS_OpenFile_DIR;
	search->pfnFileTime = FS_FileTime_DIR;
	search->pfnFindFile = FS_FindFile_DIR;
	search->pfnSearch = FS_Search_DIR;

	// create cache root
	search->dir = Mem_Malloc( fs_mempool, sizeof( dir_t ));
	Q_strncpy( search->dir->name, search->filename, sizeof( search->dir->name ));
	search->dir->type = FS_ENT_DIR; // nothing lists the root, so nothing else would set this
	FS_PopulateDirEntries( search->dir, path );
}

searchpath_t *FS_AddDir_Fullpath( const char *path, int flags )
{
	searchpath_t *search = (searchpath_t *)Mem_Calloc( fs_mempool, sizeof( searchpath_t ));

	FS_InitDirectorySearchpath( search, path, flags );
	Con_Printf( "Adding directory: %s\n", path );

	return search;
}
