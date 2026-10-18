// Copyright MapModelers. All Rights Reserved.
//
// A tab's name turned to read down an edge - the SidePanel's tabs (S177a)
// and the UV window's quickpanel (S216a) share it.

#pragma once

#include "CoreMinimal.h"
#include "SurfTheme.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/SLeafWidget.h"

/**
 * A tab's name, turned to read down the edge. Measured as a line of
 * text, then drawn turned a quarter about its own middle, so the space
 * it asks for is the line's height across and its length down.
 */
class SSurfVerticalLabel : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfVerticalLabel) {}
		SLATE_ARGUMENT(FText, Text)
		SLATE_ATTRIBUTE(FSlateColor, Colour)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Text = InArgs._Text.ToString();
		Colour = InArgs._Colour;
		Font = SurfTheme::Bold(8);
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const FVector2D Size = Measured();
		return FVector2D(Size.Y, Size.X);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const FVector2D Line = Measured();
		const FVector2D Room = AllottedGeometry.GetLocalSize();
		const FVector2f Offset(static_cast<float>((Room.X - Line.X) * 0.5), static_cast<float>((Room.Y - Line.Y) * 0.5));
		const FSlateRenderTransform Turn(FQuat2D(FMath::DegreesToRadians(90.0f)));
		const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry(FVector2f(static_cast<float>(Line.X), static_cast<float>(Line.Y)),
			FSlateLayoutTransform(Offset), Turn, FVector2f(0.5f, 0.5f));
		const FLinearColor Tint = Colour.Get(FSlateColor::UseForeground()).GetColor(InWidgetStyle) * InWidgetStyle.GetColorAndOpacityTint();
		FSlateDrawElement::MakeText(OutDrawElements, LayerId, Paint, Text, Font, ESlateDrawEffect::None, Tint);
		return LayerId;
	}

private:
	FVector2D Measured() const
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FVector2D(40.0, 12.0);
		}
		const FVector2D Size = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font);
		return Size;
	}

	FString Text;
	TAttribute<FSlateColor> Colour;
	FSlateFontInfo Font;
};
