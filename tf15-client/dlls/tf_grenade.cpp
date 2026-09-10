/***
*
*	TFC-6 Phase 2 -- hand grenade + the class grenade-2 types.
*
*	tf15-client shipped no grenade gameplay. This wires the prime/throw pipeline
*	and the seven grenade-2 types (concussion, nail, MIRV, napalm, gas, EMP, and
*	the scout's caltrop can).
*
*	Round A (this pass) after the Opus behaviour review:
*	  - fuse is GR_PRIMETIME (3s), caltrop can is GR_CALTROP_PRIME (0.5s).
*	  - concussion: 0 damage, velocity push for EVERYONE in radius (conc jump),
*	    disorient ENEMIES ONLY, ramped down over 5s via the stock m_iConc* fields
*	    (start value 200 -> real +-60deg view sway; 3 was invisible).
*	  - caltrop is a thrown can (tf_weapon_caltropgrenade) that scatters 5
*	    SOLID_TRIGGER shards; a shard does DMG_CALTROP + stacks leg_damage, whose
*	    speed penalty lives in CBasePlayer::TeamFortress_SetSpeed so nothing wipes
*	    it. No friendly caltrops.
*	  - every type plays its real client FX event (tf_concuss.sc etc, already
*	    precached in world.cpp) instead of a generic TE_EXPLOSION -- real sounds.
*	  - effect loops collect victims first, then apply (a killed victim mid-walk
*	    was a latent use-after-free).
*	Round B: nail-grenade rotation, per-tick napalm fire field, partial EMP ammo,
*	active-grenade caps, death messages.
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

extern int gmsgGrenades;
extern int gmsgStatusIcon;
extern int gmsgConcuss;

#define TF_GREN_MODEL       "models/grenade.mdl"   // precached by W_Precache (weapons.cpp)
#define TF_GREN_ICON        "grenade"              // status_icons.cpp keys the timer beep on this substring

#define TF_DMG_NORMAL       120
#define TF_RADIUS_NORMAL    240.0f                 // ::RadiusDamage falloff = dmg/radius -> matches QWTF at dmg 120
#define TF_DMG_MIRV_MAIN    90
#define TF_DMG_MIRV_LET     70
#define TF_DMG_EMP_BASE     45

#define TF_CONC_RADIUS      240.0f
#define TF_CONC_STARTVAL    200      // client v_idlescale; -> ~+-60deg yaw sway
#define TF_CONC_DURATION    5.0f
#define TF_CONC_PUSH        900.0f   // peak conc-jump impulse at zero distance

#define TF_GAS_STARTVAL     120
#define TF_GAS_DURATION     14.0f
#define TF_GAS_RADIUS       265.0f

#define TF_NAIL_LIFETIME    4.0f
#define TF_NAIL_PULSE       0.3f
#define TF_NAIL_PULSEDMG    9
#define TF_NAIL_RADIUS      170.0f

#define TF_NAPALM_BLAST     20.0f
#define TF_NAPALM_RADIUS    200.0f
#define TF_NAPALM_BURN      5.0f
#define TF_BURN_TICK        1.0f
#define TF_BURN_TICKDMG     8

#define TF_CALTROP_SHARDS   5
#define TF_CALTROP_DMG      6
#define TF_CALTROP_LIFETIME 12.0f
#define TF_LEG_MAX          6.0f

static unsigned short g_usTFConc, g_usTFNormal, g_usTFGas, g_usTFEmp;
static unsigned short g_usTFFire, g_usTFMirvMain, g_usTFNail;

static void TF_DiagG( const char *fmt, ... )
{
	char buf[192];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	g_engfuncs.pfnServerPrint( buf );
}

// weapons.h declares the 7-arg global ::RadiusDamage; CBaseMonster hides it with
// its own 5/6-arg member inside these methods, so it is always called qualified.

static void TF_PlayFX( edict_t *ed, unsigned short us, const Vector &org )
{
	if ( !us )
		return;
	Vector o = org;
	PLAYBACK_EVENT_FULL( FEV_GLOBAL, ed, us, 0.0f, (float *)&o, (float *)&o, 0.0f, 0.0f, 0, 0, 0, 0 );
}

//=========================================================
// Throw geometry + prime timing.
//=========================================================
static float TF_PrimeSeconds( int grtype )
{
	return ( grtype == GR_TYPE_CALTROP ) ? (float)GR_CALTROP_PRIME : (float)GR_PRIMETIME;
}

static void TF_ThrowVectors( CBasePlayer *pOwner, Vector &vSrc, Vector &vVel )
{
	Vector angThrow = pOwner->pev->v_angle + pOwner->pev->punchangle;

	if ( angThrow.x < 0.0f )
		angThrow.x = -10.0f + angThrow.x * ( ( 90.0f - 10.0f ) / 90.0f );
	else
		angThrow.x = -10.0f + angThrow.x * ( ( 90.0f + 10.0f ) / 90.0f );

	float flVel = ( 90.0f - angThrow.x ) * 4.0f;
	if ( flVel > 500.0f )
		flVel = 500.0f;

	UTIL_MakeVectors( angThrow );
	vSrc = pOwner->pev->origin + pOwner->pev->view_ofs + gpGlobals->v_forward * 16.0f;
	vVel = gpGlobals->v_forward * flVel + pOwner->pev->velocity;
}

//=========================================================
// Player-effect helpers.
//=========================================================
static bool TF_IsEnemyOf( CBaseEntity *pTarget, CBaseEntity *pOwner )
{
	if ( !pOwner || pTarget == pOwner )
		return false;
	return g_pGameRules->PlayerRelationship( pTarget, pOwner ) != GR_TEAMMATE;
}

static bool TF_HasLOS( edict_t *pGren, const Vector &org, CBaseEntity *pTarget )
{
	TraceResult tr;
	UTIL_TraceLine( org, pTarget->pev->origin + pTarget->pev->view_ofs, ignore_monsters, pGren, &tr );
	return tr.flFraction >= 1.0f || tr.pHit == pTarget->edict();
}

// Restart the disorientation ramp from max( current, new ). One ramp serves both
// the concussion grenade and the gas grenade.
static void TF_AddConcussion( CBasePlayer *pl, int startval, float duration )
{
	pl->m_iConcStartVal   = ( startval > pl->m_iConcussion ) ? startval : pl->m_iConcussion;
	pl->m_flConcStartTime = gpGlobals->time;
	pl->m_flConcDuration  = duration;
}

// The server velocity write is authoritative: the engine copies ent->v.velocity
// into pmove before PM_Move (sv_pmove.c), and TFC grenade think runs from
// PlayerThink inside CBasePlayer::PreThink. Same one-shot pattern as trigger_push
// (velocity += push, clear FL_ONGROUND); basevelocity is for conveyors only.
// frac is the pre-computed 0..1 blast strength (point-blank == 1).
static void TF_ConcPush( CBasePlayer *pl, const Vector &vecSrc, float flMax, float frac )
{
	Vector vecDir = pl->pev->origin - vecSrc;
	if ( vecDir.Length() < 48.0f )
		vecDir = Vector( 0, 0, 1 );      // in-hand / at-feet: launch straight up
	else
		vecDir = vecDir.Normalize();

	pl->pev->velocity = pl->pev->velocity + vecDir * ( flMax * frac );
	pl->pev->flags &= ~FL_ONGROUND;      // or ground friction eats it next frame
}

static void TF_ApplyBurn( CBasePlayer *pl, entvars_t *pevAttacker )
{
	pl->m_flTFBurnEnd = gpGlobals->time + TF_NAPALM_BURN;
	if ( pl->m_flTFBurnNextTick < gpGlobals->time )
		pl->m_flTFBurnNextTick = gpGlobals->time + TF_BURN_TICK;
	pl->tfstate |= TFSTATE_BURNING;
	pl->m_hTFEffectAttacker = pevAttacker ? CBaseEntity::Instance( pevAttacker ) : NULL;
}

//=========================================================
// Collect living players in a sphere (optionally enemies-only + LOS). Returns
// count; caller applies the effect AFTER the walk, so killing a victim can't
// corrupt UTIL_FindEntityInSphere's resume pointer.
//=========================================================
static int TF_GatherPlayers( edict_t *pGren, const Vector &org, float radius,
                             CBaseEntity *pOwner, bool enemiesOnly, bool needLOS,
                             CBaseEntity **out, int maxOut )
{
	int n = 0;
	CBaseEntity *pe = NULL;
	while ( n < maxOut && ( pe = UTIL_FindEntityInSphere( pe, org, radius ) ) != NULL )
	{
		if ( !pe->IsPlayer() || !pe->IsAlive() )
			continue;
		if ( enemiesOnly && !TF_IsEnemyOf( pe, pOwner ) )
			continue;
		if ( needLOS && !TF_HasLOS( pGren, org, pe ) )
			continue;
		out[n++] = pe;
	}
	return n;
}

//=========================================================
// CTFTossGrenade -- base for every thrown grenade. The tumble think is a copy
// of stock CGrenade::TumbleThink; at the fuse it calls the VIRTUAL Detonate2().
//=========================================================
class CTFTossGrenade : public CGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	void EXPORT TossThink( void );
	void EXPORT NailSpin( void );
	virtual void Detonate2( void );   // default: normal blast

	static CTFTossGrenade *TossCreate( const char *classname, edict_t *pOwner,
	                                   const Vector &vOrigin, const Vector &vVel, float fuse );
};

void CTFTossGrenade::Precache( void )
{
	PRECACHE_MODEL( TF_GREN_MODEL );
	PRECACHE_SOUND( "weapons/timer.wav" );
	PRECACHE_SOUND( "weapons/tink1.wav" );

	// Handles for the client FX events world.cpp already precaches. PRECACHE_EVENT
	// dedupes by name, so re-registering here just fetches the index.
	g_usTFConc     = PRECACHE_EVENT( 1, "events/explode/tf_concuss.sc" );
	g_usTFNormal   = PRECACHE_EVENT( 1, "events/explode/tf_normalgren.sc" );
	g_usTFGas      = PRECACHE_EVENT( 1, "events/explode/tf_gas.sc" );
	g_usTFEmp      = PRECACHE_EVENT( 1, "events/explode/tf_emp.sc" );
	g_usTFFire     = PRECACHE_EVENT( 1, "events/explode/tf_fire.sc" );
	g_usTFMirvMain = PRECACHE_EVENT( 1, "events/explode/tf_mirvmain.sc" );
	g_usTFNail     = PRECACHE_EVENT( 1, "events/explode/tf_ng.sc" );
}

void CTFTossGrenade::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid    = SOLID_BBOX;
	SET_MODEL( ENT( pev ), TF_GREN_MODEL );
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	pev->dmg = TF_DMG_NORMAL;
	m_fRegisteredSound = FALSE;
	// classname stays whatever CREATE_NAMED_ENTITY set -- used by RemoveLiveGrenades
}

CTFTossGrenade *CTFTossGrenade::TossCreate( const char *classname, edict_t *pOwner,
                                           const Vector &vOrigin, const Vector &vVel, float fuse )
{
	CBaseEntity *pEnt = CBaseEntity::Create( classname, vOrigin, g_vecZero, pOwner );
	if ( !pEnt )
	{
		TF_DiagG( "[tfc] grenade Create(%s) NULL -- missing from exports.txt?\n", classname );
		return NULL;
	}

	CTFTossGrenade *pGren = (CTFTossGrenade *)pEnt;

	pGren->pev->owner     = pOwner;
	pGren->pev->velocity  = vVel;
	pGren->pev->angles    = UTIL_VecToAngles( vVel );
	pGren->pev->avelocity = Vector( RANDOM_FLOAT( -100, -500 ), 0, 0 );
	pGren->pev->gravity   = 0.5f;
	pGren->pev->friction  = 0.8f;
	pGren->pev->sequence  = RANDOM_LONG( 3, 6 );
	pGren->pev->framerate = 1.0f;

	pGren->pev->dmgtime = gpGlobals->time + fuse;
	pGren->SetTouch( &CGrenade::BounceTouch );
	pGren->SetThink( &CTFTossGrenade::TossThink );
	pGren->pev->nextthink = gpGlobals->time + ( fuse < 0.1f ? 0.0f : 0.1f );
	if ( fuse < 0.1f )
		pGren->pev->velocity = g_vecZero;

	return pGren;
}

void CTFTossGrenade::TossThink( void )
{
	if ( !IsInWorld() )
	{
		UTIL_Remove( this );
		return;
	}

	StudioFrameAdvance();
	pev->nextthink = gpGlobals->time + 0.1f;

	if ( pev->dmgtime <= gpGlobals->time )
	{
		Detonate2();          // virtual -> per-type payload
		return;
	}

	if ( pev->waterlevel != 0 )
	{
		pev->velocity = pev->velocity * 0.5f;
		pev->framerate = 0.2f;
	}
}

static entvars_t *TF_GrenOwner( CBaseEntity *pGren )
{
	return pGren->pev->owner ? VARS( pGren->pev->owner ) : pGren->pev;
}

// Base payload = the stock normal grenade blast, but with the QWTF-matching
// explicit radius (stock CGrenade::Detonate uses dmg*2.5 -> 300, too wide).
void CTFTossGrenade::Detonate2( void )
{
	Vector     org    = pev->origin;
	entvars_t *pevAtk = TF_GrenOwner( this );

	TF_DiagG( "[tfc] detonate cls=%s dmg=%d\n", STRING( pev->classname ), (int)pev->dmg );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	pev->takedamage = DAMAGE_NO;

	TF_PlayFX( edict(), g_usTFNormal, org );
	::RadiusDamage( org, pev, pevAtk, pev->dmg, pev->dmg * 2.0f, CLASS_NONE, DMG_BLAST );

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFTossGrenade::NailSpin( void )
{
	entvars_t *pevAtk = TF_GrenOwner( this );

	if ( !IsInWorld() || pev->dmgtime <= gpGlobals->time )
	{
		UTIL_Remove( this );
		return;
	}

	StudioFrameAdvance();
	::RadiusDamage( pev->origin, pev, pevAtk, TF_NAIL_PULSEDMG, TF_NAIL_RADIUS, CLASS_NONE, DMG_BULLET );

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_SPARKS );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z );
	MESSAGE_END();

	pev->nextthink = gpGlobals->time + TF_NAIL_PULSE;
}

//=========================================================
// Type subclasses -- classname carries the type, Detonate2 carries the payload.
//=========================================================
#define TF_GREN_SUBCLASS( CLS, NAME ) \
	class CLS : public CTFTossGrenade { public: void Detonate2( void ); }; \
	LINK_ENTITY_TO_CLASS( NAME, CLS )

LINK_ENTITY_TO_CLASS( tf_weapon_normalgrenade, CTFTossGrenade )   // base = normal blast

TF_GREN_SUBCLASS( CTFConcGrenade,    tf_weapon_concussiongrenade );
TF_GREN_SUBCLASS( CTFNailGrenade,    tf_weapon_nailgrenade );
TF_GREN_SUBCLASS( CTFMirvGrenade,    tf_weapon_mirvgrenade );
TF_GREN_SUBCLASS( CTFNapalmGrenade,  tf_weapon_napalmgrenade );
TF_GREN_SUBCLASS( CTFGasGrenade,     tf_weapon_gasgrenade );
TF_GREN_SUBCLASS( CTFEmpGrenade,     tf_weapon_empgrenade );
TF_GREN_SUBCLASS( CTFCaltropGrenade, tf_weapon_caltropgrenade );

void CTFConcGrenade::Detonate2( void )
{
	Vector       org      = pev->origin;
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	TF_DiagG( "[tfc] detonate cls=%s (concussion) usFX=%u\n", STRING( pev->classname ), (unsigned)g_usTFConc );

	TF_PlayFX( edict(), g_usTFConc, org );

	CBaseEntity *push[32];
	int nPush = TF_GatherPlayers( edict(), org, TF_CONC_RADIUS, pThrower, false, true, push, 32 );
	for ( int i = 0; i < nPush; i++ )
	{
		CBasePlayer *pl = (CBasePlayer *)push[i];

		float dist = ( pl->pev->origin - org ).Length();
		float frac = ( dist < 48.0f ) ? 1.0f : ( 1.0f - dist / TF_CONC_RADIUS );
		if ( frac < 0.0f ) frac = 0.0f;

		TF_ConcPush( pl, org, TF_CONC_PUSH, frac );

		// Disorient scales with proximity. An enemy at the blast gets it all; a
		// conc-jumper (grenade lands far) gets a brief swim; a grenade held to
		// the fuse and blown at the feet gets the full punishment.
		if ( !TF_IsEnemyOf( pl, pThrower ) && pl != pThrower )
			continue;                                  // teammates: push only

		int startval = (int)( TF_CONC_STARTVAL * frac );
		if ( pl == pThrower )
			startval = (int)( startval * 0.6f );        // your own throw: softened
		if ( startval > 20 )
			TF_AddConcussion( pl, startval, TF_CONC_DURATION );
	}
	UTIL_Remove( this );
}

void CTFGasGrenade::Detonate2( void )
{
	Vector       org      = pev->origin;
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	TF_DiagG( "[tfc] detonate cls=%s (gas)\n", STRING( pev->classname ) );

	TF_PlayFX( edict(), g_usTFGas, org );

	CBaseEntity *v[32];
	int n = TF_GatherPlayers( edict(), org, TF_GAS_RADIUS, pThrower, true, true, v, 32 );
	for ( int i = 0; i < n; i++ )
	{
		( (CBasePlayer *)v[i] )->tfstate |= TFSTATE_HALLUCINATING;
		TF_AddConcussion( (CBasePlayer *)v[i], TF_GAS_STARTVAL, TF_GAS_DURATION );
	}
	UTIL_Remove( this );
}

void CTFNapalmGrenade::Detonate2( void )
{
	Vector       org      = pev->origin;
	entvars_t   *pevAtk   = TF_GrenOwner( this );
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	TF_DiagG( "[tfc] detonate cls=%s (napalm)\n", STRING( pev->classname ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	TF_PlayFX( edict(), g_usTFFire, org );
	::RadiusDamage( org, pev, pevAtk, TF_NAPALM_BLAST, TF_NAPALM_RADIUS, CLASS_NONE, DMG_BLAST | DMG_BURN );

	CBaseEntity *v[32];
	int n = TF_GatherPlayers( edict(), org, TF_NAPALM_RADIUS, pThrower, false, true, v, 32 );
	for ( int i = 0; i < n; i++ )
		TF_ApplyBurn( (CBasePlayer *)v[i], pevAtk );

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFEmpGrenade::Detonate2( void )
{
	Vector       org      = pev->origin;
	entvars_t   *pevAtk   = TF_GrenOwner( this );
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	TF_DiagG( "[tfc] detonate cls=%s (emp)\n", STRING( pev->classname ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	TF_PlayFX( edict(), g_usTFEmp, org );
	::RadiusDamage( org, pev, pevAtk, TF_DMG_EMP_BASE, TF_RADIUS_NORMAL, CLASS_NONE, DMG_BLAST );

	CBaseEntity *v[32];
	int n = TF_GatherPlayers( edict(), org, TF_RADIUS_NORMAL, pThrower, true, true, v, 32 );
	for ( int i = 0; i < n; i++ )
	{
		CBasePlayer *pl = (CBasePlayer *)v[i];
		int cooked = pl->ammo_shells + pl->ammo_cells * 2 + pl->ammo_rockets * 3 + pl->ammo_nails / 2;
		if ( cooked <= 0 )
			continue;

		// destroy half the volatile ammo, damage scaled by what went off
		pl->ammo_shells  -= pl->ammo_shells  / 2;
		pl->ammo_cells   -= pl->ammo_cells   / 2;
		pl->ammo_rockets -= pl->ammo_rockets / 2;
		pl->ammo_nails   -= pl->ammo_nails   / 4;

		float dmg = cooked * 0.35f;
		if ( dmg > 120.0f )
			dmg = 120.0f;
		pl->TakeDamage( pev, pevAtk, dmg, DMG_BLAST );
	}

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFMirvGrenade::Detonate2( void )
{
	Vector     org    = pev->origin;
	entvars_t *pevAtk = TF_GrenOwner( this );
	TF_DiagG( "[tfc] detonate cls=%s (mirv)\n", STRING( pev->classname ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	TF_PlayFX( edict(), g_usTFMirvMain, org );
	::RadiusDamage( org, pev, pevAtk, TF_DMG_MIRV_MAIN, TF_RADIUS_NORMAL, CLASS_NONE, DMG_BLAST );

	for ( int i = 0; i < GR_TYPE_MIRV_NO; i++ )
	{
		Vector vel( RANDOM_FLOAT( -220, 220 ), RANDOM_FLOAT( -220, 220 ), RANDOM_FLOAT( 250, 400 ) );
		CTFTossGrenade *pLet = CTFTossGrenade::TossCreate( "tf_weapon_normalgrenade", pev->owner,
		                                                   org + Vector( 0, 0, 8 ), vel,
		                                                   RANDOM_FLOAT( 1.0f, 1.8f ) );
		if ( pLet )
			pLet->pev->dmg = TF_DMG_MIRV_LET;
	}

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CTFNailGrenade::Detonate2( void )
{
	TF_DiagG( "[tfc] detonate cls=%s (nail spin)\n", STRING( pev->classname ) );
	TF_PlayFX( edict(), g_usTFNail, pev->origin );

	pev->origin.z += 32.0f;
	UTIL_SetOrigin( pev, pev->origin );
	pev->dmgtime   = gpGlobals->time + TF_NAIL_LIFETIME;
	pev->velocity  = g_vecZero;
	pev->avelocity = Vector( 0, 700, 0 );
	pev->movetype  = MOVETYPE_NONE;
	SetThink( &CTFTossGrenade::NailSpin );
	pev->nextthink = gpGlobals->time;
}

void CTFCaltropGrenade::Detonate2( void )
{
	TF_DiagG( "[tfc] detonate cls=%s (caltrop can)\n", STRING( pev->classname ) );

	for ( int i = 0; i < TF_CALTROP_SHARDS; i++ )
	{
		CBaseEntity *pShard = CBaseEntity::Create( "tf_weapon_caltrop",
		                                           pev->origin + Vector( 0, 0, 8 ),
		                                           g_vecZero, pev->owner );
		if ( pShard )
			pShard->pev->velocity = Vector( RANDOM_FLOAT( -140, 140 ),
			                                RANDOM_FLOAT( -140, 140 ),
			                                RANDOM_FLOAT( 80, 180 ) );
	}
	EMIT_SOUND( ENT( pev ), CHAN_WEAPON, "weapons/tink1.wav", 0.8f, ATTN_NORM );
	UTIL_Remove( this );
}

//=========================================================
// CTFCaltrop -- one scattered caltrop. Slows the first enemy to step on it.
//=========================================================
class CTFCaltrop : public CBaseEntity
{
public:
	void Spawn( void );
	void Precache( void );
	void EXPORT ShardTouch( CBaseEntity *pOther );
	void EXPORT ShardExpire( void );
};

LINK_ENTITY_TO_CLASS( tf_weapon_caltrop, CTFCaltrop )

void CTFCaltrop::Precache( void )
{
	PRECACHE_MODEL( "models/caltrop.mdl" );
	PRECACHE_SOUND( "weapons/tink1.wav" );
}

void CTFCaltrop::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_TOSS;
	pev->solid = SOLID_TRIGGER;   // walk-over, not a floor obstacle
	SET_MODEL( ENT( pev ), "models/caltrop.mdl" );
	UTIL_SetSize( pev, Vector( -4, -4, -2 ), Vector( 4, 4, 2 ) );

	SetTouch( &CTFCaltrop::ShardTouch );
	SetThink( &CTFCaltrop::ShardExpire );
	pev->nextthink = gpGlobals->time + TF_CALTROP_LIFETIME;
}

void CTFCaltrop::ShardTouch( CBaseEntity *pOther )
{
	if ( !pOther->IsPlayer() || !pOther->IsAlive() )
		return;

	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	if ( pOther == pOwner )
		return;
	if ( pOwner && g_pGameRules->PlayerRelationship( pOther, pOwner ) == GR_TEAMMATE )
		return;   // no friendly caltrops

	CBasePlayer *pl = (CBasePlayer *)pOther;

	pl->TakeDamage( pev, pOwner ? pOwner->pev : pev, TF_CALTROP_DMG, DMG_CALTROP );

	pl->leg_damage += 1.0f;
	if ( pl->leg_damage > TF_LEG_MAX )
		pl->leg_damage = TF_LEG_MAX;
	pl->TeamFortress_SetSpeed();

	EMIT_SOUND( ENT( pev ), CHAN_BODY, "weapons/tink1.wav", 0.6f, ATTN_NORM );
	UTIL_Remove( this );
}

void CTFCaltrop::ShardExpire( void )
{
	UTIL_Remove( this );
}

//=========================================================
// Per-class grenade loadout, straight off tf_defs.h's PC_*_GRENADE_* defines.
//=========================================================
struct tf_gren_row_t { int type1, init1, type2, init2; };

#define TFGRENROW( P ) { PC_##P##_GRENADE_TYPE_1, PC_##P##_GRENADE_INIT_1, \
                         PC_##P##_GRENADE_TYPE_2, PC_##P##_GRENADE_INIT_2 }

static const tf_gren_row_t sTFGren[PC_LASTCLASS] =
{
	{ 0, 0, 0, 0 },            // PC_UNDEFINED
	TFGRENROW( SCOUT ),
	TFGRENROW( SNIPER ),
	TFGRENROW( SOLDIER ),
	TFGRENROW( DEMOMAN ),
	TFGRENROW( MEDIC ),
	TFGRENROW( HVYWEAP ),
	TFGRENROW( PYRO ),
	TFGRENROW( SPY ),
	TFGRENROW( ENGINEER ),
	{ 0, 0, 0, 0 },            // PC_RANDOM
	TFGRENROW( CIVILIAN ),
};

static const char *TF_GrenClassname( int grtype )
{
	switch ( grtype )
	{
	case GR_TYPE_CONCUSSION: return "tf_weapon_concussiongrenade";
	case GR_TYPE_NAIL:       return "tf_weapon_nailgrenade";
	case GR_TYPE_MIRV:       return "tf_weapon_mirvgrenade";
	case GR_TYPE_NAPALM:     return "tf_weapon_napalmgrenade";
	case GR_TYPE_GAS:        return "tf_weapon_gasgrenade";
	case GR_TYPE_EMP:        return "tf_weapon_empgrenade";
	case GR_TYPE_CALTROP:    return "tf_weapon_caltropgrenade";
	default:                 return "tf_weapon_normalgrenade";
	}
}

//=========================================================
// HUD count. gmsgGrenades ("SecAmmoVal") is a per-index 2-byte message
// (index, value) -- MsgFunc_SecAmmoVal, cl_dll/ammo_secondary.cpp.
//=========================================================
void TeamFortress_SendGrenadeCounts( CBasePlayer *pPlayer )
{
	MESSAGE_BEGIN( MSG_ONE, gmsgGrenades, NULL, pPlayer->edict() );
		WRITE_BYTE( 0 );
		WRITE_BYTE( pPlayer->no_grenades_1 );
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_ONE, gmsgGrenades, NULL, pPlayer->edict() );
		WRITE_BYTE( 1 );
		WRITE_BYTE( pPlayer->no_grenades_2 );
	MESSAGE_END();
}

static void TeamFortress_SetGrenadeIcon( CBasePlayer *pPlayer, BOOL bEnable )
{
	MESSAGE_BEGIN( MSG_ONE, gmsgStatusIcon, NULL, pPlayer->edict() );
		WRITE_BYTE( bEnable ? 1 : 0 );
		WRITE_STRING( TF_GREN_ICON );
		if ( bEnable )
		{
			WRITE_BYTE( 255 );
			WRITE_BYTE( 160 );
			WRITE_BYTE( 0 );
		}
	MESSAGE_END();
}

static void TeamFortress_ClearPrime( CBasePlayer *pPlayer )
{
	pPlayer->tfstate &= ~TFSTATE_GRENPRIMED;
	pPlayer->m_iPrimedGrenType = GR_TYPE_NONE;
	pPlayer->m_iPrimedGrenSlot = 0;
	TeamFortress_SetGrenadeIcon( pPlayer, FALSE );
}

//=========================================================
// Remove this player's live thrown grenades (class change / respawn).
//=========================================================
void CBasePlayer::TeamFortress_RemoveLiveGrenades( void )
{
	static const char *kClasses[] =
	{
		"tf_weapon_normalgrenade", "tf_weapon_concussiongrenade", "tf_weapon_nailgrenade",
		"tf_weapon_mirvgrenade", "tf_weapon_napalmgrenade", "tf_weapon_gasgrenade",
		"tf_weapon_empgrenade", "tf_weapon_caltropgrenade", "tf_weapon_caltrop",
	};
	edict_t *pMe = edict();

	for ( int i = 0; i < (int)ARRAYSIZE( kClasses ); i++ )
	{
		CBaseEntity *pEnt = NULL;
		while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, kClasses[i] ) ) != NULL )
		{
			if ( pEnt->pev->owner == pMe )
				UTIL_Remove( pEnt );
		}
	}
}

//=========================================================
// Spawn: seed the class grenade counts, reset primed + lingering effect state.
//=========================================================
void TeamFortress_SetupGrenades( CBasePlayer *pPlayer )
{
	int pc = pPlayer->pev->playerclass;
	const tf_gren_row_t *g = ( pc > PC_UNDEFINED && pc < PC_LASTCLASS ) ? &sTFGren[pc] : NULL;

	pPlayer->TeamFortress_RemoveLiveGrenades();
	TeamFortress_ClearPrime( pPlayer );

	pPlayer->m_iConcussion = pPlayer->m_iConcStartVal = 0;
	pPlayer->m_flConcStartTime = pPlayer->m_flConcDuration = 0;
	pPlayer->leg_damage = pPlayer->old_leg_damage = 0;
	pPlayer->m_flTFBurnEnd = 0;
	pPlayer->tfstate &= ~( TFSTATE_HALLUCINATING | TFSTATE_BURNING );
	if ( pPlayer->m_iClientConcussion != 0 )
	{
		pPlayer->m_iClientConcussion = 0;
		MESSAGE_BEGIN( MSG_ONE, gmsgConcuss, NULL, pPlayer->edict() );
			WRITE_BYTE( 0 );
		MESSAGE_END();
	}

	if ( g )
	{
		pPlayer->tp_grenades_1 = g->type1;
		pPlayer->tp_grenades_2 = g->type2;
		pPlayer->no_grenades_1 = g->init1;
		pPlayer->no_grenades_2 = g->init2;
	}
	else
	{
		pPlayer->tp_grenades_1 = pPlayer->tp_grenades_2 = GR_TYPE_NONE;
		pPlayer->no_grenades_1 = pPlayer->no_grenades_2 = 0;
	}

	TeamFortress_SendGrenadeCounts( pPlayer );
}

//=========================================================
// Prime / throw.
//=========================================================
void CBasePlayer::TeamFortress_PrimeGrenade( int iSlot )
{
	if ( pev->deadflag != DEAD_NO )
		return;
	if ( pev->playerclass < PC_SCOUT || pev->playerclass >= PC_RANDOM )
		return;
	if ( tfstate & TFSTATE_GRENPRIMED )
		return;

	int iType  = ( iSlot == 2 ) ? tp_grenades_2 : tp_grenades_1;
	int iCount = ( iSlot == 2 ) ? no_grenades_2 : no_grenades_1;

	if ( iType == GR_TYPE_NONE || iCount <= 0 )
	{
		TF_DiagG( "[tfc] prime slot=%d rejected (type=%d count=%d)\n", iSlot, iType, iCount );
		return;
	}

	tfstate |= TFSTATE_GRENPRIMED;
	m_iPrimedGrenType    = iType;
	m_iPrimedGrenSlot    = iSlot;
	m_flGrenadePrimeTime = gpGlobals->time;

	TeamFortress_SetGrenadeIcon( this, TRUE );
	TF_DiagG( "[tfc] prime slot=%d type=%d count=%d\n", iSlot, iType, iCount );
}

void CBasePlayer::TeamFortress_ThrowPrimedGrenade( void )
{
	if ( !( tfstate & TFSTATE_GRENPRIMED ) )
		return;

	int iSlot = m_iPrimedGrenSlot ? m_iPrimedGrenSlot : 1;
	int iType = m_iPrimedGrenType ? m_iPrimedGrenType : GR_TYPE_NORMAL;

	float flPrime = TF_PrimeSeconds( iType );
	float fuse = flPrime - ( gpGlobals->time - m_flGrenadePrimeTime );
	if ( fuse < 0.0f )    fuse = 0.0f;
	if ( fuse > flPrime ) fuse = flPrime;   // stale prime time must never stop detonation

	Vector vSrc, vVel;
	TF_ThrowVectors( this, vSrc, vVel );

	// Held to the fuse -> it goes off in the hand: drop it just above the current
	// hull bottom (pev->mins tracks stand vs duck) so it is at the feet, not 16u
	// out in front of the face. TF_ConcPush's point-blank branch then launches
	// the player straight up regardless of the exact offset.
	if ( fuse < 0.1f )
	{
		vSrc = pev->origin;
		vSrc.z += pev->mins.z + 4.0f;
		vVel = g_vecZero;
	}

	CTFTossGrenade::TossCreate( TF_GrenClassname( iType ), edict(), vSrc, vVel, fuse );

	if ( iSlot == 2 )
		no_grenades_2 = ( no_grenades_2 > 0 ) ? no_grenades_2 - 1 : 0;
	else
		no_grenades_1 = ( no_grenades_1 > 0 ) ? no_grenades_1 - 1 : 0;

	TF_DiagG( "[tfc] throw slot=%d type=%d fuse=%.1f left=%d\n",
	          iSlot, iType, fuse, iSlot == 2 ? no_grenades_2 : no_grenades_1 );

	TeamFortress_ClearPrime( this );
	TeamFortress_SendGrenadeCounts( this );
}

//=========================================================
// Per-frame: prime auto-detonate + lingering effect upkeep. From PlayerThink.
//=========================================================
void TeamFortress_GrenadeThink( CBasePlayer *pPlayer )
{
	float now = gpGlobals->time;

	// blow in hand if held past the (per-type) prime time
	if ( pPlayer->tfstate & TFSTATE_GRENPRIMED )
	{
		float flPrime = TF_PrimeSeconds( pPlayer->m_iPrimedGrenType );
		if ( now >= pPlayer->m_flGrenadePrimeTime + flPrime )
		{
			TF_DiagG( "[tfc] grenade blew in hand\n" );
			pPlayer->TeamFortress_ThrowPrimedGrenade();
		}
	}

	// concussion / gas disorientation ramp -> gmsgConcuss on change
	int want = 0;
	if ( pPlayer->m_flConcDuration > 0.0f )
	{
		float t = ( now - pPlayer->m_flConcStartTime ) / pPlayer->m_flConcDuration;
		if ( t >= 1.0f )
			pPlayer->m_flConcDuration = 0.0f;
		else
		{
			want = (int)( pPlayer->m_iConcStartVal * ( 1.0f - t ) );
			want -= want % GR_CONCUSS_DEC;        // quantise -> caps the resend rate
			if ( want > 255 ) want = 255;
		}
	}
	pPlayer->m_iConcussion = want;
	if ( pPlayer->m_iConcussion != pPlayer->m_iClientConcussion )
	{
		pPlayer->m_iClientConcussion = pPlayer->m_iConcussion;
		MESSAGE_BEGIN( MSG_ONE, gmsgConcuss, NULL, pPlayer->edict() );
			WRITE_BYTE( pPlayer->m_iConcussion );
		MESSAGE_END();
	}
	if ( want == 0 )
		pPlayer->tfstate &= ~TFSTATE_HALLUCINATING;

	// napalm burn DoT
	if ( pPlayer->m_flTFBurnEnd > now )
	{
		if ( pPlayer->m_flTFBurnNextTick <= now )
		{
			pPlayer->m_flTFBurnNextTick = now + TF_BURN_TICK;
			entvars_t *pevAtk = pPlayer->m_hTFEffectAttacker ?
			                    pPlayer->m_hTFEffectAttacker->pev : pPlayer->pev;
			pPlayer->TakeDamage( pevAtk, pevAtk, TF_BURN_TICKDMG, DMG_BURN );
		}
	}
	else if ( pPlayer->tfstate & TFSTATE_BURNING )
	{
		pPlayer->tfstate &= ~TFSTATE_BURNING;
	}

	// leg_damage (caltrop / legshot slow) decays ~1 point / 2s
	if ( pPlayer->leg_damage > 0.0f )
	{
		pPlayer->leg_damage -= gpGlobals->frametime * 0.5f;
		if ( pPlayer->leg_damage < 0.0f )
			pPlayer->leg_damage = 0.0f;
		if ( (int)pPlayer->leg_damage != (int)pPlayer->old_leg_damage )
		{
			pPlayer->old_leg_damage = pPlayer->leg_damage;
			pPlayer->TeamFortress_SetSpeed();
		}
	}
}

//=========================================================
// Command dispatch -- from TeamFortress_ClientCommand (tf_client.cpp). The
// engine forwards the unknown +gren1/-gren1/+gren2/-gren2 console commands.
//=========================================================
BOOL TeamFortress_GrenadeCommand( CBasePlayer *pPlayer, const char *pcmd )
{
	if ( FStrEq( pcmd, "+gren1" ) )
	{
		pPlayer->TeamFortress_PrimeGrenade( 1 );
		return TRUE;
	}
	if ( FStrEq( pcmd, "+gren2" ) )
	{
		pPlayer->TeamFortress_PrimeGrenade( 2 );
		return TRUE;
	}
	if ( FStrEq( pcmd, "-gren1" ) || FStrEq( pcmd, "-gren2" ) )
	{
		pPlayer->TeamFortress_ThrowPrimedGrenade();
		return TRUE;
	}

	return FALSE;
}
