// Copyright MapModelers. All Rights Reserved.

#include "SurfToolPalette.h"
#include "Engine/Selection.h"
#include "SurfSettingsWindow.h"
#include "SurfTheme.h"
#include "SurfIcons.h"
#include "Algo/Sort.h"

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfMenu.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "SurfPatchSurfacing.h"
#include "SurfSettings.h"
#include "SurfKeymap.h"
#include "SurfProjection.h"
#include "SurfOutliner.h"
#include "SurfUVView.h"
#include "SurfPaintView.h"
#include "SurfFlagsView.h"
#include "SurfSidePanel.h"
#include "EditorModeManager.h"
#include "SurfParamsView.h"
#include "WorkspaceMenuStructureModule.h"
#include "WorkspaceMenuStructure.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "SurfTexturesView.h"
#include "SurfNotesView.h"
#include "Widgets/Images/SImage.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "SurfMaterialsView.h"
#include "SurfMaterialManager.h"
#include "SurfBuild.h"
#include "SurfLevelBake.h"
#include "SurfAutoBake.h"
#include "Editor.h"

#include "Styling/AppStyle.h"
#include "Serialization/ObjectWriter.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SLeafWidget.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/Input/SSpinBox.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "SurfSession.h"
#include "LevelEditorViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "Framework/Application/IMenu.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Input/DragAndDrop.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleDefaults.h"

#define LOCTEXT_NAMESPACE "SurfacingProPalette"

/** How long a verb's result stays on the bar's status line. */
static constexpr double ResultLineSeconds = 4.0;

namespace
{
	/** Colour says family, and nothing else says family. */
	/**
	 * Unreal's own accent set (S91a). These are the colours the editor already
	 * marks folders and channels with, so a family reads as part of the editor
	 * instead of as a palette somebody invented for this one panel.
	 */
	FSlateColor GroupColour(ESurfVerbGroup Group)
	{
		switch (Group)
		{
		case ESurfVerbGroup::Create:   return FStyleColors::AccentOrange;
		case ESurfVerbGroup::Geometry: return FStyleColors::AccentGreen;
		case ESurfVerbGroup::Shape:    return FStyleColors::AccentBlue;
		case ESurfVerbGroup::Surface:  return FStyleColors::AccentPurple;
		case ESurfVerbGroup::UV:       return FStyleColors::AccentYellow;
		case ESurfVerbGroup::View:     return FStyleColors::AccentWhite;
		default:                       return FStyleColors::AccentGray;
		}
	}


	/**
	 * The sub-mode icons, drawn rather than typed.
	 *
	 * The first attempt used Unicode geometric shapes in a text block, and the
	 * editor font has none of them - four empty boxes is what that renders as.
	 * A leaf widget that paints its own lines has no font to depend on, and it
	 * also gives each icon a fixed, legible shape at 22 pixels, which a glyph
	 * at that size would not have had either.
	 */
	enum class ESurfIcon : uint8 { Object, Face, Edge, Vertex, In, Out, Soft, Snap, Mesh, Name, Mark, Left, Right, Close };

	/**
	 * The view gizmo (S110a): "maybe have that gizmo a clickable one (it edits
	 * the view) but in top right area."
	 *
	 * The editor's own axis indicator is drawn bottom-left, where the readout
	 * is, and it cannot be moved or clicked. This one is ours: the three axes
	 * as the camera sees them, in the top right, and a click on a tip turns the
	 * view to look down that axis about whatever is being orbited - the hand
	 * goes to it for the same reason it goes to the corner of a car windscreen
	 * to find the mirror.
	 *
	 * Drawn furthest-first so the axes overlap the way they actually lie.
	 */
	class SSurfViewGizmo : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfViewGizmo) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&) {}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(58.0f, 58.0f);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
			const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
			int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();
			const FVector2D Size = AllottedGeometry.GetLocalSize();
			const FVector2D Centre = Size * 0.5;
			const double Reach = FMath::Min(Size.X, Size.Y) * 0.5 - 10.0;

			const ESlateDrawEffect Effects = ESlateDrawEffect::None;
			const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");

			FTip Tips[6];
			const int32 Count = Arrange(Centre, Reach, Tips);
			if (Count == 0)
			{
				return LayerId;
			}

			for (int32 I = 0; I < Count; ++I)
			{
				const FTip& Tip = Tips[I];

				if (Tip.bPositive)
				{
					TArray<FVector2D> Points;
					Points.Add(Centre);
					Points.Add(Tip.At);
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Paint, Points, Effects,
						Tip.Colour.CopyWithNewOpacity(0.9f), true, 1.5f);
				}

				// "its good but more subtle-smaller squares" (S125a). Small
				// enough to read as a mark rather than a control, and still
				// wider than the thirteen-pixel reach a click needs.
				const float Radius = Tip.bPositive ? 6.5f : 4.0f;
				FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
					AllottedGeometry.ToPaintGeometry(
						FVector2D(Radius * 2.0f, Radius * 2.0f),
						FSlateLayoutTransform(Tip.At - FVector2D(Radius, Radius))),
					White, Effects,
					Tip.bPositive ? Tip.Colour : Tip.Colour.CopyWithNewOpacity(0.35f));

				if (Tip.bPositive)
				{
					const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 7);
					FSlateDrawElement::MakeText(OutDrawElements, LayerId + 2,
						AllottedGeometry.ToPaintGeometry(
							FVector2D(10.0f, 10.0f),
							FSlateLayoutTransform(Tip.At - FVector2D(2.5f, 5.0f))),
						Tip.Label, Font, Effects, FLinearColor::Black);
				}
			}

			return LayerId + 3;
		}

		virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
			{
				return FReply::Unhandled();
			}

			const FVector2D Size = Geometry.GetLocalSize();
			const FVector2D Centre = Size * 0.5;
			const double Reach = FMath::Min(Size.X, Size.Y) * 0.5 - 10.0;
			const FVector2D Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());

			FTip Tips[6];
			const int32 Count = Arrange(Centre, Reach, Tips);

			int32 Best = INDEX_NONE;
			double BestSquared = 13.0 * 13.0;
			for (int32 I = 0; I < Count; ++I)
			{
				const double D = FVector2D::DistSquared(Local, Tips[I].At);
				if (D <= BestSquared)
				{
					BestSquared = D;
					Best = I;
				}
			}
			if (Best == INDEX_NONE)
			{
				return FReply::Unhandled();
			}

			FEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (!Client || !Client->IsPerspective())
			{
				return FReply::Handled();
			}

			// Round to where it is orbiting, at the distance it is already at:
			// a view snap should change the angle and nothing else.
			const FVector LookAt = Client->GetLookAtLocation();
			const double Distance = FMath::Max(50.0, FVector::Dist(Client->GetViewLocation(), LookAt));
			const FVector Towards = -Tips[Best].Axis;

			Client->SetViewLocation(LookAt - Towards * Distance);
			Client->SetViewRotation(Towards.Rotation());
			Client->Invalidate();

			SurfSession::Gesture(TEXT("view_gizmo"), Tips[Best].Label);
			return FReply::Handled();
		}

		virtual FCursorReply OnCursorQuery(const FGeometry&, const FPointerEvent&) const override
		{
			return FCursorReply::Cursor(EMouseCursor::Hand);
		}

	private:
		struct FTip
		{
			FVector Axis = FVector::ZeroVector;
			FVector2D At = FVector2D::ZeroVector;
			FLinearColor Colour = FLinearColor::White;
			FString Label;
			bool bPositive = false;
			double Depth = 0.0;
		};

		/** The six axis tips where the camera puts them, furthest first. */
		static int32 Arrange(const FVector2D& Centre, double Reach, FTip (&Out)[6])
		{
			FEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (!Client)
			{
				return 0;
			}

			const FRotator View = Client->GetViewRotation();
			const FVector Forward = View.Vector();
			const FVector Right = FRotationMatrix(View).GetScaledAxis(EAxis::Y);
			const FVector Up = FRotationMatrix(View).GetScaledAxis(EAxis::Z);

			static const FVector Axes[3] = { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };
			static const FLinearColor Colours[3] = {
				FLinearColor(0.85f, 0.20f, 0.22f),
				FLinearColor(0.35f, 0.72f, 0.20f),
				FLinearColor(0.20f, 0.45f, 0.90f) };
			static const TCHAR* Labels[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };

			int32 N = 0;
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 Sign = 0; Sign < 2; ++Sign)
				{
					const FVector Axis = Sign == 0 ? Axes[I] : -Axes[I];
					FTip& Tip = Out[N++];
					Tip.Axis = Axis;
					Tip.Colour = Colours[I];
					Tip.Label = Labels[I];
					Tip.bPositive = Sign == 0;
					Tip.Depth = FVector::DotProduct(Axis, Forward);
					// Screen Y runs down, so up on the screen is minus Up.
					Tip.At = Centre + FVector2D(
						FVector::DotProduct(Axis, Right) * Reach,
						-FVector::DotProduct(Axis, Up) * Reach);
				}
			}

			// Furthest away is drawn first, so a near tip covers a far one.
			Algo::Sort(Out, [](const FTip& L, const FTip& R) { return L.Depth > R.Depth; });
			return N;
		}
	};

	class SSurfModeIcon : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfModeIcon) {}
			SLATE_ARGUMENT(ESurfIcon, Icon)
			SLATE_ATTRIBUTE(FSlateColor, ColorAndOpacity)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Icon = InArgs._Icon;
			Tint = InArgs._ColorAndOpacity;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(16.0f, 16.0f);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
			const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
			int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();
			const FVector2D Size = AllottedGeometry.GetLocalSize();

			// One square, inset, is the frame every icon is built on, so the
			// four read as a set rather than four unrelated marks.
			const bool bArrow = Icon == ESurfIcon::In || Icon == ESurfIcon::Out
				|| Icon == ESurfIcon::Left || Icon == ESurfIcon::Right
				|| Icon == ESurfIcon::Close;
			const float Inset = bArrow ? 1.0f : 3.0f;
			const float X0 = Inset;
			const float Y0 = Inset;
			const float X1 = FMath::Max(Size.X - Inset, X0 + 1.0f);
			const float Y1 = FMath::Max(Size.Y - Inset, Y0 + 1.0f);

			// With no tint of its own an icon takes the colour of whatever it
			// is sitting in - so an icon inside a toggle turns white when the
			// toggle does, without either of them knowing about the other.
			const FLinearColor Colour = Tint.IsSet()
				? Tint.Get().GetSpecifiedColor()
				: InWidgetStyle.GetColorAndOpacityTint();

			const FLinearColor Faint = Colour.CopyWithNewOpacity(0.45f);
			const ESlateDrawEffect Effects = ESlateDrawEffect::None;
			const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");

			auto Line = [&](double AX, double AY, double BX, double BY, const FLinearColor& C, float Thick)
			{
				TArray<FVector2D> Points;
				Points.Add(FVector2D(AX, AY));
				Points.Add(FVector2D(BX, BY));
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Paint, Points, Effects, C, true, Thick);
			};

			auto Frame = [&](const FLinearColor& C, float Thick)
			{
				Line(X0, Y0, X1, Y0, C, Thick);
				Line(X1, Y0, X1, Y1, C, Thick);
				Line(X1, Y1, X0, Y1, C, Thick);
				Line(X0, Y1, X0, Y0, C, Thick);
			};

			// A solid head, drawn as lines across it: Slate has no filled
			// triangle, and an arrow whose head is an outline is a chevron,
			// which is not what "actual arrow up down icons" means (S92a).
			auto Head = [&](double CX, double TipX, double TipY, double BaseX, double BaseY, bool bVertical, const FLinearColor& C)
			{
				const int32 Steps = 8;
				for (int32 I = 0; I <= Steps; ++I)
				{
					const double T = (double)I / (double)Steps;
					if (bVertical)
					{
						const double Y = FMath::Lerp(TipY, BaseY, T);
						const double W = BaseX * T;
						Line(CX - W, Y, CX + W, Y, C, 1.5f);
					}
					else
					{
						const double X = FMath::Lerp(TipX, BaseX, T);
						const double W = BaseY * T;
						Line(X, CX - W, X, CX + W, C, 1.5f);
					}
				}
			};

			auto Dot = [&](double CX, double CY, const FLinearColor& C, float Radius)
			{
				FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
					AllottedGeometry.ToPaintGeometry(
						FVector2D(Radius * 2.0f, Radius * 2.0f),
						FSlateLayoutTransform(FVector2D(CX - Radius, CY - Radius))),
					White, Effects, C);
			};

			switch (Icon)
			{
			case ESurfIcon::Soft:
			{
				// A point and the reach around it.
				const double CX = (X0 + X1) * 0.5;
				const double CY = (Y0 + Y1) * 0.5;
				const double R = FMath::Min(X1 - X0, Y1 - Y0) * 0.5;
				TArray<FVector2D> Ring;
				for (int32 I = 0; I <= 12; ++I)
				{
					const double A = (double)I / 12.0 * 2.0 * PI;
					Ring.Add(FVector2D(CX + FMath::Cos(A) * R, CY + FMath::Sin(A) * R));
				}
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Paint, Ring, Effects, Faint, true, 1.0f);
				Dot(CX, CY, Colour, 1.8f);
				break;
			}

			case ESurfIcon::Snap:
			{
				// A crossing, and the point it lands on.
				const double CX = (X0 + X1) * 0.5;
				const double CY = (Y0 + Y1) * 0.5;
				Line(X0, CY, X1, CY, Faint, 1.0f);
				Line(CX, Y0, CX, Y1, Faint, 1.0f);
				Dot(CX, CY, Colour, 2.0f);
				break;
			}

			case ESurfIcon::Mesh:
				// Somebody else's geometry: a box with a corner drawn on.
				Frame(Faint, 1.0f);
				Line(X0, Y0, (X0 + X1) * 0.5, (Y0 + Y1) * 0.5, Colour, 1.2f);
				Line((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, X1, Y1, Colour, 1.2f);
				break;

			case ESurfIcon::Name:
				// A label: two lines of writing on a plate.
				Frame(Faint, 1.0f);
				Line(X0 + 2.0, (Y0 + Y1) * 0.5 - 1.5, X1 - 2.0, (Y0 + Y1) * 0.5 - 1.5, Colour, 1.2f);
				Line(X0 + 2.0, (Y0 + Y1) * 0.5 + 1.5, X1 - 3.5, (Y0 + Y1) * 0.5 + 1.5, Colour, 1.2f);
				break;

			case ESurfIcon::Mark:
			{
				// Four corners and an origin: what a placed prefab draws.
				const double L = FMath::Min(X1 - X0, Y1 - Y0) * 0.35;
				Line(X0, Y0, X0 + L, Y0, Colour, 1.4f);
				Line(X0, Y0, X0, Y0 + L, Colour, 1.4f);
				Line(X1, Y0, X1 - L, Y0, Colour, 1.4f);
				Line(X1, Y0, X1, Y0 + L, Colour, 1.4f);
				Line(X0, Y1, X0 + L, Y1, Colour, 1.4f);
				Line(X0, Y1, X0, Y1 - L, Colour, 1.4f);
				Line(X1, Y1, X1 - L, Y1, Colour, 1.4f);
				Line(X1, Y1, X1, Y1 - L, Colour, 1.4f);
				Dot((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, Faint, 1.4f);
				break;
			}

			case ESurfIcon::Out:
			{
				// Up: a slim stem under a small head (S174a) - "adjust arrows
				// icon, it's not very aesthetic, perhaps too big".
				const double CX = (X0 + X1) * 0.5;
				const double Top = Y0 + 3.0;
				const double Bottom = Y1 - 3.0;
				Head(CX, 0.0, Top, 2.6, Top + 3.5, true, Colour);
				Line(CX, Top + 3.5, CX, Bottom, Colour, 1.3f);
				break;
			}

			case ESurfIcon::In:
			{
				const double CX = (X0 + X1) * 0.5;
				const double Top = Y0 + 3.0;
				const double Bottom = Y1 - 3.0;
				Head(CX, 0.0, Bottom, 2.6, Bottom - 3.5, true, Colour);
				Line(CX, Top, CX, Bottom - 3.5, Colour, 1.3f);
				break;
			}

			case ESurfIcon::Close:
				Line(X0 + 1.0, Y0 + 1.0, X1 - 1.0, Y1 - 1.0, Colour, 1.5f);
				Line(X1 - 1.0, Y0 + 1.0, X0 + 1.0, Y1 - 1.0, Colour, 1.5f);
				break;

			case ESurfIcon::Left:
			{
				const double CY = (Y0 + Y1) * 0.5;
				Head(CY, X0, 0.0, X0 + 5.0, 3.4, false, Colour);
				Line(X0 + 5.0, CY, X1, CY, Colour, 1.8f);
				break;
			}

			case ESurfIcon::Right:
			{
				const double CY = (Y0 + Y1) * 0.5;
				Head(CY, X1, 0.0, X1 - 5.0, 3.4, false, Colour);
				Line(X0, CY, X1 - 5.0, CY, Colour, 1.8f);
				break;
			}

			case ESurfIcon::Object:
				// The whole thing: a plain frame, no corners marked - corners
				// are what the vertex icon shows, and this is the one that is
				// not about them.
				Frame(Colour, 1.6f);
				break;

			case ESurfIcon::Face:
				// A filled quad: the interior is what is being picked.
				FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
					AllottedGeometry.ToPaintGeometry(
						FVector2D(X1 - X0, Y1 - Y0),
						FSlateLayoutTransform(FVector2D(X0, Y0))),
					White, Effects, Colour);
				break;

			case ESurfIcon::Edge:
				// A faint frame with one side bright, and its two ends marked.
				Frame(Faint, 1.0f);
				Line(X0, Y1, X1, Y1, Colour, 2.4f);
				Dot(X0, Y1, Colour, 1.7f);
				Dot(X1, Y1, Colour, 1.7f);
				break;

			case ESurfIcon::Vertex:
				// A faint frame with one corner bright: a single point.
				Frame(Faint, 1.0f);
				Dot(X0, Y0, Faint, 1.5f);
				Dot(X1, Y0, Faint, 1.5f);
				Dot(X0, Y1, Faint, 1.5f);
				Dot(X1, Y1, Colour, 2.6f);
				break;
			}

			return LayerId + 2;
		}

	private:
		ESurfIcon Icon = ESurfIcon::Object;
		TAttribute<FSlateColor> Tint;
	};

	FSurfVerb MakeVerb(const FText& Label, const FText& Tip, ESurfVerbGroup Group,
		TFunction<void()> Run, TFunction<bool()> IsEnabled = nullptr)
	{
		FSurfVerb Verb;
		Verb.Label = Label;
		Verb.Tip = Tip;
		Verb.Group = Group;
		// A verb that runs brings the Parameters window to itself, when the
		// window is open (S154a): "the option available in the right click
		// menu need also be visible in the param popup upon using an action".
		const FString Id = Label.ToString();
		Verb.Run = [Id, Run = MoveTemp(Run)]()
		{
			SurfParamsTab::Follow(Id);
			if (Run) { Run(); }
		};
		Verb.IsEnabled = MoveTemp(IsEnabled);
		return Verb;
	}

	FText SurfSheetPresetName(int32 Preset)
	{
		switch (static_cast<ESurfSheetPreset>(FMath::Clamp(Preset, 0, static_cast<int32>(ESurfSheetPreset::Count) - 1)))
		{
		case ESurfSheetPreset::Ramp: return LOCTEXT("PresetRamp", "ramp");
		case ESurfSheetPreset::Arch: return LOCTEXT("PresetArch", "arch");
		case ESurfSheetPreset::Dome: return LOCTEXT("PresetDome", "dome");
		case ESurfSheetPreset::Bowl: return LOCTEXT("PresetBowl", "bowl");
		default: return LOCTEXT("PresetFlat", "flat");
		}
	}

	FText SurfSheetPresetTip(int32 Preset)
	{
		switch (static_cast<ESurfSheetPreset>(FMath::Clamp(Preset, 0, static_cast<int32>(ESurfSheetPreset::Count) - 1)))
		{
		case ESurfSheetPreset::Ramp: return LOCTEXT("PresetRampTip", "Level at one edge, half the patch high at the other.");
		case ESurfSheetPreset::Arch: return LOCTEXT("PresetArchTip", "A barrel vault: a half-round along one direction, straight along the other.");
		case ESurfSheetPreset::Dome: return LOCTEXT("PresetDomeTip", "Round both ways, highest in the middle.");
		case ESurfSheetPreset::Bowl: return LOCTEXT("PresetBowlTip", "A dome the other way up: a dish, or the floor of a crater.");
		default: return LOCTEXT("PresetFlatTip", "A flat grid. Everything else starts from this one.");
		}
	}

	FSurfVerb WithLiveLabel(FSurfVerb Verb, TFunction<FText()> Label)
	{
		Verb.LiveLabel = MoveTemp(Label);
		return Verb;
	}

	/** A window's verb (S247f): View › Windows lists it; the View family's rows do not. */
	FSurfVerb AsWindow(FSurfVerb Verb)
	{
		Verb.bWindow = true;
		return Verb;
	}

	FSurfVerb WithOptions(FSurfVerb Verb, TFunction<void(FMenuBuilder&)> Options)
	{
		Verb.Options = MoveTemp(Options);
		return Verb;
	}

	/** A patch has to be selected before most of this means anything. */
	TFunction<bool()> NeedsPatch(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return [Mode]() { return Mode.IsValid() && Mode->GetSelectedPatch().IsValid(); };
	}

	TFunction<bool()> NeedsPoints(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return [Mode]() { return Mode.IsValid() && Mode->GetPointSelection().Num() > 0; };
	}

	/** A surfacing verb runs over patches and brush faces alike (S67a): either is enough. */
	TFunction<bool()> NeedsSurface(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return [Mode]() { return Mode.IsValid() && (Mode->GetSelectedPatch().IsValid() || Mode->GetSelection().Num() > 0); };
	}

	/**
	 * Separate needs picked FACES, not points.
	 *
	 * Gating it on points meant the button lit up in vertex mode, did
	 * nothing, and wrote its reason into a line nobody was looking at - which
	 * is indistinguishable from a broken button. A verb that cannot run
	 * should look like it cannot run.
	 */
	/** A brush face is picked: the brush verbs, not the patch cells NeedsFaces means. */
	TFunction<bool()> NeedsBrushFaces(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return [Mode]() { return Mode.IsValid() && (Mode->GetSelection().Num() > 0 || Mode->GetActiveFace().IsValid()); }; 
	}

	TFunction<bool()> NeedsFaces(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return [Mode]() { return Mode.IsValid() && Mode->GetQuadSelection().Num() > 0; };
	}

	USurfInspector* Panel(TWeakObjectPtr<USurfEdMode> Mode)
	{
		return Mode.IsValid() ? Mode->Inspector : nullptr;
	}

	// --- Option builders --------------------------------------------------------
	// Each one fills a menu with the knobs that change what its verb does.
	// Spin boxes rather than the docked panel's fields, because the panel is
	// the developer's side of the tool and the user should never need it.

	/**
	 * Backspace over a knob puts it back to its default, as in Blender. Keys
	 * go to the focused widget, not the hovered one, so the knobs are kept
	 * in a list and a listener ahead of all input finds the one under the
	 * cursor. The bar owns the listener and clears the list when it goes.
	 */
	struct FSurfResettable
	{
		TWeakPtr<SWidget> Widget;
		TFunction<void()> Reset;
	};

	TArray<FSurfResettable> GResettables;

	void ResetHoveredKnob(const FKeyEvent& Event)
	{
		/**
		 * Backspace or Return (S150a): "whenever hitting return over a
		 * parameter : reset it to default!"
		 *
		 * Both, and neither while the knob is being typed into - there
		 * Backspace is a letter going away and Return is the number being
		 * kept, which is what the focus test below is for.
		 */
		if ((Event.GetKey() != EKeys::BackSpace && Event.GetKey() != EKeys::Enter)
			|| !FSlateApplication::IsInitialized())
		{
			return;
		}

		for (int32 I = GResettables.Num() - 1; I >= 0; --I)
		{
			TSharedPtr<SWidget> Widget = GResettables[I].Widget.Pin();
			if (!Widget.IsValid())
			{
				GResettables.RemoveAt(I);
				continue;
			}

			// Not while typing into it: Backspace is then a letter going away
			// and Return is the number being kept.
			if (Widget->IsHovered() && !FSlateApplication::Get().HasFocusedDescendants(Widget.ToSharedRef()))
			{
				GResettables[I].Reset();
			}
		}
	}

	TSharedRef<SWidget> Spin(const TAttribute<double>& Value, double Min, double Max, TFunction<void(double)> OnChanged,
		TFunction<void()> Reset = nullptr)
	{
		TSharedRef<SSpinBox<double>> Box = SNew(SSpinBox<double>)
			.Value(Value)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			// The drag covers a sensible span; a bigger number is typed
			// (S166a): a slider to 100000 turned a twitch into a 420 m box.
			.MaxSliderValue(FMath::Min(Max, FMath::Max(Min * 4.0, 2000.0)))
			.MaxFractionalDigits(2)
			.OnValueChanged_Lambda([OnChanged](double New) { OnChanged(New); })
			.OnValueCommitted_Lambda([OnChanged](double New, ETextCommit::Type) { OnChanged(New); });

		if (Reset)
		{
			GResettables.Add({ Box, MoveTemp(Reset) });
		}

		return SNew(SBox).WidthOverride(140.0f)
			.ToolTipText(LOCTEXT("KnobTip", "Drag, or type a number. Backspace over it puts the default back."))
		[
			Box
		];
	}

	TSharedRef<SWidget> SpinInt(const TAttribute<int32>& Value, int32 Min, int32 Max, TFunction<void(int32)> OnChanged,
		TFunction<void()> Reset = nullptr)
	{
		TSharedRef<SSpinBox<int32>> Box = SNew(SSpinBox<int32>)
			.Value(Value)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.OnValueChanged_Lambda([OnChanged](int32 New) { OnChanged(New); })
			.OnValueCommitted_Lambda([OnChanged](int32 New, ETextCommit::Type) { OnChanged(New); });

		if (Reset)
		{
			GResettables.Add({ Box, MoveTemp(Reset) });
		}

		return SNew(SBox).WidthOverride(140.0f)
			.ToolTipText(LOCTEXT("KnobTip", "Drag, or type a number. Backspace over it puts the default back."))
		[
			Box
		];
	}

	/**
	 * True while a verb's options are built into its frame (S206a), under
	 * the frame's own knobs: "on bevel popup param you show twice keep
	 * crease only subdivide ... make sure this dont happen". An option the
	 * frame already has as a knob stays out of the frame; the right-click
	 * still shows it, before the verb has run.
	 */
	bool GSurfOptionsInFrame = false;

	void AddToggle(FMenuBuilder& Menu, const FText& Label, const FText& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Get, Set]() { Set(!Get()); }),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Get]() { return Get(); })),
			NAME_None,
			EUserInterfaceActionType::ToggleButton);
	}

	void SaveSettings()
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->SaveConfig();
		}
	}

	/**
	 * A button that is a state, not a verb: filled the whole way across while
	 * on, a plain dark button while off. The first version only changed the
	 * colour of the label, and at eight points that read as "clicking does
	 * nothing".
	 *
	 * The fill is Unreal's own Primary (S91a). It was amber, which is a
	 * colour this editor uses nowhere else - a bar that means to sit inside
	 * Unreal has to be made of Unreal's parts, not of a palette of mine.
	 */
	const FCheckBoxStyle& ToggleStyle()
	{
		static FCheckBoxStyle Style = FCheckBoxStyle(FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox"))
			.SetUncheckedImage(FSlateRoundedBoxBrush(FStyleColors::Secondary, 4.0f))
			.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(FStyleColors::Hover, 4.0f))
			.SetUncheckedPressedImage(FSlateRoundedBoxBrush(FStyleColors::Dropdown, 4.0f))
			.SetCheckedImage(FSlateRoundedBoxBrush(FStyleColors::Primary, 4.0f))
			.SetCheckedHoveredImage(FSlateRoundedBoxBrush(FStyleColors::PrimaryHover, 4.0f))
			.SetCheckedPressedImage(FSlateRoundedBoxBrush(FStyleColors::PrimaryPress, 4.0f))
			.SetForegroundColor(SurfTheme::Quiet())
			.SetHoveredForegroundColor(FStyleColors::ForegroundHover)
			.SetPressedForegroundColor(FStyleColors::ForegroundHover)
			.SetCheckedForegroundColor(FSlateColor(FLinearColor::White))
			.SetCheckedHoveredForegroundColor(FSlateColor(FLinearColor::White))
			.SetCheckedPressedForegroundColor(FSlateColor(FLinearColor::White))
			.SetPadding(FMargin(0.0f));
		return Style;
	}

	/**
	 * A verb chip's three states (S92a): "buttons need slight hover + clicked
	 * too". A button that does not answer the cursor reads as a label, and a
	 * bar of labels is a bar nobody tries. Normal, hovered, pressed and
	 * disabled are all Unreal's own greys, and the foreground comes with them,
	 * so a dead verb dims without anybody computing a colour for it.
	 */
	const FButtonStyle& VerbButtonStyle()
	{
		// The editor's viewport-toolbar button (S192b): Dropdown at rest,
		// Hover under the hand, Header while pressed, its 4-pixel corners.
		// Dark and nearly square (S193b): "more squarish and dark like the
		// quickpanel" - the SidePanel's tab colours.
		static FButtonStyle Style = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f))
			.SetHovered(FSlateRoundedBoxBrush(FStyleColors::Hover, 2.0f))
			.SetPressed(FSlateRoundedBoxBrush(FStyleColors::Header, 2.0f))
			.SetDisabled(FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f))
			.SetNormalForeground(SurfTheme::Quiet())
			.SetHoveredForeground(FStyleColors::ForegroundHover)
			.SetPressedForeground(FStyleColors::ForegroundHover)
			.SetDisabledForeground(FStyleColors::Hover2)
			.SetNormalPadding(FMargin(0.0f))
			.SetPressedPadding(FMargin(0.0f));
		return Style;
	}

	/** The bar's scroll box: a Ctrl+wheel while arranging is not a scroll, it is the chips' size (S166a). */
	/** Whether the last Patch ran on picked faces (S172a): its frame then has no size to offer. */
	bool& SurfPatchFromFaces()
	{
		static bool bFromFaces = false;
		return bFromFaces;
	}

	class SSurfBarScrollBox : public SScrollBox
	{
	public:
		virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (Event.IsControlDown() && SurfVerbLayout::IsEditing())
			{
				return FReply::Unhandled();
			}
			return SScrollBox::OnMouseWheel(Geometry, Event);
		}
	};

	/**
	 * The bar's three grips, one look (S169a): "it must be much better
	 * graphically - all of the blue resize grips/bars. it looks terribly bad".
	 *
	 * Nothing to look at unless there is something to do: at rest the grip is
	 * clear glass - it is found by the cursor, which changes over it. While
	 * the interface is being arranged it shows as a faint line so the hand
	 * knows where to go; under the hand or while dragging it is one bright
	 * line, two pixels, as the editor's own splitters do. The move handle is
	 * the same line, short, in the middle of its strip.
	 */
	class SSurfGrip : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfGrip) : _Vertical(true), _Handle(false), _Thickness(6.0f) {}
			/** Down the edge (true) or across the column (false). */
			SLATE_ARGUMENT(bool, Vertical)
			/** The move handle: a short dash rather than a whole line, and the cross cursor. */
			SLATE_ARGUMENT(bool, Handle)
			/** How many pixels it takes across its length. */
			SLATE_ARGUMENT(float, Thickness)
			/** Along its length, when the parent does not size it. */
			SLATE_ATTRIBUTE(FOptionalSize, Length)
			/** True while the bar is being dragged by this grip. */
			SLATE_ATTRIBUTE(bool, Dragging)
			/** Shown at rest, faint - while the interface is being edited. */
			SLATE_ATTRIBUTE(bool, Shown)
			SLATE_ATTRIBUTE(FText, Tip)
			SLATE_EVENT(FPointerEventHandler, OnDown)
			SLATE_EVENT(FPointerEventHandler, OnDoubleClick)
		SLATE_END_ARGS()

		void Construct(const FArguments& A)
		{
			bVertical = A._Vertical;
			bHandle = A._Handle;
			Thickness = A._Thickness;
			Length = A._Length;
			Dragging = A._Dragging;
			Shown = A._Shown;
			OnDown = A._OnDown;
			OnDoubleClick = A._OnDoubleClick;
			SetToolTipText(A._Tip);
			SetCursor(bHandle ? EMouseCursor::CardinalCross : (bVertical ? EMouseCursor::ResizeLeftRight : EMouseCursor::ResizeUpDown));
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			const FOptionalSize Along = Length.Get(FOptionalSize());
			const float L = Along.IsSet() ? Along.Get() : 0.0f;
			return bVertical ? FVector2D(Thickness, L) : FVector2D(L, Thickness);
		}

		virtual FReply OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E) override
		{
			return OnDown.IsBound() ? OnDown.Execute(G, E) : FReply::Unhandled();
		}

		virtual FReply OnMouseButtonDoubleClick(const FGeometry& G, const FPointerEvent& E) override
		{
			return OnDoubleClick.IsBound() ? OnDoubleClick.Execute(G, E) : FReply::Unhandled();
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Rect, FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override
		{
			const bool bActive = Dragging.Get(false) || IsHovered();
			if (!bActive && !Shown.Get(false))
			{
				return LayerId;
			}
			const FLinearColor Blue = FStyleColors::Primary.GetColor(Style);
			const FLinearColor Colour = bActive ? Blue : FLinearColor(Blue.R, Blue.G, Blue.B, 0.35f);
			const float Line = bActive ? 2.0f : 1.0f;
			const FVector2D Size = Geometry.GetLocalSize();
			FVector2D At, Extent;
			if (bVertical)
			{
				At = FVector2D(FMath::RoundToFloat((Size.X - Line) * 0.5f), 0.0f);
				Extent = FVector2D(Line, Size.Y);
			}
			else if (bHandle)
			{
				const float Dash = FMath::Min(28.0f, Size.X);
				At = FVector2D(FMath::RoundToFloat((Size.X - Dash) * 0.5f), FMath::RoundToFloat((Size.Y - Line) * 0.5f));
				Extent = FVector2D(Dash, Line);
			}
			else
			{
				At = FVector2D(0.0f, FMath::RoundToFloat((Size.Y - Line) * 0.5f));
				Extent = FVector2D(Size.X, Line);
			}
			FSlateDrawElement::MakeBox(Out, LayerId + 1, Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(At)),
				FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, Colour);
			return LayerId + 1;
		}

	private:
		bool bVertical = true;
		bool bHandle = false;
		float Thickness = 6.0f;
		TAttribute<FOptionalSize> Length;
		TAttribute<bool> Dragging;
		TAttribute<bool> Shown;
		FPointerEventHandler OnDown;
		FPointerEventHandler OnDoubleClick;
	};

	/** A scrollbar that is nearly not there (S166a): "make sidebar scrollbar much much more discreet almost invisible." A two-pixel thumb, faint, brighter under the hand. */
	const FScrollBarStyle& QuietScrollBarStyle()
	{
		static FScrollBarStyle Style = FScrollBarStyle(FAppStyle::Get().GetWidgetStyle<FScrollBarStyle>("ScrollBar"))
			.SetVerticalBackgroundImage(FSlateNoResource())
			.SetHorizontalBackgroundImage(FSlateNoResource())
			.SetNormalThumbImage(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.10f), 1.0f))
			.SetHoveredThumbImage(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.30f), 1.0f))
			.SetDraggedThumbImage(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.45f), 1.0f))
			.SetThickness(2.0f);
		return Style;
	}

	/** The same answer, on something that is only an icon: nothing until the cursor is on it. */
	/**
	 * A rule between the groups of a line rather than a gap: it is what
	 * Unreal puts between the sections of its own toolbars, and unlike a gap
	 * it holds the eye's place when the line is scrolled (S91a).
	 */
	TSharedRef<SWidget> LineRule()
	{
		return SNew(SBox).WidthOverride(1.0f).HeightOverride(13.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FStyleColors::Recessed)
			.Padding(0.0f)
			[ SNullWidget::NullWidget ]
		];
	}

	const FButtonStyle& IconButtonStyle()
	{
		static FButtonStyle Style = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(FStyleColors::Transparent, 4.0f))
			.SetHovered(FSlateRoundedBoxBrush(FStyleColors::Hover, 4.0f))
			.SetPressed(FSlateRoundedBoxBrush(FStyleColors::Primary, 4.0f))
			.SetDisabled(FSlateRoundedBoxBrush(FStyleColors::Transparent, 4.0f))
			.SetNormalForeground(SurfTheme::Quiet())
			.SetHoveredForeground(FStyleColors::White)
			.SetPressedForeground(FStyleColors::White)
			.SetDisabledForeground(FStyleColors::Secondary)
			.SetNormalPadding(FMargin(0.0f))
			.SetPressedPadding(FMargin(0.0f));
		return Style;
	}

	/**
	 * What a button wears while the interface is being edited (S92a): an
	 * outline, not a coat of paint. The amber veil made every chip a
	 * different colour and every row a different height - "interface is
	 * totally different under edit interface mode enabled", and it was. A
	 * one-pixel border says the same thing and moves nothing.
	 */
	const FSlateBrush* EditOutlineBrush()
	{
		static FSlateRoundedBoxBrush Brush(FStyleColors::Transparent, 3.0f, FStyleColors::Primary, 1.0f);
		return &Brush;
	}

	/**
	 * The plate the top line sits on: Unreal's panel colour, with Unreal's
	 * corner radius, so it reads as one of the editor's own floating toolbars
	 * rather than as a stripe painted across the viewport (S91a).
	 */
	const FSlateBrush* TopBarBrush()
	{
		static FSlateRoundedBoxBrush Brush(FStyleColors::Panel, 4.0f);
		return &Brush;
	}

	/** A floating frame (S192d): the editor's panel, its corners, its dark 1-pixel edge. */
	const FSlateBrush* FrameBrush()
	{
		static FSlateRoundedBoxBrush Brush(FStyleColors::Panel, 4.0f, FStyleColors::WindowBorder, 1.0f);
		return &Brush;
	}

	/** A frame's title strip: the editor's header grey, rounded at the top only. */
	const FSlateBrush* FrameHeaderBrush()
	{
		static FSlateRoundedBoxBrush Brush(FStyleColors::Header, FVector4(4.0f, 4.0f, 0.0f, 0.0f));
		return &Brush;
	}

	/**
	 * A family's tab down the sidebar's edge (S193b): "perhaps we may reuse
	 * the vertical tabbing from the other quickpanel". The name measured as
	 * a line and drawn turned a quarter, as the SidePanel's tabs are.
	 */
	class SSurfTabLabel : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfTabLabel) {}
			SLATE_ARGUMENT(FText, Text)
			SLATE_ARGUMENT(float, Size)
			SLATE_ATTRIBUTE(FSlateColor, Colour)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Text = InArgs._Text.ToString();
			Colour = InArgs._Colour;
			Font = SurfTheme::Bold(InArgs._Size > 0.0f ? InArgs._Size : 8.0f);
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			const FVector2D Size = Measured();
			return FVector2D(Size.Y, Size.X);
		}

		virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out,
			int32 LayerId, const FWidgetStyle& Style, bool) const override
		{
			const FVector2D Line = Measured();
			const FVector2D Room = Geometry.GetLocalSize();
			const FVector2f Offset(static_cast<float>((Room.X - Line.X) * 0.5), static_cast<float>((Room.Y - Line.Y) * 0.5));
			const FSlateRenderTransform Turn(FQuat2D(FMath::DegreesToRadians(90.0f)));
			const FPaintGeometry Paint = Geometry.ToPaintGeometry(FVector2f(static_cast<float>(Line.X), static_cast<float>(Line.Y)),
				FSlateLayoutTransform(Offset), Turn, FVector2f(0.5f, 0.5f));
			const FLinearColor Tint = Colour.Get(FSlateColor::UseForeground()).GetColor(Style) * Style.GetColorAndOpacityTint();
			FSlateDrawElement::MakeText(Out, LayerId, Paint, Text, Font, ESlateDrawEffect::None, Tint);
			return LayerId;
		}

	private:
		FVector2D Measured() const
		{
			if (!FSlateApplication::IsInitialized())
			{
				return FVector2D(40.0, 12.0);
			}
			return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font);
		}

		FString Text;
		TAttribute<FSlateColor> Colour;
		FSlateFontInfo Font;
	};

	const FCheckBoxStyle& SegmentStyle()
	{
		static FCheckBoxStyle Style = FCheckBoxStyle(ToggleStyle())
			.SetUncheckedImage(FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f))
			.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(FStyleColors::Hover, 2.0f))
			.SetUncheckedPressedImage(FSlateRoundedBoxBrush(FStyleColors::Header, 2.0f))
			.SetCheckedImage(FSlateRoundedBoxBrush(FStyleColors::Primary, 2.0f))
			.SetCheckedHoveredImage(FSlateRoundedBoxBrush(FStyleColors::PrimaryHover, 2.0f))
			.SetCheckedPressedImage(FSlateRoundedBoxBrush(FStyleColors::PrimaryPress, 2.0f));
		return Style;
	}

	/** True while the top line is being built: its switches wear the segment style. */
	bool GTopLineSwitches = false;

	TSharedRef<SWidget> ToggleButton(TSharedRef<SWidget> Content, const FText& Tip, float Width, float Height,
		TFunction<bool()> IsOn, TFunction<void()> Toggle)
	{
		return SNew(SBox).WidthOverride(Width).HeightOverride(Height)
		[
			SNew(SCheckBox)
			.Style(GTopLineSwitches ? &SegmentStyle() : &ToggleStyle())
			.ToolTipText(Tip)
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f))
			.IsChecked_Lambda([IsOn]() { return IsOn && IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Toggle](ECheckBoxState) { if (Toggle) { Toggle(); } })
			[
				// Centred both ways in the chip (S175a): "center C and Auto -
				// the text is not centred vertically, it's a bit up".
				SNew(SBox).HeightOverride(Height).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					Content
				]
			]
		];
	}

}

// --- Every verb, in one list ------------------------------------------------
//
// The docked panel's sections and the X search read the same list, so a verb
// that is only on the panel can still be run by name from the viewport - which
// is what "the panel is for developers" has to mean before it can be true.

namespace
{
	/**
	 * Mapping is its own family (S90a): "UV can be its own header too?" - yes.
	 * Half of Surface was about where the texture lands and half was about
	 * what is painted on it, and the two are never wanted at the same moment.
	 * Tagged by name in one place rather than at each verb, so the bar, the
	 * window and the menu cannot end up disagreeing.
	 */
	void TagUVFamily(TArray<FSurfVerb>& Verbs)
	{
		static const TCHAR* Mapping[] = { TEXT("Flow"), TEXT("Even"), TEXT("Align"), TEXT("Project"),
			TEXT("Inherit Face"), TEXT("Fit"), TEXT("Density"), TEXT("Swap UV"), TEXT("UV Smooth"),
			TEXT("UV Window"), TEXT("UVs"), TEXT("Pack"), TEXT("Stack"), TEXT("Rectify"), TEXT("Straighten") };

		for (FSurfVerb& Verb : Verbs)
		{
			if (Verb.Group != ESurfVerbGroup::Surface)
			{
				continue;
			}
			const FString Name = Verb.Label.ToString();
			for (const TCHAR* Each : Mapping)
			{
				if (Name == Each)
				{
					Verb.Group = ESurfVerbGroup::UV;
					break;
				}
			}
		}
	}

	TArray<FSurfVerb> BuildAllVerbs(TWeakObjectPtr<USurfEdMode> M)
	{
	TArray<FSurfVerb> Create;
	/**
	 * New Patch, New Box, New Cylinder, Cylinder, Bevel, Endcap, Cone and
	 * Patch left this family in S156b: Add makes every one of them, and a
	 * verb lives in one place. "I wouldn't list primitives until user is
	 * creating one."
	 */
	Create.Add(MakeVerb(LOCTEXT("MakePrefab", "Prefab from Selection"), LOCTEXT("MakePrefabTip", "What is picked becomes a prefab asset, one instance standing where it was; the Assets tab places more."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->MakePrefabFromSelection(); } }, NeedsPatch(M)));
	Create.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("Bake", "Bake"), LOCTEXT("BakeTip", "The level's geometry and every prefab to Nanite static meshes with collision, in the background; a second press stops it."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->BakeLevel(); } }),
		[M]() { return M.IsValid() && M->IsBaking() ? LOCTEXT("BakeStop", "Stop Bake") : LOCTEXT("Bake", "Bake"); }),
		[M](FMenuBuilder& Menu)
		{
			AddToggle(Menu, LOCTEXT("OptBakeBackground", "Bake in the background"),
				LOCTEXT("OptBakeBackgroundTip", "On: a slice a frame, the editor stays up, and the count is in the bottom-left readout. Off: the editor waits until it is done, in one undo step."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bBakeInBackground; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bBakeInBackground = V; S->SaveConfig(); } });
		}));
	Create.Add(MakeVerb(LOCTEXT("UnpackPrefab", "Unpack"), LOCTEXT("UnpackPrefabTip", "The selected instance back into the level as plain geometry, where it stood; the asset stays in the library."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->UnpackSelectedPrefab(); } }, [M]() { return M.IsValid() && M->HasSelectedInstance(); }));
	Create.Add(MakeVerb(LOCTEXT("UniquePrefab", "Unique"), LOCTEXT("UniquePrefabTip", "This instance gets its own copy of the prefab, under the next free name - editing it changes nothing else placed."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->MakePrefabUnique(); } }, [M]() { return M.IsValid() && M->HasSelectedInstance(); }));

	TArray<FSurfVerb> Geometry;
	Geometry.Add(MakeVerb(LOCTEXT("Extrude", "Extrude"), LOCTEXT("ExtrudeTip", "Grow past the edge the selected row or column sits on."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Extrude(); } }, NeedsPoints(M)));
	Geometry.Add(MakeVerb(LOCTEXT("Duplicate", "Duplicate"), LOCTEXT("DuplicateTip", "Copy each selected patch along its own normal."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Duplicate(); } }, NeedsPatch(M)));
	Geometry.Add(MakeVerb(LOCTEXT("Solidify", "Solidify"), LOCTEXT("SolidifyTip", "Thickness: front, back and four skirts."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Solidify(); } }, NeedsPatch(M)));
	/**
	 * Loop and Push (S156c): Insert Row and Insert Column were one verb
	 * with a direction, as were Push Out and Push In. The direction is a
	 * parameter now, and Ctrl+R's own preview keeps its key.
	 */
	Geometry.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("Loop", "Loop"), LOCTEXT("LoopTip", "A line added through the selected patch - a row or a column. Ctrl+R does it with the mouse: the wheel adds cuts, the drag slides."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { const USurfSettings* S = GetDefault<USurfSettings>(); if (S && S->bLoopIsColumn) { P->InsertColumn(); } else { P->InsertRow(); } } }, NeedsPatch(M)),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return (S && S->bLoopIsColumn) ? LOCTEXT("LoopColumn", "Loop: column") : LOCTEXT("LoopRow", "Loop: row"); }),
		[](FMenuBuilder& Menu)
		{
			auto Way = [](bool bColumn)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([bColumn]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bLoopIsColumn = bColumn; S->SaveConfig(); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bColumn]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bLoopIsColumn == bColumn; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptLoopRow", "Row"), LOCTEXT("OptLoopRowTip", "Across the patch."), FSlateIcon(), Way(false), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptLoopColumn", "Column"), LOCTEXT("OptLoopColumnTip", "Down the patch."), FSlateIcon(), Way(true), NAME_None, EUserInterfaceActionType::RadioButton);
		}));
	Geometry.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("Push", "Push"), LOCTEXT("PushTip", "The picked primitive faces one grid step along their normal - out, or in by the parameters."),
		ESurfVerbGroup::Geometry, [M]() { if (M.IsValid()) { const USurfSettings* S = GetDefault<USurfSettings>(); const double Step = S ? S->GetGridStepCm(S->DefaultGridIndex) : 100.0; M->PushSelectedFaces((S && S->bPushIn) ? -Step : Step); } }, NeedsBrushFaces(M)),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return (S && S->bPushIn) ? LOCTEXT("PushInLive", "Push: in") : LOCTEXT("PushOutLive", "Push: out"); }),
		[](FMenuBuilder& Menu)
		{
			auto Way = [](bool bIn)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([bIn]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bPushIn = bIn; S->SaveConfig(); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bIn]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bPushIn == bIn; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptPushOut", "Out"), LOCTEXT("OptPushOutTip", "Along the normal."), FSlateIcon(), Way(false), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptPushIn", "In"), LOCTEXT("OptPushInTip", "Back along it."), FSlateIcon(), Way(true), NAME_None, EUserInterfaceActionType::RadioButton);
		}));
	Geometry.Add(MakeVerb(LOCTEXT("Respace", "Respace"), LOCTEXT("RespaceTip", "Even out the spacing of the control net."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Respace(); } }, NeedsPatch(M)));
	// "Its nice but how do I Apply?" - it was called To Sheet, which says
	// what it makes and not what it is for (S73b).
	Geometry.Add(WithOptions(MakeVerb(LOCTEXT("ApplyCurve", "Apply"), LOCTEXT("ApplyCurveTip", "The curve becomes a grid of real points, as fine as the subdivision; the shape stays. Right-click: how fine."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->ConvertToSheet(); } }, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([M]()
				{
					const USurfInspector* P = M.IsValid() ? M->Inspector : nullptr;
					return P ? P->Subdivision : 8;
				}),
				FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv,
				[M](int32 V)
				{
					if (USurfInspector* P = Panel(M))
					{
						P->Subdivision = V;
						P->ApplySubdivision();
					}
				},
				[M]()
				{
					if (USurfInspector* P = Panel(M))
					{
						P->Subdivision = 8;
						P->ApplySubdivision();
					}
				}),
				LOCTEXT("OptSubdivision", "How fine the curve is drawn"));
			AddToggle(Menu, LOCTEXT("OptSurfaceWire", "Draw the subdivided surface (pink)"),
				LOCTEXT("OptSurfaceWireTip", "A curve's real surface as thin pink lines under its white cage: the cage steers the shape, the pink is the shape. Apply turns the pink into points."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bShowSurfaceWireframe; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bShowSurfaceWireframe = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
		}));

	TArray<FSurfVerb> Shape;
	Shape.Add(MakeVerb(LOCTEXT("Smooth", "Smooth"), LOCTEXT("SmoothTip", "Relax the selection. Sliders update live."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Smooth(); } }, NeedsPatch(M)));
	Shape.Add(MakeVerb(LOCTEXT("Inflate", "Inflate"), LOCTEXT("InflateTip", "Push along the surface normal."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Inflate(); } }, NeedsPatch(M)));
	Shape.Add(MakeVerb(LOCTEXT("Flatten", "Flatten"), LOCTEXT("FlattenTip", "Pull onto the best-fit plane."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Flatten(); } }, NeedsPatch(M)));
	Shape.Add(MakeVerb(LOCTEXT("BevelLine", "Bevel Line"), LOCTEXT("BevelLineTip", "Round the crease along the selected line."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Bevel(); } }, NeedsPoints(M)));

	TArray<FSurfVerb> Surface;
	Surface.Add(MakeVerb(LOCTEXT("Flow", "Flow"), LOCTEXT("FlowTip", "The texture follows the surface, a metre of surface to a metre of texture wherever it bends. For anything curved."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Natural(); } }, NeedsSurface(M)));
	Surface.Add(MakeVerb(LOCTEXT("Even", "Even"), LOCTEXT("EvenTip", "Each cell its own size in the texture, one unit for the whole net, kept on the lines - a long cell is a long run of tiles."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Even(); } }, NeedsSurface(M)));
	Surface.Add(MakeVerb(LOCTEXT("Align", "Align"), LOCTEXT("AlignTip", "Projected from each patch's own direction - the mapping a primitive face gets."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Planar(); } }, NeedsSurface(M)));
	Surface.Add(MakeVerb(LOCTEXT("Project", "Project"), LOCTEXT("ProjectTip", "One projection along a world axis for every patch selected, so a wall that bends has no seam where its patches meet."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Project(); } }, NeedsSurface(M)));
	Surface.Add(MakeVerb(LOCTEXT("Inherit", "Inherit Face"), LOCTEXT("InheritTip", "Take the last clicked face's projection, to close a seam."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->InheritFaceSurface(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("FitPatch", "Fit"), LOCTEXT("FitTip", "Choose the scale that spans a whole number of tiles."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->FitPatch(); } }, NeedsSurface(M)));
	Surface.Add(MakeVerb(LOCTEXT("Freeze", "Freeze"), LOCTEXT("FreezeTip", "Hold the UVs and stop deriving them."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Freeze(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("CycleLayer", "Layer"), LOCTEXT("CycleLayerTip", "The selected patches to the next layer, 0 to 3 and round: 0 the opaque surface, 1 to 3 drawn over it in turn - decals, alphas."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->CycleLayer(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("MakeLayer", "Copy Up"), LOCTEXT("MakeLayerTip", "Copy the selection one layer up, in place: the same patch on the next layer, for a decal that starts from the surface it sits on."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->MakeLayer(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("PaintWeight", "Paint"), LOCTEXT("PaintWeightTip", "Paint the vertex colour on the Paint tab's channels: drag paints up, Ctrl-drag down, Ctrl+wheel sizes the brush."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->TogglePaint(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("SwapUV", "Swap UV"), LOCTEXT("SwapTip", "Exchange the two axes."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->SwapPatchUV(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("UVSmooth", "UV Smooth"), LOCTEXT("UVSmoothTip", "Relax the selected nets: each picked point towards the mean of its neighbours, the border kept."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { const USurfInspector* P = M->Inspector; M->SmoothSelectedUVs(P ? P->Strength : 0.5, P ? P->Iterations : 4); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("UVPack", "Pack"), LOCTEXT("UVPackTip", "The selected nets laid into the 0,0 tile, tallest first, sizes kept relative; the gap is the Pack margin (Islands)."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { const USurfSettings* S = GetDefault<USurfSettings>(); M->PackSelectedUVs(S ? S->UVPackMargin : 0.02); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("UVStack", "Stack"), LOCTEXT("UVStackTip", "Every selected net moved onto the first's centre, so alike pieces share one place on the texture. A rule is frozen first."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->StackSelectedUVs(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("UVRectify", "Rectify"), LOCTEXT("UVRectifyTip", "Each selected net made an even grid inside its own box, its turn kept - a pulled-about net made straight again."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->RectifySelectedUVs(); } }, NeedsPatch(M)));
	Surface.Add(MakeVerb(LOCTEXT("UVStraighten", "Straighten"), LOCTEXT("UVStraightenTip", "The picked points of one row or column of a net put on one line, along the axis they lean to; each keeps its place along it."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->StraightenSelectedUVs(); } }, NeedsPatch(M)));

	TArray<FSurfVerb> Select;
	Select.Add(MakeVerb(LOCTEXT("None", "None"), LOCTEXT("NoneTip", "Drop the whole selection: the points, the cells, the primitive faces and the patch."),
		ESurfVerbGroup::Select, [M]()
		{
			if (M.IsValid())
			{
				// "clicking in none should clear selection" - it cleared the
				// points and left the faces and the patch picked (S72e).
				M->ClearPointSelection();
				M->ClearSelection();
				M->SelectPatch(FSurfPatchRef());
			}
		}));
	Select.Add(MakeVerb(LOCTEXT("Inv", "Invert"), LOCTEXT("InvTip", "Everything that is not selected."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->InvertPointSelection(); } }, NeedsPatch(M)));
	Select.Add(WithLiveLabel(MakeVerb(LOCTEXT("Names", "Names"), LOCTEXT("NamesTip", "The level's geometry, every placed prefab and every group say what they are, over them. Press again for a quiet view."),
		ESurfVerbGroup::View, [M]()
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				S->bShowNames = !S->bShowNames;
				S->SaveConfig();
			}
			if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
		}),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bShowNames ? LOCTEXT("NamesOn", "Names: on") : LOCTEXT("Names", "Names"); }));
	Select.Add(WithLiveLabel(MakeVerb(LOCTEXT("ColourIds", "Colour IDs"), LOCTEXT("ColourIdsTip", "Every patch draws its cage in a colour of its own, so a blockout reads as pieces. Press again for the one colour."),
		ESurfVerbGroup::View, [M]()
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				S->bColourIdWires = !S->bColourIdWires;
				S->SaveConfig();
			}
			if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
		}),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bColourIdWires ? LOCTEXT("ColourIdsOn", "Colour IDs on") : LOCTEXT("ColourIds", "Colour IDs"); }));
	Select.Add(MakeVerb(LOCTEXT("BoxTool", "Box"), LOCTEXT("BoxToolTip", "Drag a rectangle to select."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SetSelectTool(ESurfSelectTool::Box); } }));
	Select.Add(MakeVerb(LOCTEXT("LassoTool", "Lasso"), LOCTEXT("LassoToolTip", "Draw a freehand loop."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SetSelectTool(ESurfSelectTool::Lasso); } }));
	Select.Add(MakeVerb(LOCTEXT("BrushTool", "Brush"), LOCTEXT("BrushToolTip", "Paint over points."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SetSelectTool(ESurfSelectTool::Brush); } }));
	Surface.Add(MakeVerb(LOCTEXT("ReplaceMaterial", "Replace"), LOCTEXT("ReplaceMaterialTip", "Pick a patch wearing the material to replace, then one wearing the new: everything wearing the first takes the second."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->ReplaceMaterialFromSelection(); } }, NeedsSurface(M)));

	TArray<FSurfVerb> All;
	All.Append(Create);
	All.Append(Geometry);
	All.Append(Shape);
	All.Append(Surface);
	All.Append(Select);
	TagUVFamily(All);
	return All;
	}

	/**
	 * Asking for a name, from whichever surface asked. It used to belong to
	 * the bar; both surfaces carry the same verbs now, so it belongs to
	 * neither (S74a). One prompt at a time, as before.
	 */
	TWeakPtr<IMenu> GPromptMenu;

	namespace SurfNamePrompt
	{
	void OpenNamePrompt(TSharedRef<SWidget> Parent, const FText& Title, const FString& Suggested, TFunction<void(const FString&)> Commit);

	void OpenNewPrefabName(TSharedRef<SWidget> Parent, TWeakObjectPtr<USurfEdMode> Mode)
	{
		if (!FSlateApplication::IsInitialized() || !Mode.IsValid())
		{
			return;
		}
		TWeakObjectPtr<USurfEdMode> M = Mode;
		OpenNamePrompt(Parent, LOCTEXT("NewPrefabPrompt", "Name the new prefab, then Enter - it opens empty, ready to build in"),
			Mode->SuggestPrefabName(),
			[M](const FString& Name) { if (M.IsValid()) { M->NewPrefab(Name); } });
		SurfSession::Gesture(TEXT("new_prefab_prompt"));
	}

	void OpenSceneName(TSharedRef<SWidget> Parent, TWeakObjectPtr<USurfEdMode> Mode)
	{
		if (!FSlateApplication::IsInitialized() || !Mode.IsValid())
		{
			return;
		}
		TWeakObjectPtr<USurfEdMode> M = Mode;
		OpenNamePrompt(Parent, LOCTEXT("SceneNamePrompt", "Name the level, then Enter - it is saved under /Game/SurfacingPro/Levels"),
			Mode->SuggestSceneName(),
			[M](const FString& Name) { if (M.IsValid()) { M->MakeSceneFromSelection(Name); } });
		SurfSession::Gesture(TEXT("scene_name_prompt"));
	}

	void OpenPrefabName(TSharedRef<SWidget> Parent, TWeakObjectPtr<USurfEdMode> Mode)
	{
		if (!FSlateApplication::IsInitialized() || !Mode.IsValid())
		{
			return;
		}
		// An instance taken into the room needs no name: it has one.
		if (Mode->IsInsidePrefab() && !Mode->GetSelectedPatch().IsValid() && Mode->HasSelectedInstance())
		{
			Mode->MakePrefabFromSelection();
			return;
		}
		TWeakObjectPtr<USurfEdMode> M = Mode;
		OpenNamePrompt(Parent, LOCTEXT("PrefabNamePrompt", "Name the prefab, then Enter - it is saved under /Game/SurfacingPro/Prefabs"), Mode->SuggestPrefabName(),
			[M](const FString& Name) { if (M.IsValid()) { M->MakePrefabFromSelection(Name); } });
		SurfSession::Gesture(TEXT("prefab_name_prompt"));
	}

	void OpenSaveAsName(TSharedRef<SWidget> Parent, TWeakObjectPtr<USurfEdMode> Mode)
	{
		if (!FSlateApplication::IsInitialized() || !Mode.IsValid())
		{
			return;
		}
		const USurfPrefab* Context = Mode->ContextPrefab();
		if (!Context)
		{
			Mode->SavePrefabAs(FString());
			return;
		}
		TWeakObjectPtr<USurfEdMode> M = Mode;
		OpenNamePrompt(Parent, LOCTEXT("SaveAsPrompt", "Save a copy as, then Enter - this instance uses the copy from then on"), Mode->SuggestPrefabNameLike(Context->GetName()),
			[M](const FString& Name) { if (M.IsValid()) { M->SavePrefabAs(Name); } });
		SurfSession::Gesture(TEXT("save_as_prompt"));
	}

	void OpenNamePrompt(TSharedRef<SWidget> Parent, const FText& Title, const FString& Suggested, TFunction<void(const FString&)> Commit)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		if (TSharedPtr<IMenu> Open = GPromptMenu.Pin())
		{
			Open->Dismiss();
		}

		TSharedPtr<SEditableTextBox> Box;

		auto Done = []()
		{
			if (TSharedPtr<IMenu> Open = GPromptMenu.Pin()) { Open->Dismiss(); }
		};

		TSharedRef<SWidget> Widget =
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(6.0f)
			[
				SNew(SBox).WidthOverride(260.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock)
						.Text(Title)
						.Font(SurfTheme::Font(9))
						.ColorAndOpacity(SurfTheme::Text())
						.AutoWrapText(true)
						.WrapTextAt(250.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(Box, SEditableTextBox)
						.Text(FText::FromString(Suggested))
						.SelectAllTextWhenFocused(true)
						.Font(SurfTheme::Font(10))
						.OnTextCommitted_Lambda([Commit, Done](const FText& Text, ETextCommit::Type How)
						{
							if (How == ETextCommit::OnEnter)
							{
								Done();
								Commit(Text.ToString());
							}
						})
						.OnKeyDownHandler_Lambda([Done](const FGeometry&, const FKeyEvent& Event) -> FReply
						{
							if (Event.GetKey() == EKeys::Escape) { Done(); return FReply::Handled(); }
							return FReply::Unhandled();
						})
					]
				]
			];

		GPromptMenu = FSlateApplication::Get().PushMenu(
			Parent,
			FWidgetPath(),
			Widget,
			FSlateApplication::Get().GetCursorPos() - FVector2D(20.0, 12.0),
			FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

		if (Box.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);
		}
	}
	}
}

namespace
{
	/**
	 * Every verb there is, once each (S74a). "All buttons are moveable
	 * arrangeable as user like. Buttons cant get lose theyre either on
	 * surfacingpro either on userbar": that only works if there is one list
	 * and two surfaces drawing from it, rather than two lists built twice.
	 * Host is whichever surface is asking - a name prompt has to be pushed
	 * from a widget, and either one will do.
	 */
	TArray<FSurfVerb> BuildPoolVerbs(TWeakObjectPtr<USurfEdMode> M, TWeakPtr<SWidget> Host);
	void TagShortcuts(TArray<FSurfVerb>& Verbs);
}

namespace SurfVerbs
{
	FText VerbTip(const FSurfVerb& Verb)
	{
		if (Verb.Shortcut == ESurfAction::None)
		{
			return Verb.Tip;
		}

		// An action nobody has bound says nothing rather than "(unbound)":
		// a tooltip is not the place to be told a key is missing.
		const FString Key = SurfKeymap::Describe(Verb.Shortcut);
		if (Key.IsEmpty() || Key.Equals(TEXT("unbound")))
		{
			return Verb.Tip;
		}

		return FText::Format(NSLOCTEXT("SurfacingProPalette", "VerbTipWithKey", "({0}) - {1}"),
			FText::FromString(Key), Verb.Tip);
	}

	TArray<FSurfVerb> BuildPool(TWeakObjectPtr<USurfEdMode> Mode, TWeakPtr<SWidget> Host)
	{
		return BuildPoolVerbs(Mode, Host);
	}
}

namespace SurfSidebar
{
	/** Narrower than this, the sidebar is its tabs alone (S193b). */
	constexpr int32 CollapsedBelowPx = 60;
}

namespace
{
	/**
	 * Which verbs have a key, said in one place (S129a).
	 *
	 * By name, because a name is what the pool is keyed on everywhere else -
	 * the arrangement, the search and the layout all find a verb by its
	 * label. The risk in that is a second verb spelled the same way taking a
	 * key that is not its own: **Box** was the box brush *and* box select
	 * until S156b, when the brush went under Add; Box is box select alone
	 * now and carries B, and a test says so - if a second Box ever comes
	 * back, it goes red rather than quietly telling him the wrong thing.
	 */
	void TagShortcuts(TArray<FSurfVerb>& Verbs)
	{
		struct FPair { const TCHAR* Label; ESurfAction Action; };
		static const FPair Table[] = {
			{ TEXT("Extrude"),      ESurfAction::Extrude },
			{ TEXT("Box"),          ESurfAction::BoxSelect },
			{ TEXT("Weld"),         ESurfAction::Weld },
			{ TEXT("Bevel"),        ESurfAction::Bevel },
			{ TEXT("Delete"),       ESurfAction::Delete },
			{ TEXT("Cut"),          ESurfAction::Slice },
			{ TEXT("Duplicate"),    ESurfAction::Duplicate },
			{ TEXT("Separate"),     ESurfAction::Separate },
			{ TEXT("Face Outward"), ESurfAction::FaceOutward },
			{ TEXT("Invert"),       ESurfAction::InvertSelection },
			{ TEXT("Grow"),         ESurfAction::GrowSelection },
			{ TEXT("Shrink"),       ESurfAction::ShrinkSelection },
			{ TEXT("Linked"),       ESurfAction::SelectLinked },
			{ TEXT("Group"),        ESurfAction::GroupPieces },
			{ TEXT("Ungroup"),      ESurfAction::UngroupPieces },
			{ TEXT("Side Panel"),   ESurfAction::ToggleSidePanel },
			{ TEXT("Lasso"),        ESurfAction::LassoSelect },
			{ TEXT("Isolate"),      ESurfAction::IsolateSelection },
			{ TEXT("Hide"),         ESurfAction::HideSelected },
			{ TEXT("Unhide"),       ESurfAction::UnhideAll },
			{ TEXT("Wires"),        ESurfAction::CycleWireView },
			{ TEXT("Overlay"),      ESurfAction::ToggleOverlay },
			{ TEXT("Vertices"),     ESurfAction::SubModeVertex },
			{ TEXT("Edges"),        ESurfAction::SubModeEdge },
			{ TEXT("Faces"),        ESurfAction::SubModeFace },
			{ TEXT("Pieces"),       ESurfAction::SubModeObject },
			{ TEXT("Snap"),         ESurfAction::ToggleSnap },
			{ TEXT("Soft"),         ESurfAction::ToggleSoftSelection },
			{ TEXT("Paint"),        ESurfAction::PaintWeight },
			{ TEXT("Sheet"),        ESurfAction::ConvertToSheet },
			{ TEXT("Open"),         ESurfAction::PrefabIn },
			{ TEXT("Enter"),        ESurfAction::PrefabIn },
			{ TEXT("Close"),        ESurfAction::PrefabOut },
			{ TEXT("Exit"),         ESurfAction::PrefabOut },
		};

		for (FSurfVerb& Verb : Verbs)
		{
			const FString Label = Verb.Label.ToString();
			for (const FPair& Pair : Table)
			{
				if (Label.Equals(Pair.Label, ESearchCase::IgnoreCase))
				{
					Verb.Shortcut = Pair.Action;
					break;
				}
			}
		}
	}

	TArray<FSurfVerb> BuildPoolVerbs(TWeakObjectPtr<USurfEdMode> M, TWeakPtr<SWidget> Host)
	{
		TArray<FSurfVerb> Quick;

	// The blocks to cut, first on the bar (S67c): a box and a cylinder, in
	// front of the camera on the grid, their top face selected so the next
	// press - a patch primitive, a slice, a material - has something to act on.
	/**
	 * One verb for a new piece (S154a): a single action makes a new
	 * Primitive - Box, Cylinder or any other kind - and a Patch can be
	 * spawned through it too. The kind is remembered; the click
	 * makes the last kind, the parameters choose it and size it.
	 */
	{
		// 0 box, 1 cylinder; then the curved ones made from a box - 3 cone,
		// 4 tube, 5 bevel, 6 endcap - "primitive include all other prims!
		// cone etc.!" With a box picked they replace it; with nothing
		// picked a box is made first and becomes the shape. The patch (2)
		// left for the Patch verb (S170a): "exclude any Patch creation
		// from it so that's fully under Create > Patch" - a kind of 2 in
		// an old ini reads as the box.
		auto Kind = []() { const USurfSettings* S = GetDefault<USurfSettings>(); const int32 K = S ? FMath::Clamp(S->NewPrimitiveKind, 0, 6) : 0; return K == 2 ? 0 : K; };
		// Just the word (S172a): "Primitive button only show Primitive, not
		// the type" - the kind is in its frame.
		Quick.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("QPrimitive", "Primitive"),
			LOCTEXT("QPrimitiveTip", "A new primitive in front of you, on the grid - the kind chosen last - with the frame for kind and size. Curved ones fill the picked box."),
			ESurfVerbGroup::Create,
			[M, Kind]()
			{
				USurfInspector* P = Panel(M);
				if (!P) { return; }
				const int32 K = Kind();
				if (K >= 3 && M.IsValid() && !M->GetActiveFace().IsValid())
				{
					P->NewBox();
				}
				switch (K)
				{
				case 1: P->NewCylinder(); break;
				case 3: P->MakeCone(); break;
				case 4: P->MakeCylinder(); break;
				case 5: P->MakeBevel(); break;
				case 6: P->MakeEndcap(); break;
				default: P->NewBox(); break;
				}
				// The frame opens with the piece (S162a): "expecting Add
				// button to open the popup param frame instead of right
				// click!" - the kind and the size are what a hand wants
				// next, and a click in the viewport puts it away.
				if (M.IsValid())
				{
					M->ShowVerbOptions(TEXT("Primitive"));
				}
			}),
			[]() { return LOCTEXT("QPrimitive", "Primitive"); }),
			[Kind, M, Host](FMenuBuilder& Menu)
			{
				auto Pick = [](int32 Which)
				{
					return FUIAction(
						FExecuteAction::CreateLambda([Which]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewPrimitiveKind = Which; S->SaveConfig(); } }),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && FMath::Clamp(S->NewPrimitiveKind, 0, 6) == Which; }));
				};
				Menu.AddMenuEntry(LOCTEXT("OptPrimBox", "Box"), LOCTEXT("OptPrimBoxTip", "A box, its top side selected."), FSlateIcon(), Pick(0), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddMenuEntry(LOCTEXT("OptPrimCylinder", "Cylinder"), LOCTEXT("OptPrimCylinderTip", "A cylinder standing up, its top selected."), FSlateIcon(), Pick(1), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddSeparator();
				Menu.AddMenuEntry(LOCTEXT("OptPrimCone", "Cone"), LOCTEXT("OptPrimConeTip", "A cone patch filling the picked box; with nothing picked, a box is made first."), FSlateIcon(), Pick(3), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddMenuEntry(LOCTEXT("OptPrimTube", "Tube"), LOCTEXT("OptPrimTubeTip", "A curved cylinder patch filling the picked box; with nothing picked, a box is made first."), FSlateIcon(), Pick(4), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddMenuEntry(LOCTEXT("OptPrimBevel", "Bevel"), LOCTEXT("OptPrimBevelTip", "A bevel patch filling the picked box; with nothing picked, a box is made first."), FSlateIcon(), Pick(5), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddMenuEntry(LOCTEXT("OptPrimEndcap", "Endcap"), LOCTEXT("OptPrimEndcapTip", "An endcap patch filling the picked box; with nothing picked, a box is made first."), FSlateIcon(), Pick(6), NAME_None, EUserInterfaceActionType::RadioButton);
				Menu.AddSeparator();
				Menu.AddWidget(Spin(
					TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->NewBrushSizeCm : 200.0; }),
					10.0, 100000.0,
					[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewBrushSizeCm = V; S->SaveConfig(); } },
					[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewBrushSizeCm = 200.0; S->SaveConfig(); } }),
					LOCTEXT("OptPrimSize", "Box / cylinder size (cm)"));
				Menu.AddWidget(SpinInt(
					TAttribute<int32>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->NewCylinderSides : 8; }),
					3, 64,
					[](int32 V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewCylinderSides = V; S->SaveConfig(); } },
					[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewCylinderSides = 8; S->SaveConfig(); } }),
					LOCTEXT("OptPrimSides", "Cylinder sides"));
			}));
	}
	/**
	 * Patch, a verb of its own (S167a): "Patch from selection is its own
	 * button, else it's too confusing. Clicked alone it simply spawns a
	 * patch; clicked on a selection it converts it."
	 */
	Quick.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("QPatch", "Patch"),
		LOCTEXT("QPatchTip", "With nothing picked: a new patch in front of you, on the grid. With primitive faces picked: a patch on each, wearing its material."),
		ESurfVerbGroup::Create,
		[M]()
		{
			// Remembered for the frame: from faces, there is no size to set.
			SurfPatchFromFaces() = M.IsValid() && (M->GetSelection().Num() > 0 || M->GetActiveFace().IsValid());
			if (USurfInspector* P = Panel(M)) { P->MakeSheet(); }
			if (M.IsValid()) { M->ShowVerbOptions(TEXT("Patch")); }
		}),
		[M]() -> FText
		{
			const bool bFaces = M.IsValid() && (M->GetSelection().Num() > 0 || M->GetActiveFace().IsValid());
			return bFaces ? LOCTEXT("QPatchFromFaces", "Patch: from faces") : LOCTEXT("QPatch", "Patch");
		}),
		// Everything that makes a patch lives here (S170a): its size, and
		// the one in place of a primitive.
		[M](FMenuBuilder& Menu)
		{
			// The size is a new patch's only (S172a): "patch from selection
			// shouldnt allow user to change size" - the faces decide it.
			const bool bFromFaces = SurfPatchFromFaces() || (M.IsValid() && (M->GetSelection().Num() > 0 || M->GetActiveFace().IsValid()));
			if (!bFromFaces)
			{
				Menu.AddWidget(Spin(
					TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->NewPatchSizeCm : 1000.0; }),
					100.0, 100000.0,
					[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewPatchSizeCm = V; S->SaveConfig(); } },
					[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->NewPatchSizeCm = 1000.0; S->SaveConfig(); } }),
					LOCTEXT("OptPatchSize", "Patch size (cm)"));
				Menu.AddSeparator();
			}
			Menu.AddMenuEntry(LOCTEXT("PatchReplace", "In place of the primitive"), LOCTEXT("PatchReplaceTip", "The whole selected primitive replaced by one flat patch over its box."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->ReplaceWithSheet(); } }),
					FCanExecuteAction::CreateLambda([M]() { return M.IsValid() && (M->GetSelection().Num() > 0 || M->GetActiveFace().IsValid()); })));
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QExtrude", "Extrude"), LOCTEXT("QExtrudeTip", "Grow past the selected edge - or, with a side of a primitive picked, a new piece standing on that side (S142a)."),
		ESurfVerbGroup::Geometry,
		[M]()
		{
			USurfInspector* P = Panel(M);
			if (!P || !M.IsValid())
			{
				return;
			}
			// A brush side grows a brush; points grow the patch.
			if (M->GetPointSelection().Num() == 0 && !M->GetSelectedPatch().IsValid() && M->GetSelection().Num() > 0)
			{
				P->ExtrudeBrushFace();
				return;
			}
			P->Extrude();
		},
		[M]() { return M.IsValid() && (M->GetPointSelection().Num() > 0 || M->GetSelection().Num() > 0); }),
		[](FMenuBuilder& Menu)
		{
			// One toggle (S185b): "I wanted to extrude into a new patch rather
			// than continuity and didn't find it... too complex naming" - "1
			// toggle, not 2". The rest keep their defaults, in the settings.
			AddToggle(Menu, LOCTEXT("OptExtrudeNewPatch", "New patch"),
				LOCTEXT("OptExtrudeNewPatchTip", "Off: the patch grows past the edge. On: a new patch starts at the edge, joined to it."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bExtrudeNewPatch; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bExtrudeNewPatch = V; S->SaveConfig(); } });
			// The faces beneath a lid are the knob row's "beneath" (S160d) -
			// it writes the default too; the three radios that said it again
			// are gone: "cant read this frame fully".
		}));

	Quick.Add(MakeVerb(LOCTEXT("QLattice", "Lattice"), LOCTEXT("QLatticeTip", "A cage of points round the selected patches: click its points, G R S bend the patch smoothly. Escape leaves, Delete removes it."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->MakeLattice(); } },
		[M]() { return M.IsValid() && (M->IsLatticeEditing() || M->GetSelectedPatch().IsValid() || M->GetPointSelection().Num() > 0); }));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QSubdivide", "Subdivide"), LOCTEXT("QSubdivideTip", "A line between every pair of lines, on the selected patches. Right-click: rows, columns or both."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Subdivide(); } }, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			// Rows and columns are the frame's ticks.
			if (!GSurfOptionsInFrame)
			{
			AddToggle(Menu, LOCTEXT("OptSubRows", "Rows"), LOCTEXT("OptSubRowsTip", "A row between every pair of rows."),
				[M]() { const USurfInspector* P = Panel(M); return P && P->bSubdivideRows; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bSubdivideRows = V; } });
			AddToggle(Menu, LOCTEXT("OptSubColumns", "Columns"), LOCTEXT("OptSubColumnsTip", "A column between every pair of columns."),
				[M]() { const USurfInspector* P = Panel(M); return P && P->bSubdivideColumns; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bSubdivideColumns = V; } });
			Menu.AddSeparator();
			}
			AddToggle(Menu, LOCTEXT("OptAutoSub", "Fill a moved line's cells"),
				LOCTEXT("OptAutoSubTip", "When a whole row or column is moved, the cells it stretched are cut until none is over the density below: 0 lets a cell run to four times the mean, 0.5 holds the mean, 1 cuts to a quarter. Off, a move leaves the grid alone."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bAutoSubdivideOnMove; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bAutoSubdivideOnMove = V; S->SaveConfig(); } });
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->AutoSubdivideDensity : 0.5; }),
				0.0, 1.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->AutoSubdivideDensity = V; S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->AutoSubdivideDensity = 0.5; S->SaveConfig(); } }),
				LOCTEXT("OptAutoSubDensity", "Density (0 loose, 1 tight)"));
		}));

	// The brush cut, on the bar (S67b): one button arms it and, with the
	// points down, cuts - Enter does the same; the right-click says which
	// side stays. The button reads what it will do next.
	/**
	 * Cut is Split and Slice in one word (S156c, "parameters, not
	 * variants"): with a line of points picked it splits the patch along
	 * it; otherwise it arms the plane cut on brushes.
	 */
	Quick.Add(WithLiveLabel(WithOptions(MakeVerb(LOCTEXT("QSlice", "Cut"), LOCTEXT("QSliceTip", "A line picked: the patch cut in two along it. Otherwise a plane through primitives: click two or three points, then Enter."),
		ESurfVerbGroup::Geometry, [M]()
		{
			if (!M.IsValid()) { return; }
			if (!M->IsSlicing() && M->GetPointSelection().Num() > 1)
			{
				if (USurfInspector* P = Panel(M)) { P->Split(); }
				return;
			}
			M->PressSlice();
		}),
		[](FMenuBuilder& Menu)
		{
			auto Keep = [](ESurfSliceKeep Which)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([Which]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->SliceKeep = Which; S->SaveConfig(); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->SliceKeep == Which; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptSliceBoth", "Keep both halves"), LOCTEXT("OptSliceBothTip", "The cut face selected on each."), FSlateIcon(), Keep(ESurfSliceKeep::Both), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptSliceFront", "Keep the front only"), LOCTEXT("OptSliceFrontTip", "The side the cut's normal points to. Shift+Enter does this once."), FSlateIcon(), Keep(ESurfSliceKeep::Front), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptSliceBack", "Keep the back only"), LOCTEXT("OptSliceBackTip", "The other side. Ctrl+Enter does this once."), FSlateIcon(), Keep(ESurfSliceKeep::Back), NAME_None, EUserInterfaceActionType::RadioButton);
		}),
		[M]()
		{
			if (!M.IsValid() || !M->IsSlicing()) { return LOCTEXT("QSlice", "Cut"); }
			FPlane Cut;
			return M->GetSlicePlane(Cut) ? LOCTEXT("QSliceCut", "Cut: now") : LOCTEXT("QSliceArmed", "Cut: click");
		}));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QDup", "Duplicate"), LOCTEXT("QDupTip", "Copy the selected patches."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Duplicate(); } }, NeedsPatch(M)),
		[](FMenuBuilder& Menu)
		{
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->DuplicateOffsetCm : 0.0; }),
				0.0, 1000.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->DuplicateOffsetCm = V; S->SaveConfig(); } }),
				LOCTEXT("OptDupOffset", "Offset along the normal (cm)"));
		}));

	/**
	 * Flip and Face Outward are Blender's pair, and they are not the same
	 * verb (S126a): "fix redundance action flip and action flip normals ...
	 * shift n not sure wht its supposed to do ? same action as flip ?"
	 *
	 * There were two entries calling the same function, one called Flip and
	 * one called Flip Normals, which is the redundancy. One of them is gone.
	 * What is left is a pair that does two different things, and each tip now
	 * says which is which rather than leaving him to find out:
	 *
	 *   Flip          turns whatever is selected, every time.
	 *   Face Outward  turns only the ones facing the wrong way, and decides
	 *                 which those are. Pressing it twice does nothing.
	 */
	Quick.Add(MakeVerb(LOCTEXT("QFlip", "Flip"), LOCTEXT("QFlipTip", "The selected patches turned inside out, every one, every time. To turn only the wrong ones: Face Outward."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->FlipNormals(); } }, NeedsPatch(M)));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QWeld", "Weld"), LOCTEXT("QWeldTip", "Collapse the selected points onto their centre."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Weld(); } }, NeedsPoints(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddMenuEntry(LOCTEXT("OptWeldFirst", "Weld onto the first picked"), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { M->WeldSelectedPoints(false); } })));
			Menu.AddMenuEntry(LOCTEXT("OptWeldDistance", "Weld by distance"), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->WeldByDistance(); } })));
			// After a weld by distance the frame's "reach cm" is this number.
			if (!GSurfOptionsInFrame || !M.IsValid() || (M->LastOperation.Kind != ESurfOpKind::WeldByDistance && M->LastOperation.Kind != ESurfOpKind::WeldClusters))
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([M]() { USurfInspector* P = Panel(M); return P ? P->WeldToleranceCm : 1.0; }),
				0.01, 500.0,
				[M](double V) { if (USurfInspector* P = Panel(M)) { P->WeldToleranceCm = V; } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->WeldToleranceCm = GetDefault<USurfInspector>()->WeldToleranceCm; } }),
				LOCTEXT("OptWeldTolerance", "Distance (cm)"));
			AddToggle(Menu, LOCTEXT("OptWeldUVs", "Weld keeps the UVs"),
				LOCTEXT("OptWeldUVsTip", "The texture pinches with the surface; the mapping is frozen to do it."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bWeldKeepsUVs; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bWeldKeepsUVs = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptWeldClusters", "Each cluster welded on its own"),
				LOCTEXT("OptWeldClustersTip", "The selected points within the distance above of each other go to their own centre - six corners in three pairs make three welds, not one."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bWeldEachCluster; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bWeldEachCluster = V; S->SaveConfig(); } });
		}));

	Quick.Add(MakeVerb(LOCTEXT("QSeparate", "Separate"), LOCTEXT("QSeparateTip", "Cut the picked block of faces out; the rest comes apart into patches."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Separate(); } }, NeedsFaces(M)));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QBridge", "Bridge"), LOCTEXT("QBridgeTip", "A patch between two selected edges with the same number of points; two loops make a tube. Right-click: joining."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Bridge(); } }, NeedsPoints(M)),
		[M](FMenuBuilder& Menu)
		{
			AddToggle(Menu, LOCTEXT("OptBridgeJoin", "Join into one patch when the edges are whole borders"),
				LOCTEXT("OptBridgeJoinTip", "On: the bridge and the patches it meets point for point become one patch. Off: the bridge stays a patch of its own, overlapping the edges."),
				[M]() { USurfInspector* P = Panel(M); return P && P->bBridgeJoins; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bBridgeJoins = V; } });
		}));
	Quick.Add(MakeVerb(LOCTEXT("QFaceOutward", "Face Outward"), LOCTEXT("QFaceOutwardTip", "Every selected patch turned to face away from the selection's middle; the ones already facing out are left alone."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->FaceOutward(); } }, NeedsPatch(M)));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QSteps", "Steps"), LOCTEXT("QStepsTip", "A staircase between two selected edges, as one patch. The count or the step height is in the frame; the edges need equal points."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Steps(); } }, NeedsPoints(M)),
		[](FMenuBuilder& Menu)
		{
			// The count and the riser are the frame's knobs.
			if (GSurfOptionsInFrame)
			{
				return;
			}
			// Its parameters (S158b): "steps... its options had nothing to set".
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->StepCount : 8; }),
				1, 64,
				[](int32 V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->StepCount = V; S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->StepCount = 8; S->SaveConfig(); } }),
				LOCTEXT("OptStepCount", "Steps"));
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->StepRiseCm : 0.0; }),
				0.0, 1000.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->StepRiseCm = V; S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->StepRiseCm = 0.0; S->SaveConfig(); } }),
				LOCTEXT("OptStepRise", "Riser height (cm), 0 for the count"));
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QContinue", "Continue"), LOCTEXT("QContinueTip", "The selected border meets the one across from it and the surface carries on over the seam, no crease, easing over the soft radius."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Continue(); } }, NeedsPoints(M)),
		[](FMenuBuilder& Menu)
		{
			AddToggle(Menu, LOCTEXT("OptContinueOffer", "Offer it while moving a border"),
				LOCTEXT("OptContinueOfferTip", "A whole border moved within reach of another patch's border shows that border as a ghost; confirming the move takes it - the seam meets and the rows behind lean into it. Escape drops both."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bContinueOffers; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bContinueOffers = V; S->SaveConfig(); } });
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QJoin", "Join"), LOCTEXT("QJoinTip", "Make one patch of the selected patches, wherever two share an edge point for point."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Join(); } }, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([M]() { USurfInspector* P = Panel(M); return P ? P->JoinToleranceCm : 0.01; }),
				0.0001, 100.0,
				[M](double V) { if (USurfInspector* P = Panel(M)) { P->JoinToleranceCm = V; } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->JoinToleranceCm = GetDefault<USurfInspector>()->JoinToleranceCm; } }),
				LOCTEXT("OptJoinReach", "Edges count as shared within (cm)"));
		}));
	Quick.Add(MakeVerb(LOCTEXT("QDelete", "Delete"), LOCTEXT("QDeleteTip", "The patch, the picked faces, or the selected rows and columns."),
		ESurfVerbGroup::Geometry, [M]() { if (M.IsValid()) { M->DeleteSelection(); } }, NeedsPatch(M)));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QSolidify", "Solidify"), LOCTEXT("QSolidifyTip", "Give the patch, or the picked faces, thickness."),
		ESurfVerbGroup::Geometry, [M]() { if (USurfInspector* P = Panel(M)) { P->Solidify(); } }, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			// Thickness, which way, the walls and the evenness are all the frame's knobs.
			if (GSurfOptionsInFrame)
			{
				return;
			}
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([M]() { USurfInspector* P = Panel(M); return P ? P->ThicknessCm : 20.0; }),
				0.1, 2000.0,
				[M](double V) { if (USurfInspector* P = Panel(M)) { P->ThicknessCm = V; } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->ThicknessCm = GetDefault<USurfInspector>()->ThicknessCm; } }),
				LOCTEXT("OptThickness", "Thickness (cm)"));
			auto Side = [M](int32 Which)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([M, Which]() { if (USurfInspector* P = Panel(M)) { P->SolidifySide = Which; } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([M, Which]() { const USurfInspector* P = Panel(M); return P && P->SolidifySide == Which; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptSideIn", "Grow behind the surface"), LOCTEXT("OptSideInTip", "The front stays where it is."), FSlateIcon(), Side(-1), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptSideMid", "Grow half each way"), FText::GetEmpty(), FSlateIcon(), Side(0), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptSideOut", "Grow in front of the surface"), LOCTEXT("OptSideOutTip", "The back stays where it is."), FSlateIcon(), Side(1), NAME_None, EUserInterfaceActionType::RadioButton);
			AddToggle(Menu, LOCTEXT("OptRim", "Rim (the four skirts)"), LOCTEXT("OptRimTip", "Off: front and back only."),
				[M]() { const USurfInspector* P = Panel(M); return P && P->bSolidifyRim; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bSolidifyRim = V; } });
			AddToggle(Menu, LOCTEXT("OptEven", "Even thickness at folds"), LOCTEXT("OptEvenTip", "Keep the shell as thick at a fold as on a flat run."),
				[M]() { const USurfInspector* P = Panel(M); return P && P->bSolidifyEven; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bSolidifyEven = V; } });
		}));

	// The shape operators run the moment they are pressed; their numbers are
	// then in the adjust section at the bottom of the bar, live.
	Quick.Add(MakeVerb(LOCTEXT("QSmooth", "Smooth"), LOCTEXT("QSmoothTip", "Relax the selection. Adjust strength and passes below afterwards."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Smooth(); } }, NeedsPatch(M)));
	/**
	 * Random (S157a, A22): "need a random noise action. on a selection it'd
	 * popup param some randomness params - random offset (in an input %)
	 * (on input axis etc.) minimal efficient."
	 */
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QRandom", "Random"), LOCTEXT("QRandomTip", "Every selected point pushed a random way, up to the frame's distance, on the ticked axes or its normal. Shift+R rolls again."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Random(); } }, NeedsPatch(M)),
		[](FMenuBuilder& Menu)
		{
			// The distance is the frame's "cm" knob.
			if (!GSurfOptionsInFrame)
			{
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->RandomOffsetCm : 10.0; }),
				0.1, 10000.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->RandomOffsetCm = V; S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->RandomOffsetCm = 10.0; S->SaveConfig(); } }),
				LOCTEXT("OptRandomCm", "Up to (cm)"));
			}
			AddToggle(Menu, LOCTEXT("OptRandomX", "X"), LOCTEXT("OptRandomXTip", "Push along X."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bRandomX; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bRandomX = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptRandomY", "Y"), LOCTEXT("OptRandomYTip", "Push along Y."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bRandomY; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bRandomY = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptRandomZ", "Z"), LOCTEXT("OptRandomZTip", "Push along Z."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bRandomZ; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bRandomZ = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptRandomNormal", "Along the normal instead"),
				LOCTEXT("OptRandomNormalTip", "Each point in or out along its own normal, the axes ignored."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bRandomAlongNormal; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bRandomAlongNormal = V; S->SaveConfig(); } });
		}));
	Quick.Add(MakeVerb(LOCTEXT("QInflate", "Inflate"), LOCTEXT("QInflateTip", "Push the selection along the normal. Adjust the distance below afterwards."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Inflate(); } }, NeedsPatch(M)));
	// Smoothing (S171a): one verb for patches and primitives alike, and a
	// crease on picked lines as the override.
	// Smoothing (S184c): "make smoothing instead of shading, and the angle
	// param on right click" - a click goes smooth, again hard; the angle
	// lives in the right-click.
	// Normals (S199a): "shouldnt mention hard or soft. just Smoothing. perhaps
	// rename to Normals" - the word stays put, the state is in the tip.
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QShading", "Normals"), LOCTEXT("QShadingTip", "Smooth or hard normals on the picked pieces: a click smooths, again hardens. Right-click: by angle, and the angle."),
		ESurfVerbGroup::Shape, [M]()
		{
			USurfSettings* S = GetMutableDefault<USurfSettings>();
			if (!S || !M.IsValid())
			{
				return;
			}
			// A click flips smooth and hard; run again by the frame (its
			// options changed) it keeps what was picked (S206a): the flip
			// on every replay was the frame "cycling between options".
			if (!M->bReplayingOptions)
			{
				S->ShadingKind = S->ShadingKind == 0 ? 1 : 0;
				S->SaveConfig();
			}
			M->SetShadingOnSelection(S->ShadingKind, S->ShadingAngleDeg);
		}),
		[](FMenuBuilder& Menu)
		{
			auto Pick = [](int32 Which)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([Which]()
					{
						if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->ShadingKind = Which; S->SaveConfig(); }
						// Picked from the right-click, applied at once.
						if (USurfEdMode* Mode = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro)))
						{
							const USurfSettings* Now = GetDefault<USurfSettings>();
							Mode->SetShadingOnSelection(Which, Now ? Now->ShadingAngleDeg : 60.0);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->ShadingKind == Which; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptShadeSmooth", "Smooth"), LOCTEXT("OptShadeSmoothTip", "One normal wherever sides or cells meet."), FSlateIcon(), Pick(0), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptShadeHard", "Hard"), LOCTEXT("OptShadeHardTip", "Every side and cell flat."), FSlateIcon(), Pick(1), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptShadeAngle", "Smooth by angle"), LOCTEXT("OptShadeAngleTip", "Smooth where the turn is under the angle, hard past it."), FSlateIcon(), Pick(2), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->ShadingAngleDeg : 60.0; }),
				1.0, 179.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->ShadingAngleDeg = FMath::Clamp(V, 1.0, 179.0); S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->ShadingAngleDeg = 60.0; S->SaveConfig(); } }),
				LOCTEXT("OptShadeAngleDeg", "Angle (°)"));
		}));
	Quick.Add(MakeVerb(LOCTEXT("QCrease", "Crease"), LOCTEXT("QCreaseTip", "The picked whole lines shade hard, whatever the patch's shading; again on hard lines makes them smooth."),
		ESurfVerbGroup::Shape, [M]() { if (M.IsValid()) { M->ToggleCreaseOnSelectedLines(); } }, NeedsPatch(M)));
	// Ticks, not buttons (S167b): any of X, Y, Z, together; none is the plane.
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QFlatten", "Flatten"), LOCTEXT("QFlattenTip", "Pull the selection onto its plane - or level it along the axes ticked in the options, any of them together."),
		ESurfVerbGroup::Shape, [M]()
		{
			USurfInspector* P = Panel(M);
			const USurfSettings* S = GetDefault<USurfSettings>();
			if (!P) { return; }
			if (S && (S->bFlattenX || S->bFlattenY || S->bFlattenZ))
			{
				if (S->bFlattenX) { P->FlattenAlong(0); }
				if (S->bFlattenY) { P->FlattenAlong(1); }
				if (S->bFlattenZ) { P->FlattenAlong(2); }
			}
			else
			{
				P->Flatten();
			}
		}, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			AddToggle(Menu, LOCTEXT("OptFlattenX", "Level along X"), LOCTEXT("OptFlattenXTip", "Every selected point to the same X."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bFlattenX; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bFlattenX = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptFlattenY", "Level along Y"), LOCTEXT("OptFlattenYTip", "Every selected point to the same Y."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bFlattenY; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bFlattenY = V; S->SaveConfig(); } });
			AddToggle(Menu, LOCTEXT("OptFlattenZ", "Level along Z"), LOCTEXT("OptFlattenZTip", "Every selected point to the same height. None ticked: onto the selection's own plane."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bFlattenZ; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bFlattenZ = V; S->SaveConfig(); } });
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QBevel", "Bevel"), LOCTEXT("QBevelTip", "Round the crease along the selected lines. Adjust width and segments in the frame afterwards."),
		ESurfVerbGroup::Shape, [M]() { if (USurfInspector* P = Panel(M)) { P->Bevel(); } }, NeedsPoints(M)),
		[M](FMenuBuilder& Menu)
		{
			AddToggle(Menu, LOCTEXT("OptBevelEats", "Eat the next line when wider"),
				LOCTEXT("OptBevelEatsTip", "A bevel wider than the gap to the next line removes the lines it reaches and rounds over them. Off: the width stops at the next line."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bBevelEatsLines; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bBevelEatsLines = V; S->SaveConfig(); } });
			// The frame has it as its own tick.
			if (!GSurfOptionsInFrame)
			{
			AddToggle(Menu, LOCTEXT("OptBevelHard", "Keep the crease, only subdivide"),
				LOCTEXT("OptBevelHardTip", "The crease stays sharp; the lines are added either side of it on the surface - support lines, no rounding."),
				[M]() { const USurfInspector* P = Panel(M); return P && P->bBevelKeepsCrease; },
				[M](bool V) { if (USurfInspector* P = Panel(M)) { P->bBevelKeepsCrease = V; } });
			}
		}));

	// Surfacing, on the bar at last (S61): the four mappings, each over every
	// patch the selection holds, each adjustable in the frame afterwards -
	// metres per tile and the turn are knobs, not a trip to the panel.
	auto SurfaceOptions = [M](FMenuBuilder& Menu)
	{
		Menu.AddWidget(Spin(
			TAttribute<double>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->Scale.X : 1.0; }),
			0.01, 100.0,
			[M](double V) { if (USurfInspector* P = Panel(M)) { P->Scale.X = V; } },
			[M]() { if (USurfInspector* P = Panel(M)) { P->Scale.X = 1.0; } }),
			LOCTEXT("OptTileU", "Metres a tile, across"));
		Menu.AddWidget(Spin(
			TAttribute<double>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->Scale.Y : 1.0; }),
			0.01, 100.0,
			[M](double V) { if (USurfInspector* P = Panel(M)) { P->Scale.Y = V; } },
			[M]() { if (USurfInspector* P = Panel(M)) { P->Scale.Y = 1.0; } }),
			LOCTEXT("OptTileV", "Metres a tile, down"));
		Menu.AddWidget(Spin(
			TAttribute<double>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->RotationDegrees : 0.0; }),
			-360.0, 360.0,
			[M](double V) { if (USurfInspector* P = Panel(M)) { P->RotationDegrees = V; } },
			[M]() { if (USurfInspector* P = Panel(M)) { P->RotationDegrees = 0.0; } }),
			LOCTEXT("OptTurn", "Turn (°)"));
		Menu.AddMenuEntry(LOCTEXT("OptQuarter", "Turn a quarter"), LOCTEXT("OptQuarterTip", "Ninety degrees about the selection's pivot - the fix for a mapping that came out sideways, without the mirror a swap brings. Ctrl+R in the UV window."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { M->TurnSelectedUVsQuarter(); } })));
		Menu.AddMenuEntry(LOCTEXT("OptSwap", "Swap the two axes"), LOCTEXT("OptSwapTip", "Exchange U and V - a mirror as well as a turn. Turn a quarter is usually what is wanted."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->SwapPatchUV(); } })));
		Menu.AddMenuEntry(LOCTEXT("OptFlipU", "Flip across"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->FlipPatchU(); } })));
		Menu.AddMenuEntry(LOCTEXT("OptFlipV", "Flip down"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->FlipPatchV(); } })));
		Menu.AddMenuEntry(LOCTEXT("OptInherit", "Take the last clicked face's mapping"), LOCTEXT("OptInheritTip", "Click the wall, then the patch, then this: the seam disappears."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (USurfInspector* P = Panel(M)) { P->InheritFaceSurface(); } })));
	};

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QFlow", "Flow"), LOCTEXT("QFlowTip", "The texture follows the surface: a metre of surface to a metre of texture wherever it bends. Right-click: metres a tile, the turn, flips."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Natural(); } }, NeedsSurface(M)),
		SurfaceOptions));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QEven", "Even"), LOCTEXT("QEvenTip", "Each cell of the patch its own size in the texture, one unit for the whole patch - for trims and tiles laid by the cell."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Even(); } }, NeedsSurface(M)),
		SurfaceOptions));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QAlign", "Align"), LOCTEXT("QAlignTip", "Projected from each surface's own direction - patches and primitive faces alike. Right-click: metres a tile, the turn, flips."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Planar(); } }, NeedsSurface(M)),
		SurfaceOptions));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QProject", "Project"), LOCTEXT("QProjectTip", "One projection for every patch and primitive face selected, so a bent wall has no seam. Right-click: which way to project."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Project(); } }, NeedsSurface(M)),
		[M, SurfaceOptions](FMenuBuilder& Menu)
		{
			auto Axis = [M](int32 Which)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([M, Which]() { if (USurfInspector* P = Panel(M)) { P->ProjectAxis = Which; P->Project(); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([M, Which]() { const USurfInspector* P = Panel(M); return P && P->ProjectAxis == Which; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptAxisAuto", "The selection's own direction"), LOCTEXT("OptAxisAutoTip", "The average of the patches' normals, to its nearest axis."), FSlateIcon(),
				Axis(0), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptAxisX", "Along X"), FText::GetEmpty(), FSlateIcon(),
				Axis(1), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptAxisY", "Along Y"), FText::GetEmpty(), FSlateIcon(),
				Axis(2), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptAxisZ", "Along Z"), FText::GetEmpty(), FSlateIcon(),
				Axis(3), NAME_None, EUserInterfaceActionType::RadioButton);
			// From the camera (S230a: "project from camera option? (add to Project?)").
			Menu.AddMenuEntry(LOCTEXT("OptAxisCamera", "From the camera"), LOCTEXT("OptAxisCameraTip", "Projected the way the viewport looks, as it looks now - the texture flat to the screen. Run again from another view for another projection."), FSlateIcon(),
				Axis(4), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuSeparator();
			SurfaceOptions(Menu);
		}));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QFit", "Fit"), LOCTEXT("QFitTip", "The scale that spans a whole number of tiles across each patch or primitive face. Right-click: how many."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->FitPatch(); } }, NeedsSurface(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->FitRepeatU : 1; }),
				1, 64,
				[M](int32 V) { if (USurfInspector* P = Panel(M)) { P->FitRepeatU = V; } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->FitRepeatU = 1; } }),
				LOCTEXT("OptRepeatU", "Tiles across"));
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->FitRepeatV : 1; }),
				1, 64,
				[M](int32 V) { if (USurfInspector* P = Panel(M)) { P->FitRepeatV = V; } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->FitRepeatV = 1; } }),
				LOCTEXT("OptRepeatV", "Tiles down"));
		}));

	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QDensity", "Density"), LOCTEXT("QDensityTip", "Every selected patch and primitive face to the same texels a metre - the number on the bar, the active one's. Right-click: texture size."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->NormaliseDensity(); } }, NeedsSurface(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([M]() { const USurfInspector* P = Panel(M); return P ? P->TextureTexels.X : 1024.0; }),
				1.0, 8192.0,
				[M](double V) { if (USurfInspector* P = Panel(M)) { P->TextureTexels = FVector2D(V, V); } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->TextureTexels = FVector2D(1024.0, 1024.0); } }),
				LOCTEXT("OptTexels", "Texture size (texels)"));
		}));

	Quick.Add(MakeVerb(LOCTEXT("QFreeze", "Freeze"), LOCTEXT("QFreezeTip", "Hold the UVs where they are and stop deriving them - the mapping stops following the points."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->Freeze(); } }, NeedsPatch(M)));

	Quick.Add(WithLiveLabel(WithOptions(MakeVerb(LOCTEXT("QLayer", "Layer"), LOCTEXT("QLayerTip", "The selected patches to the next layer, 0 to 3 and round: 0 the opaque surface, 1 to 3 drawn over it in turn. Right-click: set it."),
		ESurfVerbGroup::Surface, [M]() { if (USurfInspector* P = Panel(M)) { P->CycleLayer(); } }, NeedsPatch(M)),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([M]() { const USurfInspector* P = Panel(M); const int32 L = P ? P->GetActiveLayer() : 0; return L < 0 ? 0 : L; }),
				0, 3,
				[M](int32 V) { if (USurfInspector* P = Panel(M)) { P->SetLayer(V); } },
				[M]() { if (USurfInspector* P = Panel(M)) { P->SetLayer(0); } }),
				LOCTEXT("OptLayer", "Layer (0 for the opaque surface)"));
		}),
		[M]()
		{
			const USurfInspector* P = Panel(M);
			const int32 L = P ? P->GetActiveLayer() : -1;
			return L < 0 ? LOCTEXT("QLayer", "Layer") : FText::FromString(FString::Printf(TEXT("Layer %d"), L));
		}));

	/**
	 * The adjustments (S238a: "remove any Mask option. Instead we'll just
	 * set from cavity like blender's mask does with similar slider (live
	 * view)(add, sub, replace, etc.) a smooth button too. a contrast
	 * button. level button (similar to photoshop's popup param)"): five
	 * verbs on the tab's channels of the picked points and meshes, each
	 * with a frame whose knobs run it again live.
	 */
	auto Knob = [](FMenuBuilder& Menu, const FText& Label, double Min, double Max, double Reset, TFunction<double(const USurfSettings&)> Get, TFunction<void(USurfSettings&, double)> Set)
	{
		Menu.AddWidget(Spin(
			TAttribute<double>::CreateLambda([Get]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? Get(*S) : 0.0; }),
			Min, Max,
			[Set, Min, Max](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { Set(*S, FMath::Clamp(V, Min, Max)); S->SaveConfig(); } },
			[Set, Reset]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { Set(*S, Reset); S->SaveConfig(); } }),
			Label);
	};
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QFromCavity", "From Cavity"), LOCTEXT("QFromCavityTip", "Dirt into the Paint tab's channels: none on the flat, full in crevices, across patches that meet. Its frame: blend, amount, level, invert."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->AdjustPaint(USurfSettings::PaintAdjustCavity); } }, NeedsPatch(M)),
		[Knob](FMenuBuilder& Menu)
		{
			static const TCHAR* Blends[6] = { TEXT("Replace"), TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Lighten"), TEXT("Darken") };
			for (int32 I = 0; I < 6; ++I)
			{
				Menu.AddMenuEntry(FText::FromString(Blends[I]), LOCTEXT("OptCavityBlendTip", "How the cavity meets what the channel holds."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([I]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->MaskBlend = static_cast<ESurfMaskBlend>(I); S->SaveConfig(); } }),
						FCanExecuteAction(), FIsActionChecked::CreateLambda([I]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && static_cast<int32>(S->MaskBlend) == I; })),
					NAME_None, EUserInterfaceActionType::RadioButton);
			}
			Knob(Menu, LOCTEXT("OptCavityFactor", "Amount"), 0.0, 1.0, 1.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintCavityFactor); }, [](USurfSettings& S, double V) { S.PaintCavityFactor = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptCavitySmooth", "Smooth"), 0.0, 1.0, 0.3, [](const USurfSettings& S) { return static_cast<double>(S.CavitySmooth); }, [](USurfSettings& S, double V) { S.CavitySmooth = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptCavityLevel", "Level"), 0.25, 16.0, 4.0, [](const USurfSettings& S) { return static_cast<double>(S.CavityLevel); }, [](USurfSettings& S, double V) { S.CavityLevel = static_cast<float>(V); });
			AddToggle(Menu, LOCTEXT("OptCavityInvert", "Invert"), LOCTEXT("OptCavityInvertTip", "Ridges instead of dents."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bCavityInvert; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bCavityInvert = V; S->SaveConfig(); } });
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QSmoothPaint", "Smooth Paint"), LOCTEXT("QSmoothPaintTip", "The Paint tab's channels of the picked points and meshes eased towards their neighbours. Its frame: how far, how many passes."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->AdjustPaint(USurfSettings::PaintAdjustSmooth); } }, NeedsPatch(M)),
		[Knob](FMenuBuilder& Menu)
		{
			Knob(Menu, LOCTEXT("OptSmoothAmount", "Amount"), 0.0, 1.0, 1.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintSmoothAmount); }, [](USurfSettings& S, double V) { S.PaintSmoothAmount = static_cast<float>(V); });
			Menu.AddWidget(SpinInt(
				TAttribute<int32>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->PaintSmoothTimes : 1; }),
				1, 20,
				[](int32 V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->PaintSmoothTimes = FMath::Clamp(V, 1, 20); S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->PaintSmoothTimes = 1; S->SaveConfig(); } }),
				LOCTEXT("OptSmoothTimes", "Passes"));
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QContrast", "Contrast"), LOCTEXT("QContrastTip", "The Paint tab's channels of the picked points and meshes pushed from the middle or pulled to it. Its frame: the amount."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->AdjustPaint(USurfSettings::PaintAdjustContrast); } }, NeedsPatch(M)),
		[Knob](FMenuBuilder& Menu)
		{
			Knob(Menu, LOCTEXT("OptContrast", "Contrast"), -1.0, 4.0, 0.5, [](const USurfSettings& S) { return static_cast<double>(S.PaintContrast); }, [](USurfSettings& S, double V) { S.PaintContrast = static_cast<float>(V); });
		}));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QLevels", "Levels"), LOCTEXT("QLevelsTip", "The Paint tab's channels of the picked points and meshes remapped: black and white in, the gamma, black and white out."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->AdjustPaint(USurfSettings::PaintAdjustLevels); } }, NeedsPatch(M)),
		[Knob](FMenuBuilder& Menu)
		{
			Knob(Menu, LOCTEXT("OptLevelsInLow", "In black"), 0.0, 1.0, 0.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintLevelsInLow); }, [](USurfSettings& S, double V) { S.PaintLevelsInLow = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptLevelsInHigh", "In white"), 0.0, 1.0, 1.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintLevelsInHigh); }, [](USurfSettings& S, double V) { S.PaintLevelsInHigh = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptLevelsGamma", "Gamma"), 0.1, 10.0, 1.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintLevelsGamma); }, [](USurfSettings& S, double V) { S.PaintLevelsGamma = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptLevelsOutLow", "Out black"), 0.0, 1.0, 0.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintLevelsOutLow); }, [](USurfSettings& S, double V) { S.PaintLevelsOutLow = static_cast<float>(V); });
			Knob(Menu, LOCTEXT("OptLevelsOutHigh", "Out white"), 0.0, 1.0, 1.0, [](const USurfSettings& S) { return static_cast<double>(S.PaintLevelsOutHigh); }, [](USurfSettings& S, double V) { S.PaintLevelsOutHigh = static_cast<float>(V); });
		}));
	Quick.Add(MakeVerb(LOCTEXT("QInvertPaint", "Invert Paint"), LOCTEXT("QInvertPaintTip", "The Paint tab's channels of the picked points and meshes turned over: full becomes empty, empty full."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->AdjustPaint(USurfSettings::PaintAdjustInvert); } }, NeedsPatch(M)));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QPaint", "Paint"), LOCTEXT("QPaintTip", "Paint the vertex colour on the Paint tab's channels: drag paints up, Ctrl-drag down, Ctrl+wheel sizes. Right-click: channel, strength."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->TogglePaint(); } }),
		[M](FMenuBuilder& Menu)
		{
			// A tick a channel (S248a: "instead of 'Col' just have R G B all
			// three enabled"): R, G and B ticked together paint the colour on
			// the Paint tab; the last tick stays.
			auto Channel = [M](int32 Bit)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([M, Bit]()
					{
						if (USurfSettings* S = GetMutableDefault<USurfSettings>())
						{
							const int32 Bits = S->PaintSet().Bits ^ Bit;
							S->SetPaintSet(Bits != 0 ? Bits : Bit);
							S->SaveConfig();
						}
						if (M.IsValid()) { M->RefreshPaintView(); }
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Bit]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && (S->PaintSet().Bits & Bit) != 0; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptPaintR", "Red channel"), LOCTEXT("OptPaintRTip", "What a material's vertex colour R reads. R, G and B ticked together paint the colour."), FSlateIcon(),
				Channel(1), NAME_None, EUserInterfaceActionType::ToggleButton);
			Menu.AddMenuEntry(LOCTEXT("OptPaintG", "Green channel"), FText::GetEmpty(), FSlateIcon(),
				Channel(2), NAME_None, EUserInterfaceActionType::ToggleButton);
			Menu.AddMenuEntry(LOCTEXT("OptPaintB", "Blue channel"), FText::GetEmpty(), FSlateIcon(),
				Channel(4), NAME_None, EUserInterfaceActionType::ToggleButton);
			Menu.AddMenuEntry(LOCTEXT("OptPaintA", "Alpha channel"), LOCTEXT("OptPaintATip", "The vertex alpha, for a material that blends by it."), FSlateIcon(),
				Channel(8), NAME_None, EUserInterfaceActionType::ToggleButton);
			Menu.AddSeparator();
			AddToggle(Menu, LOCTEXT("OptPaintView", "Paint View while the brush is out"),
				LOCTEXT("OptPaintViewTip", "Every surface dark where the channel is empty, lit where it is full, for as long as the brush is out. Off by default; Paint View itself stays on with the brush away."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bPaintViewOn; },
				[M](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bPaintViewOn = V; S->SaveConfig(); } if (M.IsValid()) { M->RefreshPaintView(); } });
			Menu.AddWidget(Spin(
				TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->PaintStrength : 1.0; }),
				0.05, 1.0,
				[](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->PaintStrength = FMath::Clamp(V, 0.05, 1.0); S->SaveConfig(); } },
				[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->PaintStrength = 1.0; S->SaveConfig(); } }),
				LOCTEXT("OptPaintStrength", "Strength"));
			Menu.AddMenuEntry(LOCTEXT("OptPaintWindow", "The Paint tab (N)"), LOCTEXT("OptPaintWindowTip", "Colours, size, Fill and Clear, and the adjustments - From Cavity, Smooth, Contrast, Levels, Invert."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { SurfPaintTab::Open(); })));
		}));

	// The windows are View's (S247f: "looks redundant between surface and
	// view content" - they were Surface verbs and View › Windows rows both);
	// View › Windows is their one listing, so the View rows skip them.
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QUVs", "UVs"), LOCTEXT("QUVsTip", "The UVs window: the selected nets over the tile grid, live. Wheel zooms, middle-drag pans, F frames, G R S move, turn and size."),
		ESurfVerbGroup::View, []() { SurfUVTab::Open(); })));
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QMaterials", "Material Library"), LOCTEXT("QMaterialsTip", "The project's materials with a search; click one to put it on the selection, make instances from its menu."),
		ESurfVerbGroup::View, []() { SurfMaterialsTab::Open(); })));
	// The two the Material Library opens from its own bar, as buttons of
	// their own (S169a): "mat manager and texture manager need buttons too".
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QManager", "Material Manager"), LOCTEXT("QManagerTip", "The selected material on the calibration cube, its parameters beside it to edit and save."),
		ESurfVerbGroup::View, []() { SurfMaterialManagerTab::Open(); })));
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QTextures", "Textures"), LOCTEXT("QTexturesTip", "The Textures window: any folder on this machine on the left, the library on the right; bring images in and put them on the selection."),
		ESurfVerbGroup::View, []() { SurfTexturesTab::Open(); })));
	/**
	 * Every window and the interface a verb (S177a): the menu line lost
	 * Windows and Interface - "user shall still be able to add quickfav or
	 * sidebar of any of that" - so each is a button to star, to put on the
	 * bar, or to find with X.
	 */
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QActionsWindow", "Actions"), LOCTEXT("QActionsWindowTip", "The Actions window: every action by family, to run, star or add to the sidebar. Dock it where you like."),
		ESurfVerbGroup::View, []() { SurfActionsTab::Open(); })));
	// The Assets tab (S210a): prefabs and static meshes as tiles, to place.
	Quick.Add(AsWindow(MakeVerb(LOCTEXT("QAssets", "Assets"), LOCTEXT("QAssetsTip", "The SidePanel's Assets tab: prefabs and meshes as tiles - search, double-click or drag to place, right-click for more."),
		ESurfVerbGroup::View, []() { SurfSidePanel::Show(ESurfSideTab::Assets); })));
	Quick.Add(MakeVerb(LOCTEXT("QFlags", "Flags"), LOCTEXT("QFlagsTip", "What the bake does with the picked pieces - Detail, No collision, No shadow, No draw - in the SidePanel."),
		ESurfVerbGroup::Surface, []() { SurfSidePanel::Show(ESurfSideTab::Flags); }));
	Quick.Add(AsWindow(WithOptions(MakeVerb(LOCTEXT("QSidePanel", "Side Panel"), LOCTEXT("QSidePanelTip", "The SidePanel down the viewport's edge, open or closed: Item, Paint, Flags, View, Assets, Materials. Right-click: a tab."),
		ESurfVerbGroup::View, []() { SurfSidePanel::Toggle(); }),
		[](FMenuBuilder& Menu)
		{
			for (int32 I = 0; I < static_cast<int32>(ESurfSideTab::Count); ++I)
			{
				const ESurfSideTab Tab = static_cast<ESurfSideTab>(I);
				Menu.AddMenuEntry(SurfSidePanel::TabName(Tab), FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Tab]() { SurfSidePanel::Show(Tab); }),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([Tab]() { return SurfSidePanel::IsOpen() && SurfSidePanel::IsShown(Tab); })),
					NAME_None, EUserInterfaceActionType::RadioButton);
			}
		})));
	// The Notes window is the record's (S247a, redundancy W4): there only
	// while sessions are recorded.
	if (const USurfSettings* Recording = GetDefault<USurfSettings>(); Recording && Recording->bRecordSessions)
	{
		Quick.Add(AsWindow(MakeVerb(LOCTEXT("QNotes", "Notes"), LOCTEXT("QNotesTip", "Every line the tool has said this session, in order."),
			ESurfVerbGroup::View, []() { SurfNotesTab::Open(); })));
	}
	Quick.Add(MakeVerb(LOCTEXT("QSettings", "Settings"), LOCTEXT("QSettingsTip", "Every setting, on the editor's own preferences page (Editor Preferences, Plugins)."),
		ESurfVerbGroup::View, []() { SurfSettingsWindow::Open(); }));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QEditUI", "Edit UI"), LOCTEXT("QEditUITip", "Arrange the buttons: click one to send it to the other surface, drag it onto another to swap; the rest is under the right-click."),
		ESurfVerbGroup::View, []() { SurfVerbLayout::SetEditing(!SurfVerbLayout::IsEditing()); }),
		[M](FMenuBuilder& Menu) { SurfVerbLayout::AppendInterfaceMenu(Menu, M.Get(), /*bWithEditUI*/ false); }));
	// "rename pivot to set object origin after selection" (S183a).
	Quick.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("QPivot", "Origin"), LOCTEXT("QPivotTip", "The origin to the selection: a vertex, the centre of several, a patch or a primitive. Rotation and scale go about it."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SetPivotFromSelection(); } },
		[M]() { return M.IsValid() && M->CanSetPivot(); }),
		[M]() { return M.IsValid() && M->HasPivot() ? LOCTEXT("QPivotSet", "Origin: set") : LOCTEXT("QPivot", "Origin"); }),
		[M](FMenuBuilder& Menu)
		{
			Menu.AddMenuEntry(LOCTEXT("OptPivotCentre", "Origin back to the centre"), LOCTEXT("OptPivotCentreTip", "Rotation and scale go about the selection's centre again."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { M->ClearPivot(); } }),
					FCanExecuteAction::CreateLambda([M]() { return M.IsValid() && M->HasPivot(); })));
		}));
	Quick.Add(MakeVerb(LOCTEXT("QAll", "All"), LOCTEXT("QAllTip", "Select every control point."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SelectAllPoints(); } }, NeedsPatch(M)));
	{
	}
	// A verb of its own since Primitive lost its options (S170a): the
	// named prompt that "Add › Prefab from the selection..." used to open.
	Quick.Add(MakeVerb(LOCTEXT("QPrefab", "Prefab"), LOCTEXT("QPrefabTip", "What is picked - patches, primitives, meshes, lights, anything - becomes a prefab, named by you, one instance standing where it was."),
		ESurfVerbGroup::Create, [M, Host]() { if (TSharedPtr<SWidget> W = Host.Pin()) { SurfNamePrompt::OpenPrefabName(W.ToSharedRef(), M); } },
		[M]() { return M.IsValid() && M->CanMakePrefab(); }));
	Quick.Add(MakeVerb(LOCTEXT("QScene", "Level"), LOCTEXT("QSceneTip", "The selection becomes a level of its own (LI_) where it stood - lights, meshes, our geometry, prefabs, anything; Alt+PgDn edits it."),
		ESurfVerbGroup::Create, [M, Host]() { if (TSharedPtr<SWidget> W = Host.Pin()) { SurfNamePrompt::OpenSceneName(W.ToSharedRef(), M); } },
		[M]() { return M.IsValid() && M->CanMakeScene(); }));
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QEnter", "Open"), LOCTEXT("QEnterTip", "Edit the selected prefab's asset (or double-click it), alone or where it stands - right-click to choose. Close writes it back."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->PrefabIn(); } }),
		[M](FMenuBuilder& Menu)
		{
			auto Where = [M](bool bInPlace)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([bInPlace]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bEditPrefabsInPlace = bInPlace; S->SaveConfig(); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bInPlace]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bEditPrefabsInPlace == bInPlace; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptEnterAlone", "Alone, at the origin"), LOCTEXT("OptEnterAloneTip", "The default. The prefab in its own space, nothing else in view; the camera comes to it and goes back on Close."), FSlateIcon(), Where(false), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptEnterInPlace", "Where it stands"), LOCTEXT("OptEnterInPlaceTip", "The prefab edited in the level, at its instance, everything else ghosted round it - to fit a piece against what it sits in."), FSlateIcon(), Where(true), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddSeparator();
			// Add to Prefab was a verb (S156c): it is what Open lets you do.
			Menu.AddMenuEntry(LOCTEXT("OptTakeIn", "Add the selection to the open prefab"), LOCTEXT("OptTakeInTip", "The selected meshes, blueprints, lights - anything that is not ours - become part of the prefab being edited, where they stand. Every instance carries them from then on."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { M->TakeInSelection(); } }),
					FCanExecuteAction::CreateLambda([M]() { return M.IsValid() && M->CanTakeIn(); })));
		}));
	{
		Quick.Add(WithLiveLabel(WithOptions(MakeVerb(LOCTEXT("QExit", "Close"), LOCTEXT("QExitTip", "Leave the prefab being edited, keeping and saving it - every instance follows. Right-click: drop the edits, or Save as."),
			ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->PrefabOut(); } }, [M]() { return M.IsValid() && (M->IsInsidePrefab() || M->IsInsideScene()); }),
			[M, Host](FMenuBuilder& Menu)
			{
				Menu.AddMenuEntry(LOCTEXT("OptExitDrop", "Close dropping the edits"), LOCTEXT("OptExitDropTip", "The prefab as it was on entering, placements included."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { if (M->IsInsideScene()) { M->ExitScene(false); } else { M->ExitPrefab(false); } } })));
				Menu.AddMenuEntry(LOCTEXT("OptExitSaveAs", "Save as..."), LOCTEXT("OptExitSaveAsTip", "What is on screen saved as a copy under a new name; this instance uses the copy, the others keep the original. The room stays open on the copy."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([M, Host]() { if (TSharedPtr<SWidget> W = Host.Pin()) { SurfNamePrompt::OpenSaveAsName(W.ToSharedRef(), M); } })));
			}),
			[M]() { return M.IsValid() && (M->IsInsidePrefab() || M->IsInsideScene()) ? LOCTEXT("QExitLive", "Close, keep") : LOCTEXT("QExit", "Close"); }));
	}
	Quick.Add(MakeVerb(LOCTEXT("QUnpack", "Unpack"), LOCTEXT("QUnpackTip", "The selected instance back into the level as plain geometry, where it stood; the asset stays in the library."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->UnpackSelectedPrefab(); } }, [M]() { return M.IsValid() && M->HasSelectedInstance(); }));
	/**
	 * The bake is the game's copy (S153a): the editor draws the preview
	 * and never the bake; Game View shows it; an edit drops it; and unless the
	 * level's row says otherwise it happens by itself before Play and
	 * before a save (S154a).
	 */
	Quick.Add(WithOptions(WithLiveLabel(MakeVerb(LOCTEXT("QBake", "Bake"), LOCTEXT("QBakeTip", "The level's geometry and every prefab to Nanite static meshes with collision. Game view shows them; an edit drops them."),
		ESurfVerbGroup::Create, [M]() { if (M.IsValid()) { M->BakeLevel(); } }),
		[M]() { return M.IsValid() && M->IsBaking() ? LOCTEXT("QBakeStop", "Stop Bake") : LOCTEXT("QBake", "Bake"); }),
		[M](FMenuBuilder& Menu)
		{
			// The level's own tick is in the Flags tab (S247a). No Unbake
			// (redundancy X1): an edit takes a bake back on its own.
			AddToggle(Menu, LOCTEXT("OptBakeNanite", "Nanite meshes"),
				LOCTEXT("OptBakeNaniteTip", "Baked meshes are Nanite, with the fallback kept whole for collision. Off: plain static meshes."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bBakeNanite; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bBakeNanite = V; S->SaveConfig(); } });
		}));
	Quick.Add(MakeVerb(LOCTEXT("QSame", "Same"), LOCTEXT("QSameTip", "Every patch and primitive face in the level wearing what the active one wears joins the selection. Adds, never clears."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SelectByMaterial(); } }, NeedsSurface(M)));
	Quick.Add(MakeVerb(LOCTEXT("QReplace", "Replace"), LOCTEXT("QReplaceTip", "Pick a face wearing the material to replace, then one wearing the new, press: everything wearing the first takes the second."),
		ESurfVerbGroup::Surface, [M]() { if (M.IsValid()) { M->ReplaceMaterialFromSelection(); } }, NeedsSurface(M)));
	/**
	 * The four flags a piece can wear, for the bake (S151a): "where do we
	 * flag that?" - nowhere, until now. The click says what the picked
	 * pieces wear; the right-click puts each on or takes it off. They change
	 * nothing in the preview: they are what the bake does with the piece.
	 */
	/**
	 * Flags opens its window (S152d): four ticks that read every picked
	 * piece and write all of them at once, in a tab that stays up while the
	 * next piece is picked. One way in - the ticks menu it also had was the
	 * same thing twice.
	 */
	Quick.Add(MakeVerb(LOCTEXT("QGrow", "Grow"),
		LOCTEXT("QGrowTip", "One step outward along the grid: every neighbour of a picked point, or every face beside a picked face (Ctrl + numpad +)."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->GrowSelection(true); } },
		[M]() { return M.IsValid() && (M->GetPointSelection().Num() > 0 || M->GetQuadSelection().Num() > 0); }));
	Quick.Add(MakeVerb(LOCTEXT("QShrink", "Shrink"),
		LOCTEXT("QShrinkTip", "One step inward: every picked point with an unpicked neighbour lets go, or every face at the edge of the picked faces (Ctrl + numpad -)."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->GrowSelection(false); } },
		[M]() { return M.IsValid() && (M->GetPointSelection().Num() > 0 || M->GetQuadSelection().Num() > 0); }));
	// The floor and the clay (S172b), as buttons for the bar or Q.
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QGrid", "Grid"), LOCTEXT("QGridTip", "The floor grid at zero and the axes through the origin, on or off."),
		ESurfVerbGroup::View, [M]()
		{
			// The editor's own Grid show flag (S180a).
			if (M.IsValid()) { M->SetGridShown(!M->IsGridShown()); }
		}),
		[M]() { return M.IsValid() && M->IsGridShown() ? LOCTEXT("QGridOn", "Grid: on") : LOCTEXT("QGridOff", "Grid: off"); }));
	// The top line's switches as verbs too (S199a): "i need be able right
	// clic anything ... so anything can be exposed somewhere sidebar etc.
	// quickmenu" - each can go on the sidebar or in Q, and its chip's
	// right-click says so.
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QSnap", "Snap"), LOCTEXT("QSnapTip", "Snapping on or off, whatever it lands on. The arrow beside the snap chip has what it lands on."),
		ESurfVerbGroup::Select, []()
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bSnapEnabled = !S->bSnapEnabled; S->SaveConfig(); }
		}),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bSnapEnabled ? LOCTEXT("QSnapOn", "Snap: on") : LOCTEXT("QSnapOff", "Snap: off"); }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QSoft", "Soft"), LOCTEXT("QSoftTip", "Proportional editing on or off: unselected points follow, by distance."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->ToggleSoftSelection(); } }),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bSoftSelection ? LOCTEXT("QSoftOn", "Soft: on") : LOCTEXT("QSoftOff", "Soft: off"); }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QGame", "Game"), LOCTEXT("QGameTip", "The level as the game sees it: the bake in place of the previews, no editor lines. Again for the editor's view."),
		ESurfVerbGroup::View, []()
		{
			if (FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
			{
				Client->SetGameView(!Client->IsInGameView());
				Client->Invalidate();
			}
		}),
		[]() { return GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsInGameView() ? LOCTEXT("QGameOn", "Game: on") : LOCTEXT("QGameOff", "Game: off"); }));
	// Statistics (S207a): a quiet line of counts in the viewport's corner.
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QStats", "Statistics"), LOCTEXT("QStatsTip", "A small line in the viewport's corner: patches, primitives, points, triangles and materials - of what is picked, else of the whole level."),
		ESurfVerbGroup::View, []()
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				S->bShowStats = !S->bShowStats;
				S->SaveConfig();
			}
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports();
			}
		}),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bShowStats ? LOCTEXT("QStatsOn", "Statistics: on") : LOCTEXT("QStatsOff", "Statistics: off"); }));
	// One Overlay (S247a, redundancy X2): our wires, points, marks and names
	// off or on; in Game view, drawn over the game until it is entered again.
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QOverlay", "Overlay"), LOCTEXT("QOverlayTip", "Our wires, points, marks and names off or on, to see the surfaces alone. In Game view: drawn over the game until Game view is entered again."),
		ESurfVerbGroup::View, [M]()
		{
			if (!M.IsValid()) { return; }
			if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsInGameView()) { M->ToggleOursInGameView(); }
			else { M->ToggleOverlay(); }
		}),
		[M]()
		{
			if (!M.IsValid()) { return LOCTEXT("QOverlayOn", "Overlay: on"); }
			if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsInGameView())
			{
				return M->bOursInGameView ? LOCTEXT("QOverlayOn", "Overlay: on") : LOCTEXT("QOverlayOff", "Overlay: off");
			}
			return M->IsOverlayHidden() ? LOCTEXT("QOverlayOff", "Overlay: off") : LOCTEXT("QOverlayOn", "Overlay: on");
		}));
	// The rest of the top strip as verbs (S204a): "Have you addressed it so
	// I can right click anything and q-menu/sidebar it?"
	Quick.Add(MakeVerb(LOCTEXT("QAuto", "Auto"), LOCTEXT("QAutoTip", "Edit, the click deciding: a vertex, else an edge, else a face."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { if (M->AutoAppliesNow()) { M->SetAutoKind(false); } else { M->EnterEditKinds(); } } }));
	Quick.Add(MakeVerb(LOCTEXT("QVertices", "Vertices"), LOCTEXT("QVerticesTip", "Edit vertices only; again to let the click decide."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->ToggleKindLock(ESurfSubMode::Vertex); } }));
	Quick.Add(MakeVerb(LOCTEXT("QEdges", "Edges"), LOCTEXT("QEdgesTip", "Edit edges only; again to let the click decide."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->ToggleKindLock(ESurfSubMode::Edge); } }));
	Quick.Add(MakeVerb(LOCTEXT("QFaces", "Faces"), LOCTEXT("QFacesTip", "Edit faces only; again to let the click decide."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->ToggleKindLock(ESurfSubMode::Face); } }));
	Quick.Add(MakeVerb(LOCTEXT("QObject", "Pieces"), LOCTEXT("QObjectTip", "Whole pieces: a click takes the whole patch or primitive."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SetSubMode(ESurfSubMode::Object); } }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QMeshes", "Meshes"), LOCTEXT("QMeshesTip", "Snapping lands on the level's static meshes too."),
		ESurfVerbGroup::Select, []() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bSnapToMeshes = !S->bSnapToMeshes; S->SaveConfig(); } }),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bSnapToMeshes ? LOCTEXT("QMeshesOn", "Meshes: on") : LOCTEXT("QMeshesOff", "Meshes: off"); }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QContext", "Context"), LOCTEXT("QContextTip", "Opening a prefab: where it stands with the level ghosted round it, or alone at the origin."),
		ESurfVerbGroup::View, []() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bEditPrefabsInPlace = !S->bEditPrefabsInPlace; S->SaveConfig(); } }),
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bEditPrefabsInPlace ? LOCTEXT("QContextOn", "Context: on") : LOCTEXT("QContextOff", "Context: off"); }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QMatcap", "Matcap"), LOCTEXT("QMatcapTip", "Clay on every patch and primitive - the shape alone - instead of their materials."),
		ESurfVerbGroup::View, [M]()
		{
			if (M.IsValid()) { M->SetViewKind(M->GetViewKind() == 2 ? 1 : 2); }
		}),
		[M]() { return M.IsValid() && M->GetViewKind() == 2 ? LOCTEXT("QMatcapOn", "Matcap: on") : LOCTEXT("QMatcapOff", "Matcap: off"); }));
	Quick.Add(WithLiveLabel(MakeVerb(LOCTEXT("QPaintView", "Paint View"), LOCTEXT("QPaintViewTip", "The paint channel on every surface - dark where empty, the channel's colour where full. A view kind beside Lit, Unlit and Matcap."),
		ESurfVerbGroup::View, [M]()
		{
			if (M.IsValid()) { M->SetViewKind(M->GetViewKind() == 3 ? 1 : 3); }
		}),
		[M]() { return M.IsValid() && M->GetViewKind() == 3 ? LOCTEXT("QPaintViewOn", "Paint View: on") : LOCTEXT("QPaintViewOff", "Paint View: off"); }));
	Quick.Add(MakeVerb(LOCTEXT("QLinked", "Linked"),
		LOCTEXT("QLinkedTip", "Every patch joined to the picked ones through a shared point, and on."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->SelectLinkedPatches(); } },
		[M]() { return M.IsValid() && (M->GetSelectedPatch().IsValid() || M->GetPointSelection().Num() > 0); }));
	Quick.Add(MakeVerb(LOCTEXT("QGroup", "Group"),
		LOCTEXT("QGroupTip", "The picked pieces made one group: a click picks them all, Alt+click one alone. Picked groups go inside it."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->GroupSelection(); } },
		[M]() { return M.IsValid() && M->CountPickedPieces() > 1; }));
	Quick.Add(MakeVerb(LOCTEXT("QUngroup", "Ungroup"),
		LOCTEXT("QUngroupTip", "The picked group undone, one level; a piece picked alone leaves its group."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->UngroupSelection(); } },
		[M]() { return M.IsValid() && !M->DescribePickedGroups().IsEmpty(); }));
	Quick.Add(MakeVerb(LOCTEXT("QEveryOther", "Every Other"),
		LOCTEXT("QEveryOtherTip", "Keep every other point of what is picked - a checker on a grid, one in two along a line."),
		ESurfVerbGroup::Select, [M]() { if (M.IsValid()) { M->ThinSelection(2); } },
		[M]() { return M.IsValid() && M->GetPointSelection().Num() > 1; }));
	{
		// A repair verb (S247a, redundancy X11): the Actions window and the
		// search have it; the family menus do not.
		FSurfVerb Merge = MakeVerb(LOCTEXT("QMergeContainers", "Merge Containers"),
			LOCTEXT("QMergeContainersTip", "Every SurfacingPro container actor in this level folded into one; nothing moves, the emptied actors go."),
			ESurfVerbGroup::Geometry, [M]() { if (M.IsValid()) { M->MergeLevelGroups(); } });
		Merge.bRare = true;
		Quick.Add(Merge);
	}
	Quick.Add(MakeVerb(LOCTEXT("QHide", "Hide"), LOCTEXT("QHideTip", "Hide the patches the selection holds. Shift+H hides the others instead; Alt+H shows everything again."),
		ESurfVerbGroup::View, [M]() { if (M.IsValid()) { M->HidePatches(false); } }));
	Quick.Add(MakeVerb(LOCTEXT("QUnhide", "Unhide"), LOCTEXT("QUnhideTip", "Show every hidden patch again."),
		ESurfVerbGroup::View, [M]() { if (M.IsValid()) { M->UnhideAllPatches(); } }));
	Quick.Add(MakeVerb(LOCTEXT("QIsolate", "Isolate"), LOCTEXT("QIsolateTip", "Only the selection, framed, everything else out of the way - until pressed again (numpad /)."),
		ESurfVerbGroup::View, [M]() { if (M.IsValid()) { M->ToggleIsolate(GCurrentLevelEditingViewportClient); } }));

	// Z walks the same cycle; the button is for the hand that is on the mouse.
	Quick.Add(WithOptions(MakeVerb(LOCTEXT("QWires", "Wires"), LOCTEXT("QWiresTip", "Which patches show their wires: the ones being edited, every patch, or every patch with x-ray selection. Z."),
		ESurfVerbGroup::View, [M]() { if (M.IsValid()) { M->CycleWireView(); } }),
		[M](FMenuBuilder& Menu)
		{
			auto Pick = [](ESurfWireView Which)
			{
				return FUIAction(
					FExecuteAction::CreateLambda([Which]()
					{
						if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->WireView = Which; S->SaveConfig(); }
						if (GEditor) { GEditor->RedrawLevelEditingViewports(); }
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->WireView == Which; }));
			};
			Menu.AddMenuEntry(LOCTEXT("OptWireSelected", "The patches being edited"), FText::GetEmpty(), FSlateIcon(), Pick(ESurfWireView::Selected), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptWireAll", "Every patch"), FText::GetEmpty(), FSlateIcon(), Pick(ESurfWireView::All), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddMenuEntry(LOCTEXT("OptWireXRay", "Every patch, x-ray: select through surfaces"), FText::GetEmpty(), FSlateIcon(), Pick(ESurfWireView::XRay), NAME_None, EUserInterfaceActionType::RadioButton);
			Menu.AddSeparator();
			AddToggle(Menu, LOCTEXT("OptSeeThrough", "Draw wires through surfaces"),
				LOCTEXT("OptSeeThroughTip", "Off: a wire behind a surface is hidden by it, and what can be seen is what can be picked. X-ray always draws through."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bSeeThroughWires; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bSeeThroughWires = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
			AddToggle(Menu, LOCTEXT("OptOverlaps", "Colour points that meet another"),
				LOCTEXT("OptOverlapsTip", "A point with another point at the same place, on any patch, is drawn green."),
				[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bShowOverlaps; },
				[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bShowOverlaps = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
			Menu.AddSeparator();
			AddToggle(Menu, LOCTEXT("OptModellingView", "Modelling view: unlit (Shift+Z)"),
				LOCTEXT("OptModellingViewTip", "The viewport unlit while the mode is up - nothing of the level's lighting is paid for. Off: the shaded view. Shift+Z switches either way."),
				[M]() { return M.IsValid() && M->IsModellingView(); },
				[M](bool) { if (M.IsValid()) { M->ToggleShadedView(); } });
		}));



		// Then everything that only ever lived on the docked panel, once
		// each: the pool is the union, and nothing in it is spelled twice.
		for (const FSurfVerb& Verb : BuildAllVerbs(M))
		{
			const FString Label = Verb.Label.ToString();
			if (!Quick.ContainsByPredicate([&Label](const FSurfVerb& Each) { return Each.Label.ToString().Equals(Label, ESearchCase::IgnoreCase); }))
			{
				Quick.Add(Verb);
			}
		}
		TagUVFamily(Quick);
		TagShortcuts(Quick);
		return Quick;
	}

}

/**
 * Where each verb lives, and how it is moved (S74a). A verb is on the
 * bar or in the window, never on neither: what the user has arranged is
 * saved as two ordered lists of ids, and anything never arranged - a
 * verb added to the tool later - takes its default place.
 */
namespace SurfVerbLayout
{
	int32 GSerial = 1;
	int32 Serial() { return GSerial; }
	void Touch() { ++GSerial; }

	FString IdOf(const FSurfVerb& Verb) { return Verb.Label.ToString(); }

	TArray<FString> PoolIds(const TArray<FSurfVerb>& Pool)
	{
		TArray<FString> Ids;
		Ids.Reserve(Pool.Num());
		for (const FSurfVerb& Verb : Pool)
		{
			Ids.Add(IdOf(Verb));
		}
		return Ids;
	}

	bool Same(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::IgnoreCase); }

	bool Listed(const TArray<FString>& List, const FString& Id)
	{
		return List.ContainsByPredicate([&Id](const FString& Each) { return Same(Each, Id); });
	}

	/** The ones a hand reaches for all day. Everything else starts in the window. */
	bool DefaultsToBar(const FString& Id)
	{
		// Twelve, from ten days of his records (S156c): what a hand reaches
		// for all day. The rest is in the menus, a drag away. Ten since
		// S162a: "open and close can be removed since they do the same as
		// the drill up / down arrows" - the arrows at the head of the bar
		// run the same two verbs.
		// Five since S166a: "by default, sidebar is pretty empty, shelf too,
		// and user may add anything they like there" - the bar and the shelf
		// are favourites places; the SurfacingPro window holds every verb.
		static const TArray<FString> Bar = {
			TEXT("Primitive"), TEXT("Patch"), TEXT("Extrude"), TEXT("Cut"), TEXT("Weld"), TEXT("Delete")
		};
		return Listed(Bar, Id);
	}

	bool GShelfOpen = false;
	bool IsShelfOpen() { return GShelfOpen; }
	void SetShelfOpen(bool bOpen)
	{
		if (GShelfOpen != bOpen)
		{
			GShelfOpen = bOpen;
			// The bar refills on the serial (S157h): without this the click
			// changed a flag nothing looked at - "shelf is broken".
			++GSerial;
			SurfSession::Verb(TEXT("shelf"), bOpen ? TEXT("open") : TEXT("closed"));
		}
	}

	bool IsFavorite(const FString& Id)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Listed(Settings->QuickFavorites, Id);
	}

	void ToggleFavorite(const FString& Id)
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		if (!Settings)
		{
			return;
		}
		const int32 At = Settings->QuickFavorites.IndexOfByPredicate([&Id](const FString& Each) { return Same(Each, Id); });
		if (At == INDEX_NONE)
		{
			Settings->QuickFavorites.Add(Id);
		}
		else
		{
			Settings->QuickFavorites.RemoveAt(At);
		}
		Settings->SaveConfig();
		SurfSession::Verb(TEXT("quick_favorite"), FString::Printf(TEXT("%s %s"), *Id, At == INDEX_NONE ? TEXT("on") : TEXT("off")));
		++GSerial;
	}

	/** Once each (S184c): verbs renamed since - Shading is Smoothing, Pivot is Origin - in every list that remembers them by name. */
	void RenamedVerbsOnce()
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		static const TCHAR* Renames[][2] = { { TEXT("Shading"), TEXT("Smoothing") }, { TEXT("Pivot"), TEXT("Origin") }, { TEXT("Smoothing"), TEXT("Normals") },
			{ TEXT("Manager"), TEXT("Material Manager") }, { TEXT("Materials"), TEXT("Material Library") },
			{ TEXT("Merge Groups"), TEXT("Merge Containers") }, { TEXT("Object"), TEXT("Pieces") } };
		const int32 Count = UE_ARRAY_COUNT(Renames);
		if (!Settings || Settings->VerbRenamesDone >= Count)
		{
			return;
		}
		auto Rename = [Settings](TArray<FString>& List)
		{
			for (FString& Id : List)
			{
				for (int32 I = Settings->VerbRenamesDone; I < UE_ARRAY_COUNT(Renames); ++I)
				{
					if (Same(Id, Renames[I][0]))
					{
						Id = Renames[I][1];
					}
				}
			}
		};
		Rename(Settings->BarVerbs);
		Rename(Settings->WindowVerbs);
		Rename(Settings->HiddenVerbs);
		Rename(Settings->QuickFavorites);
		for (FSurfUiLayout& Layout : Settings->UiLayouts)
		{
			Rename(Layout.Bar);
			Rename(Layout.Window);
		}
		Settings->VerbRenamesDone = Count;
		Settings->SaveConfig();
		++GSerial;
	}

	TArray<FString> OrderedIds(bool bBar, const TArray<FString>& Pool)
	{
		RenamedVerbsOnce();
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const TArray<FString> Mine = Settings ? (bBar ? Settings->BarVerbs : Settings->WindowVerbs) : TArray<FString>();
		const TArray<FString> Theirs = Settings ? (bBar ? Settings->WindowVerbs : Settings->BarVerbs) : TArray<FString>();

		TArray<FString> Out;
		// What was arranged, in the order it was arranged in - and only
		// what still exists, so a verb that has been renamed away does
		// not hold a place.
		for (const FString& Id : Mine)
		{
			if (Listed(Pool, Id) && !Listed(Out, Id))
			{
				Out.Add(Id);
			}
		}
		// Then, on the bar, what ships there and was never arranged away.
		// The shelf holds only what was put on it (S166a): it is a
		// favourites place, not the rest of the pool - the window is.
		if (bBar)
		{
			for (const FString& Id : Pool)
			{
				if (!Listed(Mine, Id) && !Listed(Theirs, Id) && DefaultsToBar(Id)
					&& !(Settings && Listed(Settings->HiddenVerbs, Id)))
				{
					Out.Add(Id);
				}
			}
		}
		return Out;
	}

	TArray<const FSurfVerb*> Ordered(bool bBar, const TArray<FSurfVerb>& Pool)
	{
		TArray<const FSurfVerb*> Out;
		for (const FString& Id : OrderedIds(bBar, PoolIds(Pool)))
		{
			const FSurfVerb* Found = Pool.FindByPredicate([&Id](const FSurfVerb& Verb) { return Same(IdOf(Verb), Id); });
			if (Found)
			{
				Out.Add(Found);
			}
		}
		return Out;
	}

	void Save(TArray<FString> BarIds, TArray<FString> WindowIds)
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->BarVerbs = MoveTemp(BarIds);
			Settings->WindowVerbs = MoveTemp(WindowIds);
			Settings->SaveConfig();
		}
		++GSerial;
	}

	/** Both lists written out as they stand, so a move edits something explicit. */
	void Materialise(const TArray<FString>& Pool, TArray<FString>& OutBar, TArray<FString>& OutWindow)
	{
		OutBar = OrderedIds(/*bBar*/ true, Pool);
		OutWindow = OrderedIds(/*bBar*/ false, Pool);
	}

	bool OnBar(const FString& Id, const TArray<FString>& Pool)
	{
		return Listed(OrderedIds(/*bBar*/ true, Pool), Id);
	}

	bool OnShelf(const FString& Id, const TArray<FString>& Pool)
	{
		return Listed(OrderedIds(/*bBar*/ false, Pool), Id);
	}

	void SendTo(const FString& Id, bool bBar, const TArray<FString>& Pool)
	{
		TArray<FString> BarIds;
		TArray<FString> WindowIds;
		Materialise(Pool, BarIds, WindowIds);
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->HiddenVerbs.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		}
		BarIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		WindowIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		(bBar ? BarIds : WindowIds).Add(Id);
		Save(MoveTemp(BarIds), MoveTemp(WindowIds));
		SurfSession::Verb(TEXT("verb_moved"), FString::Printf(TEXT("%s -> %s"), *Id, bBar ? TEXT("bar") : TEXT("window")));
	}

	/** Off both the bar and the shelf (S166a): the window still holds it. */
	void RemoveFromSurfaces(const FString& Id, const TArray<FString>& Pool)
	{
		TArray<FString> BarIds;
		TArray<FString> WindowIds;
		Materialise(Pool, BarIds, WindowIds);
		BarIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		WindowIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			// A verb that ships on the bar stays off it once taken off.
			if (!Listed(Settings->HiddenVerbs, Id))
			{
				Settings->HiddenVerbs.Add(Id);
			}
		}
		Save(MoveTemp(BarIds), MoveTemp(WindowIds));
		SurfSession::Verb(TEXT("verb_moved"), FString::Printf(TEXT("%s -> window"), *Id));
	}

	void Nudge(const FString& Id, int32 Delta, const TArray<FString>& Pool)
	{
		TArray<FString> BarIds;
		TArray<FString> WindowIds;
		Materialise(Pool, BarIds, WindowIds);
		TArray<FString>& List = Listed(BarIds, Id) ? BarIds : WindowIds;
		const int32 At = List.IndexOfByPredicate([&Id](const FString& Each) { return Same(Each, Id); });
		const int32 To = At + Delta;
		if (At == INDEX_NONE || !List.IsValidIndex(To))
		{
			return;
		}
		List.Swap(At, To);
		Save(MoveTemp(BarIds), MoveTemp(WindowIds));
	}

	void MoveBefore(const FString& Id, const FString& Target, const TArray<FString>& Pool, bool bAfter)
	{
		if (Same(Id, Target))
		{
			return;
		}
		TArray<FString> BarIds;
		TArray<FString> WindowIds;
		Materialise(Pool, BarIds, WindowIds);
		BarIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		WindowIds.RemoveAll([&Id](const FString& Each) { return Same(Each, Id); });
		TArray<FString>& List = Listed(BarIds, Target) ? BarIds : WindowIds;
		const int32 At = List.IndexOfByPredicate([&Target](const FString& Each) { return Same(Each, Target); });
		// Before the target on its left half, after it on its right (S165a).
		List.Insert(Id, At == INDEX_NONE ? List.Num() : (bAfter ? At + 1 : At));
		Save(MoveTemp(BarIds), MoveTemp(WindowIds));
		SurfSession::Verb(TEXT("verb_dragged"), FString::Printf(TEXT("%s %s %s"), *Id, bAfter ? TEXT("after") : TEXT("before"), *Target));
	}

	void Reset()
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->HiddenVerbs.Reset();
		}
		Save(TArray<FString>(), TArray<FString>());
		SurfSession::Verb(TEXT("verb_layout_reset"));
	}

	/**
	 * A button on the move (S153g, A4): "All buttons are moveable
	 * arrangeable as user like" - by drag now, as well as by the click and
	 * the arrows. Dropped on another button it takes that button's place,
	 * on either surface.
	 */
	class FSurfVerbDragOp : public FDragDropOperation
	{
	public:
		DRAG_DROP_OPERATOR_TYPE(FSurfVerbDragOp, FDragDropOperation)

		FString Id;
		TArray<FString> Pool;
		/** Dragged off the sidebar: let go on nothing, it leaves it (S203a). */
		bool bFromSidebar = false;

		static TSharedRef<FSurfVerbDragOp> New(const FString& InId, const TArray<FString>& InPool, bool bInFromSidebar = false)
		{
			TSharedRef<FSurfVerbDragOp> Op = MakeShared<FSurfVerbDragOp>();
			Op->Id = InId;
			Op->Pool = InPool;
			Op->bFromSidebar = bInFromSidebar;
			Op->Construct();
			return Op;
		}

		// "if moving icon in edit ui mode from sidebar to -nowhere- (like
		// nothing) = its like a remove."
		virtual void OnDrop(bool bDropWasHandled, const FPointerEvent& MouseEvent) override
		{
			if (!bDropWasHandled && bFromSidebar && (OnBar(Id, Pool) || OnShelf(Id, Pool)))
			{
				RemoveFromSurfaces(Id, Pool);
			}
			FDragDropOperation::OnDrop(bDropWasHandled, MouseEvent);
		}

		virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Menu.Background"))
				.Padding(FMargin(8.0f, 3.0f))
				[
					SNew(STextBlock).Text(FText::FromString(Id)).Font(SurfTheme::Font(9))
				];
		}
	};

	/**
	 * The veil over a button while the interface is being edited: a press
	 * watches for a drag, a release without one is the click that sends
	 * the button across, a drop puts the dragged button in this one's place.
	 */
	class SSurfVerbVeil : public SBorder
	{
	public:
		SLATE_BEGIN_ARGS(SSurfVerbVeil)
			: _BorderImage(nullptr), _Padding(FMargin(0.0f)) {}
			SLATE_ATTRIBUTE(const FSlateBrush*, BorderImage)
			SLATE_ATTRIBUTE(FMargin, Padding)
			SLATE_ATTRIBUTE(FText, ToolTipText)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const FString& InId, const TArray<FString>& InPool, bool bInBar, bool bInSidebar = true)
		{
			Id = InId;
			Pool = InPool;
			bBar = bInBar;
			bSidebar = bInSidebar;
			SBorder::Construct(SBorder::FArguments()
				.BorderImage(InArgs._BorderImage)
				.Padding(InArgs._Padding)
				.ToolTipText(InArgs._ToolTipText)
				[ SNullWidget::NullWidget ]);
		}

		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				return FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
			}
			// The right button still belongs to the menu below.
			return FReply::Unhandled();
		}

		/**
		 * A click does nothing any more (S165a): it used to send the chip
		 * to the other surface, which is why arranging read as "CHAOS" - a
		 * press that did not become a drag threw the chip across. Drag is
		 * the one gesture; the arrows nudge.
		 */
		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
		{
			return Event.GetEffectingButton() == EKeys::LeftMouseButton ? FReply::Handled() : FReply::Unhandled();
		}

		virtual FReply OnDragDetected(const FGeometry&, const FPointerEvent&) override
		{
			return FReply::Handled().BeginDragDrop(FSurfVerbDragOp::New(Id, Pool, bSidebar));
		}

		virtual void OnDragEnter(const FGeometry&, const FDragDropEvent& Event) override
		{
			bOver = Event.GetOperationAs<FSurfVerbDragOp>().IsValid();
		}

		virtual void OnDragLeave(const FDragDropEvent&) override
		{
			bOver = false;
		}

		virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override
		{
			bAfterHalf = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()).X > Geometry.GetLocalSize().X * 0.5f;
			return FReply::Handled();
		}

		virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override
		{
			bOver = false;
			TSharedPtr<FSurfVerbDragOp> Op = Event.GetOperationAs<FSurfVerbDragOp>();
			if (!Op.IsValid())
			{
				return FReply::Unhandled();
			}
			const bool bAfter = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()).X > Geometry.GetLocalSize().X * 0.5f;
			MoveBefore(Op->Id, Id, Pool, bAfter);
			return FReply::Handled();
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const int32 Layer = SBorder::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
			// A bar where the chip will land: the left or the right edge.
			if (bOver)
			{
				const FVector2D Size = AllottedGeometry.GetLocalSize();
				const float X = bAfterHalf ? Size.X - 2.0f : 0.0f;
				FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1,
					AllottedGeometry.ToPaintGeometry(FVector2D(2.0f, Size.Y), FSlateLayoutTransform(FVector2D(X, 0.0f))),
					FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, SurfTheme::Linear(SurfTheme::Primary()));
			}
			return Layer + 1;
		}

	private:
		FString Id;
		TArray<FString> Pool;
		bool bBar = false;
		bool bSidebar = true;
		bool bOver = false;
		bool bAfterHalf = false;
	};

	/**
	 * A surface as a drop target (S162a): a chip let go anywhere on the bar
	 * or the shelf that is not another chip goes to the end of that surface.
	 */
	class SSurfDropZone : public SBorder
	{
	public:
		SLATE_BEGIN_ARGS(SSurfDropZone) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, TSharedRef<SWidget> Content, bool bInBar, const TArray<FString>& InPool, bool bInRemoves = false)
		{
			bBar = bInBar;
			bRemoves = bInRemoves;
			Pool = InPool;
			SBorder::Construct(SBorder::FArguments()
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this]() { return bOver ? SurfTheme::WithAlpha(SurfTheme::Primary(), 0.15f) : FLinearColor::Transparent; })
				.Padding(FMargin(0.0f, 2.0f))
				[ Content ]);
		}

		virtual void OnDragEnter(const FGeometry&, const FDragDropEvent& Event) override
		{
			bOver = Event.GetOperationAs<FSurfVerbDragOp>().IsValid();
		}

		virtual void OnDragLeave(const FDragDropEvent&) override
		{
			bOver = false;
		}

		virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
		{
			bOver = false;
			TSharedPtr<FSurfVerbDragOp> Op = Event.GetOperationAs<FSurfVerbDragOp>();
			if (!Op.IsValid())
			{
				return FReply::Unhandled();
			}
			if (bRemoves)
			{
				RemoveFromSurfaces(Op->Id, Pool);
			}
			else
			{
				SendTo(Op->Id, bBar, Pool);
			}
			return FReply::Handled();
		}

	private:
		bool bBar = false;
		bool bRemoves = false;
		bool bOver = false;
		TArray<FString> Pool;
	};

	TSharedRef<SWidget> MakeDropZone(TSharedRef<SWidget> Content, bool bBar, const TArray<FString>& Pool)
	{
		return SNew(SSurfDropZone, Content, bBar, Pool);
	}

	TSharedRef<SWidget> MakeRemoveZone(TSharedRef<SWidget> Content, const TArray<FString>& Pool)
	{
		return SNew(SSurfDropZone, Content, /*bBar*/ false, Pool, /*bRemoves*/ true);
	}

	// --- Edit-UI mode, and arrangements kept by name (S82a) ----------------

	bool GEditing = false;

	bool IsEditing() { return GEditing; }

	void SetEditing(bool bOn)
	{
		if (GEditing == bOn)
		{
			return;
		}
		GEditing = bOn;
		// The surfaces watch the serial, so both change shape at once.
		++GSerial;
		SurfSession::Verb(TEXT("ui_edit"), bOn ? TEXT("on") : TEXT("off"));
	}

	TArray<FString> LayoutNames()
	{
		TArray<FString> Names;
		if (const USurfSettings* Settings = GetDefault<USurfSettings>())
		{
			for (const FSurfUiLayout& Each : Settings->UiLayouts)
			{
				if (!Each.Name.IsEmpty())
				{
					Names.Add(Each.Name);
				}
			}
		}
		return Names;
	}

	void SaveLayoutAs(const FString& Name, const TArray<FString>& Pool)
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		if (!Settings || Name.IsEmpty())
		{
			return;
		}
		// Written out in full, both surfaces, so the arrangement brought back
		// later does not depend on what the defaults were that day.
		TArray<FString> BarIds;
		TArray<FString> WindowIds;
		Materialise(Pool, BarIds, WindowIds);

		FSurfUiLayout* Found = Settings->UiLayouts.FindByPredicate(
			[&Name](const FSurfUiLayout& Each) { return Same(Each.Name, Name); });
		if (!Found)
		{
			Found = &Settings->UiLayouts[Settings->UiLayouts.AddDefaulted()];
		}
		Found->Name = Name;
		Found->Bar = MoveTemp(BarIds);
		Found->Window = MoveTemp(WindowIds);
		Settings->SaveConfig();
		++GSerial;
		SurfSession::Verb(TEXT("ui_layout_saved"), Name);
	}

	bool UseLayout(const FString& Name)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const FSurfUiLayout* Found = Settings
			? Settings->UiLayouts.FindByPredicate([&Name](const FSurfUiLayout& Each) { return Same(Each.Name, Name); })
			: nullptr;
		if (!Found)
		{
			return false;
		}
		// Copied before Save writes over the object they live in.
		TArray<FString> BarIds = Found->Bar;
		TArray<FString> WindowIds = Found->Window;
		Save(MoveTemp(BarIds), MoveTemp(WindowIds));
		SurfSession::Verb(TEXT("ui_layout_used"), Name);
		return true;
	}

	void ForgetLayout(const FString& Name)
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->UiLayouts.RemoveAll([&Name](const FSurfUiLayout& Each) { return Same(Each.Name, Name); });
			Settings->SaveConfig();
			++GSerial;
		}
	}

	void PromptSaveLayout(USurfEdMode* Mode)
	{
		if (!FSlateApplication::IsInitialized() || !Mode)
		{
			return;
		}
		TSharedPtr<SWidget> Parent;
		if (TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow())
		{
			Parent = Root->GetContent();
		}
		if (!Parent.IsValid())
		{
			return;
		}
		const TArray<FString> Pool = PoolIds(SurfVerbs::BuildPool(Mode, nullptr));
		const FString Suggested = FString::Printf(TEXT("Arrangement %d"), LayoutNames().Num() + 1);
		SurfNamePrompt::OpenNamePrompt(Parent.ToSharedRef(),
			LOCTEXT("LayoutNamePrompt", "Name this arrangement, then Enter - it comes back from the menu or the settings"),
			Suggested,
			[Pool](const FString& Name) { SaveLayoutAs(Name, Pool); });
	}

	void AppendInterfaceMenu(FMenuBuilder& Menu, USurfEdMode* Mode, bool bWithEditUI)
	{
		TWeakObjectPtr<USurfEdMode> M = Mode;

		Menu.BeginSection("Interface");

		// Settings left the pool for this menu (S156c): a window, not a verb.
		// As verb rows where they are verbs (S204a): their right-click is theirs.
		if (!SurfMenu::AddVerbNamed(Menu, TEXT("Settings"), LOCTEXT("SettingsEntry", "Settings...")))
		{
			Menu.AddMenuEntry(LOCTEXT("SettingsEntry", "Settings..."), LOCTEXT("SettingsEntryTip", "Every setting, on the editor's own preferences page."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { SurfSettingsWindow::Open(); })));
		}
		AddToggle(Menu, LOCTEXT("OptGizmo", "Show the transform gizmo"),
			LOCTEXT("OptGizmoTip", "Off, G R S are the whole interface. On, the gizmo is there too."),
			[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bShowGizmo; },
			[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bShowGizmo = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
		AddToggle(Menu, LOCTEXT("OptOwnGizmo", "Our own gizmo: move, rotate and scale in one"),
			LOCTEXT("OptOwnGizmoTip", "The plugin's gizmo in place of the editor's: arrows, plane squares, rings and cubes together, half the size, quiet until the cursor reaches a handle. Off: the editor's own, one of W E R at a time."),
			[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bOwnGizmo; },
			[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bOwnGizmo = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
		AddToggle(Menu, LOCTEXT("OptGizmoBall", "A ball to turn, inside the rotate rings"),
			LOCTEXT("OptGizmoBallTip", "Drag the ball and the selection turns about whatever axis the drag asks for, all the way round - the rings are still there for one axis, and X, Y or Z during the drag locks it to that one (the same key again lets it go). The ball is only there while the gizmo is in rotate: with the cursor on a ring, or after R."),
			[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bGizmoBall; },
			[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bGizmoBall = V; S->SaveConfig(); } if (GEditor) { GEditor->RedrawLevelEditingViewports(); } });
		// The surface wireframe moved to the viewport's view-mode menu (S178a).

		// Words, icons or both, per surface (S195a).
		Menu.AddSubMenu(LOCTEXT("IconsMenu", "Icons"),
			LOCTEXT("IconsMenuTip", "What the buttons show - words, icons, or both - for the sidebar, the Actions window and the menus, each on its own. A button with no icon keeps its word."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				struct FChoice { ESurfIconMode Mode; FText Label; };
				const FChoice Choices[] = {
					{ ESurfIconMode::Words, LOCTEXT("IconModeWords", "Words") },
					{ ESurfIconMode::Icons, LOCTEXT("IconModeIcons", "Icons") },
					{ ESurfIconMode::IconAndWord, LOCTEXT("IconModeBoth", "Icon and word") },
				};
				auto Section = [&Sub, &Choices](FName Id, const FText& Heading, ESurfIconMode USurfSettings::* Field)
				{
					Sub.BeginSection(Id, Heading);
					for (const FChoice& Choice : Choices)
					{
						const ESurfIconMode Value = Choice.Mode;
						Sub.AddMenuEntry(Choice.Label, FText::GetEmpty(), FSlateIcon(),
							FUIAction(
								FExecuteAction::CreateLambda([Field, Value]()
								{
									if (USurfSettings* S = GetMutableDefault<USurfSettings>())
									{
										S->*Field = Value;
										S->SaveConfig();
									}
									Touch();
								}),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([Field, Value]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->*Field == Value; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
					Sub.EndSection();
				};
				Section(TEXT("IconsSidebar"), LOCTEXT("IconsSidebar", "Sidebar"), &USurfSettings::SidebarIconMode);
				Section(TEXT("IconsActions"), LOCTEXT("IconsActions", "Actions window"), &USurfSettings::ActionsIconMode);
				Section(TEXT("IconsMenus"), LOCTEXT("IconsMenus", "Menus"), &USurfSettings::MenuIconMode);
			}));

		// Back in the menu (S154h): "instead of the pen button: interface >
		// Edit UI".
		if (bWithEditUI && !SurfMenu::AddVerbNamed(Menu, TEXT("Edit UI"), FText::GetEmpty()))
		{
			Menu.AddMenuEntry(
				LOCTEXT("EditUI", "Edit UI"),
				LOCTEXT("EditUITip", "A button becomes a thing you move: click one to send it to the other surface, drag it onto another to take that place, the arrows either side move it along. Nothing runs while this is on."),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([]() { SetEditing(!IsEditing()); }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([]() { return IsEditing(); })),
				NAME_None, EUserInterfaceActionType::ToggleButton);
		}

		AddToggle(Menu, LOCTEXT("ShowTips", "Hints"),
			LOCTEXT("ShowTipsTip", "One grey line at the foot of the viewport: the keys that mean something right now, and for a few seconds what the last verb did or why it would not. Off, the tooltips still say the keys."),
			[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bShowTips; },
			[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bShowTips = V; S->SaveConfig(); } });

		AddToggle(Menu, LOCTEXT("ShowMenuLine", "The menu line"),
			LOCTEXT("ShowMenuLineTip", "The line of family menus under the top line. The right button and the editor's menu bar open the same menus."),
			[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bShowMenuLine; },
			[](bool V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bShowMenuLine = V; S->SaveConfig(); } });

		Menu.AddMenuEntry(
			LOCTEXT("BarOnRight", "Bar on the right"),
			LOCTEXT("BarOnRightTip", "Which edge of the viewport the bar sits against. Right keeps it clear of Unreal's own viewport toolbar."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([]()
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->bBarOnRight = !Settings->bBarOnRight;
						Settings->SaveConfig();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]()
				{
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					return Settings && Settings->bBarOnRight;
				})),
			NAME_None, EUserInterfaceActionType::ToggleButton);

		Menu.AddMenuEntry(
			LOCTEXT("SaveLayout", "Save this arrangement\u2026"),
			LOCTEXT("SaveLayoutTip", "Both surfaces as they stand, under a name."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([M]() { if (M.IsValid()) { PromptSaveLayout(M.Get()); } })));

		const TArray<FString> Saved = LayoutNames();
		if (Saved.Num() > 0)
		{
			Menu.AddSubMenu(LOCTEXT("UseLayout", "Use an arrangement"), FText::GetEmpty(),
				FNewMenuDelegate::CreateLambda([Saved](FMenuBuilder& Sub)
				{
					for (const FString& Name : Saved)
					{
						Sub.AddMenuEntry(FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([Name]() { UseLayout(Name); })));
					}
				}));
			Menu.AddSubMenu(LOCTEXT("ForgetLayout", "Forget an arrangement"), FText::GetEmpty(),
				FNewMenuDelegate::CreateLambda([Saved](FMenuBuilder& Sub)
				{
					for (const FString& Name : Saved)
					{
						Sub.AddMenuEntry(FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([Name]() { ForgetLayout(Name); })));
					}
				}));
		}

		Menu.AddMenuEntry(LOCTEXT("InterfaceReset", "Put every button back"),
			LOCTEXT("InterfaceResetTip", "The arrangement as it ships: the most used on the bar, the rest in the window."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { Reset(); })));

		Menu.EndSection();
	}

	/**
	 * A button, dressed for editing: an arrow either side to move it along
	 * its surface, and a veil over the face that takes the click instead of
	 * the verb and sends the button across. Nothing can be lost - every
	 * button is on one surface or the other - so a click is always undoable
	 * by clicking again.
	 */
	/**
	 * What a button looks like while the interface is being edited (S92a).
	 *
	 * It used to gain an arrow on each side and a coat of amber, which added
	 * 26 pixels to every chip - so the column reflowed to one button a row and
	 * the whole bar changed shape the moment the switch went on. "interface is
	 * totally different under edit interface mode enabled", and it was: you
	 * could not arrange an interface you could no longer recognise.
	 *
	 * Nothing moves now. The chip keeps its size and gains a one-pixel blue
	 * outline; the two arrows live over its own ends and appear when the
	 * cursor is on it, so they cost no room at all.
	 */
	TSharedRef<SWidget> DressForEditing(TSharedRef<SWidget> Face, const FString& Id, const TArray<FString>& Pool, float Height, bool bDragOnly)
	{
		// The window's chips are only picked up (S166a): the window holds
		// every verb in its own order, so nothing lands on it and nothing
		// nudges along it.
		// A right-click that lands on the veil comes up here (S206a): the
		// section's words / icons.
		auto CatchRight = [](TSharedRef<SWidget> Content, ESurfIconSurface Surface) -> TSharedRef<SWidget>
		{
			return SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(0.0f)
				.OnMouseButtonDown_Lambda([Surface](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() == EKeys::RightMouseButton)
					{
						OpenShowAs(Surface);
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
				[
					Content
				];
		};
		if (bDragOnly)
		{
			return CatchRight(SNew(SOverlay)
				+ SOverlay::Slot()[ Face ]
				+ SOverlay::Slot()
				[
					SNew(SSurfVerbVeil, Id, Pool, /*bBar*/ false, /*bSidebar*/ false)
					.BorderImage(EditOutlineBrush())
					.Padding(0.0f)
					.ToolTipText(LOCTEXT("VeilPickUp", "Drag this verb onto the bar or the shelf to keep it there; drag one from them back here to take it off. Right-click: words or icons for this window."))
				], ESurfIconSurface::Actions);
		}
		// The overlay has to exist before the arrows can ask whether it is
		// hovered, and the arrows are built first - so they are handed a box
		// to look in, and it is filled in a moment later.
		TSharedRef<TWeakPtr<SWidget>> Hot = MakeShared<TWeakPtr<SWidget>>();

		const bool bBar = OnBar(Id, Pool);

		TSharedPtr<SOverlay> Stack;
		TSharedRef<SWidget> Dressed = SAssignNew(Stack, SOverlay)
			+ SOverlay::Slot()
			[
				Face
			]
			+ SOverlay::Slot()
			[
				SNew(SSurfVerbVeil, Id, Pool, bBar)
				.BorderImage(EditOutlineBrush())
				.Padding(0.0f)
				.ToolTipText(LOCTEXT("VeilDrag", "Drag this chip where it should go: onto another chip (its left or right half), onto the bar, or onto the shelf; onto the window to take it off."))
			]
			;
		// No arrows since S167b: "now we don't need the arrows anymore to
		// move left or right" - a drop lands where it is let go.

		*Hot = Stack;
		return CatchRight(Dressed, ESurfIconSurface::Sidebar);
	}

	void AddShowAsLines(FMenuBuilder& Menu, ESurfIconSurface Surface)
	{
		ESurfIconMode USurfSettings::* Field = Surface == ESurfIconSurface::Sidebar ? &USurfSettings::SidebarIconMode
			: (Surface == ESurfIconSurface::Actions ? &USurfSettings::ActionsIconMode : &USurfSettings::MenuIconMode);
		const FText Heading = Surface == ESurfIconSurface::Sidebar ? LOCTEXT("ShowAsSidebar", "The sidebar shows")
			: (Surface == ESurfIconSurface::Actions ? LOCTEXT("ShowAsActions", "The Actions window shows") : LOCTEXT("ShowAsMenus", "The menus show"));
		struct FChoice { ESurfIconMode Mode; FText Label; };
		const FChoice Choices[] = {
			{ ESurfIconMode::Words, LOCTEXT("ShowAsWords", "Words") },
			{ ESurfIconMode::Icons, LOCTEXT("ShowAsIcons", "Icons") },
			{ ESurfIconMode::IconAndWord, LOCTEXT("ShowAsBoth", "Icon and word") },
		};
		Menu.BeginSection(NAME_None, Heading);
		for (const FChoice& Choice : Choices)
		{
			const ESurfIconMode Value = Choice.Mode;
			Menu.AddMenuEntry(Choice.Label, FText::GetEmpty(), FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Field, Value]()
					{
						if (USurfSettings* S = GetMutableDefault<USurfSettings>())
						{
							S->*Field = Value;
							S->SaveConfig();
						}
						Touch();
						if (FSlateApplication::IsInitialized())
						{
							FSlateApplication::Get().DismissAllMenus();
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Field, Value]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->*Field == Value; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
	}

	void OpenShowAs(ESurfIconSurface Surface)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
		AddShowAsLines(Menu, Surface);
		TSharedPtr<SWindow> Top = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (!Top.IsValid())
		{
			return;
		}
		FSlateApplication::Get().PushMenu(Top.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
	}

	/**
	 * Where this button lives, as icons (S154a): "'where this button
	 * lives': way too much text in the options. use icons." One word says
	 * which surface; four small buttons do the rest, each with its tip.
	 */
	/**
	 * Two toggles (S166a): "it's right click: (remove or) add to
	 * favourites (Q); (remove or) add to sidebar." The earlier / later /
	 * reset buttons are gone - a drag in Edit UI places a chip, and the
	 * settings still put everything back.
	 */
	TSharedRef<SWidget> MakeArrangeRow(const FSurfVerb& Verb, TArray<FString> Pool)
	{
		const FString Id = IdOf(Verb);
		auto Toggle = [](TFunction<bool()> IsOn, TFunction<void()> Flip, const FText& OnLabel, const FText& OffLabel, const FText& Tip, const TCHAR* OnBrush, const TCHAR* OffBrush) -> TSharedRef<SWidget>
		{
			return SNew(SBox).HeightOverride(22.0f).Padding(1.0f)
			[
				// A real button (S192d), the editor's, not a bare icon.
				SNew(SButton)
				.ButtonStyle(&VerbButtonStyle())
				.ContentPadding(FMargin(6.0f, 0.0f))
				.ToolTipText(Tip)
				.OnClicked_Lambda([Flip]() { Flip(); return FReply::Handled(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
						[
							SNew(SImage)
							.Image_Lambda([IsOn, OnBrush, OffBrush]() { return FAppStyle::GetBrush(IsOn() ? OnBrush : OffBrush); })
							.ColorAndOpacity_Lambda([IsOn]() { return IsOn() ? SurfTheme::Star() : FSlateColor::UseForeground(); })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(SurfTheme::Font(8))
						.Text_Lambda([IsOn, OnLabel, OffLabel]() { return IsOn() ? OnLabel : OffLabel; })
					]
				]
			];
		};
		// Wrapping, and short (S199a): "Pinned goes out of frame".
		return SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 2.0f))
			+ SWrapBox::Slot()
			[
				Toggle([Id]() { return IsFavorite(Id); }, [Id]() { ToggleFavorite(Id); },
					LOCTEXT("FavOnShort", "In Q"), LOCTEXT("FavOffShort", "Q menu"),
					LOCTEXT("FavoriteTip", "The Q menu: the verbs you starred, at the cursor."),
					TEXT("DetailsView.PropertyIsFavorite"), TEXT("PropertyWindow.Favorites_Disabled"))
			]
			+ SWrapBox::Slot()
			[
				// In the sidebar = under its family's tab; pinned = at the top,
				// always in view (S193b). The two lists underneath are the old
				// bar (pinned) and shelf (the rest of the sidebar).
				Toggle([Id, Pool]() { return OnBar(Id, Pool) || OnShelf(Id, Pool); },
					[Id, Pool]() { if (OnBar(Id, Pool) || OnShelf(Id, Pool)) { RemoveFromSurfaces(Id, Pool); } else { SendTo(Id, /*bBar*/ false, Pool); } },
					LOCTEXT("BarOnShort", "In sidebar"), LOCTEXT("BarOffShort", "Sidebar"),
					LOCTEXT("SidebarTip", "The sidebar beside the viewport: this verb under its family's tab. In Edit UI, drag buttons to arrange them."),
					TEXT("Icons.Layout"), TEXT("Icons.Layout"))
			]
			+ SWrapBox::Slot()
			[
				Toggle([Id, Pool]() { return OnBar(Id, Pool); },
					[Id, Pool]() { SendTo(Id, /*bBar*/ !OnBar(Id, Pool), Pool); },
					LOCTEXT("PinOn", "Pinned"), LOCTEXT("PinOff", "Pin"),
					LOCTEXT("PinTip", "Pinned at the top of the sidebar, in view whatever tab is folded."),
					TEXT("Icons.Pinned"), TEXT("Icons.Unpinned"))
			];
	}


}

// --- The grid ---------------------------------------------------------------

void SSurfToolPalette::Construct(const FArguments& InArgs)
{
	Mode = InArgs._Mode;

	// The same pool the bar draws from (S74a): what is here is what is not
	// there, and a right-click moves a button between the two.
	Pool = BuildPoolVerbs(Mode, SharedThis(this));

	// The window takes a chip dragged off the bar or the shelf (S166a).
	ChildSlot
	[
		SurfVerbLayout::MakeRemoveZone(SAssignNew(Body, SVerticalBox), SurfVerbLayout::PoolIds(Pool))
	];
	Refill();

	RegisterActiveTimer(0.4f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		if (LayoutSeen != SurfVerbLayout::Serial())
		{
			Refill();
		}
		return EActiveTimerReturnType::Continue;
	}));
}

void SSurfToolPalette::Refill()
{
	if (!Body.IsValid())
	{
		return;
	}
	LayoutSeen = SurfVerbLayout::Serial();
	Body->ClearChildren();

	// Every verb, always (S166a): "the surfacingpro actions window should
	// always contain ALL actions even those in the sidebar already." The
	// bar and the shelf are where favourites go; this is where they come
	// from.
	TArray<const FSurfVerb*> Mine;
	for (const FSurfVerb& Verb : Pool)
	{
		Mine.Add(&Verb);
	}
	auto Of = [&Mine](ESurfVerbGroup Group)
	{
		TArray<const FSurfVerb*> Out;
		for (const FSurfVerb* Verb : Mine)
		{
			if (Verb->Group == Group)
			{
				Out.Add(Verb);
			}
		}
		return Out;
	};
	auto Section = [this](const FText& Heading, const TArray<const FSurfVerb*>& Verbs)
	{
		if (Verbs.Num() > 0)
		{
			Body->AddSlot().AutoHeight()[BuildSection(Heading, Verbs)];
		}
	};

	Section(LOCTEXT("SecCreate", "Create"), Of(ESurfVerbGroup::Create));
	Section(LOCTEXT("SecGeom", "Geometry"), Of(ESurfVerbGroup::Geometry));
	Section(LOCTEXT("SecShape", "Shape"), Of(ESurfVerbGroup::Shape));
	Section(LOCTEXT("SecSurface", "Surface"), Of(ESurfVerbGroup::Surface));
	Section(LOCTEXT("SecUV", "UV"), Of(ESurfVerbGroup::UV));
	Section(LOCTEXT("SecSelect", "Select"), Of(ESurfVerbGroup::Select));
	Section(LOCTEXT("SecView", "View"), Of(ESurfVerbGroup::View));
}

TSharedRef<SWidget> SSurfToolPalette::BuildSection(const FText& Heading, const TArray<const FSurfVerb*>& Verbs)
{
	// A wrap box rather than a fixed grid: the panel gets docked at whatever
	// width the user likes, and buttons should reflow rather than clip.
	TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(3.0f, 3.0f));

	for (const FSurfVerb* Verb : Verbs)
	{
		Wrap->AddSlot()[BuildButton(*Verb)];
	}

	// A Details category (S192d): the header grey, the name in its bold.
	// Its right-click: the whole family into the Q menu (S196a).
	const bool bHasFamily = Verbs.Num() > 0;
	const ESurfVerbGroup Family = bHasFamily ? Verbs[0]->Group : ESurfVerbGroup::View;
	// A click on the heading folds the family, remembered (S203a); editing
	// the interface opens them all, as the sidebar does.
	const int32 Bit = 1 << static_cast<int32>(Family);
	auto Folded = [Bit]()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return !SurfVerbLayout::IsEditing() && S && (S->ActionsFoldedFamilies & Bit) != 0;
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(6.0f, 0.0f))
			.Cursor(EMouseCursor::Hand)
			.ToolTipText(LOCTEXT("ActionsHeadingTip", "Click: fold or unfold. Right-click: the whole menu into the Q menu."))
			.OnMouseButtonDown_Lambda([bHasFamily, Family, Bit](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (bHasFamily && Event.GetEffectingButton() == EKeys::RightMouseButton)
				{
					SurfMenu::OpenFamilyContext(Family);
					return FReply::Handled();
				}
				if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
				{
					if (USurfSettings* S = GetMutableDefault<USurfSettings>())
					{
						S->ActionsFoldedFamilies ^= Bit;
						S->SaveConfig();
					}
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			[
				SNew(SBox).HeightOverride(24.0f).VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
					[
						SNew(SImage)
						.Image_Lambda([Folded]() { return FAppStyle::GetBrush(Folded() ? TEXT("Icons.ChevronRight") : TEXT("Icons.ChevronDown")); })
						.DesiredSizeOverride(FVector2D(10.0, 10.0))
						.ColorAndOpacity(SurfTheme::Dim())
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Heading)
						.Font(SurfTheme::Bold(8))
						.ColorAndOpacity(SurfTheme::Header())
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f, 4.0f, 4.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([Folded]() { return Folded() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				Wrap
			]
		];
}

TSharedRef<SWidget> SSurfToolPalette::BuildButton(const FSurfVerb& Verb)
{
	TFunction<void()> Run = Verb.Run;
	TFunction<bool()> Enabled = Verb.IsEnabled;
	const FSurfVerb Copy = Verb;
	const bool bHasOptions = static_cast<bool>(Verb.Options);
	TWeakObjectPtr<USurfEdMode> Repeatable = Mode;
	const FString VerbName = Verb.Label.ToString();
	// Words, icons or both, as the Actions window's setting says (S195a).
	const ESurfIconMode IconMode = SurfIcons::ModeFor(ESurfIconSurface::Actions);
	const FSlateBrush* Icon = IconMode != ESurfIconMode::Words ? SurfIcons::ForVerb(VerbName) : nullptr;
	const bool bShowWord = !Icon || IconMode == ESurfIconMode::IconAndWord;

	TSharedRef<SWidget> Button = SNew(SBox).MinDesiredWidth(bShowWord ? 112.0f : 26.0f).HeightOverride(24.0f)
	[
		SNew(SButton)
		.ButtonStyle(&VerbButtonStyle())
		.ToolTipText_Lambda([Copy, bHasOptions]()
		{
			return bHasOptions
				? FText::Format(LOCTEXT("PanelTipWithOptions", "{0}\nRight-click: options · favourites · sidebar."), SurfVerbs::VerbTip(Copy))
				: FText::Format(LOCTEXT("PanelTipMoveOnly", "{0}\nRight-click: favourites · sidebar."), SurfVerbs::VerbTip(Copy));
		})
		.ContentPadding(FMargin(0.0f))
		.IsEnabled_Lambda([Enabled]() { return Enabled ? Enabled() : true; })
		.OnClicked_Lambda([Run, Repeatable, VerbName, bOffer = bHasOptions && Verb.Group != ESurfVerbGroup::View]()
		{
			if (Run)
			{
				// Remembered before it runs, so Shift+R repeats it.
				if (Repeatable.IsValid())
				{
					Repeatable->NoteVerb(VerbName, Run);
				}
				Run();
				// Its frame, every time it runs (S184c): "shading window
				// param opened once and never again".
				if (Repeatable.IsValid())
				{
					Repeatable->AfterVerbRan(VerbName, bOffer);
				}
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(Icon ? FMargin(6.0f, 0.0f, bShowWord ? 0.0f : 6.0f, 0.0f) : FMargin(0.0f))
			[
				SNew(SImage)
				.Image(Icon)
				.DesiredSizeOverride(FVector2D(14.0, 14.0))
				.ColorAndOpacity(FSlateColor::UseForeground())
				.Visibility(Icon ? EVisibility::Visible : EVisibility::Collapsed)
			]
			// No family stripe (S192d): the family is the header above.
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(bShowWord ? FMargin(Icon ? 6.0f : 8.0f, 0.0f, 4.0f, 0.0f) : FMargin(0.0f))
			[
				SNew(STextBlock)
				.Text_Lambda([Copy]() { return Copy.LiveLabel ? Copy.LiveLabel() : Copy.Label; })
				.Visibility(bShowWord ? EVisibility::Visible : EVisibility::Collapsed)
				.Font(SurfTheme::Font(9))
				.ColorAndOpacity_Lambda([Enabled]()
				{
					const bool bOn = Enabled ? Enabled() : true;
					return bOn ? SurfTheme::Text() : FSlateColor(FStyleColors::Hover2);
				})
			]

		]
	];

	// The right button, for the options and for moving the button itself.
	TSharedRef<SWidget> Face = SNew(SBorder)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.Padding(0.0f)
		.OnMouseButtonDown_Lambda([this, Copy](const FGeometry& Geometry, const FPointerEvent& Event) -> FReply
		{
			if (Event.GetEffectingButton() == EKeys::RightMouseButton)
			{
				OpenOptions(Copy, Geometry);
				return FReply::Handled();
			}
			return FReply::Unhandled();
		})
		[
			Button
		];

	if (!SurfVerbLayout::IsEditing())
	{
		return Face;
	}
	return SurfVerbLayout::DressForEditing(Face, VerbName, SurfVerbLayout::PoolIds(Pool), 26.0f, /*bDragOnly*/ true);
}

void SSurfToolPalette::OpenOptions(const FSurfVerb& Verb, const FGeometry&)
{
	// The one right-click (S196a): options, the Q menu, its whole menu, the
	// sidebar - the same wherever the button is.
	SurfMenu::OpenVerbContext(Verb, nullptr, static_cast<int32>(ESurfIconSurface::Actions));
}

// --- The viewport bar -------------------------------------------------------

SSurfViewportBar::~SSurfViewportBar()
{
	if (ResetListener.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationPreInputKeyDownListener().Remove(ResetListener);
	}
	ResetListener.Reset();
	GResettables.Reset();
}

void SSurfViewportBar::Construct(const FArguments& InArgs)
{
	// An overlay widget is handed the whole viewport, and a Visible widget
	// is hit-tested over the whole of it - so the frame added after this bar
	// sat over the bar's buttons, invisible, and took every click for the
	// viewport: "no buttons on the left bar work". Only the children are
	// hit-tested now; the widget itself is glass.
	SetVisibility(EVisibility::SelfHitTestInvisible);

	if (FSlateApplication::IsInitialized())
	{
		ResetListener = FSlateApplication::Get().OnApplicationPreInputKeyDownListener().AddStatic(&ResetHoveredKnob);
	}

	Mode = InArgs._Mode;
	TWeakObjectPtr<USurfEdMode> M = Mode;

	QuickVerbs = BuildPoolVerbs(M, SharedThis(this));

	/**
	 * Three pieces, each with one job (S90a).
	 *
	 * A thin line across the top carries every state and every switch, and
	 * the name of what is being edited; it scrolls sideways, so it is one
	 * line at any width. The numbers sit small in the corner beneath it,
	 * where they are read rather than aimed at. And the side is nothing but
	 * verbs: no panel behind them, no headings between them, each one as
	 * wide as its own word.
	 *
	 * What this replaces is the reason for it. Everything used to be in the
	 * one column - four chips, three switches, two arrows, a path, a status
	 * line, then the verbs - which made the column wide, made it tall, and
	 * put a slab of grey over the corner of the viewport you model in.
	 */
	SAssignNew(VerbFlow, SVerticalBox);
	RefillVerbs();

	// A verb sent here from the window, or moved along the bar, shows up
	// without anything being reopened (S74a).
	RegisterActiveTimer(0.4f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		if (LayoutSeen != SurfVerbLayout::Serial())
		{
			RefillVerbs();
		}
		return EActiveTimerReturnType::Continue;
	}));

	auto OnRight = []()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Settings->bBarOnRight;
	};

	/**
	 * The edge you drag to size the bar: five pixels of nothing, carrying the
	 * cursor that says what it does, on the inner side of the bar where the
	 * hand already is. "its sizeable (doesnt need a visible bar for it)" - so
	 * there is no strip to look at, only one to find, and it shows itself only
	 * while it is being dragged.
	 */
	// The column is built first so the grips beside it can be as tall as it
	// is (S169a): sized from the room instead, the width grip ran the whole
	// height of the viewport - "visible at all time even when not in edit ui
	// mode".
	TSharedPtr<SWidget> Column;
	TSharedRef<TWeakPtr<SWidget>> ColumnHolder = MakeShared<TWeakPtr<SWidget>>();

	auto MakeGrip = [this, OnRight, ColumnHolder](bool bWhenOnRight) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.Visibility_Lambda([OnRight, bWhenOnRight]()
			{
				return OnRight() == bWhenOnRight ? EVisibility::Visible : EVisibility::Collapsed;
			})
			// Down the edge by what the handle set, like the column beside it.
			.Padding(TAttribute<FMargin>::CreateLambda([]()
			{
				const USurfSettings* Settings = GetDefault<USurfSettings>();
				return FMargin(0.0f, Settings ? static_cast<float>(Settings->ViewportBarTopPx) : 0.0f, 0.0f, 0.0f);
			}))
			[
				SNew(SSurfGrip)
				.Vertical(true)
				.Thickness(6.0f)
				.Length_Lambda([ColumnHolder]() -> FOptionalSize
				{
					TSharedPtr<SWidget> Live = ColumnHolder->Pin();
					if (!Live.IsValid())
					{
						return FOptionalSize();
					}
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					const float Top = Settings ? static_cast<float>(Settings->ViewportBarTopPx) : 0.0f;
					return FOptionalSize(FMath::Max(0.0f, Live->GetCachedGeometry().GetLocalSize().Y - Top));
				})
				.Dragging_Lambda([this]() { return bResizingBar; })
				.Shown_Lambda([]() { return SurfVerbLayout::IsEditing(); })
				.Tip(LOCTEXT("BarGripTip", "Width: drag to make the bar wider or narrower - the buttons stack into as many columns as fit. Double-click puts it back."))
				.OnDown_Lambda([this](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
					{
						return FReply::Unhandled();
					}
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					BarDragStartWidth = Settings ? Settings->ViewportBarWidthPx : 138;
					BarDragStartX = static_cast<float>(Event.GetScreenSpacePosition().X);
					bResizingBar = true;
					// The capture goes to the bar, which is what finishes the
					// drag - a grip that loses the pointer halfway leaves the
					// bar stuck at whatever width it reached.
					return FReply::Handled().CaptureMouse(SharedThis(this));
				})
				.OnDoubleClick_Lambda([this](const FGeometry&, const FPointerEvent&) -> FReply
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->ViewportBarWidthPx = 138;
						Settings->SaveConfig();
					}
					bResizingBar = false;
					return FReply::Handled();
				})
			];
	};

	/**
	 * And which edge it sits on, while the interface is being edited (S91a).
	 *
	 * The side was a tick in a menu, which is where a setting goes and not
	 * where a position goes: a thing that has a place on the screen should be
	 * moved by dragging it to the place. Edit the interface is already the
	 * mode in which the buttons are rearranged, so the bar moves there too,
	 * and nowhere else - a bar that can be dragged away by accident while
	 * modelling is worse than one that cannot be dragged at all.
	 */
	auto MoveHandle = [this]() -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.Visibility_Lambda([]()
			{
				return SurfVerbLayout::IsEditing() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SSurfGrip)
				.Vertical(false)
				.Handle(true)
				.Thickness(10.0f)
				.Dragging_Lambda([this]() { return bMovingBar; })
				.Shown_Lambda([]() { return SurfVerbLayout::IsEditing(); })
				.Tip(LOCTEXT("MoveBarTip", "Drag: down the edge to where the bar should start, across the viewport to the other edge. Double-click puts it back at the top."))
				.OnDoubleClick_Lambda([this](const FGeometry&, const FPointerEvent&) -> FReply
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->ViewportBarTopPx = 0;
						Settings->SaveConfig();
					}
					bMovingBar = false;
					return FReply::Handled();
				})
				.OnDown_Lambda([this](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
					{
						return FReply::Unhandled();
					}
					bMovingBar = true;
					BarTopOffset = 0.0f;
					return FReply::Handled().CaptureMouse(SharedThis(this));
				})
			];
	};

	// Game view hides what we draw in the viewport - wires, boxes, labels
	// - and not the bar (S239a: "should not leave surfacing pro mode ...
	// toggle off surfacing pro ui elements (wire; BBox; labels etc..) and
	// user may toggle them back on while in game view"): the chips stay,
	// so the toggles are at hand.
	ChildSlot
	.HAlign(HAlign_Fill)
	.VAlign(VAlign_Fill)
	.Padding(6.0f, 6.0f, 6.0f, 6.0f)
	[
		SNew(SBox)
		.Visibility(EVisibility::SelfHitTestInvisible)
		[
		SNew(SVerticalBox)

		// The line floats: as wide as it needs, clear of the top edge, on a
		// plate of Unreal's own colour (S91a).
		// Flush under the editor's viewport toolbar, edge to edge (S193a).
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(-6.0f, -6.0f, -6.0f, 0.0f)
		[
			BuildTopBar()
		]


		/**
		 * The verbs start right under the menu line (S155c): the view gizmo
		 * used to take a row of its own above them - top right, but a whole
		 * row's height - which left the column floating a hand below the
		 * lines. It sits over the same row now, at the right.
		 */
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SOverlay)

			// Beside the SidePanel when it is open on this edge (S177a).
			+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(TAttribute<FMargin>::CreateLambda([OnRight]()
			{
				const float Panel = OnRight() ? 0.0f : SurfSidePanel::Width();
				return FMargin(0.0f, 1.0f, Panel > 0.0f ? Panel + 6.0f : 2.0f, 0.0f);
			}))
			[
				SNew(SSurfViewGizmo)
				.ToolTipText(LOCTEXT("ViewGizmoTip", "The three axes as the camera sees them. Click a tip to look down that axis, about whatever the view is orbiting."))
			]

			+ SOverlay::Slot()
			[
			SNew(SHorizontalBox)

			// The SidePanel on the edge the bar is not (S177a): here when the bar is on the right.
			// A little down from the top (S222a: "move the sidepanels down a bit so theres some space").
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([OnRight]() { return OnRight() && SurfSidePanel::IsOpen() ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SSurfSidePanel).Mode(M).TabsOnRight(false)
					.MaxHeight(TAttribute<FOptionalSize>::CreateSP(this, &SSurfViewportBar::GetSidePanelMaxHeight))
				]
			]

			/**
			 * The foot line lives in whichever side of the bar is free
			 * (S158a): one line of readings and keys at the bottom of the
			 * viewport, never under the bar whatever the resolution - "info
			 * panel currently overlaps with sidebar. be adaptive."
			 */
			+ SHorizontalBox::Slot()
			.FillWidth(TAttribute<float>::CreateLambda([OnRight]() { return OnRight() ? 1.0f : 0.0f; }))
			.VAlign(VAlign_Bottom)
			[
				SNew(SBox).Visibility_Lambda([OnRight]() { return OnRight() ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
				[ BuildFootLine() ]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				MakeGrip(true)
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				/**
				 * No frame behind the verbs: "No frame background, so viewport
				 * shows behind." Each chip carries its own plate, which is what
				 * has to be legible; the space between them is the scene, which
				 * is what the screen is for.
				 */
				SAssignNew(Column, SBox)
				.WidthOverride(TAttribute<FOptionalSize>::CreateLambda([this]()
				{
					// Only as wide as the tabs while nothing is open, or once
					// dragged narrower than a button (S193b): "can even go all
					// the way to collapsed".
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					const int32 Width = Settings ? Settings->ViewportBarWidthPx : 138;
					const int32 Open = Settings ? Settings->SidebarOpenFamilies : 0;
					const bool bShelf = Settings ? Settings->bSidebarShelfOpen : true;
					// No tabs to open since S203a: the shelf shows whatever it holds.
					(void)Open;
					const bool bAnything = SurfVerbLayout::IsEditing() || bSidebarHasPinned || (bShelf && SidebarFamilyMask != 0);
					return Width >= SurfSidebar::CollapsedBelowPx && bAnything ? FOptionalSize(static_cast<float>(Width)) : FOptionalSize();
				}))
				// Down the edge by what the handle set (S165a).
				.Padding(TAttribute<FMargin>::CreateLambda([]()
				{
					const USurfSettings* Settings = GetDefault<USurfSettings>();
					return FMargin(0.0f, Settings ? static_cast<float>(Settings->ViewportBarTopPx) : 0.0f, 0.0f, 0.0f);
				}))
				.Visibility(this, &SSurfViewportBar::GetVerbVisibility)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
					[
						MoveHandle()
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox)
						.MaxDesiredHeight(TAttribute<FOptionalSize>::CreateSP(this, &SSurfViewportBar::GetBarMaxHeight))
						[
							// No panel behind the sidebar (S193c): "no background for sidebar".
							SNew(SBorder)
							.BorderImage(FStyleDefaults::GetNoBrush())
							.Padding(FMargin(1.0f, 2.0f))
							[
							SNew(SSurfBarScrollBox)
							.ScrollBarThickness(FVector2D(2.0f, 2.0f))
							.ScrollBarStyle(&QuietScrollBarStyle())
							.ScrollBarPadding(FMargin(1.0f, 0.0f, 0.0f, 0.0f))
							// The wheel over the bar is the bar's, at its end too
							// (S158a): "once at the end, scroll must not scroll
							// the viewport". Ctrl+wheel in Edit UI is let through
							// to the bar, which sizes the chips (S166a).
							.ConsumeMouseWheel(EConsumeMouseWheel::Always)
							+ SScrollBox::Slot()
							[
								VerbFlow.ToSharedRef()
							]
						]
						]
					]
					/**
					 * The grip at the foot of the column (S162a): "should be
					 * able to edit the sidebar/shelf length with mouse". Five
					 * pixels with a hairline, like the one down the edge; drag
					 * sets how long the column may be before it scrolls,
					 * double-click gives it the whole viewport again.
					 */
					+ SVerticalBox::Slot().AutoHeight()
					[
						// Only while arranging (S167b): "two horizontal bars...
						// remove" - the hairline is gone from the plain bar.
						// Always there (S193b), glass until the hand finds it, as the
						// width's grip: "need be able to resize length like width".
						SNew(SBox)
						.Visibility(EVisibility::Visible)
						[
							SNew(SSurfGrip)
							.Vertical(false)
							.Thickness(6.0f)
							.Dragging_Lambda([this]() { return bLengtheningBar; })
							.Shown_Lambda([]() { return SurfVerbLayout::IsEditing(); })
							.Tip(LOCTEXT("BarLengthGripTip", "Drag to make the column longer or shorter - it scrolls past the end. Double-click gives it the whole viewport."))
							.OnDown_Lambda([this](const FGeometry&, const FPointerEvent& Event) -> FReply
							{
								if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
								{
									return FReply::Unhandled();
								}
								const FOptionalSize Now = GetBarMaxHeight();
								BarDragStartLength = Now.IsSet() ? FMath::RoundToInt(Now.Get()) : 300;
								BarDragStartY = static_cast<float>(Event.GetScreenSpacePosition().Y);
								bLengtheningBar = true;
								return FReply::Handled().CaptureMouse(SharedThis(this));
							})
							.OnDoubleClick_Lambda([this](const FGeometry&, const FPointerEvent&) -> FReply
							{
								if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
								{
									Settings->ViewportBarLengthPx = 0;
									Settings->SaveConfig();
								}
								bLengtheningBar = false;
								return FReply::Handled();
							})
						]
					]
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				MakeGrip(false)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(TAttribute<float>::CreateLambda([OnRight]() { return OnRight() ? 0.0f : 1.0f; }))
			.VAlign(VAlign_Bottom)
			[
				SNew(SBox).Visibility_Lambda([OnRight]() { return OnRight() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })
				[ BuildFootLine() ]
			]

			// And here when the bar is on the left, the usual case.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([OnRight]() { return !OnRight() && SurfSidePanel::IsOpen() ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SSurfSidePanel).Mode(M).TabsOnRight(true)
					.MaxHeight(TAttribute<FOptionalSize>::CreateSP(this, &SSurfViewportBar::GetSidePanelMaxHeight))
				]
			]
			]
		]
		]
	];
	*ColumnHolder = Column;
}

FOptionalSize SSurfViewportBar::GetTopBarMaxWidth() const
{
	const FEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	const FViewport* Port = Client ? Client->Viewport : nullptr;
	const float Scale = FMath::Max(0.1f, GetCachedGeometry().Scale);
	const float Pixels = Port ? static_cast<float>(Port->GetSizeXY().X) : 0.0f;
	if (Pixels < 100.0f)
	{
		return FOptionalSize();
	}
	return FOptionalSize(FMath::Max(160.0f, Pixels / Scale - 16.0f));
}

namespace
{
	/** A chip whose right-click is its verb's (S199a, S204a): the Q menu, the sidebar. */
	TSharedRef<SWidget> ChipWithVerb(TSharedRef<SWidget> Chip, const TArray<FSurfVerb>& Verbs, const TCHAR* Label)
	{
		const FSurfVerb* Found = Verbs.FindByPredicate([Label](const FSurfVerb& V) { return V.Label.ToString() == Label; });
		if (!Found)
		{
			return Chip;
		}
		TSharedRef<FSurfVerb> Own = MakeShared<FSurfVerb>(*Found);
		TSharedRef<TWeakPtr<SWidget>> Self = MakeShared<TWeakPtr<SWidget>>();
		TSharedRef<SWidget> Wrapped = SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			.OnMouseButtonDown_Lambda([Own, Self](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() == EKeys::RightMouseButton)
				{
					SurfMenu::OpenVerbContext(*Own, Self->Pin());
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			[
				Chip
			];
		*Self = Wrapped;
		return Wrapped;
	}
}

TSharedRef<SWidget> SSurfViewportBar::BuildTopBar()
{
	// Every switch built here wears the segment style (S192b).
	TGuardValue<bool> Switches(GTopLineSwitches, true);

	/**
	 * One strip (S193a): "i liked our top bar as a single bar rather than
	 * disconnected sections ... merge it to ue actual toolbar, but on a
	 * second lower line". It sits flush under the editor's own viewport
	 * toolbar, full width, in that toolbar's colour, groups split by its
	 * thin rule. When the viewport is too narrow a whole group goes to a
	 * second line rather than scrolling out of sight.
	 */
	TSharedRef<SWrapBox> Line = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0.0f, 3.0f));
	auto Add = [&Line](TSharedRef<SWidget> Group, float Left = 4.0f)
	{
		TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
		if (Left > 0.0f)
		{
			Box->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(1.0f).HeightOverride(16.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FStyleColors::Hover)
					.Padding(0.0f)
				]
			];
		}
		Box->AddSlot().AutoWidth().VAlign(VAlign_Center)[ Group ];
		Line->AddSlot().VAlign(VAlign_Center)[ Box ];
	};

	auto WithVerb = [this](TSharedRef<SWidget> Chip, const TCHAR* Label) -> TSharedRef<SWidget>
	{
		return ChipWithVerb(Chip, QuickVerbs, Label);
	};

	// The selection kinds first, always at the top left (S199a): "is it
	// possible no matter viewport size, selection mode ... is always top
	// left". Then the family menus (S193a), then in and out (S99a).
	Add(BuildSubModeRow(), 0.0f);
	Add(BuildMenuLine());

	// Then what is being edited, because it is the answer to the question
	// asked most often: the path in the plain colour, the name bright (S86a).
	Add(SNew(SHorizontalBox)
		// The level's own mark before its name (S199a): the editor's Level
		// Instance icon, in the level's tint; a prefab wears it too for now.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("ClassIcon.LevelInstance"))
				.ColorAndOpacity(this, &SSurfViewportBar::GetWhereColour)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SSurfViewportBar::GetWherePath)
			.ColorAndOpacity(SurfTheme::Dim())
			.Font(SurfTheme::Font(8))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SSurfViewportBar::GetWhereName)
			.ColorAndOpacity(this, &SSurfViewportBar::GetWhereColour)
			.Font(SurfTheme::Bold(9))
		]
		// C, out and in right after the name (S193b).
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			BuildNavSteps()
		]);

	// In the order a hand reaches for them, so what scrolls off the end is
	// what is set once a session.
	Add(BuildNavRow());
	Add(BuildSoftRow());

	/**
	 * Game, a state (S154g): the level as the game sees it - the bake in
	 * place of the previews, no lines - filled while it is on, the word
	 * never changing. The editor's own switch (G with nothing to move, the
	 * viewport menu) lights it the same, and the bake follows either way.
	 */
	{
		auto GameOn = []() { return GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsInGameView(); };
		Add(WithVerb(
			ToggleButton(
				SNew(STextBlock)
				.Text(LOCTEXT("GameChip", "Game"))
				.Font(SurfTheme::Bold(8))
				.ColorAndOpacity_Lambda([GameOn]() { return GameOn() ? SurfTheme::Bright() : SurfTheme::Text(); }),
				LOCTEXT("GameChipTip", "The level as the game sees it, lit: the bake in place of the previews, none of our wires, boxes or labels. Whatever changed is baked first, on its own. Overlay draws ours over it. Press again for the editor's view."),
				38.0f, 18.0f, GameOn,
				[]()
				{
					if (FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
					{
						Client->SetGameView(!Client->IsInGameView());
						Client->Invalidate();
					}
				}), TEXT("Game")));
	}

	// The SidePanel's chip is gone (S206a): "remove Panel button from
	// topbar. leave it toggled on." N and the Side Panel verb still fold it.


	// Which build is in hand, at the end of the line (S206a): "Build in hand
	// display: OK. clicking it shouldnt disable the sidebar" - a label now,
	// the fold it carried is gone (the sidebar folds from its own chevron).
	Add(
		SNew(SBox)
		.VAlign(VAlign_Center)
		.ToolTipText(LOCTEXT("BuildTip", "Which build is in hand."))
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString(SURF_BUILD_LABEL)))
			.Font(SurfTheme::Font(8))
			.ColorAndOpacity(SurfTheme::Dim())
		]);

	/**
	 * One line, whatever the width (S90a): scrolling sideways keeps the order
	 * fixed, so the same switch is always the same distance from the name.
	 * No plate of its own now (S192b) - each group carries one.
	 */
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(6.0f, 3.0f))
		[
			Line
		];
}

TSharedRef<SWidget> SSurfViewportBar::BuildMenuLine()
{
	TWeakObjectPtr<USurfEdMode> M = Mode;

	// One pull-down: the word in white with the family's colour as a stripe
	// beside it, as the buttons wear it (S154a: "write in white, but have
	// the side-color like for the buttons"); the menu built when it opens,
	// so it says what the pool says now.
	auto PullDown = [](const FText& Label, const FText& Tip, FSlateColor Colour, TFunction<void(FMenuBuilder&)> Fill, bool bView, ESurfVerbGroup Family) -> TSharedRef<SWidget>
	{
		// Its right-click puts the whole menu in the Q menu (S196a).
		return SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			.OnMouseButtonDown_Lambda([Family](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() == EKeys::RightMouseButton)
				{
					SurfMenu::OpenFamilyContext(Family);
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			[
			SNew(SComboButton)
			.ButtonStyle(&VerbButtonStyle())
			.HasDownArrow(false)
			.ContentPadding(FMargin(7.0f, 2.0f, 6.0f, 2.0f))
			.ToolTipText(Tip)
			.OnGetMenuContent_Lambda([Fill, bView]() -> TSharedRef<SWidget>
			{
				// View is the merged menu (S193c).
				if (bView)
				{
					return SurfMenu::MakeViewMenu();
				}
				// No search box in a menu (S156c): "don't need search in the
				// menus top bar" - X is the search.
				FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
				Fill(Menu);
				return Menu.MakeWidget();
			})
			.ButtonContent()
			[
				// The editor's menu-bar look (S192b): the word, and its small
				// chevron; the family colour stripe is gone.
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(SurfTheme::Font(9))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 1.0f, 0.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.ChevronDown"))
						.ColorAndOpacity(SurfTheme::Dim())
					]
				]
			]
			];
	};

	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox);

	const ESurfVerbGroup Families[] = { ESurfVerbGroup::Create, ESurfVerbGroup::Geometry,
		ESurfVerbGroup::Shape, ESurfVerbGroup::Surface, ESurfVerbGroup::UV, ESurfVerbGroup::Select, ESurfVerbGroup::View };
	for (ESurfVerbGroup Family : Families)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(Family == ESurfVerbGroup::Create ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f)
		[
			PullDown(SurfMenu::FamilyTitle(Family),
				LOCTEXT("FamilyMenuTip", "This family's verbs, the same as the right button's. Right-click here: the whole menu into the Q menu."),
				GroupColour(Family),
				[M, Family](FMenuBuilder& Menu) { SurfMenu::BuildFamily(Menu, M.Get(), Family); },
				Family == ESurfVerbGroup::View, Family)
		];
	}
	// Windows and Interface left this line (S177a): every window and the
	// interface are verbs now, in these families, to star or put on the
	// bar; the editor's SurfacingPro menu and the right-click keep both.
	Line->AddSlot().AutoWidth()[ SNew(SBox).WidthOverride(3.0f) ];

	// On the one strip now (S193a): the menus are its first group.
	return SNew(SBox)
		.Visibility_Lambda([]()
		{
			const USurfSettings* Settings = GetDefault<USurfSettings>();
			return (!Settings || Settings->bShowMenuLine) ? EVisibility::Visible : EVisibility::Collapsed;
		})
	[
		Line
	];
}

TSharedRef<SWidget> SSurfViewportBar::BuildTipPanel()
{
	/**
	 * The hint line (S156f): one line at the foot of the viewport, grey,
	 * with the keys that mean something right now - and, for a few seconds,
	 * what the last verb did or why it would not (amber). No plate, no
	 * cross: "a hint discreet line down in frame, toggleable." Interface
	 * turns it off; off, the tooltips still carry the keys.
	 */
	return SNew(SBox)
	.Visibility(this, &SSurfViewportBar::GetTipVisibility)
	.MaxDesiredWidth(640.0f)
	[
		SNew(STextBlock)
		.Text(this, &SSurfViewportBar::GetTip)
		.ColorAndOpacity(this, &SSurfViewportBar::GetStatusColour)
		.Font(SurfTheme::Font(8))
		.ShadowOffset(FVector2D(1.0f, 1.0f))
		.ShadowColorAndOpacity(SurfTheme::Shadow())
		.AutoWrapText(true)
	];
}

TSharedRef<SWidget> SSurfViewportBar::BuildFootLine()
{
	/**
	 * One line (S158a): "may it be in a one liner info down viewport, along
	 * with others? and don't have them vertical boxes. it's unreadable atm."
	 * The readings first - what is selected, the texel density, the layer,
	 * a bake running - then the keys of the moment or the last result. Left
	 * to right, 8 pt, a shadow under it, never wrapping: what does not fit
	 * is cut, not stacked.
	 */
	auto Reading = [this](TAttribute<FText> Text, TAttribute<FSlateColor> Colour, const FText& Tip) -> TSharedRef<SWidget>
	{
		return SNew(STextBlock)
			.Text(Text)
			.ColorAndOpacity(Colour)
			.ToolTipText(Tip)
			.Font(SurfTheme::Font(8))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
			.ShadowColorAndOpacity(SurfTheme::Shadow())
			.Visibility_Lambda([Text]() { return Text.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; });
	};
	auto Dot = []() -> TSharedRef<SWidget>
	{
		return SNew(STextBlock)
			.Text(FText::FromString(TEXT("  \u00b7  ")))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Font(SurfTheme::Font(8))
			.ShadowOffset(FVector2D(1.0f, 1.0f))
			.ShadowColorAndOpacity(SurfTheme::Shadow());
	};
	return SNew(SBox).Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f)).Clipping(EWidgetClipping::ClipToBounds)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Reading(TAttribute<FText>::CreateLambda([this]() { return Mode.IsValid() && Mode->IsBaking() ? FText::FromString(Mode->BakeProgress()) : FText::GetEmpty(); }),
				FSlateColor(FStyleColors::AccentYellow), LOCTEXT("BakingTip", "A bake is running a slice at a time between frames. Carry on modelling; Bake again stops it."))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Reading(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SSurfViewportBar::GetStatus)),
				TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateSP(this, &SSurfViewportBar::GetStatusColour)), FText::GetEmpty())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).Visibility_Lambda([this]() { return GetDensity().IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })[ Dot() ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Reading(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SSurfViewportBar::GetDensity)),
				TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateSP(this, &SSurfViewportBar::GetDensityColour)),
				LOCTEXT("DensityTip", "Texels a metre across the selected patches, at the texture size set under Density. Amber when they do not match."))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).Visibility_Lambda([this]() { return GetStack().IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })[ Dot() ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Reading(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SSurfViewportBar::GetStack)),
				TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateSP(this, &SSurfViewportBar::GetStackColour)),
				LOCTEXT("StackTip", "The layer the selected patches are on: 0 is the opaque surface, 1, 2 and 3 draw over it in turn."))
		]
		// The keys take what width is left and end in an ellipsis when it is
		// not enough (S225a: "the circled area has text thats truncated" -
		// cut mid-word at the SidePanel's edge).
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(14.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox).Visibility(this, &SSurfViewportBar::GetTipVisibility)
			[
				SNew(STextBlock)
				.Text(this, &SSurfViewportBar::GetTip)
				.ColorAndOpacity(this, &SSurfViewportBar::GetStatusColour)
				.Font(SurfTheme::Font(8))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(SurfTheme::Shadow())
			]
		]
	];
}

TSharedRef<SWidget> SSurfViewportBar::BuildStatColumn()
{
	// "stats in bottom left !" - the numbers are read, never aimed at, so they
	// are text on the viewport and nothing else: no plate, no frame, no
	// button, nothing that takes a click away from the scene behind them. In
	// the bottom corner they are out of the way of the modelling entirely,
	// which is where every other tool puts its readout (S90a, S91a).
	return SNew(SVerticalBox)
		// A bake running in the background says so here and nowhere else
		// (S106a): the one line that is already read, and gone when there is
		// no bake, so nothing moves for the sake of it.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return Mode.IsValid() ? FText::FromString(Mode->BakeProgress()) : FText::GetEmpty();
			})
			.Visibility_Lambda([this]()
			{
				return Mode.IsValid() && Mode->IsBaking() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ColorAndOpacity(FStyleColors::AccentYellow)
			.ToolTipText(LOCTEXT("BakingTip", "A bake is running a slice at a time between frames. Carry on modelling; Bake again stops it."))
			.Font(SurfTheme::Bold(7))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).MaxDesiredWidth(300.0f)
			[
				SNew(STextBlock)
				.Text(this, &SSurfViewportBar::GetStatus)
				.ColorAndOpacity(this, &SSurfViewportBar::GetStatusColour)
				.Font(SurfTheme::Font(7))
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(this, &SSurfViewportBar::GetDensity)
			.ColorAndOpacity(this, &SSurfViewportBar::GetDensityColour)
			.ToolTipText(LOCTEXT("DensityTip", "Texels a metre across the selected patches, at the texture size set under Density. Amber when they do not match each other."))
			.Font(SurfTheme::Font(7))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(this, &SSurfViewportBar::GetStack)
			.ColorAndOpacity(this, &SSurfViewportBar::GetStackColour)
			.ToolTipText(LOCTEXT("StackTip", "The layer the selected patches are on: 0 is the opaque surface, 1, 2 and 3 draw over it in turn."))
			.Font(SurfTheme::Font(7))
		];
}

FReply SSurfViewportBar::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (bMovingBar)
	{
		// Which half of the viewport the pointer is in is which edge the bar
		// wants: dragging it across the middle puts it on the other side, and
		// dragging back brings it home (S91a).
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			const FGeometry& Mine = GetCachedGeometry();
			const FVector2D Local = Mine.AbsoluteToLocal(Event.GetScreenSpacePosition());
			Settings->bBarOnRight = Local.X > Mine.GetLocalSize().X * 0.5;
			// And down the edge (S165a): the handle follows the pointer, so
			// the column starts under the hand; never below the foot.
			const float Room = FMath::Max(0.0f, Mine.GetLocalSize().Y - 200.0f);
			Settings->ViewportBarTopPx = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Local.Y) - BarTopOffset - 60.0f), 0, FMath::RoundToInt(Room));
		}
		return FReply::Handled();
	}

	if (bLengtheningBar)
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			const float Moved = static_cast<float>(Event.GetScreenSpacePosition().Y) - BarDragStartY;
			Settings->ViewportBarLengthPx = FMath::Clamp(BarDragStartLength + FMath::RoundToInt(Moved), 60, 4000);
		}
		return FReply::Handled();
	}

	if (!bResizingBar)
	{
		return SCompoundWidget::OnMouseMove(Geometry, Event);
	}

	if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
	{
		// The grip is on the inner edge, so on the right-hand bar dragging
		// left is what makes it wider (S90a).
		const bool bOnRight = Settings->bBarOnRight;
		const float Moved = (static_cast<float>(Event.GetScreenSpacePosition().X) - BarDragStartX) * (bOnRight ? -1.0f : 1.0f);
		// Narrow enough for one column, wide enough for four; past that it
		// is a docked panel, and there is one of those already.
		Settings->ViewportBarWidthPx = FMath::Clamp(BarDragStartWidth + FMath::RoundToInt(Moved), 30, 640);
	}
	return FReply::Handled();
}

FReply SSurfViewportBar::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
	// Ctrl+wheel while arranging sizes the chips (S166a); the arrangement
	// rebuilds on the serial.
	if (SurfVerbLayout::IsEditing() && Event.IsControlDown())
	{
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->ViewportBarScale = FMath::Clamp(Settings->ViewportBarScale * (Event.GetWheelDelta() > 0.0f ? 1.1f : 1.0f / 1.1f), 0.7f, 2.0f);
			Settings->SaveConfig();
			SurfVerbLayout::Touch();
			SurfSession::Verb(TEXT("bar_scale"), FString::Printf(TEXT("%.2f"), Settings->ViewportBarScale));
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnMouseWheel(Geometry, Event);
}

FReply SSurfViewportBar::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (bMovingBar)
	{
		bMovingBar = false;
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->SaveConfig();
			SurfSession::Verb(TEXT("bar_side"), Settings->bBarOnRight ? TEXT("right") : TEXT("left"));
		}
		SurfVerbLayout::Touch();
		return FReply::Handled().ReleaseMouseCapture();
	}

	if (bLengtheningBar)
	{
		bLengtheningBar = false;
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->SaveConfig();
			SurfSession::Verb(TEXT("bar_length"), FString::FromInt(Settings->ViewportBarLengthPx));
		}
		return FReply::Handled().ReleaseMouseCapture();
	}

	if (!bResizingBar)
	{
		return SCompoundWidget::OnMouseButtonUp(Geometry, Event);
	}

	bResizingBar = false;
	if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
	{
		Settings->SaveConfig();
		SurfSession::Verb(TEXT("bar_width"), FString::FromInt(Settings->ViewportBarWidthPx));
	}
	return FReply::Handled().ReleaseMouseCapture();
}

float SSurfViewportBar::GetVerbWrapWidth() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const float Width = Settings ? static_cast<float>(Settings->ViewportBarWidthPx) : 138.0f;
	// The border and the scrollbar take their share before the verbs do.
	return FMath::Max(80.0f, Width - 16.0f);
}

namespace
{
	double GSurfBarPaintMs = 0.0;
}

double SurfTakeBarPaintMs()
{
	const double Out = GSurfBarPaintMs;
	GSurfBarPaintMs = 0.0;
	return Out;
}

int32 SSurfViewportBar::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const double Start = FPlatformTime::Seconds();
	const int32 Out = SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	GSurfBarPaintMs += (FPlatformTime::Seconds() - Start) * 1000.0;
	return Out;
}

FOptionalSize SSurfViewportBar::GetSidePanelMaxHeight() const
{
	// The bar's own height less the lines above the panel and the foot line under it.
	const float Mine = GetCachedGeometry().GetLocalSize().Y;
	if (Mine < 100.0f)
	{
		return FOptionalSize();
	}
	return FOptionalSize(FMath::Max(120.0f, Mine - 110.0f));
}

FOptionalSize SSurfViewportBar::GetBarMaxHeight() const
{
	// How tall the viewport is, in the units Slate lays out in: the pixels
	// the viewport reports, over this widget's own DPI scale. A margin at
	// the bottom keeps the last button clear of the viewport's own edge.
	// This widget's own height, less the two lines above the column, the
	// move handle, the shelf chip and the foot's room (S160c): the column
	// used to be sized from the viewport and ran off its bottom, taking the
	// shelf chip with it - "that extension overlaps with the bottom info
	// line".
	const float Mine = GetCachedGeometry().GetLocalSize().Y;
	if (Mine < 100.0f)
	{
		return FOptionalSize();
	}
	const USurfSettings* Top = GetDefault<USurfSettings>();
	float Room = FMath::Max(120.0f, Mine - 150.0f - (Top ? static_cast<float>(Top->ViewportBarTopPx) : 0.0f));
	// A length he set with the grip (S162a), never past the room there is.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (Settings && Settings->ViewportBarLengthPx > 0)
	{
		Room = FMath::Clamp(static_cast<float>(Settings->ViewportBarLengthPx), 60.0f, Room);
	}
	return FOptionalSize(Room);
}

TSharedRef<SWidget> SSurfViewportBar::BuildSubModeRow()
{
	TWeakObjectPtr<USurfEdMode> M = Mode;

	// Four chips on one line (S78). They were two rows of two wide buttons,
	// which cost a third of the bar's head for four numbers. The icon says
	// which kind of thing a click will pick, the number says how to get
	// there, and the one that is running is filled - which sub-mode is up
	// changes what every click means, so it has to read at a glance.
	// One Edit mode (S83a): Auto first, then the three kinds it decides
	// between, then Object. A chip is filled when the hand locked it, amber
	// when the last click settled there on its own - so the row says both
	// what is running and whether you or the click chose it.
	auto Chip = [M](ESurfIcon Icon, const FText& Name, const FText& Tip, ESurfSubMode Which) -> TSharedRef<SWidget>
	{
		const bool bIsObject = Which == ESurfSubMode::Object;
		auto IsOn = [M, Which, bIsObject]()
		{
			if (!M.IsValid())
			{
				return false;
			}
			return M->GetSubMode() == Which && (bIsObject || !M->IsAutoKind());
		};
		auto IsLive = [M, Which]() { return M.IsValid() && M->GetSubMode() == Which; };
        auto Tint = [IsOn, IsLive]()
        {
            if (IsOn())
            {
                return FSlateColor(FLinearColor::White);
            }
            // Blue while the click is deciding, plain while it is not.
            return IsLive() ? FStyleColors::AccentBlue : FStyleColors::Foreground;
        };

		// The icon alone (S192b), as the editor's own selection-mode switch:
		// the key is in the tip.
		// Object wears a box (S199a: "new icon for object mode ... better
		// read"); the three kinds keep their drawn marks.
		const FSlateBrush* BoxBrush = Icon == ESurfIcon::Object ? SurfIcons::Named(TEXT("box")) : nullptr;
		return ToggleButton(
			BoxBrush
				? StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
				[
					SNew(SImage)
					.Image(BoxBrush)
					.ColorAndOpacity_Lambda([Tint]() { return Tint(); })
				])
				: StaticCastSharedRef<SWidget>(SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
				[
					SNew(SSurfModeIcon)
					.Icon(Icon)
					.ColorAndOpacity_Lambda([Tint]() { return Tint(); })
				]),
			Tip, 24.0f, 20.0f, IsOn,
			[M, Which, bIsObject]()
			{
				if (!M.IsValid())
				{
					return;
				}
				if (bIsObject)
				{
					M->SetSubMode(ESurfSubMode::Object);
				}
				else
				{
					M->ToggleKindLock(Which);
				}
			});
	};

	// The switch between the two questions: is the click deciding, or have
	// you told it. Filled while it is deciding.
	auto AutoOn = [M]() { return M.IsValid() && M->AutoAppliesNow(); };
	TSharedRef<SWidget> AutoChip = ToggleButton(
		SNew(STextBlock)
		.Text(LOCTEXT("AutoName", "Auto"))
		.Margin(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
		.Font(SurfTheme::Bold(8))
		.ColorAndOpacity_Lambda([AutoOn]() { return AutoOn() ? SurfTheme::Bright() : SurfTheme::Text(); }),
		LOCTEXT("AutoTip", "Edit (Tab) - the click decides: a vertex, else an edge, else a face. 1 2 3 lock one kind; again to let go."),
		31.0f, 18.0f, AutoOn,
		[M]() { if (M.IsValid()) { if (M->AutoAppliesNow()) { M->SetAutoKind(false); } else { M->EnterEditKinds(); } } });

	// Each has its verb's right-click (S204a): "right click anything".
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ ChipWithVerb(AutoChip, QuickVerbs, TEXT("Auto")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ ChipWithVerb(Chip(ESurfIcon::Vertex, LOCTEXT("VertName", "1"), LOCTEXT("VertTip", "Vertices (1) - Alt-click a row or column, Ctrl+Alt adds one. Press 1 again to let the click decide."), ESurfSubMode::Vertex), QuickVerbs, TEXT("Vertices")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ ChipWithVerb(Chip(ESurfIcon::Edge, LOCTEXT("EdgeName", "2"), LOCTEXT("EdgeTip", "Edges (2) - click one, Alt-click the whole line"), ESurfSubMode::Edge), QuickVerbs, TEXT("Edges")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ ChipWithVerb(Chip(ESurfIcon::Face, LOCTEXT("FaceName", "3"), LOCTEXT("FaceTip", "Faces (3) - Alt-click a row of faces, E pulls a block out of them"), ESurfSubMode::Face), QuickVerbs, TEXT("Faces")) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ ChipWithVerb(Chip(ESurfIcon::Object, LOCTEXT("ObjName", "4"), LOCTEXT("ObjTip", "Pieces (Tab or 4) - whole patches and primitives; Shift-click adds, Ctrl-click takes out."), ESurfSubMode::Object), QuickVerbs, TEXT("Pieces")) ]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNullWidget::NullWidget ];
}

namespace
{
	/**
	 * A switch's menu arrow (S206a): "make the dropdown arrow separate. and
	 * simply hovering opens the menu. so dont need to click (and therefore
	 * switch) to open". The switch beside it only toggles; resting on the
	 * arrow a moment opens the menu under it, and a click on it does too.
	 */
	class SSurfHoverArrow : public SCompoundWidget
	{
	public:
		using FOpen = TFunction<TSharedPtr<IMenu>(const FVector2D&)>;

		SLATE_BEGIN_ARGS(SSurfHoverArrow) {}
			SLATE_ARGUMENT(FText, Tip)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, FOpen InOpen)
		{
			Open = MoveTemp(InOpen);
			SetToolTipText(InArgs._Tip);
			ChildSlot
			[
				SNew(SBorder)
				.BorderImage_Lambda([this]() { return IsHovered() || Menu.IsValid() ? &HoverBrush : &RestBrush; })
				.Padding(FMargin(3.0f, 0.0f))
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				[
					SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
					[
						SNew(SImage).Image(FAppStyle::GetBrush("Icons.ChevronDown")).ColorAndOpacity(SurfTheme::Quiet())
					]
				]
			];
		}

		virtual void OnMouseEnter(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			SCompoundWidget::OnMouseEnter(Geometry, Event);
			if (!Timer.IsValid())
			{
				Timer = RegisterActiveTimer(0.18f, FWidgetActiveTimerDelegate::CreateSP(this, &SSurfHoverArrow::AfterRest));
			}
		}

		virtual void OnMouseLeave(const FPointerEvent& Event) override
		{
			SCompoundWidget::OnMouseLeave(Event);
			if (TSharedPtr<FActiveTimerHandle> Live = Timer.Pin())
			{
				UnRegisterActiveTimer(Live.ToSharedRef());
			}
			Timer.Reset();
		}

		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				OpenNow();
				return FReply::Handled();
			}
			return FReply::Unhandled();
		}

	private:
		EActiveTimerReturnType AfterRest(double, float)
		{
			Timer.Reset();
			if (IsHovered())
			{
				OpenNow();
			}
			return EActiveTimerReturnType::Stop;
		}

		void OpenNow()
		{
			if (Menu.IsValid() || !Open)
			{
				return;
			}
			const FGeometry& Geometry = GetCachedGeometry();
			Menu = Open(Geometry.LocalToAbsolute(FVector2D(0.0f, Geometry.GetLocalSize().Y)));
		}

		FOpen Open;
		TWeakPtr<FActiveTimerHandle> Timer;
		TWeakPtr<IMenu> Menu;
		FSlateRoundedBoxBrush RestBrush = FSlateRoundedBoxBrush(FStyleColors::Recessed, 2.0f);
		FSlateRoundedBoxBrush HoverBrush = FSlateRoundedBoxBrush(FStyleColors::Hover, 2.0f);
	};
}

TSharedRef<SWidget> SSurfViewportBar::BuildSoftRow()
{
	TWeakObjectPtr<USurfEdMode> M = Mode;

	// One row for what is on and off (S78): soft selection, point snapping,
	// and whether snapping reaches the level's meshes. It was two rows with
	// four 18-pixel steppers between them, which read as broken boxes at a
	// glance and could not be hit without aiming. The numbers are in the
	// labels now - "Soft 60", "Snap 22" - and the steppers are on the
	// right-click of the thing they size, where the rest of the options are.
	auto SoftOn = []()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Settings->bSoftSelection;
	};
	auto SnapOn = []()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Settings->bSnapEnabled;
	};

	auto SoftMenu = [M](FMenuBuilder& Menu)
	{
		// The reach as a number (S230a: "improve soft selection menu - perhaps
		// more icon things"): a field, in centimetres; the wheel during a
		// transform still nudges it.
		Menu.BeginSection("SoftRadius", LOCTEXT("SoftRadiusHeading", "Reach (cm)"));
		Menu.AddWidget(Spin(
			TAttribute<double>::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? S->SoftSelectionRadiusCm : 200.0; }),
			USurfSettings::SoftReachMinCm, 5000.0,
			[M](double V)
			{
				if (USurfSettings* S = GetMutableDefault<USurfSettings>())
				{
					S->SoftSelectionRadiusCm = FMath::Max(USurfSettings::SoftReachMinCm, V);
					S->bSoftSelection = true;
					S->SaveConfig();
					if (M.IsValid()) { M->RefreshSoftSelection(); if (M->Inspector) { M->Inspector->PullSoftSelection(); } M->RedrawViewports(); }
				}
			},
			[M]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->SoftSelectionRadiusCm = 200.0; S->SaveConfig(); if (M.IsValid()) { M->RefreshSoftSelection(); } } }),
			LOCTEXT("SoftReach", "Reach"));
		Menu.EndSection();
		Menu.BeginSection("Profile", LOCTEXT("SoftProfileHeading", "Falloff (Shift+O cycles)"));
		const ESurfFalloff All[] = { ESurfFalloff::Smooth, ESurfFalloff::Sphere, ESurfFalloff::Root, ESurfFalloff::InverseSquare,
			ESurfFalloff::Sharp, ESurfFalloff::Linear, ESurfFalloff::Constant, ESurfFalloff::Random };
		static const TCHAR* Drawn[] = { TEXT("falloff-smooth"), TEXT("falloff-sphere"), TEXT("falloff-root"), TEXT("falloff-inverse-square"),
			TEXT("falloff-sharp"), TEXT("falloff-linear"), TEXT("falloff-constant"), TEXT("falloff-random") };
		for (int32 I = 0; I < 8; ++I)
		{
			const ESurfFalloff Which = All[I];
			// Each with its curve drawn (S230a): the shape of the falloff at a glance.
			Menu.AddMenuEntry(FText::FromString(USurfSettings::FalloffName(Which)), FText::GetEmpty(),
				FSlateIcon(TEXT("SurfacingProStyle"), FName(*(FString(TEXT("SurfacingPro.Verb.")) + Drawn[I]))),
				FUIAction(
					FExecuteAction::CreateLambda([M, Which]() { if (M.IsValid()) { M->SetSoftFalloff(Which); } }),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->SoftSelectionFalloff == Which; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
		Menu.EndSection();
		Menu.BeginSection("SoftMeasure", LOCTEXT("SoftMeasureHeading", "Measured"));
		Menu.AddMenuEntry(LOCTEXT("SoftAlongGrid", "Along the grid"), LOCTEXT("SoftAlongGridTip", "The reach walks the control net from the selection, a segment at a time, instead of measuring through the air - so a point across a fold is as far as the walk to it."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([M]()
				{
					if (USurfSettings* S = GetMutableDefault<USurfSettings>())
					{
						S->bSoftAlongTheGrid = !S->bSoftAlongTheGrid;
						S->SaveConfig();
						if (M.IsValid()) { M->RefreshSoftSelection(); }
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bSoftAlongTheGrid; })),
			NAME_None, EUserInterfaceActionType::ToggleButton);
		Menu.EndSection();
	};

	/**
	 * The snap menu (S198a), lightened (S199a): "a lot of text ... could be
	 * simply using collapsed sections, submenus". On top the switch and the
	 * three targets; everything set once a session folded into submenus.
	 * Every line's tip says the rest.
	 */
	auto SnapMenu = [](FMenuBuilder& Menu)
	{
		auto Settle = [](TFunction<void(USurfSettings&)> Do)
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				Do(*S);
				S->SaveConfig();
			}
		};
		auto Is = [](TFunction<bool(const USurfSettings&)> Ask)
		{
			return [Ask]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && Ask(*S); };
		};

		AddToggle(Menu, FText::Format(LOCTEXT("SnapSwitchShort", "Snap   {0}"), FText::FromString(SurfKeymap::Describe(ESurfAction::ToggleSnap))),
			LOCTEXT("SnapSwitchTip", "Off, nothing below snaps. Ctrl held during a move flips it for as long as it is held - and the editor's move, rotation and scale steps with it."),
			Is([](const USurfSettings& S) { return S.bSnapEnabled; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapEnabled = V; }); });
		Menu.AddSeparator();
		AddToggle(Menu, LOCTEXT("SnapToVertex", "Vertex"),
			LOCTEXT("SnapToVertexTip", "A dragged point lands exactly on a control point or a corner within reach."),
			Is([](const USurfSettings& S) { return S.bSnapToPoints; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToPoints = V; }); });
		AddToggle(Menu, LOCTEXT("SnapToEdge", "Edge"),
			LOCTEXT("SnapToEdgeTip", "The nearest point of an edge of ours - a patch's cage line, a primitive's edge - when no point is in reach."),
			Is([](const USurfSettings& S) { return S.bSnapToEdges; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToEdges = V; }); });
		AddToggle(Menu, LOCTEXT("SnapToEdgeCentre", "Edge centre"),
			LOCTEXT("SnapToEdgeCentreTip", "The middle of an edge of ours."),
			Is([](const USurfSettings& S) { return S.bSnapToEdgeCentres; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToEdgeCentres = V; }); });
		AddToggle(Menu, LOCTEXT("SnapToEdgePerp", "Edge perpendicular"),
			LOCTEXT("SnapToEdgePerpTip", "Square onto an edge of ours: the foot of the perpendicular from where the point started - walls meeting at right angles."),
			Is([](const USurfSettings& S) { return S.bSnapToEdgePerpendicular; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToEdgePerpendicular = V; }); });
		AddToggle(Menu, LOCTEXT("SnapToFace", "Face"),
			LOCTEXT("SnapSurfacesOptTip", "With no point under the cursor, the moving point nearest the cursor lands on whatever surface the cursor is over - another piece's, or the level's."),
			Is([](const USurfSettings& S) { return S.bSnapToSurfaces; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToSurfaces = V; }); });
		AddToggle(Menu, LOCTEXT("SnapToIncrement", "Increment"),
			LOCTEXT("SnapToIncrementTip", "With nothing to land on, a dragged point settles on a step (Increment size)."),
			Is([](const USurfSettings& S) { return S.PointSnap != ESurfPointSnap::Off; }),
			[Settle](bool V) { Settle([V](USurfSettings& S) { S.PointSnap = V ? ESurfPointSnap::Fine : ESurfPointSnap::Off; }); });
		Menu.AddSeparator();

		// Which point lands (S205a).
		Menu.AddSubMenu(LOCTEXT("SnapWithSub", "Snap with"), LOCTEXT("SnapWithSubTip", "Which point of what moves is the one that lands on the target."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				struct FWith { ESurfSnapWith With; FText Label; FText Tip; };
				const FWith Withs[] = {
					{ ESurfSnapWith::Closest, LOCTEXT("SnapWithClosest", "Closest"), LOCTEXT("SnapWithClosestTip", "Whichever moving point is nearest a target.") },
					{ ESurfSnapWith::Centre, LOCTEXT("SnapWithCentre", "Centre"), LOCTEXT("SnapWithCentreTip", "The middle of the moving points' box.") },
					{ ESurfSnapWith::Median, LOCTEXT("SnapWithMedian", "Median"), LOCTEXT("SnapWithMedianTip", "The average of the moving points.") },
					{ ESurfSnapWith::Active, LOCTEXT("SnapWithActive", "Active"), LOCTEXT("SnapWithActiveTip", "The point picked last.") },
				};
				for (const FWith& Each : Withs)
				{
					const ESurfSnapWith Which = Each.With;
					Sub.AddMenuEntry(Each.Label, Each.Tip, FSlateIcon(),
						FUIAction(
							FExecuteAction::CreateLambda([Which]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->SnapWith = Which; S->SaveConfig(); } }),
							FCanExecuteAction(),
							FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->SnapWith == Which; })),
						NAME_None, EUserInterfaceActionType::RadioButton);
				}
			}));

		Menu.AddSubMenu(LOCTEXT("SnapTargetsSub", "Targets"), LOCTEXT("SnapTargetsSubTip", "What else it may land on."),
			FNewMenuDelegate::CreateLambda([Settle, Is](FMenuBuilder& Sub)
			{
				AddToggle(Sub, LOCTEXT("SnapMeshesShort", "Level meshes"), LOCTEXT("SnapMeshesOptTip", "Their corners first, else the nearest point on an edge or a face."),
					Is([](const USurfSettings& S) { return S.bSnapToMeshes; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapToMeshes = V; }); });
				AddToggle(Sub, LOCTEXT("SnapWithinShort", "Own piece"), LOCTEXT("SnapWithinOptTip", "Points of the patch being edited. Off by default: a point landing on its own neighbour is how a patch collapses by accident."),
					Is([](const USurfSettings& S) { return S.bSnapWithinPatch; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapWithinPatch = V; }); });
				AddToggle(Sub, LOCTEXT("SnapAlignShort", "Axis guides"), LOCTEXT("SnapAlignOptTip", "With nothing in reach: land on the X, Y or Z line through any other point that passes within reach - a dotted guide shows which."),
					Is([](const USurfSettings& S) { return S.bSnapAlign; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapAlign = V; }); });
				AddToggle(Sub, LOCTEXT("SnapTurnShort", "Stand on the face"), LOCTEXT("SnapTurnOptTip", "A placed mesh, prefab or level that lands on a surface under G turns to stand on it - its up along the surface."),
					Is([](const USurfSettings& S) { return S.bSnapTurnsToFace; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapTurnsToFace = V; }); });
				AddToggle(Sub, LOCTEXT("SnapDroppedShort", "Dropped assets"), LOCTEXT("SnapDroppedOptTip", "A mesh or a prefab dragged in from the Content Browser lands on the grid rather than wherever the cursor's ray met something."),
					Is([](const USurfSettings& S) { return S.bSnapDroppedActors; }), [Settle](bool V) { Settle([V](USurfSettings& S) { S.bSnapDroppedActors = V; }); });
			}));

		const USurfSettings* Now = GetDefault<USurfSettings>();
		Menu.AddSubMenu(FText::Format(LOCTEXT("SnapReachSub", "Reach   {0} cm"), FText::AsNumber(FMath::RoundToInt(Now ? Now->PointSnapRadiusCm : 0.0))),
			LOCTEXT("SnapReachSubTip", "How close something has to be to be landed on. Ctrl+wheel during a move, or Shift+[ ]."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				auto Nudge = [](double Factor)
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->PointSnapRadiusCm = FMath::Clamp(Settings->PointSnapRadiusCm * Factor, USurfSettings::SnapReachMinCm, USurfSettings::SnapReachMaxCm);
						Settings->SaveConfig();
					}
				};
				Sub.AddMenuEntry(LOCTEXT("SnapShorter", "Shorter"), LOCTEXT("SnapShorterTip", "Land only on something closer."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Nudge]() { Nudge(1.0 / 1.5); })));
				Sub.AddMenuEntry(LOCTEXT("SnapLonger", "Longer"), LOCTEXT("SnapLongerTip", "Land on something further away."), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Nudge]() { Nudge(1.5); })));
			}));

		Menu.AddSubMenu(LOCTEXT("SnapIncrementSub", "Increment size"), LOCTEXT("SnapIncrementSubTip", "The step a dragged point settles on when Increment is on."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				const ESurfPointSnap Modes[] = { ESurfPointSnap::Fine, ESurfPointSnap::SameAsGrid };
				const FText Names[] = { LOCTEXT("PointSnapFineShort", "Fine (1 cm)"), LOCTEXT("PointSnapGridShort", "The grid step") };
				for (int32 I = 0; I < 2; ++I)
				{
					const ESurfPointSnap Which = Modes[I];
					Sub.AddMenuEntry(Names[I], FText::GetEmpty(), FSlateIcon(),
						FUIAction(
							FExecuteAction::CreateLambda([Which]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->PointSnap = Which; S->SaveConfig(); } }),
							FCanExecuteAction(),
							FIsActionChecked::CreateLambda([Which]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->PointSnap == Which; })),
						NAME_None, EUserInterfaceActionType::RadioButton);
				}
			}));

		Menu.AddSubMenu(LOCTEXT("SnapGridSub", "Grid step"), LOCTEXT("SnapGridSubTip", "The step primitives are drawn and moved in."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				const USurfSettings* Ladder = GetDefault<USurfSettings>();
				for (int32 I = 0; Ladder && I < Ladder->GridLadderCm.Num(); ++I)
				{
					const double Step = Ladder->GridLadderCm[I];
					Sub.AddMenuEntry(
						Step >= 100.0 ? FText::FromString(FString::Printf(TEXT("%.2g m"), Step * 0.01)) : FText::FromString(FString::Printf(TEXT("%.4g cm"), Step)),
						FText::GetEmpty(), FSlateIcon(),
						FUIAction(
							FExecuteAction::CreateLambda([I]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->DefaultGridIndex = I; S->SaveConfig(); } }),
							FCanExecuteAction(),
							FIsActionChecked::CreateLambda([I]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->DefaultGridIndex == I; })),
						NAME_None, EUserInterfaceActionType::RadioButton);
				}
			}));

		// The editor's own steps (S198a): the same switches and sizes as its
		// viewport toolbar - "thinking unreal would be easier for user".
		Menu.AddSubMenu(LOCTEXT("SnapUnrealSub", "Move, rotate, scale steps"), LOCTEXT("SnapUnrealSubTip", "Unreal's own grid, rotation and scale steps - the same as its viewport toolbar."),
			FNewMenuDelegate::CreateLambda([](FMenuBuilder& Sub)
			{
				// The three switches are bitfields: reached by number, not by address.
				auto Flip = [](int32 Which)
				{
					return [Which](bool On)
					{
						if (ULevelEditorViewportSettings* W = GetMutableDefault<ULevelEditorViewportSettings>())
						{
							if (Which == 0) { W->GridEnabled = On; }
							else if (Which == 1) { W->RotGridEnabled = On; }
							else { W->SnapScaleEnabled = On; }
							W->SaveConfig();
						}
					};
				};
				auto Read = [](int32 Which)
				{
					return [Which]() -> bool
					{
						const ULevelEditorViewportSettings* R = GetDefault<ULevelEditorViewportSettings>();
						if (!R) { return false; }
						return Which == 0 ? !!R->GridEnabled : (Which == 1 ? !!R->RotGridEnabled : !!R->SnapScaleEnabled);
					};
				};
				const bool bLive = GEditor != nullptr;
				AddToggle(Sub, FText::Format(LOCTEXT("SnapAffectMoveShort", "Move   {0}"), bLive ? FText::FromString(FString::Printf(TEXT("%g cm"), GEditor->GetGridSize())) : FText::GetEmpty()),
					LOCTEXT("SnapAffectMoveTip", "Primitives and actors move in the editor's grid steps - its toolbar's grid switch."), Read(0), Flip(0));
				AddToggle(Sub, FText::Format(LOCTEXT("SnapAffectTurnShort", "Rotate   {0}"), bLive ? FText::FromString(FString::Printf(TEXT("%g°"), GEditor->GetRotGridSize().Yaw)) : FText::GetEmpty()),
					LOCTEXT("SnapAffectTurnTip", "Turns go in the editor's rotation steps - its toolbar's rotation switch."), Read(1), Flip(1));
				AddToggle(Sub, FText::Format(LOCTEXT("SnapAffectSizeShort", "Scale   {0}"), bLive ? FText::FromString(FString::Printf(TEXT("%g"), GEditor->GetScaleGridSize())) : FText::GetEmpty()),
					LOCTEXT("SnapAffectSizeTip", "Sizes go in the editor's scale steps - its toolbar's scale switch."), Read(2), Flip(2));
				if (!bLive)
				{
					return;
				}
				Sub.AddSeparator();
				Sub.AddSubMenu(LOCTEXT("SnapAffectMoveSize", "Grid step"), FText::GetEmpty(), FNewMenuDelegate::CreateLambda([](FMenuBuilder& Step)
				{
					const ULevelEditorViewportSettings* R = GetDefault<ULevelEditorViewportSettings>();
					const TArray<float>& Sizes = R->bUsePowerOf2SnapSize ? R->Pow2GridSizes : R->DecimalGridSizes;
					for (int32 I = 0; I < Sizes.Num(); ++I)
					{
						Step.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%g cm"), Sizes[I])), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([I]() { if (GEditor) { GEditor->SetGridSize(I); } }), FCanExecuteAction(),
								FIsActionChecked::CreateLambda([I]() { return GetDefault<ULevelEditorViewportSettings>()->CurrentPosGridSize == I; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
				}));
				Sub.AddSubMenu(LOCTEXT("SnapAffectTurnSize", "Rotation step"), FText::GetEmpty(), FNewMenuDelegate::CreateLambda([](FMenuBuilder& Step)
				{
					const ULevelEditorViewportSettings* R = GetDefault<ULevelEditorViewportSettings>();
					for (int32 I = 0; I < R->CommonRotGridSizes.Num(); ++I)
					{
						Step.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%g°"), R->CommonRotGridSizes[I])), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([I]() { if (GEditor) { GEditor->SetRotGridSize(I, GridMode_Common); } }), FCanExecuteAction(),
								FIsActionChecked::CreateLambda([I]()
								{
									const ULevelEditorViewportSettings* C = GetDefault<ULevelEditorViewportSettings>();
									return C->CurrentRotGridMode == GridMode_Common && C->CurrentRotGridSize == I;
								})),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
				}));
				Sub.AddSubMenu(LOCTEXT("SnapAffectSizeSize", "Scale step"), FText::GetEmpty(), FNewMenuDelegate::CreateLambda([](FMenuBuilder& Step)
				{
					const ULevelEditorViewportSettings* R = GetDefault<ULevelEditorViewportSettings>();
					for (int32 I = 0; I < R->ScalingGridSizes.Num(); ++I)
					{
						Step.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%g"), R->ScalingGridSizes[I])), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([I]() { if (GEditor) { GEditor->SetScaleGridSize(I); } }), FCanExecuteAction(),
								FIsActionChecked::CreateLambda([I]() { return GetDefault<ULevelEditorViewportSettings>()->CurrentScalingGridSize == I; })),
							NAME_None, EUserInterfaceActionType::RadioButton);
					}
				}));
			}));
	};

	// A toggle whose right-click carries what used to be beside it.
	// Its right-click also carries the button's own verb lines (S199a): "i
	// need be able right clic anything" - the Q menu, the sidebar.
	auto WithMenu = [this](TSharedRef<SWidget> Button, TFunction<void(FMenuBuilder&)> Options, const TCHAR* VerbLabel = nullptr) -> TSharedRef<SWidget>
	{
		const FSurfVerb* Found = VerbLabel ? QuickVerbs.FindByPredicate([VerbLabel](const FSurfVerb& V) { return V.Label.ToString() == VerbLabel; }) : nullptr;
		TSharedPtr<FSurfVerb> Own = Found ? TSharedPtr<FSurfVerb>(MakeShared<FSurfVerb>(*Found)) : TSharedPtr<FSurfVerb>();
		return SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			.OnMouseButtonDown_Lambda([this, Options, Own](const FGeometry& Geometry, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() == EKeys::RightMouseButton && FSlateApplication::IsInitialized())
				{
					FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
					Options(Menu);
					if (Own.IsValid())
					{
						Menu.AddSeparator();
						SurfMenu::AddVerbLines(Menu, *Own);
					}
					const FVector2D Anchor = Geometry.LocalToAbsolute(FVector2D(Geometry.GetLocalSize().X + 4.0f, 0.0f));
					FSlateApplication::Get().PushMenu(AsShared(), FWidgetPath(), Menu.MakeWidget(), Anchor,
						FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			[
				Button
			];
	};

	// A Tabler mark and the digit (S206a): "improve icon readability for snap and softselect".
	auto MarkAnd = [](const TCHAR* Mark, TAttribute<FText> Number, TAttribute<FSlateColor> Tint) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SImage)
				.Image(SurfIcons::Named(Mark))
				.DesiredSizeOverride(FVector2D(14.0, 14.0))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(3.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Number)
				.Font(SurfTheme::Font(8))
				.ColorAndOpacity(Tint.IsSet() ? Tint : TAttribute<FSlateColor>(FSlateColor::UseForeground()))
			];
	};

	// A menu pushed at a point, for the arrows: kept open while a tick in it is clicked.
	auto OpenAt = [this](TFunction<void(FMenuBuilder&)> Fill) -> SSurfHoverArrow::FOpen
	{
		TWeakPtr<SWidget> Owner = AsShared();
		return [Owner, Fill](const FVector2D& At) -> TSharedPtr<IMenu>
		{
			TSharedPtr<SWidget> Parent = Owner.Pin();
			if (!Parent.IsValid() || !FSlateApplication::IsInitialized())
			{
				return nullptr;
			}
			FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
			Fill(Menu);
			return FSlateApplication::Get().PushMenu(Parent.ToSharedRef(), FWidgetPath(), Menu.MakeWidget(), At,
				FPopupTransitionEffect(FPopupTransitionEffect::ComboButton));
		};
	};

	auto IconOnly = [](ESurfIcon Icon) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
		[
			SNew(SSurfModeIcon).Icon(Icon)
		];
	};

	return SNew(SHorizontalBox)
		// Soft and Snap (S206a): a clearer mark (a magnet for snapping, the
		// falloff's rings for soft), a click that switches, and the arrow on
		// its own beside it - resting on the arrow opens the menu.
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			WithMenu(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					ToggleButton(
						MarkAnd(TEXT("target"), TAttribute<FText>::CreateLambda([]()
						{
							const USurfSettings* S = GetDefault<USurfSettings>();
							return S ? FText::AsNumber(USurfSettings::ReachDigit(S->SoftSelectionRadiusCm, USurfSettings::SoftReachMinCm, USurfSettings::SoftReachMaxCm)) : FText::GetEmpty();
						}),
						/**
						 * Amber while the falloff is not the usual one (S137a):
						 * the random falloff makes a rough surface on purpose,
						 * and with nothing saying so it reads as a broken mesh.
						 */
						TAttribute<FSlateColor>::CreateLambda([]()
						{
							const USurfSettings* S = GetDefault<USurfSettings>();
							if (S && !USurfSettings::FalloffIsUsual(S->SoftSelectionFalloff))
							{
								return SurfTheme::Warn();
							}
							return FSlateColor::UseForeground();
						})),
						LOCTEXT("SoftTip", "Proportional editing: unselected points follow, by distance (O). Click: on and off. The digit is the reach, 0 (10 cm) to 9 (50 m); the wheel sizes it while a move is running. Amber while the falloff is not the usual smooth one. The arrow beside it: the reach and the falloff."),
						32.0f, 18.0f, SoftOn,
						[M]() { if (M.IsValid()) { M->ToggleSoftSelection(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox).HeightOverride(18.0f)
					[
						SNew(SSurfHoverArrow, OpenAt(SoftMenu))
						.Tip(LOCTEXT("SoftArrowTip", "Soft selection's menu: the reach and the falloff."))
					]
				],
				SoftMenu, TEXT("Soft"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			WithMenu(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					ToggleButton(
						MarkAnd(TEXT("magnet"), TAttribute<FText>::CreateLambda([]()
						{
							const USurfSettings* S = GetDefault<USurfSettings>();
							return S ? FText::AsNumber(USurfSettings::ReachDigit(S->PointSnapRadiusCm, USurfSettings::SnapReachMinCm, USurfSettings::SnapReachMaxCm)) : FText::GetEmpty();
						}), TAttribute<FSlateColor>()),
						FText::Format(LOCTEXT("SnapTip", "Snapping. Click: on and off ({0} too); Ctrl held during a move flips it. The digit is the reach, 0 (5 mm) to 9 (5 m); Ctrl+wheel sizes it while a move is running. The arrow beside it: what it lands on, the reach, the steps."),
							FText::FromString(SurfKeymap::Describe(ESurfAction::ToggleSnap))),
						32.0f, 18.0f, SnapOn,
						[]()
						{
							if (USurfSettings* S = GetMutableDefault<USurfSettings>())
							{
								S->bSnapEnabled = !S->bSnapEnabled;
								S->SaveConfig();
								SurfSession::Verb(TEXT("snap"), S->bSnapEnabled ? TEXT("on") : TEXT("off"));
							}
						})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox).HeightOverride(18.0f)
					[
						SNew(SSurfHoverArrow, OpenAt(SnapMenu))
						.Tip(LOCTEXT("SnapArrowTip", "The snap menu: what it lands on, the reach, the steps."))
					]
				],
				SnapMenu, TEXT("Snap"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			ChipWithVerb(ToggleButton(
				IconOnly(ESurfIcon::Mesh),
				LOCTEXT("SnapMeshesTip", "Snapping lands on the level's static meshes too, within the same reach: their corners first, else the nearest point on an edge or a face."),
				22.0f, 18.0f,
				[]() { const USurfSettings* Settings = GetDefault<USurfSettings>(); return Settings && Settings->bSnapToMeshes; },
				[]()
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->bSnapToMeshes = !Settings->bSnapToMeshes;
						Settings->SaveConfig();
					}
				}), QuickVerbs, TEXT("Meshes"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNullWidget::NullWidget ];
}

/**
 * Where you are and what just happened, in a frame of their own at the head
 * of the bar (S78). This used to be a line painted across the top of the
 * viewport, which a docked panel covered and a narrow viewport cut in half:
 * "the text displayed in viewport is hidden by other windows". A frame in
 * the bar cannot be covered by anything, and it is where the eye already is.
 */
TSharedRef<SWidget> SSurfViewportBar::BuildNavSteps()
{
	// "perhaps navigation bar? simple toolbar icons?" - in, out, and the two
	// toggles that change what the level shows you, on one 16-pixel line at
	// the head of the frame that already says where you are (S83a). The
	// verbs themselves are the pool's, found by name, so the arrow and the
	// button do exactly the same thing.
	auto Find = [this](const TCHAR* Label) -> const FSurfVerb*
	{
		return QuickVerbs.FindByPredicate([Label](const FSurfVerb& Verb) { return Verb.Label.ToString() == Label; });
	};

	const FSurfVerb* OpenVerb = Find(TEXT("Open"));
	const FSurfVerb* CloseVerb = Find(TEXT("Close"));

	auto Step = [](const FSurfVerb* Verb, ESurfIcon Which, const FText& Tip) -> TSharedRef<SWidget>
	{
		TFunction<void()> Run = Verb ? Verb->Run : TFunction<void()>();
		TFunction<bool()> Enabled = Verb ? Verb->IsEnabled : TFunction<bool()>();
		// Its verb's right-click (S199a).
		TSharedPtr<FSurfVerb> Own = Verb ? TSharedPtr<FSurfVerb>(MakeShared<FSurfVerb>(*Verb)) : TSharedPtr<FSurfVerb>();
		TSharedRef<TWeakPtr<SWidget>> Self = MakeShared<TWeakPtr<SWidget>>();

		TSharedRef<SWidget> Made = SNew(SBox).WidthOverride(22.0f).HeightOverride(18.0f)
		[
			SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			.OnMouseButtonDown_Lambda([Own, Self](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Own.IsValid() && Event.GetEffectingButton() == EKeys::RightMouseButton)
				{
					SurfMenu::OpenVerbContext(*Own, Self->Pin());
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			[
			SNew(SButton)
			.ButtonStyle(&IconButtonStyle())
			.ContentPadding(FMargin(0.0f))
			.ToolTipText(Tip)
			.IsEnabled_Lambda([Enabled]() { return Enabled ? Enabled() : true; })
			.OnClicked_Lambda([Run]() { if (Run) { Run(); } return FReply::Handled(); })
			[
				// The editor's own chevrons (S175a): "arrows don't look ok" -
				// drawn marks of ours read as a second hand beside UE's.
				SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(12.0f).HeightOverride(12.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(Which == ESurfIcon::Out ? TEXT("Icons.ChevronUp") : TEXT("Icons.ChevronDown")))
						.ColorAndOpacity_Lambda([Enabled]()
						{
							// In the level's tint (S199a), grey when there is nowhere to go.
							const bool bLive = Enabled ? Enabled() : true;
							return FSlateColor(bLive
								? SurfTheme::Linear(SurfTheme::Level())
								: SurfTheme::DimLinear());
						})
					]
				]
			]
			]
		];
		*Self = Made;
		return Made;
	};

	/**
	 * C: open with the context (S174a) - "next to the up/down arrow of the
	 * bar, add a C button: it enables context upon going down." On, In
	 * opens a prefab where it stands with the level ghosted round it; off,
	 * alone at the origin. The same choice as Open's frame.
	 */
	// A ghost for the C (S199a): the level ghosted round what is opened.
	auto ContextOn = []() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bEditPrefabsInPlace; };
	TSharedRef<SWidget> Context = ToggleButton(
		SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(13.0f).HeightOverride(13.0f)
			[
				SNew(SImage)
				.Image(SurfIcons::Named(TEXT("ghost-3")))
				.ColorAndOpacity_Lambda([ContextOn]() { return ContextOn() ? SurfTheme::On() : FSlateColor::UseForeground(); })
			]
		],
		LOCTEXT("ContextTip", "Context: In opens a prefab where it stands, the level ghosted round it. Off: alone at the origin."),
		16.0f, 18.0f,
		[]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bEditPrefabsInPlace; },
		[]() { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bEditPrefabsInPlace = !S->bEditPrefabsInPlace; S->SaveConfig(); } });

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			ChipWithVerb(Context, QuickVerbs, TEXT("Context"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			Step(CloseVerb, ESurfIcon::Out, LOCTEXT("NavOutTip", "Out (Alt+PgUp) - come back up a level; what you were editing is kept and saved, and every instance of it follows."))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			Step(OpenVerb, ESurfIcon::In, LOCTEXT("NavInTip", "In (Alt+PgDn) - open what is selected: a prefab, or a level."))
		];
}

TSharedRef<SWidget> SSurfViewportBar::BuildNavRow()
{
	auto Switch = [](ESurfIcon Icon, const FText& Tip, TFunction<bool()> IsOn, TFunction<void()> Flip) -> TSharedRef<SWidget>
	{
		return ToggleButton(
			SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
			[
				SNew(SSurfModeIcon).Icon(Icon)
			],
			Tip, 22.0f, 18.0f, IsOn, Flip);
	};

	auto NamesOn = []()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Settings->bShowNames;
	};
	auto MarksOn = []()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return !Settings || Settings->bShowPrefabMarks;
	};

	// Names and Marks: what the level draws, rather than where you are.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			Switch(ESurfIcon::Name, LOCTEXT("NavNamesTip", "Draw what things are called in the viewport, on a plate, like the editor's own labels."),
				NamesOn,
				[NamesOn]()
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->bShowNames = !NamesOn();
						Settings->SaveConfig();
					}
				})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[
			Switch(ESurfIcon::Mark, LOCTEXT("NavMarksTip", "Draw a placed prefab's origin, faintly - it says where the thing is anchored, and that it is placed rather than open. Its box is Prefab Bounds (Show menu, settings)."),
				MarksOn,
				[MarksOn]()
				{
					if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
					{
						Settings->bShowPrefabMarks = !MarksOn();
						Settings->SaveConfig();
					}
				})
		];
}

FText SSurfViewportBar::GetWhere() const
{
	USurfEdMode* M = Mode.Get();
	if (!M)
	{
		return LOCTEXT("WhereNone", "SurfacingPro");
	}
	if (M->IsInsideScene())
	{
		return LOCTEXT("WhereScene", "in a level \u00b7 Alt+PgUp keeps it");
	}
	if (M->IsInsidePrefab())
	{
		// The whole way down, from the level: "i'd like the path to be seen
		// too like house1.umap/room1.prefab/drawer1.prefab" (S81a).
		FString Level = TEXT("level");
		if (const UWorld* World = M->GetWorld())
		{
			Level = World->GetMapName();
		}
		FString Path = Level + TEXT(".umap");
		for (const FString& Step : M->RoomPathNames())
		{
			Path += TEXT(" / ") + Step + TEXT(".prefab");
		}
		return FText::FromString(FString::Printf(TEXT("%s%s"),
			*Path, M->IsRoomInPlace() ? TEXT("  \u00b7 where it stands") : TEXT("")));
	}
	// The level by name (S79): "the level" is true of every level.
	if (const UWorld* World = M->GetWorld())
	{
		const FString Name = World->GetMapName();
		if (!Name.IsEmpty())
		{
			return FText::FromString(Name);
		}
	}
	return LOCTEXT("WhereLevel", "the level");
}

FText SSurfViewportBar::GetWherePath() const
{
	USurfEdMode* M = Mode.Get();
	if (!M)
	{
		return FText::GetEmpty();
	}

	// Nothing open, or open alone at the origin: there is no context, so
	// there is no path - only the thing's own name (S86a).
	const bool bInside = M->IsInsidePrefab() || M->IsInsideScene();
	if (!bInside || !M->IsRoomInPlace())
	{
		return FText::GetEmpty();
	}

	FString Path;
	if (const UWorld* World = M->GetWorld())
	{
		Path = World->GetMapName();
	}
	if (Path.IsEmpty())
	{
		Path = TEXT("level");
	}

	// Everything above the one being edited; the leaf is the name, not the
	// path.
	const TArray<FString> Steps = M->RoomPathNames();
	for (int32 I = 0; I + 1 < Steps.Num(); ++I)
	{
		Path += TEXT("/") + Steps[I];
	}
	return FText::FromString(Path + TEXT("/"));
}

FText SSurfViewportBar::GetWhereName() const
{
	USurfEdMode* M = Mode.Get();
	if (!M)
	{
		return LOCTEXT("WhereNoMode", "SurfacingPro");
	}
	if (M->IsInsideScene())
	{
		return LOCTEXT("WhereInScene", "level");
	}
	if (M->IsInsidePrefab())
	{
		const TArray<FString> Steps = M->RoomPathNames();
		return FText::FromString(Steps.Num() > 0 ? Steps.Last() : FString(TEXT("prefab")));
	}
	if (const UWorld* World = M->GetWorld())
	{
		const FString Name = World->GetMapName();
		if (!Name.IsEmpty())
		{
			return FText::FromString(Name);
		}
	}
	return LOCTEXT("WhereTheLevel", "the level");
}

FSlateColor SSurfViewportBar::GetWhereColour() const
{
	USurfEdMode* M = Mode.Get();
	const bool bInside = M && (M->IsInsidePrefab() || M->IsInsideScene());
	// A level in a quiet blue (S199a: "slightly subtle color it"), a prefab
	// or a level opened inside another in its amber.
	return bInside ? SurfTheme::Warn() : SurfTheme::Level();
}

void SSurfViewportBar::RefillVerbs()
{
	if (!VerbFlow.IsValid())
	{
		return;
	}
	LayoutSeen = SurfVerbLayout::Serial();
	VerbFlow->ClearChildren();

	/**
	 * The sidebar (S193b): "we keep the other window too. and thats our new
	 * sidebar right. except its empty and in here will show what has been
	 * marked for sidebar. and user may pin some." The actions window keeps
	 * every verb; the sidebar holds only what was added to it (the shelf
	 * list), a tab per family down its edge, each folding and remembered
	 * (SidebarOpenFamilies); what is pinned (the bar list) stands at the
	 * top in view whatever is folded. Buttons are as wide as their word.
	 * Dragged narrower than CollapsedBelowPx the sidebar is its tabs alone.
	 */
	const ESurfVerbGroup Families[] = { ESurfVerbGroup::Create, ESurfVerbGroup::Geometry,
		ESurfVerbGroup::Shape, ESurfVerbGroup::Surface, ESurfVerbGroup::UV, ESurfVerbGroup::Select, ESurfVerbGroup::View };

	const bool bEditing = SurfVerbLayout::IsEditing();
	const TArray<FString> PoolIds = SurfVerbLayout::PoolIds(QuickVerbs);
	const USurfSettings* Sized = GetDefault<USurfSettings>();
	const float Scale = Sized ? FMath::Clamp(Sized->ViewportBarScale, 0.7f, 2.0f) : 1.0f;
	const bool bRight = Sized && Sized->bBarOnRight;

	auto Wide = []() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->ViewportBarWidthPx >= SurfSidebar::CollapsedBelowPx; };
	auto NewWrap = [bRight]()
	{
		return SNew(SWrapBox).UseAllottedSize(true).HAlign(bRight ? HAlign_Right : HAlign_Left).InnerSlotPadding(FVector2D(2.0f, 2.0f));
	};

	// One row: the tab on the outer edge, the buttons beside it.
	auto Row = [bRight](TSharedRef<SWidget> Tab, TSharedRef<SWidget> Content) -> TSharedRef<SWidget>
	{
		TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
		if (!bRight)
		{
			Box->AddSlot().AutoWidth()[ Tab ];
		}
		Box->AddSlot().FillWidth(1.0f).VAlign(VAlign_Top)[ Content ];
		if (bRight)
		{
			Box->AddSlot().AutoWidth()[ Tab ];
		}
		return Box;
	};

	// The tab's plate: recessed while folded, the panel while open, lifted under the hand.
	auto TabPlate = [](TSharedRef<SWidget> Label, TFunction<bool()> IsOpen, const FText& Tip, TFunction<void()> Click) -> TSharedRef<SWidget>
	{
		TSharedPtr<SBorder> Holder;
		SAssignNew(Holder, SBorder)
			.Padding(FMargin(2.0f, 6.0f))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Cursor(EMouseCursor::Default)
			.ToolTipText(Tip)
			.OnMouseButtonDown_Lambda([Click](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() != EKeys::LeftMouseButton || !Click)
				{
					return FReply::Unhandled();
				}
				Click();
				return FReply::Handled();
			})
			[
				Label
			];
		TWeakPtr<SBorder> Weak = Holder;
		Holder->SetBorderImage(TAttribute<const FSlateBrush*>::CreateLambda([Weak, IsOpen]() -> const FSlateBrush*
		{
			TSharedPtr<SBorder> Live = Weak.Pin();
			if (Live.IsValid() && Live->IsHovered())
			{
				return FAppStyle::GetBrush("Brushes.Header");
			}
			return IsOpen && IsOpen() ? FAppStyle::GetBrush("Brushes.Panel") : FAppStyle::GetBrush("Brushes.Recessed");
		}));
		return SNew(SBox).WidthOverride(18.0f)[ Holder.ToSharedRef() ];
	};

	// Pinned, at the top: a pin in the tab column, the buttons beside it.
	const TArray<const FSurfVerb*> Pinned = SurfVerbLayout::Ordered(/*bBar*/ true, QuickVerbs);
	bSidebarHasPinned = Pinned.Num() > 0;
	if (Pinned.Num() > 0 || bEditing)
	{
		TSharedRef<SWrapBox> Wrap = NewWrap();
		for (const FSurfVerb* Verb : Pinned)
		{
			Wrap->AddSlot()[ BuildVerb(*Verb) ];
		}
		TSharedRef<SVerticalBox> PinPart = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Wrap ];
		if (bEditing && Pinned.Num() == 0)
		{
			PinPart->AddSlot().AutoHeight().Padding(2.0f, 3.0f)
			[
				SNew(STextBlock).Font(SurfTheme::Italic(8)).ColorAndOpacity(SurfTheme::Dim())
				.Text(LOCTEXT("PinnedEmpty", "Pinned: drop buttons here"))
			];
		}
		TSharedRef<SWidget> Content = SNew(SBox)
			.Padding(FMargin(3.0f, 3.0f))
			.Visibility_Lambda([Wide]() { return Wide() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				bEditing ? SurfVerbLayout::MakeDropZone(PinPart, /*bBar*/ true, PoolIds) : StaticCastSharedRef<SWidget>(PinPart)
			];
		// No pin mark (S193c): "the pin takes too much screenspace".
		VerbFlow->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)[ Content ];
	}

	/**
	 * The shelf's fold (S193c): "shelf must be always visible (instead of
	 * writing shelf just a collapse icon subtle)". A small arrow, always
	 * there, folds or unfolds the family tabs under it; remembered.
	 */
	auto ShelfOpen = [bEditing]() { const USurfSettings* S = GetDefault<USurfSettings>(); return bEditing || !S || S->bSidebarShelfOpen; };
	VerbFlow->AddSlot().AutoHeight().HAlign(bRight ? HAlign_Right : HAlign_Left).Padding(1.0f, 0.0f, 1.0f, 1.0f)
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ContentPadding(FMargin(3.0f, 1.0f))
		.Cursor(EMouseCursor::Default)
		.ToolTipText(LOCTEXT("ShelfFoldTip", "The shelf: what you added to the sidebar, a tab per family. Click to fold or unfold it."))
		.OnClicked_Lambda([]()
		{
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				S->bSidebarShelfOpen = !S->bSidebarShelfOpen;
				S->SaveConfig();
			}
			return FReply::Handled();
		})
		[
			SNew(SImage)
			.Image_Lambda([ShelfOpen]() { return FAppStyle::GetBrush(ShelfOpen() ? TEXT("Icons.ChevronDown") : TEXT("Icons.ChevronRight")); })
			.DesiredSizeOverride(FVector2D(10.0, 10.0))
			.ColorAndOpacity(FSlateColor(FStyleColors::Hover2))
		]
	];

	// The sidebar's own verbs, a tab per family.
	SidebarFamilyMask = 0;
	TSharedRef<SVerticalBox> Sections = SNew(SVerticalBox);
	for (ESurfVerbGroup Family : Families)
	{
		TArray<const FSurfVerb*> Mine;
		for (const FSurfVerb* Verb : SurfVerbLayout::Ordered(/*bBar*/ false, QuickVerbs))
		{
			if (Verb->Group == Family)
			{
				Mine.Add(Verb);
			}
		}
		if (Mine.Num() == 0)
		{
			continue;
		}
		const int32 Bit = 1 << static_cast<int32>(Family);
		// A family is a group without a title (S203a): "remove the category
		// titles. still categorize them but no title" - a small gap between
		// families is all that says where one ends.
		const bool bFirst = SidebarFamilyMask == 0;
		SidebarFamilyMask |= Bit;
		TSharedRef<SWrapBox> Wrap = NewWrap();
		for (const FSurfVerb* Verb : Mine)
		{
			Wrap->AddSlot()[ BuildVerb(*Verb) ];
		}
		Sections->AddSlot().AutoHeight().Padding(0.0f, bFirst ? 0.0f : 5.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Padding(FMargin(3.0f, 1.0f))
			.Visibility_Lambda([Wide]() { return Wide() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				Wrap
			]
		];
	}
	if (SidebarFamilyMask == 0)
	{
		Sections->AddSlot().AutoHeight().Padding(4.0f, 2.0f)
		[
			SNew(STextBlock).Font(SurfTheme::Italic(8)).ColorAndOpacity(SurfTheme::Dim())
			.Text(bEditing ? LOCTEXT("SidebarEmpty", "Shelf: drop buttons here") : LOCTEXT("SidebarEmptyHint", "Right-click a button: Add to sidebar"))
		];
	}
	VerbFlow->AddSlot().AutoHeight()
	[
		SNew(SBox)
		.Visibility_Lambda([ShelfOpen]() { return ShelfOpen() ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			bEditing ? SurfVerbLayout::MakeDropZone(Sections, /*bBar*/ false, PoolIds) : StaticCastSharedRef<SWidget>(Sections)
		]
	];
}

TSharedRef<SWidget> SSurfViewportBar::BuildVerb(const FSurfVerb& Verb)
{
	TFunction<void()> Run = Verb.Run;
	TFunction<bool()> Enabled = Verb.IsEnabled;
	const bool bHasOptions = static_cast<bool>(Verb.Options);
	const FSurfVerb Copy = Verb;
	TWeakObjectPtr<USurfEdMode> Repeatable = Mode;
	const FString VerbName = Verb.Label.ToString();

	// As wide as the word, not a column width: short verbs take the room
	// they need and the rest wrap onto the next line, so the bar can be
	// widened into two or three columns instead of one long run.
	/**
	 * A verb that cannot run right now keeps its place and loses its box
	 * (S88a).
	 *
	 * Hiding them shrank the bar and moved every remaining button every
	 * time the selection changed - "the sidebar cant keep changing and
	 * moving its not good the user eyes", and he is right: a tool bar is
	 * learned by position, and a position that moves has to be read every
	 * time. So nothing moves. What changes is weight: a live verb is a
	 * filled chip, a dead one is a faint word on the viewport, and the
	 * space it occupies costs nothing because there is no panel behind it.
	 */
	auto Live = [Enabled]() { return Enabled ? Enabled() : true; };

	const USurfSettings* Sized = GetDefault<USurfSettings>();
	const float Scale = Sized ? FMath::Clamp(Sized->ViewportBarScale, 0.7f, 2.0f) : 1.0f;
	// Words, icons or both, as the sidebar's setting says (S195a).
	const ESurfIconMode IconMode = SurfIcons::ModeFor(ESurfIconSurface::Sidebar);
	const FSlateBrush* Icon = IconMode != ESurfIconMode::Words ? SurfIcons::ForVerb(VerbName) : nullptr;
	const bool bShowWord = !Icon || IconMode == ESurfIconMode::IconAndWord;
	TSharedRef<SWidget> Button = SNew(SBox).HeightOverride(18.0f * Scale)
	[
		SNew(SButton)
		.ButtonStyle(&VerbButtonStyle())
		// The arrow, not the viewport's cursor (S165a): "when mouse over the
		// shelf button or shelf, the cursor is the viewport cursor".
		.Cursor(EMouseCursor::Default)
		.ToolTipText_Lambda([Copy, bHasOptions]()
		{
			return bHasOptions
				? FText::Format(LOCTEXT("VerbTipWithOptions", "{0}\nRight-click: options · favourites · sidebar."), SurfVerbs::VerbTip(Copy))
				: FText::Format(LOCTEXT("VerbTipMoveOnly", "{0}\nRight-click: favourites · sidebar."), SurfVerbs::VerbTip(Copy));
		})
		.ContentPadding(FMargin(0.0f))
		.IsEnabled_Lambda([Live]() { return Live(); })
		.OnClicked_Lambda([Run, Repeatable, VerbName, bOffer = bHasOptions && Verb.Group != ESurfVerbGroup::View]()
		{
			if (Run)
			{
				// Remembered before it runs, so Shift+R repeats it.
				if (Repeatable.IsValid())
				{
					Repeatable->NoteVerb(VerbName, Run);
				}
				Run();
				// Its frame, every time it runs (S184c): "shading window
				// param opened once and never again".
				if (Repeatable.IsValid())
				{
					Repeatable->AfterVerbRan(VerbName, bOffer);
				}
			}
			return FReply::Handled();
		})
		[
			// The fill, the hover and the press are the button's own style
			// now, so this is a frame for the contents and nothing else
			// (S92a).
			SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(0.0f)
			[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(Icon ? FMargin(5.0f, 0.0f, bShowWord ? 0.0f : 5.0f, 0.0f) : FMargin(0.0f))
			[
				SNew(SImage)
				.Image(Icon)
				.DesiredSizeOverride(FVector2D(13.0 * Scale, 13.0 * Scale))
				.ColorAndOpacity(FSlateColor::UseForeground())
				.Visibility(Icon ? EVisibility::Visible : EVisibility::Collapsed)
			]
			// No family stripe (S192b): the editor's buttons carry no colour.
			// As wide as the word (S193b): "buttons take up too much screenspace".
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(bShowWord ? FMargin(Icon ? 4.0f : 6.0f, 0.0f, 6.0f, 0.0f) : FMargin(0.0f))
			[
				/**
				 * The word (S133a): "The sideview remain short words as
				 * before." S131a put a drawn mark here and he rolled it
				 * back the same day. **The bar is learned by position and
				 * read by word**, and twenty-six marks in a column is a
				 * second language to learn for verbs he already knows by
				 * name. The marks went where he meant them - the prefabs
				 * window, whose verbs are a file manager's.
				 */
				SNew(STextBlock)
				.Text_Lambda([Copy]() { return Copy.LiveLabel ? Copy.LiveLabel() : Copy.Label; })
				.Visibility(bShowWord ? EVisibility::Visible : EVisibility::Collapsed)
				.Font(SurfTheme::Font(FMath::RoundToInt(8.0f * Scale)))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			]
		]
	];

	// The right button is caught outside the SButton, which only knows about
	// the left one. The border is invisible; it is there for the event.
	// Every verb has it, not only the ones with options: a right-click is
	// how a button is moved between the two surfaces (S74a).
	TSharedRef<SWidget> Face = SNew(SBorder)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.Padding(0.0f)
		.OnMouseButtonDown_Lambda([this, Copy](const FGeometry& Geometry, const FPointerEvent& Event) -> FReply
		{
			if (Event.GetEffectingButton() == EKeys::RightMouseButton)
			{
				OpenOptions(Copy, Geometry);
				return FReply::Handled();
			}
			return FReply::Unhandled();
		})
		[
			Button
		];

	if (!SurfVerbLayout::IsEditing())
	{
		return Face;
	}
	// While the interface is being arranged every button is there, or the
	// ones that cannot run right now could never be moved.
	return SurfVerbLayout::DressForEditing(Face, VerbName, SurfVerbLayout::PoolIds(QuickVerbs), 21.0f, /*bDragOnly*/ false);
}

namespace
{
	/** Every setting as bytes, hashed: the cheap way to know one changed. */
	uint32 HashSettings()
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		if (!Settings)
		{
			return 0;
		}
		TArray<uint8> Bytes;
		FObjectWriter Writer(Settings, Bytes);
		return FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
	}
}

namespace
{
	/**
	 * Where the frame was dragged to (S206a): "user need be able to move the
	 * popup param window by dragging its top. show a move cursor too". The
	 * usual spot keeps its nudge for the session; a frame opened at the
	 * cursor keeps its own until the next one opens.
	 */
	FVector2D GSurfFrameNudge = FVector2D::ZeroVector;
	FVector2D GSurfCursorFrameNudge = FVector2D::ZeroVector;
	uint32 GSurfCursorFrameSerial = 0;

	/** The frame's top edge, to drag it by. */
	class SSurfFrameGrip : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfFrameGrip) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, TFunction<void(const FVector2D&)> InMove)
		{
			Move = MoveTemp(InMove);
			SetCursor(EMouseCursor::CardinalCross);
			SetToolTipText(LOCTEXT("FrameGripTip", "Drag to move the frame."));
			ChildSlot
			[
				SNew(SBox).HeightOverride(9.0f).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage_Lambda([this]() { return IsHovered() || bDragging ? &HotBrush : &RestBrush; })
					.Padding(0.0f)
					[
						SNew(SBox).WidthOverride(34.0f).HeightOverride(3.0f)
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
			bDragging = true;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}

		virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (!bDragging || !HasMouseCapture())
			{
				return FReply::Unhandled();
			}
			if (Move)
			{
				const float Scale = FMath::Max(0.01f, Geometry.Scale);
				Move(Event.GetCursorDelta() / Scale);
			}
			return FReply::Handled();
		}

		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
		{
			if (!bDragging)
			{
				return FReply::Unhandled();
			}
			bDragging = false;
			return FReply::Handled().ReleaseMouseCapture();
		}

		virtual void OnMouseCaptureLost(const FCaptureLostEvent& Event) override
		{
			bDragging = false;
			SCompoundWidget::OnMouseCaptureLost(Event);
		}

	private:
		TFunction<void(const FVector2D&)> Move;
		bool bDragging = false;
		FSlateRoundedBoxBrush RestBrush = FSlateRoundedBoxBrush(FStyleColors::Hover, 1.5f);
		FSlateRoundedBoxBrush HotBrush = FSlateRoundedBoxBrush(FStyleColors::ForegroundHover, 1.5f);
	};
}

void SSurfAdjustFrame::Construct(const FArguments& InArgs)
{
	TWeakObjectPtr<USurfEdMode> M = InArgs._Mode;

	/**
	 * Options tweaked in the frame re-run the last verb (S166a): "the
	 * purpose of the popup param frame is: if you tweak parameters, it
	 * applies to the last performed action... live. that's for all popup
	 * param windows." A press or a key inside the frame marks it touched;
	 * on the next tick, if the settings changed under that touch and the
	 * frame is on the verb that just ran, that verb is undone and run
	 * again. Changes from elsewhere - the wheel, a key - never count.
	 */
	RegisterActiveTimer(0.12f, FWidgetActiveTimerDelegate::CreateLambda([this, M](double, float)
	{
		if (!M.IsValid() || !M->bOptionsShown)
		{
			bTouched = false;
			return EActiveTimerReturnType::Continue;
		}
		if (HashedFor != M->OptionsVerbId || HashedSerial != M->OptionsSerial)
		{
			HashedFor = M->OptionsVerbId;
			HashedSerial = M->OptionsSerial;
			SettingsHash = HashSettings();
			bTouched = false;
			return EActiveTimerReturnType::Continue;
		}
		if (!bTouched)
		{
			return EActiveTimerReturnType::Continue;
		}
		// A knob being dragged replays as it goes (S170a): "make sure all
		// paramwindow slider work realtime" - each tick the numbers moved,
		// the verb is undone and run again; the frame keeps its widgets
		// meanwhile (bReplayingOptions), so the knob stays under the hand.
		const bool bDragging = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0;
		if (!bDragging)
		{
			bTouched = false;
		}
		const uint32 Now = HashSettings();
		if (Now != SettingsHash)
		{
			SettingsHash = Now;
			M->ReplayLastVerbForOptions(HashedFor);
		}
		return EActiveTimerReturnType::Continue;
	}));

	// Glass, like the bar: hit-testable only where its content is. See
	// SSurfViewportBar::Construct.
	SetVisibility(EVisibility::SelfHitTestInvisible);

	// Down and right of where the cursor was when the operation ran, a
	// little way off so it does not sit on what was just made, in the
	// viewport that held the cursor; the primary viewport's bottom-left
	// when none did (a verb pressed on the docked panel). Sized to its
	// content; the rest of the overlay is not hit-tested, so the viewport
	// under it still takes every click.
	TSharedPtr<SBox> Box;
	TSharedRef<TSharedPtr<SBox>> BoxHolder = MakeShared<TSharedPtr<SBox>>();
	const SWidget* Self = this;
	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	.Padding(TAttribute<FMargin>::CreateLambda([M, Self, BoxHolder, this]() -> FMargin
	{
		const FGeometry& Geometry = GetCachedGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		const FVector2D Frame = BoxHolder->IsValid() ? (*BoxHolder)->GetDesiredSize() : FVector2D(200.0, 160.0);
		if (!M.IsValid() || Size.X < 1.0)
		{
			return FMargin(6.0f, 0.0f, 0.0f, 0.0f);
		}
		const int32 Host = M->HostIndexOf(Self);

		// The bar runs down the left of the main viewport, and a frame that
		// lands under it is a frame that cannot be read - his screenshot,
		// the solidify numbers behind the buttons (S83a). Nothing of ours
		// is allowed to start left of where the bar ends.
		double Least = 6.0;
		if (Host <= 0)
		{
			const USurfSettings* BarSettings = GetDefault<USurfSettings>();
			Least = (BarSettings ? static_cast<double>(BarSettings->ViewportBarWidthPx) : 138.0) + 16.0;
		}
		const double Most = FMath::Max(6.0, Size.X - Frame.X - 6.0);
		// A viewport too narrow to hold both: the frame wins, because the
		// bar is still readable underneath and the frame would not be.
		Least = FMath::Min(Least, Most);

		/**
		 * One place, low and to the left (S110a).
		 *
		 * "make popup box in some bottom-left corner of the viewport floating
		 * in the area but not too close to the corner. mid."
		 *
		 * It used to open down and right of the cursor (S83a), which put it
		 * somewhere different every time and sometimes over the thing being
		 * adjusted - the one place it must never be. **A panel you look at
		 * while dragging should be where your eye already knows to go**, and
		 * that is worth more than being near the hand. So: clear of the bar,
		 * low in the viewport but off the corner, and the same spot every
		 * time.
		 */
		const double Bottom = FMath::Max(6.0, Size.Y - Frame.Y - 6.0);
		// A right-click's frame sits at the cursor (S166a), kept inside.
		if (M->bOptionsAtCursor)
		{
			if (GSurfCursorFrameSerial != M->AdjustFrameSerial)
			{
				GSurfCursorFrameSerial = M->AdjustFrameSerial;
				GSurfCursorFrameNudge = FVector2D::ZeroVector;
			}
			const FVector2D At = Geometry.AbsoluteToLocal(M->OptionsCursor) + GSurfCursorFrameNudge;
			return FMargin(
				static_cast<float>(FMath::Clamp(At.X + 10.0, 6.0, FMath::Max(6.0, Size.X - Frame.X - 6.0))),
				static_cast<float>(FMath::Clamp(At.Y + 6.0, 6.0, Bottom)), 0.0f, 0.0f);
		}
		// Dragged, it may go over the bar: he put it there.
		const double X = FMath::Clamp(Least + 24.0 + GSurfFrameNudge.X, 6.0, FMath::Max(6.0, Size.X - Frame.X - 6.0));
		const double Y = FMath::Clamp(Size.Y * 0.66 - Frame.Y * 0.5 + GSurfFrameNudge.Y, 6.0, Bottom);
		return FMargin(static_cast<float>(X), static_cast<float>(Y), 0.0f, 0.0f);
	}))
	[
		SAssignNew(Box, SBox).WidthOverride(290.0f)
		.Visibility_Lambda([M, Self]()
		{
			if (!M.IsValid() || !M->IsAdjustFrameShown())
			{
				return EVisibility::Collapsed;
			}
			// In the viewport that held the cursor, else in the primary.
			const int32 Host = M->HostIndexOf(Self);
			const bool bShow = M->AdjustFrameHost >= 0 ? M->AdjustFrameHost == Host : Host <= 0;
			return bShow ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			// The editor's own panel, small (S158a): "its style must be
			// more ue-sober. keep it minimal and small."
			SNew(SBorder)
			.BorderImage(FrameBrush())
			.BorderBackgroundColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.97f))
			.Padding(FMargin(7.0f, 0.0f, 7.0f, 6.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSurfFrameGrip, [M](const FVector2D& Delta)
					{
						if (M.IsValid() && M->bOptionsAtCursor)
						{
							GSurfCursorFrameNudge += Delta;
						}
						else
						{
							GSurfFrameNudge += Delta;
						}
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SSurfViewportBar::MakeAdjustSection(M)
				]
			]
		]
	];
	*BoxHolder = Box;
}

/**
 * The focused verb's options as a widget that follows the mode's
 * OptionsVerbId (S160a): the same menu builders the right-click used to
 * open, standing in the frame, rebuilt when the verb changes - never on a
 * timer, which would pull a spin box out from under a drag.
 */
class SSurfVerbOptions : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfVerbOptions) {}
		SLATE_ARGUMENT(TWeakObjectPtr<USurfEdMode>, Mode)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Mode = InArgs._Mode;
		ChildSlot[ SAssignNew(Body, SBox) ];
		Rebuild();
	}

	virtual void Tick(const FGeometry&, const double, const float) override
	{
		USurfEdMode* Live = Mode.Get();
		const uint32 Serial = Live ? Live->OptionsSerial : 0u;
		if (Serial != BuiltSerial || BuiltLayout != SurfVerbLayout::Serial())
		{
			Rebuild();
		}
	}

private:
	TWeakObjectPtr<USurfEdMode> Mode;
	TSharedPtr<SBox> Body;
	uint32 BuiltSerial = ~0u;
	int32 BuiltLayout = -1;

	void Rebuild()
	{
		USurfEdMode* Live = Mode.Get();
		BuiltSerial = Live ? Live->OptionsSerial : 0u;
		BuiltLayout = SurfVerbLayout::Serial();
		if (!Live || Live->OptionsVerbId.IsEmpty())
		{
			Body->SetContent(SNullWidget::NullWidget);
			return;
		}
		const TArray<FSurfVerb> Pool = SurfVerbs::BuildPool(Live, nullptr);
		const FString Id = Live->OptionsVerbId;
		const FSurfVerb* Verb = Pool.FindByPredicate([&Id](const FSurfVerb& Each) { return SurfVerbLayout::IdOf(Each).Equals(Id, ESearchCase::IgnoreCase); });
		if (!Verb)
		{
			Body->SetContent(SNullWidget::NullWidget);
			return;
		}
		TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
		if (Verb->Options)
		{
			FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &FCoreStyle::Get(), /*bSearchable*/ false);
			GSurfOptionsInFrame = true;
			Verb->Options(Menu);
			GSurfOptionsInFrame = false;
			Column->AddSlot().AutoHeight()[ Menu.MakeWidget() ];
		}
		Column->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SurfVerbLayout::MakeArrangeRow(*Verb, SurfVerbLayout::PoolIds(Pool))
		];
		Body->SetContent(Column);
	}
};

TSharedRef<SWidget> SSurfViewportBar::MakeAdjustSection(TWeakObjectPtr<USurfEdMode> M)
{

	// One row per number: a label and a knob. Every knob writes into the
	// last operation and replays it, so the mesh follows the slider.
	auto Row = [](const FText& Label, TSharedRef<SWidget> Knob) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(46.0f)
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8))
					.ColorAndOpacity(SurfTheme::Text())
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				Knob
			];
	};

	// A knob with a slider: typed values go from Min to Max, the slider
	// covers the same, for the numbers that have a natural range - a turn,
	// a strength, a scale, a bevel that cannot pass the next line.
	auto Number = [M](TFunction<double&(FSurfOperation&)> Field, double Min, TAttribute<TOptional<double>> Max, double Default) -> TSharedRef<SWidget>
	{
		return SNew(SSpinBox<double>)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Value_Lambda([M, Field]() { return M.IsValid() ? Field(M->LastOperation) : 0.0; })
			.OnValueChanged_Lambda([M, Field](double V) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(); } })
			.OnValueCommitted_Lambda([M, Field](double V, ETextCommit::Type) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(/*bFinal*/ true); } })
			.ToolTipText(FText::Format(LOCTEXT("AdjustKnobTip", "Drag, or type a number. The operation runs again with it. Default {0}."), FText::AsNumber(Default)));
	};

	// A distance: dragged the way a number field is dragged in a modelling
	// package - so many centimetres per pixel, paced by the size of a cell
	// on the patch (SliderRange), Shift ten times as fast, Ctrl a tenth -
	// rather than a slider from one end of a range to the other. A slider
	// across the whole level was a metre a pixel; one across the patch
	// still could not set a wall's thickness to twenty centimetres.
	// No MaxValue: a spin box with both ends set is a slider across them,
	// however far apart they are (a kilometre a pixel, the first time);
	// with an end open it spins by the drag alone.
	auto Distance = [M](TFunction<double&(FSurfOperation&)> Field, double Min, double Default) -> TSharedRef<SWidget>
	{
		return SNew(SSpinBox<double>)
			.MinValue(Min > -99999.0 ? TOptional<double>(Min) : TOptional<double>())
			.Delta_Lambda([M]() { return M.IsValid() && M->LastOperation.SliderRange > 0.0 ? M->LastOperation.SliderRange : 0.5; })
			.LinearDeltaSensitivity(1)
			.Value_Lambda([M, Field]() { return M.IsValid() ? Field(M->LastOperation) : 0.0; })
			.OnValueChanged_Lambda([M, Field](double V) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(); } })
			.OnValueCommitted_Lambda([M, Field](double V, ETextCommit::Type) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(/*bFinal*/ true); } })
			.ToolTipText(FText::Format(LOCTEXT("AdjustDistanceTip", "Drag left or right (Shift: ten times as fast, Ctrl: a tenth), or type a number. The operation runs again with it. Default {0}."), FText::AsNumber(Default)));
	};

	auto Whole = [M](TFunction<int32&(FSurfOperation&)> Field, int32 Min, int32 Max, TFunction<int32(const FSurfOperation&)> MaxOf = nullptr) -> TSharedRef<SWidget>
	{
		const TAttribute<TOptional<int32>> Top = TAttribute<TOptional<int32>>::CreateLambda([M, Max, MaxOf]()
		{
			return TOptional<int32>(MaxOf && M.IsValid() ? FMath::Clamp(MaxOf(M->LastOperation), 1, Max) : Max);
		});
		return SNew(SSpinBox<int32>)
			.MinValue(Min).MaxValue(Top).MinSliderValue(Min).MaxSliderValue(Top)
			.Value_Lambda([M, Field]() { return M.IsValid() ? Field(M->LastOperation) : 0; })
			.OnValueChanged_Lambda([M, Field](int32 V) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(); } })
			.OnValueCommitted_Lambda([M, Field](int32 V, ETextCommit::Type) { if (M.IsValid()) { Field(M->LastOperation) = V; M->ReplayLastOperation(/*bFinal*/ true); } });
	};

	// The knobs belong to the last operation; up for another verb's options
	// alone, they stay away (S160a).
	auto Only = [M](std::initializer_list<ESurfOpKind> Kinds) -> TAttribute<EVisibility>
	{
		TArray<ESurfOpKind> List(Kinds);
		return TAttribute<EVisibility>::CreateLambda([M, List]()
		{
			return M.IsValid() && !M->bOptionsOnly && M->CanAdjustLastOperation() && List.Contains(M->LastOperation.Kind) ? EVisibility::Visible : EVisibility::Collapsed;
		});
	};

	auto When = [M](TFunction<bool(const FSurfOperation&)> Test) -> TAttribute<EVisibility>
	{
		return TAttribute<EVisibility>::CreateLambda([M, Test]()
		{
			return M.IsValid() && !M->bOptionsOnly && M->CanAdjustLastOperation() && Test(M->LastOperation) ? EVisibility::Visible : EVisibility::Collapsed;
		});
	};

	// A move's X Y Z belong to a move, and to an extrude that came with one (E).
	auto MovedRows = When([](const FSurfOperation& O) { return O.Kind == ESurfOpKind::Move || (O.Kind == ESurfOpKind::Extrude && O.bMoved); });
	auto TileRows = When([](const FSurfOperation& O) { return O.Kind == ESurfOpKind::Surface && O.SurfaceVerb != ESurfSurfaceVerb::Fit && O.SurfaceVerb != ESurfSurfaceVerb::Normalise; });
	auto FitRows = When([](const FSurfOperation& O) { return O.Kind == ESurfOpKind::Surface && O.SurfaceVerb == ESurfSurfaceVerb::Fit; });
	auto ProjectRows = When([](const FSurfOperation& O) { return O.Kind == ESurfOpKind::Surface && O.SurfaceVerb == ESurfSurfaceVerb::Project; });
	auto DensityRows = When([](const FSurfOperation& O) { return O.Kind == ESurfOpKind::Surface && O.SurfaceVerb == ESurfSurfaceVerb::Normalise; });
	auto DistanceRow = When([](const FSurfOperation& O) { return (O.Kind == ESurfOpKind::Extrude && !O.bMoved) || O.Kind == ESurfOpKind::Inflate || O.Kind == ESurfOpKind::Duplicate || O.Kind == ESurfOpKind::Random; });

	const TAttribute<TOptional<double>> One(TOptional<double>(1.0));
	const TAttribute<TOptional<double>> Turn(TOptional<double>(360.0));
	const TAttribute<TOptional<double>> Tile(TOptional<double>(32.0));
	const TAttribute<TOptional<double>> Texels(TOptional<double>(8192.0));
	const TAttribute<TOptional<double>> Times(TOptional<double>(100.0));
	const TAttribute<TOptional<double>> BevelMax = TAttribute<TOptional<double>>::CreateLambda([M]()
	{
		return TOptional<double>(M.IsValid() ? FMath::Max(1.0, M->LastOperation.WidthLimit) : 500.0);
	});

	auto SideChoice = [](TWeakObjectPtr<USurfEdMode> Weak) -> TSharedRef<SWidget>
	{
		auto One = [Weak](const FText& Label, const FText& Tip, int32 Which) -> TSharedRef<SWidget>
		{
			return ToggleButton(
				SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8)),
				Tip, 44.0f, 18.0f,
				[Weak, Which]() { return Weak.IsValid() && Weak->LastOperation.Side == Which; },
				[Weak, Which]() { if (Weak.IsValid()) { Weak->LastOperation.Side = Which; Weak->ReplayLastOperation(/*bFinal*/ true); } });
		};

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjSideIn", "Behind"), LOCTEXT("AdjSideInTip", "The shell grows behind the surface; the front stays where it was. This is which way the thickness goes - the walls round the border are the tick below, and you have both."), -1) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjSideMid", "Centred"), LOCTEXT("AdjSideMidTip", "Half the thickness each way."), 0) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ One(LOCTEXT("AdjSideOut", "In front"), LOCTEXT("AdjSideOutTip", "The shell grows in front of the surface; the back stays where it was."), 1) ];
	};

	auto AxisChoice = [](TWeakObjectPtr<USurfEdMode> Weak) -> TSharedRef<SWidget>
	{
		auto One = [Weak](const FText& Label, const FText& Tip, int32 Which) -> TSharedRef<SWidget>
		{
			return ToggleButton(
				SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8)),
				Tip, 32.0f, 18.0f,
				[Weak, Which]() { return Weak.IsValid() && Weak->LastOperation.Axis == Which; },
				[Weak, Which]() { if (Weak.IsValid()) { Weak->LastOperation.Axis = Which; Weak->ReplayLastOperation(/*bFinal*/ true); } });
		};

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjAxisAuto", "Own"), LOCTEXT("AdjAxisAutoTip", "Projected from the selection's own direction, to its nearest axis."), 0) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjAxisX", "X"), LOCTEXT("AdjAxisXTip", "Projected along X."), 1) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjAxisY", "Y"), LOCTEXT("AdjAxisYTip", "Projected along Y."), 2) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ One(LOCTEXT("AdjAxisZ", "Z"), LOCTEXT("AdjAxisZTip", "Projected along Z."), 3) ];
	};

	auto UnderChoice = [](TWeakObjectPtr<USurfEdMode> Weak) -> TSharedRef<SWidget>
	{
		auto One = [Weak](const FText& Label, const FText& Tip, ESurfExtrudeUnder Which) -> TSharedRef<SWidget>
		{
			return ToggleButton(
				SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8)),
				Tip, 44.0f, 18.0f,
				[Weak, Which]() { return Weak.IsValid() && Weak->LastOperation.Under == Which; },
				[Weak, Which]()
				{
					if (Weak.IsValid()) { Weak->LastOperation.Under = Which; Weak->ReplayLastOperation(/*bFinal*/ true); }
					if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->ExtrudeUnder = Which; S->SaveConfig(); }
				});
		};

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjUnderDrop", "Drop"), LOCTEXT("AdjUnderDropTip", "The faces beneath the lid go: a hollow block, the rest of the patch in pieces round the hole."), ESurfExtrudeUnder::Drop) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)
			[ One(LOCTEXT("AdjUnderClose", "Close"), LOCTEXT("AdjUnderCloseTip", "The faces beneath become a patch of their own, facing out of the block: a closed box."), ESurfExtrudeUnder::Close) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ One(LOCTEXT("AdjUnderKeep", "Keep"), LOCTEXT("AdjUnderKeepTip", "The source patch stays as it was, its faces still under the lid."), ESurfExtrudeUnder::Keep) ];
	};

	return SNew(SVerticalBox)
		.Visibility_Lambda([M]() { return M.IsValid() && M->IsAdjustFrameShown() ? EVisibility::Visible : EVisibility::Collapsed; })

		// The frame's title strip (S192d), as an editor window's: the
		// header grey across the whole width, the name in its header type.
		+ SVerticalBox::Slot().AutoHeight().Padding(-7.0f, -1.0f, -7.0f, 5.0f)
		[
			SNew(SBorder)
			.BorderImage(FrameHeaderBrush())
			.Padding(FMargin(8.0f, 0.0f))
			[
				SNew(SBox).HeightOverride(24.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([M]() { return M.IsValid() ? FText::FromString(M->OptionsVerbId.IsEmpty() ? M->DescribeLastOperation() : M->OptionsVerbId) : FText::GetEmpty(); })
					.Font(SurfTheme::Bold(9))
					.ColorAndOpacity(SurfTheme::Header())
				]
			]
		]

		// Move, and an extrude that came with one
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(MovedRows)[ Row(LOCTEXT("AdjX", "X cm"), Distance([](FSurfOperation& O) -> double& { return O.Delta.X; }, -100000.0, 0.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(MovedRows)[ Row(LOCTEXT("AdjY", "Y cm"), Distance([](FSurfOperation& O) -> double& { return O.Delta.Y; }, -100000.0, 0.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(MovedRows)[ Row(LOCTEXT("AdjZ", "Z cm"), Distance([](FSurfOperation& O) -> double& { return O.Delta.Z; }, -100000.0, 0.0)) ] ]

		// Rotate
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Rotate }))[ Row(LOCTEXT("AdjYaw", "Yaw"), Number([](FSurfOperation& O) -> double& { return O.Rotation.Yaw; }, -360.0, Turn, 0.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Rotate }))[ Row(LOCTEXT("AdjPitch", "Pitch"), Number([](FSurfOperation& O) -> double& { return O.Rotation.Pitch; }, -360.0, Turn, 0.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Rotate }))[ Row(LOCTEXT("AdjRoll", "Roll"), Number([](FSurfOperation& O) -> double& { return O.Rotation.Roll; }, -360.0, Turn, 0.0)) ] ]

		// Scale
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Scale }))[ Row(LOCTEXT("AdjSX", "X"), Number([](FSurfOperation& O) -> double& { return O.Scale.X; }, 0.01, Times, 1.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Scale }))[ Row(LOCTEXT("AdjSY", "Y"), Number([](FSurfOperation& O) -> double& { return O.Scale.Y; }, 0.01, Times, 1.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Scale }))[ Row(LOCTEXT("AdjSZ", "Z"), Number([](FSurfOperation& O) -> double& { return O.Scale.Z; }, 0.01, Times, 1.0)) ] ]

		// Distances
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(DistanceRow)[ Row(LOCTEXT("AdjDist", "Distance cm"), Distance([](FSurfOperation& O) -> double& { return O.Distance; }, -100000.0, 10.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Extrude }))
		[
			// The faces beneath the lid: one of three, as buttons, because a
			// tick can only say two things and a box has three ways to end.
			Row(LOCTEXT("AdjUnder", "Beneath"), UnderChoice(M))
		] ]
		// UVs carried on over a grown line (S207b), a patch whose UVs are held.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ SNew(SBox).Visibility(Only({ ESurfOpKind::Extrude }))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->bExtrudeCarriesUVs ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M](ECheckBoxState State)
			{
				if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bExtrudeCarriesUVs = State == ECheckBoxState::Checked; S->SaveConfig(); }
				if (M.IsValid()) { M->ReplayLastOperation(/*bFinal*/ true); }
			})
			.ToolTipText(LOCTEXT("AdjCarryUVsTip", "The new line's texture carries on from the edge by the line's real length, so a long pull does not stretch it. Off: one cell's worth, stretched. Only a patch whose UVs are held (frozen); the others are measured anyway."))
			[
				SNew(STextBlock).Text(LOCTEXT("AdjCarryUVs", "UVs carry on")).Font(SurfTheme::Font(8))
			]
		] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Solidify }))[ Row(LOCTEXT("AdjThick", "Thick cm"), Distance([](FSurfOperation& O) -> double& { return O.Distance; }, -100000.0, 20.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Solidify }))[ Row(LOCTEXT("AdjSide", "Grows"), SideChoice(M)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Solidify }))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bRim ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bRim = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
				.ToolTipText(LOCTEXT("AdjRimTip", "The four skirts joining front and back. Off: front and back only."))
				[ SNew(STextBlock).Text(LOCTEXT("AdjRim", "Walls all round")).Font(SurfTheme::Font(8)) ]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bEven ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bEven = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
				.ToolTipText(LOCTEXT("AdjEvenTip", "Keep the shell as thick at a fold as on a flat run."))
				[ SNew(STextBlock).Text(LOCTEXT("AdjEven", "Even thickness")).Font(SurfTheme::Font(8)) ]
			]
		] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::WeldByDistance, ESurfOpKind::WeldClusters }))[ Row(LOCTEXT("AdjReach", "Reach cm"), Distance([](FSurfOperation& O) -> double& { return O.Distance; }, 0.01, 1.0)) ] ]

		// Subdivide: which way, how many times over
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Subdivide }))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bRows ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bRows = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
				.ToolTipText(LOCTEXT("AdjRowsTip", "A row between every pair of rows."))
				[ SNew(STextBlock).Text(LOCTEXT("AdjRows", "Rows")).Font(SurfTheme::Font(8)) ]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bColumns ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bColumns = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
				.ToolTipText(LOCTEXT("AdjColumnsTip", "A column between every pair of columns."))
				[ SNew(STextBlock).Text(LOCTEXT("AdjColumns", "Columns")).Font(SurfTheme::Font(8)) ]
			]
		] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Subdivide }))[ Row(LOCTEXT("AdjTimes", "Times"), Whole([](FSurfOperation& O) -> int32& { return O.Iterations; }, 1, 4)) ] ]

		/**
		 * A new patch: the points, the size, and the shape it started as
		 * (S111a). "some parameter exposed. and a dropdown for some custom
		 * preset shapes. (subdivision, dimensions params, useful param)".
		 *
		 * Every one of them remakes the patch, which is what the frame does
		 * for every other verb - so the shape is a choice he can change his
		 * mind about rather than one he has to get right first time.
		 */
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::NewSheet }))[ Row(LOCTEXT("AdjAcross", "Points across"), Whole([](FSurfOperation& O) -> int32& { return O.Iterations; }, 2, 32)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::NewSheet }))[ Row(LOCTEXT("AdjDown", "Points down"), Whole([](FSurfOperation& O) -> int32& { return O.Segments; }, 2, 32)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::NewSheet }))[ Row(LOCTEXT("AdjSize", "Size cm"), Distance([](FSurfOperation& O) -> double& { return O.Distance; }, 10.0, 1000.0)) ] ]
		// The lattice's points each way (S189a).
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Lattice }))[ Row(LOCTEXT("AdjLatX", "Points X"), Whole([](FSurfOperation& O) -> int32& { return O.Iterations; }, 2, 8)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Lattice }))[ Row(LOCTEXT("AdjLatY", "Points Y"), Whole([](FSurfOperation& O) -> int32& { return O.Segments; }, 2, 8)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Lattice }))[ Row(LOCTEXT("AdjLatZ", "Points Z"), Whole([](FSurfOperation& O) -> int32& { return O.Layers; }, 2, 8)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::NewSheet }))[ Row(LOCTEXT("AdjShape", "Shape"),
			SNew(SComboButton)
			.ContentPadding(FMargin(4.0f, 1.0f))
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(SurfTheme::Font(8))
				.Text_Lambda([M]()
				{
					return M.IsValid() ? SurfSheetPresetName(M->LastOperation.Preset) : FText::GetEmpty();
				})
			]
			.OnGetMenuContent_Lambda([M]()
			{
				FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
				for (int32 I = 0; I < static_cast<int32>(ESurfSheetPreset::Count); ++I)
				{
					Menu.AddMenuEntry(
						SurfSheetPresetName(I),
						SurfSheetPresetTip(I),
						FSlateIcon(),
						FUIAction(
							FExecuteAction::CreateLambda([M, I]()
							{
								if (M.IsValid())
								{
									M->LastOperation.Preset = I;
									M->ReplayLastOperation(/*bFinal*/ true);
								}
							}),
							FCanExecuteAction(),
							FIsActionChecked::CreateLambda([M, I]() { return M.IsValid() && M->LastOperation.Preset == I; })),
						NAME_None, EUserInterfaceActionType::RadioButton);
				}
				return Menu.MakeWidget();
			}) ) ] ]

		// Steps: how many, or how tall one is - "by cm and/or amount". Setting
		// one clears the other: two numbers for the same thing that disagree
		// is a knob that does nothing, and a knob that does nothing is worse
		// than no knob.
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Steps }))[ Row(LOCTEXT("AdjSteps", "Steps"),
			SNew(SSpinBox<int32>)
			.MinValue(1).MaxValue(128).MinSliderValue(1).MaxSliderValue(128)
			.Value_Lambda([M]() { return M.IsValid() ? M->LastOperation.Iterations : 0; })
			.OnValueChanged_Lambda([M](int32 V) { if (M.IsValid()) { M->LastOperation.Iterations = V; M->LastOperation.Distance = 0.0; M->ReplayLastOperation(); } })
			.OnValueCommitted_Lambda([M](int32 V, ETextCommit::Type) { if (M.IsValid()) { M->LastOperation.Iterations = V; M->LastOperation.Distance = 0.0; M->ReplayLastOperation(/*bFinal*/ true); } })
			.ToolTipText(LOCTEXT("AdjStepsTip", "How many steps. Setting this clears the rise below.")) ) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Steps }))[ Row(LOCTEXT("AdjRise", "Rise cm"), Distance([](FSurfOperation& O) -> double& { return O.Distance; }, 0.0, 0.0)) ] ]

		// Strength, passes, border
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Smooth, ESurfOpKind::Flatten }))[ Row(LOCTEXT("AdjStrength", "Strength"), Number([](FSurfOperation& O) -> double& { return O.Strength; }, 0.0, One, 0.5)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Bevel }))[ Row(LOCTEXT("AdjRound", "Round"), Number([](FSurfOperation& O) -> double& { return O.Strength; }, 0.0, One, 0.5)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Smooth }))[ Row(LOCTEXT("AdjPasses", "Passes"), Whole([](FSurfOperation& O) -> int32& { return O.Iterations; }, 1, 50)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Smooth }))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bBorder ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bBorder = State == ECheckBoxState::Checked; M->ReplayLastOperation(); } })
			[
				SNew(STextBlock).Text(LOCTEXT("AdjBorder", "Hold the border")).Font(SurfTheme::Font(8))
			]
		] ]

		// Surfacing: metres per tile and the turn for Flow, Align and Project;
		// the tiles across for Fit; which way for Project.
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(TileRows)[ Row(LOCTEXT("AdjTileU", "M/tile U"), Number([](FSurfOperation& O) -> double& { return O.Tiles.X; }, 0.01, Tile, 1.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(TileRows)[ Row(LOCTEXT("AdjTileV", "M/tile V"), Number([](FSurfOperation& O) -> double& { return O.Tiles.Y; }, 0.01, Tile, 1.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(FitRows)[ Row(LOCTEXT("AdjRepeatU", "Tiles U"), Whole([](FSurfOperation& O) -> int32& { return O.RepeatU; }, 1, 64)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(FitRows)[ Row(LOCTEXT("AdjRepeatV", "Tiles V"), Whole([](FSurfOperation& O) -> int32& { return O.RepeatV; }, 1, 64)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(DensityRows)[ Row(LOCTEXT("AdjDensity", "Texels a metre"), Number([](FSurfOperation& O) -> double& { return O.Density; }, 1.0, Texels, 512.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Surface }))[ Row(LOCTEXT("AdjSurfaceTurn", "Turn"), Number([](FSurfOperation& O) -> double& { return O.SurfaceRotation; }, -360.0, Turn, 0.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ SNew(SBox).Visibility(Only({ ESurfOpKind::Surface }))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bMatchFirst ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bMatchFirst = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
			.ToolTipText(LOCTEXT("AdjMatchTip", "Off: each patch changes from its own numbers by what the knobs did to the active one - twice the metres a tile here is twice on each. On: every patch takes the knobs' numbers outright, for hard alignment."))
			[
				SNew(STextBlock).Text(LOCTEXT("AdjMatch", "Match the active patch's numbers")).Font(SurfTheme::Font(8))
			]
		] ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ SNew(SBox).Visibility(ProjectRows)[ AxisChoice(M) ] ]

		// Bevel
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Bevel }))[ Row(LOCTEXT("AdjWidth", "Width cm"), Number([](FSurfOperation& O) -> double& { return O.Width; }, 0.0, BevelMax, 50.0)) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Bevel }))[ Row(LOCTEXT("AdjSegments", "Segments"), Whole([](FSurfOperation& O) -> int32& { return O.Segments; }, 1, 8, [](const FSurfOperation& O) { return O.SegmentsLimit; })) ] ]
		+ SVerticalBox::Slot().AutoHeight()[ SNew(SBox).Visibility(Only({ ESurfOpKind::Bevel }))
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([M]() { return M.IsValid() && M->LastOperation.bHard ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([M](ECheckBoxState State) { if (M.IsValid()) { M->LastOperation.bHard = State == ECheckBoxState::Checked; M->ReplayLastOperation(/*bFinal*/ true); } })
			.ToolTipText(LOCTEXT("AdjHardTip", "Keep the crease sharp and only add the lines either side of it: support lines, no rounding."))
			[
				SNew(STextBlock).Text(LOCTEXT("AdjHard", "Keep the crease, only subdivide")).Font(SurfTheme::Font(8))
			]
		] ]

		// The verb's options and where its button lives, under the knobs
		// (S160a): the one parameter surface.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(SSurfVerbOptions).Mode(M)
		];
}

void SSurfViewportBar::OpenPrefabName()
{
	SurfNamePrompt::OpenPrefabName(AsShared(), Mode);
}

void SSurfViewportBar::OpenSaveAsName()
{
	SurfNamePrompt::OpenSaveAsName(AsShared(), Mode);
}

void SSurfViewportBar::OpenNamePrompt(const FText& Title, const FString& Suggested, TFunction<void(const FString&)> Commit)
{
	SurfNamePrompt::OpenNamePrompt(AsShared(), Title, Suggested, MoveTemp(Commit));
}

namespace
{
	/** A setting's property told it changed: saved, and what shows it follows. */
	void NoteSettingChanged(FProperty* Property)
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		if (!Settings || !Property)
		{
			return;
		}
		Settings->SaveConfig();
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(Event);
		if (GEditor)
		{
			GEditor->RedrawLevelEditingViewports();
		}
	}

	double ReadSettingNumber(FProperty* Property)
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
		if (!Settings || !Numeric)
		{
			return 0.0;
		}
		const void* At = Numeric->ContainerPtrToValuePtr<void>(Settings);
		return Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(At) : static_cast<double>(Numeric->GetSignedIntPropertyValue(At));
	}

	void SetSettingNumber(FProperty* Property, double Value)
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
		if (!Settings || !Numeric)
		{
			return;
		}
		if (Numeric->HasMetaData(TEXT("ClampMin"))) { Value = FMath::Max(Value, FCString::Atod(*Numeric->GetMetaData(TEXT("ClampMin")))); }
		if (Numeric->HasMetaData(TEXT("ClampMax"))) { Value = FMath::Min(Value, FCString::Atod(*Numeric->GetMetaData(TEXT("ClampMax")))); }
		void* At = Numeric->ContainerPtrToValuePtr<void>(Settings);
		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(At, Value);
		}
		else
		{
			Numeric->SetIntPropertyValue(At, static_cast<int64>(FMath::RoundToDouble(Value)));
		}
		NoteSettingChanged(Property);
	}

	/** The knob at the end of a number setting's row in the search (S208a). */
	TSharedRef<SWidget> MakeSettingKnob(FProperty* Property)
	{
		FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
		const bool bWhole = Numeric && !Numeric->IsFloatingPoint();
		TOptional<double> Min, Max;
		if (Numeric && Numeric->HasMetaData(TEXT("ClampMin"))) { Min = FCString::Atod(*Numeric->GetMetaData(TEXT("ClampMin"))); }
		if (Numeric && Numeric->HasMetaData(TEXT("ClampMax"))) { Max = FCString::Atod(*Numeric->GetMetaData(TEXT("ClampMax"))); }
		// A field to type in and two arrows to step (S209a: "make it a text
		// field; with some up/down arrows") - a slider cut the number short
		// and could not be dragged inside the menu.
		const double Step = bWhole ? 1.0 : (Min.IsSet() && Max.IsSet() ? FMath::Max(0.01, (Max.GetValue() - Min.GetValue()) / 100.0) : 1.0);
		auto Arrow = [Property, Step](const TCHAR* Brush, double Sign)
		{
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f, 0.0f))
				.OnClicked_Lambda([Property, Step, Sign]() { SetSettingNumber(Property, ReadSettingNumber(Property) + Sign * Step); return FReply::Handled(); })
				[
					SNew(SBox).WidthOverride(9.0f).HeightOverride(9.0f)
					[
						SNew(SImage).Image(FAppStyle::GetBrush(Brush)).ColorAndOpacity(SurfTheme::Dim())
					]
				];
		};
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(64.0f)
				[
					SNew(SEditableTextBox)
					.Font(SurfTheme::Font(8))
					.SelectAllTextWhenFocused(true)
					.Text_Lambda([Property, bWhole]()
					{
						const double V = ReadSettingNumber(Property);
						return FText::FromString(bWhole ? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(V))) : FString::Printf(TEXT("%g"), V));
					})
					.OnTextCommitted_Lambda([Property](const FText& Text, ETextCommit::Type How)
					{
						if (How == ETextCommit::OnEnter || How == ETextCommit::OnUserMovedFocus)
						{
							const FString Typed = Text.ToString().TrimStartAndEnd();
							if (Typed.IsNumeric()) { SetSettingNumber(Property, FCString::Atod(*Typed)); }
						}
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ Arrow(TEXT("Icons.ChevronUp"), 1.0) ]
				+ SVerticalBox::Slot().AutoHeight()[ Arrow(TEXT("Icons.ChevronDown"), -1.0) ]
			];
	}

	/**
	 * Every setting of ours the search can find and set (S208a): "now the
	 * search bar also allow to find Settings parameters (and edit them!)
	 * but show them LAST". Read from the class: a tick, a number, a choice.
	 * Added after every verb, so they come last.
	 */
	void AddSettingEntries(TArray<FSurfSearchEntry>& Entries)
	{
		for (TFieldIterator<FProperty> It(USurfSettings::StaticClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Config) || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst))
			{
				continue;
			}
			const FString Category = Property->GetMetaData(TEXT("Category"));
			if (Category.IsEmpty() || Category == TEXT("Colours"))
			{
				continue;
			}
			FSurfSearchEntry Entry;
			Entry.bSetting = true;
			Entry.Label = Property->GetDisplayNameText();
			Entry.Section = FText::Format(LOCTEXT("SettingSection", "Settings › {0} ›"), FText::FromString(Category));
			Entry.Tip = Property->GetToolTipText();

			if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
			{
				Entry.Value = [Bool]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && Bool->GetPropertyValue_InContainer(S) ? LOCTEXT("SettingOn", "on") : LOCTEXT("SettingOff", "off"); };
				Entry.Run = [Bool, Property]()
				{
					if (USurfSettings* S = GetMutableDefault<USurfSettings>())
					{
						Bool->SetPropertyValue_InContainer(S, !Bool->GetPropertyValue_InContainer(S));
						NoteSettingChanged(Property);
					}
				};
			}
			else if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			{
				UEnum* Names = Enum->GetEnum();
				FNumericProperty* Under = Enum->GetUnderlyingProperty();
				if (!Names || !Under)
				{
					continue;
				}
				auto Read = [Enum, Under]() -> int64 { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? Under->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(S)) : 0; };
				Entry.Value = [Names, Read]() { return Names->GetDisplayNameTextByValue(Read()); };
				Entry.Run = [Names, Read, Enum, Under, Property]()
				{
					USurfSettings* S = GetMutableDefault<USurfSettings>();
					if (!S)
					{
						return;
					}
					// The next choice round, skipping hidden ones and a Count.
					const int32 Count = Names->NumEnums() - (Names->ContainsExistingMax() ? 1 : 0);
					int32 Index = Names->GetIndexByValue(Read());
					for (int32 Tries = 0; Tries < Count; ++Tries)
					{
						Index = (Index + 1) % Count;
						if (!Names->HasMetaData(TEXT("Hidden"), Index) && !Names->GetNameStringByIndex(Index).EndsWith(TEXT("Count")))
						{
							break;
						}
					}
					Under->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(S), Names->GetValueByIndex(Index));
					NoteSettingChanged(Property);
				};
			}
			else if (FByteProperty* Byte = CastField<FByteProperty>(Property))
			{
				UEnum* Names = Byte->Enum;
				if (!Names)
				{
					continue;
				}
				Entry.Value = [Names, Byte]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S ? Names->GetDisplayNameTextByValue(Byte->GetPropertyValue_InContainer(S)) : FText::GetEmpty(); };
				Entry.Run = [Names, Byte, Property]()
				{
					USurfSettings* S = GetMutableDefault<USurfSettings>();
					if (!S)
					{
						return;
					}
					const int32 Count = Names->NumEnums() - (Names->ContainsExistingMax() ? 1 : 0);
					int32 Index = Names->GetIndexByValue(Byte->GetPropertyValue_InContainer(S));
					for (int32 Tries = 0; Tries < Count; ++Tries)
					{
						Index = (Index + 1) % Count;
						if (!Names->HasMetaData(TEXT("Hidden"), Index) && !Names->GetNameStringByIndex(Index).EndsWith(TEXT("Count")))
						{
							break;
						}
					}
					Byte->SetPropertyValue_InContainer(S, static_cast<uint8>(Names->GetValueByIndex(Index)));
					NoteSettingChanged(Property);
				};
			}
			else if (CastField<FNumericProperty>(Property))
			{
				Entry.Number = Property;
				Entry.Run = []() {};
			}
			else
			{
				continue;
			}
			Entries.Add(MoveTemp(Entry));
		}
	}
}

void SSurfViewportBar::OpenSearch(const FVector2D& ScreenPosition)
{
	OpenSearch(ScreenPosition, nullptr, nullptr);
}

void SSurfViewportBar::OpenSearch(const FVector2D& ScreenPosition, TFunction<bool(const FSurfSearchEntry&)> Keep, TSharedPtr<SWidget> Parent)
{
	if (!FSlateApplication::IsInitialized() || !Mode.IsValid())
	{
		return;
	}
	if (TSharedPtr<IMenu> Open = SearchMenu.Pin())
	{
		Open->Dismiss();
	}

	TWeakObjectPtr<USurfEdMode> M = Mode;
	SearchOpenedAt = FPlatformTime::Seconds();

	// Everything that can be asked for by name: the bar's verbs, then the
	// switches that have keys of their own.
	using FEntry = FSurfSearchEntry;
	// Built once and kept (S175a): building every verb on each X was the
	// search's third of a second - "search_open" led the record's hitches.
	TSharedRef<TArray<FEntry>> Entries = SearchEntries.IsValid() ? SearchEntries.ToSharedRef() : MakeShared<TArray<FEntry>>();
	if (Entries->Num() == 0)
	{
	TSet<FString> Seen;
	for (const FSurfVerb& Verb : QuickVerbs)
	{
		Entries->Add({ Verb.Label, Verb.Tip, Verb.Run, Verb.IsEnabled });
		Entries->Last().Verb = MakeShared<FSurfVerb>(Verb);
		Seen.Add(Verb.Label.ToString().ToLower());
	}

	// Then everything else there is: the verbs that only ever lived on the
	// docked panel are searchable by name from the viewport.
	for (const FSurfVerb& Verb : BuildAllVerbs(M))
	{
		const FString Label = Verb.Label.ToString().ToLower();
		if (!Seen.Contains(Label))
		{
			Seen.Add(Label);
			Entries->Add({ Verb.Label, Verb.Tip, Verb.Run, Verb.IsEnabled });
			Entries->Last().Verb = MakeShared<FSurfVerb>(Verb);
		}
	}
	auto Add = [&](const FText& Label, const FText& Tip, TFunction<void()> Run)
	{
		Entries->Add({ Label, Tip, Run, nullptr });
	};
	Add(LOCTEXT("SPromote", "Promote to Level"), LOCTEXT("SPromoteTip", "The selected prefab instance becomes a level of its own (LI_), holding what the prefab held as its own geometry, editable in the outliner and able to hold anything a level holds."),
		[M]() { if (M.IsValid()) { M->PromoteToScene(M->SuggestSceneName()); } });
	// The kinds, Snap, Soft, None and Invert are the pool's own verbs since
	// (S252a): the search listed each twice.
	// The windows (S157h): "flag is GONE from the X search... i needed it
	// right now." Each left the pool for its one home; the search knows
	// them all by name.
	// The windows are verbs since S177a and the search lists the pool, so
	// only the Paint tab - no verb of its own - is named here (S247f).
	Add(LOCTEXT("SWinPaint", "Paint tab (SidePanel)"), LOCTEXT("SWinPaintTip", "The paint brush in the SidePanel: channels, colours, strength, size, the adjustments."), []() { SurfPaintTab::Open(); });
	Add(LOCTEXT("SSoftProfile", "Soft falloff: next profile (Shift+O)"), LOCTEXT("SSoftProfileTip", "Smooth, sphere, root, inverse square, sharp, linear, constant, random."), [M]() { if (M.IsValid()) { M->CycleSoftFalloff(1); } });
	Add(LOCTEXT("SView", "Modelling / shaded view (Shift+Z)"), LOCTEXT("SViewTip", "Unlit, or the level's lighting."), [M]() { if (M.IsValid()) { M->ToggleShadedView(); } });
	Add(LOCTEXT("SFocus", "Focus the selection (F)"), LOCTEXT("SFocusTip", "Frame what is selected."), [M]() { if (M.IsValid()) { M->FocusSelection(GCurrentLevelEditingViewportClient); } });
	AddSettingEntries(*Entries);
	SearchEntries = Entries;
	}

	// Every word typed is the start of a word of the name: "sub" finds
	// Subdivide, "sel n" finds Select none. Only what can run right now.
	auto Matches = [Entries, Keep](const FString& Typed) -> TArray<int32>
	{
		TArray<FString> Words;
		Typed.ToLower().ParseIntoArrayWS(Words);
		TArray<int32> Out;
		// Nothing typed, nothing listed: the hint shows, and no verb is
		// asked whether it can run (S175a).
		if (Words.Num() == 0)
		{
			return Out;
		}
		const USurfSettings* Finds = GetDefault<USurfSettings>();
		const bool bSettings = !Finds || Finds->bSearchFindsSettings;
		for (int32 I = 0; I < Entries->Num(); ++I)
		{
			const FEntry& Entry = (*Entries)[I];
			if (Entry.IsEnabled && !Entry.IsEnabled())
			{
				continue;
			}
			if (Entry.bSetting && !bSettings)
			{
				continue;
			}
			if (Keep && !Keep(Entry))
			{
				continue;
			}
			TArray<FString> NameWords;
			// A setting is found by its section's words too (S208a).
			(Entry.Label.ToString() + TEXT(" ") + Entry.Section.ToString()).ToLower().ParseIntoArrayWS(NameWords);
			bool bAll = true;
			for (const FString& Word : Words)
			{
				bool bAny = false;
				for (const FString& NameWord : NameWords)
				{
					if (NameWord.StartsWith(Word))
					{
						bAny = true;
						break;
					}
				}
				if (!bAny)
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				Out.Add(I);
			}
		}
		return Out;
	};

	TSharedRef<int32> Highlight = MakeShared<int32>(0);
	TSharedRef<FString> Typed = MakeShared<FString>();
	TSharedRef<TSharedPtr<SVerticalBox>> ListHolder = MakeShared<TSharedPtr<SVerticalBox>>();
	// A row's left click runs it (S211a: "some i cant seem to edit" - only
	// Enter did); filled in once Commit exists below.
	TSharedRef<TFunction<void(int32)>> ClickRow = MakeShared<TFunction<void(int32)>>();
	TSharedPtr<SVerticalBox> List;
	TSharedPtr<SEditableTextBox> Box;
	TSharedRef<TSharedPtr<SEditableTextBox>> BoxHolder = MakeShared<TSharedPtr<SEditableTextBox>>();
	TWeakPtr<SSurfViewportBar> Self = SharedThis(this);

	auto Refresh = [Entries, Matches, Highlight, Typed, ListHolder, ClickRow]()
	{
		TSharedPtr<SVerticalBox> List = *ListHolder;
		if (!List.IsValid())
		{
			return;
		}
		List->ClearChildren();
		const TArray<int32> Found = Matches(*Typed);
		*Highlight = FMath::Clamp(*Highlight, 0, FMath::Max(0, Found.Num() - 1));
		if (Found.Num() == 0)
		{
			List->AddSlot().AutoHeight().Padding(4.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(Typed->IsEmpty() ? LOCTEXT("SearchHint", "type a verb - or a note, Enter writes it to the session record") : LOCTEXT("SearchNote", "no verb by that name - Enter writes it as a note"))
				.Font(SurfTheme::Italic(8))
				.ColorAndOpacity(SurfTheme::Dim())
			];
			return;
		}
		for (int32 Row = 0; Row < FMath::Min(Found.Num(), 12); ++Row)
		{
			const FEntry& Entry = (*Entries)[Found[Row]];
			const bool bHot = Row == *Highlight;
			// A result's right-click is its verb's (S199a): "right click
			// xsearch menu results to quickfav/sidebar".
			TSharedPtr<FSurfVerb> RowVerb = Entry.Verb;
			TSharedRef<TWeakPtr<SWidget>> RowSelf = MakeShared<TWeakPtr<SWidget>>();
			TSharedRef<SBorder> RowWidget = SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush(bHot ? "Brushes.Select" : "NoBorder"))
				.Padding(FMargin(6.0f, 2.0f))
				.OnMouseButtonDown_Lambda([RowVerb, RowSelf, ClickRow, Row, bNumber = Entry.Number != nullptr](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (RowVerb.IsValid() && Event.GetEffectingButton() == EKeys::RightMouseButton)
					{
						SurfMenu::OpenVerbContext(*RowVerb, RowSelf->Pin());
						return FReply::Handled();
					}
					// A number's row leaves the click to its field and arrows.
					if (Event.GetEffectingButton() == EKeys::LeftMouseButton && !bNumber && *ClickRow)
					{
						(*ClickRow)(Row);
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
				.ToolTipText(Entry.Tip)
				[
					SNew(SHorizontalBox)
					// A setting says where it lives first, dim (S208a: "make
					// sure user will know what they editing").
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(Entry.Section)
						.Visibility(Entry.Section.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						.Font(SurfTheme::Font(8))
						.ColorAndOpacity(SurfTheme::Dim())
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						// A verb that says its state says it here too (Grid: on).
						.Text_Lambda([RowVerb, Label = Entry.Label]() { return RowVerb.IsValid() && RowVerb->LiveLabel ? RowVerb->LiveLabel() : Label; })
						.Font(SurfTheme::Font(bHot ? "Bold" : "Regular", 9))
						.ColorAndOpacity(bHot ? SurfTheme::Bright() : SurfTheme::Text())
					]
					// Its value: a word, or a knob for a number.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						Entry.Number ? MakeSettingKnob(Entry.Number) :
						(Entry.Value
							? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text_Lambda([Value = Entry.Value]() { return Value(); }).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::On()))
							: SNullWidget::NullWidget)
					]
				];
			*RowSelf = RowWidget;
			List->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
			[
				RowWidget
			];
		}
	};
	TSharedRef<TFunction<void()>> RefreshRef = MakeShared<TFunction<void()>>(Refresh);

	auto Commit = [M, Entries, Matches, Highlight, Typed, Self, RefreshRef](bool bAsNote)
	{
		// A setting keeps the search open (S208a): set, seen, set again.
		{
			const TArray<int32> Kept = Matches(*Typed);
			if (!bAsNote && Kept.Num() > 0 && (*Entries)[Kept[FMath::Clamp(*Highlight, 0, Kept.Num() - 1)]].bSetting)
			{
				const FEntry& Entry = (*Entries)[Kept[FMath::Clamp(*Highlight, 0, Kept.Num() - 1)]];
				SurfSession::Verb(TEXT("search_set"), Entry.Label.ToString());
				if (Entry.Number)
				{
					TArray<FString> Words;
					Typed->ParseIntoArrayWS(Words);
					if (Words.Num() > 0 && Words.Last().IsNumeric())
					{
						SetSettingNumber(Entry.Number, FCString::Atod(*Words.Last()));
					}
				}
				else if (Entry.Run)
				{
					Entry.Run();
				}
				(*RefreshRef)();
				return;
			}
		}
		if (TSharedPtr<SSurfViewportBar> Bar = Self.Pin())
		{
			if (TSharedPtr<IMenu> Open = Bar->SearchMenu.Pin())
			{
				Open->Dismiss();
			}
		}
		if (!M.IsValid())
		{
			return;
		}
		const TArray<int32> Found = Matches(*Typed);
		if (!bAsNote && Found.Num() > 0)
		{
			const FEntry& Entry = (*Entries)[Found[FMath::Clamp(*Highlight, 0, Found.Num() - 1)]];
			SurfSession::Verb(TEXT("search_run"), Entry.Label.ToString());
			if (Entry.Run)
			{
				M->NoteVerb(Entry.Label.ToString(), Entry.Run);
				Entry.Run();
			}
		}
		else
		{
			M->AddNote(*Typed);
		}
	};

	*ClickRow = [Highlight, Commit](int32 Row)
	{
		*Highlight = Row;
		Commit(/*bAsNote*/ false);
	};

	TSharedRef<SWidget> Widget =
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(6.0f)
		[
			// Wider since S209a: the settings' names were cut short at 260.
			SNew(SBox).WidthOverride(400.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
					SAssignNew(Box, SEditableTextBox)
					.HintText(LOCTEXT("SearchBoxHint", "verb, or a note"))
					.Font(SurfTheme::Font(10))
					.OnTextChanged_Lambda([Typed, Highlight, RefreshRef, Self, BoxHolder](const FText& Text)
					{
						/**
						 * The key that opened this also typed itself into it
						 * (S96a): Slate sends the character to whatever has
						 * focus, and by the time it arrives the box does.
						 * Three notes about it - "dont default the xsearch to
						 * x", "shouldnt not start an x-search with x
						 * automatically", "also dont have x written by
						 * default, having none".
						 *
						 * One character, in the same breath as the opening:
						 * nobody types that fast, so it is the echo and it
						 * goes. Any character - the key is a setting, and a
						 * rule that names one key would be wrong the day it
						 * is changed.
						 */
						const FString Now = Text.ToString();
						TSharedPtr<SSurfViewportBar> Bar = Self.Pin();
						if (Bar.IsValid() && Typed->IsEmpty() && Now.Len() == 1
							&& FPlatformTime::Seconds() - Bar->SearchOpenedAt < 0.2)
						{
							Bar->SearchOpenedAt = 0.0;
							if (BoxHolder->IsValid())
							{
								(*BoxHolder)->SetText(FText::GetEmpty());
							}
							return;
						}

						*Typed = Now;
						*Highlight = 0;
						(*RefreshRef)();
					})
					.OnTextCommitted_Lambda([Commit](const FText&, ETextCommit::Type How)
					{
						if (How == ETextCommit::OnEnter)
						{
							const FModifierKeysState Keys = FSlateApplication::Get().GetModifierKeys();
							Commit(Keys.IsControlDown() || Keys.IsShiftDown());
						}
					})
					.OnKeyDownHandler_Lambda([Highlight, RefreshRef, Self](const FGeometry&, const FKeyEvent& Event) -> FReply
					{
						if (Event.GetKey() == EKeys::Down) { ++*Highlight; (*RefreshRef)(); return FReply::Handled(); }
						if (Event.GetKey() == EKeys::Up) { *Highlight = FMath::Max(0, *Highlight - 1); (*RefreshRef)(); return FReply::Handled(); }
						if (Event.GetKey() == EKeys::Escape)
						{
							if (TSharedPtr<SSurfViewportBar> Bar = Self.Pin())
							{
								if (TSharedPtr<IMenu> Open = Bar->SearchMenu.Pin()) { Open->Dismiss(); }
							}
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
					]
					// The settings tick beside the field (S209a), so it is
					// found where it acts.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SCheckBox)
						.ToolTipText(LOCTEXT("SearchSettingsTip", "The settings in the results too, after the verbs."))
						.IsChecked_Lambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return !S || S->bSearchFindsSettings ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([RefreshRef](ECheckBoxState State)
						{
							if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->bSearchFindsSettings = State == ECheckBoxState::Checked; S->SaveConfig(); }
							(*RefreshRef)();
						})
						[
							SNew(STextBlock).Text(LOCTEXT("SearchSettingsWord", "settings")).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Dim())
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SAssignNew(List, SVerticalBox)
				]
			]
		];

	*ListHolder = List;
	*BoxHolder = Box;
	Refresh();

	SearchMenu = FSlateApplication::Get().PushMenu(
		Parent.IsValid() ? Parent.ToSharedRef() : AsShared(),
		FWidgetPath(),
		Widget,
		ScreenPosition - FVector2D(20.0, 12.0),
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

	if (Box.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);

		// The key that opened the frame is still on its way to whatever has
		// focus, as a character: X opened the search and then typed itself
		// into it. The field is emptied once, on the next tick, after that
		// character has landed - nobody types in the sixteen milliseconds
		// between, and the alternative (focusing a tick later) loses the
		// first letter of a fast hand instead.
		TWeakPtr<SEditableTextBox> WeakBox = Box;
		TSharedRef<FString> TypedRef = Typed;
		TSharedRef<TFunction<void()>> RefreshAgain = RefreshRef;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakBox, TypedRef, RefreshAgain](float) -> bool
			{
				if (TSharedPtr<SEditableTextBox> Live = WeakBox.Pin())
				{
					Live->SetText(FText::GetEmpty());
				}
				*TypedRef = FString();
				(*RefreshAgain)();
				return false;
			}), 0.0f);
	}
	SurfSession::Gesture(TEXT("search_open"));
}

void SSurfViewportBar::OpenOptions(const FSurfVerb& Verb, const FGeometry&)
{
	// The same right-click as everywhere (S196a); Options... opens the frame.
	SurfMenu::OpenVerbContext(Verb, nullptr, static_cast<int32>(ESurfIconSurface::Sidebar));
}

EVisibility SSurfViewportBar::GetVerbVisibility() const
{
	/**
	 * Open unless told otherwise.
	 *
	 * Folding on hover was worse than the problem it solved: collapsing the
	 * bar changes its size, which changes whether the cursor is over it,
	 * which opens it again - so it blinked on and off at the edge. A bar that
	 * flickers is a broken bar, whatever it is trying to do. The button folds
	 * it deliberately instead.
	 */
	// SelfHitTestInvisible, not Visible: with no panel behind the verbs the
	// gaps between the chips are viewport, and a click in one belongs to the
	// scene (S90a).
	return bFolded ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible;
}

FText SSurfViewportBar::GetTip() const
{
	if (!Mode.IsValid())
	{
		return FText::GetEmpty();
	}

	// A transform in progress is the only thing worth the line while it is
	// running: what it is doing, what it is locked to, and what has been
	// typed so far.
	const FString Running = Mode->DescribeModal();
	if (!Running.IsEmpty())
	{
		return FText::FromString(Running);
	}

	// What the last verb did or why it would not, for a few seconds after.
	// The docked panel has always said it; nobody reads a panel while
	// modelling, so a refusal there was the same as silence.
	if (const USurfInspector* Inspector = Mode->Inspector)
	{
		if (Inspector->ResultSerial > 0 && !Inspector->LastResult.IsEmpty()
			&& FPlatformTime::Seconds() - Inspector->ResultAt < ResultLineSeconds)
		{
			return FText::FromString(Inspector->LastResult);
		}
	}

	// Otherwise the keys of the moment (S156f), in the scheme he chose.
	auto K = [](ESurfAction Action, const TCHAR* Word) -> FString
	{
		const FString Key = SurfKeymap::Describe(Action);
		return (Key.IsEmpty() || Key.Equals(TEXT("unbound"))) ? FString() : FString::Printf(TEXT("%s %s"), *Key, Word);
	};
	auto Join = [](std::initializer_list<FString> Parts) -> FText
	{
		FString Out;
		for (const FString& Part : Parts)
		{
			if (!Part.IsEmpty())
			{
				Out += Out.IsEmpty() ? Part : (TEXT("  \u00b7  ") + Part);
			}
		}
		return FText::FromString(Out);
	};
	if (Mode->IsSlicing())
	{
		return Join({ TEXT("click 2 or 3 points"), TEXT("Enter cut"), TEXT("Shift/Ctrl+Enter one side"), TEXT("Esc") });
	}
	if (Mode->IsPainting())
	{
		return Join({ TEXT("drag paints"), TEXT("Ctrl-drag down"), TEXT("[ ] size"), TEXT("Esc") });
	}
	if (Mode->GetSubMode() == ESurfSubMode::Object)
	{
		return Join({ K(ESurfAction::Translate, TEXT("move")), K(ESurfAction::Rotate, TEXT("rotate")), K(ESurfAction::Scale, TEXT("scale")),
			K(ESurfAction::SubModeVertex, TEXT("vertices")), K(ESurfAction::Duplicate, TEXT("duplicate")), K(ESurfAction::Delete, TEXT("delete")),
			K(ESurfAction::QuickFavorites, TEXT("favorites")), K(ESurfAction::Search, TEXT("search")) });
	}
	return Join({ K(ESurfAction::Translate, TEXT("move")), K(ESurfAction::Rotate, TEXT("rotate")), K(ESurfAction::Scale, TEXT("scale")),
		K(ESurfAction::Extrude, TEXT("extrude")), K(ESurfAction::Weld, TEXT("weld")), K(ESurfAction::InsertLoop, TEXT("loop")),
		K(ESurfAction::SelectAll, TEXT("all")), K(ESurfAction::SubModeObject, TEXT("pieces")),
		K(ESurfAction::QuickFavorites, TEXT("favorites")), K(ESurfAction::Search, TEXT("search")) });
}

EVisibility SSurfViewportBar::GetTipVisibility() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (Settings && !Settings->bShowTips)
	{
		return EVisibility::Collapsed;
	}
	if (GetTip().IsEmpty())
	{
		return EVisibility::Collapsed;
	}
	return EVisibility::SelfHitTestInvisible;
}

FText SSurfViewportBar::GetStatus() const
{
	// What is picked, and nothing else (S93a). The sentence about what just
	// happened went to a panel of its own that can be closed; this line is a
	// reading, so it stays with the other readings.
	if (!Mode.IsValid())
	{
		return FText::GetEmpty();
	}

	const ASurfGroup* Group = Mode->GetSelectedPatch().Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Mode->GetSelectedPatch().PatchId) : nullptr;

	/**
	 * A brush picked and no patch: say that it is a brush (S100a).
	 *
	 * The readout said "no patch selected" while a box sat picked in front of
	 * him, which is true and useless. A brush is a different kind of thing -
	 * sides, not a grid - and the line that says what is picked has to be
	 * able to say so.
	 */
	if (!Patch)
	{
		const FSurfFaceRef Active = Mode->GetActiveFace();
		const TArray<FSurfFaceRef>& Faces = Mode->GetSelection();
		if (Active.IsValid() || Faces.Num() > 0)
		{
			const FSurfFaceRef& Any = Active.IsValid() ? Active : Faces[0];
			const ASurfGroup* BrushGroup = Any.Group.Get();
			const FSurfBrush* Brush = BrushGroup ? BrushGroup->FindBrush(Any.BrushId) : nullptr;
			const int32 Sides = Brush ? Brush->Faces.Num() : 0;
			return FText::FromString(FString::Printf(TEXT("primitive, %d sides  %d picked"), Sides, Faces.Num()));
		}
		// Placed things picked (S252a): the line said "nothing selected"
		// beside a picked prefab.
		if (GEditor)
		{
			int32 Prefabs = 0;
			int32 Others = 0;
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				const AActor* Actor = Cast<AActor>(*It);
				if (!Actor) { continue; }
				if (Actor->IsA<ASurfPrefabInstance>()) { ++Prefabs; }
				else if (Actor->Tags.Contains(USurfEdMode::PlacedMeshTag)) { ++Others; }
			}
			if (Prefabs + Others > 0)
			{
				if (Others == 0) { return FText::FromString(FString::Printf(TEXT("%d placed prefab%s"), Prefabs, Prefabs == 1 ? TEXT("") : TEXT("s"))); }
				if (Prefabs == 0) { return FText::FromString(FString::Printf(TEXT("%d placed %s"), Others, Others == 1 ? TEXT("mesh or level") : TEXT("meshes or levels"))); }
				return FText::FromString(FString::Printf(TEXT("%d placed things"), Prefabs + Others));
			}
		}
		return LOCTEXT("NoPatch", "nothing selected");
	}

	const FVector Normal = SurfPatchSurfacing::AverageNormal(*Patch);
	const TCHAR* Facing = Normal.Z > 0.7 ? TEXT("up") : (Normal.Z < -0.7 ? TEXT("DOWN") : TEXT("sideways"));

	const int32 Patches = Mode->GetSelectedPatches().Num();
	return FText::FromString(FString::Printf(TEXT("%s %dx%d  %s  %d sel%s"),
		Patch->Type == ESurfPatchType::Curve ? TEXT("Curve") : TEXT("Sheet"),
		Patch->Width, Patch->Height, Facing, Mode->GetPointSelection().Num(),
		Patches > 1 ? *FString::Printf(TEXT("  (%d patches)"), Patches) : TEXT("")));
}

namespace
{
	/** Texels a metre across a selection: the lowest and the highest of them, and whether they agree. */
	bool MeasureDensity(const USurfEdMode& Mode, double& OutLow, double& OutHigh)
	{
		const USurfInspector* Inspector = Mode.Inspector;
		if (!Inspector)
		{
			return false;
		}

		OutLow = TNumericLimits<double>::Max();
		OutHigh = 0.0;
		int32 Seen = 0;

		TArray<FSurfPatchRef> Patches = Mode.GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode.PatchesHeldBySelection())
		{
			Patches.AddUnique(Held);
		}
		for (const FSurfPatchRef& Ref : Patches)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch)
			{
				continue;
			}
			const FVector2D Density = SurfPatchSurfacing::TexelDensity(*Patch, Inspector->TextureTexels);
			for (double Each : { Density.X, Density.Y })
			{
				if (Each > 0.0)
				{
					OutLow = FMath::Min(OutLow, Each);
					OutHigh = FMath::Max(OutHigh, Each);
					++Seen;
				}
			}
		}

		// The selected brush faces count the same way (S67a).
		for (const FSurfFaceRef& Ref : Mode.GetSelection())
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (!Brush || !Brush->Faces.IsValidIndex(Ref.FaceIndex))
			{
				continue;
			}
			const FVector2D Density = SurfProjection::TexelDensity(Brush->Faces[Ref.FaceIndex].UV, Inspector->TextureTexels);
			for (double Each : { Density.X, Density.Y })
			{
				if (Each > 0.0)
				{
					OutLow = FMath::Min(OutLow, Each);
					OutHigh = FMath::Max(OutHigh, Each);
					++Seen;
				}
			}
		}

		return Seen > 0;
	}
}

FText SSurfViewportBar::GetDensity() const
{
	double Low = 0.0;
	double High = 0.0;
	if (!Mode.IsValid() || !MeasureDensity(*Mode, Low, High))
	{
		return FText::GetEmpty();
	}

	// A hair apart is the same number to the hand; a tenth apart is not.
	if (High <= Low * 1.02)
	{
		return FText::FromString(FString::Printf(TEXT("%.0f texels/m"), High));
	}
	return FText::FromString(FString::Printf(TEXT("%.0f-%.0f texels/m"), Low, High));
}

FSlateColor SSurfViewportBar::GetDensityColour() const
{
	double Low = 0.0;
	double High = 0.0;
	if (Mode.IsValid() && MeasureDensity(*Mode, Low, High) && High > Low * 1.02)
	{
		return SurfTheme::Warn();
	}
	return SurfTheme::Dim();
}

FText SSurfViewportBar::GetStack() const
{
	// The level the selection is on, whenever a patch is selected - a number
	// that only appears once it is above zero is one nobody sees change.
	USurfEdMode* Live = Mode.Get();
	const int32 Depth = Live ? Live->SelectionStackDepth() : 0;
	if (Depth <= 0)
	{
		return FText::GetEmpty();
	}
	return FText::FromString(FString::Printf(TEXT("layer %d"), Depth - 1));
}

FSlateColor SSurfViewportBar::GetStackColour() const
{
	const USurfSettings* StackSettings = GetDefault<USurfSettings>();
	const int32 Warn = StackSettings ? FMath::Max(1, StackSettings->StackDepthWarn) : 3;

	USurfEdMode* Live = Mode.Get();
	const int32 Depth = Live ? Live->SelectionStackDepth() : 0;
	if (Depth > Warn)
	{
		return SurfTheme::Warn();
	}
	return SurfTheme::Dim();
}

FSlateColor SSurfViewportBar::GetStatusColour() const
{
	if (!Mode.IsValid())
	{
		return SurfTheme::Dim();
	}

	if (Mode->DescribeModal().IsEmpty())
	{
		if (const USurfInspector* Inspector = Mode->Inspector)
		{
			if (Inspector->ResultSerial > 0 && !Inspector->LastResult.IsEmpty()
				&& FPlatformTime::Seconds() - Inspector->ResultAt < ResultLineSeconds)
			{
				// Amber for a refusal, so the eye learns that colour means
				// "it did not happen, and here is why"; green for a result.
				return Inspector->LastResultWasRefusal()
					? SurfTheme::Warn()
					: SurfTheme::Good();
			}
		}
	}

	const ASurfGroup* Group = Mode->GetSelectedPatch().Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Mode->GetSelectedPatch().PatchId) : nullptr;

	if (!Patch)
	{
		return SurfTheme::Dim();
	}

	// Red only when something is actually wrong, so red keeps meaning
	// something: a surface facing away from you is almost never intended.
	const FVector Normal = SurfPatchSurfacing::AverageNormal(*Patch);
	if (Normal.Z < -0.7)
	{
		return SurfTheme::Bad();
	}

	return SurfTheme::Text();
}

// --- The Actions window (S193e) ----------------------------------------------

namespace SurfActionsTab
{
	const FName TabName(TEXT("SurfacingProActions"));

	/** The palette, rebuilt when the mode comes or goes: its buttons act on the mode that built them. */
	class SSurfActionsHost : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSurfActionsHost) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			ChildSlot[ SAssignNew(Holder, SBox) ];
			Rebuild();
			RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
			{
				if (Live() != Built.Get())
				{
					Rebuild();
				}
				return EActiveTimerReturnType::Continue;
			}));
		}

	private:
		static USurfEdMode* Live()
		{
			return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
		}

		void Rebuild()
		{
			USurfEdMode* Mode = Live();
			Built = Mode;
			if (!Mode)
			{
				Holder->SetContent(
					SNew(SBox).Padding(8.0f)
					[
						SNew(STextBlock).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Dim())
						.Text(LOCTEXT("ActionsModeOff", "Enter the SurfacingPro mode to use its actions."))
					]);
				return;
			}
			Holder->SetContent(SNew(SScrollBox) + SScrollBox::Slot()[ SNew(SSurfToolPalette).Mode(Mode) ]);
		}

		TSharedPtr<SBox> Holder;
		TWeakObjectPtr<USurfEdMode> Built;
	};

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("ActionsTabLabel", "Actions"))
			[
				SNew(SSurfActionsHost)
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized() || FGlobalTabmanager::Get()->HasTabSpawner(TabName))
		{
			return;
		}
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
			.SetDisplayName(LOCTEXT("ActionsTabTitle", "SurfacingPro Actions"))
			.SetTooltipText(LOCTEXT("ActionsTabTip", "Every SurfacingPro action by family: run, star, add to the sidebar."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.layout-grid"));
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
	}

	void Open()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		Register();
		FGlobalTabmanager::Get()->TryInvokeTab(TabName);
		SurfSession::Verb(TEXT("actions_window"), TEXT("open"));
	}
}

#undef LOCTEXT_NAMESPACE
