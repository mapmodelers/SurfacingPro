// Copyright MapModelers. All Rights Reserved.

#include "SurfUVView.h"
#include "SurfTheme.h"
#include "SurfEdMode.h"
#include "SurfParamsView.h"
#include "SurfToolPalette.h"
#include "SurfMenu.h"
#include "SurfWindows.h"

#include "SurfGroup.h"
#include "SurfSettings.h"
#include "SurfKeymap.h"
#include "SurfSnapState.h"
#include "SurfSession.h"
#include "SurfInspector.h"
#include "SurfPatchSurfacing.h"
#include "SurfVerticalLabel.h"
#include "SurfIcons.h"
#include "SurfNumberField.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Rendering/RenderingCommon.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "Styling/StyleColors.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "SurfPreviewMaterial.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProUV"

namespace SurfUVPanel { void Toggle(); float Covered(); }

// --- What the last G / R / S did, for the strip under the view ----------------
//
// A move, turn or scale that was confirmed leaves its numbers here; the strip
// shows them as fields the hand can set exactly, and a scale gets the ratios
// a hand reaches for. Setting a field applies the difference to the same
// points about the same pivot, so the strip is the modal's knob afterwards.

namespace SurfUVAdjust
{
	enum class EKind : uint8 { None, Move, Turn, Scale };
	static EKind Kind = EKind::None;
	static bool bShown = false;
	static FVector2D Pivot = FVector2D::ZeroVector;
	static FVector2D Moved = FVector2D::ZeroVector;
	static double Turned = 0.0;
	static FVector2D Scaled = FVector2D(1.0, 1.0);
}

// --- The maths --------------------------------------------------------------

namespace SurfUV
{
	void GatherNets(const USurfEdMode& Mode, TArray<FSurfUVNet>& Out)
	{
		Out.Reset();

		// Every patch the selection reaches into, the active one first: the
		// same list every verb runs over (D54), so what is drawn here is what
		// a verb would act on.
		TArray<FSurfPatchRef> Targets = Mode.GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode.PatchesHeldBySelection())
		{
			Targets.AddUnique(Held);
		}
		const FSurfPatchRef Active = Mode.GetSelectedPatch();
		if (Active.IsValid() && Targets.Contains(Active))
		{
			Targets.Remove(Active);
			Targets.Insert(Active, 0);
		}

		const TArray<FSurfPointRef>& Points = Mode.GetPointSelection();

		for (const FSurfPatchRef& Ref : Targets)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || Patch->Points.Num() != Patch->Width * Patch->Height)
			{
				continue;
			}

			FSurfUVNet& Net = Out.AddDefaulted_GetRef();
			Net.Patch = Ref;
			Net.Width = Patch->Width;
			Net.Height = Patch->Height;
			Net.bFrozen = Patch->Mapping == ESurfPatchMapping::Manual;
			Net.bActive = Ref == Active;
			Net.UVs.Reserve(Patch->Points.Num());
			Net.Selected.Init(false, Patch->Points.Num());

			for (const FSurfControlPoint& Point : Patch->Points)
			{
				Net.UVs.Add(Point.UV);
			}

			for (const FSurfPointRef& Selected : Points)
			{
				if (Selected.PatchId == Ref.PatchId && Selected.Group == Ref.Group
					&& Selected.X >= 0 && Selected.X < Net.Width && Selected.Y >= 0 && Selected.Y < Net.Height)
				{
					Net.Selected[Selected.Y * Net.Width + Selected.X] = true;
				}
			}

			/**
			 * Stretch (S233a): each cell's texture area over its surface
			 * area, against the net's own median, so a net that is mapped
			 * evenly reads 0 everywhere whatever its scale, and a cell
			 * pulled wide or squeezed reads by how much.
			 */
			const int32 Cells = (Net.Width - 1) * (Net.Height - 1);
			if (Cells > 0)
			{
				TArray<double> Ratios;
				Ratios.Reserve(Cells);
				for (int32 Y = 0; Y + 1 < Net.Height; ++Y)
				{
					for (int32 X = 0; X + 1 < Net.Width; ++X)
					{
						const FSurfControlPoint& A = Patch->Points[Y * Net.Width + X];
						const FSurfControlPoint& B = Patch->Points[Y * Net.Width + X + 1];
						const FSurfControlPoint& C = Patch->Points[(Y + 1) * Net.Width + X + 1];
						const FSurfControlPoint& D = Patch->Points[(Y + 1) * Net.Width + X];
						const double Surface = 0.5 * FVector::CrossProduct(C.Pos - A.Pos, D.Pos - B.Pos).Size();
						const FVector2D E = C.UV - A.UV;
						const FVector2D F = D.UV - B.UV;
						const double Texture = 0.5 * FMath::Abs(E.X * F.Y - E.Y * F.X);
						Ratios.Add(Surface > 1.e-9 ? Texture / Surface : 0.0);
					}
				}
				TArray<double> Sorted = Ratios;
				Sorted.Sort();
				const double Median = Sorted[Sorted.Num() / 2];
				Net.Stretch.Reserve(Cells);
				for (const double Ratio : Ratios)
				{
					const float Log = (Median > 1.e-12 && Ratio > 1.e-12) ? static_cast<float>(FMath::Log2(Ratio / Median)) : (Ratio > 1.e-12 ? 1.0f : 0.0f);
					Net.Stretch.Add(FMath::Clamp(Log, -1.0f, 1.0f));
				}
			}
		}
	}

	FLinearColor StretchColour(float Log)
	{
		// Blue tight, green even, red stretched: the same three every UV
		// editor's hand already knows.
		const FLinearColor Tight(0.15f, 0.35f, 1.0f);
		const FLinearColor Even(0.15f, 0.85f, 0.25f);
		const FLinearColor Wide(1.0f, 0.2f, 0.1f);
		return Log < 0.0f ? FMath::Lerp(Even, Tight, -Log) : FMath::Lerp(Even, Wide, Log);
	}

	FBox2D BoundsOf(const TArray<FSurfUVNet>& Nets)
	{
		FBox2D Bounds(ForceInit);
		for (const FSurfUVNet& Net : Nets)
		{
			for (const FVector2D& UV : Net.UVs)
			{
				Bounds += UV;
			}
		}

		// Nothing selected, or a net with no area: the tile itself is the
		// thing worth looking at.
		if (!Bounds.bIsValid || Bounds.GetSize().GetMin() < KINDA_SMALL_NUMBER)
		{
			const FVector2D Centre = Bounds.bIsValid ? Bounds.GetCenter() : FVector2D(0.5, 0.5);
			Bounds = FBox2D(Centre - FVector2D(0.5, 0.5), Centre + FVector2D(0.5, 0.5));
		}
		return Bounds;
	}

	FView Frame(const FBox2D& Bounds, const FVector2D& WidgetSize, double MarginPixels)
	{
		FView Out;

		const FVector2D Size = FVector2D(FMath::Max(32.0, WidgetSize.X), FMath::Max(32.0, WidgetSize.Y));
		const FVector2D Room = FVector2D(
			FMath::Max(16.0, Size.X - 2.0 * MarginPixels),
			FMath::Max(16.0, Size.Y - 2.0 * MarginPixels));
		const FVector2D Span = FVector2D(
			FMath::Max(1.e-3, Bounds.GetSize().X),
			FMath::Max(1.e-3, Bounds.GetSize().Y));

		// The same scale on both axes, or a square tile would not look square.
		Out.Scale = FMath::Min(Room.X / Span.X, Room.Y / Span.Y);
		Out.Origin = Bounds.GetCenter() - Size * 0.5 / Out.Scale;
		return Out;
	}

	FView ZoomAt(const FView& From, const FVector2D& Pixel, double Factor)
	{
		FView Out = From;
		Out.Scale = FMath::Clamp(From.Scale * FMath::Max(0.01, Factor), 1.0, 100000.0);

		// The UV under the cursor stays under the cursor: that is what makes
		// a zoom feel like a zoom rather than a jump.
		const FVector2D Under = From.ToUV(Pixel);
		Out.Origin = Under - Pixel / Out.Scale;
		return Out;
	}

	bool PickPoint(const TArray<FSurfUVNet>& Nets, const FView& View, const FVector2D& Pixel,
		double RadiusPixels, FSurfPointRef& OutPoint)
	{
		double Best = RadiusPixels * RadiusPixels;
		bool bFound = false;

		// The nets are active first, and a tie goes to the one listed first,
		// so a point of the active patch wins where two sit on each other.
		for (const FSurfUVNet& Net : Nets)
		{
			for (int32 Y = 0; Y < Net.Height; ++Y)
			{
				for (int32 X = 0; X < Net.Width; ++X)
				{
					const double Distance = FVector2D::DistSquared(View.ToPixel(Net.At(X, Y)), Pixel);
					if (Distance < Best)
					{
						Best = Distance;
						OutPoint.Group = Net.Patch.Group;
						OutPoint.PatchId = Net.Patch.PatchId;
						OutPoint.X = X;
						OutPoint.Y = Y;
						bFound = true;
					}
				}
			}
		}
		return bFound;
	}

	bool PickEdge(const TArray<FSurfUVNet>& Nets, const FView& View, const FVector2D& Pixel,
		double RadiusPixels, FSurfPointRef& OutA, FSurfPointRef& OutB)
	{
		double Best = RadiusPixels * RadiusPixels;
		bool bFound = false;
		auto Try = [&](const FSurfUVNet& Net, int32 X0, int32 Y0, int32 X1, int32 Y1)
		{
			const FVector2D A = View.ToPixel(Net.At(X0, Y0));
			const FVector2D B = View.ToPixel(Net.At(X1, Y1));
			const FVector2D Near = FMath::ClosestPointOnSegment2D(Pixel, A, B);
			const double Distance = FVector2D::DistSquared(Near, Pixel);
			if (Distance < Best)
			{
				Best = Distance;
				OutA.Group = Net.Patch.Group;
				OutA.PatchId = Net.Patch.PatchId;
				OutA.X = X0;
				OutA.Y = Y0;
				OutB = OutA;
				OutB.X = X1;
				OutB.Y = Y1;
				bFound = true;
			}
		};
		for (const FSurfUVNet& Net : Nets)
		{
			for (int32 Y = 0; Y < Net.Height; ++Y)
			{
				for (int32 X = 0; X < Net.Width; ++X)
				{
					if (X + 1 < Net.Width)
					{
						Try(Net, X, Y, X + 1, Y);
					}
					if (Y + 1 < Net.Height)
					{
						Try(Net, X, Y, X, Y + 1);
					}
				}
			}
		}
		return bFound;
	}

	/**
	 * Snapping in the UV space (S230a: "can the snap options be available
	 * here too?"): with the snap switch on, the anchor - the point in hand,
	 * or the selection's middle under G - lands on an unpicked point of the
	 * nets within ten pixels, else on the tile grid's lines (whole tiles,
	 * quarters when they are drawn) within eight, each axis on its own.
	 * The correction to add to the move, in UV.
	 */
	FVector2D SnapCorrection(const TArray<FSurfUVNet>& Nets, const FView& View, const FVector2D& AnchorUV)
	{
		if (!SurfSnapState::On())
		{
			return FVector2D::ZeroVector;
		}
		const FVector2D AnchorPx = View.ToPixel(AnchorUV);
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (!Settings || Settings->bSnapToPoints)
		{
			double Best = 10.0 * 10.0;
			FVector2D Found = AnchorUV;
			bool bFound = false;
			for (const FSurfUVNet& Net : Nets)
			{
				for (int32 I = 0; I < Net.UVs.Num(); ++I)
				{
					if (Net.Selected.IsValidIndex(I) && Net.Selected[I])
					{
						continue;
					}
					const double Distance = FVector2D::DistSquared(View.ToPixel(Net.UVs[I]), AnchorPx);
					if (Distance < Best)
					{
						Best = Distance;
						Found = Net.UVs[I];
						bFound = true;
					}
				}
			}
			if (bFound)
			{
				return Found - AnchorUV;
			}
		}
		const double Step = View.Scale > 480.0 ? 0.25 : 1.0;
		FVector2D Correction = FVector2D::ZeroVector;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const double On = FMath::RoundToDouble(AnchorUV[Axis] / Step) * Step;
			if (FMath::Abs(On - AnchorUV[Axis]) * View.Scale <= 8.0)
			{
				Correction[Axis] = On - AnchorUV[Axis];
			}
		}
		return Correction;
	}

	void PointsInBox(const TArray<FSurfUVNet>& Nets, const FView& View, const FBox2D& PixelBox,
		TArray<FSurfPointRef>& Out)
	{
		for (const FSurfUVNet& Net : Nets)
		{
			for (int32 Y = 0; Y < Net.Height; ++Y)
			{
				for (int32 X = 0; X < Net.Width; ++X)
				{
					if (PixelBox.IsInside(View.ToPixel(Net.At(X, Y))))
					{
						FSurfPointRef Ref;
						Ref.Group = Net.Patch.Group;
						Ref.PatchId = Net.Patch.PatchId;
						Ref.X = X;
						Ref.Y = Y;
						Out.Add(Ref);
					}
				}
			}
		}
	}
}

// --- The widget -------------------------------------------------------------

namespace
{
	/** How near the cursor has to be to take a point, in pixels. */
	constexpr double PickRadius = 10.0;
	/** A line is thinner than a point and wants a closer hand (S234a). */
	constexpr double EdgePickRadius = 6.0;

	USurfEdMode* ActiveMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** A number that changes whenever what is drawn would look different. */
	uint32 NetSerial(const TArray<FSurfUVNet>& Nets)
	{
		uint32 Serial = static_cast<uint32>(Nets.Num());
		for (const FSurfUVNet& Net : Nets)
		{
			Serial = HashCombine(Serial, GetTypeHash(Net.Patch.PatchId));
			Serial = HashCombine(Serial, static_cast<uint32>(Net.Width * 31 + Net.Height));
		}
		return Serial;
	}

	void Line(FSlateWindowElementList& Elements, int32 Layer, const FPaintGeometry& Geometry,
		const FVector2D& A, const FVector2D& B, const FLinearColor& Colour, float Thickness)
	{
		TArray<FVector2D> Points;
		Points.Add(A);
		Points.Add(B);
		FSlateDrawElement::MakeLines(Elements, Layer, Geometry, Points, ESlateDrawEffect::None, Colour, true, Thickness);
	}

	void Dot(FSlateWindowElementList& Elements, int32 Layer, const FGeometry& Geometry,
		const FVector2D& At, double Size, const FLinearColor& Colour)
	{
		FSlateDrawElement::MakeBox(Elements, Layer,
			Geometry.ToPaintGeometry(FVector2D(Size, Size), FSlateLayoutTransform(At - FVector2D(Size, Size) * 0.5)),
			FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, Colour);
	}
}

void SSurfUVView::Construct(const FArguments& InArgs)
{
	SetCanTick(true);

	// The net is drawn by hand, and a patch whose UVs run well outside the
	// tile drew its lines over the fields under the pane (his screenshot).
	// Slate does not clip a painted widget unless it is asked to.
	SetClipping(EWidgetClipping::ClipToBoundsAlways);
}

void SSurfUVView::OnMouseEnter(const FGeometry& Geometry, const FPointerEvent& Event)
{
	SCompoundWidget::OnMouseEnter(Geometry, Event);
	if (!FSlateApplication::IsInitialized() || HasKeyboardFocus())
	{
		return;
	}
	// Never from under a field being typed in. A spin box that was dragged
	// or committed keeps the focus on itself, not on its text - that one
	// gives way, else G R S are dead after a panel field (S222a: "while
	// sidepanel is open cant move").
	if (!TakeKeysFrom(FSlateApplication::Get().GetKeyboardFocusedWidget()))
	{
		return;
	}
	FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::Mouse);
}

bool SSurfUVView::TakeKeysFrom(const TSharedPtr<SWidget>& Focused) const
{
	// Never from under a field being typed in - a visible one. A spin box's
	// text stays focused after Enter while collapsed (S223a: "cant use it if
	// a sidepanel section is open"); that one gives way.
	if (!Focused.IsValid())
	{
		return true;
	}
	const FString Type = Focused->GetTypeAsString();
	return !(Type.Contains(TEXT("EditableText")) && Focused->GetVisibility().IsVisible());
}

namespace
{
	/** The texture a material shows, for the backdrop: a colour one by its name, else the first. */
	UTexture* BackdropTextureOf(UMaterialInterface* Material)
	{
		if (!Material)
		{
			return nullptr;
		}
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Material->GetAllTextureParameterInfo(Infos, Guids);
		UTexture* First = nullptr;
		// A material with no texture parameters at all (a plain sample):
		// whatever it uses.
		if (Infos.Num() == 0)
		{
			TArray<UTexture*> Used;
			// Every quality level and platform (S251h): the old five-argument form is
			// an empty body since 5.7, so this fallback found nothing.
			Material->GetUsedTextures(Used);
			for (UTexture* Texture : Used)
			{
				if (Cast<UTexture2D>(Texture))
				{
					return Texture;
				}
			}
			return nullptr;
		}
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Texture = nullptr;
			if (!Material->GetTextureParameterValue(Info, Texture) || !Texture)
			{
				continue;
			}
			const FString Name = Info.Name.ToString();
			if (Name.Contains(TEXT("Base")) || Name.Contains(TEXT("Albedo")) || Name.Contains(TEXT("Diffuse"))
				|| Name.Contains(TEXT("Color")) || Name.Contains(TEXT("Colour")))
			{
				return Texture;
			}
			if (!First)
			{
				First = Texture;
			}
		}
		return First;
	}
}

SSurfUVView::~SSurfUVView()
{
	// A tab closed mid-drag must not leave a transaction open.
	DragTransaction.Reset();
}

USurfEdMode* SSurfUVView::LiveMode() const
{
	return ActiveMode();
}

void SSurfUVView::FrameSelection(const FVector2D& WidgetSize)
{
	TArray<FSurfUVNet> Nets;
	if (const USurfEdMode* Mode = LiveMode())
	{
		SurfUV::GatherNets(*Mode, Nets);
	}
	// Clear of the quickpanel when it is open over the right edge (S223a):
	// framed in the width left of it - Frame centres the nets in the size
	// it is given, so the centre lands mid-way across the uncovered part.
	const float Covered = SurfUVPanel::Covered();
	View = SurfUV::Frame(SurfUV::BoundsOf(Nets), FVector2D(FMath::Max(64.0, WidgetSize.X - Covered), WidgetSize.Y));
}

void SSurfUVView::EndDrag()
{
	if (DragTransaction.IsValid())
	{
		DragTransaction.Reset();
		SurfSession::Verb(TEXT("uv_move"), TEXT("done"));
	}
	bDragging = false;
}

void SSurfUVView::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	LastSize = Geometry.GetLocalSize();

	/**
	 * G R S keep going past the window's edge (S188a): "same leaving frame
	 * while dragging issue with UV when scaling them". The cursor comes back
	 * in at the other side and the gesture reads it with the distance it was
	 * carried; outside, no move reaches the window, so this looks each frame.
	 */
	if (Modal != EUVModal::None && FSlateApplication::IsInitialized() && LastSize.X > 60.0 && LastSize.Y > 60.0)
	{
		const FVector2D Local = Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
		constexpr double Edge = 3.0;
		constexpr double Inset = 16.0;
		FVector2D To = Local;
		if (Local.X <= Edge) { To.X = LastSize.X - Inset; }
		else if (Local.X >= LastSize.X - 1.0 - Edge) { To.X = Inset; }
		if (Local.Y <= Edge) { To.Y = LastSize.Y - Inset; }
		else if (Local.Y >= LastSize.Y - 1.0 - Edge) { To.Y = Inset; }
		if (To != Local)
		{
			FSlateApplication::Get().SetCursorPos(Geometry.LocalToAbsolute(To));
			ModalWrap += Local - To;
			UpdateModal(To + ModalWrap);
		}
	}

	// A new selection frames itself once; after that the view is the hand's.
	TArray<FSurfUVNet> Nets;
	if (const USurfEdMode* Mode = LiveMode())
	{
		SurfUV::GatherNets(*Mode, Nets);
	}
	const uint32 Serial = NetSerial(Nets);
	if (Serial != FramedOn)
	{
		FramedOn = Serial;
		View = SurfUV::Frame(SurfUV::BoundsOf(Nets), FVector2D(FMath::Max(64.0, LastSize.X - SurfUVPanel::Covered()), LastSize.Y));
	}
}

int32 SSurfUVView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	LastSize = Size;
	const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();

	// The ground, dark like the viewport: a UV net is drawn near-white for
	// the same reason the cage is.
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Paint,
		FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, FLinearColor(0.055f, 0.055f, 0.065f));
	// Everything after the ground on a layer of its own (S208a), so the
	// texture behind the nets is never batched under it.
	++LayerId;

	const FVector2D TopLeft = View.ToUV(FVector2D::ZeroVector);
	const FVector2D BottomRight = View.ToUV(Size);

	// The active patch's texture behind the nets (S207a), faint enough for
	// the net to stay the thing read; on the first tile, or on every tile.
	const USurfSettings* Backdrop = GetDefault<USurfSettings>();
	if (Backdrop && Backdrop->bUVShowTexture)
	{
		UTexture* Texture = nullptr;
		if (const USurfEdMode* Live = LiveMode())
		{
			TArray<FSurfUVNet> Seen;
			SurfUV::GatherNets(*Live, Seen);
			const FSurfUVNet* Pick = Seen.FindByPredicate([](const FSurfUVNet& Net) { return Net.bActive; });
			if (!Pick && Seen.Num() > 0)
			{
				Pick = &Seen[0];
			}
			const ASurfGroup* Group = Pick ? Pick->Patch.Group.Get() : nullptr;
			const FSurfPatch* Patch = Group ? Group->FindPatch(Pick->Patch.PatchId) : nullptr;
			Texture = Patch ? BackdropTextureOf(Patch->Material.Get()) : nullptr;
			// A patch wearing nothing wears the checker: that, then (S208a:
			// "still all grey").
			if (Patch && !Texture)
			{
				Texture = SurfPreviewMaterial::GetCheckerTexture();
			}
		}
		if (Texture != BackdropTexture.Get() || (Texture && !BackdropBrush.IsValid()))
		{
			BackdropTexture = Texture;
			BackdropBrush.Reset();
			if (Texture)
			{
				BackdropBrush = MakeShared<FSlateBrush>();
				BackdropBrush->SetResourceObject(Texture);
				BackdropBrush->ImageSize = FVector2D(256.0, 256.0);
				BackdropBrush->DrawAs = ESlateBrushDrawType::Image;
			}
		}
		if (BackdropBrush.IsValid() && BackdropTexture.IsValid())
		{
			// The first tile a little brighter than its repeats (S209a: "make
			// it visible which one is 0 0 tile").
			auto Tile = [&](int32 U, int32 V)
			{
				const FLinearColor Faint(1.0f, 1.0f, 1.0f, (U == 0 && V == 0) ? 0.5f : 0.28f);
				const FVector2D A = View.ToPixel(FVector2D(static_cast<double>(U), static_cast<double>(V)));
				const FVector2D B = View.ToPixel(FVector2D(static_cast<double>(U + 1), static_cast<double>(V + 1)));
				const FVector2D Corner(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
				const FVector2D Extent(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y));
				FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(Extent, FSlateLayoutTransform(Corner)),
					BackdropBrush.Get(), ESlateDrawEffect::None, Faint);
			};
			if (Backdrop->bUVTileTexture)
			{
				const int32 U0 = FMath::FloorToInt(FMath::Min(TopLeft.X, BottomRight.X));
				const int32 U1 = FMath::CeilToInt(FMath::Max(TopLeft.X, BottomRight.X));
				const int32 V0 = FMath::FloorToInt(FMath::Min(TopLeft.Y, BottomRight.Y));
				const int32 V1 = FMath::CeilToInt(FMath::Max(TopLeft.Y, BottomRight.Y));
				if ((U1 - U0) * (V1 - V0) <= 900)
				{
					for (int32 V = V0; V < V1; ++V)
					{
						for (int32 U = U0; U < U1; ++U)
						{
							Tile(U, V);
						}
					}
				}
			}
			else
			{
				Tile(0, 0);
			}
		}
	}

	/**
	 * Stretch (S233a): every cell filled by how its texture area compares
	 * with its surface area - blue tight, green even, red stretched - on a
	 * layer of its own under the grid, so the lines still read over it.
	 */
	if (Backdrop && Backdrop->bUVStretchView)
	{
		if (const USurfEdMode* Live = LiveMode())
		{
			TArray<FSurfUVNet> Filled;
			SurfUV::GatherNets(*Live, Filled);
			TArray<FSlateVertex> Verts;
			TArray<SlateIndex> Indices;
			const FSlateRenderTransform& ToRender = AllottedGeometry.GetAccumulatedRenderTransform();
			for (const FSurfUVNet& Net : Filled)
			{
				const int32 Across = Net.Width - 1;
				for (int32 Y = 0; Y + 1 < Net.Height; ++Y)
				{
					for (int32 X = 0; X + 1 < Net.Width; ++X)
					{
						const int32 Cell = Y * Across + X;
						if (!Net.Stretch.IsValidIndex(Cell))
						{
							continue;
						}
						const FColor Fill = SurfUV::StretchColour(Net.Stretch[Cell]).CopyWithNewOpacity(Net.bActive ? 0.55f : 0.32f).ToFColor(false);
						const FVector2D Corners[4] = { View.ToPixel(Net.At(X, Y)), View.ToPixel(Net.At(X + 1, Y)), View.ToPixel(Net.At(X + 1, Y + 1)), View.ToPixel(Net.At(X, Y + 1)) };
						const SlateIndex First = static_cast<SlateIndex>(Verts.Num());
						for (const FVector2D& Corner : Corners)
						{
							Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(ToRender, FVector2f(Corner), FVector2f::ZeroVector, Fill));
						}
						Indices.Append({ First, First + 1, First + 2, First, First + 2, First + 3 });
					}
				}
			}
			if (Verts.Num() > 0)
			{
				const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FAppStyle::GetBrush("WhiteBrush"));
				FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Verts, Indices, nullptr, 0, 0);
			}
		}
		++LayerId;
	}

	// The tile grid. Whole tiles always; quarters once a tile is wide enough
	// for them to mean something rather than fill the pane with lines.
	const double Step = View.Scale > 480.0 ? 0.25 : 1.0;
	const int32 FirstU = FMath::FloorToInt(TopLeft.X / Step);
	const int32 LastU = FMath::CeilToInt(BottomRight.X / Step);
	const int32 FirstV = FMath::FloorToInt(TopLeft.Y / Step);
	const int32 LastV = FMath::CeilToInt(BottomRight.Y / Step);

	if (LastU - FirstU < 400 && LastV - FirstV < 400)
	{
		for (int32 I = FirstU; I <= LastU; ++I)
		{
			const double U = I * Step;
			const bool bWhole = FMath::IsNearlyEqual(U, FMath::RoundToDouble(U), 1.e-6);
			const double X = View.ToPixel(FVector2D(U, 0.0)).X;
			Line(OutDrawElements, LayerId + 1, Paint, FVector2D(X, 0.0), FVector2D(X, Size.Y),
				bWhole ? FLinearColor(0.30f, 0.30f, 0.36f) : FLinearColor(0.16f, 0.16f, 0.19f), 1.0f);
		}
		for (int32 I = FirstV; I <= LastV; ++I)
		{
			const double V = I * Step;
			const bool bWhole = FMath::IsNearlyEqual(V, FMath::RoundToDouble(V), 1.e-6);
			const double Y = View.ToPixel(FVector2D(0.0, V)).Y;
			Line(OutDrawElements, LayerId + 1, Paint, FVector2D(0.0, Y), FVector2D(Size.X, Y),
				bWhole ? FLinearColor(0.30f, 0.30f, 0.36f) : FLinearColor(0.16f, 0.16f, 0.19f), 1.0f);
		}
	}

	// The first tile, framed in the accent (S209a) and named at its corner:
	// the one the texture actually is.
	{
		const FVector2D A = View.ToPixel(FVector2D(0.0, 0.0));
		const FVector2D B = View.ToPixel(FVector2D(1.0, 1.0));
		const FLinearColor Edge = SurfTheme::Linear(SurfTheme::On()).CopyWithNewOpacity(0.9f);
		Line(OutDrawElements, LayerId + 2, Paint, FVector2D(A.X, A.Y), FVector2D(B.X, A.Y), Edge, 2.0f);
		Line(OutDrawElements, LayerId + 2, Paint, FVector2D(B.X, A.Y), FVector2D(B.X, B.Y), Edge, 2.0f);
		Line(OutDrawElements, LayerId + 2, Paint, FVector2D(B.X, B.Y), FVector2D(A.X, B.Y), Edge, 2.0f);
		Line(OutDrawElements, LayerId + 2, Paint, FVector2D(A.X, B.Y), FVector2D(A.X, A.Y), Edge, 2.0f);
		const FVector2D Corner(FMath::Min(A.X, B.X) + 4.0, FMath::Min(A.Y, B.Y) + 2.0);
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(FVector2D(40.0, 14.0), FSlateLayoutTransform(Corner)),
			TEXT("0,0"), SurfTheme::Font(8), ESlateDrawEffect::None, Edge);
	}

	const USurfEdMode* Mode = LiveMode();
	if (!Mode)
	{
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 3,
			AllottedGeometry.ToPaintGeometry(FVector2D(Size.X - 16.0, 20.0), FSlateLayoutTransform(FVector2D(8.0, 8.0))),
			LOCTEXT("NoMode", "Enter the SurfacingPro mode to see the UVs.").ToString(),
			SurfTheme::Font(9), ESlateDrawEffect::None, SurfTheme::Linear(SurfTheme::Text()));
		return LayerId + 4;
	}

	TArray<FSurfUVNet> Nets;
	SurfUV::GatherNets(*Mode, Nets);

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const FLinearColor Wire = Settings ? Settings->ControlGridColour : FLinearColor(0.88f, 0.88f, 0.92f);
	const FLinearColor Point = Settings ? Settings->ControlPointColour : FLinearColor(0.92f, 0.92f, 0.95f);
	const FLinearColor Hot = Settings ? Settings->SelectedControlPointColour : FLinearColor(1.0f, 0.08f, 0.05f);

	int32 Layer = LayerId + 3;
	for (const FSurfUVNet& Net : Nets)
	{
		// The patches that are not active are quieter, as they are in the
		// viewport: which one a verb reads its numbers from should be
		// visible without clicking anything.
		const float Fade = Net.bActive ? 1.0f : 0.45f;
		const FLinearColor NetWire(Wire.R, Wire.G, Wire.B, Wire.A * Fade);

		for (int32 Y = 0; Y < Net.Height; ++Y)
		{
			for (int32 X = 0; X < Net.Width; ++X)
			{
				const FVector2D Here = View.ToPixel(Net.At(X, Y));
				if (X + 1 < Net.Width)
				{
					Line(OutDrawElements, Layer, Paint, Here, View.ToPixel(Net.At(X + 1, Y)), NetWire, 1.0f);
				}
				if (Y + 1 < Net.Height)
				{
					Line(OutDrawElements, Layer, Paint, Here, View.ToPixel(Net.At(X, Y + 1)), NetWire, 1.0f);
				}
			}
		}

		for (int32 Y = 0; Y < Net.Height; ++Y)
		{
			for (int32 X = 0; X < Net.Width; ++X)
			{
				const bool bSelected = Net.IsSelected(X, Y);
				const FLinearColor Colour = bSelected ? Hot : FLinearColor(Point.R, Point.G, Point.B, Point.A * Fade);
				Dot(OutDrawElements, Layer + 1, AllottedGeometry, View.ToPixel(Net.At(X, Y)), bSelected ? 7.0 : 5.0, Colour);
			}
		}
	}

	// The box being dragged out, while it is being dragged.
	if (bBoxing)
	{
		const FLinearColor Edge(0.95f, 0.8f, 0.4f);
		const FVector2D A(FMath::Min(BoxFrom.X, BoxTo.X), FMath::Min(BoxFrom.Y, BoxTo.Y));
		const FVector2D B(FMath::Max(BoxFrom.X, BoxTo.X), FMath::Max(BoxFrom.Y, BoxTo.Y));
		Line(OutDrawElements, Layer + 2, Paint, FVector2D(A.X, A.Y), FVector2D(B.X, A.Y), Edge, 1.0f);
		Line(OutDrawElements, Layer + 2, Paint, FVector2D(B.X, A.Y), FVector2D(B.X, B.Y), Edge, 1.0f);
		Line(OutDrawElements, Layer + 2, Paint, FVector2D(B.X, B.Y), FVector2D(A.X, B.Y), Edge, 1.0f);
		Line(OutDrawElements, Layer + 2, Paint, FVector2D(A.X, B.Y), FVector2D(A.X, A.Y), Edge, 1.0f);
	}

	// What is being looked at, bottom left: how many patches, and whether
	// their mapping is a rule or held.
	{
		int32 Frozen = 0;
		for (const FSurfUVNet& Net : Nets)
		{
			Frozen += Net.bFrozen ? 1 : 0;
		}
		const FString Line = Nets.Num() == 0
			? FString(TEXT("nothing selected"))
			: FString::Printf(TEXT("%d patch%s%s - drag to move, %s"), Nets.Num(), Nets.Num() == 1 ? TEXT("") : TEXT("es"),
				Frozen > 0 ? *FString::Printf(TEXT(", %d frozen"), Frozen) : TEXT(""),
				Frozen == Nets.Num() ? TEXT("picked points move") : TEXT("picked points move and freeze the rule; nothing picked slides whole"));

		FSlateDrawElement::MakeText(OutDrawElements, Layer + 3,
			AllottedGeometry.ToPaintGeometry(FVector2D(Size.X - 16.0, 18.0), FSlateLayoutTransform(FVector2D(8.0, Size.Y - 22.0))),
			Line, SurfTheme::Font(9), ESlateDrawEffect::None, SurfTheme::Linear(SurfTheme::Text()));
	}

	return Layer + 4;
}

FReply SSurfUVView::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	// A modal owns the mouse while it runs: left keeps it, right drops it.
	if (Modal != EUVModal::None)
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			EndModal(false);
			return FReply::Handled();
		}
		if (Event.GetEffectingButton() == EKeys::RightMouseButton)
		{
			EndModal(true);
			return FReply::Handled();
		}
	}

	const FVector2D Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());

	// Middle or right drags the view, as they do in the viewport.
	if (Event.GetEffectingButton() == EKeys::MiddleMouseButton || Event.GetEffectingButton() == EKeys::RightMouseButton)
	{
		bPanning = true;
		PanFrom = Local;
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}

	if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	USurfEdMode* Mode = LiveMode();
	if (!Mode)
	{
		return FReply::Handled();
	}

	TArray<FSurfUVNet> Nets;
	SurfUV::GatherNets(*Mode, Nets);

	// Shift adds, Ctrl takes out (S224a) - as the viewport picks actors.
	const bool bAdd = Event.IsShiftDown();
	const bool bTake = Event.IsControlDown() && !Event.IsShiftDown();

	/**
	 * Lines and loops (S234a, UV round 4): a click on a net line with no
	 * point under the cursor picks its two points; Alt on a point cycles
	 * row, column, point as it does in the viewport; Alt on a line takes
	 * the whole row or column the line lies along. The selection is the
	 * mode's own, so the viewport shows it as it happens.
	 */
	FSurfPointRef Hit;
	FSurfPointRef EdgeA;
	FSurfPointRef EdgeB;
	const bool bOnPoint = SurfUV::PickPoint(Nets, View, Local, PickRadius, Hit);
	const bool bOnEdge = !bOnPoint && SurfUV::PickEdge(Nets, View, Local, EdgePickRadius, EdgeA, EdgeB);
	if (bOnEdge)
	{
		Hit = EdgeA;
	}
	if (bOnPoint || bOnEdge)
	{
		// A point under the cursor is picked and then dragged, as it is in
		// the viewport: no click, wait, drag. Shift adds to the selection
		// instead of starting again; Ctrl takes it out and nothing drags.
		if (Mode->GetSubMode() == ESurfSubMode::Object)
		{
			Mode->SetSubMode(ESurfSubMode::Vertex);
		}

		// What the click stands for: the point, the line's two points, or
		// the whole row or column along the line.
		TArray<FSurfPointRef> Picked;
		if (bOnEdge && Event.IsAltDown())
		{
			const bool bRow = EdgeA.Y == EdgeB.Y;
			for (const FSurfUVNet& Net : Nets)
			{
				if (Net.Patch.Group != EdgeA.Group || Net.Patch.PatchId != EdgeA.PatchId)
				{
					continue;
				}
				const int32 Count = bRow ? Net.Width : Net.Height;
				for (int32 I = 0; I < Count; ++I)
				{
					FSurfPointRef Ref = EdgeA;
					Ref.X = bRow ? I : EdgeA.X;
					Ref.Y = bRow ? EdgeA.Y : I;
					Picked.Add(Ref);
				}
			}
		}
		else if (bOnEdge)
		{
			Picked.Add(EdgeA);
			Picked.Add(EdgeB);
		}
		else if (Event.IsAltDown())
		{
			// The viewport's own cycle - row, column, point - Ctrl or Shift adding.
			Mode->SelectPointRowOrColumn(Hit, /*bAddToSelection*/ bAdd || Event.IsControlDown());
			Mode->SyncSelectionAfterUVPick();
			SurfSession::Gesture(TEXT("uv_pick"), TEXT("alt_point"));
			return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
		}
		else
		{
			Picked.Add(Hit);
		}

		bool bAlready = true;
		for (const FSurfPointRef& Ref : Picked)
		{
			bAlready = bAlready && Mode->GetPointSelection().Contains(Ref);
		}
		if (bTake)
		{
			for (const FSurfPointRef& Ref : Picked)
			{
				Mode->RemovePointFromSelection(Ref);
			}
			Mode->SyncSelectionAfterUVPick();
			return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
		}
		if (bAdd)
		{
			for (const FSurfPointRef& Ref : Picked)
			{
				Mode->AddPointToSelection(Ref);
			}
		}
		else if (!bAlready)
		{
			Mode->SelectPoint(Picked[0]);
			for (int32 I = 1; I < Picked.Num(); ++I)
			{
				Mode->AddPointToSelection(Picked[I]);
			}
		}
		if (bOnEdge)
		{
			SurfSession::Gesture(TEXT("uv_pick"), Event.IsAltDown() ? TEXT("line") : TEXT("edge"));
		}

		Mode->SyncSelectionAfterUVPick();

		bDragging = true;
		DragFrom = Local;
		DragAnchor = Hit;
		{
			// Where the point in hand is now, the raw cursor's start (S230a).
			TArray<FSurfUVNet> NetsNow;
			SurfUV::GatherNets(*Mode, NetsNow);
			DragAnchorFree = FVector2D::ZeroVector;
			for (const FSurfUVNet& Net : NetsNow)
			{
				if (Net.Patch.Group == Hit.Group && Net.Patch.PatchId == Hit.PatchId) { DragAnchorFree = Net.At(Hit.X, Hit.Y); break; }
			}
		}
		DragTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("MoveUVs", "Move UVs"));
		return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}

	// Empty space: a box, which takes what it covers when it is let go -
	// or, with Ctrl, lets go of what it covers.
	bBoxing = true;
	bBoxAdds = bAdd;
	bBoxTakes = bTake;
	BoxFrom = Local;
	BoxTo = Local;
	return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this), EFocusCause::Mouse);
}

FReply SSurfUVView::OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event)
{
	/**
	 * A double-click on a net - a point or a line of it - takes the whole
	 * net, the island (S234a); Shift adds it to what is picked.
	 */
	if (Event.GetEffectingButton() != EKeys::LeftMouseButton || Modal != EUVModal::None)
	{
		return FReply::Unhandled();
	}
	USurfEdMode* Mode = LiveMode();
	if (!Mode)
	{
		return FReply::Handled();
	}
	const FVector2D Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
	TArray<FSurfUVNet> Nets;
	SurfUV::GatherNets(*Mode, Nets);
	FSurfPointRef Hit;
	FSurfPointRef Other;
	if (!SurfUV::PickPoint(Nets, View, Local, PickRadius, Hit) && !SurfUV::PickEdge(Nets, View, Local, EdgePickRadius, Hit, Other))
	{
		return FReply::Unhandled();
	}
	// The press before this already began a drag of one point; it is over.
	if (bDragging)
	{
		bDragging = false;
		DragTransaction.Reset();
	}
	if (Mode->GetSubMode() == ESurfSubMode::Object)
	{
		Mode->SetSubMode(ESurfSubMode::Vertex);
	}
	if (!Event.IsShiftDown())
	{
		Mode->ClearPointSelection();
	}
	for (const FSurfUVNet& Net : Nets)
	{
		if (Net.Patch.Group != Hit.Group || Net.Patch.PatchId != Hit.PatchId)
		{
			continue;
		}
		for (int32 Y = 0; Y < Net.Height; ++Y)
		{
			for (int32 X = 0; X < Net.Width; ++X)
			{
				FSurfPointRef Ref = Hit;
				Ref.X = X;
				Ref.Y = Y;
				Mode->AddPointToSelection(Ref);
			}
		}
	}
	Mode->SyncSelectionAfterUVPick();
	SurfSession::Gesture(TEXT("uv_pick"), TEXT("island"));
	return FReply::Handled().ReleaseMouseCapture().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
}

FReply SSurfUVView::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (bPanning && (Event.GetEffectingButton() == EKeys::MiddleMouseButton || Event.GetEffectingButton() == EKeys::RightMouseButton))
	{
		bPanning = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

	if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	if (bDragging)
	{
		EndDrag();
		return FReply::Handled().ReleaseMouseCapture();
	}

	if (bBoxing)
	{
		bBoxing = false;

		USurfEdMode* Mode = LiveMode();
		if (Mode)
		{
			TArray<FSurfUVNet> Nets;
			SurfUV::GatherNets(*Mode, Nets);

			const FBox2D Box(
				FVector2D(FMath::Min(BoxFrom.X, BoxTo.X), FMath::Min(BoxFrom.Y, BoxTo.Y)),
				FVector2D(FMath::Max(BoxFrom.X, BoxTo.X), FMath::Max(BoxFrom.Y, BoxTo.Y)));

			TArray<FSurfPointRef> Found;
			if (Box.GetSize().GetMax() > 2.0)
			{
				SurfUV::PointsInBox(Nets, View, Box, Found);
			}

			if (Found.Num() > 0 && Mode->GetSubMode() == ESurfSubMode::Object)
			{
				Mode->SetSubMode(ESurfSubMode::Vertex);
			}
			if (bBoxTakes)
			{
				for (const FSurfPointRef& Ref : Found)
				{
					Mode->RemovePointFromSelection(Ref);
				}
			}
			else
			{
				if (!bBoxAdds)
				{
					Mode->ClearPointSelection();
				}
				for (const FSurfPointRef& Ref : Found)
				{
					Mode->AddPointToSelection(Ref);
				}
			}
			Mode->SyncSelectionAfterUVPick();

			SurfSession::Selection(FString::Printf(TEXT("uv_box points=%d%s"), Found.Num(), bBoxTakes ? TEXT(" taken out") : TEXT("")));
		}
		return FReply::Handled().ReleaseMouseCapture();
	}

	return FReply::Unhandled();
}

FReply SSurfUVView::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
	// The keys follow the cursor (S223a): over the nets and without them,
	// take them - a panel field may have kept them after the cursor came in.
	if (!HasKeyboardFocus() && !bDragging && !bBoxing && !bPanning && Modal == EUVModal::None && FSlateApplication::IsInitialized()
		&& TakeKeysFrom(FSlateApplication::Get().GetKeyboardFocusedWidget()))
	{
		FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::Mouse);
	}
	const FVector2D Now = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());

	if (Modal != EUVModal::None)
	{
		UpdateModal(Now + ModalWrap);
		return FReply::Handled();
	}

	if (bPanning)
	{
		View.Origin -= (Now - PanFrom) / FMath::Max(1.e-6, View.Scale);
		PanFrom = Now;
		return FReply::Handled();
	}

	if (bBoxing)
	{
		BoxTo = Now;
		return FReply::Handled();
	}

	if (bDragging)
	{
		FVector2D Delta = (Now - DragFrom) / FMath::Max(1.e-6, View.Scale);
		DragFrom = Now;
		if (USurfEdMode* Mode = LiveMode())
		{
			// The point in hand follows the raw cursor (DragAnchorFree) and
			// what is applied lands it snapped (S230a), so it sticks to a
			// target and leaves it as the cursor moves on.
			DragAnchorFree += Delta;
			if (SurfSnapState::On() && DragAnchor.IsValid())
			{
				TArray<FSurfUVNet> Nets;
				SurfUV::GatherNets(*Mode, Nets);
				FVector2D AnchorNow = DragAnchorFree;
				bool bHave = false;
				for (const FSurfUVNet& Net : Nets)
				{
					if (Net.Patch.Group == DragAnchor.Group && Net.Patch.PatchId == DragAnchor.PatchId && DragAnchor.X >= 0 && DragAnchor.X < Net.Width && DragAnchor.Y >= 0 && DragAnchor.Y < Net.Height)
					{
						AnchorNow = Net.At(DragAnchor.X, DragAnchor.Y);
						bHave = true;
						break;
					}
				}
				if (bHave)
				{
					const FVector2D Landing = DragAnchorFree + SurfUV::SnapCorrection(Nets, View, DragAnchorFree);
					Delta = Landing - AnchorNow;
				}
			}
			Mode->MoveSelectedUVs(Delta);
		}
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

// --- G, R, S ----------------------------------------------------------------
//
// The same three keys as the viewport, because the window is a second place to
// do the same work and a second set of keys for it would be a second thing to
// learn. What they act on is D60's rule: a frozen patch moves, turns and
// scales the points that are picked; a patch whose mapping is still a rule has
// no points to move, so the rule itself shifts, turns and rescales.

void SSurfUVView::BeginModal(EUVModal Kind, const FGeometry& Geometry)
{
	USurfEdMode* Mode = LiveMode();
	if (!Mode)
	{
		return;
	}

	FVector2D Pivot = FVector2D::ZeroVector;
	if (!Mode->SelectedUVPivot(Pivot))
	{
		return;
	}

	EndModal(true);

	Modal = Kind;
	ModalPivotUV = Pivot;
	ModalPivotPixel = View.ToPixel(Pivot);
	ModalFrom = Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
	ModalWrap = FVector2D::ZeroVector;
	ModalMoved = FVector2D::ZeroVector;
	ModalTurned = 0.0;
	ModalScaled = 1.0;
	ModalAxis = -1;
	ModalTyped.Reset();

	const FText What = Kind == EUVModal::Move
		? LOCTEXT("UVModalMove", "Move UVs")
		: (Kind == EUVModal::Turn ? LOCTEXT("UVModalTurn", "Turn UVs") : LOCTEXT("UVModalScale", "Scale UVs"));
	ModalTransaction = MakeUnique<FScopedTransaction>(What);
	ReportModal();
}

void SSurfUVView::UpdateModal(const FVector2D& Pixel)
{
	USurfEdMode* Mode = LiveMode();
	if (Modal == EUVModal::None || !Mode || !ModalTyped.IsEmpty())
	{
		return;
	}

	if (Modal == EUVModal::Move)
	{
		FVector2D Want = (Pixel - ModalFrom) / FMath::Max(1.e-6, View.Scale);
		if (ModalAxis == 0) { Want.Y = 0.0; }
		if (ModalAxis == 1) { Want.X = 0.0; }
		// The selection's middle snaps (S230a), the lock kept.
		if (SurfSnapState::On())
		{
			TArray<FSurfUVNet> Nets;
			SurfUV::GatherNets(*Mode, Nets);
			FVector2D Correction = SurfUV::SnapCorrection(Nets, View, ModalPivotUV + Want);
			if (ModalAxis == 0) { Correction.Y = 0.0; }
			if (ModalAxis == 1) { Correction.X = 0.0; }
			Want += Correction;
		}
		if (Mode->MoveSelectedUVs(Want - ModalMoved))
		{
			ModalMoved = Want;
		}
	}
	else if (Modal == EUVModal::Turn)
	{
		const FVector2D A = ModalFrom - ModalPivotPixel;
		const FVector2D B = Pixel - ModalPivotPixel;
		if (A.SizeSquared() < 4.0 || B.SizeSquared() < 4.0)
		{
			return;
		}
		double Want = FMath::RadiansToDegrees(FMath::Atan2(B.Y, B.X) - FMath::Atan2(A.Y, A.X));

		// Unwrapped against where it already is, so dragging round the pivot
		// keeps turning the same way instead of snapping back at half a turn.
		while (Want - ModalTurned > 180.0) { Want -= 360.0; }
		while (Want - ModalTurned < -180.0) { Want += 360.0; }

		if (Mode->TurnSelectedUVs(Want - ModalTurned, ModalPivotUV))
		{
			ModalTurned = Want;
		}
	}
	else
	{
		const double Was = (ModalFrom - ModalPivotPixel).Size();
		const double Now = (Pixel - ModalPivotPixel).Size();
		if (Was < 4.0)
		{
			return;
		}
		const double Want = FMath::Max(0.01, Now / Was);
		const double Step = Want / FMath::Max(1.e-4, ModalScaled);
		const FVector2D Ratio(
			ModalAxis == 1 ? 1.0 : Step,
			ModalAxis == 0 ? 1.0 : Step);
		if (Mode->ScaleSelectedUVs(Ratio, ModalPivotUV))
		{
			ModalScaled = Want;
		}
	}

	ReportModal();
}

void SSurfUVView::ApplyTypedModal()
{
	USurfEdMode* Mode = LiveMode();
	if (Modal == EUVModal::None || !Mode || ModalTyped.IsEmpty())
	{
		return;
	}

	const double Value = FCString::Atod(*ModalTyped);
	if (Modal == EUVModal::Move)
	{
		// With no axis held, a typed number is along U: one number cannot
		// mean two directions, and X or Y says which before it is typed.
		const FVector2D Want(ModalAxis == 1 ? 0.0 : Value, ModalAxis == 1 ? Value : 0.0);
		if (Mode->MoveSelectedUVs(Want - ModalMoved))
		{
			ModalMoved = Want;
		}
	}
	else if (Modal == EUVModal::Turn)
	{
		if (Mode->TurnSelectedUVs(Value - ModalTurned, ModalPivotUV))
		{
			ModalTurned = Value;
		}
	}
	else
	{
		const double Want = FMath::Abs(Value) < 0.01 ? 0.01 : Value;
		const double Step = Want / FMath::Max(1.e-4, ModalScaled);
		const FVector2D Ratio(
			ModalAxis == 1 ? 1.0 : Step,
			ModalAxis == 0 ? 1.0 : Step);
		if (Mode->ScaleSelectedUVs(Ratio, ModalPivotUV))
		{
			ModalScaled = Want;
		}
	}

	ReportModal();
}

void SSurfUVView::ReportModal() const
{
	USurfEdMode* Mode = LiveMode();
	if (Modal == EUVModal::None || !Mode || !Mode->Inspector)
	{
		return;
	}

	const TCHAR* Axis = ModalAxis == 0 ? TEXT(" u") : (ModalAxis == 1 ? TEXT(" v") : TEXT(""));
	const FString Typed = ModalTyped.IsEmpty() ? FString() : FString::Printf(TEXT(" [%s]"), *ModalTyped);

	FString Line;
	if (Modal == EUVModal::Move)
	{
		Line = FString::Printf(TEXT("move%s %.3f, %.3f tiles%s"), Axis, ModalMoved.X, ModalMoved.Y, *Typed);
	}
	else if (Modal == EUVModal::Turn)
	{
		Line = FString::Printf(TEXT("turn %.1f degrees%s"), ModalTurned, *Typed);
	}
	else
	{
		Line = FString::Printf(TEXT("scale%s %.3f%s"), Axis, ModalScaled, *Typed);
	}
	Mode->Inspector->SetResult(Line + TEXT(" - click or Enter to keep it, Escape to drop it"));
}

void SSurfUVView::EndModal(bool bCancel)
{
	if (Modal == EUVModal::None)
	{
		return;
	}

	if (bCancel || Modal == EUVModal::Move)
	{
		// A move's numbers are the foot line's U and V (S226a: "why are
		// there two bars move and U V? delete the upper one").
		SurfUVAdjust::bShown = false;
	}
	else
	{
		SurfUVAdjust::Kind = Modal == EUVModal::Turn ? SurfUVAdjust::EKind::Turn : SurfUVAdjust::EKind::Scale;
		SurfUVAdjust::Pivot = ModalPivotUV;
		SurfUVAdjust::Moved = ModalMoved;
		SurfUVAdjust::Turned = ModalTurned;
		SurfUVAdjust::Scaled = FVector2D(ModalAxis == 1 ? 1.0 : ModalScaled, ModalAxis == 0 ? 1.0 : ModalScaled);
		SurfUVAdjust::bShown = true;
	}

	Modal = EUVModal::None;
	ModalTyped.Reset();
	ModalAxis = -1;

	// Closed and then undone: cancelling a transaction drops its record and
	// keeps the edit (D49).
	ModalTransaction.Reset();
	if (bCancel && GEditor)
	{
		GEditor->UndoTransaction();
	}
}

FReply SSurfUVView::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
	const double Factor = Event.GetWheelDelta() > 0.0f ? 1.2 : 1.0 / 1.2;
	View = SurfUV::ZoomAt(View, Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()), Factor);
	return FReply::Handled();
}

FReply SSurfUVView::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	if (Modal != EUVModal::None)
	{
		const FKey Key = Event.GetKey();

		if (Key == EKeys::Escape)
		{
			EndModal(true);
			return FReply::Handled();
		}
		if (Key == EKeys::Enter)
		{
			ApplyTypedModal();
			EndModal(false);
			return FReply::Handled();
		}
		if (Key == EKeys::X || Key == EKeys::U)
		{
			ModalAxis = ModalAxis == 0 ? -1 : 0;
			ReportModal();
			return FReply::Handled();
		}
		if (Key == EKeys::Y || Key == EKeys::V)
		{
			ModalAxis = ModalAxis == 1 ? -1 : 1;
			ReportModal();
			return FReply::Handled();
		}
		if (Key == EKeys::BackSpace)
		{
			if (ModalTyped.Len() > 0)
			{
				ModalTyped.LeftChopInline(1);
				ApplyTypedModal();
			}
			return FReply::Handled();
		}

		// A typed number says it exactly, which is half of what a modelling
		// tool is for: the drag is for finding it, the number for meaning it.
		const FString Name = Key.GetFName().ToString();
		if (Name.Len() == 1 && (FChar::IsDigit(Name[0]) || Name[0] == TEXT('.')))
		{
			ModalTyped += Name;
			ApplyTypedModal();
			return FReply::Handled();
		}
		if (Key == EKeys::Hyphen || Key == EKeys::Subtract)
		{
			if (ModalTyped.StartsWith(TEXT("-"))) { ModalTyped.RightChopInline(1); }
			else { ModalTyped = TEXT("-") + ModalTyped; }
			ApplyTypedModal();
			return FReply::Handled();
		}
		if (Name == TEXT("Zero") || Name == TEXT("One") || Name == TEXT("Two") || Name == TEXT("Three")
			|| Name == TEXT("Four") || Name == TEXT("Five") || Name == TEXT("Six") || Name == TEXT("Seven")
			|| Name == TEXT("Eight") || Name == TEXT("Nine"))
		{
			static const TCHAR* Digits[] = { TEXT("0"), TEXT("1"), TEXT("2"), TEXT("3"), TEXT("4"),
				TEXT("5"), TEXT("6"), TEXT("7"), TEXT("8"), TEXT("9") };
			static const TCHAR* Names[] = { TEXT("Zero"), TEXT("One"), TEXT("Two"), TEXT("Three"), TEXT("Four"),
				TEXT("Five"), TEXT("Six"), TEXT("Seven"), TEXT("Eight"), TEXT("Nine") };
			for (int32 I = 0; I < 10; ++I)
			{
				if (Name == Names[I])
				{
					ModalTyped += Digits[I];
					break;
				}
			}
			ApplyTypedModal();
			return FReply::Handled();
		}

		return FReply::Handled();
	}

	if (!bDragging && !bBoxing && !bPanning)
	{
		// Ctrl+R: a quarter turn, the fix for a mapping ninety degrees off
		// - what Swap used to be reached for, without the mirror a swap
		// brings (S69b).
		if (Event.GetKey() == EKeys::R && Event.IsControlDown())
		{
			if (USurfEdMode* Mode = LiveMode()) { Mode->TurnSelectedUVsQuarter(); }
			return FReply::Handled();
		}
		if (Event.GetKey() == EKeys::G) { BeginModal(EUVModal::Move, Geometry); return FReply::Handled(); }
		if (Event.GetKey() == EKeys::R) { BeginModal(EUVModal::Turn, Geometry); return FReply::Handled(); }
		if (Event.GetKey() == EKeys::S) { BeginModal(EUVModal::Scale, Geometry); return FReply::Handled(); }
	}

	if (Event.GetKey() == EKeys::F || Event.GetKey() == EKeys::Home)
	{
		FrameSelection(Geometry.GetLocalSize());
		return FReply::Handled();
	}

	// N: the quickpanel, as N is the SidePanel over the viewport (S216a).
	if (Event.GetKey() == EKeys::N && !Event.IsControlDown() && !Event.IsAltDown() && !Event.IsShiftDown())
	{
		SurfUVPanel::Toggle();
		return FReply::Handled();
	}

	// The search over this window (S209a): the UV verbs and the mapping
	// settings, nothing that means nothing here.
	if (!bDragging && !bBoxing && !bPanning
		&& SurfKeymap::Match(Event.GetKey(), Event.IsControlDown(), Event.IsShiftDown(), Event.IsAltDown()) == ESurfAction::Search)
	{
		if (USurfEdMode* Mode = LiveMode())
		{
			Mode->OpenUVSearch(AsShared());
			SurfSession::Verb(TEXT("uv_search"), TEXT("open"));
		}
		return FReply::Handled();
	}

	if (Event.GetKey() == EKeys::Escape && bDragging)
	{
		// The transaction is closed and then undone: cancelling a transaction
		// drops its record and keeps the edit (D49).
		EndDrag();
		if (GEditor)
		{
			GEditor->UndoTransaction();
		}
		return FReply::Handled();
	}

	// A and Ctrl+A both select all: Ctrl+A fell through to the editor's own
	// Select All and took every actor in the level ("i tried ctrl a but it
	// selected all objects instead") (S69e).
	if (Event.GetKey() == EKeys::A)
	{
		if (USurfEdMode* Mode = LiveMode())
		{
			if (Event.IsAltDown())
			{
				Mode->ClearPointSelection();
			}
			else
			{
				Mode->SelectAllPoints();
			}
		}
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

// --- The fields ---------------------------------------------------------------
//
// The numbers of the mapping, under the nets, editable (S69b): metres a
// tile across and down, the shift in tiles, the turn in degrees, and the
// verbs that set them whole - Square, a quarter turn, the flips. A field
// reads the active patch and, committed, sets every patch the selection
// holds outright - the frame's knobs are relative; these are the numbers.

// --- The strip: the last modal's numbers, exact -----------------------------

class SSurfUVAdjust : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVAdjust) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		using namespace SurfUVAdjust;

		auto Shown = [](EKind Want)
		{
			return TAttribute<EVisibility>::CreateLambda([Want]() { return bShown && Kind == Want ? EVisibility::Visible : EVisibility::Collapsed; });
		};
		auto Word = [](const FText& Text, const FText& Tip)
		{
			return SNew(SBox).VAlign(VAlign_Center).Padding(4.0f, 0.0f, 3.0f, 0.0f)
				[
					SNew(STextBlock).Text(Text).ToolTipText(Tip).Font(SurfTheme::Small())
				];
		};
		// A field with arrows (S223a), a hundredth a step; a degree for a turn.
		auto Spin = [](TFunction<double()> Get, TFunction<void(double)> Set, double Min, double Max, const FText& Tip, double Step = 0.01, int32 Digits = 3)
		{
			return SNew(SBox).VAlign(VAlign_Center)
				[
					SNew(SSurfNumberField)
					.Width(56.0f)
					.Step(Step)
					.Digits(Digits)
					.Value_Lambda([Get]() { return TOptional<double>(Get()); })
					.OnSet([Set, Min, Max](double V) { Set(FMath::Clamp(V, Min, Max)); })
					.ToolTipText(Tip)
				];
		};
		auto Press = [](const FText& Label, const FText& Tip, TFunction<void()> Run)
		{
			return SNew(SBox).Padding(2.0f, 0.0f).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.Text(Label).ToolTipText(Tip)
					.ContentPadding(FMargin(5.0f, 0.0f))
					.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
				];
		};

		// Each setter applies the difference between what the field says and
		// what the modal left, about the modal's pivot, in its own transaction.
		auto SetScale = [](FVector2D Want)
		{
			Want.X = FMath::Max(0.01, Want.X);
			Want.Y = FMath::Max(0.01, Want.Y);
			USurfEdMode* Mode = ActiveMode();
			if (!Mode || Want.Equals(Scaled, 1.e-6))
			{
				return;
			}
			const FVector2D Ratio(Want.X / Scaled.X, Want.Y / Scaled.Y);
			FScopedTransaction Transaction(LOCTEXT("UVAdjustScale", "Scale UVs"));
			if (Mode->ScaleSelectedUVs(Ratio, Pivot))
			{
				Scaled = Want;
			}
		};
		auto SetMove = [](FVector2D Want)
		{
			USurfEdMode* Mode = ActiveMode();
			if (!Mode || Want.Equals(Moved, 1.e-6))
			{
				return;
			}
			FScopedTransaction Transaction(LOCTEXT("UVAdjustMove", "Move UVs"));
			if (Mode->MoveSelectedUVs(Want - Moved))
			{
				Moved = Want;
			}
		};
		auto SetTurn = [](double Want)
		{
			USurfEdMode* Mode = ActiveMode();
			if (!Mode || FMath::IsNearlyEqual(Want, Turned, 1.e-6))
			{
				return;
			}
			FScopedTransaction Transaction(LOCTEXT("UVAdjustTurn", "Turn UVs"));
			if (Mode->TurnSelectedUVs(Want - Turned, Pivot))
			{
				Turned = Want;
			}
		};

		const FText ScaleTip = LOCTEXT("UVAdjustScaleTip", "The scale the last S left, across and down, about its pivot. Type the exact ratio; the buttons halve, double and undo it.");
		const FText MoveTip = LOCTEXT("UVAdjustMoveTip", "What the last G moved, in tiles across and down. Type the exact figure.");
		const FText TurnTip = LOCTEXT("UVAdjustTurnTip", "What the last R turned, in degrees about its pivot. Type the exact angle.");

		// A small frame at the view's foot after R or S (S226a: "make it a
		// minimalist popup param very small"), as the viewport's frames are:
		// the numbers, the shortcuts, a cross. A move has the foot line.
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(4.0f, 2.0f))
			.Visibility_Lambda([]() { return bShown && ActiveMode() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SHorizontalBox)
				// scale
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Word(LOCTEXT("UVAdjustScaleWord", "Scale"), ScaleTip) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Spin([]() { return Scaled.X; }, [SetScale](double V) { SetScale(FVector2D(V, Scaled.Y)); }, 0.01, 100.0, ScaleTip) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Spin([]() { return Scaled.Y; }, [SetScale](double V) { SetScale(FVector2D(Scaled.X, V)); }, 0.01, 100.0, ScaleTip) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Press(LOCTEXT("UVAdjustHalf", "\u00bd"), LOCTEXT("UVAdjustHalfTip", "Half the size, both ways."), [SetScale]() { SetScale(Scaled * 0.5); }) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Press(LOCTEXT("UVAdjustDouble", "\u00d72"), LOCTEXT("UVAdjustDoubleTip", "Twice the size, both ways."), [SetScale]() { SetScale(Scaled * 2.0); }) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Scale)) [ Press(LOCTEXT("UVAdjustEven", "1:1"), LOCTEXT("UVAdjustEvenTip", "The same across as down: down takes the across ratio."), [SetScale]() { SetScale(FVector2D(Scaled.X, Scaled.X)); }) ] ]
				// turn
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Turn)) [ Word(LOCTEXT("UVAdjustTurnWord", "Turn"), TurnTip) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Turn)) [ Spin([]() { return Turned; }, [SetTurn](double V) { SetTurn(V); }, -360.0, 360.0, TurnTip, 1.0, 1) ] ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SBox).Visibility(Shown(EKind::Turn)) [ Press(LOCTEXT("UVAdjustQuarter", "90"), LOCTEXT("UVAdjustQuarterTip", "Ninety degrees more."), [SetTurn]() { SetTurn(Turned + 90.0); }) ] ]
				// close
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(3.0f, 0.0f))
					.ToolTipText(LOCTEXT("UVAdjustCloseTip", "Put it away; the edit stays."))
					.OnClicked_Lambda([]() { bShown = false; return FReply::Handled(); })
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("\u00d7"))).Font(SurfTheme::Small())
					]
				]
			]
		];
	}
};

/**
 * The mapping's numbers (S69b), in the Mapping menu from S196a ("uv need a
 * menu so theres not always shitload of text showing") and in the
 * quickpanel's Mapping section since S214a. A grid, a row per figure: tile
 * size, shift, turn, flips, Smooth.
 */
class SSurfUVFoot : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVFoot) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		auto Label = [](const FText& Text, const FText& Tip) -> TSharedRef<SWidget>
		{
			return SNew(SBox).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).Text(Text).ToolTipText(Tip).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
				];
		};
		auto Column = [](const FText& Text) -> TSharedRef<SWidget>
		{
			return SNew(SBox).HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(Text).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Dim())
				];
		};
		// A field with arrows, not a slider (S223a), the step by what it is:
		// a hundredth of a tile for the shift, a centimetre-ish for the size,
		// a degree for the turn.
		auto Spin = [](TFunction<double()> Get, TFunction<void(double)> Set, double Min, double Max, const FText& Tip, double Step = 0.01, int32 Digits = 2) -> TSharedRef<SWidget>
		{
			return SNew(SBox).Padding(0.0f, 1.0f, 3.0f, 1.0f)
				[
					SNew(SSurfNumberField)
					.Width(52.0f)
					.Step(Step)
					.Digits(Digits)
					.Value_Lambda([Get]() { return SSurfUVFoot::HasPatch() ? TOptional<double>(Get()) : TOptional<double>(); })
					.OnSet([Set, Min, Max](double V) { Set(FMath::Clamp(V, Min, Max)); })
					.ToolTipText(Tip)
					.Enabled_Lambda([]() { return SSurfUVFoot::HasPatch(); })
				];
		};
		auto Verb = [](const FText& Text, const FText& Tip, TFunction<void()> Run) -> TSharedRef<SWidget>
		{
			return SNew(SBox).Padding(0.0f, 1.0f, 3.0f, 1.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(6.0f, 1.0f))
					.ToolTipText(Tip)
					.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
					.IsEnabled_Lambda([]() { return SSurfUVFoot::HasPatch(); })
					[
						SNew(STextBlock).Text(Text).Font(SurfTheme::Small())
					]
				];
		};
		auto Tiles = [](int32 Axis) -> double
		{
			FVector2D T, S; double Turn; bool bFrozen;
			USurfEdMode* Mode = ActiveMode();
			return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen) ? (Axis == 0 ? T.X : T.Y) : 1.0;
		};
		auto Shift = [](int32 Axis) -> double
		{
			FVector2D T, S; double Turn; bool bFrozen;
			USurfEdMode* Mode = ActiveMode();
			return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen) ? (Axis == 0 ? S.X : S.Y) : 0.0;
		};
		auto TurnNow = []() -> double
		{
			FVector2D T, S; double Turn = 0.0; bool bFrozen;
			USurfEdMode* Mode = ActiveMode();
			return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen) ? Turn : 0.0;
		};
		auto Apply = [](TFunction<void(USurfEdMode::FUVFields&)> Fill)
		{
			if (USurfEdMode* Mode = ActiveMode())
			{
				USurfEdMode::FUVFields Fields;
				Fill(Fields);
				Mode->SetSelectedUVFields(Fields);
			}
		};

		ChildSlot
		[
			SNew(SBox).Padding(FMargin(12.0f, 8.0f, 12.0f, 10.0f))
			[
				SNew(SGridPanel)
				+ SGridPanel::Slot(1, 0) [ Column(LOCTEXT("UVFootAcross", "Across")) ]
				+ SGridPanel::Slot(2, 0) [ Column(LOCTEXT("UVFootDown", "Down")) ]

				+ SGridPanel::Slot(0, 1) [ Label(LOCTEXT("UVFootTiles", "Tile (m)"), LOCTEXT("UVFootTilesTip", "Metres a tile, across and down, on every selected patch. Enter sets it outright.")) ]
				+ SGridPanel::Slot(1, 1)
				[
					Spin([Tiles]() { return Tiles(0); }, [Apply](double V) { Apply([V](USurfEdMode::FUVFields& F) { F.TilesU = V; }); }, 0.01, 1000.0,
						LOCTEXT("UVFootTilesUTip", "Metres a tile across, on every selected patch. Enter sets it outright."))
				]
				+ SGridPanel::Slot(2, 1)
				[
					Spin([Tiles]() { return Tiles(1); }, [Apply](double V) { Apply([V](USurfEdMode::FUVFields& F) { F.TilesV = V; }); }, 0.01, 1000.0,
						LOCTEXT("UVFootTilesVTip", "Metres a tile down, on every selected patch."))
				]
				+ SGridPanel::Slot(3, 1)
				[
					Verb(LOCTEXT("UVFootSquare", "Square"), LOCTEXT("UVFootSquareTip", "A square tile: down takes the across figure, on every selected patch."),
						[Apply]() { Apply([](USurfEdMode::FUVFields& F) { F.bSquare = true; }); })
				]

				+ SGridPanel::Slot(0, 2) [ Label(LOCTEXT("UVFootShift", "Shift"), LOCTEXT("UVFootShiftTip", "The mapping slid, in tiles.")) ]
				+ SGridPanel::Slot(1, 2)
				[
					Spin([Shift]() { return Shift(0); }, [Apply](double V) { Apply([V](USurfEdMode::FUVFields& F) { F.ShiftU = V; }); }, -1000.0, 1000.0,
						LOCTEXT("UVFootShiftUTip", "The mapping slid across, in tiles."))
				]
				+ SGridPanel::Slot(2, 2)
				[
					Spin([Shift]() { return Shift(1); }, [Apply](double V) { Apply([V](USurfEdMode::FUVFields& F) { F.ShiftV = V; }); }, -1000.0, 1000.0,
						LOCTEXT("UVFootShiftVTip", "The mapping slid down, in tiles."))
				]

				+ SGridPanel::Slot(0, 3) [ Label(LOCTEXT("UVFootTurn", "Turn"), LOCTEXT("UVFootTurnTip", "The mapping's turn, in degrees, set outright.")) ]
				+ SGridPanel::Slot(1, 3)
				[
					Spin([TurnNow]() { return TurnNow(); }, [Apply](double V) { Apply([V](USurfEdMode::FUVFields& F) { F.Turn = V; }); }, -360.0, 360.0,
						LOCTEXT("UVFootTurnTip", "The mapping's turn, in degrees, set outright."), 1.0, 1)
				]
				+ SGridPanel::Slot(2, 3)
				[
					Verb(LOCTEXT("UVFootQuarter", "¼"), LOCTEXT("UVFootQuarterTip", "A quarter turn about the selection's pivot (Ctrl+R in the window). The fix for a mapping ninety degrees off, without the mirror a swap brings."),
						[]() { if (USurfEdMode* Mode = ActiveMode()) { Mode->TurnSelectedUVsQuarter(); } })
				]

				+ SGridPanel::Slot(0, 4) [ Label(LOCTEXT("UVFootFlip", "Flip"), LOCTEXT("UVFootFlipTip", "Mirror the mapping.")) ]
				+ SGridPanel::Slot(1, 4)
				[
					Verb(LOCTEXT("UVFootFlipU", "↔"), LOCTEXT("UVFootFlipUTip", "Flip the mapping across."),
						[]() { if (USurfEdMode* Mode = ActiveMode()) { if (Mode->Inspector) { Mode->Inspector->FlipPatchU(); } } })
				]
				+ SGridPanel::Slot(2, 4)
				[
					Verb(LOCTEXT("UVFootFlipV", "↕"), LOCTEXT("UVFootFlipVTip", "Flip the mapping down."),
						[]() { if (USurfEdMode* Mode = ActiveMode()) { if (Mode->Inspector) { Mode->Inspector->FlipPatchV(); } } })
				]

				+ SGridPanel::Slot(0, 5).ColumnSpan(4).Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(&SurfTheme::ToolButton())
						.ContentPadding(FMargin(6.0f, 1.0f))
						.ToolTipText(LOCTEXT("UVFootSmoothTip", "Relax the UVs: each picked point towards the mean of its neighbours (every interior point when none is picked), the border kept. A patch with a rule is frozen first. Strength and passes are the Smooth verb's."))
						.OnClicked_Lambda([]()
						{
							if (USurfEdMode* Mode = ActiveMode())
							{
								const USurfInspector* P = Mode->Inspector;
								Mode->SmoothSelectedUVs(P ? P->Strength : 0.5, P ? P->Iterations : 4);
							}
							return FReply::Handled();
						})
						.IsEnabled_Lambda([]()
						{
							FVector2D T, S; double Turn; bool bFrozen = false;
							USurfEdMode* Mode = ActiveMode();
							return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen);
						})
						[
							SNew(STextBlock).Text(LOCTEXT("UVFootSmooth", "Smooth")).Font(SurfTheme::Small())
						]
					]
					// Even here too (S226a: "probably things like Even belongs here too").
					+ SHorizontalBox::Slot().AutoWidth().Padding(3.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(&SurfTheme::ToolButton())
						.ContentPadding(FMargin(6.0f, 1.0f))
						.ToolTipText(LOCTEXT("UVFootEvenTip", "Each cell its own size in the texture, in one unit for the patch - measured once and kept on the lines, so a long cell is a long run of tiles. For trims laid by the cell."))
						.OnClicked_Lambda([]()
						{
							if (USurfEdMode* Mode = ActiveMode()) { if (Mode->Inspector) { Mode->NoteVerb(TEXT("Even"), [Mode]() { Mode->Inspector->Even(); }); Mode->Inspector->Even(); } }
							return FReply::Handled();
						})
						.IsEnabled_Lambda([]()
						{
							FVector2D T, S; double Turn; bool bFrozen = false;
							USurfEdMode* Mode = ActiveMode();
							return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen);
						})
						[
							SNew(STextBlock).Text(LOCTEXT("UVFootEven", "Even")).Font(SurfTheme::Small())
						]
					]
				]
			]
		];
	}

	static bool HasPatch()
	{
		FVector2D T, S; double Turn; bool bFrozen = false;
		USurfEdMode* Mode = ActiveMode();
		return Mode && Mode->ReadActiveUVFields(T, S, Turn, bFrozen) && !bFrozen;
	}

	/** One dim line, empty when all is well: nothing picked, or a frozen patch. */
	static FText Status()
	{
		FVector2D T, S; double Turn; bool bFrozen = false;
		USurfEdMode* Mode = ActiveMode();
		if (!Mode || !Mode->ReadActiveUVFields(T, S, Turn, bFrozen)) { return LOCTEXT("UVFootNone", "select a patch"); }
		return bFrozen ? LOCTEXT("UVFootFrozen", "frozen: the points are the mapping") : FText::GetEmpty();
	}
};

/**
 * The foot line (S208a, redone S222a - "the uv panel edit is FUCKED ...
 * compare to other softwares"): as every UV editor has it, two fields, U
 * and V, the middle of what is picked (the picked points, else the
 * selected nets); type a number, step the arrows or wheel over the field
 * and the selection moves at once; a small crosshair beside puts it into the 0,0
 * tile by whole tiles (nothing torn). The box of the selection in grey
 * after, when it has a size.
 */
class SSurfUVFootLine : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVFootLine) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		auto Field = [](int32 Axis, const FText& Tip) -> TSharedRef<SWidget>
		{
			auto Read = [Axis]() -> TOptional<double>
			{
				FVector2D Low, High, Centre;
				USurfEdMode* Mode = ActiveMode();
				return Mode && Mode->SelectedUVExtent(Low, High, Centre) ? TOptional<double>(Centre[Axis]) : TOptional<double>();
			};
			return SNew(SSurfNumberField)
				.Width(66.0f)
				.Step(0.01)
				.Digits(3)
				.Mark(Axis == 0 ? FLinearColor(0.85f, 0.25f, 0.25f) : FLinearColor(0.35f, 0.75f, 0.3f))
				.ToolTipText(Tip)
				.Value_Lambda([Read]() { return Read(); })
				.Enabled_Lambda([Read]() { return Read().IsSet(); })
				.OnSet([Axis, Read](double Value)
				{
					USurfEdMode* Mode = ActiveMode();
					const TOptional<double> Now = Read();
					if (!Mode || !Now.IsSet()) { return; }
					FVector2D Delta = FVector2D::ZeroVector;
					Delta[Axis] = Value - Now.GetValue();
					if (Delta.IsNearlyZero(1.e-6)) { return; }
					FScopedTransaction Transaction(LOCTEXT("UVFootSet", "Move UVs"));
					if (!Mode->MoveSelectedUVs(Delta)) { Transaction.Cancel(); }
					Mode->RedrawViewports();
					SurfSession::Verb(TEXT("uv_foot"), Axis == 0 ? TEXT("u set") : TEXT("v set"));
				});
		};
		auto Word = [](const FText& Text) -> TSharedRef<SWidget>
		{
			return SNew(SBox).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock).Text(Text).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
				];
		};
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(6.0f, 2.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth() [ Word(LOCTEXT("UVFootU", "U")) ]
				+ SHorizontalBox::Slot().AutoWidth() [ Field(0, LOCTEXT("UVFootUTip", "Where the selection's middle is across - the picked points, else the selected nets. Type a number, step the arrows or wheel over it (Shift: ten times, Ctrl: a tenth); the selection moves at once.")) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f) [ Word(LOCTEXT("UVFootV", "V")) ]
				+ SHorizontalBox::Slot().AutoWidth() [ Field(1, LOCTEXT("UVFootVTip", "Where the selection's middle is down. Type a number, step the arrows or wheel over it; the selection moves at once.")) ]
				// One press (S209a: two were "confusing as hell"), an icon
				// since S222a: the selection back into the 0,0 tile by whole
				// tiles, so nothing tears.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&SurfTheme::ToolButton())
					.ContentPadding(FMargin(3.0f, 1.0f))
					.ToolTipText(LOCTEXT("UVFootTileTip", "The selection's low corner onto 0,0."))
					.IsEnabled_Lambda([]() { FVector2D L, H, C; USurfEdMode* M = ActiveMode(); return M && M->SelectedUVExtent(L, H, C); })
					.OnClicked_Lambda([]() { Shift(); return FReply::Handled(); })
					[
						SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
						[
							SNew(SImage).Image(SurfIcons::Named(TEXT("focus-centered"))).ColorAndOpacity(SurfTheme::Quiet())
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(SurfTheme::Small())
					.ColorAndOpacity(SurfTheme::Dim())
					.Text_Lambda([]()
					{
						FVector2D Low, High, Centre;
						USurfEdMode* Mode = ActiveMode();
						if (!Mode || !Mode->SelectedUVExtent(Low, High, Centre) || (High - Low).IsNearlyZero(1.e-4))
						{
							return FText::GetEmpty();
						}
						return FText::Format(LOCTEXT("UVFootBox", "box {0} × {1}"), FText::AsNumber(High.X - Low.X, &Two()), FText::AsNumber(High.Y - Low.Y, &Two()));
					})
				]
			]
		];
	}

private:
	static const FNumberFormattingOptions& Two()
	{
		static const FNumberFormattingOptions Options = FNumberFormattingOptions().SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		return Options;
	}

	static void Shift()
	{
		FVector2D Low, High, Centre;
		USurfEdMode* Mode = ActiveMode();
		if (!Mode || !Mode->SelectedUVExtent(Low, High, Centre))
		{
			return;
		}
		// The selection's low corner onto 0,0 (S226a: "the 0 0 button doesnt
		// bring back to 0 0" - it slid by whole tiles, which is nothing when
		// the selection already sat in the first tile).
		const FVector2D Delta = -Low;
		if (Delta.IsNearlyZero(1.e-6))
		{
			return;
		}
		FScopedTransaction Transaction(LOCTEXT("UVFootShift", "Move UVs"));
		if (!Mode->MoveSelectedUVs(Delta))
		{
			Transaction.Cancel();
		}
		Mode->RedrawViewports();
		SurfSession::Verb(TEXT("uv_foot"), TEXT("corner to 0,0"));
	}
};

/**
 * The window's quickpanel (S214a, tabbed S216a: "the uv options shall be in
 * a Quickpanel like our Npanel in viewport"): the SidePanel's manner down
 * the window's right edge - tabs on the outside, Select, Transform,
 * Islands, Mapping, View; a click shows one alone, again folds the panel to
 * its tabs, Ctrl+click pins one beside the others; N over the window opens
 * and closes it. The mapping's numbers moved in here from the menu bar.
 */
namespace SurfUVPanel
{
	enum class ETab : uint8 { Select, Transform, Islands, Mapping, View, Count };

	constexpr float TabStripWidth = 20.0f;
	constexpr float GripWidth = 5.0f;

	/** The tabs top to bottom, as he arranged them (S219a). */
	TArray<ETab> Order()
	{
		const int32 Count = static_cast<int32>(ETab::Count);
		TArray<ETab> Out;
		if (const USurfSettings* S = GetDefault<USurfSettings>())
		{
			TArray<FString> Parts;
			S->UVPanelOrder.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				const int32 Index = FCString::Atoi(*Part);
				if (Index >= 0 && Index < Count && !Out.Contains(static_cast<ETab>(Index)))
				{
					Out.Add(static_cast<ETab>(Index));
				}
			}
		}
		for (int32 I = 0; I < Count; ++I)
		{
			Out.AddUnique(static_cast<ETab>(I));
		}
		return Out;
	}

	void SetOrder(const TArray<ETab>& NewOrder)
	{
		if (USurfSettings* S = GetMutableDefault<USurfSettings>())
		{
			TArray<FString> Parts;
			for (ETab Tab : NewOrder) { Parts.Add(FString::FromInt(static_cast<int32>(Tab))); }
			S->UVPanelOrder = FString::Join(Parts, TEXT(","));
		}
	}

	int32 BodyWidth()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return S ? FMath::Clamp(S->UVPanelWidthPx, 180, 520) : 250;
	}

	int32 Bit(ETab Tab) { return 1 << static_cast<int32>(Tab); }

	int32 ShownMask()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return S ? (S->UVPanelShown & ((1 << static_cast<int32>(ETab::Count)) - 1)) : 0;
	}

	void SetShownMask(int32 Mask)
	{
		if (USurfSettings* S = GetMutableDefault<USurfSettings>())
		{
			S->UVPanelShown = Mask;
			S->SaveConfig();
		}
	}

	bool IsOpen()
	{
		const USurfSettings* S = GetDefault<USurfSettings>();
		return S && S->bUVPanelOpen;
	}

	bool IsShown(ETab Tab) { return (ShownMask() & Bit(Tab)) != 0; }
	bool IsFolded() { return ShownMask() == 0; }

	/** How much of the view's right edge the panel covers: its body when a tab is shown, else its tabs. */
	float Covered()
	{
		if (!IsOpen()) { return 0.0f; }
		return TabStripWidth + (IsFolded() ? 0.0f : GripWidth + static_cast<float>(BodyWidth()));
	}

	void Toggle()
	{
		if (USurfSettings* S = GetMutableDefault<USurfSettings>())
		{
			S->bUVPanelOpen = !S->bUVPanelOpen;
			if (S->bUVPanelOpen && ShownMask() == 0)
			{
				S->UVPanelShown = Bit(ETab::Mapping);
			}
			S->SaveConfig();
			SurfSession::Verb(TEXT("uv_panel"), S->bUVPanelOpen ? TEXT("open") : TEXT("closed"));
		}
	}

	/** A click on a tab: the one shown alone, or folded when it already was; with Ctrl, pinned or unpinned. */
	void ClickTab(ETab Tab, bool bCtrl)
	{
		const int32 Mask = ShownMask();
		if (bCtrl)
		{
			SetShownMask(Mask ^ Bit(Tab));
		}
		else if (Mask == Bit(Tab))
		{
			SetShownMask(0);
		}
		else
		{
			SetShownMask(Bit(Tab));
		}
		SurfSession::Verb(TEXT("uv_panel_tab"), FString::Printf(TEXT("%d shown=%d"), static_cast<int32>(Tab), ShownMask()));
	}

	FText TabName(ETab Tab)
	{
		switch (Tab)
		{
		case ETab::Select:    return LOCTEXT("UVTabSelect", "Select");
		case ETab::Transform: return LOCTEXT("UVTabTransform", "Transform");
		case ETab::Islands:   return LOCTEXT("UVTabIslands", "Islands");
		case ETab::Mapping:   return LOCTEXT("UVTabMapping", "Mapping");
		case ETab::View:      return LOCTEXT("UVTabView", "View");
		default:              return FText::GetEmpty();
		}
	}

	TSharedRef<SWidget> Press(const FText& Label, const FText& Tip, TFunction<void()> Run, TFunction<bool()> Can)
	{
		return SNew(SBox).Padding(0.0f, 1.0f, 3.0f, 1.0f)
			[
				SNew(SButton)
				.ButtonStyle(&SurfTheme::ToolButton())
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(7.0f, 1.0f))
				.ToolTipText(Tip)
				.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
				.IsEnabled_Lambda([Can]() { return Can ? Can() : ActiveMode() != nullptr; })
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Small())
				]
			];
	}

	TSharedRef<SWidget> Word(const FText& Text, const FText& Tip)
	{
		return SNew(SBox).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f).WidthOverride(46.0f)
			[
				SNew(STextBlock).Text(Text).ToolTipText(Tip).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
			];
	}

	TSharedRef<SWidget> Tick(const FText& Label, const FText& Tip, bool USurfSettings::* Field)
	{
		return SNew(SBox).Padding(0.0f, 1.0f)
			[
				SNew(SCheckBox)
				.ToolTipText(Tip)
				.IsChecked_Lambda([Field]() { const USurfSettings* S = GetDefault<USurfSettings>(); return S && S->*Field ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Field](ECheckBoxState State)
				{
					if (USurfSettings* S = GetMutableDefault<USurfSettings>())
					{
						S->*Field = State == ECheckBoxState::Checked;
						S->SaveConfig();
					}
				})
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Text())
				]
			];
	}

	bool Held()
	{
		USurfEdMode* Mode = ActiveMode();
		return Mode && (Mode->GetSelectedPatches().Num() > 0 || Mode->PatchesHeldBySelection().Num() > 0);
	}

	bool Picked()
	{
		USurfEdMode* Mode = ActiveMode();
		return Mode && Mode->GetPointSelection().Num() > 0;
	}

	TSharedRef<SWidget> BuildSelect()
	{
		return SNew(SWrapBox).UseAllottedSize(true)
			+ SWrapBox::Slot() [ Press(LOCTEXT("UVSelAll", "All"), LOCTEXT("UVSelAllTip", "Every point of the selected patches (A in the window)."), []() { if (USurfEdMode* M = ActiveMode()) { M->SelectAllPoints(); } }, &Held) ]
			+ SWrapBox::Slot() [ Press(LOCTEXT("UVSelNone", "None"), LOCTEXT("UVSelNoneTip", "No point picked (Alt+A in the window); the patches stay selected."), []() { if (USurfEdMode* M = ActiveMode()) { M->ClearPointSelection(); M->RedrawViewports(); } }, &Picked) ]
			+ SWrapBox::Slot() [ Press(LOCTEXT("UVSelInvert", "Invert"), LOCTEXT("UVSelInvertTip", "The picked points unpicked and the rest picked, on the selected patches."), []() { if (USurfEdMode* M = ActiveMode()) { M->InvertPointSelection(); } }, &Held) ]
			+ SWrapBox::Slot() [ Press(LOCTEXT("UVSelLinked", "Linked"), LOCTEXT("UVSelLinkedTip", "The patches joined to the selected ones by a shared edge, added to the selection."), []() { if (USurfEdMode* M = ActiveMode()) { M->SelectLinkedPatches(); } }, &Held) ]
			+ SWrapBox::Slot() [ Press(LOCTEXT("UVSelBorder", "Border"), LOCTEXT("UVSelBorderTip", "The outer ring of points of each selected patch."), []() { if (USurfEdMode* M = ActiveMode()) { M->SelectBorderPoints(); } }, &Held) ];
	}

	TSharedRef<SWidget> AlignRow(int32 Axis)
	{
		const bool bU = Axis == 0;
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth() [ Word(bU ? LOCTEXT("UVAlignU", "Align U") : LOCTEXT("UVAlignV", "Align V"), LOCTEXT("UVAlignTip", "The picked points put on one line: the selection's low edge, its centre, or its high edge. A patch with a rule is frozen first.")) ]
			+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVAlignLow", "Low"), bU ? LOCTEXT("UVAlignLowUTip", "Every picked point to the selection's left edge.") : LOCTEXT("UVAlignLowVTip", "Every picked point to the selection's bottom edge."), [Axis]() { if (USurfEdMode* M = ActiveMode()) { M->AlignSelectedUVs(Axis, 0); } }, &Held) ]
			+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVAlignMid", "Centre"), LOCTEXT("UVAlignMidTip", "Every picked point to the selection's middle."), [Axis]() { if (USurfEdMode* M = ActiveMode()) { M->AlignSelectedUVs(Axis, 1); } }, &Held) ]
			+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVAlignHigh", "High"), bU ? LOCTEXT("UVAlignHighUTip", "Every picked point to the selection's right edge.") : LOCTEXT("UVAlignHighVTip", "Every picked point to the selection's top edge."), [Axis]() { if (USurfEdMode* M = ActiveMode()) { M->AlignSelectedUVs(Axis, 2); } }, &Held) ];
	}

	TSharedRef<SWidget> BuildTransform()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight() [ AlignRow(0) ]
			+ SVerticalBox::Slot().AutoHeight() [ AlignRow(1) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth() [ Word(LOCTEXT("UVMirror", "Mirror"), LOCTEXT("UVMirrorTip", "The picked points mirrored about the selection's centre. A patch with a rule is frozen first; the Mapping's flips turn a rule instead.")) ]
				+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVMirrorU", "↔"), LOCTEXT("UVMirrorUTip", "Mirror the picked points across."), []() { if (USurfEdMode* M = ActiveMode()) { M->FlipSelectedUVs(0); } }, &Held) ]
				+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVMirrorV", "↕"), LOCTEXT("UVMirrorVTip", "Mirror the picked points down."), []() { if (USurfEdMode* M = ActiveMode()) { M->FlipSelectedUVs(1); } }, &Held) ]
				+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVQuarter", "¼ turn"), LOCTEXT("UVQuarterTip", "A quarter turn about the selection's pivot (Ctrl+R in the window)."), []() { if (USurfEdMode* M = ActiveMode()) { M->TurnSelectedUVsQuarter(); } }, &Held) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Dim())
				.Text(LOCTEXT("UVTransformNote", "G R S move, turn, scale; the strip under the view holds their numbers."))
				.AutoWrapText(true)
			];
	}

	TSharedRef<SWidget> BuildIslands()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth() [ Press(LOCTEXT("UVPack", "Pack"), LOCTEXT("UVPackTip", "The selected patches' nets laid into the 0,0 tile, tallest first, their sizes kept relative to one another and the whole scaled to fill; the gap is the margin beside. A rule is frozen first."), []() { if (USurfEdMode* M = ActiveMode()) { const USurfSettings* S = GetDefault<USurfSettings>(); M->PackSelectedUVs(S ? S->UVPackMargin : 0.02); } }, &Held) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SSurfNumberField)
					.Width(46.0f)
					.Step(0.005)
					.Digits(3)
					.ToolTipText(LOCTEXT("UVPackMarginTip", "Pack's gap between islands and round the tile, in tiles."))
					.Value_Lambda([]() { const USurfSettings* S = GetDefault<USurfSettings>(); return TOptional<double>(S ? S->UVPackMargin : 0.02f); })
					.OnSet([](double V) { if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->UVPackMargin = FMath::Clamp(static_cast<float>(V), 0.0f, 0.2f); S->SaveConfig(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("UVPackMargin", "Margin")).Font(SurfTheme::Small()).ColorAndOpacity(SurfTheme::Dim())
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true)
				+ SWrapBox::Slot() [ Press(LOCTEXT("UVStack", "Stack"), LOCTEXT("UVStackTip", "Every selected net moved onto the first's centre, so alike pieces share one place on the texture. A rule is frozen first."), []() { if (USurfEdMode* M = ActiveMode()) { M->StackSelectedUVs(); } }, &Held) ]
				+ SWrapBox::Slot() [ Press(LOCTEXT("UVRectify", "Rectify"), LOCTEXT("UVRectifyTip", "Each selected net made an even grid inside its own box, its turn kept - the straight-edged mapping of a net that has been pulled about. A rule is frozen first."), []() { if (USurfEdMode* M = ActiveMode()) { M->RectifySelectedUVs(); } }, &Held) ]
				+ SWrapBox::Slot() [ Press(LOCTEXT("UVStraighten", "Straighten"), LOCTEXT("UVStraightenTip", "The picked points of one row or column of a net put on one line, along the axis they lean to; each keeps its place along it."), []() { if (USurfEdMode* M = ActiveMode()) { M->StraightenSelectedUVs(); } }, &Picked) ]
			];
	}

	TSharedRef<SWidget> BuildView()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight() [ Tick(LOCTEXT("UVViewTexture", "Texture behind the nets"), LOCTEXT("UVViewTextureTip", "The active patch's texture - its colour one - faint under the nets."), &USurfSettings::bUVShowTexture) ]
			+ SVerticalBox::Slot().AutoHeight() [ Tick(LOCTEXT("UVViewTile", "Repeat it on every tile"), LOCTEXT("UVViewTileTip", "Off: on the first tile only, the one the texture is."), &USurfSettings::bUVTileTexture) ]
			+ SVerticalBox::Slot().AutoHeight() [ Tick(LOCTEXT("UVViewStretch", "Stretch"), LOCTEXT("UVViewStretchTip", "Each cell coloured by how its share of the texture compares with its share of the surface, against the net's own middle: blue tight, green even, red stretched."), &USurfSettings::bUVStretchView) ];
	}
}

class SSurfUVPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		using namespace SurfUVPanel;
		for (int32 I = 0; I < static_cast<int32>(ETab::Count); ++I)
		{
			Tabs[I] = BuildTab(static_cast<ETab>(I));
			Sections[I] = BuildSection(static_cast<ETab>(I));
		}
		SAssignNew(TabBox, SVerticalBox).Visibility(EVisibility::SelfHitTestInvisible);
		SAssignNew(SectionBox, SVerticalBox);
		Rearrange();

		// The edge that sizes it (S219a): five pixels on the inner side, shown
		// only while it is dragged - the SidePanel's grip.
		TSharedRef<SWidget> Grip = SNew(SBox)
			.WidthOverride(GripWidth)
			.Visibility_Lambda([]() { return IsFolded() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this]() { return bResizing ? FStyleColors::White25 : FSlateColor(FLinearColor::Transparent); })
				.Cursor(EMouseCursor::ResizeLeftRight)
				.ToolTipText(LOCTEXT("UVGripTip", "Drag to make the panel wider or narrower."))
				.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent& Event) -> FReply
				{
					if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
					{
						return FReply::Unhandled();
					}
					DragStartWidth = BodyWidth();
					DragStartX = static_cast<float>(Event.GetScreenSpacePosition().X);
					bResizing = true;
					return FReply::Handled().CaptureMouse(SharedThis(this));
				})
			];

		// The grip stands as tall as the body (S220a: an empty border in a
		// top-aligned slot was nothing to grab), the pair as tall as the
		// content and no taller (S217a), framed by a hair of the darker grey
		// so the foot shows against the window (S219b).
		// The panel's own boxes let the hand through (S224a): the panel's
		// rectangle is the tabs' full length by the body's width, and the
		// empty part of it - under a short section, left of the tabs - is
		// over the nets. A hit on a box of ours went nowhere; "cant scroll,
		// cant click". Only the body, the grip and the tabs take the hand.
		SetVisibility(EVisibility::SelfHitTestInvisible);
		TSharedRef<SHorizontalBox> Sheet = SNew(SHorizontalBox).Visibility(EVisibility::SelfHitTestInvisible);
		Sheet->AddSlot().AutoWidth()[ Grip ];
		Sheet->AddSlot().AutoWidth()
		[
				SNew(SBox)
				.WidthOverride_Lambda([]() { return FOptionalSize(static_cast<float>(BodyWidth())); })
				.Visibility_Lambda([]() { return IsFolded() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					.Padding(FMargin(1.0f, 0.0f, 0.0f, 1.0f))
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
						.Padding(0.0f)
						[
							SNew(SScrollBox)
							.ScrollBarThickness(FVector2D(2.0f, 2.0f))
							.ConsumeMouseWheel(EConsumeMouseWheel::Always)
							+ SScrollBox::Slot()
							[
								SectionBox.ToSharedRef()
							]
						]
					]
				]
		];

		ChildSlot
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([]() { return IsOpen() ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				Sheet
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SBox).WidthOverride(TabStripWidth).Visibility(EVisibility::SelfHitTestInvisible)
				[
					TabBox.ToSharedRef()
				]
			]
		];
	}

	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		using namespace SurfUVPanel;
		if (bResizing && HasMouseCapture())
		{
			const float Scale = FMath::Max(0.1f, Geometry.Scale);
			const float Moved = (static_cast<float>(Event.GetScreenSpacePosition().X) - DragStartX) / Scale;
			// The grip is on the left: dragging left widens it.
			if (USurfSettings* S = GetMutableDefault<USurfSettings>())
			{
				S->UVPanelWidthPx = FMath::Clamp(FMath::RoundToInt(DragStartWidth - Moved), 180, 520);
			}
			return FReply::Handled();
		}
		if (PressedTab >= 0 && HasMouseCapture())
		{
			const float Y = static_cast<float>(Event.GetScreenSpacePosition().Y);
			if (!bTabDragging && FMath::Abs(Y - PressY) > 5.0f * FMath::Max(0.1f, Geometry.Scale))
			{
				bTabDragging = true;
			}
			if (bTabDragging)
			{
				// Over another tab's half: the two change places, live.
				TArray<ETab> Now = Order();
				const int32 From = Now.IndexOfByKey(static_cast<ETab>(PressedTab));
				for (int32 To = 0; To < Now.Num(); ++To)
				{
					if (To == From)
					{
						continue;
					}
					const FGeometry& Other = Tabs[static_cast<int32>(Now[To])]->GetCachedGeometry();
					const float Top = static_cast<float>(Other.GetAbsolutePosition().Y);
					const float Height = static_cast<float>(Other.GetAbsoluteSize().Y);
					if (Height < 1.0f)
					{
						continue;
					}
					const bool bPast = To < From ? Y < Top + Height * 0.5f : Y > Top + Height * 0.5f;
					const bool bOver = Y >= Top && Y <= Top + Height;
					if (bOver && bPast)
					{
						const ETab Moving = Now[From];
						Now.RemoveAt(From);
						Now.Insert(Moving, To);
						SetOrder(Now);
						Rearrange();
						break;
					}
				}
			}
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		using namespace SurfUVPanel;
		if (bResizing)
		{
			bResizing = false;
			if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->SaveConfig(); }
			return FReply::Handled().ReleaseMouseCapture();
		}
		if (PressedTab >= 0)
		{
			const ETab Tab = static_cast<ETab>(PressedTab);
			if (bTabDragging)
			{
				if (USurfSettings* S = GetMutableDefault<USurfSettings>()) { S->SaveConfig(); }
				SurfSession::Verb(TEXT("uv_panel_order"), GetDefault<USurfSettings>()->UVPanelOrder);
			}
			else
			{
				ClickTab(Tab, bPressedCtrl);
			}
			PressedTab = -1;
			bTabDragging = false;
			return FReply::Handled().ReleaseMouseCapture();
		}
		return FReply::Unhandled();
	}

private:
	void Rearrange()
	{
		TabBox->ClearChildren();
		SectionBox->ClearChildren();
		for (SurfUVPanel::ETab Tab : SurfUVPanel::Order())
		{
			const int32 I = static_cast<int32>(Tab);
			TabBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)[ Tabs[I].ToSharedRef() ];
			SectionBox->AddSlot().AutoHeight()[ Sections[I].ToSharedRef() ];
		}
	}

	bool bResizing = false;
	float DragStartX = 0.0f;
	int32 DragStartWidth = 250;
	int32 PressedTab = -1;
	bool bPressedCtrl = false;
	bool bTabDragging = false;
	float PressY = 0.0f;
	TSharedPtr<SVerticalBox> TabBox;
	TSharedPtr<SVerticalBox> SectionBox;
	TSharedPtr<SWidget> Tabs[static_cast<int32>(SurfUVPanel::ETab::Count)];
	TSharedPtr<SWidget> Sections[static_cast<int32>(SurfUVPanel::ETab::Count)];

private:
	/** A tab down the outer edge, the SidePanel's: grey when shown with a blue line outside, darker otherwise, lifted under the cursor. */
	TSharedRef<SWidget> BuildTab(SurfUVPanel::ETab Tab)
	{
		using namespace SurfUVPanel;
		TSharedPtr<SBorder> Holder;
		SAssignNew(Holder, SBorder)
			.Padding(FMargin(3.0f, 9.0f))
			.Cursor(EMouseCursor::Hand)
			.ToolTipText(FText::Format(LOCTEXT("UVTabTip", "{0}. Click: this one alone - again to fold the panel to its tabs. Ctrl+click: pin it beside the others. Drag: up or down. N over the window opens and closes the panel."), TabName(Tab)))
			.OnMouseButtonDown_Lambda([this, Tab](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
				{
					return FReply::Unhandled();
				}
				PressedTab = static_cast<int32>(Tab);
				bPressedCtrl = Event.IsControlDown();
				bTabDragging = false;
				PressY = static_cast<float>(Event.GetScreenSpacePosition().Y);
				return FReply::Handled().CaptureMouse(SharedThis(this));
			})
			[
				SNew(SSurfVerticalLabel)
				.Text(TabName(Tab))
				.Colour_Lambda([Tab]() { return IsShown(Tab) ? FSlateColor(FStyleColors::White) : FSlateColor(FStyleColors::Foreground); })
			];
		TWeakPtr<SBorder> Weak = Holder;
		Holder->SetBorderImage(TAttribute<const FSlateBrush*>::CreateLambda([this, Weak, Tab]() -> const FSlateBrush*
		{
			if (bTabDragging && PressedTab == static_cast<int32>(Tab))
			{
				return FAppStyle::GetBrush("Brushes.Hover");
			}
			if (IsShown(Tab))
			{
				return FAppStyle::GetBrush("Brushes.Panel");
			}
			TSharedPtr<SBorder> Live = Weak.Pin();
			return Live.IsValid() && Live->IsHovered() ? FAppStyle::GetBrush("Brushes.Header") : FAppStyle::GetBrush("Brushes.Recessed");
		}));
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				Holder.ToSharedRef()
			]
			+ SOverlay::Slot().HAlign(HAlign_Right)
			[
				SNew(SBox)
				.WidthOverride(2.0f)
				.Visibility_Lambda([Tab]() { return IsShown(Tab) ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FStyleColors::Primary)
					.Padding(0.0f)
				]
			];
	}

	/** A shown tab's section: the header strip with its name and a cross, the content under. */
	TSharedRef<SWidget> BuildSection(SurfUVPanel::ETab Tab)
	{
		using namespace SurfUVPanel;
		TSharedRef<SWidget> Content = SNullWidget::NullWidget;
		// A little in from the edges (S226a: "the side panel buttons are too
		// close from the edge of the window").
		FMargin Pad(12.0f, 8.0f, 12.0f, 10.0f);
		switch (Tab)
		{
		case ETab::Select:    Content = BuildSelect(); break;
		case ETab::Transform: Content = BuildTransform(); break;
		case ETab::Islands:   Content = BuildIslands(); break;
		case ETab::Mapping:   Content = SNew(SSurfUVFoot); Pad = FMargin(0.0f, 0.0f, 0.0f, 2.0f); break;
		case ETab::View:      Content = BuildView(); break;
		default: break;
		}
		return SNew(SBox)
			.Visibility_Lambda([Tab]() { return IsShown(Tab) ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
					.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
					[
						SNew(SBox).HeightOverride(24.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[
								SNew(STextBlock).Text(TabName(Tab)).Font(SurfTheme::Bold(8)).ColorAndOpacity(SurfTheme::Header())
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton)
								.ButtonStyle(FAppStyle::Get(), "SimpleButton")
								.ContentPadding(FMargin(3.0f, 0.0f))
								.ToolTipText(LOCTEXT("UVCloseTip", "Put this one away; its tab stays."))
								.OnClicked_Lambda([Tab]() { ClickTab(Tab, /*bCtrl*/ true); return FReply::Handled(); })
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush("Icons.X"))
									.DesiredSizeOverride(FVector2D(10.0, 10.0))
									.ColorAndOpacity(FSlateColor::UseSubduedForeground())
								]
							]
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(Pad)
				[
					Content
				]
			];
	}
};

/**
 * The window's menu bar (S196a): "uv need a menu so theres not always
 * shitload of text showing". One pull-down over the net - Actions, the UV verbs as the top strip's UV menu
 * lists them (right-click: options, the Q menu, the sidebar) - and one dim
 * word at the right when something needs saying, and at the far right the
 * chevron that shows or puts away the quickpanel, where the mapping's
 * numbers and the view's ticks live since S214a.
 */
class SSurfUVMenuBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVMenuBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		auto PullDown = &SurfWindows::MenuBarPullDown;

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(FMargin(2.0f, 1.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					PullDown(LOCTEXT("UVMenuActions", "Actions"), LOCTEXT("UVMenuActionsTip", "The UV verbs. Right-click one: options, the Q menu, the sidebar."),
						[]() -> TSharedRef<SWidget> { return MakeActionsMenu(); })
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SSpacer)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(SurfTheme::Small())
					.ColorAndOpacity(SurfTheme::Dim())
					.Text_Lambda([]() { return SSurfUVFoot::Status(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "NoBorder")
					.ContentPadding(FMargin(5.0f, 3.0f))
					.ToolTipText(LOCTEXT("UVMenuPanelTip", "The quickpanel down the window's right - Select, Transform, Islands, Mapping, View - shown or put away (N over the window)."))
					.OnClicked_Lambda([]() { SurfUVPanel::Toggle(); return FReply::Handled(); })
					[
						SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)
						[
							SNew(SImage)
							.ColorAndOpacity(SurfTheme::Dim())
							.Image_Lambda([]() { return FAppStyle::GetBrush(SurfUVPanel::IsOpen() ? "Icons.ChevronRight" : "Icons.ChevronLeft"); })
						]
					]
				]
			]
		];
	}

private:
	/** The top strip's UV family menu, the same build (S226a: "the theme of it - like the menu of the viewport"). */
	static TSharedRef<SWidget> MakeActionsMenu()
	{
		FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
		if (USurfEdMode* Mode = ActiveMode())
		{
			SurfMenu::BuildFamily(Menu, Mode, ESurfVerbGroup::UV);
		}
		else
		{
			Menu.AddMenuEntry(LOCTEXT("UVMenuNoMode", "Enter the SurfacingPro mode to use these"), FText::GetEmpty(), FSlateIcon(), FUIAction());
		}
		return Menu.MakeWidget();
	}
};

// --- The tab ----------------------------------------------------------------

namespace SurfUVTab
{
	const FName TabName("SurfacingProUV");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("UVTabLabel", "UVs"))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSurfUVMenuBar)
				]
				// The panel floats over the view's right edge, as the SidePanel
				// floats over the viewport (S221a: "in red on screen = WASTED
				// SPACE" - a column of its own left the view narrow and the
				// space under the panel black).
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SSurfUVView)
					]
					// A little below the bar, as the viewport's panel sits (S222a).
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SSurfUVPanel)
					]
					// The R / S numbers as a small frame over the foot of the
					// view (S226a), gone with the next pick or the cross.
					+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(8.0f, 0.0f, 0.0f, 28.0f)
					[
						SNew(SSurfUVAdjust)
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SSurfUVFootLine)
				]
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized() || FGlobalTabmanager::Get()->HasTabSpawner(TabName))
		{
			return;
		}

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
			.SetDisplayName(LOCTEXT("UVTabTitle", "SurfacingPro UVs"))
			.SetTooltipText(LOCTEXT("UVTabTip", "The selected patches' UV nets, over the tile grid."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.grid-dots"));
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
		SurfSession::Verb(TEXT("uv_view"), TEXT("open"));
	}
}

#undef LOCTEXT_NAMESPACE
