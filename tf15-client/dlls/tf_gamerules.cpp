#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "teamplay_gamerules.h"
#include "tf_gamerules.h"

#include "skill.h"
#include "game.h"
#include "items.h"
#ifndef NO_VOICEGAMEMGR
#include "voice_gamemgr.h"
#endif
#include "tf_defs.h"

#define MAX_INTERMISSION_TIME 120

extern int gmsgViewMode;
extern int gmsgTeamInfo;
extern int gmsgGameMode;

extern float g_flIntermissionStartTime;

extern cvar_t mp_chattime;

extern DLL_GLOBAL BOOL g_fGameOver;

CTeamFortress::CTeamFortress( void )
{
	CHalfLifeTeamplay::CHalfLifeMultiplay();
#ifndef NO_VOICEGAMEMGR
	m_VoiceGameMgr.Init( &g_GameMgrHelper, gpGlobals->maxClients );
#endif
}

void CTeamFortress::Think( void )
{
#ifndef NO_VOICEGAMEMGR
	m_VoiceGameMgr->Update( gpGlobals->frametime );
#endif
	CHalfLifeMultiplay::Think();
}

BOOL CTeamFortress::IsTeamplay( void )
{
	return gpGlobals->teamplay;
}

void CTeamFortress::PlayerSpawn( CBasePlayer *pPlayer )
{
	pPlayer->pev->weapons |= ( 1 << WEAPON_SUIT );

	// TFC-6 Phase 1: apply the class stat block + loadout (or hold the player
	// in the no-class wait state).
	TeamFortress_PlayerSpawn( pPlayer );
}

BOOL CTeamFortress::ClientCommand( CBasePlayer *pPlayer, const char *pcmd )
{
#ifndef NO_VOICEGAMEMGR
	if ( m_VoiceGameMgr.ClientCommand( pPlayer, pcmd ) )
		return TRUE;
#endif

	// TFC-6 Phase 1: jointeam / class-name / _special from the VGUI menus.
	if ( TeamFortress_ClientCommand( pPlayer, pcmd ) )
		return TRUE;

	return CHalfLifeTeamplay::ClientCommand( pPlayer, pcmd );
}

void CTeamFortress::ClientUserInfoChanged( CBasePlayer *pPlayer, char *infobuffer )
{
	char *s;

	pPlayer->TeamFortress_SetSkin();

	pPlayer->exec_scripts = 0;

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "ec" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->exec_scripts = 1;
	}

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "exec_class" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->exec_scripts = 1;
	}

	pPlayer->exec_map_scripts = 0;

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "em" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->exec_map_scripts = 1;
	}

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "exec_map" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->exec_map_scripts = 1;
	}

	pPlayer->display_class_briefing = 0;
	pPlayer->take_screenshots = 0;

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "take_sshot" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->take_screenshots = 1;
	}

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "ts" );
	if ( s && s[0] && ( !strcmp( s, "1" ) || !strcmp( s, "on" ) ) )
	{
		pPlayer->take_screenshots = 1;
	}

	pPlayer->local_blood = 1;

	s = g_engfuncs.pfnInfoKeyValue( infobuffer, "cl_lb" );
	if ( s && s[0] )
	{
		pPlayer->local_blood = atoi( s ) != 0;
	}
}

int CTeamFortress::DeadPlayerWeapons( CBasePlayer *pPlayer )
{
	return GR_PLR_DROP_GUN_NO;
}

int CTeamFortress::DeadPlayerAmmo( CBasePlayer *pPlayer )
{
	return GR_PLR_DROP_AMMO_ALL;
}

float CTeamFortress::FlItemRespawnTime( CItem *pItem )
{
	return gpGlobals->time + pItem->m_flRespawnTime;
}

BOOL CTeamFortress::CanHaveItem( CBasePlayer *pPlayer, CItem *pItem )
{
	if ( !pPlayer->is_feigning && !( pPlayer->tfstate & ( TFSTATE_CANT_MOVE | TFSTATE_AIMING ) ) && cb_prematch_time <= gpGlobals->time )
	{
		// TFC-6 Phase 1: was `return ActivationSucceeded(...)`, but that helper
		// is an unfinished stub that always returns FALSE, which blocked every
		// item_healthkit / item_battery / ammo pickup. Fall back to the base
		// multiplayer rule (allow it). Real goal-item activation is a later phase.
		return CHalfLifeMultiplay::CanHaveItem( pPlayer, pItem );
	}

	return FALSE;
}

const char *CTeamFortress::GetGameDescription( void )
{
	return "TF Classic";
}

const char *CTeamFortress::GetTeamID( CBaseEntity *pEntity )
{
	return GetTeamName( pEntity->team_no );
}

void CTeamFortress::InitHUD( CBasePlayer *pl )
{
	TeamFortress_SetupDefaultTeams();

	// TFC-6 Phase 1: skip CHalfLifeTeamplay::InitHUD -- it derived a team from
	// the "model" userinfo (a class name in TFC) and sent an empty gmsgTeamNames
	// that left the client with zero selectable teams. Go straight to the
	// multiplayer base (MOTD, scoreboard, game mode).
	CHalfLifeMultiplay::InitHUD( pl );

	MESSAGE_BEGIN( MSG_ONE, gmsgGameMode, NULL, pl->edict() );
		WRITE_BYTE( 1 );  // teamplay
	MESSAGE_END();

	UTIL_LogPrintf( "\"%s<%i><%s><>\" entered the game\n",
	                STRING( pl->pev->netname ),
	                GETPLAYERUSERID( pl->edict() ),
	                GETPLAYERAUTHID( pl->edict() ) );

	UTIL_LogPrintf( "\"%s<%i><%s><>\" joined team \"SPECTATOR\"\n",
	                STRING( pl->pev->netname ),
	                GETPLAYERUSERID( pl->edict() ),
	                GETPLAYERAUTHID( pl->edict() ) );

	pl->TeamFortress_ExecMapScript();

	//pl->m_bDisplayedMOTD = 0;

	if ( CVAR_GET_FLOAT( "sv_cheats" ) == 0.0f )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgViewMode, NULL, pl->edict() );
		MESSAGE_END();
	}

	for ( int i = 2; i <= (int)number_of_teams; i++ )
		TeamFortress_TeamIncreaseScore( i, 0 );

	// Bring the new client up to date on everyone else's team.
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *plr = UTIL_PlayerByIndex( i );
		if ( plr && plr != pl && ( (CBasePlayer *)plr )->team_no >= 1 )
		{
			MESSAGE_BEGIN( MSG_ONE, gmsgTeamInfo, NULL, pl->edict() );
				WRITE_BYTE( plr->entindex() );
				WRITE_STRING( GetTeamName( ( (CBasePlayer *)plr )->team_no ) );
			MESSAGE_END();
		}
	}

	TeamFortress_SendTeamMenu( pl );
}

BOOL CTeamFortress::ClientConnected( edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128] )
{
#ifndef NO_VOICEGAMEMGR
	m_VoiceGameMgr.ClientConnected( pEntity );
#endif

	if ( !g_bFirstClient )
	{
		g_bFirstClient = TRUE;
		ParseTFServerSettings();
	}

	MESSAGE_BEGIN( MSG_ALL, gmsgTeamInfo );
	WRITE_BYTE( ENTINDEX( pEntity ) );
	WRITE_STRING( "" );
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_ONE, SVC_STUFFTEXT, NULL, pEntity );
	WRITE_STRING( UTIL_VarArgs( "cl_forwardspeed %d\ncl_backspeed %d\ncl_sidespeed %d\n", 400, 400, 400 ) );
	MESSAGE_END();

	return CHalfLifeMultiplay::ClientConnected( pEntity, pszName, pszAddress, szRejectReason );
}

void CTeamFortress::GoToIntermission( void )
{
	if ( g_fGameOver )
		return;

	MESSAGE_BEGIN( MSG_ALL, SVC_INTERMISSION );
	MESSAGE_END();

	DumpClanScores();
	clan_scores_dumped = 1.0f;

	// bounds check
	int time = (int)CVAR_GET_FLOAT( "mp_chattime" );
	if ( time < 1 )
		CVAR_SET_STRING( "mp_chattime", "1" );
	else if ( time > MAX_INTERMISSION_TIME )
		CVAR_SET_STRING( "mp_chattime", UTIL_dtos1( MAX_INTERMISSION_TIME ) );

	m_flIntermissionEndTime = gpGlobals->time + ( (int)mp_chattime.value );
	g_flIntermissionStartTime = gpGlobals->time;

	g_fGameOver = TRUE;
	m_iEndIntermissionButtonHit = FALSE;
}

//=========================================================
// TFC-6 Phase 1 -- real team model. The generic CHalfLifeTeamplay versions
// key off the "model" userinfo string, which in TFC is the player's class,
// not their team.
//=========================================================
const char *CTeamFortress::SetDefaultPlayerTeam( CBasePlayer *pPlayer )
{
	if ( pPlayer->team_no >= 1 )
	{
		strncpy( pPlayer->m_szTeamName, GetTeamName( pPlayer->team_no ), TEAM_NAME_LENGTH );
		pPlayer->m_szTeamName[TEAM_NAME_LENGTH - 1] = '\0';
	}
	else
	{
		pPlayer->m_szTeamName[0] = '\0';
	}

	return pPlayer->m_szTeamName;
}

int CTeamFortress::GetTeamIndex( const char *pTeamName )
{
	if ( pTeamName && *pTeamName )
	{
		for ( int i = 1; i <= (int)number_of_teams; i++ )
		{
			if ( !stricmp( pTeamName, GetTeamName( i ) ) )
				return i - 1;   // callers add 1 back
		}
	}

	return -1;
}

const char *CTeamFortress::GetIndexedTeamName( int teamIndex )
{
	if ( teamIndex >= 0 && teamIndex < (int)number_of_teams )
		return GetTeamName( teamIndex + 1 );

	return "";
}

BOOL CTeamFortress::IsValidTeam( const char *pTeamName )
{
	return GetTeamIndex( pTeamName ) != -1;
}

edict_t *CTeamFortress::GetPlayerSpawnSpot( CBasePlayer *pPlayer )
{
	edict_t *pSpot = TeamFortress_SelectTeamSpawnPoint( pPlayer );

	if ( pSpot )
	{
		pPlayer->pev->origin     = VARS( pSpot )->origin + Vector( 0, 0, 1 );
		pPlayer->pev->v_angle    = g_vecZero;
		pPlayer->pev->velocity   = g_vecZero;
		pPlayer->pev->angles     = VARS( pSpot )->angles;
		pPlayer->pev->punchangle = g_vecZero;
		pPlayer->pev->fixangle   = TRUE;
		return pSpot;
	}

	// No matching info_player_teamspawn -- fall back to DM/start spawns.
	return CHalfLifeMultiplay::GetPlayerSpawnSpot( pPlayer );
}

void CTeamFortress::PlayerThink( CBasePlayer *pPlayer )
{
	CHalfLifeMultiplay::PlayerThink( pPlayer );

	// Runs in PreThink, before UpdateClientData -> SendAmmoUpdate.
	TeamFortress_SyncAmmo( pPlayer );
	TeamFortress_GrenadeThink( pPlayer );
}