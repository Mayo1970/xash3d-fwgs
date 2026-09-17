#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "game.h"

#include "tf_defs.h"

float g_fNextPrematchAlert;

// Velaron: TODO
void LogMatchResults( int *iNoPlayers, int *iUnaccountedFrags, int bDraw )
{
	
}

// Velaron: TODO
void DumpClanScores( void )
{
	int iUnaccountedFrags[5];
	int iNoPlayers[5];
	CBaseEntity *pPlayer;

	for ( int i = 0; i < 5; i++ )
		iNoPlayers[i] = TeamFortress_TeamGetNoPlayers( i );
	
	for ( int i = 0; i < gpGlobals->maxClients; i++ )
	{
		pPlayer = UTIL_PlayerByIndex( i );

		if ( pPlayer )
			iUnaccountedFrags[pPlayer->team_no] += pPlayer->real_frags;
	}

	iUnaccountedFrags[4] = teamfrags[4] - iUnaccountedFrags[4];
	UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Match_results" );
	LogMatchResults( iNoPlayers, iUnaccountedFrags, 0 ); // sort teams
}

static void TF_MatchBegins( void )
{
	UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_matchbegin" );
	UTIL_LogPrintf( "World triggered \"Match_Begins_Now\"\n" );

	if ( tfc_clanbattle_locked.value != 0.0f )
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_matchlocked" );

	for ( int t = 1; t <= 4; t++ )
	{
		teamscores[t] = 0;
		teamfrags[t] = 0;
	}

	cb_prematch_time = 0;

	// [tfc.so] everyone is killed and their score wiped as the match starts
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( i );
		if ( !pPlayer )
			continue;

		pPlayer->TeamFortress_RemoveTimers();
		pPlayer->TeamFortress_RemoveLiveGrenades();
		pPlayer->TakeDamage( pPlayer->pev, pPlayer->pev, 10000, DMG_IGNOREARMOR | DMG_NEVERGIB );
		pPlayer->pev->frags = 0;
		pPlayer->real_frags = 0;
		pPlayer->m_iDeaths = 0;
	}

	CBaseEntity *pGren = NULL;
	while ( ( pGren = UTIL_FindEntityByClassname( pGren, "grenade" ) ) != NULL )
	{
		pGren->SetThink( &CBaseEntity::SUB_Remove );
		pGren->pev->nextthink = gpGlobals->time;
	}
}

void Display_Prematch( void )
{
	if ( g_fNextPrematchAlert > gpGlobals->time )
		return;

	int iSecs = (int)ceil( cb_prematch_time - gpGlobals->time );

	if ( iSecs % 60 == 0 && iSecs > 119 )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_minsleft", UTIL_dtos1( iSecs / 60 ) );
		g_fNextPrematchAlert = gpGlobals->time + ( iSecs - 60 );
	}
	else if ( iSecs == 60 )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_oneminleft" );
		g_fNextPrematchAlert = gpGlobals->time + 30.0f;
	}
	else if ( iSecs == 30 )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_thirtysecleft" );
		g_fNextPrematchAlert = gpGlobals->time + 20.0f;
	}
	else if ( iSecs >= 2 && iSecs <= 10 )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_secsleft", UTIL_dtos1( iSecs ) );
		g_fNextPrematchAlert = gpGlobals->time + 1.0f;
	}
	else if ( iSecs == 1 )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_onesec" );
		g_fNextPrematchAlert = gpGlobals->time + 1.0f;
	}
	else if ( iSecs <= 0 )
	{
		TF_MatchBegins();
	}
}

static void TF_SetPlayersFrozen( BOOL bFrozen )
{
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)UTIL_PlayerByIndex( i );
		if ( !pPlayer )
			continue;

		pPlayer->pev->iuser4 = bFrozen ? 1 : 0;
		if ( bFrozen )
			pPlayer->tfstate |= TFSTATE_CANT_MOVE;
		else
			pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
		pPlayer->immune_to_check = gpGlobals->time + 10.0f;
		pPlayer->TeamFortress_SetSpeed();
	}
}

static void TF_ResumeFire( void )
{
	if ( !no_cease_fire_text )
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_resumefire" );

	UTIL_LogPrintf( "World triggered \"Resume_Fire\"\n" );
	TF_SetPlayersFrozen( FALSE );
}

void Check_Ceasefire( void )
{
	if ( gpGlobals->time > cb_ceasefire_time && initial_cease_fire )
	{
		if ( cease_fire )
		{
			initial_cease_fire = FALSE;
			cease_fire = FALSE;
		}
	}
	else if ( cease_fire )
	{
		last_cease_fire = cease_fire;
		return;
	}

	TF_ResumeFire();
	last_cease_fire = cease_fire;
}

// "tf_ceasefire" server command: toggles a cease fire.
void Admin_CeaseFire( void )
{
	if ( !cease_fire )
	{
		cease_fire = TRUE;
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Admin_ceasefire" );
		TF_SetPlayersFrozen( TRUE );
		return;
	}

	cease_fire = FALSE;
	UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Game_resumefire" );
	UTIL_LogPrintf( "World triggered \"Resume_Fire\"\n" );
	TF_SetPlayersFrozen( FALSE );
}