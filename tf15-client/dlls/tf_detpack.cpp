// TFC-6 Phase 5 -- the demoman's detpack. Ported from the retail tfc.so.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "effects.h"
#include "gamerules.h"
#include "tf_defs.h"

extern int gmsgDetpackState;

#define TF_MDL_DETPACK      "models/detpack.mdl"
#define TF_MDL_PRESENT      "models/presentlg.mdl"
#define TF_SPR_FLARE        "sprites/flare3.spr"
#define TF_SND_DET_HIT      "weapons/mortarhit.wav"
#define TF_SND_DET_ACTIVATE "weapons/mine_activate.wav"
#define TF_SND_DET_CHARGE   "weapons/mine_charge.wav"

#define TF_DET_SETTIME      3.0f    // WEAP_DETPACK_SETTIME
#define TF_DET_SIZE         700.0f  // WEAP_DETPACK_SIZE
#define TF_DET_GOAL_SIZE    1500.0f // WEAP_DETPACK_GOAL_SIZE
#define TF_DET_BLOCK_RADIUS 65.0f
#define TF_DET_IMMUNE_TIME  10.0f

static void TF_DetLogName( CBaseEntity *pEnt, char *buf, int len )
{
	_snprintf( buf, len, "%s<%i><%s><%s>", STRING( pEnt->pev->netname ), GETPLAYERUSERID( pEnt->edict() ),
	           GETPLAYERAUTHID( pEnt->edict() ), pEnt->team_no ? GetTeamName( pEnt->team_no ) : "SPECTATOR" );
}

class CDetpack : public CGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	int Classify( void ) { return CLASS_MACHINE; }
	void TeamFortress_TakeEMPBlast( entvars_t *pevGren );

	void EXPORT DetpackTouch( CBaseEntity *pOther );
	void EXPORT DetpackThink( void );

	void DetpackExplode( void );
	void Remove( void );
	void RemoveFlare( void );

	BOOL m_bChargeSound;
	BOOL m_bFlareOn;
	EHANDLE m_hFlare;
};

LINK_ENTITY_TO_CLASS( detpack, CDetpack )

void CDetpack::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_PRESENT );
	PRECACHE_MODEL( TF_MDL_DETPACK );
	PRECACHE_MODEL( TF_SPR_FLARE );
	PRECACHE_SOUND( TF_SND_DET_HIT );
	PRECACHE_SOUND( TF_SND_DET_ACTIVATE );
	PRECACHE_SOUND( TF_SND_DET_CHARGE );
}

// pev->health holds the detonation time; weaponmode 1 marks a pack being disarmed.
void CDetpack::Spawn( void )
{
	Precache();

	m_bFlareOn = FALSE;
	m_bChargeSound = FALSE;
	m_hFlare = NULL;
	pev->movetype = MOVETYPE_TOSS;
	pev->solid = SOLID_BBOX;
	weaponmode = 0;

	SET_MODEL( ENT( pev ), birthday ? TF_MDL_PRESENT : TF_MDL_DETPACK );
	SetTouch( &CDetpack::DetpackTouch );
	SetThink( &CDetpack::DetpackThink );
	pev->nextthink = gpGlobals->time + 0.1f;

	UTIL_SetSize( pev, Vector( -4, -4, -4 ), Vector( 4, 4, 4 ) );
	UTIL_SetOrigin( pev, pev->origin );

	EMIT_SOUND_DYN( ENT( pev ), CHAN_WEAPON, TF_SND_DET_ACTIVATE, 1.0f, 0.8f, 0, PITCH_NORM );
}

// [tfc.so] an EMP sets it off 1-3 s later
void CDetpack::TeamFortress_TakeEMPBlast( entvars_t *pevGren )
{
	pev->health = gpGlobals->time + 1.0f + RANDOM_FLOAT( 0, 2 );
}

void CDetpack::RemoveFlare( void )
{
	CBaseEntity *pFlare = m_hFlare;
	if ( pFlare )
		UTIL_Remove( pFlare );
	m_hFlare = NULL;
}

// [tfc.so] an enemy scout disarms it by touch
void CDetpack::DetpackTouch( CBaseEntity *pOther )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( !pOwner || pOther->Classify() != CLASS_PLAYER || pOther->pev->playerclass != PC_SCOUT || !pOther->IsAlive() )
		return;
	if ( pOwner->IsAlly( pOther ) )
		return;

	if ( birthday )
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Detpack_disarm_bday", STRING( pOther->pev->netname ), STRING( pOwner->pev->netname ) );
	else
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Detpack_disarm", STRING( pOwner->pev->netname ), STRING( pOther->pev->netname ) );

	char szScout[256], szOwner[256];
	TF_DetLogName( pOther, szScout, sizeof( szScout ) );
	TF_DetLogName( pOwner, szOwner, sizeof( szOwner ) );
	UTIL_LogPrintf( "\"%s\" triggered \"Detpack_Disarmed\" against \"%s\"\n", szScout, szOwner );

	( (CBasePlayer *)pOther )->TF_AddFrags( 1 );

	// retail leaves the countdown flare behind; ours goes with the pack
	RemoveFlare();
	pev->solid = SOLID_NOT;
	SetTouch( NULL );
	SetThink( NULL );
	UTIL_Remove( this );
}

void CDetpack::DetpackThink( void )
{
	if ( pev->health != 0 && gpGlobals->time + 0.9f >= pev->health )
	{
		DetpackExplode();
		return;
	}

	CheckBelowBuilding( 8 );

	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	int iLeft = (int)( pev->health - gpGlobals->time );
	pev->nextthink = gpGlobals->time + 1.0f;

	if ( iLeft > 10 )
		return;

	if ( pOwner )
		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Detpack_countdown", UTIL_dtos1( iLeft ) );

	if ( iLeft > 5 )
		return;

	if ( !m_bChargeSound )
	{
		EMIT_SOUND_DYN( ENT( pev ), CHAN_WEAPON, TF_SND_DET_CHARGE, 1.0f, 0.8f, 0, PITCH_NORM );
		m_bChargeSound = TRUE;

		CSprite *pFlare = CSprite::SpriteCreate( TF_SPR_FLARE, pev->origin + Vector( 0, 0, 64 ), FALSE );
		if ( pFlare )
		{
			pFlare->pev->rendermode = kRenderGlow;
			pFlare->pev->rendercolor = Vector( 255, 0, 0 );
			pFlare->pev->renderamt = 0;
			pFlare->pev->renderfx = kRenderFxNoDissipation;
			m_hFlare = pFlare;
		}
		return;
	}

	if ( iLeft == 5 )
		return;

	// blinks once a second for the last few seconds
	m_bFlareOn = !m_bFlareOn;
	CBaseEntity *pFlare = m_hFlare;
	if ( pFlare )
		pFlare->pev->renderamt = m_bFlareOn ? 64 : 0;
}

void CDetpack::DetpackExplode( void )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	char szOwner[256];

	int iContents = UTIL_PointContents( pev->origin );
	if ( !pOwner || iContents == CONTENTS_SOLID || iContents == CONTENTS_SKY || pOwner->has_disconnected == 1 )
	{
		if ( pOwner )
		{
			ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Detpack_fizzle" );
			TF_DetLogName( pOwner, szOwner, sizeof( szOwner ) );
			UTIL_LogPrintf( "\"<-1><><>\" triggered \"Detpack_Fizzle\" against \"%s\"\n", szOwner );
		}
	}
	else
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#FITH" );
		EMIT_SOUND_DYN( ENT( pev ), CHAN_WEAPON, TF_SND_DET_HIT, 1.0f, 0.8f, 0, PITCH_NORM );
		if ( birthday )
			UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#SpreadsCheer", STRING( pOwner->pev->netname ) );

		TF_DetLogName( pOwner, szOwner, sizeof( szOwner ) );
		UTIL_LogPrintf( "\"%s\" triggered \"Detpack_Explode\"\n", szOwner );

		// goal triggers reach 1500 u, damage 700 u, both line-of-sight gated
		Vector vecSpot = pev->origin + Vector( 0, 0, 16 );
		CBaseEntity *pEnt = NULL;
		while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, vecSpot, TF_DET_GOAL_SIZE ) ) != NULL )
		{
			TraceResult tr;
			UTIL_TraceLine( vecSpot, pEnt->Center() + pEnt->pev->view_ofs, dont_ignore_monsters, ENT( pev ), &tr );
			if ( tr.flFraction < 1.0f && tr.pHit != pEnt->edict() )
				continue;

			if ( FClassnameIs( pEnt->pev, "info_tfgoal" ) )
			{
				if ( ( pEnt->goal_activation & TFGA_TOUCH_DETPACK ) && pEnt->search_time == 0 )
					ActivateDoResults( pEnt, (CBasePlayer *)pOwner, pEnt );
				continue;
			}

			if ( pEnt->pev->takedamage == DAMAGE_NO )
				continue;
			if ( ( pEnt->pev->origin - pev->origin ).Length() > TF_DET_SIZE )
				continue;

			Vector vecCenter = pEnt->pev->origin + ( pEnt->pev->mins + pEnt->pev->maxs ) * 0.5f;
			float flDmg = ( TF_DET_SIZE - ( pev->origin - vecCenter ).Length() * 0.5f ) * 2.0f;
			if ( flDmg == 0 )
				continue;

			pEnt->TakeDamage( pev, pOwner->pev, flDmg, DMG_BLAST );
		}

		MESSAGE_BEGIN( MSG_PAS, SVC_TEMPENTITY, pev->origin );
			WRITE_BYTE( TE_EXPLOSION );
			WRITE_COORD( pev->origin.x );
			WRITE_COORD( pev->origin.y );
			WRITE_COORD( pev->origin.z );
			WRITE_SHORT( g_sModelIndexFireball );
			WRITE_BYTE( 50 );
			WRITE_BYTE( 15 );
			WRITE_BYTE( TE_EXPLFLAG_NOADDITIVE | TE_EXPLFLAG_NODLIGHTS );
		MESSAGE_END();
	}

	// [tfc.so] frees a scout caught mid-disarm
	if ( weaponmode == 1.0f )
	{
		CBaseEntity *pScout = pev->enemy ? CBaseEntity::Instance( pev->enemy ) : NULL;
		if ( pScout && pScout->IsPlayer() )
		{
			CBasePlayer *pPlayer = (CBasePlayer *)pScout;
			pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
			pPlayer->pev->flags &= ~FL_FROZEN;
			pPlayer->TeamFortress_SetSpeed();
			CBaseEntity *pTimer = pPlayer->FindTimer( TF_TIMER_DETPACKDISARM );
			if ( pTimer )
				UTIL_Remove( pTimer );
		}
	}

	RemoveFlare();
	pev->solid = SOLID_NOT;
	SetTouch( NULL );
	SetThink( NULL );
	UTIL_Remove( this );
}

// [tfc.so] CBaseDoor::Blocked: a door crushing a pack hands it back to the owner
void CDetpack::Remove( void )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( pOwner )
	{
		pOwner->ammo_detpack++;
		ClientPrint( pOwner->pev, HUD_PRINTCENTER, "#Detpack_blocking_door_destroyed" );

		char szOwner[256];
		TF_DetLogName( pOwner, szOwner, sizeof( szOwner ) );
		UTIL_LogPrintf( "\"<-1><><>\" triggered \"Detpack_blocking_door_destroyed\" against \"%s\"\n", szOwner );
	}

	RemoveFlare();

	MESSAGE_BEGIN( MSG_PAS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z );
		WRITE_SHORT( g_sModelIndexSmoke );
		WRITE_BYTE( 30 );
		WRITE_BYTE( 15 );
		WRITE_BYTE( TE_EXPLFLAG_NOADDITIVE | TE_EXPLFLAG_NODLIGHTS );
	MESSAGE_END();

	pev->solid = SOLID_NOT;
	SetTouch( NULL );
	SetThink( NULL );
	UTIL_Remove( this );
}

BOOL TeamFortress_DetpackBlocked( CBaseEntity *pOther )
{
	if ( !pOther || !FClassnameIs( pOther->pev, "detpack" ) )
		return FALSE;

	( (CDetpack *)pOther )->Remove();
	return TRUE;
}

static BOOL TF_DetpackSpotBlocked( CBasePlayer *pPlayer )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, pPlayer->pev->origin, TF_DET_BLOCK_RADIUS ) ) != NULL )
	{
		const char *pszMsg = NULL;

		if ( FClassnameIs( pEnt->pev, "player" ) && pEnt != pPlayer && !( pEnt->pev->effects & EF_NODRAW ) )
			pszMsg = "#Detpack_someone";
		else if ( FClassnameIs( pEnt->pev, "building_sentrygun" ) )
			pszMsg = "#Detpack_sentry";
		else if ( FClassnameIs( pEnt->pev, "building_dispenser" ) )
			pszMsg = "#Detpack_dispenser";
		else if ( FClassnameIs( pEnt->pev, "building_teleporter" ) )
			pszMsg = "#Detpack_teleporter";
		else if ( FClassnameIs( pEnt->pev, "detpack" ) )
			pszMsg = "#Detpack_stack";

		if ( pszMsg )
		{
			ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, pszMsg );
			return TRUE;
		}
	}

	return FALSE;
}

// [tfc.so] starts the 3 s set; the pack itself appears in Timer_DetpackSet
void CBasePlayer::TeamFortress_SetDetpack( int iTimer )
{
	if ( !( weapons_carried & WEAP_DETPACK ) || ammo_detpack <= 0 )
		return;

	if ( TF_DetpackSpotBlocked( this ) )
		return;

	if ( !( pev->flags & FL_ONGROUND ) )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Detpack_air" );
		return;
	}

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "detpack" ) ) != NULL )
	{
		if ( pEnt->pev->owner == edict() )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Detpack_oneactive" );
			return;
		}
	}

	if ( iTimer <= 4 )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Detpack_fivesec" );
		return;
	}

	ammo_detpack--;
	is_detpacking = 1;
	immune_to_check = gpGlobals->time + TF_DET_IMMUNE_TIME;
	tfstate |= TFSTATE_CANT_MOVE;
	pev->flags |= FL_FROZEN;   // stops jumping too (Phase 4 lesson)
	TeamFortress_SetSpeed();

	if ( m_pActiveItem )
		m_pActiveItem->Holster();
	pev->weaponmodel = 0;

	ClientPrint( pev, HUD_PRINTNOTIFY, "#Detpack_set", UTIL_dtos1( iTimer ) );

	CBaseEntity *pTimer = CreateTimer( TF_TIMER_DETPACKSET );
	if ( pTimer )
	{
		pTimer->pev->nextthink = gpGlobals->time + TF_DET_SETTIME;
		pTimer->SetThink( &CBaseEntity::Timer_DetpackSet );
		pTimer->pev->health = (float)iTimer;
	}
}

// Runs on the timer entity; its owner is the demoman, its health the countdown.
void CBaseEntity::Timer_DetpackSet( void )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( pOwner && pOwner->IsPlayer() )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)pOwner;

		pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
		pPlayer->pev->flags &= ~FL_FROZEN;
		pPlayer->is_detpacking = 0;
		pPlayer->TeamFortress_SetSpeed();
		if ( pPlayer->m_pActiveItem )
			pPlayer->m_pActiveItem->Deploy();

		CBaseEntity *pDet = CBaseEntity::Create( "detpack", pPlayer->pev->origin, g_vecZero, pPlayer->edict() );
		if ( pDet )
			pDet->pev->health = gpGlobals->time + pev->health;

		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Detpack_finishset" );

		char szOwner[256];
		TF_DetLogName( pPlayer, szOwner, sizeof( szOwner ) );
		UTIL_LogPrintf( "\"%s\" triggered \"Detpack_Set\"\n", szOwner );
	}

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time;
}

// [tfc.so] cancels a detpack still being set: the pack goes back to the player.
void CBasePlayer::TeamFortress_DetpackStop( void )
{
	CBaseEntity *pTimer = FindTimer( TF_TIMER_DETPACKSET );
	if ( !pTimer )
		return;

	ClientPrint( pev, HUD_PRINTNOTIFY, "#Detpack_retrieve" );
	ammo_detpack++;
	dremove( pTimer );

	tfstate &= ~TFSTATE_CANT_MOVE;
	pev->flags &= ~FL_FROZEN;
	is_detpacking = 0;
	TeamFortress_SetSpeed();

	if ( m_pActiveItem )
		m_pActiveItem->Deploy();
}

// [tfc.so] removes the first detpack this player owns; TRUE if there was one.
BOOL CBasePlayer::TeamFortress_RemoveDetpacks( void )
{
	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "detpack" ) ) != NULL )
	{
		if ( pEnt->pev->owner == edict() )
		{
			( (CDetpack *)pEnt )->RemoveFlare();
			dremove( pEnt );
			return TRUE;
		}
	}

	return FALSE;
}

// [tfc.so] UpdateClientCommandMenu: 1 = setting, 2 = has a pack, 0 = neither
void TeamFortress_SendDetpackState( CBasePlayer *pPlayer )
{
	if ( pPlayer->is_detpacking == pPlayer->m_iClientIsDetpacking
	     && pPlayer->ammo_detpack == pPlayer->m_iClientDetpackAmmo )
		return;

	MESSAGE_BEGIN( MSG_ONE, gmsgDetpackState, NULL, pPlayer->pev );
		WRITE_BYTE( pPlayer->is_detpacking ? 1 : ( pPlayer->ammo_detpack ? 2 : 0 ) );
	MESSAGE_END();

	pPlayer->m_iClientIsDetpacking = pPlayer->is_detpacking;
	pPlayer->m_iClientDetpackAmmo = pPlayer->ammo_detpack;
}

// [tfc.so] ClientCommand: detstart N (5..50, default 20), +det5/20/50, detstop, -det5/20/50
BOOL TeamFortress_DetpackCommand( CBasePlayer *pPlayer, const char *pcmd )
{
	if ( FStrEq( pcmd, "detstart" ) )
	{
		int iTimer = 20;
		if ( CMD_ARGC() > 1 )
		{
			iTimer = atoi( CMD_ARGV( 1 ) );
			if ( iTimer > 50 )
				iTimer = 50;
			else if ( iTimer <= 4 )
				iTimer = 5;
		}
		pPlayer->TeamFortress_SetDetpack( iTimer );
		return TRUE;
	}

	if ( FStrEq( pcmd, "+det5" ) )
	{
		pPlayer->TeamFortress_SetDetpack( 5 );
		return TRUE;
	}
	if ( FStrEq( pcmd, "+det20" ) )
	{
		pPlayer->TeamFortress_SetDetpack( 20 );
		return TRUE;
	}
	if ( FStrEq( pcmd, "+det50" ) )
	{
		pPlayer->TeamFortress_SetDetpack( 50 );
		return TRUE;
	}

	if ( FStrEq( pcmd, "detstop" ) || FStrEq( pcmd, "-det5" ) || FStrEq( pcmd, "-det20" ) || FStrEq( pcmd, "-det50" ) )
	{
		pPlayer->TeamFortress_DetpackStop();
		return TRUE;
	}

	return FALSE;
}
