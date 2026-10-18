// Copyright MapModelers. All Rights Reserved.
//
// Modal transforms: press a key, the selection follows the mouse, click to
// keep it or Escape to put it back.
//
// This is not the gizmo with a different trigger. The gizmo asks you to find
// and grab an axis before anything moves; a modal transform starts the moment
// the key is pressed and takes its direction from the mouse, with X, Y or Z
// narrowing it afterwards if you want. For grid editing that is the whole
// difference between nudging points and shaping a surface.
//
// Every gesture works the same way: cache the points, re-derive from the
// cache on every mouse move, and either commit once or restore exactly. The
// cache is what makes cancel honest and what stops the transform compounding
// as the mouse moves.

#include "SurfEdMode.h"
#include "SurfSnapState.h"
#include "Editor/TransBuffer.h"
#include "SurfPickContext.h"
#include "SurfInspector.h"
#include "SurfSession.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "ScopedTransaction.h"
#include "Containers/Ticker.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "LevelEditorViewport.h"
#include "Framework/Application/SlateApplication.h"

#include "SurfGroup.h"
#include "SurfPrefab.h"
#include "SurfBrushMath.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "SurfPrefabInstance.h"
#include "SurfPatchSurfacing.h"
#include "SurfSettings.h"
#include "SurfLattice.h"
#include "SurfKeymap.h"
#include "Selection.h"

#define LOCTEXT_NAMESPACE "SurfacingProModal"

namespace
{
	/** Camera basis at the pivot, which is what a screen-space drag means in world terms. */
	bool GetViewBasis(FEditorViewportClient* ViewportClient, FViewport* Viewport,
		FVector& OutRight, FVector& OutUp, FVector& OutForward)
	{
		if (!ViewportClient || !Viewport)
		{
			return false;
		}

		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
			Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));

		FSceneView* View = ViewportClient->CalcSceneView(&Family);
		if (!View)
		{
			return false;
		}

		const FMatrix ViewMatrix = View->ViewMatrices.GetWorldToView();
		OutRight   = FVector(ViewMatrix.M[0][0], ViewMatrix.M[1][0], ViewMatrix.M[2][0]);
		OutUp      = FVector(ViewMatrix.M[0][1], ViewMatrix.M[1][1], ViewMatrix.M[2][1]);
		OutForward = FVector(ViewMatrix.M[0][2], ViewMatrix.M[1][2], ViewMatrix.M[2][2]);
		return true;
	}

	/**
	 * How far the cursor has been carried round the viewport's edges during
	 * the gesture (S187a): "it shall be able to scroll even further than the
	 * screen limit - respawn the other side and keep going". The gesture
	 * reads the cursor plus this, so a wrap is not a jump.
	 */
	FVector2D GModalWrap = FVector2D::ZeroVector;

	/**
	 * The cursor where it really is, in the viewport's pixels, even outside
	 * it (S187b): the viewport's own record stops at the last move it saw,
	 * so once the cursor left, the gesture froze and nothing wrapped - "long
	 * g through screen edge: does not work".
	 */
	/** The scene viewport behind an editor viewport (5.8: FViewport no longer says its type), by its client's widget. */
	FSceneViewport* SceneViewportOf(FViewport* Viewport)
	{
		FEditorViewportClient* Client = Viewport ? static_cast<FEditorViewportClient*>(Viewport->GetClient()) : nullptr;
		TSharedPtr<SEditorViewport> Widget = Client ? Client->GetEditorViewportWidget() : nullptr;
		TSharedPtr<FSceneViewport> Scene = Widget.IsValid() ? Widget->GetSceneViewport() : nullptr;
		return Scene.IsValid() && static_cast<FViewport*>(Scene.Get()) == Viewport ? Scene.Get() : nullptr;
	}

	bool ModalCursorPixels(FViewport* Viewport, FVector2D& Out)
	{
		FSceneViewport* Scene = SceneViewportOf(Viewport);
		if (!Scene || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		const FGeometry& Geometry = Scene->GetCachedGeometry();
		if (Geometry.GetLocalSize().X < 1.0f || Geometry.Scale <= 0.0f)
		{
			return false;
		}
		Out = FVector2D(Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos())) * Geometry.Scale;
		return true;
	}

	FVector2D CurrentMouse(FViewport* Viewport)
	{
		if (!Viewport)
		{
			return FVector2D::ZeroVector;
		}

		FVector2D Real;
		if (ModalCursorPixels(Viewport, Real))
		{
			return Real + GModalWrap;
		}
		FIntPoint Cursor;
		Viewport->GetMousePos(Cursor);
		return FVector2D(Cursor.X, Cursor.Y) + GModalWrap;
	}

	/**
	 * What the editor's own snapping says, so a drag of ours behaves as a
	 * drag of the editor's: zero means free. The grid, the rotation grid and
	 * the scale grid each have their own switch on the level toolbar.
	 */
	namespace SurfSnap
	{
		double MoveStep()
		{
			const ULevelEditorViewportSettings* Viewport = GetDefault<ULevelEditorViewportSettings>();
			if (!GEditor || !Viewport || !SurfSnapState::Flipped(Viewport->GridEnabled))
			{
				return 0.0;
			}
			return FMath::Max(0.0, static_cast<double>(GEditor->GetGridSize()));
		}

		double TurnStep()
		{
			const ULevelEditorViewportSettings* Viewport = GetDefault<ULevelEditorViewportSettings>();
			if (!GEditor || !Viewport || !SurfSnapState::Flipped(Viewport->RotGridEnabled))
			{
				return 0.0;
			}
			return FMath::Max(0.0, static_cast<double>(GEditor->GetRotGridSize().Yaw));
		}

		double SizeStep()
		{
			const ULevelEditorViewportSettings* Viewport = GetDefault<ULevelEditorViewportSettings>();
			if (!GEditor || !Viewport || !SurfSnapState::Flipped(Viewport->SnapScaleEnabled))
			{
				return 0.0;
			}
			return FMath::Max(0.0, static_cast<double>(GEditor->GetScaleGridSize()) * 0.01);
		}
	}

	FVector AxisVectorFwd(ESurfAxisLock Lock, const FVector& Custom = FVector::ZeroVector);

	FVector AxisVector(ESurfAxisLock Lock, const FVector& Custom = FVector::ZeroVector)
	{
		switch (Lock)
		{
		case ESurfAxisLock::X:      return FVector::ForwardVector;
		case ESurfAxisLock::Y:      return FVector::RightVector;
		case ESurfAxisLock::Z:      return FVector::UpVector;
		case ESurfAxisLock::Custom: return Custom.GetSafeNormal();
		default:                    return FVector::ZeroVector;
		}
	}

	/** What to call the axis a turn is about, for the line the modal shows. */
	FString AxisNamed(ESurfAxisLock Lock)
	{
		switch (Lock)
		{
		case ESurfAxisLock::X:
		case ESurfAxisLock::NotX: return TEXT("X");
		case ESurfAxisLock::Y:
		case ESurfAxisLock::NotY: return TEXT("Y");
		case ESurfAxisLock::Z:
		case ESurfAxisLock::NotZ: return TEXT("Z");
		case ESurfAxisLock::Custom: return TEXT("the way it grew");
		default: return TEXT("the view");
		}
	}

	/** The axis a plane lock excludes, or zero when it is not a plane lock. */
	FVector PlaneNormal(ESurfAxisLock Lock)
	{
		switch (Lock)
		{
		case ESurfAxisLock::NotX: return FVector::ForwardVector;
		case ESurfAxisLock::NotY: return FVector::RightVector;
		case ESurfAxisLock::NotZ: return FVector::UpVector;
		default:                  return FVector::ZeroVector;
		}
	}

	FVector AxisVectorFwd(ESurfAxisLock Lock, const FVector& Custom)
	{
		return AxisVector(Lock, Custom);
	}

	/**
	 * How many world centimetres one screen pixel spans at this distance.
	 * Without this a drag moves a near patch a hair and a far one a mile.
	 */
	double WorldPerPixel(FEditorViewportClient* ViewportClient, const FVector& At)
	{
		if (!ViewportClient)
		{
			return 1.0;
		}

		const double Distance = FMath::Max(1.0, FVector::Dist(ViewportClient->GetViewLocation(), At));
		const double HalfFov = FMath::DegreesToRadians(FMath::Max(1.0f, ViewportClient->ViewFOV) * 0.5f);
		const int32 Height = FMath::Max(1, ViewportClient->Viewport ? ViewportClient->Viewport->GetSizeXY().Y : 1080);

		return (2.0 * Distance * FMath::Tan(HalfFov)) / Height;
	}
}

bool USurfEdMode::BeginPlacementModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// A placement is a transform, not a set of points, so the drag writes to
	// the transform and the stand-in redraws (S71d). Everything else about
	// the gesture - the axis keys, Enter, Escape, the right-click - is the
	// modal's own, so the hand learns one thing.
	if (!Viewport || !RoomPickedPlacement.IsValid() || !IsInsidePrefab())
	{
		return false;
	}
	if (InModal != ESurfModal::Move && InModal != ESurfModal::Rotate && InModal != ESurfModal::Scale)
	{
		return false;
	}
	if (Modal != ESurfModal::None)
	{
		CancelModal();
	}

	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	const USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Placement = RoomAsset ? RoomAsset->FindPlacement(RoomPickedPlacement) : nullptr;
	if (!Placement)
	{
		return false;
	}

	Modal = InModal;
	bModalOnPlacement = true;
	ModalPlacementAdded.Invalidate();
	ModalPlacementId = RoomPickedPlacement;
	ModalPlacementStart = Placement->Transform;
	AxisLock = ESurfAxisLock::None;
	CustomAxis = FVector::ZeroVector;
	ModalTyped[0].Reset();
	ModalTyped[1].Reset();
	ModalTyped[2].Reset();
	ModalTypedField = 0;
	GModalWrap = FVector2D::ZeroVector;
	ModalStartMouse = CurrentMouse(Viewport);
	NoteModalStartRay(ViewportClient, Viewport);
	ModalPivot = (ModalPlacementStart * RoomFrame).GetLocation();

	ModalTransaction = MakeUnique<FScopedTransaction>(
		InModal == ESurfModal::Move ? LOCTEXT("PlacementMove", "Move Placement") :
		InModal == ESurfModal::Rotate ? LOCTEXT("PlacementTurn", "Turn Placement") : LOCTEXT("PlacementSize", "Size Placement"));

	if (Inspector)
	{
		Inspector->SetResult(
			InModal == ESurfModal::Move ? TEXT("moving the placement - X Y Z lock an axis, the grid holds it; Enter or click keeps it, Escape puts it back")
			: InModal == ESurfModal::Rotate ? TEXT("turning the placement - a quarter at a time round Z, X Y Z pick the axis; Enter keeps it, Escape puts it back")
			: TEXT("sizing the placement - Enter keeps it, Escape puts it back"));
	}
	SurfSession::Gesture(TEXT("placement_modal"), FString::Printf(TEXT("kind=%d"), static_cast<int32>(InModal)));
	return true;
}

void USurfEdMode::UpdatePlacementModal(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	if (!RoomAsset || !ViewportClient || !Viewport)
	{
		return;
	}
	FSurfPlacement* Placement = RoomAsset->ModifyPlacement(ModalPlacementId);
	if (!Placement)
	{
		return;
	}

	FVector Right, Up, Forward;
	if (!GetViewBasis(ViewportClient, Viewport, Right, Up, Forward))
	{
		return;
	}
	const FVector2D Delta = CurrentMouse(Viewport) - ModalStartMouse;
	const double Step = SurfSnap::MoveStep();
	const double TurnStep = SurfSnap::TurnStep();
	const double SizeStep = SurfSnap::SizeStep();

	FTransform Now = ModalPlacementStart;
	FString Line;

	switch (Modal)
	{
	case ESurfModal::Move:
	{
		// The drag is in the world; the placement lives in the prefab's
		// space, so it goes back through the room's frame before the grid
		// catches it - a prefab placed in a turned room still lands on the
		// prefab's own grid.
		FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * WorldPerPixel(ViewportClient, ModalPivot);
		const FVector Axis = AxisVector(AxisLock, CustomAxis);
		const FVector Excluded = PlaneNormal(AxisLock);
		// Along the line or across the plane under the hand (S247f: "moving
		// + shift Z to move only on two axis works, but is hard to handle
		// with mouse" - a placed thing took the screen-plane motion, so a
		// plane seen edge-on crawled), as the points' and the primitives'
		// moves have since S167b.
		FVector ByRays;
		if ((!Axis.IsZero() || !Excluded.IsZero()) && AxisLock != ESurfAxisLock::Custom
			&& LockedMoveFromRays(ViewportClient, Viewport, ModalPivot, Axis, Excluded, ByRays))
		{
			WorldDelta = ByRays;
		}
		else if (!Axis.IsZero())
		{
			WorldDelta = Axis * FVector::DotProduct(WorldDelta, Axis);
		}
		else if (!Excluded.IsZero())
		{
			WorldDelta -= Excluded * FVector::DotProduct(WorldDelta, Excluded);
		}

		FVector Local = ModalPlacementStart.GetLocation() + RoomFrame.InverseTransformVector(WorldDelta);
		if (Step > 0.0)
		{
			Local.X = FMath::RoundToDouble(Local.X / Step) * Step;
			Local.Y = FMath::RoundToDouble(Local.Y / Step) * Step;
			Local.Z = FMath::RoundToDouble(Local.Z / Step) * Step;
		}
		Now.SetLocation(Local);
		const FVector Moved = Local - ModalPlacementStart.GetLocation();
		Line = FString::Printf(TEXT("%.0f %.0f %.0f cm"), Moved.X, Moved.Y, Moved.Z);
		break;
	}
	case ESurfModal::Rotate:
	{
		// Sideways drag turns it, a quarter at a time: what a prefab on a
		// grid is turned by, nine times out of ten. About whatever axis is
		// named, and the way the camera looks when none is (S143a).
		const FVector WorldAxis = TurnAxis(AxisLock, CustomAxis, Forward);
		const FVector LocalAxis = RoomFrame.InverseTransformVector(WorldAxis).GetSafeNormal();
		const double Degrees = TurnStep > 0.0 ? FMath::GridSnap(Delta.X * 0.5, TurnStep) : Delta.X * 0.5;
		Now.SetRotation(FQuat(LocalAxis.IsNearlyZero() ? FVector::UpVector : LocalAxis, FMath::DegreesToRadians(Degrees)) * ModalPlacementStart.GetRotation());
		Line = FString::Printf(TEXT("%.0f degrees"), Degrees);
		break;
	}
	case ESurfModal::Scale:
	{
		// One number, every way: a placement sized unevenly would put the
		// prefab's own grid off its cells.
		const double Raw = 1.0 + Delta.X * 0.005;
		const double Factor = FMath::Max(0.05, SizeStep > 0.0 ? FMath::GridSnap(Raw, SizeStep) : Raw);
		Now.SetScale3D(ModalPlacementStart.GetScale3D() * Factor);
		Line = FString::Printf(TEXT("x %.2f"), Factor);
		break;
	}
	default:
		return;
	}

	Placement->Transform = Now;
	RoomAsset->MarkPackageDirty();
	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->RebuildPreview();
	}
	if (Inspector && !Line.IsEmpty())
	{
		Inspector->SetResult(Line);
	}
}

bool USurfEdMode::BeginBrushModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// A brush is planes, not points, so the drag writes the planes - but the
	// gesture is the modal the hand already has (S72c).
	if (!Viewport || InModal == ESurfModal::None || InModal == ESurfModal::Slide)
	{
		return false;
	}
	/**
	 * Corners picked by hand are what the gesture moves (S148a).
	 *
	 * "cant move selected verts on a box !" - G reached here, and here every
	 * question was about sides, so the answer was always "move the whole
	 * brush". The corners come first now: they name their own brushes and
	 * they stand on their own pivot, and the sides are along for the ride.
	 */
	const bool bOnCorners = BrushCornerSelection.Num() > 0;
	const TArray<FSurfFaceRef> PickedSides = bOnCorners ? TArray<FSurfFaceRef>() : PickedFaces();

	TArray<FSurfFaceRef> Brushes = BrushesHeldBySelection();
	if (bOnCorners)
	{
		Brushes.Reset();
		for (const FSurfBrushCornerRef& Corner : BrushCornerSelection)
		{
			if (!Corner.Group.IsValid())
			{
				continue;
			}
			FSurfFaceRef Ref;
			Ref.Group = Corner.Group;
			Ref.BrushId = Corner.BrushId;
			Ref.FaceIndex = 0;
			const bool bHave = Brushes.ContainsByPredicate([&Ref](const FSurfFaceRef& Each)
			{
				return Each.Group == Ref.Group && Each.BrushId == Ref.BrushId;
			});
			if (!bHave)
			{
				Brushes.Add(Ref);
			}
		}
	}

	FVector Pivot = FVector::ZeroVector;
	const bool bHavePivot = bOnCorners ? BrushCornersPivot(Pivot) : BrushSelectionPivot(Pivot);
	if (Brushes.Num() == 0 || !bHavePivot)
	{
		return false;
	}

	/**
	 * A size works about the middle of the sides picked, not the middle of
	 * the brush (S146a): a face grows **in place** rather than swinging away
	 * from the brush's centre while it grows. With every side picked the two
	 * are the same point, so nothing changes for a whole-brush size.
	 */
	if (InModal == ESurfModal::Scale && !bOnCorners)
	{
		FVector Middle = FVector::ZeroVector;
		if (SelectedFacesPivot(Middle))
		{
			Pivot = Middle;
		}
	}
	if (Modal != ESurfModal::None)
	{
		CancelModal();
	}

	ModalBrushes.Reset();
	for (const FSurfFaceRef& Ref : Brushes)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfBrushModalTarget Target;
		Target.Group = Ref.Group;
		Target.BrushId = Ref.BrushId;
		for (const FSurfFace& Face : Brush->Faces)
		{
			Target.Planes.Add(Face.Plane);
		}

		Target.Faces = Brush->Faces;

		// And where its corners stand, for the size that scales them (S146a).
		FSurfHull Hull;
		if (SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			Target.Corners = Hull.Vertices;

			// Picked by hand, where there are any (S148a).
			for (const FSurfBrushCornerRef& Corner : BrushCornerSelection)
			{
				if (Corner.Group == Ref.Group && Corner.BrushId == Ref.BrushId
					&& Hull.Vertices.IsValidIndex(Corner.Corner))
				{
					Target.PickedCorners.AddUnique(Corner.Corner);
				}
			}

			for (const FSurfFaceRef& Picked : PickedSides)
			{
				if (Picked.Group != Ref.Group || Picked.BrushId != Ref.BrushId)
				{
					continue;
				}
				for (const FSurfHullFace& Poly : Hull.Faces)
				{
					if (Poly.SourceFaceIndex != Picked.FaceIndex)
					{
						continue;
					}
					for (const int32 Corner : Poly.VertexIndices)
					{
						Target.PickedCorners.AddUnique(Corner);
					}
				}
			}
		}
		for (const FSurfFaceRef& Picked : PickedSides)
		{
			if (Picked.Group == Ref.Group && Picked.BrushId == Ref.BrushId && Brush->Faces.IsValidIndex(Picked.FaceIndex))
			{
				Target.PushedFaces.AddUnique(Picked.FaceIndex);
			}
		}
		ModalBrushes.Add(MoveTemp(Target));
	}
	if (ModalBrushes.Num() == 0)
	{
		return false;
	}

	/**
	 * A move with sides picked moves their corners, anywhere (S147a).
	 *
	 * Pushing them along their own normals is what the gizmo's arrow does -
	 * "a box should allow for transforming scale from one side, one side
	 * remains, one extend" - and it stays there, where the direction is
	 * the handle you grabbed. On the key, G means move, as it does
	 * everywhere else in the tool: "i cant move it freely either".
	 */
	/**
	 * With corners picked by hand the gesture is theirs, whatever it is -
	 * a move or a turn - and the size takes the branch below (S148a). The
	 * sides keep the rule they had: G on a picked side moves its corners,
	 * R on one still turns the brush.
	 */
	bModalMovesCorners = (bOnCorners && InModal != ESurfModal::Scale)
		|| (InModal == ESurfModal::Move && SubMode == ESurfSubMode::Face && PickedFaces().Num() > 0);
	bModalPushesFaces = false;

	// And a size does the same to them (S141a): "when scaling a box face the
	// whole box will scale ? bug". Move was per-face and size was not, which
	// is the inconsistency. Now both mean the sides that are picked.
	/**
	 * Whatever sub-mode he is in (S145a): "s should scale only this face.
	 * its bug atm", with S141a already shipped.
	 *
	 * Sizing the picked sides needs no sub-mode test, because **sizing all
	 * six of them about the middle is sizing the brush** - the two answers
	 * are the same wherever they overlap, which the test asserts. So the
	 * condition only ever cost us the cases where the sub-mode was not what
	 * the rule expected, and gave nothing back.
	 *
	 * Move is not like that: pushing all six sides along their own normals
	 * inflates the brush rather than moving it, so that one keeps its test.
	 */
	bModalScalesFaces = InModal == ESurfModal::Scale && (bOnCorners || PickedFaces().Num() > 0);

	Modal = InModal;
	bModalOnBrush = true;
	AxisLock = ESurfAxisLock::None;
	CustomAxis = FVector::ZeroVector;
	ModalTyped[0].Reset();
	ModalTyped[1].Reset();
	ModalTyped[2].Reset();
	ModalTypedField = 0;
	GModalWrap = FVector2D::ZeroVector;
	ModalStartMouse = CurrentMouse(Viewport);
	NoteModalStartRay(ViewportClient, Viewport);
	ModalPivot = (InModal == ESurfModal::Rotate || InModal == ESurfModal::Scale) ? PivotOr(Pivot) : Pivot;

	ModalTransaction = MakeUnique<FScopedTransaction>(
		bModalPushesFaces ? LOCTEXT("PushFaces", "Push Faces") :
		bOnCorners ? LOCTEXT("MoveBrushCornersModal", "Edit Primitive Corners") :
		bModalMovesCorners ? LOCTEXT("MoveFaces", "Move Faces") :
		bModalScalesFaces ? LOCTEXT("SizeFaces", "Size Faces") :
		InModal == ESurfModal::Move ? LOCTEXT("MoveBrush", "Move Primitive") :
		InModal == ESurfModal::Rotate ? LOCTEXT("TurnBrush", "Turn Primitive") : LOCTEXT("SizeBrush", "Size Primitive"));

	if (Inspector)
	{
		Inspector->SetResult(
			bOnCorners ? TEXT("moving the picked corners - the solid re-forms round where they land; X Y Z lock an axis, Enter keeps, Escape puts it back")
			: bModalPushesFaces ? TEXT("pushing the picked faces along their own normals - the rest of the primitive stays; X Y Z lock an axis, Enter keeps, Escape puts it back")
			: bModalMovesCorners ? TEXT("moving the picked sides - the solid re-forms round where their corners land; X Y Z lock an axis, the gizmo's arrow pushes along the normal instead, Enter keeps, Escape puts it back")
			: bModalScalesFaces ? TEXT("sizing the picked sides about the middle - the rest of the primitive stays; X Y Z lock an axis, Enter keeps, Escape puts it back")
			: InModal == ESurfModal::Move ? TEXT("moving the primitive - X Y Z lock an axis; the editor's grid snapping holds it when it is on; Enter keeps, Escape puts it back")
			: InModal == ESurfModal::Rotate ? TEXT("turning the primitive - a quarter at a time round Z, X Y Z pick the axis; Enter keeps, Escape puts it back")
			: TEXT("sizing the primitive about its middle - X Y Z for one axis; Enter keeps, Escape puts it back"));
	}
	SurfSession::Gesture(TEXT("brush_modal"), FString::Printf(TEXT("kind=%d push=%d brushes=%d"), static_cast<int32>(InModal), bModalPushesFaces ? 1 : 0, ModalBrushes.Num()));
	return true;
}

void USurfEdMode::UpdateBrushModal(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (!ViewportClient || !Viewport)
	{
		return;
	}
	FVector Right, Up, Forward;
	if (!GetViewBasis(ViewportClient, Viewport, Right, Up, Forward))
	{
		return;
	}
	const FVector2D Delta = CurrentMouse(Viewport) - ModalStartMouse;
	const double PerPixel = WorldPerPixel(ViewportClient, ModalPivot);

	// The editor's own snapping decides, as the gizmo's rings already do:
	// "box is snapping on move rotate etc even if snap is off in ue?". With
	// the grid off, the drag is free.
	const double Step = SurfSnap::MoveStep();
	const double TurnStep = SurfSnap::TurnStep();
	const double SizeStep = SurfSnap::SizeStep();
	const FVector Axis = AxisVector(AxisLock, CustomAxis);
	const FVector Excluded = PlaneNormal(AxisLock);

	FString Line;
	FTransform About = FTransform::Identity;
	double PushCm = 0.0;

	if (bModalPushesFaces)
	{
		// How far the drag went along the faces' own normal, in the world,
		// snapped to the grid: a wall pulled out lands on a grid line.
		FVector Normal = FVector::ZeroVector;
		for (const FSurfBrushModalTarget& Target : ModalBrushes)
		{
			const ASurfGroup* Group = Target.Group.Get();
			if (!Group)
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			for (const int32 Index : Target.PushedFaces)
			{
				if (Target.Planes.IsValidIndex(Index))
				{
					Normal += ToWorld.TransformVectorNoScale(FVector(Target.Planes[Index].X, Target.Planes[Index].Y, Target.Planes[Index].Z));
				}
			}
		}
		Normal = Normal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}
		FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * PerPixel;
		// Along the side's own line under the hand (S189a), as a locked move
		// reads it, so a side facing the camera still pulls out.
		FVector ByRays;
		if (LockedMoveFromRays(ViewportClient, Viewport, ModalPivot, Normal, FVector::ZeroVector, ByRays))
		{
			WorldDelta = ByRays;
		}
		PushCm = FVector::DotProduct(WorldDelta, Normal);
		if (Step > 0.0)
		{
			PushCm = FMath::RoundToDouble(PushCm / Step) * Step;
		}
		Line = FString::Printf(TEXT("%.0f cm out"), PushCm);
	}
	else
	{
		switch (Modal)
		{
		case ESurfModal::Move:
		{
			FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * PerPixel;
			FVector ByRays;
			if ((!Axis.IsZero() || !Excluded.IsZero()) && AxisLock != ESurfAxisLock::Custom
				&& LockedMoveFromRays(ViewportClient, Viewport, ModalPivot, Axis, Excluded, ByRays))
			{
				WorldDelta = ByRays;
			}
			else if (!Axis.IsZero())
			{
				WorldDelta = Axis * FVector::DotProduct(WorldDelta, Axis);
			}
			else if (!Excluded.IsZero())
			{
				WorldDelta -= Excluded * FVector::DotProduct(WorldDelta, Excluded);
			}
			if (Step > 0.0)
			{
				WorldDelta.X = FMath::RoundToDouble(WorldDelta.X / Step) * Step;
				WorldDelta.Y = FMath::RoundToDouble(WorldDelta.Y / Step) * Step;
				WorldDelta.Z = FMath::RoundToDouble(WorldDelta.Z / Step) * Step;
			}

			/**
			 * A brush move snaps like a patch move does (S157e): "should be
			 * able snap between patch/brushes too." The corners on the move
			 * - the picked ones, or all of a brush moved whole - against every
			 * patch point and brush corner not moving: the one under the
			 * cursor first, then a surface under the cursor if asked, then
			 * the reach in centimetres. One correction for the whole gesture.
			 */
			if (!bModalPushesFaces)
			{
				TArray<FVector> Moving;
				TArray<FVector> MovingFrom;
				TArray<FGuid> MovingIds;
				const ASurfGroup* MovingGroup = nullptr;
				for (const FSurfBrushModalTarget& Target : ModalBrushes)
				{
					const ASurfGroup* Group = Target.Group.Get();
					if (!Group || (bModalMovesCorners && Target.PickedCorners.Num() == 0))
					{
						continue;
					}
					MovingGroup = Group;
					MovingIds.Add(Target.BrushId);
					const FTransform ToWorld = Group->GetActorTransform();
					if (bModalMovesCorners)
					{
						for (const int32 Corner : Target.PickedCorners)
						{
							if (Target.Corners.IsValidIndex(Corner))
							{
								const FVector From = ToWorld.TransformPosition(Target.Corners[Corner]);
								MovingFrom.Add(From);
								Moving.Add(From + WorldDelta);
							}
						}
					}
					else
					{
						for (const FVector& Corner : Target.Corners)
						{
							const FVector From = ToWorld.TransformPosition(Corner);
							MovingFrom.Add(From);
							Moving.Add(From + WorldDelta);
						}
					}
				}
				if (Moving.Num() > 0 && MovingGroup)
				{
					SnapAlongAxis = Axis;
					SnapExcludedAxis = Excluded;
					TArray<FVector> Targets;
					GatherSnapTargets(MovingGroup, nullptr, &MovingIds, Targets);
					FVector Correction = FVector::ZeroVector;
					const FVector2D Cursor = CurrentMouse(Viewport);
					if (!FindCursorSnapAmong(ViewportClient, Viewport, Cursor, Targets, Moving, MovingFrom, Correction)
						&& !FindSurfaceSnap(ViewportClient, Viewport, Cursor, MovingGroup, Moving, Correction))
					{
						FindReachSnapAmong(Targets, Moving, MovingFrom, Correction);
					}
					WorldDelta += Correction;
					SnapAlongAxis = FVector::ZeroVector;
					SnapExcludedAxis = FVector::ZeroVector;
				}
			}
			About = FTransform(WorldDelta);
			Line = FString::Printf(TEXT("%.0f %.0f %.0f cm"), WorldDelta.X, WorldDelta.Y, WorldDelta.Z);
			break;
		}
		case ESurfModal::Rotate:
		{
			// One rule for which axis a turn is about, whatever is being
			// turned (S143a) - and the line says which, because "didnt
			// work" is what a lock that does nothing visible looks like.
			const FVector Turn = TurnAxis(AxisLock, CustomAxis, Forward);
			const double Degrees = TurnStep > 0.0 ? FMath::GridSnap(Delta.X * 0.5, TurnStep) : Delta.X * 0.5;
			About = FTransform(FQuat(Turn, FMath::DegreesToRadians(Degrees)));
			Line = FString::Printf(TEXT("%.0f degrees about %s"), Degrees, *AxisNamed(AxisLock));
			break;
		}
		case ESurfModal::Scale:
		{
			const double Raw = 1.0 + Delta.X * 0.005;
			const double Factor = FMath::Max(0.05, SizeStep > 0.0 ? FMath::GridSnap(Raw, SizeStep) : Raw);
			FVector Scale(Factor);
			if (!Axis.IsZero())
			{
				Scale = FVector::OneVector + Axis.GetAbs() * (Factor - 1.0);
			}
			About = FTransform(FQuat::Identity, FVector::ZeroVector, Scale);
			Line = FString::Printf(TEXT("x %.2f"), Factor);
			break;
		}
		default:
			return;
		}
	}

	// Always from the planes as they were: re-applying to the result drifts.
	int32 Done = 0;
	for (const FSurfBrushModalTarget& Target : ModalBrushes)
	{
		ASurfGroup* Group = Target.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Target.BrushId) : nullptr;
		if (!Brush || (!bModalScalesFaces && !bModalMovesCorners && Brush->Faces.Num() != Target.Planes.Num()))
		{
			continue;
		}

		// A gesture that moves what is picked, and nothing picked on this
		// brush: it stays where it is (S148a). Falling through to the last
		// branch would have moved the whole of it instead.
		if ((bModalScalesFaces || bModalMovesCorners) && Target.PickedCorners.Num() == 0)
		{
			continue;
		}
		const TArray<FSurfFace> Was = Brush->Faces;
		if (bModalPushesFaces)
		{
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				Brush->Faces[I].Plane = Target.Planes[I];
			}
			for (const int32 Index : Target.PushedFaces)
			{
				if (Brush->Faces.IsValidIndex(Index))
				{
					Brush->Faces[Index].Plane.W = Target.Planes[Index].W + PushCm;
				}
			}
		}
		else if (bModalMovesCorners && Target.PickedCorners.Num() > 0)
		{
			/**
			 * The picked corners, carried by the gesture, and the planes
			 * derived from where they land - always from where they stood
			 * when it began (S147a).
			 *
			 * About the pivot rather than by the raw translation (S148a):
			 * for a move the pivot cancels and this is the same arithmetic
			 * it always was, and for a turn it is the turn.
			 */
			const FTransform ToWorld = Group->GetActorTransform();

			TArray<FVector> Moved = Target.Corners;
			for (const int32 Corner : Target.PickedCorners)
			{
				if (!Moved.IsValidIndex(Corner))
				{
					continue;
				}
				const FVector World = ToWorld.TransformPosition(Target.Corners[Corner]);
				Moved[Corner] = ToWorld.InverseTransformPosition(
					ModalPivot + About.TransformPosition(World - ModalPivot));
			}

			Brush->Faces = Target.Faces;

			FSurfBrush Rebuilt;
			FString Why;
			if (SurfBrushMath::RebuildFromPoints(*Brush, Moved, Rebuilt, &Why))
			{
				const FGuid Keep = Brush->Id;
				const int32 Serial = Brush->EditSerial;
				*Brush = MoveTemp(Rebuilt);
				Brush->Id = Keep;
				Brush->EditSerial = Serial;
				RepickFacesByNormal(Group, Target.BrushId, Target.Planes);
			}
			else
			{
				Brush->Faces = Was;
			}
		}
		else if (bModalScalesFaces && Target.PickedCorners.Num() > 0)
		{
			/**
			 * The picked sides' corners, scaled about the middle, and the
			 * planes derived from where they land (S146a). Always from the
			 * corners as they stood when the gesture began, so a drag back
			 * and forth lands where it started.
			 */
			const FTransform ToWorld = Group->GetActorTransform();
			const FVector Factor = About.GetScale3D();

			TArray<FVector> Moved = Target.Corners;
			for (const int32 Corner : Target.PickedCorners)
			{
				if (!Moved.IsValidIndex(Corner))
				{
					continue;
				}
				const FVector World = ToWorld.TransformPosition(Target.Corners[Corner]);
				Moved[Corner] = ToWorld.InverseTransformPosition(ModalPivot + (World - ModalPivot) * Factor);
			}

			// The whole face list back, not just the planes: a rebuild
			// writes a new list, so last frame's may be a different length
			// in a different order (S146c).
			Brush->Faces = Target.Faces;

			FSurfBrush Rebuilt;
			FString Why;
			if (SurfBrushMath::RebuildFromPoints(*Brush, Moved, Rebuilt, &Why))
			{
				const FGuid Keep = Brush->Id;
				const int32 Serial = Brush->EditSerial;
				*Brush = MoveTemp(Rebuilt);
				Brush->Id = Keep;
				Brush->EditSerial = Serial;
				RepickFacesByNormal(Group, Target.BrushId, Target.Planes);
			}
			else
			{
				Brush->Faces = Was;
			}
		}
		else
		{
			const FTransform ToWorld = Group->GetActorTransform();
			const FMatrix Matrix = ToWorld.ToMatrixWithScale()
				* FTranslationMatrix(-ModalPivot) * About.ToMatrixWithScale() * FTranslationMatrix(ModalPivot)
				* ToWorld.Inverse().ToMatrixWithScale();
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				Brush->Faces[I].Plane = Target.Planes[I].TransformBy(Matrix);
			}
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			Brush->Faces = Was;
			continue;
		}
		++Brush->EditSerial;
		++Done;
	}
	if (Done > 0)
	{
		RebuildTouchedGroups();
	}
	if (Inspector && !Line.IsEmpty())
	{
		Inspector->SetResult(Line);
	}
}

/**
 * The same gesture, over the editor's own selection (S94a).
 *
 * "could move it with unreal transform (original gizmo) but not with g key."
 * Both halves of that sentence are the bug: the widget worked because it is
 * the editor's, and the key did not because G, R and S were only ever wired
 * to things of ours. A tool whose keys stop at the edge of its own data makes
 * the hand switch schemes in the middle of a job, which is the one thing the
 * Blender scheme is here to prevent.
 *
 * Everything about the gesture is the modal's own - the axis keys, the typed
 * numbers, Enter, Escape, the grid - so a prefab instance moves exactly the
 * way a row of control points does.
 */
bool USurfEdMode::BeginActorModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (!Viewport || !GEditor)
	{
		return false;
	}
	if (InModal != ESurfModal::Move && InModal != ESurfModal::Rotate && InModal != ESurfModal::Scale)
	{
		return false;
	}

	TArray<AActor*> Chosen;
	if (USelection* Selected = GEditor->GetSelectedActors())
	{
		Selected->GetSelectedObjects<AActor>(Chosen);
	}
	if (Chosen.Num() == 0)
	{
		return false;
	}

	if (Modal != ESurfModal::None)
	{
		CancelModal();
	}

	ModalActors.Reset();
	ModalActorStart.Reset();
	FVector Pivot = FVector::ZeroVector;
	for (AActor* Actor : Chosen)
	{
		if (!Actor)
		{
			continue;
		}
		Actor->Modify();
		ModalActors.Add(Actor);
		ModalActorStart.Add(Actor->GetActorTransform());
		Pivot += Actor->GetActorLocation();
	}
	if (ModalActors.Num() == 0)
	{
		return false;
	}
	Pivot /= static_cast<double>(ModalActors.Num());

	Modal = InModal;
	bModalOnActors = true;
	bActorTrackball = false;
	AxisLock = ESurfAxisLock::None;
	CustomAxis = FVector::ZeroVector;
	ModalTyped[0].Reset();
	ModalTyped[1].Reset();
	ModalTyped[2].Reset();
	ModalTypedField = 0;
	GModalWrap = FVector2D::ZeroVector;
	ModalStartMouse = CurrentMouse(Viewport);
	NoteModalStartRay(ViewportClient, Viewport);
	ModalPivot = (InModal == ESurfModal::Rotate || InModal == ESurfModal::Scale) ? PivotOr(Pivot) : Pivot;

	ModalTransaction = MakeUnique<FScopedTransaction>(
		InModal == ESurfModal::Move ? LOCTEXT("ActorMove", "Move") :
		InModal == ESurfModal::Rotate ? LOCTEXT("ActorTurn", "Turn") : LOCTEXT("ActorSize", "Size"));

	if (Inspector)
	{
		Inspector->SetResult(
			InModal == ESurfModal::Move ? TEXT("moving - X Y Z lock an axis, a number types it; Enter or click keeps it, Escape puts it back")
			: InModal == ESurfModal::Rotate ? TEXT("turning - X Y Z pick the axis; Enter keeps it, Escape puts it back")
			: TEXT("sizing - Enter keeps it, Escape puts it back"));
	}
	SurfSession::Gesture(TEXT("actor_modal"), FString::Printf(TEXT("kind=%d n=%d"), static_cast<int32>(InModal), ModalActors.Num()));
	return true;
}

void USurfEdMode::UpdateActorModal(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (!ViewportClient || !Viewport || ModalActors.Num() != ModalActorStart.Num())
	{
		return;
	}

	FVector Right, Up, Forward;
	if (!GetViewBasis(ViewportClient, Viewport, Right, Up, Forward))
	{
		return;
	}

	const FVector2D Delta = CurrentMouse(Viewport) - ModalStartMouse;
	const double Step = SurfSnap::MoveStep();
	const double TurnStep = SurfSnap::TurnStep();
	const double SizeStep = SurfSnap::SizeStep();
	FString Line;

	switch (Modal)
	{
	case ESurfModal::Move:
	{
		FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * WorldPerPixel(ViewportClient, ModalPivot);
		const FVector Axis = AxisVector(AxisLock, CustomAxis);
		const FVector Excluded = PlaneNormal(AxisLock);
		// Along the line or across the plane under the hand (S247f: "moving
		// + shift Z to move only on two axis works, but is hard to handle
		// with mouse" - a placed thing took the screen-plane motion, so a
		// plane seen edge-on crawled), as the points' and the primitives'
		// moves have since S167b.
		FVector ByRays;
		if ((!Axis.IsZero() || !Excluded.IsZero()) && AxisLock != ESurfAxisLock::Custom
			&& LockedMoveFromRays(ViewportClient, Viewport, ModalPivot, Axis, Excluded, ByRays))
		{
			WorldDelta = ByRays;
		}
		else if (!Axis.IsZero())
		{
			WorldDelta = Axis * FVector::DotProduct(WorldDelta, Axis);
		}
		else if (!Excluded.IsZero())
		{
			WorldDelta -= Excluded * FVector::DotProduct(WorldDelta, Excluded);
		}

		// A typed number is the move, exactly, whatever the cursor did.
		if (HasTypedValue())
		{
			WorldDelta = TypedVector();
		}
		else if (Step > 0.0)
		{
			WorldDelta.X = FMath::RoundToDouble(WorldDelta.X / Step) * Step;
			WorldDelta.Y = FMath::RoundToDouble(WorldDelta.Y / Step) * Step;
			WorldDelta.Z = FMath::RoundToDouble(WorldDelta.Z / Step) * Step;
		}
		if (!HasTypedValue())
		{
			// Snapping as a primitive snaps (S227a: "didnt manage to snap a
			// staticmesh asset on a patch, nor a prefab on patches or on a
			// prefab"): the actors' origins and box corners against the
			// points of ours, the surface under the cursor, what is in reach.
			TArray<FVector> Moving, MovingFrom;
			for (int32 I = 0; I < ModalActors.Num(); ++I)
			{
				const AActor* Actor = ModalActors[I].Get();
				if (!Actor) { continue; }
				const FVector Origin = ModalActorStart[I].GetLocation();
				MovingFrom.Add(Origin);
				Moving.Add(Origin + WorldDelta);
				// The box where the actor started: the one it has now, moved back.
				const FBox Now = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
				if (Now.IsValid)
				{
					const FVector Back = Origin - Actor->GetActorLocation();
					for (int32 C = 0; C < 8; ++C)
					{
						const FVector Corner(
							(C & 1) ? Now.Max.X : Now.Min.X,
							(C & 2) ? Now.Max.Y : Now.Min.Y,
							(C & 4) ? Now.Max.Z : Now.Min.Z);
						MovingFrom.Add(Corner + Back);
						Moving.Add(Corner + Back + WorldDelta);
					}
				}
			}
			if (Moving.Num() > 0)
			{
				SnapAlongAxis = Axis;
				SnapExcludedAxis = Excluded;
				SnapMarkerKind = ESurfSnapKind::None;
				TArray<FVector> Targets;
				GatherSnapTargets(nullptr, nullptr, nullptr, Targets);
				FVector Correction = FVector::ZeroVector;
				const FVector2D Cursor = CurrentMouse(Viewport);
				if (!FindCursorSnapAmong(ViewportClient, Viewport, Cursor, Targets, Moving, MovingFrom, Correction)
					&& !FindSurfaceSnap(ViewportClient, Viewport, Cursor, ModalActors[0].Get(), Moving, Correction))
				{
					FindReachSnapAmong(Targets, Moving, MovingFrom, Correction);
				}
				WorldDelta += Correction;
				SnapAlongAxis = FVector::ZeroVector;
				SnapExcludedAxis = FVector::ZeroVector;
			}
		}

		/**
		 * Standing on the face (S235a, snap round 4): a placed thing that
		 * landed on a surface turns so its up runs along that surface's
		 * normal - a prop set down on a slope sits on the slope. Landing
		 * anywhere else, or the option off, puts its start turn back.
		 */
		const USurfSettings* Stand = GetDefault<USurfSettings>();
		const bool bTurnToFace = Stand && Stand->bSnapTurnsToFace && SnapMarkerKind == ESurfSnapKind::Face && !HasTypedValue();
		for (int32 I = 0; I < ModalActors.Num(); ++I)
		{
			if (AActor* Actor = ModalActors[I].Get())
			{
				const FVector At = ModalActorStart[I].GetLocation() + WorldDelta;
				if (bTurnToFace)
				{
					const FQuat Start = ModalActorStart[I].GetRotation();
					const FQuat Turn = FQuat::FindBetweenNormals(Start.GetUpVector(), SnapMarkerNormal);
					Actor->SetActorLocationAndRotation(At, Turn * Start);
				}
				else
				{
					Actor->SetActorLocationAndRotation(At, ModalActorStart[I].GetRotation());
				}
			}
		}
		Line = FString::Printf(TEXT("%.0f %.0f %.0f cm"), WorldDelta.X, WorldDelta.Y, WorldDelta.Z);
		break;
	}
	case ESurfModal::Rotate:
	{
		FVector WorldAxis = TurnAxis(AxisLock, CustomAxis, Forward);
		double Degrees = TurnStep > 0.0 ? FMath::GridSnap(Delta.X * 0.5, TurnStep) : Delta.X * 0.5;
		// The ball (S211a): the axis is across the drag, half the viewport
		// is half a turn - as a patch turns with no axis named.
		if (bActorTrackball && AxisLock == ESurfAxisLock::None)
		{
			const FVector Dragged = (Right * Delta.X - Up * Delta.Y);
			if (!Dragged.IsNearlyZero())
			{
				WorldAxis = FVector::CrossProduct(Forward, Dragged).GetSafeNormal();
				const double Across = FMath::Max(200.0, static_cast<double>(Viewport->GetSizeXY().Y));
				Degrees = FMath::RadiansToDegrees((Delta.Size() / Across) * PI);
				if (TurnStep > 0.0) { Degrees = FMath::GridSnap(Degrees, TurnStep); }
			}
			else
			{
				Degrees = 0.0;
			}
		}
		const FQuat Turn(WorldAxis, FMath::DegreesToRadians(Degrees));

		for (int32 I = 0; I < ModalActors.Num(); ++I)
		{
			if (AActor* Actor = ModalActors[I].Get())
			{
				const FVector Moved = ModalPivot + Turn.RotateVector(ModalActorStart[I].GetLocation() - ModalPivot);
				Actor->SetActorLocationAndRotation(Moved, Turn * ModalActorStart[I].GetRotation());
			}
		}
		Line = FString::Printf(TEXT("%.0f degrees"), Degrees);
		break;
	}
	case ESurfModal::Scale:
	{
		const double Raw = 1.0 + Delta.X * 0.005;
		double Factor = FMath::Max(0.01, SizeStep > 0.0 ? FMath::GridSnap(Raw, SizeStep) : Raw);
		if (HasTypedValue())
		{
			Factor = FMath::Max(0.01, TypedVector().X);
		}
		// An axis named scales along it alone (S238a: "S then Y to scale
		// only one axis"), in the world's axes as the placed thing's box
		// is read; a plane lock leaves that axis out.
		const FVector Axis = AxisVector(AxisLock, CustomAxis);
		const FVector Excluded = PlaneNormal(AxisLock);
		FVector Scale(Factor);
		if (!Axis.IsZero())
		{
			Scale = FVector::OneVector + Axis.GetAbs() * (Factor - 1.0);
		}
		else if (!Excluded.IsZero())
		{
			Scale = FVector(Factor) - Excluded.GetAbs() * (Factor - 1.0);
		}
		for (int32 I = 0; I < ModalActors.Num(); ++I)
		{
			if (AActor* Actor = ModalActors[I].Get())
			{
				Actor->SetActorLocation(ModalPivot + (ModalActorStart[I].GetLocation() - ModalPivot) * Scale);
				Actor->SetActorScale3D(ModalActorStart[I].GetScale3D() * Scale);
			}
		}
		Line = Axis.IsZero() && Excluded.IsZero()
			? FString::Printf(TEXT("x %.2f"), Factor)
			: FString::Printf(TEXT("x %.2f along %s"), Factor, Axis.IsZero() ? TEXT("the plane") : (Axis.X != 0.0 ? TEXT("X") : Axis.Y != 0.0 ? TEXT("Y") : TEXT("Z")));
		break;
	}
	default:
		return;
	}

	if (Inspector && !Line.IsEmpty())
	{
		Inspector->SetResult(Line);
	}
}

double USurfEdMode::SnappedSlideAmount(ASurfGroup& Group, FSurfPatch& Patch, const TArray<FVector>& Axes,
	TFunctionRef<FVector(int32, int32)> Original, double Amount)
{
	auto Apply = [&](double By)
	{
		for (int32 I = 0; I < PointSelection.Num(); ++I)
		{
			const FSurfPointRef& Ref = PointSelection[I];
			if (Ref.PatchId != Patch.Id || Ref.X < 0 || Ref.X >= Patch.Width || Ref.Y < 0 || Ref.Y >= Patch.Height)
			{
				continue;
			}
			const FVector Axis = Axes.IsValidIndex(I) ? Axes[I] : FVector::ZeroVector;
			Patch.At(Ref.X, Ref.Y).Pos = Original(Ref.X, Ref.Y) + Axis * By;
		}
	};
	Apply(Amount);

	// The lead: the first point with somewhere to slide to.
	int32 Lead = INDEX_NONE;
	for (int32 I = 0; I < Axes.Num() && I < PointSelection.Num(); ++I)
	{
		if (!Axes[I].IsNearlyZero())
		{
			Lead = I;
			break;
		}
	}
	if (Lead == INDEX_NONE)
	{
		return Amount;
	}

	const FTransform ToWorld = Group.GetActorTransform();
	const FVector WorldAxis = ToWorld.TransformVector(Axes[Lead]);
	const double Length = WorldAxis.Size();
	if (Length <= KINDA_SMALL_NUMBER)
	{
		return Amount;
	}

	TArray<FVector> MovingFrom;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Ref.PatchId == Patch.Id && Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
		{
			MovingFrom.Add(ToWorld.TransformPosition(Original(Ref.X, Ref.Y)));
		}
	}

	SnapAlongAxis = WorldAxis / Length;
	SnapExcludedAxis = FVector::ZeroVector;
	const FVector Correction = FindPointSnap(Group, Patch, MovingFrom);
	SnapAlongAxis = FVector::ZeroVector;

	if (Correction.IsNearlyZero())
	{
		return Amount;
	}

	// Centimetres along the segment, as a share of the segment.
	const double Snapped = FMath::Clamp(Amount + FVector::DotProduct(Correction, WorldAxis) / (Length * Length), -1.0, 1.0);
	Apply(Snapped);
	return Snapped;
}

bool USurfEdMode::BeginModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// What is picked inside a prefab may be a placement rather than points
	// (S71d); the same keys move it.
	if (IsInsidePrefab() && RoomPickedPlacement.IsValid() && PointSelection.Num() == 0)
	{
		return BeginPlacementModal(InModal, ViewportClient, Viewport);
	}

	// A brush, its faces, or the corners picked on it, and no points: G R S
	// work on it (S72c; the corners, S148a).
	if (PointSelection.Num() == 0 && !SelectedPatch.IsValid()
		&& (Selection.Num() > 0 || ActiveFace.IsValid() || BrushCornerSelection.Num() > 0))
	{
		if (BeginBrushModal(InModal, ViewportClient, Viewport))
		{
			return true;
		}
	}

	// Nothing of ours at all, and the editor holding something: the same keys
	// move that (S94a).
	if (PointSelection.Num() == 0 && !SelectedPatch.IsValid() && Selection.Num() == 0 && !ActiveFace.IsValid())
	{
		// R again while turning actors (S211a): "expecting free rotate upon
		// the second press" - the ball, as a patch's R does; a third R the
		// twist again. G again and S again start over as before.
		if (bModalOnActors && Modal == ESurfModal::Rotate && InModal == ESurfModal::Rotate)
		{
			bActorTrackball = !bActorTrackball;
			AxisLock = ESurfAxisLock::None;
			CustomAxis = FVector::ZeroVector;
			ModalStartMouse = CurrentMouse(Viewport);
			for (int32 I = 0; I < ModalActors.Num(); ++I)
			{
				if (AActor* Actor = ModalActors[I].Get())
				{
					Actor->SetActorTransform(ModalActorStart[I]);
				}
			}
			SurfSession::Gesture(TEXT("actor_turn_mode"), bActorTrackball ? TEXT("ball") : TEXT("twist"));
			RedrawViewports();
			return true;
		}
		if (BeginActorModal(InModal, ViewportClient, Viewport))
		{
			return true;
		}
	}

	// Edge sub-mode selects control points too, so a transform started from
	// it is the same transform. Refusing it here is what made extrude a
	// one-shot: the line was made and then nothing moved, because the modal
	// never began.
	// In object mode the patch is the thing being moved, so a transform
	// takes every point of it. Refusing there meant G did nothing in the one
	// sub-mode where "move the whole thing" is exactly what is meant.
	if (SubMode == ESurfSubMode::Object && PointSelection.Num() == 0 && SelectedPatch.IsValid())
	{
		SelectAllPoints();
	}

	if (InModal == ESurfModal::None || PointSelection.Num() == 0)
	{
		return false;
	}

	// A second press of the move key turns a move into a slide, which is the
	// gesture for moving a loop without leaving the surface it sits on.
	if (Modal == ESurfModal::Move && InModal == ESurfModal::Move)
	{
		InModal = ESurfModal::Slide;
	}

	// Restore first, so pressing one modal key during another starts from the
	// geometry as it was rather than from a half-finished transform - and
	// before anything below looks at a patch, because the restore can take
	// an extrude's new patches away and leave a pointer to nothing (the
	// 2026-09-13 crash: E, then R). The extrude itself is carried over: E
	// then R rotates what was extruded, and a cancel later still takes the
	// extrude back with it.
	const bool bCarryFollow = bModalFollowsExtrude;
	const int32 CarryPosition = ExtrudeUndoPosition;
	const FString CarryVerb = ModalFollowsVerb;
	if (Modal != ESurfModal::None)
	{
		bModalFollowsExtrude = false;
		CancelModal();

		// Object mode gave its borrowed points back on the cancel; borrow
		// them again for the gesture that replaces it.
		if (SubMode == ESurfSubMode::Object && PointSelection.Num() == 0 && SelectedPatch.IsValid())
		{
			SelectAllPoints();
		}
		if (PointSelection.Num() == 0)
		{
			return false;
		}
	}

	// The active patch carries the gesture - the soft selection and the slide
	// axes belong to it. Any selected points on other patches are remembered
	// separately and carried along by the same world-space transform.
	ASurfGroup* Group = SelectedPatch.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;
	if (!Patch || !Group)
	{
		Group = PointSelection[0].Group.Get();
		Patch = Group ? Group->FindPatch(PointSelection[0].PatchId) : nullptr;
	}
	if (!Patch || !Group)
	{
		return false;
	}

	ModalOthers.Reset();
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Ref.PatchId == Patch->Id)
		{
			continue;
		}

		const ASurfGroup* OtherGroup = Ref.Group.Get();
		const FSurfPatch* Other = OtherGroup ? OtherGroup->FindPatch(Ref.PatchId) : nullptr;
		if (Other && Ref.X >= 0 && Ref.X < Other->Width && Ref.Y >= 0 && Ref.Y < Other->Height)
		{
			FModalOther Entry;
			Entry.Ref = Ref;
			Entry.OriginalWorld = OtherGroup->GetActorTransform().TransformPosition(Other->At(Ref.X, Ref.Y).Pos);
			// A carried loop cut slides between its own neighbours, the way
			// the cut runs (S190a).
			if (InModal == ESurfModal::Slide)
			{
				if (const FIntPoint* Chain = LoopSlideChain.Find(Ref.PatchId))
				{
					const bool bAlongX = Chain->X != 0;
					const int32 Low = bAlongX ? FMath::Max(0, Ref.X - 1) : FMath::Max(0, Ref.Y - 1);
					const int32 High = bAlongX ? FMath::Min(Other->Width - 1, Ref.X + 1) : FMath::Min(Other->Height - 1, Ref.Y + 1);
					const FVector Local = bAlongX
						? Other->At(High, Ref.Y).Pos - Other->At(Low, Ref.Y).Pos
						: Other->At(Ref.X, High).Pos - Other->At(Ref.X, Low).Pos;
					Entry.SlideAxisWorld = OtherGroup->GetActorTransform().TransformVector(Local * 0.5) * static_cast<double>(Chain->Y);
				}
			}
			ModalOthers.Add(Entry);
		}
	}

	// Primitives picked with the patches - a group of both (S249b) - go with
	// the gesture, from their planes as they stand now.
	ModalCarried.Reset();
	if (SubMode == ESurfSubMode::Object)
	{
		TArray<FSurfPieceRef> Pieces;
		GatherPickedPieces(Pieces);
		for (const FSurfPieceRef& Piece : Pieces)
		{
			const ASurfGroup* BrushGroup = Piece.Group.Get();
			const FSurfBrush* Brush = Piece.bBrush && BrushGroup ? BrushGroup->FindBrush(Piece.Id) : nullptr;
			if (!Brush)
			{
				continue;
			}
			FSurfBrushModalTarget Target;
			Target.Group = Piece.Group;
			Target.BrushId = Piece.Id;
			for (const FSurfFace& Face : Brush->Faces)
			{
				Target.Planes.Add(Face.Plane);
			}
			ModalCarried.Add(MoveTemp(Target));
		}
	}

	Modal = InModal;
	AxisLock = ESurfAxisLock::None;
	CustomAxis = FVector::ZeroVector;
	ModalTyped[0].Reset();
	ModalTyped[1].Reset();
	ModalTyped[2].Reset();
	ModalTypedField = 0;
	ModalPatchId = Patch->Id;
	ModalGroup = Group;
	GModalWrap = FVector2D::ZeroVector;
	ModalStartMouse = CurrentMouse(Viewport);
	NoteModalStartRay(ViewportClient, Viewport);
	ModalPivot = (InModal == ESurfModal::Rotate || InModal == ESurfModal::Scale) ? PivotOr(GetWidgetLocation()) : GetWidgetLocation();

	ModalOriginal.Reset(Patch->NumPoints());
	for (const FSurfControlPoint& Point : Patch->Points)
	{
		ModalOriginal.Add(Point.Pos);
	}

	// Slide needs a direction per point: along its own row or column, whichever
	// the selection runs across. A loop slides between its neighbours, so the
	// axis is the line joining them.
	SlideAxes.Reset();
	if (Modal == ESurfModal::Slide)
	{
		// The active patch's points decide (S190a): a carried cut's points on
		// other patches have their own lines.
		bool bSameRow = true;
		bool bSameColumn = true;
		const FSurfPointRef* First = PointSelection.FindByPredicate([Patch](const FSurfPointRef& Ref) { return Ref.PatchId == Patch->Id; });
		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (First && Ref.PatchId == Patch->Id)
			{
				bSameRow    &= (Ref.Y == First->Y);
				bSameColumn &= (Ref.X == First->X);
			}
		}

		// A row - one Y, many X - slides between the rows beside it, which
		// is along Y; a column, along X. It was the other way round, so a
		// loop cut's line slid along itself instead of across, and moving
		// the mouse the way the line should go did nothing (his round,
		// 2026-09-14: "the slide on the other axis"). A single point slides
		// along X, as before.
		const bool bAlongX = bSameRow && !bSameColumn ? false : true;

		SlideAxes.Reserve(PointSelection.Num());
		for (const FSurfPointRef& Ref : PointSelection)
		{
			FVector Axis = FVector::ZeroVector;

			if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				if (bAlongX)
				{
					const int32 Low = FMath::Max(0, Ref.X - 1);
					const int32 High = FMath::Min(Patch->Width - 1, Ref.X + 1);
					Axis = Patch->At(High, Ref.Y).Pos - Patch->At(Low, Ref.Y).Pos;
				}
				else
				{
					const int32 Low = FMath::Max(0, Ref.Y - 1);
					const int32 High = FMath::Min(Patch->Height - 1, Ref.Y + 1);
					Axis = Patch->At(Ref.X, High).Pos - Patch->At(Ref.X, Low).Pos;
				}
			}

			SlideAxes.Add(Axis * 0.5);
		}
	}

	ModalTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("ModalTransform", "Transform Control Points"));
	ModalAppliedDelta = FVector::ZeroVector;
	ModalAppliedRotation = FRotator::ZeroRotator;
	ModalAppliedScale = FVector::OneVector;
	bModalFollowsExtrude = bCarryFollow;
	ExtrudeUndoPosition = bCarryFollow ? CarryPosition : 0;
	ModalFollowsVerb = bCarryFollow ? CarryVerb : FString();

	SurfSession::Gesture(TEXT("modal_begin"), FString::Printf(TEXT("kind=%d points=%d"),
		static_cast<int32>(Modal), PointSelection.Num()));

	return true;
}

bool USurfEdMode::HasTypedValue() const
{
	return ModalTyped[0].Len() > 0 || ModalTyped[1].Len() > 0 || ModalTyped[2].Len() > 0;
}

FVector USurfEdMode::TypedVector() const
{
	double Values[3] = { 0.0, 0.0, 0.0 };

	for (int32 I = 0; I < 3; ++I)
	{
		// An unfilled field is zero, not "leave this axis alone": typing 250
		// into X and nothing else means move 250 along X and nowhere else,
		// which is what it looks like it means.
		if (ModalTyped[I].Len() > 0)
		{
			LexTryParseString(Values[I], *ModalTyped[I]);
		}
	}

	return FVector(Values[0], Values[1], Values[2]);
}

FString USurfEdMode::DescribeModal() const
{
	if (Modal == ESurfModal::None)
	{
		return FString();
	}

	const TCHAR* Which =
		Modal == ESurfModal::Move   ? TEXT("move") :
		Modal == ESurfModal::Rotate ? TEXT("turn") :
		Modal == ESurfModal::Scale  ? TEXT("size") : TEXT("slide");

	const TCHAR* Lock =
		AxisLock == ESurfAxisLock::X    ? TEXT(" X") :
		AxisLock == ESurfAxisLock::Y    ? TEXT(" Y") :
		AxisLock == ESurfAxisLock::Z    ? TEXT(" Z") :
		AxisLock == ESurfAxisLock::NotX ? TEXT(" YZ") :
		AxisLock == ESurfAxisLock::NotY ? TEXT(" XZ") :
		AxisLock == ESurfAxisLock::NotZ ? TEXT(" XY") :
		AxisLock == ESurfAxisLock::Custom ? TEXT(" outward") : TEXT("");

	if (!HasTypedValue())
	{
		return FString::Printf(TEXT("%s%s"), Which, Lock);
	}

	// A move shows all three fields with the one being filled marked, because
	// which field Tab has landed on is the thing you cannot otherwise tell.
	if (Modal == ESurfModal::Move)
	{
		FString Fields;
		for (int32 I = 0; I < 3; ++I)
		{
			const TCHAR* Name = I == 0 ? TEXT("X") : (I == 1 ? TEXT("Y") : TEXT("Z"));
			const FString Value = ModalTyped[I].Len() > 0 ? ModalTyped[I] : FString(TEXT("0"));

			if (I == ModalTypedField)
			{
				Fields += FString::Printf(TEXT("[%s %s] "), Name, *Value);
			}
			else
			{
				Fields += FString::Printf(TEXT("%s %s "), Name, *Value);
			}
		}

		return FString::Printf(TEXT("%s  %scm"), Which, *Fields);
	}

	const TCHAR* Units = Modal == ESurfModal::Rotate ? TEXT(" deg") : TEXT("x");

	return FString::Printf(TEXT("%s%s  %s%s"), Which, Lock, *ModalTyped[0], Units);
}

FVector USurfEdMode::TurnAxis(ESurfAxisLock Lock, const FVector& Custom, const FVector& Forward)
{
	switch (Lock)
	{
	case ESurfAxisLock::X:
	case ESurfAxisLock::NotX: return FVector::ForwardVector;
	case ESurfAxisLock::Y:
	case ESurfAxisLock::NotY: return FVector::RightVector;
	case ESurfAxisLock::Z:
	case ESurfAxisLock::NotZ: return FVector::UpVector;
	case ESurfAxisLock::Custom:
	{
		const FVector Named = Custom.GetSafeNormal();
		return Named.IsNearlyZero() ? FVector::UpVector : Named;
	}
	default: break;
	}

	// Nothing named: the way the camera looks.
	const FVector Looking = Forward.GetSafeNormal();
	return Looking.IsNearlyZero() ? FVector::UpVector : Looking;
}

void USurfEdMode::SetAxisLock(ESurfAxisLock Lock, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (Modal == ESurfModal::None)
	{
		return;
	}

	// Axis, then plane, then free. Pressing the same key again used to just
	// free it, which meant "everything but Z" - as common a thing to want as
	// "only Z" - took two keys and a moment's thought.
	// An axis named while a primitive's side is pulled out after E (S189a):
	// the side moves along that axis instead, as a move of its corners.
	if (bModalOnBrush && bModalPushesFaces)
	{
		bModalPushesFaces = false;
		bModalMovesCorners = true;
	}

	const ESurfAxisLock Plane =
		Lock == ESurfAxisLock::X ? ESurfAxisLock::NotX :
		Lock == ESurfAxisLock::Y ? ESurfAxisLock::NotY :
		Lock == ESurfAxisLock::Z ? ESurfAxisLock::NotZ : ESurfAxisLock::None;

	AxisLock =
		AxisLock == Lock  ? Plane :
		AxisLock == Plane ? ESurfAxisLock::None : Lock;
	UpdateModal(ViewportClient, Viewport);

	SurfSession::Gesture(TEXT("modal_axis"), FString::Printf(TEXT("%d"), static_cast<int32>(AxisLock)));
}

bool USurfEdMode::LockedMoveFromRays(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector& Pivot, const FVector& Axis, const FVector& Excluded, FVector& OutDelta) const
{
	/**
	 * The point under the hand, along the lock (S167b): "moving xy only in g
	 * mode is possible but not practical... it must be smooth like in
	 * blender. when moving mouse i can only reach certain close distances."
	 * A locked move used to take the screen-plane motion and keep its part
	 * along the axis - fine when the axis lies across the screen, a crawl
	 * when it points into it. Now the axis is a line and the plane a plane:
	 * where the mouse ray meets them is where the selection goes, so the
	 * point stays under the cursor whatever the view.
	 */
	if (ModalStartRayDirection.IsNearlyZero())
	{
		return false;
	}
	// The ray through the cursor as the gesture reads it - where it really
	// is plus how far it has been carried round the edges (S248a: "the mouse
	// should go and loop through the screen ... right now it's locked on the
	// edges of the screen"). Past the viewport the view's projection still
	// gives a ray, so the move runs on.
	const FSurfPickContext Now = FSurfPickContext::FromPixel(ViewportClient, Viewport, CurrentMouse(Viewport));
	if (!Now.IsValid())
	{
		return false;
	}
	auto AlongLine = [&Pivot, &Axis](const FVector& O, const FVector& D, double& OutT) -> bool
	{
		// Closest point between the ray O+Ds and the line Pivot+Axis t.
		// W runs from the ray's origin to the line's (S184c): the other way
		// round gave the point mirrored along the axis - "to move that
		// cylinder down I must g+z+mouse upward".
		const FVector W = Pivot - O;
		const double A = 1.0, B = FVector::DotProduct(Axis, D), C = FVector::DotProduct(D, D);
		const double Dd = FVector::DotProduct(Axis, W), E = FVector::DotProduct(D, W);
		const double Denom = A * C - B * B;
		if (Denom < 1.e-4) { return false; } // edge-on
		OutT = (B * E - C * Dd) / Denom;
		return true;
	};
	auto OnPlane = [&Pivot, &Excluded](const FVector& O, const FVector& D, FVector& OutP) -> bool
	{
		const double Denom = FVector::DotProduct(D, Excluded);
		if (FMath::Abs(Denom) < 0.05) { return false; } // grazing
		const double S = FVector::DotProduct(Pivot - O, Excluded) / Denom;
		if (S < 0.0) { return false; }
		OutP = O + D * S;
		return true;
	};
	// Past the horizon, or the line seen end-on (S248a): the last delta the
	// rays gave holds, so a cursor carried on round the screen never throws
	// the piece back to the pixels' reading.
	auto Held = [this, &OutDelta]() -> bool
	{
		if (!bModalRayHeld)
		{
			return false;
		}
		OutDelta = ModalRayHeld;
		return true;
	};
	auto Keep = [this, &OutDelta]() -> bool
	{
		ModalRayHeld = OutDelta;
		bModalRayHeld = true;
		return true;
	};
	if (!Axis.IsZero())
	{
		double T0 = 0.0, T1 = 0.0;
		if (!AlongLine(ModalStartRayOrigin, ModalStartRayDirection, T0))
		{
			return false;
		}
		if (!AlongLine(Now.Origin, Now.Direction, T1))
		{
			return Held();
		}
		OutDelta = Axis * (T1 - T0);
		return Keep();
	}
	if (!Excluded.IsZero())
	{
		FVector P0, P1;
		if (!OnPlane(ModalStartRayOrigin, ModalStartRayDirection, P0))
		{
			return false;
		}
		if (!OnPlane(Now.Origin, Now.Direction, P1))
		{
			return Held();
		}
		OutDelta = P1 - P0;
		return Keep();
	}
	return false;
}

void USurfEdMode::ResetModalMouse(FViewport* Viewport)
{
	ModalStartMouse = CurrentMouse(Viewport);
}

void USurfEdMode::NoteModalStartRay(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// Through the cursor as the gesture reads it, so the first update moves nothing.
	const FSurfPickContext StartRay = FSurfPickContext::FromPixel(ViewportClient, Viewport, CurrentMouse(Viewport));
	ModalStartRayOrigin = StartRay.Origin;
	ModalStartRayDirection = StartRay.IsValid() ? StartRay.Direction : FVector::ZeroVector;
	ModalRayHeld = FVector::ZeroVector;
	bModalRayHeld = false;
}

void USurfEdMode::WrapModalCursor(FViewport* Viewport)
{
	// Every gesture wraps (S248a): a locked move reads the ray through the
	// cursor plus the wraps, which a wrap no longer breaks.
	if (!Viewport)
	{
		return;
	}
	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X < 60 || Size.Y < 60)
	{
		return;
	}
	FVector2D Real;
	FIntPoint Cursor;
	if (ModalCursorPixels(Viewport, Real))
	{
		Cursor = FIntPoint(FMath::RoundToInt(Real.X), FMath::RoundToInt(Real.Y));
	}
	else
	{
		Viewport->GetMousePos(Cursor);
	}
	constexpr int32 Edge = 3;
	constexpr int32 Inset = 16;
	FIntPoint To = Cursor;
	if (Cursor.X <= Edge) { To.X = Size.X - Inset; }
	else if (Cursor.X >= Size.X - 1 - Edge) { To.X = Inset; }
	if (Cursor.Y <= Edge) { To.Y = Size.Y - Inset; }
	else if (Cursor.Y >= Size.Y - 1 - Edge) { To.Y = Inset; }
	if (To != Cursor)
	{
		Viewport->SetMouse(To.X, To.Y);
		GModalWrap += FVector2D(Cursor.X - To.X, Cursor.Y - To.Y);
	}
}

void USurfEdMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{

	/**
	 * A gesture keeps going once the cursor is past the viewport (S187b):
	 * no move reaches the viewport out there, so each frame looks at the
	 * cursor itself, wraps it back in and moves the piece.
	 */
	if (Modal == ESurfModal::None || !ViewportClient || ViewportClient != GCurrentLevelEditingViewportClient || !ViewportClient->Viewport)
	{
		return;
	}
	FViewport* Viewport = ViewportClient->Viewport;
	FVector2D Real;
	if (!ModalCursorPixels(Viewport, Real))
	{
		return;
	}
	const FIntPoint Size = Viewport->GetSizeXY();
	constexpr double Margin = 3.0;
	if (Real.X > Margin && Real.Y > Margin && Real.X < Size.X - 1 - Margin && Real.Y < Size.Y - 1 - Margin)
	{
		return;
	}
	UpdateModal(ViewportClient, Viewport);
	ViewportClient->Invalidate();
}

void USurfEdMode::UpdateModal(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	if (Modal == ESurfModal::None || !ViewportClient || !Viewport)
	{
		return;
	}
	WrapModalCursor(Viewport);

	if (bModalOnPlacement)
	{
		UpdatePlacementModal(ViewportClient, Viewport);
		return;
	}

	if (bModalOnBrush)
	{
		UpdateBrushModal(ViewportClient, Viewport);
		return;
	}

	if (bModalOnActors)
	{
		UpdateActorModal(ViewportClient, Viewport);
		return;
	}

	ASurfGroup* Group = ModalGroup.Get();
	FSurfPatch* Patch = Group ? Group->ModifyPatch(ModalPatchId) : nullptr;

	if (!Patch || Patch->Points.Num() != ModalOriginal.Num())
	{
		return;
	}

	// What this update did to the world, for the points on other patches.
	FVector OthersDelta = FVector::ZeroVector;
	FQuat OthersRotation = FQuat::Identity;
	FVector OthersMultiplier = FVector::OneVector;

	FVector Right, Up, Forward;
	if (!GetViewBasis(ViewportClient, Viewport, Right, Up, Forward))
	{
		return;
	}

	const FVector2D Mouse = CurrentMouse(Viewport);
	const FVector2D Delta = Mouse - ModalStartMouse;

	const FTransform ToWorld = Group->GetActorTransform();
	const double Scale = WorldPerPixel(ViewportClient, ModalPivot);

	// Everything is computed from the cached original, never from the current
	// positions: re-applying to the result is what makes a modal drag drift.
	auto Original = [this, Patch](int32 X, int32 Y) -> FVector
	{
		return ModalOriginal[Y * Patch->Width + X];
	};

	/**
	 * The whole patch back where it began, every frame (S149a).
	 *
	 * "while im in edit mode, example move, and i enable soft select : i
	 * move, then revert by scroll down : the points will have moved ! i need
	 * everything in place no moving by mistake."
	 *
	 * Each branch below writes the points it is moving and no others - which
	 * is right until the set of points it is moving **shrinks**. Turning the
	 * falloff down during a drag does exactly that: a point that was pulled
	 * last frame has no influence this frame, so nothing writes to it and it
	 * keeps last frame's position for the rest of the gesture. Confirm, and
	 * it stays there.
	 *
	 * Putting the patch back first costs a copy of its points per frame and
	 * makes every branch below a statement about where things go rather than
	 * about what it happens to have touched.
	 */
	for (int32 I = 0; I < ModalOriginal.Num(); ++I)
	{
		Patch->Points[I].Pos = ModalOriginal[I];
	}

	switch (Modal)
	{
	case ESurfModal::Move:
	{
		FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * Scale;

		const FVector Axis = AxisVector(AxisLock, CustomAxis);
		const FVector Excluded = PlaneNormal(AxisLock);

		FVector ByRays;
		if ((!Axis.IsZero() || !Excluded.IsZero()) && AxisLock != ESurfAxisLock::Custom
			&& LockedMoveFromRays(ViewportClient, Viewport, ModalPivot, Axis, Excluded, ByRays))
		{
			WorldDelta = ByRays;
		}
		else if (!Axis.IsZero())
		{
			// Project onto the locked axis rather than zeroing the others, so
			// the point tracks the mouse instead of lagging behind it.
			WorldDelta = Axis * FVector::DotProduct(WorldDelta, Axis);
		}
		else if (!Excluded.IsZero())
		{
			WorldDelta -= Excluded * FVector::DotProduct(WorldDelta, Excluded);
		}

		// A typed number wins over the mouse outright. With an axis named it
		// is a distance along that axis; with none, it is the three numbers
		// Tab steps between - which is what removed the old rule that a typed
		// move had to name an axis before it meant anything.
		if (HasTypedValue())
		{
			// A custom direction takes the first number as a distance along
			// it; a world axis takes its own component; free takes all three.
			WorldDelta = Axis.IsZero()
				? TypedVector()
				: (AxisLock == ESurfAxisLock::Custom
					? Axis * TypedVector().X
					: Axis * TypedVector()[Axis.X != 0.0 ? 0 : (Axis.Y != 0.0 ? 1 : 2)]);
		}

		// The snap is bound by the same lock as the move (S150a).
		SnapAlongAxis = Axis;
		SnapExcludedAxis = Excluded;

		const FVector LocalDelta = ToWorld.InverseTransformVector(WorldDelta);
		OthersDelta = WorldDelta;
		ModalAppliedDelta = WorldDelta;

		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				Patch->At(Ref.X, Ref.Y).Pos = Original(Ref.X, Ref.Y) + LocalDelta;
			}
		}

		// A modal move snaps for the same reason a gizmo drag does: the point
		// of moving an edge across the level is almost always to put it on
		// something. One correction for the whole selection, so what is being
		// dragged arrives with its shape intact.
		// Where the selection was before this gesture, so it cannot be snapped
		// straight back onto a point it is being pulled off.
		TArray<FVector> MovingFrom;
		MovingFrom.Reserve(PointSelection.Num());
		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				MovingFrom.Add(ToWorld.TransformPosition(Original(Ref.X, Ref.Y)));
			}
		}

		// The point under the cursor first (S157a, A18); the reach in
		// centimetres otherwise.
		FVector Correction = FVector::ZeroVector;
		if (!FindCursorSnap(ViewportClient, Viewport, Mouse, *Group, *Patch, MovingFrom, Correction))
		{
			Correction = FindPointSnap(*Group, *Patch, MovingFrom);
		}
		if (!Correction.IsNearlyZero())
		{
			const FVector LocalCorrection = ToWorld.InverseTransformVector(Correction);

			for (const FSurfPointRef& Ref : PointSelection)
			{
				if (Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
				{
					Patch->At(Ref.X, Ref.Y).Pos += LocalCorrection;
				}
			}
		}

		if (SoftInfluence.Num() == Patch->NumPoints())
		{
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch->Width + X];
					if (Influence > 0.0f)
					{
						Patch->At(X, Y).Pos = Original(X, Y) + LocalDelta * Influence;
					}
				}
			}
		}

		/**
		 * The continuity suggestion (S159a, his idea): while a whole border
		 * is on the move and comes within reach of another patch's border
		 * with the same count, that border is drawn as a ghost; confirming
		 * the move takes it - Continue runs, the seam meets and the rows
		 * behind lean into it. "Yes, show and confirm. Nothing happens on
		 * its own."
		 */
		{
			const USurfSettings* Offer = GetDefault<USurfSettings>();
			bContinueOffered = Offer && Offer->bContinueOffers && ContinueSelectedBorder(/*bDryRun*/ true, &ContinueOffer);
			if (!bContinueOffered)
			{
				ContinueOffer.Reset();
			}
		}
		break;
	}

	case ESurfModal::Slide:
	{
		// How far along the line, from -1 at the previous neighbour to +1 at
		// the next - measured along the slide axis as it appears on screen,
		// not along the mouse's X. Using Delta.X meant a line running up the
		// screen could only be pushed sideways, and moving the mouse the way
		// the line actually ran did nothing.
		FVector2D ScreenAxis(1.0, 0.0);

		if (SlideAxes.Num() > 0 && PointSelection.Num() > 0)
		{
			const FVector Axis = ToWorld.TransformVector(SlideAxes[0]);
			const FVector2D Projected(
				FVector::DotProduct(Axis, Right),
				-FVector::DotProduct(Axis, Up));

			if (!Projected.IsNearlyZero())
			{
				ScreenAxis = Projected.GetSafeNormal();
			}
		}

		const double Wanted = FMath::Clamp(FVector2D::DotProduct(Delta, ScreenAxis) / 200.0, -1.0, 1.0);

		// Put where asked, then where the snap says (S150f): a loop cut
		// lands on a neighbouring patch's line when snap is on.
		const double Amount = SnappedSlideAmount(*Group, *Patch, SlideAxes, Original, Wanted);
		ModalSlideAmount = Amount;
		if (Inspector && !FMath::IsNearlyEqual(Amount, Wanted, 1.e-6))
		{
			Inspector->SetResult(FString::Printf(TEXT("slide: snapped at %.0f%% of the way"), Amount * 100.0));
		}
		break;
	}

	case ESurfModal::Rotate:
	{
		// The angle the mouse has swept around the pivot on screen.
		FVector2D PivotScreen = ModalStartMouse;
		{
			FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
				Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));

			if (FSceneView* View = ViewportClient->CalcSceneView(&Family))
			{
				View->WorldToPixel(ModalPivot, PivotScreen);
			}
		}

		const FVector2D From = ModalStartMouse - PivotScreen;
		const FVector2D To = Mouse - PivotScreen;

		if (From.IsNearlyZero() || To.IsNearlyZero())
		{
			break;
		}

		double Angle = FMath::Atan2(To.Y, To.X) - FMath::Atan2(From.Y, From.X);

		// Typed in degrees, which is how an angle is said out loud.
		double TypedAngle = 0.0;
		if (ModalTyped[0].Len() > 0 && LexTryParseString(TypedAngle, *ModalTyped[0]))
		{
			Angle = -FMath::DegreesToRadians(TypedAngle);
		}

		// A plane lock names its own normal, which is the axis to turn about.
		const FVector Named = AxisVector(AxisLock, CustomAxis) + PlaneNormal(AxisLock);

		FVector WorldAxis = Named.IsZero() ? Forward : Named;
		double Turn = -Angle;

		// With no axis named, the selection behaves like a ball under the
		// cursor: the axis is across the drag, so dragging sideways brings the
		// far side towards you. Twisting about the view direction - which is
		// what a swept angle gives - is only what the gesture looks like when
		// the thing being turned is flat on the screen.
		const USurfSettings* Turning = GetDefault<USurfSettings>();
		const bool bTrackball = Named.IsZero()
			&& (!Turning || Turning->bTrackballRotate)
			&& !HasTypedValue();

		if (bTrackball)
		{
			const FVector Dragged = (Right * Delta.X - Up * Delta.Y);

			if (!Dragged.IsNearlyZero())
			{
				WorldAxis = FVector::CrossProduct(Forward, Dragged).GetSafeNormal();

				// A drag across half the viewport is half a turn, which is
				// about what a ball of that size would do under a finger.
				const double Across = FMath::Max(200.0, static_cast<double>(Viewport->GetSizeXY().Y));
				Turn = (Delta.Size() / Across) * PI;

				if (WorldAxis.IsNearlyZero())
				{
					WorldAxis = Forward;
					Turn = -Angle;
				}
			}
			else
			{
				Turn = 0.0;
			}
		}

		const FQuat WorldRotation(WorldAxis.GetSafeNormal(), Turn);
		OthersRotation = WorldRotation;
		ModalAppliedRotation = WorldRotation.Rotator();

		const FVector LocalPivot = ToWorld.InverseTransformPosition(ModalPivot);
		const FQuat LocalRotation = ToWorld.GetRotation().Inverse() * WorldRotation * ToWorld.GetRotation();

		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				Patch->At(Ref.X, Ref.Y).Pos = LocalPivot + LocalRotation.RotateVector(Original(Ref.X, Ref.Y) - LocalPivot);
			}
		}

		if (SoftInfluence.Num() == Patch->NumPoints())
		{
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch->Width + X];
					if (Influence > 0.0f)
					{
						const FVector Turned = LocalPivot + LocalRotation.RotateVector(Original(X, Y) - LocalPivot);
						Patch->At(X, Y).Pos = FMath::Lerp(Original(X, Y), Turned, Influence);
					}
				}
			}
		}
		break;
	}

	case ESurfModal::Scale:
	{
		FVector2D PivotScreen = ModalStartMouse;
		{
			FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
				Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));

			if (FSceneView* View = ViewportClient->CalcSceneView(&Family))
			{
				View->WorldToPixel(ModalPivot, PivotScreen);
			}
		}

		// The ratio of the two distances from the pivot: move away to grow,
		// towards it to shrink, exactly as it looks.
		const double Start = FMath::Max(1.0, FVector2D::Distance(ModalStartMouse, PivotScreen));
		const double Now = FVector2D::Distance(Mouse, PivotScreen);
		double Factor = FMath::Clamp(Now / Start, 0.001, 1000.0);

		// Typed as a plain multiplier: 2 is twice, 0.5 is half.
		double TypedFactor = 0.0;
		if (ModalTyped[0].Len() > 0 && LexTryParseString(TypedFactor, *ModalTyped[0]) && TypedFactor != 0.0)
		{
			Factor = FMath::Clamp(TypedFactor, -1000.0, 1000.0);
		}

		FVector Multiplier(Factor, Factor, Factor);

		const FVector ScaleAxis = AxisVector(AxisLock, CustomAxis);
		const FVector ScaleExcluded = PlaneNormal(AxisLock);

		if (!ScaleAxis.IsZero())
		{
			Multiplier = FVector::OneVector + ScaleAxis * (Factor - 1.0);
		}
		else if (!ScaleExcluded.IsZero())
		{
			// A plane lock scales the two axes it keeps and leaves the third.
			Multiplier = FVector(Factor, Factor, Factor)
				+ ScaleExcluded * (1.0 - Factor);
		}

		const FVector LocalPivot = ToWorld.InverseTransformPosition(ModalPivot);
		OthersMultiplier = Multiplier;
		ModalAppliedScale = Multiplier;

		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				Patch->At(Ref.X, Ref.Y).Pos = LocalPivot + (Original(Ref.X, Ref.Y) - LocalPivot) * Multiplier;
			}
		}

		if (SoftInfluence.Num() == Patch->NumPoints())
		{
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch->Width + X];
					if (Influence > 0.0f)
					{
						const FVector Scaled = LocalPivot + (Original(X, Y) - LocalPivot) * Multiplier;
						Patch->At(X, Y).Pos = FMath::Lerp(Original(X, Y), Scaled, Influence);
					}
				}
			}
		}
		break;
	}

	default:
		break;
	}

	SurfPatchSurfacing::Reapply(*Patch);
	++Patch->EditSerial;

	// The points on other patches follow by the same world transform, from
	// their own originals, so a selection across two patches moves as one.
	TArray<ASurfGroup*> OtherGroups;
	for (const FModalOther& Entry : ModalOthers)
	{
		ASurfGroup* OtherGroup = Entry.Ref.Group.Get();
		FSurfPatch* Other = OtherGroup ? OtherGroup->ModifyPatch(Entry.Ref.PatchId) : nullptr;
		if (!Other || Entry.Ref.X < 0 || Entry.Ref.X >= Other->Width || Entry.Ref.Y < 0 || Entry.Ref.Y >= Other->Height)
		{
			continue;
		}

		FVector World = Entry.OriginalWorld;
		switch (Modal)
		{
		case ESurfModal::Move:   World += OthersDelta; break;
		case ESurfModal::Rotate: World = ModalPivot + OthersRotation.RotateVector(World - ModalPivot); break;
		case ESurfModal::Scale:  World = ModalPivot + (World - ModalPivot) * OthersMultiplier; break;
		case ESurfModal::Slide:  World += Entry.SlideAxisWorld * ModalSlideAmount; break;
		default: break;
		}

		Other->At(Entry.Ref.X, Entry.Ref.Y).Pos = OtherGroup->GetActorTransform().InverseTransformPosition(World);
		SurfPatchSurfacing::Reapply(*Other);
		++Other->EditSerial;
		OtherGroups.AddUnique(OtherGroup);
	}

	// The primitives carried with them (S249b), by the same world transform,
	// from their planes as they were.
	if (ModalCarried.Num() > 0)
	{
		FMatrix Carry = FMatrix::Identity;
		switch (Modal)
		{
		case ESurfModal::Move:   Carry = FTranslationMatrix(OthersDelta); break;
		case ESurfModal::Rotate: Carry = FTranslationMatrix(-ModalPivot) * FQuatRotationMatrix(OthersRotation) * FTranslationMatrix(ModalPivot); break;
		case ESurfModal::Scale:  Carry = FTranslationMatrix(-ModalPivot) * FScaleMatrix(OthersMultiplier) * FTranslationMatrix(ModalPivot); break;
		default: break;
		}
		for (const FSurfBrushModalTarget& Target : ModalCarried)
		{
			ASurfGroup* BrushGroup = Target.Group.Get();
			FSurfBrush* Brush = BrushGroup ? BrushGroup->ModifyBrush(Target.BrushId) : nullptr;
			if (!Brush || Brush->Faces.Num() != Target.Planes.Num())
			{
				continue;
			}
			const FTransform BrushToWorld = BrushGroup->GetActorTransform();
			const FMatrix Local = BrushToWorld.ToMatrixWithScale() * Carry * BrushToWorld.Inverse().ToMatrixWithScale();
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				Brush->Faces[I].Plane = Target.Planes[I].TransformBy(Local);
			}
			++Brush->EditSerial;
			OtherGroups.AddUnique(BrushGroup);
		}
	}

	// The walls an extrude made are shown facing the way they will be kept,
	// as the pull gives them their height, rather than the way they were
	// guessed before it - a wall seen inside out for the whole drag reads as
	// a wrong result, even when the confirm would have turned it.
	if (bModalFollowsExtrude && ModalFollowsVerb == TEXT("extrude") && ExtrudeWallFacing.Num() > 0)
	{
		TurnExtrudedWallsOut(/*bPreview*/ true);
	}

	// Rebuilding on every mouse move is what makes a drag feel heavy: the
	// preview is a full mesh build, and the mouse moves faster than it. Cap it
	// to a rate the eye reads as continuous and let the release do the last
	// one, so the geometry is always correct when the gesture ends.
	const double Now = FPlatformTime::Seconds();
	if ((Now - LastModalRebuild) >= 1.0 / 60.0)
	{
		LastModalRebuild = Now;
		Group->RebuildPreview();
		for (ASurfGroup* OtherGroup : OtherGroups)
		{
			if (OtherGroup != Group)
			{
				OtherGroup->RebuildPreview();
			}
		}

		// Invalidate the viewport in hand rather than redrawing every one of
		// them; the others are not looking at this. The display only: the
		// hit proxies are refreshed when the gesture ends, not on every
		// move of it (each refresh is the scene rendered once more).
		ViewportClient->Invalidate(/*bInvalidateChildViews*/ false, /*bInvalidateHitProxies*/ false);
	}
}

void USurfEdMode::ConfirmModal()
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	if (Modal == ESurfModal::None)
	{
		return;
	}

	// The actors have already been moved; keeping it is letting the
	// transaction close (S94a).
	if (bModalOnActors)
	{
		bModalOnActors = false;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		for (const TWeakObjectPtr<AActor>& Weak : ModalActors)
		{
			if (AActor* Actor = Weak.Get())
			{
				Actor->PostEditMove(/*bFinished*/ true);
			}
		}
		ModalActors.Reset();
		ModalActorStart.Reset();
		ModalTransaction.Reset();
		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Gesture(TEXT("actor_modal"), TEXT("confirm"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	// A brush drag has already written its planes; keeping it is letting the
	// transaction close (S72c).
	if (bModalOnBrush)
	{
		bModalOnBrush = false;
		bModalPushesFaces = false;
		bModalScalesFaces = false;
		bModalMovesCorners = false;
		SnapAlongAxis = FVector::ZeroVector;
		SnapExcludedAxis = FVector::ZeroVector;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		ModalBrushes.Reset();
		ModalTransaction.Reset();
		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Gesture(TEXT("brush_modal"), TEXT("confirm"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	// A placement drag has already written where it stands; keeping it is
	// letting the transaction close (S71d).
	if (bModalOnPlacement)
	{
		bModalOnPlacement = false;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		ModalPlacementAdded.Invalidate();
		ModalTransaction.Reset();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("placement kept - Alt+PgUp saves the prefab it is in"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Gesture(TEXT("placement_modal"), TEXT("confirm"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	// The throttle above may have skipped the last move, so the geometry is
	// rebuilt once here where it is cheap and correctness matters.
	if (ASurfGroup* Group = ModalGroup.Get())
	{
		Group->RebuildPreview();
	}
	for (const FModalOther& Entry : ModalOthers)
	{
		if (ASurfGroup* OtherGroup = Entry.Ref.Group.Get())
		{
			OtherGroup->RebuildPreview();
		}
	}
	ModalOthers.Reset();
	for (const FSurfBrushModalTarget& Target : ModalCarried)
	{
		if (ASurfGroup* BrushGroup = Target.Group.Get())
		{
			BrushGroup->RebuildPreview();
		}
	}
	ModalCarried.Reset();

	SurfSession::Gesture(TEXT("modal_confirm"), FString::Printf(TEXT("kind=%d"), static_cast<int32>(Modal)));

	// The walls an extrude made were turned to face out before they had any
	// height - guessed from the way the surface continues. Now that the move
	// has given them their height, each is checked against what it was made
	// from and turned if it faces in: a wall pulled the other way used to
	// come out facing the wrong way. Inside the move's transaction, so undo
	// takes it with the move.
	bool bPulledAlong = false;
	if (bModalFollowsExtrude && ModalFollowsVerb == TEXT("extrude"))
	{
		TurnExtrudedWallsOut();
		bPulledAlong = bExtrudePulledAlongSurface;
		// The grown line has its length now: its UVs carry on by it (S207b).
		if (ASurfGroup* Grown = GrownGroup.Get())
		{
			CarryGrownUVs();
			if (FSurfPatch* Patch = Grown->ModifyPatch(GrownPatch))
			{
				SurfPatchSurfacing::Reapply(*Patch);
			}
			Grown->RebuildPreview();
		}
		GrownGroup.Reset();
	}

	// A line moved fills what it stretched, when asked to (S150d) - inside
	// the move's transaction, so one undo takes both.
	int32 Densified = 0;
	if (Modal == ESurfModal::Move && !ModalAppliedDelta.IsNearlyZero(0.01))
	{
		Densified = DensifyStretchedCells();
	}

	// The transaction closing is what turns the whole gesture into one undo
	// step; the geometry is already where it should be.
	ModalTransaction.Reset();

	const ESurfModal Done = Modal;
	Modal = ESurfModal::None;

	// An extrude confirmed without moving leaves walls of no height and lids
	// on top of what they came from - geometry with edges and no surface,
	// which is what "ghosts" were. Nothing moved, so nothing was wanted. A
	// block pulled along its own surface rather than out of it is the same
	// thing with the lid moved sideways: a box with no height.
	const bool bWentNowhere =
		(Done == ESurfModal::Move && ModalAppliedDelta.IsNearlyZero(0.01))
		|| (Done == ESurfModal::Rotate && ModalAppliedRotation.IsNearlyZero(0.01))
		|| (Done == ESurfModal::Scale && ModalAppliedScale.Equals(FVector::OneVector, 0.001))
		|| bPulledAlong;
	// Remembered before the flag is cleared: the note below needs to know.
	const bool bFollowedExtrude = bModalFollowsExtrude && ModalFollowsVerb == TEXT("extrude");
	const bool bFollowedAny = bModalFollowsExtrude;
	const int32 FollowedFrom = ExtrudeUndoPosition;

	if (Densified > 0 && Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("moved, and %d line%s went in to keep the density"),
			Densified, Densified == 1 ? TEXT("") : TEXT("s")));
	}

	// The offered border taken (S159a): one more transaction, after the move's.
	const bool bTakeContinue = bContinueOffered && Done == ESurfModal::Move && !bWentNowhere;
	bContinueOffered = false;
	ContinueOffer.Reset();
	if (bTakeContinue)
	{
		ContinueSelectedBorder();
	}

	if (bModalFollowsExtrude && bWentNowhere)
	{
		bModalFollowsExtrude = false;
		UndoBackTo(ExtrudeUndoPosition);
		if (Inspector)
		{
			Inspector->SetResult(bPulledAlong
				? TEXT("extrude: pulled along the surface, not out of it - a block with no height, so nothing was made (pull along the normal, or X Y Z)")
				: FString::Printf(TEXT("%s: nothing moved, so nothing was made"),
					ModalFollowsVerb.IsEmpty() ? TEXT("extrude") : *ModalFollowsVerb));
		}
		if (bPulledAlong)
		{
			SurfSession::Friction(TEXT("extrude_pulled_along_surface"));
		}
		return;
	}
	bModalFollowsExtrude = false;

	// After E or Shift+D, the extrude and this pull undo together (S244a).
	if (bFollowedAny && UndoPosition() > FollowedFrom + 1)
	{
		FSurfUndoPair Pair;
		Pair.From = FollowedFrom;
		Pair.Top = UndoPosition();
		UndoPairs.Add(Pair);
		while (UndoPairs.Num() > 16)
		{
			UndoPairs.RemoveAt(0);
		}
	}

	// What was done, with its numbers, so it can be adjusted afterwards.
	// After E, the extrude and its move are one thing to adjust: the move's
	// X Y Z, and whether the faces beneath were dropped.
	FSurfOperation Op;
	switch (Done)
	{
	case ESurfModal::Move:   Op.Kind = ESurfOpKind::Move;   Op.Delta = ModalAppliedDelta; break;
	case ESurfModal::Rotate: Op.Kind = ESurfOpKind::Rotate; Op.Rotation = ModalAppliedRotation; break;
	case ESurfModal::Scale:  Op.Kind = ESurfOpKind::Scale;  Op.Scale = ModalAppliedScale; break;
	default: break;
	}
	if (Op.Kind == ESurfOpKind::Move && bFollowedExtrude)
	{
		Op.Kind = ESurfOpKind::Extrude;
		Op.bMoved = true;
		Op.Under = LastExtrudeUnder;
		Op.UndoSteps = FMath::Max(1, UndoPosition() - FollowedFrom);
	}
	if (Op.Kind != ESurfOpKind::None)
	{
		NoteOperation(Op);
	}

	// Object mode borrowed every point for the gesture; give them back.
	if (SubMode == ESurfSubMode::Object)
	{
		ClearPointSelection();
	}
	AxisLock = ESurfAxisLock::None;
	ModalOriginal.Reset();
	SlideAxes.Reset();

	// An extrude's move is over: soft selection is back for whatever comes next.
	if (bSoftSuspended)
	{
		bSoftSuspended = false;
		RefreshSoftSelection();
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

void USurfEdMode::CancelModal()
{
	// An offer goes with the gesture (S159a).
	bContinueOffered = false;
	ContinueOffer.Reset();

	if (Modal == ESurfModal::None)
	{
		return;
	}

	if (bModalOnActors)
	{
		bModalOnActors = false;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		for (int32 I = 0; I < ModalActors.Num() && I < ModalActorStart.Num(); ++I)
		{
			if (AActor* Actor = ModalActors[I].Get())
			{
				Actor->SetActorTransform(ModalActorStart[I]);
				Actor->PostEditMove(/*bFinished*/ true);
			}
		}
		ModalActors.Reset();
		ModalActorStart.Reset();
		if (ModalTransaction)
		{
			ModalTransaction->Cancel();
			ModalTransaction.Reset();
		}
		SurfSession::Gesture(TEXT("actor_modal"), TEXT("cancel"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	if (bModalOnBrush)
	{
		bModalOnBrush = false;
		bModalPushesFaces = false;
		bModalScalesFaces = false;
		bModalMovesCorners = false;
		SnapAlongAxis = FVector::ZeroVector;
		SnapExcludedAxis = FVector::ZeroVector;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		for (const FSurfBrushModalTarget& Target : ModalBrushes)
		{
			ASurfGroup* Group = Target.Group.Get();
			FSurfBrush* Brush = Group ? Group->ModifyBrush(Target.BrushId) : nullptr;
			if (Brush && Brush->Faces.Num() == Target.Planes.Num())
			{
				for (int32 I = 0; I < Brush->Faces.Num(); ++I)
				{
					Brush->Faces[I].Plane = Target.Planes[I];
				}
				++Brush->EditSerial;
			}
		}
		ModalBrushes.Reset();
		RebuildTouchedGroups();
		if (ModalTransaction)
		{
			ModalTransaction->Cancel();
			ModalTransaction.Reset();
		}
		SurfSession::Gesture(TEXT("brush_modal"), TEXT("cancel"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	if (bModalOnPlacement)
	{
		bModalOnPlacement = false;
		Modal = ESurfModal::None;
		AxisLock = ESurfAxisLock::None;
		CustomAxis = FVector::ZeroVector;
		if (ASurfPrefabInstance* Drilled = DrilledInstance.Get())
		{
			if (USurfPrefab* RoomAsset = Drilled->Prefab.Get())
			{
				// A copy the gesture made goes back with the gesture: a
				// cancelled Shift+D must not leave a second placement sitting
				// exactly on the first, where nothing would show it.
				if (ModalPlacementAdded.IsValid())
				{
					RoomAsset->RemovePlacement(ModalPlacementAdded);
					RoomPickedPlacement = ModalPlacementId == ModalPlacementAdded ? FGuid() : ModalPlacementId;
				}
				else if (FSurfPlacement* Placement = RoomAsset->ModifyPlacement(ModalPlacementId))
				{
					Placement->Transform = ModalPlacementStart;
				}
				if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
				{
					StandIn->RebuildPreview();
				}
			}
		}
		ModalPlacementAdded.Invalidate();
		if (ModalTransaction)
		{
			ModalTransaction->Cancel();
			ModalTransaction.Reset();
		}
		SurfSession::Gesture(TEXT("placement_modal"), TEXT("cancel"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return;
	}

	ASurfGroup* Group = ModalGroup.Get();

	if (FSurfPatch* Patch = Group ? Group->ModifyPatch(ModalPatchId) : nullptr)
	{
		if (Patch->Points.Num() == ModalOriginal.Num())
		{
			for (int32 I = 0; I < ModalOriginal.Num(); ++I)
			{
				Patch->Points[I].Pos = ModalOriginal[I];
			}

			SurfPatchSurfacing::Reapply(*Patch);
			Group->RebuildPreview();
		}
	}

	for (const FModalOther& Entry : ModalOthers)
	{
		ASurfGroup* OtherGroup = Entry.Ref.Group.Get();
		FSurfPatch* Other = OtherGroup ? OtherGroup->ModifyPatch(Entry.Ref.PatchId) : nullptr;
		if (Other && Entry.Ref.X >= 0 && Entry.Ref.X < Other->Width && Entry.Ref.Y >= 0 && Entry.Ref.Y < Other->Height)
		{
			Other->At(Entry.Ref.X, Entry.Ref.Y).Pos = OtherGroup->GetActorTransform().InverseTransformPosition(Entry.OriginalWorld);
			SurfPatchSurfacing::Reapply(*Other);
			OtherGroup->RebuildPreview();
		}
	}
	ModalOthers.Reset();
	for (const FSurfBrushModalTarget& Target : ModalCarried)
	{
		ASurfGroup* BrushGroup = Target.Group.Get();
		FSurfBrush* Brush = BrushGroup ? BrushGroup->ModifyBrush(Target.BrushId) : nullptr;
		if (Brush && Brush->Faces.Num() == Target.Planes.Num())
		{
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				Brush->Faces[I].Plane = Target.Planes[I];
			}
			++Brush->EditSerial;
			BrushGroup->RebuildPreview();
		}
	}
	ModalCarried.Reset();

	SurfSession::Gesture(TEXT("modal_cancel"), FString::Printf(TEXT("kind=%d"), static_cast<int32>(Modal)));

	// Cancelled rather than closed: the undo stack should have no memory of a
	// gesture that was abandoned.
	if (ModalTransaction)
	{
		ModalTransaction->Cancel();
		ModalTransaction.Reset();
	}

	Modal = ESurfModal::None;

	// Object mode borrowed every point for the gesture; give them back.
	if (SubMode == ESurfSubMode::Object)
	{
		ClearPointSelection();
	}
	AxisLock = ESurfAxisLock::None;
	ModalOriginal.Reset();
	SlideAxes.Reset();

	// Cancelling the move that follows an extrude cancels the extrude: its
	// new lines sit exactly on the old ones and would stay there as walls of
	// no height, edges with no surface.
	if (bModalFollowsExtrude)
	{
		bModalFollowsExtrude = false;
		UndoBackTo(ExtrudeUndoPosition);
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s cancelled"),
				ModalFollowsVerb.IsEmpty() ? TEXT("extrude") : *ModalFollowsVerb));
		}
	}

	if (bSoftSuspended)
	{
		bSoftSuspended = false;
		RefreshSoftSelection();
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

// bPreview: called on every move of the pull that follows an extrude, so the
// walls are seen facing the way they will be kept rather than the way they
// were guessed before they had any height (his note: "normal was reverse all
// along until i clicked and applied"). Only the walls whose facing is read
// off their own normal are touched - that check is stable frame to frame.
// Nothing is dropped and nothing is reset; the confirm does all of it once.
int32 USurfEdMode::TurnExtrudedWallsOut(bool bPreview)
{
	int32 Turned = 0;
	TArray<ASurfGroup*> Groups;
	bExtrudePulledAlongSurface = false;
	TArray<FSurfPatchRef> Dropped;
	int32 WallsSeen = 0;
	int32 LidsSeen = 0;

	auto AreaOf = [](const FSurfPatch& P)
	{
		double Area = 0.0;
		for (int32 Y = 0; Y + 1 < P.Height; ++Y)
		{
			for (int32 X = 0; X + 1 < P.Width; ++X)
			{
				const FVector& P00 = P.At(X, Y).Pos;
				const FVector& P11 = P.At(X + 1, Y + 1).Pos;
				Area += FVector::CrossProduct(P.At(X + 1, Y).Pos - P00, P.At(X, Y + 1).Pos - P00).Size() * 0.5;
				Area += FVector::CrossProduct(P.At(X, Y + 1).Pos - P11, P.At(X + 1, Y).Pos - P11).Size() * 0.5;
			}
		}
		return Area;
	};

	for (const FExtrudeFacing& Entry : ExtrudeWallFacing)
	{
		ASurfGroup* PatchGroup = Entry.Patch.Group.Get();
		FSurfPatch* Patch = PatchGroup ? PatchGroup->ModifyPatch(Entry.Patch.PatchId) : nullptr;
		if (!Patch || Patch->Points.Num() == 0)
		{
			continue;
		}

		// How the piece was pulled from where it was made: along the
		// surface's normal (out, or in), or across it.
		FVector Centre = FVector::ZeroVector;
		for (const FSurfControlPoint& Point : Patch->Points)
		{
			Centre += Point.Pos;
		}
		Centre /= Patch->Points.Num();
		const FVector Pull = Centre - Entry.Base;
		const double Along = Entry.Normal.IsNearlyZero() ? 0.0 : FVector::DotProduct(Pull, Entry.Normal);
		const bool bPulledIn = Along < -0.05;

		if (bPreview && (Entry.bLid || Entry.bOutline))
		{
			continue;
		}

		if (Entry.bLid)
		{
			++LidsSeen;
			// A lid pulled along its surface rather than out of it leaves a
			// block with no height and walls folded onto themselves - the
			// "corrupted mesh". The caller takes the extrude back.
			const double Lateral = (Pull - Entry.Normal * Along).Size();
			if (FMath::Abs(Along) < 0.1 && Lateral > 0.1)
			{
				bExtrudePulledAlongSurface = true;
			}
			// A box's lid was made facing as its source and, pulled in,
			// faces the other way. A pit's floor keeps facing up.
			if (Entry.bBox && bPulledIn)
			{
				Patch->bInvert = !Patch->bInvert;
				++Patch->EditSerial;
				Groups.AddUnique(PatchGroup);
				++Turned;
			}
			continue;
		}

		// A wall pulled along itself has no surface: dropped, and said so.
		++WallsSeen;
		if (!bPreview && AreaOf(*Patch) < 0.5)
		{
			Dropped.Add(Entry.Patch);
			Groups.AddUnique(PatchGroup);
			continue;
		}

		// An outline's wall was made with its cells on the left, so pulled
		// out of the surface it faces out of them and pulled in, into them
		// (a pit's walls are seen from inside) - whatever the shape, a hole's
		// wall included, which no reference point could tell. Only a box's
		// wall pulled in is turned, as its lid is: a box hangs under its
		// surface, walls out. An edge's wall pulled out of the surface
		// faces away from its patch; pulled within the surface (a floor
		// grown past its edge) it faces as the surface does - "away from
		// the patch" means nothing for a piece in the same plane, and that
		// guess is what needed a Flip afterwards.
		bool bTurn = false;
		if (Entry.bOutline)
		{
			bTurn = Entry.bBox && bPulledIn;
		}
		else
		{
			const FVector WallNormal = SurfPatchSurfacing::AverageNormal(*Patch).GetSafeNormal();
			const double Agreement = Entry.Normal.IsNearlyZero() ? 0.0 : FVector::DotProduct(WallNormal, Entry.Normal);
			bTurn = FMath::Abs(Agreement) > 0.7
				? Agreement < 0.0
				: !PatchFacesAwayFrom(*Patch, Entry.Reference);
		}
		if (bTurn)
		{
			Patch->bInvert = !Patch->bInvert;
			++Patch->EditSerial;
			Groups.AddUnique(PatchGroup);
			++Turned;
		}
	}

	if (bPreview)
	{
		return Turned;
	}

	// A piece closing a box faces away from the lid it closes, now that the
	// lid has been pulled off it.
	for (const TPair<FSurfPatchRef, FSurfPatchRef>& Entry : ExtrudeClosingPieces)
	{
		ASurfGroup* PieceGroup = Entry.Key.Group.Get();
		const ASurfGroup* LidGroup = Entry.Value.Group.Get();
		FSurfPatch* Piece = PieceGroup ? PieceGroup->ModifyPatch(Entry.Key.PatchId) : nullptr;
		const FSurfPatch* Lid = LidGroup ? LidGroup->FindPatch(Entry.Value.PatchId) : nullptr;
		if (!Piece || !Lid || Lid->Points.Num() == 0)
		{
			continue;
		}

		FVector LidCentre = FVector::ZeroVector;
		for (const FSurfControlPoint& Point : Lid->Points)
		{
			LidCentre += Point.Pos;
		}
		LidCentre = PieceGroup->GetActorTransform().InverseTransformPosition(
			LidGroup->GetActorTransform().TransformPosition(LidCentre / Lid->Points.Num()));

		if (!PatchFacesAwayFrom(*Piece, LidCentre))
		{
			Piece->bInvert = !Piece->bInvert;
			++Piece->EditSerial;
			Groups.AddUnique(PieceGroup);
			++Turned;
		}
	}

	// Every wall pulled along itself and nothing else made: the extrude
	// made nothing, and the caller takes it back whole.
	if (Dropped.Num() > 0 && Dropped.Num() == WallsSeen && LidsSeen == 0 && ExtrudeClosingPieces.Num() == 0)
	{
		bExtrudePulledAlongSurface = true;
	}

	for (const FSurfPatchRef& Gone : Dropped)
	{
		if (ASurfGroup* GoneGroup = Gone.Group.Get())
		{
			GoneGroup->RemovePatch(Gone.PatchId);
		}
		for (int32 I = PointSelection.Num() - 1; I >= 0; --I)
		{
			if (PointSelection[I].PatchId == Gone.PatchId)
			{
				PointSelection.RemoveAt(I);
			}
		}
		if (SelectedPatch.PatchId == Gone.PatchId)
		{
			SelectedPatch = FSurfPatchRef();
		}
	}
	if (Dropped.Num() > 0)
	{
		SyncPatchSelectionToPoints();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("extrude: %d wall%s pulled along %s - no surface, so dropped"),
				Dropped.Num(), Dropped.Num() == 1 ? TEXT("") : TEXT("s"), Dropped.Num() == 1 ? TEXT("itself") : TEXT("themselves")));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_wall_dropped"), FString::Printf(TEXT("walls=%d"), Dropped.Num()));
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}
	ExtrudeWallFacing.Reset();
	ExtrudeClosingPieces.Reset();
	return Turned;
}

int32 USurfEdMode::UndoPosition() const
{
	// Where the history stands: how many steps are done and not undone -
	// counted by the history's own events while the mode is up (S176a).
	if (bTrackingHistory)
	{
		// A transaction still open is already in the history, as the queue
		// counted it.
		return HistoryPosition + (GUndo != nullptr ? 1 : 0);
	}
	return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() : 0;
}

void USurfEdMode::TrackHistory(bool bOn)
{
	UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	if (bOn && !bTrackingHistory && Buffer)
	{
		HistoryPosition = 0;
		HistoryFinalizedHandle = Buffer->OnTransactionStateChanged().AddLambda([this](const FTransactionContext&, ETransactionStateEventType State)
		{
			// A step that changed nothing is finalized and then dropped from
			// the history; it is not a step. (The history sends no event at
			// a start; one open is counted in UndoPosition.)
			if (State == ETransactionStateEventType::TransactionFinalized && !(GUndo && GUndo->IsTransient()))
			{
				++HistoryPosition;
				// A new step over an undone pair: the pair is history's no more.
				const int32 At = HistoryPosition;
				UndoPairs.RemoveAll([At](const FSurfUndoPair& Pair) { return Pair.Top >= At; });
			}
		});
		HistoryUndoHandle = Buffer->OnUndo().AddLambda([this](const FTransactionContext&, bool bSucceeded) { if (bSucceeded) { --HistoryPosition; SettleUndoPair(true); } });
		HistoryRedoHandle = Buffer->OnRedo().AddLambda([this](const FTransactionContext&, bool bSucceeded) { if (bSucceeded) { ++HistoryPosition; SettleUndoPair(false); } });
		bTrackingHistory = true;
	}
	else if (!bOn && bTrackingHistory)
	{
		if (Buffer)
		{
			Buffer->OnTransactionStateChanged().Remove(HistoryFinalizedHandle);
			Buffer->OnUndo().Remove(HistoryUndoHandle);
			Buffer->OnRedo().Remove(HistoryRedoHandle);
		}
		bTrackingHistory = false;
	}
}

void USurfEdMode::UndoBackTo(int32 Position)
{
	// Undo until the history is back where it was, and no further. Bounded,
	// because an undo that changes nothing must not spin. Our own undoing
	// settles no pair: this is the extrude being taken back whole already.
	++UndoPairingSuspended;
	for (int32 Guard = 0; Guard < 4 && GEditor && UndoPosition() > Position; ++Guard)
	{
		if (!GEditor->UndoTransaction())
		{
			break;
		}
	}
	--UndoPairingSuspended;
}

void USurfEdMode::SettleUndoPair(bool bWasUndo)
{
	if (UndoPairingSuspended > 0 || bUndoPairPending || !GEditor)
	{
		return;
	}
	const int32 At = HistoryPosition;
	const FSurfUndoPair* Inside = UndoPairs.FindByPredicate([At](const FSurfUndoPair& Pair) { return Pair.From < At && At < Pair.Top; });
	if (!Inside)
	{
		return;
	}
	// The history is still inside its own undo here; the rest goes a frame
	// later, and is checked again then.
	bUndoPairPending = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, bWasUndo](float) -> bool
	{
		bUndoPairPending = false;
		if (UndoPairingSuspended > 0 || Modal != ESurfModal::None || !GEditor)
		{
			return false;
		}
		const int32 Now = HistoryPosition;
		const FSurfUndoPair* Pair = UndoPairs.FindByPredicate([Now](const FSurfUndoPair& P) { return P.From < Now && Now < P.Top; });
		if (!Pair)
		{
			return false;
		}
		const int32 From = Pair->From;
		const int32 Top = Pair->Top;
		++UndoPairingSuspended;
		int32 Steps = 0;
		if (bWasUndo)
		{
			for (int32 Guard = 0; Guard < 8 && HistoryPosition > From; ++Guard)
			{
				if (!GEditor->UndoTransaction()) { break; }
				++Steps;
			}
		}
		else
		{
			for (int32 Guard = 0; Guard < 8 && HistoryPosition < Top; ++Guard)
			{
				if (!GEditor->RedoTransaction()) { break; }
				++Steps;
			}
		}
		--UndoPairingSuspended;
		SurfSession::Verb(TEXT("undo_pair"), FString::Printf(TEXT("%s steps=%d"), bWasUndo ? TEXT("undo") : TEXT("redo"), Steps));
		if (Inspector && bWasUndo && Steps > 0)
		{
			Inspector->SetResult(TEXT("undone: the extrude and its pull, together"));
			Inspector->RefreshSelectionCount();
		}
		RedrawViewports();
		return false;
	}), 0.0f);
}


// --- Lattice (S189a) ---------------------------------------------------------
//
// "add an option to create a lattice for any patch, and user choose the
// lattice res x y z". A cage of points round the patch; while it is being
// edited, clicks pick its points and G R S move them, and the patch follows
// by the cage's smooth blend. Escape leaves the cage, Delete removes it (the
// shape stays), the Lattice verb goes in and out.

TArray<FSurfPatchRef> USurfEdMode::LatticeTargets() const
{
	TArray<FSurfPatchRef> Out;
	if (SelectedPatch.IsValid())
	{
		Out.AddUnique(SelectedPatch);
	}
	for (const FSurfPatchRef& Ref : PatchSelection)
	{
		if (Ref.IsValid())
		{
			Out.AddUnique(Ref);
		}
	}
	for (const FSurfPointRef& Point : PointSelection)
	{
		FSurfPatchRef Ref;
		Ref.Group = Point.Group;
		Ref.PatchId = Point.PatchId;
		if (Ref.IsValid())
		{
			Out.AddUnique(Ref);
		}
	}
	return Out;
}

bool USurfEdMode::IsLatticeEditing() const
{
	const ASurfGroup* Group = LatticePatch.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(LatticePatch.PatchId) : nullptr;
	return Patch && Patch->Lattice.HasCage();
}

bool USurfEdMode::MakeLattice(int32 ResX, int32 ResY, int32 ResZ)
{
	int32 Made = 0;
	FSurfPatchRef First;
	for (const FSurfPatchRef& Ref : LatticeTargets())
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (Patch && SurfLattice::Bind(*Patch, ResX, ResY, ResZ))
		{
			++Made;
			if (!First.IsValid())
			{
				First = Ref;
			}
		}
	}
	if (Made == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("lattice: pick a patch first"));
		}
		return false;
	}
	LatticePatch = First;
	LatticePicked.Reset();
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("lattice %d x %d x %d round %d patch%s - click its points (A for all), G R S bend the patch; Escape leaves it, Delete removes it and the shape stays"),
			FMath::Clamp(ResX, SurfLattice::MinRes, SurfLattice::MaxRes), FMath::Clamp(ResY, SurfLattice::MinRes, SurfLattice::MaxRes),
			FMath::Clamp(ResZ, SurfLattice::MinRes, SurfLattice::MaxRes), Made, Made == 1 ? TEXT("") : TEXT("es")));
	}
	SurfSession::Verb(TEXT("lattice"), FString::Printf(TEXT("res=%d,%d,%d patches=%d"), ResX, ResY, ResZ, Made));
	RedrawViewports();
	return true;
}

bool USurfEdMode::ToggleLattice()
{
	if (IsLatticeEditing())
	{
		LeaveLattice();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("left the lattice - the Lattice verb goes back in"));
		}
		return true;
	}
	for (const FSurfPatchRef& Ref : LatticeTargets())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch && Patch->Lattice.HasCage())
		{
			LatticePatch = Ref;
			LatticePicked.Reset();
			if (Inspector)
			{
				Inspector->SetResult(TEXT("editing the lattice - click its points (A for all), G R S bend the patch; Escape leaves, Delete removes it"));
			}
			SurfSession::Verb(TEXT("lattice_edit"), TEXT("in"));
			RedrawViewports();
			return true;
		}
	}
	return false;
}

void USurfEdMode::LeaveLattice()
{
	if (bLatticeModal)
	{
		EndLatticeModal(/*bKeep*/ false);
	}
	if (LatticePatch.IsValid())
	{
		SurfSession::Verb(TEXT("lattice_edit"), TEXT("out"));
	}
	LatticePatch = FSurfPatchRef();
	LatticePicked.Reset();
	bLatticeClickHeld = false;
	RedrawViewports();
}

bool USurfEdMode::RemoveLattice()
{
	ASurfGroup* Group = LatticePatch.Group.Get();
	if (!Group)
	{
		return false;
	}
	FScopedTransaction Transaction(LOCTEXT("RemoveLattice", "Remove Lattice"));
	FSurfPatch* Patch = Group->ModifyPatch(LatticePatch.PatchId);
	if (!Patch)
	{
		Transaction.Cancel();
		return false;
	}
	Patch->Lattice.Reset();
	LeaveLattice();
	if (Inspector)
	{
		Inspector->SetResult(TEXT("lattice removed - the patch keeps its shape"));
	}
	SurfSession::Verb(TEXT("lattice_remove"), TEXT(""));
	return true;
}

int32 USurfEdMode::PickLatticePoint(FEditorViewportClient* ViewportClient, FViewport* Viewport) const
{
	const ASurfGroup* Group = LatticePatch.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(LatticePatch.PatchId) : nullptr;
	if (!Patch || !ViewportClient || !Viewport || !Patch->Lattice.HasCage())
	{
		return INDEX_NONE;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
		Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	FSceneView* View = ViewportClient->CalcSceneView(&Family);
	if (!View)
	{
		return INDEX_NONE;
	}
	FIntPoint Cursor;
	Viewport->GetMousePos(Cursor);
	const FVector2D At(Cursor.X, Cursor.Y);
	const FTransform ToWorld = Group->GetActorTransform();
	const FVector Offset = SurfLattice::FollowOffset(*Patch);
	int32 Best = INDEX_NONE;
	double BestSquared = 14.0 * 14.0;
	for (int32 I = 0; I < Patch->Lattice.Cage.Num(); ++I)
	{
		FVector2D Pixel;
		if (View->WorldToPixel(ToWorld.TransformPosition(Patch->Lattice.Cage[I] + Offset), Pixel))
		{
			const double Squared = FVector2D::DistSquared(Pixel, At);
			if (Squared < BestSquared)
			{
				BestSquared = Squared;
				Best = I;
			}
		}
	}
	return Best;
}

bool USurfEdMode::BeginLatticeModal(int32 Kind, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	ASurfGroup* Group = LatticePatch.Group.Get();
	if (!Group || LatticePicked.Num() == 0)
	{
		return false;
	}
	LatticeTransaction = MakeUnique<FScopedTransaction>(
		Kind == 1 ? LOCTEXT("LatticeMove", "Move Lattice") : (Kind == 2 ? LOCTEXT("LatticeTurn", "Turn Lattice") : LOCTEXT("LatticeSize", "Size Lattice")));
	FSurfPatch* Patch = Group->ModifyPatch(LatticePatch.PatchId);
	if (!Patch || !Patch->Lattice.HasCage())
	{
		LatticeTransaction->Cancel();
		LatticeTransaction.Reset();
		return false;
	}
	// The patch changed its points since (a cut, a subdivide): the cage is
	// fitted round it again, so every point has a place in it.
	if (!SurfLattice::IsBound(*Patch))
	{
		SurfLattice::Bind(*Patch, Patch->Lattice.ResX, Patch->Lattice.ResY, Patch->Lattice.ResZ);
		LatticePicked.RemoveAll([Patch](int32 Index) { return !Patch->Lattice.Cage.IsValidIndex(Index); });
		if (Inspector)
		{
			Inspector->SetResult(TEXT("the patch has other points since - the lattice was fitted round it again"));
		}
	}
	// The whole patch moved since: the cage goes with it.
	SurfLattice::Follow(*Patch);

	LatticeCageFrom = Patch->Lattice.Cage;
	LatticePointsFrom.SetNum(Patch->Points.Num());
	for (int32 I = 0; I < Patch->Points.Num(); ++I)
	{
		LatticePointsFrom[I] = Patch->Points[I].Pos;
	}
	const FTransform ToWorld = Group->GetActorTransform();
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;
	for (const int32 Index : LatticePicked)
	{
		if (LatticeCageFrom.IsValidIndex(Index))
		{
			Sum += ToWorld.TransformPosition(LatticeCageFrom[Index]);
			++Count;
		}
	}
	if (Count == 0)
	{
		LatticeTransaction->Cancel();
		LatticeTransaction.Reset();
		return false;
	}
	LatticePivot = Sum / Count;
	LatticeModalKind = Kind;
	LatticeAxis = 0;
	GModalWrap = FVector2D::ZeroVector;
	LatticeStartMouse = CurrentMouse(Viewport);
	NoteModalStartRay(ViewportClient, Viewport);
	bLatticeModal = true;
	SurfSession::Gesture(TEXT("lattice_modal"), FString::Printf(TEXT("kind=%d points=%d"), Kind, Count));
	return true;
}

void USurfEdMode::UpdateLatticeModal(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	ASurfGroup* Group = LatticePatch.Group.Get();
	if (!bLatticeModal || !Group || !ViewportClient || !Viewport)
	{
		return;
	}
	FSurfPatch* Patch = Group->ModifyPatch(LatticePatch.PatchId);
	if (!Patch || Patch->Lattice.Cage.Num() != LatticeCageFrom.Num())
	{
		return;
	}
	WrapModalCursor(Viewport);
	FVector Right, Up, Forward;
	if (!GetViewBasis(ViewportClient, Viewport, Right, Up, Forward))
	{
		return;
	}
	const FVector2D Delta = CurrentMouse(Viewport) - LatticeStartMouse;
	const FTransform ToWorld = Group->GetActorTransform();
	const FVector AxisV = LatticeAxis == 1 ? FVector::XAxisVector : (LatticeAxis == 2 ? FVector::YAxisVector : (LatticeAxis == 3 ? FVector::ZAxisVector : FVector::ZeroVector));
	static const TCHAR* AxisNames[4] = { TEXT("free"), TEXT("X"), TEXT("Y"), TEXT("Z") };

	TArray<FVector> Cage = LatticeCageFrom;
	FString Line;
	if (LatticeModalKind == 1)
	{
		FVector WorldDelta = (Right * Delta.X - Up * Delta.Y) * WorldPerPixel(ViewportClient, LatticePivot);
		FVector ByRays;
		if (!AxisV.IsZero() && LockedMoveFromRays(ViewportClient, Viewport, LatticePivot, AxisV, FVector::ZeroVector, ByRays))
		{
			WorldDelta = ByRays;
		}
		else if (!AxisV.IsZero())
		{
			WorldDelta = AxisV * FVector::DotProduct(WorldDelta, AxisV);
		}
		const double Step = SurfSnap::MoveStep();
		if (Step > 0.0)
		{
			WorldDelta.X = FMath::RoundToDouble(WorldDelta.X / Step) * Step;
			WorldDelta.Y = FMath::RoundToDouble(WorldDelta.Y / Step) * Step;
			WorldDelta.Z = FMath::RoundToDouble(WorldDelta.Z / Step) * Step;
		}
		for (const int32 Index : LatticePicked)
		{
			if (Cage.IsValidIndex(Index))
			{
				Cage[Index] = ToWorld.InverseTransformPosition(ToWorld.TransformPosition(LatticeCageFrom[Index]) + WorldDelta);
			}
		}
		Line = FString::Printf(TEXT("lattice: %.0f %.0f %.0f cm, %s - X Y Z lock, click or Enter keeps, Escape puts it back"), WorldDelta.X, WorldDelta.Y, WorldDelta.Z, AxisNames[LatticeAxis]);
	}
	else if (LatticeModalKind == 2)
	{
		double Degrees = Delta.X * 0.5;
		const double TurnStepNow = SurfSnap::TurnStep();
		if (TurnStepNow > 0.0)
		{
			Degrees = FMath::GridSnap(Degrees, TurnStepNow);
		}
		const FVector TurnAxis = AxisV.IsZero() ? Forward : AxisV;
		const FQuat Turn(TurnAxis.GetSafeNormal(), FMath::DegreesToRadians(Degrees));
		for (const int32 Index : LatticePicked)
		{
			if (Cage.IsValidIndex(Index))
			{
				const FVector World = ToWorld.TransformPosition(LatticeCageFrom[Index]);
				Cage[Index] = ToWorld.InverseTransformPosition(LatticePivot + Turn.RotateVector(World - LatticePivot));
			}
		}
		Line = FString::Printf(TEXT("lattice: %.0f degrees about %s"), Degrees, LatticeAxis == 0 ? TEXT("the view") : AxisNames[LatticeAxis]);
	}
	else
	{
		const double Factor = FMath::Max(0.02, 1.0 + Delta.X * 0.005);
		const FVector Scale = AxisV.IsZero() ? FVector(Factor) : FVector::OneVector + AxisV.GetAbs() * (Factor - 1.0);
		for (const int32 Index : LatticePicked)
		{
			if (Cage.IsValidIndex(Index))
			{
				const FVector World = ToWorld.TransformPosition(LatticeCageFrom[Index]);
				Cage[Index] = ToWorld.InverseTransformPosition(LatticePivot + (World - LatticePivot) * Scale);
			}
		}
		Line = FString::Printf(TEXT("lattice: x %.2f, %s"), Factor, AxisNames[LatticeAxis]);
	}

	ApplyLatticeCage(Cage);
	if (Inspector)
	{
		Inspector->SetResult(Line);
	}
	const double Now = FPlatformTime::Seconds();
	if ((Now - LastModalRebuild) >= 1.0 / 60.0)
	{
		LastModalRebuild = Now;
		Group->RebuildPreview();
	}
	ViewportClient->Invalidate();
}

void USurfEdMode::ApplyLatticeCage(const TArray<FVector>& Cage)
{
	ASurfGroup* Group = LatticePatch.Group.Get();
	FSurfPatch* Patch = Group ? Group->ModifyPatch(LatticePatch.PatchId) : nullptr;
	if (!bLatticeModal || !Patch || Cage.Num() != LatticeCageFrom.Num())
	{
		return;
	}
	Patch->Lattice.Cage = Cage;
	SurfLattice::Deform(*Patch, LatticeCageFrom, LatticePointsFrom);
}

bool USurfEdMode::ApplySlideAmount(double Amount)
{
	ASurfGroup* Group = ModalGroup.Get();
	FSurfPatch* Patch = Group ? Group->ModifyPatch(ModalPatchId) : nullptr;
	if (Modal != ESurfModal::Slide || !Patch || ModalOriginal.Num() != Patch->NumPoints())
	{
		return false;
	}
	for (int32 I = 0; I < PointSelection.Num(); ++I)
	{
		const FSurfPointRef& Ref = PointSelection[I];
		if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height && SlideAxes.IsValidIndex(I))
		{
			Patch->At(Ref.X, Ref.Y).Pos = ModalOriginal[Ref.Y * Patch->Width + Ref.X] + SlideAxes[I] * Amount;
		}
	}
	ModalSlideAmount = Amount;
	for (const FModalOther& Entry : ModalOthers)
	{
		ASurfGroup* OtherGroup = Entry.Ref.Group.Get();
		FSurfPatch* Other = OtherGroup ? OtherGroup->ModifyPatch(Entry.Ref.PatchId) : nullptr;
		if (Other && Entry.Ref.X >= 0 && Entry.Ref.X < Other->Width && Entry.Ref.Y >= 0 && Entry.Ref.Y < Other->Height)
		{
			Other->At(Entry.Ref.X, Entry.Ref.Y).Pos = OtherGroup->GetActorTransform().InverseTransformPosition(Entry.OriginalWorld + Entry.SlideAxisWorld * Amount);
		}
	}
	return true;
}

void USurfEdMode::EndLatticeModal(bool bKeep)
{
	ASurfGroup* Group = LatticePatch.Group.Get();
	FSurfPatch* Patch = Group ? Group->ModifyPatch(LatticePatch.PatchId) : nullptr;
	if (Patch)
	{
		if (!bKeep && Patch->Lattice.Cage.Num() == LatticeCageFrom.Num() && Patch->Points.Num() == LatticePointsFrom.Num())
		{
			Patch->Lattice.Cage = LatticeCageFrom;
			for (int32 I = 0; I < Patch->Points.Num(); ++I)
			{
				Patch->Points[I].Pos = LatticePointsFrom[I];
			}
		}
		SurfLattice::NoteRest(*Patch);
		if (bKeep)
		{
			++Patch->EditSerial;
		}
	}
	if (LatticeTransaction.IsValid())
	{
		if (!bKeep)
		{
			LatticeTransaction->Cancel();
		}
		LatticeTransaction.Reset();
	}
	bLatticeModal = false;
	LatticeCageFrom.Reset();
	LatticePointsFrom.Reset();
	if (Group)
	{
		Group->RebuildPreview();
	}
	SurfSession::Gesture(TEXT("lattice_modal"), bKeep ? TEXT("confirm") : TEXT("cancel"));
	RedrawViewports();
}

bool USurfEdMode::HandleLatticeKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!IsLatticeEditing())
	{
		if (LatticePatch.IsValid())
		{
			LeaveLattice();
		}
		return false;
	}
	if (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown)
	{
		return false;
	}
	const bool bShift = Viewport && (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift));
	const bool bCtrl = Viewport && (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));
	const bool bAlt = Viewport && (Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt));

	if (bLatticeModal)
	{
		if (Event != IE_Pressed)
		{
			return true;
		}
		if (Key == EKeys::LeftMouseButton || Key == EKeys::Enter || Key == EKeys::SpaceBar)
		{
			EndLatticeModal(/*bKeep*/ true);
			bLatticeClickHeld = Key == EKeys::LeftMouseButton;
			return true;
		}
		if (Key == EKeys::RightMouseButton || Key == EKeys::Escape)
		{
			EndLatticeModal(/*bKeep*/ false);
			return true;
		}
		if (Key == EKeys::X || Key == EKeys::Y || Key == EKeys::Z)
		{
			const int32 Wanted = Key == EKeys::X ? 1 : (Key == EKeys::Y ? 2 : 3);
			LatticeAxis = LatticeAxis == Wanted ? 0 : Wanted;
			UpdateLatticeModal(ViewportClient, Viewport);
			return true;
		}
		const ESurfAction Pressed = SurfKeymap::Match(Key, bCtrl, bShift, bAlt);
		if (Pressed == ESurfAction::Translate || Pressed == ESurfAction::Rotate || Pressed == ESurfAction::Scale)
		{
			const int32 Kind = Pressed == ESurfAction::Translate ? 1 : (Pressed == ESurfAction::Rotate ? 2 : 3);
			if (Kind != LatticeModalKind)
			{
				EndLatticeModal(/*bKeep*/ false);
				BeginLatticeModal(Kind, ViewportClient, Viewport);
			}
			return true;
		}
		return true;
	}

	if (Key == EKeys::LeftMouseButton && Event == IE_Released && bLatticeClickHeld)
	{
		bLatticeClickHeld = false;
		return true;
	}
	if (Event != IE_Pressed)
	{
		return false;
	}

	if (Key == EKeys::LeftMouseButton && !bAlt)
	{
		const int32 Hit = PickLatticePoint(ViewportClient, Viewport);
		if (Hit != INDEX_NONE)
		{
			if (bShift || bCtrl)
			{
				if (LatticePicked.Contains(Hit))
				{
					LatticePicked.Remove(Hit);
				}
				else
				{
					LatticePicked.Add(Hit);
				}
			}
			else
			{
				LatticePicked.Reset();
				LatticePicked.Add(Hit);
			}
		}
		else if (!bShift && !bCtrl)
		{
			LatticePicked.Reset();
		}
		bLatticeClickHeld = true;
		RedrawViewports();
		return true;
	}
	if (Key == EKeys::Escape)
	{
		LeaveLattice();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("left the lattice - the Lattice verb goes back in"));
		}
		return true;
	}
	if (Key == EKeys::Delete)
	{
		RemoveLattice();
		return true;
	}
	const ESurfAction Pressed = SurfKeymap::Match(Key, bCtrl, bShift, bAlt);
	if (Pressed == ESurfAction::SelectAll)
	{
		const ASurfGroup* Group = LatticePatch.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(LatticePatch.PatchId) : nullptr;
		const int32 Count = Patch ? Patch->Lattice.Cage.Num() : 0;
		if (bAlt || LatticePicked.Num() == Count)
		{
			LatticePicked.Reset();
		}
		else
		{
			LatticePicked.Reset();
			for (int32 I = 0; I < Count; ++I)
			{
				LatticePicked.Add(I);
			}
		}
		RedrawViewports();
		return true;
	}
	if (Pressed == ESurfAction::Translate || Pressed == ESurfAction::Rotate || Pressed == ESurfAction::Scale)
	{
		if (LatticePicked.Num() == 0)
		{
			if (Inspector)
			{
				Inspector->SetResult(TEXT("lattice: click its points first (A for all)"));
			}
			return true;
		}
		BeginLatticeModal(Pressed == ESurfAction::Translate ? 1 : (Pressed == ESurfAction::Rotate ? 2 : 3), ViewportClient, Viewport);
		return true;
	}
	// A sub-mode key leaves the cage and does what it says.
	if (Pressed == ESurfAction::SubModeObject || Pressed == ESurfAction::SubModeFace
		|| Pressed == ESurfAction::SubModeEdge || Pressed == ESurfAction::SubModeVertex)
	{
		LeaveLattice();
		return false;
	}
	return false;
}

void USurfEdMode::DrawLattice(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	if (!PDI)
	{
		return;
	}
	auto DrawOne = [PDI](const ASurfGroup* Group, const FSurfPatch& Patch, bool bEditing, const TArray<int32>& Picked)
	{
		const FSurfLattice& L = Patch.Lattice;
		if (!L.HasCage())
		{
			return;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		const FVector Offset = SurfLattice::FollowOffset(Patch);
		auto At = [&](int32 X, int32 Y, int32 Z) { return ToWorld.TransformPosition(L.Cage[L.Index(X, Y, Z)] + Offset); };
		const FLinearColor Line = bEditing ? FLinearColor(0.72f, 0.52f, 1.0f) : FLinearColor(0.42f, 0.34f, 0.58f);
		const float Width = bEditing ? 1.5f : 1.0f;
		for (int32 Z = 0; Z < L.ResZ; ++Z)
		{
			for (int32 Y = 0; Y < L.ResY; ++Y)
			{
				for (int32 X = 0; X < L.ResX; ++X)
				{
					const FVector P = At(X, Y, Z);
					if (X + 1 < L.ResX) { PDI->DrawLine(P, At(X + 1, Y, Z), Line, SDPG_Foreground, Width, 0.0f, true); }
					if (Y + 1 < L.ResY) { PDI->DrawLine(P, At(X, Y + 1, Z), Line, SDPG_Foreground, Width, 0.0f, true); }
					if (Z + 1 < L.ResZ) { PDI->DrawLine(P, At(X, Y, Z + 1), Line, SDPG_Foreground, Width, 0.0f, true); }
					if (bEditing)
					{
						const bool bPicked = Picked.Contains(L.Index(X, Y, Z));
						PDI->DrawPoint(P, bPicked ? FLinearColor(1.0f, 0.55f, 0.1f) : FLinearColor(0.85f, 0.75f, 1.0f), bPicked ? 10.0f : 7.0f, SDPG_Foreground);
					}
				}
			}
		}
	};

	const ASurfGroup* EditGroup = LatticePatch.Group.Get();
	const FSurfPatch* Editing = EditGroup ? EditGroup->FindPatch(LatticePatch.PatchId) : nullptr;
	if (Editing)
	{
		DrawOne(EditGroup, *Editing, true, LatticePicked);
	}
	// A picked patch with a lattice shows it, quietly.
	for (const FSurfPatchRef& Ref : LatticeTargets())
	{
		if (Ref == LatticePatch)
		{
			continue;
		}
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch)
		{
			DrawOne(Group, *Patch, false, TArray<int32>());
		}
	}
}

#undef LOCTEXT_NAMESPACE
