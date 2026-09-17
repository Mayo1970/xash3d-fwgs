// TFC-6 Phase 2: the grenade prime/throw pipeline + the 7 grenade-2 types.
// tf15-client shipped no grenade gameplay at all; this is a reimplementation.

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

// Real TFC world models -- names taken from client.cpp's force-unmodified list.
// models/grenade.mdl (what this used before) is not shipped by TFC at all.
#define TF_MDL_NORMAL       "models/w_grenade.mdl"
#define TF_MDL_CONC         "models/conc_grenade.mdl"
#define TF_MDL_NAIL         "models/ngrenade.mdl"
#define TF_MDL_MIRV         "models/mirv_grenade.mdl"
#define TF_MDL_BOMBLET      "models/bomblet.mdl"
#define TF_MDL_NAPALM       "models/napalm.mdl"
#define TF_MDL_GAS          "models/spy_grenade.mdl"
#define TF_MDL_EMP          "models/emp_grenade.mdl"
#define TF_MDL_CALTROP      "models/caltrop.mdl"

#define TF_GREN_ICON        "grenade"              // status_icons.cpp keys the timer beep on this substring
// Values marked [tfc.so] were read out of the retail TFC server binary, which
// Steam ships with full DWARF symbols (tfc/dlls/tfc.so). Prefer them to guesses.
#define TF_GREN_FUSE        ( (float)GR_PRIMETIME + 0.8f )   // [tfc.so] getPrimeTime 3.0 + getPinTime 0.8

// [tfc.so] CTFPrimeGrenade::Throw: from the thrower's origin, no inherited
// velocity, no tumble; the grenade keeps angles 0, which is why it lands upright.
#define TF_THROW_FWD        600.0f
#define TF_THROW_UP         200.0f
#define TF_THROW_JITTER     10.0f
#define TF_GREN_GRAVITY     0.81f
#define TF_GREN_FRICTION    0.6f
#define TF_BOUNCE_DAMP      0.6f     // [tfc.so] CGrenade::BounceTouch, on the ground
#define TF_BOUNCE_QUIET     30.0f    // below this speed it stops clinking

#define TF_DMG_NORMAL       120
#define TF_RADIUS_NORMAL    240.0f                 // ::RadiusDamage falloff = dmg/radius -> matches QWTF at dmg 120
#define TF_DMG_MIRV_MAIN    90
#define TF_DMG_MIRV_LET     70
#define TF_EMP_RADIUS       240.0f   // [tfc.so] CTFEMPGrenade::setRadius

#define TF_CONC_RADIUS      240.0f
#define TF_CONC_STARTVAL    200      // client v_idlescale; -> ~+-60deg yaw sway
#define TF_CONC_DURATION    5.0f
#define TF_CONC_PUSH        900.0f   // peak conc-jump impulse at zero distance

#define TF_GAS_STARTVAL     120
#define TF_GAS_DURATION     14.0f
#define TF_GAS_RADIUS       265.0f

// Nail grenade, all [tfc.so]: fuse -> rise 32 and hover upright (0.4s) ->
// 41 bursts, 0.1s apart, of 4 nails each -> a full 180 grenade blast.
#define TF_NAIL_RISE        32.0f
#define TF_NAIL_HOVER       0.4f     // Explode -> NailGrenadeNailEm
#define TF_NAIL_WINDUP      0.1f     // NailGrenadeNailEm -> first burst
#define TF_NAIL_PULSE       0.1f
#define TF_NAIL_BURSTS      40       // stops once the burst counter passes this
#define TF_NAIL_COUNT       4        // server nails per burst (the client draws 5)
#define TF_NAIL_STEP_MIN    30.0f    // per-burst yaw step, RANDOM_FLOAT(30,40)
#define TF_NAIL_STEP_MAX    40.0f
#define TF_NAIL_DMG         18
#define TF_NAIL_SPEED       1000.0f  // must match EV_TFC_NailgrenadeNail's VectorScale
#define TF_NAIL_SPAWN       12.0f    // and its VectorMA offset
#define TF_NAIL_LIFE        6.0f
#define TF_NAIL_BLAST       180.0f   // inherited CTFPrimeGrenade::setDamage

#define TF_NAPALM_BLAST     20.0f
#define TF_NAPALM_RADIUS    200.0f
#define TF_NAPALM_FIELD     6.0f     // how long the fire field lingers
#define TF_NAPALM_TICK      0.4f
#define TF_NAPALM_BURN      5.0f
#define TF_BURN_TICK        1.0f
#define TF_BURN_TICKDMG     8
#define TF_WATER_DOUSE      2        // pev->waterlevel that puts fire out (waist-deep)

#define TF_CALTROP_SHARDS   5
#define TF_CALTROP_DMG      6
#define TF_CALTROP_LEG      2.0f     // leg_damage per shard; 1 point == 10% slower, decays 0.5/s
#define TF_CALTROP_LIFETIME 12.0f
#define TF_CALTROP_TOSS     200.0f   // the can is lobbed backwards, low, at the feet
#define TF_LEG_MAX          6.0f

static unsigned short g_usTFConc, g_usTFNormal, g_usTFGas, g_usTFEmp;
static unsigned short g_usTFFire, g_usTFBurn, g_usTFMirvMain, g_usTFMirvLet;
static unsigned short g_usTFNailBlast, g_usTFNailSpray;

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

// Throw geometry + prime timing.
// [tfc.so] CTFPrimeGrenade::Throw, draw order kept: right jitter, then up jitter.
static void TF_ThrowVectors( CBasePlayer *pOwner, Vector &vSrc, Vector &vVel )
{
	UTIL_MakeVectors( pOwner->pev->v_angle );

	float flRight = RANDOM_FLOAT( -TF_THROW_JITTER, TF_THROW_JITTER );
	float flUp    = TF_THROW_UP + RANDOM_FLOAT( -TF_THROW_JITTER, TF_THROW_JITTER );

	vSrc = pOwner->pev->origin;
	vVel = gpGlobals->v_forward * TF_THROW_FWD + gpGlobals->v_up * flUp + gpGlobals->v_right * flRight;
}

// Player-effect helpers.
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

// Authoritative: the engine copies ent->v.velocity into pmove before PM_Move
// (sv_pmove.c) and this runs from PreThink. frac is the 0..1 blast strength.
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

// Water and slime put fire out; lava obviously does not.
static bool TF_Douses( int contents )
{
	return contents == CONTENTS_WATER || contents == CONTENTS_SLIME;
}

// Waist-deep counts as in the water, so wading ankle-deep does not put napalm
// out but dunking yourself does -- the standard TFC answer to being on fire.
static bool TF_InWater( CBasePlayer *pl )
{
	return pl->pev->waterlevel >= TF_WATER_DOUSE && TF_Douses( pl->pev->watertype );
}

static void TF_ApplyBurn( CBasePlayer *pl, entvars_t *pevAttacker )
{
	if ( TF_InWater( pl ) )
		return;

	pl->m_flTFBurnEnd = gpGlobals->time + TF_NAPALM_BURN;
	if ( pl->m_flTFBurnNextTick < gpGlobals->time )
		pl->m_flTFBurnNextTick = gpGlobals->time + TF_BURN_TICK;
	pl->tfstate |= TFSTATE_BURNING;
	pl->m_hTFEffectAttacker = pevAttacker ? CBaseEntity::Instance( pevAttacker ) : NULL;
}

// Collect living players in a sphere. Two-pass on purpose: killing a victim
// mid-walk would corrupt UTIL_FindEntityInSphere's resume pointer.
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

// CTFGrenNail -- one nail from a nail grenade. The client draws its own nails
// from tf_nailgren.sc, so EF_NODRAW keeps this off the wire (client.cpp:1309).
class CTFGrenNail : public CBaseEntity
{
public:
	void Spawn( void );
	void Precache( void );
	void EXPORT NailTouch( CBaseEntity *pOther );

	static void Fire( edict_t *pAttacker, const Vector &org, Vector dir );

	EHANDLE m_hAttacker;   // pev->owner cannot carry it, see Fire()
};

LINK_ENTITY_TO_CLASS( tf_grenade_nail, CTFGrenNail )

void CTFGrenNail::Precache( void )
{
	PRECACHE_MODEL( "models/nail.mdl" );
}

void CTFGrenNail::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_FLYMISSILE;   // [tfc.so] CTFNailgunNail::Spawn
	pev->solid    = SOLID_BBOX;

	// a model only so the engine links and clips it like any other missile
	SET_MODEL( ENT( pev ), "models/nail.mdl" );
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	pev->effects |= EF_NODRAW;

	pev->dmg = TF_NAIL_DMG;
	SetTouch( &CTFGrenNail::NailTouch );
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + TF_NAIL_LIFE;
}

// pev->owner MUST stay null -- SV_ClipToLinks skips a mover's owner
// (sv_world.c:1194), and in TFC your own nail grenade does hurt you.
void CTFGrenNail::Fire( edict_t *pAttacker, const Vector &org, Vector dir )
{
	// dir by value: a const& would alias gpGlobals->v_forward
	CTFGrenNail *pNail = (CTFGrenNail *)CBaseEntity::Create( "tf_grenade_nail", org,
	                                                         UTIL_VecToAngles( dir ), NULL );
	if ( !pNail )
	{
		TF_DiagG( "[tfc] nail Create NULL -- tf_grenade_nail missing from exports.txt?\n" );
		return;
	}
	pNail->m_hAttacker   = pAttacker ? CBaseEntity::Instance( pAttacker ) : NULL;
	pNail->pev->velocity = dir * TF_NAIL_SPEED;
}

void CTFGrenNail::NailTouch( CBaseEntity *pOther )
{
	// UTIL_Remove only flags the edict, so go non-solid now or a second touch
	// in the same frame would land a second hit.
	pev->solid = SOLID_NOT;
	SetTouch( NULL );

	// [tfc.so] CTFNailgunNail::NailTouch: straight TakeDamage (no hitgroups),
	// DMG_NAIL, and blood only when the hit actually landed.
	if ( pOther->pev->takedamage != DAMAGE_NO )
	{
		entvars_t *pevAtk = m_hAttacker ? m_hAttacker->pev : pev;
		if ( pOther->TakeDamage( pev, pevAtk, pev->dmg, DMG_NAIL ) )
			SpawnBlood( pev->origin, pOther->BloodColor(), pev->dmg );
	}
	UTIL_Remove( this );
}

// CTFTossGrenade -- base for every thrown grenade. The tumble think is a copy
// of stock CGrenade::TumbleThink; at the fuse it calls the VIRTUAL Detonate2().
class CTFTossGrenade : public CGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	void EXPORT TossThink( void );
	void EXPORT TFBounceTouch( CBaseEntity *pOther );
	void EXPORT NailWindup( void );
	void EXPORT NailBurst( void );
	void EXPORT NailFinish( void );
	void EXPORT NapalmField( void );

	BOOL m_bQuiet;        // stopped rolling: no more bounce clinks
	int  m_iNailBursts;

	virtual const char *GrenModel( void ) { return TF_MDL_NORMAL; }
	virtual unsigned short FXEvent( void ) { return g_usTFNormal; }
	virtual void Detonate2( void );   // default: normal blast

	static CTFTossGrenade *TossCreate( const char *classname, edict_t *pOwner,
	                                   const Vector &vOrigin, const Vector &vVel, float fuse );
};

// Every subclass shares this, so one grenade type reaching the map precaches
// the lot -- a MIRV bomblet or caltrop spawned mid-round is never a late precache.
void CTFTossGrenade::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_NORMAL );
	PRECACHE_MODEL( TF_MDL_CONC );
	PRECACHE_MODEL( TF_MDL_NAIL );
	PRECACHE_MODEL( TF_MDL_MIRV );
	PRECACHE_MODEL( TF_MDL_BOMBLET );
	PRECACHE_MODEL( TF_MDL_NAPALM );
	PRECACHE_MODEL( TF_MDL_GAS );
	PRECACHE_MODEL( TF_MDL_EMP );
	PRECACHE_MODEL( TF_MDL_CALTROP );
	PRECACHE_SOUND( "weapons/timer.wav" );
	PRECACHE_SOUND( "weapons/tink1.wav" );

	// Handles for the client FX events world.cpp already precaches. PRECACHE_EVENT
	// dedupes by name, so re-registering here just fetches the index.
	g_usTFConc      = PRECACHE_EVENT( 1, "events/explode/tf_concuss.sc" );
	g_usTFNormal    = PRECACHE_EVENT( 1, "events/explode/tf_normalgren.sc" );
	g_usTFGas       = PRECACHE_EVENT( 1, "events/explode/tf_gas.sc" );
	g_usTFEmp       = PRECACHE_EVENT( 1, "events/explode/tf_emp.sc" );
	g_usTFFire      = PRECACHE_EVENT( 1, "events/explode/tf_fire.sc" );
	g_usTFBurn      = PRECACHE_EVENT( 1, "events/explode/tf_burn.sc" );
	g_usTFMirvMain  = PRECACHE_EVENT( 1, "events/explode/tf_mirvmain.sc" );
	g_usTFMirvLet   = PRECACHE_EVENT( 1, "events/explode/tf_mirv.sc" );
	g_usTFNailBlast   = PRECACHE_EVENT( 1, "events/explode/tf_ng.sc" );
	g_usTFNailSpray = PRECACHE_EVENT( 1, "events/explode/tf_nailgren.sc" );
}

void CTFTossGrenade::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid    = SOLID_BBOX;
	SET_MODEL( ENT( pev ), GrenModel() );
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

	// Upright and not spinning, as TFC does: it never aims or tumbles the
	// grenade, so every type flies and comes to rest the right way up.
	pGren->pev->owner     = pOwner;
	pGren->pev->velocity  = vVel;
	pGren->pev->angles    = g_vecZero;
	pGren->pev->avelocity = g_vecZero;
	pGren->pev->gravity   = TF_GREN_GRAVITY;
	pGren->pev->friction  = TF_GREN_FRICTION;
	pGren->pev->framerate = 1.0f;
	pGren->m_bQuiet       = FALSE;

	pGren->pev->dmgtime = gpGlobals->time + fuse;
	pGren->SetTouch( &CTFTossGrenade::TFBounceTouch );
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
		// [tfc.so] every hand grenade's Explode checks this first; bomblets do not
		if ( !FClassnameIs( pev, "tf_weapon_mirvbomblet" ) && TeamFortress_InNoGrenadeZone( this ) )
			return;

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

	TF_PlayFX( edict(), FXEvent(), org );
	::RadiusDamage( org, pev, pevAtk, pev->dmg, pev->dmg * 2.0f, CLASS_NONE, DMG_BLAST );

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

// [tfc.so] CGrenade::BounceTouch. Unlike the HL one this deals no club damage,
// posts no danger sound and never touches the model sequence.
void CTFTossGrenade::TFBounceTouch( CBaseEntity *pOther )
{
	if ( pOther->edict() == pev->owner )
		return;

	if ( pev->flags & FL_ONGROUND )
	{
		pev->velocity = pev->velocity * TF_BOUNCE_DAMP;
		if ( pev->velocity.Length() <= TF_BOUNCE_QUIET )
			m_bQuiet = TRUE;
	}
	else if ( !m_bQuiet )
	{
		BounceSound();
	}
}

// [tfc.so] CTFNailGrenade::NailGrenadeNailEm -- a short wind-up while it hovers.
void CTFTossGrenade::NailWindup( void )
{
	m_iNailBursts = 0;
	SetThink( &CTFTossGrenade::NailBurst );
	pev->nextthink = gpGlobals->time + TF_NAIL_WINDUP;
}

// [tfc.so] CTFNailGrenade::NailGrenadeLaunchNail. pev->angles.y IS the ring
// yaw; the event packs yaw*4 in bits 0-10 and (owner index - 1) in 11-15.
void CTFTossGrenade::NailBurst( void )
{
	if ( !IsInWorld() )
	{
		UTIL_Remove( this );
		return;
	}

	float  step = RANDOM_FLOAT( TF_NAIL_STEP_MIN, TF_NAIL_STEP_MAX );
	Vector org  = pev->origin;
	int    idx  = pev->owner ? ENTINDEX( pev->owner ) - 1 : -1;
	int    own  = ( idx >= 0 && idx < gpGlobals->maxClients ) ? ( idx << 11 ) : 0;
	int    pack = ( ( (int)( pev->angles.y * 4.0f ) & 0x7FF ) | own ) & 0xFFFF;

	PLAYBACK_EVENT_FULL( 0, edict(), g_usTFNailSpray, 0.0f, (float *)&org, (float *)&g_vecZero,
	                     step, 0.0f, pack, 0, 0, 0 );

	// the client advances the yaw before drawing each nail; so does the server
	for ( int i = 0; i < TF_NAIL_COUNT; i++ )
	{
		pev->angles.y = UTIL_AngleMod( pev->angles.y + step );
		UTIL_MakeVectors( pev->angles );
		CTFGrenNail::Fire( pev->owner, org + gpGlobals->v_forward * TF_NAIL_SPAWN, gpGlobals->v_forward );
	}

	pev->nextthink = gpGlobals->time + TF_NAIL_PULSE;
	if ( ++m_iNailBursts > TF_NAIL_BURSTS )
		SetThink( &CTFTossGrenade::NailFinish );
}

// [tfc.so] CTFNailGrenade::FinishedExplode: a real grenade blast, which is what
// kills a thrower still standing under it. tf_ng.sc is THIS explosion.
void CTFTossGrenade::NailFinish( void )
{
	Vector     org    = pev->origin;
	entvars_t *pevAtk = TF_GrenOwner( this );
	TF_DiagG( "[tfc] nail grenade finished, blast %d\n", (int)TF_NAIL_BLAST );

	TF_PlayFX( edict(), g_usTFNailBlast, org );
	::RadiusDamage( org, pev, pevAtk, TF_NAIL_BLAST, TF_NAIL_BLAST * 2.5f, CLASS_NONE,
	                DMG_BLAST | DMG_RADIUS_QUAKE );

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

// Napalm leaves a burning patch: anyone standing in it keeps getting re-lit.
void CTFTossGrenade::NapalmField( void )
{
	entvars_t   *pevAtk   = TF_GrenOwner( this );
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;

	// MOVETYPE_NONE never refreshes waterlevel, so ask the point directly --
	// rising water or a flooded room has to put the patch out.
	if ( !IsInWorld() || pev->dmgtime <= gpGlobals->time
	     || TF_Douses( UTIL_PointContents( pev->origin ) ) )
	{
		UTIL_Remove( this );
		return;
	}

	TF_PlayFX( edict(), g_usTFBurn, pev->origin );

	CBaseEntity *v[32];
	int n = TF_GatherPlayers( edict(), pev->origin, TF_NAPALM_RADIUS, pThrower, false, true, v, 32 );
	for ( int i = 0; i < n; i++ )
		TF_ApplyBurn( (CBasePlayer *)v[i], pevAtk );

	pev->nextthink = gpGlobals->time + TF_NAPALM_TICK;
}

// Type subclasses -- classname carries the type, Detonate2 carries the payload.
#define TF_GREN_SUBCLASS( CLS, NAME, MDL, FX ) \
	class CLS : public CTFTossGrenade { public: \
		const char *GrenModel( void ) { return MDL; } \
		unsigned short FXEvent( void ) { return FX; } \
		void Detonate2( void ); }; \
	LINK_ENTITY_TO_CLASS( NAME, CLS )

// Model + FX only; the base normal-blast Detonate2 is right for these.
#define TF_GREN_SKIN( CLS, NAME, MDL, FX ) \
	class CLS : public CTFTossGrenade { public: \
		const char *GrenModel( void ) { return MDL; } \
		unsigned short FXEvent( void ) { return FX; } }; \
	LINK_ENTITY_TO_CLASS( NAME, CLS )

LINK_ENTITY_TO_CLASS( tf_weapon_normalgrenade, CTFTossGrenade )   // base = normal blast

TF_GREN_SKIN( CTFMirvBomblet, tf_weapon_mirvbomblet, TF_MDL_BOMBLET, g_usTFMirvLet );

TF_GREN_SUBCLASS( CTFConcGrenade,    tf_weapon_concussiongrenade, TF_MDL_CONC,    g_usTFConc );
TF_GREN_SUBCLASS( CTFNailGrenade,    tf_weapon_nailgrenade,       TF_MDL_NAIL,    0 );
TF_GREN_SUBCLASS( CTFMirvGrenade,    tf_weapon_mirvgrenade,       TF_MDL_MIRV,    g_usTFMirvMain );
TF_GREN_SUBCLASS( CTFNapalmGrenade,  tf_weapon_napalmgrenade,     TF_MDL_NAPALM,  g_usTFFire );
TF_GREN_SUBCLASS( CTFGasGrenade,     tf_weapon_gasgrenade,        TF_MDL_GAS,     g_usTFGas );
TF_GREN_SUBCLASS( CTFEmpGrenade,     tf_weapon_empgrenade,        TF_MDL_EMP,     g_usTFEmp );
TF_GREN_SUBCLASS( CTFCaltropGrenade, tf_weapon_caltropgrenade,    TF_MDL_CALTROP, 0 );

void CTFConcGrenade::Detonate2( void )
{
	Vector       org      = pev->origin;
	CBaseEntity *pThrower = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	TF_DiagG( "[tfc] detonate cls=%s (concussion)\n", STRING( pev->classname ) );

	TF_PlayFX( edict(), FXEvent(), org );

	CBaseEntity *push[32];
	int nPush = TF_GatherPlayers( edict(), org, TF_CONC_RADIUS, pThrower, false, true, push, 32 );
	for ( int i = 0; i < nPush; i++ )
	{
		CBasePlayer *pl = (CBasePlayer *)push[i];

		float dist = ( pl->pev->origin - org ).Length();
		float frac = ( dist < 48.0f ) ? 1.0f : ( 1.0f - dist / TF_CONC_RADIUS );
		if ( frac < 0.0f ) frac = 0.0f;

		TF_ConcPush( pl, org, TF_CONC_PUSH, frac );

		// Disorient scales with proximity, so a conc-jumper only gets a brief
		// swim but one held to the fuse at your feet is the full punishment.
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

	TF_PlayFX( edict(), FXEvent(), org );

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

	// Drowned napalm is just a dull blast: no burn bit, no burning, no fire field.
	bool bWet = TF_Douses( UTIL_PointContents( org ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	pev->effects |= EF_NODRAW;
	TF_PlayFX( edict(), FXEvent(), org );
	::RadiusDamage( org, pev, pevAtk, TF_NAPALM_BLAST, TF_NAPALM_RADIUS, CLASS_NONE,
	                bWet ? DMG_BLAST : ( DMG_BLAST | DMG_BURN ) );

	if ( bWet )
	{
		TF_DiagG( "[tfc] napalm doused -- detonated in water\n" );
		SetThink( &CBaseEntity::SUB_Remove );
		pev->nextthink = gpGlobals->time + 0.1f;
		return;
	}

	CBaseEntity *v[32];
	int n = TF_GatherPlayers( edict(), org, TF_NAPALM_RADIUS, pThrower, false, true, v, 32 );
	for ( int i = 0; i < n; i++ )
		TF_ApplyBurn( (CBasePlayer *)v[i], pevAtk );

	// stay alive as the burning patch
	pev->movetype  = MOVETYPE_NONE;
	pev->velocity  = g_vecZero;
	pev->dmgtime   = gpGlobals->time + TF_NAPALM_FIELD;
	SetThink( &CTFTossGrenade::NapalmField );
	pev->nextthink = gpGlobals->time;
}

// [tfc.so] CTFEMPGrenade::Explode deals no damage itself. Everything in range reacts
// through TeamFortress_TakeEMPBlast, and the ammo it carries is what explodes.
void CTFEmpGrenade::Detonate2( void )
{
	Vector org = pev->origin;
	TF_DiagG( "[tfc] detonate cls=%s (emp)\n", STRING( pev->classname ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	TF_PlayFX( edict(), FXEvent(), org );

	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = UTIL_FindEntityInSphere( pEnt, org, TF_EMP_RADIUS ) ) != NULL )
		pEnt->TeamFortress_TakeEMPBlast( pev );

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

// [tfc.so] the carried ammo is the blast; a hidden entity carries nothing
void CBaseEntity::TeamFortress_CalcEMPDmgRad( float &dmg, float &rad )
{
	if ( pev->effects & EF_NODRAW )
	{
		dmg = rad = 0;
		return;
	}

	dmg = rad = ammo_shells * 0.75f + ammo_rockets * 1.5f + ammo_cells * 1.5f;
}

// [tfc.so] blast centred on this entity, credited to the grenade's thrower
void CBaseEntity::TeamFortress_EMPExplode( entvars_t *pevGren, float damage, float radius )
{
	entvars_t *pevAttacker = VARS( pevGren->owner ? pevGren->owner : INDEXENT( 0 ) );

	int iScale = (int)( damage * 0.75f );
	if ( iScale > 255 )
		iScale = 255;
	else if ( iScale <= 4 )
		iScale = 5;

	::RadiusDamage( pev->origin, pevGren, pevAttacker, damage, radius, CLASS_NONE, DMG_BLAST | DMG_RADIUS_QUAKE );

	MESSAGE_BEGIN( MSG_PAS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z );
		WRITE_SHORT( g_sModelIndexFireball );
		WRITE_BYTE( iScale );
		WRITE_BYTE( 15 );
		WRITE_BYTE( TE_EXPLFLAG_NOADDITIVE | TE_EXPLFLAG_NODLIGHTS );
	MESSAGE_END();
}

// [tfc.so] an engineer's metal is not volatile: cells are neither counted nor lost
void CBasePlayer::TeamFortress_CalcEMPDmgRad( float &damage, float &radius )
{
	float flDmg = ammo_shells * 0.75f + ammo_rockets * 1.5f;
	if ( pev->playerclass != PC_ENGINEER )
		flDmg += ammo_cells * 1.5f;

	damage = radius = flDmg;
}

// [tfc.so] a quarter (rounded up) of the volatile ammo cooks off. The thrower is always
// hit; other allies only when mp_teamplay lacks TEAMPLAY_NOEXPLOSIVE.
void CBasePlayer::TeamFortress_TakeEMPBlast( entvars_t *pevGren )
{
	CBaseEntity *pAttacker = CBaseEntity::Instance( pevGren->owner ? pevGren->owner : INDEXENT( 0 ) );

	if ( !pev->playerclass )
		return;

	if ( ( (int)gpGlobals->teamplay & TEAMPLAY_NOEXPLOSIVE ) && IsAlly( pAttacker ) && pAttacker != this )
		return;

	float flDmg, flRad;
	TeamFortress_CalcEMPDmgRad( flDmg, flRad );

	ammo_shells  -= (int)ceil( ammo_shells * 0.25 );
	ammo_rockets -= (int)ceil( ammo_rockets * 0.25 );
	if ( pev->playerclass != PC_ENGINEER )
		ammo_cells -= (int)ceil( ammo_cells * 0.25 );

	if ( flDmg > 0 )
		TeamFortress_EMPExplode( pevGren, flDmg, flRad );
}

void CTFMirvGrenade::Detonate2( void )
{
	Vector     org    = pev->origin;
	entvars_t *pevAtk = TF_GrenOwner( this );
	TF_DiagG( "[tfc] detonate cls=%s (mirv)\n", STRING( pev->classname ) );

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	TF_PlayFX( edict(), FXEvent(), org );
	::RadiusDamage( org, pev, pevAtk, TF_DMG_MIRV_MAIN, TF_RADIUS_NORMAL, CLASS_NONE, DMG_BLAST );

	for ( int i = 0; i < GR_TYPE_MIRV_NO; i++ )
	{
		Vector vel( RANDOM_FLOAT( -220, 220 ), RANDOM_FLOAT( -220, 220 ), RANDOM_FLOAT( 250, 400 ) );
		CTFTossGrenade *pLet = CTFTossGrenade::TossCreate( "tf_weapon_mirvbomblet", pev->owner,
		                                                   org + Vector( 0, 0, 8 ), vel,
		                                                   RANDOM_FLOAT( 1.0f, 1.8f ) );
		if ( pLet )
			pLet->pev->dmg = TF_DMG_MIRV_LET;
	}

	pev->effects |= EF_NODRAW;
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

// [tfc.so] CTFNailGrenade::Explode: no blast at the fuse. It pops up 32 units
// (traced, so a ceiling stops it), goes non-solid and hovers upright.
void CTFNailGrenade::Detonate2( void )
{
	TF_DiagG( "[tfc] detonate cls=%s (nail: rise + hover)\n", STRING( pev->classname ) );

	TraceResult tr;
	Vector vTop = pev->origin + Vector( 0, 0, TF_NAIL_RISE );
	UTIL_TraceLine( pev->origin, vTop, ignore_monsters, edict(), &tr );
	UTIL_SetOrigin( pev, tr.vecEndPos );

	SetTouch( NULL );
	pev->flags    &= ~FL_ONGROUND;
	pev->movetype  = MOVETYPE_FLY;
	pev->solid     = SOLID_NOT;      // or the hovering can blocks its own nails
	pev->angles    = g_vecZero;
	pev->velocity  = g_vecZero;
	pev->avelocity = g_vecZero;
	pev->effects  |= EF_NOINTERP;    // a jump, not a slide, on the clients
	SetThink( &CTFTossGrenade::NailWindup );
	pev->nextthink = gpGlobals->time + TF_NAIL_HOVER;
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

// CTFCaltrop -- one scattered caltrop. Slows the first enemy to step on it.

// The mid-left "legs hurt" icon, up while leg_damage is; sprite name from tfc/sprites/hud.txt.
static void TF_SetLegIcon( CBasePlayer *pl, BOOL bOn )
{
	MESSAGE_BEGIN( MSG_ONE, gmsgStatusIcon, NULL, pl->edict() );
		WRITE_BYTE( bOn ? 1 : 0 );
		WRITE_STRING( "dmg_caltrop" );
		if ( bOn )
		{
			WRITE_BYTE( 255 );   // RGB_YELLOWISH, as the client's own dmg_concuss icon
			WRITE_BYTE( 160 );
			WRITE_BYTE( 0 );
		}
	MESSAGE_END();
}

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

// No hand-rolled team or self test: FPlayerCanTakeDamage already refuses a
// teammate yet allows self-damage (teamplay_gamerules.cpp:389), so it gates both.
void CTFCaltrop::ShardTouch( CBaseEntity *pOther )
{
	if ( !pOther->IsPlayer() || !pOther->IsAlive() )
		return;

	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	CBasePlayer *pl     = (CBasePlayer *)pOther;

	// spikes in the foot: body armor should not soak this
	if ( pl->TakeDamage( pev, pOwner ? pOwner->pev : pev, TF_CALTROP_DMG,
	                     DMG_CALTROP | DMG_IGNOREARMOR ) )
	{
		if ( pl->leg_damage <= 0.0f )
			TF_SetLegIcon( pl, TRUE );   // only on the 0 -> hurt edge, not every shard

		pl->leg_damage += TF_CALTROP_LEG;
		if ( pl->leg_damage > TF_LEG_MAX )
			pl->leg_damage = TF_LEG_MAX;
		pl->TeamFortress_SetSpeed();
	}

	EMIT_SOUND( ENT( pev ), CHAN_BODY, "weapons/tink1.wav", 0.6f, ATTN_NORM );
	UTIL_Remove( this );
}

void CTFCaltrop::ShardExpire( void )
{
	UTIL_Remove( this );
}

// Per-class grenade loadout, straight off tf_defs.h's PC_*_GRENADE_* defines.
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

// HUD count. gmsgGrenades ("SecAmmoVal") is a per-index 2-byte message
// (index, value) -- MsgFunc_SecAmmoVal, cl_dll/ammo_secondary.cpp.
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

// A goal took the stock of the primed grenade away: drop the prime, throw nothing.
void TeamFortress_CancelPrimedGrenade( CBasePlayer *pPlayer )
{
	TeamFortress_ClearPrime( pPlayer );
}

// Remove this player's live thrown grenades (class change / respawn).
void CBasePlayer::TeamFortress_RemoveLiveGrenades( void )
{
	static const char *kClasses[] =
	{
		"tf_weapon_normalgrenade", "tf_weapon_concussiongrenade", "tf_weapon_nailgrenade",
		"tf_weapon_mirvgrenade", "tf_weapon_mirvbomblet", "tf_weapon_napalmgrenade",
		"tf_weapon_gasgrenade",
		"tf_weapon_empgrenade", "tf_weapon_caltropgrenade", "tf_weapon_caltrop",
	};   // nails are ownerless, and expire in TF_NAIL_LIFE anyway
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

// Spawn: seed the class grenade counts, reset primed + lingering effect state.
void TeamFortress_SetupGrenades( CBasePlayer *pPlayer )
{
	int pc = pPlayer->pev->playerclass;
	const tf_gren_row_t *g = ( pc > PC_UNDEFINED && pc < PC_LASTCLASS ) ? &sTFGren[pc] : NULL;

	pPlayer->TeamFortress_RemoveLiveGrenades();
	TeamFortress_ClearPrime( pPlayer );

	pPlayer->m_iConcussion = pPlayer->m_iConcStartVal = 0;
	pPlayer->m_flConcStartTime = pPlayer->m_flConcDuration = 0;
	pPlayer->leg_damage = pPlayer->old_leg_damage = 0;
	pPlayer->m_bitsDamageType &= ~( DMG_CALTROP | DMG_IGNOREARMOR );
	TF_SetLegIcon( pPlayer, FALSE );   // client icons outlive death unless told
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

// TFC caps how many of the lingering types you can have out at once. Counted
// live rather than bookkept on a counter, so nothing can leak the slot.
static int TF_ActiveCap( int grtype )
{
	switch ( grtype )
	{
	case GR_TYPE_NAIL:    return MAX_NAIL_GRENS;
	case GR_TYPE_NAPALM:  return MAX_NAPALM_GRENS;
	case GR_TYPE_GAS:     return MAX_GAS_GRENS;
	case GR_TYPE_CALTROP: return MAX_CALTROP_CANS;
	default:              return 0;   // uncapped
	}
}

static int TF_CountLive( CBasePlayer *pPlayer, const char *classname )
{
	int n = 0;
	edict_t *pMe = pPlayer->edict();
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, classname ) ) != NULL )
	{
		if ( pEnt->pev->owner == pMe )
			n++;
	}
	return n;
}

// The caltrop can is thrown flat and backwards from the feet, not aimed: it is
// meant to land on the ground the scout just left.
static void TF_DeployCaltropCan( CBasePlayer *pPlayer )
{
	UTIL_MakeVectors( Vector( 0, pPlayer->pev->v_angle.y, 0 ) );

	Vector vSrc = pPlayer->pev->origin;
	vSrc.z += pPlayer->pev->mins.z + 8.0f;      // hull bottom, so stand and duck both work

	Vector vVel = gpGlobals->v_forward * -TF_CALTROP_TOSS;
	vVel.z = TF_CALTROP_TOSS * 0.5f;

	// deliberately NOT inheriting the player velocity: a fleeing scout wants the
	// can to drop where they were, not to be carried out in front of them.
	CTFTossGrenade::TossCreate( "tf_weapon_caltropgrenade", pPlayer->edict(),
	                            vSrc, vVel, (float)GR_CALTROP_PRIME );
}

// Prime / throw.
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

	int cap = TF_ActiveCap( iType );
	if ( cap > 0 && TF_CountLive( this, TF_GrenClassname( iType ) ) >= cap )
	{
		TF_DiagG( "[tfc] prime slot=%d type=%d refused: %d already live\n", iSlot, iType, cap );
		return;
	}

	// Caltrops never prime -- the tap itself deploys the can, so a press-and-hold
	// cannot blow it in your hand.
	if ( iType == GR_TYPE_CALTROP )
	{
		TF_DeployCaltropCan( this );

		if ( iSlot == 2 )
			no_grenades_2 = iCount - 1;
		else
			no_grenades_1 = iCount - 1;

		TeamFortress_SendGrenadeCounts( this );
		TF_DiagG( "[tfc] caltrop can dropped slot=%d left=%d\n", iSlot, iCount - 1 );
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

	float fuse = TF_GREN_FUSE - ( gpGlobals->time - m_flGrenadePrimeTime );
	if ( fuse < 0.0f )          fuse = 0.0f;
	if ( fuse > TF_GREN_FUSE )  fuse = TF_GREN_FUSE;   // stale prime time must never stop detonation

	Vector vSrc, vVel;
	TF_ThrowVectors( this, vSrc, vVel );

	// Held to the fuse -> it blows in hand: drop it at the current hull bottom
	// (pev->mins tracks stand vs duck), not 16u out in front of the face.
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

// Per-frame: prime auto-detonate + lingering effect upkeep. From PlayerThink.
void TeamFortress_GrenadeThink( CBasePlayer *pPlayer )
{
	float now = gpGlobals->time;

	// held past the fuse -> it goes off in your hand
	if ( pPlayer->tfstate & TFSTATE_GRENPRIMED )
	{
		if ( now >= pPlayer->m_flGrenadePrimeTime + TF_GREN_FUSE )
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

	// napalm burn DoT -- getting in the water puts it out
	if ( TF_InWater( pPlayer ) )
		pPlayer->m_flTFBurnEnd = 0.0f;

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
		if ( pPlayer->leg_damage <= 0.0f )
		{
			pPlayer->leg_damage = 0.0f;
			TF_SetLegIcon( pPlayer, FALSE );   // healed -> the mid-left icon goes
		}
		if ( (int)pPlayer->leg_damage != (int)pPlayer->old_leg_damage )
		{
			pPlayer->old_leg_damage = pPlayer->leg_damage;
			pPlayer->TeamFortress_SetSpeed();
		}
	}
	else
	{
		// Both bits are above DMG_TIMEBASED's 0x3fff, so UpdateClientData's
		// "m_bitsDamageType &= DMG_TIMEBASED" would keep them set for life.
		pPlayer->m_bitsDamageType &= ~( DMG_CALTROP | DMG_IGNOREARMOR );
	}
}

// Command dispatch -- from TeamFortress_ClientCommand (tf_client.cpp). The
// engine forwards the unknown +gren1/-gren1/+gren2/-gren2 console commands.
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
