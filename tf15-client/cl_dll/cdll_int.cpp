/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
//
//  cdll_int.c
//
// this implementation handles the linking of the engine to the DLL
//

#ifdef _WIN32
#define HSPRITE HSPRITE_win32
#include "windows.h"
#undef HSPRITE
#else
#define MAX_PATH PATH_MAX
#include <limits.h>
#endif

#include "hud.h"
#include "cl_util.h"
#include "netadr.h"
#include "interface.h"

extern "C"
{
#include "pm_shared.h"
}

#include <string.h>
#include "hud_servers.h"

// vgui_int.h/vgui_TeamFortressViewport.h (and every VGui_Startup/Scheme_Init/
// GetClientVoiceMgr call site below) are gated the same way hlsdk-portable's
// own cdll_int.cpp already gates them -- USE_VGUI is never defined in this
// project (TFC-5's job), and this tree's copy of cdll_int.cpp was simply
// missing the guard hlsdk-portable's otherwise-identical file already has.
// Real "No such file" build errors confirmed vgui_int.h/
// vgui_TeamFortressViewport.h transitively need real VGUI1 headers not
// vendored here.
#if USE_VGUI
#include "vgui_int.h"
#include "vgui_TeamFortressViewport.h"
#endif

cl_enginefunc_t gEngfuncs;
CHud gHUD;
#if USE_VGUI
TeamFortressViewport *gViewPort = NULL;
#endif

mobile_engfuncs_t *gMobileEngfuncs = NULL;

#ifdef USE_PARTICLEMAN
#include "particleman.h"

CSysModule *g_hParticleManModule = NULL;
IParticleMan *g_pParticleMan = NULL;

void CL_LoadParticleMan( void );
void CL_UnloadParticleMan( void );
#endif

#ifdef TF15CLIENT_ADDITIONS
// IGameMenuExports.h pulls in mainui_cpp's own FontRenderer.h (real fatal
// "No such file" build error, confirmed) -- mainui_cpp isn't vendored in
// this project yet (that's TFC-4's job). g_hMainUIModule/g_pMainUI are only
// ever used inside CL_LoadMainUI/CL_UnloadMainUI below, so the whole block
// stays behind the same guard those already use.
#include "IGameMenuExports.h"
CSysModule *g_hMainUIModule = NULL;
IGameMenuExports *g_pMainUI = NULL;

void CL_LoadMainUI( void );
void CL_UnloadMainUI( void );
#endif // TF15CLIENT_ADDITIONS

void InitInput( void );
void EV_HookEvents( void );
void IN_Commands( void );

/*
========================== 
    Initialize

Called when the DLL is first loaded.
==========================
*/
extern "C"
{
	int DLLEXPORT Initialize( cl_enginefunc_t *pEnginefuncs, int iVersion );
	int DLLEXPORT HUD_VidInit( void );
	void DLLEXPORT HUD_Init( void );
	int DLLEXPORT HUD_Redraw( float flTime, int intermission );
	int DLLEXPORT HUD_UpdateClientData( client_data_t *cdata, float flTime );
	void DLLEXPORT HUD_Reset( void );
	void DLLEXPORT HUD_PlayerMove( struct playermove_s *ppmove, int server );
	void DLLEXPORT HUD_PlayerMoveInit( struct playermove_s *ppmove );
	char DLLEXPORT HUD_PlayerMoveTexture( char *name );
	int DLLEXPORT HUD_ConnectionlessPacket( const struct netadr_s *net_from, const char *args, char *response_buffer, int *response_buffer_size );
	int DLLEXPORT HUD_GetHullBounds( int hullnumber, float *mins, float *maxs );
	void DLLEXPORT HUD_Frame( double time );
	void DLLEXPORT HUD_VoiceStatus( int entindex, qboolean bTalking );
	void DLLEXPORT HUD_DirectorMessage( int iSize, void *pbuf );
	void DLLEXPORT HUD_MobilityInterface( mobile_engfuncs_t *gpMobileEngfuncs );
}

/*
================================
HUD_GetHullBounds

  Engine calls this to enumerate player collision hulls, for prediction.  Return 0 if the hullnumber doesn't exist.
================================
*/
int DLLEXPORT HUD_GetHullBounds( int hullnumber, float *mins, float *maxs )
{
	int iret = 0;

	switch ( hullnumber )
	{
	case 0: // Normal player
		Vector( -16, -16, -36 ).CopyToArray( mins );
		Vector( 16, 16, 36 ).CopyToArray( maxs );
		iret = 1;
		break;
	case 1: // Crouched player
		Vector( -16, -16, -18 ).CopyToArray( mins );
		Vector( 16, 16, 18 ).CopyToArray( maxs );
		iret = 1;
		break;
	case 2: // Point based hull
		Vector( 0, 0, 0 ).CopyToArray( mins );
		Vector( 0, 0, 0 ).CopyToArray( maxs );
		iret = 1;
		break;
	}

	return iret;
}

/*
================================
HUD_ConnectionlessPacket

 Return 1 if the packet is valid.  Set response_buffer_size if you want to send a response packet.  Incoming, it holds the max
  size of the response_buffer, so you must zero it out if you choose not to respond.
================================
*/
int DLLEXPORT HUD_ConnectionlessPacket( const struct netadr_s *net_from, const char *args, char *response_buffer, int *response_buffer_size )
{
	// Parse stuff from args
	// int max_buffer_size = *response_buffer_size;

	// Zero it out since we aren't going to respond.
	// If we wanted to response, we'd write data into response_buffer
	*response_buffer_size = 0;

	// Since we don't listen for anything here, just respond that it's a bogus message
	// If we didn't reject the message, we'd return 1 for success instead.
	return 0;
}

void DLLEXPORT HUD_PlayerMoveInit( struct playermove_s *ppmove )
{
	PM_Init( ppmove );
}

char DLLEXPORT HUD_PlayerMoveTexture( char *name )
{
	return PM_FindTextureType( name );
}

void DLLEXPORT HUD_PlayerMove( struct playermove_s *ppmove, int server )
{
	PM_Move( ppmove, server );
}

int DLLEXPORT Initialize( cl_enginefunc_t *pEnginefuncs, int iVersion )
{
	gEngfuncs = *pEnginefuncs;

	if ( iVersion != CLDLL_INTERFACE_VERSION )
		return 0;

	memcpy( &gEngfuncs, pEnginefuncs, sizeof( cl_enginefunc_t ) );

	EV_HookEvents();
#ifdef USE_PARTICLEMAN
	CL_LoadParticleMan();
#endif

#ifdef TF15CLIENT_ADDITIONS
	CL_LoadMainUI();
#endif // TF15CLIENT_ADDITIONS

	return 1;
}

/*
=================
HUD_GetRect

VGui stub
=================
*/
int *HUD_GetRect( void )
{
	static int extent[4];

	extent[0] = gEngfuncs.GetWindowCenterX() - ScreenWidth / 2;
	extent[1] = gEngfuncs.GetWindowCenterY() - ScreenHeight / 2;
	extent[2] = gEngfuncs.GetWindowCenterX() + ScreenWidth / 2;
	extent[3] = gEngfuncs.GetWindowCenterY() + ScreenHeight / 2;

	return extent;
}

/*
==========================
	HUD_VidInit

Called when the game initializes
and whenever the vid_mode is changed
so the HUD can reinitialize itself.
==========================
*/

int DLLEXPORT HUD_VidInit( void )
{
	gHUD.VidInit();

#if USE_VGUI
	VGui_Startup();
#endif

	return 1;
}

/*
==========================
	HUD_Init

Called whenever the client connects
to a server.  Reinitializes all 
the hud variables.
==========================
*/

void DLLEXPORT HUD_Init( void )
{
	InitInput();
	gHUD.Init();
#if USE_VGUI
	Scheme_Init();
#endif
}

/*
==========================
	HUD_Redraw

called every screen frame to
redraw the HUD.
===========================
*/

int DLLEXPORT HUD_Redraw( float time, int intermission )
{
	gHUD.Redraw( time, intermission );

	return 1;
}

/*
==========================
	HUD_UpdateClientData

called every time shared client
dll/engine data gets changed,
and gives the cdll a chance
to modify the data.

returns 1 if anything has been changed, 0 otherwise.
==========================
*/

int DLLEXPORT HUD_UpdateClientData( client_data_t *pcldata, float flTime )
{
	IN_Commands();

	return gHUD.UpdateClientData( pcldata, flTime );
}

/*
==========================
	HUD_Reset

Called at start and end of demos to restore to "non"HUD state.
==========================
*/

void DLLEXPORT HUD_Reset( void )
{
	gHUD.VidInit();
}

/*
==========================
HUD_Frame

Called by engine every frame that client .dll is loaded
==========================
*/

void DLLEXPORT HUD_Frame( double time )
{
	// ServersThink( time );

#if USE_VGUI
	GetClientVoiceMgr()->Frame( time );
#endif
}

/*
==========================
HUD_VoiceStatus

Called when a player starts or stops talking.
==========================
*/

void DLLEXPORT HUD_VoiceStatus( int entindex, qboolean bTalking )
{
#if USE_VGUI
	GetClientVoiceMgr()->UpdateSpeakerStatus( entindex, bTalking );
#endif
}

/*
==========================
HUD_DirectorEvent

Called when a director event message was received
==========================
*/

void DLLEXPORT HUD_DirectorMessage( int iSize, void *pbuf )
{
	gHUD.m_Spectator.DirectorMessage( iSize, pbuf );
}

#ifdef USE_PARTICLEMAN
void CL_UnloadParticleMan( void )
{
	Sys_UnloadModule( g_hParticleManModule );

	g_pParticleMan = NULL;
	g_hParticleManModule = NULL;
}

void CL_LoadParticleMan( void )
{
	char szPDir[512];

	if ( gEngfuncs.COM_ExpandFilename( PARTICLEMAN_DLLNAME, szPDir, sizeof( szPDir ) ) == FALSE )
	{
		g_pParticleMan = NULL;
		g_hParticleManModule = NULL;
		return;
	}

	g_hParticleManModule = Sys_LoadModule( szPDir );
	CreateInterfaceFn particleManFactory = Sys_GetFactory( g_hParticleManModule );

	if ( particleManFactory == NULL )
	{
		g_pParticleMan = NULL;
		g_hParticleManModule = NULL;
		return;
	}

	g_pParticleMan = (IParticleMan *)particleManFactory( PARTICLEMAN_INTERFACE, NULL );

	if ( g_pParticleMan )
	{
		g_pParticleMan->SetUp( &gEngfuncs );

		// Add custom particle classes here BEFORE calling anything else or you will die.
		g_pParticleMan->AddCustomParticleClassSize( sizeof( CBaseParticle ) );
	}
}
#endif

void DLLEXPORT HUD_MobilityInterface( mobile_engfuncs_t *gpMobileEngfuncs )
{
	if ( gpMobileEngfuncs->version != MOBILITY_API_VERSION )
		return;

	gMobileEngfuncs = gpMobileEngfuncs;
}

bool HUD_MessageBox( const char *msg )
{
	gEngfuncs.Con_Printf( msg ); // just in case
#ifdef TF15CLIENT_ADDITIONS
	if ( IsXashFWGS() )
	{
		gMobileEngfuncs->pfnSys_Warn( msg );
		return true;
	}
#endif
	// TODO: Load SDL2 and call ShowSimpleMessageBox

	return false;
}
#ifdef TF15CLIENT_ADDITIONS
bool IsXashFWGS()
{
	return gMobileEngfuncs != NULL;
}

void CL_UnloadMainUI( void )
{
	Sys_UnloadModule( g_hMainUIModule );

	g_pMainUI = NULL;
	g_hMainUIModule = NULL;
}

void CL_LoadMainUI( void )
{
	char szDir[MAX_PATH];

#if defined( __ANDROID__ )
	snprintf( szDir, 1024, "%s/%s", getenv( "XASH3D_GAMELIBDIR" ), MAINUI_DLLNAME );
#else
	if ( gEngfuncs.COM_ExpandFilename( MAINUI_DLLNAME, szDir, sizeof( szDir ) ) == false )
	{
		CL_UnloadMainUI();
		return;
	}
#endif

	g_hMainUIModule = Sys_LoadModule( szDir );
	CreateInterfaceFn MainUIFactory = Sys_GetFactory( g_hMainUIModule );

	if ( MainUIFactory == NULL )
	{
		CL_UnloadMainUI();
		return;
	}

	g_pMainUI = (IGameMenuExports *)MainUIFactory( GAMEMENUEXPORTS_INTERFACE_VERSION, NULL );

	if ( g_pMainUI )
	{
		g_pMainUI->Initialize( CreateInterface );
	}
}
#endif // TF15CLIENT_ADDITIONS
#include "cl_dll/IGameClientExports.h"

//-----------------------------------------------------------------------------
// Purpose: Exports functions that are used by the gameUI for UI dialogs
//-----------------------------------------------------------------------------
class CClientExports : public IGameClientExports
{
public:
	// returns the name of the server the user is connected to, if any
	virtual const char *GetServerHostName()
	{
		//if (gViewPortInterface)
		//{
		//	return gViewPortInterface->GetServerName();
		//}
		return "";
	}

	// ingame voice manipulation -- GetClientVoiceMgr() only exists when
	// USE_VGUI is on (see the guard at this file's top); every caller here
	// already treats "no voice manager" as a safe, handled case (the
	// existing null checks below), so gating the whole body just picks that
	// same safe path unconditionally when VGUI/voice isn't built at all.
	virtual bool IsPlayerGameVoiceMuted( int playerIndex )
	{
#if USE_VGUI
		if ( GetClientVoiceMgr() )
			return GetClientVoiceMgr()->IsPlayerBlocked( playerIndex );
#endif
		return false;
	}

	virtual void MutePlayerGameVoice( int playerIndex )
	{
#if USE_VGUI
		if ( GetClientVoiceMgr() )
		{
			GetClientVoiceMgr()->SetPlayerBlockedState( playerIndex, true );
		}
#endif
	}

	virtual void UnmutePlayerGameVoice( int playerIndex )
	{
#if USE_VGUI
		if ( GetClientVoiceMgr() )
		{
			GetClientVoiceMgr()->SetPlayerBlockedState( playerIndex, false );
		}
#endif
	}
};

EXPOSE_SINGLE_INTERFACE( CClientExports, IGameClientExports, GAMECLIENTEXPORTS_INTERFACE_VERSION );