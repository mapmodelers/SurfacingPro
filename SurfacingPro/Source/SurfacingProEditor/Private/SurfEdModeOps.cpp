// Copyright MapModelers. All Rights Reserved.
//
// The operations a keyboard action runs: framing, whole-selection transforms,
// loop insertion at the cursor, and turning a curve into editable vertices.
//
// Kept apart from picking and selection because these are verbs - each one
// takes the current selection, does a thing, and states what the selection is
// afterwards (D16.1).

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfPreviewMaterial.h"

#include "EditorViewportClient.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "SceneView.h"
#include "SurfMatcapExtension.h"
#include "DynamicMeshBuilder.h"
#include "SceneManagement.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "SLevelViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "SurfCamera.h"

#include "SurfGroup.h"
#include "SurfChunk.h"
#include "SurfPatchMath.h"
#include "SurfPatchPrimitives.h"
#include "SurfBrushMath.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Engine/StaticMeshActor.h"
#include "SurfPatchOps.h"
#include "LevelEditorViewport.h"
#include "SurfPickContext.h"
#include "Algo/Reverse.h"
#include "SurfPatchSurfacing.h"
#include "Components/StaticMeshComponent.h"
#include "SurfProjection.h"
#include "SurfCellSet.h"
#include "SurfSettings.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "Components/DynamicMeshComponent.h"
#include "Engine/Selection.h"

#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModeOps"

namespace
{
	/** The selected patch, read-only, or null. */
	const FSurfPatch* ResolveSelectedPatch(const USurfEdMode& Mode, ASurfGroup*& OutGroup)
	{
		OutGroup = Mode.GetSelectedPatch().Group.Get();
		return OutGroup ? OutGroup->FindPatch(Mode.GetSelectedPatch().PatchId) : nullptr;
	}
}

// --- Adjust the last operation ------------------------------------------------

void USurfEdMode::ShowVerbOptions(const FString& Id, bool bAtCursor)
{
	// Run again from its own frame (S170a), the frame stays as it is: a
	// rebuilt frame takes the knob out from under the hand mid-drag -
	// "cant be slided realtime".
	if (bReplayingOptions && Id.Equals(OptionsVerbId) && bOptionsShown)
	{
		return;
	}
	OptionsVerbId = Id;
	++OptionsSerial;
	bOptionsShown = true;
	// "when user is right clicking a button the popup shows at cursor; when
	// clicking it shows where you did" (S166a).
	bOptionsAtCursor = bAtCursor && FSlateApplication::IsInitialized();
	if (bOptionsAtCursor)
	{
		OptionsCursor = FSlateApplication::Get().GetCursorPos();
	}
	bOptionsOnly = !Id.Equals(DescribeLastOperation(), ESearchCase::IgnoreCase);
	bAdjustFrameDismissed = false;
	if (GEditor)
	{
		RedrawViewports();
	}
	SurfSession::Verb(TEXT("params_window"), Id);
}

void USurfEdMode::AfterVerbRan(const FString& Id, bool bHasOptions)
{
	if (!bHasOptions || Modal != ESurfModal::None || (bOptionsShown && OptionsVerbId.Equals(Id, ESearchCase::IgnoreCase)))
	{
		return;
	}
	ShowVerbOptions(Id, /*bAtCursor*/ true);
}

void USurfEdMode::TurnOptionsTo(const FString& Id)
{
	if (!Id.Equals(OptionsVerbId))
	{
		OptionsVerbId = Id;
		++OptionsSerial;
	}
}

void USurfEdMode::NoteOperation(const FSurfOperation& Op)
{
	bAdjustFrameDismissed = false;
	// The options under the knobs are the operation's own (S160a).
	bOptionsOnly = false;
	bOptionsShown = true;
	bOptionsAtCursor = false;

	// A new operation (not a replay from the frame) puts the frame where the
	// hand is: down and right of the cursor, in the viewport that holds it.
	if (ContinuousEditDepth == 0 && !bReplayingOptions && FSlateApplication::IsInitialized())
	{
		AdjustFrameAnchor = FSlateApplication::Get().GetCursorPos();
		AdjustFrameHost = -1;
		for (int32 I = 0; I < ViewportHosts.Num(); ++I)
		{
			TSharedPtr<SLevelViewport> Host = ViewportHosts[I].Pin();
			if (Host.IsValid() && Host->GetCachedGeometry().IsUnderLocation(AdjustFrameAnchor))
			{
				AdjustFrameHost = I;
				break;
			}
		}
		++AdjustFrameSerial;
	}

	LastOperation = Op;
	TurnOptionsTo(DescribeLastOperation());

	// How far a knob moves per pixel: from the size of a cell on the active
	// patch - fine on a detailed patch, quick on a coarse one - kept across
	// a replay so the knob does not change pace under the hand dragging it.
	if (LastOperation.SliderRange <= 0.0)
	{
		double Sum = 0.0;
		int32 Count = 0;
		const ASurfGroup* Group = SelectedPatch.Group.Get();
		if (const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr)
		{
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					if (X + 1 < Patch->Width)  { Sum += FVector::Dist(Patch->At(X, Y).Pos, Patch->At(X + 1, Y).Pos); ++Count; }
					if (Y + 1 < Patch->Height) { Sum += FVector::Dist(Patch->At(X, Y).Pos, Patch->At(X, Y + 1).Pos); ++Count; }
				}
			}
		}
		const double CellCm = Count > 0 ? Sum / Count : 100.0;
		// A cell of a metre moves half a centimetre a pixel; ten centimetres,
		// a twentieth; three metres, a centimetre and a half.
		LastOperation.SliderRange = FMath::Clamp(CellCm * 0.005, 0.05, 5.0);
	}

	// Anything noted ran and did something.
	LastGoodOperation = LastOperation;
	LastOperation.QueueLengthAfter = GEditor && GEditor->Trans ? UndoPosition() : -1;
	LastOperation.UndoCountAfter = GEditor && GEditor->Trans ? GEditor->Trans->GetUndoCount() : -1;
}

bool USurfEdMode::CanAdjustLastOperation() const
{
	// Only while the operation is still the last thing in the history: an
	// adjustment undoes it, and undoing something else would be a lie.
	if (LastOperation.Kind == ESurfOpKind::None || !GEditor || !GEditor->Trans || Modal != ESurfModal::None)
	{
		return false;
	}

	return UndoPosition() == LastOperation.QueueLengthAfter
		&& GEditor->Trans->GetUndoCount() == LastOperation.UndoCountAfter;
}

// The point a turn or a scale in the UV window happens about. The middle of
// what is picked, across every patch at once - the window draws all the nets
// in one UV plane, so two patches turning about two different centres is not
// what the eye is being shown. With nothing picked it is the middle of the
// nets themselves, which is what framing already shows.
//
// Blender turns about its 2D cursor; there is no 2D cursor here, and adding
// one to place a pivot is a second thing to learn for a case this covers.
bool USurfEdMode::SelectedUVPivot(FVector2D& OutPivot)
{
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	FVector2D PickedSum = FVector2D::ZeroVector;
	int32 PickedCount = 0;
	FVector2D Low = FVector2D::ZeroVector;
	FVector2D High = FVector2D::ZeroVector;
	bool bAny = false;

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}

		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				const FVector2D UV = Patch->At(X, Y).UV;
				if (!bAny)
				{
					Low = UV;
					High = UV;
					bAny = true;
				}
				else
				{
					Low = FVector2D(FMath::Min(Low.X, UV.X), FMath::Min(Low.Y, UV.Y));
					High = FVector2D(FMath::Max(High.X, UV.X), FMath::Max(High.Y, UV.Y));
				}

				for (const FSurfPointRef& Each : PointSelection)
				{
					if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group && Each.X == X && Each.Y == Y)
					{
						PickedSum += UV;
						++PickedCount;
						break;
					}
				}
			}
		}
	}

	if (PickedCount > 0)
	{
		OutPivot = PickedSum / static_cast<double>(PickedCount);
		return true;
	}
	if (bAny)
	{
		OutPivot = (Low + High) * 0.5;
		return true;
	}
	return false;
}

bool USurfEdMode::SelectedUVExtent(FVector2D& OutLow, FVector2D& OutHigh, FVector2D& OutCentre)
{
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}
	const bool bPicked = PointSelection.Num() > 0;
	bool bAny = false;
	FVector2D Sum = FVector2D::ZeroVector;
	int32 Count = 0;
	auto Take = [&](const FVector2D& UV)
	{
		if (!bAny)
		{
			OutLow = UV;
			OutHigh = UV;
			bAny = true;
		}
		else
		{
			OutLow = FVector2D(FMath::Min(OutLow.X, UV.X), FMath::Min(OutLow.Y, UV.Y));
			OutHigh = FVector2D(FMath::Max(OutHigh.X, UV.X), FMath::Max(OutHigh.Y, UV.Y));
		}
		Sum += UV;
		++Count;
	};
	if (bPicked)
	{
		for (const FSurfPointRef& Each : PointSelection)
		{
			const ASurfGroup* Group = Each.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Each.PatchId) : nullptr;
			if (Patch && Patch->HasValidDimensions() && Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Take(Patch->At(Each.X, Each.Y).UV);
			}
		}
	}
	else
	{
		for (const FSurfPatchRef& Ref : Targets)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Patch->HasValidDimensions())
			{
				continue;
			}
			for (const FSurfControlPoint& Point : Patch->Points)
			{
				Take(Point.UV);
			}
		}
	}
	if (!bAny)
	{
		return false;
	}
	OutCentre = Sum / static_cast<double>(FMath::Max(1, Count));
	return true;
}

// Turn and scale read the same three cases as the move (D60), for the same
// reason: a patch whose mapping is a rule has no single UV to turn, so its
// mapping turns instead - by the angle, about its own origin rather than the
// pivot, because a rule does not have a point to pivot on. The line says so,
// and Freeze is what makes the points themselves movable.
bool USurfEdMode::TurnSelectedUVsQuarter()
{
	FVector2D Pivot = FVector2D::ZeroVector;
	SelectedUVPivot(Pivot);
	FScopedTransaction Transaction(LOCTEXT("TurnUVQuarter", "Turn UVs a Quarter"));
	if (!TurnSelectedUVs(90.0, Pivot))
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("turn: select a patch first"));
		}
		return false;
	}
	SurfSession::Verb(TEXT("uv_turn_quarter"), TEXT("90"));
	return true;
}

bool USurfEdMode::ReadActiveUVFields(FVector2D& OutTiles, FVector2D& OutShift, double& OutTurn, bool& bOutFrozen) const
{
	FSurfPatchRef Ref = SelectedPatch;
	if (!Ref.IsValid())
	{
		TArray<FSurfPatchRef> Held = PatchesHeldBySelection();
		if (Held.Num() > 0)
		{
			Ref = Held[0];
		}
	}
	const ASurfGroup* Group = Ref.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
	if (!Patch)
	{
		return false;
	}
	OutTiles = Patch->Mapping == ESurfPatchMapping::Planar ? Patch->Projection.Scale : Patch->Scale;
	OutShift = Patch->UVShift;
	OutTurn = Patch->UVRotation;
	bOutFrozen = Patch->Mapping == ESurfPatchMapping::Manual;
	return true;
}

bool USurfEdMode::SetSelectedUVFields(const FUVFields& Fields)
{
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}
	if (Targets.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("uv: select a patch first"));
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("UVFields", "UV Fields"));
	TArray<ASurfGroup*> Groups;
	int32 Done = 0;
	int32 Frozen = 0;
	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		if (Patch->Mapping == ESurfPatchMapping::Manual)
		{
			++Frozen;
			continue;
		}

		FVector2D Tiles = Patch->Mapping == ESurfPatchMapping::Planar ? Patch->Projection.Scale : Patch->Scale;
		if (Fields.TilesU.IsSet()) { Tiles.X = Fields.TilesU.GetValue(); }
		if (Fields.TilesV.IsSet()) { Tiles.Y = Fields.TilesV.GetValue(); }
		if (Fields.bSquare) { Tiles.Y = Tiles.X; }
		Tiles.X = FMath::Max(0.01, Tiles.X);
		Tiles.Y = FMath::Max(0.01, Tiles.Y);
		SurfPatchSurfacing::SetScale(*Patch, Tiles);

		if (Fields.ShiftU.IsSet()) { Patch->UVShift.X = Fields.ShiftU.GetValue(); }
		if (Fields.ShiftV.IsSet()) { Patch->UVShift.Y = Fields.ShiftV.GetValue(); }
		if (Fields.Turn.IsSet()) { Patch->UVRotation = Fields.Turn.GetValue(); }

		SurfPatchSurfacing::Reapply(*Patch);
		++Patch->EditSerial;
		Groups.AddUnique(Group);
		++Done;
	}

	if (Done == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(Frozen > 0
				? TEXT("uv: a frozen patch has no rule to set - Flow or Align it first, or move its points")
				: TEXT("uv: nothing to set"));
		}
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("uv: set on %d patch%s%s"), Done, Done == 1 ? TEXT("") : TEXT("es"),
			Frozen > 0 ? TEXT(" - the frozen ones left alone") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_fields"), FString::Printf(TEXT("patches=%d frozen=%d square=%d"), Done, Frozen, Fields.bSquare ? 1 : 0));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::TurnSelectedUVs(double DeltaDegrees, const FVector2D& PivotUV)
{
	if (FMath::IsNearlyZero(DeltaDegrees))
	{
		return false;
	}

	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	const double Radians = FMath::DegreesToRadians(DeltaDegrees);
	const double CosA = FMath::Cos(Radians);
	const double SinA = FMath::Sin(Radians);
	auto TurnUV = [&PivotUV, CosA, SinA](FVector2D& UV)
	{
		const FVector2D R = UV - PivotUV;
		UV = PivotUV + FVector2D(R.X * CosA - R.Y * SinA, R.X * SinA + R.Y * CosA);
	};

	TArray<ASurfGroup*> Groups;
	int32 Points = 0;
	int32 Whole = 0;
	int32 Frozen = 0;

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}

		TArray<FSurfPointRef> Picked;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group
				&& Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Picked.Add(Each);
			}
		}

		// Picked points on a patch with a rule: the rule is frozen first and
		// the points move (S223a: "they move the whole thing not the
		// selection") - as Align, Mirror and UV Smooth have it. With nothing
		// picked a rule still slides whole, in its own numbers.
		if (Patch->Mapping != ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			SurfPatchSurfacing::SetManual(*Patch);
			++Frozen;
		}
		if (Patch->Mapping == ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			for (const FSurfPointRef& Each : Picked)
			{
				TurnUV(Patch->At(Each.X, Each.Y).UV);
				++Points;
			}
			++Patch->EditSerial;
		}
		else if (Patch->Mapping == ESurfPatchMapping::Manual)
		{
			for (FSurfControlPoint& Point : Patch->Points)
			{
				TurnUV(Point.UV);
			}
			++Patch->EditSerial;
			++Whole;
		}
		else
		{
			SurfPatchSurfacing::Rotate(*Patch, DeltaDegrees);
			++Whole;
		}

		Groups.AddUnique(Group);
	}

	if (Groups.Num() == 0)
	{
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Points > 0
			? FString::Printf(TEXT("turned %d UV%s%s"), Points, Points == 1 ? TEXT("") : TEXT("s"), Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT(""))
			: FString::Printf(TEXT("turned the mapping of %d patch%s - Freeze one to turn its points about the pivot"),
				Whole, Whole == 1 ? TEXT("") : TEXT("es")));
	}

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::ScaleSelectedUVs(const FVector2D& Ratio, const FVector2D& PivotUV)
{
	if (FMath::Abs(Ratio.X) < 1.e-4 || FMath::Abs(Ratio.Y) < 1.e-4)
	{
		return false;
	}
	if (FMath::IsNearlyEqual(Ratio.X, 1.0) && FMath::IsNearlyEqual(Ratio.Y, 1.0))
	{
		return false;
	}

	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	TArray<ASurfGroup*> Groups;
	int32 Points = 0;
	int32 Whole = 0;
	int32 Frozen = 0;

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}

		TArray<FSurfPointRef> Picked;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group
				&& Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Picked.Add(Each);
			}
		}

		// Picked points on a patch with a rule: the rule is frozen first and
		// the points move (S223a: "they move the whole thing not the
		// selection") - as Align, Mirror and UV Smooth have it. With nothing
		// picked a rule still slides whole, in its own numbers.
		if (Patch->Mapping != ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			SurfPatchSurfacing::SetManual(*Patch);
			++Frozen;
		}
		if (Patch->Mapping == ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			for (const FSurfPointRef& Each : Picked)
			{
				FVector2D& UV = Patch->At(Each.X, Each.Y).UV;
				UV = PivotUV + (UV - PivotUV) * Ratio;
				++Points;
			}
			++Patch->EditSerial;
		}
		else if (Patch->Mapping == ESurfPatchMapping::Manual)
		{
			for (FSurfControlPoint& Point : Patch->Points)
			{
				Point.UV = PivotUV + (Point.UV - PivotUV) * Ratio;
			}
			++Patch->EditSerial;
			++Whole;
		}
		else
		{
			// A bigger net covers more tiles, so the texture repeats more
			// often: the metres a tile goes down by the same factor. Planar
			// keeps its scale on the projection, which is the one that wins.
			const FVector2D Current = Patch->Mapping == ESurfPatchMapping::Planar
				? Patch->Projection.Scale
				: Patch->Scale;
			SurfPatchSurfacing::SetScale(*Patch, FVector2D(
				FMath::Max(1.e-3, Current.X / Ratio.X),
				FMath::Max(1.e-3, Current.Y / Ratio.Y)));
			++Whole;
		}

		Groups.AddUnique(Group);
	}

	if (Groups.Num() == 0)
	{
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Points > 0
			? FString::Printf(TEXT("scaled %d UV%s%s"), Points, Points == 1 ? TEXT("") : TEXT("s"), Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT(""))
			: FString::Printf(TEXT("scaled the mapping of %d patch%s - the metres a tile changed, not the points"),
				Whole, Whole == 1 ? TEXT("") : TEXT("es")));
	}

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::SmoothSelectedUVs(double Strength, int32 Iterations)
{
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}
	if (Targets.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("uv smooth: select a patch first"));
		}
		return false;
	}
	Strength = FMath::Clamp(Strength, 0.0, 1.0);
	Iterations = FMath::Clamp(Iterations, 1, 50);

	FScopedTransaction Transaction(LOCTEXT("SmoothUVs", "Smooth UVs"));
	TArray<ASurfGroup*> Groups;
	int32 Points = 0;
	int32 Frozen = 0;
	int32 Done = 0;

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions() || Patch->Width < 3 || Patch->Height < 3)
		{
			continue;
		}
		if (Patch->Mapping != ESurfPatchMapping::Manual)
		{
			SurfPatchSurfacing::SetManual(*Patch);
			++Frozen;
		}

		// The points to move: the picked ones on this patch, else every
		// interior point. A border point stays, so the net keeps its edge.
		TArray<FIntPoint> Moving;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group
				&& Each.X > 0 && Each.X < Patch->Width - 1 && Each.Y > 0 && Each.Y < Patch->Height - 1)
			{
				Moving.Add(FIntPoint(Each.X, Each.Y));
			}
		}
		if (Moving.Num() == 0)
		{
			bool bAnyPickedHere = false;
			for (const FSurfPointRef& Each : PointSelection)
			{
				bAnyPickedHere = bAnyPickedHere || (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group);
			}
			if (bAnyPickedHere)
			{
				// Only border points picked: nothing to relax.
				continue;
			}
			for (int32 Y = 1; Y < Patch->Height - 1; ++Y)
			{
				for (int32 X = 1; X < Patch->Width - 1; ++X)
				{
					Moving.Add(FIntPoint(X, Y));
				}
			}
		}

		for (int32 Pass = 0; Pass < Iterations; ++Pass)
		{
			TArray<FVector2D> Next;
			Next.Reserve(Moving.Num());
			for (const FIntPoint& P : Moving)
			{
				const FVector2D Mean = (Patch->At(P.X - 1, P.Y).UV + Patch->At(P.X + 1, P.Y).UV
					+ Patch->At(P.X, P.Y - 1).UV + Patch->At(P.X, P.Y + 1).UV) * 0.25;
				Next.Add(FMath::Lerp(Patch->At(P.X, P.Y).UV, Mean, Strength));
			}
			for (int32 I = 0; I < Moving.Num(); ++I)
			{
				Patch->At(Moving[I].X, Moving[I].Y).UV = Next[I];
			}
		}
		Points += Moving.Num();
		++Patch->EditSerial;
		Groups.AddUnique(Group);
		++Done;
	}

	if (Done == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("uv smooth: nothing to relax - pick interior points, or a patch of three lines or more each way"));
		}
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("smoothed the UVs of %d point%s on %d patch%s%s"),
			Points, Points == 1 ? TEXT("") : TEXT("s"), Done, Done == 1 ? TEXT("") : TEXT("es"),
			Frozen > 0 ? TEXT(" - frozen to do it: the points are the mapping now") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_smooth"), FString::Printf(TEXT("points=%d patches=%d frozen=%d strength=%.2f passes=%d"), Points, Done, Frozen, Strength, Iterations));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::MoveSelectedUVs(const FVector2D& DeltaUV)
{
	if (DeltaUV.IsNearlyZero())
	{
		return false;
	}

	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	TArray<ASurfGroup*> Groups;
	int32 Points = 0;
	int32 Whole = 0;
	int32 Frozen = 0;

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}

		// The picked points on this patch, if any.
		TArray<FSurfPointRef> Picked;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group
				&& Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Picked.Add(Each);
			}
		}

		// Picked points on a patch with a rule: the rule is frozen first and
		// the points move (S223a: "they move the whole thing not the
		// selection") - as Align, Mirror and UV Smooth have it. With nothing
		// picked a rule still slides whole, in its own numbers.
		if (Patch->Mapping != ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			SurfPatchSurfacing::SetManual(*Patch);
			++Frozen;
		}
		if (Patch->Mapping == ESurfPatchMapping::Manual && Picked.Num() > 0)
		{
			// Soft selection in the UV space (S231a: "can the soft selection
			// options be available here too?"): the unpicked points of the
			// net follow by their UV distance to the nearest picked one,
			// the reach in tiles being the 3D reach at this patch's metres a
			// tile, the profile the soft chip's. Weighed before anything moves.
			const USurfSettings* Soft = GetDefault<USurfSettings>();
			TArray<float> Follow;
			if (Soft && Soft->bSoftSelection && Picked.Num() < Patch->Points.Num())
			{
				const double TileM = FMath::Max(0.01, Patch->Scale.X);
				const double Reach = FMath::Max(1.e-4, Soft->SoftSelectionRadiusCm / 100.0 / TileM);
				Follow.SetNumZeroed(Patch->Points.Num());
				TArray<bool> IsPicked;
				IsPicked.SetNumZeroed(Patch->Points.Num());
				for (const FSurfPointRef& Each : Picked) { IsPicked[Each.Y * Patch->Width + Each.X] = true; }
				for (int32 I = 0; I < Patch->Points.Num(); ++I)
				{
					if (IsPicked[I]) { continue; }
					double Nearest = TNumericLimits<double>::Max();
					for (const FSurfPointRef& Each : Picked)
					{
						Nearest = FMath::Min(Nearest, FVector2D::Distance(Patch->Points[I].UV, Patch->At(Each.X, Each.Y).UV));
					}
					if (Nearest < Reach)
					{
						Follow[I] = static_cast<float>(USurfSettings::Falloff(Soft->SoftSelectionFalloff, Nearest / Reach, I));
					}
				}
			}
			// Held UVs: the points that are picked are the ones that move.
			for (const FSurfPointRef& Each : Picked)
			{
				Patch->At(Each.X, Each.Y).UV += DeltaUV;
				++Points;
			}
			for (int32 I = 0; I < Follow.Num(); ++I)
			{
				if (Follow[I] > 0.0f)
				{
					Patch->Points[I].UV += DeltaUV * Follow[I];
				}
			}
			++Patch->EditSerial;
		}
		else if (Patch->Mapping == ESurfPatchMapping::Manual)
		{
			// Frozen, with nothing picked: every point moves. Not through
			// UVShift, which nothing re-derives while the mapping is held -
			// it would only surface later, the day the patch is unfrozen.
			for (FSurfControlPoint& Point : Patch->Points)
			{
				Point.UV += DeltaUV;
			}
			++Patch->EditSerial;
			++Whole;
		}
		else
		{
			// A rule has no single point to move: the mapping slides whole.
			// Shift is in tiles, and a tile is a UV unit.
			SurfPatchSurfacing::Shift(*Patch, DeltaUV.X, DeltaUV.Y);
			++Whole;
		}

		Groups.AddUnique(Group);
	}

	if (Groups.Num() == 0)
	{
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Points > 0
			? FString::Printf(TEXT("moved %d UV%s%s%s"), Points, Points == 1 ? TEXT("") : TEXT("s"),
				Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT(""),
				Whole > 0 ? *FString::Printf(TEXT("; %d patch(es) with a rule slid whole"), Whole) : TEXT(""))
			: FString::Printf(TEXT("slid the mapping of %d patch%s - Freeze one to move its points"),
				Whole, Whole == 1 ? TEXT("") : TEXT("es")));
		Inspector->RefreshSelectionCount();
	}

	RedrawViewports();
	return true;
}

namespace
{
	/** The picked points of the selected patches, else every point of them (a rule is frozen first, counted in OutRuled); Write sets each UV. */
	int32 OverHeldUVs(USurfEdMode& Mode, TFunctionRef<void(FVector2D&)> Write, int32& OutRuled)
	{
		OutRuled = 0;
		TArray<FSurfPatchRef> Targets = Mode.GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode.PatchesHeldBySelection())
		{
			Targets.AddUnique(Held);
		}
		TArray<ASurfGroup*> Groups;
		int32 Points = 0;
		for (const FSurfPatchRef& Ref : Targets)
		{
			ASurfGroup* Group = Ref.Group.Get();
			FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Patch->HasValidDimensions())
			{
				continue;
			}
			if (Patch->Mapping != ESurfPatchMapping::Manual)
			{
				SurfPatchSurfacing::SetManual(*Patch);
				++OutRuled;
			}
			TArray<FIntPoint> Which;
			for (const FSurfPointRef& Each : Mode.GetPointSelection())
			{
				if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group && Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
				{
					Which.Add(FIntPoint(Each.X, Each.Y));
				}
			}
			if (Which.Num() == 0 && Mode.GetPointSelection().Num() == 0)
			{
				for (int32 Y = 0; Y < Patch->Height; ++Y) { for (int32 X = 0; X < Patch->Width; ++X) { Which.Add(FIntPoint(X, Y)); } }
			}
			for (const FIntPoint& P : Which)
			{
				Write(Patch->At(P.X, P.Y).UV);
				++Points;
			}
			if (Which.Num() > 0)
			{
				++Patch->EditSerial;
				Groups.AddUnique(Group);
			}
		}
		for (ASurfGroup* Each : Groups) { Each->RebuildPreview(); }
		return Points;
	}
}

bool USurfEdMode::AlignSelectedUVs(int32 Axis, int32 Where)
{
	FVector2D Low, High, Centre;
	if (!SelectedUVExtent(Low, High, Centre))
	{
		return false;
	}
	const double Value = Where == 0 ? Low[Axis] : (Where == 2 ? High[Axis] : (Low[Axis] + High[Axis]) * 0.5);
	FScopedTransaction Transaction(LOCTEXT("AlignUVs", "Align UVs"));
	int32 Ruled = 0;
	const int32 Points = OverHeldUVs(*this, [Axis, Value](FVector2D& UV) { UV[Axis] = Value; }, Ruled);
	if (Points == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("align: pick some UV points first"));
		}
		return false;
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("aligned %d UV%s on %s to %s%s"), Points, Points == 1 ? TEXT("") : TEXT("s"), Axis == 0 ? TEXT("U") : TEXT("V"),
			Where == 0 ? TEXT("the low edge") : (Where == 2 ? TEXT("the high edge") : TEXT("the centre")), Ruled > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Ruled) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_align"), FString::Printf(TEXT("axis=%d where=%d points=%d"), Axis, Where, Points));
	RedrawViewports();
	return true;
}

bool USurfEdMode::FlipSelectedUVs(int32 Axis)
{
	FVector2D Low, High, Centre;
	if (!SelectedUVExtent(Low, High, Centre))
	{
		return false;
	}
	const double Mid = (Low[Axis] + High[Axis]) * 0.5;
	FScopedTransaction Transaction(LOCTEXT("FlipUVs", "Flip UVs"));
	int32 Ruled = 0;
	const int32 Points = OverHeldUVs(*this, [Axis, Mid](FVector2D& UV) { UV[Axis] = 2.0 * Mid - UV[Axis]; }, Ruled);
	if (Points == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("flip: pick some UV points first"));
		}
		return false;
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("flipped %d UV%s on %s%s"), Points, Points == 1 ? TEXT("") : TEXT("s"), Axis == 0 ? TEXT("U") : TEXT("V"), Ruled > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Ruled) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_flip"), FString::Printf(TEXT("axis=%d points=%d"), Axis, Points));
	RedrawViewports();
	return true;
}

bool USurfEdMode::SelectBorderPoints()
{
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Patches.AddUnique(Held);
	}
	if (Patches.Num() == 0)
	{
		if (Inspector) { Inspector->SetResult(TEXT("border: select a patch first")); }
		return false;
	}
	PointSelection.Reset();
	for (const FSurfPatchRef& Ref : Patches)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				if (X == 0 || Y == 0 || X == Patch->Width - 1 || Y == Patch->Height - 1)
				{
					FSurfPointRef P;
					P.Group = Ref.Group;
					P.PatchId = Ref.PatchId;
					P.X = X;
					P.Y = Y;
					PointSelection.Add(P);
				}
			}
		}
	}
	SyncSelectionAfterUVPick();
	SurfSession::Selection(FString::Printf(TEXT("border points=%d"), PointSelection.Num()));
	if (Inspector) { Inspector->RefreshSelectionCount(); }
	RedrawViewports();
	return true;
}

// --- Islands (S215a) ----------------------------------------------------------
//
// An island is one patch's net. Pack, Stack and Rectify take the whole nets
// of the selected patches (a rule is frozen first, as UV Smooth does);
// Straighten takes the picked points of one row or column.

namespace
{
	struct FSurfUVIsland
	{
		ASurfGroup* Group = nullptr;
		FSurfPatch* Patch = nullptr;
		FVector2D Low = FVector2D::ZeroVector;
		FVector2D High = FVector2D::ZeroVector;
	};

	/** The selected patches (and the ones the selection holds) as islands, frozen; their boxes. */
	TArray<FSurfUVIsland> HeldIslands(USurfEdMode& Mode, int32& OutFrozen)
	{
		OutFrozen = 0;
		TArray<FSurfPatchRef> Targets = Mode.GetSelectedPatches();
		for (const FSurfPatchRef& Held : Mode.PatchesHeldBySelection())
		{
			Targets.AddUnique(Held);
		}
		TArray<FSurfUVIsland> Out;
		for (const FSurfPatchRef& Ref : Targets)
		{
			ASurfGroup* Group = Ref.Group.Get();
			FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Patch->HasValidDimensions())
			{
				continue;
			}
			if (Patch->Mapping != ESurfPatchMapping::Manual)
			{
				SurfPatchSurfacing::SetManual(*Patch);
				++OutFrozen;
			}
			FSurfUVIsland Island;
			Island.Group = Group;
			Island.Patch = Patch;
			Island.Low = Island.High = Patch->Points[0].UV;
			for (const FSurfControlPoint& Point : Patch->Points)
			{
				Island.Low = FVector2D(FMath::Min(Island.Low.X, Point.UV.X), FMath::Min(Island.Low.Y, Point.UV.Y));
				Island.High = FVector2D(FMath::Max(Island.High.X, Point.UV.X), FMath::Max(Island.High.Y, Point.UV.Y));
			}
			Out.Add(Island);
		}
		return Out;
	}

	void IslandsChanged(const TArray<FSurfUVIsland>& Islands)
	{
		TArray<ASurfGroup*> Groups;
		for (const FSurfUVIsland& Island : Islands)
		{
			++Island.Patch->EditSerial;
			Groups.AddUnique(Island.Group);
		}
		for (ASurfGroup* Each : Groups) { Each->RebuildPreview(); }
	}
}

bool USurfEdMode::PackSelectedUVs(double Margin)
{
	Margin = FMath::Clamp(Margin, 0.0, 0.2);
	FScopedTransaction Transaction(LOCTEXT("PackUVs", "Pack UVs"));
	int32 Frozen = 0;
	TArray<FSurfUVIsland> Islands = HeldIslands(*this, Frozen);
	if (Islands.Num() == 0)
	{
		Transaction.Cancel();
		if (Inspector) { Inspector->SetResult(TEXT("pack: select a patch first")); }
		return false;
	}

	// Tallest first, laid in rows across the tile; the islands keep their
	// sizes relative to one another and the whole is scaled to fill.
	Islands.Sort([](const FSurfUVIsland& A, const FSurfUVIsland& B) { return (A.High.Y - A.Low.Y) > (B.High.Y - B.Low.Y); });
	double Area = 0.0;
	for (const FSurfUVIsland& Island : Islands)
	{
		Area += FMath::Max(1e-6, (Island.High.X - Island.Low.X) * (Island.High.Y - Island.Low.Y));
	}
	const double Room = FMath::Max(0.1, 1.0 - 2.0 * Margin);
	double Scale = Room / FMath::Sqrt(Area);
	TArray<FVector2D> Corners;
	Corners.SetNum(Islands.Num());
	double BestScale = 0.0;
	TArray<FVector2D> BestCorners;
	for (int32 Try = 0; Try < 12; ++Try)
	{
		double X = Margin, Y = Margin, RowHeight = 0.0, Height = 0.0, Width = 0.0;
		for (int32 i = 0; i < Islands.Num(); ++i)
		{
			const FVector2D Size = (Islands[i].High - Islands[i].Low) * Scale;
			if (X > Margin && X + Size.X > 1.0 - Margin)
			{
				X = Margin;
				Y += RowHeight + Margin;
				RowHeight = 0.0;
			}
			Corners[i] = FVector2D(X, Y);
			X += Size.X + Margin;
			RowHeight = FMath::Max(RowHeight, Size.Y);
			Width = FMath::Max(Width, X - Margin);
			Height = Y + RowHeight;
		}
		const double Fill = FMath::Max(Width, Height);
		if (Fill <= 1.0 - Margin + 1e-9)
		{
			if (Scale > BestScale)
			{
				BestScale = Scale;
				BestCorners = Corners;
			}
			Scale *= FMath::Min(1.25, (1.0 - Margin) / FMath::Max(Fill, 1e-6));
			if (Scale <= BestScale * 1.001) { break; }
		}
		else
		{
			Scale *= (1.0 - Margin) / Fill * 0.98;
		}
	}
	if (BestScale <= 0.0)
	{
		Transaction.Cancel();
		if (Inspector) { Inspector->SetResult(TEXT("pack: the islands do not fit - a smaller margin")); }
		return false;
	}
	int32 Points = 0;
	for (int32 i = 0; i < Islands.Num(); ++i)
	{
		FSurfUVIsland& Island = Islands[i];
		for (FSurfControlPoint& Point : Island.Patch->Points)
		{
			Point.UV = BestCorners[i] + (Point.UV - Island.Low) * BestScale;
			++Points;
		}
	}
	IslandsChanged(Islands);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("packed %d island%s into the tile at %.0f%%%s"), Islands.Num(), Islands.Num() == 1 ? TEXT("") : TEXT("s"), BestScale * 100.0,
			Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_pack"), FString::Printf(TEXT("islands=%d scale=%.3f margin=%.3f"), Islands.Num(), BestScale, Margin));
	RedrawViewports();
	return true;
}

bool USurfEdMode::StackSelectedUVs()
{
	FScopedTransaction Transaction(LOCTEXT("StackUVs", "Stack UVs"));
	int32 Frozen = 0;
	TArray<FSurfUVIsland> Islands = HeldIslands(*this, Frozen);
	if (Islands.Num() < 2)
	{
		Transaction.Cancel();
		if (Inspector) { Inspector->SetResult(TEXT("stack: select two patches or more")); }
		return false;
	}
	const FVector2D Centre = (Islands[0].Low + Islands[0].High) * 0.5;
	for (int32 i = 1; i < Islands.Num(); ++i)
	{
		const FVector2D Shift = Centre - (Islands[i].Low + Islands[i].High) * 0.5;
		for (FSurfControlPoint& Point : Islands[i].Patch->Points)
		{
			Point.UV += Shift;
		}
	}
	IslandsChanged(Islands);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("stacked %d islands on the first%s"), Islands.Num(), Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_stack"), FString::Printf(TEXT("islands=%d"), Islands.Num()));
	RedrawViewports();
	return true;
}

bool USurfEdMode::RectifySelectedUVs()
{
	FScopedTransaction Transaction(LOCTEXT("RectifyUVs", "Rectify UVs"));
	int32 Frozen = 0;
	TArray<FSurfUVIsland> Islands = HeldIslands(*this, Frozen);
	if (Islands.Num() == 0)
	{
		Transaction.Cancel();
		if (Inspector) { Inspector->SetResult(TEXT("rectify: select a patch first")); }
		return false;
	}
	for (FSurfUVIsland& Island : Islands)
	{
		FSurfPatch& Patch = *Island.Patch;
		const int32 W = Patch.Width, H = Patch.Height;
		// Which UV axis the net's rows run along, and which way, so the
		// rectangle keeps the net's turn and is not mirrored.
		const FVector2D Across = Patch.At(W - 1, 0).UV - Patch.At(0, 0).UV;
		const FVector2D Down = Patch.At(0, H - 1).UV - Patch.At(0, 0).UV;
		const int32 AxisX = FMath::Abs(Across.X) >= FMath::Abs(Across.Y) ? 0 : 1;
		const int32 AxisY = 1 - AxisX;
		const double SignX = Across[AxisX] < 0.0 ? -1.0 : 1.0;
		const double SignY = Down[AxisY] < 0.0 ? -1.0 : 1.0;
		const FVector2D Size = Island.High - Island.Low;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const double TX = W > 1 ? static_cast<double>(X) / (W - 1) : 0.0;
				const double TY = H > 1 ? static_cast<double>(Y) / (H - 1) : 0.0;
				FVector2D& UV = Patch.At(X, Y).UV;
				UV[AxisX] = SignX > 0.0 ? Island.Low[AxisX] + TX * Size[AxisX] : Island.High[AxisX] - TX * Size[AxisX];
				UV[AxisY] = SignY > 0.0 ? Island.Low[AxisY] + TY * Size[AxisY] : Island.High[AxisY] - TY * Size[AxisY];
			}
		}
	}
	IslandsChanged(Islands);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("rectified %d island%s: an even net in its own box%s"), Islands.Num(), Islands.Num() == 1 ? TEXT("") : TEXT("s"), Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_rectify"), FString::Printf(TEXT("islands=%d"), Islands.Num()));
	RedrawViewports();
	return true;
}

bool USurfEdMode::StraightenSelectedUVs()
{
	if (PointSelection.Num() < 2)
	{
		if (Inspector) { Inspector->SetResult(TEXT("straighten: pick a row or a column of points first")); }
		return false;
	}
	FScopedTransaction Transaction(LOCTEXT("StraightenUVs", "Straighten UVs"));
	TArray<FSurfUVIsland> Touched;
	int32 Lines = 0, Points = 0, Frozen = 0;
	TArray<FSurfPatchRef> Patches;
	for (const FSurfPointRef& Each : PointSelection)
	{
		FSurfPatchRef Ref;
		Ref.Group = Each.Group;
		Ref.PatchId = Each.PatchId;
		Patches.AddUnique(Ref);
	}
	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		TArray<FIntPoint> Picked;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group && Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Picked.AddUnique(FIntPoint(Each.X, Each.Y));
			}
		}
		if (Picked.Num() < 2)
		{
			continue;
		}
		bool bRow = true, bColumn = true;
		for (const FIntPoint& P : Picked)
		{
			bRow &= P.Y == Picked[0].Y;
			bColumn &= P.X == Picked[0].X;
		}
		if (!bRow && !bColumn)
		{
			continue;
		}
		if (Patch->Mapping != ESurfPatchMapping::Manual)
		{
			SurfPatchSurfacing::SetManual(*Patch);
			++Frozen;
		}
		// Along the axis the ends lean to; each point keeps its place along it.
		Picked.Sort([bRow](const FIntPoint& A, const FIntPoint& B) { return bRow ? A.X < B.X : A.Y < B.Y; });
		const FVector2D Span = Patch->At(Picked.Last().X, Picked.Last().Y).UV - Patch->At(Picked[0].X, Picked[0].Y).UV;
		const int32 Along = FMath::Abs(Span.X) >= FMath::Abs(Span.Y) ? 0 : 1;
		const int32 Fixed = 1 - Along;
		double Mean = 0.0;
		for (const FIntPoint& P : Picked) { Mean += Patch->At(P.X, P.Y).UV[Fixed]; }
		Mean /= Picked.Num();
		for (const FIntPoint& P : Picked)
		{
			Patch->At(P.X, P.Y).UV[Fixed] = Mean;
			++Points;
		}
		++Lines;
		FSurfUVIsland Island;
		Island.Group = Group;
		Island.Patch = Patch;
		Touched.Add(Island);
	}
	if (Lines == 0)
	{
		Transaction.Cancel();
		if (Inspector) { Inspector->SetResult(TEXT("straighten: the picked points must be one row or one column of a net")); }
		return false;
	}
	IslandsChanged(Touched);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("straightened %d line%s, %d points%s"), Lines, Lines == 1 ? TEXT("") : TEXT("s"), Points, Frozen > 0 ? *FString::Printf(TEXT(" (%d frozen)"), Frozen) : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("uv_straighten"), FString::Printf(TEXT("lines=%d points=%d"), Lines, Points));
	RedrawViewports();
	return true;
}

void USurfEdMode::SyncSelectionAfterUVPick()
{
	SyncPatchSelectionToPoints();
	RefreshSoftSelection();
	UpdateOrbitPivot();
	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
	RedrawViewports();
}

void USurfEdMode::NoteVerb(const FString& Name, TFunction<void()> Run)
{
	LastVerbName = Name;
	LastVerbRun = MoveTemp(Run);
	LastVerbQueueBefore = GEditor && GEditor->Trans ? UndoPosition() : -1;
}

bool USurfEdMode::ReplayLastVerbForOptions(const FString& Id)
{
	if (!LastVerbRun || !Id.Equals(LastVerbName, ESearchCase::IgnoreCase) || !GEditor || !GEditor->Trans
		|| LastVerbQueueBefore < 0 || Modal != ESurfModal::None || ContinuousEditDepth > 0)
	{
		return false;
	}
	// What the verb wrote is the whole of the stack above where it started,
	// and nothing has been undone since; more than a few entries means
	// other things happened and the verb is no longer "the last action".
	const int32 Grew = UndoPosition() - LastVerbQueueBefore;
	if (Grew <= 0 || Grew > 4 || GEditor->Trans->GetUndoCount() != 0)
	{
		return false;
	}
	// Set before the undo (S247f): the undo delegate reads it.
	bReplayingOptions = true;
	for (int32 I = 0; I < Grew; ++I)
	{
		GEditor->UndoTransaction();
	}
	// Undo brings back the selection the verb ran on (D34), so the run
	// lands on the same thing with the new options.
	TFunction<void()> Again = LastVerbRun;
	const FString Name = LastVerbName;
	LastVerbQueueBefore = UndoPosition();
	SurfSession::Verb(TEXT("replay_options"), Name);
	Again();
	bReplayingOptions = false;
	return true;
}

bool USurfEdMode::RepeatLastVerb()
{
	if (!LastVerbRun)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("repeat: no verb has been pressed yet"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("repeat_nothing"));
		return true;
	}

	// A copy: the verb may press another one, which would rewrite the field
	// while it is being read.
	TFunction<void()> Again = LastVerbRun;
	const FString Name = LastVerbName;
	SurfSession::Verb(TEXT("repeat"), Name);
	Again();

	// The verb states its own result; this only says which one ran when the
	// verb had nothing to say.
	if (Inspector && Inspector->LastResult.IsEmpty())
	{
		Inspector->SetResult(FString::Printf(TEXT("%s again"), *Name));
		Inspector->RefreshSelectionCount();
	}
	return true;
}

void USurfEdMode::RedrawViewports()
{
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies*/ ContinuousEditDepth <= 0);
	}
}

bool USurfEdMode::ReplayLastOperation(bool bFinal)
{
	if (!CanAdjustLastOperation())
	{
		return false;
	}

	// A knob being dragged replays many times a second; the hit proxies
	// wait for the drag to end, and the session record takes the drag as
	// one line rather than three per replay.
	++ContinuousEditDepth;
	if (!bFinal)
	{
		SurfSession::BeginBurst(TEXT("adjust"));
	}
	ON_SCOPE_EXIT
	{
		--ContinuousEditDepth;
		if (bFinal)
		{
			SurfSession::EndBurst();
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies*/ true);
			}
		}
	};

	// Undo hands back the geometry and the selection as they were when the
	// operation ran; running it again with the new numbers is then exact.
	// An extrude followed by its move is two steps the first time.
	const FSurfOperation Op = LastOperation;
	++UndoPairingSuspended;
	for (int32 Step = 0; Step < FMath::Max(1, Op.UndoSteps); ++Step)
	{
		if (!GEditor->UndoTransaction())
		{
			--UndoPairingSuspended;
			return false;
		}
	}
	--UndoPairingSuspended;

	bool bRan = RunOperation(Op);
	if (!bRan)
	{
		// A knob dragged to a value the verb refuses - a thickness of zero,
		// a move of nothing - used to leave the geometry undone and the
		// frame gone, which read as "it cancelled". The last numbers that
		// did something are run again instead, and the knob goes back to
		// them.
		if (LastGoodOperation.Kind == Op.Kind && RunOperation(LastGoodOperation))
		{
			bRan = true;
			if (Inspector)
			{
				Inspector->SetResult(TEXT("adjust: that value does nothing - back to the last that did"));
				Inspector->RefreshSelectionCount();
			}
		}
		else
		{
			LastOperation.Kind = ESurfOpKind::None;
		}
	}

	SurfSession::Verb(TEXT("adjust"), FString::Printf(TEXT("kind=%d ok=%d"), static_cast<int32>(Op.Kind), bRan ? 1 : 0));
	return bRan;
}

bool USurfEdMode::RunOperation(const FSurfOperation& Op)
{
	switch (Op.Kind)
	{
	case ESurfOpKind::Move:
	{
		FScopedTransaction Transaction(LOCTEXT("AdjustMove", "Move Control Points"));
		MoveSelectedPoints(Op.Delta);
		NoteOperation(Op);
		return true;
	}

	case ESurfOpKind::Rotate:
	{
		FScopedTransaction Transaction(LOCTEXT("AdjustRotate", "Rotate Control Points"));
		RotateSelectedPoints(Op.Rotation);
		NoteOperation(Op);
		return true;
	}

	case ESurfOpKind::Scale:
	{
		FScopedTransaction Transaction(LOCTEXT("AdjustScale", "Scale Control Points"));
		ScaleSelectedPoints(Op.Scale);
		NoteOperation(Op);
		return true;
	}

	case ESurfOpKind::Extrude:
		if (Op.bMoved)
		{
			// A move of nothing would leave walls of no height.
			if (Op.Delta.IsNearlyZero(0.01))
			{
				return false;
			}

			// E: the extrude in place and the move that followed, as one step
			// now, so the next adjustment undoes one.
			bool bPulledAlong = false;
			{
				FScopedTransaction Transaction(LOCTEXT("AdjustExtrudeMove", "Extrude and Move"));
				const int32 Before = UndoPosition();
				if (!ExtrudeSelectedEdge(/*bInPlace*/ true, ExtrudeBySpacing, static_cast<int32>(Op.Under)))
				{
					// Nothing was changed: the record can go.
					Transaction.Cancel();
					return false;
				}
				bSoftSuspended = true;
				RefreshSoftSelection();
				MoveSelectedPoints(Op.Delta);
				bSoftSuspended = false;
				RefreshSoftSelection();

				// The walls and any closing piece were made in place, facing a
				// guess; with the move done they are turned to face out, as the
				// modal's confirm does. A move along the surface is refused, as
				// the confirm refuses it - by undoing the closed step, because
				// cancelling a transaction drops its record and keeps the edit.
				TurnExtrudedWallsOut();
				bPulledAlong = bExtrudePulledAlongSurface;
			}
			if (bPulledAlong)
			{
				if (GEditor)
				{
					GEditor->UndoTransaction();
				}
				return false;
			}

			FSurfOperation Noted = Op;
			Noted.UndoSteps = 1;
			NoteOperation(Noted);
			return true;
		}
		return ExtrudeSelectedEdge(/*bInPlace*/ false, Op.Distance, static_cast<int32>(Op.Under));

	case ESurfOpKind::Bevel:
	case ESurfOpKind::Smooth:
	case ESurfOpKind::Inflate:
	case ESurfOpKind::Flatten:
	case ESurfOpKind::Random:
		return Inspector ? Inspector->ApplyShape(Op) : false;

	case ESurfOpKind::Surface:
		return Inspector ? Inspector->ApplySurface(Op) : false;

	case ESurfOpKind::Solidify:
		if (FMath::Abs(Op.Distance) < 0.05)
		{
			return false;
		}
		return SolidifySelectedPatch(Op.Distance, Op.Side, Op.bRim, Op.bEven);

	case ESurfOpKind::Duplicate:
		return DuplicateSelectionToPatch(Op.Distance);

	case ESurfOpKind::WeldByDistance:
		return WeldSelectionByDistance(Op.Distance);

	case ESurfOpKind::WeldClusters:
		return WeldSelectionInClusters(Op.Distance);

	case ESurfOpKind::Subdivide:
		return SubdivideSelectedPatches(Op.bRows, Op.bColumns, FMath::Clamp(Op.Iterations, 1, 4));

	case ESurfOpKind::Steps:
		return StepsBetweenSelectedLines(Op.Iterations, Op.Distance);

	case ESurfOpKind::Lattice:
		if (USurfSettings* Remember = GetMutableDefault<USurfSettings>())
		{
			Remember->LatticeResX = FMath::Clamp(Op.Iterations, 2, 8);
			Remember->LatticeResY = FMath::Clamp(Op.Segments, 2, 8);
			Remember->LatticeResZ = FMath::Clamp(Op.Layers, 2, 8);
			Remember->SaveConfig();
		}
		return MakeLattice(Op.Iterations, Op.Segments, Op.Layers);

	case ESurfOpKind::NewSheet:
		if (!Inspector)
		{
			return false;
		}
		Inspector->NewPatchWith(Op.Iterations, Op.Segments, Op.Distance, Op.Preset);
		return true;

	default:
		return false;
	}
}

FString USurfEdMode::DescribeLastOperation() const
{
	switch (LastOperation.Kind)
	{
	case ESurfOpKind::Move:           return TEXT("Move");
	case ESurfOpKind::Rotate:         return TEXT("Rotate");
	case ESurfOpKind::Scale:          return TEXT("Scale");
	case ESurfOpKind::Extrude:        return TEXT("Extrude");
	case ESurfOpKind::Bevel:          return TEXT("Bevel");
	case ESurfOpKind::Smooth:         return TEXT("Smooth");
	case ESurfOpKind::Inflate:        return TEXT("Inflate");
	case ESurfOpKind::Flatten:        return TEXT("Flatten");
	case ESurfOpKind::Random:         return TEXT("Random");
	case ESurfOpKind::Solidify:       return TEXT("Solidify");
	case ESurfOpKind::Duplicate:      return TEXT("Duplicate");
	case ESurfOpKind::WeldByDistance: return TEXT("Weld by distance");
	case ESurfOpKind::WeldClusters:   return TEXT("Weld clusters");
	case ESurfOpKind::Subdivide:      return TEXT("Subdivide");
	case ESurfOpKind::Steps:          return TEXT("Steps");
	case ESurfOpKind::NewSheet:      return TEXT("New Patch");
	case ESurfOpKind::Lattice:        return TEXT("Lattice");
	case ESurfOpKind::Surface:
		switch (LastOperation.SurfaceVerb)
		{
		case ESurfSurfaceVerb::Align:   return TEXT("Align");
		case ESurfSurfaceVerb::Project: return TEXT("Project");
		case ESurfSurfaceVerb::Fit:       return TEXT("Fit");
		case ESurfSurfaceVerb::Normalise: return TEXT("Match density");
		case ESurfSurfaceVerb::Even:      return TEXT("Even");
		default:                          return TEXT("Flow");
		}
	default:                          return TEXT("");
	}
}

// --- Whole-selection transforms ---------------------------------------------

void USurfEdMode::RotateSelectedPoints(const FRotator& WorldRotation)
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	if (PointSelection.Num() == 0 || WorldRotation.IsNearlyZero())
	{
		return;
	}

	// About the selection's own centre, world space, so a selection across
	// two patches turns as one thing. A row of control points is one thing,
	// so turning it has to turn the thing rather than spin each point about
	// the world origin.
	const FVector WorldPivot = PivotOr(GetWidgetLocation());
	const FQuat Rotation = WorldRotation.Quaternion();
	// Primitives picked with the patches turn with them (S249b).
	CarryPickedBrushes(FTranslationMatrix(-WorldPivot) * FQuatRotationMatrix(Rotation) * FTranslationMatrix(WorldPivot));

	ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& Refs)
	{
		const FTransform ToWorld = Group.GetActorTransform();
		const FVector Pivot = ToWorld.InverseTransformPosition(WorldPivot);
		const FQuat LocalRotation = ToWorld.GetRotation().Inverse() * Rotation * ToWorld.GetRotation();

		for (const FSurfPointRef& Ref : Refs)
		{
			if (Ref.X < 0 || Ref.X >= Patch.Width || Ref.Y < 0 || Ref.Y >= Patch.Height)
			{
				continue;
			}

			FSurfControlPoint& Point = Patch.At(Ref.X, Ref.Y);
			Point.Pos = Pivot + LocalRotation.RotateVector(Point.Pos - Pivot);
		}

		// Soft selection applies here too. Turning a row and having its
		// neighbours stay put is not a partial effect, it is a tear.
		if (Patch.Id == SelectedPatch.PatchId && SoftInfluence.Num() == Patch.NumPoints())
		{
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch.Width + X];
					if (Influence <= 0.0f)
					{
						continue;
					}

					FSurfControlPoint& Point = Patch.At(X, Y);
					const FVector Turned = Pivot + LocalRotation.RotateVector(Point.Pos - Pivot);
					Point.Pos = FMath::Lerp(Point.Pos, Turned, Influence);
				}
			}
		}
	});
}

void USurfEdMode::ScaleSelectedPoints(const FVector& Scale)
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	if (PointSelection.Num() == 0 || Scale.Equals(FVector::OneVector, 1.e-6))
	{
		return;
	}

	const FVector WorldPivot = PivotOr(GetWidgetLocation());

	// Never let an axis collapse to nothing: a scale of zero is unrecoverable
	// by scaling back up, and it is easy to do by accident on a dragged gizmo.
	const FVector Safe(
		FMath::IsNearlyZero(Scale.X) ? 1.e-3 : Scale.X,
		FMath::IsNearlyZero(Scale.Y) ? 1.e-3 : Scale.Y,
		FMath::IsNearlyZero(Scale.Z) ? 1.e-3 : Scale.Z);
	// Primitives picked with the patches size with them (S249b).
	CarryPickedBrushes(FTranslationMatrix(-WorldPivot) * FScaleMatrix(Safe) * FTranslationMatrix(WorldPivot));

	ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& Refs)
	{
		const FTransform ToWorld = Group.GetActorTransform();
		const FVector Pivot = ToWorld.InverseTransformPosition(WorldPivot);

		for (const FSurfPointRef& Ref : Refs)
		{
			if (Ref.X < 0 || Ref.X >= Patch.Width || Ref.Y < 0 || Ref.Y >= Patch.Height)
			{
				continue;
			}

			FSurfControlPoint& Point = Patch.At(Ref.X, Ref.Y);
			Point.Pos = Pivot + (Point.Pos - Pivot) * Safe;
		}

		if (Patch.Id == SelectedPatch.PatchId && SoftInfluence.Num() == Patch.NumPoints())
		{
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch.Width + X];
					if (Influence <= 0.0f)
					{
						continue;
					}

					FSurfControlPoint& Point = Patch.At(X, Y);
					const FVector Scaled = Pivot + (Point.Pos - Pivot) * Safe;
					Point.Pos = FMath::Lerp(Point.Pos, Scaled, Influence);
				}
			}
		}
	});
}

void USurfEdMode::SnapshotSelectionInto(USurfChunk& Chunk)
{
	FSurfSelectionSnapshot& Saved = Chunk.SavedSelection;
	Saved.bValid = true;
	Saved.ActivePatchId = SelectedPatch.PatchId;
	Saved.SubMode = static_cast<uint8>(SubMode);
	Saved.Points.Reset();
	Saved.Faces.Reset();

	// Only what lives on this chunk: the chunk is restored on its own, and a
	// selection on some other patch is that patch's chunk's business.
	auto Owns = [&Chunk](const FGuid& PatchId)
	{
		return Chunk.FindPatch(PatchId) != nullptr;
	};

	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Owns(Ref.PatchId))
		{
			FSurfSavedRef& Out = Saved.Points.AddDefaulted_GetRef();
			Out.PatchId = Ref.PatchId;
			Out.X = Ref.X;
			Out.Y = Ref.Y;
		}
	}

	for (const FSurfQuadRef& Ref : QuadSelection)
	{
		if (Owns(Ref.PatchId))
		{
			FSurfSavedRef& Out = Saved.Faces.AddDefaulted_GetRef();
			Out.PatchId = Ref.PatchId;
			Out.X = Ref.X;
			Out.Y = Ref.Y;
		}
	}

	Saved.Patches.Reset();
	for (const FSurfPatchRef& Ref : PatchSelection)
	{
		if (Owns(Ref.PatchId))
		{
			Saved.Patches.Add(Ref.PatchId);
		}
	}
}

bool USurfEdMode::RestoreSelectionAfterUndo()
{
	// Every chunk undo just restored carries the selection as it stood before
	// the edit it undid. Put those together and that is the selection back.
	TArray<FSurfPointRef> Points;
	TArray<FSurfQuadRef> Quads;
	TArray<FSurfPatchRef> Patches;
	FSurfPatchRef Active;
	int32 Restored = 0;
	int32 SavedSubMode = INDEX_NONE;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return false;
	}

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}

		for (USurfChunk* Chunk : Group->Chunks)
		{
			if (!Chunk || !Chunk->bSelectionRestorePending)
			{
				continue;
			}

			Chunk->bSelectionRestorePending = false;

			const FSurfSelectionSnapshot& Saved = Chunk->SavedSelection;
			if (!Saved.bValid)
			{
				continue;
			}

			++Restored;

			if (SavedSubMode == INDEX_NONE)
			{
				SavedSubMode = Saved.SubMode;
			}

			for (const FSurfSavedRef& In : Saved.Points)
			{
				const FSurfPatch* Patch = Group->FindPatch(In.PatchId);
				if (Patch && In.X >= 0 && In.X < Patch->Width && In.Y >= 0 && In.Y < Patch->Height)
				{
					FSurfPointRef Ref;
					Ref.Group = Group;
					Ref.PatchId = In.PatchId;
					Ref.X = In.X;
					Ref.Y = In.Y;
					Points.AddUnique(Ref);
				}
			}

			for (const FSurfSavedRef& In : Saved.Faces)
			{
				const FSurfPatch* Patch = Group->FindPatch(In.PatchId);
				if (Patch && In.X >= 0 && In.X + 1 < Patch->Width && In.Y >= 0 && In.Y + 1 < Patch->Height)
				{
					FSurfQuadRef Ref;
					Ref.Group = Group;
					Ref.PatchId = In.PatchId;
					Ref.X = In.X;
					Ref.Y = In.Y;
					Quads.AddUnique(Ref);
				}
			}

			for (const FGuid& In : Saved.Patches)
			{
				if (Group->FindPatch(In))
				{
					FSurfPatchRef Ref;
					Ref.Group = Group;
					Ref.PatchId = In;
					Patches.AddUnique(Ref);
				}
			}

			if (!Active.IsValid() && Group->FindPatch(Saved.ActivePatchId))
			{
				Active.Group = Group;
				Active.PatchId = Saved.ActivePatchId;
			}
		}
	}

	if (Restored == 0)
	{
		return false;
	}

	if (!Active.IsValid() && Points.Num() > 0)
	{
		Active.Group = Points[0].Group;
		Active.PatchId = Points[0].PatchId;
	}

	if (SavedSubMode >= 0 && SavedSubMode <= static_cast<int32>(ESurfSubMode::Vertex))
	{
		SubMode = static_cast<ESurfSubMode>(SavedSubMode);
	}

	// Every patch that was picked as a whole comes back too, the active one
	// first: a verb over several patches undone and adjusted runs over the
	// same several again.
	SelectedPatch = Active;
	PatchSelection.Reset();
	if (Active.IsValid())
	{
		PatchSelection.Add(Active);
	}
	for (const FSurfPatchRef& Ref : Patches)
	{
		PatchSelection.AddUnique(Ref);
	}
	PointSelection = MoveTemp(Points);
	QuadSelection = MoveTemp(Quads);
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;

	SurfSession::Selection(FString::Printf(TEXT("after undo: restored %d points, %d faces, %d patches from %d chunks"),
		PointSelection.Num(), QuadSelection.Num(), PatchSelection.Num(), Restored));

	return true;
}

void USurfEdMode::OnUndoRedo()
{
	// A modal gesture cannot survive the geometry changing underneath it.
	if (Modal != ESurfModal::None)
	{
		CancelModal();
	}
	// The frame was the last step's; the step is gone, so is the frame -
	// it used to stay as a bare "Move" header (S246c, the Round 2 walk).
	// Not during a replay from the frame itself (S247f): its undo is the
	// knob's, and the verb runs again at once - "trying to adjust slider
	// after clicking From Cavity - has shut the frame".
	if (bOptionsShown && !bReplayingOptions && !CanAdjustLastOperation())
	{
		DismissOptions();
	}

	// Enter undone: the editing group is gone, so the room is over - what
	// was set aside for it comes back, and the camera.
	if (DrilledInstance.IsValid() && !IsValid(DrilledGroup.Get()))
	{
		if (ASurfPrefabInstance* Instance = DrilledInstance.Get())
		{
			Instance->bDrawOwnGeometry = true;
			Instance->RebuildPreview();
		}
		DrilledInstance.Reset();
		DrilledGroup.Reset();
		ClearRoomAside();
		LeaveRoomView();
		RoomFrame = FTransform::Identity;
		bRoomInPlace = false;

		// The stand-ins and the trail's instances are ours and are not part
		// of the undo: dropping the pointers would have left them drawing in
		// the level with nothing able to reach them again.
		auto Sweep = [](TWeakObjectPtr<ASurfPrefabInstance>& Weak)
		{
			if (ASurfPrefabInstance* Each = Weak.Get())
			{
				if (Each->HasAnyFlags(RF_Transient))
				{
					Each->Destroy();
				}
			}
			Weak.Reset();
		};
		Sweep(RoomStandIn);
		Sweep(RoomEditOnlyInstance);
		for (FSurfRoomStep& Step : RoomTrail)
		{
			Sweep(Step.Instance);
		}
		RoomPickedPlacement.Invalidate();
		RoomPlacementsBefore.Reset();
		RoomTrail.Reset();
		SurfSession::Verb(TEXT("exit_prefab"), TEXT("undone"));
	}

	// The chunks that were just restored carry the selection from before the
	// edit; when they do, that is the selection to show. Otherwise keep every
	// reference that still points at something. Undo restores the patch's
	// contents, not its identity, so the references are almost always still
	// good - and dropping them wholesale is what made an undo feel like it
	// had cleared the selection as well.
	const int32 Before = PointSelection.Num();
	const bool bRestored = RestoreSelectionAfterUndo();

	// Every reference that still resolves stays, whichever patch it is on.
	for (int32 I = PointSelection.Num() - 1; I >= 0; --I)
	{
		const FSurfPointRef& Ref = PointSelection[I];
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;

		const bool bStillThere = Patch
			&& Ref.X >= 0 && Ref.X < Patch->Width
			&& Ref.Y >= 0 && Ref.Y < Patch->Height;

		if (!bStillThere)
		{
			PointSelection.RemoveAt(I);
		}
	}

	if (PointSelection.Num() == 0)
	{
		SoftInfluence.Reset();
	}
	else
	{
		RefreshSoftSelection();
	}

	// The preview mesh is built from the patch data, and undo restores the
	// data without telling anything to build again: the cage snapped back
	// while the mesh still showed the state before the undo. Every group is
	// rebuilt because a single undo step can touch more than the one that
	// happens to be selected.
	int32 Rebuilt = 0;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ASurfGroup> It(World); It; ++It)
		{
			if (ASurfGroup* Each = *It)
			{
				Each->RebuildPreview();
				++Rebuilt;
			}
		}
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	if (!bRestored)
	{
		SurfSession::Selection(FString::Printf(TEXT("after undo: kept %d of %d, rebuilt %d"),
			PointSelection.Num(), Before, Rebuilt));
	}

	if (GEditor)
	{
		RedrawViewports();
	}
}

// --- Soft selection -----------------------------------------------------------

void USurfEdMode::RefreshSoftSelection()
{
	SoftInfluence.Reset();

	// Off for the length of an extrude and the move that pulls it out: the
	// falloff would drag the lines the new piece came from along with it,
	// and an extrude is exactly the case where those must stay.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (bSoftSuspended || !Settings || !Settings->bSoftSelection || PointSelection.Num() == 0)
	{
		return;
	}

	ASurfGroup* Group = nullptr;
	const FSurfPatch* Patch = ResolveSelectedPatch(*this, Group);
	if (!Patch || !Group)
	{
		return;
	}

	const double Radius = FMath::Max(1.0, Settings->SoftSelectionRadiusCm);
	SoftInfluence.AddZeroed(Patch->NumPoints());

	/**
	 * Along the grid (S165a): the reach is a walk from the selection over
	 * the control net, each step the length of its segment, so a point on
	 * the far side of a fold is as far as the walk to it. Dijkstra from
	 * every picked point at once; the falloff reads the walked distance.
	 */
	if (Settings->bSoftAlongTheGrid)
	{
		const int32 Count = Patch->NumPoints();
		TArray<double> Walk;
		Walk.Init(TNumericLimits<double>::Max(), Count);
		TArray<int32> Open;
		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.PatchId == Patch->Id && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				const int32 Index = Ref.Y * Patch->Width + Ref.X;
				Walk[Index] = 0.0;
				Open.Add(Index);
			}
		}
		while (Open.Num() > 0)
		{
			// The nearest open point next; small grids, a plain scan will do.
			int32 Best = 0;
			for (int32 I = 1; I < Open.Num(); ++I)
			{
				if (Walk[Open[I]] < Walk[Open[Best]])
				{
					Best = I;
				}
			}
			const int32 Here = Open[Best];
			Open.RemoveAtSwap(Best);
			if (Walk[Here] >= Radius)
			{
				continue;
			}
			const int32 X = Here % Patch->Width;
			const int32 Y = Here / Patch->Width;
			const FVector& From = Patch->At(X, Y).Pos;
			for (const FIntPoint& D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
			{
				const int32 NX = X + D.X;
				const int32 NY = Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= Patch->Width || NY >= Patch->Height)
				{
					continue;
				}
				const int32 Next = NY * Patch->Width + NX;
				const double Through = Walk[Here] + FVector::Dist(From, Patch->At(NX, NY).Pos);
				if (Through < Walk[Next])
				{
					Walk[Next] = Through;
					Open.AddUnique(Next);
				}
			}
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Walk[Index] > 0.0 && Walk[Index] < Radius)
			{
				SoftInfluence[Index] = static_cast<float>(
					USurfSettings::Falloff(Settings->SoftSelectionFalloff, Walk[Index] / Radius, Index + 1));
			}
		}
		return;
	}

	// Distance to the nearest selected point, not to their centroid: a long
	// selection should pull along its whole length, not from its middle.
	for (int32 Y = 0; Y < Patch->Height; ++Y)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			const int32 Index = Y * Patch->Width + X;
			const FVector& Position = Patch->At(X, Y).Pos;

			double Nearest = TNumericLimits<double>::Max();
			bool bIsSelected = false;

			for (const FSurfPointRef& Ref : PointSelection)
			{
				if (Ref.X == X && Ref.Y == Y)
				{
					bIsSelected = true;
					break;
				}

				if (Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
				{
					Nearest = FMath::Min(Nearest, FVector::Dist(Position, Patch->At(Ref.X, Ref.Y).Pos));
				}
			}

			if (bIsSelected || Nearest >= Radius)
			{
				continue;
			}

			SoftInfluence[Index] = static_cast<float>(
				USurfSettings::Falloff(Settings->SoftSelectionFalloff, Nearest / Radius, Index + 1));
		}
	}
}

float USurfEdMode::GetSoftInfluence(const FSurfPointRef& Point) const
{
	if (SoftInfluence.Num() == 0)
	{
		return 0.0f;
	}

	const ASurfGroup* Group = SelectedPatch.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;
	if (!Patch || Point.PatchId != SelectedPatch.PatchId)
	{
		return 0.0f;
	}

	if (Point.X < 0 || Point.X >= Patch->Width || Point.Y < 0 || Point.Y >= Patch->Height)
	{
		return 0.0f;
	}

	const int32 Index = Point.Y * Patch->Width + Point.X;
	return SoftInfluence.IsValidIndex(Index) ? SoftInfluence[Index] : 0.0f;
}

bool USurfEdMode::ToggleSoftSelection()
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return false;
	}

	Settings->bSoftSelection = !Settings->bSoftSelection;
	SoftRadiusShownAt = FPlatformTime::Seconds();
	RefreshSoftSelection();

	if (GEditor)
	{
		RedrawViewports();
	}

	if (Inspector)
	{
		Inspector->PullSoftSelection();
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("soft_selection"), Settings->bSoftSelection ? TEXT("on") : TEXT("off"));
	return Settings->bSoftSelection;
}

bool USurfEdMode::CycleSoftFalloff(int32 Direction)
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return false;
	}
	// The size of the set, not the eight it happened to have: a falloff
	// added to the end would have been skipped by the cycle for ever, and
	// nothing would have said so (S137a).
	const int32 Shapes = static_cast<int32>(ESurfFalloff::Count);
	return SetSoftFalloff(static_cast<ESurfFalloff>(
		(static_cast<int32>(Settings->SoftSelectionFalloff) + Direction + Shapes) % Shapes));
}

bool USurfEdMode::SetSoftFalloff(ESurfFalloff Shape)
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return false;
	}

	Settings->SoftSelectionFalloff = Shape;
	Settings->SaveConfig();

	// Choosing a profile is reaching for proportional editing: on, if it
	// was off, so the choice is seen.
	if (!Settings->bSoftSelection)
	{
		Settings->bSoftSelection = true;
	}
	SoftRadiusShownAt = FPlatformTime::Seconds();
	RefreshSoftSelection();

	if (Inspector)
	{
		Inspector->PullSoftSelection();
		// And what an unusual one will do, said once, where he is looking.
		Inspector->SetResult(Shape == ESurfFalloff::Random
			? FString(TEXT("soft selection: random falloff - each point follows by its own share, so a drag makes a rough surface on purpose"))
			: (USurfSettings::FalloffIsUsual(Shape)
				? FString::Printf(TEXT("soft selection: %s falloff"), *USurfSettings::FalloffName(Shape))
				: FString::Printf(TEXT("soft selection: %s falloff - the reach on the bar is amber while it is not smooth"),
					*USurfSettings::FalloffName(Shape))));
		Inspector->RefreshSelectionCount();
	}
	if (GEditor)
	{
		RedrawViewports();
	}

	SurfSession::Verb(TEXT("soft_falloff"), USurfSettings::FalloffName(Shape));
	return true;
}

// --- Same and Replace (S64a) -----------------------------------------------
//
// The two verbs that turn a retexture pass from an afternoon into a minute
// (product definition §2b, §2g). Same reads the material off what is active
// and takes everything in the level that wears it; Replace swaps one
// material for another everywhere it appears and touches nothing else, so
// every projection survives. Neither knows what a material is beyond an
// identity to compare - which is what lets them work on the checker too: a
// patch wearing nothing is as findable as one wearing marble.

bool USurfEdMode::CopySurfaceToClick(const FSurfPickContext& Ctx, bool bFaceHit, const FSurfFaceRef& FaceUnder, const FVector& FaceHit, bool bMaterialOnly)
{
	// The target: whatever is nearer under the cursor, a face or a patch.
	FSurfPatchRef PatchUnder;
	FVector PatchHit = FVector::ZeroVector;
	const bool bPatchHit = PickPatchWith(Ctx, PatchUnder, &PatchHit);
	if (!bFaceHit && !bPatchHit)
	{
		return false;
	}
	const bool bToPatch = bPatchHit
		&& (!bFaceHit || FVector::Dist(Ctx.Origin, PatchHit) <= FVector::Dist(Ctx.Origin, FaceHit));

	// The source: the active patch, else the active face.
	const FSurfPatchRef SourceRef = GetSelectedPatch();
	const ASurfGroup* SourceGroup = SourceRef.IsValid() ? SourceRef.Group.Get() : ActiveFace.Group.Get();
	const FSurfPatch* SourcePatchPtr = SourceRef.IsValid() && SourceGroup ? SourceGroup->FindPatch(SourceRef.PatchId) : nullptr;
	const FSurfBrush* SourceBrush = !SourcePatchPtr && SourceGroup ? SourceGroup->FindBrush(ActiveFace.BrushId) : nullptr;
	const FSurfFace* SourceFacePtr = SourceBrush && SourceBrush->Faces.IsValidIndex(ActiveFace.FaceIndex) ? &SourceBrush->Faces[ActiveFace.FaceIndex] : nullptr;
	if (!SourcePatchPtr && !SourceFacePtr)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("copy: pick the surface to copy from first, then Shift+middle-click the one that takes it (Ctrl+Shift for its mapping too)"));
		}
		SurfSession::Friction(TEXT("copy_surface_no_source"));
		return true;
	}
	if ((bToPatch && SourcePatchPtr && PatchUnder == SourceRef) || (!bToPatch && SourceFacePtr && FaceUnder == ActiveFace))
	{
		return true;
	}

	const FSurfPatch SourcePatch = SourcePatchPtr ? *SourcePatchPtr : FSurfPatch();
	const FSurfFace SourceFace = SourceFacePtr ? *SourceFacePtr : FSurfFace();
	UMaterialInterface* Material = SourcePatchPtr ? SourcePatch.Material.Get() : SourceFace.Material.Get();

	FScopedTransaction Transaction(NSLOCTEXT("SurfacingProEdMode", "CopySurfaceTo", "Copy Surface Onto"));
	ASurfGroup* Group = bToPatch ? PatchUnder.Group.Get() : FaceUnder.Group.Get();
	if (!Group)
	{
		Transaction.Cancel();
		return true;
	}
	if (bToPatch)
	{
		FSurfPatch* Patch = Group->ModifyPatch(PatchUnder.PatchId);
		if (!Patch)
		{
			Transaction.Cancel();
			return true;
		}
		const int32 Layer = Patch->StackLevel;
		if (bMaterialOnly)
		{
			Patch->Material = Material;
		}
		else if (SourcePatchPtr)
		{
			SurfPatchSurfacing::CopySurfaceDefinition(SourcePatch, *Patch);
		}
		else
		{
			SurfPatchSurfacing::CopyFaceSurface(SourceFace, *Patch);
		}
		Patch->StackLevel = Layer;
		SurfPatchSurfacing::Reapply(*Patch);
		++Patch->EditSerial;
	}
	else
	{
		FSurfBrush* Brush = Group->ModifyBrush(FaceUnder.BrushId);
		if (!Brush || !Brush->Faces.IsValidIndex(FaceUnder.FaceIndex))
		{
			Transaction.Cancel();
			return true;
		}
		FSurfFace& Face = Brush->Faces[FaceUnder.FaceIndex];
		if (bMaterialOnly)
		{
			Face.Material = Material;
		}
		else if (SourcePatchPtr)
		{
			const FVector Normal = FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z).GetSafeNormal();
			Face.Material = SourcePatch.Material;
			Face.SmoothingGroup = SourcePatch.SmoothingGroup;
			Face.UV.Scale = SourcePatch.Scale;
			SurfProjection::Align(Face.UV, Normal);
			if (!FMath::IsNearlyZero(SourcePatch.UVRotation))
			{
				SurfProjection::SetRotationDegrees(Face.UV, Normal, SourcePatch.UVRotation);
			}
		}
		else
		{
			SurfProjection::CopySurfaceDefinition(SourceFace, Face);
		}
		++Brush->EditSerial;
	}
	Group->RebuildPreview();

	if (Inspector)
	{
		const FString What = Material ? Material->GetName() : FString(TEXT("the checker"));
		Inspector->SetResult(FString::Printf(TEXT("copied %s%s onto the %s under the cursor"),
			*What, bMaterialOnly ? TEXT("") : TEXT(" and its mapping"), bToPatch ? TEXT("patch") : TEXT("face")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("copy_surface_to"), FString::Printf(TEXT("to=%s material_only=%d"), bToPatch ? TEXT("patch") : TEXT("face"), bMaterialOnly ? 1 : 0));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::CopySurfaceFromClick(const FSurfPickContext& Ctx, bool bFaceHit, const FSurfFaceRef& FaceUnder, const FVector& FaceHit)
{
	// The source is whatever is nearer under the cursor: a brush face or a
	// patch. It used to be a face only, and only the selected faces took it;
	// the gesture is the same for a patch on either end (S68d, §2b).
	FSurfPatchRef PatchUnder;
	FVector PatchHit = FVector::ZeroVector;
	const bool bPatchHit = PickPatchWith(Ctx, PatchUnder, &PatchHit);
	if (!bFaceHit && !bPatchHit)
	{
		return false;
	}
	const bool bFromPatch = bPatchHit
		&& (!bFaceHit || FVector::Dist(Ctx.Origin, PatchHit) <= FVector::Dist(Ctx.Origin, FaceHit));

	const ASurfGroup* SourceGroup = bFromPatch ? PatchUnder.Group.Get() : FaceUnder.Group.Get();
	const FSurfPatch* SourcePatchPtr = bFromPatch && SourceGroup ? SourceGroup->FindPatch(PatchUnder.PatchId) : nullptr;
	const FSurfBrush* SourceBrush = !bFromPatch && SourceGroup ? SourceGroup->FindBrush(FaceUnder.BrushId) : nullptr;
	const FSurfFace* SourceFacePtr = SourceBrush && SourceBrush->Faces.IsValidIndex(FaceUnder.FaceIndex) ? &SourceBrush->Faces[FaceUnder.FaceIndex] : nullptr;
	if (!SourcePatchPtr && !SourceFacePtr)
	{
		return true;
	}

	// The targets: the selected faces, and every patch the selection holds -
	// the source itself excepted, whichever end it is on.
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Patches.AddUnique(Held);
	}
	if (bFromPatch)
	{
		Patches.Remove(PatchUnder);
	}
	TArray<FSurfFaceRef> Faces = Selection;
	if (!bFromPatch)
	{
		Faces.Remove(FaceUnder);
	}
	if (Patches.Num() == 0 && Faces.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("copy: select what should take the surface first, then middle-click the surface to copy"));
		}
		SurfSession::Friction(TEXT("copy_surface_no_target"));
		return true;
	}

	// Copies, taken before anything is modified: a target may share the
	// source's chunk.
	const FSurfPatch SourcePatch = SourcePatchPtr ? *SourcePatchPtr : FSurfPatch();
	const FSurfFace SourceFace = SourceFacePtr ? *SourceFacePtr : FSurfFace();
	UMaterialInterface* Material = bFromPatch ? SourcePatch.Material.Get() : SourceFace.Material.Get();

	FScopedTransaction Transaction(NSLOCTEXT("SurfacingProEdMode", "CopySurface", "Copy Surface"));

	TArray<ASurfGroup*> Groups;
	int32 DonePatches = 0;
	int32 DoneFaces = 0;

	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		// The layer is where the patch sits, not part of its surface: a
		// decal that takes the floor's marble stays a decal.
		const int32 Layer = Patch->StackLevel;
		if (bFromPatch)
		{
			SurfPatchSurfacing::CopySurfaceDefinition(SourcePatch, *Patch);
		}
		else
		{
			SurfPatchSurfacing::CopyFaceSurface(SourceFace, *Patch);
		}
		Patch->StackLevel = Layer;
		SurfPatchSurfacing::Reapply(*Patch);
		++Patch->EditSerial;
		Groups.AddUnique(Group);
		++DonePatches;
	}

	for (const FSurfFaceRef& Ref : Faces)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush || !Brush->Faces.IsValidIndex(Ref.FaceIndex))
		{
			continue;
		}
		FSurfFace& Face = Brush->Faces[Ref.FaceIndex];
		if (bFromPatch)
		{
			// A patch's mapping is a rule; a face gets the rule's numbers on
			// its own axial projection: material, scale, turn, smoothing.
			const FVector Normal = FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z).GetSafeNormal();
			Face.Material = SourcePatch.Material;
			Face.SmoothingGroup = SourcePatch.SmoothingGroup;
			Face.UV.Scale = SourcePatch.Scale;
			SurfProjection::Align(Face.UV, Normal);
			if (!FMath::IsNearlyZero(SourcePatch.UVRotation))
			{
				SurfProjection::SetRotationDegrees(Face.UV, Normal, SourcePatch.UVRotation);
			}
		}
		else
		{
			SurfProjection::CopySurfaceDefinition(SourceFace, Face);
		}
		++Brush->EditSerial;
		Groups.AddUnique(Group);
		++DoneFaces;
	}

	if (DonePatches == 0 && DoneFaces == 0)
	{
		Transaction.Cancel();
		return true;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		const FString What = Material ? Material->GetName() : FString(TEXT("the checker"));
		const FString PatchPart = DonePatches > 0 ? FString::Printf(TEXT("%d patch%s"), DonePatches, DonePatches == 1 ? TEXT("") : TEXT("es")) : FString();
		const FString FacePart = DoneFaces > 0 ? FString::Printf(TEXT("%d face%s"), DoneFaces, DoneFaces == 1 ? TEXT("") : TEXT("s")) : FString();
		Inspector->SetResult(FString::Printf(TEXT("copied %s and its mapping from the %s onto %s%s%s"),
			*What, bFromPatch ? TEXT("patch") : TEXT("face"),
			*PatchPart, DonePatches > 0 && DoneFaces > 0 ? TEXT(" and ") : TEXT(""), *FacePart));
		Inspector->RefreshSelectionCount();
		if (DoneFaces > 0)
		{
			Inspector->PullFromActiveFace();
		}
	}
	SurfSession::Verb(TEXT("copy_surface"), FString::Printf(TEXT("from=%s patches=%d faces=%d"),
		bFromPatch ? TEXT("patch") : TEXT("face"), DonePatches, DoneFaces));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::MaterialOfActive(UMaterialInterface*& OutMaterial) const
{
	OutMaterial = nullptr;

	if (SelectedPatch.IsValid())
	{
		const ASurfGroup* Group = SelectedPatch.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;
		if (Patch)
		{
			OutMaterial = Patch->Material.Get();
			return true;
		}
	}

	if (ActiveFace.IsValid())
	{
		const ASurfGroup* Group = ActiveFace.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(ActiveFace.BrushId) : nullptr;
		if (Brush && Brush->Faces.IsValidIndex(ActiveFace.FaceIndex))
		{
			OutMaterial = Brush->Faces[ActiveFace.FaceIndex].Material.Get();
			return true;
		}
	}

	return false;
}

int32 USurfEdMode::SelectByMaterial()
{
	UMaterialInterface* Wanted = nullptr;
	if (!MaterialOfActive(Wanted))
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("same: nothing selected - pick a patch or a face wearing the material to find"));
		}
		return 0;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	// The active one stays active: what joins is company, not a new leader.
	const FSurfPatchRef WasActive = SelectedPatch;
	int32 Patches = 0;
	int32 Faces = 0;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (Patch.Material.Get() != Wanted)
			{
				return;
			}
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch.Id;
			if (!PatchSelection.Contains(Ref) && !(Ref == SelectedPatch))
			{
				AddPatchToSelection(Ref, /*bToggle*/ false);
				++Patches;
			}
		});

		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			for (int32 I = 0; I < Brush.Faces.Num(); ++I)
			{
				if (Brush.Faces[I].Material.Get() != Wanted)
				{
					continue;
				}
				FSurfFaceRef Ref;
				Ref.Group = Group;
				Ref.BrushId = Brush.Id;
				Ref.FaceIndex = I;
				if (!Selection.Contains(Ref))
				{
					SelectFace(Ref, /*bAddToSelection*/ true);
					++Faces;
				}
			}
		});
	}

	if (WasActive.IsValid())
	{
		SelectedPatch = WasActive;
	}

	const FString Name = Wanted ? Wanted->GetName() : FString(TEXT("nothing (the checker)"));
	if (Inspector)
	{
		if (Patches + Faces == 0)
		{
			Inspector->SetResult(FString::Printf(TEXT("same: nothing else in the level wears %s"), *Name));
		}
		else
		{
			Inspector->SetResult(FString::Printf(TEXT("same: %d patch%s and %d face%s wearing %s joined the selection"),
				Patches, Patches == 1 ? TEXT("") : TEXT("es"), Faces, Faces == 1 ? TEXT("") : TEXT("s"), *Name));
		}
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("select_by_material"), FString::Printf(TEXT("patches=%d faces=%d"), Patches, Faces));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Patches + Faces;
}

int32 USurfEdMode::ReplaceMaterialEverywhere(UMaterialInterface* From, UMaterialInterface* To)
{
	if (From == To)
	{
		return 0;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	FScopedTransaction Transaction(LOCTEXT("ReplaceMaterial", "Replace Material"));

	int32 Patches = 0;
	int32 Faces = 0;
	TArray<ASurfGroup*> Touched;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}

		TArray<FGuid> PatchIds;
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (Patch.Material.Get() == From)
			{
				PatchIds.Add(Patch.Id);
			}
		});
		TArray<FGuid> BrushIds;
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			for (const FSurfFace& Face : Brush.Faces)
			{
				if (Face.Material.Get() == From)
				{
					BrushIds.AddUnique(Brush.Id);
					break;
				}
			}
		});
		if (PatchIds.Num() == 0 && BrushIds.Num() == 0)
		{
			continue;
		}

		Group->Modify();
		Touched.Add(Group);

		// The material and nothing else: the mapping, the scale, the turn,
		// the flips and the held UVs are exactly what a replace must keep.
		for (const FGuid& Id : PatchIds)
		{
			if (FSurfPatch* Patch = Group->ModifyPatch(Id))
			{
				Patch->Material = To;
				++Patch->EditSerial;
				++Patches;
			}
		}
		for (const FGuid& Id : BrushIds)
		{
			if (FSurfBrush* Brush = Group->ModifyBrush(Id))
			{
				for (FSurfFace& Face : Brush->Faces)
				{
					if (Face.Material.Get() == From)
					{
						Face.Material = To;
						++Faces;
					}
				}
			}
		}
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	SurfSession::Verb(TEXT("replace_material"), FString::Printf(TEXT("patches=%d faces=%d"), Patches, Faces));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Patches + Faces;
}

bool USurfEdMode::ReplaceMaterialFromSelection()
{
	// The gesture is Join's and Density's: pick the ones wearing the old
	// material, pick one wearing the new material last, press Replace.
	// The first picked names what goes, the active names what comes.
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	UMaterialInterface* From = nullptr;
	UMaterialInterface* To = nullptr;
	if (Targets.Num() >= 2 && SelectedPatch.IsValid())
	{
		const FSurfPatchRef First = Targets[0] == SelectedPatch ? Targets[1] : Targets[0];
		const ASurfGroup* FromGroup = First.Group.Get();
		const FSurfPatch* FromPatch = FromGroup ? FromGroup->FindPatch(First.PatchId) : nullptr;
		const ASurfGroup* ToGroup = SelectedPatch.Group.Get();
		const FSurfPatch* ToPatch = ToGroup ? ToGroup->FindPatch(SelectedPatch.PatchId) : nullptr;
		if (!FromPatch || !ToPatch)
		{
			return false;
		}
		From = FromPatch->Material.Get();
		To = ToPatch->Material.Get();
	}
	else if (Selection.Num() >= 2 && ActiveFace.IsValid())
	{
		// Brush faces name the pair the same way (S67a): the first picked
		// goes, the active one comes.
		const FSurfFaceRef First = Selection[0] == ActiveFace ? Selection[1] : Selection[0];
		auto MaterialOf = [](const FSurfFaceRef& Ref) -> const FSurfFace*
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			return Brush && Brush->Faces.IsValidIndex(Ref.FaceIndex) ? &Brush->Faces[Ref.FaceIndex] : nullptr;
		};
		const FSurfFace* FromFace = MaterialOf(First);
		const FSurfFace* ToFace = MaterialOf(ActiveFace);
		if (!FromFace || !ToFace)
		{
			return false;
		}
		From = FromFace->Material.Get();
		To = ToFace->Material.Get();
	}
	else
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("replace: pick a patch or primitive face wearing the material to replace, then one wearing the new material, then Replace"));
		}
		return false;
	}
	if (From == To)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("replace: the first picked and the active one wear the same material - nothing to replace it with"));
		}
		return false;
	}

	const int32 Changed = ReplaceMaterialEverywhere(From, To);
	if (Inspector)
	{
		const FString FromName = From ? From->GetName() : FString(TEXT("nothing"));
		const FString ToName = To ? To->GetName() : FString(TEXT("nothing"));
		Inspector->SetResult(FString::Printf(TEXT("replaced %s with %s on %d surface%s across the level - every projection kept"),
			*FromName, *ToName, Changed, Changed == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	return Changed > 0;
}

bool USurfEdMode::NudgeSnapReach(double Factor)
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return false;
	}
	Settings->PointSnapRadiusCm = FMath::Clamp(Settings->PointSnapRadiusCm * Factor, USurfSettings::SnapReachMinCm, USurfSettings::SnapReachMaxCm);
	Settings->SaveConfig();
	SnapReachShownAt = FPlatformTime::Seconds();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("snap reach %.0f cm"), Settings->PointSnapRadiusCm));
	}
	SurfSession::Verb(TEXT("snap_reach"), FString::Printf(TEXT("%.1f cm"), Settings->PointSnapRadiusCm));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::NudgeSoftRadius(double Factor)
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings || Factor <= 0.0)
	{
		return false;
	}

	// Reaching for the radius is the same intent as reaching for the toggle:
	// growing it turns proportional editing on, and shrinking it past the
	// smallest useful radius turns it off - a falloff of a few centimetres
	// is not proportional editing, it is a setting left on by mistake.
	constexpr double SmallestCm = USurfSettings::SoftReachMinCm;
	if (Factor < 1.0)
	{
		if (!Settings->bSoftSelection)
		{
			return false;
		}
		if (Settings->SoftSelectionRadiusCm <= SmallestCm + KINDA_SMALL_NUMBER)
		{
			Settings->bSoftSelection = false;
			Settings->SoftSelectionRadiusCm = SmallestCm;
			Settings->SaveConfig();
			RefreshSoftSelection();
			if (GEditor)
			{
				RedrawViewports();
			}
			if (Inspector)
			{
				Inspector->PullSoftSelection();
				Inspector->SetResult(TEXT("soft selection: off (the radius reached its smallest)"));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Verb(TEXT("soft_selection"), TEXT("off - radius at its smallest"));
			return true;
		}
	}
	else if (!Settings->bSoftSelection)
	{
		Settings->bSoftSelection = true;
		Settings->SoftSelectionRadiusCm = SmallestCm;
		SurfSession::Verb(TEXT("soft_selection"), TEXT("on - radius grown from off"));
	}

	Settings->SoftSelectionRadiusCm = FMath::Clamp(Settings->SoftSelectionRadiusCm * Factor, SmallestCm, USurfSettings::SoftReachMaxCm);

	// Show the ring, and redraw now rather than on the next click - the
	// feedback is the whole point of adjusting it in the viewport.
	SoftRadiusShownAt = FPlatformTime::Seconds();

	RefreshSoftSelection();

	if (GEditor)
	{
		RedrawViewports();
	}

	if (Inspector)
	{
		Inspector->PullSoftSelection();
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("soft_radius"), FString::Printf(TEXT("%.0f cm"), Settings->SoftSelectionRadiusCm));
	return true;
}

// --- Selection verbs ---------------------------------------------------------

bool USurfEdMode::SelectAllPoints()
{
	// All of every patch that is selected: one, or the several picked in
	// object sub-mode, so a move of several patches moves all of them - or
	// of every patch in the level when none is selected, so A always
	// selects something.
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		// Every patch the points and faces reach into, whether or not the
		// patch selection has caught up with them.
		Patches.AddUnique(Held);
	}

	// A primitive being worked on and no patch (S225a: "ive clicked it - i
	// press A it selects all from the other mesh"): all of the primitive -
	// its corners in 1, its sides otherwise.
	if (Patches.Num() == 0)
	{
		TArray<TPair<TWeakObjectPtr<ASurfGroup>, FGuid>> Worked;
		if (ActiveFace.IsValid()) { Worked.AddUnique(TPair<TWeakObjectPtr<ASurfGroup>, FGuid>(ActiveFace.Group, ActiveFace.BrushId)); }
		for (const FSurfFaceRef& Ref : Selection) { if (Ref.IsValid()) { Worked.AddUnique(TPair<TWeakObjectPtr<ASurfGroup>, FGuid>(Ref.Group, Ref.BrushId)); } }
		for (const FSurfBrushCornerRef& Ref : BrushCornerSelection) { if (Ref.Group.IsValid()) { Worked.AddUnique(TPair<TWeakObjectPtr<ASurfGroup>, FGuid>(Ref.Group, Ref.BrushId)); } }
		if (Worked.Num() > 0)
		{
			int32 Corners = 0, Sides = 0;
			for (const TPair<TWeakObjectPtr<ASurfGroup>, FGuid>& Each : Worked)
			{
				ASurfGroup* Group = Each.Key.Get();
				const FSurfBrush* Brush = Group ? Group->FindBrush(Each.Value) : nullptr;
				if (!Brush) { continue; }
				if (SubMode == ESurfSubMode::Vertex)
				{
					FSurfHull Hull;
					if (!SurfBrushMath::DeriveHull(*Brush, Hull)) { continue; }
					for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
					{
						FSurfBrushCornerRef Corner;
						Corner.Group = Group;
						Corner.BrushId = Brush->Id;
						Corner.Corner = I;
						BrushCornerSelection.AddUnique(Corner);
						++Corners;
					}
				}
				else
				{
					for (int32 I = 0; I < Brush->Faces.Num(); ++I)
					{
						FSurfFaceRef Face;
						Face.Group = Group;
						Face.BrushId = Brush->Id;
						Face.FaceIndex = I;
						Selection.AddUnique(Face);
						++Sides;
					}
				}
			}
			if (Inspector)
			{
				Inspector->SetResult(Corners > 0
					? FString::Printf(TEXT("select all: %d corner%s of the primitive"), Corners, Corners == 1 ? TEXT("") : TEXT("s"))
					: FString::Printf(TEXT("select all: %d side%s of the primitive"), Sides, Sides == 1 ? TEXT("") : TEXT("s")));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Selection(FString::Printf(TEXT("select_all primitive corners=%d sides=%d"), Corners, Sides));
			RedrawViewports();
			return true;
		}
	}

	if (Patches.Num() == 0)
	{
		Patches = GetAllPatches();
	}
	if (Patches.Num() == 0)
	{
		// Claimed even with nothing to select. Returning false handed the key
		// to the editor, whose Select All then took every actor in the level -
		// which is not a smaller version of what was asked for.
		if (Inspector)
		{
			Inspector->SetResult(TEXT("select all: there is no patch in the level"));
			Inspector->RefreshSelectionCount();
		}

		SurfSession::Friction(TEXT("select_all_no_patch"));
		return true;
	}

	PointSelection.Reset();

	for (const FSurfPatchRef& Each : Patches)
	{
		const ASurfGroup* EachGroup = Each.Group.Get();
		const FSurfPatch* EachPatch = EachGroup ? EachGroup->FindPatch(Each.PatchId) : nullptr;
		if (!EachPatch)
		{
			continue;
		}

		PointSelection.Reserve(PointSelection.Num() + EachPatch->NumPoints());
		for (int32 Y = 0; Y < EachPatch->Height; ++Y)
		{
			for (int32 X = 0; X < EachPatch->Width; ++X)
			{
				FSurfPointRef Ref;
				Ref.Group = Each.Group;
				Ref.PatchId = Each.PatchId;
				Ref.X = X;
				Ref.Y = Y;
				PointSelection.Add(Ref);
			}
		}
	}

	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;

	if (!SelectedPatch.IsValid())
	{
		SelectedPatch = Patches.Last();
	}
	SyncPatchSelectionToPoints();
	RefreshSoftSelection();

	SurfSession::Selection(FString::Printf(TEXT("select_all points=%d patches=%d"), PointSelection.Num(), Patches.Num()));

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("selected every point: %d on %d patch%s"), PointSelection.Num(), Patches.Num(), Patches.Num() == 1 ? TEXT("") : TEXT("es")));
		Inspector->RefreshSelectionCount();
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::InvertPointSelection()
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Patch = ResolveSelectedPatch(*this, Group);
	if (!Patch || !Group)
	{
		return false;
	}

	/**
	 * In faces (3) it is the faces that invert (S165a): "im selecting some
	 * face - pressing ctrl i to invert - pressing delete: everything has
	 * deleted!" - the points inverted and the faces stayed as they were, so
	 * Delete took the six he had picked, not the rest. Every cell of the
	 * patch not picked is picked, and the points are their corners.
	 */
	if (SubMode == ESurfSubMode::Face)
	{
		TArray<FSurfQuadRef> Inverted;
		for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
		{
			for (int32 X = 0; X + 1 < Patch->Width; ++X)
			{
				FSurfQuadRef Quad;
				Quad.Group = Group;
				Quad.PatchId = Patch->Id;
				Quad.X = X;
				Quad.Y = Y;
				if (!QuadSelection.Contains(Quad))
				{
					Inverted.Add(Quad);
				}
			}
		}
		QuadSelection = MoveTemp(Inverted);
		SyncQuadSelectionToPoints();
		LastRowColumnPoint = FSurfPointRef();
		RowColumnStage = 2;
		RefreshSoftSelection();
		SurfSession::Selection(FString::Printf(TEXT("invert faces=%d points=%d"), QuadSelection.Num(), PointSelection.Num()));
		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
		if (GEditor)
		{
			RedrawViewports();
		}
		return true;
	}

	TArray<FSurfPointRef> Inverted;
	Inverted.Reserve(Patch->NumPoints());

	for (int32 Y = 0; Y < Patch->Height; ++Y)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			FSurfPointRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch->Id;
			Ref.X = X;
			Ref.Y = Y;

			if (PointSelection.IndexOfByKey(Ref) == INDEX_NONE)
			{
				Inverted.Add(Ref);
			}
		}
	}

	PointSelection = MoveTemp(Inverted);
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;

	RefreshSoftSelection();

	SurfSession::Selection(FString::Printf(TEXT("invert points=%d"), PointSelection.Num()));

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	return true;
}

// --- Growing and copying ------------------------------------------------------

void USurfEdMode::CarryGrownUVs()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	ASurfGroup* Group = GrownGroup.Get();
	if (!Settings || !Settings->bExtrudeCarriesUVs || !Group)
	{
		return;
	}
	const FSurfPatch* Found = Group->FindPatch(GrownPatch);
	const int32 Along = Found ? (bGrownColumn ? Found->Width : Found->Height) : 0;
	if (!Found || Along < 3 || Found->Mapping != ESurfPatchMapping::Manual)
	{
		return;
	}
	FSurfPatch* Patch = Group->ModifyPatch(GrownPatch);
	if (!Patch)
	{
		return;
	}
	const int32 NewAt = bGrownFromEnd ? Along - 1 : 0;
	const int32 EdgeAt = bGrownFromEnd ? Along - 2 : 1;
	const int32 InnerAt = bGrownFromEnd ? Along - 3 : 2;
	const int32 Across = bGrownColumn ? Patch->Height : Patch->Width;
	auto At = [Patch](bool bColumn, int32 Line, int32 B) -> FSurfControlPoint& { return bColumn ? Patch->At(Line, B) : Patch->At(B, Line); };
	for (int32 B = 0; B < Across; ++B)
	{
		FSurfControlPoint& New = At(bGrownColumn, NewAt, B);
		const FSurfControlPoint& Edge = At(bGrownColumn, EdgeAt, B);
		const FSurfControlPoint& Inner = At(bGrownColumn, InnerAt, B);
		const double Cell = FVector::Dist(Edge.Pos, Inner.Pos);
		const double Grown = FVector::Dist(New.Pos, Edge.Pos);
		const double Ratio = Cell > KINDA_SMALL_NUMBER ? Grown / Cell : 1.0;
		New.UV = Edge.UV + (Edge.UV - Inner.UV) * Ratio;
		New.SavedUV = Edge.SavedUV + (Edge.SavedUV - Inner.SavedUV) * Ratio;
	}
}

bool USurfEdMode::GrowSelectedBorder(bool bColumn, bool bFromEnd, bool bInPlace, double OffsetCm)
{
	// The one case where the patch itself grows: the whole of one border
	// line is selected, so the grid can carry on past it. Everything else
	// extrude does is in SurfEdModeExtrude.cpp, and ends in new patches.
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group || PointSelection.Num() == 0)
	{
		return false;
	}

	const FSurfPatch Patch = *Found;
	const FGuid PatchId = Patch.Id;

	FScopedTransaction Transaction(LOCTEXT("Extrude", "Extrude Edge"));

	FSurfPatch* Mutable = Group->ModifyPatch(PatchId);
	if (!Mutable || !SurfPatchPrimitives::ExtrudeEdge(*Mutable, bColumn, bFromEnd, 1, bInPlace))
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("extrude: the patch has no room left to grow"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_refused"));
		return false;
	}

	// Growing the low edge used to slide every existing UV along by the
	// new line's spacing - and by nothing at all when the line was grown in
	// place, so the move that followed did the sliding instead. The patch
	// now keeps its UV origin on the point it was measured from (see
	// FSurfPatch::UVOriginX), whichever side it grows on.

	// A distance asked for (the adjust frame's knob, the button with an
	// offset): the new line goes that far along the way the grid continues,
	// one cell being what the grow itself gave it. Negative folds it back
	// under the patch.
	if (!bInPlace && OffsetCm > ExtrudeBySpacing * 0.5)
	{
		const double Distance = FMath::Abs(OffsetCm) < 0.1 ? (OffsetCm < 0.0 ? -0.1 : 0.1) : OffsetCm;
		const int32 NewLineAt = bFromEnd ? (bColumn ? Mutable->Width - 1 : Mutable->Height - 1) : 0;
		const int32 EdgeAt = bFromEnd ? NewLineAt - 1 : 1;
		const int32 InnerAt = bFromEnd ? NewLineAt - 2 : 2;
		const int32 AcrossAll = bColumn ? Mutable->Height : Mutable->Width;
		for (int32 B = 0; B < AcrossAll; ++B)
		{
			FSurfControlPoint& New = bColumn ? Mutable->At(NewLineAt, B) : Mutable->At(B, NewLineAt);
			const FVector& EdgePos = bColumn ? Mutable->At(EdgeAt, B).Pos : Mutable->At(B, EdgeAt).Pos;
			const FVector& InnerPos = bColumn ? Mutable->At(InnerAt, B).Pos : Mutable->At(B, InnerAt).Pos;
			New.Pos = EdgePos + (EdgePos - InnerPos).GetSafeNormal() * Distance;
		}
	}

	// Remember which way it grew, for the move that follows: the mean of the
	// edge-to-inner step across the line, in world space.
	{
		const int32 AcrossLine = bColumn ? Patch.Height : Patch.Width;
		const FTransform ToWorld = Group->GetActorTransform();
		FVector Sum = FVector::ZeroVector;

		for (int32 B = 0; B < AcrossLine; ++B)
		{
			const FVector& EdgePos  = bColumn
				? Patch.At(bFromEnd ? Patch.Width - 1 : 0, B).Pos
				: Patch.At(B, bFromEnd ? Patch.Height - 1 : 0).Pos;
			const FVector& InnerPos = bColumn
				? Patch.At(bFromEnd ? Patch.Width - 2 : 1, B).Pos
				: Patch.At(B, bFromEnd ? Patch.Height - 2 : 1).Pos;

			Sum += ToWorld.TransformVector(EdgePos - InnerPos);
		}

		LastExtrudeDirection = Sum.GetSafeNormal();
	}

	GrownGroup = Group;
	GrownPatch = PatchId;
	bGrownColumn = bColumn;
	bGrownFromEnd = bFromEnd;
	CarryGrownUVs();

	SurfPatchSurfacing::Reapply(*Mutable);
	Group->RebuildPreview();

	// Select the line that was just made, so extruding again continues from it
	// rather than repeating the same edge.
	ClearPointSelection();

	const int32 NewLine = bFromEnd ? (bColumn ? Mutable->Width - 1 : Mutable->Height - 1) : 0;
	const int32 Across = bColumn ? Mutable->Height : Mutable->Width;

	for (int32 B = 0; B < Across; ++B)
	{
		FSurfPointRef Ref;
		Ref.Group = Group;
		Ref.PatchId = PatchId;
		Ref.X = bColumn ? NewLine : B;
		Ref.Y = bColumn ? B : NewLine;
		AddPointToSelection(Ref);
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("extruded: now %d x %d"), Mutable->Width, Mutable->Height));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("extrude"), FString::Printf(TEXT("%s -> %dx%d"),
		bColumn ? TEXT("column") : TEXT("row"), Mutable->Width, Mutable->Height));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::WeldSelectedPoints(bool bAtCentre)
{
	// World-space, across every patch the selection touches: welding an edge
	// of one patch onto an edge of another is the case this exists for, and
	// it is the same operation as welding within one - the points go to one
	// place. Each patch is written in its own space.
	TArray<FVector> WorldPositions;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
		{
			WorldPositions.Add(Group->GetActorTransform().TransformPosition(Patch->At(Ref.X, Ref.Y).Pos));
		}
	}

	if (WorldPositions.Num() < 2)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("weld: pick at least two points (Shift-click adds, on any patch)"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	FVector Target = WorldPositions[0];
	if (bAtCentre)
	{
		Target = FVector::ZeroVector;
		for (const FVector& P : WorldPositions)
		{
			Target += P;
		}
		Target /= WorldPositions.Num();
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bKeepUVs = !Settings || Settings->bWeldKeepsUVs;

	FScopedTransaction Transaction(LOCTEXT("Weld", "Weld Points"));

	int32 Patches = 0;
	ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& Refs)
	{
		const FVector Local = Group.GetActorTransform().InverseTransformPosition(Target);

		for (const FSurfPointRef& Ref : Refs)
		{
			if (Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
			{
				Patch.At(Ref.X, Ref.Y).Pos = Local;
			}
		}

		// A weld moves geometry, and the UVs are asked to stay put: the
		// texture should pinch with the surface rather than be re-measured
		// across it. Natural mapping re-derives on every edit, so keeping
		// the UVs means freezing them - the patch becomes Manual, and stays
		// that way until a mapping verb is chosen again.
		if (bKeepUVs)
		{
			SurfPatchSurfacing::SetManual(Patch);
		}

		++Patches;
	});

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("welded %d points%s%s%s"),
			WorldPositions.Num(),
			bAtCentre ? TEXT(" at their centre") : TEXT(" onto the first"),
			Patches > 1 ? *FString::Printf(TEXT(" across %d patches"), Patches) : TEXT(""),
			bKeepUVs ? TEXT(" - UVs kept, mapping frozen") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("weld"), FString::Printf(TEXT("%d points patches=%d centre=%d"),
		WorldPositions.Num(), Patches, bAtCentre ? 1 : 0));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::WeldSelectionInClusters(double ReachCm)
{
	// World-space, across patches, like the weld at centre - but the
	// selection is sorted into clusters first, each point joined to any
	// other within reach, and each cluster goes to its own centre.
	struct FMember
	{
		FSurfPointRef Ref;
		FVector World = FVector::ZeroVector;
		int32 Cluster = INDEX_NONE;
	};
	TArray<FMember> Members;

	for (const FSurfPointRef& Ref : PointSelection)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
		{
			FMember& Member = Members.AddDefaulted_GetRef();
			Member.Ref = Ref;
			Member.World = Group->GetActorTransform().TransformPosition(Patch->At(Ref.X, Ref.Y).Pos);
		}
	}

	if (Members.Num() < 2)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("weld: pick at least two points (Shift-click adds, on any patch)"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	// Union by reach: a chain of points each within reach of the next is one
	// cluster, which is what "the same corner" means when three patches meet.
	const double Reach = FMath::Max(0.0, ReachCm);
	TArray<int32> Parent;
	Parent.SetNum(Members.Num());
	for (int32 I = 0; I < Parent.Num(); ++I)
	{
		Parent[I] = I;
	}
	auto Find = [&Parent](int32 I)
	{
		while (Parent[I] != I)
		{
			Parent[I] = Parent[Parent[I]];
			I = Parent[I];
		}
		return I;
	};
	for (int32 A = 0; A < Members.Num(); ++A)
	{
		for (int32 B = A + 1; B < Members.Num(); ++B)
		{
			if (FVector::Dist(Members[A].World, Members[B].World) <= Reach)
			{
				Parent[Find(A)] = Find(B);
			}
		}
	}

	TArray<FVector> Centres;
	TArray<int32> Counts;
	TMap<int32, int32> RootToCluster;
	for (int32 I = 0; I < Members.Num(); ++I)
	{
		const int32 Root = Find(I);
		int32* Cluster = RootToCluster.Find(Root);
		if (!Cluster)
		{
			Cluster = &RootToCluster.Add(Root, Centres.Num());
			Centres.Add(FVector::ZeroVector);
			Counts.Add(0);
		}
		Members[I].Cluster = *Cluster;
		Centres[*Cluster] += Members[I].World;
		Counts[*Cluster] += 1;
	}
	for (int32 C = 0; C < Centres.Num(); ++C)
	{
		Centres[C] /= FMath::Max(1, Counts[C]);
	}

	int32 Welded = 0;
	int32 Clusters = 0;
	for (int32 C = 0; C < Counts.Num(); ++C)
	{
		if (Counts[C] >= 2)
		{
			++Clusters;
			Welded += Counts[C];
		}
	}

	if (Clusters == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("weld: no two selected points are within %.1f cm of each other"), Reach));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("weld_clusters_none"));
		return false;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bKeepUVs = !Settings || Settings->bWeldKeepsUVs;

	FScopedTransaction Transaction(LOCTEXT("WeldClusters", "Weld Clusters"));

	int32 Patches = 0;
	ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& Refs)
	{
		const FTransform ToLocal = Group.GetActorTransform().Inverse();
		bool bTouched = false;

		for (const FSurfPointRef& Ref : Refs)
		{
			const FMember* Member = Members.FindByPredicate([&Ref](const FMember& M) { return M.Ref == Ref; });
			if (!Member || Member->Cluster == INDEX_NONE || Counts[Member->Cluster] < 2)
			{
				continue;
			}
			if (Ref.X >= 0 && Ref.X < Patch.Width && Ref.Y >= 0 && Ref.Y < Patch.Height)
			{
				Patch.At(Ref.X, Ref.Y).Pos = ToLocal.TransformPosition(Centres[Member->Cluster]);
				bTouched = true;
			}
		}

		if (bTouched)
		{
			if (bKeepUVs)
			{
				SurfPatchSurfacing::SetManual(Patch);
			}
			++Patches;
		}
	});

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("welded %d points in %d clusters within %.1f cm%s%s"),
			Welded, Clusters, Reach,
			Patches > 1 ? *FString::Printf(TEXT(" across %d patches"), Patches) : TEXT(""),
			bKeepUVs ? TEXT(" - UVs kept, mapping frozen") : TEXT("")));
	}

	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::WeldClusters;
		Op.Distance = Reach;
		NoteOperation(Op);
	}
	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("weld_clusters"), FString::Printf(TEXT("%d points clusters=%d reach=%.1f patches=%d"),
		Welded, Clusters, Reach, Patches));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::WeldSelectionByDistance(double ToleranceCm)
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	const FGuid PatchId = Found->Id;
	const int32 Width = Found->Width;

	// Nothing picked means the whole patch, which is the usual way this one
	// is wanted: tidy up everywhere rather than somewhere.
	TArray<int32> Indices;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Ref.X >= 0 && Ref.X < Width && Ref.Y >= 0 && Ref.Y < Found->Height)
		{
			Indices.AddUnique(Ref.Y * Width + Ref.X);
		}
	}

	FScopedTransaction Transaction(LOCTEXT("WeldDistance", "Weld By Distance"));

	FSurfPatch* Mutable = Group->ModifyPatch(PatchId);
	if (!Mutable)
	{
		Transaction.Cancel();
		return false;
	}

	const int32 Moved = SurfPatchOps::WeldByDistance(*Mutable, Indices, ToleranceCm);

	if (Moved == 0)
	{
		Transaction.Cancel();

		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(
				TEXT("weld by distance: nothing was within %.1f cm"), ToleranceCm));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || Settings->bWeldKeepsUVs)
	{
		SurfPatchSurfacing::SetManual(*Mutable);
	}
	else
	{
		SurfPatchSurfacing::Reapply(*Mutable);
	}

	Group->RebuildPreview();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("welded %d points within %.1f cm"), Moved, ToleranceCm));
	}

	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::WeldByDistance;
		Op.Distance = ToleranceCm;
		NoteOperation(Op);
	}
	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("weld_distance"), FString::Printf(TEXT("moved=%d tol=%.1f"), Moved, ToleranceCm));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::JoinPickedPatches(const TArray<FSurfPatchRef>& Picked, double ToleranceCm)
{
	FScopedTransaction Transaction(LOCTEXT("JoinPickedPatches", "Join Patches"));

	// Pairs are tried until a pass joins nothing. A join keeps the id of the
	// first of the pair - its identity, material and mapping - so the
	// survivor stays in the list and can take the next one. The active patch
	// (the last picked) goes first, so it is the one everything folds into.
	TArray<FSurfPatchRef> Remaining = Picked;
	const int32 ActiveAt = Remaining.IndexOfByKey(SelectedPatch);
	if (ActiveAt > 0)
	{
		Remaining.Swap(0, ActiveAt);
	}
	TArray<ASurfGroup*> Touched;
	int32 Joins = 0;

	for (bool bJoinedOne = true; bJoinedOne;)
	{
		bJoinedOne = false;

		for (int32 A = 0; A < Remaining.Num() && !bJoinedOne; ++A)
		{
			for (int32 B = A + 1; B < Remaining.Num() && !bJoinedOne; ++B)
			{
				ASurfGroup* GroupA = Remaining[A].Group.Get();
				if (!GroupA || GroupA != Remaining[B].Group.Get())
				{
					continue;
				}

				const FSurfPatch* PatchA = GroupA->FindPatch(Remaining[A].PatchId);
				const FSurfPatch* PatchB = GroupA->FindPatch(Remaining[B].PatchId);
				if (!PatchA || !PatchB)
				{
					continue;
				}

				FSurfPatch Joined;
				if (!SurfPatchPrimitives::Join(Joined, *PatchA, *PatchB, ToleranceCm))
				{
					continue;
				}

				if (!Touched.Contains(GroupA))
				{
					GroupA->Modify();
					Touched.Add(GroupA);
				}

				const FGuid KeepId = PatchA->Id;
				const FGuid DropId = PatchB->Id;
				Joined.Id = KeepId;

				if (FSurfPatch* Mutable = GroupA->ModifyPatch(KeepId))
				{
					*Mutable = MoveTemp(Joined);
					SurfPatchSurfacing::Reapply(*Mutable);
				}
				GroupA->RemovePatch(DropId);
				Remaining.RemoveAt(B);
				++Joins;
				bJoinedOne = true;
			}
		}
	}

	if (Joins == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(
				TEXT("join: none of the %d patches share an edge within %.2f cm with matching point counts"), Picked.Num(), ToleranceCm));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("join_no_shared_edge"));
		return false;
	}

	ClearPointSelection();
	SelectPatch(Remaining.Num() > 0 ? Remaining[0] : FSurfPatchRef());
	for (int32 I = 1; I < Remaining.Num(); ++I)
	{
		AddPatchToSelection(Remaining[I], /*bToggle*/ false);
	}

	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Remaining.Num() == 1
			? FString::Printf(TEXT("joined %d patches into one"), Picked.Num())
			: FString::Printf(TEXT("joined %d times - %d patches left, the rest share no edge"), Joins, Remaining.Num()));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("join_patches"), FString::Printf(TEXT("picked=%d joins=%d"), Picked.Num(), Joins));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::JoinSelectedPatches(double ToleranceCm)
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	// Several patches picked: fold them into each other wherever two share
	// an edge, as many times as it takes, and what cannot be joined is said.
	// The patches the point selection reaches into count as picked too, so
	// Shift-clicking a point on each of two patches and pressing Join works.
	TArray<FSurfPatchRef> Picked = GetSelectedPatches();
	for (const FSurfPointRef& Ref : PointSelection)
	{
		FSurfPatchRef Key;
		Key.Group = Ref.Group;
		Key.PatchId = Ref.PatchId;
		if (Key.IsValid() && !Picked.Contains(Key))
		{
			Picked.Add(Key);
		}
	}

	if (Picked.Num() > 1)
	{
		return JoinPickedPatches(Picked, ToleranceCm);
	}

	const FSurfPatch Source = *Found;
	const FGuid SourceId = Source.Id;

	// The other patch is whichever one shares an edge with this one. Asking
	// the user to select two patches when only one selection exists would be
	// a worse question than working it out.
	FGuid OtherId;
	FSurfPatch Joined;
	bool bFoundOne = false;

	Group->ForEachPatch([&](const FSurfPatch& Candidate)
	{
		if (bFoundOne || Candidate.Id == SourceId)
		{
			return;
		}

		FSurfPatch Attempt;
		if (SurfPatchPrimitives::Join(Attempt, Source, Candidate, ToleranceCm))
		{
			Joined = MoveTemp(Attempt);
			OtherId = Candidate.Id;
			bFoundOne = true;
		}
	});

	if (!bFoundOne)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(
				TEXT("join: no patch shares an edge within %.2f cm, with matching point counts"), ToleranceCm));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("join_no_shared_edge"));
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("JoinPatches", "Join Patches"));

	Group->Modify();

	// The joined patch keeps the id of the one that was selected, so anything
	// pointing at it still points at it.
	Joined.Id = SourceId;

	FSurfPatch* Mutable = Group->ModifyPatch(SourceId);
	if (!Mutable)
	{
		Transaction.Cancel();
		return false;
	}

	*Mutable = MoveTemp(Joined);
	SurfPatchSurfacing::Reapply(*Mutable);

	Group->RemovePatch(OtherId);
	ClearPointSelection();
	Group->RebuildPreview();

	if (Inspector)
	{
		const FSurfPatch* Now = Group->FindPatch(SourceId);
		Inspector->SetResult(Now
			? FString::Printf(TEXT("joined: now %d x %d"), Now->Width, Now->Height)
			: TEXT("joined"));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("join_patches"));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::CyclePatch(int32 Direction)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || Direction == 0)
	{
		return false;
	}

	// Every patch in the level, in a stable order: group by group, and within
	// a group in the order the group keeps them. Stable matters more than
	// clever here - the order has to be the same every time or cycling is
	// not cycling, it is shuffling.
	TArray<FSurfPatchRef> All;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		Group->ForEachPatch([&All, Group](const FSurfPatch& Patch)
		{
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch.Id;
			All.Add(Ref);
		});
	}

	if (All.Num() == 0)
	{
		return false;
	}

	const int32 Current = All.IndexOfByKey(SelectedPatch);
	const int32 Next = Current == INDEX_NONE
		? (Direction > 0 ? 0 : All.Num() - 1)
		: ((Current + Direction) % All.Num() + All.Num()) % All.Num();

	SelectPatch(All[Next]);
	ClearPointSelection();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("patch %d of %d"), Next + 1, All.Num()));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("cycled to patch %d of %d"), Next + 1, All.Num()));

	// Framing it as well would take the camera somewhere the user did not ask
	// to go. Saying which one it is, in the panel, is enough.
	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

FString USurfEdMode::DescribePatches() const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TEXT("no world");
	}

	FString Text;
	int32 Index = 0;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			++Index;

			const bool bSelected = Group == SelectedPatch.Group.Get() && Patch.Id == SelectedPatch.PatchId;

			Text += FString::Printf(TEXT("%s %d. %s %dx%d\n"),
				bSelected ? TEXT(">") : TEXT(" "),
				Index,
				Patch.Type == ESurfPatchType::Curve ? TEXT("Curve") : TEXT("Sheet"),
				Patch.Width, Patch.Height);
		});
	}

	return Index == 0 ? TEXT("no patches yet") : Text;
}

bool USurfEdMode::SeparateSelectedFaces()
{
	return CutSelectedFaces(ESurfCutPiece::Keep, 0.0);
}

bool USurfEdMode::DeleteSelectedFaces()
{
	return CutSelectedFaces(ESurfCutPiece::Drop, 0.0);
}

bool USurfEdMode::SolidifySelectedFaces(double ThicknessCm, int32 Side, bool bRim, bool bEven)
{
	return CutSelectedFaces(ESurfCutPiece::Solidify, ThicknessCm, Side, bRim, bEven);
}

bool USurfEdMode::CutSelectedFaces(ESurfCutPiece What, double ThicknessCm, int32 Side, bool bRim, bool bEven,
	TArray<FSurfPatchRef>* OutPieces, int32* OutRest)
{
	// The verb this is for, so a refusal names the key that was pressed.
	const TCHAR* Verb = What == ESurfCutPiece::Drop ? TEXT("delete") : (What == ESurfCutPiece::Solidify ? TEXT("solidify") : TEXT("separate"));

	// The picked faces, per patch they sit on: each patch is cut on its own,
	// all in one step. A rectangle of cells is a sub-grid, and a sub-grid is
	// a patch; a ragged set of them is several - a piece per rectangle - and
	// so is what is left, since a grid cannot have a hole.
	struct FCut
	{
		ASurfGroup* Group = nullptr;
		FGuid SourceId;
		bool bActive = false;
		FSurfCellSet Picked;
	};
	TArray<FCut> Cuts;
	for (const FSurfQuadRef& Quad : QuadSelection)
	{
		ASurfGroup* EachGroup = Quad.Group.Get();
		const FSurfPatch* EachPatch = EachGroup && Quad.PatchId.IsValid() ? EachGroup->FindPatch(Quad.PatchId) : nullptr;
		if (!EachPatch)
		{
			continue;
		}
		FCut* Cut = Cuts.FindByPredicate([&](const FCut& Each) { return Each.Group == EachGroup && Each.SourceId == Quad.PatchId; });
		if (!Cut)
		{
			Cut = &Cuts.AddDefaulted_GetRef();
			Cut->Group = EachGroup;
			Cut->SourceId = Quad.PatchId;
			Cut->bActive = SelectedPatch.Group.Get() == EachGroup && SelectedPatch.PatchId == Quad.PatchId;
			Cut->Picked = FSurfCellSet(EachPatch->Width - 1, EachPatch->Height - 1);
		}
		Cut->Picked.Set(Quad.X, Quad.Y);
	}

	if (Cuts.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s: pick some faces first (face sub-mode, key 3)"), Verb));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	for (const FCut& Cut : Cuts)
	{
		const FSurfPatch& Source = *Cut.Group->FindPatch(Cut.SourceId);

		// A curve's control points are not its surface, so cutting its net
		// into sub-grids would change the shape rather than divide it. Say
		// so instead of quietly producing something different.
		if (Source.Type == ESurfPatchType::Curve)
		{
			if (Inspector)
			{
				Inspector->SetResult(FString::Printf(TEXT("%s: convert to a sheet first - cutting a curve's net would change its shape"), Verb));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Friction(TEXT("separate_curve"));
			return false;
		}

		if (Cut.Picked.Num() == 0)
		{
			return false;
		}
	}

	FScopedTransaction Transaction(LOCTEXT("Separate", "Separate Faces"));

	TArray<ASurfGroup*> Groups;
	TArray<FSurfPatchRef> Pieces;
	FSurfPatchRef ActivePiece;
	int32 Rest = 0;
	for (const FCut& Cut : Cuts)
	{
		if (!Groups.Contains(Cut.Group))
		{
			Cut.Group->Modify();
			Groups.Add(Cut.Group);
		}

		const FSurfPatch Source = *Cut.Group->FindPatch(Cut.SourceId);

		// The pieces first, in the order the decomposition gives - the
		// extrude pairs them with the lids it made the same way - then what
		// is left round them.
		TArray<FSurfCellRect> Blocks = SurfCells::Decompose(Cut.Picked);
		const int32 PieceCount = Blocks.Num();
		Blocks.Append(SurfCells::Decompose(Cut.Picked.Inverse()));
		Rest += Blocks.Num() - PieceCount;

		for (int32 B = 0; B < Blocks.Num(); ++B)
		{
			const FSurfCellRect& Block = Blocks[B];
			const bool bIsTheCutPiece = B < PieceCount;

			FSurfPatch Made;
			Made.Type = ESurfPatchType::Sheet;
			Made.EnsureId();
			Made.SubdivU = Source.SubdivU;
			Made.SubdivV = Source.SubdivV;
			Made.Material = Source.Material;
			Made.Scale = Source.Scale;
			Made.Mapping = Source.Mapping;
			Made.Projection = Source.Projection;
			Made.UVShift = Source.UVShift;
			Made.UVRotation = Source.UVRotation;
			Made.bUVSwap = Source.bUVSwap;
			Made.bUVFlipU = Source.bUVFlipU;
			Made.bUVFlipV = Source.bUVFlipV;
			Made.StackLevel = Source.StackLevel;
			Made.Shading = Source.Shading;
			Made.SmoothingAngleDegrees = Source.SmoothingAngleDegrees;
			Made.bInvert = Source.bInvert;
			Made.SmoothingGroup = Source.SmoothingGroup;

			Made.Width = Block.Columns() + 1;
			Made.Height = Block.Rows() + 1;
			Made.Points.SetNum(Made.Width * Made.Height);

			for (int32 Y = 0; Y < Made.Height; ++Y)
			{
				for (int32 X = 0; X < Made.Width; ++X)
				{
					Made.At(X, Y) = Source.At(Block.X0 + X, Block.Y0 + Y);
				}
			}

			// Natural mapping measures from a patch's own first line, so
			// every piece would restart its UVs at zero and every new seam
			// would show. Each one continues from the corner it was cut at.
			if (SurfPatchSurfacing::FollowsTheNet(Made.Mapping))
			{
				Made.UVShift = Source.At(Block.X0, Block.Y0).UV;
			}

			// A piece: kept as a patch, dropped (that is deleting faces on a
			// grid - the rest comes apart around the hole), or thickened
			// into a solid of its own.
			FGuid PieceId;
			if (bIsTheCutPiece)
			{
				if (What == ESurfCutPiece::Drop)
				{
					continue;
				}

				if (What == ESurfCutPiece::Solidify)
				{
					SurfPatchSurfacing::Reapply(Made);

					SurfPatchPrimitives::FSurfSolidifyOptions Options;
					Options.Side = FMath::Clamp(Side, -1, 1);
					Options.bRim = bRim;
					Options.bEven = bEven;

					TArray<FSurfPatch> Solid;
					if (SurfPatchPrimitives::Solidify(Made, ThicknessCm, Solid, Options) && Solid.Num() > 0)
					{
						PieceId = Solid[0].Id;
						for (FSurfPatch& One : Solid)
						{
							SurfPatchSurfacing::Reapply(One);
							Cut.Group->AddPatch(MoveTemp(One));
						}
					}
				}
			}

			if (!PieceId.IsValid())
			{
				const FGuid MadeId = Made.Id;
				FSurfPatch& Added = Cut.Group->AddPatch(MoveTemp(Made));
				SurfPatchSurfacing::Reapply(Added);
				if (bIsTheCutPiece)
				{
					PieceId = MadeId;
				}
			}

			if (PieceId.IsValid())
			{
				FSurfPatchRef Piece;
				Piece.Group = Cut.Group;
				Piece.PatchId = PieceId;
				Pieces.Add(Piece);
				if (Cut.bActive || !ActivePiece.IsValid())
				{
					ActivePiece = Piece;
				}
			}
		}

		// The original goes: it has been replaced by its pieces. Leaving it
		// would put two surfaces in the same place, which is what made the
		// surface flicker the first time this verb was tried.
		Cut.Group->RemovePatch(Cut.SourceId);
	}

	ClearPointSelection();
	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// What is selected afterwards says what happened: the pieces when they
	// were kept, nothing when they were dropped.
	SelectPatch(ActivePiece);
	if (Pieces.Num() > 0)
	{
		PatchSelection = Pieces;
		SelectedPatch = ActivePiece;
	}

	if (OutPieces)
	{
		*OutPieces = Pieces;
	}
	if (OutRest)
	{
		*OutRest = Rest;
	}

	if (Inspector)
	{
		const FString On = Cuts.Num() > 1 ? FString::Printf(TEXT(" on %d patches"), Cuts.Num()) : FString();
		Inspector->SetResult(
			What == ESurfCutPiece::Drop
				? FString::Printf(TEXT("deleted the faces%s - the rest is now %d patch(es)"), *On, Rest)
			: What == ESurfCutPiece::Solidify
				? FString::Printf(TEXT("solidified the picked faces%s - the rest is %d patch(es)"), *On, Rest)
			: Cuts.Num() > 1
				? FString::Printf(TEXT("separated %d patches into %d - the pieces you picked are selected"), Cuts.Num(), Rest + Pieces.Num())
			: Pieces.Num() > 1
				? FString::Printf(TEXT("separated into %d patches - the %d pieces you picked are selected"), Rest + Pieces.Num(), Pieces.Num())
				: FString::Printf(TEXT("separated into %d patches - the piece you picked is selected"), Rest + Pieces.Num()));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("cut_faces"), FString::Printf(TEXT("%s into %d patches=%d pieces=%d"),
		What == ESurfCutPiece::Drop ? TEXT("drop") : (What == ESurfCutPiece::Solidify ? TEXT("solidify") : TEXT("keep")),
		Rest + Pieces.Num(), Cuts.Num(), Pieces.Num()));

	// A thickness, a side, a rim can be adjusted afterwards, as the
	// whole-patch solidify's can.
	if (What == ESurfCutPiece::Solidify)
	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::Solidify;
		Op.Distance = ThicknessCm;
		Op.Side = FMath::Clamp(Side, -1, 1);
		Op.bRim = bRim;
		Op.bEven = bEven;
		NoteOperation(Op);
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

// BridgeSelectedLines lives in SurfEdModeExtrude.cpp, beside the chain planner it shares.

void USurfEdMode::BrushCornersOfSelection(TArray<FSurfBrushCornerRef>& Out) const
{
	Out.Reset();
	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
		{
			FSurfBrushCornerRef Corner;
			Corner.Group = Group;
			Corner.BrushId = Ref.BrushId;
			Corner.Corner = I;
			Out.AddUnique(Corner);
		}
	}
}

bool USurfEdMode::PickBrushCornerWith(const FSurfPickContext& Ctx, FSurfBrushCornerRef& Out) const
{
	/**
	 * The corners of the brushes being worked on (S113a).
	 *
	 * Only those: the same rule the wires already follow, so what can be
	 * clicked is exactly what is drawn. Every brush in the level offering its
	 * corners would put eight targets on top of every box in the scene, and a
	 * click meant for a patch would land on one of them.
	 */
	if (!Ctx.IsValid())
	{
		return false;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Reach = Settings ? FMath::Max(4.0, Settings->PointPickRadiusPx) : 10.0;
	double BestSquared = Reach * Reach;
	bool bFound = false;

	/**
	 * And the one under the cursor, when nothing is picked yet (S139a):
	 * "why can i not select verts from a box ?"
	 *
	 * Corners were offered only on a brush already being worked on, which
	 * meant the first click never found one - and the first click is the
	 * one that was going to select the corner. A brush the ray actually
	 * hits is not "every primitive in the level": there is one of it, under the
	 * cursor, which is where the hand already is.
	 */
	TArray<FSurfFaceRef> Offering = BrushesHeldBySelection();
	if (Offering.Num() == 0)
	{
		FSurfFaceRef Under;
		if (PickFaceWith(Ctx, Under, nullptr))
		{
			Offering.Add(Under);
		}
	}

	for (const FSurfFaceRef& Ref : Offering)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
		{
			FVector2D Screen;
			if (!Ctx.WorldToPixel(ToWorld.TransformPosition(Hull.Vertices[I]), Screen))
			{
				continue;
			}
			const double D = FVector2D::DistSquared(Screen, Ctx.Pixel);
			if (D <= BestSquared)
			{
				BestSquared = D;
				Out.Group = Group;
				Out.BrushId = Ref.BrushId;
				Out.Corner = I;
				bFound = true;
			}
		}
	}
	return bFound;
}

void USurfEdMode::SelectBrushEdgeLoop(const FSurfBrushCornerRef& A, const FSurfBrushCornerRef& B, bool bAdd)
{
	ASurfGroup* Group = A.Group.Get();
	const FSurfBrush* Brush = Group ? Group->FindBrush(A.BrushId) : nullptr;
	FSurfHull Hull;
	if (!Brush || !SurfBrushMath::DeriveHull(*Brush, Hull) || !Hull.Vertices.IsValidIndex(A.Corner) || !Hull.Vertices.IsValidIndex(B.Corner))
	{
		return;
	}
	// The faces either side of the edge; a many-sided one gives its rim.
	int32 Rim = INDEX_NONE;
	for (int32 F = 0; F < Hull.Faces.Num(); ++F)
	{
		const TArray<int32>& V = Hull.Faces[F].VertexIndices;
		for (int32 I = 0; I < V.Num(); ++I)
		{
			const int32 P = V[I];
			const int32 Q = V[(I + 1) % V.Num()];
			if ((P == A.Corner && Q == B.Corner) || (P == B.Corner && Q == A.Corner))
			{
				if (V.Num() > 4 && (Rim == INDEX_NONE || V.Num() > Hull.Faces[Rim].VertexIndices.Num()))
				{
					Rim = F;
				}
				break;
			}
		}
	}
	TArray<int32> Corners;
	if (Rim != INDEX_NONE)
	{
		Corners = Hull.Faces[Rim].VertexIndices;
	}
	else
	{
		// Every edge running the same way: the ring round the sides.
		const FVector Dir = (Hull.Vertices[B.Corner] - Hull.Vertices[A.Corner]).GetSafeNormal();
		for (const FSurfHullFace& Face : Hull.Faces)
		{
			const TArray<int32>& V = Face.VertexIndices;
			for (int32 I = 0; I < V.Num(); ++I)
			{
				const int32 P = V[I];
				const int32 Q = V[(I + 1) % V.Num()];
				const FVector Along = (Hull.Vertices[Q] - Hull.Vertices[P]).GetSafeNormal();
				if (FMath::Abs(FVector::DotProduct(Along, Dir)) > 0.999)
				{
					Corners.AddUnique(P);
					Corners.AddUnique(Q);
				}
			}
		}
	}
	if (!bAdd)
	{
		BrushCornerSelection.Reset();
	}
	for (const int32 Corner : Corners)
	{
		FSurfBrushCornerRef Ref;
		Ref.Group = Group;
		Ref.BrushId = A.BrushId;
		Ref.Corner = Corner;
		BrushCornerSelection.AddUnique(Ref);
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("primitive loop: %d corners - G moves them"), BrushCornerSelection.Num()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Gesture(TEXT("brush_edge_loop"), FString::Printf(TEXT("%d %s"), Corners.Num(), Rim != INDEX_NONE ? TEXT("rim") : TEXT("ring")));
	RedrawViewports();
}

void USurfEdMode::SelectBrushFaceLoop(const FSurfFaceRef& Picked, const FVector& WorldHit, bool bAdd)
{
	ASurfGroup* Group = Picked.Group.Get();
	const FSurfBrush* Brush = Group ? Group->FindBrush(Picked.BrushId) : nullptr;
	FSurfHull Hull;
	if (!Brush || !SurfBrushMath::DeriveHull(*Brush, Hull))
	{
		SelectFace(Picked, bAdd);
		return;
	}
	const FSurfHullFace* Clicked = Hull.Faces.FindByPredicate([&Picked](const FSurfHullFace& F) { return F.SourceFaceIndex == Picked.FaceIndex; });
	if (!Clicked || Clicked->VertexIndices.Num() < 3)
	{
		SelectFace(Picked, bAdd);
		return;
	}
	// The clicked face's edge nearest the click names the loop's direction.
	const FVector Local = Group->GetActorTransform().InverseTransformPosition(WorldHit);
	FVector Dir = FVector::ZeroVector;
	double Best = TNumericLimits<double>::Max();
	const TArray<int32>& V = Clicked->VertexIndices;
	for (int32 I = 0; I < V.Num(); ++I)
	{
		const FVector P = Hull.Vertices[V[I]];
		const FVector Q = Hull.Vertices[V[(I + 1) % V.Num()]];
		const double Distance = FMath::PointDistToSegment(Local, P, Q);
		if (Distance < Best)
		{
			Best = Distance;
			Dir = (Q - P).GetSafeNormal();
		}
	}
	if (!bAdd)
	{
		ClearSelection();
	}
	int32 Taken = 0;
	for (const FSurfHullFace& Face : Hull.Faces)
	{
		if (!Brush->Faces.IsValidIndex(Face.SourceFaceIndex))
		{
			continue;
		}
		const FPlane& Plane = Brush->Faces[Face.SourceFaceIndex].Plane;
		const FVector Normal(Plane.X, Plane.Y, Plane.Z);
		if (FMath::Abs(FVector::DotProduct(Normal.GetSafeNormal(), Dir)) < 0.02)
		{
			FSurfFaceRef Ref;
			Ref.Group = Group;
			Ref.BrushId = Picked.BrushId;
			Ref.FaceIndex = Face.SourceFaceIndex;
			SelectFace(Ref, /*bAddToSelection*/ true);
			++Taken;
		}
	}
	if (Taken == 0)
	{
		SelectFace(Picked, bAdd);
	}
	SurfSession::Gesture(TEXT("brush_face_loop"), FString::Printf(TEXT("%d"), Taken));
}

bool USurfEdMode::PickBrushEdgeWith(const FSurfPickContext& Ctx, FSurfBrushCornerRef& OutA, FSurfBrushCornerRef& OutB) const
{
	/**
	 * An edge of a primitive in 2 (S166a): "not able to select only a
	 * primitive edge in 2 mode - it selects all 4 edges of the face." The
	 * hull's edges are the sides of its faces; the one whose segment on
	 * screen passes nearest the cursor is taken as its two corners, and G
	 * moves them with the sides following, as corners do.
	 */
	if (!Ctx.IsValid())
	{
		return false;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Reach = Settings ? FMath::Max(6.0, Settings->EdgePickRadiusPx) : 22.0;
	double BestSquared = Reach * Reach;
	bool bFound = false;

	TArray<FSurfFaceRef> Offering = BrushesHeldBySelection();
	{
		FSurfFaceRef Under;
		if (PickFaceWith(Ctx, Under, nullptr))
		{
			Offering.AddUnique(Under);
		}
	}
	for (const FSurfFaceRef& Ref : Offering)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		FSurfHull Hull;
		if (!Brush || !SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		TArray<FVector2D> Screen;
		TArray<bool> OnScreen;
		Screen.SetNum(Hull.Vertices.Num());
		OnScreen.SetNum(Hull.Vertices.Num());
		for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
		{
			OnScreen[I] = Ctx.WorldToPixel(ToWorld.TransformPosition(Hull.Vertices[I]), Screen[I]);
		}
		for (const FSurfHullFace& Face : Hull.Faces)
		{
			const int32 N = Face.VertexIndices.Num();
			for (int32 K = 0; K < N; ++K)
			{
				const int32 A = Face.VertexIndices[K];
				const int32 B = Face.VertexIndices[(K + 1) % N];
				if (A >= B || !OnScreen[A] || !OnScreen[B])
				{
					continue; // each edge once, from the face that lists it low to high
				}
				const FVector2D P = Ctx.Pixel;
				const FVector2D AB = Screen[B] - Screen[A];
				const double Len2 = AB.SizeSquared();
				const double T = Len2 > 1.e-6 ? FMath::Clamp(FVector2D::DotProduct(P - Screen[A], AB) / Len2, 0.0, 1.0) : 0.0;
				const double D = FVector2D::DistSquared(P, Screen[A] + AB * T);
				if (D <= BestSquared)
				{
					BestSquared = D;
					OutA.Group = Group; OutA.BrushId = Ref.BrushId; OutA.Corner = A;
					OutB.Group = Group; OutB.BrushId = Ref.BrushId; OutB.Corner = B;
					bFound = true;
				}
			}
		}
	}
	return bFound;
}

void USurfEdMode::SelectBrushCorner(const FSurfBrushCornerRef& Corner, bool bAdd)
{
	if (!bAdd)
	{
		BrushCornerSelection.Reset();
		BrushCornerSelection.Add(Corner);
	}
	else if (int32 At = BrushCornerSelection.IndexOfByKey(Corner); At != INDEX_NONE)
	{
		BrushCornerSelection.RemoveAt(At);
	}
	else
	{
		BrushCornerSelection.Add(Corner);
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("primitive corner%s: %d picked - G moves them, and the sides follow"),
			BrushCornerSelection.Num() == 1 ? TEXT("") : TEXT("s"), BrushCornerSelection.Num()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Gesture(TEXT("brush_corner_pick"), FString::Printf(TEXT("%d"), BrushCornerSelection.Num()));
	RedrawViewports();
}

void USurfEdMode::GatherBrushCorners(TArray<FVector>& Out) const
{
	Out.Reset();
	for (const FSurfBrushCornerRef& Ref : BrushCornerSelection)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull) || !Hull.Vertices.IsValidIndex(Ref.Corner))
		{
			continue;
		}
		Out.Add(Group->GetActorTransform().TransformPosition(Hull.Vertices[Ref.Corner]));
	}
}

int32 USurfEdMode::MoveBrushCorners(const FVector& WorldDelta)
{
	/**
	 * Corners moved, planes derived (S112a).
	 *
	 * "we need be able to select brushes points (vertices) and edit them
	 * freely ... you can make them any shape, as long it remain enclosed box."
	 *
	 * A brush is a set of planes and a mesh is a set of points, and this is
	 * the one place they have to meet. The corners move, the convex hull of
	 * the moved corners says what planes they imply, and those planes become
	 * the brush. **Nothing here interpolates**: the hull is exact, so a corner
	 * dragged onto the grid lands on the grid and the sides that meet there
	 * are the sides that meet there.
	 *
	 * The whole move goes in or none of it does. A brush that cannot take the
	 * move is put back and said, rather than left as whatever the arithmetic
	 * produced.
	 */
	if (BrushCornerSelection.Num() == 0 || WorldDelta.IsNearlyZero())
	{
		return 0;
	}

	// Per brush: which of its corners are moving.
	struct FPlan
	{
		ASurfGroup* Group = nullptr;
		FGuid BrushId;
		TArray<int32> Corners;
	};
	TArray<FPlan> Plans;
	for (const FSurfBrushCornerRef& Ref : BrushCornerSelection)
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (!Group)
		{
			continue;
		}
		FPlan* Found = Plans.FindByPredicate([&](const FPlan& P)
		{
			return P.Group == Group && P.BrushId == Ref.BrushId;
		});
		if (!Found)
		{
			Found = &Plans.AddDefaulted_GetRef();
			Found->Group = Group;
			Found->BrushId = Ref.BrushId;
		}
		Found->Corners.AddUnique(Ref.Corner);
	}

	int32 Changed = 0;
	FString Refused;
	TArray<ASurfGroup*> Touched;

	for (const FPlan& Plan : Plans)
	{
		const FSurfBrush* Read = Plan.Group->FindBrush(Plan.BrushId);
		if (!Read)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Read, Hull))
		{
			continue;
		}

		const FTransform ToWorld = Plan.Group->GetActorTransform();
		const FVector LocalDelta = ToWorld.InverseTransformVector(WorldDelta);

		TArray<FVector> Corners = Hull.Vertices;
		for (const int32 I : Plan.Corners)
		{
			if (Corners.IsValidIndex(I))
			{
				Corners[I] += LocalDelta;
			}
		}

		FSurfBrush Rebuilt;
		FString Why;
		if (!SurfBrushMath::RebuildFromPoints(*Read, Corners, Rebuilt, &Why))
		{
			if (Refused.IsEmpty())
			{
				Refused = Why;
			}
			continue;
		}

		FSurfBrush* Write = Plan.Group->ModifyBrush(Plan.BrushId);
		if (!Write)
		{
			continue;
		}
		const FGuid Keep = Write->Id;
		*Write = MoveTemp(Rebuilt);
		Write->Id = Keep;
		++Changed;
		Touched.AddUnique(Plan.Group);
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	if (Inspector)
	{
		if (Changed > 0)
		{
			Inspector->SetResult(FString::Printf(TEXT("moved %d corner%s on %d primitive%s"),
				BrushCornerSelection.Num(), BrushCornerSelection.Num() == 1 ? TEXT("") : TEXT("s"),
				Changed, Changed == 1 ? TEXT("") : TEXT("s")));
		}
		else if (!Refused.IsEmpty())
		{
			Inspector->SetResult(FString::Printf(TEXT("primitive corners: %s - a primitive has to stay a closed solid"), *Refused));
			SurfSession::Friction(TEXT("brush_corner_refused"), Refused);
		}
	}

	return Changed;
}

int32 USurfEdMode::FaceSelectionOutward()
{
	/**
	 * "in blender it's face outside recalculate face outside" (S121a).
	 *
	 * Blender's Shift+N, and it means one thing: make every selected face
	 * point away from the inside of what is selected rather than into it.
	 * Outward of *what* is the only real question, and the honest answer is
	 * the middle of the selection - a shell built from a few patches has a
	 * centre, and each patch's own average normal either points away from it
	 * or at it.
	 *
	 * A patch that already faces out is left alone, so pressing it twice does
	 * nothing the second time. That is what "recalculate" means and it is why
	 * it is not called Flip: Flip turns whatever it is given, this one
	 * decides.
	 *
	 * Brushes are not in it. A brush's faces are its planes and they face out
	 * by construction (S108a); there is nothing to recalculate, and the line
	 * says so rather than leaving him wondering.
	 */
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	if (Patches.Num() == 0 && SelectedPatch.IsValid())
	{
		Patches.Add(SelectedPatch);
	}
	if (Patches.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("face outward: select the patches to turn - a primitive already faces out"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("face_outward_nothing"));
		return 0;
	}

	// The middle of everything selected, in the world.
	FVector Sum = FVector::ZeroVector;
	int32 Points = 0;
	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FSurfControlPoint& Point : Patch->Points)
		{
			Sum += ToWorld.TransformPosition(Point.Pos);
			++Points;
		}
	}
	if (Points == 0)
	{
		return 0;
	}
	const FVector Centre = Sum / static_cast<double>(Points);

	FScopedTransaction Transaction(LOCTEXT("FaceOutward", "Face Outward"));

	int32 Turned = 0;
	int32 Already = 0;
	TArray<ASurfGroup*> Touched;
	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Read = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Read)
		{
			continue;
		}

		if (PatchFacesAwayFrom(*Read, Group->GetActorTransform().InverseTransformPosition(Centre)))
		{
			++Already;
			continue;
		}

		if (FSurfPatch* Write = Group->ModifyPatch(Ref.PatchId))
		{
			Write->bInvert = !Write->bInvert;
			++Write->EditSerial;
			++Turned;
			Touched.AddUnique(Group);
		}
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Turned > 0
			? FString::Printf(TEXT("face outward: turned %d patch%s; %d already faced out"),
				Turned, Turned == 1 ? TEXT("") : TEXT("es"), Already)
			: FString::Printf(TEXT("face outward: all %d already face out"), Already));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("face_outward"), FString::Printf(TEXT("turned=%d already=%d"), Turned, Already));

	if (GEditor)
	{
		RedrawViewports();
	}
	return Turned;
}

bool USurfEdMode::DeleteSelectedBrushes()
{
	TArray<FSurfFaceRef> Picked = Selection;
	if (ActiveFace.IsValid())
	{
		Picked.AddUnique(ActiveFace);
	}
	if (Picked.Num() == 0)
	{
		return false;
	}

	// In 3, a side or two of a primitive picked is not the primitive
	// (S170a): "looks i can even delete faces off a primitive cylinder? i
	// thought it were not possible" - it cannot lose a side, and Delete
	// on some of them took the whole thing. Whole in 3 (every side, as A
	// picks them) or picked in 4, it goes; part of it, the line says how.
	if (SubMode == ESurfSubMode::Face)
	{
		for (const FSurfFaceRef& Ref : Picked)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (!Brush)
			{
				continue;
			}
			int32 Here = 0;
			for (const FSurfFaceRef& Each : Picked)
			{
				if (Each.Group == Ref.Group && Each.BrushId == Ref.BrushId)
				{
					++Here;
				}
			}
			if (Here < Brush->Faces.Num())
			{
				if (Inspector)
				{
					Inspector->SetResult(TEXT("delete: a primitive keeps every side - pick it whole (4, or A in 3) to delete it; Cut takes a part off"));
					Inspector->RefreshSelectionCount();
				}
				SurfSession::Friction(TEXT("delete_brush_part"));
				return true;
			}
		}
	}

	FScopedTransaction Transaction(LOCTEXT("DeleteBrush", "Delete Primitive"));

	TArray<ASurfGroup*> Touched;
	TArray<FGuid> Done;
	int32 Removed = 0;

	for (const FSurfFaceRef& Ref : Picked)
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (!Group || Done.Contains(Ref.BrushId))
		{
			continue;
		}
		Done.Add(Ref.BrushId);
		if (!Touched.Contains(Group))
		{
			Group->Modify();
			Touched.Add(Group);
		}
		Removed += Group->RemoveBrush(Ref.BrushId) ? 1 : 0;
	}

	if (Removed == 0)
	{
		Transaction.Cancel();
		return false;
	}

	ClearSelection();
	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Removed == 1
			? FString(TEXT("primitive deleted"))
			: FString::Printf(TEXT("%d primitives deleted"), Removed));
		Inspector->RefreshSelectionCount();
	}
	// A group whose last brush has just gone is not a group any more.
	SweepEmptyGroups();

	SurfSession::Verb(TEXT("delete_brush"), FString::FromInt(Removed));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::DeleteSelection()
{
	// A placement picked inside a room (S71b): it is what goes.
	if (IsInsidePrefab() && RoomPickedPlacement.IsValid())
	{
		return RemovePickedPlacement();
	}

	// A brush picked and nothing of a patch: the brush is what goes (S99a).
	// "need be able delete a box ?" - Delete resolved a patch first and gave
	// up when there was not one, so a box could be made and never unmade.
	if (PointSelection.Num() == 0 && QuadSelection.Num() == 0 && !SelectedPatch.IsValid()
		&& (Selection.Num() > 0 || ActiveFace.IsValid()))
	{
		return DeleteSelectedBrushes();
	}

	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	const FGuid PatchId = Found->Id;

	// Faces picked: they go, and the grid comes apart around the hole into
	// as many patches as it takes. That is what deleting faces means on a
	// grid, and it used to be refused as "not a row or column".
	if (QuadSelection.Num() > 0)
	{
		return DeleteSelectedFaces();
	}

	// Nothing picked inside the patch, or the whole patch is the thing being
	// worked on: the patch is what goes.
	const bool bWholePatch = SubMode == ESurfSubMode::Object || PointSelection.Num() == 0;

	if (bWholePatch)
	{
		FScopedTransaction Transaction(LOCTEXT("DeletePatch", "Delete Patch"));

		// Every patch picked, not only the active one.
		const TArray<FSurfPatchRef> Patches = GetSelectedPatches();
		TArray<ASurfGroup*> Touched;
		int32 Removed = 0;

		for (const FSurfPatchRef& Each : Patches)
		{
			ASurfGroup* EachGroup = Each.Group.Get();
			if (!EachGroup)
			{
				continue;
			}
			if (!Touched.Contains(EachGroup))
			{
				EachGroup->Modify();
				Touched.Add(EachGroup);
			}
			Removed += EachGroup->RemovePatch(Each.PatchId) ? 1 : 0;
		}

		// Primitives picked with them in Object (a group of both, S249b) go too.
		if (SubMode == ESurfSubMode::Object)
		{
			TArray<FSurfFaceRef> PickedSides = Selection;
			if (ActiveFace.IsValid())
			{
				PickedSides.AddUnique(ActiveFace);
			}
			TSet<FGuid> Gone;
			for (const FSurfFaceRef& Ref : PickedSides)
			{
				ASurfGroup* EachGroup = Ref.Group.Get();
				if (!EachGroup || Gone.Contains(Ref.BrushId))
				{
					continue;
				}
				Gone.Add(Ref.BrushId);
				if (!Touched.Contains(EachGroup))
				{
					EachGroup->Modify();
					Touched.Add(EachGroup);
				}
				Removed += EachGroup->RemoveBrush(Ref.BrushId) ? 1 : 0;
			}
			if (Gone.Num() > 0)
			{
				Selection.Reset();
				ActiveFace = FSurfFaceRef();
			}
		}

		if (Removed == 0)
		{
			Transaction.Cancel();
			return false;
		}

		ClearPointSelection();
		SelectPatch(FSurfPatchRef());
		for (ASurfGroup* Each : Touched)
		{
			Each->RebuildPreview();
		}

		if (Inspector)
		{
			Inspector->SetResult(Removed == 1 ? FString(TEXT("patch deleted")) : FString::Printf(TEXT("%d patches deleted"), Removed));
			Inspector->RefreshSelectionCount();
		}

		SurfSession::Verb(TEXT("delete"), TEXT("patch"));

		if (GEditor)
		{
			RedrawViewports();
		}

		return true;
	}

	// Inside a patch, a grid can only lose whole lines. Every distinct row or
	// column the selection touches goes, highest index first so the earlier
	// ones do not move underneath the loop.
	bool bSameRow = true;
	bool bSameColumn = true;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		bSameRow    &= (Ref.Y == PointSelection[0].Y);
		bSameColumn &= (Ref.X == PointSelection[0].X);
	}

	if (!bSameRow && !bSameColumn)
	{
		// Not a line, but cells (S165a): "on this selection very simple -
		// delete just dont work". The cells whose corners are all picked go
		// as faces do, and the rest comes apart round the hole.
		TArray<FSurfQuadRef> Held = QuadsHeldByPoints();
		if (Held.Num() > 0)
		{
			QuadSelection = MoveTemp(Held);
			return DeleteSelectedFaces();
		}
		if (Inspector)
		{
			Inspector->SetResult(TEXT("delete: pick a row, a column, or whole cells - a grid cannot lose single points"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("delete_not_a_line"));
		return false;
	}

	const bool bColumn = bSameColumn;

	TArray<int32> Lines;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		Lines.AddUnique(bColumn ? Ref.X : Ref.Y);
	}
	Lines.Sort();

	FScopedTransaction Transaction(LOCTEXT("DeleteLine", "Delete Line"));

	FSurfPatch* Mutable = Group->ModifyPatch(PatchId);
	if (!Mutable)
	{
		Transaction.Cancel();
		return false;
	}

	int32 Removed = 0;
	for (int32 I = Lines.Num() - 1; I >= 0; --I)
	{
		const bool bOk = bColumn
			? SurfPatchPrimitives::RemoveColumn(*Mutable, Lines[I])
			: SurfPatchPrimitives::RemoveRow(*Mutable, Lines[I]);

		if (bOk)
		{
			++Removed;
		}
	}

	if (Removed == 0)
	{
		Transaction.Cancel();

		if (Inspector)
		{
			Inspector->SetResult(TEXT("delete: the patch is already as small as it goes"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("delete_refused"));
		return false;
	}

	SurfPatchSurfacing::Reapply(*Mutable);
	ClearPointSelection();
	Group->RebuildPreview();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("deleted %d %s: now %d x %d"),
			Removed, bColumn ? TEXT("column(s)") : TEXT("row(s)"), Mutable->Width, Mutable->Height));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("delete"), FString::Printf(TEXT("%d %s -> %dx%d"),
		Removed, bColumn ? TEXT("columns") : TEXT("rows"), Mutable->Width, Mutable->Height));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

int32 USurfEdMode::SelectionStackDepth()
{
	TArray<FSurfPatchRef> Reached = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Reached.AddUnique(Held);
	}

	int32 Depth = 0;
	for (const FSurfPatchRef& Ref : Reached)
	{
		ASurfGroup* EachGroup = Ref.Group.Get();
		const FSurfPatch* Found = EachGroup ? EachGroup->FindPatch(Ref.PatchId) : nullptr;
		if (Found)
		{
			Depth = FMath::Max(Depth, Found->StackLevel + 1);
		}
	}
	return Depth;
}

// A layer is not a shader feature. It is another patch on the one below,
// lifted a hair along the normal at build and in the preview rather than by
// world position offset in a material (D14) - so the verb is a copy in place
// at the next stack level, and nothing else about it changes. The lift is
// what gives decals and alphas an order: layer 0 is the opaque surface, 1, 2
// and 3 draw over it in turn, each by its own material's alpha (D64). The
// weight is left alone; it is a vertex attribute a material may read, not
// what makes a layer visible.
bool USurfEdMode::MakeLayerFromSelection()
{
	TArray<FSurfPatchRef> Sources = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Sources.AddUnique(Held);
	}

	struct FCopy
	{
		ASurfGroup* Group = nullptr;
		FSurfPatch Patch;
		bool bActive = false;
	};
	TArray<FCopy> Copies;
	int32 Deepest = 0;
	for (const FSurfPatchRef& Ref : Sources)
	{
		ASurfGroup* EachGroup = Ref.Group.Get();
		const FSurfPatch* Found = EachGroup ? EachGroup->FindPatch(Ref.PatchId) : nullptr;
		if (!Found)
		{
			continue;
		}
		Deepest = FMath::Max(Deepest, Found->StackLevel + 1);

		FCopy& Copy = Copies.AddDefaulted_GetRef();
		Copy.Group = EachGroup;
		Copy.Patch = *Found;
		Copy.Patch.Id.Invalidate();
		Copy.Patch.EnsureId();
		Copy.Patch.StackLevel = Found->StackLevel + 1;
		Copy.bActive = Ref == SelectedPatch;
	}

	if (Copies.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("layer: nothing selected - pick a patch to put a layer on"));
		}
		return false;
	}

	// Nine is where it stops, not where it warns: past that the overlap is
	// shaded nine times over and the answer is a different material, not
	// another patch.
	if (Deepest >= SurfPatchMath::MaxStackDepth)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(
				TEXT("layer: the stack is already %d deep, which is the limit - bake or merge before adding another"),
				SurfPatchMath::MaxStackDepth));
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("MakeLayer", "Make Layer"));

	TArray<ASurfGroup*> Groups;
	TArray<FSurfPatchRef> Made;
	FSurfPatchRef ActiveCopy;
	for (FCopy& Copy : Copies)
	{
		if (!Groups.Contains(Copy.Group))
		{
			Copy.Group->Modify();
			Groups.Add(Copy.Group);
		}
		FSurfPatchRef Ref;
		Ref.Group = Copy.Group;
		Ref.PatchId = Copy.Patch.Id;
		Copy.Group->AddPatch(MoveTemp(Copy.Patch));
		Made.Add(Ref);
		if (Copy.bActive || !ActiveCopy.IsValid())
		{
			ActiveCopy = Ref;
		}
	}
	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// The layers are the selection after, so the material or the paint that
	// follows goes on them and not on what they were made from.
	SetSubMode(ESurfSubMode::Vertex);
	if (ActiveCopy.IsValid() && !(Made[0] == ActiveCopy))
	{
		Made.Remove(ActiveCopy);
		Made.Insert(ActiveCopy, 0);
	}
	SelectMadePatches(Made);

	const int32 Depth = Deepest + 1;
	if (Inspector)
	{
		const USurfSettings* LayerSettings = GetDefault<USurfSettings>();
		const int32 Warn = LayerSettings ? FMath::Max(1, LayerSettings->StackDepthWarn) : 3;
		const FString Which = Made.Num() == 1
			? FString(TEXT("layer added"))
			: FString::Printf(TEXT("%d layers added"), Made.Num());
		Inspector->SetResult(Depth > Warn
			? FString::Printf(TEXT("%s - %d deep, past %d: every layer shades the overlap again"), *Which, Depth, Warn)
			: FString::Printf(TEXT("%s, drawn over what is beneath - give it its material"), *Which));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("make_layer"), FString::Printf(TEXT("patches=%d depth=%d"), Made.Num(), Depth));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::DuplicateSelectionToPatch(double OffsetCm)
{
	// Every patch the selection holds - several picked as wholes in object
	// sub-mode, or the patches a point selection spans - each copied whole,
	// all in one step. The copies are the selection after, so the drag that
	// follows takes them and not what they came from.
	TArray<FSurfPatchRef> Sources = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Sources.AddUnique(Held);
	}

	// Optionally pushed out along the normal, which is the difference between
	// a copy that z-fights until it is moved and one that does not. The
	// setting is the default; an adjustment afterwards passes its own.
	const USurfSettings* DupSettings = GetDefault<USurfSettings>();
	const double Offset = OffsetCm >= 0.0 ? OffsetCm : (DupSettings ? DupSettings->DuplicateOffsetCm : 0.0);

	struct FCopy
	{
		ASurfGroup* Group = nullptr;
		FSurfPatch Patch;
		bool bActive = false;
	};
	TArray<FCopy> Copies;
	for (const FSurfPatchRef& Ref : Sources)
	{
		ASurfGroup* EachGroup = Ref.Group.Get();
		const FSurfPatch* Found = EachGroup ? EachGroup->FindPatch(Ref.PatchId) : nullptr;
		if (!Found)
		{
			continue;
		}
		FCopy& Copy = Copies.AddDefaulted_GetRef();
		Copy.Group = EachGroup;
		Copy.Patch = *Found;
		Copy.Patch.Id.Invalidate();
		Copy.Patch.EnsureId();
		Copy.bActive = Ref == SelectedPatch;
		if (Offset > KINDA_SMALL_NUMBER)
		{
			// Along its own normal: two patches facing different ways each
			// come off their own surface.
			const FVector Normal = SurfPatchSurfacing::AverageNormal(Copy.Patch);
			for (FSurfControlPoint& Point : Copy.Patch.Points)
			{
				Point.Pos += Normal * Offset;
			}
		}
	}
	if (Copies.Num() == 0)
	{
		return false;
	}

	// In place, not offset. An offset copy has to be found and moved back
	// before it can be put anywhere; a copy on top of the original is already
	// where the next drag starts from.
	//
	// The cost of that choice: until it is dragged off, there are two
	// surfaces in the same place and they fight for the same depth, which
	// looks like the surface flickering. The result line says so, because
	// "the surface is flickering" is otherwise a bug report about nothing.
	FScopedTransaction Transaction(LOCTEXT("DuplicatePatch", "Duplicate Patch"));

	TArray<ASurfGroup*> Groups;
	TArray<FSurfPatchRef> Made;
	FSurfPatchRef ActiveCopy;
	for (FCopy& Copy : Copies)
	{
		if (!Groups.Contains(Copy.Group))
		{
			Copy.Group->Modify();
			Groups.Add(Copy.Group);
		}
		FSurfPatchRef Ref;
		Ref.Group = Copy.Group;
		Ref.PatchId = Copy.Patch.Id;
		Copy.Group->AddPatch(MoveTemp(Copy.Patch));
		Made.Add(Ref);
		if (Copy.bActive || !ActiveCopy.IsValid())
		{
			ActiveCopy = Ref;
		}
	}
	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// Every point of every copy is selected, so they can be dragged straight
	// off the originals rather than hunted for; the active patch's copy is
	// the active one.
	SetSubMode(ESurfSubMode::Vertex);
	if (ActiveCopy.IsValid() && !(Made[0] == ActiveCopy))
	{
		Made.Remove(ActiveCopy);
		Made.Insert(ActiveCopy, 0);
	}
	SelectMadePatches(Made);

	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::Duplicate;
		Op.Distance = Offset;
		NoteOperation(Op);
	}

	if (Inspector)
	{
		const FString Which = Made.Num() == 1 ? FString(TEXT("duplicated")) : FString::Printf(TEXT("duplicated %d patches"), Made.Num());
		Inspector->SetResult(Offset > KINDA_SMALL_NUMBER
			? FString::Printf(TEXT("%s %.0f cm along the normal"), *Which, Offset)
			: FString::Printf(TEXT("%s in place - drag %s off with G, or the surfaces will flicker against each other"), *Which, Made.Num() == 1 ? TEXT("it") : TEXT("them")));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("duplicate_patch"), FString::Printf(TEXT("patches=%d offset=%.0f"), Made.Num(), Offset));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::SplitSelectedPatch()
{
	// Every patch the point selection reaches into is cut along its own
	// line - which line is read off its points the same way extrude reads
	// its edge - all in one step. The near halves are the selection after.
	struct FSplit
	{
		ASurfGroup* Group = nullptr;
		FGuid OldId;
		bool bActive = false;
		bool bColumn = false;
		int32 Index = INDEX_NONE;
		TArray<FSurfPointRef> Points;
		FSurfPatch A;
		FSurfPatch B;
		bool bRefused = false;
	};
	TArray<FSplit> Splits;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		ASurfGroup* EachGroup = Ref.Group.Get();
		if (!EachGroup || !Ref.PatchId.IsValid())
		{
			continue;
		}
		FSplit* Split = Splits.FindByPredicate([&](const FSplit& Each) { return Each.Group == EachGroup && Each.OldId == Ref.PatchId; });
		if (!Split)
		{
			if (!EachGroup->FindPatch(Ref.PatchId))
			{
				continue;
			}
			Split = &Splits.AddDefaulted_GetRef();
			Split->Group = EachGroup;
			Split->OldId = Ref.PatchId;
			Split->bActive = SelectedPatch.Group.Get() == EachGroup && SelectedPatch.PatchId == Ref.PatchId;
		}
		Split->Points.Add(Ref);
	}

	if (Splits.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("split: select the row or column to cut along"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	int32 Refused = 0;
	FString Reason;
	for (FSplit& Split : Splits)
	{
		const FSurfPatch& Patch = *Split.Group->FindPatch(Split.OldId);

		// The cut follows a whole line, and which line is read off the
		// selection the same way extrude reads its edge.
		bool bSameRow = true;
		bool bSameColumn = true;
		for (const FSurfPointRef& Ref : Split.Points)
		{
			bSameRow    &= (Ref.Y == Split.Points[0].Y);
			bSameColumn &= (Ref.X == Split.Points[0].X);
		}
		Split.bColumn = bSameColumn && !bSameRow;
		Split.Index = Split.bColumn ? Split.Points[0].X : Split.Points[0].Y;

		if (!SurfPatchPrimitives::Split(Patch, Split.bColumn, Split.Index, Split.A, Split.B))
		{
			Split.bRefused = true;
			++Refused;
			if (Reason.IsEmpty())
			{
				Reason = Patch.Type == ESurfPatchType::Curve
					? TEXT("a curve can only be cut where both halves stay odd")
					: TEXT("pick a line that is not on the border");
			}
		}
	}

	if (Refused == Splits.Num())
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("split: %s"), *Reason));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("split_refused"));
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("SplitPatch", "Split Patch"));

	TArray<ASurfGroup*> Groups;
	TArray<FSurfPatchRef> Keeps;
	FSurfPatchRef ActiveKeep;
	for (FSplit& Split : Splits)
	{
		if (Split.bRefused)
		{
			continue;
		}
		if (!Groups.Contains(Split.Group))
		{
			Split.Group->Modify();
			Groups.Add(Split.Group);
		}
		FSurfPatchRef Keep;
		Keep.Group = Split.Group;
		Keep.PatchId = Split.A.Id;
		Split.Group->RemovePatch(Split.OldId);
		Split.Group->AddPatch(MoveTemp(Split.A));
		Split.Group->AddPatch(MoveTemp(Split.B));
		Keeps.Add(Keep);
		if (Split.bActive || !ActiveKeep.IsValid())
		{
			ActiveKeep = Keep;
		}

		SurfSession::Verb(TEXT("split_patch"), FString::Printf(TEXT("%s at %d"),
			Split.bColumn ? TEXT("column") : TEXT("row"), Split.Index));
	}
	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	SelectPatch(ActiveKeep);
	PatchSelection = Keeps;
	SelectedPatch = ActiveKeep;

	if (Inspector)
	{
		const FString Skipped = Refused > 0 ? FString::Printf(TEXT(" (%d not cut: %s)"), Refused, *Reason) : FString();
		Inspector->SetResult(Keeps.Num() == 1
			? FString::Printf(TEXT("split in two - the near half is selected%s"), *Skipped)
			: FString::Printf(TEXT("split %d patches in two - the near halves are selected%s"), Keeps.Num(), *Skipped));
		Inspector->RefreshSelectionCount();
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::SolidifySelectedPatch(double ThicknessCm, int32 Side, bool bRim, bool bEven)
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	// With faces picked, those are what get thickness - not the whole patch.
	// Thickening the whole patch when a block of it was picked read as
	// "solidify only did one row". Every face picked is the whole patch
	// again, and goes the whole-patch way, which a curve can take.
	if (QuadSelection.Num() > 0)
	{
		// Per patch the faces sit on: every face of every one of them picked
		// is the whole patches again.
		struct FOnPatch
		{
			FSurfPatchRef Ref;
			int32 Picked = 0;
		};
		TArray<FOnPatch> OnEach;
		for (const FSurfQuadRef& Quad : QuadSelection)
		{
			FSurfPatchRef Key;
			Key.Group = Quad.Group;
			Key.PatchId = Quad.PatchId;
			FOnPatch* On = OnEach.FindByPredicate([&Key](const FOnPatch& Each) { return Each.Ref == Key; });
			if (!On)
			{
				On = &OnEach.AddDefaulted_GetRef();
				On->Ref = Key;
			}
			++On->Picked;
		}
		bool bWhole = true;
		for (const FOnPatch& On : OnEach)
		{
			const ASurfGroup* EachGroup = On.Ref.Group.Get();
			const FSurfPatch* Each = EachGroup ? EachGroup->FindPatch(On.Ref.PatchId) : nullptr;
			const int32 Faces = Each ? FMath::Max(0, Each->Width - 1) * FMath::Max(0, Each->Height - 1) : 0;
			bWhole &= Each != nullptr && On.Picked >= Faces;
		}
		if (!bWhole)
		{
			return SolidifySelectedFaces(ThicknessCm, Side, bRim, bEven);
		}
	}

	SurfPatchPrimitives::FSurfSolidifyOptions Options;
	Options.Side = FMath::Clamp(Side, -1, 1);
	Options.bRim = bRim;
	Options.bEven = bEven;

	// Every patch the selection holds - several when they were picked as
	// wholes in object sub-mode - each solidified on its own, all in one
	// step. The fronts are the selection after, the active patch's front
	// active, so the step undoes and adjusts as one.
	struct FSolidified
	{
		ASurfGroup* Group = nullptr;
		FGuid OldId;
		TArray<FSurfPatch> Made;
		bool bActive = false;
	};
	TArray<FSolidified> Results;
	TArray<FSurfPatchRef> Sources = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Sources.AddUnique(Held);
	}
	for (const FSurfPatchRef& Ref : Sources)
	{
		ASurfGroup* EachGroup = Ref.Group.Get();
		const FSurfPatch* Each = EachGroup ? EachGroup->FindPatch(Ref.PatchId) : nullptr;
		if (!Each)
		{
			continue;
		}
		FSolidified& Result = Results.AddDefaulted_GetRef();
		Result.Group = EachGroup;
		Result.OldId = Each->Id;
		Result.bActive = Ref == SelectedPatch;
		if (!SurfPatchPrimitives::Solidify(*Each, ThicknessCm, Result.Made, Options) || Result.Made.Num() == 0)
		{
			Results.Pop();
		}
	}

	if (Results.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("solidify: needs a thickness other than zero"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("solidify_refused"));
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("SolidifyPatch", "Solidify Patch"));

	TArray<ASurfGroup*> Groups;
	TArray<FSurfPatchRef> Fronts;
	FSurfPatchRef ActiveFront;
	int32 MadeCount = 0;
	for (FSolidified& Result : Results)
	{
		if (!Groups.Contains(Result.Group))
		{
			Result.Group->Modify();
			Groups.Add(Result.Group);
		}
		Result.Group->RemovePatch(Result.OldId);

		FSurfPatchRef Front;
		Front.Group = Result.Group;
		Front.PatchId = Result.Made[0].Id;
		for (FSurfPatch& One : Result.Made)
		{
			Result.Group->AddPatch(MoveTemp(One));
			++MadeCount;
		}
		Fronts.Add(Front);
		if (Result.bActive || !ActiveFront.IsValid())
		{
			ActiveFront = Front;
		}
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	SelectPatch(ActiveFront);
	PatchSelection = Fronts;
	SelectedPatch = ActiveFront;

	if (Inspector)
	{
		Inspector->SetResult(Results.Num() == 1
			? FString::Printf(TEXT("solidified into %d patches - front selected"), MadeCount)
			: FString::Printf(TEXT("solidified %d patches into %d - their fronts selected"), Results.Num(), MadeCount));
	}

	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::Solidify;
		Op.Distance = ThicknessCm;
		Op.Side = Options.Side;
		Op.bRim = bRim;
		Op.bEven = bEven;
		NoteOperation(Op);
	}
	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("solidify"), FString::Printf(TEXT("%.0f cm side=%d rim=%d even=%d patches=%d -> %d patches"), ThicknessCm, Options.Side, bRim ? 1 : 0, bEven ? 1 : 0, Results.Num(), MadeCount));

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

// --- Framing ------------------------------------------------------------------

bool USurfEdMode::FindPointUnderCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport, FVector& OutPoint, FVector* OutDirection) const
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

	FIntPoint Cursor;
	Viewport->GetMousePos(Cursor);

	const FViewportClick Click(View, ViewportClient, EKeys::LeftMouseButton, IE_Released, Cursor.X, Cursor.Y);

	const FVector Origin = Click.GetOrigin();
	const FVector Direction = Click.GetDirection().GetSafeNormal();
	if (OutDirection)
	{
		*OutDirection = Direction;
	}

	double Nearest = TNumericLimits<double>::Max();
	FVector Landing = FVector::ZeroVector;
	bool bFound = false;

	// Our own geometry first, because it is what is being worked on and it is
	// not always something the physics trace can see.
	FSurfPatchRef Patch;
	FVector PatchHit = FVector::ZeroVector;
	if (PickPatch(ViewportClient, Click, Patch, &PatchHit))
	{
		Nearest = FVector::Dist(Origin, PatchHit);
		Landing = PatchHit;
		bFound = true;
	}

	FSurfFaceRef Face;
	FVector FaceHit = FVector::ZeroVector;
	if (PickFace(ViewportClient, Click, Face, &FaceHit))
	{
		const double Distance = FVector::Dist(Origin, FaceHit);
		if (Distance < Nearest)
		{
			Nearest = Distance;
			Landing = FaceHit;
			bFound = true;
		}
	}

	// Then anything else in the level, so the key works before there is any
	// of our geometry to aim at.
	if (UWorld* World = ViewportClient->GetWorld())
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SurfUnderCursor), /*bTraceComplex*/ true);

		if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 1.0e7, ECC_Visibility, Params))
		{
			const double Distance = FVector::Dist(Origin, Hit.ImpactPoint);
			if (Distance < Nearest)
			{
				Nearest = Distance;
				Landing = Hit.ImpactPoint;
				bFound = true;
			}
		}
	}

	if (bFound)
	{
		OutPoint = Landing;
	}
	return bFound;
}

bool USurfEdMode::TeleportToCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	FVector Landing = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	if (!FindPointUnderCursor(ViewportClient, Viewport, Landing, &Direction))
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("nothing under the cursor to go to"));
			Inspector->RefreshSelectionCount();
		}
		return ViewportClient != nullptr;
	}

	// Stop short of the surface rather than inside it, and keep looking the
	// way the camera was already looking - a teleport that also turned the
	// camera would lose which way is which.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Standoff = Settings ? Settings->TeleportStandoffCm : 200.0;

	ViewportClient->SetViewLocation(Landing - Direction * Standoff);
	ViewportClient->GetViewTransform().SetLookAt(Landing);
	ViewportClient->Invalidate();

	SurfSession::Gesture(TEXT("teleport"), FString::Printf(TEXT("%.0f cm away"), FVector::Dist(ViewportClient->GetViewLocation(), Landing)));

	return true;
}

void USurfEdMode::UpdateOrbitPivot()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !GEditor)
	{
		return;
	}

	// The centroid of whatever is selected, at whatever depth: the points if
	// any are picked, otherwise the patch.
	FVector Pivot = FVector::ZeroVector;
	int32 Counted = 0;

	if (PointSelection.Num() > 0)
	{
		const ASurfGroup* Group = PointSelection[0].Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(PointSelection[0].PatchId) : nullptr;

		if (Patch)
		{
			const FTransform ToWorld = Group->GetActorTransform();

			for (const FSurfPointRef& Ref : PointSelection)
			{
				if (Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
				{
					Pivot += ToWorld.TransformPosition(Patch->At(Ref.X, Ref.Y).Pos);
					++Counted;
				}
			}
		}
	}

	if (Counted == 0)
	{
		const ASurfGroup* Group = SelectedPatch.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;

		if (!Patch)
		{
			// Nothing selected: the orbit goes back to the point ahead of the
			// camera, rather than around wherever the last selection was.
			if (Camera)
			{
				Camera->ClearSelectionPivot();
			}
			return;
		}

		const FTransform ToWorld = Group->GetActorTransform();

		for (const FSurfControlPoint& Point : Patch->Points)
		{
			Pivot += ToWorld.TransformPosition(Point.Pos);
			++Counted;
		}
	}

	if (Counted == 0)
	{
		return;
	}

	Pivot /= Counted;

	// The editor's own look-at, only when asked: its Alt-drag orbit swings
	// the camera to centre that point the moment a drag starts, which with
	// the look-at following the selection read as the view snapping about.
	// The look-at only, never the location: moving the camera because the
	// selection changed would be its own kind of maddening.
	if (Settings->bKeepOrbitPointOnSelection)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client)
			{
				Client->GetViewTransform().SetLookAt(Pivot);
			}
		}
	}

	// And the orbit pivot of our own camera, which is the one the middle
	// button turns around. Same point, so the two schemes agree about what
	// "orbit the selection" means.
	if (!Camera)
	{
		Camera = MakeUnique<FSurfCamera>();
	}

	Camera->SetPivot(Pivot);
}

bool USurfEdMode::BoundsToFrame(FBox& Bounds) const
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Patch = ResolveSelectedPatch(*this, Group);

	Bounds = FBox(ForceInit);

	// Narrowest meaningful thing first: the points, then the patch they are
	// on, then the group. Pressing focus twice should not zoom out.
	if (Patch && Group && PointSelection.Num() > 0)
	{
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FSurfPointRef& Ref : PointSelection)
		{
			if (Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				Bounds += ToWorld.TransformPosition(Patch->At(Ref.X, Ref.Y).Pos);
			}
		}
	}
	else if (Patch && Group)
	{
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FSurfControlPoint& Point : Patch->Points)
		{
			Bounds += ToWorld.TransformPosition(Point.Pos);
		}
	}
	/**
	 * A brush is framed by the brush (S135a): "created some box cant focus
	 * on it", "f should focus on box selection".
	 *
	 * The branch that was here asked the **group** for its bounding box, and
	 * a group is everything he has built in it - so framing a box he had
	 * just made framed the whole room around it, which looks exactly like
	 * nothing happening. Worse, it only ran when a face was in `Selection`,
	 * and a box fresh from the bar has its top in `ActiveFace` and nothing
	 * in `Selection` at all: the commonest case fell straight through to
	 * "every group we own".
	 *
	 * Corners first, then the brushes the picked faces belong to - the same
	 * narrowest-first order the patch side follows. A picked face frames its
	 * **brush** rather than its own flat polygon, because a face has no
	 * thickness and framing one puts the camera inside the solid.
	 */
	if (!Bounds.IsValid && BrushCornerSelection.Num() > 0)
	{
		TArray<FVector> Corners;
		GatherBrushCorners(Corners);
		for (const FVector& Corner : Corners)
		{
			Bounds += Corner;
		}
	}

	if (!Bounds.IsValid)
	{
		for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
		{
			const ASurfGroup* Holder = Ref.Group.Get();
			const FSurfBrush* Brush = Holder ? Holder->FindBrush(Ref.BrushId) : nullptr;
			if (!Brush)
			{
				continue;
			}
			FSurfHull Hull;
			if (!SurfBrushMath::DeriveHull(*Brush, Hull))
			{
				continue;
			}
			const FTransform ToWorld = Holder->GetActorTransform();
			for (const FVector& Vertex : Hull.Vertices)
			{
				Bounds += ToWorld.TransformPosition(Vertex);
			}
		}
	}

	/**
	 * And a selected actor (S135a): the same failure one level up. F with a
	 * prefab instance picked framed every group in the level, because
	 * nothing of *ours* was selected - and a placed prefab is a thing he
	 * selects all day.
	 */
	if (!Bounds.IsValid && GEditor)
	{
		TArray<AActor*> Chosen;
		if (USelection* Selected = GEditor->GetSelectedActors())
		{
			Selected->GetSelectedObjects<AActor>(Chosen);
		}
		for (const AActor* Actor : Chosen)
		{
			if (Actor)
			{
				Bounds += Actor->GetComponentsBoundingBox(true);
			}
		}
	}

	// Nothing of ours selected. Framing every group we own beats returning
	// false, which handed F back to the editor and framed the sky sphere.
	if (!Bounds.IsValid)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				if (IsOpen(*It))
				{
					Bounds += It->GetComponentsBoundingBox(true);
				}
			}
		}
	}

	if (!Bounds.IsValid)
	{
		// Still nothing: the key is answered anyway rather than let the
		// editor frame the whole level from inside our mode.
		SurfSession::Friction(TEXT("focus_nothing_to_frame"));
		return false;
	}

	// A single point has no extent, and framing a zero box puts the camera
	// inside it.
	if (Bounds.GetSize().IsNearlyZero())
	{
		Bounds = Bounds.ExpandBy(100.0);
	}
	return true;
}

bool USurfEdMode::FocusSelection(FEditorViewportClient* ViewportClient)
{
	if (!ViewportClient)
	{
		return false;
	}

	FBox Bounds(ForceInit);
	if (!BoundsToFrame(Bounds))
	{
		return true;
	}

	// Framing also sets what the next orbit turns around. Without that, F puts
	// something on screen and the camera then swings away from it.
	if (FSurfCamera::IsBlenderScheme())
	{
		if (!Camera)
		{
			Camera = MakeUnique<FSurfCamera>();
		}
		Camera->Frame(ViewportClient, Bounds);
	}
	else
	{
		ViewportClient->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent().GetMax() * 0.25 + 25.0));
	}

	SurfSession::Verb(TEXT("focus"), FString::Printf(TEXT("points=%d extent=%.0f"),
		PointSelection.Num(), Bounds.GetExtent().GetMax()));

	return true;
}

TArray<FSurfFaceRef> USurfEdMode::BrushesHeldBySelection() const
{
	// One entry a brush, the face index left as it came: what the callers
	// want is the brush, and the first face that reached it.
	TArray<FSurfFaceRef> Brushes;
	auto Add = [&Brushes](const FSurfFaceRef& Ref)
	{
		if (!Ref.IsValid())
		{
			return;
		}
		for (const FSurfFaceRef& Have : Brushes)
		{
			if (Have.Group == Ref.Group && Have.BrushId == Ref.BrushId)
			{
				return;
			}
		}
		Brushes.Add(Ref);
	};
	for (const FSurfFaceRef& Ref : Selection)
	{
		Add(Ref);
	}
	Add(ActiveFace);
	return Brushes;
}

bool USurfEdMode::BrushCornersPivot(FVector& OutPivot) const
{
	TArray<FVector> Corners;
	GatherBrushCorners(Corners);
	if (Corners.Num() == 0)
	{
		return false;
	}
	FVector Sum = FVector::ZeroVector;
	for (const FVector& Corner : Corners)
	{
		Sum += Corner;
	}
	OutPivot = Sum / Corners.Num();
	return true;
}

bool USurfEdMode::BrushSelectionPivot(FVector& OutPivot) const
{
	FBox Bounds(ForceInit);
	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FVector& Vertex : Hull.Vertices)
		{
			Bounds += ToWorld.TransformPosition(Vertex);
		}
	}
	if (!Bounds.IsValid)
	{
		return false;
	}
	OutPivot = Bounds.GetCenter();
	return true;
}

bool USurfEdMode::SelectedFacesPivot(FVector& OutPivot) const
{
	// The middle of the polygons the picked faces actually bound - not of
	// their planes, which are infinite - so the handles stand on the wall.
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;
	for (const FSurfFaceRef& Ref : PickedFaces())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush || !Brush->Faces.IsValidIndex(Ref.FaceIndex))
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FSurfHullFace& Face : Hull.Faces)
		{
			if (Face.SourceFaceIndex != Ref.FaceIndex || Face.VertexIndices.Num() == 0)
			{
				continue;
			}
			FVector Middle = FVector::ZeroVector;
			for (const int32 Index : Face.VertexIndices)
			{
				if (Hull.Vertices.IsValidIndex(Index))
				{
					Middle += Hull.Vertices[Index];
				}
			}
			Sum += ToWorld.TransformPosition(Middle / Face.VertexIndices.Num());
			++Count;
		}
	}
	if (Count == 0)
	{
		return false;
	}
	OutPivot = Sum / Count;
	return true;
}

int32 USurfEdMode::PushSelectedFacesAlong(const FVector& WorldDelta)
{
	// An arrow pulled across a face's normal does nothing to that face: the
	// side facing the arrow is the side that moves (S72g). Which is what
	// per-side push means with a hand on the gizmo rather than on G.
	const TArray<FSurfFaceRef> Picked = PickedFaces();
	if (Picked.Num() == 0 || WorldDelta.IsNearlyZero())
	{
		return 0;
	}

	int32 Pushed = 0;
	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		const TArray<FSurfFace> Was = Brush->Faces;
		int32 Here = 0;
		for (const FSurfFaceRef& Face : Picked)
		{
			if (Face.Group != Ref.Group || Face.BrushId != Ref.BrushId || !Brush->Faces.IsValidIndex(Face.FaceIndex))
			{
				continue;
			}
			const FPlane Plane = Brush->Faces[Face.FaceIndex].Plane;
			const FVector Normal = ToWorld.TransformVectorNoScale(FVector(Plane.X, Plane.Y, Plane.Z)).GetSafeNormal();
			const double Cm = FVector::DotProduct(WorldDelta, Normal);
			if (FMath::IsNearlyZero(Cm))
			{
				continue;
			}
			// Inside is Dot(N, X) - W <= 0, so a bigger W is further out.
			Brush->Faces[Face.FaceIndex].Plane.W = Plane.W + Cm;
			++Here;
		}
		if (Here == 0)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			// A push that leaves no volume is no push.
			Brush->Faces = Was;
			continue;
		}
		++Brush->EditSerial;
		Pushed += Here;
	}
	if (Pushed > 0)
	{
		RebuildTouchedGroups();
		if (GEditor)
		{
			RedrawViewports();
		}
	}
	return Pushed;
}

void USurfEdMode::RepickFacesByNormal(ASurfGroup* Group, const FGuid& BrushId, const TArray<FPlane>& WasPlanes)
{
	const FSurfBrush* Now = Group ? Group->FindBrush(BrushId) : nullptr;
	if (!Now || Now->Faces.Num() == 0 || WasPlanes.Num() == 0)
	{
		return;
	}

	auto Nearest = [Now](const FPlane& Was)
	{
		const FVector Looking(Was.X, Was.Y, Was.Z);
		int32 Best = INDEX_NONE;
		double Closest = -2.0;
		for (int32 I = 0; I < Now->Faces.Num(); ++I)
		{
			const FPlane& Have = Now->Faces[I].Plane;
			const double Alike = FVector::DotProduct(Looking, FVector(Have.X, Have.Y, Have.Z));
			if (Alike > Closest)
			{
				Closest = Alike;
				Best = I;
			}
		}
		return Best;
	};

	auto Repoint = [&](FSurfFaceRef& Ref)
	{
		if (Ref.Group.Get() != Group || Ref.BrushId != BrushId || !WasPlanes.IsValidIndex(Ref.FaceIndex))
		{
			return;
		}
		const int32 Found = Nearest(WasPlanes[Ref.FaceIndex]);
		if (Found != INDEX_NONE)
		{
			Ref.FaceIndex = Found;
		}
	};

	for (FSurfFaceRef& Ref : Selection)
	{
		Repoint(Ref);
	}
	Repoint(ActiveFace);
}

int32 USurfEdMode::ScaleSelectedFacesAbout(const FVector& Factor, const FVector& Pivot)
{
	/**
	 * The corners of the picked sides, scaled about a point - and the planes
	 * derived from where they end up (S146a).
	 *
	 * "S on a box face make it closer or further from origin - it shall
	 * SCALE". S141a moved the face's **plane** away from the middle, which
	 * slides the side along its own normal - a push wearing a scale's name.
	 * Scaling a side means its outline grows: the four corners spread out
	 * in the plane of the face, the sides that meet them tilt, and a box
	 * becomes a frustum. That is what scaling a face does in every tool
	 * that has one.
	 *
	 * Corners moved, planes derived - the same road S112a built for
	 * dragging a corner, and for the same reason: the hull of the moved
	 * corners says exactly what planes they imply. Scaling about a point in
	 * the face's own plane keeps the face in its plane, so the side stays
	 * flat while it grows.
	 *
	 * Every corner of every side picked is every corner of the brush, so
	 * this is also the whole-brush scale - the two answers agree where they
	 * overlap, and there is one road.
	 */
	const TArray<FSurfFaceRef> Picked = PickedFaces();
	if (Picked.Num() == 0 || Factor.Equals(FVector::OneVector, 1.e-6))
	{
		return 0;
	}

	int32 Sized = 0;
	FString Refused;
	TArray<ASurfGroup*> Touched;

	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Read = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Read)
		{
			continue;
		}

		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Read, Hull))
		{
			continue;
		}

		// Which corners the picked sides stand on.
		TSet<int32> Corners;
		for (const FSurfFaceRef& Face : Picked)
		{
			if (Face.Group != Ref.Group || Face.BrushId != Ref.BrushId)
			{
				continue;
			}
			for (const FSurfHullFace& Poly : Hull.Faces)
			{
				if (Poly.SourceFaceIndex != Face.FaceIndex)
				{
					continue;
				}
				for (const int32 Corner : Poly.VertexIndices)
				{
					Corners.Add(Corner);
				}
			}
		}
		if (Corners.Num() == 0)
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		TArray<FVector> Moved = Hull.Vertices;
		for (const int32 Corner : Corners)
		{
			if (!Moved.IsValidIndex(Corner))
			{
				continue;
			}
			const FVector World = ToWorld.TransformPosition(Hull.Vertices[Corner]);
			Moved[Corner] = ToWorld.InverseTransformPosition(Pivot + (World - Pivot) * Factor);
		}

		FSurfBrush Rebuilt;
		FString Why;
		if (!SurfBrushMath::RebuildFromPoints(*Read, Moved, Rebuilt, &Why))
		{
			// A size that leaves no solid is no size.
			if (Refused.IsEmpty())
			{
				Refused = Why;
			}
			continue;
		}

		TArray<FPlane> WasPlanes;
		for (const FSurfFace& Face : Read->Faces)
		{
			WasPlanes.Add(Face.Plane);
		}

		FSurfBrush* Write = Group->ModifyBrush(Ref.BrushId);
		if (!Write)
		{
			continue;
		}
		const FGuid Keep = Write->Id;
		*Write = MoveTemp(Rebuilt);
		Write->Id = Keep;

		// The rebuild wrote a new face list, so the refs that named the
		// sides find them again by their normals (S146c).
		RepickFacesByNormal(Group, Ref.BrushId, WasPlanes);

		++Sized;
		Touched.AddUnique(Group);
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	if (Sized == 0 && !Refused.IsEmpty() && Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("size: %s - a primitive has to stay a closed solid"), *Refused));
		SurfSession::Friction(TEXT("brush_size_refused"), Refused);
	}

	if (Sized > 0 && GEditor)
	{
		RedrawViewports();
	}
	return Sized;
}

bool USurfEdMode::TransformSelectedBrushes(const FTransform& About, const FVector& Pivot)
{
	const TArray<FSurfFaceRef> Brushes = BrushesHeldBySelection();
	if (Brushes.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("move: click a primitive face first"));
		}
		SurfSession::Friction(TEXT("brush_transform_nothing"));
		return false;
	}

	int32 Done = 0;
	for (const FSurfFaceRef& Ref : Brushes)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		// The transform is given in the world; a brush's planes are in its
		// group's space, so it goes there and back.
		const FTransform ToWorld = Group->GetActorTransform();
		const FMatrix Matrix = ToWorld.ToMatrixWithScale()
			* FTranslationMatrix(-Pivot) * About.ToMatrixWithScale() * FTranslationMatrix(Pivot)
			* ToWorld.Inverse().ToMatrixWithScale();

		const TArray<FSurfFace> Was = Brush->Faces;
		for (FSurfFace& Face : Brush->Faces)
		{
			Face.Plane = Face.Plane.TransformBy(Matrix);
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			// A transform that leaves no volume is no transform.
			Brush->Faces = Was;
			continue;
		}
		++Brush->EditSerial;
		++Done;
	}
	if (Done > 0)
	{
		RebuildTouchedGroups();
		if (GEditor)
		{
			RedrawViewports();
		}
	}
	return Done > 0;
}

int32 USurfEdMode::PushSelectedFaces(double Cm)
{
	if (Selection.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("push: pick the faces to push first"));
		}
		SurfSession::Friction(TEXT("push_nothing_selected"));
		return 0;
	}

	// Grouped by brush, because a brush is only still a brush if every one
	// of its pushes leaves a volume.
	int32 Pushed = 0;
	int32 Refused = 0;
	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		const TArray<FSurfFace> Was = Brush->Faces;
		int32 Here = 0;
		for (const FSurfFaceRef& Face : Selection)
		{
			if (Face.Group != Ref.Group || Face.BrushId != Ref.BrushId)
			{
				continue;
			}
			if (Brush->Faces.IsValidIndex(Face.FaceIndex))
			{
				// Inside is Dot(N, X) - W <= 0, so a bigger W is further out
				// along the face's own normal.
				Brush->Faces[Face.FaceIndex].Plane.W += Cm;
				++Here;
			}
		}
		FSurfHull Hull;
		if (Here == 0)
		{
			continue;
		}
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			Brush->Faces = Was;
			Refused += Here;
			continue;
		}
		++Brush->EditSerial;
		Pushed += Here;
	}
	if (Pushed > 0)
	{
		RebuildTouchedGroups();
		if (GEditor)
		{
			RedrawViewports();
		}
	}
	if (Inspector && (Pushed > 0 || Refused > 0))
	{
		Inspector->SetResult(Refused > 0
			? FString::Printf(TEXT("pushed %d face%s by %.0f cm; %d would have left no primitive and stayed"), Pushed, Pushed == 1 ? TEXT("") : TEXT("s"), Cm, Refused)
			: FString::Printf(TEXT("pushed %d face%s by %.0f cm"), Pushed, Pushed == 1 ? TEXT("") : TEXT("s"), Cm));
        Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("push_faces"), FString::Printf(TEXT("faces=%d cm=%.1f refused=%d"), Pushed, Cm, Refused));
	return Pushed;
}

namespace SurfFaceStrips
{
	/** The two ends of edge K of a wound polygon. */
	void EdgeAt(const TArray<int32>& V, int32 K, int32& OutA, int32& OutB)
	{
		OutA = V[K];
		OutB = V[(K + 1) % V.Num()];
	}

	/** Where the unordered edge {A,B} sits in this polygon's winding, or INDEX_NONE. */
	int32 EdgeIndexOf(const TArray<int32>& V, int32 A, int32 B)
	{
		for (int32 K = 0; K < V.Num(); ++K)
		{
			const int32 P = V[K];
			const int32 Q = V[(K + 1) % V.Num()];
			if ((P == A && Q == B) || (P == B && Q == A))
			{
				return K;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * The picked sides of one brush, grouped into runs that touch along an
	 * edge (S148b): "i had selected two. which vertices are shared. so i was
	 * expecting a single patch out of it."
	 *
	 * Only four-cornered sides run together, because a grid patch is rows of
	 * quads. A side with more corners, or one with three neighbours or more
	 * - a grid cannot fork - stands on its own and becomes its own patch, as
	 * every side used to. A run that closes on itself is cut once, so a band
	 * round a box comes out as a strip whose first and last lines land on
	 * each other.
	 */
	void Chains(const TArray<const FSurfHullFace*>& Polys, TArray<TArray<int32>>& Out)
	{
		const int32 Count = Polys.Num();
		TArray<TArray<int32>> Next;
		Next.SetNum(Count);

		for (int32 I = 0; I < Count; ++I)
		{
			if (Polys[I]->VertexIndices.Num() != 4)
			{
				continue;
			}
			for (int32 J = I + 1; J < Count; ++J)
			{
				if (Polys[J]->VertexIndices.Num() != 4)
				{
					continue;
				}
				bool bTouch = false;
				for (int32 K = 0; K < 4 && !bTouch; ++K)
				{
					int32 A = INDEX_NONE;
					int32 B = INDEX_NONE;
					EdgeAt(Polys[I]->VertexIndices, K, A, B);
					bTouch = EdgeIndexOf(Polys[J]->VertexIndices, A, B) != INDEX_NONE;
				}
				if (bTouch)
				{
					Next[I].Add(J);
					Next[J].Add(I);
				}
			}
		}

		TArray<bool> bRuns;
		bRuns.Init(true, Count);
		for (int32 I = 0; I < Count; ++I)
		{
			if (Polys[I]->VertexIndices.Num() != 4 || Next[I].Num() > 2)
			{
				bRuns[I] = false;
			}
		}
		for (int32 I = 0; I < Count; ++I)
		{
			if (bRuns[I])
			{
				continue;
			}
			for (int32 J = 0; J < Count; ++J)
			{
				Next[J].Remove(I);
			}
		}

		TArray<bool> bUsed;
		bUsed.Init(false, Count);

		auto Walk = [&Next, &bUsed, &Out](int32 Start)
		{
			TArray<int32> Run;
			int32 Cur = Start;
			int32 Prev = INDEX_NONE;
			while (Cur != INDEX_NONE && !bUsed[Cur])
			{
				bUsed[Cur] = true;
				Run.Add(Cur);
				int32 Step = INDEX_NONE;
				for (const int32 Neighbour : Next[Cur])
				{
					if (Neighbour != Prev && !bUsed[Neighbour])
					{
						Step = Neighbour;
						break;
					}
				}
				Prev = Cur;
				Cur = Step;
			}
			Out.Add(MoveTemp(Run));
		};

		// From an end, so a run is walked whole rather than from its middle.
		for (int32 I = 0; I < Count; ++I)
		{
			if (bRuns[I] && !bUsed[I] && Next[I].Num() <= 1)
			{
				Walk(I);
			}
		}
		// What is left has no end: a ring, cut wherever the walk begins.
		for (int32 I = 0; I < Count; ++I)
		{
			if (bRuns[I] && !bUsed[I])
			{
				Walk(I);
			}
		}
		for (int32 I = 0; I < Count; ++I)
		{
			if (!bUsed[I])
			{
				bUsed[I] = true;
				TArray<int32> One;
				One.Add(I);
				Out.Add(MoveTemp(One));
			}
		}
	}

	/**
	 * The lines a run of sides makes, in order: each line is the two ends of
	 * an edge, matched end for end with the line before it, so the grid does
	 * not twist where the sides fold (S148b).
	 *
	 * The far edge of the first side comes first and the shared edge second,
	 * which is what keeps each cell wound the way the side it came from was
	 * - a strip that faced inward would be the whole point of the verb lost.
	 */
	bool Lines(const TArray<const FSurfHullFace*>& Polys, const TArray<int32>& Run, TArray<TPair<int32, int32>>& Out)
	{
		if (Run.Num() < 2)
		{
			return false;
		}

		const TArray<int32>& First = Polys[Run[0]]->VertexIndices;
		const TArray<int32>& Second = Polys[Run[1]]->VertexIndices;
		if (First.Num() != 4 || Second.Num() != 4)
		{
			return false;
		}

		int32 Shared = INDEX_NONE;
		for (int32 K = 0; K < 4 && Shared == INDEX_NONE; ++K)
		{
			int32 A = INDEX_NONE;
			int32 B = INDEX_NONE;
			EdgeAt(First, K, A, B);
			if (EdgeIndexOf(Second, A, B) != INDEX_NONE)
			{
				Shared = K;
			}
		}
		if (Shared == INDEX_NONE)
		{
			return false;
		}

		Out.Add(TPair<int32, int32>(First[(Shared + 2) % 4], First[(Shared + 3) % 4]));
		Out.Add(TPair<int32, int32>(First[(Shared + 1) % 4], First[Shared]));

		for (int32 At = 1; At < Run.Num(); ++At)
		{
			const TArray<int32>& V = Polys[Run[At]]->VertexIndices;
			if (V.Num() != 4)
			{
				return false;
			}
			const TPair<int32, int32> Line = Out.Last();
			const int32 T = EdgeIndexOf(V, Line.Key, Line.Value);
			if (T == INDEX_NONE)
			{
				return false;
			}

			// Across a quad, the first corner of an edge is joined to the
			// last of the polygon and the second to the third.
			const int32 P = V[T];
			const int32 Q = V[(T + 1) % 4];
			const int32 R = V[(T + 2) % 4];
			const int32 S = V[(T + 3) % 4];
			auto Across = [P, Q, R, S](int32 X) { return X == P ? S : (X == Q ? R : INDEX_NONE); };

			const int32 A = Across(Line.Key);
			const int32 B = Across(Line.Value);
			if (A == INDEX_NONE || B == INDEX_NONE)
			{
				return false;
			}
			Out.Add(TPair<int32, int32>(A, B));
		}

		return Out.Num() >= 2;
	}
}

int32 USurfEdMode::FacesToPatches()
{
	/**
	 * The sides that are picked - which has meant the active one too since
	 * S145a (S148a).
	 *
	 * This asked `Selection` alone, and a box fresh from the bar has its
	 * top in `ActiveFace` and nothing in `Selection` at all: Sheet on it
	 * answered "pick the primitive faces to convert first" with a side plainly
	 * lit. Every other verb on a side had already been moved to this rule;
	 * this one was missed.
	 */
	const TArray<FSurfFaceRef> Picked = PickedFaces();
	if (Picked.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("to patches: pick the primitive faces to convert first"));
		}
		SurfSession::Friction(TEXT("to_patches_nothing"));
		return 0;
	}

	FScopedTransaction Transaction(LOCTEXT("FacesToPatches", "Faces to Patches"));

	TArray<FSurfPatchRef> Made;
	TArray<ASurfGroup*> Groups;
	int32 Fitted = 0;
	int32 Folded = 0;

	// The picked sides, gathered per brush, because sides that touch along
	// an edge become one patch and only sides of the same brush can touch.
	struct FBrushPick
	{
		TWeakObjectPtr<ASurfGroup> Group;
		FGuid BrushId;
		TArray<int32> FaceIndices;
	};
	TArray<FBrushPick> ByBrush;
	for (const FSurfFaceRef& Ref : Picked)
	{
		if (!Ref.Group.IsValid())
		{
			continue;
		}
		FBrushPick* Found = ByBrush.FindByPredicate([&Ref](const FBrushPick& Each)
		{
			return Each.Group == Ref.Group && Each.BrushId == Ref.BrushId;
		});
		if (!Found)
		{
			Found = &ByBrush.AddDefaulted_GetRef();
			Found->Group = Ref.Group;
			Found->BrushId = Ref.BrushId;
		}
		Found->FaceIndices.AddUnique(Ref.FaceIndex);
	}

	for (const FBrushPick& Pick : ByBrush)
	{
		ASurfGroup* Group = Pick.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Pick.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}

		// The sides copied out before anything is made: adding a patch can
		// move the chunk the brush lives in, and the loop below makes
		// several. The old shape re-found the brush for every face, which
		// hid this; hoisting the lookup without hoisting the data would
		// have left a stale pointer on the second patch.
		const TArray<FSurfFace> Sides = Brush->Faces;

		// The polygon each picked side actually bounds - not the brush's
		// box, which is what To Sheet used and why one box became one sheet.
		TArray<const FSurfHullFace*> Polys;
		TArray<int32> Sources;
		for (const int32 FaceIndex : Pick.FaceIndices)
		{
			if (!Sides.IsValidIndex(FaceIndex))
			{
				continue;
			}
			const FSurfHullFace* Bound = nullptr;
			for (const FSurfHullFace& HullFace : Hull.Faces)
			{
				if (HullFace.SourceFaceIndex == FaceIndex)
				{
					Bound = &HullFace;
					break;
				}
			}
			if (!Bound || Bound->VertexIndices.Num() < 3)
			{
				continue;
			}
			Polys.Add(Bound);
			Sources.Add(FaceIndex);
		}

		TArray<TArray<int32>> Runs;
		SurfFaceStrips::Chains(Polys, Runs);

		// One patch, made and taken into the level. The two roads below -
		// a run of sides folded into one strip, or a side on its own - meet
		// here, so the material, the surfacing and the bookkeeping are
		// written once.
		auto Keep = [&](FSurfPatch& Patch, int32 SourceFace, bool bExact)
		{
			Patch.Material = Sides[SourceFace].Material;
			Patch.EnsureId();
			SurfPatchSurfacing::Reapply(Patch);
			Fitted += bExact ? 0 : 1;
			const FGuid PatchId = Patch.Id;
			Group->AddPatch(MoveTemp(Patch));
			Groups.AddUnique(Group);

			FSurfPatchRef PatchRef;
			PatchRef.Group = Group;
			PatchRef.PatchId = PatchId;
			Made.Add(PatchRef);
		};

		/**
		 * One side on its own. Four corners are a two-by-two grid, exactly
		 * (S101a): every face used to become a grid over its own bounding
		 * box, which for a box's side is the same shape and for anything
		 * tilted is not - "tried sheet from selected face of a cylinder - it
		 * didnt recreate each face well". The hull's loop is wound about the
		 * outward normal, so taking it round - 0, 1, 2, 3 - and reading it
		 * as (0,0), (1,0), (1,1), (0,1) keeps the patch facing the way the
		 * face did.
		 */
		auto Alone = [&](int32 PolyIndex)
		{
			const FSurfHullFace* Bound = Polys[PolyIndex];
			FSurfPatch Patch;

			if (Bound->VertexIndices.Num() == 4)
			{
				Patch.Type = ESurfPatchType::Sheet;
				Patch.Width = 2;
				Patch.Height = 2;
				Patch.Points.SetNum(4);
				Patch.At(0, 0).Pos = Hull.Vertices[Bound->VertexIndices[0]];
				Patch.At(1, 0).Pos = Hull.Vertices[Bound->VertexIndices[1]];
				Patch.At(1, 1).Pos = Hull.Vertices[Bound->VertexIndices[2]];
				Patch.At(0, 1).Pos = Hull.Vertices[Bound->VertexIndices[3]];
				Keep(Patch, Sources[PolyIndex], /*bExact*/ true);
				return;
			}

			// More than four corners - a cylinder's cap, a sliced end. A
			// grid cannot be an n-gon, so it is fitted to the face's bounds
			// and the line below says so.
			FBox Bounds(ForceInit);
			for (const int32 Index : Bound->VertexIndices)
			{
				if (Hull.Vertices.IsValidIndex(Index))
				{
					Bounds += Hull.Vertices[Index];
				}
			}
			if (!Bounds.IsValid)
			{
				return;
			}
			const FPlane& Plane = Sides[Sources[PolyIndex]].Plane;
			Patch = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Simple, Bounds, 3, 3,
				FVector(Plane.X, Plane.Y, Plane.Z));
			Keep(Patch, Sources[PolyIndex], /*bExact*/ false);
		};

		for (const TArray<int32>& Run : Runs)
		{
			if (Run.Num() == 0)
			{
				continue;
			}

			/**
			 * Sides that touch become one patch (S148b): "i had selected
			 * two. which vertices are shared. so i was expecting a single
			 * patch out of it."
			 *
			 * A grid patch is rows of quads and its cells are flat, so a
			 * fold along a shared edge is just a row of the grid - two
			 * sides of a box are one sheet with a crease down the middle,
			 * four round it are a band. That is what surfacing a blockout
			 * wants: one surface to model, not one per side.
			 */
			TArray<TPair<int32, int32>> Lines;
			if (Run.Num() > 1
				&& SurfFaceStrips::Lines(Polys, Run, Lines)
				&& Lines.Num() >= 2
				&& Lines.Num() <= FSurfPatch::MaxSheetDim)
			{
				FSurfPatch Patch;
				Patch.Type = ESurfPatchType::Sheet;
				Patch.Width = 2;
				Patch.Height = Lines.Num();
				Patch.Points.SetNum(2 * Lines.Num());
				for (int32 Y = 0; Y < Lines.Num(); ++Y)
				{
					Patch.At(0, Y).Pos = Hull.Vertices[Lines[Y].Key];
					Patch.At(1, Y).Pos = Hull.Vertices[Lines[Y].Value];
				}
				Keep(Patch, Sources[Run[0]], /*bExact*/ true);
				++Folded;
				continue;
			}

			// A run that will not lay out as a strip is made a side at a
			// time, as every side used to be.
			for (const int32 PolyIndex : Run)
			{
				Alone(PolyIndex);
			}
		}
	}

	if (Made.Num() == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("to patches: nothing was converted - the faces bound no polygon"));
		}
		return 0;
	}

	for (ASurfGroup* Group : Groups)
	{
		Group->RebuildPreview();
	}
	if (Inspector && Fitted > 0)
	{
		Inspector->SetResult(FString::Printf(
			TEXT("to patches: %d made, the primitives kept - %d had more than four corners and were fitted to the face's bounds"),
			Made.Num(), Fitted));
	}

	// The verb states its selection (D16.1): the patches it made, and the
	// brushes are exactly as they were.
	ClearSelection();
	ClearPointSelection();
	SelectMadePatches(Made);
	SetSubMode(ESurfSubMode::Object);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%d side%s became %d patch%s%s - the primitives are untouched; Delete the faces' primitives if they are in the way"),
			Picked.Num(), Picked.Num() == 1 ? TEXT("") : TEXT("s"),
			Made.Num(), Made.Num() == 1 ? TEXT("") : TEXT("es"),
			Folded > 0 ? TEXT(", sides that touch folded into one") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("faces_to_patches"), FString::Printf(TEXT("sides=%d patches=%d folded=%d"),
		Picked.Num(), Made.Num(), Folded));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Made.Num();
}

int32 USurfEdMode::SweepEmptyGroups()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	TArray<ASurfGroup*> Empty;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!IsValid(Group) || Group->NumPrimitives() > 0)
		{
			continue;
		}
		// A room being edited is empty for as long as it takes to put the
		// first wall in it; a baked group stands behind meshes that point
		// at it; a transient one is not the level's to tidy.
		if (Group->bBaked || Group->HasAnyFlags(RF_Transient) || Group == GetDrilledGroup())
		{
			continue;
		}
		if (Group->GetLevel() != World->GetCurrentLevel())
		{
			continue;
		}
		Empty.Add(Group);
	}

	for (ASurfGroup* Group : Empty)
	{
		if (LastGroupUsed.Get() == Group)
		{
			LastGroupUsed = nullptr;
		}
		Group->Destroy();
	}

	if (Empty.Num() > 0)
	{
		SurfSession::Verb(TEXT("empty_groups_swept"), FString::FromInt(Empty.Num()));
	}
	return Empty.Num();
}

int32 USurfEdMode::MergeLevelGroups()
{
	UWorld* World = GetWorld();
	if (!World || IsInsidePrefab() || IsInsideScene())
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("merge containers: come out to the level first"));
		}
		SurfSession::Friction(TEXT("merge_groups_inside"));
		return 0;
	}

	// Everything of this level's that could be folded, oldest first, so the
	// answer does not depend on the order actors happen to be iterated in.
	TArray<ASurfGroup*> Found;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!IsValid(Group) || Group->GetLevel() != World->GetCurrentLevel())
		{
			continue;
		}
		// A baked group stands behind static meshes that point at it; a
		// group being edited as a prefab's room is not the level's.
		if (Group->bBaked || Group->HasAnyFlags(RF_Transient) || Group == GetDrilledGroup())
		{
			continue;
		}
		Found.Add(Group);
	}
	Found.Sort([](const ASurfGroup& A, const ASurfGroup& B) { return A.GroupId < B.GroupId; });

	if (Found.Num() < 2)
	{
		if (Inspector)
		{
			Inspector->SetResult(Found.Num() == 1
				? TEXT("merge containers: there is only one already")
				: TEXT("merge containers: this level has none"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("merge_groups"), FString::Printf(TEXT("groups=%d folded=0"), Found.Num()));
		return 0;
	}

	// The one that keeps: a marked group if there is one, else the oldest.
	ASurfGroup* Keep = nullptr;
	for (ASurfGroup* Group : Found)
	{
		if (Group->bLevelGroup)
		{
			Keep = Group;
			break;
		}
	}
	if (!Keep)
	{
		Keep = Found[0];
	}

	FScopedTransaction Transaction(LOCTEXT("MergeLevelContainers", "Merge Containers"));

	ClearPointSelection();
	ClearSelection();
	SelectPatch(FSurfPatchRef());

	Keep->Modify();
	Keep->bLevelGroup = true;

	const FTransform ToKeep = Keep->GetActorTransform();
	int32 Folded = 0;
	int32 Moved = 0;

	for (ASurfGroup* Group : Found)
	{
		if (Group == Keep)
		{
			continue;
		}

		Group->Modify();

		const FTransform FromGroup = Group->GetActorTransform();
		const FMatrix Between = FromGroup.ToMatrixWithScale() * ToKeep.Inverse().ToMatrixWithScale();

		TArray<FSurfPatch> Patches;
		Group->ForEachPatchIncludingHidden([&Patches](const FSurfPatch& Patch) { Patches.Add(Patch); });
		TArray<FSurfBrush> Brushes;
		Group->ForEachBrush([&Brushes](const FSurfBrush& Brush) { Brushes.Add(Brush); });

		for (FSurfPatch& Patch : Patches)
		{
			for (FSurfControlPoint& Point : Patch.Points)
			{
				Point.Pos = ToKeep.InverseTransformPosition(FromGroup.TransformPosition(Point.Pos));
			}
			Patch.Id = FGuid::NewGuid();
			SurfPatchSurfacing::Reapply(Patch);
			Keep->AddPatch(MoveTemp(Patch));
			++Moved;
		}

		for (FSurfBrush& Brush : Brushes)
		{
			for (FSurfFace& Face : Brush.Faces)
			{
				Face.Plane = Face.Plane.TransformBy(Between);
			}
			Brush.Id = FGuid::NewGuid();
			Keep->AddBrush(MoveTemp(Brush));
			++Moved;
		}

		Group->Destroy();
		++Folded;
	}

	Keep->RebuildPreview();
	LastGroupUsed = Keep;

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(
			TEXT("%d container%s folded into one - %d piece%s moved, and nothing shifted"),
			Folded, Folded == 1 ? TEXT("") : TEXT("s"), Moved, Moved == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("merge_groups"), FString::Printf(TEXT("groups=%d folded=%d pieces=%d"),
		Found.Num(), Folded, Moved));

	if (GEditor)
	{
		RedrawViewports();
	}
	return Folded;
}

namespace
{
	const TCHAR* FlagName(ESurfPrimitiveFlags Flag)
	{
		switch (Flag)
		{
		case ESurfPrimitiveFlags::Detail:       return TEXT("detail");
		case ESurfPrimitiveFlags::NonColliding: return TEXT("no collision");
		case ESurfPrimitiveFlags::NoShadow:     return TEXT("no shadow");
		case ESurfPrimitiveFlags::NoDraw:       return TEXT("no draw");
		default:                                return TEXT("?");
		}
	}
}

void USurfEdMode::PiecesPicked(TArray<FSurfPatchRef>& OutPatches, TArray<FSurfFaceRef>& OutBrushes) const
{
	OutPatches.Reset();
	OutBrushes = BrushesHeldBySelection();

	// Picked as wholes - the list itself, never its fallback to the active
	// patch.
	for (const FSurfPatchRef& Ref : PatchSelection)
	{
		if (Ref.IsValid())
		{
			OutPatches.AddUnique(Ref);
		}
	}
	// Under the picked points and faces.
	for (const FSurfPointRef& Point : PointSelection)
	{
		FSurfPatchRef Ref;
		Ref.Group = Point.Group;
		Ref.PatchId = Point.PatchId;
		if (Ref.IsValid())
		{
			OutPatches.AddUnique(Ref);
		}
	}
	for (const FSurfQuadRef& Quad : QuadSelection)
	{
		FSurfPatchRef Ref;
		Ref.Group = Quad.Group;
		Ref.PatchId = Quad.PatchId;
		if (Ref.IsValid())
		{
			OutPatches.AddUnique(Ref);
		}
	}
	// Never the patch left active from a minute ago (S154a): with nothing
	// picked, the Flags window wrote to it - "i think it said 1 piece?" -
	// and the box he meant kept its flag. A patch just made from the bar
	// is in PatchSelection already.
}

int32 USurfEdMode::PieceFlagState(ESurfPrimitiveFlags Flag) const
{
	const int32 Bit = static_cast<int32>(Flag);
	int32 On = 0;
	int32 Off = 0;

	TArray<FSurfPatchRef> Patches;
	TArray<FSurfFaceRef> Brushes;
	PiecesPicked(Patches, Brushes);
	for (const FSurfPatchRef& Ref : Patches)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch)
		{
			((Patch->Flags & Bit) ? On : Off) += 1;
		}
	}
	for (const FSurfFaceRef& Ref : Brushes)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (Brush)
		{
			((Brush->Flags & Bit) ? On : Off) += 1;
		}
	}
	if (On == 0 && Off == 0) { return 0; }
	if (On > 0 && Off > 0) { return -1; }
	return On > 0 ? 1 : 0;
}

int32 USurfEdMode::TogglePieceFlag(ESurfPrimitiveFlags Flag)
{
	return SetPieceFlag(Flag, PieceFlagState(Flag) != 1);
}

int32 USurfEdMode::SetPieceFlag(ESurfPrimitiveFlags Flag, bool bOn)
{
	const int32 Bit = static_cast<int32>(Flag);
	const bool bPutOn = bOn;

	TArray<FSurfPatchRef> Patches;
	TArray<FSurfFaceRef> Brushes;
	PiecesPicked(Patches, Brushes);
	if (Patches.Num() == 0 && Brushes.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s: pick a patch or a primitive first"), FlagName(Flag)));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("piece_flag_nothing"));
		return 0;
	}

	FScopedTransaction Transaction(LOCTEXT("PieceFlag", "Piece Flag"));
	int32 Touched = 0;
	TArray<ASurfGroup*> Groups;

	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		Patch->Flags = bPutOn ? (Patch->Flags | Bit) : (Patch->Flags & ~Bit);
		Groups.AddUnique(Group);
		++Touched;
	}
	for (const FSurfFaceRef& Ref : Brushes)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		Brush->Flags = bPutOn ? (Brush->Flags | Bit) : (Brush->Flags & ~Bit);
		Groups.AddUnique(Group);
		++Touched;
	}

	// The flags are the bake's, not the preview's - but a change is an edit,
	// and an edit un-bakes (D75). Modify() on the chunk did that.
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s %s on %d piece%s - now: %s"),
			FlagName(Flag), bPutOn ? TEXT("on") : TEXT("off"), Touched, Touched == 1 ? TEXT("") : TEXT("s"), *PieceFlagsLine()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("piece_flag"), FString::Printf(TEXT("%s=%d pieces=%d patches=%d brushes=%d"), FlagName(Flag), bPutOn ? 1 : 0, Touched, Patches.Num(), Brushes.Num()));
	return Touched;
}

TArray<FString> USurfEdMode::PieceFlagLines() const
{
	TArray<FString> Lines;
	auto Worn = [](int32 Flags) -> FString
	{
		TArray<FString> Names;
		for (const ESurfPrimitiveFlags Flag : { ESurfPrimitiveFlags::Detail, ESurfPrimitiveFlags::NonColliding,
			ESurfPrimitiveFlags::NoShadow, ESurfPrimitiveFlags::NoDraw })
		{
			if (Flags & static_cast<int32>(Flag)) { Names.Add(FlagName(Flag)); }
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("no flags"));
	};
	TArray<FSurfPatchRef> Patches;
	TArray<FSurfFaceRef> Brushes;
	PiecesPicked(Patches, Brushes);
	for (const FSurfFaceRef& Ref : Brushes)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (Brush)
		{
			Lines.Add(FString::Printf(TEXT("%s, %d sides: %s"), Brush->Faces.Num() == 6 ? TEXT("box") : TEXT("solid"), Brush->Faces.Num(), *Worn(Brush->Flags)));
		}
	}
	for (const FSurfPatchRef& Ref : Patches)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch)
		{
			Lines.Add(FString::Printf(TEXT("patch %dx%d: %s"), Patch->Width, Patch->Height, *Worn(Patch->Flags)));
		}
	}
	return Lines;
}

TArray<ASurfPrefabInstance*> USurfEdMode::PlacementsPicked() const
{
	TArray<ASurfPrefabInstance*> Out;
	if (GEditor)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (ASurfPrefabInstance* Instance = Cast<ASurfPrefabInstance>(*It))
			{
				Out.Add(Instance);
			}
		}
	}
	return Out;
}

int32 USurfEdMode::PlacementOverrideState() const
{
	const TArray<ASurfPrefabInstance*> Picked = PlacementsPicked();
	int32 On = 0;
	for (const ASurfPrefabInstance* Instance : Picked)
	{
		On += Instance->bOverrideFlags ? 1 : 0;
	}
	return Picked.Num() == 0 || On == 0 ? 0 : (On == Picked.Num() ? 1 : -1);
}

void USurfEdMode::SetPlacementOverride(bool bOn)
{
	const TArray<ASurfPrefabInstance*> Picked = PlacementsPicked();
	if (Picked.Num() == 0)
	{
		return;
	}
	const FScopedTransaction Transaction(bOn ? LOCTEXT("PlacementOverride", "Override Prefab Flags") : LOCTEXT("PlacementOverrideOff", "Cancel Prefab Flags Override"));
	for (ASurfPrefabInstance* Instance : Picked)
	{
		Instance->Modify();
		Instance->bOverrideFlags = bOn;
		// Its bake wore the old flags: it goes, the next bake wears these.
		Instance->Unbake();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s on %d placed prefab%s"), bOn ? TEXT("flags overridden") : TEXT("override cancelled - its pieces' own flags again"),
			Picked.Num(), Picked.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	SurfSession::Verb(TEXT("placement_override"), FString::Printf(TEXT("%d placements=%d"), bOn ? 1 : 0, Picked.Num()));
}

int32 USurfEdMode::PlacementPaintOverrideState() const
{
	const TArray<ASurfPrefabInstance*> Picked = PlacementsPicked();
	int32 On = 0;
	for (const ASurfPrefabInstance* Instance : Picked)
	{
		On += Instance->bOverridePaint ? 1 : 0;
	}
	return Picked.Num() == 0 || On == 0 ? 0 : (On == Picked.Num() ? 1 : -1);
}

void USurfEdMode::SetPlacementPaintOverride(bool bOn)
{
	const TArray<ASurfPrefabInstance*> Picked = PlacementsPicked();
	if (Picked.Num() == 0)
	{
		return;
	}
	const FScopedTransaction Transaction(bOn ? LOCTEXT("PlacementPaintOverride", "Paint Override") : LOCTEXT("PlacementPaintOverrideOff", "Cancel Paint Override"));
	for (ASurfPrefabInstance* Instance : Picked)
	{
		Instance->Modify();
		Instance->bOverridePaint = bOn;
		if (!bOn)
		{
			Instance->PaintOverrides.Reset();
		}
		Instance->Unbake();
		Instance->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s on %d placed prefab%s"),
			bOn ? TEXT("paint override - Shift+B paints it where it stands, from the prefab's paint") : TEXT("paint override cancelled - the prefab's paint again"),
			Picked.Num(), Picked.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	SurfSession::Verb(TEXT("placement_paint_override"), FString::Printf(TEXT("%d placements=%d"), bOn ? 1 : 0, Picked.Num()));
}

void USurfEdMode::PaintInstanceTargets(TArray<ASurfPrefabInstance*>& Out) const
{
	Out.Reset();
	UWorld* World = GetWorld();
	if (!World || IsInsidePrefab() || IsInsideScene())
	{
		return;
	}
	const TArray<ASurfPrefabInstance*> Picked = PlacementsPicked();
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		ASurfPrefabInstance* Instance = *It;
		if (!Instance || !Instance->bOverridePaint || !Instance->Prefab || Instance->bGhosted || Instance->IsTemporarilyHiddenInEditor())
		{
			continue;
		}
		if (Picked.Num() > 0 && !Picked.Contains(Instance))
		{
			continue;
		}
		Out.Add(Instance);
	}
}

bool USurfEdMode::GetCursor(EMouseCursor::Type& OutCursor) const
{
	if (bPaintArmed && bPaintHoverBlocked)
	{
		OutCursor = EMouseCursor::Crosshairs;
		return true;
	}
	return false;
}

int32 USurfEdMode::PlacementFlagState(ESurfPrimitiveFlags Flag) const
{
	const int32 Bit = static_cast<int32>(Flag);
	int32 On = 0;
	int32 Seen = 0;
	for (const ASurfPrefabInstance* Instance : PlacementsPicked())
	{
		if (!Instance->bOverrideFlags)
		{
			continue;
		}
		++Seen;
		On += (Instance->OverrideFlags & Bit) ? 1 : 0;
	}
	return Seen == 0 || On == 0 ? 0 : (On == Seen ? 1 : -1);
}

void USurfEdMode::SetPlacementFlag(ESurfPrimitiveFlags Flag, bool bOn)
{
	const int32 Bit = static_cast<int32>(Flag);
	const FScopedTransaction Transaction(LOCTEXT("PlacementFlag", "Prefab Flag"));
	int32 Touched = 0;
	for (ASurfPrefabInstance* Instance : PlacementsPicked())
	{
		if (!Instance->bOverrideFlags)
		{
			continue;
		}
		Instance->Modify();
		Instance->OverrideFlags = bOn ? (Instance->OverrideFlags | Bit) : (Instance->OverrideFlags & ~Bit);
		Instance->Unbake();
		++Touched;
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s %s on %d placed prefab%s"), FlagName(Flag), bOn ? TEXT("on") : TEXT("off"), Touched, Touched == 1 ? TEXT("") : TEXT("s")));
	}
	SurfSession::Verb(TEXT("placement_flag"), FString::Printf(TEXT("%s=%d placements=%d"), FlagName(Flag), bOn ? 1 : 0, Touched));
}

FString USurfEdMode::PieceFlagsLine() const
{
	TArray<FString> Worn;
	TArray<FString> Mixed;
	for (const ESurfPrimitiveFlags Flag : { ESurfPrimitiveFlags::Detail, ESurfPrimitiveFlags::NonColliding,
		ESurfPrimitiveFlags::NoShadow, ESurfPrimitiveFlags::NoDraw })
	{
		const int32 State = PieceFlagState(Flag);
		if (State == 1) { Worn.Add(FlagName(Flag)); }
		else if (State == -1) { Mixed.Add(FlagName(Flag)); }
	}
	FString Line = Worn.Num() > 0 ? FString::Join(Worn, TEXT(", ")) : TEXT("no flags");
	if (Mixed.Num() > 0)
	{
		Line += FString::Printf(TEXT(" (%s on some)"), *FString::Join(Mixed, TEXT(", ")));
	}
	return Line;
}

namespace
{
	/** The actors picked in the editor's own selection. */
	TArray<AActor*> StripActors()
	{
		TArray<AActor*> Out;
		if (GEditor)
		{
			if (USelection* Selected = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*Selected); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
					{
						Out.Add(Actor);
					}
				}
			}
		}
		return Out;
	}
}

bool USurfEdMode::GetStripTransform(FVector& OutPosition, FRotator& OutRotation, FVector& OutScale, bool& bOutAbsolute) const
{
	const bool bOurs = PointSelection.Num() > 0 || Selection.Num() > 0 || ActiveFace.IsValid() || SelectedPatch.IsValid() || PatchSelection.Num() > 0;
	if (bOurs)
	{
		OutPosition = GetWidgetLocation();
		OutRotation = FRotator::ZeroRotator;
		OutScale = FVector::OneVector;
		bOutAbsolute = false;
		return true;
	}
	const TArray<AActor*> Actors = StripActors();
	if (Actors.Num() > 0)
	{
		const FTransform& Transform = Actors[0]->GetActorTransform();
		OutPosition = Transform.GetLocation();
		OutRotation = Transform.Rotator();
		OutScale = Transform.GetScale3D();
		bOutAbsolute = true;
		return true;
	}
	return false;
}

bool USurfEdMode::ApplyStripTransform(const FVector* NewPosition, const FRotator* NewRotation, const FVector* NewScale)
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	FVector Position;
	FRotator Rotation;
	FVector Scale;
	bool bAbsolute = false;
	if (!GetStripTransform(Position, Rotation, Scale, bAbsolute))
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("transform: pick something first"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("StripTransform", "Transform"));

	// Actors: their own transform, set outright; several move together.
	if (bAbsolute)
	{
		const TArray<AActor*> Actors = StripActors();
		const FVector Move = NewPosition ? *NewPosition - Position : FVector::ZeroVector;
		for (AActor* Actor : Actors)
		{
			Actor->Modify();
			if (NewPosition)
			{
				Actor->SetActorLocation(Actor->GetActorLocation() + Move);
			}
			if (NewRotation)
			{
				Actor->SetActorRotation(*NewRotation);
			}
			if (NewScale)
			{
				Actor->SetActorScale3D(*NewScale);
			}
			Actor->PostEditMove(true);
		}
		SurfSession::Verb(TEXT("transform_strip"), FString::Printf(TEXT("actors=%d"), Actors.Num()));
		if (GEditor)
		{
			GEditor->NoteSelectionChange();
			RedrawViewports();
		}
		return Actors.Num() > 0;
	}

	// Ours: one matrix about the pivot - the centre unless one is set
	// (S177a) - size, turn, then to the new place.
	const FVector Centre = Position;
	const FVector Target = NewPosition ? *NewPosition : Centre;
	const FVector About = PivotOr(Centre);
	const FMatrix World = FTranslationMatrix(-About)
		* FScaleMatrix(NewScale ? *NewScale : FVector::OneVector)
		* FRotationMatrix(NewRotation ? *NewRotation : FRotator::ZeroRotator)
		* FTranslationMatrix(About + (Target - Centre));

	TArray<ASurfGroup*> Touched;
	auto Touch = [&Touched](ASurfGroup* Group)
	{
		if (!Touched.Contains(Group))
		{
			Group->Modify();
			Touched.Add(Group);
		}
	};
	auto MovePoint = [&World](const FTransform& ToWorld, FVector& Local)
	{
		Local = ToWorld.InverseTransformPosition(World.TransformPosition(ToWorld.TransformPosition(Local)));
	};

	int32 Moved = 0;
	TSet<TPair<ASurfGroup*, FGuid>> PatchesDone;
	if (PointSelection.Num() > 0)
	{
		for (const FSurfPointRef& Ref : PointSelection)
		{
			ASurfGroup* Group = Ref.Group.Get();
			if (!Group)
			{
				continue;
			}
			Touch(Group);
			FSurfPatch* Patch = Group->ModifyPatch(Ref.PatchId);
			if (!Patch || Ref.X < 0 || Ref.Y < 0 || Ref.X >= Patch->Width || Ref.Y >= Patch->Height)
			{
				continue;
			}
			MovePoint(Group->GetActorTransform(), Patch->At(Ref.X, Ref.Y).Pos);
			PatchesDone.Add(TPair<ASurfGroup*, FGuid>(Group, Ref.PatchId));
			++Moved;
		}
	}
	else
	{
		// Whole patches, picked in Object.
		TArray<FSurfPatchRef> Patches = GetSelectedPatches();
		for (const FSurfPatchRef& Held : PatchesHeldBySelection())
		{
			Patches.AddUnique(Held);
		}
		for (const FSurfPatchRef& Ref : Patches)
		{
			ASurfGroup* Group = Ref.Group.Get();
			if (!Group)
			{
				continue;
			}
			Touch(Group);
			FSurfPatch* Patch = Group->ModifyPatch(Ref.PatchId);
			if (!Patch)
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			for (FSurfControlPoint& Point : Patch->Points)
			{
				MovePoint(ToWorld, Point.Pos);
			}
			PatchesDone.Add(TPair<ASurfGroup*, FGuid>(Group, Ref.PatchId));
			++Moved;
		}
	}
	for (const TPair<ASurfGroup*, FGuid>& Each : PatchesDone)
	{
		if (FSurfPatch* Patch = Each.Key->ModifyPatch(Each.Value))
		{
			SurfPatchSurfacing::Reapply(*Patch);
			++Patch->EditSerial;
		}
	}

	// Primitives: in Object the whole of each, in Edit the picked sides.
	{
		TMap<TPair<ASurfGroup*, FGuid>, TArray<int32>> Sides;
		TArray<FSurfFaceRef> Picked = Selection;
		if (ActiveFace.IsValid())
		{
			Picked.AddUnique(ActiveFace);
		}
		for (const FSurfFaceRef& Ref : Picked)
		{
			if (ASurfGroup* Group = Ref.Group.Get())
			{
				Sides.FindOrAdd(TPair<ASurfGroup*, FGuid>(Group, Ref.BrushId)).AddUnique(Ref.FaceIndex);
			}
		}
		for (const TPair<TPair<ASurfGroup*, FGuid>, TArray<int32>>& Each : Sides)
		{
			ASurfGroup* Group = Each.Key.Key;
			Touch(Group);
			FSurfBrush* Brush = Group->ModifyBrush(Each.Key.Value);
			if (!Brush)
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			const FMatrix Local = ToWorld.ToMatrixWithScale() * World * ToWorld.ToMatrixWithScale().Inverse();
			const TArray<FSurfFace> Was = Brush->Faces;
			const bool bWhole = SubMode == ESurfSubMode::Object || Each.Value.Num() >= Brush->Faces.Num();
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				if (bWhole || Each.Value.Contains(I))
				{
					Brush->Faces[I].Plane = Brush->Faces[I].Plane.TransformBy(Local);
				}
			}
			FSurfHull Check;
			if (!SurfBrushMath::DeriveHull(*Brush, Check))
			{
				Brush->Faces = Was;
				continue;
			}
			++Brush->EditSerial;
			++Moved;
		}
	}

	if (Moved == 0)
	{
		Transaction.Cancel();
		return false;
	}
	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}
	UpdateOrbitPivot();
	SurfSession::Verb(TEXT("transform_strip"), FString::Printf(TEXT("pieces=%d"), Moved));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::CanSetPivot() const
{
	// Ours only (S181a): "not so wise to be able to edit the origin of a
	// prefab... not sure we can set origin to anything else than prim and
	// patch" - an actor, a placed prefab, keeps its own.
	FVector Position;
	FRotator Rotation;
	FVector Scale;
	bool bAbsolute = false;
	return GetStripTransform(Position, Rotation, Scale, bAbsolute) && !bAbsolute;
}

bool USurfEdMode::SetPivotFromSelection()
{
	FVector Centre;
	FRotator Rotation;
	FVector Scale;
	bool bAbsolute = false;
	if (!GetStripTransform(Centre, Rotation, Scale, bAbsolute) || bAbsolute)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("origin: pick vertices, edges, faces, a patch or a primitive"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}
	SetPivot(Centre);
	return true;
}

void USurfEdMode::SetPivot(const FVector& World)
{
	bPivotSet = true;
	PivotWorld = World;
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("origin at %.1f %.1f %.1f: rotation and scale go about it - right-click, Origin back to the centre"), World.X, World.Y, World.Z));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("pivot"), FString::Printf(TEXT("set %.1f %.1f %.1f"), World.X, World.Y, World.Z));
	RedrawViewports();
}

void USurfEdMode::ClearPivot()
{
	if (!bPivotSet)
	{
		return;
	}
	bPivotSet = false;
	if (Inspector)
	{
		Inspector->SetResult(TEXT("origin back at the centre of the selection"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("pivot"), TEXT("centre"));
	RedrawViewports();
}

void USurfEdMode::DrawPivot(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	if (!bPivotSet || !View || !PDI)
	{
		return;
	}
	const FVector4 Screen = View->WorldToScreen(PivotWorld);
	if (Screen.W <= 0.0)
	{
		return;
	}
	// A pixel's worth of the world at the pivot, so the mark is the same size on screen anywhere.
	const double PerPixel = Screen.W * (2.0 / FMath::Max(1, View->UnscaledViewRect.Width()) / View->ViewMatrices.GetViewToClip().M[0][0]);
	const double Radius = PerPixel * 8.0;
	const FVector Right = View->GetViewRight();
	const FVector Up = View->GetViewUp();
	const FLinearColor Colour(1.0f, 0.55f, 0.12f, 1.0f);
	constexpr int32 Sides = 24;
	for (int32 I = 0; I < Sides; ++I)
	{
		const double A0 = UE_TWO_PI * I / Sides;
		const double A1 = UE_TWO_PI * (I + 1) / Sides;
		PDI->DrawLine(PivotWorld + (Right * FMath::Cos(A0) + Up * FMath::Sin(A0)) * Radius,
			PivotWorld + (Right * FMath::Cos(A1) + Up * FMath::Sin(A1)) * Radius, Colour, SDPG_Foreground, 1.5f, 0.0f, true);
	}
	const FVector Ticks[4] = { Right, -Right, Up, -Up };
	for (const FVector& Tick : Ticks)
	{
		PDI->DrawLine(PivotWorld + Tick * Radius * 0.45, PivotWorld + Tick * Radius * 1.6, Colour, SDPG_Foreground, 1.5f, 0.0f, true);
	}
}

int32 USurfEdMode::SelectLinkedPatches()
{
	TArray<FSurfPatchRef> Start = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Start.AddUnique(Held);
	}
	if (Start.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("linked: pick a patch first"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("linked_nothing_selected"));
		return 0;
	}

	// Every visible patch's points, in world space, on a coarse hash so a
	// shared point is found without comparing every pair.
	struct FEntry { FSurfPatchRef Ref; TArray<FVector> Points; };
	TArray<FEntry> All;
	for (const FSurfPatchRef& Ref : GetAllPatches())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		FEntry Entry;
		Entry.Ref = Ref;
		const FTransform ToWorld = Group->GetActorTransform();
		// The border is where patches meet; the inside cannot share a point
		// with another patch that the border does not also share.
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				if (X == 0 || Y == 0 || X == Patch->Width - 1 || Y == Patch->Height - 1)
				{
					Entry.Points.Add(ToWorld.TransformPosition(Patch->At(X, Y).Pos));
				}
			}
		}
		All.Add(MoveTemp(Entry));
	}
	constexpr double Tolerance = 0.5;
	auto Cell = [](const FVector& P) { return FIntVector(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y), FMath::FloorToInt(P.Z)); };
	TMap<FIntVector, TArray<int32>> Grid;
	for (int32 I = 0; I < All.Num(); ++I)
	{
		for (const FVector& P : All[I].Points)
		{
			Grid.FindOrAdd(Cell(P)).AddUnique(I);
		}
	}

	TSet<int32> Reached;
	TArray<int32> Queue;
	for (int32 I = 0; I < All.Num(); ++I)
	{
		if (Start.Contains(All[I].Ref))
		{
			Reached.Add(I);
			Queue.Add(I);
		}
	}
	while (Queue.Num() > 0)
	{
		const int32 I = Queue.Pop(EAllowShrinking::No);
		for (const FVector& P : All[I].Points)
		{
			const FIntVector C = Cell(P);
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DZ = -1; DZ <= 1; ++DZ)
					{
						const TArray<int32>* There = Grid.Find(C + FIntVector(DX, DY, DZ));
						if (!There)
						{
							continue;
						}
						for (const int32 J : *There)
						{
							if (Reached.Contains(J))
							{
								continue;
							}
							for (const FVector& Q : All[J].Points)
							{
								if (FVector::DistSquared(P, Q) <= Tolerance * Tolerance)
								{
									Reached.Add(J);
									Queue.Add(J);
									break;
								}
							}
						}
					}
				}
			}
		}
	}

	const int32 Added = Reached.Num() - Start.Num();
	ClearPointSelection();
	PatchSelection.Reset();
	for (const int32 I : Reached)
	{
		PatchSelection.Add(All[I].Ref);
	}
	if (PatchSelection.Num() > 0)
	{
		SelectedPatch = PatchSelection.Last();
	}
	if (SubMode != ESurfSubMode::Object)
	{
		// Whole patches are what was asked for.
		SetSubMode(ESurfSubMode::Object);
	}
	UpdateOrbitPivot();
	if (Inspector)
	{
		Inspector->SetResult(Added > 0
			? FString::Printf(TEXT("linked: %d more patch%s joined through a shared point - %d in all"), Added, Added == 1 ? TEXT("") : TEXT("es"), PatchSelection.Num())
			: FString(TEXT("linked: no other patch shares a point with these")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Selection(FString::Printf(TEXT("select_linked added=%d patches=%d"), Added, PatchSelection.Num()));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Added;
}

int32 USurfEdMode::SetShadingOnSelection(int32 Kind, double AngleDeg)
{
	const double Angle = FMath::Clamp(AngleDeg, 1.0, 179.0);
	int32 Changed = 0;

	FScopedTransaction Transaction(LOCTEXT("SetShadingSelection", "Shading"));

	// The patches, through the inspector's own verb.
	const TArray<FSurfPatchRef> Patches = PatchesHeldBySelection();
	if (Patches.Num() > 0 && Inspector)
	{
		Inspector->Shading = Kind == 0 ? ESurfShading::Smooth : Kind == 1 ? ESurfShading::Faceted : ESurfShading::ByAngle;
		Inspector->SmoothingAngleDegrees = Angle;
		Inspector->ApplyShading();
		Changed += Patches.Num();
	}

	// The primitives: the crease angle the kind means.
	const float Crease = Kind == 0 ? 180.0f : Kind == 1 ? 0.0f : static_cast<float>(Angle);
	TArray<ASurfGroup*> Touched;
	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (!Group)
		{
			continue;
		}
		if (!Touched.Contains(Group))
		{
			Group->Modify();
			Touched.Add(Group);
		}
		if (FSurfBrush* Brush = Group->ModifyBrush(Ref.BrushId))
		{
			Brush->CreaseAngleDeg = Crease;
			++Brush->EditSerial;
			++Changed;
		}
	}
	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}

	if (Changed == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("shading: pick a patch or a primitive first"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("shading_nothing_selected"));
		return 0;
	}
	if (Inspector)
	{
		Inspector->SetResult(Kind == 0 ? FString::Printf(TEXT("shading: smooth on %d piece%s"), Changed, Changed == 1 ? TEXT("") : TEXT("s"))
			: Kind == 1 ? FString::Printf(TEXT("shading: hard on %d piece%s"), Changed, Changed == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(TEXT("shading: smooth under %.0f degrees on %d piece%s"), Angle, Changed, Changed == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("shading"), FString::Printf(TEXT("kind=%d angle=%.0f pieces=%d"), Kind, Angle, Changed));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Changed;
}

int32 USurfEdMode::ToggleCreaseOnSelectedLines()
{
	// The whole lines picked, per patch: a row is whole when every point
	// on it is picked, likewise a column. Only lines the surface passes
	// through count (even indices on a curve; any inner line on a sheet)
	// - a crease on a control line off the surface would show nowhere.
	struct FLines { TArray<int32> Rows; TArray<int32> Columns; };
	TMap<FSurfPatchRef, FLines> Picked;
	for (const FSurfPatchRef& Ref : PatchesHeldBySelection())
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		auto IsPicked = [&](int32 X, int32 Y)
		{
			FSurfPointRef P;
			P.Group = Ref.Group;
			P.PatchId = Ref.PatchId;
			P.X = X;
			P.Y = Y;
			return PointSelection.Contains(P);
		};
		const bool bSheet = Patch->Type == ESurfPatchType::Sheet;
		FLines Lines;
		for (int32 Y = 1; Y + 1 < Patch->Height; ++Y)
		{
			if (!bSheet && (Y % 2) != 0) { continue; }
			bool bWhole = true;
			for (int32 X = 0; X < Patch->Width && bWhole; ++X) { bWhole = IsPicked(X, Y); }
			if (bWhole) { Lines.Rows.Add(Y); }
		}
		for (int32 X = 1; X + 1 < Patch->Width; ++X)
		{
			if (!bSheet && (X % 2) != 0) { continue; }
			bool bWhole = true;
			for (int32 Y = 0; Y < Patch->Height && bWhole; ++Y) { bWhole = IsPicked(X, Y); }
			if (bWhole) { Lines.Columns.Add(X); }
		}
		if (Lines.Rows.Num() > 0 || Lines.Columns.Num() > 0)
		{
			Picked.Add(Ref, MoveTemp(Lines));
		}
	}
	if (Picked.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("crease: pick a whole inner line first (Alt-click a point in 1, on a line the surface passes through) - the border is hard already"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("crease_no_line"));
		return 0;
	}

	// Hard unless every picked line is hard already, in which case smooth.
	bool bAllHard = true;
	for (const TPair<FSurfPatchRef, FLines>& Each : Picked)
	{
		const ASurfGroup* Group = Each.Key.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Each.Key.PatchId) : nullptr;
		if (!Patch) { continue; }
		for (int32 Row : Each.Value.Rows) { bAllHard = bAllHard && Patch->HardRows.Contains(Row); }
		for (int32 Column : Each.Value.Columns) { bAllHard = bAllHard && Patch->HardColumns.Contains(Column); }
	}
	const bool bMakeHard = !bAllHard;

	FScopedTransaction Transaction(bMakeHard ? LOCTEXT("CreaseHard", "Crease") : LOCTEXT("CreaseSmooth", "Uncrease"));
	int32 Changed = 0;
	TArray<ASurfGroup*> Touched;
	for (const TPair<FSurfPatchRef, FLines>& Each : Picked)
	{
		ASurfGroup* Group = Each.Key.Group.Get();
		if (!Group) { continue; }
		if (!Touched.Contains(Group))
		{
			Group->Modify();
			Touched.Add(Group);
		}
		FSurfPatch* Patch = Group->ModifyPatch(Each.Key.PatchId);
		if (!Patch) { continue; }
		for (int32 Row : Each.Value.Rows)
		{
			if (bMakeHard) { Patch->HardRows.AddUnique(Row); } else { Patch->HardRows.Remove(Row); }
			++Changed;
		}
		for (int32 Column : Each.Value.Columns)
		{
			if (bMakeHard) { Patch->HardColumns.AddUnique(Column); } else { Patch->HardColumns.Remove(Column); }
			++Changed;
		}
		++Patch->EditSerial;
	}
	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(bMakeHard
			? FString::Printf(TEXT("crease: %d line%s hard - Crease again makes them smooth"), Changed, Changed == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(TEXT("crease: %d line%s smooth again"), Changed, Changed == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("crease"), FString::Printf(TEXT("hard=%d lines=%d"), bMakeHard ? 1 : 0, Changed));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Changed;
}

int32 USurfEdMode::GrowSelection(bool bOutward)
{
	const TCHAR* Name = bOutward ? TEXT("grow") : TEXT("shrink");
	if (PointSelection.Num() == 0 && QuadSelection.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s: pick some points or faces first"), Name));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("grow_nothing"));
		return 0;
	}

	int32 Changed = 0;
	if (SubMode == ESurfSubMode::Face)
	{
		// Cells: outward, every cell sharing an edge with a picked one;
		// inward, only the cells whose four neighbours are picked stay.
		TSet<FSurfQuadRef> Picked(QuadSelection);
		TArray<FSurfQuadRef> Next;
		auto Neighbour = [](const FSurfQuadRef& Q, int32 DX, int32 DY)
		{
			FSurfQuadRef N = Q;
			N.X += DX;
			N.Y += DY;
			return N;
		};
		if (bOutward)
		{
			Next = QuadSelection;
			for (const FSurfQuadRef& Quad : QuadSelection)
			{
				const ASurfGroup* Group = Quad.Group.Get();
				const FSurfPatch* Patch = Group ? Group->FindPatch(Quad.PatchId) : nullptr;
				if (!Patch)
				{
					continue;
				}
				for (const FIntPoint& D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const FSurfQuadRef N = Neighbour(Quad, D.X, D.Y);
					if (N.X >= 0 && N.Y >= 0 && N.X + 1 < Patch->Width && N.Y + 1 < Patch->Height && !Picked.Contains(N))
					{
						Picked.Add(N);
						Next.Add(N);
						++Changed;
					}
				}
			}
		}
		else
		{
			for (const FSurfQuadRef& Quad : QuadSelection)
			{
				const ASurfGroup* Group = Quad.Group.Get();
				const FSurfPatch* Patch = Group ? Group->FindPatch(Quad.PatchId) : nullptr;
				bool bInside = Patch != nullptr;
				for (const FIntPoint& D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const FSurfQuadRef N = Neighbour(Quad, D.X, D.Y);
					// A neighbour off the grid counts as unpicked: the border
					// of the patch is an edge of the selection too.
					const bool bOnGrid = Patch && N.X >= 0 && N.Y >= 0 && N.X + 1 < Patch->Width && N.Y + 1 < Patch->Height;
					if (!bOnGrid || !Picked.Contains(N))
					{
						bInside = false;
						break;
					}
				}
				if (bInside)
				{
					Next.Add(Quad);
				}
				else
				{
					++Changed;
				}
			}
			if (Next.Num() == 0)
			{
				if (Inspector)
				{
					Inspector->SetResult(TEXT("shrink: that would leave nothing, so nothing changed"));
					Inspector->RefreshSelectionCount();
				}
				return 0;
			}
		}
		QuadSelection = MoveTemp(Next);
		SyncQuadSelectionToPoints();
	}
	else
	{
		TSet<FSurfPointRef> Picked(PointSelection);
		TArray<FSurfPointRef> Next;
		auto Neighbour = [](const FSurfPointRef& P, int32 DX, int32 DY)
		{
			FSurfPointRef N = P;
			N.X += DX;
			N.Y += DY;
			return N;
		};
		if (bOutward)
		{
			Next = PointSelection;
			for (const FSurfPointRef& Ref : PointSelection)
			{
				const ASurfGroup* Group = Ref.Group.Get();
				const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
				if (!Patch)
				{
					continue;
				}
				for (const FIntPoint& D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const FSurfPointRef N = Neighbour(Ref, D.X, D.Y);
					if (N.X >= 0 && N.Y >= 0 && N.X < Patch->Width && N.Y < Patch->Height && !Picked.Contains(N))
					{
						Picked.Add(N);
						Next.Add(N);
						++Changed;
					}
				}
			}
		}
		else
		{
			for (const FSurfPointRef& Ref : PointSelection)
			{
				const ASurfGroup* Group = Ref.Group.Get();
				const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
				bool bInside = Patch != nullptr;
				for (const FIntPoint& D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const FSurfPointRef N = Neighbour(Ref, D.X, D.Y);
					const bool bOnGrid = Patch && N.X >= 0 && N.Y >= 0 && N.X < Patch->Width && N.Y < Patch->Height;
					// A neighbour off the grid does not count against a point:
					// a whole patch picked shrinks from its border inward.
					if (bOnGrid && !Picked.Contains(N))
					{
						bInside = false;
						break;
					}
				}
				if (bInside)
				{
					Next.Add(Ref);
				}
				else
				{
					++Changed;
				}
			}
			if (Next.Num() == 0)
			{
				if (Inspector)
				{
					Inspector->SetResult(TEXT("shrink: that would leave nothing, so nothing changed"));
					Inspector->RefreshSelectionCount();
				}
				return 0;
			}
		}
		PointSelection = MoveTemp(Next);
	}

	SyncPatchSelectionToPoints();
	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s: %d %s, %d picked"), Name, Changed,
			SubMode == ESurfSubMode::Face ? (bOutward ? TEXT("faces added") : TEXT("faces let go")) : (bOutward ? TEXT("points added") : TEXT("points let go")),
			SubMode == ESurfSubMode::Face ? QuadSelection.Num() : PointSelection.Num()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("grow_selection"), FString::Printf(TEXT("%s changed=%d"), Name, Changed));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Changed;
}

int32 USurfEdMode::ThinSelection(int32 Step)
{
	Step = FMath::Clamp(Step, 2, 8);

	if (PointSelection.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("every other: pick some points first"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("thin_nothing"));
		return 0;
	}

	const int32 Was = PointSelection.Num();

	// Counted along both ways at once, so on a grid it is a checker and on a
	// single row it is one in every Step. Into a list of its own first: the
	// test that went red here removed the points and then asked whether any
	// were left, which is the wrong way round to "leave it alone if none".
	TArray<FSurfPointRef> Kept;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (((Ref.X + Ref.Y) % Step) == 0)
		{
			Kept.Add(Ref);
		}
	}

	if (Kept.Num() == 0)
	{
		// Nothing left is not a selection, it is a mistake: nothing changes.
		if (Inspector)
		{
			Inspector->SetResult(TEXT("every other: that would leave nothing, so nothing changed"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("thin_everything"));
		return 0;
	}
	PointSelection = MoveTemp(Kept);

	if (SubMode == ESurfSubMode::Face)
	{
		SyncQuadSelectionToPoints();
	}
	SyncPatchSelectionToPoints();
	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("every %d: %d of %d points kept"),
			Step, PointSelection.Num(), Was));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("thin_selection"), FString::Printf(TEXT("step=%d %d -> %d"), Step, Was, PointSelection.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return PointSelection.Num();
}

int32 USurfEdMode::DensifyStretchedCells()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !Settings->bAutoSubdivideOnMove || PointSelection.Num() == 0)
	{
		return 0;
	}

	// 0 -> four times the mean, 0.5 -> the mean, 1 -> a quarter of it.
	const double Share = FMath::Clamp(Settings->AutoSubdivideDensity, 0.0, 1.0);
	const double Factor = FMath::Pow(4.0, 1.0 - 2.0 * Share);
	// A cell a little over is left: cutting it would leave two under, which
	// is not what "hold the density" means.
	const double Slack = 1.25;

	struct FOnPatch
	{
		FSurfPatchRef Ref;
		TArray<FSurfPointRef> Points;
	};
	TArray<FOnPatch> ByPatch;
	for (const FSurfPointRef& Point : PointSelection)
	{
		FOnPatch* Found = ByPatch.FindByPredicate([&Point](const FOnPatch& Each)
		{
			return Each.Ref.Group == Point.Group && Each.Ref.PatchId == Point.PatchId;
		});
		if (!Found)
		{
			Found = &ByPatch.AddDefaulted_GetRef();
			Found->Ref.Group = Point.Group;
			Found->Ref.PatchId = Point.PatchId;
		}
		Found->Points.Add(Point);
	}

	int32 Inserted = 0;
	TArray<ASurfGroup*> Touched;

	for (const FOnPatch& On : ByPatch)
	{
		ASurfGroup* Group = On.Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(On.Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions() || Patch->Type == ESurfPatchType::Curve)
		{
			continue;
		}

		// Which whole lines are in the selection.
		TArray<int32> PerColumn;
		PerColumn.SetNumZeroed(Patch->Width);
		TArray<int32> PerRow;
		PerRow.SetNumZeroed(Patch->Height);
		for (const FSurfPointRef& Point : On.Points)
		{
			if (Point.X >= 0 && Point.X < Patch->Width && Point.Y >= 0 && Point.Y < Patch->Height)
			{
				++PerColumn[Point.X];
				++PerRow[Point.Y];
			}
		}
		TArray<int32> Columns;
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			if (PerColumn[X] == Patch->Height) { Columns.Add(X); }
		}
		TArray<int32> Rows;
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			if (PerRow[Y] == Patch->Width) { Rows.Add(Y); }
		}
		// A whole patch picked is a move, not a stretch.
		if (Columns.Num() == Patch->Width) { Columns.Reset(); }
		if (Rows.Num() == Patch->Height) { Rows.Reset(); }
		if (Columns.Num() == 0 && Rows.Num() == 0)
		{
			continue;
		}

		auto ColumnWidth = [Patch](int32 C) -> double
		{
			double Sum = 0.0;
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				Sum += FVector::Dist(Patch->At(C, Y).Pos, Patch->At(C + 1, Y).Pos);
			}
			return Sum / Patch->Height;
		};
		auto RowHeight = [Patch](int32 R) -> double
		{
			double Sum = 0.0;
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				Sum += FVector::Dist(Patch->At(X, R).Pos, Patch->At(X, R + 1).Pos);
			}
			return Sum / Patch->Width;
		};

		// How far every line moved on: the selection follows.
		TArray<int32> ColumnShift;
		ColumnShift.SetNumZeroed(Patch->Width + 1);
		TArray<int32> RowShift;
		RowShift.SetNumZeroed(Patch->Height + 1);
		int32 Here = 0;

		// Columns: the mean over the cells that were not stretched, so the
		// stretched one does not pull its own target up.
		if (Columns.Num() > 0)
		{
			TArray<int32> Cells;
			for (const int32 X : Columns)
			{
				if (X - 1 >= 0) { Cells.AddUnique(X - 1); }
				if (X + 1 < Patch->Width) { Cells.AddUnique(X); }
			}
			double Sum = 0.0;
			int32 Count = 0;
			for (int32 C = 0; C + 1 < Patch->Width; ++C)
			{
				if (!Cells.Contains(C)) { Sum += ColumnWidth(C); ++Count; }
			}
			if (Count == 0)
			{
				for (int32 C = 0; C + 1 < Patch->Width; ++C) { Sum += ColumnWidth(C); ++Count; }
			}
			const double Target = Count > 0 ? (Sum / Count) * Factor : 0.0;

			// Highest cell first, so the indices below are still the ones
			// measured.
			Cells.Sort([](int32 A, int32 B) { return A > B; });
			for (const int32 C : Cells)
			{
				if (Target <= KINDA_SMALL_NUMBER) { break; }
				int32 Span = 1;
				for (int32 Guard = 0; Guard < 16; ++Guard)
				{
					int32 Widest = C;
					double Width = 0.0;
					for (int32 K = C; K < C + Span; ++K)
					{
						const double W = ColumnWidth(K);
						if (W > Width) { Width = W; Widest = K; }
					}
					if (Width <= Target * Slack || !SurfPatchPrimitives::InsertColumn(*Patch, Widest))
					{
						break;
					}
					++Span;
					++Here;
					// Every line past this cell moved on by one.
					for (int32 L = C + 1; L < ColumnShift.Num(); ++L) { ++ColumnShift[L]; }
				}
			}
		}

		if (Rows.Num() > 0)
		{
			TArray<int32> Cells;
			for (const int32 Y : Rows)
			{
				if (Y - 1 >= 0) { Cells.AddUnique(Y - 1); }
				if (Y + 1 < Patch->Height) { Cells.AddUnique(Y); }
			}
			double Sum = 0.0;
			int32 Count = 0;
			for (int32 R = 0; R + 1 < Patch->Height; ++R)
			{
				if (!Cells.Contains(R)) { Sum += RowHeight(R); ++Count; }
			}
			if (Count == 0)
			{
				for (int32 R = 0; R + 1 < Patch->Height; ++R) { Sum += RowHeight(R); ++Count; }
			}
			const double Target = Count > 0 ? (Sum / Count) * Factor : 0.0;

			Cells.Sort([](int32 A, int32 B) { return A > B; });
			for (const int32 R : Cells)
			{
				if (Target <= KINDA_SMALL_NUMBER) { break; }
				int32 Span = 1;
				for (int32 Guard = 0; Guard < 16; ++Guard)
				{
					int32 Tallest = R;
					double Height = 0.0;
					for (int32 K = R; K < R + Span; ++K)
					{
						const double H = RowHeight(K);
						if (H > Height) { Height = H; Tallest = K; }
					}
					if (Height <= Target * Slack || !SurfPatchPrimitives::InsertRow(*Patch, Tallest))
					{
						break;
					}
					++Span;
					++Here;
					for (int32 L = R + 1; L < RowShift.Num(); ++L) { ++RowShift[L]; }
				}
			}
		}

		if (Here == 0)
		{
			continue;
		}

		// The selection follows its line to where the line is now.
		for (FSurfPointRef& Point : PointSelection)
		{
			if (Point.Group == On.Ref.Group && Point.PatchId == On.Ref.PatchId)
			{
				const int32 OldX = Point.X;
				const int32 OldY = Point.Y;
				if (ColumnShift.IsValidIndex(OldX)) { Point.X = OldX + ColumnShift[OldX]; }
				if (RowShift.IsValidIndex(OldY)) { Point.Y = OldY + RowShift[OldY]; }
			}
		}

		++Patch->EditSerial;
		SurfPatchSurfacing::Reapply(*Patch);
		Touched.AddUnique(Group);
		Inserted += Here;
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}
	if (Inserted > 0)
	{
		if (SubMode == ESurfSubMode::Face)
		{
			SyncQuadSelectionToPoints();
		}
		RefreshSoftSelection();
		SurfSession::Verb(TEXT("auto_subdivide"), FString::Printf(TEXT("lines=%d density=%.2f"), Inserted, Share));
	}
	return Inserted;
}

bool USurfEdMode::SnapViewToNearestAxis()
{
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client)
	{
		return false;
	}
	if (!Camera)
	{
		Camera = MakeUnique<FSurfCamera>();
	}
	Camera->SnapToNearestAxis(Client);
	if (Inspector)
	{
		Inspector->SetResult(TEXT("view straightened to the nearest axis - Alt with the middle button does this any time"));
	}
	SurfSession::Verb(TEXT("view_axis"), TEXT("nearest"));
	return true;
}

bool USurfEdMode::SnapViewToDirection(const FVector& Direction, const FString& Named)
{
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client)
	{
		return false;
	}
	if (!Camera)
	{
		Camera = MakeUnique<FSurfCamera>();
	}
	Camera->SnapToDirection(Client, Direction);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s view"), *Named));
	}
	SurfSession::Verb(TEXT("view_axis"), Named);
	return true;
}

bool USurfEdMode::FocusAllViewports()
{
	// Every viewport at once, at once: the flat panes open on the level's
	// origin, far from the model, and the hand should not have to visit each.
	FBox Bounds(ForceInit);
	if (!BoundsToFrame(Bounds) || !GEditor)
	{
		return true;
	}

	int32 Framed = 0;
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (!Client)
		{
			continue;
		}
		Client->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent().GetMax() * 0.25 + 25.0), /*bInstant*/ true);
		++Framed;
	}
	if (FSurfCamera::IsBlenderScheme())
	{
		if (!Camera)
		{
			Camera = MakeUnique<FSurfCamera>();
		}
		Camera->SetPivot(Bounds.GetCenter());
	}

	SurfSession::Verb(TEXT("focus_all"), FString::Printf(TEXT("viewports=%d extent=%.0f"), Framed, Bounds.GetExtent().GetMax()));
	return true;
}

// --- Loop insertion -----------------------------------------------------------

namespace
{
	/**
	 * Which row or column the cursor is nearest. Shared by the preview and the
	 * commit, so the line drawn is the line cut.
	 */
	bool FindLoopUnderCursor(const USurfEdMode& Mode, FEditorViewportClient* ViewportClient, FViewport* Viewport,
		const FSurfPatch& Patch, const FTransform& ToWorld, bool& bOutColumn, int32& OutIndex)
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

		FIntPoint Cursor;
		Viewport->GetMousePos(Cursor);
		const FVector2D Mouse(Cursor.X, Cursor.Y);

		// Which way the loop runs is decided by the edge the cursor is over,
		// not by which axis is longer. The old rule measured the patch and
		// ignored the mouse entirely, so there was no way to ask for the
		// other direction - you got whichever one the patch happened to
		// favour. A loop crosses the edge it is pointed at: hover a segment
		// that runs across the columns and the new line is a column.
		double BestDistance = TNumericLimits<double>::Max();
		bool bBestColumn = false;
		int32 BestIndex = INDEX_NONE;

		auto ScreenOf = [&](int32 X, int32 Y, FVector2D& Out) -> bool
		{
			return View->WorldToPixel(ToWorld.TransformPosition(Patch.At(X, Y).Pos), Out);
		};

		// Distance from the mouse to a screen-space segment, which is what
		// "the edge under the cursor" means once the patch is in perspective.
		auto Consider = [&](const FVector2D& A, const FVector2D& B, bool bColumn, int32 Index)
		{
			const FVector2D Span = B - A;
			const double Length = Span.SizeSquared();

			const double T = Length > SMALL_NUMBER
				? FMath::Clamp(FVector2D::DotProduct(Mouse - A, Span) / Length, 0.0, 1.0)
				: 0.0;

			const double Distance = FVector2D::Distance(Mouse, A + Span * T);

			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				bBestColumn = bColumn;
				BestIndex = Index;
			}
		};

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				FVector2D Here;
				if (!ScreenOf(X, Y, Here))
				{
					continue;
				}

				FVector2D Next;

				// A segment running along X is crossed by a new column, and
				// the column goes between X and X + 1.
				if (X + 1 < Patch.Width && ScreenOf(X + 1, Y, Next))
				{
					Consider(Here, Next, /*bColumn*/ true, X);
				}

				if (Y + 1 < Patch.Height && ScreenOf(X, Y + 1, Next))
				{
					Consider(Here, Next, /*bColumn*/ false, Y);
				}
			}
		}

		if (BestIndex == INDEX_NONE)
		{
			return false;
		}

		bOutColumn = bBestColumn;
		OutIndex = bOutColumn
			? FMath::Clamp(BestIndex, 0, FMath::Max(0, Patch.Width - 2))
			: FMath::Clamp(BestIndex, 0, FMath::Max(0, Patch.Height - 2));

		return true;
	}
}

namespace
{
	/** Even-odd crossing test. A lasso is whatever its closed path encloses. */
	bool IsInsidePolygon(const TArray<FVector2D>& Path, const FVector2D& Point)
	{
		if (Path.Num() < 3)
		{
			return false;
		}

		bool bInside = false;
		for (int32 I = 0, J = Path.Num() - 1; I < Path.Num(); J = I++)
		{
			const FVector2D& A = Path[I];
			const FVector2D& B = Path[J];

			if (((A.Y > Point.Y) != (B.Y > Point.Y))
				&& (Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X))
			{
				bInside = !bInside;
			}
		}

		return bInside;
	}

	/** Whether two screen segments cross (ends touching count). */
	bool SegmentsCross(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
	{
		auto Side = [](const FVector2D& P, const FVector2D& Q, const FVector2D& R)
		{
			return (Q.X - P.X) * (R.Y - P.Y) - (Q.Y - P.Y) * (R.X - P.X);
		};
		const double S1 = Side(A, B, C);
		const double S2 = Side(A, B, D);
		const double S3 = Side(C, D, A);
		const double S4 = Side(C, D, B);
		if (((S1 > 0.0) != (S2 > 0.0)) && ((S3 > 0.0) != (S4 > 0.0)))
		{
			return true;
		}
		// Collinear and overlapping.
		auto Between = [](const FVector2D& P, const FVector2D& Q, const FVector2D& R)
		{
			return R.X >= FMath::Min(P.X, Q.X) - 0.5 && R.X <= FMath::Max(P.X, Q.X) + 0.5
				&& R.Y >= FMath::Min(P.Y, Q.Y) - 0.5 && R.Y <= FMath::Max(P.Y, Q.Y) + 0.5;
		};
		return (FMath::Abs(S1) < 1.e-6 && Between(A, B, C)) || (FMath::Abs(S2) < 1.e-6 && Between(A, B, D))
			|| (FMath::Abs(S3) < 1.e-6 && Between(C, D, A)) || (FMath::Abs(S4) < 1.e-6 && Between(C, D, B));
	}

	/** Distance from a point to a screen segment. */
	double DistanceToSegment(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double Len2 = AB.SizeSquared();
		const double T = Len2 > 1.e-9 ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / Len2, 0.0, 1.0) : 0.0;
		return FVector2D::Distance(P, A + AB * T);
	}

	/** Whether a point lies in a screen quad (two triangles, either winding). */
	bool IsInsideQuad(const FVector2D& P, const FVector2D& Q00, const FVector2D& Q10, const FVector2D& Q11, const FVector2D& Q01)
	{
		auto InTriangle = [](const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			const double D1 = (P.X - B.X) * (A.Y - B.Y) - (A.X - B.X) * (P.Y - B.Y);
			const double D2 = (P.X - C.X) * (B.Y - C.Y) - (B.X - C.X) * (P.Y - C.Y);
			const double D3 = (P.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (P.Y - A.Y);
			const bool bNeg = (D1 < 0.0) || (D2 < 0.0) || (D3 < 0.0);
			const bool bPos = (D1 > 0.0) || (D2 > 0.0) || (D3 > 0.0);
			return !(bNeg && bPos);
		};
		return InTriangle(P, Q00, Q10, Q11) || InTriangle(P, Q00, Q11, Q01);
	}
}

void USurfEdMode::SetSelectTool(ESurfSelectTool Tool)
{
	ActiveSelectTool = Tool;

	if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
	{
		Settings->SelectTool = Tool;
	}

	BeginSelectGesture();
	SurfSession::Verb(TEXT("select_tool"), FString::Printf(TEXT("%d"), static_cast<int32>(Tool)));
}

bool USurfEdMode::CycleSelectTool()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	ESurfSelectTool Tool = Settings ? Settings->SelectTool : ActiveSelectTool;

	// Round the three, but the brush only where it has points to paint - so
	// in face or object sub-mode the cycle is box, lasso, box.
	for (int32 Step = 0; Step < 3; ++Step)
	{
		Tool = Tool == ESurfSelectTool::Box ? ESurfSelectTool::Lasso
			: Tool == ESurfSelectTool::Lasso ? ESurfSelectTool::Brush
			: ESurfSelectTool::Box;

		if (Tool != ESurfSelectTool::Brush || SubMode == ESurfSubMode::Vertex)
		{
			break;
		}
	}

	SetSelectTool(Tool);

	if (Inspector)
	{
		Inspector->SetResult(
			Tool == ESurfSelectTool::Lasso ? TEXT("lasso: draw round what you want")
			: Tool == ESurfSelectTool::Brush ? TEXT("brush: paint over the points - Ctrl+wheel or [ ] for the size")
			: TEXT("box: drag a rectangle over what you want"));
		Inspector->RefreshSelectionCount();
	}
	return true;
}

void USurfEdMode::BeginSelectGesture()
{
	if (const USurfSettings* Settings = GetDefault<USurfSettings>())
	{
		ActiveSelectTool = Settings->SelectTool;
	}

	bSelectArmed = true;
	bSelectDragging = false;
	SelectPath.Reset();
	BrushTouched.Reset();

	SurfSession::Gesture(TEXT("select_gesture_begin"),
		FString::Printf(TEXT("tool=%d"), static_cast<int32>(ActiveSelectTool)));
}

bool USurfEdMode::ClaimsSelectPress() const
{
	/**
	 * "i need the box select in 4 mode too ! and prolly others too" (S63d),
	 * and again ten days later: "how do i click to select with the box while
	 * in 4 mode ? it doesnt work and pan the view instead" (S106b).
	 *
	 * I closed it the first time as already built, because the *action* is
	 * there - B arms the box in every sub-mode. The drag was not: object
	 * sub-mode never claimed the press, so the editor's own marquee took it
	 * and the view panned. **A verb you can turn on and cannot use is not
	 * built**, and the only reason I believed otherwise is that I read the
	 * code that arms it and not the code that uses it.
	 */
	if (SubMode == ESurfSubMode::Vertex || SubMode == ESurfSubMode::Edge || SubMode == ESurfSubMode::Face)
	{
		return true;
	}
	// Object sub-mode too, every press (S243a: "after cycling with w: the
	// first drag works. after that camera moves"): arming once per drag was
	// the old rule - the box is the default tool and would take every press
	// - but a press on empty space that is not a drag stays a click, and a
	// drag that draws a box is what a hand in Object mode means by it, as it
	// does in the other sub-modes. The gizmo's handles are kept out above.
	return SubMode == ESurfSubMode::Object;
}

void USurfEdMode::CancelSelectGesture()
{
	if (!bSelectArmed && !bSelectDragging)
	{
		return;
	}

	bSelectArmed = false;
	bSelectDragging = false;
	bSelectPressed = false;
	SelectPath.Reset();
	BrushTouched.Reset();

	SurfSession::Gesture(TEXT("select_gesture_cancel"));
}

bool USurfEdMode::CommitSelectGesture(FEditorViewportClient* ViewportClient, FViewport* Viewport, bool bSelect)
{
	const bool bExtend = Viewport
		&& (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift)
			|| Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));

	return CommitSelectGestureWith(FSurfPickContext::FromCursor(ViewportClient, Viewport), bSelect, bExtend);
}

bool USurfEdMode::CommitSelectGestureWith(const FSurfPickContext& Ctx, bool bSelect, bool bExtend)
{
	const TArray<FVector2D> Path = SelectPath;
	const ESurfSelectTool Tool = ActiveSelectTool;

	/**
	 * A bare box replaces what was selected; Shift and Ctrl extend it.
	 *
	 * Every marquee used to add, so boxing one point and then another left
	 * two selected - which is not what boxing a point means anywhere else,
	 * and made it impossible to correct a mis-box without clearing by hand
	 * first. Ctrl still subtracts, Shift still adds.
	 */
	if (bSelect && !bExtend)
	{
		PointSelection.Reset();
		QuadSelection.Reset();
		BrushCornerSelection.Reset();
		SoftInfluence.Reset();
		if (SubMode == ESurfSubMode::Object)
		{
			PatchSelection.Reset();
			SelectedPatch = FSurfPatchRef();
		}
	}

	bSelectArmed = false;
	bSelectDragging = false;
	SelectPath.Reset();
	BrushTouched.Reset();

	if (!Ctx.IsValid() || Path.Num() == 0 || !Ctx.World)
	{
		return false;
	}

	// Box from the two ends of the path; the lasso uses the path itself.
	const FVector2D First = Path[0];
	const FVector2D Last = Path.Last();

	const FBox2D Rect(
		FVector2D(FMath::Min(First.X, Last.X), FMath::Min(First.Y, Last.Y)),
		FVector2D(FMath::Max(First.X, Last.X), FMath::Max(First.Y, Last.Y)));

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double BrushRadius = Settings ? Settings->BrushSelectRadiusPx : 40.0;

	// Tested in screen space, because the gesture is drawn in screen space:
	// what it covers on the screen is what it should take.
	auto Covers = [&](const FVector2D& Screen)
	{
		switch (Tool)
		{
		case ESurfSelectTool::Lasso:
			return IsInsidePolygon(Path, Screen);

		case ESurfSelectTool::Brush:
			// Everything under the path was taken as the brush passed it,
			// so the commit only catches whatever the last step covered.
			for (const FVector2D& Step : Path)
			{
				if (FVector2D::Distance(Step, Screen) <= BrushRadius)
				{
					return true;
				}
			}
			return false;

		case ESurfSelectTool::Box:
		default:
			return Rect.IsInside(Screen);
		}
	};

	// Whether a screen segment crosses the gesture's outline - a box drawn
	// across a face touches the face, with no corner of it inside.
	const FVector2D RectCorners[4] = {
		Rect.Min, FVector2D(Rect.Max.X, Rect.Min.Y), Rect.Max, FVector2D(Rect.Min.X, Rect.Max.Y) };
	auto CrossesOutline = [&](const FVector2D& A, const FVector2D& B)
	{
		switch (Tool)
		{
		case ESurfSelectTool::Lasso:
			for (int32 I = 0, J = Path.Num() - 1; I < Path.Num(); J = I++)
			{
				if (SegmentsCross(A, B, Path[I], Path[J]))
				{
					return true;
				}
			}
			return false;

		case ESurfSelectTool::Brush:
			for (const FVector2D& Step : Path)
			{
				if (DistanceToSegment(Step, A, B) <= BrushRadius)
				{
					return true;
				}
			}
			return false;

		case ESurfSelectTool::Box:
		default:
			for (int32 I = 0; I < 4; ++I)
			{
				if (SegmentsCross(A, B, RectCorners[I], RectCorners[(I + 1) % 4]))
				{
					return true;
				}
			}
			return false;
		}
	};

	/**
	 * What the gesture may reach (S149a): "when working on the brush box and
	 * selecting its vertices im also selecting other object verts. should be
	 * able to select (overall!) for selected objects only."
	 *
	 * With a piece picked, a gesture in vertex, edge or face is about that
	 * piece's parts; every patch in the level was in range, so a box round
	 * a corner took the wall behind it too. Whole pieces (4) is left alone -
	 * there the gesture is how a second piece is added - and with nothing
	 * picked the gesture reaches everything, which is how the first one is.
	 *
	 * Shift reaches out: an extending gesture is how points on a second
	 * piece are added to the ones already held, so it is not narrowed. Bare
	 * is stay where you are, Shift is reach further - which is what those
	 * two modifiers mean everywhere else in the tool.
	 */
	TSet<FGuid> OnlyThese;
	if (SubMode != ESurfSubMode::Object && !bExtend && (!Settings || Settings->bSelectStaysOnPicked))
	{
		for (const FSurfPatchRef& Ref : PatchesHeldBySelection())
		{
			if (Ref.PatchId.IsValid())
			{
				OnlyThese.Add(Ref.PatchId);
			}
		}
		for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
		{
			if (Ref.BrushId.IsValid())
			{
				OnlyThese.Add(Ref.BrushId);
			}
		}
		for (const FSurfBrushCornerRef& Ref : BrushCornerSelection)
		{
			if (Ref.BrushId.IsValid())
			{
				OnlyThese.Add(Ref.BrushId);
			}
		}
	}
	const bool bNarrowed = OnlyThese.Num() > 0;

	/**
	 * Every patch in the level, not only the active one: a box is how
	 * several patches are taken at once, and a point behind a surface is
	 * skipped unless the wire view is x-ray - what the gesture covers on the
	 * screen is what can be seen on the screen.
	 */
	bool bChanged = false;
	int32 PatchesTouched = 0;
	FSurfPatchRef FirstTouched;

	for (TActorIterator<ASurfGroup> It(Ctx.World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (!Patch.HasValidDimensions())
			{
				return;
			}
			if (bNarrowed && !OnlyThese.Contains(Patch.Id))
			{
				return;
			}

			// Every point on the screen, and which of them the gesture covers:
			// inside it, and not hidden behind a surface.
			const int32 N = Patch.NumPoints();
			TArray<FVector2D> Screen;
			Screen.SetNumZeroed(N);
			TBitArray<> OnScreen(false, N);
			TBitArray<> Covered(false, N);
			int32 CoveredCount = 0;

			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					const int32 I = Y * Patch.Width + X;
					const FVector World = ToWorld.TransformPosition(Patch.At(X, Y).Pos);
					if (!Ctx.WorldToPixel(World, Screen[I]))
					{
						continue;
					}
					OnScreen[I] = true;
					if (!Covers(Screen[I]))
					{
						continue;
					}

					FSurfPointRef Ref;
					Ref.Group = Group;
					Ref.PatchId = Patch.Id;
					Ref.X = X;
					Ref.Y = Y;

					if (!IsPointVisibleFrom(Ctx.Origin, World, &Ref))
					{
						continue;
					}

					Covered[I] = true;
					++CoveredCount;
				}
			}

			// What the gesture touches is selected, not only what it covers
			// whole: a segment of the grid with an end covered, or crossing
			// the outline (and not hidden at its middle); a face with a
			// corner covered, an edge touched, or the gesture inside it.
			auto MidVisible = [&](int32 I, int32 J)
			{
				const FVector Mid = ToWorld.TransformPosition((Patch.Points[I].Pos + Patch.Points[J].Pos) * 0.5);
				return IsPointVisibleFrom(Ctx.Origin, Mid, nullptr);
			};
			auto SegmentTouched = [&](int32 I, int32 J)
			{
				if (Covered[I] || Covered[J])
				{
					return true;
				}
				return OnScreen[I] && OnScreen[J] && CrossesOutline(Screen[I], Screen[J]) && MidVisible(I, J);
			};
			auto FaceTouched = [&](int32 X, int32 Y)
			{
				const int32 I00 = Y * Patch.Width + X;
				const int32 I10 = I00 + 1;
				const int32 I01 = I00 + Patch.Width;
				const int32 I11 = I01 + 1;
				if (Covered[I00] || Covered[I10] || Covered[I01] || Covered[I11])
				{
					return true;
				}
				if (!OnScreen[I00] || !OnScreen[I10] || !OnScreen[I01] || !OnScreen[I11])
				{
					return false;
				}
				const bool bEdge = CrossesOutline(Screen[I00], Screen[I10]) || CrossesOutline(Screen[I10], Screen[I11])
					|| CrossesOutline(Screen[I11], Screen[I01]) || CrossesOutline(Screen[I01], Screen[I00]);
				const bool bWithin = !bEdge && Path.Num() > 0
					&& IsInsideQuad(Path[0], Screen[I00], Screen[I10], Screen[I11], Screen[I01]);
				if (!bEdge && !bWithin)
				{
					return false;
				}
				const FVector Centre = ToWorld.TransformPosition(
					(Patch.Points[I00].Pos + Patch.Points[I10].Pos + Patch.Points[I01].Pos + Patch.Points[I11].Pos) * 0.25);
				return IsPointVisibleFrom(Ctx.Origin, Centre, nullptr);
			};

			bool bAnyTouched = CoveredCount > 0;
			if (!bAnyTouched && SubMode == ESurfSubMode::Object)
			{
				for (int32 Y = 0; Y + 1 < Patch.Height && !bAnyTouched; ++Y)
				{
					for (int32 X = 0; X + 1 < Patch.Width && !bAnyTouched; ++X)
					{
						bAnyTouched = FaceTouched(X, Y);
					}
				}
			}
			if (!bAnyTouched && SubMode != ESurfSubMode::Object && SubMode != ESurfSubMode::Vertex)
			{
				// Edge and face sub-modes reach segments and faces with no
				// corner inside; a look at the faces says whether anything is.
				for (int32 Y = 0; Y + 1 < Patch.Height && !bAnyTouched; ++Y)
				{
					for (int32 X = 0; X + 1 < Patch.Width && !bAnyTouched; ++X)
					{
						bAnyTouched = FaceTouched(X, Y);
					}
				}
			}
			if (!bAnyTouched)
			{
				return;
			}

			FSurfPatchRef PatchRef;
			PatchRef.Group = Group;
			PatchRef.PatchId = Patch.Id;

			++PatchesTouched;
			if (!FirstTouched.IsValid())
			{
				FirstTouched = PatchRef;
			}

			// Object sub-mode: the patch as a whole.
			if (SubMode == ESurfSubMode::Object)
			{
				const int32 Existing = PatchSelection.IndexOfByKey(PatchRef);
				if (bSelect && Existing == INDEX_NONE)
				{
					PatchSelection.Add(PatchRef);
					SelectedPatch = PatchRef;
					bChanged = true;
				}
				else if (!bSelect && Existing != INDEX_NONE)
				{
					PatchSelection.RemoveAt(Existing);
					if (SelectedPatch == PatchRef)
					{
						SelectedPatch = PatchSelection.Num() > 0 ? PatchSelection.Last() : FSurfPatchRef();
					}
					bChanged = true;
				}
				return;
			}

			// Face sub-mode: every face the gesture touches.
			if (SubMode == ESurfSubMode::Face)
			{
				for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
				{
					for (int32 X = 0; X + 1 < Patch.Width; ++X)
					{
						if (!FaceTouched(X, Y))
						{
							continue;
						}

						FSurfQuadRef Quad;
						Quad.Group = Group;
						Quad.PatchId = Patch.Id;
						Quad.X = X;
						Quad.Y = Y;

						const int32 Existing = QuadSelection.IndexOfByKey(Quad);
						if (bSelect && Existing == INDEX_NONE)
						{
							QuadSelection.Add(Quad);
							bChanged = true;
						}
						else if (!bSelect && Existing != INDEX_NONE)
						{
							QuadSelection.RemoveAt(Existing);
							bChanged = true;
						}
					}
				}
				return;
			}

			// Which points the gesture takes. Vertex sub-mode: those covered.
			// Edge sub-mode: the segments with both ends covered - or, when
			// there are none, every segment the gesture touches, so a box
			// drawn across a line takes the line.
			TBitArray<> Taken = Covered;
			if (SubMode == ESurfSubMode::Edge)
			{
				bool bWhole = false;
				for (int32 Y = 0; Y < Patch.Height && !bWhole; ++Y)
				{
					for (int32 X = 0; X < Patch.Width && !bWhole; ++X)
					{
						const int32 I = Y * Patch.Width + X;
						bWhole = (X + 1 < Patch.Width && Covered[I] && Covered[I + 1])
							|| (Y + 1 < Patch.Height && Covered[I] && Covered[I + Patch.Width]);
					}
				}
				if (!bWhole)
				{
					Taken.Init(false, N);
					for (int32 Y = 0; Y < Patch.Height; ++Y)
					{
						for (int32 X = 0; X < Patch.Width; ++X)
						{
							const int32 I = Y * Patch.Width + X;
							if (X + 1 < Patch.Width && SegmentTouched(I, I + 1))
							{
								Taken[I] = true;
								Taken[I + 1] = true;
							}
							if (Y + 1 < Patch.Height && SegmentTouched(I, I + Patch.Width))
							{
								Taken[I] = true;
								Taken[I + Patch.Width] = true;
							}
						}
					}
				}
			}

			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					if (!Taken[Y * Patch.Width + X])
					{
						continue;
					}

					FSurfPointRef Ref;
					Ref.Group = Group;
					Ref.PatchId = Patch.Id;
					Ref.X = X;
					Ref.Y = Y;

					const int32 Existing = PointSelection.IndexOfByKey(Ref);
					if (bSelect && Existing == INDEX_NONE)
					{
						PointSelection.Add(Ref);
						bChanged = true;
					}
					else if (!bSelect && Existing != INDEX_NONE)
					{
						PointSelection.RemoveAt(Existing);
						bChanged = true;
					}
				}
			}
		});

		/**
		 * And a brush's corners, in vertex sub-mode (S149a).
		 *
		 * "when working on the brush box and selecting its vertices im also
		 * selecting other object verts" - he was boxing a box's corners, and
		 * the gesture had never known what a brush corner was: it took the
		 * patch points behind the box and left the corners alone, which from
		 * the chair is one thing going wrong, not two. A corner is drawn
		 * where it can be clicked (S113a); it is taken where it can be boxed
		 * too.
		 */
		if (SubMode == ESurfSubMode::Vertex)
		{
			Group->ForEachBrush([&](const FSurfBrush& Brush)
			{
				if (bNarrowed && !OnlyThese.Contains(Brush.Id))
				{
					return;
				}
				FSurfHull Hull;
				if (!SurfBrushMath::DeriveHull(Brush, Hull))
				{
					return;
				}
				for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
				{
					const FVector World = ToWorld.TransformPosition(Hull.Vertices[I]);
					FVector2D Screen;
					if (!Ctx.WorldToPixel(World, Screen) || !Covers(Screen))
					{
						continue;
					}
					if (!IsPointVisibleFrom(Ctx.Origin, World, nullptr))
					{
						continue;
					}

					FSurfBrushCornerRef Corner;
					Corner.Group = Group;
					Corner.BrushId = Brush.Id;
					Corner.Corner = I;

					const int32 Existing = BrushCornerSelection.IndexOfByKey(Corner);
					if (bSelect && Existing == INDEX_NONE)
					{
						BrushCornerSelection.Add(Corner);
						bChanged = true;
					}
					else if (!bSelect && Existing != INDEX_NONE)
					{
						BrushCornerSelection.RemoveAt(Existing);
						bChanged = true;
					}
				}
			});
		}
	}

	if (SubMode == ESurfSubMode::Face)
	{
		SyncQuadSelectionToPoints();
	}

	// A gesture that reached into a patch makes it the active one when none
	// was, so the verbs have something to act on; every patch the points
	// reach into is selected with them.
	if (!SelectedPatch.IsValid() && FirstTouched.IsValid())
	{
		SelectedPatch = FirstTouched;
	}
	if (SubMode != ESurfSubMode::Object)
	{
		SyncPatchSelectionToPoints();
	}

	// Whole groups (S249b): a gesture touching a member takes its group.
	if (SubMode == ESurfSubMode::Object && bSelect && ExpandSelectionToGroups())
	{
		bChanged = true;
	}

	if (bChanged)
	{
		LastRowColumnPoint = FSurfPointRef();
		RowColumnStage = 2;
		RefreshSoftSelection();
		UpdateOrbitPivot();

		if (GEditor && GEditor->GetSelectedActorCount() > 0)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		}

		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
	}

	int32 PlacedTaken = 0;
	if (SubMode == ESurfSubMode::Object && GEditor && !IsInsidePrefab() && !IsInsideScene()
		&& Selection.Num() == 0 && PatchSelection.Num() == 0 && !SelectedPatch.IsValid())
	{
		TArray<AActor*> Covered;
		for (TActorIterator<AActor> It(Ctx.World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsHidden() || Actor->IsTemporarilyHiddenInEditor())
			{
				continue;
			}
			const ASurfPrefabInstance* Placed = Cast<ASurfPrefabInstance>(Actor);
			const bool bOurs = Placed
				? (!Placed->bGhosted && !IsRoomContext(Placed))
				: (Actor->Tags.Contains(PlacedMeshTag) && (Actor->IsA(AStaticMeshActor::StaticClass()) || Actor->IsA(ALevelInstance::StaticClass())));
			if (!bOurs)
			{
				continue;
			}
			const FBox Box = Placed ? Placed->WorldBounds() : Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
			if (!Box.IsValid)
			{
				continue;
			}
			FVector2D Screen;
			bool bIn = Ctx.WorldToPixel(Box.GetCenter(), Screen) && Covers(Screen);
			for (int32 Corner = 0; Corner < 8 && !bIn; ++Corner)
			{
				const FVector At((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
				bIn = Ctx.WorldToPixel(At, Screen) && Covers(Screen);
			}
			if (bIn)
			{
				Covered.Add(Actor);
			}
		}
		if (Covered.Num() > 0 || (bSelect && !bExtend && GEditor->GetSelectedActorCount() > 0))
		{
			if (bSelect && !bExtend)
			{
				GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			}
			for (AActor* Actor : Covered)
			{
				GEditor->SelectActor(Actor, /*bInSelected*/ bSelect, /*bNotify*/ false);
			}
			GEditor->NoteSelectionChange();
			PlacedTaken = Covered.Num();
			if (Inspector)
			{
				if (PlacedTaken > 0)
				{
					Inspector->SetResult(FString::Printf(TEXT("%d placed thing%s %s - they move and copy together"),
						PlacedTaken, PlacedTaken == 1 ? TEXT("") : TEXT("s"), bSelect ? TEXT("picked") : TEXT("let go")));
				}
				Inspector->RefreshSelectionCount();
			}
		}
	}

	SurfSession::Gesture(TEXT("select_gesture_commit"),
		FString::Printf(TEXT("tool=%d points=%d corners=%d faces=%d patches=%d touched=%d narrowed=%d changed=%d placed=%d"),
			static_cast<int32>(Tool), PointSelection.Num(), BrushCornerSelection.Num(), QuadSelection.Num(),
			PatchSelection.Num(), PatchesTouched, bNarrowed ? 1 : 0, bChanged ? 1 : 0, PlacedTaken));

	return bChanged || PlacedTaken > 0;
}

void USurfEdMode::ClickAtCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (!ViewportClient || !Viewport)
	{
		return;
	}

	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
		Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));

	FSceneView* View = ViewportClient->CalcSceneView(&Family);
	if (!View)
	{
		return;
	}

	FIntPoint Cursor;
	Viewport->GetMousePos(Cursor);

	// Built against the same constructor the editor uses, so the ray and the
	// modifier state are exactly what a real click would have carried.
	const FViewportClick Click(View, ViewportClient, EKeys::LeftMouseButton, IE_Released, Cursor.X, Cursor.Y);

	HandleClick(ViewportClient, nullptr, Click);
	ViewportClient->Invalidate();
}

bool USurfEdMode::TrackSelectDrag(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	if (bPaintStroking)
	{
		PaintWeightAt(ViewportClient, Viewport, FVector2D(X, Y));
		if (ViewportClient)
		{
			ViewportClient->Invalidate();
		}
		return true;
	}

	// The ring follows the cursor while the brush is out, so the view has to
	// redraw as the hand moves even when nothing is being painted.
	if (bPaintArmed && ViewportClient)
	{
		ViewportClient->Invalidate();
	}

	if (!bSelectPressed && !bSelectDragging)
	{
		return false;
	}

	const FVector2D Position(X, Y);

	// A press only becomes a drag once it has actually moved. Below that, it
	// is a click and belongs to point selection.
	if (!bSelectDragging)
	{
		if (FVector2D::Distance(SelectPressAt, Position) < DragThresholdPx)
		{
			return false;
		}

		// Alt and moving is the orbit, not a box: the press was claimed so
		// that an Alt-click could pick a whole line, and a hand that then
		// drags wants the camera. Our own orbit, about the same centre as the
		// middle button, so nothing swings to re-centre.
		const bool bAlt = Viewport && (Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt));
		if (bAlt && FSurfCamera::IsBlenderScheme())
		{
			bSelectPressed = false;
			if (!Camera)
			{
				Camera = MakeUnique<FSurfCamera>();
			}
			Camera->BeginAltOrbit(ViewportClient, Viewport);
			return true;
		}

		if (!bSelectArmed)
		{
			BeginSelectGesture();
		}

		bSelectDragging = true;
		bSelectArmed = false;
		SelectPath.Reset();
		SelectPath.Add(SelectPressAt);
		BrushTouched.Reset();
	}

	// A box only needs its two ends; a lasso and a brush need the whole path,
	// thinned so a slow drag does not store thousands of points.
	if (ActiveSelectTool == ESurfSelectTool::Box)
	{
		if (SelectPath.Num() < 2)
		{
			SelectPath.Add(Position);
		}
		else
		{
			SelectPath.Last() = Position;
		}
	}
	else if (SelectPath.Num() == 0 || FVector2D::Distance(SelectPath.Last(), Position) > 3.0)
	{
		SelectPath.Add(Position);

		// The brush paints as it passes, rather than waiting for the button to
		// come up and deciding everything at once. Waiting is what made it
		// feel unresponsive: nothing on screen changed until the gesture was
		// already over.
		if (ActiveSelectTool == ESurfSelectTool::Brush)
		{
			PaintBrushAt(ViewportClient, Viewport, Position);
		}
	}

	if (ViewportClient)
	{
		ViewportClient->Invalidate();
	}

	return true;
}

void USurfEdMode::PaintBrushAt(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& At)
{
	const bool bRemove = Viewport
		&& (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));

	PaintBrushWith(FSurfPickContext::FromCursor(ViewportClient, Viewport), At, bRemove);
}

void USurfEdMode::PaintBrushWith(const FSurfPickContext& Ctx, const FVector2D& At, bool bRemove)
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Patch = ResolveSelectedPatch(*this, Group);
	if (!Patch || !Group || !Ctx.IsValid())
	{
		return;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Radius = Settings ? Settings->BrushSelectRadiusPx : 40.0;

	const FTransform ToWorld = Group->GetActorTransform();
	bool bChanged = false;

	for (int32 Y = 0; Y < Patch->Height; ++Y)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			const int32 Index = Y * Patch->Width + X;

			// Each point is decided once per gesture, so dragging back and
			// forth over the same points does not flicker them on and off.
			if (BrushTouched.Contains(Index))
			{
				continue;
			}

			FVector2D Screen;
			if (!Ctx.WorldToPixel(ToWorld.TransformPosition(Patch->At(X, Y).Pos), Screen))
			{
				continue;
			}

			if (FVector2D::Distance(Screen, At) > Radius)
			{
				continue;
			}

			BrushTouched.Add(Index);

			FSurfPointRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch->Id;
			Ref.X = X;
			Ref.Y = Y;

			const int32 Existing = PointSelection.IndexOfByKey(Ref);

			if (!bRemove && Existing == INDEX_NONE)
			{
				PointSelection.Add(Ref);
				bChanged = true;
			}
			else if (bRemove && Existing != INDEX_NONE)
			{
				PointSelection.RemoveAt(Existing);
				bChanged = true;
			}
		}
	}

	if (bChanged)
	{
		LastRowColumnPoint = FSurfPointRef();
		RowColumnStage = 2;
		RefreshSoftSelection();

		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
	}
}

// --- Painting the layer weight (D14, S63b) -------------------------------------
//
// The brush is the select brush's - same ring, same size on the wheel -
// acting on the control points' weight instead of on the selection. The
// weight is a vertex attribute: interpolated across the tessellation, it
// lands in the preview mesh's vertex alpha (S63a) for any material that
// reads it. It is not what makes a layer visible; a layer is order (D64).
//
// A stroke sets each point to the brush's falloff where it passed nearest,
// and never lowers what it painted: dragging back and forth over a point
// does not pump it up and down with the mouse's event rate. Ctrl does the
// same downward. So one pass at full strength is a clean edge, and a second
// pass can only add - which is what a hand expects from a brush.

static float SurfChannelOf(const FSurfControlPoint& Point, ESurfPaintChannel Channel)
{
	switch (Channel)
	{
	case ESurfPaintChannel::G: return Point.Tint.G / 255.0f;
	case ESurfPaintChannel::B: return Point.Tint.B / 255.0f;
	case ESurfPaintChannel::A: return Point.Weight;
	default: return Point.Tint.R / 255.0f;
	}
}

static void SurfSetChannel(FSurfControlPoint& Point, ESurfPaintChannel Channel, float Value)
{
	const float Clamped = FMath::Clamp(Value, 0.0f, 1.0f);
	const uint8 Byte = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Clamped * 255.0f), 0, 255));
	switch (Channel)
	{
	case ESurfPaintChannel::G: Point.Tint.G = Byte; break;
	case ESurfPaintChannel::B: Point.Tint.B = Byte; break;
	case ESurfPaintChannel::A: Point.Weight = Clamped; break;
	default: Point.Tint.R = Byte; break;
	}
}

bool USurfEdMode::FillSelectedPaint(bool bFull)
{
	TArray<FSurfPatchRef> Targets;
	PaintTargets(Targets);
	TArray<UStaticMeshComponent*> Meshes;
	PaintMeshTargets(Meshes);
	if (Targets.Num() == 0 && Meshes.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("fill: select a patch, some of its points, or a mesh first"));
		}
		return false;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	// The ticked channels (S248a): R, G and B together are the colour.
	const FSurfPaintSet Set = Settings ? Settings->PaintSet() : FSurfPaintSet();
	TArray<ESurfPaintChannel, TInlineAllocator<4>> Singles;
	Set.Singles(Singles);
	const FLinearColor Colour = bFull ? (Settings ? Settings->PaintColour : FLinearColor::Red) : (Settings ? Settings->PaintColourBack : FLinearColor::Black);

	FScopedTransaction Transaction(bFull ? LOCTEXT("PaintFill", "Fill Paint") : LOCTEXT("PaintClear", "Clear Paint"));
	TArray<ASurfGroup*> Groups;
	int32 Points = FillMeshPaint(bFull);
	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		TArray<FIntPoint> Which;
		for (const FSurfPointRef& Each : PointSelection)
		{
			if (Each.PatchId == Ref.PatchId && Each.Group == Ref.Group
				&& Each.X >= 0 && Each.X < Patch->Width && Each.Y >= 0 && Each.Y < Patch->Height)
			{
				Which.Add(FIntPoint(Each.X, Each.Y));
			}
		}
		if (Which.Num() == 0)
		{
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					Which.Add(FIntPoint(X, Y));
				}
			}
		}
		for (const FIntPoint& P : Which)
		{
			FSurfControlPoint& Point = Patch->At(P.X, P.Y);
			const float Through = 1.0f;
			if (Set.IsColour())
			{
				const FLinearColor Now(Point.Tint.R / 255.0f, Point.Tint.G / 255.0f, Point.Tint.B / 255.0f, 1.0f);
				const FLinearColor Want = FMath::Lerp(Now, Colour, Through);
				Point.Tint = FColor(
					static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.R * 255.0f), 0, 255)),
					static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.G * 255.0f), 0, 255)),
					static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.B * 255.0f), 0, 255)),
					Point.Tint.A);
			}
			// Every other ticked channel - A beside the colour - to full or to nothing.
			for (const ESurfPaintChannel Each : Singles)
			{
				SurfSetChannel(Point, Each, FMath::Lerp(SurfChannelOf(Point, Each), bFull ? 1.0f : 0.0f, Through));
			}
		}
		Points += Which.Num();
		++Patch->EditSerial;
		Groups.AddUnique(Group);
	}
	if (Points == 0)
	{
		Transaction.Cancel();
		return false;
	}
	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s %s on %d point%s"), bFull ? TEXT("filled") : TEXT("cleared"),
			*Set.Name(), Points, Points == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("paint_fill"), FString::Printf(TEXT("full=%d channel=%s points=%d"), bFull ? 1 : 0, *Set.Name(), Points));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

void USurfEdMode::PaintTargets(TArray<FSurfPatchRef>& Out)
{
	Out = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Out.AddUnique(Held);
	}
	if (Out.Num() == 0 && SelectedPatch.IsValid())
	{
		Out.Add(SelectedPatch);
	}
	// The brush out with nothing picked paints whatever it reaches (S241a:
	// "cant click Brush to enable paint mode" - Shift+B four times with
	// nothing picked did nothing, and said so only in the readout).
	if (Out.Num() == 0 && bPaintArmed && bPaintAnything)
	{
		Out = GetAllPatches();
	}
}

void USurfEdMode::TogglePaint()
{
	if (bPaintArmed)
	{
		EndPaintStroke();
		bPaintArmed = false;
		RefreshPaintView();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("paint brush put away"));
		}
		SurfSession::Verb(TEXT("paint_weight"), TEXT("off"));
		return;
	}

	TArray<FSurfPatchRef> Targets;
	PaintTargets(Targets);
	TArray<UStaticMeshComponent*> Meshes;
	PaintMeshTargets(Meshes);
	// Nothing picked: the brush paints whatever it reaches (S241a).
	bPaintAnything = Targets.Num() == 0 && Meshes.Num() == 0;
	TArray<ASurfPrefabInstance*> OwnPaint;
	PaintInstanceTargets(OwnPaint);
	if (bPaintAnything && GetAllPatches().Num() == 0 && OwnPaint.Num() == 0)
	{
		SurfSession::Friction(TEXT("paint_nothing"), TEXT("brush out over nothing it paints"));
	}

	CancelSelectGesture();
	bPaintArmed = true;
	RefreshPaintView();

	if (Inspector)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		Inspector->SetResult(FString::Printf(
			TEXT("paint brush out over %d patch%s and %d mesh%s, channel %s%s - drag paints up, Ctrl-drag down, Shift-drag smooths, Ctrl+wheel or [ ] for the size (%.0f px), Escape puts it away; the Paint window has the rest"),
			Targets.Num(), Targets.Num() == 1 ? TEXT("") : TEXT("es"), Meshes.Num(), Meshes.Num() == 1 ? TEXT("") : TEXT("es"),
			Settings ? *Settings->PaintSet().Name() : TEXT("R"),
			Settings && Settings->bPaintViewOn ? TEXT(" shown on every surface") : TEXT(""),
			Settings ? Settings->BrushSelectRadiusPx : 40.0));
	}
	SurfSession::Verb(TEXT("paint_weight"), FString::Printf(TEXT("on patches=%d placed=%d"), Targets.Num(), OwnPaint.Num()));
}

void USurfEdMode::SetMatcap(bool bOn)
{
	if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
	{
		Settings->bMatcap = bOn;
		Settings->SaveConfig();
	}
	SurfPreviewMaterial::RefreshViewColours();
	RefreshPaintView();
	if (Inspector)
	{
		Inspector->SetResult(bOn ? TEXT("matcap: clay on every patch and primitive - again for their materials") : TEXT("matcap off: the materials are back"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("matcap"), bOn ? TEXT("on") : TEXT("off"));
}

void USurfEdMode::SetViewKind(int32 Kind)
{
	// Unlit or Matcap picked in Game view (S187a): "why gradient not showing
	// now" - the game view hides the clay and the backdrop, so picking one
	// of them leaves it.
	if (Kind != 0 && GEditor)
	{
		bool bLeft = false;
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective() && Client->IsInGameView())
			{
				Client->SetGameView(false);
				bLeft = true;
			}
		}
		if (bLeft)
		{
			ViewKindBeforeGame = -1;
		}
	}
	const bool bUnlit = Kind != 0;
	if (IsModellingView() != bUnlit)
	{
		ToggleShadedView();
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bMatcap = Kind == 2;
	if ((Settings && Settings->bMatcap) != bMatcap)
	{
		SetMatcap(bMatcap);
	}
	// Paint (S229a): the channel on every surface, brush or not - kind 3.
	if (USurfSettings* Mutable = GetMutableDefault<USurfSettings>())
	{
		const bool bChannel = Kind == 3;
		if (Mutable->bChannelView != bChannel)
		{
			Mutable->bChannelView = bChannel;
			Mutable->SaveConfig();
		}
	}
	RefreshPaintView();
	if (Inspector)
	{
		Inspector->SetResult(Kind == 3 ? TEXT("paint view: the channel on every surface") : Kind == 2 ? TEXT("matcap: the clay on everything, unlit underneath") : Kind == 1 ? TEXT("unlit") : TEXT("lit"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("view_kind"), Kind == 3 ? TEXT("paint") : Kind == 2 ? TEXT("matcap") : Kind == 1 ? TEXT("unlit") : TEXT("lit"));
	RedrawViewports();
}

bool USurfEdMode::IsGridShown() const
{
	if (!GEditor)
	{
		return false;
	}
	// The viewport in hand first (S208b): the first perspective client in
	// the list may be a pane that is not on screen, and its flag is not
	// the one he sees - Grid said "on" three times running.
	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective())
	{
		return GCurrentLevelEditingViewportClient->EngineShowFlags.Grid;
	}
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client && Client->IsPerspective())
		{
			return Client->EngineShowFlags.Grid;
		}
	}
	return false;
}

void USurfEdMode::SetGridShown(bool bOn)
{
	if (!GEditor)
	{
		return;
	}
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client && Client->IsPerspective())
		{
			Client->EngineShowFlags.SetGrid(bOn);
			Client->Invalidate();
		}
	}
	SurfSession::Verb(TEXT("grid"), bOn ? TEXT("on") : TEXT("off"));
	RedrawViewports();
}

void USurfEdMode::KeepSelectionInView()
{
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client || !Client->IsPerspective() || !Client->Viewport)
	{
		return;
	}
	FBox Box(ForceInit);
	if (const ASurfGroup* Group = SelectedPatch.Group.Get())
	{
		if (const FSurfPatch* Patch = Group->FindPatch(SelectedPatch.PatchId))
		{
			const FTransform ToWorld = Group->GetActorTransform();
			for (const FSurfControlPoint& Point : Patch->Points)
			{
				Box += ToWorld.TransformPosition(Point.Pos);
			}
		}
	}
	if (!Box.IsValid)
	{
		if (const ASurfGroup* Group = ActiveFace.Group.Get())
		{
			FSurfHull Hull;
			const FSurfBrush* Brush = Group->FindBrush(ActiveFace.BrushId);
			if (Brush && SurfBrushMath::DeriveHull(*Brush, Hull))
			{
				const FTransform ToWorld = Group->GetActorTransform();
				for (const FVector& V : Hull.Vertices)
				{
					Box += ToWorld.TransformPosition(V);
				}
			}
		}
	}
	if (!Box.IsValid)
	{
		return;
	}
	/**
	 * A new piece is seen (S187a): "make sure creating something moves the
	 * camera to see it, without editing too much the current camera". When
	 * it is already wholly in view nothing moves; otherwise the camera keeps
	 * its turn and slides so the piece sits in the middle, as far off as
	 * before or far enough for it to fit.
	 */
	const FVector Centre = Box.GetCenter();
	const double Radius = FMath::Max(25.0, Box.GetExtent().Size());
	const FVector Location = Client->GetViewLocation();
	const FRotator Turn = Client->GetViewRotation();
	const FRotationMatrix Axes(Turn);
	const FVector Forward = Axes.GetScaledAxis(EAxis::X);
	const FVector Right = Axes.GetScaledAxis(EAxis::Y);
	const FVector Up = Axes.GetScaledAxis(EAxis::Z);
	const FVector ToCentre = Centre - Location;
	const double Depth = FVector::DotProduct(ToCentre, Forward);
	const FIntPoint Size = Client->Viewport->GetSizeXY();
	const double Aspect = Size.Y > 0 ? static_cast<double>(Size.X) / Size.Y : 1.6;
	const double TanH = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(Client->ViewFOV), 10.0, 170.0) * 0.5));
	const double TanV = TanH / FMath::Max(0.2, Aspect);
	const bool bInView = Depth > Radius
		&& FMath::Abs(FVector::DotProduct(ToCentre, Right)) + Radius <= Depth * TanH
		&& FMath::Abs(FVector::DotProduct(ToCentre, Up)) + Radius <= Depth * TanV;
	const double Fit = Radius / FMath::Max(0.05, FMath::Min(TanH, TanV)) * 1.2;
	/**
	 * Near enough to work on, too (S188a): "creating patch need focus cam" -
	 * a patch made in view but far off was left a small thing in the middle.
	 * In view and within a bit less than twice the framing distance stays
	 * put; otherwise the camera slides in to it.
	 */
	const double Farthest = Fit * 1.8;
	if (bInView && Depth <= Farthest)
	{
		return;
	}
	const double Distance = FMath::Clamp(Depth, Fit, Farthest);
	Client->SetViewLocation(Centre - Forward * Distance);
	Client->SetLookAtLocation(Centre);
	Client->Invalidate();
	SurfSession::Verb(TEXT("camera_to_new"), FString::Printf(TEXT("%.0f cm"), Distance));
}

int32 USurfEdMode::GetViewKind() const
{
	const bool bUnlit = IsModellingView();
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (Settings && Settings->bChannelView)
	{
		return 3;
	}
	if (bUnlit && Settings && Settings->bMatcap)
	{
		return 2;
	}
	return bUnlit ? 1 : 0;
}

void USurfEdMode::DrawBackdrop(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!View || !PDI || !Settings || !Settings->bBackdropGradient || !View->Family
		|| View->Family->EngineShowFlags.Lighting || View->Family->EngineShowFlags.Game || !View->IsPerspectiveProjection())
	{
		return;
	}
	// The view pass paints it while the mode is up (S250a), before the
	// see-through ghosts; this quad is the fallback alone.
	if (MatcapExtension.IsValid() && MatcapExtension->bBackdrop && SurfPreviewMaterial::GetBackdropPost())
	{
		return;
	}
	UMaterialInterface* Material = SurfPreviewMaterial::GetBackdrop();
	if (!Material)
	{
		return;
	}
	// Far enough to be behind anything built, near enough to keep its depth.
	constexpr double Distance = 1.0e6;
	const FMatrix& Projection = View->ViewMatrices.GetViewToClip();
	const double HalfX = Distance * 1.1 / FMath::Max(1.e-3, static_cast<double>(Projection.M[0][0]));
	const double HalfY = Distance * 1.1 / FMath::Max(1.e-3, static_cast<double>(Projection.M[1][1]));
	const FVector Eye = View->ViewMatrices.GetViewOrigin();
	const FVector Forward = View->GetViewDirection();
	const FVector Right = View->GetViewRight();
	const FVector Up = View->GetViewUp();
	const FVector Middle = Eye + Forward * Distance;

	FDynamicMeshBuilder Builder(View->GetFeatureLevel());
	auto Corner = [&](double X, double Y, float U, float V)
	{
		return Builder.AddVertex(FDynamicMeshVertex(FVector3f(Middle + Right * X + Up * Y), FVector2f(U, V), FColor::White));
	};
	// UV.y runs 0 at the top of the view to 1 at the bottom - the extra
	// tenth each side keeps the visible run from 0.045 to 0.955, near enough.
	const int32 A = Corner(-HalfX, HalfY, 0.0f, 0.0f);
	const int32 B = Corner(HalfX, HalfY, 1.0f, 0.0f);
	const int32 C = Corner(HalfX, -HalfY, 1.0f, 1.0f);
	const int32 D = Corner(-HalfX, -HalfY, 0.0f, 1.0f);
	Builder.AddTriangle(A, B, C);
	Builder.AddTriangle(A, C, D);
	PDI->SetHitProxy(nullptr);
	Builder.Draw(PDI, FMatrix::Identity, Material->GetRenderProxy(), SDPG_World, /*bDisableBackfaceCulling*/ true, /*bReceivesDecals*/ false);
}

void USurfEdMode::DrawGrid(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!View || !PDI || !Settings || !View->Family || !View->Family->EngineShowFlags.Grid || View->Family->EngineShowFlags.Game || !View->IsPerspectiveProjection())
	{
		return;
	}
	const FVector Eye = View->ViewMatrices.GetViewOrigin();
	// How high the eye is decides the spacing: a line every metre from
	// about five metres up, every ten centimetres from fifty, every ten
	// metres from fifty metres - metric, and each level fading into the
	// next as the eye rises, so the grid never jumps.
	const double Height = FMath::Max(20.0, FMath::Abs(Eye.Z));
	const double Level = FMath::LogX(10.0, Height / 5.0);
	const double Minor = FMath::Pow(10.0, FMath::FloorToDouble(Level));
	const double Major = Minor * 10.0;
	const float MinorFade = 1.0f - static_cast<float>(Level - FMath::FloorToDouble(Level));
	// Out to a few heights, at most a hundred lines each way.
	const double Reach = FMath::Min(Height * 6.0, Minor * 100.0);
	const double CentreX = FMath::RoundToDouble(Eye.X / Major) * Major;
	const double CentreY = FMath::RoundToDouble(Eye.Y / Major) * Major;
	const double Opacity = FMath::Clamp(Settings->GridOpacity, 0.02f, 1.0f);
	const FLinearColor Line(0.55f, 0.56f, 0.60f, 1.0f);
	constexpr int32 Pieces = 6;

	auto Fade = [&](const FVector& P) -> float
	{
		const double Out = FVector2D(P.X - Eye.X, P.Y - Eye.Y).Size() / Reach;
		return static_cast<float>(FMath::Clamp(1.0 - Out * Out, 0.0, 1.0));
	};
	auto Draw = [&](const FVector& From, const FVector& To, const FLinearColor& Colour, float Alpha, float Thickness)
	{
		FVector Previous = From;
		for (int32 I = 1; I <= Pieces; ++I)
		{
			const FVector Next = FMath::Lerp(From, To, static_cast<double>(I) / Pieces);
			const float A = Alpha * Fade((Previous + Next) * 0.5);
			if (A > 0.004f)
			{
				// Translucent (S173a): DrawLine ignores alpha - the grid was
				// solid white, "must be a bit more subtle".
				PDI->DrawTranslucentLine(Previous, Next, Colour.CopyWithNewOpacity(A), SDPG_World, Thickness, 0.0f, Thickness > 0.0f);
			}
			Previous = Next;
		}
	};

	PDI->SetHitProxy(nullptr);
	const int32 Steps = FMath::CeilToInt(Reach / Minor);
	for (int32 I = -Steps; I <= Steps; ++I)
	{
		const double X = CentreX + I * Minor;
		const double Y = CentreY + I * Minor;
		const bool bMajorX = FMath::IsNearlyZero(FMath::Fmod(FMath::Abs(X), Major), Minor * 0.01) || FMath::IsNearlyEqual(FMath::Fmod(FMath::Abs(X), Major), Major, Minor * 0.01);
		const bool bMajorY = FMath::IsNearlyZero(FMath::Fmod(FMath::Abs(Y), Major), Minor * 0.01) || FMath::IsNearlyEqual(FMath::Fmod(FMath::Abs(Y), Major), Major, Minor * 0.01);
		const bool bAxisX = Settings->bShowGridAxes && FMath::Abs(X) < Minor * 0.01;
		const bool bAxisY = Settings->bShowGridAxes && FMath::Abs(Y) < Minor * 0.01;
		if (!bAxisX)
		{
			const float A = static_cast<float>(Opacity) * (bMajorX ? 1.0f : 0.45f * MinorFade);
			Draw(FVector(X, CentreY - Reach, 0.0), FVector(X, CentreY + Reach, 0.0), Line, A, 0.0f);
		}
		if (!bAxisY)
		{
			const float A = static_cast<float>(Opacity) * (bMajorY ? 1.0f : 0.45f * MinorFade);
			Draw(FVector(CentreX - Reach, Y, 0.0), FVector(CentreX + Reach, Y, 0.0), Line, A, 0.0f);
		}
	}

	// The axes: X red along Y = 0, Y green along X = 0, a short Z blue up
	// from the origin - a touch stronger and wider than the grid.
	if (Settings->bShowGridAxes)
	{
		const float A = FMath::Min(1.0f, static_cast<float>(Opacity) * 2.6f);
		if (FMath::Abs(CentreY) <= Reach)
		{
			Draw(FVector(CentreX - Reach, 0.0, 0.0), FVector(CentreX + Reach, 0.0, 0.0), FLinearColor(0.85f, 0.22f, 0.22f), A, 1.5f);
		}
		if (FMath::Abs(CentreX) <= Reach)
		{
			Draw(FVector(0.0, CentreY - Reach, 0.0), FVector(0.0, CentreY + Reach, 0.0), FLinearColor(0.35f, 0.78f, 0.22f), A, 1.5f);
		}
		PDI->DrawTranslucentLine(FVector::ZeroVector, FVector(0.0, 0.0, Major), FLinearColor(0.25f, 0.45f, 0.95f).CopyWithNewOpacity(A), SDPG_World, 1.5f, 0.0f, true);
	}
}

void USurfEdMode::RefreshPaintView()
{
	// "i need to be able to enable some attribute view mode so we see what
	// we paintin": the brush out is the view. Every slot of every group
	// shows the channel, and the materials come back with the brush put
	// away - no mode to find, nothing to remember to switch off.
	const USurfSettings* ViewSettings = GetDefault<USurfSettings>();
	// The Paint view kind shows it with the brush away too (S229a).
	if (ViewSettings && ((bPaintArmed && ViewSettings->bPaintViewOn) || ViewSettings->bChannelView))
	{
		ASurfGroup::PreviewOverrideProvider.BindLambda([]() -> UMaterialInterface*
		{
			const USurfSettings* Settings = GetDefault<USurfSettings>();
			return SurfPreviewMaterial::GetPaintView(Settings ? Settings->PaintSet().ViewIndex() : 0);
		});
	}
	else if (bViewOverridesAllowed && ViewSettings && ViewSettings->bMatcap && SurfPreviewMaterial::GetMatcap())
	{
		// Ours wear a plain lit grey (S174a), so the scene pass below
		// lights them exactly as it lights everything else.
		// Clay on everything of ours while the mode is up (S172b).
		ASurfGroup::PreviewOverrideProvider.BindLambda([]() -> UMaterialInterface*
		{
			return SurfPreviewMaterial::GetMatcap();
		});
	}
	else
	{
		ASurfGroup::PreviewOverrideProvider.Unbind();
	}
	// The scene pass (S173a): on with the tick, off while painting shows its channel.
	if (MatcapExtension.IsValid())
	{
		const bool bWas = MatcapExtension->bEnabled;
		MatcapExtension->bEnabled = bViewOverridesAllowed && ViewSettings && ViewSettings->bMatcap && !(bPaintArmed && ViewSettings->bPaintViewOn) && !ViewSettings->bChannelView;
		// Every change of the clay pass on the record (S206a): "somehow
		// using steps switched me to matcap mode?" - the record had none.
		if (bWas != MatcapExtension->bEnabled)
		{
			SurfSession::Verb(TEXT("matcap_pass"), FString::Printf(TEXT("%s after=%s"), MatcapExtension->bEnabled ? TEXT("on") : TEXT("off"), *LastVerbName));
		}
		// The Paint view drawn gamma only while it shows (S248a).
		MatcapExtension->bPaintView = bViewOverridesAllowed && ViewSettings && ((bPaintArmed && ViewSettings->bPaintViewOn) || ViewSettings->bChannelView);
	}

	if (UWorld* World = GetWorld())
	{
		UMaterialInterface* Wanted = ASurfGroup::PreviewOverrideProvider.IsBound() ? ASurfGroup::PreviewOverrideProvider.Execute() : nullptr;
		int32 Looked = 0;
		int32 Missed = 0;
		FString FirstMissed;
		for (TActorIterator<ASurfGroup> It(World); It; ++It)
		{
			if (ASurfGroup* Group = *It)
			{
				Group->RefreshPreviewMaterial();
				++Looked;
				const UMaterialInterface* Worn = Group->Preview ? Group->Preview->GetMaterial(0) : nullptr;
				if (Wanted && !Group->bGhosted && Worn != Wanted)
				{
					if (++Missed == 1)
					{
						FirstMissed = FString::Printf(TEXT("%s@%s:%s"), *Group->GetActorLabel(),
							Group->GetLevel() && Group->GetLevel()->GetOuter() ? *Group->GetLevel()->GetOuter()->GetName() : TEXT("-"), Worn ? *Worn->GetName() : TEXT("none"));
					}
				}
			}
		}
		if (Wanted)
		{
			SurfSession::Verb(TEXT("look_check"), FString::Printf(TEXT("wanted=%s groups=%d missed=%d first=%s"), *Wanted->GetName(), Looked, Missed, *FirstMissed));
		}
		// Placed prefabs wear the same override (S174a).
		UMaterialInterface* Override = ASurfGroup::PreviewOverrideProvider.IsBound() ? ASurfGroup::PreviewOverrideProvider.Execute() : nullptr;
		if (Override != InstancesOverride.Get() || bInstancesOverridden != (Override != nullptr))
		{
			InstancesOverride = Override;
			bInstancesOverridden = Override != nullptr;
			// Only their materials (S175a): a full rebuild of every placed
			// prefab was the second-long hitch on the matcap tick.
			for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
			{
				if (ASurfPrefabInstance* Instance = *It)
				{
					Instance->RefreshPreviewMaterial();
				}
			}
		}
		// Placed meshes show the channel too (S236a): "the paint view's
		// channel override does not show on meshes yet".
		const bool bMeshView = bViewOverridesAllowed && ViewSettings && ((bPaintArmed && ViewSettings->bPaintViewOn) || ViewSettings->bChannelView);
		RefreshMeshPaintView(World, bMeshView ? SurfPreviewMaterial::GetPaintView(ViewSettings->PaintSet().ViewIndex()) : nullptr);
	}
	if (GEditor)
	{
		RedrawViewports();
	}
}

void USurfEdMode::BeginPaintStroke(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	EndPaintStroke();

	PaintTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("PaintWeight", "Paint Layer Weight"));
	bPaintStroking = true;

	// Every group under the brush is marked once, at the start, so the whole
	// stroke is one undo whatever the hand does in between.
	TArray<FSurfPatchRef> Targets;
	PaintTargets(Targets);
	TArray<ASurfGroup*> Groups;
	for (const FSurfPatchRef& Ref : Targets)
	{
		if (ASurfGroup* Group = Ref.Group.Get())
		{
			if (!Groups.Contains(Group))
			{
				Group->Modify();
				Groups.Add(Group);
			}
		}
	}
	TArray<UStaticMeshComponent*> Meshes;
	PaintMeshTargets(Meshes);
	for (UStaticMeshComponent* Component : Meshes)
	{
		Component->Modify();
	}
	TArray<ASurfPrefabInstance*> Instances;
	PaintInstanceTargets(Instances);
	for (ASurfPrefabInstance* Instance : Instances)
	{
		Instance->Modify();
	}

	if (Viewport)
	{
		FIntPoint Cursor;
		Viewport->GetMousePos(Cursor);
		PaintWeightAt(ViewportClient, Viewport, FVector2D(Cursor.X, Cursor.Y));
	}
}

void USurfEdMode::EndPaintStroke()
{
	if (!bPaintStroking)
	{
		return;
	}
	bPaintStroking = false;
	PaintTransaction.Reset();
}

void USurfEdMode::PaintWeightAt(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& At)
{
	const bool bErase = Viewport
		&& (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));
	const bool bSmooth = Viewport
		&& (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift));
	PaintWeightWith(FSurfPickContext::FromCursor(ViewportClient, Viewport), At, bErase, bSmooth);
}

int32 USurfEdMode::PaintWeightWith(const FSurfPickContext& Ctx, const FVector2D& At, bool bErase, bool bSmooth)
{
	if (!Ctx.IsValid())
	{
		return 0;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Radius = FMath::Max(1.0, Settings ? Settings->BrushSelectRadiusPx : 40.0);
	const double Strength = FMath::Clamp(Settings ? Settings->PaintStrength : 1.0, 0.05, 1.0);
	// The ticked channels (S248a): R, G and B together are the colour.
	const FSurfPaintSet Set = Settings ? Settings->PaintSet() : FSurfPaintSet();
	TArray<ESurfPaintChannel, TInlineAllocator<4>> Singles;
	Set.Singles(Singles);

	TArray<FSurfPatchRef> Targets;
	PaintTargets(Targets);

	TArray<ASurfGroup*> Groups;
	int32 Changed = 0;

	// One patch's pass (S251j: the same on a placed prefab's own paint):
	// Read is what it reads, Writable hands the patch to write the first
	// time a point is in reach.
	auto Stroke = [&](const FSurfPatch* Read, const FTransform& ToWorld, TFunctionRef<FSurfPatch*()> Writable, FSurfPatch*& Patch)
	{
		/**
		 * Smoothing reads from a copy taken before the pass begins (S104a).
		 *
		 * A point pulled towards its neighbours while those neighbours are
		 * themselves being pulled is a smear, not a smooth: the pass would
		 * chase itself across the grid and the result would depend on which
		 * corner the loop started from.
		 */
		TArray<float> Was;
		TArray<FColor> WasTint;
		if (bSmooth)
		{
			Was.Reserve(Read->NumPoints());
			WasTint.Reserve(Read->NumPoints());
			for (int32 Y = 0; Y < Read->Height; ++Y)
			{
				for (int32 X = 0; X < Read->Width; ++X)
				{
					// The alpha here, the colour channels in WasTint: every
					// channel smooths towards its own mean (S248a).
					Was.Add(Read->At(X, Y).Weight);
					WasTint.Add(Read->At(X, Y).Tint);
				}
			}
		}

		// The nine points about one, the grid's edge included by leaving out
		// what is not there: a corner averages four, a side six.
		auto Around = [&Was, &WasTint, Read](int32 X, int32 Y, float& OutValue, FLinearColor& OutColour)
		{
			double Sum = 0.0;
			FLinearColor Colour(0.0f, 0.0f, 0.0f, 0.0f);
			int32 Count = 0;
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 NX = X + DX;
					const int32 NY = Y + DY;
					if (NX < 0 || NY < 0 || NX >= Read->Width || NY >= Read->Height)
					{
						continue;
					}
					const int32 I = NY * Read->Width + NX;
					if (!Was.IsValidIndex(I))
					{
						continue;
					}
					Sum += Was[I];
					Colour += FLinearColor(WasTint[I].R / 255.0f, WasTint[I].G / 255.0f, WasTint[I].B / 255.0f, 1.0f);
					++Count;
				}
			}
			OutValue = Count > 0 ? static_cast<float>(Sum / static_cast<double>(Count)) : 0.0f;
			OutColour = Count > 0 ? Colour / static_cast<float>(Count) : FLinearColor::Black;
		};

		for (int32 Y = 0; Y < Read->Height; ++Y)
		{
			for (int32 X = 0; X < Read->Width; ++X)
			{
				FVector2D Screen;
				if (!Ctx.WorldToPixel(ToWorld.TransformPosition(Read->At(X, Y).Pos), Screen))
				{
					continue;
				}
				const double Distance = FVector2D::Distance(Screen, At);
				if (Distance > Radius)
				{
					continue;
				}

				// Smooth from the centre to nothing at the ring.
				const double T = 1.0 - Distance / Radius;
				const double Falloff = T * T * (3.0 - 2.0 * T);
				// Above one, a pass reaches full before the ring does: "can we
				// get more power on Strength?" (S72e). Held at full, since a
				// channel past full is not a channel.
				const float Reach = static_cast<float>(FMath::Clamp(Strength * Falloff, 0.0, 1.0));

				if (!Patch)
				{
					Patch = Writable();
					if (!Patch)
					{
						break;
					}
				}

				FSurfControlPoint& Point = Patch->At(X, Y);
				bool bMoved = false;
				if (Set.IsColour())
				{
					// The three colour channels together, towards the paint
					// colour (or black, erasing) by the brush's reach: a
					// colour converges, it cannot run past itself.
					// Ctrl-drag paints the second colour (S212a), not black.
					FLinearColor Target = bErase ? (Settings ? Settings->PaintColourBack : FLinearColor::Black) : (Settings ? Settings->PaintColour : FLinearColor::Red);
					if (bSmooth)
					{
						float Mean = 0.0f;
						FLinearColor Mixed = FLinearColor::Black;
						Around(X, Y, Mean, Mixed);
						Target = Mixed;
					}
					const FLinearColor Now(Point.Tint.R / 255.0f, Point.Tint.G / 255.0f, Point.Tint.B / 255.0f, 1.0f);
					const FLinearColor Want = FMath::Lerp(Now, Target, Reach);
					const FColor Byte(
						static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.R * 255.0f), 0, 255)),
						static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.G * 255.0f), 0, 255)),
						static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Want.B * 255.0f), 0, 255)),
						Point.Tint.A);
					if (Byte.R != Point.Tint.R || Byte.G != Point.Tint.G || Byte.B != Point.Tint.B)
					{
						Point.Tint = Byte;
						bMoved = true;
					}
				}
				// Every other ticked channel (S229a) - A beside the colour - the same stroke on each.
				for (const ESurfPaintChannel Each : Singles)
				{
					const float Now = SurfChannelOf(Point, Each);
					float Want;
					if (bSmooth)
					{
						// Towards the average of the points around it, by the
						// brush's reach: the one pass that can move a channel
						// down as well as up without being told which - each
						// channel towards its own (S248a; it was the first
						// ticked channel's mean for all of them).
						float MeanA = 0.0f;
						FLinearColor Mixed = FLinearColor::Black;
						Around(X, Y, MeanA, Mixed);
						const float Mean = Each == ESurfPaintChannel::G ? Mixed.G : Each == ESurfPaintChannel::B ? Mixed.B : Each == ESurfPaintChannel::A ? MeanA : Mixed.R;
						Want = FMath::Lerp(Now, Mean, Reach);
					}
					else
					{
						Want = bErase
							? FMath::Min(Now, 1.0f - Reach)
							: FMath::Max(Now, Reach);
					}
					if (!FMath::IsNearlyEqual(Want, Now, 1.0f / 512.0f))
					{
						SurfSetChannel(Point, Each, Want);
						bMoved = true;
					}
				}
				if (bMoved)
				{
					++Changed;
				}
			}
		}
	};

	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (!Group)
		{
			continue;
		}
		const FSurfPatch* Read = Group->FindPatch(Ref.PatchId);
		if (!Read || !Read->HasValidDimensions())
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		FSurfPatch* Patch = nullptr;
		Stroke(Read, ToWorld, [Group, &Ref]() { return Group->ModifyPatch(Ref.PatchId); }, Patch);

		if (Patch && Changed > 0)
		{
			++Patch->EditSerial;
			Groups.AddUnique(Group);
		}
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// Placed prefabs with Paint override (S251j): each of the prefab's own
	// patches as the instance shows it, painted, and kept on the instance.
	{
		TArray<ASurfPrefabInstance*> Instances;
		PaintInstanceTargets(Instances);
		for (ASurfPrefabInstance* Instance : Instances)
		{
			const int32 ChangedBefore = Changed;
			const FTransform InstanceToWorld = Instance->GetActorTransform();
			TArray<FSurfPatch> Painted;
			Instance->Prefab->ForEachPatch([&](const FSurfPatch& Own)
			{
				if (!Own.HasValidDimensions())
				{
					return;
				}
				FSurfPatch Copy = Own;
				Instance->ApplyPaintOverride(Copy);
				FSurfPatch* Written = nullptr;
				const int32 PatchBefore = Changed;
				Stroke(&Copy, InstanceToWorld, [&Copy]() { return &Copy; }, Written);
				if (Changed > PatchBefore)
				{
					Painted.Add(MoveTemp(Copy));
				}
			});
			if (Changed > ChangedBefore)
			{
				for (const FSurfPatch& Each : Painted)
				{
					Instance->StorePaintOverride(Each);
				}
				if (Instance->bBaked)
				{
					Instance->Unbake();
				}
				Instance->RebuildPreview();
			}
		}
	}

	// The picked meshes too (S213a).
	Changed += PaintMeshWeightWith(Ctx, At, bErase, bSmooth);

	if (Changed > 0 && Inspector)
	{
		// A smoothing pass goes both ways at once, so "up" and "down" have
		// nothing to say about it (S104a).
		Inspector->SetResult(FString::Printf(TEXT("%s %s%s on %d point%s"),
			bSmooth ? TEXT("smoothed") : TEXT("painted"),
			*Set.Name(),
			bSmooth ? TEXT("") : (bErase ? TEXT(" down") : TEXT(" up")),
			Changed, Changed == 1 ? TEXT("") : TEXT("s")));
	}
	return Changed;
}

bool USurfEdMode::DisallowMouseDeltaTracking() const
{
	// The editor accumulates every captured mouse move into its own camera,
	// whether or not a mode claimed the move, and acts on the sum each frame.
	// While our camera has the drag, that sum must stay unused: two cameras
	// answering one hand is a jump at best.
	return Camera && Camera->IsNavigating();
}

bool USurfEdMode::CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	// Claiming the move is what stops the viewport's own camera running: it
	// only acts on moves it was allowed to accumulate.
	if (Camera && Camera->HandleCapturedMove(ViewportClient, Viewport, X, Y))
	{
		return true;
	}

	if (TrackSelectDrag(ViewportClient, Viewport, X, Y))
	{
		return true;
	}

	return Super::CapturedMouseMove(ViewportClient, Viewport, X, Y);
}

FString USurfEdMode::CountStats() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return FString();
	}
	auto Thousands = [](int64 N) { return FText::AsNumber(N).ToString(); };
	TSet<const UMaterialInterface*> Materials;
	bool bChecker = false;
	auto Wear = [&Materials, &bChecker](const UMaterialInterface* Material)
	{
		if (Material)
		{
			Materials.Add(Material);
		}
		else
		{
			bChecker = true;
		}
	};
	auto PatchTris = [](const FSurfPatch& Patch) -> int64
	{
		return static_cast<int64>(FMath::Max(0, Patch.Width - 1)) * FMath::Max(0, Patch.Height - 1)
			* FMath::Max(1, Patch.SubdivU) * FMath::Max(1, Patch.SubdivV) * 2;
	};

	// What is picked, when anything is.
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	for (const FSurfPointRef& Point : PointSelection)
	{
		FSurfPatchRef Ref;
		Ref.Group = Point.Group;
		Ref.PatchId = Point.PatchId;
		Patches.AddUnique(Ref);
	}
	TSet<FGuid> Brushes;
	TArray<TPair<const ASurfGroup*, FGuid>> BrushRefs;
	for (const FSurfFaceRef& Face : Selection)
	{
		if (!Brushes.Contains(Face.BrushId))
		{
			Brushes.Add(Face.BrushId);
			BrushRefs.Add({ Face.Group.Get(), Face.BrushId });
		}
	}
	if (Patches.Num() > 0 || BrushRefs.Num() > 0)
	{
		int64 Tris = 0;
		int32 Points = 0;
		int32 PatchCount = 0;
		for (const FSurfPatchRef& Ref : Patches)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			if (const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr)
			{
				++PatchCount;
				Tris += PatchTris(*Patch);
				Points += Patch->Width * Patch->Height;
				Wear(Patch->Material.Get());
			}
		}
		int32 BrushCount = 0;
		for (const TPair<const ASurfGroup*, FGuid>& Ref : BrushRefs)
		{
			if (const FSurfBrush* Brush = Ref.Key ? Ref.Key->FindBrush(Ref.Value) : nullptr)
			{
				++BrushCount;
				for (const FSurfFace& Face : Brush->Faces)
				{
					Tris += 2;
					Wear(Face.Material.Get());
				}
			}
		}
		const int32 Picked = PointSelection.Num();
		return FString::Printf(TEXT("picked   %d patch%s   %d primitive%s   %s points%s   ~%s tris   %d material%s"),
			PatchCount, PatchCount == 1 ? TEXT("") : TEXT("es"), BrushCount, BrushCount == 1 ? TEXT("") : TEXT("s"),
			*Thousands(Points), Picked > 0 ? *FString::Printf(TEXT(" (%d picked)"), Picked) : TEXT(""),
			*Thousands(Tris), Materials.Num() + (bChecker ? 1 : 0), Materials.Num() + (bChecker ? 1 : 0) == 1 ? TEXT("") : TEXT("s"));
	}

	// The whole level.
	int64 Tris = 0;
	int32 PatchCount = 0;
	int32 BrushCount = 0;
	int32 Points = 0;
	int32 Placed = 0;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!IsValid(Group) || Group->HasAnyFlags(RF_Transient))
		{
			continue;
		}
		Tris += Group->TriangleCount;
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			++PatchCount;
			Points += Patch.Width * Patch.Height;
			Wear(Patch.Material.Get());
		});
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			++BrushCount;
			for (const FSurfFace& Face : Brush.Faces)
			{
				Wear(Face.Material.Get());
			}
		});
	}
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->HasAnyFlags(RF_Transient))
		{
			++Placed;
			Tris += It->TriangleCount;
		}
	}
	const int32 MaterialCount = Materials.Num() + (bChecker ? 1 : 0);
	return FString::Printf(TEXT("level   %d patches   %d primitives   %d prefabs placed   %s points   %s tris   %d material%s"),
		PatchCount, BrushCount, Placed, *Thousands(Points), *Thousands(Tris), MaterialCount, MaterialCount == 1 ? TEXT("") : TEXT("s"));
}

void USurfEdMode::DrawStats(FViewport* Viewport, FCanvas* Canvas)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !Settings->bShowStats || !Viewport || !Canvas || !GEngine)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now - StatsAt > 0.5 || StatsLine.IsEmpty())
	{
		StatsAt = Now;
		StatsLine = CountStats();
	}
	if (StatsLine.IsEmpty())
	{
		return;
	}
	UFont* Font = GEngine->GetSmallFont();
	int32 Wide = 0;
	int32 High = 0;
	if (Font)
	{
		StringSize(Font, Wide, High, *StatsLine);
	}
	// Bottom right, clear of the SidePanel when it stands on that side.
	const float Dpi = Canvas->GetDPIScale();
	const FIntPoint Size = Viewport->GetSizeXY();
	const bool bPanelRight = !Settings->bBarOnRight && Settings->bSidePanelOpen;
	const float Inset = (bPanelRight ? static_cast<float>(Settings->SidePanelWidthPx) : 0.0f) * Dpi + 14.0f;
	const float X = static_cast<float>(Size.X) - Inset - static_cast<float>(Wide);
	const float Y = static_cast<float>(Size.Y) - 34.0f * Dpi - static_cast<float>(High);
	FCanvasTextItem Text(FVector2D(FMath::Max(8.0f, X), Y), FText::FromString(StatsLine), Font, FLinearColor(0.62f, 0.62f, 0.64f, 0.9f));
	Text.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f));
	Canvas->DrawItem(Text);
}

void USurfEdMode::DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	Super::DrawHUD(ViewportClient, Viewport, View, Canvas);

	if (!Canvas || IsGameViewOf(Viewport))
	{
		return;
	}

	DrawStats(Viewport, Canvas);

	// The toast at the cursor is gone (S158a): "the text spawning is
	// annoying. may it be in a one liner info down viewport?" The foot line
	// carries the result for a few seconds instead.

	// The room (S71a): an amber frame round the view, so there is no
	// mistaking the level for the prefab. The line of text that used to run
	// across the top is gone (S78): a docked panel covered it, a narrow
	// viewport cut it in half, and the bar's own frame says the same thing
	// where nothing can cover it - "the text displayed in viewport is
	// hidden by other windows etc".
	if (IsInsidePrefab() && Viewport)
	{
		const FIntPoint Size = Viewport->GetSizeXY();
		const FLinearColor Amber(1.0f, 0.75f, 0.2f, 0.95f);

		FCanvasLineItem Edge;
		Edge.SetColor(Amber);
		Edge.LineThickness = 3.0f;
		const float W = static_cast<float>(Size.X) - 2.0f;
		const float H = static_cast<float>(Size.Y) - 2.0f;
		Edge.Origin = FVector(1.0f, 1.0f, 0.0f); Edge.EndPos = FVector(W, 1.0f, 0.0f); Canvas->DrawItem(Edge);
		Edge.Origin = FVector(W, 1.0f, 0.0f); Edge.EndPos = FVector(W, H, 0.0f); Canvas->DrawItem(Edge);
		Edge.Origin = FVector(W, H, 0.0f); Edge.EndPos = FVector(1.0f, H, 0.0f); Canvas->DrawItem(Edge);
		Edge.Origin = FVector(1.0f, H, 0.0f); Edge.EndPos = FVector(1.0f, 1.0f, 0.0f); Canvas->DrawItem(Edge);
	}

	// The picked group says what it is (S250a: "ctrl g has added a bbox. idk
	// if its worked"): "Group of 3" on a plate over the top of its brackets.
	if (View && GEngine && !IsOverlayHidden())
	{
		TArray<FBox> Boxes;
		TArray<int32> Counts;
		PickedGroupBounds(Boxes, &Counts);
		const int32 PickedPlates = Boxes.Num();
		// Every group with Names on, quieter (S251a: "no name label for groups ?").
		const USurfSettings* GroupPlates = GetDefault<USurfSettings>();
		if (GroupPlates && GroupPlates->bShowNames)
		{
			TArray<FBox> AllPlates;
			TArray<int32> AllPlateCounts;
			AllGroupBounds(AllPlates, &AllPlateCounts);
			for (int32 Plated = 0; Plated < AllPlates.Num(); ++Plated)
			{
				if (!Boxes.Contains(AllPlates[Plated]))
				{
					Boxes.Add(AllPlates[Plated]);
					Counts.Add(AllPlateCounts.IsValidIndex(Plated) ? AllPlateCounts[Plated] : 0);
				}
			}
		}
		for (int32 I = 0; I < Boxes.Num(); ++I)
		{
			const bool bPickedPlate = I < PickedPlates;
			const FVector Over(Boxes[I].GetCenter().X, Boxes[I].GetCenter().Y, Boxes[I].Max.Z);
			FVector2D Screen;
			if (!View->WorldToPixel(Over, Screen))
			{
				continue;
			}
			const FString Text = FString::Printf(TEXT("Group of %d"), Counts.IsValidIndex(I) ? Counts[I] : 0);
			UFont* Font = GEngine->GetSmallFont();
			int32 TextWide = 0;
			int32 TextHigh = 0;
			if (Font)
			{
				StringSize(Font, TextWide, TextHigh, *Text);
			}
			const double PlateWide = TextWide + 10.0;
			const double PlateHigh = FMath::Max(14, TextHigh + 4);
			const FVector2D Corner(Screen.X - PlateWide * 0.5, Screen.Y - PlateHigh - 10.0);
			FCanvasTileItem Plate(Corner, FVector2D(PlateWide, PlateHigh), FLinearColor(0.10f, 0.12f, 0.18f, bPickedPlate ? 0.75f : 0.55f));
			Plate.BlendMode = SE_BLEND_AlphaBlend;
			Canvas->DrawItem(Plate);
			FCanvasTextItem Item(FVector2D(Screen.X, Corner.Y + 2.0), FText::FromString(Text), Font, FLinearColor(0.80f, 0.85f, 0.95f, bPickedPlate ? 1.0f : 0.65f));
			Item.bCentreX = true;
			Canvas->DrawItem(Item);
		}
	}

	// Names in the world (S73a): what each thing is, over its middle, grey
	// when it draws nothing of its own.
	{
		const USurfSettings* Naming = GetDefault<USurfSettings>();
		UWorld* World = GetWorld();
		if (Naming && Naming->bShowNames && View && World && GEngine)
		{
			// A plate under the words, as the editor's own labels have: white
			// text on a dark viewport is unreadable over white geometry, and
			// a label that has to be hunted for is not a label (S81a).
			// Two labels in the same place read as one word with the letters
			// interleaved - "Editsomewallvall" in his screenshot. A plate
			// that would land on one already drawn moves up, then down, and
			// if neither is free it is not drawn at all: an unreadable label
			// is worse than a missing one (S86a).
			TArray<FBox2D> Taken;
			Taken.Reserve(64);
			NameLabelHits.Reset();

			auto Name = [&](const FVector& At, const FString& Text, bool bDim, const AActor* Named = nullptr)
			{
				FVector2D Screen;
				if (Text.IsEmpty() || !View->WorldToPixel(At, Screen))
				{
					return;
				}
				UFont* Font = GEngine->GetSmallFont();
				int32 TextWide = 0;
				int32 TextHigh = 0;
				if (Font)
				{
					StringSize(Font, TextWide, TextHigh, *Text);
				}
				const double PlateWide = TextWide + 10.0;
				const double PlateHigh = FMath::Max(14, TextHigh + 4);
				const double Left = Screen.X - PlateWide * 0.5;

				// The natural place, then one line up, then one line down.
				double Top = Screen.Y - PlateHigh * 0.5;
				bool bRoom = false;
				const double Tries[3] = { 0.0, -(PlateHigh + 2.0), PlateHigh + 2.0 };
				for (const double Shift : Tries)
				{
					const FBox2D Want(FVector2D(Left, Top + Shift), FVector2D(Left + PlateWide, Top + Shift + PlateHigh));
					bool bClear = true;
					for (const FBox2D& Each : Taken)
					{
						if (Each.Intersect(Want))
						{
							bClear = false;
							break;
						}
					}
					if (bClear)
					{
						Taken.Add(Want);
						if (Named)
						{
							NameLabelHits.Emplace(Want, const_cast<AActor*>(Named));
						}
						Top += Shift;
						bRoom = true;
						break;
					}
				}
				if (!bRoom)
				{
					return;
				}

				FCanvasTileItem Plate(FVector2D(Left, Top), FVector2D(PlateWide, PlateHigh),
					FLinearColor(0.02f, 0.02f, 0.03f, bDim ? 0.45f : 0.65f));
				Plate.BlendMode = SE_BLEND_AlphaBlend;
				Canvas->DrawItem(Plate);

				FCanvasTextItem Item(FVector2D(Screen.X, Top + 2.0), FText::FromString(Text), Font,
					bDim ? FLinearColor(0.68f, 0.68f, 0.74f, 1.0f) : FLinearColor(0.96f, 0.96f, 0.99f, 1.0f));
				Item.bCentreX = true;
				Canvas->DrawItem(Item);
			};

			// "displaying names i see 'surfgroup' = thats the current level.
			// couldnt we have that by default the level's name?" A group
			// nobody has renamed is the level's own geometry, so it is
			// called after the level (S86a).
			const FString MapName = World->GetMapName();
			auto LabelOf = [&MapName](const AActor* Actor) -> FString
			{
				const FString Label = Actor ? Actor->GetActorLabel() : FString();
				return Label.StartsWith(TEXT("SurfGroup")) && !MapName.IsEmpty() ? MapName : Label;
			};

			for (TActorIterator<ASurfGroup> GroupIt(World); GroupIt; ++GroupIt)
			{
				const ASurfGroup* Group = *GroupIt;
				if (!Group || Group->IsHidden() || Group->IsTemporarilyHiddenInEditor())
				{
					continue;
				}
				const FBox Box = Group->GetComponentsBoundingBox(true);
				Name(Box.IsValid ? Box.GetCenter() : Group->GetActorLocation(), LabelOf(Group), Group->bGhosted, Group);
			}
			for (TActorIterator<ASurfPrefabInstance> InstanceIt(World); InstanceIt; ++InstanceIt)
			{
				const ASurfPrefabInstance* Instance = *InstanceIt;
				if (!Instance || Instance->IsHidden() || Instance->IsTemporarilyHiddenInEditor())
				{
					continue;
				}
				const FBox Box = Instance->DrawsNothing() ? Instance->WorldBounds() : Instance->GetComponentsBoundingBox(true);
				const FString Label = Instance->Prefab ? Instance->Prefab->GetName() : FString(TEXT("(empty prefab)"));
				Name(Box.IsValid ? Box.GetCenter() : Instance->GetActorLocation(), Label,
					Instance->bGhosted || Instance->DrawsNothing(), Instance);
			}
			// The placed meshes and levels (S227a: "dont see a text label tag
			// for the level i added"), named as the prefabs are.
			for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
			{
				const AActor* Actor = *ActorIt;
				if (!Actor || Actor->IsHidden() || Actor->IsTemporarilyHiddenInEditor() || !Actor->Tags.Contains(PlacedMeshTag)
					|| !(Actor->IsA(AStaticMeshActor::StaticClass()) || Actor->IsA(ALevelInstance::StaticClass())) || IsRoomPartActor(Actor))
				{
					continue;
				}
				const FBox Box = Actor->GetComponentsBoundingBox(true);
				Name(Box.IsValid ? Box.GetCenter() : Actor->GetActorLocation(), Actor->GetActorLabel(), false, Actor);
			}
		}
	}

	const FLinearColor Colour(1.0f, 0.85f, 0.1f);

	FCanvasLineItem Line;
	Line.SetColor(Colour);

	auto Edge = [&Canvas, &Line](const FVector2D& A, const FVector2D& B)
	{
		Line.Origin = FVector(A.X, A.Y, 0.0f);
		Line.EndPos = FVector(B.X, B.Y, 0.0f);
		Canvas->DrawItem(Line);
	};

	// While the paint brush is out, every point it can reach shows its
	// weight - white painted in, dark painted out - so the paint is visible
	// before any material reads it.
	if (bPaintArmed && Viewport && ViewportClient)
	{
		const FSurfPickContext Ctx = FSurfPickContext::FromCursor(ViewportClient, Viewport);
		TArray<FSurfPatchRef> Targets;
		PaintTargets(Targets);
		for (const FSurfPatchRef& Ref : Targets)
		{
			ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Ctx.IsValid())
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			const USurfSettings* PaintSettings = GetDefault<USurfSettings>();
			// The dots show what the view shows (S248a): the colour for R G B or several.
			const int32 ViewIndex = PaintSettings ? PaintSettings->PaintSet().ViewIndex() : 0;
			const ESurfPaintChannel Channel = ViewIndex >= 4 ? ESurfPaintChannel::Colour : static_cast<ESurfPaintChannel>(ViewIndex);
			for (const FSurfControlPoint& Point : Patch->Points)
			{
				FVector2D Screen;
				if (!Ctx.WorldToPixel(ToWorld.TransformPosition(Point.Pos), Screen))
				{
					continue;
				}
				// The channel's value, in the channel's colour: red for R, green
				// for G, blue for B, white for A - and black where nothing is
				// painted (S242a: "make sure vertex go BLACK if painted BLACK.
				// not gray"), on a thin grey rim so a black dot still reads.
				const float W = Channel == ESurfPaintChannel::Colour ? 1.0f : FMath::Clamp(SurfChannelOf(Point, Channel), 0.0f, 1.0f);
				const FLinearColor DotColour =
					Channel == ESurfPaintChannel::Colour ? FLinearColor(Point.Tint.R / 255.0f, Point.Tint.G / 255.0f, Point.Tint.B / 255.0f, 1.0f) :
					Channel == ESurfPaintChannel::G ? FLinearColor(0.0f, W, 0.0f, 1.0f) :
					Channel == ESurfPaintChannel::B ? FLinearColor(0.0f, 0.0f, W, 1.0f) :
					Channel == ESurfPaintChannel::A ? FLinearColor(W, W, W, 1.0f) :
					FLinearColor(W, 0.0f, 0.0f, 1.0f);
				FCanvasTileItem Rim(Screen - FVector2D(4.5, 4.5), FVector2D(9.0, 9.0), FLinearColor(0.55f, 0.55f, 0.6f, 0.7f));
				Rim.BlendMode = SE_BLEND_Translucent;
				Canvas->DrawItem(Rim);
				FCanvasTileItem Dot(Screen - FVector2D(3.5, 3.5), FVector2D(7.0, 7.0), DotColour);
				Dot.BlendMode = SE_BLEND_Translucent;
				Canvas->DrawItem(Dot);
			}
		}
	}

	// The brush ring follows the cursor whether or not a stroke is running, so
	// its size is visible before you commit to a stroke.
	if ((bPaintArmed || ((bSelectArmed || bSelectDragging) && ActiveSelectTool == ESurfSelectTool::Brush)) && Viewport)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const double Radius = Settings ? Settings->BrushSelectRadiusPx : 40.0;

		FIntPoint Cursor;
		Viewport->GetMousePos(Cursor);

		constexpr int32 Steps = 32;
		FVector2D Previous(Cursor.X + Radius, Cursor.Y);
		for (int32 I = 1; I <= Steps; ++I)
		{
			const double Angle = (2.0 * PI * I) / Steps;
			const FVector2D Point(Cursor.X + Radius * FMath::Cos(Angle), Cursor.Y + Radius * FMath::Sin(Angle));
			Edge(Previous, Point);
			Previous = Point;
		}
	}

	if (!bSelectDragging || SelectPath.Num() == 0)
	{
		return;
	}

	if (ActiveSelectTool == ESurfSelectTool::Box && SelectPath.Num() >= 2)
	{
		const FVector2D A = SelectPath[0];
		const FVector2D B = SelectPath.Last();

		Edge(FVector2D(A.X, A.Y), FVector2D(B.X, A.Y));
		Edge(FVector2D(B.X, A.Y), FVector2D(B.X, B.Y));
		Edge(FVector2D(B.X, B.Y), FVector2D(A.X, B.Y));
		Edge(FVector2D(A.X, B.Y), FVector2D(A.X, A.Y));
	}
	else if (ActiveSelectTool == ESurfSelectTool::Lasso)
	{
		for (int32 I = 0; I + 1 < SelectPath.Num(); ++I)
		{
			Edge(SelectPath[I], SelectPath[I + 1]);
		}

		// Drawn closed, because that is how it is tested.
		if (SelectPath.Num() >= 3)
		{
			Edge(SelectPath.Last(), SelectPath[0]);
		}
	}
}

void USurfEdMode::BeginLoopPreview()
{
	// One cut, until the wheel says otherwise. A count carried over from the
	// last cut is a surprise the second time.
	LoopCount = 1;

	bPreviewingLoop = true;
	LoopIndex = INDEX_NONE;

	/**
	 * And says so (S150a): "when i hit click on a ctrl+r i should have a
	 * popup and param 'amount' and by default is 1 but you can add more
	 * edges at once."
	 *
	 * It has taken a count since it was built, on the wheel, where every
	 * tool with a loop cut puts it - and nothing ever said so, so it was a
	 * feature nobody could find. The line says it while the cut is armed,
	 * which is the one moment it is worth reading.
	 */
	if (Inspector)
	{
		Inspector->SetResult(TEXT("loop cut: 1 line - the wheel makes more, click to cut, Escape to drop it"));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Gesture(TEXT("loop_preview_begin"));
}

void USurfEdMode::CancelLoopPreview()
{
	if (!bPreviewingLoop)
	{
		return;
	}

	bPreviewingLoop = false;
	LoopIndex = INDEX_NONE;
	SurfSession::Gesture(TEXT("loop_preview_cancel"));
}

void USurfEdMode::UpdateLoopPreview(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	if (!bPreviewingLoop)
	{
		return;
	}

	ASurfGroup* Group = nullptr;
	const FSurfPatch* Patch = ResolveSelectedPatch(*this, Group);
	if (!Patch || !Group)
	{
		LoopIndex = INDEX_NONE;
		return;
	}

	if (!FindLoopUnderCursor(*this, ViewportClient, Viewport, *Patch, Group->GetActorTransform(), bLoopIsColumn, LoopIndex))
	{
		LoopIndex = INDEX_NONE;
	}
}

bool USurfEdMode::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	if (bLatticeModal)
	{
		UpdateLatticeModal(ViewportClient, Viewport);
		return true;
	}

	// A modal transform follows the free mouse, with no button held - that is
	// what makes it modal rather than a drag.
	if (Modal != ESurfModal::None)
	{
		UpdateModal(ViewportClient, Viewport);
		return true;
	}

	if (TrackSelectDrag(ViewportClient, Viewport, X, Y))
	{
		return true;
	}

	// Our gizmo: the handle under the cursor sets the editor's widget mode.
	UpdateGizmoHover(ViewportClient, Viewport, X, Y);

	// The brush over a placed prefab it does not paint (S251j): a cross.
	if (bPaintArmed && !IsInsidePrefab() && !IsInsideScene())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - PaintHoverAt > 0.08)
		{
			PaintHoverAt = Now;
			double Distance = 0.0;
			const ASurfPrefabInstance* Under = InstanceUnderForTest(FSurfPickContext::FromCursor(ViewportClient, Viewport), Distance);
			bPaintHoverBlocked = Under && !Under->bOverridePaint;
		}
	}
	else
	{
		bPaintHoverBlocked = false;
	}

	if (bPreviewingLoop)
	{
		UpdateLoopPreview(ViewportClient, Viewport);

		// Redraw as the cursor moves, or the preview line lags a frame behind
		// and reads as the wrong line.
		if (ViewportClient)
		{
			ViewportClient->Invalidate();
		}
	}

	return Super::MouseMove(ViewportClient, Viewport, X, Y);
}

bool USurfEdMode::LostFocus(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// A gesture that survives losing the viewport is a gesture you cannot get
	// out of.
	// A gesture that survives losing the viewport is one you cannot get out of.
	CancelModal();
	CancelLoopPreview();
	CancelSelectGesture();
	return Super::LostFocus(ViewportClient, Viewport);
}

bool USurfEdMode::SubdivideSelectedPatches(bool bRows, bool bColumns, int32 Times)
{
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	Times = FMath::Clamp(Times, 1, 4);
	if (Patches.Num() == 0 || (!bRows && !bColumns))
	{
		if (Inspector)
		{
			Inspector->SetResult(Patches.Num() == 0
				? TEXT("subdivide: select a patch first")
				: TEXT("subdivide: tick rows, columns or both in the options"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("subdivide_refused"));
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("SubdividePatches", "Subdivide Patches"));

	int32 Done = 0;
	int32 NoRoom = 0;
	int32 Short = 0;
	bool bNarrowed = false;
	FString Sizes;
	TArray<ASurfGroup*> Groups;

	for (const FSurfPatchRef& Ref : Patches)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Found = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Found || !Found->HasValidDimensions())
		{
			continue;
		}

		// A line between every pair of lines: a sheet gains W - 1 columns, a
		// curve (which grows two at a time, to stay odd) gains one pair per
		// segment - Times over. The cap is the grid's: 64 lines on a sheet,
		// 15 on a curve. Each way is taken as far as it fits: a strip of 43
		// by 6 cannot double across but can down, and did nothing at all
		// before - "couldnt subdivide" on a wall that had room the other
		// way. Only a patch full both ways is left as it is.
		const bool bCurve = Found->Type == ESurfPatchType::Curve;
		const int32 Step = bCurve ? 2 : 1;
		const int32 Max = bCurve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;
		auto Doublings = [&](int32 Lines, bool bWanted) -> int32
		{
			if (!bWanted) { return 0; }
			int32 Count = 0;
			for (int32 T = 0; T < Times; ++T)
			{
				const int32 Next = Lines + Step * ((Lines - 1) / Step);
				if (Next > Max) { break; }
				Lines = Next;
				++Count;
			}
			return Count;
		};
		const int32 TimesAcross = Doublings(Found->Width, bColumns);
		const int32 TimesDown = Doublings(Found->Height, bRows);
		if (TimesAcross == 0 && TimesDown == 0)
		{
			++NoRoom;
			continue;
		}
		if ((bColumns && TimesAcross < Times) || (bRows && TimesDown < Times))
		{
			++Short;
		}

		FSurfPatch* Mutable = Group->ModifyPatch(Ref.PatchId);
		if (!Mutable)
		{
			continue;
		}

		const int32 WidthWas = Mutable->Width;
		const int32 HeightWas = Mutable->Height;

		/**
		 * Only where the selection is, when there is one (S150b): "should be
		 * able to subdivide only selection too."
		 *
		 * A grid line runs the whole patch, so this cannot mean "cut these
		 * faces and not their neighbours" - it means cut **between these
		 * lines**: a wall made dense where the detail is going without
		 * trebling the whole of it. The span from the first selected line to
		 * the last, each way; with nothing selected on the patch it is the
		 * whole of it, as it always was.
		 */
		int32 FromX = 0;
		int32 ToX = Mutable->Width - 2;
		int32 FromY = 0;
		int32 ToY = Mutable->Height - 2;
		bool bNarrow = false;
		{
			int32 LowX = MAX_int32;
			int32 HighX = -1;
			int32 LowY = MAX_int32;
			int32 HighY = -1;
			for (const FSurfPointRef& Picked : PointSelection)
			{
				if (Picked.PatchId != Ref.PatchId || Picked.Group != Ref.Group)
				{
					continue;
				}
				LowX = FMath::Min(LowX, Picked.X);
				HighX = FMath::Max(HighX, Picked.X);
				LowY = FMath::Min(LowY, Picked.Y);
				HighY = FMath::Max(HighY, Picked.Y);
			}
			// Two lines apart at least: one line on its own bounds no cell.
			if (HighX - LowX >= 1 || HighY - LowY >= 1)
			{
				bNarrow = true;
				FromX = HighX - LowX >= 1 ? LowX : 0;
				ToX = HighX - LowX >= 1 ? HighX - 1 : Mutable->Width - 2;
				FromY = HighY - LowY >= 1 ? LowY : 0;
				ToY = HighY - LowY >= 1 ? HighY - 1 : Mutable->Height - 2;
			}
		}

		for (int32 T = 0; T < Times; ++T)
		{
			// From the far end back, so the indices still to come are not
			// moved by the lines already added.
			if (T < TimesAcross)
			{
				for (int32 X = FMath::Min(ToX, Mutable->Width - 1 - Step); X >= FromX; X -= Step)
				{
					SurfPatchPrimitives::InsertColumn(*Mutable, X);
				}
				// A run of k cells cut once is 2k cells, and every new line
				// went in at or after the first, so the span starts where it
				// started and ends twice as far along.
				ToX = FromX + 2 * (ToX - FromX + 1) - 1;
			}
			if (T < TimesDown)
			{
				for (int32 Y = FMath::Min(ToY, Mutable->Height - 1 - Step); Y >= FromY; Y -= Step)
				{
					SurfPatchPrimitives::InsertRow(*Mutable, Y);
				}
				ToY = FromY + 2 * (ToY - FromY + 1) - 1;
			}
		}
		bNarrowed |= bNarrow;

		++Mutable->EditSerial;
		SurfPatchSurfacing::Reapply(*Mutable);
		Groups.AddUnique(Group);
		++Done;

		if (Sizes.IsEmpty())
		{
			Sizes = FString::Printf(TEXT("%d x %d -> %d x %d"), WidthWas, HeightWas, Mutable->Width, Mutable->Height);
		}
	}

	if (Done == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(NoRoom > 0
				? TEXT("subdivide: no room - the patch is full both ways (a sheet holds 64 lines each way, a curve 15)")
				: TEXT("subdivide: nothing to subdivide"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("subdivide_refused"));
		return false;
	}

	for (ASurfGroup* Each : Groups)
	{
		Each->RebuildPreview();
	}

	// The whole of what was subdivided is the selection: every new line is
	// there to be seen and grabbed.
	PatchSelection = Patches;
	SelectAllPoints();

	if (Inspector)
	{
		FString Line = Done == 1
			? FString::Printf(TEXT("subdivided the patch: %s"), *Sizes)
			: FString::Printf(TEXT("subdivided %d patches (the first: %s)"), Done, *Sizes);
		if (Short > 0)
		{
			Line += Short == 1 && Done == 1
				? FString(TEXT(" - full one way, so only the other was subdivided (64 lines is the most a sheet holds, 15 a curve)"))
				: FString::Printf(TEXT(" - %d full one way, subdivided the other way only"), Short);
		}
		if (NoRoom > 0)
		{
			Line += FString::Printf(TEXT(" - %d left as they were, full both ways"), NoRoom);
		}
		if (bNarrowed)
		{
			Line += TEXT(" - between the lines that were picked; pick nothing to cut the whole patch");
		}
		Inspector->SetResult(Line);
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("subdivide"), FString::Printf(TEXT("patches=%d rows=%d columns=%d times=%d noroom=%d short=%d narrow=%d"),
		Done, bRows ? 1 : 0, bColumns ? 1 : 0, Times, NoRoom, Short, bNarrowed ? 1 : 0));

	// Which way and how many times can be changed afterwards.
	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::Subdivide;
		Op.bRows = bRows;
		Op.bColumns = bColumns;
		Op.Iterations = Times;
		NoteOperation(Op);
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::InsertLoopAtCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	const FSurfPatch Patch = *Found;
	const FGuid PatchId = Patch.Id;

	bool bColumn = bLoopIsColumn;
	int32 Index = LoopIndex;

	// When committed from the preview the line is already chosen; when called
	// cold, work it out now.
	if (Index == INDEX_NONE
		&& !FindLoopUnderCursor(*this, ViewportClient, Viewport, Patch, Group->GetActorTransform(), bColumn, Index))
	{
		return false;
	}

	// The joined patches it carries on into, read before anything is cut.
	const TArray<FSurfLoopLink> Chain = TraceLoopChain(Group, PatchId, bColumn, Index);

	FScopedTransaction Transaction(LOCTEXT("InsertLoop", "Insert Loop"));

	FSurfPatch* Mutable = Group->ModifyPatch(PatchId);
	if (!Mutable)
	{
		return false;
	}

	// More than one cut, spread across the cell rather than piled on top of
	// each other: each new line goes one place further along than the last,
	// so asking for three gives three evenly through the span.
	const int32 Wanted = FMath::Clamp(LoopCount, 1, 8);
	int32 Inserted = 0;

	for (int32 I = 0; I < Wanted; ++I)
	{
		const int32 At = bColumn
			? FMath::Clamp(Index + I, 0, FMath::Max(0, Mutable->Width - 2))
			: FMath::Clamp(Index + I, 0, FMath::Max(0, Mutable->Height - 2));

		const bool bOne = bColumn
			? SurfPatchPrimitives::InsertColumn(*Mutable, At)
			: SurfPatchPrimitives::InsertRow(*Mutable, At);

		if (!bOne)
		{
			break;
		}

		++Inserted;
	}

	const bool bInserted = Inserted > 0;

	if (!bInserted)
	{
		Transaction.Cancel();
		SurfSession::Friction(TEXT("insert_loop_refused"),
			FString::Printf(TEXT("%s at %d size=%dx%d"),
				bColumn ? TEXT("column") : TEXT("row"), Index, Patch.Width, Patch.Height));
		return false;
	}

	SurfPatchSurfacing::Reapply(*Mutable);
	Group->RebuildPreview();

	// On into the joined patches (S190a, backlog 18).
	TArray<FSurfPointRef> Carried;
	const int32 Across = CarryLoopAcross(Chain, Inserted, Carried);

	// Points from here on (S190e): leaving whole-piece (4) clears the point
	// selection, so it has to happen before the new lines are picked - a cut
	// made from 4 used to arrive with nothing picked and no slide.
	if (SubMode == ESurfSubMode::Object)
	{
		SetSubMode(ESurfSubMode::Vertex);
	}

	// Select the lines that were just cut and hand them to the slide, so the
	// cut can be placed rather than landing wherever the arithmetic put it -
	// which is the half of the gesture that was missing.
	ClearPointSelection();

	const int32 FirstNew = Index + 1;
	const int32 LastNew = FMath::Min(Index + Inserted,
		(bColumn ? Mutable->Width : Mutable->Height) - 1);

	const int32 AcrossLine = bColumn ? Mutable->Height : Mutable->Width;

	for (int32 Line = FirstNew; Line <= LastNew; ++Line)
	{
		for (int32 B = 0; B < AcrossLine; ++B)
		{
			FSurfPointRef Ref;
			Ref.Group = Group;
			Ref.PatchId = PatchId;
			Ref.X = bColumn ? Line : B;
			Ref.Y = bColumn ? B : Line;
			AddPointToSelection(Ref);
		}
	}

	for (const FSurfPointRef& Ref : Carried)
	{
		AddPointToSelection(Ref);
	}

	CancelLoopPreview();

	BeginModal(ESurfModal::Slide, ViewportClient, Viewport);
	LoopSlideChain.Reset();

	SurfSession::Verb(TEXT("insert_loop"), FString::Printf(TEXT("%d %s at %d -> %dx%d across=%d of %d found"),
		Inserted, bColumn ? TEXT("columns") : TEXT("rows"), Index, Mutable->Width, Mutable->Height, Across, Chain.Num()));

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	return true;
}

TArray<USurfEdMode::FSurfLoopLink> USurfEdMode::TraceLoopChain(const ASurfGroup* Group, const FGuid& PatchId, bool bColumn, int32 Index) const
{
	TArray<FSurfLoopLink> Chain;
	const FSurfPatch* Start = Group ? Group->FindPatch(PatchId) : nullptr;
	UWorld* World = GetWorld();
	const int32 Lines = Start ? (bColumn ? Start->Width : Start->Height) : 0;
	if (!Start || !World || Start->Type != ESurfPatchType::Sheet || !Start->HasValidDimensions() || Index < 0 || Index + 1 >= Lines)
	{
		return Chain;
	}

	/**
	 * A span of a border the cut crosses, in the world: Low is the end at
	 * the lower line, so a slide towards High is the cut's own "forward".
	 */
	struct FSpan
	{
		FVector Low = FVector::ZeroVector;
		FVector High = FVector::ZeroVector;
	};
	TArray<FSpan> Fronts;
	TSet<FGuid> Done;
	Done.Add(PatchId);
	{
		const FTransform ToWorld = Group->GetActorTransform();
		const int32 Ends = bColumn ? Start->Height : Start->Width;
		for (const int32 End : { 0, Ends - 1 })
		{
			FSpan Span;
			Span.Low = ToWorld.TransformPosition(bColumn ? Start->At(Index, End).Pos : Start->At(End, Index).Pos);
			Span.High = ToWorld.TransformPosition(bColumn ? Start->At(Index + 1, End).Pos : Start->At(End, Index + 1).Pos);
			Fronts.Add(Span);
		}
	}

	constexpr double Tolerance = 0.5;
	while (Fronts.Num() > 0 && Chain.Num() < 64)
	{
		const FSpan Front = Fronts.Pop();
		const ASurfGroup* FoundGroup = nullptr;
		const FSurfPatch* Found = nullptr;
		FSurfLoopLink Link;
		int32 FoundBorder = 0;
		for (TActorIterator<ASurfGroup> It(World); It && !FoundGroup; ++It)
		{
			const ASurfGroup* Other = *It;
			if (!Other || !IsOpen(Other))
			{
				continue;
			}
			const FTransform OtherToWorld = Other->GetActorTransform();
			Other->ForEachPatch([&](const FSurfPatch& Patch)
			{
				if (FoundGroup || Done.Contains(Patch.Id) || Patch.Type != ESurfPatchType::Sheet || !Patch.HasValidDimensions())
				{
					return;
				}
				auto Match = [&](const FVector& A, const FVector& B, bool bAsColumn, int32 At, int32 Border)
				{
					const bool bSame = A.Equals(Front.Low, Tolerance) && B.Equals(Front.High, Tolerance);
					const bool bFlip = A.Equals(Front.High, Tolerance) && B.Equals(Front.Low, Tolerance);
					if (!FoundGroup && (bSame || bFlip))
					{
						FoundGroup = Other;
						Found = &Patch;
						Link.Group = const_cast<ASurfGroup*>(Other);
						Link.PatchId = Patch.Id;
						Link.bColumn = bAsColumn;
						Link.At = At;
						Link.Sign = bSame ? 1 : -1;
						FoundBorder = Border;
					}
				};
				// Along a row border a column goes in; along a column border, a row.
				for (const int32 Y : { 0, Patch.Height - 1 })
				{
					for (int32 X = 0; X + 1 < Patch.Width && !FoundGroup; ++X)
					{
						Match(OtherToWorld.TransformPosition(Patch.At(X, Y).Pos), OtherToWorld.TransformPosition(Patch.At(X + 1, Y).Pos), true, X, Y);
					}
				}
				for (const int32 X : { 0, Patch.Width - 1 })
				{
					for (int32 Y = 0; Y + 1 < Patch.Height && !FoundGroup; ++Y)
					{
						Match(OtherToWorld.TransformPosition(Patch.At(X, Y).Pos), OtherToWorld.TransformPosition(Patch.At(X, Y + 1).Pos), false, Y, X);
					}
				}
			});
		}
		if (!FoundGroup || !Found)
		{
			continue;
		}
		Done.Add(Link.PatchId);
		Chain.Add(Link);

		// And on across its far border.
		const int32 Ends = Link.bColumn ? Found->Height : Found->Width;
		const int32 Far = FoundBorder == 0 ? Ends - 1 : 0;
		const FTransform FoundToWorld = FoundGroup->GetActorTransform();
		auto Where = [&](int32 Line, int32 Along)
		{
			return FoundToWorld.TransformPosition(Link.bColumn ? Found->At(Line, Along).Pos : Found->At(Along, Line).Pos);
		};
		const FVector P0 = Where(Link.At, Far);
		const FVector P1 = Where(Link.At + 1, Far);
		FSpan Next;
		Next.Low = Link.Sign > 0 ? P0 : P1;
		Next.High = Link.Sign > 0 ? P1 : P0;
		Fronts.Add(Next);
	}
	return Chain;
}

int32 USurfEdMode::CarryLoopAcross(const TArray<FSurfLoopLink>& Chain, int32 Inserted, TArray<FSurfPointRef>& OutPoints)
{
	LoopSlideChain.Reset();
	TArray<ASurfGroup*> Touched;
	int32 Cut = 0;
	for (const FSurfLoopLink& Link : Chain)
	{
		ASurfGroup* Group = Link.Group.Get();
		FSurfPatch* Target = Group ? Group->ModifyPatch(Link.PatchId) : nullptr;
		if (!Target)
		{
			continue;
		}
		// The same lines at the same places: running the other way, each
		// new one goes in beside the low end, so they land where the cut's do.
		int32 Made = 0;
		for (int32 I = 0; I < Inserted; ++I)
		{
			const int32 At = Link.Sign > 0 ? Link.At + I : Link.At;
			const bool bOne = Link.bColumn ? SurfPatchPrimitives::InsertColumn(*Target, At) : SurfPatchPrimitives::InsertRow(*Target, At);
			if (!bOne)
			{
				break;
			}
			++Made;
		}
		if (Made == 0)
		{
			continue;
		}
		SurfPatchSurfacing::Reapply(*Target);
		++Target->EditSerial;
		++Cut;
		Touched.AddUnique(Group);
		LoopSlideChain.Add(Link.PatchId, FIntPoint(Link.bColumn ? 1 : 0, Link.Sign));

		const int32 Ends = Link.bColumn ? Target->Height : Target->Width;
		for (int32 Line = Link.At + 1; Line <= Link.At + Made; ++Line)
		{
			for (int32 B = 0; B < Ends; ++B)
			{
				FSurfPointRef Ref;
				Ref.Group = Group;
				Ref.PatchId = Link.PatchId;
				Ref.X = Link.bColumn ? Line : B;
				Ref.Y = Link.bColumn ? B : Line;
				OutPoints.Add(Ref);
			}
		}
	}
	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}
	return Cut;
}

// --- Curve to sheet -----------------------------------------------------------

bool USurfEdMode::ConvertSelectedPatchToSheet()
{
	ASurfGroup* Group = nullptr;
	const FSurfPatch* Found = ResolveSelectedPatch(*this, Group);
	if (!Found || !Group)
	{
		return false;
	}

	const FSurfPatch Source = *Found;
	const FGuid OldId = Source.Id;

	FSurfPatch Sheet;
	int32 SubdivisionUsed = 0;

	if (!SurfPatchPrimitives::ConvertToSheet(Source, Sheet, &SubdivisionUsed))
	{
		// Refusing without saying why is indistinguishable from being broken,
		// which is exactly how this read.
		const FString Reason = Source.Type == ESurfPatchType::Sheet
			? TEXT("nothing to apply - this patch's points already are its surface")
			: TEXT("cannot apply: the patch has no valid dimensions");

		if (Inspector)
		{
			Inspector->SetResult(Reason);
			Inspector->RefreshSelectionCount();
		}

		SurfSession::Friction(TEXT("convert_to_sheet_refused"),
			FString::Printf(TEXT("type=%d %dx%d"), static_cast<int32>(Source.Type), Source.Width, Source.Height));
		return false;
	}

	const FGuid NewId = Sheet.Id;

	FScopedTransaction Transaction(LOCTEXT("ConvertToSheet", "Convert To Sheet"));

	Group->Modify();
	Group->RemovePatch(OldId);
	Group->AddPatch(MoveTemp(Sheet));
	Group->RebuildPreview();

	// The verb states its resulting selection: the sheet it made, in vertex
	// editing, because that is the whole reason for converting.
	FSurfPatchRef Ref;
	Ref.Group = Group;
	Ref.PatchId = NewId;
	SelectPatch(Ref);
	SetSubMode(ESurfSubMode::Vertex);

	const FSurfPatch* Made = Group->FindPatch(NewId);

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("applied at subdivision %d: %d x %d points, every one editable"),
			SubdivisionUsed, Made ? Made->Width : 0, Made ? Made->Height : 0));
		Inspector->RefreshSelectionCount();
	}

	// The surface does not change shape, so without a redraw the only visible
	// difference - a denser cage - did not appear until the next click.
	if (GEditor)
	{
		RedrawViewports();
	}

	SurfSession::Verb(TEXT("convert_to_sheet"), FString::Printf(TEXT("subdiv=%d -> %dx%d"),
		SubdivisionUsed, Made ? Made->Width : 0, Made ? Made->Height : 0));

	return true;
}

#undef LOCTEXT_NAMESPACE
