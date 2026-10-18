// Copyright MapModelers. All Rights Reserved.
//
// Our own gizmo (S57).
//
// Drawn by the mode in the editor's look - the same arrows, plane squares
// and rings, coloured by axis - at half the editor's size, translucent when
// idle and solid on the handle under the cursor, and all three of move,
// rotate and scale at once rather than one at a time. The editor's own
// widget is not drawn while this is on, but it is still there: SurfGizmo.h
// says how its hover, tracking and drag maths carry our handles, and why a
// ring's turn is worked out here. What a drag does is one transaction
// (StartTracking to EndTracking), noted for the adjust frame when it ends,
// so a gizmo move adjusts like G does.

#include "SurfEdMode.h"
#include "SurfSnapState.h"
#include "SurfGizmo.h"
#include "SurfPreviewMaterial.h"
#include "SurfSettings.h"

#include "SurfInspector.h"
#include "SurfSession.h"
#include "Selection.h"
#include "GameFramework/Actor.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "PrimitiveDrawingUtils.h"
#include "Materials/MaterialInterface.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "UnrealWidget.h"

IMPLEMENT_HIT_PROXY(HSurfGizmoAxis, HWidgetAxis);

namespace
{
	/** The editor's axis colours, so the gizmo reads as the one everyone knows. A plane takes the colour of the axis it is normal to. */
	FLinearColor AxisColour(EAxisList::Type Axis)
	{
		switch (Axis)
		{
		case EAxisList::X:
		case EAxisList::YZ: return FLinearColor(0.594f, 0.0197f, 0.0f);
		case EAxisList::Y:
		case EAxisList::XZ: return FLinearColor(0.1349f, 0.3959f, 0.0f);
		case EAxisList::Z:
		case EAxisList::XY: return FLinearColor(0.0251f, 0.207f, 0.85f);
		default:            return FLinearColor(0.75f, 0.75f, 0.78f);
		}
	}

	SurfPreviewMaterial::ESurfGizmoTint TintOf(EAxisList::Type Axis)
	{
		switch (Axis)
		{
		case EAxisList::X:
		case EAxisList::YZ: return SurfPreviewMaterial::ESurfGizmoTint::X;
		case EAxisList::Y:
		case EAxisList::XZ: return SurfPreviewMaterial::ESurfGizmoTint::Y;
		case EAxisList::Z:
		case EAxisList::XY: return SurfPreviewMaterial::ESurfGizmoTint::Z;
		default:            return SurfPreviewMaterial::ESurfGizmoTint::Neutral;
		}
	}

	FVector AxisDirection(EAxisList::Type Axis)
	{
		switch (Axis)
		{
		case EAxisList::X: return FVector::XAxisVector;
		case EAxisList::Y: return FVector::YAxisVector;
		default:           return FVector::ZAxisVector;
		}
	}

	/** A matrix whose X runs along the axis, for the cone (which DrawCone opens along X). */
	FMatrix AlongAxis(EAxisList::Type Axis)
	{
		switch (Axis)
		{
		case EAxisList::Y: return FRotationMatrix::MakeFromXZ(FVector::YAxisVector, FVector::ZAxisVector);
		case EAxisList::Z: return FRotationMatrix::MakeFromXY(FVector::ZAxisVector, FVector::YAxisVector);
		default:           return FMatrix::Identity;
		}
	}
}

bool USurfEdMode::OwnGizmoOn() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	return Settings && Settings->bShowGizmo && Settings->bOwnGizmo;
}

void USurfEdMode::DrawOwnGizmo(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	// Not while the brush is out either (S247f): a stroke, not a drag.
	if (!View || !PDI || !Viewport || !OwnGizmoOn() || Modal != ESurfModal::None || bPaintArmed)
	{
		return;
	}

	// The show flag that hides the editor's widget hides ours too: turning
	// it off is how the editor looks under a widget for what a click on it
	// was meant for.
	if (View->Family && !View->Family->EngineShowFlags.ModeWidgets)
	{
		return;
	}

	// Only with something to move: the same rule as the editor's widget - and
	// a brush counts, which is why a box had no gizmo at all (S72g).
	const bool bBrushes = PointSelection.Num() == 0 && !SelectedPatch.IsValid() && (Selection.Num() > 0 || ActiveFace.IsValid());

	/**
	 * An actor counts too (S120a): "expecting surfacingpro gizmo ?".
	 *
	 * S93a gave a selected actor the editor's own widget, because ours did
	 * not know how to move one - and then it turned out it does not have to.
	 * Our handles are HSurfGizmoAxis, which **is** an HWidgetAxis, so the
	 * editor reads a press on one as a press on its own widget and its drag
	 * maths moves the actor exactly as before. What was missing was the
	 * drawing, not the moving.
	 *
	 * One gizmo in the viewport, always, whatever is picked. Two tools that
	 * look different depending on what you clicked is how a modelling tool
	 * stops feeling like one tool.
	 */
	const bool bActors = GEditor && GEditor->GetSelectedActorCount() > 0
		&& PointSelection.Num() == 0 && !SelectedPatch.IsValid()
		&& Selection.Num() == 0 && !ActiveFace.IsValid();

	const bool bHasSelection = bBrushes || bActors
		|| (SubMode != ESurfSubMode::Object ? PointSelection.Num() > 0 : SelectedPatch.IsValid());
	if (!bHasSelection)
	{
		return;
	}

	const FEditorViewportClient* Client = static_cast<const FEditorViewportClient*>(Viewport->GetClient());
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Scale = Settings ? Settings->OwnGizmoScale : 0.5;
	const float Idle = Settings ? Settings->OwnGizmoIdleOpacity : 0.4f;
	// On the origin when one is set (S189a, backlog 27): the rings and the
	// cubes turn and size about it, so that is where they are drawn. A move
	// is the same wherever the arrows stand.
	const FVector Location = PivotOr(GetWidgetLocation());

	// The editor's own screen-constant scale, so the gizmo is the same size
	// on the screen wherever it is. Lengths at our share of it; thickness
	// reduced less, so a smaller gizmo is not a fainter one.
	const double Base = View->WorldToScreen(Location).W
		* (4.0 / FMath::Max(1, View->UnscaledViewRect.Width()) / View->ViewMatrices.GetViewToClip().M[0][0]);
	if (!FMath::IsFinite(Base) || Base <= 0.0)
	{
		return;
	}
	const double Unit = Base * Scale;
	const double Thick = Base * FMath::Sqrt(Scale);

	// Which handle the cursor is on, or the one being dragged: the editor
	// keeps both, as its widget mode and axis.
	const UE::Widget::EWidgetMode HotMode = Client ? Client->GetWidgetMode() : UE::Widget::WM_None;
	const EAxisList::Type HotAxis = Client ? Client->GetCurrentWidgetAxis() : EAxisList::None;

	// In a flat view the axis looking at the camera has nothing to show for
	// an arrow: a dot under a ring, over the centre of everything else.
	const bool bPerspective = View->IsPerspectiveProjection();
	const FVector ViewDirection = View->GetViewDirection();
	auto LooksAtCamera = [&](EAxisList::Type Axis)
	{
		return !bPerspective && FMath::Abs(FVector::DotProduct(AxisDirection(Axis), ViewDirection)) > 0.99;
	};

	auto MaterialFor = [&](EAxisList::Type Axis, UE::Widget::EWidgetMode Mode, FLinearColor& OutColour) -> const FMaterialRenderProxy*
	{
		const bool bHot = HotMode == Mode && HotAxis == Axis;
		OutColour = bHot ? FLinearColor(1.0f, 0.85f, 0.1f) : AxisColour(Axis);
		OutColour.A = bHot ? 1.0f : Idle;
		UMaterialInterface* Material = SurfPreviewMaterial::GetGizmoMaterial(
			bHot ? SurfPreviewMaterial::ESurfGizmoTint::Hot : TintOf(Axis), OutColour);
		return Material ? Material->GetRenderProxy() : nullptr;
	};

	// The editor's arrow is 35 units of shaft and 13 of head; ours keep the
	// proportions, and everything else sits where the editor puts it.
	const double Length = 48.0 * Unit;
	const double HeadLength = 12.0 * Unit;
	const double HeadRadius = 4.5 * Thick;
	const double ShaftRadius = 1.2 * Thick;
	const double ShaftFrom = 3.0 * Thick;
	/**
	 * Whether the rotate ball is up, and so whether the handles it would
	 * swallow are drawn at all (S149a): "cant select the plane move on the
	 * gizmo because selecting rotating sphere."
	 *
	 * The ball is a solid sphere round the pivot. The plane squares and the
	 * centre scale cube sit **inside** it, so while it is up they can be
	 * seen and never reached - which is the one thing a handle must not be
	 * (D95). They are not drawn while it is up; hovering an arrow, a ring or
	 * a corner cube - all of which stand clear of it - puts the gizmo back
	 * to that mode and brings them back.
	 */
	const USurfSettings* BallSettings = GetDefault<USurfSettings>();
	/**
	 * Only while a turn is actually the thing (S158c): the cursor on a ring,
	 * or R running. The editor's widget mode stays "rotate" after a ring was
	 * touched, and the ball up on that alone covered the plane squares -
	 * "the transparent rotate sphere prevents me from clicking the side
	 * plane of the gizmo and move!" And barely drawn: a hit region, not a
	 * thing to look at.
	 */
	const bool bBallUp = BallSettings && BallSettings->bGizmoBall
		&& ((HotMode == UE::Widget::WM_Rotate && HotAxis != EAxisList::None) || Modal == ESurfModal::Rotate);

	const double PlaneOffset = 16.0 * Unit;
	const double PlaneHalf = 6.0 * Unit;
	const double RingRadius = 58.0 * Unit;
	const double RingTube = 0.7 * Thick; // "make gizmo ring less thick" (S165a)
	const double CentreHalf = 3.0 * Thick;

	const EAxisList::Type Axes[3] = { EAxisList::X, EAxisList::Y, EAxisList::Z };

	// --- Move: an arrow per axis ---------------------------------------------------
	for (EAxisList::Type Axis : Axes)
	{
		if (LooksAtCamera(Axis))
		{
			continue;
		}
		const FVector Dir = AxisDirection(Axis);
		FLinearColor Colour;
		const FMaterialRenderProxy* Proxy = MaterialFor(Axis, UE::Widget::WM_Translate, Colour);
		PDI->SetHitProxy(new HSurfGizmoAxis(Axis, UE::Widget::WM_Translate));
		if (Proxy)
		{
			DrawCylinder(PDI, Location + Dir * ShaftFrom, Location + Dir * (Length - HeadLength), ShaftRadius, 12, Proxy, SDPG_Foreground);
			const FMatrix Cone = FScaleMatrix(-HeadLength) * FTranslationMatrix(FVector(Length, 0.0, 0.0)) * AlongAxis(Axis) * FTranslationMatrix(Location);
			const float Angle = static_cast<float>(FMath::Atan2(HeadRadius, HeadLength));
			DrawCone(PDI, Cone, Angle, Angle, 24, false, FLinearColor::White, Proxy, SDPG_Foreground);
		}
		else
		{
			// No material (its compilation failed): lines.
			PDI->DrawLine(Location, Location + Dir * Length, Colour, SDPG_Foreground, 2.0f);
		}
		PDI->SetHitProxy(nullptr);
	}

	// --- Move: a plane square per pair of axes, near the centre --------------------
	struct FPlaneHandle
	{
		EAxisList::Type Axis;
		FVector A;
		FVector B;
	};
	const FPlaneHandle Planes[3] = {
		{ EAxisList::XY, FVector::XAxisVector, FVector::YAxisVector },
		{ EAxisList::XZ, FVector::XAxisVector, FVector::ZAxisVector },
		{ EAxisList::YZ, FVector::YAxisVector, FVector::ZAxisVector },
	};
	for (const FPlaneHandle& Plane : Planes)
	{
		if (bBallUp)
		{
			// Inside the ball: it would be drawn and not reachable.
			continue;
		}
		FLinearColor Colour;
		const FMaterialRenderProxy* Proxy = MaterialFor(Plane.Axis, UE::Widget::WM_Translate, Colour);
		const FVector Centre = Location + (Plane.A + Plane.B) * PlaneOffset;
		PDI->SetHitProxy(new HSurfGizmoAxis(Plane.Axis, UE::Widget::WM_Translate));
		if (Proxy)
		{
			const FMatrix ToWorld = FRotationMatrix::MakeFromXY(Plane.A, Plane.B) * FTranslationMatrix(Centre);
			DrawPlane10x10(PDI, ToWorld, static_cast<float>(PlaneHalf), FVector2D::ZeroVector, FVector2D::UnitVector, Proxy, SDPG_Foreground);
		}
		// Its outline, so the square reads where it lies over the model.
		const FVector C0 = Centre - Plane.A * PlaneHalf - Plane.B * PlaneHalf;
		const FVector C1 = Centre + Plane.A * PlaneHalf - Plane.B * PlaneHalf;
		const FVector C2 = Centre + Plane.A * PlaneHalf + Plane.B * PlaneHalf;
		const FVector C3 = Centre - Plane.A * PlaneHalf + Plane.B * PlaneHalf;
		PDI->DrawLine(C0, C1, Colour, SDPG_Foreground);
		PDI->DrawLine(C1, C2, Colour, SDPG_Foreground);
		PDI->DrawLine(C2, C3, Colour, SDPG_Foreground);
		PDI->DrawLine(C3, C0, Colour, SDPG_Foreground);
		PDI->SetHitProxy(nullptr);
	}

	// --- Rotate: a ring per axis --------------------------------------------------
	for (EAxisList::Type Axis : Axes)
	{
		FLinearColor Colour;
		const FMaterialRenderProxy* Proxy = MaterialFor(Axis, UE::Widget::WM_Rotate, Colour);
		const FVector Dir = AxisDirection(Axis);
		const FVector Across = Axis == EAxisList::X ? FVector::YAxisVector : FVector::XAxisVector;
		const FVector Other = FVector::CrossProduct(Dir, Across);
		PDI->SetHitProxy(new HSurfGizmoAxis(Axis, UE::Widget::WM_Rotate));
		if (Proxy)
		{
			DrawTorus(PDI, FTranslationMatrix(Location), Across, Other, RingRadius, RingTube, 48, 6, Proxy, SDPG_Foreground, false, 0.0f, false);
		}
		else
		{
			DrawCircle(PDI, Location, Across, Other, Colour, RingRadius, 48, SDPG_Foreground, 2.0f);
		}
		PDI->SetHitProxy(nullptr);
	}

	/**
	 * --- Rotate: the ball, while rotate is the mode ------------------------
	 *
	 * Armed only then, and that is the whole reason it is safe to make it
	 * the size of the rings. A filled ball with a hit proxy is a disc the
	 * cursor cannot click through, and the pivot sits in the middle of
	 * whatever he has selected - which is exactly where he clicks next. So
	 * it appears when the gizmo is in rotate (the cursor on a ring, or R
	 * pressed) and is gone the moment it is not.
	 *
	 * Slightly smaller than the rings so the rings stay proud of it and keep
	 * their own hit proxies: a turn about one axis is still one grab.
	 */
	if (bBallUp)
	{
		const bool bHot = HotAxis == EAxisList::Screen;
		const float Sheer = FMath::Clamp(Settings->GizmoBallOpacity, 0.0f, 0.6f);
		// As sheer under the hand as at rest (S167b): "gizmo sphere must be
		// much less visible. way more." It is a hit region, not a thing.
		FLinearColor Colour = bHot ? FLinearColor(1.0f, 0.85f, 0.1f) : FLinearColor(0.72f, 0.74f, 0.80f);
		Colour.A = Sheer;
		UMaterialInterface* Material = SurfPreviewMaterial::GetGizmoMaterial(
			bHot ? SurfPreviewMaterial::ESurfGizmoTint::Hot : SurfPreviewMaterial::ESurfGizmoTint::Neutral, Colour);

		PDI->SetHitProxy(new HSurfGizmoAxis(EAxisList::Screen, UE::Widget::WM_Rotate));
		if (Material)
		{
			DrawSphere(PDI, Location, FRotator::ZeroRotator, FVector(RingRadius * SurfGizmo::BallShare),
				28, 14, Material->GetRenderProxy(), SDPG_Foreground);
		}
		else
		{
			// No material: a faint wire, never the bright one it used to be.
			DrawWireSphere(PDI, Location, FLinearColor(Colour.R, Colour.G, Colour.B, FMath::Max(Sheer, 0.02f)), RingRadius * SurfGizmo::BallShare, 24, SDPG_Foreground);
		}
		PDI->SetHitProxy(nullptr);
	}

	/**
	 * --- Scale: one cube at the centre, for all three ------------------------
	 *
	 * The three cubes past the arrows are gone (S163a, A23): "readability,
	 * simple efficient, very few info to read" - his yes to "the axis cubes
	 * go". Sizing along one axis is S then X, Y or Z; the centre cube is
	 * the whole thing at once, and the gizmo is arrows, squares and rings.
	 */
	if (!bBallUp)
	{
		FLinearColor Colour;
		const FMaterialRenderProxy* Proxy = MaterialFor(EAxisList::XYZ, UE::Widget::WM_Scale, Colour);
		PDI->SetHitProxy(new HSurfGizmoAxis(EAxisList::XYZ, UE::Widget::WM_Scale));
		if (Proxy)
		{
			DrawBox(PDI, FTranslationMatrix(Location), FVector(CentreHalf), Proxy, SDPG_Foreground);
		}
		else
		{
			DrawWireBox(PDI, FBox(Location - FVector(CentreHalf), Location + FVector(CentreHalf)), Colour, SDPG_Foreground);
		}
		PDI->SetHitProxy(nullptr);
	}
}

void USurfEdMode::UpdateGizmoHover(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	if (!ViewportClient || !Viewport || !OwnGizmoOn() || Modal != ESurfModal::None || DragTransaction.IsValid())
	{
		return;
	}

	// The editor sets its widget axis from the hit proxy under the cursor
	// (our handles are its kind); the widget mode is ours to set, so a ring
	// means rotate and an arrow means move without a key being pressed.
	HHitProxy* Proxy = Viewport->GetHitProxy(X, Y);
	const bool bOnGizmo = Proxy && Proxy->IsA(HSurfGizmoAxis::StaticGetType());
	if (bOnGizmo)
	{
		const HSurfGizmoAxis* Handle = static_cast<const HSurfGizmoAxis*>(Proxy);
		if (ViewportClient->GetWidgetMode() != Handle->Mode)
		{
			ViewportClient->SetWidgetMode(Handle->Mode);
		}
		// Remembered so leaving can undo it, below.
		bGizmoRotateFromHover = Handle->Mode == UE::Widget::WM_Rotate;
	}
	else if (bGizmoRotateFromHover)
	{
		/**
		 * And rotate ends when the cursor leaves the gizmo (S130a).
		 *
		 * Hovering a ring has always put the viewport in rotate, and nothing
		 * ever put it back - which did not matter while the rotate handles
		 * were three thin rings. The ball is the size of them and filled, so
		 * a mode left in rotate would leave a disc over the middle of the
		 * selection that no click could get through. Rotate entered by the
		 * hand now leaves with it; rotate entered by pressing R stays, which
		 * is the point of pressing R.
		 */
		bGizmoRotateFromHover = false;
		if (ViewportClient->GetWidgetMode() == UE::Widget::WM_Rotate)
		{
			ViewportClient->SetWidgetMode(UE::Widget::WM_Translate);
		}
	}
	if (bOnGizmo != bGizmoHovered)
	{
		bGizmoHovered = bOnGizmo;
		ViewportClient->Invalidate(/*bInvalidateChildViews*/ false, /*bInvalidateHitProxies*/ false);
	}
}

void USurfEdMode::BeginGizmoDrag(FEditorViewportClient* InViewportClient, FViewport* InViewport, bool bShift)
{
	DragTotalDelta = FVector::ZeroVector;
	DragTotalRotation = FRotator::ZeroRotator;
	DragTotalScale = FVector::OneVector;
	bDragScaled = false;
	bDragRotated = false;

	// A Shift-drag is a copy or an extrude with a move on top: more than
	// one thing, so not the one thing the adjust frame does again.
	bDragAdjustable = !bShift;

	GizmoSweep = 0.0;
	GizmoSweepApplied = 0.0;
	bGizmoDragMouseValid = false;
	if (InViewport)
	{
		GizmoDragMouse = FVector2D(InViewport->GetMouseX(), InViewport->GetMouseY());
		bGizmoDragMouseValid = true;
	}

	// The handle under the cursor names the mode, for a press on a handle
	// the cursor reached without moving (the view changed under it, say):
	// the editor takes the axis from the handle at a press; the mode is ours.
	if (OwnGizmoOn() && InViewportClient && InViewport)
	{
		HHitProxy* Proxy = InViewport->GetHitProxy(InViewport->GetMouseX(), InViewport->GetMouseY());
		if (Proxy && Proxy->IsA(HSurfGizmoAxis::StaticGetType()))
		{
			const HSurfGizmoAxis* Handle = static_cast<const HSurfGizmoAxis*>(Proxy);
			if (FEditorModeTools* Tools = GetModeManager())
			{
				if (Tools->GetWidgetMode() != Handle->Mode)
				{
					Tools->SetWidgetMode(Handle->Mode);
				}
			}
		}
	}
}

bool USurfEdMode::GizmoRingRotation(FEditorViewportClient* ViewportClient, FViewport* Viewport, FRotator& OutRotation)
{
	OutRotation = FRotator::ZeroRotator;
	if (!ViewportClient || !Viewport)
	{
		return false;
	}

	const EAxisList::Type Axis = ViewportClient->GetCurrentWidgetAxis();
	const bool bBall = Axis == EAxisList::Screen;
	if (!bBall && Axis != EAxisList::X && Axis != EAxisList::Y && Axis != EAxisList::Z)
	{
		return false;
	}

	// The pivot on screen, and which way the camera is from it.
	FVector2D PivotScreen = FVector2D::ZeroVector;
	FVector ToCamera = FVector::ZeroVector;

	// And, for the ball, the camera's own three directions and how wide the
	// ball is on screen - measured by projecting a point on its rim rather
	// than worked out from the projection, so it is right in an orthographic
	// view too.
	FVector CamRight = FVector::ZeroVector;
	FVector CamUp = FVector::ZeroVector;
	FVector CamForward = FVector::ZeroVector;
	double BallPixels = 0.0;
	{
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
			Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
		FSceneView* View = ViewportClient->CalcSceneView(&Family);
		if (!View)
		{
			return false;
		}
		const FVector Pivot = PivotOr(GetWidgetLocation());
		if (!View->WorldToPixel(Pivot, PivotScreen))
		{
			return false;
		}
		ToCamera = View->IsPerspectiveProjection() ? (View->ViewMatrices.GetViewOrigin() - Pivot) : -View->GetViewDirection();

		if (bBall)
		{
			const FMatrix FromView = View->ViewMatrices.GetViewToWorld();
			CamForward = FromView.GetUnitAxis(EAxis::X);
			CamRight = FromView.GetUnitAxis(EAxis::Y);
			CamUp = FromView.GetUnitAxis(EAxis::Z);

			const USurfSettings* Settings = GetDefault<USurfSettings>();
			const double Base = View->WorldToScreen(Pivot).W
				* (4.0 / FMath::Max(1, View->UnscaledViewRect.Width()) / View->ViewMatrices.GetViewToClip().M[0][0]);
			if (!FMath::IsFinite(Base) || Base <= 0.0)
			{
				return false;
			}
			const double Radius = 58.0 * Base * (Settings ? Settings->OwnGizmoScale : 0.5) * SurfGizmo::BallShare;

			FVector2D RimScreen = FVector2D::ZeroVector;
			if (!View->WorldToPixel(Pivot + CamRight * Radius, RimScreen))
			{
				return false;
			}
			BallPixels = FVector2D::Distance(RimScreen, PivotScreen);
			if (BallPixels < 4.0)
			{
				return false;
			}
		}
	}

	const FVector2D Mouse(Viewport->GetMouseX(), Viewport->GetMouseY());
	if (!bGizmoDragMouseValid)
	{
		GizmoDragMouse = Mouse;
		bGizmoDragMouseValid = true;
		return false;
	}

	/**
	 * The ball: the turn that carries the point grabbed to the point under
	 * the cursor now (S130a).
	 *
	 * Applied as it comes, frame by frame, rather than accumulated the way a
	 * ring's sweep is - turns about different axes compose, so there is
	 * nothing to add up. It is not snapped either: there is no sensible
	 * grid for a turn about an axis nobody named, and that is what X, Y or
	 * Z during the drag is for.
	 */
	if (bBall)
	{
		const FVector From = SurfGizmo::BallPoint(GizmoDragMouse - PivotScreen, BallPixels, CamRight, CamUp, CamForward);
		const FVector To = SurfGizmo::BallPoint(Mouse - PivotScreen, BallPixels, CamRight, CamUp, CamForward);
		GizmoDragMouse = Mouse;

		OutRotation = SurfGizmo::BallRotation(From, To);
		return !OutRotation.IsNearlyZero();
	}

	GizmoSweep += SurfGizmo::SweptRadians(GizmoDragMouse - PivotScreen, Mouse - PivotScreen);
	GizmoDragMouse = Mouse;

	// Snapped as the editor's rings snap: to the rotation grid on its toolbar.
	double Degrees = FMath::RadiansToDegrees(GizmoSweep);
	const ULevelEditorViewportSettings* ViewportSettings = GetDefault<ULevelEditorViewportSettings>();
	if (GEditor && ViewportSettings && SurfSnapState::Flipped(ViewportSettings->RotGridEnabled))
	{
		const double Step = GEditor->GetRotGridSize().Yaw;
		if (Step > 0.01)
		{
			Degrees = FMath::GridSnap(Degrees, Step);
		}
	}

	// Only what has not been applied yet: the points turn a little more each
	// frame, and it has to add up to the sweep.
	const double Turn = Degrees - GizmoSweepApplied;
	if (FMath::Abs(Turn) < 1.e-3)
	{
		return false;
	}
	GizmoSweepApplied = Degrees;
	OutRotation = SurfGizmo::RingRotation(AxisDirection(Axis), ToCamera, Turn);
	return true;
}

/**
 * X, Y or Z while the ball is turning (S130a): "Its still possible to follow
 * some 'main axis' (where the rings were) and snap with x y z."
 *
 * The same key again lets it go, back to the free turn - so the two are one
 * press apart in both directions, which is how Blender's axis locks behave
 * and the only way a lock is usable without looking at anything.
 *
 * Locking hands the drag to the ring maths, which means the rotation grid
 * applies from that moment: an axis is a thing you can snap about, and a
 * turn about nothing in particular is not.
 */
bool USurfEdMode::LockGizmoBallToAxis(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FKey& Key)
{
	if (!ViewportClient || !Viewport || !OwnGizmoOn() || Modal != ESurfModal::None)
	{
		return false;
	}
	if (ViewportClient->GetWidgetMode() != UE::Widget::WM_Rotate || !Viewport->KeyState(EKeys::LeftMouseButton))
	{
		return false;
	}

	EAxisList::Type Wanted = EAxisList::None;
	if (Key == EKeys::X) { Wanted = EAxisList::X; }
	else if (Key == EKeys::Y) { Wanted = EAxisList::Y; }
	else if (Key == EKeys::Z) { Wanted = EAxisList::Z; }
	if (Wanted == EAxisList::None)
	{
		return false;
	}

	const EAxisList::Type Now = ViewportClient->GetCurrentWidgetAxis();
	if (Now != EAxisList::Screen && Now != Wanted)
	{
		// A ring was grabbed, not the ball: it is already one axis and the
		// key would be answering a question nobody asked.
		return false;
	}

	const bool bLetGo = Now == Wanted;
	ViewportClient->SetCurrentWidgetAxis(bLetGo ? EAxisList::Screen : Wanted);

	// The ring maths starts from where the cursor is now, not from where the
	// ball drag began, or the selection jumps at the moment of the press.
	GizmoSweep = 0.0;
	GizmoSweepApplied = 0.0;
	bGizmoDragMouseValid = false;

	if (Inspector)
	{
		Inspector->SetResult(bLetGo
			? FString(TEXT("turning freely again - X, Y or Z locks to one axis"))
			: FString::Printf(TEXT("turning about %s only - the same key again lets it go"),
				Wanted == EAxisList::X ? TEXT("X") : (Wanted == EAxisList::Y ? TEXT("Y") : TEXT("Z"))));
	}
	SurfSession::Verb(TEXT("gizmo_ball_axis"), bLetGo ? TEXT("free") :
		(Wanted == EAxisList::X ? TEXT("X") : (Wanted == EAxisList::Y ? TEXT("Y") : TEXT("Z"))));
	return true;
}

int32 USurfEdMode::RotateSelectedActorsAbout(const FRotator& Turn, const FVector& Pivot)
{
	if (!GEditor || Turn.IsNearlyZero())
	{
		return 0;
	}

	TArray<AActor*> Chosen;
	if (USelection* Selected = GEditor->GetSelectedActors())
	{
		Selected->GetSelectedObjects<AActor>(Chosen);
	}

	const FQuat Spin = Turn.Quaternion();
	int32 Turned = 0;
	for (AActor* Actor : Chosen)
	{
		if (!Actor)
		{
			continue;
		}

		// Where it stands goes round the pivot, and which way it faces goes
		// round with it - both, or a row of pillars turns on the spot
		// instead of swinging round the middle of the row.
		Actor->Modify();
		FTransform Where = Actor->GetActorTransform();
		Where.SetLocation(Pivot + Spin.RotateVector(Where.GetLocation() - Pivot));
		Where.SetRotation(Spin * Where.GetRotation());
		Actor->SetActorTransform(Where);
		Actor->PostEditMove(/*bFinished*/ false);
		++Turned;
	}
	return Turned;
}

bool USurfEdMode::BrushGizmoDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale)
{
	// Alt is the camera, whatever the drag started on: the same rule the
	// point drag follows, and for the same reason.
	const bool bAlt = InViewport && (InViewport->KeyState(EKeys::LeftAlt) || InViewport->KeyState(EKeys::RightAlt));
	if (bAlt)
	{
		return false;
	}

	// No axis under the cursor means this is the camera, not the gizmo.
	if (!InViewportClient || InViewportClient->GetCurrentWidgetAxis() == EAxisList::None)
	{
		return false;
	}

	// The brush's middle as it stands now: it moves with the brush, so the
	// turn and the size stay about the middle through the whole drag.
	FVector Pivot = BrushDragPivot;
	if (!BrushSelectionPivot(Pivot))
	{
		Pivot = BrushDragPivot;
	}

	// Whatever the editor's grid says, since the delta is already snapped by
	// it: "box is snapping on move rotate etc even if snap is off in ue?".
	if (!InDrag.IsNearlyZero())
	{
		if (bBrushDragPushesFaces)
		{
			PushSelectedFacesAlong(InDrag);
		}
		else
		{
			TransformSelectedBrushes(FTransform(InDrag), Pivot);
		}
		DragMovedCm += InDrag.Size();
		BrushDragMoved += InDrag;
	}

	// A ring of ours turns by the sweep of the cursor about the pivot; the
	// editor's rotate maths reads its own rings, which are not drawn while
	// ours are (SurfGizmo.h).
	FRotator Rotation = InRot;
	if (OwnGizmoOn() && InViewportClient->GetWidgetMode() == UE::Widget::WM_Rotate)
	{
		if (!GizmoRingRotation(InViewportClient, InViewport, Rotation))
		{
			Rotation = FRotator::ZeroRotator;
		}
	}
	if (!Rotation.IsNearlyZero())
	{
		TransformSelectedBrushes(FTransform(Rotation.Quaternion()), PivotOr(Pivot));
	}

	if (!InScale.IsNearlyZero())
	{
		// Unreal hands scale as a per-drag increment, not a factor.
		const FVector Factor = FVector::OneVector + InScale * 0.01;

		// With sides picked in face sub-mode, the sides are what is sized -
		// the same rule the move above follows (S141a).
		// No sub-mode test: sizing every side about the middle is sizing the
		// brush, so the two agree wherever they overlap (S145a).
		if (PickedFaces().Num() > 0)
		{
			ScaleSelectedFacesAbout(Factor, PivotOr(Pivot));
		}
		else
		{
			TransformSelectedBrushes(FTransform(FQuat::Identity, FVector::ZeroVector, Factor), PivotOr(Pivot));
		}
	}

	// Handled whether or not this frame's delta came to anything: a delta
	// left unhandled with an axis under the cursor is applied by the editor
	// to the selected actors - the group - instead.
	return true;
}

void USurfEdMode::NoteGizmoDrag()
{
	if (!bDragAdjustable)
	{
		return;
	}

	// What the drag did, with its numbers, so the adjust frame can do it
	// again with other numbers - as it does after G, R and S.
	FSurfOperation Op;
	if (bDragRotated)
	{
		Op.Kind = ESurfOpKind::Rotate;
		Op.Rotation = DragTotalRotation;
	}
	else if (bDragScaled)
	{
		Op.Kind = ESurfOpKind::Scale;
		Op.Scale = DragTotalScale;
	}
	else if (!DragTotalDelta.IsNearlyZero(0.01))
	{
		Op.Kind = ESurfOpKind::Move;
		Op.Delta = DragTotalDelta;
	}

	if (Op.Kind != ESurfOpKind::None)
	{
		NoteOperation(Op);
	}
}
