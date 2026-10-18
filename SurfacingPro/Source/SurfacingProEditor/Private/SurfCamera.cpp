// Copyright MapModelers. All Rights Reserved.

#include "SurfCamera.h"

#include "Containers/Ticker.h"
#include "EditorViewportClient.h"
#include "UnrealWidget.h"

namespace
{
	/** Blender's own wheel step, from its source: exactly 1.2x per click. */
	constexpr double BlenderWheelStep = 1.2;

	/**
	 * Just short of the pole. Blender itself does not clamp at all - it orbits
	 * over the top freely - but Unreal's camera is a rotator with no roll, so
	 * passing through vertical flips the view rather than continuing. This is
	 * the one place the two models cannot be reconciled without owning the
	 * camera outright.
	 */
	constexpr double MaxPitch = 89.99;

	/** Blender's U.smooth_viewtx, in seconds. */
	constexpr double SmoothViewSeconds = 0.2;

	const USurfSettings* Settings()
	{
		return GetDefault<USurfSettings>();
	}
}

bool FSurfCamera::IsFlatPane(const FEditorViewportClient* ViewportClient)
{
	return ViewportClient && !ViewportClient->IsPerspective() && ViewportClient->GetViewportType() != LVT_OrthoFreelook;
}

FVector FSurfCamera::ViewDirectionOf(const FEditorViewportClient* ViewportClient)
{
	if (!ViewportClient)
	{
		return FVector::ForwardVector;
	}

	// The editor's fixed views, from the matrices it draws them with.
	switch (ViewportClient->GetViewportType())
	{
	case LVT_OrthoXY:         return -FVector::ZAxisVector;
	case LVT_OrthoNegativeXY: return  FVector::ZAxisVector;
	case LVT_OrthoXZ:         return -FVector::YAxisVector;
	case LVT_OrthoNegativeXZ: return  FVector::YAxisVector;
	case LVT_OrthoYZ:         return  FVector::XAxisVector;
	case LVT_OrthoNegativeYZ: return -FVector::XAxisVector;
	default:                  return ViewportClient->GetViewRotation().Vector();
	}
}

ELevelViewportType FSurfCamera::FlatViewFor(const FVector& Direction)
{
	const FVector D = Direction.GetSafeNormal();
	const double AX = FMath::Abs(D.X);
	const double AY = FMath::Abs(D.Y);
	const double AZ = FMath::Abs(D.Z);

	if (AZ >= AX && AZ >= AY)
	{
		return D.Z < 0.0 ? LVT_OrthoXY : LVT_OrthoNegativeXY;
	}
	if (AY >= AX)
	{
		return D.Y < 0.0 ? LVT_OrthoXZ : LVT_OrthoNegativeXZ;
	}
	return D.X > 0.0 ? LVT_OrthoYZ : LVT_OrthoNegativeYZ;
}

FSurfCamera::~FSurfCamera()
{
	// A smooth view still in flight must not tick into a camera that is gone.
	if (SmoothTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SmoothTicker);
		SmoothTicker.Reset();
	}
}

bool FSurfCamera::IsBlenderScheme()
{
	const USurfSettings* S = Settings();
	if (!S)
	{
		return false;
	}

	switch (S->CameraScheme)
	{
	case ESurfCameraScheme::Blender: return true;
	case ESurfCameraScheme::Unreal:  return false;
	default:                         return S->KeyScheme == ESurfKeyScheme::Blender;
	}
}

void FSurfCamera::Write(FEditorViewportClient* ViewportClient, const FVector& Location, const FRotator& Rotation, const FVector& LookAt)
{
	if (!ViewportClient)
	{
		return;
	}

	ViewportClient->SetViewLocation(Location);
	ViewportClient->SetViewRotation(Rotation);
	ViewportClient->SetLookAtLocation(LookAt);

	// Redraw, but leave the hit proxies alone while a drag is running: an
	// invalidated hit-proxy map is rendered again - the whole scene, once
	// more - the next time the cursor asks what it is over, which during an
	// orbit was every mouse move. They are refreshed once, when the drag ends.
	ViewportClient->Invalidate(/*bInvalidateChildViews*/ false, /*bInvalidateHitProxies*/ Drag == EDrag::None);

	// Remember what we wrote, so the next call can tell our own change from
	// somebody else's.
	LastWrittenLocation = Location;
	LastWrittenRotation = Rotation;
}

void FSurfCamera::ReconcileWithViewport(FEditorViewportClient* ViewportClient)
{
	if (!ViewportClient)
	{
		return;
	}

	const FVector Location = ViewportClient->GetViewLocation();
	const FRotator Rotation = ViewportClient->GetViewRotation();

	// Somebody else moved the camera - focus, a bookmark, Alt-drag, the fly
	// camera. Rebuild the view pivot from where it is now rather than yanking
	// the view back to a stale one on the next orbit, which is exactly what
	// "the addon is fighting the editor" looks like from the outside.
	if (!bHasPivot || !Location.Equals(LastWrittenLocation, 0.1) || !Rotation.Equals(LastWrittenRotation, 0.01f))
	{
		if (!bHasPivot)
		{
			// First time: take the point the view is already centred on,
			// which is what Blender orbits around by default, at whatever
			// distance the editor was already using.
			const double Existing = FVector::Dist(Location, ViewportClient->GetLookAtLocation());
			Distance = Existing > 1.0 ? Existing : 500.0;
		}

		Pivot = Location + Rotation.Vector() * Distance;
		bHasPivot = true;
		LastWrittenLocation = Location;
		LastWrittenRotation = Rotation;
	}
}

FVector FSurfCamera::ResolveCentre(FEditorViewportClient* ViewportClient)
{
	ReconcileWithViewport(ViewportClient);

	// The selection, when there is one and the setting says so; otherwise the
	// point ahead of the camera. This is Blender's Orbit Around Selection,
	// which it ships off and which was asked for on.
	const bool bUseSelection = bHasSelectionPivot && Settings() && Settings()->bOrbitAroundSelection;
	return bUseSelection ? SelectionPivot : Pivot;
}

void FSurfCamera::Orbit(FEditorViewportClient* ViewportClient, double DeltaYaw, double DeltaPitch, bool bAroundSelection)
{
	if (!ViewportClient)
	{
		return;
	}

	ReconcileWithViewport(ViewportClient);

	const FVector Centre = Drag == EDrag::Orbit ? DragCentre
		: (bAroundSelection && bHasSelectionPivot) ? SelectionPivot : ResolveCentre(ViewportClient);

	const FVector Location = ViewportClient->GetViewLocation();
	FRotator Rotation = ViewportClient->GetViewRotation();

	// Pitch is clamped before anything is composed, so the position and the
	// direction are turned by exactly the same amount and cannot disagree at
	// the pole.
	const double AllowedPitch = FMath::Clamp(Rotation.Pitch + DeltaPitch, -MaxPitch, MaxPitch) - Rotation.Pitch;

	// Turntable, not trackball: yaw about the world's up axis and pitch about
	// the camera's right. Pitch first, then yaw, both in world space - the
	// same order Blender composes them - and roll is never touched, which is
	// why a Blender view stays level however long you orbit.
	const FVector Right = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Y);
	const FQuat QPitch(Right, FMath::DegreesToRadians(-AllowedPitch));
	const FQuat QYaw(FVector::UpVector, FMath::DegreesToRadians(DeltaYaw));
	const FQuat Turn = QYaw * QPitch;

	// The camera's position swings about the centre and the camera turns by
	// the same amount, so whatever was under the cursor stays under it.
	const FVector NewLocation = Centre + Turn.RotateVector(Location - Centre);

	Rotation.Yaw += DeltaYaw;
	Rotation.Pitch += AllowedPitch;
	Rotation.Roll = 0.0;

	Write(ViewportClient, NewLocation, Rotation, Centre);

	Pivot = Centre;
	Distance = FMath::Max(1.0, FVector::Dist(NewLocation, Centre));
}

void FSurfCamera::Zoom(FEditorViewportClient* ViewportClient, double Factor)
{
	if (!ViewportClient)
	{
		return;
	}

	const FVector Centre = ResolveCentre(ViewportClient);
	const FVector Location = ViewportClient->GetViewLocation();

	// Towards or away from the centre, never all the way: a point you have
	// arrived at cannot be orbited around or backed away from.
	const FVector Offset = Location - Centre;
	const double Now = Offset.Size();
	const double Wanted = FMath::Clamp(Now * Factor, 1.0, 1.0e7);

	const FVector NewLocation = Now > KINDA_SMALL_NUMBER
		? Centre + Offset * (Wanted / Now)
		: Location;

	Write(ViewportClient, NewLocation, ViewportClient->GetViewRotation(), Centre);

	Pivot = Centre;
	Distance = Wanted;
}

void FSurfCamera::Slide(FEditorViewportClient* ViewportClient, const FVector& Delta)
{
	if (!ViewportClient)
	{
		return;
	}

	ReconcileWithViewport(ViewportClient);

	// Camera and pivot together: the view moves sideways and keeps looking
	// at the same distance ahead. A pan that only moved the pivot re-aimed
	// the camera at it, which is not what panning is.
	const FVector NewLocation = ViewportClient->GetViewLocation() + Delta;
	Pivot += Delta;

	Write(ViewportClient, NewLocation, ViewportClient->GetViewRotation(), Pivot);
}

bool FSurfCamera::TickSmoothView(float DeltaSeconds)
{
	if (!SmoothClient || SmoothDuration <= 0.0)
	{
		return false;
	}

	SmoothElapsed += DeltaSeconds;
	const double T = FMath::Clamp(SmoothElapsed / SmoothDuration, 0.0, 1.0);

	// Blender's ease: smoothstep, 3t^2 - 2t^3.
	const double Eased = T * T * (3.0 - 2.0 * T);

	const FVector Location = FMath::Lerp(SmoothFromLocation, SmoothToLocation, Eased);
	const FQuat Rotation = FQuat::Slerp(SmoothFromRotation, SmoothToRotation, static_cast<float>(Eased));

	Write(SmoothClient, Location, Rotation.Rotator(), SmoothLookAt);

	if (T >= 1.0)
	{
		SmoothClient = nullptr;
		SmoothDuration = 0.0;
		return false;
	}

	return true;
}

void FSurfCamera::SnapToDirection(FEditorViewportClient* ViewportClient, const FVector& Direction)
{
	if (!Direction.IsNearlyZero())
	{
		// Straight down and straight up are a degenerate rotator - a hair off
		// keeps the roll the turntable expects, as the numpad views do.
		FVector Safe = Direction.GetSafeNormal();
		if (FMath::Abs(Safe.Z) > 0.999)
		{
			SnapTo(ViewportClient, FRotator(Safe.Z > 0.0 ? 89.9 : -89.9, 0.0, 0.0));
			return;
		}
		SnapTo(ViewportClient, Safe.Rotation());
	}
}

void FSurfCamera::SnapToNearestAxis(FEditorViewportClient* ViewportClient)
{
	if (!ViewportClient)
	{
		return;
	}
	// Whichever way it is mostly looking already: the view straightens
	// rather than jumping somewhere else, which is the whole point of the
	// gesture - one hand movement and the wall is square on.
	const FVector Looking = ViewDirectionOf(ViewportClient);
	const FVector Abs(FMath::Abs(Looking.X), FMath::Abs(Looking.Y), FMath::Abs(Looking.Z));
	FVector Axis = FVector::ForwardVector * FMath::Sign(Looking.X);
	if (Abs.Y >= Abs.X && Abs.Y >= Abs.Z)
	{
		Axis = FVector::RightVector * FMath::Sign(Looking.Y);
	}
	else if (Abs.Z >= Abs.X && Abs.Z >= Abs.Y)
	{
		Axis = FVector::UpVector * FMath::Sign(Looking.Z);
	}
	if (Axis.IsNearlyZero())
	{
		Axis = FVector::ForwardVector;
	}
	SnapToDirection(ViewportClient, Axis);
}

void FSurfCamera::SnapTo(FEditorViewportClient* ViewportClient, const FRotator& Rotation)
{
	if (!ViewportClient)
	{
		return;
	}

	// A flat pane turns by becoming another of the editor's fixed views:
	// its rotation is never read, its side is.
	if (IsFlatPane(ViewportClient))
	{
		ViewportClient->SetViewportType(FlatViewFor(Rotation.Vector()));
		ViewportClient->Invalidate();
		return;
	}

	const FVector Centre = ResolveCentre(ViewportClient);
	const double Radius = FMath::Max(1.0, FVector::Dist(ViewportClient->GetViewLocation(), Centre));

	// A snap re-aims at the centre from the new direction, at the same
	// distance: Blender keeps ofs and dist and replaces viewquat.
	const FVector Target = Centre - Rotation.Vector() * Radius;

	SmoothClient = ViewportClient;
	SmoothFromLocation = ViewportClient->GetViewLocation();
	SmoothToLocation = Target;
	SmoothFromRotation = ViewportClient->GetViewRotation().Quaternion();
	SmoothToRotation = Rotation.Quaternion();
	SmoothLookAt = Centre;
	SmoothElapsed = 0.0;

	// Scaled by how far there is to turn, as Blender does for rotation-only
	// changes: a quarter turn takes half as long as a half turn.
	const double Angle = SmoothFromRotation.AngularDistance(SmoothToRotation);
	SmoothDuration = SmoothViewSeconds * FMath::Clamp(Angle / PI, 0.25, 1.0);

	Pivot = Centre;
	Distance = Radius;

	if (!SmoothTicker.IsValid())
	{
		SmoothTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float DeltaSeconds) -> bool
			{
				const bool bKeepGoing = TickSmoothView(DeltaSeconds);
				if (!bKeepGoing)
				{
					SmoothTicker.Reset();
				}
				return bKeepGoing;
			}));
	}
}

void FSurfCamera::Frame(FEditorViewportClient* ViewportClient, const FBox& Bounds)
{
	if (!ViewportClient || !Bounds.IsValid)
	{
		return;
	}

	// A flat pane frames by the editor's own means: its centre and its zoom.
	if (!ViewportClient->IsPerspective())
	{
		ViewportClient->FocusViewportOnBox(Bounds, /*bInstant*/ true);
		Pivot = Bounds.GetCenter();
		bHasPivot = true;
		return;
	}

	const FRotator Rotation = ViewportClient->GetViewRotation();

	Pivot = Bounds.GetCenter();
	bHasPivot = true;
	Distance = FMath::Max(100.0, Bounds.GetExtent().Size() * 2.5);

	Write(ViewportClient, Pivot - Rotation.Vector() * Distance, Rotation, Pivot);
}

bool FSurfCamera::HandleKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FKey& Key, EInputEvent Event)
{
	if (!IsBlenderScheme() || !ViewportClient || !Viewport)
	{
		return false;
	}

	const bool bShift = Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift);
	const bool bCtrl  = Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl);

	// A flat pane (top, front, side) keeps the editor's own navigation: the
	// wheel zooms the plane and the middle button pans it, where ours would
	// have moved a camera the pane never reads (the wheel in a flat pane did
	// nothing). The number keys still pick which side it shows.
	if (IsFlatPane(ViewportClient)
		&& Key != EKeys::NumPadOne && Key != EKeys::NumPadThree && Key != EKeys::NumPadSeven && Key != EKeys::NumPadFive)
	{
		return false;
	}

	if (Key == EKeys::MiddleMouseButton)
	{
		const bool bAlt = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);

		// Alt with the middle button straightens the view to the axis it is
		// nearest (S71e) - Blender's own gesture, and the one the author asked
		// for: "snapping active camera to a view, while being free to also
		// just keep moving". Claimed, so no orbit begins under it.
		if (Event == IE_Pressed && bAlt)
		{
			SnapToNearestAxis(ViewportClient);
			return true;
		}
		if (Event == IE_Released && bAlt && Drag == EDrag::None)
		{
			return true;
		}

		if (Event == IE_Pressed)
		{
			BeginDrag(ViewportClient, Viewport, bShift ? EDrag::Pan : (bCtrl ? EDrag::Dolly : EDrag::Orbit), Key);

			// Deliberately not claimed: letting the editor capture the mouse is
			// what keeps the cursor in the viewport for the length of a drag.
			// The moves are claimed instead, so its camera never sees them.
			return false;
		}

		if (Event == IE_Released)
		{
			Drag = EDrag::None;
			bSnappingOrbit = false;
			return false;
		}
	}

	if (Event != IE_Pressed)
	{
		return false;
	}

	const USurfSettings* S = Settings();
	const double Turn = S ? S->NumpadOrbitDegrees : 15.0;

	if (Key == EKeys::MouseScrollUp)
	{
		Zoom(ViewportClient, 1.0 / BlenderWheelStep);
		return true;
	}

	if (Key == EKeys::MouseScrollDown)
	{
		Zoom(ViewportClient, BlenderWheelStep);
		return true;
	}

	// The standard views. Ctrl gives the opposite side, as it does in Blender.
	if (Key == EKeys::NumPadOne)   { SnapTo(ViewportClient, FRotator(0.0, bCtrl ? -90.0 : 90.0, 0.0)); return true; }
	if (Key == EKeys::NumPadThree) { SnapTo(ViewportClient, FRotator(0.0, bCtrl ? 0.0 : 180.0, 0.0)); return true; }
	if (Key == EKeys::NumPadSeven) { SnapTo(ViewportClient, FRotator(bCtrl ? 89.9 : -89.9, 0.0, 0.0)); return true; }

	// About the selection when there is one (S174a): "it should allow
	// rotating around selection" - the numpad turns what is being worked
	// on, whatever the mouse orbit's own setting.
	if (Key == EKeys::NumPadFour)  { Orbit(ViewportClient, -Turn, 0.0, true); return true; }
	if (Key == EKeys::NumPadSix)   { Orbit(ViewportClient,  Turn, 0.0, true); return true; }
	if (Key == EKeys::NumPadEight) { Orbit(ViewportClient, 0.0,  Turn, true); return true; }
	if (Key == EKeys::NumPadTwo)   { Orbit(ViewportClient, 0.0, -Turn, true); return true; }
	if (Key == EKeys::NumPadNine)  { Orbit(ViewportClient, 180.0, 0.0, true); return true; }

	if (Key == EKeys::NumPadFive)
	{
		// Orthographic and back, keeping where you were looking from.
		const bool bPerspective = ViewportClient->IsPerspective();
		ViewportClient->SetViewportType(bPerspective ? LVT_OrthoFreelook : LVT_Perspective);
		ViewportClient->Invalidate();
		return true;
	}

	return false;
}

void FSurfCamera::BeginDrag(FEditorViewportClient* ViewportClient, FViewport* Viewport, EDrag InDrag, const FKey& Button)
{
	if (!ViewportClient || !Viewport)
	{
		return;
	}

	Drag = InDrag;
	DragButton = Button;
	LastMouse = FIntPoint(Viewport->GetMouseX(), Viewport->GetMouseY());
	bAwaitingFirstMove = true;

	// The centre for the whole drag, taken now: a selection that changes
	// mid-drag must not move the centre under the hand. What is under the
	// cursor wins - an orbit turns about the surface you are pointing at -
	// then the selection if that is asked for, then the view's own pivot.
	DragCentre = ResolveCentre(ViewportClient);
	FVector Under = FVector::ZeroVector;
	if (InDrag == EDrag::Orbit && Settings() && Settings()->bOrbitUnderCursor && UnderCursor && UnderCursor(ViewportClient, Viewport, Under))
	{
		DragCentre = Under;
	}

	// Latched now rather than per event: upside down, a horizontal drag has
	// to reverse so the scene still follows the hand, but flipping halfway
	// through a drag is worse than either. Blender latches the same value
	// for the same reason.
	YawSign = FMath::Abs(ViewportClient->GetViewRotation().Pitch) > 90.0 ? -1.0 : 1.0;

	// Pan speed latched too - Blender's zfac - so a pan is linear for its
	// whole length even as the pivot moves.
	const double Radius = FMath::Max(1.0, FVector::Dist(ViewportClient->GetViewLocation(), DragCentre));
	PanPerPixel = (2.0 * Radius * FMath::Tan(FMath::DegreesToRadians(ViewportClient->ViewFOV * 0.5f)))
		/ FMath::Max(1, Viewport->GetSizeXY().Y);
}

void FSurfCamera::BeginAltOrbit(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// Alt with the left button, for a hand that reaches for the Unreal
	// gesture: the same relative orbit as the middle button, about the same
	// centre, and never the editor's own orbit - which swings the camera to
	// centre its look-at point the moment the drag starts, and read as the
	// view snapping.
	BeginDrag(ViewportClient, Viewport, EDrag::Orbit, EKeys::LeftMouseButton);
}

void FSurfCamera::AimAt(FEditorViewportClient* ViewportClient, const FVector& Centre, const FRotator& Rotation)
{
	if (!ViewportClient)
	{
		return;
	}
	const double Radius = FMath::Max(1.0, FVector::Dist(ViewportClient->GetViewLocation(), Centre));
	Write(ViewportClient, Centre - Rotation.Vector() * Radius, Rotation, Centre);
	Pivot = Centre;
	Distance = Radius;
}

void FSurfCamera::EndDrag(FEditorViewportClient* ViewportClient)
{
	const bool bWasDragging = Drag != EDrag::None;
	Drag = EDrag::None;
	bSnappingOrbit = false;

	// The view has moved: what is under the cursor has too.
	if (bWasDragging && ViewportClient)
	{
		ViewportClient->Invalidate();
	}
}

bool FSurfCamera::HandleCapturedMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	if (Drag == EDrag::None || !IsBlenderScheme() || !ViewportClient || !Viewport)
	{
		return false;
	}

	// The button going up can be missed while the editor holds capture,
	// which would leave the camera stuck to the mouse.
	if (!Viewport->KeyState(DragButton))
	{
		Drag = EDrag::None;
		bSnappingOrbit = false;
		return false;
	}

	// The first position reported after the editor takes the mouse is not
	// always where the press was measured: once the cursor is hidden and
	// locked the positions can arrive from another origin, and a delta from
	// the press to the first of them was the small jump at the start of every
	// orbit. The first move only says where the drag starts from.
	if (bAwaitingFirstMove)
	{
		bAwaitingFirstMove = false;
		LastMouse = FIntPoint(X, Y);
		return true;
	}

	const double DeltaX = X - LastMouse.X;
	const double DeltaY = Y - LastMouse.Y;
	LastMouse = FIntPoint(X, Y);

	if (FMath::IsNearlyZero(DeltaX) && FMath::IsNearlyZero(DeltaY))
	{
		return true;
	}

	const USurfSettings* S = Settings();
	const double Sensitivity = S ? S->OrbitSensitivity : 0.4;

	switch (Drag)
	{
	case EDrag::Orbit:
	{
		// Dragging right turns the view rightward around the centre, and
		// dragging up tips it to look from below: the scene follows the hand.
		const double SignX = (S && S->bInvertOrbitX) ? -1.0 : 1.0;
		const double SignY = (S && S->bInvertOrbitY) ? -1.0 : 1.0;
		const double DeltaYaw = -DeltaX * Sensitivity * SignX * YawSign;
		const double DeltaPitch = DeltaY * Sensitivity * SignY;

		// Alt with the middle button: the view snaps to whichever of the
		// six axis views the hand has turned nearest to - Blender's
		// Alt-MMB. (Alt with the left button is the plain orbit, for the
		// hand that reaches for the editor's chord.)
		const bool bAlt = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
		if (bAlt && DragButton == EKeys::MiddleMouseButton)
		{
			ReconcileWithViewport(ViewportClient);
			if (!bSnappingOrbit)
			{
				bSnappingOrbit = true;
				SnapFreeRotation = ViewportClient->GetViewRotation();
				SnapLastApplied = FRotator(FLT_MAX, FLT_MAX, 0.0);
			}
			SnapFreeRotation.Yaw = FRotator::NormalizeAxis(SnapFreeRotation.Yaw + DeltaYaw);
			SnapFreeRotation.Pitch = FMath::Clamp(SnapFreeRotation.Pitch + DeltaPitch, -MaxPitch, MaxPitch);

			FRotator Snapped;
			Snapped.Pitch = FMath::Abs(SnapFreeRotation.Pitch) > 45.0 ? (SnapFreeRotation.Pitch > 0.0 ? 89.0 : -89.0) : 0.0;
			Snapped.Yaw = FRotator::NormalizeAxis(FMath::RoundToDouble(SnapFreeRotation.Yaw / 90.0) * 90.0);
			Snapped.Roll = 0.0;
			if (!Snapped.Equals(SnapLastApplied, 0.01))
			{
				SnapLastApplied = Snapped;
				AimAt(ViewportClient, DragCentre, Snapped);
			}
			break;
		}
		if (bSnappingOrbit)
		{
			// Alt let go mid-drag: free again from where the snap left it.
			bSnappingOrbit = false;
		}

		Orbit(ViewportClient, DeltaYaw, DeltaPitch);
		break;
	}

	case EDrag::Pan:
	{
		const FRotator Rotation = ViewportClient->GetViewRotation();
		const FVector Right = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Y);
		const FVector Up = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Z);

		Slide(ViewportClient, (-Right * DeltaX + Up * DeltaY) * PanPerPixel);
		break;
	}

	case EDrag::Dolly:
	{
		// Vertical only, scaled by the radius so it covers the same fraction
		// of what you can see whether the centre is near or far. Dolly moves
		// through the world; zoom shrinks the radius. Two gestures, two
		// meanings, as in the source.
		const double Radius = FMath::Max(1.0, FVector::Dist(ViewportClient->GetViewLocation(), DragCentre));
		Slide(ViewportClient, ViewportClient->GetViewRotation().Vector() * (-DeltaY * 0.005 * Radius));
		break;
	}

	default:
		break;
	}

	return true;
}
