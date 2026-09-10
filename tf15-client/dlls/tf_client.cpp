/***
*
*	TFC-6 Phase 1 -- server-side team + class selection and per-class loadouts.
*
*	tf15-client's dlls/ tree shipped no TFC gameplay: the client's VGUI team and
*	class menus send "jointeam N" / bare class-name commands that nothing on the
*	server handled, so a listen-server host could never leave the no-team,
*	no-class, no-weapon state. This file wires those menus to real server logic:
*	a default two-team model, the menu sends, the command handlers, and a
*	class-stat/loadout table driven straight off tf_defs.h's PC_* constants.
*
*	Deferred to later TFC phases: grenades, projectile weapons, engineer/spy
*	abilities, map goals, prematch, class scripts, respawn bags.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "teamplay_gamerules.h"
#include "tf_defs.h"

extern int gmsgTeamInfo;
extern int gmsgScoreInfo;
extern int gmsgTeamNames;
extern int gmsgValidClasses;
extern int gmsgVGUIMenu;
extern int gmsgGameMode;
extern int g_teamplay;

// Console commands the client's class menu issues via pfnClientCmd -- these are
// sTFClassSelection[] from cl_dll/vgui_TeamFortressViewport.cpp, indexed by PC_*.
static const char *sTFClassCmd[PC_LASTCLASS] =
{
	"",                                                        // PC_UNDEFINED
	"scout", "sniper", "soldier", "demoman", "medic",          // 1..5
	"hwguy", "pyro", "spy", "engineer",                        // 6..9
	"randompc",                                                // PC_RANDOM
	"civilian",                                                // PC_CIVILIAN
};

struct tf_class_info_t
{
	int   health;
	int   maxspeed;
	int   initarmor;
	int   maxarmor;
	float armortype;
	int   armorclass;
	int   weapons;                                             // WEAP_* bitmask
	int   init_shells, init_nails, init_cells, init_rockets;
	int   max_shells,  max_nails,  max_cells,  max_rockets;
};

#define TFCLASSROW( P ) { \
	PC_##P##_MAXHEALTH, PC_##P##_MAXSPEED, \
	PC_##P##_INITARMOR, PC_##P##_MAXARMOR, (float)( PC_##P##_INITARMORTYPE ), PC_##P##_INITARMORCLASS, \
	PC_##P##_WEAPONS, \
	PC_##P##_INITAMMO_SHOT, PC_##P##_INITAMMO_NAIL, PC_##P##_INITAMMO_CELL, PC_##P##_INITAMMO_ROCKET, \
	PC_##P##_MAXAMMO_SHOT,  PC_##P##_MAXAMMO_NAIL,  PC_##P##_MAXAMMO_CELL,  PC_##P##_MAXAMMO_ROCKET }

static const tf_class_info_t sTFClass[PC_LASTCLASS] =
{
	{ 0 },                     // PC_UNDEFINED
	TFCLASSROW( SCOUT ),       // PC_SCOUT
	TFCLASSROW( SNIPER ),      // PC_SNIPER
	TFCLASSROW( SOLDIER ),     // PC_SOLDIER
	TFCLASSROW( DEMOMAN ),     // PC_DEMOMAN
	TFCLASSROW( MEDIC ),       // PC_MEDIC
	TFCLASSROW( HVYWEAP ),     // PC_HVYWEAP
	TFCLASSROW( PYRO ),        // PC_PYRO
	TFCLASSROW( SPY ),         // PC_SPY
	TFCLASSROW( ENGINEER ),    // PC_ENGINEER
	{ 0 },                     // PC_RANDOM
	TFCLASSROW( CIVILIAN ),    // PC_CIVILIAN
};

// TFC-6 Phase 1 diagnostic sink -- pfnServerPrint (Con_Printf), always logs,
// unlike ALERT() which the engine drops at developer 0. Remove the TF_DIAG
// calls once the loadout path is confirmed on hardware.
static void TF_DIAG( const char *fmt, ... )
{
	char buf[256];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	g_engfuncs.pfnServerPrint( buf );
}

static int TF_CountCarried( CBasePlayer *pPlayer )
{
	int n = 0;
	for ( int s = 0; s < MAX_ITEM_TYPES; s++ )
	{
		CBasePlayerItem *it = pPlayer->m_rgpPlayerItems[s];
		while ( it ) { n++; it = it->m_pNext; }
	}
	return n;
}

//=========================================================
// Two-team Blue/Red default. A real map info_tfdetect parse (a later phase)
// would override this; for now it is the only source of team data.
//=========================================================
void TeamFortress_SetupDefaultTeams( void )
{
	// TFC is always teamplay; a listen server's teamplay cvar can be 0.
	g_teamplay = 1;
	gpGlobals->teamplay = 1;

	if ( number_of_teams >= 1.0f )
		return;

	team_names[1] = ALLOC_STRING( "Blue" );
	team_names[2] = ALLOC_STRING( "Red" );
	number_of_teams = 2.0f;
}

void TeamFortress_ShowVGUIMenu( CBasePlayer *pPlayer, int iMenu )
{
	MESSAGE_BEGIN( MSG_ONE, gmsgVGUIMenu, NULL, pPlayer->edict() );
		WRITE_BYTE( iMenu );
	MESSAGE_END();
}

void TeamFortress_SendTeamMenu( CBasePlayer *pPlayer )
{
	int nTeams = (int)number_of_teams;

	MESSAGE_BEGIN( MSG_ONE, gmsgTeamNames, NULL, pPlayer->edict() );
		WRITE_BYTE( nTeams );
		for ( int i = 1; i <= nTeams; i++ )
			WRITE_STRING( GetTeamName( i ) );
	MESSAGE_END();

	// ValClass carries the illegal-class bitmask per team slot (0..4); zero
	// everywhere means every class is selectable.
	MESSAGE_BEGIN( MSG_ONE, gmsgValidClasses, NULL, pPlayer->edict() );
		for ( int i = 0; i < 5; i++ )
			WRITE_SHORT( 0 );
	MESSAGE_END();

	TeamFortress_ShowVGUIMenu( pPlayer, MENU_TEAM );
}

//=========================================================
// Team assignment
//=========================================================
static int TeamFortress_TeamWithFewest( void )
{
	int count[5] = { 0 };

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pEnt = UTIL_PlayerByIndex( i );
		if ( pEnt )
		{
			int t = ( (CBasePlayer *)pEnt )->team_no;
			if ( t >= 1 && t <= 4 )
				count[t]++;
		}
	}

	int best = 1;
	for ( int t = 2; t <= (int)number_of_teams; t++ )
	{
		if ( count[t] < count[best] )
			best = t;
	}
	return best;
}

void TeamFortress_JoinTeam( CBasePlayer *pPlayer, int iTeam )
{
	if ( iTeam < 1 || iTeam > (int)number_of_teams )
		iTeam = TeamFortress_TeamWithFewest();          // covers "jointeam 5" (auto)

	TF_DIAG( "[tfc] JoinTeam: team=%d (%s)\n", iTeam, GetTeamName( iTeam ) );

	pPlayer->team_no = iTeam;
	pPlayer->pev->team = iTeam;
	pPlayer->pev->playerclass = PC_UNDEFINED;
	pPlayer->nextpc = PC_UNDEFINED;

	strncpy( pPlayer->m_szTeamName, GetTeamName( iTeam ), TEAM_NAME_LENGTH );
	pPlayer->m_szTeamName[TEAM_NAME_LENGTH - 1] = '\0';

	int idx = pPlayer->entindex();
	g_engfuncs.pfnSetClientKeyValue( idx, g_engfuncs.pfnGetInfoKeyBuffer( pPlayer->edict() ),
	                                 "team", pPlayer->m_szTeamName );

	MESSAGE_BEGIN( MSG_ALL, gmsgTeamInfo );
		WRITE_BYTE( idx );
		WRITE_STRING( pPlayer->m_szTeamName );
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_ALL, gmsgScoreInfo );
		WRITE_BYTE( idx );
		WRITE_SHORT( (int)pPlayer->pev->frags );
		WRITE_SHORT( pPlayer->m_iDeaths );
		WRITE_SHORT( 0 );
		WRITE_SHORT( iTeam );
	MESSAGE_END();

	UTIL_LogPrintf( "\"%s<%i><%s><%s>\" joined team \"%s\"\n",
	                STRING( pPlayer->pev->netname ),
	                GETPLAYERUSERID( pPlayer->edict() ),
	                GETPLAYERAUTHID( pPlayer->edict() ),
	                pPlayer->m_szTeamName, pPlayer->m_szTeamName );

	TeamFortress_ShowVGUIMenu( pPlayer, MENU_CLASS );
}

//=========================================================
// Class assignment
//=========================================================
void TeamFortress_ChangeClass( CBasePlayer *pPlayer, int iClass )
{
	if ( pPlayer->team_no < 1 )
	{
		TeamFortress_ShowVGUIMenu( pPlayer, MENU_TEAM );
		return;
	}

	if ( iClass == PC_RANDOM )
		iClass = RANDOM_LONG( PC_SCOUT, PC_ENGINEER );

	if ( iClass < PC_SCOUT || iClass > PC_ENGINEER )
		return;

	TF_DIAG( "[tfc] ChangeClass: pc=%d observer=%d\n", iClass, pPlayer->IsObserver() );

	pPlayer->pev->playerclass = iClass;
	pPlayer->nextpc = iClass;
	pPlayer->lastpc = iClass;

	// Phase 1: a class pick is a clean strip-and-respawn. Real TFC kills the
	// player on a mid-life class change; that (and the frag penalty) is a later
	// phase.
	pPlayer->RemoveAllItems( FALSE );
	pPlayer->pev->deadflag = DEAD_NO;
	pPlayer->pev->effects &= ~EF_NODRAW;

	if ( pPlayer->IsObserver() )
		pPlayer->StopObserver();        // StopObserver() itself calls Spawn()
	else
		pPlayer->Spawn();
}

//=========================================================
// Per-class stats + loadout. Called from CTeamFortress::PlayerSpawn, which runs
// at the tail of CBasePlayer::Spawn() after health/armor/ammo have been reset.
//=========================================================
static void TeamFortress_GiveWeapons( CBasePlayer *pPlayer, int bits )
{
	static const struct { int bit; const char *cls; } wmap[] =
	{
		{ WEAP_AXE,              "tf_weapon_axe" },
		{ WEAP_SPANNER,          "tf_weapon_spanner" },
		{ WEAP_MEDIKIT,          "tf_weapon_medikit" },
		{ WEAP_SHOTGUN,          "tf_weapon_shotgun" },
		{ WEAP_SUPER_SHOTGUN,    "tf_weapon_supershotgun" },
		{ WEAP_NAILGUN,          "tf_weapon_ng" },
		{ WEAP_SUPER_NAILGUN,    "tf_weapon_superng" },
		{ WEAP_SNIPER_RIFLE,     "tf_weapon_sniperrifle" },
		{ WEAP_AUTO_RIFLE,       "tf_weapon_autorifle" },
		{ WEAP_ASSAULT_CANNON,   "tf_weapon_ac" },
		{ WEAP_GRENADE_LAUNCHER, "tf_weapon_gl" },
		{ WEAP_FLAMETHROWER,     "tf_weapon_flamethrower" },
		{ WEAP_ROCKET_LAUNCHER,  "tf_weapon_rpg" },
		{ WEAP_INCENDIARY,       "tf_weapon_ic" },
		{ WEAP_TRANQ,            "tf_weapon_tranq" },
		{ WEAP_LASER,            "tf_weapon_railgun" },
	};

	for ( int i = 0; i < (int)ARRAYSIZE( wmap ); i++ )
	{
		if ( !( bits & wmap[i].bit ) )
			continue;

		// Not GiveNamedItem(): the TFC weapon Spawn() functions never call
		// FallInit()/SetTouch(), so GiveNamedItem's simulated DispatchTouch hits
		// a NULL touch handler and the weapon is never added. Do what a real
		// pickup's DefaultTouch does, straight -- same path CWeaponBox uses.
		CBaseEntity *pWeapon = CBaseEntity::Create( wmap[i].cls, pPlayer->pev->origin,
		                                            pPlayer->pev->angles, pPlayer->edict() );
		if ( !pWeapon )
		{
			TF_DIAG( "[tfc]  %s -> Create() NULL (missing from exports.txt?)\n", wmap[i].cls );
			continue;
		}
		pWeapon->pev->spawnflags |= SF_NORESPAWN;

		int before = TF_CountCarried( pPlayer );
		if ( pPlayer->AddPlayerItem( (CBasePlayerItem *)pWeapon ) )
			( (CBasePlayerItem *)pWeapon )->AttachToPlayer( pPlayer );
		else
			UTIL_Remove( pWeapon );   // don't leave a ghost trigger at the feet
		int after = TF_CountCarried( pPlayer );

		TF_DIAG( "[tfc]  %s -> carried %d->%d%s\n", wmap[i].cls, before, after,
		         after > before ? "" : "  (AddPlayerItem refused)" );
	}
}

void TeamFortress_PlayerSpawn( CBasePlayer *pPlayer )
{
	int pc = pPlayer->pev->playerclass;
	TF_DIAG( "[tfc] PlayerSpawn enter: pc=%d team=%d\n", pc, pPlayer->team_no );

	// No class yet: hold the player still and untouchable until they pick one.
	// Phase 1 has no real observer camera; this is the minimal "wait here" state.
	if ( pc < PC_SCOUT || pc >= PC_RANDOM )
	{
		pPlayer->pev->takedamage = DAMAGE_NO;
		pPlayer->pev->solid = SOLID_NOT;
		pPlayer->pev->movetype = MOVETYPE_NONE;
		pPlayer->pev->effects |= EF_NODRAW;
		pPlayer->pev->velocity = g_vecZero;
		pPlayer->pev->maxspeed = 1;
		pPlayer->m_iHideHUD |= ( HIDEHUD_WEAPONS | HIDEHUD_HEALTH );
		TeamFortress_SetupGrenades( pPlayer );   // clears counts + any primed state
		return;
	}

	const tf_class_info_t *ci = &sTFClass[pc];

	pPlayer->pev->effects &= ~EF_NODRAW;
	pPlayer->pev->takedamage = DAMAGE_AIM;
	pPlayer->pev->solid = SOLID_SLIDEBOX;
	pPlayer->pev->movetype = MOVETYPE_WALK;
	pPlayer->m_iHideHUD = 0;

	pPlayer->pev->health = pPlayer->pev->max_health = ci->health;
	pPlayer->tfstate &= ~TFSTATE_AIMING;
	pPlayer->TeamFortress_SetSpeed();

	pPlayer->pev->armorvalue = ci->initarmor;
	pPlayer->pev->armortype = ci->armortype;
	pPlayer->armorclass = ci->armorclass;
	pPlayer->maxarmor = ci->maxarmor;

	// Keep both ammo representations in step: TFC weapon code reads ammo_shells
	// etc directly, while the generic HUD/pickup path tracks m_rgAmmo via
	// GiveAmmo().
	pPlayer->maxammo_shells  = ci->max_shells;
	pPlayer->maxammo_nails   = ci->max_nails;
	pPlayer->maxammo_cells   = ci->max_cells;
	pPlayer->maxammo_rockets = ci->max_rockets;
	pPlayer->ammo_shells  = ci->init_shells;
	pPlayer->ammo_nails   = ci->init_nails;
	pPlayer->ammo_cells   = ci->init_cells;
	pPlayer->ammo_rockets = ci->init_rockets;

	if ( ci->init_shells )  pPlayer->GiveAmmo( ci->init_shells,  "buckshot", ci->max_shells );
	if ( ci->init_nails )   pPlayer->GiveAmmo( ci->init_nails,   "9mm",      ci->max_nails );
	if ( ci->init_cells )   pPlayer->GiveAmmo( ci->init_cells,   "uranium",  ci->max_cells );
	if ( ci->init_rockets ) pPlayer->GiveAmmo( ci->init_rockets, "rockets",  ci->max_rockets );

	TeamFortress_GiveWeapons( pPlayer, ci->weapons );

	// Phase 2: seed the class grenade counts + reset any primed state.
	TeamFortress_SetupGrenades( pPlayer );

	TF_DIAG( "[tfc] PlayerSpawn done: want hp=%d spd=%d armor=%d/%d weapbits=0x%x | "
	         "got hp=%d spd=%d armor=%d carried=%d active=%s\n",
	         ci->health, ci->maxspeed, ci->initarmor, ci->maxarmor, (unsigned)ci->weapons,
	         (int)pPlayer->pev->health, (int)pPlayer->pev->maxspeed, (int)pPlayer->pev->armorvalue,
	         TF_CountCarried( pPlayer ),
	         pPlayer->m_pActiveItem ? STRING( pPlayer->m_pActiveItem->pev->classname ) : "(none)" );
}

//=========================================================
// Command dispatch -- called from CTeamFortress::ClientCommand.
//=========================================================
BOOL TeamFortress_ClientCommand( CBasePlayer *pPlayer, const char *pcmd )
{
	// Phase 2: +gren1 / +gren2 (L1 / R1) prime + throw.
	if ( TeamFortress_GrenadeCommand( pPlayer, pcmd ) )
		return TRUE;

	if ( FStrEq( pcmd, "jointeam" ) )
	{
		if ( CMD_ARGC() >= 2 )
			TeamFortress_JoinTeam( pPlayer, atoi( CMD_ARGV( 1 ) ) );
		else
			TeamFortress_ShowVGUIMenu( pPlayer, MENU_TEAM );
		return TRUE;
	}

	// F4-equivalent: reopen whichever selection the player still owes.
	if ( FStrEq( pcmd, "_special" ) )
	{
		if ( pPlayer->team_no < 1 )
			TeamFortress_ShowVGUIMenu( pPlayer, MENU_TEAM );
		else if ( pPlayer->pev->playerclass < PC_SCOUT || pPlayer->pev->playerclass >= PC_RANDOM )
			TeamFortress_ShowVGUIMenu( pPlayer, MENU_CLASS );
		return TRUE;
	}

	if ( FStrEq( pcmd, "changeteam" ) )
	{
		TeamFortress_ShowVGUIMenu( pPlayer, MENU_TEAM );
		return TRUE;
	}

	if ( FStrEq( pcmd, "changeclass" ) )
	{
		TeamFortress_ShowVGUIMenu( pPlayer, pPlayer->team_no < 1 ? MENU_TEAM : MENU_CLASS );
		return TRUE;
	}

	for ( int pc = PC_SCOUT; pc <= PC_ENGINEER; pc++ )
	{
		if ( FStrEq( pcmd, sTFClassCmd[pc] ) )
		{
			TeamFortress_ChangeClass( pPlayer, pc );
			return TRUE;
		}
	}

	if ( FStrEq( pcmd, "randompc" ) )
	{
		TeamFortress_ChangeClass( pPlayer, PC_RANDOM );
		return TRUE;
	}

	// Civilian is map-enforced only, never player-chosen -- swallow it quietly.
	if ( FStrEq( pcmd, "civilian" ) )
		return TRUE;

	return FALSE;
}

//=========================================================
// Team-aware spawn point selection. Returns an info_player_teamspawn edict for
// the player's team, or NULL to let the caller fall back to DM/start spawns.
//=========================================================
static bool TeamFortress_SpotClear( CBaseEntity *pSpot, CBaseEntity *pIgnore )
{
	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, pSpot->pev->origin, 96 ) ) != NULL )
	{
		if ( pEnt->IsPlayer() && pEnt != pIgnore )
			return false;
	}
	return true;
}

edict_t *TeamFortress_SelectTeamSpawnPoint( CBasePlayer *pPlayer )
{
	CBaseEntity *pSpot = NULL;
	CBaseEntity *pChoices[64];
	CBaseEntity *pAnyTeam = NULL;
	int nChoices = 0;

	while ( ( pSpot = UTIL_FindEntityByClassname( pSpot, "info_player_teamspawn" ) ) != NULL )
	{
		if ( pSpot->team_no != 0 && pSpot->team_no != pPlayer->team_no )
			continue;
		if ( pSpot->pev->origin == g_vecZero )
			continue;

		pAnyTeam = pSpot;
		if ( TeamFortress_SpotClear( pSpot, pPlayer ) && nChoices < (int)ARRAYSIZE( pChoices ) )
			pChoices[nChoices++] = pSpot;
	}

	if ( nChoices > 0 )
		return pChoices[RANDOM_LONG( 0, nChoices - 1 )]->edict();

	// every matching spawn is currently blocked -- take one anyway rather than
	// fall through to a possibly enemy-side DM spawn
	if ( pAnyTeam )
		return pAnyTeam->edict();

	return NULL;
}

//=========================================================
// The tf_wpn_* weapons drain the scalar ammo_shells/nails/cells/rockets, but
// the HUD reserve count comes from m_rgAmmo[] (gmsgAmmoX). Mirror the scalars
// into m_rgAmmo each frame so SendAmmoUpdate picks up the change. One-way for
// now -- no Phase 1 ammo pickup writes m_rgAmmo behind the weapons' backs.
//=========================================================
void TeamFortress_SyncAmmo( CBasePlayer *pPlayer )
{
	int i;

	if ( ( i = pPlayer->GetAmmoIndex( "buckshot" ) ) > 0 ) pPlayer->m_rgAmmo[i] = pPlayer->ammo_shells;
	if ( ( i = pPlayer->GetAmmoIndex( "9mm" ) )      > 0 ) pPlayer->m_rgAmmo[i] = pPlayer->ammo_nails;
	if ( ( i = pPlayer->GetAmmoIndex( "uranium" ) )  > 0 ) pPlayer->m_rgAmmo[i] = pPlayer->ammo_cells;
	if ( ( i = pPlayer->GetAmmoIndex( "rockets" ) )  > 0 ) pPlayer->m_rgAmmo[i] = pPlayer->ammo_rockets;
}
