/*
filesystem.h - engine FS
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

#ifndef FILESYSTEM_INTERNAL_H
#define FILESYSTEM_INTERNAL_H

#include <stdlib.h>
#include "xash3d_types.h"
#include "filesystem.h"
#include "miniz.h"

#if XASH_ANDROID
#include <android/asset_manager.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct searchpath_s searchpath_t;
typedef struct dir_s dir_t;
typedef struct zip_s zip_t;
typedef struct pack_s pack_t;
typedef struct wfile_s wfile_t;
typedef struct android_assets_s android_assets_t;

#if XASH_PS3
// LV2 file syscalls are latency-bound, not bandwidth-bound: what a read costs
// is dominated by the call, not by the bytes. The 2 KB default was tuned for
// hosts where that is not true, and it makes every sequential parse (script
// and .res text via FS_Gets, pak/wad directory walks, small wad lumps) pay a
// syscall per 2 KB.
//
// Two effects, both wanted here. FS_Read sends anything larger than half the
// buffer straight to read() (io.c), so whole-file loads via FS_LoadFile are
// untouched -- this only changes the many-small-reads path. And FS_Seek skips
// the syscall entirely when the target is already inside the buffered window
// (io.c:541), so a wider window converts wad lump seek+read pairs into hits
// rather than costing extra reads.
//
// file_t is only ever heap-allocated (one site, sys.c) and few are open at
// once, so the ~30 KB growth per handle is not a concern against the ~190 MB
// budget. Do not raise this without re-checking that -- it is a fixed array
// inside the struct.
#define FILE_BUFF_SIZE (32768)
#else
#define FILE_BUFF_SIZE (2048)
#endif
#define FILE_DEFLATED BIT( 0 )

typedef struct ztoolkit_s
{
	z_stream zstream;
	size_t   comp_length;
	size_t   in_ind, in_len;
	size_t   in_position;
	byte     input[FILE_BUFF_SIZE];
} ztoolkit_t;

struct file_s
{
	int          handle;      // file descriptor
	int          ungetc;      // single stored character from ungetc, cleared to EOF when read
	time_t       filetime;    // pak, wad or real filetime
	searchpath_t *searchpath;
	fs_offset_t  real_length; // uncompressed file size (for files opened in "read" mode)
	fs_offset_t  position;    // current position in the file
	fs_offset_t  offset;      // offset into the package (0 if external file)
	uint32_t     flags;
	ztoolkit_t   *ztk; // if not NULL, all read functions must go through decompression

	// contents buffer
	fs_offset_t buff_ind; // buffer current index
	fs_offset_t buff_len; // buffer current length
	byte		buff[FILE_BUFF_SIZE]; // intermediate buffer

#ifdef XASH_REDUCE_FD
	const char *backup_path;
	fs_offset_t backup_position;
	uint backup_options;
#endif
};

typedef enum searchpathtype_e
{
	SEARCHPATH_PLAIN = 0,
	SEARCHPATH_PAK,
	SEARCHPATH_WAD,
	SEARCHPATH_ZIP,
	SEARCHPATH_PK3DIR, // it's actually a plain directory but it must behave like a ZIP archive,
	SEARCHPATH_ANDROID_ASSETS
} searchpathtype_t;

// what a directory entry is, when the lister happened to know. readdir() reports
// this in d_type for free, which lets the dir cache answer "is this a file?"
// without a stat(). Anything the lister can't vouch for stays FS_ENT_UNKNOWN and
// is resolved the old way -- symlinks included, since d_type doesn't follow them.
enum
{
	FS_ENT_UNKNOWN = 0, // must stat() to find out
	FS_ENT_FILE,
	FS_ENT_DIR,
};

typedef struct stringlist_s
{
	// maxstrings changes as needed, causing reallocation of strings[] array
	int		maxstrings;
	int		numstrings;
	char		**strings;
	byte		*types; // FS_ENT_*, parallel to strings[]
} stringlist_t;

typedef struct searchpath_s
{
	string           filename;
	searchpathtype_t type;
	int              flags;

	union
	{
		dir_t   *dir;
		pack_t  *pack;
		wfile_t *wad;
		zip_t   *zip;
		android_assets_t *assets;
	};

	struct searchpath_s *next;

	void    (*pfnPrintInfo)( struct searchpath_s *search, char *dst, size_t size );
	void    (*pfnClose)( struct searchpath_s *search );
	file_t *(*pfnOpenFile)( struct searchpath_s *search, const char *filename, const char *mode, int pack_ind );
	int     (*pfnFileTime)( struct searchpath_s *search, const char *filename );
	int     (*pfnFindFile)( struct searchpath_s *search, const char *path, char *fixedname, size_t len );
	void    (*pfnSearch)( struct searchpath_s *search, stringlist_t *list, const char *pattern, int caseinsensitive );
	byte   *(*pfnLoadFile)( struct searchpath_s *search, const char *path, int pack_ind, fs_offset_t *filesize, void *( *pfnAlloc )( size_t ), void ( *pfnFree )( void * ));
} searchpath_t;

typedef searchpath_t *(*FS_ADDARCHIVE_FULLPATH)( const char *path, int flags );

extern char fs_rootdir[MAX_SYSPATH], fs_basedir[MAX_SYSPATH], fs_rodir[MAX_SYSPATH];
extern fs_globals_t FI;
extern searchpath_t *fs_writepath, *fs_searchpaths;
extern poolhandle_t fs_mempool;
extern fs_interface_t g_engfuncs;
extern const fs_api_t g_api;

#define GI FI.GameInfo

#define Mem_Malloc( pool, size ) _Mem_Alloc( pool, size, false, __FILE__, __LINE__ )
#define Mem_Calloc( pool, size ) _Mem_Alloc( pool, size, true, __FILE__, __LINE__ )
#define Mem_Realloc( pool, ptr, size ) g_engfuncs._Mem_Realloc( pool, ptr, size, true, __FILE__, __LINE__ )
#define Mem_Free( mem ) _Mem_Free( mem, __FILE__, __LINE__ )
#define Mem_AllocPool( name ) g_engfuncs._Mem_AllocPool( name, 0, __FILE__, __LINE__ )
#define Mem_AllocPoolExt( name, flags ) g_engfuncs._Mem_AllocPool( name, flags, __FILE__, __LINE__ )
#define Mem_FreePool( pool ) g_engfuncs._Mem_FreePool( pool, __FILE__, __LINE__ )

#define Con_Printf  (*g_engfuncs._Con_Printf)
#define Con_DPrintf (*g_engfuncs._Con_DPrintf)
#define Con_Reportf (*g_engfuncs._Con_Reportf)
#define Sys_Error   (*g_engfuncs._Sys_Error)
#define Sys_GetNativeObject (*g_engfuncs._Sys_GetNativeObject)

//
// filesystem.c
//
void FS_InitMemory( void );
void _Mem_Free( void *data, const char *filename, int fileline );
void *_Mem_Alloc( poolhandle_t poolptr, size_t size, qboolean clear, const char *filename, int fileline )
	ALLOC_CHECK( 2 ) MALLOC_LIKE( _Mem_Free, 1 ) WARN_UNUSED_RESULT;
void FS_EnsureOpenFile( file_t *file );
void FS_BackupFileName( file_t *file, const char *path, uint options );

//
// searchpath.c
//
searchpath_t *FS_MountArchive_Fullpath( const char *file, int flags );
void FS_AddGameDirectory( const char *dir, uint flags );
void FS_ClearSearchPath( void );
int FS_CheckNastyPath( const char *path );
void FS_AddGameHierarchy( const char *dir, uint flags );
void FS_Rescan( uint32_t flags, const char *language );
const char *FS_Gamedir( void );
void FS_LoadGameInfo( uint32_t flags, const char *language );
qboolean FS_InitStdio( qboolean caseinsensitive, const char *rootdir, const char *basedir, const char *gamedir, const char *rodir );
void FS_AllowDirectPaths( qboolean enable );
void FS_ShutdownStdio( void );
void FS_Path_f( void );
void FS_FindFile_f( const char *filename );
searchpath_t *FS_FindFile( const char *name, int *index, char *fixedname, size_t len, uint32_t flags );
qboolean FS_FindLibrary( const char *dllname, qboolean directpath, fs_dllinfo_t *dllInfo );
qboolean FS_FullPathToRelativePath( char *dst, const char *src, size_t size );
search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly ) MALLOC_LIKE( _Mem_Free, 1 ) WARN_UNUSED_RESULT;
qboolean FS_IsArchiveExtensionSupported( const char *ext, uint flags );
searchpath_t *FS_GetArchiveByName( const char *name, searchpath_t *prev );
int FS_FindFileInArchive( searchpath_t *sp, const char *path, char *truepath, size_t len );
file_t *FS_OpenFileFromArchive( searchpath_t *sp, const char *path, const char *mode, int pack_ind );

//
// gameinfo.c
//
void FS_MakeGameInfo( void );
qboolean FS_ParseGameInfo( const char *gamedir, gameinfo_t *GameInfo, qboolean rodir );

//
// sys.c
//
void stringlistinit( stringlist_t *list );
void stringlistfreecontents( stringlist_t *list );
void stringlistappend( stringlist_t *list, const char *text );
void stringlistappendtyped( stringlist_t *list, const char *text, byte type );
void stringlistsort( stringlist_t *list );
void listdirectory( stringlist_t *list, const char *path, qboolean dirs_only );
void FS_CreatePath( char *path );
#if XASH_PS3
const char *PS3_ResolvePath( const char *in, char *out, size_t outsize );

// FS_LoadFile lookup profiler (goal 14). Locating a file costs 100-350ms on
// this platform and the cost repeats per name across boots, so the question is
// *which* LV2 primitive is being called and how often -- not disk throughput.
// Counters are free-running; FS_FindFile snapshots them around each lookup.
typedef struct fs_prof_s
{
	uint32_t listdir_calls;   // listdirectory() invocations
	uint32_t listdir_entries; // readdir() results consumed across all of them
	uint32_t stat_calls;      // FS_SysFile*Exists / FS_SysFileTime
	uint32_t open_calls;      // FS_SysOpen
	uint32_t populate_calls;  // dir.c: first-time scan of a directory
	uint32_t rescan_calls;    // dir.c: miss-path rescan of an already-known dir
	uint32_t refresh_calls;   // dir.c: re-listing of a directory we wrote to
	// readdir()'s d_type across every entry we consume. If LV2 fills this in,
	// the cache can record file-vs-directory at listing time and FS_FindFile_DIR
	// can stop paying a stat() per searchpath per lookup -- now the dominant FS
	// cost. A single DT_UNKNOWN anywhere sinks that; count them all, not one dir.
	uint32_t dtype_reg;
	uint32_t dtype_dir;
	uint32_t dtype_unknown;
	uint32_t dtype_other;
	double   listdir_msec;
	double   stat_msec;
	double   open_msec;
} fs_prof_t;

extern fs_prof_t fs_prof;
double FS_Prof_Time( void );
#endif
int FS_SysFileTime( const char *filename );
file_t *FS_SysOpen( const char *filepath, const char *mode );
file_t *FS_OpenHandle( searchpath_t *search, int handle, fs_offset_t offset, fs_offset_t len );
qboolean FS_SysFileExists( const char *path );
qboolean FS_SysFolderExists( const char *path );
qboolean FS_SysFileOrFolderExists( const char *path );
int FS_SetCurrentDirectory( const char *path );

//
// io.c
//
file_t *FS_OpenReadFile( const char *filename, const char *mode, qboolean gamedironly );
int FS_Close( file_t *file );
file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly ) MALLOC_LIKE( FS_Close, 1 ) WARN_UNUSED_RESULT;
int FS_Flush( file_t *file );
fs_offset_t FS_Write( file_t *file, const void *data, size_t datasize );
fs_offset_t FS_Read( file_t *file, void *buffer, size_t buffersize );
int FS_Print( file_t *file, const char *msg );
int FS_Printf( file_t *file, const char *format, ... ) FORMAT_CHECK( 2 );
int FS_VPrintf( file_t *file, const char *format, va_list ap );
int FS_Getc( file_t *file );
int FS_UnGetc( file_t *file, char c );
int FS_Gets( file_t *file, char *string, size_t bufsize );
int FS_Seek( file_t *file, fs_offset_t offset, int whence );
fs_offset_t FS_Tell( const file_t *file );
qboolean FS_Eof( const file_t *file );
byte *FS_LoadFileFromArchive( searchpath_t *sp, const char *path, int pack_ind, fs_offset_t *filesizeptr, const qboolean sys_malloc );
byte *FS_LoadFileMalloc( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly ) MALLOC_LIKE( free, 1 ) WARN_UNUSED_RESULT;
byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly ) MALLOC_LIKE( _Mem_Free, 1 ) WARN_UNUSED_RESULT;
qboolean CRC32_File( dword *crcvalue, const char *filename );
qboolean MD5_HashFile( byte digest[16], const char *pszFileName, uint seed[4] );
byte *FS_LoadDirectFile( const char *path, fs_offset_t *filesizeptr ) MALLOC_LIKE( _Mem_Free, 1 ) WARN_UNUSED_RESULT;
qboolean FS_WriteFile( const char *filename, const void *data, fs_offset_t len );
int FS_FileExists( const char *filename, int gamedironly );
const char *FS_GetDiskPath( const char *name, qboolean gamedironly );
qboolean FS_GetFullDiskPath( char *buffer, size_t size, const char *name, qboolean gamedironly );
fs_offset_t FS_FileSize( const char *filename, qboolean gamedironly );
fs_offset_t FS_FileLength( const file_t *f );
int FS_FileTime( const char *filename, qboolean gamedironly );
qboolean FS_Rename( const char *oldname, const char *newname );
qboolean FS_Delete( const char *path );
qboolean FS_FileCopy( file_t *pOutput, file_t *pInput, int fileSize );

//
// pak.c
//
qboolean FS_CheckForQuakePak( const char *pakfile, const char *files[], size_t num_files );
searchpath_t *FS_AddPak_Fullpath( const char *pakfile, int flags );

//
// wad.c
//
searchpath_t *FS_AddWad_Fullpath( const char *wadfile, int flags );

//
// zip.c
//
searchpath_t *FS_AddZip_Fullpath( const char *zipfile, int flags );

//
// dir.c
//
searchpath_t *FS_AddDir_Fullpath( const char *path, int flags );
qboolean FS_FixFileCase( dir_t *dir, const char *path, char *dst, const size_t len, qboolean createpath );
void FS_InvalidateDirCache( dir_t *dir, const char *path );
void FS_InitDirectorySearchpath( searchpath_t *search, const char *path, int flags );

//
// android.c
//
void FS_InitAndroid( void );
searchpath_t *FS_AddAndroidAssets_Fullpath( const char *path, int flags );

#ifdef __cplusplus
}
#endif

#endif // FILESYSTEM_INTERNAL_H
