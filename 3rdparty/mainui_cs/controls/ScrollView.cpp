#include "ScrollView.h"
#include "Scissor.h"

CMenuScrollView::CMenuScrollView() : CMenuItemsHolder (),
	m_bHoldingMouse1( false )
{
}

void CMenuScrollView::VidInit()
{
	colorStroke.SetDefault( uiInputFgColor );

	BaseClass::VidInit();

	m_bHoldingMouse1 = false;
	m_iMaxY = 0;
	m_iPosY = 0;
	m_iMaxX = 0;
	m_iPosX = 0;

	FOR_EACH_VEC( m_pItems, i )
	{
		Point pt = m_pItems[i]->pos;
		Size sz = m_pItems[i]->size;

		if( pt.y + sz.h > m_iMaxY )
			m_iMaxY = pt.y + sz.h;
		if( pt.x + sz.w > m_iMaxX )
			m_iMaxX = pt.x + sz.w;
	}
	m_bDisableScrollingY = (m_iMaxY <= size.h);
	m_bDisableScrollingX = (m_iMaxX <= size.w);

	m_iMaxY *= uiStatic.scaleY;
	m_iMaxX *= uiStatic.scaleX;
}

bool CMenuScrollView::KeyDown( int key )
{
	if( UI::Key::IsLeftMouse( key ))
	{
		if( UI_CursorInRect( m_scPos, m_scSize ))
		{
			m_bHoldingMouse1 = true;
			m_HoldingPoint = Point( uiStatic.cursorX, uiStatic.cursorY );
			return true;
		}
	}

	// act when key is pressed or repeated
	if( !m_bDisableScrollingY || !m_bDisableScrollingX )
	{
		int newPosY = m_iPosY;
		int newPosX = m_iPosX;
		if( UI::Key::IsUpArrow( key ) || key == K_MWHEELUP )
			newPosY -= 20;
		else if( UI::Key::IsDownArrow( key ) || key == K_MWHEELDOWN )
			newPosY += 20;
		else if( UI::Key::IsPageUp( key ))
			newPosY -= 100;
		else if( UI::Key::IsPageDown( key ))
			newPosY += 100;
		else if( UI::Key::IsLeftArrow( key ))
			newPosX -= 20;
		else if( UI::Key::IsRightArrow( key ))
			newPosX += 20;

		// TODO: overscrolling
		newPosY = bound( 0, newPosY, Q_max(0, m_iMaxY - m_scSize.h) );
		newPosX = bound( 0, newPosX, Q_max(0, m_iMaxX - m_scSize.w) );

		// recalc
		if( newPosY != m_iPosY || newPosX != m_iPosX )
		{
			m_iPosY = newPosY;
			m_iPosX = newPosX;
			FOR_EACH_VEC( m_pItems, i )
			{
				CMenuBaseItem *pItem = m_pItems[i];

				pItem->VidInit();
			}
			CMenuItemsHolder::MouseMove( uiStatic.cursorX, uiStatic.cursorY );
		}
	}

	return CMenuItemsHolder::KeyDown( key );
}

bool CMenuScrollView::KeyUp( int key )
{
	if( UI::Key::IsLeftMouse( key ))
	{
		m_bHoldingMouse1 = false;
	}

	return CMenuItemsHolder::KeyUp( key );
}

Point CMenuScrollView::GetPositionOffset() const
{
	return Point( -m_iPosX, -m_iPosY ) + BaseClass::GetPositionOffset();
}

bool CMenuScrollView::MouseMove( int x, int y )
{
	return CMenuItemsHolder::MouseMove( x, y );
}

bool CMenuScrollView::IsRectVisible(Point pt, Size sz)
{
	bool x = isrange( m_scPos.x, pt.x, m_scPos.x + m_scSize.w ) ||
			 isrange( pt.x, m_scPos.x, pt.x + sz.w );

	bool y = isrange( m_scPos.y, pt.y, m_scPos.y + m_scSize.h ) ||
			 isrange( pt.y, m_scPos.y, pt.y + sz.h );

	return x && y;
}

void CMenuScrollView::Draw()
{
	if( m_bHoldingMouse1 && !(m_bDisableScrollingY && m_bDisableScrollingX) )
	{
		int newPosY = m_iPosY;
		int newPosX = m_iPosX;

		if( !m_bDisableScrollingY )
			newPosY -= ( uiStatic.cursorY - m_HoldingPoint.y ) / 2;
		if( !m_bDisableScrollingX )
			newPosX -= ( uiStatic.cursorX - m_HoldingPoint.x ) / 2;

		// TODO: overscrolling
		newPosY = bound( 0, newPosY, Q_max(0, m_iMaxY - m_scSize.h) );
		newPosX = bound( 0, newPosX, Q_max(0, m_iMaxX - m_scSize.w) );

		// recalc
		if( newPosY != m_iPosY || newPosX != m_iPosX )
		{
			m_iPosY = newPosY;
			m_iPosX = newPosX;
			FOR_EACH_VEC( m_pItems, i )
			{
				CMenuBaseItem *pItem = m_pItems[i];

				pItem->VidInit();
			}
		}
		m_HoldingPoint = Point( uiStatic.cursorX, uiStatic.cursorY );
	}

	if( bDrawStroke )
	{
		UI_DrawRectangleExt( m_scPos, m_scSize, colorStroke, iStrokeWidth );
	}

	int drawn = 0, skipped = 0;
	FOR_EACH_VEC( m_pItems, i )
	{
		if( !IsRectVisible( m_pItems[i]->GetRenderPosition(), m_pItems[i]->GetRenderSize() ) )
		{
			m_pItems[i]->iFlags |= QMF_HIDDENBYPARENT;
			skipped++;
		}
		else
		{
			m_pItems[i]->iFlags &= ~QMF_HIDDENBYPARENT;
			drawn++;
		}
	}
	// Con_NPrintf( 0, "Drawn: %i Skipped: %i", drawn, skipped );

	UI::Scissor::PushScissor( m_scPos, m_scSize );
		CMenuItemsHolder::Draw();
	UI::Scissor::PopScissor();
}
