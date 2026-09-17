// TFC map scripting: goals, goal items, timer goals, team spawns and map settings.
// Ported function by function from the retail tfc.so; [tfc.so] marks its quirks.

#include <time.h>

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "skill.h"
#include "shake.h"
#include "items.h"
#include "effects.h"
#include "tf_defs.h"

LINK_ENTITY_TO_CLASS( info_player_teamspawn, CTFSpawn )
LINK_ENTITY_TO_CLASS( i_p_t, CTFSpawn )
LINK_ENTITY_TO_CLASS( info_tf_teamcheck, CTeamCheck )
LINK_ENTITY_TO_CLASS( info_tf_teamset, CTeamSet )
LINK_ENTITY_TO_CLASS( info_tfdetect, CTFDetect )
LINK_ENTITY_TO_CLASS( info_tfgoal, CTFGoal )
LINK_ENTITY_TO_CLASS( i_t_g, CTFGoal )
LINK_ENTITY_TO_CLASS( info_tfgoal_timer, CTFTimerGoal )
LINK_ENTITY_TO_CLASS( i_t_t, CTFTimerGoal )
LINK_ENTITY_TO_CLASS( item_tfgoal, CTFGoalItem )

static void DoResultsWork( CBaseEntity *Goal, CBasePlayer *AP, int bAddBonuses );
static void Apply_Results( CBaseEntity *Goal, CBasePlayer *Player, CBasePlayer *AP, int bAddBonuses );
static void RemoveResults( CBaseEntity *Goal, CBasePlayer *Player );
static void DoItemGroupWork( CBaseEntity *Item, CBasePlayer *AP );
static void DoTriggerWork( CBaseEntity *Goal, CBasePlayer *AP );
static void SetupRespawn( CBaseEntity *Goal );
static void EndRound( CBaseEntity *Goal );

#define TF_ITEM_CLASSNAME  "item_tfgoal"
#define TF_GOAL_CLASSNAME  "info_tfgoal"
#define TF_SPAWN_CLASSNAME "info_player_teamspawn"

static inline BOOL TF_Prematch( void )
{
	return cb_prematch_time > gpGlobals->time;
}

static const char *TF_TeamLogName( CBaseEntity *pPlayer )
{
	return pPlayer->team_no ? GetTeamName( pPlayer->team_no ) : "SPECTATOR";
}

static void TF_LogTriggered( CBaseEntity *pPlayer, const char *pszWhat )
{
	UTIL_LogPrintf( "\"%s<%i><%s><%s>\" triggered \"%s\"\n",
	                STRING( pPlayer->pev->netname ),
	                GETPLAYERUSERID( pPlayer->edict() ),
	                GETPLAYERAUTHID( pPlayer->edict() ),
	                TF_TeamLogName( pPlayer ), pszWhat );
}

// [tfc.so] a NULL owner resolves to the world entity; callers only read from it.
static CBaseEntity *TF_OwnerOf( CBaseEntity *pEnt )
{
	return CBaseEntity::Instance( pEnt->pev->owner );
}

// [tfc.so] the skill gate shared by CheckExistence and every map-script Spawn.
static BOOL TF_SkillAllows( CBaseEntity *e )
{
	int mn = e->ex_skill_min;
	int mx = e->ex_skill_max;
	int sk = g_iSkillLevel;

	if ( mn == -1 && sk < 0 )
		return FALSE;

	if ( mx == -1 )
	{
		if ( sk > 0 )
			return FALSE;
		return mn == -1 || mn == 0 || mn <= sk;
	}

	if ( mn != -1 && mn != 0 && mn > sk )
		return FALSE;

	return mx == 0 || sk <= mx;
}

BOOL CBaseEntity::CheckExistence( void )
{
	return TF_SkillAllows( this );
}

// [tfc.so] removes at once; only call it from spawn, think or command code.
void dremove( CBaseEntity *te )
{
	if ( !te || te->is_removed == TRUE )
		return;

	te->is_removed = TRUE;
	REMOVE_ENTITY( te->edict() );
}

CBaseEntity *Findgoal( int gno )
{
	CBaseEntity *pGoal = NULL;

	while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
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

	while ( ( pItem = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME ) ) != NULL )
	{
		if ( pItem->goal_no == ino )
			return pItem;
	}

	ALERT( at_console, "Could not find an item with a goal_no of %d.\n", ino );
	return NULL;
}

// [tfc.so] team spawns carry their classname in netname too, which is what this searches.
CBaseEntity *Findteamspawn( int gno )
{
	CBaseEntity *pSpawn = NULL;

	while ( ( pSpawn = UTIL_FindEntityByString( pSpawn, "netname", TF_SPAWN_CLASSNAME ) ) != NULL )
	{
		if ( pSpawn->goal_no == gno )
			return pSpawn;
	}

	ALERT( at_console, "Could not find a Teamspawn with a goal_no of %d.\n", gno );
	return NULL;
}

static CBaseEntity *FindTeamCheck( const char *pszName )
{
	edict_t *pent = FIND_ENTITY_BY_TARGETNAME( NULL, pszName );

	if ( FNullEnt( pent ) || !FClassnameIs( pent, "info_tf_teamcheck" ) )
		return NULL;

	return CBaseEntity::Instance( pent );
}

static int GetTeamCheckTeam( const char *pszName )
{
	CBaseEntity *pCheck = FindTeamCheck( pszName );

	return pCheck ? pCheck->team_no : 0;
}

static void TF_ResolveOwnedBy( CBaseEntity *pEnt )
{
	if ( pEnt->owned_by_teamcheck )
		pEnt->owned_by = GetTeamCheckTeam( STRING( pEnt->owned_by_teamcheck ) );
}

static BOOL TF_IsGoalItem( CBaseEntity *pEnt )
{
	return FClassnameIs( pEnt->pev, TF_ITEM_CLASSNAME );
}

static BOOL TF_ModelIsBrush( CBaseEntity *pEnt )
{
	return ( STRING( pEnt->pev->model ) )[0] == '*';
}

static void InactivateGoalBody( CBaseEntity *Goal )
{
	if ( Goal->Classify() != CLASS_TFGOAL_TIMER )
	{
		int iClass = Goal->Classify();

		if ( ( Goal->goal_activation & TFGI_SOLID ) && ( iClass == CLASS_TFGOAL || iClass == CLASS_TFGOAL_ITEM ) )
			Goal->pev->solid = SOLID_BBOX;
		else
			Goal->pev->solid = SOLID_TRIGGER;
	}

	Goal->goal_state = TFGS_INACTIVE;

	if ( !TF_ModelIsBrush( Goal ) )
		Goal->pev->effects &= ~EF_NODRAW;
}

static void InactivateGoal( CBaseEntity *Goal )
{
	if ( Goal->goal_state == TFGS_ACTIVE )
		InactivateGoalBody( Goal );
}

static void RestoreGoal( CBaseEntity *Goal )
{
	if ( Goal->goal_state != TFGS_REMOVED )
		return;

	if ( Goal->search_time != 0.0f )
		Goal->pev->nextthink = gpGlobals->time + Goal->search_time;
	else if ( ( Goal->goal_activation & TFGI_SOLID ) && TF_IsGoalItem( Goal ) )
		Goal->pev->solid = SOLID_BBOX;
	else
		Goal->pev->solid = SOLID_TRIGGER;

	Goal->goal_state = TFGS_INACTIVE;

	if ( !TF_ModelIsBrush( Goal ) )
		Goal->pev->effects &= ~EF_NODRAW;
}

void RemoveGoal( CBaseEntity *Goal )
{
	Goal->pev->solid = SOLID_NOT;
	Goal->goal_state = TFGS_REMOVED;
	Goal->pev->effects |= EF_NODRAW;
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

	while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
	{
		if ( pGoal->group_no == gno && pGoal->goal_state != iState )
			return FALSE;
	}

	return TRUE;
}

static BOOL HasItemFromGroup( CBaseEntity *AP, int gno )
{
	CBaseEntity *pItem = NULL;

	while ( ( pItem = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME ) ) != NULL )
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

		if ( Goal->teamcheck && AP->team_no != GetTeamCheckTeam( STRING( Goal->teamcheck ) ) )
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

// State and criteria test shared by ActivationSucceeded and ActivateDoResults.
static BOOL TF_CriteriaPass( CBaseEntity *Goal, CBaseEntity *AP )
{
	int iState = Goal->goal_state;

	if ( iState == TFGS_ACTIVE || iState == TFGS_REMOVED || iState == TFGS_DELAYED )
		return FALSE;

	BOOL bMet = APMeetsCriteria( Goal, AP );
	BOOL bReverse = TF_IsGoalItem( Goal ) ? ( Goal->goal_activation & TFGI_REVERSE_AP ) != 0
	                                      : ( Goal->goal_activation & TFGA_REVERSE_AP ) != 0;

	return bMet != bReverse;
}

static void TF_ActivateElseGoal( CBaseEntity *Goal, CBasePlayer *AP )
{
	if ( !Goal->else_goal )
		return;

	CBaseEntity *pElse = Findgoal( Goal->else_goal );
	if ( pElse )
		ActivateDoResults( pElse, AP, Goal );
}

BOOL ActivationSucceeded( CBaseEntity *Goal, CBasePlayer *AP, CBaseEntity *ActivatingGoal )
{
	if ( TF_Prematch() && Goal->Classify() != CLASS_TFGOAL_TIMER )
		return FALSE;

	if ( TF_CriteriaPass( Goal, AP ) )
		return TRUE;

	TF_ActivateElseGoal( Goal, AP );
	return FALSE;
}

void DoResults( CBaseEntity *Goal, CBasePlayer *AP, BOOL bAddBonuses )
{
	if ( TF_Prematch() && Goal->Classify() != CLASS_TFGOAL_TIMER )
		return;

	DoResultsWork( Goal, AP, bAddBonuses );
}

BOOL ActivateDoResults( CBaseEntity *Goal, CBasePlayer *AP, CBaseEntity *ActivatingGoal )
{
	if ( TF_Prematch() && Goal->Classify() != CLASS_TFGOAL_TIMER )
		return FALSE;

	if ( !TF_CriteriaPass( Goal, AP ) )
	{
		TF_ActivateElseGoal( Goal, AP );
		return FALSE;
	}

	if ( Goal == ActivatingGoal || Goal->m_bAddBonuses == TRUE )
		DoResults( Goal, AP, TRUE );
	else if ( ActivatingGoal )
		DoResults( Goal, AP, ActivatingGoal->goal_result & TFGR_ADD_BONUSES );
	else
		DoResults( Goal, AP, FALSE );

	return TRUE;
}

static BOOL IsAffectedBy( CBaseEntity *Goal, CBasePlayer *Player, CBasePlayer *AP )
{
	if ( !Player->pev->playerclass )
		return FALSE;

	if ( ( Goal->goal_effects & TFGE_SAME_ENVIRONMENT )
	     && UTIL_PointContents( Goal->pev->origin ) != UTIL_PointContents( Player->pev->origin ) )
		return FALSE;

	if ( Goal->t_length != 0.0f && ( Goal->pev->origin - Player->pev->origin ).Length() <= Goal->t_length )
	{
		if ( !( Goal->goal_effects & TFGE_WALL ) )
			return TRUE;

		TraceResult tr;
		UTIL_TraceLine( Goal->pev->origin, Player->pev->origin, ignore_monsters, Goal->edict(), &tr );
		if ( tr.flFraction == 1.0f )
			return TRUE;
	}

	if ( Goal->Classify() != CLASS_TFGOAL_TIMER && AP )
	{
		BOOL bIsAP = ( Player == AP );

		if ( Goal->Classify() == CLASS_TFSPAWN && bIsAP )
			return TRUE;
		if ( ( Goal->goal_effects & TFGE_AP ) && bIsAP )
			return TRUE;
		if ( ( Goal->goal_effects & TFGE_AP_TEAM ) && AP->team_no == Player->team_no )
			return TRUE;
	}

	if ( ( Goal->goal_effects & TFGE_NOT_AP_TEAM ) && ( !AP || AP->team_no != Player->team_no ) )
		return TRUE;
	if ( ( Goal->goal_effects & TFGE_NOT_AP ) && Player != AP )
		return TRUE;

	// [tfc.so] maxammo_shells/nails are reused as "this team" / "every other team" filters
	if ( Goal->maxammo_shells && Goal->maxammo_shells == Player->team_no )
		return TRUE;
	if ( Goal->maxammo_nails && Goal->maxammo_nails != Player->team_no )
		return TRUE;

	return FALSE;
}

static string_t TF_PickOwnedMsg( CBaseEntity *Goal, CBaseEntity *pPlayer, string_t iszOwners, string_t iszNonOwners, string_t iszOther )
{
	if ( iszOwners && pPlayer->team_no == Goal->owned_by )
		return iszOwners;
	if ( iszNonOwners && pPlayer->team_no != Goal->owned_by )
		return iszNonOwners;
	return iszOther;
}

// [tfc.so] only CTF_Map (Quake-CTF style maps with no info_tfdetect) reaches this,
// and only for flag 1 / capture point 3 -- the 2 and 4 cases are dead there too.
static void TF_CTFGoalMessages( CBaseEntity *Goal, CBasePlayer *AP )
{
	int gno = Goal->goal_no;

	if ( gno != CTF_FLAG1 && gno != CTF_DROPOFF1 )
		return;

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *p = UTIL_PlayerByIndex( i );
		if ( !p )
			continue;

		int got, lost, capped, ours;
		if ( p->team_no == 2 )
		{
			got = CTF_FLAG1; lost = CTF_FLAG2; capped = CTF_DROPOFF1; ours = CTF_DROPOFF2;
		}
		else if ( p->team_no == 1 )
		{
			got = CTF_FLAG2; lost = CTF_FLAG1; capped = CTF_DROPOFF2; ours = CTF_DROPOFF1;
		}
		else
		{
			got = lost = capped = ours = -1;
			if ( gno == CTF_FLAG1 || gno == CTF_FLAG2 )
				lost = gno;
			else
				ours = gno;
		}

		const char *pszMsg = NULL;
		if ( gno == got )
			pszMsg = ( p == AP ) ? "You got the enemy flag!\n\nReturn to base!" : "Your team GOT the ENEMY flag!!";
		else if ( gno == lost )
			pszMsg = "Your flag has been TAKEN!!";
		else if ( gno == capped )
			pszMsg = ( p == AP ) ? "You CAPTURED the FLAG!!" : "Your flag was CAPTURED!!";
		else if ( gno == ours )
			pszMsg = "Your team CAPTURED the flag!!";

		if ( pszMsg )
			ClientPrint( p->pev, HUD_PRINTCENTER, pszMsg );
	}

	const char *pszName = STRING( AP->pev->netname );
	switch ( gno )
	{
	case CTF_FLAG1:
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, UTIL_VarArgs( "%s GOT the BLUE flag!", pszName ) );
		TF_LogTriggered( AP, "Stole_Blue_Flag" );
		AP->items |= IT_KEY1;
		break;
	case CTF_FLAG2:
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, UTIL_VarArgs( "%s GOT the RED flag!", pszName ) );
		TF_LogTriggered( AP, "Stole_Red_Flag" );
		AP->items |= IT_KEY2;
		break;
	case CTF_DROPOFF1:
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, UTIL_VarArgs( "%s CAPTURED the RED flag!", pszName ) );
		TF_LogTriggered( AP, "Captured_Red_Flag" );
		AP->items &= ~IT_KEY2;
		break;
	case CTF_DROPOFF2:
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, UTIL_VarArgs( "%s CAPTURED the BLUE flag!", pszName ) );
		TF_LogTriggered( AP, "Captured_Blue_Flag" );
		AP->items &= ~IT_KEY1;
		break;
	}
}

static void TF_SetSpawnGroupState( int gno, int iState )
{
	CBaseEntity *pSpawn = NULL;

	while ( ( pSpawn = UTIL_FindEntityByString( pSpawn, "netname", TF_SPAWN_CLASSNAME ) ) != NULL )
	{
		if ( pSpawn->group_no == gno )
			pSpawn->goal_state = iState;
	}
}

static void TF_PrintWithAP( CBaseEntity *pPlayer, string_t iszMsg, CBasePlayer *AP )
{
	ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, STRING( iszMsg ), STRING( AP->pev->netname ) );
}

static void TF_GoalPlayerMessages( CBaseEntity *Goal, CBaseEntity *p, CBasePlayer *AP )
{
	BOOL bAP = ( AP != NULL );

	if ( Goal->broadcast )
	{
		if ( !CTF_Map )
			UTIL_ShowMessage( STRING( Goal->broadcast ), p );
	}

	if ( !Goal->broadcast || !CTF_Map )
	{
		if ( Goal->netname_broadcast && !CTF_Map && bAP )
			TF_PrintWithAP( p, Goal->netname_broadcast, AP );
		if ( Goal->org_broadcast && !CTF_Map )
			ClientPrint( p->pev, HUD_PRINTCENTER, STRING( Goal->org_broadcast ) );
	}

	if ( Goal->speak )
		( (CBasePlayer *)p )->ClientHearVox( STRING( Goal->speak ) );

	if ( p == AP )
	{
		if ( Goal->pev->message && Goal->Classify() != CLASS_TFSPAWN )
			UTIL_ShowMessage( STRING( Goal->pev->message ), AP );
		if ( Goal->org_message && Goal->Classify() != CLASS_TFSPAWN )
			ClientPrint( AP->pev, HUD_PRINTCENTER, STRING( Goal->org_message ) );
		if ( Goal->AP_speak )
			AP->ClientHearVox( STRING( Goal->AP_speak ) );
		return;
	}

	string_t iszMsg;

	if ( bAP && AP->team_no == p->team_no )
	{
		iszMsg = TF_PickOwnedMsg( Goal, p, Goal->owners_team_broadcast, Goal->non_owners_team_broadcast, Goal->team_broadcast );
		if ( iszMsg )
			UTIL_ShowMessage( STRING( iszMsg ), p );

		iszMsg = TF_PickOwnedMsg( Goal, p, Goal->org_owners_team_broadcast, Goal->org_non_owners_team_broadcast, Goal->org_team_broadcast );
		if ( iszMsg )
			ClientPrint( p->pev, HUD_PRINTCENTER, STRING( iszMsg ) );

		iszMsg = TF_PickOwnedMsg( Goal, p, Goal->owners_team_speak, Goal->non_owners_team_speak, Goal->team_speak );
		if ( iszMsg )
			( (CBasePlayer *)p )->ClientHearVox( STRING( iszMsg ) );

		if ( Goal->netname_owners_team_broadcast && p->team_no == Goal->owned_by )
			TF_PrintWithAP( p, Goal->netname_owners_team_broadcast, AP );
		else if ( Goal->netname_team_broadcast )
			TF_PrintWithAP( p, Goal->netname_team_broadcast, AP );
		return;
	}

	iszMsg = TF_PickOwnedMsg( Goal, p, Goal->owners_team_broadcast, Goal->non_owners_team_broadcast, Goal->non_team_broadcast );
	if ( iszMsg )
		UTIL_ShowMessage( STRING( iszMsg ), p );

	iszMsg = TF_PickOwnedMsg( Goal, p, Goal->org_owners_team_broadcast, Goal->org_non_owners_team_broadcast, Goal->org_non_team_broadcast );
	if ( iszMsg )
		ClientPrint( p->pev, HUD_PRINTCENTER, STRING( iszMsg ) );

	iszMsg = TF_PickOwnedMsg( Goal, p, Goal->owners_team_speak, Goal->non_owners_team_speak, Goal->non_team_speak );
	if ( iszMsg )
		( (CBasePlayer *)p )->ClientHearVox( STRING( iszMsg ) );

	if ( !bAP )
		return;

	if ( Goal->netname_owners_team_broadcast && p->team_no == Goal->owned_by )
		TF_PrintWithAP( p, Goal->netname_owners_team_broadcast, AP );
	else if ( Goal->netname_non_team_broadcast )
		TF_PrintWithAP( p, Goal->netname_non_team_broadcast, AP );
}

static void DoResultsWork( CBaseEntity *Goal, CBasePlayer *AP, int bAddBonuses )
{
	if ( Goal->goal_state == TFGS_ACTIVE )
		return;

	if ( Goal->delay_time > 0.0f && Goal->goal_state != TFGS_DELAYED )
	{
		// [tfc.so] also spawns an unused TF_TIMER_DELAYEDGOAL timer that never dies; not reproduced.
		Goal->goal_state = TFGS_DELAYED;
		Goal->pev->enemy = AP ? AP->edict() : NULL;
		Goal->SetThink( &CBaseEntity::DelayedResult );
		Goal->pev->nextthink = gpGlobals->time + Goal->delay_time;
		Goal->weapon = bAddBonuses;
		return;
	}

	TF_ResolveOwnedBy( Goal );
	Goal->goal_state = TFGS_INACTIVE;

	if ( Goal->Classify() == CLASS_TFGOAL || Goal->Classify() == CLASS_TFGOAL_TIMER )
		Goal->pev->effects |= EF_NODRAW;

	if ( Goal->pev->noise )
		EMIT_SOUND_DYN( Goal->edict(), CHAN_VOICE, STRING( Goal->pev->noise ), 1.0f, ATTN_NORM, 0, PITCH_NORM );

	for ( int t = 0; t < 4; t++ )
	{
		if ( Goal->increase_team[t] )
			TeamFortress_TeamIncreaseScore( t + 1, Goal->increase_team[t] );
	}

	if ( Goal->increase_team_owned_by && Goal->owned_by )
		TeamFortress_TeamIncreaseScore( Goal->owned_by, Goal->increase_team_owned_by );

	if ( ( STRING( Goal->pev->netname ) )[0] )
	{
		if ( AP )
			TF_LogTriggered( AP, STRING( Goal->pev->netname ) );
		else
			UTIL_LogPrintf( "World triggered \"%s\"\n", STRING( Goal->pev->netname ) );
	}

	if ( AP && CTF_Map == TRUE )
		TF_CTFGoalMessages( Goal, AP );

	if ( Goal->remove_spawngroup )
		TF_SetSpawnGroupState( Goal->remove_spawngroup, TFGS_REMOVED );
	if ( Goal->restore_spawngroup )
		TF_SetSpawnGroupState( Goal->restore_spawngroup, TFGS_INACTIVE );

	if ( Goal->broadcast && !CTF_Map )
		UTIL_LogPrintf( "World triggered \"%s\"\n", STRING( Goal->broadcast ) );
	if ( Goal->netname_broadcast && !CTF_Map && AP )
		TF_LogTriggered( AP, STRING( Goal->netname_broadcast ) );

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pEnt = UTIL_PlayerByIndex( i );
		if ( !pEnt )
			continue;

		CBasePlayer *p = (CBasePlayer *)pEnt;
		TF_GoalPlayerMessages( Goal, p, AP );

		if ( !IsAffectedBy( Goal, p, AP ) )
			continue;

		if ( Goal->search_time != 0.0f && ( Goal->goal_effects & TFGE_TIMER_CHECK_AP ) && !APMeetsCriteria( Goal, p ) )
			continue;

		Apply_Results( Goal, p, AP, bAddBonuses );
	}

	if ( Goal->Classify() == CLASS_TFGOAL_TIMER || Goal->Classify() == CLASS_TFGOAL )
		Goal->goal_state = TFGS_ACTIVE;

	if ( Goal->goal_result & TFGR_ENDGAME )
	{
		TeamFortress_TeamShowScores( TRUE, NULL );
		if ( g_pGameRules->IsMultiplayer() )
			g_pGameRules->EndMultiplayerGame();
		return;
	}

	if ( Goal->m_flEndRoundTime != 0.0f )
		EndRound( Goal );

	DoGroupWork( Goal, AP );
	DoGoalWork( Goal, AP );

	int iClass = Goal->Classify();
	if ( iClass == CLASS_TFGOAL_TIMER || iClass == CLASS_TFGOAL_ITEM || iClass == CLASS_TFGOAL
	     || iClass == CLASS_TFSPAWN || Goal->do_triggerwork == TRUE )
		DoTriggerWork( Goal, AP );

	if ( iClass == CLASS_TFGOAL || iClass == CLASS_TFGOAL_TIMER )
		SetupRespawn( Goal );
}

void CBaseEntity::DelayedResult( void )
{
	CBasePlayer *AP = pev->enemy ? (CBasePlayer *)CBaseEntity::Instance( pev->enemy ) : NULL;

	if ( goal_state == TFGS_DELAYED )
		DoResults( this, AP, weapon );
}

static void Apply_Results( CBaseEntity *Goal, CBasePlayer *Player, CBasePlayer *AP, int bAddBonuses )
{
	if ( TF_IsGoalItem( Goal ) )
		Player->item_list |= Goal->item_list;

	if ( Player == AP && Goal->count && AP->team_no > 0 )
		TeamFortress_TeamIncreaseScore( AP->team_no, Goal->count );

	if ( bAddBonuses )
	{
		if ( Player->IsAlive() )
		{
			if ( Goal->pev->health > 0.0f )
				Player->TakeHealth( Goal->pev->health, DMG_GENERIC );
			if ( Goal->pev->health < 0.0f )
				Player->TakeDamage( Goal->pev, Goal->pev, -Goal->pev->health, DMG_IGNOREARMOR | DMG_NEVERGIB );
		}

		if ( Player->IsAlive() )
		{
			if ( Goal->pev->armortype > 0.0f )
				Player->pev->armortype = Goal->pev->armortype;
			else if ( Goal->pev->armorvalue > 0.0f )
				Player->pev->armortype = Player->armor_allowed;

			Player->pev->armorvalue += Goal->pev->armorvalue;

			if ( Goal->armorclass > 0 )
				Player->armorclass = Goal->armorclass;

			Player->GiveTFAmmo( Goal->ammo_shells, Goal->ammo_nails, Goal->ammo_rockets, Goal->ammo_cells );
			Player->ammo_medikit += Goal->ammo_medikit;
			Player->ammo_detpack += Goal->ammo_detpack;
			if ( Player->ammo_detpack > Player->maxammo_detpack )
				Player->ammo_detpack = Player->maxammo_detpack;

			if ( Player->tp_grenades_1 )
				Player->no_grenades_1 += Goal->no_grenades_1;
			if ( Player->tp_grenades_2 )
				Player->no_grenades_2 += Goal->no_grenades_2;

			// A primed grenade whose stock the goal just took away is dropped.
			if ( Player->tfstate & TFSTATE_GRENPRIMED )
			{
				BOOL bLost = ( Player->m_iPrimedGrenSlot == 1 && Player->no_grenades_1 <= 0 && Goal->no_grenades_1 < 0 )
				          || ( Player->m_iPrimedGrenSlot == 2 && Player->no_grenades_2 <= 0 && Goal->no_grenades_2 < 0 );
				if ( bLost )
					TeamFortress_CancelPrimedGrenade( Player );
			}

			// [tfc.so] a powerup from a goal item lasts for as long as it is carried (666 s, refreshed)
			BOOL bItem = TF_IsGoalItem( Goal );
			if ( Goal->invincible_finished > 0.0f )
			{
				Player->items |= IT_INVULNERABILITY;
				Player->invincible_finished = gpGlobals->time + Goal->invincible_finished;
				if ( bItem )
				{
					Player->tfstate |= TFSTATE_INVINCIBLE;
					Player->invincible_finished = gpGlobals->time + 666.0f;
				}
				Player->pev->renderfx = kRenderFxNone;
			}
			if ( Goal->invisible_finished > 0.0f )
			{
				Player->items |= IT_INVISIBILITY;
				Player->invisible_finished = gpGlobals->time + Goal->invisible_finished;
				if ( bItem )
				{
					Player->tfstate |= TFSTATE_INVISIBLE;
					Player->invisible_finished = gpGlobals->time + 666.0f;
				}
				Player->pev->renderfx = kRenderFxNone;
			}
			if ( Goal->super_damage_finished > 0.0f )
			{
				Player->items |= IT_QUAD;
				Player->super_damage_finished = gpGlobals->time + Goal->super_damage_finished;
				if ( bItem )
				{
					Player->tfstate |= TFSTATE_QUAD;
					Player->super_damage_finished = gpGlobals->time + 666.0f;
				}
				Player->pev->renderfx = kRenderFxNone;
			}
			if ( Goal->radsuit_finished > 0.0f )
			{
				Player->items |= IT_SUIT;
				Player->radsuit_finished = gpGlobals->time + Goal->radsuit_finished;
				if ( bItem )
				{
					Player->tfstate |= TFSTATE_RADSUIT;
					Player->radsuit_finished = gpGlobals->time + 666.0f;
				}
			}
		}

		Player->lives += Goal->lives;

		if ( Goal->pev->frags != 0.0f )
			Player->TF_AddFrags( (int)Goal->pev->frags );

		Player->TeamFortress_CheckClassStats();
	}

	if ( Player->pev->playerclass == PC_SPY && ( Goal->goal_result & TFGR_REMOVE_DISGUISE ) )
	{
		Player->immune_to_check = gpGlobals->time + 10.0f;
		Player->Spy_RemoveDisguise();
	}

	// "items" gives another goal item; a goal item never gives one itself.
	if ( Goal->items && !TF_IsGoalItem( Goal ) )
	{
		CBaseEntity *pItem = Finditem( Goal->items );
		if ( pItem && pItem != Goal )
			tfgoalitem_GiveToPlayer( pItem, Player, Goal );
	}

	// [tfc.so] axhitme doubles as "take this item back from the player"
	if ( Goal->axhitme )
	{
		CBaseEntity *pItem = Finditem( Goal->axhitme );
		if ( pItem && pItem->pev->owner == Player->edict() )
			tfgoalitem_RemoveFromPlayer( pItem, Player, GI_DROP_REMOVEGOAL );
	}

	if ( Goal->remove_item_group )
	{
		CBaseEntity *pItem = UTIL_FindEntityByClassname( NULL, TF_ITEM_CLASSNAME );
		while ( pItem )
		{
			CBaseEntity *pNext = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME );

			if ( pItem->group_no == Goal->remove_item_group && pItem->pev->owner == Player->edict() )
				tfgoalitem_RemoveFromPlayer( pItem, Player, GI_DROP_REMOVEGOAL );

			pItem = pNext;
		}
	}

	Player->DisplayLocalItemStatus( Goal );

	if ( Goal->goal_result & TFGR_DESTROY_BUILDINGS )
	{
		Player->no_entry_teleporter_message = 1;
		Player->no_exit_teleporter_message = 1;
		Player->no_sentry_message = 1;
		Player->no_dispenser_message = 1;
		Player->Engineer_RemoveBuildings();
		Player->TeamFortress_RemoveLiveGrenades();
		Player->TeamFortress_RemoveRockets();
		Player->RemovePipebombs();

		if ( Player->is_detpacking )
			Player->TeamFortress_DetpackStop();
		else if ( Player->TeamFortress_RemoveDetpacks() )
			Player->ammo_detpack++;
	}

	if ( ( Goal->goal_result & TFGR_FORCE_RESPAWN ) && Player->IsAlive() )
		Player->ForceRespawn();

	if ( Goal->replacement_model && !Player->replacement_model )
	{
		Player->replacement_model = Goal->replacement_model;
		Player->replacement_model_body = Goal->replacement_model_body;
		Player->replacement_model_skin = Goal->replacement_model_skin;
		Player->replacement_model_flags = Goal->replacement_model_flags;
		Player->TeamFortress_SetSkin();
	}
}

static void RemoveResults( CBaseEntity *Goal, CBasePlayer *Player )
{
	if ( TF_IsGoalItem( Goal ) )
	{
		if ( !( Player->item_list & Goal->item_list ) )
			return;
		if ( Goal->goal_activation & TFGI_DONTREMOVERES )
			return;

		Player->item_list &= ~Goal->item_list;
	}

	if ( Goal->pev->health > 0.0f )
		Player->TakeDamage( Goal->pev, Goal->pev, Goal->pev->health, DMG_IGNOREARMOR );
	if ( Goal->pev->health < 0.0f )
		Player->TakeHealth( -Goal->pev->health, DMG_GENERIC );

	Player->lives -= Goal->lives;
	Player->pev->armortype -= Goal->pev->armortype;
	Player->pev->armorvalue -= Goal->pev->armorvalue;
	Player->armorclass &= ~Goal->armorclass;

	// [tfc.so] adds the frags again rather than taking them back
	if ( Goal->pev->frags != 0.0f )
		Player->TF_AddFrags( (int)Goal->pev->frags );

	Player->ammo_shells -= Goal->ammo_shells;
	Player->ammo_nails -= Goal->ammo_nails;
	Player->ammo_rockets -= Goal->ammo_rockets;
	Player->ammo_cells -= Goal->ammo_cells;
	Player->ammo_medikit -= Goal->ammo_medikit;
	Player->ammo_detpack -= Goal->ammo_detpack;
	if ( Player->ammo_detpack > Player->maxammo_detpack )
		Player->ammo_detpack = Player->maxammo_detpack;

	Player->no_grenades_1 -= Goal->no_grenades_1;
	Player->no_grenades_2 -= Goal->no_grenades_2;

	if ( ( Player->tfstate & TFSTATE_GRENPRIMED ) && ( Player->no_grenades_1 <= 0 || Player->no_grenades_2 <= 0 ) )
		TeamFortress_CancelPrimedGrenade( Player );

	// Another carried item may still grant the same powerup.
	BOOL puinvin = FALSE, puinvis = FALSE, puquad = FALSE, purad = FALSE;
	CBaseEntity *pItem = NULL;
	while ( ( pItem = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME ) ) != NULL )
	{
		if ( pItem->pev->owner != Player->edict() || pItem == Goal )
			continue;

		if ( pItem->invincible_finished > 0.0f ) puinvin = TRUE;
		if ( pItem->invisible_finished > 0.0f ) puinvis = TRUE;
		if ( pItem->super_damage_finished > 0.0f ) puquad = TRUE;
		if ( pItem->radsuit_finished > 0.0f ) purad = TRUE;
	}

	// [tfc.so] a lost item's powerup keeps running for its own duration
	if ( Goal->invincible_finished > 0.0f && !puinvin )
	{
		Player->tfstate &= ~TFSTATE_INVINCIBLE;
		Player->items |= IT_INVULNERABILITY;
		Player->invincible_finished = gpGlobals->time + Goal->invincible_finished;
	}
	if ( Goal->invisible_finished > 0.0f && !puinvis )
	{
		Player->tfstate &= ~TFSTATE_INVISIBLE;
		Player->items |= IT_INVISIBILITY;
		Player->invisible_finished = gpGlobals->time + Goal->invisible_finished;
	}
	if ( Goal->super_damage_finished > 0.0f && !puquad )
	{
		Player->tfstate &= ~TFSTATE_QUAD;
		Player->items |= IT_QUAD;
		Player->super_damage_finished = gpGlobals->time + Goal->super_damage_finished;
	}
	if ( Goal->radsuit_finished > 0.0f && !purad )
	{
		Player->tfstate &= ~TFSTATE_RADSUIT;
		Player->items |= IT_SUIT;
		Player->radsuit_finished = gpGlobals->time + Goal->radsuit_finished;
	}

	Player->TeamFortress_CheckClassStats();

	if ( Goal->replacement_model && Goal->replacement_model == Player->replacement_model )
	{
		Player->replacement_model = iStringNull;
		Player->replacement_model_body = 0;
		Player->replacement_model_skin = 0;
		Player->replacement_model_flags = 0;
		Player->TeamFortress_SetSkin();
	}
}

static void TF_StartReturnTimer( CBaseEntity *Item, float flDelay, int iMethod )
{
	CBaseEntity *pTimer = Item->CreateTimer( TF_TIMER_RETURNITEM );
	if ( !pTimer )
		return;

	pTimer->weapon = iMethod;
	pTimer->SetThink( &CBaseEntity::ReturnItem );
	pTimer->pev->nextthink = gpGlobals->time + flDelay;
}

void DoGoalWork( CBaseEntity *Goal, CBasePlayer *AP )
{
	CBaseEntity *pGoal;

	if ( Goal->activate_goal_no && ( pGoal = Findgoal( Goal->activate_goal_no ) ) != NULL )
		ActivateDoResults( pGoal, AP, Goal );

	if ( Goal->inactivate_goal_no && ( pGoal = Findgoal( Goal->inactivate_goal_no ) ) != NULL )
		InactivateGoal( pGoal );

	if ( Goal->restore_goal_no && ( pGoal = Findgoal( Goal->restore_goal_no ) ) != NULL )
		RestoreGoal( pGoal );

	if ( Goal->remove_goal_no && ( pGoal = Findgoal( Goal->remove_goal_no ) ) != NULL )
		RemoveGoal( pGoal );

	if ( Goal->return_item_no )
	{
		CBaseEntity *pItem = Finditem( Goal->return_item_no );
		if ( pItem )
		{
			if ( pItem->goal_state == TFGS_ACTIVE )
				tfgoalitem_RemoveFromPlayer( pItem, (CBasePlayer *)TF_OwnerOf( pItem ), GI_DROP_REMOVEGOAL );

			// [tfc.so] this path tags the return as GI_RET_TIME, not GI_RET_GOAL
			TF_StartReturnTimer( pItem, 0.1f, GI_RET_TIME );
			pItem->pev->solid = SOLID_NOT;
			UTIL_SetOrigin( pItem->pev, pItem->pev->origin );
		}
	}

	if ( Goal->remove_spawnpoint )
	{
		CBaseEntity *pSpawn = Findteamspawn( Goal->remove_spawnpoint );
		if ( pSpawn )
			pSpawn->goal_state = TFGS_REMOVED;
	}

	if ( Goal->restore_spawnpoint )
	{
		CBaseEntity *pSpawn = Findteamspawn( Goal->restore_spawnpoint );
		if ( pSpawn && pSpawn->goal_state == TFGS_REMOVED )
			pSpawn->goal_state = TFGS_INACTIVE;
	}
}

void DoGroupWork( CBaseEntity *Goal, CBasePlayer *AP )
{
	CBaseEntity *pGoal;

	if ( Goal->all_active )
	{
		if ( !Goal->last_impulse )
		{
			ALERT( at_console, "Goal %d has .all_active specified, but no .last_impulse\n", Goal->goal_no );
		}
		else if ( GroupInState( Goal->all_active, TFGS_ACTIVE ) )
		{
			pGoal = Findgoal( Goal->last_impulse );
			if ( pGoal )
				DoResults( pGoal, AP, Goal->goal_result & TFGR_ADD_BONUSES );
		}
	}

	if ( Goal->activate_group_no )
	{
		pGoal = NULL;
		while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
		{
			if ( pGoal->group_no == Goal->activate_group_no )
				ActivateDoResults( pGoal, AP, Goal );
		}
	}

	if ( Goal->inactivate_group_no )
	{
		pGoal = NULL;
		while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
		{
			if ( pGoal->group_no == Goal->inactivate_group_no )
				InactivateGoal( pGoal );
		}
	}

	if ( Goal->remove_group_no )
	{
		pGoal = NULL;
		while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
		{
			if ( pGoal->group_no == Goal->remove_group_no )
				RemoveGoal( pGoal );
		}
	}

	if ( Goal->restore_group_no )
	{
		pGoal = NULL;
		while ( ( pGoal = UTIL_FindEntityByClassname( pGoal, TF_GOAL_CLASSNAME ) ) != NULL )
		{
			if ( pGoal->group_no == Goal->restore_group_no )
				RestoreGoal( pGoal );
		}
	}
}

// [tfc.so] goal items reuse distance/pev->pain_finished ("all carried") and
// pev->speed/attack_finished ("all carried by one player") as group triggers.
static void DoItemGroupWork( CBaseEntity *Item, CBasePlayer *AP )
{
	CBaseEntity *te;

	if ( Item->distance != 0.0f )
	{
		if ( Item->pev->pain_finished == 0.0f )
			ALERT( at_console, "GoalItem %d has .distance specified, but no .pain_finished\n", Item->goal_no );

		BOOL bAllCarried = TRUE;
		te = NULL;
		while ( ( te = UTIL_FindEntityByClassname( te, TF_ITEM_CLASSNAME ) ) != NULL )
		{
			if ( (float)te->group_no == Item->distance && te->goal_state != TFGS_ACTIVE )
			{
				bAllCarried = FALSE;
				break;
			}
		}

		if ( bAllCarried )
		{
			CBaseEntity *pGoal = Findgoal( (int)Item->pev->pain_finished );
			if ( pGoal )
				DoResults( pGoal, AP, Item->goal_result & TFGR_ADD_BONUSES );
		}
	}

	if ( Item->pev->speed != 0.0f )
	{
		if ( Item->attack_finished == 0.0f )
			ALERT( at_console, "GoalItem %d has .speed specified, but no .attack_finished\n", Item->goal_no );

		CBaseEntity *pCarrier = NULL;
		te = NULL;
		while ( ( te = UTIL_FindEntityByClassname( te, TF_ITEM_CLASSNAME ) ) != NULL )
		{
			if ( (float)te->group_no != Item->pev->speed )
				continue;

			if ( te->goal_state != TFGS_ACTIVE )
				return;

			CBaseEntity *pOwner = TF_OwnerOf( te );
			if ( !pCarrier )
				pCarrier = pOwner;
			else if ( pCarrier != pOwner )
				return;
		}

		CBaseEntity *pGoal = Findgoal( (int)Item->attack_finished );
		if ( pGoal )
			DoResults( pGoal, AP, Item->goal_result & TFGR_ADD_BONUSES );
	}
}

static void DoTriggerWork( CBaseEntity *Goal, CBasePlayer *AP )
{
	edict_t *pent;

	if ( Goal->killtarget )
	{
		pent = NULL;
		while ( !FNullEnt( pent = FIND_ENTITY_BY_TARGETNAME( pent, STRING( Goal->killtarget ) ) ) )
			UTIL_Remove( CBaseEntity::Instance( pent ) );
	}

	if ( Goal->pev->target )
	{
		pent = NULL;
		while ( !FNullEnt( pent = FIND_ENTITY_BY_TARGETNAME( pent, STRING( Goal->pev->target ) ) ) )
		{
			CBaseEntity *pTarget = CBaseEntity::Instance( pent );
			if ( pTarget && !( pTarget->pev->flags & FL_KILLME ) )
				pTarget->Use( AP, Goal, USE_TOGGLE, 0 );
		}
	}
}

static void SetupRespawn( CBaseEntity *Goal )
{
	Goal->m_bAddBonuses = FALSE;

	if ( Goal->goal_result & TFGR_SINGLE )
	{
		RemoveGoal( Goal );
		return;
	}

	if ( Goal->Classify() == CLASS_TFGOAL_TIMER )
	{
		InactivateGoal( Goal );
		Goal->SetThink( &CBaseEntity::tfgoal_timer_tick );
		Goal->pev->nextthink = gpGlobals->time + Goal->search_time;
		return;
	}

	if ( Goal->wait > 0.0f )
	{
		Goal->SetThink( &CBaseEntity::DoRespawn );
		Goal->pev->nextthink = gpGlobals->time + Goal->wait;
		return;
	}

	if ( Goal->wait == -1.0f )
		return;

	InactivateGoal( Goal );
}

void CBaseEntity::DoRespawn( void )
{
	RestoreGoal( this );
	InactivateGoal( this );
}

static void EndRound( CBaseEntity *Goal )
{
	UTIL_ScreenFadeAll( g_vecZero, 0.3f, Goal->m_flEndRoundTime, 255, FFADE_OUT | FFADE_MODULATE );
	TeamFortress_TeamShowScores( TRUE, NULL );

	const char *pszWinMsg = "";
	int iWinner = 1;

	if ( Goal->m_iszEndRoundMsg_Team1_Win )
	{
		int iBest = -99990;
		for ( int t = 1; t <= 4; t++ )
		{
			int iScore = TeamFortress_TeamGetScoreFrags( t );
			if ( iScore > iBest )
			{
				iWinner = t;
				iBest = iScore;
			}
		}

		string_t iszWin[5] = { 0, Goal->m_iszEndRoundMsg_Team1_Win, Goal->m_iszEndRoundMsg_Team2_Win,
		                       Goal->m_iszEndRoundMsg_Team3_Win, Goal->m_iszEndRoundMsg_Team4_Win };
		pszWinMsg = STRING( iszWin[iWinner] );
	}

	string_t iszLose[5] = { 0, Goal->m_iszEndRoundMsg_Team1_Lose, Goal->m_iszEndRoundMsg_Team2_Lose,
	                        Goal->m_iszEndRoundMsg_Team3_Lose, Goal->m_iszEndRoundMsg_Team4_Lose };
	string_t iszTeam[5] = { 0, Goal->m_iszEndRoundMsg_Team1, Goal->m_iszEndRoundMsg_Team2,
	                        Goal->m_iszEndRoundMsg_Team3, Goal->m_iszEndRoundMsg_Team4 };

	no_cease_fire_text = TRUE;
	cease_fire = TRUE;

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "player" ) ) != NULL )
	{
		if ( !pEnt->pev || FNullEnt( pEnt->edict() ) )
			break;

		CBasePlayer *p = (CBasePlayer *)pEnt;

		if ( p->pev->iuser1 == 0 )
		{
			p->pev->iuser4 = 1;
			p->m_iHideHUD |= HIDEHUD_WEAPONS | HIDEHUD_HEALTH;
			p->tfstate |= TFSTATE_CANT_MOVE;
		}
		p->TeamFortress_SetSpeed();

		int t = ( p->team_no >= 1 && p->team_no <= 4 ) ? p->team_no : 0;
		const char *pszMsg;

		if ( Goal->m_iszEndRoundMsg_OwnedBy && p->team_no == Goal->owned_by )
			pszMsg = STRING( Goal->m_iszEndRoundMsg_OwnedBy );
		else if ( Goal->m_iszEndRoundMsg_NonOwnedBy && p->team_no != Goal->owned_by )
			pszMsg = STRING( Goal->m_iszEndRoundMsg_NonOwnedBy );
		else if ( Goal->m_iszEndRoundMsg_Team1_Win )
			pszMsg = ( p->team_no == iWinner ) ? pszWinMsg : STRING( iszLose[t] );
		else
			pszMsg = STRING( iszTeam[t] );

		UTIL_ShowMessage( pszMsg, p );
	}

	CBaseEntity *pTimer = Goal->CreateTimer( TF_TIMER_ENDROUND );
	if ( pTimer )
	{
		pTimer->SetThink( &CBaseEntity::EndRoundEnd );
		pTimer->pev->nextthink = gpGlobals->time + Goal->m_flEndRoundTime;
	}
}

// Runs on the TF_TIMER_ENDROUND timer.
void CBaseEntity::EndRoundEnd( void )
{
	cease_fire = FALSE;

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "player" ) ) != NULL )
	{
		if ( !pEnt->pev || FNullEnt( pEnt->edict() ) )
			break;

		CBasePlayer *p = (CBasePlayer *)pEnt;

		if ( p->pev->iuser1 == 0 )
		{
			p->pev->iuser4 = 0;
			p->tfstate &= ~TFSTATE_CANT_MOVE;
			p->m_iHideHUD = 0;
		}
		p->TeamFortress_SetSpeed();
	}

	pev->flags |= FL_KILLME;
	no_cease_fire_text = FALSE;
}

void CBaseEntity::tfgoal_touch( CBaseEntity *pOther )
{
	if ( TF_Prematch() )
		return;
	if ( !( goal_activation & TFGA_TOUCH ) )
		return;
	if ( pOther->Classify() != CLASS_PLAYER || !pOther->IsAlive() )
		return;
	if ( goal_state == TFGS_ACTIVE )
		return;

	// CTF capture points only work while your own flag is at home.
	if ( CTF_Map )
	{
		int iFlag = 0;
		if ( goal_no == CTF_DROPOFF1 && pOther->team_no == 1 )
			iFlag = CTF_FLAG1;
		else if ( goal_no == CTF_DROPOFF2 && pOther->team_no == 2 )
			iFlag = CTF_FLAG2;

		if ( iFlag )
		{
			CBaseEntity *pFlag = Finditem( iFlag );
			if ( !pFlag || pFlag->goal_state == TFGS_ACTIVE || pFlag->pev->origin != pFlag->pev->oldorigin )
				return;
		}
	}

	ActivateDoResults( this, (CBasePlayer *)pOther, this );
}

void CBaseEntity::tfgoal_timer_tick( void )
{
	if ( goal_state == TFGS_REMOVED )
		return;

	if ( TF_Prematch() )
	{
		SetThink( &CBaseEntity::tfgoal_timer_tick );
		pev->nextthink = cb_prematch_time + search_time;
		return;
	}

	if ( !APMeetsCriteria( this, NULL ) )
	{
		SetThink( &CBaseEntity::tfgoal_timer_tick );
		pev->nextthink = gpGlobals->time + search_time;
		return;
	}

	DoResults( this, NULL, TRUE );
}

void tfgoalitem_GiveToPlayer( CBaseEntity *Item, CBasePlayer *AP, CBaseEntity *Goal )
{
	if ( Item->redrop_count )
		Item->SetThink( NULL );

	Item->pev->owner = AP->edict();
	Item->pev->movetype = MOVETYPE_FOLLOW;
	Item->pev->aiment = AP->edict();

	if ( Item->pev->model )
	{
		CBaseAnimating *pAnim = (CBaseAnimating *)Item;

		Item->pev->effects &= ~EF_NODRAW;
		Item->pev->sequence = pAnim->LookupSequence( "carried" );
		if ( Item->pev->sequence != -1 )
		{
			pAnim->ResetSequenceInfo();
			Item->pev->frame = 0;
		}
	}

	Item->pev->solid = SOLID_NOT;

	if ( Item->goal_activation & TFGI_GLOW )
		AP->pev->effects |= EF_BRIGHTFIELD;
	if ( Item->goal_activation & TFGI_SLOW )
		AP->TeamFortress_SetSpeed();
	if ( Item->speed_reduction )
		AP->TeamFortress_SetSpeed();

	if ( Item->goal_activation & TFGI_ITEMGLOWS )
	{
		Item->pev->renderfx = kRenderFxNone;
		Item->pev->rendercolor = g_vecZero;
		Item->pev->renderamt = 0;
	}

	AP->items |= Item->items & ( IT_KEY1 | IT_KEY2 | IT_KEY3 | IT_KEY4 );

	if ( Item != Goal && ( Goal->goal_result & TFGR_NO_ITEM_RESULTS ) )
	{
		Item->goal_state = TFGS_ACTIVE;
		return;
	}

	if ( Item->goal_result & TFGR_REMOVE_DISGUISE )
		AP->is_unableto_spy_or_teleport = 1;

	DoResults( Item, AP, TRUE );
	DoItemGroupWork( Item, AP );
}

static void TF_ItemGlowOnGround( CBaseEntity *Item )
{
	if ( !( Item->goal_activation & TFGI_ITEMGLOWS ) )
		return;

	Item->pev->renderfx = kRenderFxGlowShell;
	if ( Item->owned_by >= 1 && Item->owned_by <= 4 )
		Item->pev->rendercolor = rgbcolors[Item->owned_by];
	else
		Item->pev->rendercolor = rgbcolors[0];
	Item->pev->renderamt = 100;
}

void tfgoalitem_RemoveFromPlayer( CBaseEntity *Item, CBasePlayer *AP, int iMethod )
{
	TF_ResolveOwnedBy( Item );

	// What the player keeps from other carried items.
	BOOL lighton = FALSE, spyoff = FALSE;
	int iKeys = 0;
	CBaseEntity *te = NULL;
	while ( ( te = UTIL_FindEntityByClassname( te, TF_ITEM_CLASSNAME ) ) != NULL )
	{
		if ( te->pev->owner != AP->edict() || te == Item )
			continue;

		if ( te->goal_activation & TFGI_GLOW )
			lighton = TRUE;
		iKeys |= te->items & ( IT_KEY1 | IT_KEY2 | IT_KEY3 | IT_KEY4 );
		if ( te->goal_result & TFGR_REMOVE_DISGUISE )
			spyoff = TRUE;
	}

	if ( !lighton && AP->invincible_finished <= gpGlobals->time + 3.0f && AP->super_damage_finished <= gpGlobals->time + 3.0f )
		AP->pev->effects &= ~( EF_BRIGHTFIELD | EF_DIMLIGHT );

	TF_ItemGlowOnGround( Item );

	if ( !spyoff )
		AP->is_unableto_spy_or_teleport = 0;

	AP->items &= ~( ( IT_KEY1 | IT_KEY2 | IT_KEY3 | IT_KEY4 ) & ~iKeys );

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pEnt = UTIL_PlayerByIndex( i );
		if ( pEnt && IsAffectedBy( Item, (CBasePlayer *)pEnt, AP ) )
			RemoveResults( Item, (CBasePlayer *)pEnt );
	}

	if ( Item->pev->model )
	{
		CBaseAnimating *pAnim = (CBaseAnimating *)Item;

		Item->pev->sequence = pAnim->LookupSequence( "not_carried" );
		if ( Item->pev->sequence != -1 )
		{
			pAnim->ResetSequenceInfo();
			Item->pev->frame = 0;
		}
	}

	if ( iMethod == GI_DROP_REMOVEGOAL )
	{
		Item->pev->owner = NULL;

		if ( Item->goal_activation & TFGI_RETURN_GOAL )
		{
			TF_StartReturnTimer( Item, 0.5f, GI_RET_GOAL );
		}
		else
		{
			Item->pev->solid = SOLID_NOT;
			Item->pev->effects |= EF_NODRAW;
			Item->pev->movetype = MOVETYPE_NONE;
			Item->pev->aiment = NULL;
		}

		AP->TeamFortress_SetSpeed();
		return;
	}

	if ( iMethod != GI_DROP_PLAYERDEATH && iMethod != GI_DROP_PLAYERDROP )
		return;

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *p = UTIL_PlayerByIndex( i );
		if ( !p )
			continue;

		string_t iszOrg;
		if ( p->team_no == Item->owned_by )
		{
			if ( Item->team_drop )
				UTIL_ShowMessage( STRING( Item->team_drop ), p );
			if ( Item->netname_team_drop )
				TF_PrintWithAP( p, Item->netname_team_drop, AP );
			iszOrg = Item->org_team_drop;
		}
		else
		{
			if ( Item->non_team_drop )
				UTIL_ShowMessage( STRING( Item->non_team_drop ), p );
			if ( Item->netname_non_team_drop )
				TF_PrintWithAP( p, Item->netname_non_team_drop, AP );
			iszOrg = Item->org_non_team_drop;
		}

		if ( iszOrg )
			ClientPrint( p->pev, HUD_PRINTCENTER, STRING( iszOrg ) );
	}

	if ( Item->goal_activation & TFGI_RETURN_DROP )
	{
		TF_StartReturnTimer( Item, 0.5f, iMethod != GI_DROP_PLAYERDEATH ? GI_RET_DROP_LIVING : GI_RET_DROP_DEAD );
	}
	else if ( Item->goal_activation & TFGI_DROP )
	{
		BOOL bAlive = ( iMethod == GI_DROP_PLAYERDROP && ( Item->goal_activation & TFGI_CANBEDROPPED ) );
		tfgoalitem_drop( Item, bAlive, AP );
	}
	else
	{
		Item->pev->owner = NULL;
		Item->pev->nextthink = gpGlobals->time;
		Item->SetThink( &CBaseEntity::SUB_Remove );
		AP->TeamFortress_SetSpeed();
		return;
	}

	Item->pev->owner = NULL;
	Item->pev->flags &= ~FL_ONGROUND;
	UTIL_SetSize( Item->pev, Item->goal_min, Item->goal_max );
	AP->TeamFortress_SetSpeed();
}

// Common tail of a drop and a re-drop.
static void TF_ItemToss( CBaseEntity *Item )
{
	Item->pev->movetype = MOVETYPE_TOSS;
	Item->pev->aiment = NULL;
	Item->pev->velocity.z = 400;

	if ( Item->redrop_count > 1 )
	{
		Item->pev->velocity.x = RANDOM_FLOAT( -50, 50 );
		Item->pev->velocity.y = RANDOM_FLOAT( -50, 50 );
	}

	Item->goal_state = TFGS_INACTIVE;
	Item->pev->angles = g_vecZero;
	Item->pev->solid = ( Item->goal_activation & TFGI_SOLID ) ? SOLID_BBOX : SOLID_TRIGGER;
	Item->pev->effects &= ~EF_NODRAW;
	UTIL_SetSize( Item->pev, Item->goal_min, Item->goal_max );
	Item->redrop_count++;
	Item->SetThink( &CBaseEntity::tfgoalitem_dropthink );
	Item->pev->nextthink = gpGlobals->time + 5.0f;
}

void tfgoalitem_drop( CBaseEntity *Item, BOOL PAlive, CBasePlayer *P )
{
	CBaseEntity *pOwner = TF_OwnerOf( Item );
	Vector vecOrigin = pOwner->pev->origin + Vector( 0, 0, ( pOwner->pev->flags & FL_DUCKING ) ? 8 : 26 );

	Item->redrop_count = 0;
	Item->SetTouch( &CBaseEntity::item_tfgoal_touch );
	Item->redrop_origin = vecOrigin;
	Item->pev->origin = vecOrigin;
	UTIL_SetOrigin( Item->pev, Item->pev->origin );

	TF_ItemToss( Item );
	Item->pev->owner = P->edict();

	if ( PAlive == TRUE )
	{
		UTIL_MakeAimVectors( P->pev->angles );
		Item->pev->velocity = gpGlobals->v_forward * 400 + gpGlobals->v_up * 200;
		Item->SetTouch( NULL );
		Item->SetThink( &CBaseEntity::tfgoalitem_droptouch );
		Item->pev->nextthink = gpGlobals->time + 0.75f;
		Item->pev->enemy = P->edict();
		( (CTFGoalItem *)Item )->m_flDroppedAt = gpGlobals->time;
	}
}

void CBaseEntity::DoDrop( Vector p_vecOrigin )
{
	pev->origin = p_vecOrigin;
	UTIL_SetOrigin( pev, pev->origin );
	TF_ItemToss( this );
}

void CBaseEntity::tfgoalitem_dropthink( void )
{
	pev->movetype = MOVETYPE_TOSS;
	pev->aiment = NULL;

	if ( drop_time == 0.0f )
		return;

	int iContents = UTIL_PointContents( pev->origin );

	if ( iContents == CONTENTS_SLIME )
	{
		pev->nextthink = gpGlobals->time + drop_time * 0.25f;
	}
	else if ( iContents == CONTENTS_LAVA )
	{
		pev->nextthink = gpGlobals->time + 5.0f;
	}
	else if ( iContents == CONTENTS_SOLID || iContents == CONTENTS_SKY )
	{
		if ( redrop_count <= 2 )
		{
			DoDrop( redrop_origin );
			return;
		}
		pev->nextthink = gpGlobals->time + 2.0f;
	}
	else
	{
		pev->nextthink = gpGlobals->time + drop_time;
	}

	SetThink( &CBaseEntity::tfgoalitem_remove );
}

void CBaseEntity::tfgoalitem_droptouch( void )
{
	SetTouch( &CBaseEntity::item_tfgoal_touch );
	SetThink( &CBaseEntity::tfgoalitem_dropthink );
	pev->nextthink = gpGlobals->time + 4.25f;
}

void CBaseEntity::tfgoalitem_remove( void )
{
	if ( goal_state == TFGS_ACTIVE )
		return;

	if ( goal_activation & TFGI_RETURN_REMOVE )
		TF_StartReturnTimer( this, 0.1f, GI_RET_TIME );
	else
		dremove( this );
}

// Runs on a TF_TIMER_RETURNITEM timer owned by the item.
void CBaseEntity::ReturnItem( void )
{
	CBaseEntity *Item = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( !Item )
	{
		UTIL_Remove( this );
		return;
	}

	Item->SetThink( NULL );
	Item->goal_state = TFGS_INACTIVE;
	Item->pev->solid = ( ( Item->goal_activation & TFGI_SOLID ) && TF_IsGoalItem( Item ) ) ? SOLID_BBOX : SOLID_TRIGGER;
	Item->pev->movetype = MOVETYPE_NONE;
	Item->pev->aiment = NULL;
	Item->SetTouch( &CBaseEntity::item_tfgoal_touch );
	Item->pev->angles = g_vecZero;
	Item->pev->origin = Item->pev->oldorigin;
	Item->pev->effects &= ~EF_NODRAW;
	UTIL_SetOrigin( Item->pev, Item->pev->origin );
	EMIT_SOUND_DYN( Item->edict(), CHAN_WEAPON, "items/itembk2.wav", 1.0f, ATTN_NORM, 0, 150 );

	tfgoalitem_checkgoalreturn( Item );

	if ( weapon != GI_RET_GOAL && ( Item->pev->noise3 || Item->noise4 || Item->org_noise3 || Item->org_noise4 ) )
	{
		for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		{
			CBaseEntity *p = UTIL_PlayerByIndex( i );
			if ( !p )
				continue;

			TF_ResolveOwnedBy( Item );

			if ( p->team_no == Item->owned_by )
			{
				if ( Item->pev->noise3 )
					UTIL_ShowMessage( STRING( Item->pev->noise3 ), p );
				else if ( Item->org_noise3 )
					ClientPrint( p->pev, HUD_PRINTCENTER, STRING( Item->org_noise3 ) );
			}
			else
			{
				if ( Item->noise4 )
					UTIL_ShowMessage( STRING( Item->noise4 ), p );
				else if ( Item->org_noise4 )
					ClientPrint( p->pev, HUD_PRINTCENTER, STRING( Item->org_noise4 ) );
			}
		}
	}

	UTIL_Remove( this );
}

// [tfc.so] an item's pev->impulse names the goal to fire when it gets home.
void tfgoalitem_checkgoalreturn( CBaseEntity *Item )
{
	if ( !Item->pev->impulse )
		return;

	CBaseEntity *pGoal = Findgoal( Item->pev->impulse );
	if ( pGoal )
		ActivateDoResults( pGoal, NULL, Item );
}

// [tfc.so] a CTF_Map returned flag is judged by the item's own team_no, not the toucher's.
static void TF_CTFReturnFlag( CBaseEntity *Item, CBaseEntity *pOther )
{
	BOOL bBlue = ( Item->goal_no == CTF_FLAG1 );

	UTIL_ClientPrintAll( HUD_PRINTNOTIFY, UTIL_VarArgs( bBlue ? "%s RETURNED the BLUE flag!" : "%s RETURNED the RED flag!",
	                                                    STRING( pOther->pev->netname ) ) );
	TF_LogTriggered( pOther, bBlue ? "Returned_Blue_Flag" : "Returned_Red_Flag" );

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *p = UTIL_PlayerByIndex( i );
		if ( !p )
			continue;

		if ( p->team_no == 1 )
		{
			if ( bBlue )
				ClientPrint( p->pev, HUD_PRINTCENTER, "Your flag was RETURNED!!\n" );
		}
		else if ( bBlue )
		{
			ClientPrint( p->pev, HUD_PRINTCENTER, "The ENEMY flag was RETURNED!!\n" );
		}
		else if ( p->team_no == 2 && Item->goal_no == CTF_FLAG2 )
		{
			ClientPrint( p->pev, HUD_PRINTCENTER, "Your flag was RETURNED!!\n" );
		}
	}

	Item->goal_state = TFGS_INACTIVE;
	Item->pev->solid = SOLID_TRIGGER;
	Item->SetTouch( &CBaseEntity::item_tfgoal_touch );
	Item->pev->origin = Item->pev->oldorigin;
	UTIL_SetOrigin( Item->pev, Item->pev->origin );
	EMIT_SOUND_DYN( Item->edict(), CHAN_WEAPON, "items/itembk2.wav", 1.0f, ATTN_NORM, 0, 150 );
}

void CBaseEntity::item_tfgoal_touch( CBaseEntity *pOther )
{
	if ( pOther->Classify() != CLASS_PLAYER || !pOther->IsAlive() )
		return;
	if ( TF_Prematch() )
		return;
	if ( pOther->is_feigning )
		return;
	if ( replacement_model && pOther->replacement_model )
		return;

	CTFGoalItem *pThis = (CTFGoalItem *)this;

	// The player who threw it cannot catch it again for 5 seconds.
	if ( pev->enemy && pThis->m_flDroppedAt != 0.0f && pev->enemy == pOther->edict()
	     && pThis->m_flDroppedAt + 5.0f > gpGlobals->time )
		return;

	pThis->m_flDroppedAt = 0;

	TraceResult tr;
	UTIL_TraceLine( Center(), pOther->Center(), ignore_monsters, edict(), &tr );
	if ( tr.flFraction != 1.0f && tr.pHit != pOther->edict() )
		return;

	if ( CTF_Map == TRUE )
	{
		if ( pev->origin == pev->oldorigin )
		{
			if ( pOther->team_no == 1 && goal_no == CTF_FLAG1 )
				return;
			if ( pOther->team_no == 2 && goal_no == CTF_FLAG2 )
				return;
		}
		else if ( team_no == 1 )
		{
			TF_CTFReturnFlag( this, pOther );
			return;
		}
	}

	if ( !ActivationSucceeded( this, (CBasePlayer *)pOther, NULL ) )
		return;

	tfgoalitem_GiveToPlayer( this, (CBasePlayer *)pOther, this );

	if ( pOther->pev->health > 0.0f )
		goal_state = TFGS_ACTIVE;
}

void DisplayItemStatus( CBaseEntity *Goal, CBasePlayer *Player, CBaseEntity *Item )
{
	TF_ResolveOwnedBy( Item );

	BOOL bOwners = ( Player->team_no == Item->owned_by );

	if ( Item->goal_state == TFGS_ACTIVE )
	{
		if ( !Goal->team_str_carried && !Goal->non_team_str_carried )
			return;

		string_t iszMsg = bOwners ? Goal->team_str_carried : Goal->non_team_str_carried;
		const char *pszWho = ( Player->edict() == Item->pev->owner ) ? "you" : STRING( TF_OwnerOf( Item )->pev->netname );
		ClientPrint( Player->pev, HUD_PRINTTALK, STRING( iszMsg ), pszWho );
		return;
	}

	string_t iszTeam, iszOther;
	if ( Item->pev->origin != Item->pev->oldorigin )
	{
		iszTeam = Goal->team_str_moved;
		iszOther = Goal->non_team_str_moved;
	}
	else
	{
		iszTeam = Goal->team_str_home;
		iszOther = Goal->non_team_str_home;
	}

	if ( iszTeam || iszOther )
		ClientPrint( Player->pev, HUD_PRINTTALK, STRING( bOwners ? iszTeam : iszOther ) );
}

void CBasePlayer::DisplayLocalItemStatus( CBaseEntity *pGoal )
{
	for ( int i = 0; i < 4; i++ )
	{
		if ( !pGoal->display_item_status[i] )
			continue;

		CBaseEntity *pItem = Finditem( pGoal->display_item_status[i] );
		if ( pItem )
			DisplayItemStatus( pGoal, this, pItem );
		else
			ClientPrint( pev, HUD_PRINTTALK, "#Item_missing" );
	}
}

// "flaginfo", and the scout's special.
void CBasePlayer::TeamFortress_DisplayDetectionItems( void )
{
	CBaseEntity *pDetect = UTIL_FindEntityByClassname( NULL, "info_tfdetect" );

	if ( pDetect )
		DisplayLocalItemStatus( pDetect );
}

// "dropitems": only items flagged TFGI_CANBEDROPPED, and only with room in front.
void CBasePlayer::DropGoalItems( void )
{
	UTIL_MakeVectors( pev->v_angle );

	Vector vecSrc = GetGunPosition();
	TraceResult tr;
	UTIL_TraceHull( vecSrc, vecSrc + gpGlobals->v_forward * 32, dont_ignore_monsters, human_hull, edict(), &tr );

	if ( tr.flFraction != 1.0f )
	{
		ClientPrint( pev, HUD_PRINTCENTER, "#Dropitems_noroom" );
		return;
	}

	CBaseEntity *pItem = NULL;
	while ( ( pItem = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME ) ) != NULL )
	{
		if ( pItem->pev->owner == edict() && ( pItem->goal_activation & TFGI_CANBEDROPPED ) )
			tfgoalitem_RemoveFromPlayer( pItem, this, GI_DROP_PLAYERDROP );
	}
}

// [tfc.so] death (RemoveTimers) and feign: every item not flagged TFGI_KEEP drops.
// A disconnecting player drops kept items too, as tfc.so's feign path allows.
void TeamFortress_DropCarriedItems( CBasePlayer *pPlayer )
{
	CBaseEntity *pItem = NULL;
	while ( ( pItem = UTIL_FindEntityByClassname( pItem, TF_ITEM_CLASSNAME ) ) != NULL )
	{
		if ( pItem->pev->owner != pPlayer->edict() )
			continue;

		if ( !( pItem->goal_activation & TFGI_KEEP ) || pPlayer->has_disconnected == TRUE )
			tfgoalitem_RemoveFromPlayer( pItem, pPlayer, GI_DROP_PLAYERDEATH );

		if ( CTF_Map != TRUE )
			continue;

		if ( pItem->goal_no == CTF_FLAG1 )
		{
			UTIL_ClientPrintAll( HUD_PRINTCENTER, UTIL_VarArgs( "%s LOST the BLUE flag!\n", STRING( pPlayer->pev->netname ) ) );
			TF_LogTriggered( pPlayer, "Dropped_Blue_Flag" );
		}
		else if ( pItem->goal_no == CTF_FLAG2 )
		{
			UTIL_ClientPrintAll( HUD_PRINTCENTER, UTIL_VarArgs( "%s LOST the RED flag!\n", STRING( pPlayer->pev->netname ) ) );
			TF_LogTriggered( pPlayer, "Dropped_Red_Flag" );
		}
	}
}

// ----------------------------------------------------------------------------
// Entities

void CTFGoal::Spawn( void )
{
	if ( !TF_SkillAllows( this ) )
	{
		dremove( this );
		return;
	}

	pev->classname = MAKE_STRING( TF_GOAL_CLASSNAME );

	if ( pev->model )
	{
		if ( TF_ModelIsBrush( this ) )
			pev->effects |= EF_NODRAW;

		PRECACHE_MODEL( (char *)STRING( pev->model ) );
		SET_MODEL( ENT( pev ), STRING( pev->model ) );
	}

	if ( pev->noise )
		PRECACHE_SOUND( (char *)STRING( pev->noise ) );

	// the powerup sounds any goal may hand out
	PRECACHE_SOUND( "items/protect.wav" );
	PRECACHE_SOUND( "items/protect2.wav" );
	PRECACHE_SOUND( "items/protect3.wav" );
	PRECACHE_SOUND( "FVox/HEV_logon.wav" );
	PRECACHE_SOUND( "FVox/hev_shutdown.wav" );
	PRECACHE_SOUND( "items/inv1.wav" );
	PRECACHE_SOUND( "items/inv2.wav" );
	PRECACHE_SOUND( "items/inv3.wav" );
	PRECACHE_SOUND( "items/damage.wav" );
	PRECACHE_SOUND( "items/damage2.wav" );
	PRECACHE_SOUND( "items/damage3.wav" );

	pev->solid = SOLID_TRIGGER;

	if ( !goal_state )
		goal_state = TFGS_INACTIVE;

	if ( goal_min != g_vecZero && goal_max != g_vecZero )
		UTIL_SetSize( pev, goal_min, goal_max );

	UTIL_SetOrigin( pev, pev->origin );
	StartGoal();
}

void CTFGoal::StartGoal( void )
{
	m_bAddBonuses = FALSE;
	SetThink( &CTFGoal::PlaceGoal );
	pev->nextthink = gpGlobals->time + 0.2f;

	if ( goal_state == TFGS_REMOVED )
		RemoveGoal( this );
}

void CTFGoal::PlaceGoal( void )
{
	if ( FClassnameIs( pev, "info_tfgoal_timer" ) )
	{
		SetThink( &CBaseEntity::tfgoal_timer_tick );
		pev->nextthink = gpGlobals->time + search_time;
	}
	else if ( goal_activation & TFGA_TOUCH )
	{
		SetTouch( &CBaseEntity::tfgoal_touch );
	}

	// [tfc.so] timer goals are renamed too, so Findgoal sees them
	pev->classname = MAKE_STRING( TF_GOAL_CLASSNAME );

	if ( goal_activation & TFGA_DROPTOGROUND )
	{
		pev->movetype = MOVETYPE_TOSS;
		pev->origin.z += 6;
		UTIL_SetOrigin( pev, pev->origin );

		if ( !DROP_TO_FLOOR( ENT( pev ) ) )
		{
			ALERT( at_error, "TF Goal %s fell out of level at %f,%f,%f", STRING( pev->netname ),
			       pev->origin.x, pev->origin.y, pev->origin.z );
			UTIL_Remove( this );
			return;
		}
	}

	pev->movetype = MOVETYPE_NONE;
	pev->velocity = g_vecZero;
	pev->oldorigin = pev->origin;
}

void CTFGoal::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	if ( pActivator && pActivator->Classify() != CLASS_PLAYER )
		pActivator = NULL;

	m_bAddBonuses = TRUE;
	ActivateDoResults( this, (CBasePlayer *)pActivator, pCaller );
}

// Point goals touch through a fixed 48x48x16 box, whatever their model.
void CTFGoal::SetObjectCollisionBox( void )
{
	if ( !TF_ModelIsBrush( this ) )
	{
		pev->absmin = pev->origin + Vector( -24, -24, 0 );
		pev->absmax = pev->origin + Vector( 24, 24, 16 );
		return;
	}

	float flMax = 0;
	for ( int i = 0; i < 3; i++ )
	{
		float v = fabs( pev->mins[i] );
		if ( v > flMax )
			flMax = v;
		v = fabs( pev->maxs[i] );
		if ( v > flMax )
			flMax = v;
	}

	for ( int i = 0; i < 3; i++ )
	{
		pev->absmin[i] = pev->origin[i] - flMax - 1;
		pev->absmax[i] = pev->origin[i] + flMax + 1;
	}
}

void CTFGoalItem::Spawn( void )
{
	if ( !TF_SkillAllows( this ) )
	{
		dremove( this );
		return;
	}

	pev->classname = MAKE_STRING( TF_ITEM_CLASSNAME );

	if ( pev->model )
	{
		PRECACHE_MODEL( (char *)STRING( pev->model ) );
		SET_MODEL( ENT( pev ), STRING( pev->model ) );

		pev->sequence = LookupSequence( "not_carried" );
		if ( pev->sequence != -1 )
		{
			ResetSequenceInfo();
			pev->frame = 0;
		}
	}

	PRECACHE_SOUND( "items/itembk2.wav" );

	if ( pev->noise )
		PRECACHE_SOUND( (char *)STRING( pev->noise ) );

	if ( !goal_state )
		goal_state = TFGS_INACTIVE;

	pev->solid = ( goal_activation & TFGI_SOLID ) ? SOLID_BBOX : SOLID_TRIGGER;

	if ( goal_min == g_vecZero )
		goal_min = Vector( -16, -16, -24 );
	if ( goal_max == g_vecZero )
		goal_max = Vector( 16, 16, 32 );

	if ( !pev->netname )
		pev->netname = MAKE_STRING( "goalitem" );

	if ( drop_time <= 0 )
		drop_time = 60;

	UTIL_SetSize( pev, goal_min, goal_max );
	UTIL_SetOrigin( pev, pev->origin );
	SetTouch( &CBaseEntity::item_tfgoal_touch );
	StartItem();
}

void CTFGoalItem::StartItem( void )
{
	SetThink( &CTFGoalItem::PlaceItem );
	pev->nextthink = gpGlobals->time + 0.2f;

	if ( goal_state == TFGS_REMOVED )
		RemoveGoal( this );
}

void CTFGoalItem::PlaceItem( void )
{
	static int item_list_bit = 1;

	pev->velocity = g_vecZero;

	if ( goal_activation & TFGI_DROPTOGROUND )
	{
		pev->movetype = MOVETYPE_TOSS;
		pev->origin.z += 6;
		UTIL_SetOrigin( pev, pev->origin );

		if ( !DROP_TO_FLOOR( ENT( pev ) ) )
		{
			ALERT( at_error, "TF GoalItem %s fell out of level at %f,%f,%f", STRING( pev->netname ),
			       pev->origin.x, pev->origin.y, pev->origin.z );
			UTIL_Remove( this );
			return;
		}
	}

	pev->movetype = MOVETYPE_NONE;
	pev->oldorigin = pev->origin;

	if ( goal_activation & TFGI_ITEMGLOWS )
	{
		TF_ResolveOwnedBy( this );
		TF_ItemGlowOnGround( this );
	}

	// [tfc.so] one bit per item, handed out in spawn order; never reset
	item_list = item_list_bit;
	item_list_bit <<= 1;
}

void CTFTimerGoal::Spawn( void )
{
	if ( !TF_SkillAllows( this ) )
	{
		dremove( this );
		return;
	}

	CTFGoal::Spawn();
	pev->classname = MAKE_STRING( "info_tfgoal_timer" );

	if ( search_time <= 0 )
	{
		ALERT( at_console, "Timer Goal %s created with no specified time.\n", STRING( pev->netname ) );
		dremove( this );
		return;
	}

	pev->solid = SOLID_NOT;
}

void CTFDetect::Spawn( void )
{
}

void CTFSpawn::Spawn( void )
{
	if ( !TF_SkillAllows( this ) )
	{
		dremove( this );
		return;
	}

	if ( ( team_no < 1 || team_no > 4 ) && !teamcheck )
	{
		ALERT( at_console, "Teamspawnpoint with an invalid team_no of %d\n", team_no );
		return;
	}

	// [tfc.so] team spawns are how number_of_teams gets its value on maps without a detect
	if ( (float)team_no > number_of_teams )
		number_of_teams = team_no;

	pev->classname = MAKE_STRING( TF_SPAWN_CLASSNAME );
	pev->netname = pev->classname;
}

void CTFSpawn::Activate( void )
{
	m_pTeamCheck = NULL;

	if ( teamcheck )
		m_pTeamCheck = FindTeamCheck( STRING( teamcheck ) );
}

BOOL CTFSpawn::CheckTeam( int iTeamNo )
{
	if ( team_no )
		return team_no == iTeamNo;

	if ( m_pTeamCheck )
		return m_pTeamCheck->team_no == iTeamNo;

	return FALSE;
}

// [tfc.so] round-robin from the last spawn this team used.
static BOOL TF_SpawnPointClear( const Vector &pos )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, pos, 96 ) ) != NULL )
	{
		if ( pEnt->pev->flags & FL_CLIENT )
			return FALSE;
	}

	return TRUE;
}

CBaseEntity *CBaseEntity::FindTeamSpawnPoint( void )
{
	if ( team_no < 1 || team_no > 4 )
		return NULL;

	CBaseEntity *pSpot = g_pLastSpawns[team_no];
	int iRemoved = 0;
	int iOccupied = 0;

	// tfc.so has no bound here; an all-refused team would loop forever.
	for ( int iGuard = 0; iGuard < 4096; iGuard++ )
	{
		pSpot = UTIL_FindEntityByString( pSpot, "classname", TF_SPAWN_CLASSNAME );
		if ( !pSpot )
		{
			pSpot = UTIL_FindEntityByString( NULL, "classname", TF_SPAWN_CLASSNAME );
			if ( !pSpot )
				return NULL;
		}

		if ( !( (CTFSpawn *)pSpot )->CheckTeam( team_no ) )
			continue;

		if ( iRemoved > 127 && iOccupied == 0 )
			return NULL;

		if ( pSpot->goal_state == TFGS_REMOVED )
		{
			iRemoved++;
			continue;
		}

		if ( !TF_SpawnPointClear( pSpot->pev->origin ) && iOccupied <= 31 )
		{
			iOccupied++;
			continue;
		}

		if ( TF_Prematch() )
			return pSpot;

		if ( !ActivationSucceeded( pSpot, (CBasePlayer *)this, NULL ) )
			continue;

		g_pLastSpawns[team_no] = pSpot;
		return pSpot;
	}

	return NULL;
}

// [tfc.so] brush volumes: engineers cannot build in a func_nobuild (CheckArea reads them),
// and a hand grenade whose fuse ends in a func_nogrenades fizzles.
class CTFAreaVolume : public CBaseEntity
{
public:
	void Spawn( void )
	{
		pev->solid = SOLID_TRIGGER;
		pev->movetype = MOVETYPE_NONE;
		pev->effects |= EF_NODRAW;
		SET_MODEL( ENT( pev ), STRING( pev->model ) );
		UTIL_SetSize( pev, pev->mins, pev->maxs );
		UTIL_SetOrigin( pev, pev->origin );
	}
};

LINK_ENTITY_TO_CLASS( func_nobuild, CTFAreaVolume )
LINK_ENTITY_TO_CLASS( func_nogrenades, CTFAreaVolume )

BOOL TeamFortress_InNoGrenadeZone( CBaseEntity *pGren )
{
	const Vector &pos = pGren->pev->origin;
	CBaseEntity *pArea = NULL;

	while ( ( pArea = UTIL_FindEntityByClassname( pArea, "func_nogrenades" ) ) != NULL )
	{
		const Vector &mn = pArea->pev->mins;
		const Vector &mx = pArea->pev->maxs;

		if ( pos.x < mn.x || pos.y < mn.y || pos.z < mn.z || pos.x > mx.x || pos.y > mx.y || pos.z > mx.z )
			continue;

		CSprite *pSprite = CSprite::SpriteCreate( "sprites/xflare1.spr", pos, TRUE );
		if ( pSprite )
		{
			pSprite->AnimateAndDie( 60 );
			pSprite->SetTransparency( kRenderTransAdd, 255, 255, 255, 255, kRenderFxNoDissipation );
			pSprite->SetScale( 0.25f );
		}

		pGren->pev->flags |= FL_KILLME;
		return TRUE;
	}

	return FALSE;
}

// [tfc.so] item_armor1/2/3: green 100 at 0.3, yellow 150 at 0.6, red 200 at 0.8.
class CTFItemArmor : public CItem
{
public:
	void Spawn( void );
	void Precache( void );
	BOOL MyTouch( CBasePlayer *pPlayer );

	virtual const char *ArmorModel( void ) = 0;
	virtual float ArmorCount( void ) = 0;
	virtual float ArmorType( void ) = 0;
};

void CTFItemArmor::Precache( void )
{
	PRECACHE_MODEL( (char *)ArmorModel() );
	PRECACHE_SOUND( "items/armoron_1.wav" );
}

void CTFItemArmor::Spawn( void )
{
	if ( !CheckExistence() )
	{
		dremove( this );
		return;
	}

	Precache();
	SET_MODEL( ENT( pev ), ArmorModel() );
	CItem::Spawn();
}

BOOL CTFItemArmor::MyTouch( CBasePlayer *pPlayer )
{
	float flCount = ArmorCount();

	if ( pPlayer->pev->armorvalue >= pPlayer->maxarmor )
	{
		// a full engineer still takes it, for the metal
		if ( pPlayer->pev->playerclass != PC_ENGINEER || pPlayer->ammo_cells >= pPlayer->maxammo_cells )
			return FALSE;
	}

	// [tfc.so] compares the player's armour VALUE with the item's type, then caps at armor_allowed
	float flType = Q_max( pPlayer->pev->armorvalue, ArmorType() );
	flType = Q_min( flType, pPlayer->armor_allowed );

	int iBit = 0;
	if ( flType == 0.3f )
		iBit = IT_ARMOR1;
	else if ( flType == 0.6f )
		iBit = IT_ARMOR2;
	else if ( flType == 0.8f )
		iBit = IT_ARMOR3;
	else
		ALERT( at_console, "Bad ArmorType.\n" );

	// armour past the cap becomes metal for an engineer
	if ( flCount > pPlayer->maxarmor )
	{
		if ( pPlayer->pev->playerclass == PC_ENGINEER && pPlayer->ammo_cells < pPlayer->maxammo_cells )
		{
			int iCells = pPlayer->ammo_cells + (int)( (int)flCount - pPlayer->maxarmor );
			pPlayer->ammo_cells = Q_min( iCells, pPlayer->maxammo_cells );
		}
		flCount = pPlayer->maxarmor;
	}

	pPlayer->pev->armortype = flType;
	pPlayer->pev->armorvalue = Q_min( pPlayer->pev->armorvalue + flCount, (float)pPlayer->maxarmor );
	pPlayer->items = ( pPlayer->items & ~( IT_ARMOR1 | IT_ARMOR2 | IT_ARMOR3 ) ) | iBit;

	if ( armorclass > 0 )
		pPlayer->armorclass = armorclass;

	EMIT_SOUND_DYN( pPlayer->edict(), CHAN_ITEM, "items/armoron_1.wav", 1.0f, 0.8f, 0, PITCH_NORM );

	// [tfc.so] also respawns and fires targets here; CItem::ItemTouch already does both
	return TRUE;
}

#define TF_ARMOR_CLASS( cls, mdl, count, type ) \
	class cls : public CTFItemArmor \
	{ \
	public: \
		const char *ArmorModel( void ) { return mdl; } \
		float ArmorCount( void ) { return count; } \
		float ArmorType( void ) { return type; } \
	};

TF_ARMOR_CLASS( CTFItemArmorGreen, "models/g_armor.mdl", 100.0f, 0.3f )
TF_ARMOR_CLASS( CTFItemArmorYellow, "models/y_armor.mdl", 150.0f, 0.6f )
TF_ARMOR_CLASS( CTFItemArmorRed, "models/r_armor.mdl", 200.0f, 0.8f )

LINK_ENTITY_TO_CLASS( item_armor1, CTFItemArmorGreen )
LINK_ENTITY_TO_CLASS( item_armor2, CTFItemArmorYellow )
LINK_ENTITY_TO_CLASS( item_armor3, CTFItemArmorRed )

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

void CTeamSet::Spawn( void )
{
}

void CTeamSet::Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
{
	if ( team_no )
		SUB_UseTargets( pActivator, USE_SET, (float)team_no );
	else
		SUB_UseTargets( pActivator, USE_TOGGLE, 0 );
}

// ----------------------------------------------------------------------------
// Map and server settings

// [tfc.so] the detect entity's fields are reused: team_broadcast is the team menu
// text, the speak strings are team names, ammo counts are lives/limits/classes.
static void ParseTFDetect( CBaseEntity *pDetect )
{
	if ( pDetect->team_broadcast )
		team_menu_string = pDetect->team_broadcast;

	if ( pDetect->last_impulse )
		number_of_teams = pDetect->last_impulse;

	if ( pDetect->speak )
		team_names[1] = pDetect->speak;
	if ( pDetect->AP_speak )
		team_names[2] = pDetect->AP_speak;
	if ( pDetect->team_speak )
		team_names[3] = pDetect->team_speak;
	if ( pDetect->owners_team_speak )
		team_names[4] = pDetect->owners_team_speak;

	if ( pDetect->pev->message )
		SERVER_COMMAND( (char *)STRING( pDetect->pev->message ) );

	teamlives[1] = pDetect->ammo_shells;
	teamlives[2] = pDetect->ammo_nails;
	teamlives[3] = pDetect->ammo_rockets;
	teamlives[4] = pDetect->ammo_cells;

	teammaxplayers[1] = pDetect->ammo_medikit;
	teammaxplayers[2] = pDetect->ammo_detpack;
	teammaxplayers[3] = pDetect->maxammo_medikit;
	teammaxplayers[4] = pDetect->maxammo_detpack;

	illegalclasses[0] = pDetect->pev->playerclass;
	illegalclasses[1] = pDetect->maxammo_shells;
	illegalclasses[2] = pDetect->maxammo_nails;
	illegalclasses[3] = pDetect->maxammo_rockets;
	illegalclasses[4] = pDetect->maxammo_cells;

	civilianteams = 0;

	for ( int t = 1; t <= 4; t++ )
	{
		if ( teamlives[t] == 0 )
			teamlives[t] = -1;
		if ( teammaxplayers[t] == 0 )
			teammaxplayers[t] = 100;

		// -1 means "civilians only"
		if ( illegalclasses[t] == -1 )
		{
			illegalclasses[t] = 0;
			civilianteams |= 1 << ( t - 1 );
		}
	}
}

void ParseTFMapSettings( void )
{
	flagem_checked = 0;

	CBaseEntity *pDetect = UTIL_FindEntityByClassname( NULL, "info_tfdetect" );

	if ( pDetect )
	{
		if ( teamplay.value == 0.0f )
		{
			CVAR_SET_FLOAT( "mp_teamplay", 1.0f );
			gpGlobals->teamplay = 1.0f;
		}

		ParseTFDetect( pDetect );
	}
	else
	{
		if ( UTIL_FindEntityByClassname( NULL, "info_player_team1" ) || CTF_Map == TRUE )
		{
			CTF_Map = TRUE;

			if ( teamplay.value == 0.0f )
			{
				CVAR_SET_FLOAT( "mp_teamplay", 1.0f );
				gpGlobals->teamplay = 1.0f;
			}

			number_of_teams = 2;
		}
		else
		{
			number_of_teams = 4;
		}

		for ( int t = 1; t <= 4; t++ )
		{
			teamlives[t] = -1;
			teammaxplayers[t] = 100;
			illegalclasses[t] = 0;
			g_pLastSpawns[t] = NULL;
		}
		civilianteams = 0;
	}

	if ( number_of_teams <= 0 || number_of_teams >= 5 )
		number_of_teams = 4;

	for ( int t = 1; t <= 4; t++ )
	{
		teamfrags[t] = 0;
		teamscores[t] = 0;
	}

	cease_fire = FALSE;
	autokick_kills = 0;
	g_fNextPrematchAlert = 0;
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
