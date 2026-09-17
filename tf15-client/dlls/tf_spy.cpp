// TFC-6 Phase 4 -- Spy disguise and feign death.
// Values read from the retail tfc/dlls/tfc.so unless marked.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "tf_defs.h"

extern int gmsgFeignState;
extern int gmsgDeathMsg;

// Seconds the disguise timer runs before the new identity takes effect [tfc.so].
#define TF_DISGUISE_OWNTEAM   4
#define TF_DISGUISE_ENEMY     8
#define TF_FEIGN_RADIUS       64.0f    // CanFeign: nobody else may be this close
#define TF_FEIGN_STANDUP_Z    18.0f    // headroom traced before getting back up
#define TF_SPY_CHECK_GRACE    10.0f    // immune_to_check window after any skin change

// [tfc.so] SpyFakeWeaponArray: third-person weapon the disguise carries,
// indexed by the class being impersonated.
static const char *sSpyFakeWeaponModels[PC_LASTCLASS] =
{
	"",                        // PC_UNDEFINED
	"models/p_nailgun.mdl",    // scout
	"models/p_sniper.mdl",     // sniper
	"models/p_srpg.mdl",       // soldier
	"models/p_glauncher.mdl",  // demoman
	"models/p_snailgun.mdl",   // medic
	"models/p_mini.mdl",       // hwguy
	"models/p_egon.mdl",       // pyro
	"models/p_knife.mdl",      // spy
	"models/p_9mmhandgun.mdl", // engineer
	"",                        // PC_RANDOM
	""                         // PC_CIVILIAN
};

static const char *sTFClassNames[PC_LASTCLASS] =
{
	"Observer", "Scout", "Sniper", "Soldier", "Demoman", "Medic",
	"HWGuy", "Pyro", "Spy", "Engineer", "RandomPC", "Civilian"
};

// Disguise

// Picks a real enemy whose name the HUD can show over the disguised spy.
void CBasePlayer::TeamFortress_SpyCalcName( void )
{
	CBaseEntity *pOld = undercover_target;
	CBaseEntity *pAnyOnTeam = NULL;

	undercover_target = NULL;

	if ( !undercover_team )
		return;

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pPlayer = UTIL_PlayerByIndex( i );
		if ( !pPlayer || pPlayer == pOld || pPlayer == this )
			continue;
		if ( pPlayer->team_no != undercover_team )
			continue;

		if ( !pAnyOnTeam )
			pAnyOnTeam = pPlayer;

		if ( pPlayer->pev->playerclass == undercover_skin )
		{
			undercover_target = pPlayer;
			return;
		}
	}

	undercover_target = pAnyOnTeam;
}

void CBasePlayer::TeamFortress_SpyChangeColor( int iTeamNo )
{
	undercover_team = iTeamNo;
	TeamFortress_SetSkin();
}

void CBasePlayer::Spy_DisguiseExternalWeaponModel( void )
{
	if ( undercover_skin < PC_SCOUT || undercover_skin >= PC_LASTCLASS )
		return;
	if ( !sSpyFakeWeaponModels[undercover_skin][0] )
		return;

	// [tfc.so] the disguise carries the class's usual third-person weapon
	if ( !m_iszSavedWeaponModel )
		m_iszSavedWeaponModel = pev->weaponmodel;

	pev->weaponmodel = MAKE_STRING( sSpyFakeWeaponModels[undercover_skin] );
}

void CBasePlayer::Spy_ResetExternalWeaponModel( void )
{
	if ( m_iszSavedWeaponModel )
	{
		pev->weaponmodel = m_iszSavedWeaponModel;
		m_iszSavedWeaponModel = iStringNull;
	}
}

// A spy cannot start a disguise while glowing, on fire, or holding a goal item.
static BOOL TF_SpyCanDisguise( CBasePlayer *pPlayer )
{
	if ( pPlayer->pev->effects & ( EF_BRIGHTLIGHT | EF_NODRAW ) )
		return FALSE;
	if ( pPlayer->is_unableto_spy_or_teleport == 1 )
		return FALSE;
	return TRUE;
}

// Starts the disguise timer. Own team takes 4 s, another team 8 s [tfc.so].
void CBasePlayer::TeamFortress_SpyChangeSkin( int iClass )
{
	int iTeam = m_iTeamToDisguiseAs;
	int iDelay = TF_DISGUISE_OWNTEAM;

	if ( iTeam )
	{
		if ( iTeam != undercover_team && iTeam != team_no )
			iDelay = TF_DISGUISE_ENEMY;
	}

	// asking for your own class on your own team just drops the disguise
	if ( iClass == PC_SPY && ( !iTeam || iTeam == team_no ) )
	{
		m_iTeamToDisguiseAs = 0;
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Disguise_resetclass" );
		undercover_skin = 0;
		undercover_team = 0;
		is_undercover = 0;
		if ( !is_feigning )
			Spy_ResetExternalWeaponModel();
		TeamFortress_SetSkin();
		return;
	}

	if ( iClass == undercover_skin && iTeam == undercover_team )
	{
		m_iTeamToDisguiseAs = 0;
		return;
	}

	if ( !TF_SpyCanDisguise( this ) )
	{
		m_iTeamToDisguiseAs = 0;
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Spy_unable" );
		return;
	}

	ClientPrint( pev, HUD_PRINTNOTIFY, "#Disguise_start" );
	is_undercover = 2;

	// Counted down in TeamFortress_SpyThink: a spawned "timer" entity never fired
	// on hardware, and a player field cannot fail to tick.
	m_iSpyDisguiseClass = iClass;
	m_iSpyDisguiseTeam = iTeam;
	m_flSpyDisguiseTime = gpGlobals->time + iDelay;

	m_iTeamToDisguiseAs = 0;
	TeamFortress_SetSkin();
}

void CBasePlayer::TeamFortress_SpyDisguise( int iTeam, int iClass )
{
	if ( pev->playerclass != PC_SPY )
		return;

	if ( iTeam <= 0 || iTeam > (int)number_of_teams )
		return;
	if ( iClass < PC_SCOUT || iClass > PC_ENGINEER )
		return;

	if ( !TF_SpyCanDisguise( this ) )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Spy_unable" );
		return;
	}

	m_iTeamToDisguiseAs = iTeam;
	TeamFortress_SpyChangeSkin( iClass );
}

// bEnemy picks a random team that is not ours; otherwise disguise as our own.
void CBasePlayer::TeamFortress_SpyDisguiseEnemy( BOOL bEnemy, int iClass )
{
	int iTeam;

	if ( !bEnemy )
	{
		iTeam = team_no;
	}
	else
	{
		if ( (int)number_of_teams < 2 )
			return;
		do
		{
			iTeam = RANDOM_LONG( 1, (int)number_of_teams );
		} while ( iTeam == team_no );
	}

	TeamFortress_SpyDisguise( iTeam, iClass );
}

// The "!SPY" menu button with no class: cancel a running disguise.
void CBasePlayer::TeamFortress_SpyGoUndercover( void )
{
	if ( pev->playerclass != PC_SPY )
		return;

	if ( !TF_SpyCanDisguise( this ) )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Spy_unable" );
		return;
	}

	if ( is_undercover == 2 )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Disguise_stop" );
		m_flSpyDisguiseTime = 0;
		is_undercover = 0;
	}
}

void CBasePlayer::Spy_RemoveDisguise( void )
{
	if ( pev->playerclass != PC_SPY )
		return;

	m_flSpyDisguiseTime = 0;

	if ( undercover_team || undercover_skin )
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Disguise_Lost" );

	undercover_team = 0;
	undercover_skin = 0;
	undercover_target = NULL;
	is_undercover = 0;
	immune_to_check = gpGlobals->time + TF_SPY_CHECK_GRACE;

	if ( !is_feigning )
		Spy_ResetExternalWeaponModel();

	TeamFortress_SetSkin();
	TeamFortress_SpyCalcName();
}

// The countdown has run out: the cover is now live.
static void TF_SpyBecomeUndercover( CBasePlayer *pSpy )
{
	pSpy->immune_to_check = gpGlobals->time + TF_SPY_CHECK_GRACE;
	pSpy->m_flSpyDisguiseTime = 0;

	if ( pSpy->m_iSpyDisguiseClass > 0 && pSpy->m_iSpyDisguiseClass < PC_LASTCLASS )
	{
		ClientPrint( pSpy->pev, HUD_PRINTNOTIFY, "#Disguise_asclass",
		             UTIL_VarArgs( "#%s", sTFClassNames[pSpy->m_iSpyDisguiseClass] ) );
		pSpy->undercover_skin = pSpy->m_iSpyDisguiseClass;
	}

	if ( pSpy->m_iSpyDisguiseTeam )
	{
		ClientPrint( pSpy->pev, HUD_PRINTNOTIFY, "#Disguise_asteam",
		             UTIL_VarArgs( "%d", pSpy->m_iSpyDisguiseTeam ) );
		pSpy->undercover_team = pSpy->m_iSpyDisguiseTeam;
	}

	ClientPrint( pSpy->pev, HUD_PRINTNOTIFY, "#Disguise_now" );
	pSpy->is_undercover = 1;
	pSpy->TeamFortress_SpyCalcName();
	pSpy->Spy_DisguiseExternalWeaponModel();
	pSpy->TeamFortress_SetSkin();
}

// Kept for the header's sake; the disguise no longer uses a timer entity.
void CBaseEntity::Timer_SpyUndercoverThink( void )
{
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time;
}

// Feign death

// [tfc.so] nobody else may be standing on top of the spy.
BOOL CBasePlayer::CanFeign( void )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, pev->origin, TF_FEIGN_RADIUS ) ) != NULL )
	{
		if ( pEnt == this )
			continue;
		if ( ( pEnt->pev->flags & FL_CLIENT ) && pEnt->IsPlayer() )
			return FALSE;
	}

	return TRUE;
}

// Not a real obituary: it writes the same kill-feed line a world death would,
// so the enemy team reads it as a genuine kill.
static void TF_FakeDeathMessage( CBasePlayer *pPlayer )
{
	MESSAGE_BEGIN( MSG_ALL, gmsgDeathMsg );
		WRITE_BYTE( 0 );
		WRITE_BYTE( ENTINDEX( pPlayer->edict() ) );
		WRITE_STRING( "world" );
	MESSAGE_END();
}

// Shared by the feign get-up and a goal's forced respawn.
void TeamFortress_SpyStandUp( CBasePlayer *pPlayer )
{
	pPlayer->is_feigning = 0;
	pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
	pPlayer->pev->flags &= ~FL_FROZEN;
	pPlayer->pev->view_ofs = VEC_VIEW;
	UTIL_SetSize( pPlayer->pev, VEC_HULL_MIN, VEC_HULL_MAX );
	pPlayer->SetAnimation( PLAYER_IDLE );

	if ( pPlayer->undercover_skin )
		pPlayer->Spy_DisguiseExternalWeaponModel();
	else
		pPlayer->Spy_ResetExternalWeaponModel();

	pPlayer->TeamFortress_SetSpeed();
	pPlayer->TeamFortress_SetSkin();
}

void CBasePlayer::TeamFortress_SpyFeignDeath( BOOL bSilent )
{
	if ( pev->playerclass != PC_SPY || !IsAlive() )
		return;

	if ( is_feigning )
	{
		// getting up: need headroom and nobody standing on us
		TraceResult tr;
		UTIL_TraceHull( pev->origin, pev->origin + Vector( 0, 0, TF_FEIGN_STANDUP_Z ),
		                dont_ignore_monsters, human_hull, edict(), &tr );

		if ( tr.fStartSolid || tr.flFraction != 1.0f || !CanFeign() )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Feign_unabletogetup" );
			return;
		}

		TeamFortress_SpyStandUp( this );
	}
	else
	{
		if ( !( pev->flags & FL_ONGROUND ) )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Feign_air" );
			return;
		}

		if ( is_unableto_spy_or_teleport == 1 )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Feign_unable" );
			return;
		}

		if ( !CanFeign() )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Feign_onspy" );
			return;
		}

		TraceResult tr;
		UTIL_TraceHull( pev->origin, pev->origin, dont_ignore_monsters, head_hull, edict(), &tr );
		if ( tr.fStartSolid || tr.fAllSolid )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Feign_noroom" );
			return;
		}

		// [tfc.so] a feigning spy lets go of every goal item it may drop
		TeamFortress_DropCarriedItems( this );

		is_feigning = 1;
		tfstate |= TFSTATE_CANT_MOVE;
		// Clearing pev->button does NOT stop a jump -- pmove reads the usercmd,
		// not pev. FL_FROZEN is what SV_PlayerIsFrozen honours.
		pev->flags |= FL_FROZEN;
		UTIL_SetSize( pev, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX );
		pev->view_ofs = Vector( 0, 0, -8 );
		SetAnimation( PLAYER_DIE );

		if ( !m_iszSavedWeaponModel )
			m_iszSavedWeaponModel = pev->weaponmodel;
		pev->weaponmodel = iStringNull;

		TeamFortress_SetSpeed();
		TeamFortress_SetSkin();
		immune_to_check = gpGlobals->time + TF_SPY_CHECK_GRACE;

		if ( !bSilent )
			TF_FakeDeathMessage( this );
	}

}

// Change-detected, so a respawn reset reaches the command menu too.
static void TF_SendFeignState( CBasePlayer *pPlayer )
{
	if ( pPlayer->m_iClientIsFeigning == pPlayer->is_feigning )
		return;
	pPlayer->m_iClientIsFeigning = pPlayer->is_feigning;

	MESSAGE_BEGIN( MSG_ONE, gmsgFeignState, NULL, pPlayer->pev );
		WRITE_BYTE( pPlayer->is_feigning );
	MESSAGE_END();
}

// Runs from CTeamFortress::PlayerThink, before pmove reads the buttons.
void TeamFortress_SpyThink( CBasePlayer *pPlayer )
{
	TF_SendFeignState( pPlayer );

	if ( pPlayer->pev->playerclass != PC_SPY )
		return;

	if ( pPlayer->is_feigning )
	{
		if ( !pPlayer->IsAlive() )
		{
			pPlayer->is_feigning = 0;
			return;
		}

		pPlayer->pev->button &= ~( IN_ATTACK | IN_ATTACK2 | IN_JUMP | IN_DUCK );
		pPlayer->m_flNextAttack = 0.5f;
		return;
	}

	if ( pPlayer->m_flSpyDisguiseTime > 0 && gpGlobals->time >= pPlayer->m_flSpyDisguiseTime )
	{
		if ( pPlayer->is_undercover == 2 )
			TF_SpyBecomeUndercover( pPlayer );
		else
			pPlayer->m_flSpyDisguiseTime = 0;
		return;
	}

	// Firing blows a LIVE cover only (TFC also cancels the countdown, which read as
	// a silent failure). The knife is the axe slot; a backstab keeps the cover.
	if ( pPlayer->is_undercover == 1 && ( pPlayer->pev->button & ( IN_ATTACK | IN_ATTACK2 ) ) )
	{
		CBasePlayerItem *pItem = pPlayer->m_pActiveItem;
		if ( !pItem || !( FClassnameIs( pItem->pev, "tf_weapon_knife" )
		                  || FClassnameIs( pItem->pev, "tf_weapon_axe" ) ) )
			pPlayer->Spy_RemoveDisguise();
	}
}

// Commands sent by the tf15 VGUI command menu

BOOL TeamFortress_SpyCommand( CBasePlayer *pPlayer, const char *pcmd )
{
	if ( FStrEq( pcmd, "feign" ) )
	{
		pPlayer->TeamFortress_SpyFeignDeath( FALSE );
		return TRUE;
	}

	if ( FStrEq( pcmd, "sfeign" ) )
	{
		pPlayer->TeamFortress_SpyFeignDeath( TRUE );
		return TRUE;
	}

	if ( FStrEq( pcmd, "disguise" ) )
	{
		if ( CMD_ARGC() >= 3 )
			pPlayer->TeamFortress_SpyDisguise( atoi( CMD_ARGV( 1 ) ), atoi( CMD_ARGV( 2 ) ) );
		else
			pPlayer->TeamFortress_SpyGoUndercover();
		return TRUE;
	}

	if ( FStrEq( pcmd, "disguise_enemy" ) )
	{
		if ( CMD_ARGC() >= 2 )
			pPlayer->TeamFortress_SpyDisguiseEnemy( TRUE, atoi( CMD_ARGV( 1 ) ) );
		return TRUE;
	}

	if ( FStrEq( pcmd, "disguise_friendly" ) )
	{
		if ( CMD_ARGC() >= 2 )
			pPlayer->TeamFortress_SpyDisguiseEnemy( FALSE, atoi( CMD_ARGV( 1 ) ) );
		return TRUE;
	}

	return FALSE;
}
