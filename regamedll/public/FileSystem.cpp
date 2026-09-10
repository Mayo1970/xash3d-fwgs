/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

#include "precompiled.h"

IFileSystem *g_pFileSystem = nullptr;
CSysModule *g_pFileSystemModule = nullptr;

#ifdef __PPU__
// PSL1GHT has no dlopen, so filesystem_stdio cannot be loaded as a module here
// -- on PS3 it is statically linked into the engine binary and exports
// CreateInterface (filesystem/exports.txt). The engine reaches it through its
// own native-object registry: Sys_GetNativeObject() (engine/common/system.c)
// falls through to FS_GetNativeObject() (engine/common/filesystem_engine.c),
// which calls filesystem_stdio's CreateInterface with the name given. Asking
// for FILESYSTEM_INTERFACE_VERSION therefore yields the very same
// g_VFileSystem009 object a dlopen build would have got.
//
// The two IFileSystem declarations are vtable-compatible: this SDK's
// public/FileSystem.h and the engine's filesystem/VFileSystem009.h are both
// the stock Valve VFileSystem009 layout, member for member in the same order
// (verified through GetLocalPath; FileExists, the only method this tree calls,
// is the 8th virtual after the destructor in both).
extern "C" void *Sys_GetNativeObject(const char *obj);

bool FileSystem_Init()
{
	g_pFileSystem = (IFileSystem *)Sys_GetNativeObject(FILESYSTEM_INTERFACE_VERSION);
	if (!g_pFileSystem)
	{
		Sys_Error("Can not retrive filesystem interface version '" FILESYSTEM_INTERFACE_VERSION "'.");
		return false;
	}

	return true;
}

void FileSystem_Shutdown()
{
	// nothing to unload: the interface belongs to the engine binary
	g_pFileSystem = nullptr;
}
#else
bool FileSystem_Init()
{
	g_pFileSystemModule = Sys_LoadModule(STDIO_FILESYSTEM_LIB);
	if (!g_pFileSystemModule)
	{
		return false;
	}

	// Get FileSystem interface
	CreateInterfaceFn filesystemFactoryFn = Sys_GetFactory(g_pFileSystemModule);
	if (!filesystemFactoryFn)
	{
		Sys_Error("Unable to get filesystem factory.");
	}

	g_pFileSystem = (IFileSystem *)filesystemFactoryFn(FILESYSTEM_INTERFACE_VERSION, nullptr);
	if (!g_pFileSystem)
	{
		Sys_Error("Can not retrive filesystem interface version '" FILESYSTEM_INTERFACE_VERSION "'.");
	}

	return true;
}

void FileSystem_Shutdown()
{
	if (g_pFileSystemModule)
	{
		Sys_UnloadModule(g_pFileSystemModule);
		g_pFileSystemModule = nullptr;
	}
}
#endif // __PPU__
