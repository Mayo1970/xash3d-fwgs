/*
ps3_osk.h - PS3 native on-screen keyboard via sysutil/osk
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
#ifndef PS3_OSK_H
#define PS3_OSK_H

#include "xash3d_types.h"
#include <ppu-types.h>

void PS3_OSK_Init( void );
void PS3_OSK_Shutdown( void );

// opens the OSK (no-op if already open)
void PS3_OSK_Open( void );

// forwards an OSK sysutil status/param pair; called from PS3_SysutilCallback
void PS3_OSK_SysutilCallback( u64 status, u64 param );

qboolean PS3_OSK_IsActive( void );

#endif // PS3_OSK_H
