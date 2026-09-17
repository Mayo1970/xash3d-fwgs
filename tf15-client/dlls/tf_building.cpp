// TFC-6 Phase 4 -- Engineer buildings (sentry gun, dispenser) and the spanner.
// Every constant here is read from the retail tfc/dlls/tfc.so unless marked.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "tf_defs.h"
#include "soundent.h"
#include "decals.h"
#include "game.h"
#include "shake.h"

#define TF_MIN( a, b ) ( ( a ) < ( b ) ? ( a ) : ( b ) )
#define TF_MAX( a, b ) ( ( a ) > ( b ) ? ( a ) : ( b ) )

extern int gmsgBuildState;

// Assets, from tfc.so's Precache lists.
#define TF_MDL_BASE       "models/base.mdl"
#define TF_MDL_SENTRY1    "models/sentry1.mdl"
#define TF_MDL_SENTRY2    "models/sentry2.mdl"
#define TF_MDL_SENTRY3    "models/sentry3.mdl"
#define TF_MDL_DISPENSER  "models/dispenser.mdl"
#define TF_MDL_COMPGIBS   "models/computergibs.mdl"
#define TF_SPR_SHELL      "sprites/shellchrome.spr"
#define TF_SND_TURRIDLE   "weapons/turridle.wav"
#define TF_SND_TURRSPOT   "weapons/turrspot.wav"
#define TF_SND_TURRSET    "weapons/turrset.wav"
#define TF_SND_TURRFIRE   "weapons/pl_gun3.wav"
#define TF_SND_TURRROCKET "weapons/rocketfire1.wav"
#define TF_SND_BUILDING   "weapons/building.wav"
#define TF_SND_AMMOPICKUP "items/ammopickup2.wav"
#define TF_SND_ARMORGIVE  "items/r_item2.wav"

// Sentry tunables [tfc.so].
#define TF_SENTRY_THINK       0.1f
#define TF_SENTRY_YAW_ARC     45      // half-arc, degrees either side of the build yaw
#define TF_SENTRY_BASE_TURN   6       // m_iBaseTurnRate
#define TF_SENTRY_PITCH_CAP   50.0f
#define TF_SENTRY_FOV         0.7f
#define TF_SENTRY_RANGE       1000.0f
#define TF_SENTRY_BULLET_DMG  16
#define TF_SENTRY_BULLET_DIST 2048.0f
#define TF_SENTRY_TRACERFREQ  4
#define TF_SENTRY_ROCKET_WAIT 3.0f
#define TF_SENTRY_ROCKET_SPD  800.0f
#define TF_SENTRY_UPGRADE_MUL 1.2f
#define TF_SENTRY_MAXSHELLS   100
#define TF_SENTRY_SHELLS      25
#define TF_SENTRY_MAXROCKETS  20
#define TF_SENTRY_AIM_TOL     10.0f   // goal/current angle gap that still lets it fire
#define TF_SENTRY_CHECK_WAIT  3.0f
#define TF_SENTRY_FALL_DIST   24      // CheckBelowBuilding drop tolerance
// [tfc.so] CTFSentrygunBase::Finished lifts the gun 21.2 u above its base plate;
// sentry*.mdl is authored around that pivot, so without it the model is buried.
#define TF_SENTRY_LIFT        21.2f

// Spanner repair costs 1 metal per 5 hp; sentry reloads come from the player's own ammo [tfc.so].
#define TF_METAL_PER_HEALTH   5
#define TF_SENTRY_RELOAD_SHELLS   40
#define TF_SENTRY_RELOAD_ROCKETS  20
#define TF_DISP_RELOAD_SHELLS     40
#define TF_DISP_RELOAD_NAILS      40
#define TF_DISP_RELOAD_ROCKETS    10
#define TF_DISP_RELOAD_CELLS      40
#define TF_SPANNER_ARMOR_MAX  50      // armour per spanner hit, 5 per metal
#define TF_SPANNER_ARMOR_RATE 5
#define TF_DISP_USE_WAIT      3.0f

// Dispenser [tfc.so] DispenserThink/RefillPlayer.
#define TF_DISP_TICK          12.0f
#define TF_DISP_GEN_SHELLS    20
#define TF_DISP_GEN_NAILS     30
#define TF_DISP_GEN_ROCKETS   15
#define TF_DISP_GEN_CELLS     20
#define TF_DISP_GEN_ARMOR     50.0f
#define TF_DISP_GIVE_SHELLS   20
#define TF_DISP_GIVE_NAILS    20
#define TF_DISP_GIVE_ROCKETS  10
#define TF_DISP_GIVE_CELLS    10
#define TF_DISP_GIVE_ARMOR    20.0f

// Build placement.
#define TF_BUILD_FORWARD      64.0f
#define TF_DISMANTLE_REFUND   100     // DestroyBuilding gives back 100 metal

// Sentry rockets spawn from a model attachment; if the model has none the
// attachment reads back as the origin, which would detonate inside the gun.
#define TF_SENTRY_MUZZLE_MIN  20.0f
#define TF_SENTRY_MUZZLE_FWD  24.0f

// [tfc.so] CheckSentry / TeamFortress_TakeEMPBlast.
#define TF_SENTRY_BASE_DIST   24.0f   // gun further than this from its legs malfunctions
#define TF_BUILDING_EMP_DMG   200.0f
#define TF_EVENT_BUILDING     "events/misc/tf_buildingevent.sc"

enum tfturret_anim_e
{
	TURRET_ANIM_IDLE = 0,
	TURRET_ANIM_FIRE = 1,
	TURRET_ANIM_SCAN = 2,
};

// Shared helpers

// Declared in tf_defs.h but never defined in this tree.
void teamsprint( int tno, CBaseEntity *ignore, int msg_dest, const char *st,
                 const char *param1, const char *param2, const char *param3 )
{
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pPlayer = UTIL_PlayerByIndex( i );
		if ( !pPlayer || pPlayer == ignore || pPlayer->team_no != tno )
			continue;
		ClientPrint( pPlayer->pev, msg_dest, st, param1, param2, param3 );
	}
}

// Adds up to the cap and returns what was really added.
static int TF_GiveOneAmmo( int &iAmmo, int iAdd, int iMax )
{
	iAmmo += iAdd;
	if ( iAmmo > iMax )
	{
		iAdd -= iAmmo - iMax;
		iAmmo = iMax;
	}
	return iAdd;
}

// TFC ammo lives in the scalar ammo_* fields (TeamFortress_SyncAmmo feeds the HUD).
// [tfc.so] negative amounts take ammo away (goals do this); CheckClassStats floors at 0.
BOOL CBasePlayer::GiveTFAmmo( int shells, int nails, int rockets, int cells )
{
	int iTotal = 0;

	iTotal |= TF_GiveOneAmmo( ammo_shells, shells, maxammo_shells );
	iTotal |= TF_GiveOneAmmo( ammo_nails, nails, maxammo_nails );
	iTotal |= TF_GiveOneAmmo( ammo_rockets, rockets, maxammo_rockets );
	iTotal |= TF_GiveOneAmmo( ammo_cells, cells, maxammo_cells );
	return iTotal != 0;
}

// [tfc.so] a hit building flashes a team-coloured glow shell that fades at
// 40 renderamt per think (CTFSentrygun::TakeDamage / CheckShield).
#define TF_GLOW_AMT   150.0f
#define TF_GLOW_FADE  40.0f

static void TF_BuildingHitGlow( CBaseEntity *pEnt )
{
	pEnt->pev->renderfx = kRenderFxGlowShell;
	pEnt->pev->renderamt = TF_GLOW_AMT;

	switch ( pEnt->team_no )
	{
	case 1:  pEnt->pev->rendercolor = Vector( 0, 0, 255 );   break;
	case 2:  pEnt->pev->rendercolor = Vector( 255, 0, 0 );   break;
	case 3:  pEnt->pev->rendercolor = Vector( 245, 255, 0 ); break;
	default: pEnt->pev->rendercolor = Vector( 0, 215, 45 );  break;
	}
}

static void TF_BuildingFadeGlow( CBaseEntity *pEnt )
{
	if ( !pEnt->pev->renderfx )
		return;

	pEnt->pev->renderamt -= TF_GLOW_FADE;
	if ( pEnt->pev->renderamt <= 0 )
	{
		pEnt->pev->renderfx = kRenderFxNone;
		pEnt->pev->renderamt = 0;
		pEnt->pev->rendercolor = g_vecZero;
	}
}

// [tfc.so disasm] DMG_BLAST is gated on TEAMPLAY_NOEXPLOSIVE, not let through free;
// retail's mp_teamplay 21 sets that bit too, so allies' splash must be refused as well.
static BOOL TF_BuildingCanTakeDamage( CBaseEntity *pBuilding, entvars_t *pevAttacker, int bitsDamageType )
{
	if ( !pevAttacker || !( pevAttacker->flags & FL_CLIENT ) )
		return TRUE;

	CBaseEntity *pAttacker = CBaseEntity::Instance( pevAttacker );
	if ( !pAttacker || pAttacker == pBuilding || !pBuilding->IsAlly( pAttacker ) )
		return TRUE;

	int iTeamplay = (int)gpGlobals->teamplay;
	if ( bitsDamageType & DMG_BLAST )
		return ( iTeamplay & TEAMPLAY_NOEXPLOSIVE ) ? FALSE : TRUE;

	return ( iTeamplay & TEAMPLAY_NODIRECT ) ? FALSE : TRUE;
}

// [tfc.so] same body for all three buildings: a flat blast. With any teamplay damage
// rule on, an ally's EMP only counts if it is the builder's own.
static void TF_BuildingTakeEMP( CBaseEntity *pBuilding, entvars_t *pevGren )
{
	CBaseEntity *pAttacker = CBaseEntity::Instance( pevGren->owner ? pevGren->owner : INDEXENT( 0 ) );
	int iRules = TEAMPLAY_HALFDIRECT | TEAMPLAY_NODIRECT | TEAMPLAY_HALFEXPLOSIVE | TEAMPLAY_NOEXPLOSIVE;

	if ( !pAttacker )
		return;

	if ( ( (int)gpGlobals->teamplay & iRules ) && pBuilding->IsAlly( pAttacker )
	     && (CBaseEntity *)pBuilding->real_owner != pAttacker )
		return;

	pBuilding->TakeDamage( pevGren, pAttacker->pev, TF_BUILDING_EMP_DMG, DMG_BLAST );
}

static void TF_UpdateEntityEvents( CBaseEntity *pEnt );
static void TF_RemoveEntityEvents( CBaseEntity *pEnt );

static BOOL TF_IsEngineer( CBasePlayer *pPlayer )
{
	return pPlayer && pPlayer->IsAlive() && pPlayer->pev->playerclass == PC_ENGINEER;
}

// [tfc.so] a building with nothing under it falls; MOVETYPE_TOSS lets it drop.
void CBaseEntity::CheckBelowBuilding( int iDist )
{
	TraceResult tr;

	if ( pev->movetype == MOVETYPE_TOSS )
		return;

	UTIL_TraceLine( pev->origin, pev->origin - Vector( 0, 0, (float)iDist ),
	                ignore_monsters, ENT( pev ), &tr );

	if ( tr.flFraction == 1.0f )
	{
		pev->movetype = MOVETYPE_TOSS;
		pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
	}
}

// [tfc.so] the spot must be empty and reachable from the builder. NOT a hull-fit
// test: a hull centred on the floor starts 36 u underground and always fails.
int CBaseEntity::CheckArea( CBaseEntity *pIgnore )
{
	CBaseEntity *pArea = NULL;
	TraceResult tr;

	int iContents = UTIL_PointContents( pev->origin );
	if ( iContents != CONTENTS_EMPTY && iContents != CONTENTS_WATER )
		return CAREA_BLOCKED;

	while ( ( pArea = UTIL_FindEntityByClassname( pArea, "func_nobuild" ) ) != NULL )
	{
		if ( pev->origin.x < pArea->pev->absmin.x || pev->origin.x > pArea->pev->absmax.x )
			continue;
		if ( pev->origin.y < pArea->pev->absmin.y || pev->origin.y > pArea->pev->absmax.y )
			continue;
		if ( pev->origin.z < pArea->pev->absmin.z || pev->origin.z > pArea->pev->absmax.z )
			continue;
		return CAREA_NOBUILD;
	}

	if ( !pIgnore )
		return CAREA_CLEAR;

	Vector vecStart = pev->origin;
	if ( pIgnore->pev->flags & FL_DUCKING )
		vecStart.z += 18;

	// ignore the BUILDING, not the builder: it is solid and sits on vecStart,
	// so anything else reports fStartSolid everywhere.
	UTIL_TraceHull( vecStart, pIgnore->pev->origin + pIgnore->pev->view_ofs,
	                ignore_monsters, human_hull, ENT( pev ), &tr );

	if ( tr.fStartSolid || tr.fAllSolid || tr.flFraction != 1.0f )
		return CAREA_BLOCKED;

	return CAREA_CLEAR;
}

// Sentry gun base -- the legs. base.mdl is 21.6 u tall and sentry*.mdl draws only
// the gun body, so the base must stay a separate entity.

class CTFSentrygunBase : public CBaseAnimating
{
public:
	void Spawn( void );
	int Classify( void ) { return CLASS_MACHINE; }
	int BloodColor( void ) { return DONT_BLEED; }
	int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );
};

LINK_ENTITY_TO_CLASS( building_sentrygun_base, CTFSentrygunBase )

void CTFSentrygunBase::Spawn( void )
{
	PRECACHE_MODEL( TF_MDL_BASE );

	pev->classname  = MAKE_STRING( "building_sentrygun_base" );
	pev->movetype   = MOVETYPE_NONE;
	pev->solid      = SOLID_BBOX;
	pev->takedamage = DAMAGE_AIM;
	pev->flags     |= FL_MONSTER;

	SET_MODEL( ENT( pev ), TF_MDL_BASE );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 4 ) );
}

// Shots into the legs have to hurt the gun, or the base silently eats them.
int CTFSentrygunBase::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
	CBaseEntity *pGun = m_pOtherSection;

	if ( !pGun )
		return 0;

	return pGun->TakeDamage( pevInflictor, pevAttacker, flDamage, bitsDamageType );
}

// Sentry gun

class CTFSentrygun : public CBaseMonster
{
public:
	void Spawn( void );
	void Precache( void );
	int Classify( void ) { return CLASS_MACHINE; }
	int BloodColor( void ) { return DONT_BLEED; }
	int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );
	void Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib );
	BOOL EngineerUse( CBasePlayer *pPlayer );
	void TeamFortress_TakeEMPBlast( entvars_t *pevGren ) { TF_BuildingTakeEMP( this, pevGren ); }

	void EXPORT SentryRotate( void );
	void EXPORT Attack( void );
	void EXPORT BuildThink( void );

	void Finished( void );
	void Upgrade( void );
	void Detonate( void );
	void EXPORT Blowup( void );
	void SetSentryAnim( int iAnim );
	void SetBuildAngles( float flYaw );

	BOOL CheckSentry( void );
	BOOL FindTarget( void );
	BOOL ValidTarget( CBaseEntity *pTarget );
	BOOL MoveTurret( void );
	void FoundTarget( void );
	void Fire( void );

	int m_iLevel;
	int m_iRightBound;
	int m_iLeftBound;
	int m_bTurningRight;
	int m_iBaseTurnRate;
	float m_fTurnRate;
	float m_flNextCheck;
	float m_flNextRocket;
	Vector m_vecCurAngles;
	Vector m_vecGoalAngles;
	int m_iShellSprite;
};

LINK_ENTITY_TO_CLASS( building_sentrygun, CTFSentrygun )

void CTFSentrygun::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_BASE );
	PRECACHE_MODEL( TF_MDL_SENTRY1 );
	PRECACHE_MODEL( TF_MDL_SENTRY2 );
	PRECACHE_MODEL( TF_MDL_SENTRY3 );
	m_iShellSprite = PRECACHE_MODEL( TF_SPR_SHELL );
	PRECACHE_SOUND( TF_SND_TURRIDLE );
	PRECACHE_SOUND( TF_SND_TURRSPOT );
	PRECACHE_SOUND( TF_SND_TURRSET );
	PRECACHE_SOUND( TF_SND_TURRFIRE );
	PRECACHE_SOUND( TF_SND_TURRROCKET );
	UTIL_PrecacheOther( "tf_rpg_rocket" );
	m_usBuildingEvent = PRECACHE_EVENT( 1, TF_EVENT_BUILDING );
}

void CTFSentrygun::Spawn( void )
{
	Precache();

	pev->classname  = MAKE_STRING( "building_sentrygun" );
	deathtype       = MAKE_STRING( "sentrygun" );
	pev->movetype   = MOVETYPE_FLY;
	pev->solid      = SOLID_BBOX;
	pev->takedamage = DAMAGE_AIM;
	pev->health     = BUILD_HEALTH_SENTRYGUN;
	pev->max_health = pev->health;
	pev->view_ofs   = Vector( 0, 0, 22 );
	pev->flags     |= FL_MONSTER;
	pev->sequence   = 0;
	pev->frame      = 0;

	m_flFieldOfView  = TF_SENTRY_FOV;
	m_iBaseTurnRate  = TF_SENTRY_BASE_TURN;
	m_fTurnRate      = (float)TF_SENTRY_BASE_TURN;
	m_vecCurAngles   = g_vecZero;
	m_vecGoalAngles  = g_vecZero;
	m_hEnemy         = NULL;
	m_flNextRocket   = 0;
	m_bTurningRight  = 0;

	maxammo_shells  = TF_SENTRY_MAXSHELLS;
	ammo_shells     = TF_SENTRY_SHELLS;
	maxammo_rockets = TF_SENTRY_MAXROCKETS;
	ammo_rockets    = 0;

	SET_MODEL( ENT( pev ), TF_MDL_BASE );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 48 ) );
	SetBoneController( 0, 0 );
	SetBoneController( 1, 0 );

	SetBuildAngles( pev->angles.y );
	SetThink( &CTFSentrygun::BuildThink );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

// The scan arc and both bone controllers are absolute world yaw, so the bounds
// have to be re-derived whenever the gun is turned.
void CTFSentrygun::SetBuildAngles( float flYaw )
{
	pev->angles.x = 0;
	pev->angles.z = 0;
	pev->angles.y = UTIL_AngleMod( flYaw );

	m_iRightBound = (int)UTIL_AngleMod( pev->angles.y + TF_SENTRY_YAW_ARC );
	m_iLeftBound  = (int)UTIL_AngleMod( pev->angles.y - TF_SENTRY_YAW_ARC );

	m_vecCurAngles.y  = pev->angles.y;
	m_vecGoalAngles.y = (float)m_iRightBound;
	m_bTurningRight   = 1;
}

// Held in the base.mdl pose until the build timer calls Finished().
void CTFSentrygun::BuildThink( void )
{
	CheckBelowBuilding( TF_SENTRY_FALL_DIST );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

void CTFSentrygun::Finished( void )
{
	m_iLevel = 1;
	SET_MODEL( ENT( pev ), TF_MDL_SENTRY1 );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 48 ) );

	// The gun is lifted onto its legs, so the legs stay behind as their own
	// entity at the spot the build drop settled on.
	CBaseEntity *pBase = CBaseEntity::Create( (char *)"building_sentrygun_base",
	                                          pev->origin, pev->angles, NULL );
	if ( pBase )
	{
		pBase->real_owner = real_owner;
		pBase->team_no = team_no;
		pBase->pev->team = pev->team;
		pBase->pev->colormap = pev->colormap;   // legs are a separate entity, same team remap
		pBase->m_pOtherSection = this;
		m_pOtherSection = pBase;
	}

	// The build drop left it MOVETYPE_TOSS; go back to FLY first or gravity
	// pulls the lift straight back out.
	pev->movetype = MOVETYPE_FLY;
	pev->flags &= ~FL_ONGROUND;
	pev->velocity = g_vecZero;
	UTIL_SetOrigin( pev, pev->origin + Vector( 0, 0, TF_SENTRY_LIFT ) );

	ResetSequenceInfo();
	SetSentryAnim( TURRET_ANIM_SCAN );

	CBaseEntity *pOwner = real_owner;
	if ( pOwner && pOwner->IsPlayer() )
	{
		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_finish" );
		teamsprint( team_no, pOwner, HUD_PRINTNOTIFY, "#Sentry_built",
		            STRING( pOwner->pev->netname ), NULL, NULL );
	}

	EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRSET, 1.0f, 0.8f, 0, PITCH_NORM );

	SetThink( &CTFSentrygun::SentryRotate );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

void CTFSentrygun::SetSentryAnim( int iAnim )
{
	if ( pev->sequence == iAnim )
		return;

	// Fire and scan blend into each other; anything else restarts at frame 0.
	if ( !( iAnim >= TURRET_ANIM_FIRE && iAnim <= TURRET_ANIM_SCAN
	        && pev->sequence >= TURRET_ANIM_FIRE && pev->sequence <= TURRET_ANIM_SCAN ) )
		pev->frame = 0;

	pev->sequence = iAnim;
	ResetSequenceInfo();
}

void CTFSentrygun::Upgrade( void )
{
	m_iLevel++;
	pev->max_health *= TF_SENTRY_UPGRADE_MUL;
	pev->health      = pev->max_health;
	maxammo_shells   = (int)( maxammo_shells * TF_SENTRY_UPGRADE_MUL );

	EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRSET, 1.0f, 0.8f, 0, PITCH_NORM );
	SET_MODEL( ENT( pev ), m_iLevel == 2 ? TF_MDL_SENTRY2 : TF_MDL_SENTRY3 );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 48 ) );

	if ( m_iLevel == 3 )
		SetBoneController( 2, -m_vecCurAngles.x );

	ResetSequenceInfo();
}

BOOL CTFSentrygun::ValidTarget( CBaseEntity *pTarget )
{
	if ( !pTarget || !pTarget->IsPlayer() )
		return FALSE;

	if ( pTarget->is_feigning )
		return FALSE;

	if ( gpGlobals->teamplay && team_no )
	{
		// TEST AID, not parity: mp_friendlyfire lifts the ally check so a lone
		// PS3 host (TFC-6 half B) can see the gun fire.
		if ( IsAlly( pTarget ) && !friendlyfire.value )
			return FALSE;

		// a spy wearing our colours reads as friendly
		if ( pTarget->undercover_team == team_no && !friendlyfire.value )
			return FALSE;
	}

	if ( pTarget->pev->flags & FL_NOTARGET )
		return FALSE;

	if ( !pTarget->IsAlive() )
		return FALSE;

	return FVisible( pTarget );
}

BOOL CTFSentrygun::FindTarget( void )
{
	CBaseEntity *pBest = NULL;
	float flBestDist = TF_SENTRY_RANGE;
	Vector vecSrc = pev->origin + pev->view_ofs;

	UTIL_MakeVectors( pev->angles );

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pPlayer = UTIL_PlayerByIndex( i );
		if ( !ValidTarget( pPlayer ) )
			continue;

		Vector vecDir = ( pPlayer->pev->origin + pPlayer->pev->view_ofs ) - vecSrc;
		float flDist = vecDir.Length();
		if ( flDist > flBestDist )
			continue;

		Vector vec2D = vecDir.Normalize();
		vec2D.z = 0;
		vec2D = vec2D.Normalize();
		if ( DotProduct( vec2D, gpGlobals->v_forward ) < m_flFieldOfView )
			continue;

		pBest = pPlayer;
		flBestDist = flDist;
	}

	if ( !pBest )
		return FALSE;

	m_hEnemy = pBest;
	FoundTarget();
	return TRUE;
}

void CTFSentrygun::FoundTarget( void )
{
	if ( ammo_shells > 0 || ( ammo_rockets > 0 && m_iLevel == 3 ) )
		EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRSPOT, 1.0f, 0.8f, 0, PITCH_NORM );

	SetThink( &CTFSentrygun::Attack );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
	m_flNextAttack = gpGlobals->time + TF_SENTRY_THINK;

	if ( m_flNextRocket < gpGlobals->time )
		m_flNextRocket = gpGlobals->time + 0.5f;
}

// Steps m_vecCurAngles toward m_vecGoalAngles; FALSE once it is there.
BOOL CTFSentrygun::MoveTurret( void )
{
	BOOL bMoved = FALSE;

	if ( m_vecCurAngles.x != m_vecGoalAngles.x )
	{
		float flStep = m_iBaseTurnRate * 5 * TF_SENTRY_THINK;

		if ( m_vecGoalAngles.x > m_vecCurAngles.x )
			m_vecCurAngles.x = TF_MIN( m_vecCurAngles.x + flStep, m_vecGoalAngles.x );
		else
			m_vecCurAngles.x = TF_MAX( m_vecCurAngles.x - flStep, m_vecGoalAngles.x );

		SetBoneController( 1, -m_vecCurAngles.x );
		if ( m_iLevel == 3 )
			SetBoneController( 2, -m_vecCurAngles.x );

		bMoved = TRUE;
	}

	if ( m_vecCurAngles.y == m_vecGoalAngles.y )
	{
		if ( !bMoved )
			m_fTurnRate = (float)m_iBaseTurnRate;
		return bMoved;
	}

	float flDir = ( m_vecGoalAngles.y > m_vecCurAngles.y ) ? 1.0f : -1.0f;
	float flDist = fabs( m_vecGoalAngles.y - m_vecCurAngles.y );
	if ( flDist > 180.0f )
	{
		flDist = 360.0f - flDist;
		flDir = -flDir;
	}

	// With a target the rate only ramps up; it resets when the turret settles.
	if ( m_hEnemy != NULL )
	{
		if ( flDist > 30.0f && m_fTurnRate < m_iBaseTurnRate * 30 )
			m_fTurnRate += m_iBaseTurnRate * 3;
	}
	else if ( flDist > 30.0f )
	{
		if ( m_fTurnRate < m_iBaseTurnRate * 10 )
			m_fTurnRate += m_iBaseTurnRate;
	}
	else if ( m_fTurnRate > m_iBaseTurnRate * 5 )
	{
		m_fTurnRate -= m_iBaseTurnRate;
	}

	if ( flDist <= m_fTurnRate * TF_SENTRY_THINK )
	{
		m_vecCurAngles.y = m_vecGoalAngles.y;
	}
	else
	{
		m_vecCurAngles.y += m_fTurnRate * TF_SENTRY_THINK * flDir;
		if ( m_vecCurAngles.y < 0 )
			m_vecCurAngles.y += 360.0f;
		else if ( m_vecCurAngles.y >= 360.0f )
			m_vecCurAngles.y -= 360.0f;
	}

	SetBoneController( 0, m_vecCurAngles.y - pev->angles.y );
	return TRUE;
}

// Idle think: scan the arc, look for a target.
void CTFSentrygun::SentryRotate( void )
{
	SetSentryAnim( TURRET_ANIM_SCAN );
	StudioFrameAdvance( 0 );
	DoDamageEffects( pev, pev->max_health * 0.66f, pev->max_health * 0.33f );

	if ( gpGlobals->time > m_flNextCheck )
	{
		if ( !CheckSentry() )
			return;
		m_flNextCheck = gpGlobals->time + TF_SENTRY_CHECK_WAIT;
	}

	TF_BuildingFadeGlow( this );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;

	if ( FindTarget() )
		return;

	if ( MoveTurret() )
		return;

	// [tfc.so] the legs are what can fall; the gun stays put and CheckSentry notices
	CBaseEntity *pBase = m_pOtherSection;
	if ( pBase )
		pBase->CheckBelowBuilding( TF_SENTRY_FALL_DIST );
	TF_UpdateEntityEvents( this );

	if ( RANDOM_FLOAT( 0, 1.0f ) < 0.1f )
		EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRIDLE, 1.0f, 0.8f, 0, PITCH_NORM );

	// swing to the other end of the arc, with an occasional new pitch
	if ( m_bTurningRight )
	{
		m_vecGoalAngles.y = (float)m_iLeftBound;
		m_bTurningRight = 0;
	}
	else
	{
		m_vecGoalAngles.y = (float)m_iRightBound;
		m_bTurningRight = 1;
	}

	if ( RANDOM_FLOAT( 0, 1.0f ) < 0.3f )
		m_vecGoalAngles.x = (float)(int)RANDOM_FLOAT( -10, 10 );
}

// Combat think: track the target and fire when aligned.
void CTFSentrygun::Attack( void )
{
	StudioFrameAdvance( 0 );
	DoDamageEffects( pev, pev->max_health * 0.66f, pev->max_health * 0.33f );
	TF_BuildingFadeGlow( this );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;

	CBaseEntity *pEnemy = m_hEnemy;
	BOOL bDry = ( m_iLevel == 3 ) ? ( ammo_shells <= 0 && ammo_rockets <= 0 ) : ( ammo_shells <= 0 );

	if ( !ValidTarget( pEnemy ) || bDry )
	{
		m_hEnemy = NULL;
		SetThink( &CTFSentrygun::SentryRotate );
		return;
	}

	Vector vecSrc = pev->origin + pev->view_ofs;
	Vector vecAng = UTIL_VecToAngles( pEnemy->Center() - vecSrc );

	vecAng.y = UTIL_AngleMod( vecAng.y );
	if ( vecAng.x < -180.0f )
		vecAng.x += 360.0f;
	if ( vecAng.x > 180.0f )
		vecAng.x -= 360.0f;
	vecAng.x = TF_MAX( -TF_SENTRY_PITCH_CAP, TF_MIN( TF_SENTRY_PITCH_CAP, vecAng.x ) );

	m_vecGoalAngles.y = vecAng.y;
	m_vecGoalAngles.x = vecAng.x;
	MoveTurret();

	if ( gpGlobals->time < m_flNextAttack
	     || ( m_vecGoalAngles - m_vecCurAngles ).Length() > TF_SENTRY_AIM_TOL )
	{
		SetSentryAnim( TURRET_ANIM_SCAN );
		return;
	}

	Fire();
	m_flNextAttack = gpGlobals->time + ( m_iLevel == 1 ? 0.2f : 0.1f );
}

// [tfc.so] the gun must still sit on its legs and outside every func_nobuild,
// or it malfunctions and blows up.
BOOL CTFSentrygun::CheckSentry( void )
{
	CBaseEntity *pBase = m_pOtherSection;
	BOOL bOK = pBase && ( pev->origin - pBase->pev->origin ).Length() <= TF_SENTRY_BASE_DIST;

	CBaseEntity *pArea = NULL;
	while ( bOK && ( pArea = UTIL_FindEntityByClassname( pArea, "func_nobuild" ) ) != NULL )
	{
		const Vector &o = pev->origin;
		if ( o.x >= pArea->pev->mins.x && o.y >= pArea->pev->mins.y && o.z >= pArea->pev->mins.z
		     && o.x <= pArea->pev->maxs.x && o.y <= pArea->pev->maxs.y && o.z <= pArea->pev->maxs.z )
			bOK = FALSE;
	}

	if ( bOK )
		return TRUE;

	CBaseEntity *pOwner = real_owner;
	if ( pOwner && pOwner->IsPlayer() )
	{
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Sentry_malfunc", STRING( pOwner->pev->netname ) );
		UTIL_LogPrintf( "\"<-1><><>\" triggered \"Sentry_Malfunction\" against \"%s<%i><%s><%s>\"\n",
		                STRING( pOwner->pev->netname ), GETPLAYERUSERID( pOwner->edict() ),
		                GETPLAYERAUTHID( pOwner->edict() ),
		                pOwner->team_no ? GetTeamName( pOwner->team_no ) : "SPECTATOR" );
	}

	Killed( NULL, NULL, GIB_NORMAL );
	return FALSE;
}

void CTFSentrygun::Fire( void )
{
	CBaseEntity *pEnemy = m_hEnemy;
	CBaseEntity *pOwner = real_owner;
	Vector vecSrc, vecAng;

	if ( !pEnemy )
		return;

	UTIL_MakeAimVectors( m_vecCurAngles );

	Vector vecDir = pEnemy->pev->origin - pev->origin;
	if ( vecDir.Length() == 0 )
		vecDir = Vector( 0, 0, 1 );
	else
		vecDir = vecDir.Normalize();

	if ( m_iLevel == 3 && ammo_rockets > 0 && gpGlobals->time > m_flNextRocket )
	{
		// not the attachment: the rocket's owner is the engineer, so SV_ClipToLinks
		// does not skip the sentry and a muzzle inside its hull would hit it
		vecSrc = pev->origin + pev->view_ofs + vecDir * TF_SENTRY_MUZZLE_FWD;
		vecAng = UTIL_VecToAngles( vecDir );
		vecAng.x = -vecAng.x;

		// [tfc.so] the rocket belongs to the engineer, so kills credit him and
		// SV_ClipToLinks lets it pass through him on the way out.
		CTFRpgRocket *pRocket = CTFRpgRocket::CreateRpgRocket( vecSrc, vecAng,
		                                                       pOwner ? pOwner : this, NULL );
		if ( pRocket )
		{
			pRocket->pev->velocity = vecDir * TF_SENTRY_ROCKET_SPD;
			pRocket->deathtype = MAKE_STRING( "sentrygun" );
		}

		EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRROCKET, 1.0f, 0.8f, 0, PITCH_NORM );
		m_flNextRocket = gpGlobals->time + TF_SENTRY_ROCKET_WAIT;

		ammo_rockets--;
		if ( pOwner )
		{
			if ( ammo_rockets == 10 )
				ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_rocketslow" );
			else if ( ammo_rockets == 0 )
				ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_rocketsout" );
		}

		if ( ammo_shells <= 0 )
			return;
	}

	if ( ammo_shells <= 0 )
		return;

	SetSentryAnim( TURRET_ANIM_FIRE );
	pev->effects |= EF_MUZZLEFLASH;

	GetAttachment( ( m_iLevel == 3 && ( ammo_shells & 1 ) ) ? 1 : 0, vecSrc, vecAng );
	if ( ( vecSrc - pev->origin ).Length() < TF_SENTRY_MUZZLE_MIN )
		vecSrc = pev->origin + pev->view_ofs + vecDir * TF_SENTRY_MUZZLE_FWD;

	FireBullets( 1, vecSrc, vecDir, g_vecZero, TF_SENTRY_BULLET_DIST, BULLET_MONSTER_12MM,
	             TF_SENTRY_TRACERFREQ, TF_SENTRY_BULLET_DMG, pOwner ? pOwner->pev : pev );

	EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRFIRE, 1.0f, 0.8f, 0, PITCH_NORM );

	ammo_shells--;
	if ( pOwner )
	{
		if ( ammo_shells == 20 )
			ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_shellslow" );
		else if ( ammo_shells == 0 && RANDOM_FLOAT( 0, 1.0f ) < 0.1f )
			ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_shellsout" );
	}
}

// [tfc.so] Spanner hit: upgrade if metal allows, else repair, else refill shells
// and rockets from the player's own ammo. No per-hit gate beyond the swing rate.
BOOL CTFSentrygun::EngineerUse( CBasePlayer *pPlayer )
{
	if ( !pPlayer || !IsAlly( pPlayer ) || !m_iLevel )
		return FALSE;

	if ( m_iLevel < 3 && pPlayer->ammo_cells >= BUILD_COST_SENTRYGUN )
	{
		pPlayer->ammo_cells -= BUILD_COST_SENTRYGUN;
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Sentry_upgrade", UTIL_dtos1( m_iLevel + 1 ) );
		Upgrade();
		return TRUE;
	}

	if ( pev->health < pev->max_health )
	{
		int iPoints = (int)ceil( ( pev->max_health - pev->health ) / TF_METAL_PER_HEALTH );
		iPoints = TF_MIN( iPoints, pPlayer->ammo_cells );

		if ( iPoints > 0 )
		{
			pPlayer->ammo_cells -= iPoints;
			pev->health = TF_MIN( pev->max_health, pev->health + iPoints * TF_METAL_PER_HEALTH );
			ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Sentry_repair" );
			return TRUE;
		}
	}

	if ( ammo_shells < maxammo_shells && pPlayer->ammo_shells > 0 )
	{
		int iGive = TF_MIN( TF_MIN( pPlayer->ammo_shells, TF_SENTRY_RELOAD_SHELLS ), maxammo_shells - ammo_shells );
		pPlayer->ammo_shells -= iGive;
		ammo_shells += iGive;
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Sentry_inshells" );
	}

	if ( ammo_rockets < maxammo_rockets && m_iLevel == 3 && pPlayer->ammo_rockets > 0 )
	{
		int iGive = TF_MIN( TF_MIN( pPlayer->ammo_rockets, TF_SENTRY_RELOAD_ROCKETS ), maxammo_rockets - ammo_rockets );
		pPlayer->ammo_rockets -= iGive;
		ammo_rockets += iGive;
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Sentry_inrockets" );
	}

	return TRUE;
}

int CTFSentrygun::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
	if ( pev->takedamage == DAMAGE_NO )
		return 0;

	if ( !TF_BuildingCanTakeDamage( this, pevAttacker, bitsDamageType ) )
		return 0;

	TF_BuildingHitGlow( this );
	pev->health -= flDamage;
	if ( pev->health <= 0 )
	{
		Killed( pevInflictor, pevAttacker, GIB_NORMAL );
		return 0;
	}

	return 1;
}

// Deferred: Engineer_RemoveBuildings runs inside CBasePlayer::Killed, and a
// synchronous blast there would re-enter TakeDamage on the dying engineer.
void CTFSentrygun::Detonate( void )
{
	pev->takedamage = DAMAGE_NO;
	SetThink( &CTFSentrygun::Blowup );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFSentrygun::Blowup( void )
{
	Killed( pev, real_owner ? real_owner->pev : pev, GIB_NORMAL );
}

void CTFSentrygun::Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib )
{
	CBaseEntity *pOwner = real_owner;
	CBaseEntity *pBase = m_pOtherSection;

	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	SetThink( NULL );
	TF_RemoveEntityEvents( this );

	if ( pBase )
	{
		pBase->pev->takedamage = DAMAGE_NO;
		pBase->pev->solid = SOLID_NOT;
		pBase->m_pOtherSection = NULL;
		UTIL_Remove( pBase );
		m_pOtherSection = NULL;
	}

	if ( pOwner && pOwner->IsPlayer() )
	{
		( (CBasePlayer *)pOwner )->has_sentry = 0;
		( (CBasePlayer *)pOwner )->building = NULL;
		TeamFortress_SendBuildState( (CBasePlayer *)pOwner );
		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Sentry_destroyed" );
	}

	// the shell count sets the size of the blast, as in TFC
	float flDmg = 75.0f + ammo_rockets * 10.0f;
	::RadiusDamage( pev->origin, pev, pOwner ? pOwner->pev : pev, flDmg, flDmg * 2.5f,
	                CLASS_NONE, DMG_BLAST | DMG_RADIUS_QUAKE );

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z + 16 );
		WRITE_SHORT( g_sModelIndexFireball );
		WRITE_BYTE( 25 );
		WRITE_BYTE( 30 );
		WRITE_BYTE( TE_EXPLFLAG_NONE );
	MESSAGE_END();

	UTIL_Remove( this );
}

// Dispenser

class CTFDispenser : public CBaseAnimating
{
public:
	void Spawn( void );
	void Precache( void );
	int Classify( void ) { return CLASS_MACHINE; }
	int BloodColor( void ) { return DONT_BLEED; }
	int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );
	void Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib );
	BOOL EngineerUse( CBasePlayer *pPlayer );
	void TeamFortress_TakeEMPBlast( entvars_t *pevGren ) { TF_BuildingTakeEMP( this, pevGren ); }

	void EXPORT DispenserThink( void );
	void EXPORT BuildThink( void );
	void EXPORT DispenserTouch( CBaseEntity *pOther );

	void Finished( void );
	void Detonate( void );
	void EXPORT Blowup( void );
	BOOL RefillPlayer( CBasePlayer *pPlayer );

	float m_flNextRefillTime;
	float m_flNextUseTime;
	int m_bBuilt;
};

LINK_ENTITY_TO_CLASS( building_dispenser, CTFDispenser )

void CTFDispenser::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_DISPENSER );
	PRECACHE_MODEL( TF_MDL_COMPGIBS );
	PRECACHE_SOUND( TF_SND_TURRSET );
	PRECACHE_SOUND( TF_SND_AMMOPICKUP );
	m_usBuildingEvent = PRECACHE_EVENT( 1, TF_EVENT_BUILDING );
}

void CTFDispenser::Spawn( void )
{
	Precache();

	pev->classname  = MAKE_STRING( "building_dispenser" );
	deathtype       = MAKE_STRING( "dispenser" );
	pev->movetype   = MOVETYPE_FLY;
	pev->solid      = SOLID_BBOX;
	pev->takedamage = DAMAGE_AIM;
	pev->health     = BUILD_HEALTH_DISPENSER;
	pev->max_health = pev->health;

	maxammo_shells  = BUILD_DISPENSER_MAX_SHELLS;
	maxammo_nails   = BUILD_DISPENSER_MAX_NAILS;
	maxammo_rockets = BUILD_DISPENSER_MAX_ROCKETS;
	maxammo_cells   = BUILD_DISPENSER_MAX_CELLS;

	pev->angles.x = 0;
	pev->angles.z = 0;

	// [tfc.so] dispenser.mdl is worn from spawn, not base.mdl -- there is no
	// separate legs entity like the sentry, the model's own sequence builds.
	SET_MODEL( ENT( pev ), TF_MDL_DISPENSER );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 24 ) );

	SetThink( &CTFDispenser::BuildThink );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

void CTFDispenser::BuildThink( void )
{
	CheckBelowBuilding( TF_SENTRY_FALL_DIST );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

// [tfc.so] the engineer hands a quarter of his own ammo over at completion.
void CTFDispenser::Finished( void )
{
	CBasePlayer *pOwner = (CBasePlayer *)( (CBaseEntity *)real_owner );

	m_bBuilt = TRUE;
	SET_MODEL( ENT( pev ), TF_MDL_DISPENSER );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 24 ) );

	if ( pOwner && pOwner->IsPlayer() )
	{
		ammo_shells  = (int)( pOwner->ammo_shells * 0.25f );
		ammo_nails   = (int)( pOwner->ammo_nails * 0.25f );
		ammo_rockets = (int)( pOwner->ammo_rockets * 0.25f );
		ammo_cells   = (int)( pOwner->ammo_cells * 0.25f );
		pev->armorvalue = pOwner->pev->armorvalue * 0.25f;

		pOwner->ammo_shells  -= ammo_shells;
		pOwner->ammo_nails   -= ammo_nails;
		pOwner->ammo_rockets -= ammo_rockets;
		pOwner->ammo_cells   -= ammo_cells;
		pOwner->pev->armorvalue -= pev->armorvalue;

		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Dispenser_finish" );
		teamsprint( team_no, pOwner, HUD_PRINTNOTIFY, "#Dispenser_built",
		            STRING( pOwner->pev->netname ), NULL, NULL );
	}

	EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, TF_SND_TURRSET, 1.0f, 0.8f, 0, PITCH_NORM );

	SetThink( &CTFDispenser::DispenserThink );
	SetTouch( &CTFDispenser::DispenserTouch );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
	m_flNextRefillTime = gpGlobals->time + TF_DISP_TICK;
}

void CTFDispenser::DispenserThink( void )
{
	CheckBelowBuilding( TF_SENTRY_FALL_DIST );
	TF_UpdateEntityEvents( this );
	DoDamageEffects( pev, pev->max_health * 0.66f, pev->max_health * 0.33f );
	TF_BuildingFadeGlow( this );

	if ( gpGlobals->time > m_flNextRefillTime )
	{
		ammo_shells  = TF_MIN( maxammo_shells,  ammo_shells + TF_DISP_GEN_SHELLS );
		ammo_nails   = TF_MIN( maxammo_nails,   ammo_nails + TF_DISP_GEN_NAILS );
		ammo_rockets = TF_MIN( maxammo_rockets, ammo_rockets + TF_DISP_GEN_ROCKETS );
		ammo_cells   = TF_MIN( maxammo_cells,   ammo_cells + TF_DISP_GEN_CELLS );
		pev->armorvalue = TF_MIN( (float)BUILD_DISPENSER_MAX_ARMOR, pev->armorvalue + TF_DISP_GEN_ARMOR );
		m_flNextRefillTime = gpGlobals->time + TF_DISP_TICK;
	}

	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

void CTFDispenser::DispenserTouch( CBaseEntity *pOther )
{
	if ( !pOther || !pOther->IsPlayer() || !pOther->IsAlive() )
		return;

	if ( gpGlobals->time < m_flNextUseTime )
		return;

	if ( !IsAlly( pOther ) )
		return;

	m_flNextUseTime = gpGlobals->time + 0.2f;
	RefillPlayer( (CBasePlayer *)pOther );
}

BOOL CTFDispenser::RefillPlayer( CBasePlayer *pPlayer )
{
	int iShells  = TF_MIN( TF_DISP_GIVE_SHELLS,  TF_MIN( ammo_shells,  pPlayer->maxammo_shells - pPlayer->ammo_shells ) );
	int iNails   = TF_MIN( TF_DISP_GIVE_NAILS,   TF_MIN( ammo_nails,   pPlayer->maxammo_nails - pPlayer->ammo_nails ) );
	int iRockets = TF_MIN( TF_DISP_GIVE_ROCKETS, TF_MIN( ammo_rockets, pPlayer->maxammo_rockets - pPlayer->ammo_rockets ) );
	int iCells   = TF_MIN( TF_DISP_GIVE_CELLS,   TF_MIN( ammo_cells,   pPlayer->maxammo_cells - pPlayer->ammo_cells ) );

	iShells  = TF_MAX( 0, iShells );
	iNails   = TF_MAX( 0, iNails );
	iRockets = TF_MAX( 0, iRockets );
	iCells   = TF_MAX( 0, iCells );

	float flArmor = TF_MIN( TF_DISP_GIVE_ARMOR,
	                       TF_MIN( pev->armorvalue, pPlayer->maxarmor - pPlayer->pev->armorvalue ) );
	if ( flArmor < 0 )
		flArmor = 0;

	if ( !iShells && !iNails && !iRockets && !iCells && flArmor <= 0 )
	{
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Dispenser_empty" );
		return FALSE;
	}

	pPlayer->GiveTFAmmo( iShells, iNails, iRockets, iCells );
	ammo_shells  -= iShells;
	ammo_nails   -= iNails;
	ammo_rockets -= iRockets;
	ammo_cells   -= iCells;

	if ( flArmor > 0 )
	{
		if ( pPlayer->pev->armortype == 0 )
			pPlayer->pev->armortype = pPlayer->armor_allowed;
		pPlayer->pev->armorvalue += flArmor;
		pev->armorvalue -= flArmor;
	}

	EMIT_SOUND_DYN( pPlayer->edict(), CHAN_ITEM, TF_SND_AMMOPICKUP, 1.0f, 0.8f, 0, PITCH_NORM );
	return TRUE;
}

BOOL CTFDispenser::EngineerUse( CBasePlayer *pPlayer )
{
	if ( !pPlayer || !IsAlly( pPlayer ) || !m_bBuilt )
		return FALSE;

	if ( gpGlobals->time < m_flNextUseTime )
		return TRUE;
	m_flNextUseTime = gpGlobals->time + TF_DISP_USE_WAIT;

	int iMetal = pPlayer->ammo_cells;

	if ( pev->health < pev->max_health && iMetal > 0 )
	{
		int iSpend = TF_MIN( iMetal, (int)( pev->max_health - pev->health ) * TF_METAL_PER_HEALTH );
		pev->health = TF_MIN( pev->max_health, pev->health + iSpend / TF_METAL_PER_HEALTH );
		pPlayer->ammo_cells -= iSpend;
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Menu_RepairDisp" );
		return TRUE;
	}

	// otherwise stock it from the engineer's own ammo
	int iShells  = TF_MIN( TF_DISP_RELOAD_SHELLS,  TF_MIN( pPlayer->ammo_shells,  maxammo_shells - ammo_shells ) );
	int iNails   = TF_MIN( TF_DISP_RELOAD_NAILS,   TF_MIN( pPlayer->ammo_nails,   maxammo_nails - ammo_nails ) );
	int iRockets = TF_MIN( TF_DISP_RELOAD_ROCKETS, TF_MIN( pPlayer->ammo_rockets, maxammo_rockets - ammo_rockets ) );
	int iCells   = TF_MIN( TF_DISP_RELOAD_CELLS,   TF_MIN( pPlayer->ammo_cells,   maxammo_cells - ammo_cells ) );

	iShells  = TF_MAX( 0, iShells );
	iNails   = TF_MAX( 0, iNails );
	iRockets = TF_MAX( 0, iRockets );
	iCells   = TF_MAX( 0, iCells );

	if ( !iShells && !iNails && !iRockets && !iCells )
	{
		ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Dispenser_AmmoNoMore" );
		return TRUE;
	}

	ammo_shells  += iShells;
	ammo_nails   += iNails;
	ammo_rockets += iRockets;
	ammo_cells   += iCells;

	pPlayer->ammo_shells  -= iShells;
	pPlayer->ammo_nails   -= iNails;
	pPlayer->ammo_rockets -= iRockets;
	pPlayer->ammo_cells   -= iCells;

	ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, "#Menu_AmmoDisp" );
	EMIT_SOUND_DYN( ENT( pev ), CHAN_ITEM, TF_SND_AMMOPICKUP, 1.0f, 0.8f, 0, PITCH_NORM );
	return TRUE;
}

int CTFDispenser::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
	if ( pev->takedamage == DAMAGE_NO )
		return 0;

	if ( !TF_BuildingCanTakeDamage( this, pevAttacker, bitsDamageType ) )
		return 0;

	TF_BuildingHitGlow( this );
	pev->health -= flDamage;
	if ( pev->health <= 0 )
	{
		Killed( pevInflictor, pevAttacker, GIB_NORMAL );
		return 0;
	}

	return 1;
}

void CTFDispenser::Detonate( void )
{
	pev->takedamage = DAMAGE_NO;
	SetTouch( NULL );
	SetThink( &CTFDispenser::Blowup );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFDispenser::Blowup( void )
{
	Killed( pev, real_owner ? real_owner->pev : pev, GIB_NORMAL );
}

void CTFDispenser::Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib )
{
	CBaseEntity *pOwner = real_owner;

	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	SetThink( NULL );
	SetTouch( NULL );
	TF_RemoveEntityEvents( this );

	if ( pOwner && pOwner->IsPlayer() )
	{
		( (CBasePlayer *)pOwner )->has_dispenser = 0;
		( (CBasePlayer *)pOwner )->building = NULL;
		TeamFortress_SendBuildState( (CBasePlayer *)pOwner );
		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, "#Dispenser_destroyed" );
	}

	// the stored ammo is what makes a full dispenser worth killing
	float flDmg = 10.0f + ammo_shells * 0.15f + ammo_rockets * 0.5f + ammo_cells * 0.2f;
	::RadiusDamage( pev->origin, pev, pOwner ? pOwner->pev : pev, flDmg, flDmg * 2.5f,
	                CLASS_NONE, DMG_BLAST | DMG_RADIUS_QUAKE );

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z + 16 );
		WRITE_SHORT( g_sModelIndexFireball );
		WRITE_BYTE( 20 );
		WRITE_BYTE( 30 );
		WRITE_BYTE( TE_EXPLFLAG_NONE );
	MESSAGE_END();

	UTIL_Remove( this );
}

// Teleporters [tfc.so]. FX are tf_buildingevent.sc nodes the client keeps alive.

#define TF_MDL_TELEPORTER     "models/teleporter.mdl"
#define TF_TELE_HEALTH        125.0f
#define TF_TELE_THINK         0.05f
#define TF_TELE_FADE          0.25f   // fade out / fade in, x m_flDamageDelay
#define TF_TELE_RECHARGE      10.0f
#define TF_TELE_ENTRY_WAIT    10.5f
#define TF_TELE_EXIT_Z        49.0f
#define TF_TELE_GLOW_TIME     12.0f
#define TF_TELE_DISMANTLE     62      // metal back from a dismantle
#define TF_TELE_DISMANTLE_DIST 128.0f

// m_iState
#define TELE_STATE_INIT       0
#define TELE_STATE_IDLE       1       // built, no partner
#define TELE_STATE_READY      2
#define TELE_STATE_SENDING    3       // entrance: someone stepped on
#define TELE_STATE_RECEIVING  4       // exit: fading the player in
#define TELE_STATE_ARRIVED    5
#define TELE_STATE_RECHARGE   6

// m_iBuildingEventState / event iparam1 bits (cl_dll/ev_tfc.h)
#define TELE_EV_SPARK         1
#define TELE_EV_SMOKE         2
#define TELE_EV_ENTRY         4
#define TELE_EV_EXIT          8
#define TELE_EV_READY         32
#define TELE_EV_IN            64
#define TELE_EV_OUT           128
#define TELE_EV_MOVED         0x100
#define TELE_EV_PARTICLES     0x200
#define TELE_EV_REMOVE        0x400

extern int gmsgSpecFade;
extern int gmsgResetFade;

static Vector TF_TeleTeamColor( int iTeam )
{
	switch ( iTeam )
	{
	case 1:  return Vector( 0, 0, 255 );
	case 2:  return Vector( 255, 0, 0 );
	case 3:  return Vector( 255, 255, 0 );
	case 4:  return Vector( 0, 255, 0 );
	default: return g_vecZero;
	}
}

static void TF_BuildingEvent( CBaseEntity *pEnt, int iBits, int iTeam, int bOn )
{
	PLAYBACK_EVENT_FULL( FEV_RELIABLE | FEV_GLOBAL, pEnt->edict(), pEnt->m_usBuildingEvent, 0,
	                     pEnt->pev->origin, g_vecZero, 0, 0, iBits, iTeam, bOn, 0 );
}

// [tfc.so] UpdateEntityEvents: the client's FX node follows a building that moved
static void TF_UpdateEntityEvents( CBaseEntity *pEnt )
{
	if ( pEnt->pev->origin == pEnt->m_vOldOrigin )
		return;

	TF_BuildingEvent( pEnt, TELE_EV_MOVED, 0, 0 );
	pEnt->m_vOldOrigin = pEnt->pev->origin;
}

static void TF_RemoveEntityEvents( CBaseEntity *pEnt )
{
	if ( pEnt->m_usBuildingEvent )
		TF_BuildingEvent( pEnt, TELE_EV_REMOVE, 0, 0 );
}

// [tfc.so] sparks under 66% health, smoke too under 33%
void DoDamageEffects( entvars_t *pevBuilding, float flSpark, float flSmoke )
{
	CBaseEntity *pEnt = CBaseEntity::Instance( pevBuilding );
	int &bits = pEnt->m_iBuildingEventState;

	if ( pevBuilding->health < flSpark )
	{
		if ( !( bits & TELE_EV_SPARK ) )
		{
			TF_BuildingEvent( pEnt, TELE_EV_SPARK, pEnt->team_no, 1 );
			bits |= TELE_EV_SPARK;
		}

		if ( pevBuilding->health < flSmoke )
		{
			if ( !( bits & TELE_EV_SMOKE ) )
			{
				TF_BuildingEvent( pEnt, TELE_EV_SMOKE, pEnt->team_no, 1 );
				bits |= TELE_EV_SMOKE;
			}
			return;
		}
	}
	else if ( bits & TELE_EV_SPARK )
	{
		TF_BuildingEvent( pEnt, TELE_EV_SPARK, 0, 0 );
		bits &= ~TELE_EV_SPARK;
	}

	if ( bits & TELE_EV_SMOKE )
	{
		TF_BuildingEvent( pEnt, TELE_EV_SMOKE, 0, 0 );
		bits &= ~TELE_EV_SMOKE;
	}
}

// [tfc.so] an invisible trigger at the exit that kills whoever stands there
void CTelefragDeath::Spawn( void )
{
	pev->movetype = MOVETYPE_NONE;
	pev->solid = SOLID_TRIGGER;
	pev->effects = EF_NODRAW;
	pev->angles = g_vecZero;
	pev->classname = MAKE_STRING( "teledeath" );

	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	if ( !pOwner )
		return;

	UTIL_SetSize( pev, pOwner->pev->mins - Vector( 4, 4, 4 ), pOwner->pev->maxs + Vector( 4, 4, 4 ) );
	SetTouch( &CTelefragDeath::DeathTouch );
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.2f;
	gpGlobals->force_retouch = 2;
}

void CTelefragDeath::DeathTouch( CBaseEntity *pOther )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( !pOwner || pOther == pOwner || pOther == (CBaseEntity *)m_hTeleporter )
		return;

	if ( pOther->IsPlayer() && ( (CBasePlayer *)pOther )->invincible_finished > gpGlobals->time )
	{
		// [tfc.so] an invincible target telefrags the traveller instead
		deathtype = MAKE_STRING( "teledeath2" );
		pOwner->TakeDamage( pev, pev, 5000, DMG_ALWAYSGIB );
		return;
	}

	if ( pOther->pev->takedamage != DAMAGE_NO )
	{
		deathtype = MAKE_STRING( "teledeath" );
		pOther->TakeDamage( pev, pev, 5000, DMG_ALWAYSGIB );
	}

	if ( FClassnameIs( pOther->pev, "detpack" ) )
	{
		CBaseEntity *pDetOwner = pOther->pev->owner ? CBaseEntity::Instance( pOther->pev->owner ) : NULL;
		UTIL_ClientPrintAll( HUD_PRINTNOTIFY, "#Detpack_telefragged",
		                     pDetOwner ? STRING( pDetOwner->pev->netname ) : "",
		                     STRING( pOwner->pev->netname ) );
		pOther->pev->solid = SOLID_NOT;
		pOther->SetTouch( NULL );
		UTIL_Remove( pOther );
	}
}

class CTFTeleporter : public CBaseAnimating
{
public:
	void Spawn( void );
	void Precache( void );
	int Classify( void ) { return CLASS_MACHINE; }
	int BloodColor( void ) { return DONT_BLEED; }
	int TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType );
	void Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib );
	BOOL EngineerUse( CBasePlayer *pPlayer );
	void TeamFortress_TakeEMPBlast( entvars_t *pevGren ) { TF_BuildingTakeEMP( this, pevGren ); }

	void EXPORT BuildThink( void );
	void EXPORT TeleporterThink( void );
	void EXPORT TeleporterTouch( CBaseEntity *pOther );
	void EXPORT TeleporterExplode( void );

	void Finished( void );
	void Detonate( void );
	void Dismantle( CBasePlayer *pOwner );
	CTFTeleporter *FindMatch( void );
	void SetTeleporterRings( int iState );
	void SetTeleporterParticles( int iState );
	void TeleporterKilled( void );
	void StopPlayer( CBasePlayer *pPlayer, const char *pszMsg );

	BOOL IsEntrance( void ) { return m_iType == BUILD_TELEPORTER_ENTRANCE; }
	CBasePlayer *Traveller( void ) { return (CBasePlayer *)(CBaseEntity *)m_hPlayer; }

	EHANDLE m_hPlayer;
	int m_iType;
	int m_iState;
	int m_iDestroyed;
	int m_iShardIndex;
	float m_flMyNextThink;
	float m_flDamageDelay;
	BOOL m_bBuilt;
};

LINK_ENTITY_TO_CLASS( building_teleporter, CTFTeleporter )

void CTFTeleporter::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_TELEPORTER );
	PRECACHE_SOUND( "misc/teleport_out.wav" );
	PRECACHE_SOUND( "misc/teleport_in.wav" );
	PRECACHE_SOUND( "misc/teleport_ready.wav" );
	PRECACHE_SOUND( TF_SND_TURRSET );
	PRECACHE_MODEL( TF_SPR_SHELL );
	PRECACHE_MODEL( "sprites/particle.spr" );
	m_iShardIndex = PRECACHE_MODEL( TF_MDL_COMPGIBS );
	m_usBuildingEvent = PRECACHE_EVENT( 1, TF_EVENT_BUILDING );
}

void CTFTeleporter::Spawn( void )
{
	Precache();

	pev->classname  = MAKE_STRING( "building_teleporter" );
	deathtype       = MAKE_STRING( "teleporter" );
	pev->solid      = SOLID_BBOX;
	pev->movetype   = MOVETYPE_TOSS;
	pev->flags     &= ~FL_ONGROUND;
	pev->velocity   = Vector( 0, 0, 8 );
	pev->max_health = TF_TELE_HEALTH;
	pev->health     = pev->max_health;
	pev->takedamage = DAMAGE_NO;
	pev->flags     |= FL_MONSTER;
	pev->angles.x   = 0;
	pev->angles.z   = 0;
	m_iDestroyed    = 0;

	SET_MODEL( ENT( pev ), TF_MDL_TELEPORTER );
	UTIL_SetSize( pev, Vector( -16, -16, 0 ), Vector( 16, 16, 12 ) );

	SetThink( &CTFTeleporter::BuildThink );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

void CTFTeleporter::BuildThink( void )
{
	CheckBelowBuilding( TF_SENTRY_FALL_DIST );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;
}

// The metal is taken by Timer_FinishedBuilding, as for the other buildings.
void CTFTeleporter::Finished( void )
{
	CBasePlayer *pOwner = (CBasePlayer *)( (CBaseEntity *)real_owner );
	BOOL bEntrance = IsEntrance();
	const char *pszFinished = bEntrance ? "#Teleporter_Entrance_Finished" : "#Teleporter_Exit_Finished";
	const char *pszBuilt = bEntrance ? "#Teleporter_Entrance_Built" : "#Teleporter_Exit_Built";

	m_bBuilt = TRUE;

	if ( pOwner && pOwner->IsPlayer() )
	{
		if ( bEntrance )
			pOwner->has_entry_teleporter = 1;
		else
			pOwner->has_exit_teleporter = 1;

		ClientPrint( pOwner->pev, HUD_PRINTNOTIFY, pszFinished );
		teamsprint( team_no, pOwner, HUD_PRINTCENTER, pszBuilt, STRING( pOwner->pev->netname ), NULL, NULL );
		UTIL_LogPrintf( "\"%s<%i><%s><%s>\" triggered \"%s\"\n", STRING( pOwner->pev->netname ),
		                GETPLAYERUSERID( pOwner->edict() ), GETPLAYERAUTHID( pOwner->edict() ),
		                GetTeamName( pOwner->team_no ), pszFinished + 1 );
	}

	pev->takedamage = DAMAGE_AIM;
	m_iState = TELE_STATE_IDLE;
	pev->movetype = MOVETYPE_TOSS;
	pev->velocity = Vector( 0, 0, 8 );
	pev->flags &= ~FL_ONGROUND;
	m_vOldOrigin = pev->origin;

	SetTouch( &CTFTeleporter::TeleporterTouch );
	SetThink( &CTFTeleporter::TeleporterThink );
	pev->nextthink = gpGlobals->time + TF_SENTRY_THINK;

	EMIT_SOUND_DYN( ENT( pev ), CHAN_WEAPON, TF_SND_TURRSET, 1.0f, 0.8f, 0, PITCH_NORM );
}

// The other end of this engineer's pair.
CTFTeleporter *CTFTeleporter::FindMatch( void )
{
	int iWant = IsEntrance() ? BUILD_TELEPORTER_EXIT : BUILD_TELEPORTER_ENTRANCE;
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "building_teleporter" ) ) != NULL )
	{
		CTFTeleporter *pTele = (CTFTeleporter *)pEnt;
		if ( (CBaseEntity *)pTele->real_owner == (CBaseEntity *)real_owner && pTele->m_iType == iWant )
			return pTele;
	}
	return NULL;
}

void CTFTeleporter::TeleporterTouch( CBaseEntity *pOther )
{
	if ( !( pOther->pev->flags & FL_CLIENT ) )
		return;

	CBaseEntity *pOwner = real_owner;

	if ( pOther->team_no && pOther->team_no != team_no )
	{
		if ( pOwner )
			ClientPrint( pOwner->pev, HUD_PRINTCENTER,
			             IsEntrance() ? "#Teleporter_entrance_enemy_use" : "#Teleporter_exit_enemy_use" );
		return;
	}

	if ( !IsEntrance() )
	{
		if ( m_iState == TELE_STATE_IDLE )
			ClientPrint( pOther->pev, HUD_PRINTCENTER, "#Teleporter_exit_idle" );
		return;
	}

	if ( m_iState == TELE_STATE_IDLE )
	{
		ClientPrint( pOther->pev, HUD_PRINTCENTER, "#Teleporter_entrance_idle" );
		return;
	}

	if ( m_iState != TELE_STATE_READY )
		return;

	// a goal item that bars teleporting keeps the carrier here
	if ( pOther->is_unableto_spy_or_teleport )
	{
		CBaseEntity *pItem = NULL;
		while ( ( pItem = UTIL_FindEntityByClassname( pItem, "item_tfgoal" ) ) != NULL )
		{
			if ( pItem->pev->owner == pOther->edict() && ( pItem->goal_result & TFGR_REMOVE_DISGUISE ) )
			{
				ClientPrint( pOther->pev, HUD_PRINTCENTER, "#Teleporter_carrying_goal", STRING( pItem->pev->netname ) );
				return;
			}
		}
		return;
	}

	if ( pOther->pev->velocity.Length() != 0.0f )
		return;

	CTFTeleporter *pExit = FindMatch();
	if ( !pExit || pExit->m_iState != TELE_STATE_READY )
		return;

	m_hPlayer = pOther;
	m_iState = TELE_STATE_SENDING;
	m_flMyNextThink = gpGlobals->time + 0.1f;
}

void CTFTeleporter::SetTeleporterRings( int iState )
{
	int iBit = IsEntrance() ? TELE_EV_ENTRY : TELE_EV_EXIT;
	BOOL bOn = ( iState >= TELE_STATE_READY && iState <= TELE_STATE_RECHARGE );

	if ( bOn && !( m_iBuildingEventState & iBit ) )
	{
		TF_BuildingEvent( this, iBit, team_no, 1 );
		m_iBuildingEventState |= iBit;
	}
	else if ( !bOn && ( m_iBuildingEventState & iBit ) )
	{
		TF_BuildingEvent( this, iBit, team_no, 0 );
		m_iBuildingEventState &= ~iBit;
	}
}

void CTFTeleporter::SetTeleporterParticles( int iState )
{
	if ( iState == TELE_STATE_READY )
	{
		if ( IsEntrance() && !( m_iBuildingEventState & TELE_EV_PARTICLES ) )
		{
			TF_BuildingEvent( this, TELE_EV_PARTICLES, team_no, 1 );
			m_iBuildingEventState |= TELE_EV_PARTICLES;
			TF_BuildingEvent( this, TELE_EV_READY, team_no, 1 );
		}
	}
	else if ( m_iBuildingEventState & TELE_EV_PARTICLES )
	{
		TF_BuildingEvent( this, TELE_EV_PARTICLES, team_no, 0 );
		m_iBuildingEventState &= ~TELE_EV_PARTICLES;
	}
}

static void TF_TeleResetFade( CBasePlayer *pPlayer )
{
	pPlayer->pev->rendermode = kRenderNormal;
	pPlayer->pev->renderamt = 0;
	pPlayer->m_flFadeAmount = 0;
	pPlayer->m_iFadeDirection = 0;
}

static void TF_TeleResetGlowshell( CBasePlayer *pPlayer )
{
	pPlayer->pev->renderfx = kRenderFxNone;
	pPlayer->pev->rendercolor = g_vecZero;
	pPlayer->pev->renderamt = 0;
	pPlayer->m_flTeleporterEffectEndTime = 0;
}

// [tfc.so] TeleporterResetEffects, from Spawn and StartObserver
void TeleporterResetEffects( CBasePlayer *pPlayer )
{
	if ( !pPlayer )
		return;
	TF_TeleResetFade( pPlayer );
	TF_TeleResetGlowshell( pPlayer );
}

// [tfc.so] from PreThink: the arrival glow shell runs out
void CBasePlayer::TeleporterEffectThink( void )
{
	if ( m_flTeleporterEffectEndTime != 0 && m_flTeleporterEffectEndTime <= gpGlobals->time )
		TF_TeleResetGlowshell( this );
}

static void TF_TeleSpecFade( CBasePlayer *pPlayer, int bIn, int iTeam, float flTime, int iAlpha )
{
	MESSAGE_BEGIN( MSG_ALL, gmsgSpecFade );
		WRITE_BYTE( ENTINDEX( pPlayer->edict() ) );
		WRITE_BYTE( bIn );
		WRITE_BYTE( iTeam );
		WRITE_SHORT( (int)( flTime * 100.0f ) );
		WRITE_BYTE( iAlpha );
	MESSAGE_END();
}

// Frees a traveller whose trip was cut short.
void CTFTeleporter::StopPlayer( CBasePlayer *pPlayer, const char *pszMsg )
{
	pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
	pPlayer->TeamFortress_SetSpeed();
	pPlayer->m_iBeingTeleported = 0;

	MESSAGE_BEGIN( MSG_ONE, gmsgResetFade, NULL, pPlayer->pev );
	MESSAGE_END();
	MESSAGE_BEGIN( MSG_ALL, gmsgSpecFade );
		WRITE_BYTE( ENTINDEX( pPlayer->edict() ) );
		WRITE_BYTE( 1 );
		WRITE_BYTE( 0 );
		WRITE_SHORT( 1 );
		WRITE_BYTE( 0 );
	MESSAGE_END();

	TF_TeleResetFade( pPlayer );

	if ( pszMsg )
	{
		ClientPrint( pPlayer->pev, HUD_PRINTCENTER, pszMsg );
		CBaseEntity *pOwner = real_owner;
		pPlayer->TakeDamage( pev, pOwner ? pOwner->pev : pev, 500, DMG_ALWAYSGIB );
	}
}

// [tfc.so] PlayerStoppedTeleporting, from Killed and StartObserver
void PlayerStoppedTeleporting( CBasePlayer *pPlayer )
{
	if ( !pPlayer )
		return;

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "building_teleporter" ) ) != NULL )
	{
		CTFTeleporter *pTele = (CTFTeleporter *)pEnt;
		if ( pTele->Traveller() != pPlayer )
			continue;

		pTele->m_iState = TELE_STATE_RECHARGE;
		pTele->m_flMyNextThink = gpGlobals->time + TF_TELE_RECHARGE * pTele->m_flDamageDelay;
		pTele->SetTeleporterRings( pTele->m_iState );
		pTele->SetTeleporterParticles( pTele->m_iState );
		pTele->m_hPlayer = NULL;
	}

	pPlayer->m_iBeingTeleported = 0;
	pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;

	MESSAGE_BEGIN( MSG_ONE, gmsgResetFade, NULL, pPlayer->pev );
	MESSAGE_END();
	MESSAGE_BEGIN( MSG_ALL, gmsgSpecFade );
		WRITE_BYTE( ENTINDEX( pPlayer->edict() ) );
		WRITE_BYTE( 1 );
		WRITE_BYTE( 0 );
		WRITE_SHORT( 1 );
		WRITE_BYTE( 0 );
	MESSAGE_END();

	TF_TeleResetFade( pPlayer );
}

void CTFTeleporter::TeleporterThink( void )
{
	CheckBelowBuilding( TF_SENTRY_FALL_DIST );

	if ( pev->origin != m_vOldOrigin )
	{
		TF_BuildingEvent( this, TELE_EV_MOVED, 0, 0 );
		m_vOldOrigin = pev->origin;
	}

	DoDamageEffects( pev, pev->max_health * 0.66f, pev->max_health * 0.33f );
	TF_BuildingFadeGlow( this );

	// the traveller's alpha fades between the two ends
	CBasePlayer *pPlayer = Traveller();
	if ( pPlayer && pPlayer->pev->rendermode != kRenderNormal )
	{
		if ( pPlayer->m_iFadeDirection == 1 && IsEntrance() )
			pPlayer->pev->renderamt = Q_min( pPlayer->pev->renderamt + pPlayer->m_flFadeAmount, 255.0f );
		else if ( pPlayer->m_iFadeDirection == 0 && !IsEntrance() )
			pPlayer->pev->renderamt = Q_max( pPlayer->pev->renderamt - pPlayer->m_flFadeAmount, 0.0f );
	}

	pev->nextthink = gpGlobals->time + TF_TELE_THINK;

	if ( m_flMyNextThink != 0 && m_flMyNextThink > gpGlobals->time )
		return;

	CTFTeleporter *pMatch;
	float flFade;

	switch ( m_iState )
	{
	case TELE_STATE_READY:
		pMatch = FindMatch();
		if ( !pMatch || pMatch->m_iState == TELE_STATE_INIT )
			m_iState = TELE_STATE_IDLE;
		m_flMyNextThink = gpGlobals->time + 0.1f;
		break;

	case TELE_STATE_SENDING:
		if ( !pPlayer )
		{
			m_iState = TELE_STATE_READY;
			break;
		}

		pPlayer->tfstate |= TFSTATE_CANT_MOVE;
		pPlayer->TeamFortress_SetSpeed();
		pPlayer->m_iBeingTeleported = 1;
		m_flDamageDelay = 1.0f;

		pMatch = FindMatch();
		if ( !pMatch || pMatch->m_iState != TELE_STATE_READY )
		{
			pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
			pPlayer->TeamFortress_SetSpeed();
			pPlayer->m_iBeingTeleported = 0;
			TF_TeleResetFade( pPlayer );
			m_iState = TELE_STATE_RECHARGE;
			m_flMyNextThink = gpGlobals->time + TF_TELE_RECHARGE * m_flDamageDelay;
			break;
		}

		flFade = TF_TELE_FADE * m_flDamageDelay;
		TeleporterResetEffects( pPlayer );
		UTIL_ScreenFade( pPlayer, TF_TeleTeamColor( team_no ), flFade, flFade, 255, FFADE_OUT );
		pPlayer->pev->rendermode = kRenderTransAlpha;
		pPlayer->pev->renderamt = 255;
		pPlayer->m_iFadeDirection = 0;
		pPlayer->m_flFadeAmount = 255.0f / ( flFade / TF_TELE_THINK );
		TF_TeleSpecFade( pPlayer, 0, team_no, flFade, 255 );
		TF_BuildingEvent( this, TELE_EV_OUT, team_no, 1 );

		pMatch->m_flDamageDelay = m_flDamageDelay;
		pMatch->m_hPlayer = pPlayer;
		pMatch->m_iState = TELE_STATE_RECEIVING;
		pMatch->m_flMyNextThink = gpGlobals->time + flFade;

		m_iState = TELE_STATE_RECHARGE;
		m_flMyNextThink = gpGlobals->time + TF_TELE_ENTRY_WAIT * m_flDamageDelay;
		break;

	case TELE_STATE_RECEIVING:
	{
		if ( !pPlayer )
		{
			m_iState = TELE_STATE_READY;
			break;
		}

		Vector vecDest = pev->origin + Vector( 0, 0, TF_TELE_EXIT_Z );
		CTelefragDeath *pDeath = GetClassPtr( (CTelefragDeath *)NULL );
		UTIL_SetOrigin( pDeath->pev, vecDest );
		pDeath->pev->owner = pPlayer->edict();
		pDeath->m_hTeleporter = this;
		pDeath->Spawn();

		UTIL_SetOrigin( pPlayer->pev, vecDest );

		flFade = TF_TELE_FADE * m_flDamageDelay;
		UTIL_ScreenFade( pPlayer, TF_TeleTeamColor( team_no ), flFade, 0, 255, FFADE_IN );
		pPlayer->pev->rendermode = kRenderTransAlpha;
		pPlayer->pev->renderamt = 0;
		pPlayer->m_iFadeDirection = 1;
		pPlayer->m_flFadeAmount = 255.0f / ( flFade / TF_TELE_THINK );
		TF_TeleSpecFade( pPlayer, 1, team_no, flFade, 255 );
		TF_BuildingEvent( this, TELE_EV_IN, team_no, 1 );

		m_iState = TELE_STATE_ARRIVED;
		m_flMyNextThink = gpGlobals->time + flFade;
		break;
	}

	case TELE_STATE_ARRIVED:
		if ( pPlayer )
		{
			pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
			pPlayer->TeamFortress_SetSpeed();
			pPlayer->m_iBeingTeleported = 0;
			TF_TeleResetFade( pPlayer );

			// a team-coloured glow shell marks a fresh arrival
			pPlayer->pev->renderfx = kRenderFxGlowShell;
			pPlayer->pev->rendercolor = TF_TeleTeamColor( team_no );
			pPlayer->pev->renderamt = 15;
			pPlayer->m_flTeleporterEffectEndTime = gpGlobals->time + TF_TELE_GLOW_TIME;
		}

		m_iState = TELE_STATE_RECHARGE;
		m_flMyNextThink = gpGlobals->time + TF_TELE_RECHARGE * m_flDamageDelay;
		m_hPlayer = NULL;

		if ( ( pMatch = FindMatch() ) != NULL )
			pMatch->m_hPlayer = NULL;
		break;

	case TELE_STATE_RECHARGE:
		m_iState = TELE_STATE_READY;
		m_flMyNextThink = gpGlobals->time + 0.1f;
		break;

	default:
		pMatch = FindMatch();
		if ( pMatch && pMatch->m_iState != TELE_STATE_INIT )
			m_iState = TELE_STATE_READY;
		m_flMyNextThink = gpGlobals->time + 0.1f;
		break;
	}

	SetTeleporterRings( m_iState );
	SetTeleporterParticles( m_iState );
}

// [tfc.so] 5 metal per point of health; a full pair's recharge is cut to 10 s.
BOOL CTFTeleporter::EngineerUse( CBasePlayer *pPlayer )
{
	if ( !pPlayer || !m_bBuilt )
		return FALSE;

	int iNeed = (int)ceil( ( pev->max_health - pev->health ) / 5.0f );
	int iSpend = TF_MIN( iNeed, pPlayer->ammo_cells );
	if ( iSpend <= 0 )
		return FALSE;

	ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY,
	             IsEntrance() ? "#Teleporter_Entrance_Repaired" : "#Teleporter_Exit_Repaired" );
	pPlayer->ammo_cells -= iSpend;
	pev->health = TF_MIN( pev->health + iSpend * 5, pev->max_health );

	CTFTeleporter *pMatch = FindMatch();
	if ( pMatch && m_iState == TELE_STATE_RECHARGE && pMatch->m_iState == TELE_STATE_RECHARGE
	     && pev->health == pev->max_health && pMatch->pev->health == pMatch->pev->max_health
	     && m_flMyNextThink - gpGlobals->time > TF_TELE_RECHARGE )
	{
		m_flMyNextThink = gpGlobals->time + TF_TELE_RECHARGE;
		pMatch->m_flMyNextThink = gpGlobals->time + TF_TELE_RECHARGE;
	}

	return TRUE;
}

int CTFTeleporter::TakeDamage( entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType )
{
	if ( pev->takedamage == DAMAGE_NO )
		return 0;

	if ( !TF_BuildingCanTakeDamage( this, pevAttacker, bitsDamageType ) )
		return 0;

	TF_BuildingHitGlow( this );
	pev->health -= flDamage;
	if ( pev->health <= 0 )
	{
		Killed( pevInflictor, pevAttacker, GIB_NORMAL );
		return 0;
	}

	return 1;
}

// [tfc.so] whoever was mid-trip on this pair dies with it
void CTFTeleporter::TeleporterKilled( void )
{
	CTFTeleporter *pMatch = FindMatch();
	const char *pszMsg = IsEntrance() ? "#Teleporter_Entrance_Gone_Player_Killed"
	                                  : "#Teleporter_Exit_Gone_Player_Killed";

	if ( Traveller() )
		StopPlayer( Traveller(), pszMsg );
	else if ( pMatch && pMatch->Traveller() )
		pMatch->StopPlayer( pMatch->Traveller(), pszMsg );

	m_hPlayer = NULL;

	if ( pMatch )
	{
		pMatch->m_hPlayer = NULL;
		pMatch->m_iState = TELE_STATE_IDLE;
		pMatch->m_flMyNextThink = gpGlobals->time + 0.1f;
		pMatch->SetTeleporterRings( pMatch->m_iState );
		pMatch->SetTeleporterParticles( pMatch->m_iState );
	}
}

// Deferred like the other buildings: Killed can run inside the engineer's own death.
void CTFTeleporter::Detonate( void )
{
	TakeDamage( pev, real_owner ? real_owner->pev : pev, 500, 0 );
}

void CTFTeleporter::Killed( entvars_t *pevInflictor, entvars_t *pevAttacker, int iGib )
{
	if ( m_iDestroyed )
		return;
	m_iDestroyed = 1;

	CBasePlayer *pOwner = (CBasePlayer *)( (CBaseEntity *)real_owner );
	BOOL bEntrance = IsEntrance();

	pev->takedamage = DAMAGE_NO;
	SetTouch( NULL );

	if ( pOwner && pOwner->IsPlayer() )
	{
		int &iNoMsg = bEntrance ? pOwner->no_entry_teleporter_message : pOwner->no_exit_teleporter_message;

		if ( bEntrance )
			pOwner->has_entry_teleporter = 0;
		else
			pOwner->has_exit_teleporter = 0;

		if ( !iNoMsg )
			ClientPrint( pOwner->pev, HUD_PRINTCENTER,
			             bEntrance ? "#Teleporter_Entrance_Destroyed" : "#Teleporter_Exit_Destroyed" );
		iNoMsg = 0;
		TeamFortress_SendBuildState( pOwner );
	}

	TF_BuildingEvent( this, TELE_EV_REMOVE, 0, 0 );
	TeleporterKilled();

	SetThink( &CTFTeleporter::TeleporterExplode );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFTeleporter::TeleporterExplode( void )
{
	CBaseEntity *pOwner = real_owner;

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLODEMODEL );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z );
		WRITE_COORD( 400 );
		WRITE_SHORT( m_iShardIndex );
		WRITE_SHORT( 10 );
		WRITE_BYTE( 25 );
	MESSAGE_END();

	if ( pOwner && !pOwner->has_disconnected )
		::RadiusDamage( pev->origin, pev, pOwner->pev, 25, 25 * 2.5f, CLASS_NONE, DMG_BLAST | DMG_RADIUS_QUAKE );

	MESSAGE_BEGIN( MSG_PAS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z );
		WRITE_SHORT( g_sModelIndexFireball );
		WRITE_BYTE( 30 );
		WRITE_BYTE( 15 );
		WRITE_BYTE( TE_EXPLFLAG_NOADDITIVE | TE_EXPLFLAG_NODLIGHTS );
	MESSAGE_END();

	pev->solid = SOLID_NOT;
	SetThink( NULL );
	UTIL_Remove( this );
}

// [tfc.so] Menu_Engineer_Input: next to it, nobody mid-trip, 62 metal back
void CTFTeleporter::Dismantle( CBasePlayer *pOwner )
{
	BOOL bEntrance = IsEntrance();

	if ( ( pev->origin - pOwner->pev->origin ).Length() > TF_TELE_DISMANTLE_DIST )
	{
		ClientPrint( pOwner->pev, HUD_PRINTCENTER, "#Teleporter_DistDismantle" );
		return;
	}

	CTFTeleporter *pMatch = FindMatch();
	if ( Traveller() || ( pMatch && pMatch->Traveller() ) )
	{
		ClientPrint( pOwner->pev, HUD_PRINTCENTER, "#Teleporter_UseDismantle" );
		return;
	}

	ClientPrint( pOwner->pev, HUD_PRINTNOTIFY,
	             bEntrance ? "#Teleporter_entrance_dismantle" : "#Teleporter_exit_dismantle" );
	UTIL_LogPrintf( "\"%s<%i><%s><%s>\" triggered \"%s\"\n", STRING( pOwner->pev->netname ),
	                GETPLAYERUSERID( pOwner->edict() ), GETPLAYERAUTHID( pOwner->edict() ),
	                GetTeamName( pOwner->team_no ),
	                bEntrance ? "Teleporter_Entrance_Dismantle" : "Teleporter_Exit_Dismantle" );
	TF_BuildingEvent( this, TELE_EV_REMOVE, 0, 0 );

	pOwner->ammo_cells += TF_TELE_DISMANTLE;
	pOwner->TeamFortress_CheckClassStats();

	if ( bEntrance )
		pOwner->has_entry_teleporter = 0;
	else
		pOwner->has_exit_teleporter = 0;

	m_iDestroyed = 1;
	real_owner = NULL;
	pev->solid = SOLID_NOT;
	SetThink( NULL );
	SetTouch( NULL );
	UTIL_Remove( this );
	TeamFortress_SendBuildState( pOwner );
}

// [tfc.so] "detentryteleporter" / "detexitteleporter": 500 damage, 100 metal back if it is stuck
static void DestroyTeleporter( CBasePlayer *pOwner, int iType )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "building_teleporter" ) ) != NULL )
	{
		CTFTeleporter *pTele = (CTFTeleporter *)pEnt;
		if ( (CBaseEntity *)pTele->real_owner != (CBaseEntity *)pOwner || pTele->m_iType != iType )
			continue;

		int iContents = UTIL_PointContents( pTele->pev->origin );
		if ( iContents == CONTENTS_SOLID || iContents == CONTENTS_SKY )
		{
			pOwner->ammo_cells += 100;
			pOwner->TeamFortress_CheckClassStats();
		}

		pTele->TakeDamage( pTele->pev, pTele->pev, 500, 0 );
	}
}

// [tfc.so] replays every building's live FX bits to one client; 0x800 remaps entindex to fparam1
void SendBuildingEventInfo( CBasePlayer *pPlayer )
{
	static const char *s_pszClasses[] = { "building_sentrygun", "building_dispenser", "building_teleporter" };

	for ( int i = 0; i < ARRAYSIZE( s_pszClasses ); i++ )
	{
		CBaseEntity *pEnt = NULL;
		while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, s_pszClasses[i] ) ) != NULL )
		{
			if ( !pEnt->m_iBuildingEventState || !pEnt->m_usBuildingEvent )
				continue;

			PLAYBACK_EVENT_FULL( FEV_RELIABLE | FEV_HOSTONLY, pPlayer->edict(), pEnt->m_usBuildingEvent, 0,
			                     pEnt->pev->origin, g_vecZero, (float)ENTINDEX( pEnt->edict() ), 0,
			                     pEnt->m_iBuildingEventState | 0x800, pEnt->team_no, 1, 0 );
		}
	}
}

CBaseEntity *CBasePlayer::GetTeleporter( int type )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "building_teleporter" ) ) != NULL )
	{
		if ( (CBaseEntity *)pEnt->real_owner == (CBaseEntity *)this && ( (CTFTeleporter *)pEnt )->m_iType == type )
			return pEnt;
	}
	return NULL;
}

// Spanner on a teammate: armour for metal, 5 per 1 [tfc.so]

BOOL CBasePlayer::EngineerUse( CBasePlayer *pPlayer )
{
	if ( !pPlayer || !IsAlive() )
		return FALSE;

	if ( pev->armortype == 0 )
		pev->armortype = armor_allowed;

	int iMetal = pPlayer->ammo_cells;
	float flHeadroom = maxarmor - pev->armorvalue;
	float flGive;
	int iCost;

	if ( flHeadroom <= 0 || iMetal <= 0 )
		return TRUE;

	if ( iMetal > TF_SPANNER_ARMOR_MAX / TF_SPANNER_ARMOR_RATE )
	{
		if ( flHeadroom >= TF_SPANNER_ARMOR_MAX )
		{
			flGive = TF_SPANNER_ARMOR_MAX;
			iCost = TF_SPANNER_ARMOR_MAX / TF_SPANNER_ARMOR_RATE;
		}
		else
		{
			flGive = flHeadroom;
			iCost = (int)( flHeadroom * 0.25f );
		}
	}
	else if ( iMetal * TF_SPANNER_ARMOR_RATE <= flHeadroom )
	{
		flGive = (float)( iMetal * TF_SPANNER_ARMOR_RATE );
		iCost = iMetal;
	}
	else
	{
		flGive = flHeadroom;
		iCost = (int)( flHeadroom * 0.25f );
	}

	if ( flGive <= 0 )
		return TRUE;

	pev->armorvalue = TF_MIN( (float)maxarmor, pev->armorvalue + flGive );
	pPlayer->ammo_cells = TF_MAX( 0, pPlayer->ammo_cells - iCost );

	EMIT_SOUND_DYN( pPlayer->edict(), CHAN_ITEM, TF_SND_ARMORGIVE, 1.0f, 0.8f, 0, PITCH_NORM );
	return TRUE;
}

// Build flow

// TEST AID, not parity: a dispenser plus a sentry costs 230 of a 200 cap, so at 1
// this keeps an engineer's metal topped up and both can exist at once.
cvar_t tf_build_freemetal = { "tf_build_freemetal", "0", FCVAR_SERVER };

void TeamFortress_SendBuildState( CBasePlayer *pPlayer )
{
	int iState = 0;

	if ( pPlayer->pev->playerclass == PC_ENGINEER )
	{
		if ( tf_build_freemetal.value && pPlayer->ammo_cells < pPlayer->maxammo_cells )
			pPlayer->ammo_cells = pPlayer->maxammo_cells;

		if ( pPlayer->is_building )
			iState |= BS_BUILDING;
		if ( pPlayer->has_dispenser )
			iState |= BS_HAS_DISPENSER;
		if ( pPlayer->has_sentry )
			iState |= BS_HAS_SENTRYGUN;
		if ( !pPlayer->has_dispenser && pPlayer->ammo_cells >= BUILD_COST_DISPENSER )
			iState |= BS_CANB_DISPENSER;
		if ( !pPlayer->has_sentry && pPlayer->ammo_cells >= BUILD_COST_SENTRYGUN )
			iState |= BS_CANB_SENTRYGUN;
		if ( pPlayer->has_entry_teleporter )
			iState |= BS_HAS_ENTRY_TELEPORTER;
		else if ( pPlayer->ammo_cells >= BUILD_COST_TELEPORTER )
			iState |= BS_CANB_ENTRY_TELEPORTER;
		if ( pPlayer->has_exit_teleporter )
			iState |= BS_HAS_EXIT_TELEPORTER;
		else if ( pPlayer->ammo_cells >= BUILD_COST_TELEPORTER )
			iState |= BS_CANB_EXIT_TELEPORTER;
	}

	if ( iState == pPlayer->m_iClientBuildState )
		return;
	pPlayer->m_iClientBuildState = iState;

	MESSAGE_BEGIN( MSG_ONE, gmsgBuildState, NULL, pPlayer->pev );
		WRITE_SHORT( iState );
	MESSAGE_END();
}

static CBaseEntity *TF_FindBuilding( CBasePlayer *pPlayer, const char *pszClass )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, pszClass ) ) != NULL )
	{
		if ( (CBaseEntity *)pEnt->real_owner == (CBaseEntity *)pPlayer )
			return pEnt;
	}
	return NULL;
}

static int TF_HealthPercent( CBaseEntity *pEnt )
{
	if ( pEnt->pev->max_health <= 0 )
		return 0;
	int iPct = (int)( pEnt->pev->health / pEnt->pev->max_health * 100.0f );
	return TF_MAX( 0, TF_MIN( 100, iPct ) );
}

// [tfc.so] TeamFortress_UpdateStatusBar engineer part: each entry takes line 1,
// or line 0 once line 1 is used. Line text carries literal "%%" for the client parser.
void TeamFortress_EngineerStatusBar( CBasePlayer *pPlayer, char *sbuf0, char *sbuf1, int *piAmmoPct )
{
	char szTele[SBAR_STRING_SIZE] = "";
	char szTmp[SBAR_STRING_SIZE];

	if ( pPlayer->has_sentry )
	{
		CBaseEntity *pEnt = NULL;

		while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "building_sentrygun" ) ) != NULL )
		{
			if ( (CBaseEntity *)pEnt->real_owner != (CBaseEntity *)pPlayer )
				continue;

			CTFSentrygun *pGun = (CTFSentrygun *)pEnt;
			BOOL bNoRockets = !pGun->ammo_rockets && pGun->m_iLevel == 3;

			_snprintf( sbuf1, SBAR_STRING_SIZE, "#Sentry_sbar %d%%%%\n4 ammo: %%i4%%%%\n0", TF_HealthPercent( pGun ) );
			sbuf1[SBAR_STRING_SIZE - 1] = 0;

			if ( pGun->ammo_shells )
			{
				if ( pGun->maxammo_shells > 0 )
					*piAmmoPct = (int)( (float)pGun->ammo_shells / pGun->maxammo_shells * 100.0f );
				if ( bNoRockets )
					strncat( sbuf1, " #No_Rockets", SBAR_STRING_SIZE - strlen( sbuf1 ) - 1 );
			}
			else
			{
				strncat( sbuf1, bNoRockets ? " #No_Ammo" : " #No_Shells", SBAR_STRING_SIZE - strlen( sbuf1 ) - 1 );
			}
		}
	}

	if ( pPlayer->has_entry_teleporter || pPlayer->has_exit_teleporter )
	{
		CBaseEntity *pEntry = pPlayer->GetTeleporter( BUILD_TELEPORTER_ENTRANCE );
		CBaseEntity *pExit  = pPlayer->GetTeleporter( BUILD_TELEPORTER_EXIT );

		if ( pEntry )
		{
			_snprintf( szTmp, sizeof( szTmp ), "#Teleporter_entry_sbar %d%%%%\n0", TF_HealthPercent( pEntry ) );
			szTmp[sizeof( szTmp ) - 1] = 0;
			strncat( szTele, szTmp, sizeof( szTele ) - strlen( szTele ) - 1 );
		}
		if ( pExit )
		{
			if ( szTele[0] )
				strncat( szTele, "   ", sizeof( szTele ) - strlen( szTele ) - 1 );
			_snprintf( szTmp, sizeof( szTmp ), "#Teleporter_exit_sbar %d%%%%\n0", TF_HealthPercent( pExit ) );
			szTmp[sizeof( szTmp ) - 1] = 0;
			strncat( szTele, szTmp, sizeof( szTele ) - strlen( szTele ) - 1 );
		}

		if ( szTele[0] )
			strcpy( sbuf1[0] ? sbuf0 : sbuf1, szTele );
	}

	if ( pPlayer->is_building )
		strcpy( sbuf1[0] ? sbuf0 : sbuf1, "#Engineer_building" );
}

// [tfc.so] crosshair-ID on a building: any player can read owner/health/armor
// off it, friend or foe -- there is no ally gate, unlike the player-ID branch.
BOOL TeamFortress_GetBuildingIDInfo( CBaseEntity *pEntity, char *sbuf2, int iSbuf2Size, int *piOwnerIndex, int *piHealthPct, int *piArmorPct )
{
	const char *pszTemplate;

	if ( FClassnameIs( pEntity->pev, "building_sentrygun" ) )
		pszTemplate = "1 Sentry gun built by: %p1\n2    Health: %i2%%\n3  Armor: %i3%%";
	else if ( FClassnameIs( pEntity->pev, "building_dispenser" ) )
		pszTemplate = "1 Dispenser built by: %p1\n2    Health: %i2%%\n3  Armor: %i3%%";
	else if ( FClassnameIs( pEntity->pev, "building_teleporter" ) )
		pszTemplate = ( (CTFTeleporter *)pEntity )->IsEntrance()
			? "1 Teleporter entrance built by: %p1\n2    Health: %i2%%\n3  Armor: %i3%%"
			: "1 Teleporter exit built by: %p1\n2    Health: %i2%%\n3  Armor: %i3%%";
	else
		return FALSE;

	CBaseEntity *pOwner = (CBaseEntity *)pEntity->real_owner;
	if ( !pOwner || !pOwner->IsPlayer() )
		return FALSE;

	strncpy( sbuf2, pszTemplate, iSbuf2Size );
	sbuf2[iSbuf2Size - 1] = 0;

	*piOwnerIndex = ENTINDEX( pOwner->edict() );
	*piHealthPct  = TF_HealthPercent( pEntity );
	*piArmorPct   = (int)pEntity->pev->armorvalue;

	return TRUE;
}

// [tfc.so] dismantling hands back a flat 100 metal, whatever the building cost.
void DestroyBuilding( CBaseEntity *eng, char *bld )
{
	CBasePlayer *pPlayer = (CBasePlayer *)eng;
	CBaseEntity *pBuilding;

	if ( !pPlayer || !pPlayer->IsPlayer() )
		return;

	pBuilding = TF_FindBuilding( pPlayer, bld );
	if ( !pBuilding )
		return;

	if ( FStrEq( bld, "building_sentrygun" ) )
		pPlayer->has_sentry = 0;
	else if ( FStrEq( bld, "building_dispenser" ) )
		pPlayer->has_dispenser = 0;

	CBaseEntity *pSection = pBuilding->m_pOtherSection;
	if ( pSection )
	{
		pSection->pev->solid = SOLID_NOT;
		pSection->m_pOtherSection = NULL;
		UTIL_Remove( pSection );
		pBuilding->m_pOtherSection = NULL;
	}

	TF_RemoveEntityEvents( pBuilding );
	pBuilding->real_owner = NULL;
	pBuilding->pev->solid = SOLID_NOT;
	pBuilding->SetThink( NULL );
	pBuilding->SetTouch( NULL );
	UTIL_Remove( pBuilding );

	pPlayer->ammo_cells = TF_MIN( pPlayer->maxammo_cells, pPlayer->ammo_cells + TF_DISMANTLE_REFUND );
	ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY,
	             FStrEq( bld, "building_sentrygun" ) ? "#Sentry_dismantle" : "#Dispenser_dismantle" );
	TeamFortress_SendBuildState( pPlayer );
}

void CBasePlayer::TeamFortress_Build( int iBuildingID )
{
	int iCost, iTime;
	const char *pszClass;

	switch ( iBuildingID )
	{
	case BUILD_DISPENSER:
		if ( has_dispenser )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_onedispenser" );
			return;
		}
		iCost = BUILD_COST_DISPENSER;
		iTime = BUILD_TIME_DISPENSER;
		pszClass = "building_dispenser";
		break;

	case BUILD_SENTRYGUN:
		if ( has_sentry )
		{
			ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_onesentry" );
			return;
		}
		iCost = BUILD_COST_SENTRYGUN;
		iTime = BUILD_TIME_SENTRYGUN;
		pszClass = "building_sentrygun";
		break;

	case BUILD_TELEPORTER_ENTRANCE:
	case BUILD_TELEPORTER_EXIT:
		if ( iBuildingID == BUILD_TELEPORTER_ENTRANCE ? has_entry_teleporter : has_exit_teleporter )
		{
			ClientPrint( pev, HUD_PRINTCENTER,
			             iBuildingID == BUILD_TELEPORTER_ENTRANCE ? "#Build_entryteleporter" : "#Build_exitteleporter" );
			return;
		}
		iCost = BUILD_COST_TELEPORTER;
		iTime = BUILD_TIME_TELEPORTER;
		pszClass = "building_teleporter";
		break;

	default:
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_nobuild" );
		return;
	}

	if ( ammo_cells < iCost )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_notenoughmetal" );
		return;
	}

	// [tfc.so] 64 u ahead at the PLAYER's origin height, X/Y truncated; it drops
	// through CheckBelowBuilding (a floor spot here fails every CheckArea).
	UTIL_MakeAimVectors( pev->angles );
	Vector vecForward = gpGlobals->v_forward;
	vecForward.z = 0;
	vecForward = vecForward.Normalize();

	Vector vecSpot( (float)(int)( pev->origin.x + vecForward.x * TF_BUILD_FORWARD ),
	                (float)(int)( pev->origin.y + vecForward.y * TF_BUILD_FORWARD ),
	                pev->origin.z );

	CBaseEntity *pBuilding = CBaseEntity::Create( (char *)pszClass, vecSpot,
	                                              Vector( 0, pev->v_angle.y, 0 ), edict() );
	if ( !pBuilding )
		return;

	pBuilding->pev->owner = NULL;
	pBuilding->real_owner = this;
	pBuilding->team_no = team_no;
	pBuilding->pev->team = pev->team;

	int iArea = pBuilding->CheckArea( this );
	if ( iArea != CAREA_CLEAR )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, iArea == CAREA_NOBUILD ? "#Build_nobuild" : "#Build_noroom" );
		pBuilding->real_owner = NULL;
		UTIL_Remove( pBuilding );
		return;
	}

	// [tfc.so] team colormap for every building type -- unset colormap 0 was
	// rendering sentry/dispenser with team 2's (red) remap regardless of team.
	if ( team_no >= 0 && team_no < 5 )
	{
		int iTop = teamcolors[team_no][PC_RANDOM].topColor;
		pBuilding->pev->colormap = iTop + ( ( ( iTop + 10 ) << 8 ) & 0xFFFF );
	}

	if ( FClassnameIs( pBuilding->pev, "building_teleporter" ) )
		( (CTFTeleporter *)pBuilding )->m_iType = iBuildingID;

	if ( iBuildingID == BUILD_SENTRYGUN )
		( (CTFSentrygun *)pBuilding )->SetBuildAngles( pev->v_angle.y );
	else
		pBuilding->pev->angles.y = UTIL_AngleMod( pev->v_angle.y + 180.0f );   // [tfc.so] faces the builder

	building = pBuilding;
	is_building = 1;
	tfstate |= TFSTATE_CANT_MOVE;
	pev->flags |= FL_FROZEN;
	TeamFortress_SetSpeed();

	CBaseEntity *pTimer = CreateTimer( TF_TIMER_BUILD );
	if ( pTimer )
	{
		pTimer->SetThink( &CBaseEntity::Timer_FinishedBuilding );
		pTimer->pev->nextthink = gpGlobals->time + iTime;
		pTimer->pev->impulse = iBuildingID;
		pTimer->pev->frags = (float)iCost;
	}

	EMIT_SOUND_DYN( edict(), CHAN_STATIC, TF_SND_BUILDING, 1.0f, ATTN_NORM, 0, PITCH_NORM );
	TeamFortress_SendBuildState( this );
}

// [tfc.so] id 0 cancels an in-progress build; the metal is NOT refunded.
void CBasePlayer::TeamFortress_EngineerBuild( int iBuildingNo )
{
	if ( !TF_IsEngineer( this ) )
		return;

	if ( !( pev->flags & FL_ONGROUND ) )
	{
		ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_air" );
		return;
	}

	if ( is_building )
	{
		if ( is_building != 1 )
			return;

		ClientPrint( pev, HUD_PRINTNOTIFY, "#Build_stop" );
		tfstate &= ~TFSTATE_CANT_MOVE;
		pev->flags &= ~FL_FROZEN;
		TeamFortress_SetSpeed();

		CBaseEntity *pTimer = FindTimer( TF_TIMER_BUILD );
		if ( pTimer )
			UTIL_Remove( pTimer );

		CBaseEntity *pBuilding = building;
		if ( pBuilding )
		{
			pBuilding->real_owner = NULL;
			UTIL_Remove( pBuilding );
		}
		building = NULL;
		is_building = 0;

		EMIT_SOUND_DYN( edict(), CHAN_STATIC, TF_SND_BUILDING, 0, 0, SND_STOP, PITCH_NORM );
		TeamFortress_SendBuildState( this );
		return;
	}

	if ( iBuildingNo > 0 )
		TeamFortress_Build( iBuildingNo );
}

// Runs on the timer entity; its owner is the building engineer.
void CBaseEntity::Timer_FinishedBuilding( void )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	if ( pOwner && pOwner->IsPlayer() )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)pOwner;
		CBaseEntity *pBuilding = pPlayer->building;

		STOP_SOUND( pPlayer->edict(), CHAN_STATIC, TF_SND_BUILDING );
		pPlayer->is_building = 0;
		pPlayer->tfstate &= ~TFSTATE_CANT_MOVE;
		pPlayer->pev->flags &= ~FL_FROZEN;
		pPlayer->TeamFortress_SetSpeed();
		pPlayer->building = NULL;

		if ( pBuilding )
		{
			pPlayer->ammo_cells = TF_MAX( 0, pPlayer->ammo_cells - (int)pev->frags );

			if ( FClassnameIs( pBuilding->pev, "building_sentrygun" ) )
			{
				( (CTFSentrygun *)pBuilding )->Finished();
				pPlayer->has_sentry = 1;
			}
			else if ( FClassnameIs( pBuilding->pev, "building_dispenser" ) )
			{
				( (CTFDispenser *)pBuilding )->Finished();
				pPlayer->has_dispenser = 1;
			}
			else if ( FClassnameIs( pBuilding->pev, "building_teleporter" ) )
			{
				( (CTFTeleporter *)pBuilding )->Finished();
			}
		}

		TeamFortress_SendBuildState( pPlayer );
	}

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time;
}

void CBaseEntity::Timer_CheckBuildDistance( void )
{
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time;
}

void CBasePlayer::Engineer_RemoveBuildings( void )
{
	CBaseEntity *pEnt;

	if ( ( pEnt = TF_FindBuilding( this, "building_sentrygun" ) ) != NULL )
		( (CTFSentrygun *)pEnt )->Detonate();

	if ( ( pEnt = TF_FindBuilding( this, "building_dispenser" ) ) != NULL )
		( (CTFDispenser *)pEnt )->Detonate();

	DestroyTeleporter( this, BUILD_TELEPORTER_ENTRANCE );
	DestroyTeleporter( this, BUILD_TELEPORTER_EXIT );

	has_sentry = 0;
	has_dispenser = 0;
	has_entry_teleporter = 0;
	has_exit_teleporter = 0;
	TeamFortress_SendBuildState( this );
}

void CBasePlayer::TeamFortress_RemoveBuildings( void )
{
	Engineer_RemoveBuildings();

	if ( is_building )
	{
		CBaseEntity *pTimer = FindTimer( TF_TIMER_BUILD );
		if ( pTimer )
			UTIL_Remove( pTimer );

		CBaseEntity *pBuilding = building;
		if ( pBuilding )
		{
			pBuilding->real_owner = NULL;
			UTIL_Remove( pBuilding );
		}
		building = NULL;
		is_building = 0;
		tfstate &= ~TFSTATE_CANT_MOVE;
		pev->flags &= ~FL_FROZEN;
		STOP_SOUND( edict(), CHAN_STATIC, TF_SND_BUILDING );
		TeamFortress_SetSpeed();
		TeamFortress_SendBuildState( this );
	}
}

// The tf15 VGUI command menu is the build UI; these are the strings it sends.
BOOL TeamFortress_BuildCommand( CBasePlayer *pPlayer, const char *pcmd )
{
	// [tfc.so] ClientCommand: the client asks once, from its HUD reset
	if ( FStrEq( pcmd, "sendevents" ) )
	{
		if ( !pPlayer->m_bSentBuildingEvents )
		{
			SendBuildingEventInfo( pPlayer );
			pPlayer->m_bSentBuildingEvents = TRUE;
		}
		return TRUE;
	}

	if ( FStrEq( pcmd, "build" ) )
	{
		pPlayer->TeamFortress_EngineerBuild( CMD_ARGC() >= 2 ? atoi( CMD_ARGV( 1 ) ) : 0 );
		return TRUE;
	}

	if ( FStrEq( pcmd, "dismantle" ) )
	{
		if ( TF_IsEngineer( pPlayer ) && CMD_ARGC() >= 2 )
		{
			int id = atoi( CMD_ARGV( 1 ) );
			if ( id == BUILD_SENTRYGUN )
				DestroyBuilding( pPlayer, (char *)"building_sentrygun" );
			else if ( id == BUILD_DISPENSER )
				DestroyBuilding( pPlayer, (char *)"building_dispenser" );
			else if ( id == BUILD_TELEPORTER_ENTRANCE || id == BUILD_TELEPORTER_EXIT )
			{
				CBaseEntity *pTele = pPlayer->GetTeleporter( id );
				if ( pTele )
					( (CTFTeleporter *)pTele )->Dismantle( pPlayer );
			}
		}
		return TRUE;
	}

	if ( FStrEq( pcmd, "detsentry" ) )
	{
		CBaseEntity *pEnt = TF_FindBuilding( pPlayer, "building_sentrygun" );
		if ( pEnt )
			( (CTFSentrygun *)pEnt )->Detonate();
		return TRUE;
	}

	if ( FStrEq( pcmd, "detdispenser" ) )
	{
		CBaseEntity *pEnt = TF_FindBuilding( pPlayer, "building_dispenser" );
		if ( pEnt )
			( (CTFDispenser *)pEnt )->Detonate();
		return TRUE;
	}

	if ( FStrEq( pcmd, "rotatesentry" ) || FStrEq( pcmd, "rotatesentry180" ) )
	{
		CBaseEntity *pEnt = TF_FindBuilding( pPlayer, "building_sentrygun" );
		if ( pEnt )
		{
			float flTurn = FStrEq( pcmd, "rotatesentry180" ) ? 180.0f : 45.0f;
			( (CTFSentrygun *)pEnt )->SetBuildAngles( pEnt->pev->angles.y + flTurn );
		}
		return TRUE;
	}

	if ( FStrEq( pcmd, "detentryteleporter" ) || FStrEq( pcmd, "detexitteleporter" ) )
	{
		if ( TF_IsEngineer( pPlayer ) )
			DestroyTeleporter( pPlayer, FStrEq( pcmd, "detentryteleporter" ) ? BUILD_TELEPORTER_ENTRANCE : BUILD_TELEPORTER_EXIT );
		return TRUE;
	}

	return FALSE;
}
