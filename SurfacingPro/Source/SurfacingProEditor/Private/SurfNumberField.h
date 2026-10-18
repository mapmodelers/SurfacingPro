// Copyright MapModelers. All Rights Reserved.
//
// A number as he wants one in a panel (S209a, S223a: "not a full slider but
// a FIELD. and on the field, some up/down arrows"): a box to type in, two
// small arrows to step, the wheel over it steps too; Shift makes a step ten
// times, Ctrl a tenth. Every step is applied at once, so the view follows.

#pragma once

#include "CoreMinimal.h"
#include "SurfTheme.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"

/** The arrow of a number field: a click, or a press-and-drag that steps as the hand moves. */
class SSurfNudgeArrow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfNudgeArrow) : _Brush(nullptr) {}
		SLATE_ARGUMENT(const FSlateBrush*, Brush)
		/** Called with the number of steps, signed by the drag, modifiers applied (Shift x10, Ctrl x0.1). */
		SLATE_ARGUMENT(TFunction<void(double)>, OnNudge)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnNudge = InArgs._OnNudge;
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.Padding(FMargin(2.0f, 0.0f))
			.Cursor(EMouseCursor::ResizeUpDown)
			[
				SNew(SBox).WidthOverride(9.0f).HeightOverride(8.0f)
				[
					SNew(SImage).Image(InArgs._Brush).ColorAndOpacity_Lambda([this]() { return bDragging || IsHovered() ? SurfTheme::Text() : SurfTheme::Dim(); })
				]
			]
		];
	}

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		bDragging = false;
		PressY = static_cast<float>(Event.GetScreenSpacePosition().Y);
		Carried = 0.0f;
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}

	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (!HasMouseCapture())
		{
			return FReply::Unhandled();
		}
		const float Scale = FMath::Max(0.1f, Geometry.Scale);
		const float Y = static_cast<float>(Event.GetScreenSpacePosition().Y);
		if (!bDragging && FMath::Abs(Y - PressY) > 3.0f * Scale)
		{
			bDragging = true;
			LastY = Y;
		}
		if (bDragging)
		{
			// Four pixels a step, up is more.
			Carried += (LastY - Y) / Scale / 4.0f;
			LastY = Y;
			const int32 Steps = FMath::TruncToInt(Carried);
			if (Steps != 0 && OnNudge)
			{
				Carried -= Steps;
				OnNudge(Steps * Times(Event));
			}
		}
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
	{
		if (!HasMouseCapture() || Event.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		if (!bDragging && OnNudge)
		{
			OnNudge(Times(Event));
		}
		bDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

private:
	static double Times(const FPointerEvent& Event)
	{
		return Event.IsShiftDown() ? 10.0 : (Event.IsControlDown() ? 0.1 : 1.0);
	}

	TFunction<void(double)> OnNudge;
	bool bDragging = false;
	float PressY = 0.0f;
	float LastY = 0.0f;
	float Carried = 0.0f;
};

class SSurfNumberField : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfNumberField)
		: _Step(0.01), _Digits(3), _Width(64.0f), _Enabled(true)
		{}
		/** The number shown; unset = nothing to show, the field dimmed. */
		SLATE_ATTRIBUTE(TOptional<double>, Value)
		/** What a typed number, an arrow or the wheel sets. */
		SLATE_ARGUMENT(TFunction<void(double)>, OnSet)
		SLATE_ARGUMENT(double, Step)
		SLATE_ARGUMENT(int32, Digits)
		SLATE_ARGUMENT(float, Width)
		SLATE_ATTRIBUTE(bool, Enabled)
		SLATE_ATTRIBUTE(FText, ToolTipText)
		/** A narrow colour mark inside the left edge, as Details marks an axis; none by default. */
		SLATE_ARGUMENT(TOptional<FLinearColor>, Mark)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Value = InArgs._Value;
		OnSet = InArgs._OnSet;
		Step = InArgs._Step;
		Digits = InArgs._Digits;
		SetToolTipText(InArgs._ToolTipText);
		SetEnabled(InArgs._Enabled);

		// An arrow: a click steps once; held and dragged up or down it keeps
		// stepping, live (S226a: "the U and V down and up arrow should be
		// possible to click and drag to increase/decrease. with live view").
		auto Arrow = [this](const TCHAR* Brush, double Sign)
		{
			return SNew(SSurfNudgeArrow)
				.Brush(FAppStyle::GetBrush(Brush))
				.OnNudge([this, Sign](double Times) { Nudge(Sign * Times); });
		};

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		if (InArgs._Mark.IsSet())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Fill).Padding(0.0f, 2.0f, 1.0f, 2.0f)
			[
				SNew(SBox).WidthOverride(2.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(InArgs._Mark.GetValue())
				]
			];
		}
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(InArgs._Width)
			[
				SAssignNew(Box, SEditableTextBox)
				.Font(SurfTheme::Small())
				.SelectAllTextWhenFocused(true)
				.ClearKeyboardFocusOnCommit(true)
				.Text_Lambda([this]() { return Shown(); })
				.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type How)
				{
					if (How != ETextCommit::OnEnter && How != ETextCommit::OnUserMovedFocus)
					{
						return;
					}
					const FString Typed = Text.ToString().TrimStartAndEnd().Replace(TEXT(","), TEXT("."));
					if (Typed.IsNumeric() && OnSet)
					{
						OnSet(FCString::Atod(*Typed));
					}
				})
			]
		];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Arrow(TEXT("Icons.ChevronUp"), 1.0) ]
			+ SVerticalBox::Slot().AutoHeight()[ Arrow(TEXT("Icons.ChevronDown"), -1.0) ]
		];
		ChildSlot [ Row ];
	}

	/** The wheel over the field steps it, as the arrows do. */
	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override
	{
		if (FMath::IsNearlyZero(Event.GetWheelDelta()))
		{
			return FReply::Unhandled();
		}
		Nudge((Event.GetWheelDelta() > 0.0f ? 1.0 : -1.0) * (Event.IsShiftDown() ? 10.0 : (Event.IsControlDown() ? 0.1 : 1.0)));
		return FReply::Handled();
	}

private:
	FText Shown() const
	{
		const TOptional<double> V = Value.Get();
		if (!V.IsSet())
		{
			return FText::GetEmpty();
		}
		return FText::FromString(FString::Printf(TEXT("%.*f"), Digits, V.GetValue()));
	}

	void Nudge(double Times)
	{
		const TOptional<double> V = Value.Get();
		if (V.IsSet() && OnSet)
		{
			OnSet(V.GetValue() + Times * Step);
		}
	}

	TAttribute<TOptional<double>> Value;
	TFunction<void(double)> OnSet;
	double Step = 0.01;
	int32 Digits = 3;
	TSharedPtr<SEditableTextBox> Box;
};
