/*
BuyMenu.cpp - Counter-Strike buy menu UI implementation
Copyright (C) 2018 a1batross
Copyright (C) 2026 $_Vladislav

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "BaseMenu.h"
#include "ClientWindow.h"
#include "utlstring.h"
#include "ScrollView.h"
#include "CheckBox.h"

struct BuyMenuWeaponInfo
{
	const char *name, *image, *command, *name2;
};

static void NormalizeTranslationString( const char *source, CUtlString &result )
{
	result.SetValue( "" );
	for ( const char *t = source; *t; ++t )
	{
		if ( t[0] == '\\' && t[1] == 'n' )
		{
			result.Append( "\n" );
			++t;
			continue;
		}

		char temp[2] = { *t, '\0' };
		result.Append( temp );
	}
}

static const char *TranslateStringIfFound( const char *key )
{
	const char *translated = L( key );
	return translated == key ? NULL : translated;
}

class CClientBaseBuyMenu : public CClientWindow
{
public:
	virtual void Init()
	{
		if( !WasInit() )
		{
			CClientWindow::Init();

			CMenuAction *cancelBtn;
			if( eAmmoClass == BUY_EQUIPMENT )
			{
				cancelBtn = AddButton( '0', L("Cstrike_Cancel"), Point( 100, 580 ), CEventCallback( MenuCb( &CClientBaseBuyMenu::cancelPressedCb ), nullptr ) );
			}
			else
			{
				cancelBtn = AddButton( '0', L("Cstrike_Cancel"), Point( 100, 530 ), CEventCallback( MenuCb( &CClientBaseBuyMenu::cancelPressedCb ), nullptr ) );
			}
			cancelBtn->onGotFocus = MenuCb( &CClientBaseBuyMenu::cancelFocusCb );

			previewBackground.SetSize( 400, 200 );
			previewBackground.SetCoord( 400, 180 );
			previewBackground.SetRenderMode( QM_DRAWNORMAL );
			previewBackground.colorBase = PackRGBA( 0, 0, 0, 160 );
			previewBackground.Hide();
			previewBackground.iFlags |= QMF_INACTIVE;
			AddItem( previewBackground );

		bitmap.SetSize( 400, 200 );
		bitmap.SetCoord( 400, 180 );
		bitmap.bDrawStroke = true;
		bitmap.bKeepAspectRatio = true;
		bitmap.SetRenderMode( QM_DRAWTRANS );
		bitmap.colorStroke  = uiInputTextColor;
		bitmap.iStrokeWidth = 1;
		bitmap.iFlags |= QMF_INACTIVE;
		AddItem( bitmap );

		int startH = 400;
		Point pt( 400, startH );

		priceLabel.SetInactive( true );
			priceLabel.pos = pt;
			priceLabel.m_bLimitBySize = false;
			priceLabel.SetText( L("CStrike_PriceLabel") );
			priceLabel.SetCharSize( QM_TINYFONT );
			AddItem( priceLabel );
			int height = g_FontMgr->GetFontTall( priceLabel.font );
			pt.y += height;

			originLabel.SetInactive( true );
			originLabel.pos = pt;
			originLabel.m_bLimitBySize = false;
			if( eAmmoClass == BUY_EQUIPMENT )
				originLabel.SetText( L("Cstrike_DescriptionLabel") );
			else originLabel.SetText( L("CStrike_OriginLabel") );
			originLabel.SetCharSize( QM_TINYFONT );
			AddItem( originLabel );
			pt.y += height;

			if( eAmmoClass != BUY_EQUIPMENT )
			{
				calibreLabel.SetInactive( true );
				calibreLabel.pos = pt;
				calibreLabel.m_bLimitBySize = false;
				calibreLabel.SetText( L("CStrike_CalibreLabel") );
				calibreLabel.SetCharSize( QM_TINYFONT );
				AddItem( calibreLabel );
				pt.y += height;

				clipLabel.SetInactive( true );
				clipLabel.pos = pt;
				clipLabel.m_bLimitBySize = false;
				clipLabel.SetText( L("CStrike_ClipCapacityLabel") );
				clipLabel.SetCharSize( QM_TINYFONT );
				AddItem( clipLabel );
				pt.y += height;

				rofLabel.SetInactive( true );
				rofLabel.pos = pt;
				rofLabel.m_bLimitBySize = false;
				rofLabel.SetText( L("CStrike_RateOfFireLabel") );
				rofLabel.SetCharSize( QM_TINYFONT );
				AddItem( rofLabel );
				pt.y += height;

				weightLabel.SetInactive( true );
				weightLabel.pos = pt;
				weightLabel.m_bLimitBySize = false;
				weightLabel.SetText( L("CStrike_WeightEmptyLabel") );
				weightLabel.SetCharSize( QM_TINYFONT );
				AddItem( weightLabel );
				pt.y += height;

				projectLabel.SetInactive( true );
				projectLabel.pos = pt;
				projectLabel.m_bLimitBySize = false;
				projectLabel.SetText( L("CStrike_ProjectileWeightLabel") );
				projectLabel.SetCharSize( QM_TINYFONT );
				AddItem( projectLabel );
				pt.y += height;

				muzzlevelLabel.SetInactive( true );
				muzzlevelLabel.pos = pt;
				muzzlevelLabel.m_bLimitBySize = false;
				muzzlevelLabel.SetText( L("CStrike_MuzzleVelocityLabel") );
				muzzlevelLabel.SetCharSize( QM_TINYFONT );
				AddItem( muzzlevelLabel );
				pt.y += height;

				muzzleenLabel.SetInactive( true );
				muzzleenLabel.pos = pt;
				muzzleenLabel.m_bLimitBySize = false;
				muzzleenLabel.SetText( L("CStrike_MuzzleEnergyLabel") );
				muzzleenLabel.SetCharSize( QM_TINYFONT );
				AddItem( muzzleenLabel );
				pt.y += height;
			}

			pt = Point( 600, startH );

			priceLabel_.SetInactive( true );
			priceLabel_.pos = pt;
			priceLabel_.m_bLimitBySize = false;
			priceLabel_.SetCharSize( QM_TINYFONT );
			AddItem( priceLabel_ );
			pt.y += height;

			originLabel_.SetInactive( true );
			originLabel_.pos = pt;
			originLabel_.m_bLimitBySize = eAmmoClass == BUY_EQUIPMENT;
			originLabel_.SetCharSize( QM_TINYFONT );
			originLabel_.SetSize( 300, 999 );
			AddItem( originLabel_ );
			pt.y += height;

			if( eAmmoClass != BUY_EQUIPMENT )
			{
				calibreLabel_.SetInactive( true );
				calibreLabel_.pos = pt;
				calibreLabel_.m_bLimitBySize = false;
				calibreLabel_.SetCharSize( QM_TINYFONT );
				AddItem( calibreLabel_ );
				pt.y += height;

				clipLabel_.SetInactive( true );
				clipLabel_.pos = pt;
				clipLabel_.m_bLimitBySize = false;
				clipLabel_.SetCharSize( QM_TINYFONT );
				AddItem( clipLabel_ );
				pt.y += height;

				rofLabel_.SetInactive( true );
				rofLabel_.pos = pt;
				rofLabel_.m_bLimitBySize = false;
				rofLabel_.SetCharSize( QM_TINYFONT );
				AddItem( rofLabel_ );
				pt.y += height;

				weightLabel_.SetInactive( true );
				weightLabel_.pos = pt;
				weightLabel_.m_bLimitBySize = false;
				weightLabel_.SetCharSize( QM_TINYFONT );
				AddItem( weightLabel_ );
				pt.y += height;

				projectLabel_.SetInactive( true );
				projectLabel_.pos = pt;
				projectLabel_.m_bLimitBySize = false;
				projectLabel_.SetCharSize( QM_TINYFONT );
				AddItem( projectLabel_ );
				pt.y += height;

				muzzlevelLabel_.SetInactive( true );
				muzzlevelLabel_.pos = pt;
				muzzlevelLabel_.m_bLimitBySize = false;
				muzzlevelLabel_.SetCharSize( QM_TINYFONT );
				AddItem( muzzlevelLabel_ );
				pt.y += height;

				muzzleenLabel_.SetInactive( true );
				muzzleenLabel_.pos = pt;
				muzzleenLabel_.m_bLimitBySize = false;
				muzzleenLabel_.SetCharSize( QM_TINYFONT );
				AddItem( muzzleenLabel_ );
				pt.y += height;
			}
		}
	}

	virtual void Reload()
	{
		previewBackground.Hide();
		bitmap.SetPicture( NULL );
		bitmap.colorBase = 0x0;
		priceLabel_.SetText( "" );
		originLabel_.SetText( "" );
		calibreLabel_.SetText( "" );
		clipLabel_.SetText( "" );
		rofLabel_.SetText( "" );
		weightLabel_.SetText( "" );
		projectLabel_.SetText( "" );
		muzzlevelLabel_.SetText( "" );
		muzzleenLabel_.SetText( "" );
		weightLabel.SetText( L( "CStrike_WeightEmptyLabel" ) );

		if( numWeapons > 0 )
		{
			weaponFocusCb( &weapons[0] );
		}
	}

	void cancelPressedCb( void *pExtra )
	{
		UI_CloseClientMenu();
	}

	void cancelFocusCb( void *pExtra )
	{
		previewBackground.Hide();
		bitmap.Hide();
		priceLabel.Hide();
		originLabel.Hide();
		calibreLabel.Hide();
		clipLabel.Hide();
		rofLabel.Hide();
		weightLabel.Hide();
		projectLabel.Hide();
		muzzlevelLabel.Hide();
		muzzleenLabel.Hide();

		priceLabel_.Hide();
		originLabel_.Hide();
		calibreLabel_.Hide();
		clipLabel_.Hide();
		rofLabel_.Hide();
		weightLabel_.Hide();
		projectLabel_.Hide();
		muzzlevelLabel_.Hide();
		muzzleenLabel_.Hide();
	}

	void AddWeapon( BuyMenuWeaponInfo info )
	{
		weapons[numWeapons] = info;

		int key = '1' + numWeapons;
		Point pt;
		pt.x = 100;
		pt.y = 180 + numWeapons * 50;

		CUtlString labelKey;
		labelKey.Format( "Cstrike_%s", info.name );

		CEventCallback pressCb = MenuCb( &CClientBaseBuyMenu::weaponPressedCb );
		pressCb.pExtra = &weapons[numWeapons];

		CMenuAction *btn = AddButton( key, L( labelKey.String() ), pt, pressCb );
		btn->onPressed.pExtra = &weapons[numWeapons];

		btn->onGotFocus = MenuCb( &CClientBaseBuyMenu::weaponFocusCb );
		btn->onGotFocus.pExtra = &weapons[numWeapons];

		numWeapons++;
	}

	CMenuBitmap previewBackground;
	CMenuBitmap bitmap;
	CMenuAction priceLabel, priceLabel_;
	CMenuAction originLabel, originLabel_;
	CMenuAction calibreLabel, calibreLabel_;
	CMenuAction clipLabel, clipLabel_;
	CMenuAction rofLabel, rofLabel_;
	CMenuAction weightLabel, weightLabel_;
	CMenuAction projectLabel, projectLabel_;
	CMenuAction muzzlevelLabel, muzzlevelLabel_;
	CMenuAction muzzleenLabel, muzzleenLabel_;

	BuyMenuWeaponInfo weapons[10];
	int numWeapons = 0;

	enum ammoClass_e
	{
		BUY_NONE,
		BUY_PRIMAMMO,
		BUY_SECAMMO,
		BUY_EQUIPMENT
	} eAmmoClass = BUY_NONE;

	void weaponFocusCb( void *pExtra )
	{
		BuyMenuWeaponInfo *weapon = (BuyMenuWeaponInfo*)pExtra;

		previewBackground.Show();
		bitmap.Show();
		priceLabel.Show();
		originLabel.Show();

		priceLabel_.Show();
		originLabel_.Show();

		if( eAmmoClass != BUY_EQUIPMENT )
		{
			calibreLabel.Show();
			clipLabel.Show();
			rofLabel.Show();
			weightLabel.Show();
			projectLabel.Show();
			muzzlevelLabel.Show();
			muzzleenLabel.Show();

			calibreLabel_.Show();
			clipLabel_.Show();
			rofLabel_.Show();
			weightLabel_.Show();
			projectLabel_.Show();
			muzzlevelLabel_.Show();
			muzzleenLabel_.Show();
		}

		CUtlString labelKey;
		labelKey.Format( "%s_%sPrice", eAmmoClass == BUY_EQUIPMENT ? "Cstrike" : "CStrike", weapon->name2 );
		priceLabel_.SetText( L( labelKey.String() ) );

		if( eAmmoClass == BUY_EQUIPMENT )
		{
			CUtlString descriptionKey;
			descriptionKey.Format( "Cstrike_%sDescription", weapon->name2 );
			CUtlString translatedDescription;
			NormalizeTranslationString( L( descriptionKey.String() ), translatedDescription );
			originLabel_.SetText( translatedDescription.String() );
		}
		else
		{
			CUtlString key;
			key.Format( "CStrike_%sOrigin", weapon->name2 );
			originLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sCalibre", weapon->name2 );
			calibreLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sClipCapacity", weapon->name2 );
			clipLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sRateOfFire", weapon->name2 );
			rofLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sWeightEmpty", weapon->name2 );
			const char *tr = TranslateStringIfFound( key.String() );
			if( tr )
			{
				weightLabel.SetText( L( "CStrike_WeightEmptyLabel" ) );
			}
			else
			{
				weightLabel.SetText( L( "CStrike_WeightLoadedLabel" ) );
				key.Format( "CStrike_%sWeightLoaded", weapon->name2 );
				tr = TranslateStringIfFound( key.String() );
			}

			if( !tr )
			{
				tr = key.String();
			}

			weightLabel_.SetText( L( tr ) );

			key.Format( "CStrike_%sProjectileWeight", weapon->name2 );
			projectLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sMuzzleVelocity", weapon->name2 );
			muzzlevelLabel_.SetText( L( key.String() ) );

			key.Format( "CStrike_%sMuzzleEnergy", weapon->name2 );
			muzzleenLabel_.SetText( L( key.String() ) );
		}

		CUtlString imagePath;
		imagePath.Format( "gfx/vgui/%s.tga", weapon->image );
		bitmap.SetPicture( imagePath.String() );
		bitmap.colorBase = uiColorWhite;
	}

	void weaponPressedCb( void *pExtra )
	{
		BuyMenuWeaponInfo *weapon = (BuyMenuWeaponInfo*)pExtra;
		confirmCb( (void*)weapon->command );
	}

	void confirmCb( void *pExtra )
	{
		const char *command = (const char*)pExtra;
		EngFuncs::ClientCmd( true, command );

		if( EngFuncs::GetCvarFloat( "ui_cs_autofill" ))
		{
			switch( eAmmoClass )
			{
			case BUY_NONE:
				break;
			case BUY_PRIMAMMO: EngFuncs::ClientCmd( false, "primammo" ); break;
			case BUY_SECAMMO:  EngFuncs::ClientCmd( false, "secammo" ); break;
			case BUY_EQUIPMENT: break;
			}
		}
		UI_CloseClientMenu();
	}
};
class CClientMainBuyMenu : public CClientWindow {
public:
	virtual void _Init() override;

	void SetTeam( int team ) { m_iTeam = team; }
	int GetTeam() const { return m_iTeam; }

private:
	CMenuAction categoriesTitle;
	CMenuBitmap columnDivider;
	CMenuCheckBox autoFill;
	int m_iTeam = TEAM_TERRORIST;
};
class CClientPistolsTMenu         : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientPistolsCTMenu        : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientShotgunsMenu         : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientSubMachineGunsTMenu  : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientSubMachineGunsCTMenu : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientRiflesTMenu          : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientRiflesCTMenu         : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientMachineGunsMenu      : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientItemTMenu            : public CClientBaseBuyMenu { virtual void _Init() override; };
class CClientItemCTMenu           : public CClientBaseBuyMenu { virtual void _Init() override; };

template <typename T>
void Menu_Show( void )
{
	EngFuncs::KEY_SetDest( KEY_MENU );
	static T menu;
	menu.Show();
}

template <typename T>
void Menu_Show( int team )
{
	EngFuncs::KEY_SetDest( KEY_MENU );
	static T menu;
	menu.Show();
}

template <>
void Menu_Show<CClientMainBuyMenu>( int team )
{
	EngFuncs::KEY_SetDest( KEY_MENU );
	static CClientMainBuyMenu menu;
	menu.SetTeam( team );
	menu.Show();
}

template <typename T, typename CT>
void Menu_Show_Team( int num ) { num == TEAM_TERRORIST ? Menu_Show<T>() : Menu_Show<CT>(); }

template <typename T, typename CT>
void Menu_Show_Team( CMenuBaseItem *item, void * ) {
	item->Parent()->Hide();
	CClientMainBuyMenu *menu = static_cast<CClientMainBuyMenu *>( item->Parent() );
	if( menu && menu->GetTeam() == TEAM_TERRORIST )
		Menu_Show<T>();
	else
		Menu_Show<CT>();
}

void UI_BuyMenu_Show( int param1, int param2 ) { Menu_Show<CClientMainBuyMenu>( param2 ); }
void UI_BuyMenu_Pistol_Show( int param1, int param2 ) { Menu_Show_Team<CClientPistolsTMenu, CClientPistolsCTMenu>( param2 ); }
void UI_BuyMenu_Shotgun_Show( int param1, int param2 ) { Menu_Show<CClientShotgunsMenu>( param2 ); }
void UI_BuyMenu_Submachine_Show( int param1, int param2 ) { Menu_Show_Team<CClientSubMachineGunsTMenu, CClientSubMachineGunsCTMenu>( param2 ); }
void UI_BuyMenu_Rifle_Show( int param1, int param2 ) { Menu_Show_Team<CClientRiflesTMenu, CClientRiflesCTMenu>( param2 ); }
void UI_BuyMenu_Machinegun_Show( int param1, int param2 ) { Menu_Show_Team<CClientMachineGunsMenu, CClientMachineGunsMenu>( param2 ); }
void UI_BuyMenu_Item_Show( int param1, int param2 ) { Menu_Show_Team<CClientItemTMenu, CClientItemCTMenu>( param2 ); }

void CClientMainBuyMenu::_Init()
{
	szName = L("Cstrike_Buy_Menu");
	const int buttonsTopY = 190;
	const int buttonsStepY = 50;
	const int buttonHeight = 32;
	const int leftButtonsCount = 9; // 1..8 + 0 cancel

	categoriesTitle.SetInactive( true );
	categoriesTitle.SetText( L( "Cstrike_Select_Category" ) );
	categoriesTitle.SetCharSize( QM_SMALLERFONT );
	categoriesTitle.pos = Point( 100, 150 );
	categoriesTitle.size = Size( 250, 24 );
	categoriesTitle.colorBase = uiPromptTextColor;
	categoriesTitle.m_bLimitBySize = false;
	AddItem( categoriesTitle );

	columnDivider.SetCoord( 375, buttonsTopY );
	columnDivider.SetSize( 2, ( leftButtonsCount - 1 ) * buttonsStepY + buttonHeight );
	columnDivider.SetRenderMode( QM_DRAWNORMAL );
	columnDivider.colorBase = PackAlpha( uiInputTextColor, 128 );
	AddItem( columnDivider );

	Point pt = Point( 100, buttonsTopY );
	AddButton( '1', L( "Cstrike_Pistols" ), pt, Menu_Show_Team<CClientPistolsTMenu, CClientPistolsCTMenu> );
	pt.y += 50;
	AddButton( '2', L( "Cstrike_Shotguns" ), pt, Menu_Show_Team<CClientShotgunsMenu, CClientShotgunsMenu> );
	pt.y += 50;
	AddButton( '3', L( "Cstrike_SubMachineGuns" ), pt, Menu_Show_Team<CClientSubMachineGunsTMenu, CClientSubMachineGunsCTMenu> );
	pt.y += 50;
	AddButton( '4', L( "Cstrike_Rifles" ), pt, Menu_Show_Team<CClientRiflesTMenu, CClientRiflesCTMenu> );
	pt.y += 50;
	AddButton( '5', L( "Cstrike_MachineGuns" ), pt, Menu_Show_Team<CClientMachineGunsMenu, CClientMachineGunsMenu> );
	pt.y += 50;
	AddButton( '6', L( "Cstrike_Prim_Ammo" ), pt, ExecAndHide( "primammo" ) );
	pt.y += 50;
	AddButton( '7', L( "Cstrike_Sec_Ammo" ), pt, ExecAndHide( "secammo" ));
	pt.y += 50;
	AddButton( '8', L( "Cstrike_Equipment" ), pt, Menu_Show_Team<CClientItemTMenu, CClientItemCTMenu> );
	pt.y += 50;
	AddButton( '0', L( "Cstrike_Cancel" ), pt, CEventCallback( []( CMenuBaseItem *, void * ) { UI_CloseClientMenu(); } ) );

	pt.x = 400;
	pt.y = buttonsTopY;
	AddButton( 'A', L( "Cstrike_BuyMenuAutobuy" ), pt, ExecAndHide( "autobuy" ) );
	pt.y += 50;
	AddButton( 'R', L( "Cstrike_BuyMenuRebuy" ), pt, ExecAndHide( "rebuy" ) );
	pt.y += 50;

	autoFill.szName = L( "Auto buy ammo" );
	autoFill.pos = pt;
	autoFill.SetCharSize( QM_SMALLERFONT );
	autoFill.colorBase = uiPromptTextColor;
	autoFill.LinkCvar( "ui_cs_autofill" );
	autoFill.onChanged = CMenuEditable::WriteCvarCb;

	AddItem( autoFill );
}
void CClientPistolsTMenu::_Init()
{
	szName = L( "Cstrike_PistolsLabel" );
	eAmmoClass = BUY_SECAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "Glock18", "glock18", "glock", "Glock" });
	AddWeapon( BuyMenuWeaponInfo{ "USP45", "usp45", "usp", "USP45" });
	AddWeapon( BuyMenuWeaponInfo{ "P228", "p228", "p228", "P228" });
	AddWeapon( BuyMenuWeaponInfo{ "DesertEagle", "deserteagle", "deagle", "DesertEagle" });
	AddWeapon( BuyMenuWeaponInfo{ "Elites", "elites", "elites", "Elites" });

}
void CClientPistolsCTMenu::_Init()
{
	szName = L( "Cstrike_PistolsLabel" );
	eAmmoClass = BUY_SECAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "Glock18", "glock18", "glock", "Glock" });
	AddWeapon( BuyMenuWeaponInfo{ "USP45", "usp45", "usp", "USP45" });
	AddWeapon( BuyMenuWeaponInfo{ "P228", "p228", "p228", "P228" });
	AddWeapon( BuyMenuWeaponInfo{ "DesertEagle", "deserteagle", "deagle", "DesertEagle" });
	AddWeapon( BuyMenuWeaponInfo{ "FiveSeven", "fiveseven", "fiveseven", "FiveSeven" });
}
void CClientShotgunsMenu::_Init()
{
	szName = L( "Cstrike_ShotgunsLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "m3", "m3", "m3", "M3" });
	AddWeapon( BuyMenuWeaponInfo{ "xm1014", "xm1014", "xm1014",	"XM1014" });
}

void CClientSubMachineGunsTMenu::_Init()
{
	szName = L( "Cstrike_SubmachinegunsLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "MAC10", "mac10", "mac10", "Mac10" });
	AddWeapon( BuyMenuWeaponInfo{ "MP5", "mp5", "mp5", "MP5" });
	AddWeapon( BuyMenuWeaponInfo{ "UMP45", "ump45", "ump45", "UMP45" });
	AddWeapon( BuyMenuWeaponInfo{ "P90", "p90", "p90", "P90" });
}
void CClientSubMachineGunsCTMenu::_Init()
{
	szName = L( "Cstrike_SubmachinegunsLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "Tmp", "tmp", "tmp", "TMP" });
	AddWeapon( BuyMenuWeaponInfo{ "MP5", "mp5", "mp5", "MP5" });
	AddWeapon( BuyMenuWeaponInfo{ "UMP45", "ump45", "ump45", "UMP45" });
	AddWeapon( BuyMenuWeaponInfo{ "P90", "p90", "p90", "P90" });
}
void CClientRiflesTMenu::_Init()
{
	szName = L( "Cstrike_RiflesLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "Galil", "galil", "galil", "Galil" });
	AddWeapon( BuyMenuWeaponInfo{ "AK47", "ak47", "ak47", "AK47" });
	AddWeapon( BuyMenuWeaponInfo{ "Scout_TER", "scout", "scout", "Scout" });
	AddWeapon( BuyMenuWeaponInfo{ "SG552", "sg552", "sg552", "SG552" });
	AddWeapon( BuyMenuWeaponInfo{ "AWP_TER", "awp", "awp", "AWP" });
	AddWeapon( BuyMenuWeaponInfo{ "G3SG1", "g3sg1", "g3sg1", "G3SG1" });

}
void CClientRiflesCTMenu::_Init()
{
	szName = L( "Cstrike_RiflesLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "Famas", "famas", "famas", "Famas" });
	AddWeapon( BuyMenuWeaponInfo{ "Scout_CT", "scout", "scout", "Scout" });
	AddWeapon( BuyMenuWeaponInfo{ "M4A1", "m4a1", "m4a1", "M4A1" });
	AddWeapon( BuyMenuWeaponInfo{ "Aug", "aug", "aug", "Aug" });
	AddWeapon( BuyMenuWeaponInfo{ "SG550", "sg550", "sg550", "SG550" });
	AddWeapon( BuyMenuWeaponInfo{ "AWP_CT", "awp", "awp", "AWP" });

}
void CClientMachineGunsMenu::_Init()
{
	szName = L( "Cstrike_MachinegunsLabel" );
	eAmmoClass = BUY_PRIMAMMO;

	AddWeapon( BuyMenuWeaponInfo{ "M249", "m249", "m249", "M249" });
}
void CClientItemTMenu::_Init()
{
	szName = L( "Cstrike_EquipmentLabel" );
	eAmmoClass = BUY_EQUIPMENT;

	AddWeapon( BuyMenuWeaponInfo{ "Kevlar", "kevlar", "vest", "Kevlar" });
	AddWeapon( BuyMenuWeaponInfo{ "Kevlar_Helmet", "kevlar_helmet", "vesthelm", "KevlarHelmet" });
	AddWeapon( BuyMenuWeaponInfo{ "Flashbang", "flashbang", "flash", "Flashbang" });
	AddWeapon( BuyMenuWeaponInfo{ "HE_Grenade", "hegrenade", "hegren", "HEGrenade" });
	AddWeapon( BuyMenuWeaponInfo{ "Smoke_Grenade", "smokegrenade", "sgren", "SmokeGrenade" });
	AddWeapon( BuyMenuWeaponInfo{ "NightVision_Button_TER", "nightvision", "nvgs", "Nightvision" });
}
void CClientItemCTMenu::_Init()
{
	szName = L( "Cstrike_EquipmentLabel" );
	eAmmoClass = BUY_EQUIPMENT;

	AddWeapon( BuyMenuWeaponInfo{ "Kevlar", "kevlar", "vest", "Kevlar" });
	AddWeapon( BuyMenuWeaponInfo{ "Kevlar_Helmet", "kevlar_helmet", "vesthelm", "KevlarHelmet" });
	AddWeapon( BuyMenuWeaponInfo{ "Flashbang", "flashbang", "flash", "Flashbang" });
	AddWeapon( BuyMenuWeaponInfo{ "HE_Grenade", "hegrenade", "hegren", "HEGrenade" });
	AddWeapon( BuyMenuWeaponInfo{ "Smoke_Grenade", "smokegrenade", "sgren", "SmokeGrenade" });
	AddWeapon( BuyMenuWeaponInfo{ "Defuser", "defuser", "defuser", "Defuser" });
	AddWeapon( BuyMenuWeaponInfo{ "NightVision_Button_CT", "nightvision", "nvgs", "Nightvision" });
	AddWeapon( BuyMenuWeaponInfo{ "Shield", "shield", "shield", "Shield" });
}
