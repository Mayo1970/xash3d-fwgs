#include <time.h>

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"

#include "tf_defs.h"

LINK_ENTITY_TO_CLASS( info_player_teamspawn, CTFSpawn )
LINK_ENTITY_TO_CLASS( i_p_t, CTFSpawn )
LINK_ENTITY_TO_CLASS( info_tf_teamcheck, CTeamCheck )

CBaseEntity *Findgoal( int gno )
{
	CBaseEntity *pGoal = NULL;

	while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, "info_tfgoal" ) ) != NULL )
	{
		if ( pGoal->goal_no == gno )
			return pGoal;
	}

	ALERT( at_console, "Could not find a goal with a goal_no of %d.\n", gno );
	return NULL;
}

CBaseEntity *Finditem( int ino )
{
	CBaseEntity *pItem = NULL;

	while ( ( pItem = UTIL_FindEntityByClassname( pItem, "item_tfgoal" ) ) != NULL )
	{
		if ( pItem->goal_no == ino )
			return pItem;
	}

	ALERT( at_console, "Could not find an item with a goal_no of %d.\n", ino );
	return NULL;
}

static CBaseEntity *FindTeamCheck( string_t iszName )
{
	edict_t *pent = FIND_ENTITY_BY_TARGETNAME( NULL, STRING( iszName ) );

	if ( FNullEnt( pent ) || !FClassnameIs( pent, "info_tf_teamcheck" ) )
		return NULL;

	return CBaseEntity::Instance( pent );
}

static int GetTeamCheckTeam( string_t iszName )
{
	CBaseEntity *pCheck = FindTeamCheck( iszName );

	return pCheck ? pCheck->team_no : 0;
}

static BOOL GoalInState( int gno, int iState )
{
	CBaseEntity *pGoal = Findgoal( gno );

	return pGoal && pGoal->goal_state == iState;
}

// TRUE when no goal carries the group, as in tfc.so.
static BOOL GroupInState( int gno, int iState )
{
	CBaseEntity *pGoal = NULL;

	while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, "info_tfgoal" ) ) != NULL )
	{
		if ( pGoal->group_no == gno && pGoal->goal_state != iState )
			return FALSE;
	}

	return TRUE;
}

static BOOL HasItemFromGroup( CBaseEntity *AP, int gno )
{
	CBaseEntity *pItem = NULL;

	while ( ( pItem = UTIL_FindEntityByClassname( pItem, "item_tfgoal" ) ) != NULL )
	{
		if ( pItem->group_no == gno && pItem->pev->owner == AP->edict() )
			return TRUE;
	}

	return FALSE;
}

static BOOL APMeetsCriteria( CBaseEntity *Goal, CBaseEntity *AP )
{
	CBaseEntity *pItem;
	BOOL bPlayer = AP && AP->Classify() == CLASS_PLAYER;

	if ( bPlayer )
	{
		if ( Goal->team_no && ( Goal->team_no != AP->team_no || AP->pev->deadflag ) )
			return FALSE;

		if ( Goal->teamcheck && AP->team_no != GetTeamCheckTeam( Goal->teamcheck ) )
			return FALSE;

		if ( Goal->pev->playerclass && AP->pev->playerclass != Goal->pev->playerclass )
			return FALSE;

		if ( Goal->items_allowed )
		{
			pItem = Finditem( Goal->items_allowed );
			if ( !pItem || pItem->pev->owner != AP->edict() )
				return FALSE;
		}
	}

	if ( Goal->if_goal_is_active && !GoalInState( Goal->if_goal_is_active, TFGS_ACTIVE ) )
		return FALSE;
	if ( Goal->if_goal_is_inactive && !GoalInState( Goal->if_goal_is_inactive, TFGS_INACTIVE ) )
		return FALSE;
	if ( Goal->if_goal_is_removed && !GoalInState( Goal->if_goal_is_removed, TFGS_REMOVED ) )
		return FALSE;
	if ( Goal->if_group_is_active && !GroupInState( Goal->if_group_is_active, TFGS_ACTIVE ) )
		return FALSE;
	if ( Goal->if_group_is_inactive && !GroupInState( Goal->if_group_is_inactive, TFGS_INACTIVE ) )
		return FALSE;
	if ( Goal->if_group_is_removed && !GroupInState( Goal->if_group_is_removed, TFGS_REMOVED ) )
		return FALSE;

	// An item still carried (TFGS_ACTIVE) counts as moved.
	if ( Goal->if_item_has_moved )
	{
		pItem = Finditem( Goal->if_item_has_moved );
		if ( !pItem )
			return FALSE;
		if ( pItem->goal_state != TFGS_ACTIVE && pItem->pev->origin == pItem->pev->oldorigin )
			return FALSE;
	}

	if ( Goal->if_item_hasnt_moved )
	{
		pItem = Finditem( Goal->if_item_hasnt_moved );
		if ( !pItem || pItem->goal_state == TFGS_ACTIVE || pItem->pev->origin != pItem->pev->oldorigin )
			return FALSE;
	}

	if ( bPlayer )
	{
		if ( Goal->has_item_from_group && !HasItemFromGroup( AP, Goal->has_item_from_group ) )
			return FALSE;
		if ( Goal->hasnt_item_from_group && HasItemFromGroup( AP, Goal->hasnt_item_from_group ) )
			return FALSE;
	}

	return TRUE;
}

BOOL ActivationSucceeded( CBaseEntity *Goal, CBasePlayer *AP, CBaseEntity *ActivatingGoal )
{
	if ( cb_prematch_time > gpGlobals->time && Goal->Classify() != CLASS_TFGOAL_TIMER )
		return FALSE;

	if ( Goal->goal_state != TFGS_ACTIVE && Goal->goal_state != TFGS_REMOVED && Goal->goal_state != TFGS_DELAYED )
	{
		BOOL bMet = APMeetsCriteria( Goal, AP );
		BOOL bReverse;

		if ( FClassnameIs( Goal->pev, "item_tfgoal" ) )
			bReverse = ( Goal->goal_activation & TFGI_REVERSE_AP ) != 0;
		else
			bReverse = ( Goal->goal_activation & TFGA_REVERSE_AP ) != 0;

		if ( bMet != bReverse )
			return TRUE;
	}

	// tfc.so runs ActivateDoResults( Findgoal( else_goal ) ) here; that needs the Phase 5 goal system.
	return FALSE;
}

void CTFSpawn::Spawn( void )
{
	if ( ( team_no < 1 || team_no > 4 ) && !teamcheck )
		ALERT( at_console, "Teamspawnpoint with an invalid team_no of %d\n", team_no );

	pev->classname = MAKE_STRING( "info_player_teamspawn" );
}

void CTFSpawn::Activate( void )
{
	m_pTeamCheck = NULL;

	if ( teamcheck )
		m_pTeamCheck = FindTeamCheck( teamcheck );
}

BOOL CTFSpawn::CheckTeam( int iTeamNo )
{
	if ( team_no )
		return team_no == iTeamNo;

	if ( m_pTeamCheck )
		return m_pTeamCheck->team_no == iTeamNo;

	return FALSE;
}

void CTeamCheck::Spawn( void )
{
}

// Maps flip attacker/defender sides with USE_TOGGLE, or pick a team with USE_SET.
void CTeamCheck::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	if ( useType == USE_TOGGLE )
		team_no = ( team_no == 1 ) ? 2 : 1;
	else if ( useType == USE_SET && value >= 1.0f && value <= 4.0f )
		team_no = (int)value;
}

BOOL CTeamCheck::TeamMatches( int iTeam )
{
	return team_no == iTeam;
}

void ParseTFServerSettings( void )
{
	time_t aclock;
	tm *ptm;
	CBaseEntity *pEntity;

	toggleflags &= ~TFLAG_CHEATCHECK;

	time( &aclock );
	ptm = localtime( &aclock );

	if ( ptm->tm_mon == 11 && ptm->tm_mday == 25 )
		christmas = 1;

	if ( tfc_birthday.value != 0.0f || ( ptm->tm_mon == 7 && ptm->tm_mday == 24 ) )
	{
		birthday = 1;

		pEntity = CBaseEntity::Create( "timer", g_vecZero, g_vecZero, NULL );
		pEntity->weapon = 10;
		pEntity->SetThink( &CBaseEntity::Timer_Birthday );
		pEntity->pev->nextthink = gpGlobals->time + 60.0f;
	}

	cb_prematch_time = tfc_clanbattle_prematch.value * 60.0f;
	if ( cb_prematch_time == 0.0f )
		cb_prematch_time = tfc_prematch.value * 60.0f;
	
	if ( tfc_clanbattle.value != 0.0f )
	{
		clan_scores_dumped = 0.0f;

		cb_ceasefire_time = tfc_clanbattle_ceasefire.value * 60.0f;
		if ( cb_ceasefire_time != 0.0f )
		{
			last_cease_fire = 1;
			initial_cease_fire = 1;
			cease_fire = 1;
		}
	}

	autokick_kills = tfc_autokick_kills.value;

	if ( tfc_fragscoring.value == 0.0f )
		toggleflags &= ~TFLAG_FRAGSCORING;
	else
		toggleflags |= TFLAG_FRAGSCORING;

	gpGlobals->teamplay = teamplay.value;
}