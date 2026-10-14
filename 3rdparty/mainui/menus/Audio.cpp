/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "Framework.h"
#include "Slider.h"
#include "Bitmap.h"
#include "PicButton.h"
#include "CheckBox.h"
#include "SpinControl.h"
#include "StringArrayModel.h"

#define ART_BANNER			"gfx/shell/head_audio"

class CMenuAudio : public CMenuFramework
{
public:
	typedef CMenuFramework BaseClass;

	CMenuAudio() : CMenuFramework("CMenuAudio") { }

private:
	void _Init() override;
	void _VidInit() override;
	void GetConfig();
	void SaveAndPopMenu() override;

	CMenuSlider	soundVolume;
	CMenuSlider	musicVolume;
	CMenuSlider	suitVolume;
	CMenuCheckBox noDSP;
	CMenuCheckBox useAlphaDSP;
	CMenuCheckBox muteFocusLost;
};

void CMenuAudio::GetConfig( void )
{
	soundVolume.LinkCvar( "volume" );
	musicVolume.LinkCvar( "MP3Volume" );
	suitVolume.LinkCvar( "suitvolume" );

	noDSP.LinkCvar( "room_off" );
	useAlphaDSP.LinkCvar( "dsp_coeff_table" );
	muteFocusLost.LinkCvar( "snd_mute_losefocus" );
}

void CMenuAudio::SaveAndPopMenu()
{
	soundVolume.WriteCvar();
	musicVolume.WriteCvar();
	suitVolume.WriteCvar();
	noDSP.WriteCvar();
	useAlphaDSP.WriteCvar();
	muteFocusLost.WriteCvar();

	CMenuFramework::SaveAndPopMenu();
}

void CMenuAudio::_Init( void )
{
	banner.SetPicture(ART_BANNER);

	soundVolume.szName = L( "GameUI_SoundEffectVolume" );
	soundVolume.Setup( 0.0, 1.0, 0.05f );
	soundVolume.onChanged = CMenuEditable::WriteCvarCb;
	soundVolume.SetCoord( 320, 280 );
	soundVolume.size.w = 300;

	musicVolume.szName = L( "GameUI_MP3Volume" );
	musicVolume.Setup( 0.0, 1.0, 0.05f );
	musicVolume.onChanged = CMenuEditable::WriteCvarCb;
	musicVolume.SetCoord( 320, 340 );
	musicVolume.size.w = 300;

	suitVolume.szName = L( "GameUI_HEVSuitVolume" );
	suitVolume.Setup( 0.0, 1.0, 0.05f );
	suitVolume.onChanged = CMenuEditable::WriteCvarCb;
	suitVolume.SetCoord( 320, 400 );
	suitVolume.size.w = 300;

	noDSP.szName = L( "Disable DSP effects" );
	noDSP.onChanged = CMenuEditable::WriteCvarCb;
	noDSP.SetCoord( 320, 460 );

	useAlphaDSP.szName = L( "Use Alpha DSP effects" );
	useAlphaDSP.onChanged = CMenuEditable::WriteCvarCb;
	useAlphaDSP.SetCoord( 320, 510 );

	muteFocusLost.szName = L( "Mute when inactive" );
	muteFocusLost.onChanged = CMenuEditable::WriteCvarCb;
	muteFocusLost.SetCoord( 320, 560 );

	AddItem( banner );
	AddButton( L( "Done" ), nullptr, PC_DONE, VoidCb( &CMenuAudio::SaveAndPopMenu ));
	AddItem( soundVolume );
	AddItem( musicVolume );
	AddItem( suitVolume );
	AddItem( noDSP );
	AddItem( useAlphaDSP );
	AddItem( muteFocusLost );
}

void CMenuAudio::_VidInit( )
{
	GetConfig();
}

ADD_MENU( menu_audio, CMenuAudio, UI_Audio_Menu );
