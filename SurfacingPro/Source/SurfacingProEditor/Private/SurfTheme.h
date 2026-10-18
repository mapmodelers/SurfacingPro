// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/SlateColor.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateRoundedBoxBrush.h"

/**
 * Every colour and font the SurfacingPro panels use (S192a).
 *
 * All of it comes from the editor's own style: its named colours (so a
 * user's editor theme reaches our panels too) and its fonts. Nothing here
 * is a colour of ours except the three axis colours, which are the ones
 * the editor's own vector fields use.
 */
namespace SurfTheme
{
	/** Ordinary text: the editor's foreground. */
	inline FSlateColor Text() { return FStyleColors::Foreground; }
	/**
	 * A button's word and icon at rest (S206a): "just bit more gray. less
	 * bright white texts/icons" - the sidebar, the bar, the top line. Under
	 * the hand they still go to the editor's white.
	 */
	inline FSlateColor Quiet() { return FSlateColor(FLinearColor::FromSRGBColor(FColor(0xA6, 0xA6, 0xA6))); }
	/** Text that is on, hovered or picked. */
	inline FSlateColor Bright() { return FStyleColors::ForegroundHover; }
	/** Secondary text: hints, units, what is off. */
	inline FSlateColor Dim() { return FSlateColor::UseSubduedForeground(); }
	/** A category header's text. */
	inline FSlateColor Header() { return FStyleColors::ForegroundHeader; }
	/** A toggle that is on (the viewport toolbar's checked colour). */
	inline FSlateColor On() { return FStyleColors::AccentBlue; }
	/** The selection / primary blue. */
	inline FSlateColor Primary() { return FStyleColors::Primary; }
	/** Something the user should notice: editing inside, a change pending. */
	inline FSlateColor Warn() { return FStyleColors::Warning; }
	inline FSlateColor Good() { return FStyleColors::Success; }
	inline FSlateColor Bad() { return FStyleColors::Error; }
	/** A favourite. */
	inline FSlateColor Star() { return FStyleColors::AccentYellow; }
	/** The level's quiet blue (S199a): its name, its mark and the in/out arrows on the top line. */
	inline FSlateColor Level() { return FSlateColor(FLinearColor::FromSRGBColor(FColor(0x8F, 0xB6, 0xD8))); }
	inline FSlateColor Clear() { return FStyleColors::Transparent; }

	/** The same colours as plain values, for drawing code. */
	inline FLinearColor Linear(const FSlateColor& Colour) { return Colour.GetSpecifiedColor(); }
	inline FLinearColor DimLinear() { return FStyleColors::Hover2.GetSpecifiedColor(); }
	inline FLinearColor WithAlpha(const FSlateColor& Colour, float Alpha) { return Colour.GetSpecifiedColor().CopyWithNewOpacity(Alpha); }

	/** The shadow under text drawn over the viewport. */
	inline FLinearColor Shadow() { return FLinearColor(0.0f, 0.0f, 0.0f, 0.75f); }

	/** X, Y, Z - as the editor's vector fields colour them. */
	inline FLinearColor AxisX() { return FLinearColor(0.594f, 0.0197f, 0.0f); }
	inline FLinearColor AxisY() { return FLinearColor(0.1349f, 0.3959f, 0.0f); }
	inline FLinearColor AxisZ() { return FLinearColor(0.0251f, 0.207f, 0.85f); }
	inline FLinearColor Axis(int32 Index) { return Index == 0 ? AxisX() : (Index == 1 ? AxisY() : AxisZ()); }

	/** The editor's font at a size; never below the editor's smallest (8). */
	inline FSlateFontInfo Font(float Size) { return FCoreStyle::GetDefaultFontStyle("Regular", FMath::Max(8.0f, Size)); }
	inline FSlateFontInfo Bold(float Size) { return FCoreStyle::GetDefaultFontStyle("Bold", FMath::Max(8.0f, Size)); }
	inline FSlateFontInfo Italic(float Size) { return FCoreStyle::GetDefaultFontStyle("Italic", FMath::Max(8.0f, Size)); }
	inline FSlateFontInfo Font(FName Face, float Size) { return FCoreStyle::GetDefaultFontStyle(Face, FMath::Max(8.0f, Size)); }

	/** The editor's named sizes: Details rows are Small, windows Normal. */
	inline FSlateFontInfo Small() { return FAppStyle::GetFontStyle("SmallFont"); }
	inline FSlateFontInfo SmallBold() { return FAppStyle::GetFontStyle("PropertyWindow.BoldFont"); }
	inline FSlateFontInfo Normal() { return FAppStyle::GetFontStyle("NormalFont"); }

	/**
	 * The top strip's menus (S193c): "once opened should be in the same
	 * theme as the headers" - the editor's menu style with a recessed
	 * background, a dark edge and square hover rows. Falls back to the
	 * editor's own style for everything else. Defined in the module.
	 */
	const ISlateStyle& MenuStyle();

	/**
	 * The windows' buttons (S193e): "check all other windows have this
	 * theme unified" - dark and nearly square, as the top strip's and the
	 * sidebar's, the editor's padding kept.
	 */
	inline const FButtonStyle& ToolButton()
	{
		static const FButtonStyle Style = FButtonStyle(FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
			.SetNormal(FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f, FStyleColors::InputOutline, 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(FStyleColors::Hover, 2.0f, FStyleColors::Hover, 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(FStyleColors::Header, 2.0f, FStyleColors::Header, 1.0f))
			.SetDisabled(FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f, FStyleColors::Recessed, 1.0f))
			.SetNormalForeground(Quiet())
			.SetHoveredForeground(FStyleColors::ForegroundHover)
			.SetPressedForeground(FStyleColors::ForegroundHover)
			.SetDisabledForeground(FStyleColors::Hover2);
		return Style;
	}
}
