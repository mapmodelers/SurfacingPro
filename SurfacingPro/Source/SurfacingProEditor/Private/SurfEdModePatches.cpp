// Copyright MapModelers. All Rights Reserved.
//
// Patch editing: picking, control point selection, and the gizmo.
//
// This is the half of the workflow the whole product exists for. A patch has
// fixed topology and only its control points move, so unlike mesh editing it
// cannot be corrupted - there is no non-planar face to create and no sliver
// to leave behind.

#include "SurfSnapState.h"
#include "SurfEdMode.h"
#include "Misc/PackageName.h"
#include "SurfInspector.h"

#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "ScopedTransaction.h"

#include "SurfGroup.h"
#include "SurfChunk.h"
#include "SurfPatchMath.h"
#include "SurfBrushMath.h"
#include "SurfPatchSurfacing.h"
#include "SurfSettings.h"
#include "ConvexVolume.h"
#include "SurfSession.h"
#include "SceneView.h"
#include "SurfPickContext.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Components/DynamicMeshComponent.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModePatches"

namespace
{
	/** Moller-Trumbore. Returns the ray parameter, or a negative value on a miss. */
	double RayTriangle(const FVector& Origin, const FVector& Direction,
		const FVector& A, const FVector& B, const FVector& C)
	{
		const FVector E1 = B - A;
		const FVector E2 = C - A;
		const FVector P = FVector::CrossProduct(Direction, E2);
		const double Det = FVector::DotProduct(E1, P);

		// Two-sided: a patch is a surface, and you can be behind it.
		if (FMath::Abs(Det) < 1.e-9)
		{
			return -1.0;
		}

		const double Inv = 1.0 / Det;
		const FVector T = Origin - A;
		const double U = FVector::DotProduct(T, P) * Inv;
		if (U < 0.0 || U > 1.0)
		{
			return -1.0;
		}

		const FVector Q = FVector::CrossProduct(T, E1);
		const double V = FVector::DotProduct(Direction, Q) * Inv;
		if (V < 0.0 || U + V > 1.0)
		{
			return -1.0;
		}

		const double Distance = FVector::DotProduct(E2, Q) * Inv;
		return Distance > 0.0 ? Distance : -1.0;
	}

	/**
	 * Perpendicular distance from a point to a ray, divided by the distance
	 * along it. That ratio is constant for a fixed screen size, so one
	 * threshold works at any zoom without projecting anything.
	 */
	bool PointNearRay(const FVector& Point, const FVector& Origin, const FVector& Direction,
		double& OutAlong, double& OutScreenish)
	{
		const FVector ToPoint = Point - Origin;
		OutAlong = FVector::DotProduct(ToPoint, Direction);
		if (OutAlong <= 0.0)
		{
			return false;
		}

		const double Perp = (ToPoint - Direction * OutAlong).Length();
		OutScreenish = Perp / OutAlong;
		return true;
	}
}

void USurfEdMode::SetSubMode(ESurfSubMode InSubMode)
{
	if (SubMode == InSubMode)
	{
		return;
	}

	const ESurfSubMode Was = SubMode;
	SubMode = InSubMode;

	// Several patches from 4 go into the edit together (S191a).
	if (Was == ESurfSubMode::Object && SubMode != ESurfSubMode::Object)
	{
		EditSet = PatchSelection.Num() > 1 ? PatchSelection : TArray<FSurfPatchRef>();
	}
	const TArray<FSurfPatchRef> WasEditing = EditSet;
	if (SubMode == ESurfSubMode::Object)
	{
		EditSet.Reset();
	}

	/**
	 * The selection follows the kind (S165a): "switching from mode 1 to 3
	 * doesnt convert the selected vert to faces selection". Points or edges
	 * to faces: every cell whose four corners are picked is picked; faces
	 * to points or edges: the corners stay picked, as they already were.
	 * Whole pieces (4) keeps the patch and drops the rest, as before.
	 */
	if (SubMode == ESurfSubMode::Object)
	{
		ClearPointSelection();
	}
	else if (SubMode == ESurfSubMode::Face)
	{
		if (Was != ESurfSubMode::Face)
		{
			QuadSelection = QuadsHeldByPoints();
			SyncQuadSelectionToPoints();
		}
	}
	else if (Was == ESurfSubMode::Face)
	{
		QuadSelection.Reset();
	}
	else if (Was == ESurfSubMode::Object)
	{
		ClearPointSelection();
	}

	// Back in 4, the patches edited together stay picked together.
	if (SubMode == ESurfSubMode::Object && WasEditing.Num() > 1)
	{
		PatchSelection = WasEditing;
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

TArray<FSurfQuadRef> USurfEdMode::QuadsHeldByPoints() const
{
	TArray<FSurfQuadRef> Out;
	TSet<FIntPoint> Picked;
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid PatchId;
	auto Flush = [&]()
	{
		const ASurfGroup* G = Group.Get();
		const FSurfPatch* Patch = G ? G->FindPatch(PatchId) : nullptr;
		if (Patch)
		{
			for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
			{
				for (int32 X = 0; X + 1 < Patch->Width; ++X)
				{
					if (Picked.Contains(FIntPoint(X, Y)) && Picked.Contains(FIntPoint(X + 1, Y))
						&& Picked.Contains(FIntPoint(X, Y + 1)) && Picked.Contains(FIntPoint(X + 1, Y + 1)))
					{
						FSurfQuadRef Quad;
						Quad.Group = Group;
						Quad.PatchId = PatchId;
						Quad.X = X;
						Quad.Y = Y;
						Out.Add(Quad);
					}
				}
			}
		}
		Picked.Reset();
	};
	// Per patch, in the order the points come: gather one patch's points,
	// then read its cells.
	TArray<FSurfPointRef> Sorted = PointSelection;
	Sorted.StableSort([](const FSurfPointRef& A, const FSurfPointRef& B)
	{
		return A.PatchId < B.PatchId;
	});
	for (const FSurfPointRef& Ref : Sorted)
	{
		if (Ref.PatchId != PatchId || Ref.Group != Group)
		{
			if (Picked.Num() > 0)
			{
				Flush();
			}
			Group = Ref.Group;
			PatchId = Ref.PatchId;
		}
		Picked.Add(FIntPoint(Ref.X, Ref.Y));
	}
	if (Picked.Num() > 0)
	{
		Flush();
	}
	return Out;
}

void USurfEdMode::AdoptKind(ESurfSubMode Kind)
{
	// Not SetSubMode: this is the click following the hand, not the hand
	// choosing a kind, so it must not count as a choice - the choice is
	// what 1, 2 and 3 are for.
	if (SubMode == Kind)
	{
		return;
	}

	const bool bWasVertex = SubMode == ESurfSubMode::Vertex;
	SubMode = Kind;

	if (bWasVertex)
	{
		ClearPointSelection();
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

bool USurfEdMode::IsAutoKind() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	return !Settings || Settings->bClickDecidesKind;
}

void USurfEdMode::SetAutoKind(bool bOn)
{
	if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
	{
		if (Settings->bClickDecidesKind != bOn)
		{
			Settings->bClickDecidesKind = bOn;
			Settings->SaveConfig();
			SurfSession::Verb(TEXT("click_decides"), bOn ? TEXT("on") : TEXT("off"));
		}
	}
}

void USurfEdMode::ToggleKindLock(ESurfSubMode Kind)
{
	// Pressing the kind you are already locked to lets go of it: one key,
	// both ways, so locking is never a trap.
	if (!IsAutoKind() && SubMode == Kind)
	{
		SetAutoKind(true);
		return;
	}
	SetAutoKind(false);
	SetSubMode(Kind);
}

void USurfEdMode::EnterEditKinds()
{
	SetAutoKind(true);
	if (SubMode == ESurfSubMode::Object)
	{
		SetSubMode(ESurfSubMode::Face);
	}
}

bool USurfEdMode::KindUnderCursor(const FSurfPickContext& Ctx, ESurfSubMode& OutKind) const
{
	// Tightest target first: a control point is a few pixels across, an
	// edge is a line, a face is everything else. Anything else and the
	// hand would never reach a point without aiming past a face.
	FSurfPointRef Point;
	if (PickPointWith(Ctx, Point))
	{
		OutKind = ESurfSubMode::Vertex;
		return true;
	}

	FSurfEdgeHit Edge;
	if (NearestEdgeUnder(Ctx, Edge))
	{
		OutKind = ESurfSubMode::Edge;
		return true;
	}

	FSurfQuadRef Quad;
	if (PickPatchQuadWith(Ctx, Quad))
	{
		OutKind = ESurfSubMode::Face;
		return true;
	}

	return false;
}

void USurfEdMode::SelectPatch(const FSurfPatchRef& Patch, bool bKeepPoints)
{
	SelectedPatch = Patch;

	if (bKeepPoints)
	{
		// The points stay, so the patches they span stay selected too, and
		// this one joins them: the selection is every patch it reaches into.
		// Resetting to the new patch alone left points on patches that were
		// no longer "selected", and the next thing that asked which patches
		// were selected dropped them.
		if (Patch.IsValid() && !PatchSelection.Contains(Patch))
		{
			PatchSelection.Add(Patch);
		}
	}
	else
	{
		// One patch again, whatever was picked before - and no brush either
		// (S151b): a plain pick means this and nothing else.
		PatchSelection.Reset();
		EditSet.Reset();
		if (Patch.IsValid())
		{
			PatchSelection.Add(Patch);
			Selection.Reset();
			ActiveFace = FSurfFaceRef();
		}
		ClearPointSelection();
	}
	UpdateOrbitPivot();

	// The editor's own actor selection draws an orange outline round the
	// whole group - every patch in it - which read as "everything is
	// selected" whenever a click had also landed on the actor. Our selection
	// is the patch; the actor is never selected while the mode is up.
	if (GEditor && GEditor->GetSelectedActorCount() > 0)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

void USurfEdMode::AddPatchToSelection(const FSurfPatchRef& Patch, bool bToggle)
{
	if (!Patch.IsValid())
	{
		return;
	}

	if (PatchSelection.Num() == 0 && SelectedPatch.IsValid())
	{
		PatchSelection.Add(SelectedPatch);
	}

	const int32 Existing = PatchSelection.IndexOfByKey(Patch);
	if (Existing != INDEX_NONE && bToggle)
	{
		PatchSelection.RemoveAt(Existing);
		SelectedPatch = PatchSelection.Num() > 0 ? PatchSelection.Last() : FSurfPatchRef();
	}
	else
	{
		if (Existing == INDEX_NONE)
		{
			PatchSelection.Add(Patch);
		}
		SelectedPatch = Patch;
	}

	// Points belong to the patches they are on; a patch leaving the
	// selection takes its points with it.
	for (int32 I = PointSelection.Num() - 1; I >= 0; --I)
	{
		FSurfPatchRef Key;
		Key.Group = PointSelection[I].Group;
		Key.PatchId = PointSelection[I].PatchId;
		if (!PatchSelection.Contains(Key))
		{
			PointSelection.RemoveAt(I);
		}
	}
	for (int32 I = QuadSelection.Num() - 1; I >= 0; --I)
	{
		FSurfPatchRef Key;
		Key.Group = QuadSelection[I].Group;
		Key.PatchId = QuadSelection[I].PatchId;
		if (!PatchSelection.Contains(Key))
		{
			QuadSelection.RemoveAt(I);
		}
	}

	UpdateOrbitPivot();

	if (GEditor && GEditor->GetSelectedActorCount() > 0)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	}

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("patches %d selected"), PatchSelection.Num()));
}

bool USurfEdMode::SelectAllPatches()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	PatchSelection.Reset();
	ClearPointSelection();

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch.Id;
			PatchSelection.Add(Ref);
		});
	}

	if (PatchSelection.Num() > 0)
	{
		SelectedPatch = PatchSelection.Last();
	}

	// And every primitive, whole (S170a): "A or ctrl A isnt selecting all
	// in the scene - some boxes remain unselected" - in 4 all means every
	// piece of ours, patch or primitive.
	Selection.Reset();
	int32 Brushes = 0;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			for (int32 I = 0; I < Brush.Faces.Num(); ++I)
			{
				FSurfFaceRef Ref;
				Ref.Group = Group;
				Ref.BrushId = Brush.Id;
				Ref.FaceIndex = I;
				Selection.Add(Ref);
			}
			++Brushes;
		});
	}
	ActiveFace = Selection.Num() > 0 ? Selection.Last() : FSurfFaceRef();

	UpdateOrbitPivot();

	if (GEditor && GEditor->GetSelectedActorCount() > 0)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	}

	if (Inspector)
	{
		Inspector->SetResult(Brushes > 0
			? FString::Printf(TEXT("%d patches, %d primitives selected"), PatchSelection.Num(), Brushes)
			: FString::Printf(TEXT("%d patches selected"), PatchSelection.Num()));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("select_all patches=%d brushes=%d"), PatchSelection.Num(), Brushes));

	// Claimed even with nothing to select, so the editor never takes every
	// actor in the level instead.
	return true;
}

TArray<FSurfPatchRef> USurfEdMode::GetSelectedPatches() const
{
	TArray<FSurfPatchRef> Out;
	for (const FSurfPatchRef& Ref : PatchSelection)
	{
		if (Ref.IsValid())
		{
			Out.Add(Ref);
		}
	}
	if (Out.Num() == 0 && SelectedPatch.IsValid())
	{
		Out.Add(SelectedPatch);
	}
	return Out;
}

TArray<FSurfPatchRef> USurfEdMode::GetAllPatches() const
{
	TArray<FSurfPatchRef> Out;
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<ASurfGroup> It(const_cast<UWorld*>(World)); It; ++It)
		{
			ASurfGroup* Group = *It;
			if (!Group || !IsOpen(Group))
			{
				continue;
			}
			Group->ForEachPatch([&](const FSurfPatch& Patch)
			{
				FSurfPatchRef Ref;
				Ref.Group = Group;
				Ref.PatchId = Patch.Id;
				Out.Add(Ref);
			});
		}
	}
	return Out;
}

void USurfEdMode::SelectQuadRowOrColumn(const FSurfQuadRef& Quad, bool bAdd)
{
	const ASurfGroup* Group = Quad.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Quad.PatchId) : nullptr;
	if (!Patch || !Quad.IsValid())
	{
		return;
	}

	// The same cycle as a point: the row of faces, then the column, then the
	// one face. Adding takes the previous stage back out first, so the cycle
	// swaps rather than piles up.
	const bool bSameFace = LastRowColumnQuad == Quad;
	const int32 Stage = bSameFace ? (QuadRowColumnStage + 1) % 3 : 0;

	auto FacesOfStage = [&](int32 Which, TArray<FSurfQuadRef>& OutFaces)
	{
		if (Which == 0)
		{
			for (int32 X = 0; X + 1 < Patch->Width; ++X)
			{
				FSurfQuadRef Q = Quad;
				Q.X = X;
				OutFaces.Add(Q);
			}
		}
		else if (Which == 1)
		{
			for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
			{
				FSurfQuadRef Q = Quad;
				Q.Y = Y;
				OutFaces.Add(Q);
			}
		}
		else
		{
			OutFaces.Add(Quad);
		}
	};

	if (SelectedPatch.Group != Quad.Group || SelectedPatch.PatchId != Quad.PatchId)
	{
		FSurfPatchRef Ref;
		Ref.Group = Quad.Group;
		Ref.PatchId = Quad.PatchId;
		if (bAdd)
		{
			AddPatchToSelection(Ref, /*bToggle*/ false);
		}
		else
		{
			SelectPatch(Ref);
		}
	}

	if (!bAdd)
	{
		QuadSelection.Reset();
	}
	else if (bSameFace)
	{
		// Only what this cycle put in comes back out. Recomputing the stage
		// took out faces that had been picked some other way and happened
		// to fall on the same line (S84a).
		for (const FSurfQuadRef& Q : LastRowColumnFaces)
		{
			QuadSelection.Remove(Q);
		}
	}

	TArray<FSurfQuadRef> Faces;
	FacesOfStage(Stage, Faces);
	for (const FSurfQuadRef& Q : Faces)
	{
		QuadSelection.AddUnique(Q);
	}

	LastRowColumnQuad = Quad;
	QuadRowColumnStage = Stage;
	LastRowColumnFaces = Faces;

	SyncQuadSelectionToPoints();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("faces %s of %d,%d (%d selected)"),
		Stage == 0 ? TEXT("row") : (Stage == 1 ? TEXT("column") : TEXT("one")), Quad.X, Quad.Y, QuadSelection.Num()));
}

bool USurfEdMode::PickPatchQuadWith(const FSurfPickContext& Ctx, FSurfQuadRef& Out, FVector* OutWorldHit) const
{
	UWorld* World = Ctx.World;
	if (!World)
	{
		return false;
	}

	const FVector Origin = Ctx.Origin;
	const FVector Direction = Ctx.Direction;

	double BestDistance = TNumericLimits<double>::Max();
	bool bFound = false;

	// The control net, not the tessellated surface. What is dragged is what
	// is drawn, and the drawn thing is the cage - picking the tessellation
	// would highlight a cell whose corners are somewhere else.
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		const FVector LocalOrigin = ToWorld.InverseTransformPosition(Origin);
		const FVector LocalDirection = ToWorld.InverseTransformVector(Direction).GetSafeNormal();

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (!Patch.HasValidDimensions())
			{
				return;
			}

			for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
			{
				for (int32 X = 0; X + 1 < Patch.Width; ++X)
				{
					const FVector& A = Patch.At(X, Y).Pos;
					const FVector& B = Patch.At(X + 1, Y).Pos;
					const FVector& C = Patch.At(X + 1, Y + 1).Pos;
					const FVector& D = Patch.At(X, Y + 1).Pos;

					// Both triangles, both ways round: a cell is pickable from
					// whichever side it is being looked at.
					double Distance = RayTriangle(LocalOrigin, LocalDirection, A, B, C);
					if (Distance < 0.0)
					{
						Distance = RayTriangle(LocalOrigin, LocalDirection, A, C, B);
					}
					if (Distance < 0.0)
					{
						Distance = RayTriangle(LocalOrigin, LocalDirection, A, C, D);
					}
					if (Distance < 0.0)
					{
						Distance = RayTriangle(LocalOrigin, LocalDirection, A, D, C);
					}

					if (Distance < 0.0 || Distance >= BestDistance)
					{
						continue;
					}

					BestDistance = Distance;
					bFound = true;

					Out.Group = Group;
					Out.PatchId = Patch.Id;
					Out.X = X;
					Out.Y = Y;

					if (OutWorldHit)
					{
						*OutWorldHit = ToWorld.TransformPosition(LocalOrigin + LocalDirection * Distance);
					}
				}
			}
		});
	}

	return bFound;
}

void USurfEdMode::SyncPatchSelectionToPoints()
{
	// The patches the selection spans, in the order it reached them, with
	// the active patch first when it has anything. Kept in step so that
	// everything asking which patches are selected - the cage, select all,
	// join, delete, the clipboard - sees the same set the points do.
	TArray<FSurfPatchRef> Spanned;
	if (SelectedPatch.IsValid())
	{
		Spanned.Add(SelectedPatch);
	}
	for (const FSurfPointRef& Ref : PointSelection)
	{
		FSurfPatchRef Key;
		Key.Group = Ref.Group;
		Key.PatchId = Ref.PatchId;
		if (Key.IsValid() && !Spanned.Contains(Key))
		{
			Spanned.Add(Key);
		}
	}
	for (const FSurfQuadRef& Quad : QuadSelection)
	{
		FSurfPatchRef Key;
		Key.Group = Quad.Group;
		Key.PatchId = Quad.PatchId;
		if (Key.IsValid() && !Spanned.Contains(Key))
		{
			Spanned.Add(Key);
		}
	}
	// The patches edited together stay in the selection (S191a).
	if (SubMode != ESurfSubMode::Object)
	{
		for (const FSurfPatchRef& Ref : EditSet)
		{
			if (Ref.IsValid() && !Spanned.Contains(Ref))
			{
				Spanned.Add(Ref);
			}
		}
	}
	if (Spanned.Num() > 0)
	{
		PatchSelection = MoveTemp(Spanned);
	}
}

void USurfEdMode::SyncQuadSelectionToPoints()
{
	PointSelection.Reset();

	for (const FSurfQuadRef& Quad : QuadSelection)
	{
		const ASurfGroup* Group = Quad.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Quad.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}

		// The four corners. A face is moved by moving them, which is how the
		// gizmo, the modal transforms and soft selection all keep working
		// without being taught what a face is.
		for (int32 DY = 0; DY <= 1; ++DY)
		{
			for (int32 DX = 0; DX <= 1; ++DX)
			{
				FSurfPointRef Ref;
				Ref.Group = Quad.Group;
				Ref.PatchId = Quad.PatchId;
				Ref.X = Quad.X + DX;
				Ref.Y = Quad.Y + DY;

				if (Ref.X < Patch->Width && Ref.Y < Patch->Height && !PointSelection.Contains(Ref))
				{
					PointSelection.Add(Ref);
				}
			}
		}
	}

	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	RefreshSoftSelection();
	UpdateOrbitPivot();
}

bool USurfEdMode::SelectQuadRectTo(const FSurfQuadRef& To)
{
	if (!To.IsValid() || !QuadPathAnchor.IsValid()
		|| QuadPathAnchor.Group != To.Group || QuadPathAnchor.PatchId != To.PatchId)
	{
		return false;
	}

	ASurfGroup* Group = To.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(To.PatchId) : nullptr;
	if (!Patch || !Patch->HasValidDimensions())
	{
		return false;
	}

	// Two corners name a rectangle, whichever way round they were clicked.
	const int32 X0 = FMath::Clamp(FMath::Min(QuadPathAnchor.X, To.X), 0, Patch->Width - 2);
	const int32 X1 = FMath::Clamp(FMath::Max(QuadPathAnchor.X, To.X), 0, Patch->Width - 2);
	const int32 Y0 = FMath::Clamp(FMath::Min(QuadPathAnchor.Y, To.Y), 0, Patch->Height - 2);
	const int32 Y1 = FMath::Clamp(FMath::Max(QuadPathAnchor.Y, To.Y), 0, Patch->Height - 2);

	FSurfPatchRef Ref;
	Ref.Group = To.Group;
	Ref.PatchId = To.PatchId;
	if (SelectedPatch.Group != To.Group || SelectedPatch.PatchId != To.PatchId)
	{
		AddPatchToSelection(Ref, /*bToggle*/ false);
	}

	int32 Added = 0;
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			FSurfQuadRef Quad;
			Quad.Group = To.Group;
			Quad.PatchId = To.PatchId;
			Quad.X = X;
			Quad.Y = Y;
			if (QuadSelection.IndexOfByKey(Quad) == INDEX_NONE)
			{
				QuadSelection.Add(Quad);
				++Added;
			}
		}
	}
	QuadPathAnchor = To;

	SyncQuadSelectionToPoints();
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%d face%s between - %d selected"),
			Added, Added == 1 ? TEXT("") : TEXT("s"), QuadSelection.Num()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Selection(FString::Printf(TEXT("face_rect added=%d total=%d"), Added, QuadSelection.Num()));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

void USurfEdMode::SelectQuad(const FSurfQuadRef& Quad, bool bAddToSelection)
{
	if (!Quad.IsValid())
	{
		return;
	}

	// Every face picked is the one a Ctrl-click measures the block from.
	QuadPathAnchor = Quad;

	// A plain pick ends the row/column cycle: the next Alt-click on the
	// same face starts a fresh line rather than taking one back out (S84a).
	LastRowColumnQuad = FSurfQuadRef();
	LastRowColumnFaces.Reset();

	// A face lives on a patch, and the verbs act on the selected patches -
	// so picking a face on another one selects that patch too: alone for a
	// plain click, alongside the others when adding.
	if (SelectedPatch.Group != Quad.Group || SelectedPatch.PatchId != Quad.PatchId)
	{
		FSurfPatchRef Ref;
		Ref.Group = Quad.Group;
		Ref.PatchId = Quad.PatchId;
		if (bAddToSelection)
		{
			AddPatchToSelection(Ref, /*bToggle*/ false);
		}
		else
		{
			SelectPatch(Ref);
			QuadSelection.Reset();
		}
	}

	const int32 Existing = QuadSelection.IndexOfByKey(Quad);

	if (!bAddToSelection)
	{
		QuadSelection.Reset();
		QuadSelection.Add(Quad);
	}
	else if (Existing == INDEX_NONE)
	{
		QuadSelection.Add(Quad);
	}
	else
	{
		QuadSelection.RemoveAt(Existing);
	}

	SyncQuadSelectionToPoints();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("face %d,%d (%d selected)"),
		Quad.X, Quad.Y, QuadSelection.Num()));
}

bool USurfEdMode::SelectAllBrushFaces()
{
	// The brushes already being worked on, else every brush there is: the
	// same rule the patch Select All follows, so A always means "all of
	// what I am in".
	TArray<FSurfFaceRef> Brushes = BrushesHeldBySelection();
	if (Brushes.Num() == 0)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				ASurfGroup* Group = *It;
				if (!Group || !IsOpen(Group))
				{
					continue;
				}
				Group->ForEachBrush([&Brushes, Group](const FSurfBrush& Brush)
				{
					FSurfFaceRef Ref;
					Ref.Group = Group;
					Ref.BrushId = Brush.Id;
					Ref.FaceIndex = 0;
					Brushes.Add(Ref);
				});
			}
		}
	}
	if (Brushes.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("select all: there is no primitive in the level"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("select_all_no_brush"));
		return true;
	}

	Selection.Reset();
	int32 Faces = 0;
	for (const FSurfFaceRef& Each : Brushes)
	{
		ASurfGroup* Group = Each.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Each.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		for (int32 I = 0; I < Brush->Faces.Num(); ++I)
		{
			FSurfFaceRef Ref;
			Ref.Group = Group;
			Ref.BrushId = Each.BrushId;
			Ref.FaceIndex = I;
			Selection.AddUnique(Ref);
			++Faces;
		}
		ActiveFace = Selection.Num() > 0 ? Selection.Last() : FSurfFaceRef();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%d face%s of %d primitive%s selected"),
			Faces, Faces == 1 ? TEXT("") : TEXT("s"), Brushes.Num(), Brushes.Num() == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Selection(FString::Printf(TEXT("select_all_faces faces=%d brushes=%d"), Faces, Brushes.Num()));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::SelectAllQuads()
{
	// All the faces of every patch that is selected - or of every patch in
	// the level when none is, so A always selects something: "click a patch,
	// press A, nothing" was A refusing for want of an active patch.
	TArray<FSurfPatchRef> Patches = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Patches.AddUnique(Held);
	}
	if (Patches.Num() == 0)
	{
		Patches = GetAllPatches();
	}
	if (Patches.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("select all: there is no patch in the level"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("select_all_no_patch"));
		return true;
	}

	QuadSelection.Reset();

	for (const FSurfPatchRef& Each : Patches)
	{
		const ASurfGroup* Group = Each.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Each.PatchId) : nullptr;
		if (!Patch || !Patch->HasValidDimensions())
		{
			continue;
		}
		for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
		{
			for (int32 X = 0; X + 1 < Patch->Width; ++X)
			{
				FSurfQuadRef Quad;
				Quad.Group = Each.Group;
				Quad.PatchId = Patch->Id;
				Quad.X = X;
				Quad.Y = Y;
				QuadSelection.Add(Quad);
			}
		}
	}

	if (!SelectedPatch.IsValid())
	{
		SelectedPatch = Patches.Last();
	}
	SyncQuadSelectionToPoints();
	SyncPatchSelectionToPoints();
	RefreshSoftSelection();

	SurfSession::Selection(FString::Printf(TEXT("select_all faces=%d patches=%d"), QuadSelection.Num(), Patches.Num()));

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("selected every face: %d on %d patch%s"), QuadSelection.Num(), Patches.Num(), Patches.Num() == 1 ? TEXT("") : TEXT("es")));
		Inspector->RefreshSelectionCount();
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return QuadSelection.Num() > 0;
}

ASurfGroup* USurfEdMode::GroupForNewGeometry()
{
	// Inside a prefab, into the prefab (S71a): a wall made while editing a
	// door frame is the door frame's, not the level's.
	if (IsInsidePrefab())
	{
		if (ASurfGroup* Room = GetDrilledGroup())
		{
			LastGroupUsed = Room;
			return Room;
		}
	}

	if (ASurfGroup* Group = GetActiveFace().Group.Get())
	{
		LastGroupUsed = Group;
		return Group;
	}

	if (ASurfGroup* Group = GetSelectedPatch().Group.Get())
	{
		LastGroupUsed = Group;
		return Group;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	/**
	 * Nothing of ours is being worked on - so the one already being built
	 * in, if there is one (S140a).
	 *
	 * S76c was right that reaching for **whichever group stood first in the
	 * world** is wrong: it put new geometry inside something else entirely,
	 * with that group's transform, visibility and bake, chosen by iteration
	 * order rather than by anything the hand did. But the cure never reused
	 * anything at all, and an outliner with a SurfGroup per piece - each one
	 * labelled with the level's name in the viewport - is the bill for that.
	 *
	 * Remembering is the difference. A group we made or last worked in is a
	 * group the hand chose; a group found by walking the world is not.
	 */
	ASurfGroup* Last = LastGroupUsed.Get();
	if (IsValid(Last) && Last->GetWorld() == World
		&& Last->GetLevel() == World->GetCurrentLevel() && IsOpen(Last))
	{
		return Last;
	}

	/**
	 * Then the group this level made for itself (S149b): "Cant there be only
	 * one?"
	 *
	 * The memory above lives on the mode and is gone by the next editor run,
	 * so without this the first piece of a session started a new group - one
	 * a session, all of them wearing the level's name. A marked group is one
	 * the tool made for this level, which is a choice; the first actor an
	 * iterator hands back is not, and that is what S140a refused. The oldest
	 * mark wins so the answer does not depend on iteration order at all.
	 */
	ASurfGroup* Home = nullptr;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!IsValid(Group) || !Group->bLevelGroup || Group->GetLevel() != World->GetCurrentLevel())
		{
			continue;
		}
		if (!IsOpen(Group))
		{
			continue;
		}
		if (!Home || Group->GroupId < Home->GroupId)
		{
			Home = Group;
		}
	}
	if (Home)
	{
		LastGroupUsed = Home;
		return Home;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Named after the level it is the geometry of (S202a), at birth: a label
	// set afterwards reruns the actor's construction. The parameter is a
	// view (S206a): the name has to outlive the spawn, or the label is read
	// from freed memory - "levelname was created with weird chinese caracters".
	FString LevelLabel;
	if (const ULevel* Level = World->GetCurrentLevel())
	{
		LevelLabel = FPackageName::GetShortName(Level->GetOutermost()->GetName());
		Params.InitialActorLabel = LevelLabel;
	}
	ASurfGroup* Made = World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), FTransform::Identity, Params);
	if (Made)
	{
		Made->bLevelGroup = true;
	}
	LastGroupUsed = Made;
	return Made;
}

void USurfEdMode::ClearPointSelection()
{
	// The face selection is the point selection, seen from further out.
	// Leaving faces behind when the points go makes the count lie and the
	// highlight outlive what it was highlighting.
	QuadSelection.Reset();

	PointSelection.Reset();
	BrushCornerSelection.Reset();
	SoftInfluence.Reset();
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	LastEdgePickAxis = 0;
}

void USurfEdMode::AddPointToSelection(const FSurfPointRef& Point)
{
	if (!Point.IsValid() || PointSelection.Contains(Point))
	{
		return;
	}

	PointSelection.Add(Point);
	RefreshSoftSelection();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

void USurfEdMode::RemovePointFromSelection(const FSurfPointRef& Point)
{
	const int32 Removed = PointSelection.Remove(Point);
	if (Removed == 0)
	{
		return;
	}
	RefreshSoftSelection();
	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
	RedrawViewports();
}

void USurfEdMode::SelectPoint(const FSurfPointRef& Point, bool bAddToSelection)
{
	if (!Point.IsValid())
	{
		return;
	}

	// A point on another patch makes that patch the active one. Adding keeps
	// what was selected on the previous patch, which is how a selection comes
	// to span two; a plain click starts over on the new one.
	if (Point.Group != SelectedPatch.Group || Point.PatchId != SelectedPatch.PatchId)
	{
		FSurfPatchRef Ref;
		Ref.Group = Point.Group;
		Ref.PatchId = Point.PatchId;
		SelectPatch(Ref, /*bKeepPoints*/ bAddToSelection || InEditSet(Ref));
	}

	if (bAddToSelection)
	{
		const int32 Existing = PointSelection.IndexOfByKey(Point);
		if (Existing != INDEX_NONE)
		{
			PointSelection.RemoveAt(Existing);
		}
		else
		{
			PointSelection.Add(Point);
		}
	}
	else
	{
		PointSelection.Reset();
		PointSelection.Add(Point);
	}

	// Stage 2 is the single-point stage, so the next shift-click on this same
	// point advances to stage 0 and takes the row.
	LastRowColumnPoint = Point;
	RowColumnStage = 2;
	PathAnchor = Point;

	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

bool USurfEdMode::SelectGridPathTo(const FSurfPointRef& To)
{
	const ASurfGroup* Group = To.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(To.PatchId) : nullptr;
	if (!Patch || !PathAnchor.IsValid() || PathAnchor.Group.Get() != Group || PathAnchor.PatchId != To.PatchId)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("path: click a point first, then Ctrl+Shift-click another on the same patch"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("path_no_anchor"));
		return false;
	}

	// The staircase from the anchor to the point, one grid step at a time,
	// each step along the axis that is further behind: a run when they
	// share a row or column, the nearest line across the grid otherwise -
	// and every step is an edge, so edge sub-mode gets a connected run.
	int32 X = PathAnchor.X;
	int32 Y = PathAnchor.Y;
	const int32 DX = FMath::Abs(To.X - X);
	const int32 DY = FMath::Abs(To.Y - Y);
	const int32 SX = To.X > X ? 1 : -1;
	const int32 SY = To.Y > Y ? 1 : -1;
	int32 StepsX = 0;
	int32 StepsY = 0;
	int32 Added = 0;

	auto Take = [&](int32 PX, int32 PY)
	{
		FSurfPointRef Ref;
		Ref.Group = To.Group;
		Ref.PatchId = To.PatchId;
		Ref.X = PX;
		Ref.Y = PY;
		if (!PointSelection.Contains(Ref))
		{
			PointSelection.Add(Ref);
			++Added;
		}
	};

	Take(X, Y);
	while (X != To.X || Y != To.Y)
	{
		// Which axis has covered the smaller share of its distance.
		const bool bStepX = X != To.X
			&& (Y == To.Y || (StepsX + 1) * DY <= (StepsY + 1) * DX);
		if (bStepX)
		{
			X += SX;
			++StepsX;
		}
		else
		{
			Y += SY;
			++StepsY;
		}
		Take(X, Y);
	}

	if (SelectedPatch.Group.Get() != Group || SelectedPatch.PatchId != To.PatchId)
	{
		FSurfPatchRef Ref;
		Ref.Group = To.Group;
		Ref.PatchId = To.PatchId;
		SelectPatch(Ref, /*bKeepPoints*/ true);
	}
	PathAnchor = To;
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	SyncPatchSelectionToPoints();
	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Selection(FString::Printf(TEXT("path of %d points (%d new) to %d,%d"), DX + DY + 1, Added, To.X, To.Y));
	return true;
}

bool USurfEdMode::NearestEdgeUnder(const FSurfPickContext& Ctx, FSurfEdgeHit& Out) const
{
	if (!Ctx.IsValid() || !Ctx.World)
	{
		return false;
	}

	const FVector2D Mouse = Ctx.Pixel;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Tolerance = Settings ? Settings->EdgePickRadiusPx : 14.0;

	/**
	 * The nearest segment of one patch to the cursor, measured on screen, in
	 * pixels, against the segment itself.
	 *
	 * The first version scored a segment by the angle between the click ray
	 * and the segment's midpoint, which meant a long edge seen end-on was
	 * unreachable, a short edge far away was easier to hit than a long one
	 * nearby, and the tolerance was a ratio nobody could reason about. How
	 * close the cursor is to the line you can see is the whole question.
	 */
	using FCandidate = FSurfEdgeHit;

	auto Scan = [&](ASurfGroup* Group, const FSurfPatch* Patch) -> FCandidate
	{
		FCandidate Best;
		if (!Group || !Patch || !Patch->HasValidDimensions())
		{
			return Best;
		}
		Best.Group = Group;
		Best.PatchId = Patch->Id;

		const FTransform ToWorld = Group->GetActorTransform();

		auto ScreenOf = [&](int32 X, int32 Y, FVector2D& Out) -> bool
		{
			return Ctx.WorldToPixel(ToWorld.TransformPosition(Patch->At(X, Y).Pos), Out);
		};

		auto ConsiderAt = [&](const FVector2D& A, const FVector2D& B, bool bColumn, int32 Line, int32 Index, const FVector& WorldMid)
		{
			// The segment's middle has to be in view: a segment behind a
			// surface is one the hand cannot mean, unless the view is x-ray.
			{
				FSurfPointRef Self;
				Self.Group = Group;
				Self.PatchId = Patch->Id;
				Self.X = bColumn ? Line : Index;
				Self.Y = bColumn ? Index : Line;
				if (!IsPointVisibleFrom(Ctx.Origin, WorldMid, &Self))
				{
					return;
				}
			}

			const FVector2D Span = B - A;
			const double Length = Span.SizeSquared();

			const double T = Length > SMALL_NUMBER
				? FMath::Clamp(FVector2D::DotProduct(Mouse - A, Span) / Length, 0.0, 1.0)
				: 0.0;

			const double Distance = FVector2D::Distance(Mouse, A + Span * T);

			if (Distance < Best.Distance)
			{
				Best.Distance = Distance;
				Best.bColumn = bColumn;
				Best.Line = Line;
				Best.Along = Index;
			}
		};
		auto Consider = [&](const FVector2D& A, const FVector2D& B, bool bColumn, int32 Line, int32 Index)
		{
			const FVector P0 = ToWorld.TransformPosition((bColumn ? Patch->At(Line, Index) : Patch->At(Index, Line)).Pos);
			const FVector P1 = ToWorld.TransformPosition((bColumn ? Patch->At(Line, Index + 1) : Patch->At(Index + 1, Line)).Pos);
			ConsiderAt(A, B, bColumn, Line, Index, (P0 + P1) * 0.5);
		};

		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				FVector2D Here;
				if (!ScreenOf(X, Y, Here))
				{
					continue;
				}

				FVector2D Next;

				if (X + 1 < Patch->Width && ScreenOf(X + 1, Y, Next))
				{
					Consider(Here, Next, /*bColumn*/ false, Y, X);
				}

				if (Y + 1 < Patch->Height && ScreenOf(X, Y + 1, Next))
				{
					Consider(Here, Next, /*bColumn*/ true, X, Y);
				}
			}
		}

		/**
		 * The curve as well as its net (S245a): a curved patch draws its
		 * control net straight between the points, but what the eye follows
		 * is the surface's own edge - the quadratic through each three
		 * points, which on a bent primitive runs well inside the net. The
		 * record had 67 clicks land 29-80 px from any net line; a click on
		 * the curve now takes the half of the pair the cursor is nearest.
		 */
		if (Patch->Type == ESurfPatchType::Curve)
		{
			auto Curve = [&](bool bColumn, int32 Line, int32 From)
			{
				const FVector& C0 = (bColumn ? Patch->At(Line, From) : Patch->At(From, Line)).Pos;
				const FVector& C1 = (bColumn ? Patch->At(Line, From + 1) : Patch->At(From + 1, Line)).Pos;
				const FVector& C2 = (bColumn ? Patch->At(Line, From + 2) : Patch->At(From + 2, Line)).Pos;
				auto At = [&](double T) -> FVector
				{
					const double S = 1.0 - T;
					return ToWorld.TransformPosition(C0 * (S * S) + C1 * (2.0 * S * T) + C2 * (T * T));
				};
				FVector2D Screen[5];
				FVector World[5];
				for (int32 I = 0; I < 5; ++I)
				{
					World[I] = At(I * 0.25);
					if (!Ctx.WorldToPixel(World[I], Screen[I]))
					{
						return;
					}
				}
				for (int32 I = 0; I < 4; ++I)
				{
					ConsiderAt(Screen[I], Screen[I + 1], bColumn, Line, I < 2 ? From : From + 1, (World[I] + World[I + 1]) * 0.5);
				}
			};
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X + 2 < Patch->Width; X += 2)
				{
					Curve(/*bColumn*/ false, Y, X);
				}
			}
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				for (int32 Y = 0; Y + 2 < Patch->Height; Y += 2)
				{
					Curve(/*bColumn*/ true, X, Y);
				}
			}
		}

		return Best;
	};

	// The active patch first, then every other patch: an edge on a patch
	// that is not the active one used to be a miss, and the miss fell
	// through to picking the patch - so the first click on a wall's edge
	// selected the wall and showed nothing red.
	ASurfGroup* ActiveGroup = SelectedPatch.Group.Get();
	const FSurfPatch* ActivePatch = ActiveGroup ? ActiveGroup->FindPatch(SelectedPatch.PatchId) : nullptr;

	FCandidate Best = Scan(ActiveGroup, ActivePatch);
	double NearestMiss = Best.IsValid() ? Best.Distance : TNumericLimits<double>::Max();

	if (!Best.IsValid() || Best.Distance > Tolerance)
	{
		for (TActorIterator<ASurfGroup> It(Ctx.World); It; ++It)
		{
			ASurfGroup* Group = *It;
			if (!Group || !IsOpen(Group))
			{
				continue;
			}
			Group->ForEachPatch([&](const FSurfPatch& Candidate)
			{
				if (Group == ActiveGroup && ActivePatch && Candidate.Id == ActivePatch->Id)
				{
					return;
				}
				const FCandidate Here = Scan(Group, &Candidate);
				if (Here.IsValid())
				{
					NearestMiss = FMath::Min(NearestMiss, Here.Distance);
					if (Here.Distance <= Tolerance && (!Best.IsValid() || Here.Distance < Best.Distance || Best.Distance > Tolerance))
					{
						Best = Here;
					}
				}
			});
		}
	}

	if (!Best.IsValid() || Best.Distance > Tolerance)
	{
		// A near miss is friction; a click in empty space is a click in
		// empty space (S153d: a session full of misses hundreds of pixels
		// from any line was a session of deselecting).
		if (NearestMiss <= Tolerance * 3.0)
		{
			SurfSession::Friction(TEXT("edge_click_missed"),
				FString::Printf(TEXT("nearest=%.1fpx tolerance=%.0fpx"), NearestMiss, Tolerance));
		}
		return false;
	}

	Out = Best;
	return true;
}

/** The selecting half, on whatever the measuring half found. */
bool USurfEdMode::SelectEdgeWith(const FSurfPickContext& Ctx, bool bAdd, bool bWholeLine, bool bPath)
{
	FSurfEdgeHit Best;
	if (!NearestEdgeUnder(Ctx, Best))
	{
		return false;
	}

	ASurfGroup* ActiveGroup = SelectedPatch.Group.Get();
	const FSurfPatch* ActivePatch = ActiveGroup ? ActiveGroup->FindPatch(SelectedPatch.PatchId) : nullptr;

	ASurfGroup* Group = Best.Group;
	const FSurfPatch* Patch = Group->FindPatch(Best.PatchId);
	if (!Patch)
	{
		return false;
	}

	// An edge on another patch makes that patch the active one; adding keeps
	// what was selected elsewhere.
	if (Group != ActiveGroup || !ActivePatch || Patch->Id != ActivePatch->Id)
	{
		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Patch->Id;
		SelectPatch(Ref, /*bKeepPoints*/ bAdd || InEditSet(Ref));
	}

	if (!bAdd)
	{
		PointSelection.Reset();
	}

	const int32 Across = Best.bColumn ? Patch->Height : Patch->Width;
	const int32 First = bWholeLine ? 0 : Best.Along;
	const int32 Last  = bWholeLine ? Across - 1 : FMath::Min(Best.Along + 1, Across - 1);

	// Ctrl+Shift: the edges between the last edge clicked and this one -
	// the grid path from the anchor to whichever end of this edge is nearer
	// it, then the edge itself.
	if (bPath && !bWholeLine && PathAnchor.IsValid() && PathAnchor.Group.Get() == Group && PathAnchor.PatchId == Patch->Id)
	{
		FSurfPointRef Near;
		Near.Group = Group;
		Near.PatchId = Patch->Id;
		const int32 AnchorAlong = Best.bColumn ? PathAnchor.Y : PathAnchor.X;
		const int32 NearAlong = FMath::Abs(AnchorAlong - First) <= FMath::Abs(AnchorAlong - Last) ? First : Last;
		Near.X = Best.bColumn ? Best.Line : NearAlong;
		Near.Y = Best.bColumn ? NearAlong : Best.Line;
		SelectGridPathTo(Near);
	}

	for (int32 B = First; B <= Last; ++B)
	{
		FSurfPointRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Patch->Id;
		Ref.X = Best.bColumn ? Best.Line : B;
		Ref.Y = Best.bColumn ? B : Best.Line;

		if (!PointSelection.Contains(Ref))
		{
			PointSelection.Add(Ref);
		}
	}

	// The far end of this edge from the anchor is the next path's start.
	{
		FSurfPointRef Far;
		Far.Group = Group;
		Far.PatchId = Patch->Id;
		Far.X = Best.bColumn ? Best.Line : Last;
		Far.Y = Best.bColumn ? Last : Best.Line;
		PathAnchor = Far;
	}

	SyncPatchSelectionToPoints();
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	RefreshSoftSelection();
	UpdateOrbitPivot();

	// Which way the edge ran, for a verb that needs a line and is handed
	// points: two vertical edges side by side are also two horizontal pairs.
	LastEdgePickAxis = Best.bColumn ? 1 : 2;

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Selection(FString::Printf(TEXT("edge %s %d %s at %.1fpx"),
		Best.bColumn ? TEXT("column") : TEXT("row"), Best.Line,
		bWholeLine ? TEXT("(whole line)") : *FString::Printf(TEXT("(segment %d)"), Best.Along),
		Best.Distance));

	return true;
}

void USurfEdMode::SelectPointRowOrColumn(const FSurfPointRef& Point, bool bAddToSelection)
{
	const ASurfGroup* Group = Point.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(Point.PatchId) : nullptr;
	if (!Patch)
	{
		return;
	}

	if (Point.Group != SelectedPatch.Group || Point.PatchId != SelectedPatch.PatchId)
	{
		FSurfPatchRef Ref;
		Ref.Group = Point.Group;
		Ref.PatchId = Point.PatchId;
		SelectPatch(Ref, /*bKeepPoints*/ bAddToSelection || InEditSet(Ref));
	}

	// The same point again cycles row, then column, then back to the single
	// point. This is the gesture that makes grid editing quick.
	if (LastRowColumnPoint == Point)
	{
		RowColumnStage = (RowColumnStage + 1) % 3;
	}
	else
	{
		RowColumnStage = 0;
	}

	/**
	 * Adding keeps what was there and only takes away what the previous
	 * stage of this same cycle put in - exactly those points, not the whole
	 * row and column through the click (S164a): that took a point of a line
	 * picked earlier that happened to cross them. "i selected a row, then
	 * keep shift alt and click another one twice to get parallel, but the
	 * very first vert isnt part of the final selection. bug"
	 */
	if (!bAddToSelection)
	{
		PointSelection.Reset();
	}
	else if (LastRowColumnPoint == Point)
	{
		for (const FSurfPointRef& Added : RowColumnAdded)
		{
			PointSelection.Remove(Added);
		}
	}
	RowColumnAdded.Reset();

	auto Take = [this](const FSurfPointRef& Ref)
	{
		if (!PointSelection.Contains(Ref))
		{
			PointSelection.Add(Ref);
			RowColumnAdded.Add(Ref);
		}
	};

	if (RowColumnStage == 0)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			FSurfPointRef Ref = Point;
			Ref.X = X;
			Take(Ref);
		}
	}
	else if (RowColumnStage == 1)
	{
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			FSurfPointRef Ref = Point;
			Ref.Y = Y;
			Take(Ref);
		}
	}
	else
	{
		Take(Point);
	}

	LastRowColumnPoint = Point;

	RefreshSoftSelection();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

bool USurfEdMode::PickPatchCyclingWith(const FSurfPickContext& Ctx, FSurfPatchRef& Out, FVector* OutWorldHit)
{
	UWorld* World = Ctx.World;
	if (!World)
	{
		return false;
	}

	// Everything the ray meets, nearest first.
	struct FHit
	{
		FSurfPatchRef Ref;
		FVector World = FVector::ZeroVector;
		double Distance = 0.0;
	};

	TArray<FHit> Hits;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!IsOpen(Group))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			FSurfTessellation Tess;
			if (!SurfPatchMath::Tessellate(Patch, Tess))
			{
				return;
			}

			for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
			{
				const FVector A = ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I]]);
				const FVector B = ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I + 1]]);
				const FVector C = ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I + 2]]);

				const double Distance = RayTriangle(Ctx.Origin, Ctx.Direction, A, B, C);
				if (Distance > 0.0)
				{
					FHit Hit;
					Hit.Ref.Group = Group;
					Hit.Ref.PatchId = Patch.Id;
					Hit.World = Ctx.Origin + Ctx.Direction * Distance;
					Hit.Distance = Distance;
					Hits.Add(Hit);
					return;
				}
			}
		});
	}

	if (Hits.Num() == 0)
	{
		PickDepth = 0;
		return false;
	}

	Hits.Sort([](const FHit& A, const FHit& B) { return A.Distance < B.Distance; });

	// Clicking the same spot again steps one deeper; moving the cursor starts
	// again at the front. Without this, a patch behind another is unreachable.
	// The spot is the pixel under the cursor with the camera where it was
	// (S247d): it was read from the ray's origin - the camera - so every
	// click with the camera still was "the same spot" and stepped behind
	// what was under the cursor ("it seems to be selecting what's behind
	// the faces rendered"). A double-click's second click keeps the depth.
	const bool bSameSpot = FVector2D::Distance(LastPickScreen, Ctx.Pixel) < 3.0
		&& FVector::Dist(LastPickOrigin, Ctx.Origin) < 1.0;

	PickDepth = bSameSpot ? (Ctx.bDoubleClick ? PickDepth : (PickDepth + 1) % Hits.Num()) : 0;
	PickDepth = FMath::Clamp(PickDepth, 0, Hits.Num() - 1);
	LastPickScreen = Ctx.Pixel;
	LastPickOrigin = Ctx.Origin;

	Out = Hits[PickDepth].Ref;
	if (OutWorldHit)
	{
		*OutWorldHit = Hits[PickDepth].World;
	}

	return true;
}

bool USurfEdMode::PickPatchWith(const FSurfPickContext& Ctx, FSurfPatchRef& Out, FVector* OutWorldHit) const
{
	UWorld* World = Ctx.World;
	if (!World)
	{
		return false;
	}

	const FVector Origin = Ctx.Origin;
	const FVector Direction = Ctx.Direction;

	double BestDistance = TNumericLimits<double>::Max();
	bool bFound = false;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		const FVector LocalOrigin = ToWorld.InverseTransformPosition(Origin);
		const FVector LocalDirection = ToWorld.InverseTransformVector(Direction).GetSafeNormal();

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			FSurfTessellation Tess;
			if (!SurfPatchMath::Tessellate(Patch, Tess))
			{
				return;
			}

			for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
			{
				const double Distance = RayTriangle(LocalOrigin, LocalDirection,
					Tess.Positions[Tess.Indices[I]],
					Tess.Positions[Tess.Indices[I + 1]],
					Tess.Positions[Tess.Indices[I + 2]]);

				if (Distance > 0.0 && Distance < BestDistance)
				{
					BestDistance = Distance;
					Out.Group = Group;
					Out.PatchId = Patch.Id;
					bFound = true;

					if (OutWorldHit)
					{
						*OutWorldHit = ToWorld.TransformPosition(LocalOrigin + LocalDirection * Distance);
					}
				}
			}
		});
	}

	return bFound;
}

bool USurfEdMode::IsPointVisible(const FVector& Eye, const FVector& WorldPoint) const
{
	return IsPointVisibleFrom(Eye, WorldPoint, nullptr);
}

bool USurfEdMode::IsPointVisibleFrom(const FVector& Eye, const FVector& WorldPoint, const FSurfPointRef* Self) const
{
	if (SelectsThroughSurfaces())
	{
		return true;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	const FVector Segment = WorldPoint - Eye;
	const double Length = Segment.Size();
	if (Length < 1.0)
	{
		return true;
	}

	// A control point of a curve sits off its own surface, above it or under
	// it, and its own surface must never hide it. A sheet's points are its
	// surface, and a fold of the same sheet can hide them honestly.
	int32 OwnGroup = -1;
	const ASurfGroup* OwnActor = nullptr;
	if (Self)
	{
		OwnActor = Self->Group.Get();
		const FSurfPatch* OwnPatch = OwnActor ? OwnActor->FindPatch(Self->PatchId) : nullptr;
		if (OwnPatch && OwnPatch->Type == ESurfPatchType::Curve)
		{
			if (const int32* Found = OwnActor->PatchTriangleGroups.Find(Self->PatchId))
			{
				OwnGroup = *Found;
			}
		}
	}

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group) || !Group->Preview || !Group->Preview->GetMesh() || Group->Preview->GetMesh()->TriangleCount() == 0)
		{
			continue;
		}

		// One tree per group, rebuilt when the preview is.
		TSharedPtr<FSurfOcclusionTree>& Entry = OcclusionTrees.FindOrAdd(Group);
		if (!Entry.IsValid() || Entry->Serial != Group->RebuildSerial || Entry->Mesh != Group->Preview->GetMesh())
		{
			Entry = MakeShared<FSurfOcclusionTree>();
			Entry->Serial = Group->RebuildSerial;
			Entry->Mesh = Group->Preview->GetMesh();
			Entry->Tree = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(Entry->Mesh, true);
		}

		// In the group's own space, so a scaled group is tested as drawn.
		const FTransform ToLocal = Group->GetActorTransform().Inverse();
		const FVector LocalEye = ToLocal.TransformPosition(Eye);
		const FVector LocalPoint = ToLocal.TransformPosition(WorldPoint);
		const FVector LocalSegment = LocalPoint - LocalEye;
		const double LocalLength = LocalSegment.Size();
		if (LocalLength < 1.0)
		{
			continue;
		}

		// Short of the point itself: the point is on the surface, and the
		// surface must not count as hiding it.
		UE::Geometry::FDynamicMeshAABBTree3::FQueryOptions Options;
		Options.MaxDistance = LocalLength - FMath::Max(1.0, LocalLength * 0.005);
		if (OwnGroup >= 0 && Group == OwnActor)
		{
			const UE::Geometry::FDynamicMesh3* Mesh = Entry->Mesh;
			const int32 Skip = OwnGroup;
			Options.TriangleFilterF = [Mesh, Skip](int32 Tid) { return Mesh->GetTriangleGroup(Tid) != Skip; };
		}

		double HitT = 0.0;
		int32 HitTid = -1;
		if (Entry->Tree->FindNearestHitTriangle(FRay3d(LocalEye, LocalSegment / LocalLength), HitT, HitTid, Options))
		{
			return false;
		}
	}

	return true;
}

bool USurfEdMode::PickPointWith(const FSurfPickContext& Ctx, FSurfPointRef& Out) const
{
	const ASurfGroup* Group = SelectedPatch.Group.Get();
	const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;

	// The active patch is searched first. When a click adds to a selection
	// and finds nothing on the active patch, every other patch is searched -
	// which is how a selection comes to span patches, and how two edges on
	// two patches get welded together.
	if (!Patch || !PickPointOn(Ctx, *Group, *Patch, Out))
	{
		// Plain clicks reach the other patches edited together (S191a).
		const bool bAdding = Ctx.bCtrl || Ctx.bShift;
		if ((!bAdding && EditSet.Num() <= 1) || !Ctx.World)
		{
			return false;
		}

		for (TActorIterator<ASurfGroup> It(Ctx.World); It; ++It)
		{
			const ASurfGroup* Other = *It;
			if (!Other || !IsOpen(Other))
			{
				continue;
			}

			bool bFound = false;
			Other->ForEachPatch([&](const FSurfPatch& Candidate)
			{
				if (bFound || (Other == Group && Patch && Candidate.Id == Patch->Id))
				{
					return;
				}
				if (!bAdding)
				{
					FSurfPatchRef Key;
					Key.Group = const_cast<ASurfGroup*>(Other);
					Key.PatchId = Candidate.Id;
					if (!InEditSet(Key))
					{
						return;
					}
				}
				bFound = PickPointOn(Ctx, *Other, Candidate, Out);
			});

			if (bFound)
			{
				return true;
			}
		}

		return false;
	}

	return true;
}

bool USurfEdMode::PickPointOn(const FSurfPickContext& Ctx, const ASurfGroup& GroupIn, const FSurfPatch& PatchIn, FSurfPointRef& Out) const
{
	const ASurfGroup* Group = &GroupIn;
	const FSurfPatch* Patch = &PatchIn;

	const FVector Origin = Ctx.Origin;
	const FVector Direction = Ctx.Direction;
	const FTransform ToWorld = Group->GetActorTransform();

	// In pixels, when the view can say where a point lands (a viewport or
	// a synthetic camera always can): a flat view has no field of view for
	// an angle to mean anything in - from its far-off eye every point was
	// "under" the cursor, and the first in the list was taken. The angle is
	// the fallback.
	const USurfSettings* PickSettings = GetDefault<USurfSettings>();
	const double PickRadiusPx = PickSettings ? PickSettings->PointPickRadiusPx : 16.0;
	constexpr double PickRadius = 0.02;

	/**
	 * Every point under the cursor, nearest first - not just the nearest one.
	 *
	 * A weld does not remove points, it puts them on top of each other, so
	 * after one there are two points in the same place and only one of them
	 * could ever be picked. Collecting them all and stepping through on
	 * repeated clicks is what makes a weld undoable by hand: click, click
	 * again to reach the one underneath, and drag it back out.
	 */
	TArray<TPair<double, FSurfPointRef>> Under;

	for (int32 Y = 0; Y < Patch->Height; ++Y)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			const FVector World = ToWorld.TransformPosition(Patch->At(X, Y).Pos);

			double Along = 0.0, Screenish = 0.0;
			if (!PointNearRay(World, Origin, Direction, Along, Screenish))
			{
				continue;
			}

			bool bUnder = Screenish < PickRadius;
			FVector2D PointPixel;
			if (Ctx.WorldToPixel && Ctx.WorldToPixel(World, PointPixel))
			{
				bUnder = FVector2D::Distance(PointPixel, Ctx.Pixel) <= PickRadiusPx;
			}

			if (bUnder)
			{
				FSurfPointRef Ref;
				Ref.Group = const_cast<ASurfGroup*>(Group);
				Ref.PatchId = Patch->Id;
				Ref.X = X;
				Ref.Y = Y;

				// Behind a surface is out of reach, unless the view is x-ray.
				if (!IsPointVisibleFrom(Origin, World, &Ref))
				{
					continue;
				}

				Under.Emplace(Along, Ref);
			}
		}
	}

	if (Under.Num() == 0)
	{
		return false;
	}

	// Nearest first, then by grid position so the order is the same every
	// time - a cycle through an order that changes is not a cycle.
	Under.Sort([](const TPair<double, FSurfPointRef>& A, const TPair<double, FSurfPointRef>& B)
	{
		if (!FMath::IsNearlyEqual(A.Key, B.Key, 0.5))
		{
			return A.Key < B.Key;
		}

		if (A.Value.Y != B.Value.Y)
		{
			return A.Value.Y < B.Value.Y;
		}

		return A.Value.X < B.Value.X;
	});

	// Clicking the same place again takes the next one down the stack.
	int32 Which = 0;

	if (Under.Num() > 1)
	{
		const double Now = FPlatformTime::Seconds();
		const bool bSameSpot = (Now - LastPointPickSeconds) < 2.0
			&& Under.ContainsByPredicate([this](const TPair<double, FSurfPointRef>& Entry)
			{
				return Entry.Value == LastPointPicked;
			});

		if (bSameSpot)
		{
			const int32 Previous = Under.IndexOfByPredicate([this](const TPair<double, FSurfPointRef>& Entry)
			{
				return Entry.Value == LastPointPicked;
			});

			Which = (Previous + 1) % Under.Num();
		}
	}

	Out = Under[Which].Value;

	LastPointPicked = Out;
	LastPointPickSeconds = FPlatformTime::Seconds();

	if (Under.Num() > 1 && Inspector)
	{
		Inspector->SetResult(FString::Printf(
			TEXT("point %d,%d - %d points here, click again for the next"),
			Out.X, Out.Y, Under.Num()));
	}

	return true;
}

FSurfPatch* USurfEdMode::ModifySelectedPatch()
{
	ASurfGroup* Group = SelectedPatch.Group.Get();
	return Group ? Group->ModifyPatch(SelectedPatch.PatchId) : nullptr;
}

FVector USurfEdMode::FindPointSnap(const ASurfGroup& MovingGroup, const FSurfPatch& MovingPatch, const TArray<FVector>& MovingFrom) const
{
	SnapMarkerKind = ESurfSnapKind::None;

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bEdgeTargets = Settings && (Settings->bSnapToEdges || Settings->bSnapToEdgeCentres || Settings->bSnapToEdgePerpendicular);
	if (!Settings || !SurfSnapState::On() || (!Settings->bSnapToPoints && !Settings->bSnapToMeshes && !bEdgeTargets) || PointSelection.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	UWorld* World = MovingGroup.GetWorld();
	if (!World)
	{
		return FVector::ZeroVector;
	}

	const double Radius = FMath::Max(0.1, Settings->PointSnapRadiusCm);
	const double RadiusSquared = Radius * Radius;

	/**
	 * With an axis named, the reach is along that axis and nothing else
	 * (S150a) - so a point at the right height wins from anywhere, which is
	 * what "a vertice at some other place onto his Z's" asks for. The rest
	 * of the way is still bounded, generously, because snapping to
	 * something on the far side of the level is not help.
	 */
	const FVector Along = SnapAlongAxis.GetSafeNormal();
	const FVector Excluded = SnapExcludedAxis.GetSafeNormal();
	const double FarSquared = FMath::Square(Radius * 100.0);

	auto Measure = [&Along, &Excluded, FarSquared](const FVector& Delta, FVector& OutCorrection) -> double
	{
		if (!Along.IsNearlyZero())
		{
			if (Delta.SizeSquared() > FarSquared)
			{
				return MAX_dbl;
			}
			const double AlongBy = FVector::DotProduct(Delta, Along);
			OutCorrection = Along * AlongBy;
			return AlongBy * AlongBy;
		}
		if (!Excluded.IsNearlyZero())
		{
			if (Delta.SizeSquared() > FarSquared)
			{
				return MAX_dbl;
			}
			OutCorrection = Delta - Excluded * FVector::DotProduct(Delta, Excluded);
			return OutCorrection.SizeSquared();
		}
		OutCorrection = Delta;
		return Delta.SizeSquared();
	};

	const FTransform MovingToWorld = MovingGroup.GetActorTransform();

	// Where the selection is now, in world space.
	TArray<FVector> Moving;
	Moving.Reserve(PointSelection.Num());
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Ref.X >= 0 && Ref.X < MovingPatch.Width && Ref.Y >= 0 && Ref.Y < MovingPatch.Height)
		{
			Moving.Add(MovingToWorld.TransformPosition(MovingPatch.At(Ref.X, Ref.Y).Pos));
		}
	}

	if (Moving.Num() == 0)
	{
		return FVector::ZeroVector;
	}
	// Snap with (S205a): the one point that lands, where it is and where it
	// started, unless every point may.
	TArray<FVector> Starts = MovingFrom;
	if (Starts.Num() == Moving.Num())
	{
		SurfSnapState::ReduceForSnapWith(Starts);
	}
	SurfSnapState::ReduceForSnapWith(Moving);

	double BestSquared = RadiusSquared;
	FVector BestCorrection = FVector::ZeroVector;
	FVector BestTarget = FVector::ZeroVector;
	ESurfSnapKind BestKind = ESurfSnapKind::None;

	for (TActorIterator<ASurfGroup> It(World); It && Settings->bSnapToPoints; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		const bool bSameGroup = Group == &MovingGroup;

		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			const bool bSamePatch = bSameGroup && Patch.Id == MovingPatch.Id;

			// The patch being edited is not a target unless asked for: that is
			// how points collapsed onto their neighbours by accident.
			if (bSamePatch && !Settings->bSnapWithinPatch)
			{
				return;
			}

			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					// A point cannot snap to itself, or to anything else that
					// is moving with it - the selection would never settle.
					// On any patch: a lid's corners move with the skirts'
					// tops, and a target at distance zero would beat every
					// real one.
					{
						FSurfPointRef Ref;
						Ref.Group = const_cast<ASurfGroup*>(Group);
						Ref.PatchId = Patch.Id;
						Ref.X = X;
						Ref.Y = Y;

						if (PointSelection.Contains(Ref))
						{
							continue;
						}
					}

					const FVector Target = ToWorld.TransformPosition(Patch.At(X, Y).Pos);

					// Nor back to where it started: a point being pulled off
					// another one would otherwise be pulled straight back.
					bool bWhereItCameFrom = false;
					for (const FVector& From : MovingFrom)
					{
						if (FVector::DistSquared(Target, From) < 1.0)
						{
							bWhereItCameFrom = true;
							break;
						}
					}
					if (bWhereItCameFrom)
					{
						continue;
					}

					for (const FVector& From : Moving)
					{
						FVector Correction = FVector::ZeroVector;
						const double Squared = Measure(Target - From, Correction);

						if (Squared < BestSquared)
						{
							BestSquared = Squared;
							BestCorrection = Correction;
							BestTarget = Target;
							BestKind = ESurfSnapKind::Point;
						}
					}
				}
			}
		});
	}

	/**
	 * The edges of ours (S200a): their middles and the feet of perpendiculars
	 * compete with the points, nearest wins; the nearest point of an edge
	 * only when nothing of those is in reach - an edge's nearest point is
	 * always nearer than its ends, and would take every corner from them.
	 */
	if (bEdgeTargets)
	{
		TArray<FVector> Ends;
		GatherSnapEdges(&MovingGroup, &MovingPatch, nullptr, Ends);
		auto Offer = [&](const FVector& Target, const FVector& From, ESurfSnapKind Kind)
		{
			for (const FVector& Was : MovingFrom)
			{
				if (FVector::DistSquared(Target, Was) < 1.0)
				{
					return;
				}
			}
			FVector Correction = FVector::ZeroVector;
			const double Squared = Measure(Target - From, Correction);
			if (Squared < BestSquared)
			{
				BestSquared = Squared;
				BestCorrection = Correction;
				BestTarget = Target;
				BestKind = Kind;
			}
		};
		for (int32 E = 0; E + 1 < Ends.Num(); E += 2)
		{
			const FVector& A = Ends[E];
			const FVector& B = Ends[E + 1];
			for (int32 I = 0; I < Moving.Num(); ++I)
			{
				if (Settings->bSnapToEdgeCentres)
				{
					Offer((A + B) * 0.5, Moving[I], ESurfSnapKind::EdgeMid);
				}
				FVector Foot;
				if (Settings->bSnapToEdgePerpendicular && Starts.IsValidIndex(I) && SurfSnapState::PerpendicularFoot(Starts[I], A, B, Foot))
				{
					Offer(Foot, Moving[I], ESurfSnapKind::EdgePerp);
				}
			}
		}
		if (Settings->bSnapToEdges && BestKind == ESurfSnapKind::None)
		{
			for (int32 E = 0; E + 1 < Ends.Num(); E += 2)
			{
				for (const FVector& From : Moving)
				{
					Offer(FMath::ClosestPointOnSegment(From, Ends[E], Ends[E + 1]), From, ESurfSnapKind::OwnEdge);
				}
			}
		}
	}

	// The level's meshes, within the same reach; the nearer target wins.
	{
		FVector MeshCorrection;
		FVector MeshTarget;
		double MeshSquared = 0.0;
		ESurfSnapKind MeshKind = ESurfSnapKind::None;
		if (FindMeshSnap(Moving, MeshCorrection, MeshTarget, MeshSquared, MeshKind))
		{
			// Measured again through the axis the hand named: the mesh search
			// finds its target in three dimensions, and what may be applied
			// is only ever what the lock allows.
			FVector Allowed = FVector::ZeroVector;
			const double Scored = Measure(MeshCorrection, Allowed);
			if (Scored < BestSquared)
			{
				BestSquared = Scored;
				BestCorrection = Allowed;
				BestTarget = MeshTarget;
				BestKind = MeshKind;
			}
		}
	}

	// Nothing in reach: a far point's axis line, when the option is on and
	// no lock is (S171b) - a lock already reaches across the level along
	// its axis.
	if (BestKind == ESurfSnapKind::None && Settings->bSnapAlign && Along.IsNearlyZero() && Excluded.IsNearlyZero())
	{
		TArray<FVector> Targets;
		GatherSnapTargets(&MovingGroup, &MovingPatch, nullptr, Targets);
		FVector Correction;
		if (FindAlignSnapAmong(Targets, Moving, MovingFrom, Correction))
		{
			return Correction;
		}
	}

	SnapMarkerWorld = BestTarget;
	SnapMarkerKind = BestKind;
	return BestCorrection;
}

namespace
{
	/** The correction a lock allows of a delta (S150a): along the named axis, or off the excluded one. */
	FVector Allowed(const FVector& Delta, const FVector& AlongRaw, const FVector& ExcludedRaw)
	{
		const FVector Along = AlongRaw.GetSafeNormal();
		const FVector Excluded = ExcludedRaw.GetSafeNormal();
		if (!Along.IsNearlyZero())
		{
			return Along * FVector::DotProduct(Delta, Along);
		}
		if (!Excluded.IsNearlyZero())
		{
			return Delta - Excluded * FVector::DotProduct(Delta, Excluded);
		}
		return Delta;
	}

	bool CameFrom(const FVector& At, const TArray<FVector>& MovingFrom)
	{
		for (const FVector& From : MovingFrom)
		{
			if (FVector::DistSquared(At, From) < 1.0)
			{
				return true;
			}
		}
		return false;
	}
}

bool USurfEdMode::FindAlignSnapAmong(const TArray<FVector>& Targets, const TArray<FVector>& Moving, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const
{
	OutCorrection = FVector::ZeroVector;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !SurfSnapState::On() || !Settings->bSnapAlign || Moving.Num() == 0 || !SnapAlongAxis.IsNearlyZero() || !SnapExcludedAxis.IsNearlyZero())
	{
		return false;
	}
	const double Radius = FMath::Max(0.1, Settings->PointSnapRadiusCm);
	const double RadiusSquared = Radius * Radius;
	// Not across the whole level: a guide from a point out of sight is not help.
	const double FarSquared = FMath::Square(FMath::Max(Radius * 100.0, 5000.0));
	const FVector Axes[3] = { FVector::ForwardVector, FVector::RightVector, FVector::UpVector };

	double BestSquared = RadiusSquared;
	FVector BestCorrection = FVector::ZeroVector;
	FVector BestTarget = FVector::ZeroVector;
	FVector BestAxis = FVector::ZeroVector;
	FVector BestTo = FVector::ZeroVector;
	bool bFound = false;
	for (const FVector& Target : Targets)
	{
		if (CameFrom(Target, MovingFrom))
		{
			continue;
		}
		for (const FVector& From : Moving)
		{
			const FVector Delta = Target - From;
			const double Far = Delta.SizeSquared();
			if (Far > FarSquared || Far < RadiusSquared)
			{
				// Out of sight, or the point itself - the point snap's business.
				continue;
			}
			for (const FVector& Axis : Axes)
			{
				// The perpendicular from the moving point to the axis line
				// through the target is the correction that lands it there.
				const FVector Perp = Delta - Axis * FVector::DotProduct(Delta, Axis);
				const double Squared = Perp.SizeSquared();
				if (Squared < BestSquared)
				{
					BestSquared = Squared;
					BestCorrection = Perp;
					BestTarget = Target;
					BestAxis = Axis;
					BestTo = From + Perp;
					bFound = true;
				}
			}
		}
	}
	if (!bFound)
	{
		return false;
	}
	OutCorrection = BestCorrection;
	SnapMarkerWorld = BestTarget;
	SnapMarkerKind = ESurfSnapKind::Align;
	SnapMarkerAxis = BestAxis;
	SnapMarkerTo = BestTo;
	return true;
}

void USurfEdMode::GatherSnapTargets(const ASurfGroup* MovingGroup, const FSurfPatch* MovingPatch, const TArray<FGuid>* MovingBrushes, TArray<FVector>& Out) const
{
	Out.Reset();
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	UWorld* World = GetWorld();
	if (!Settings || !World)
	{
		return;
	}
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		const bool bSameGroup = Group == MovingGroup;
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			const bool bSamePatch = bSameGroup && MovingPatch && Patch.Id == MovingPatch->Id;
			if (bSamePatch && !Settings->bSnapWithinPatch)
			{
				return;
			}
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					if (bSameGroup)
					{
						FSurfPointRef Ref;
						Ref.Group = const_cast<ASurfGroup*>(Group);
						Ref.PatchId = Patch.Id;
						Ref.X = X;
						Ref.Y = Y;
						if (PointSelection.Contains(Ref))
						{
							continue;
						}
					}
					if (Settings->bSnapToPoints)
					{
						Out.Add(ToWorld.TransformPosition(Patch.At(X, Y).Pos));
					}
				}
			}
		});
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			if (bSameGroup && MovingBrushes && MovingBrushes->Contains(Brush.Id))
			{
				return;
			}
			FSurfHull Hull;
			if (!Settings->bSnapToPoints || !SurfBrushMath::DeriveHull(Brush, Hull))
			{
				return;
			}
			for (const FVector& Corner : Hull.Vertices)
			{
				Out.Add(ToWorld.TransformPosition(Corner));
			}
		});
	}
	// Edge middles are points too (S200a), for the moves that snap to points only.
	if (Settings->bSnapToEdgeCentres)
	{
		TArray<FVector> Ends;
		GatherSnapEdges(MovingGroup, MovingPatch, MovingBrushes, Ends);
		for (int32 E = 0; E + 1 < Ends.Num(); E += 2)
		{
			Out.Add((Ends[E] + Ends[E + 1]) * 0.5);
		}
	}
}

void USurfEdMode::GatherSnapEdges(const ASurfGroup* MovingGroup, const FSurfPatch* MovingPatch, const TArray<FGuid>* MovingBrushes, TArray<FVector>& OutEnds) const
{
	OutEnds.Reset();
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	UWorld* World = GetWorld();
	if (!Settings || !World)
	{
		return;
	}
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		const bool bSameGroup = Group == MovingGroup;
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			const bool bSamePatch = bSameGroup && MovingPatch && Patch.Id == MovingPatch->Id;
			if (bSamePatch && !Settings->bSnapWithinPatch)
			{
				return;
			}
			auto Moves = [&](int32 X, int32 Y)
			{
				if (!bSameGroup)
				{
					return false;
				}
				FSurfPointRef Ref;
				Ref.Group = const_cast<ASurfGroup*>(Group);
				Ref.PatchId = Patch.Id;
				Ref.X = X;
				Ref.Y = Y;
				return PointSelection.Contains(Ref);
			};
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					if (Moves(X, Y))
					{
						continue;
					}
					const FVector Here = ToWorld.TransformPosition(Patch.At(X, Y).Pos);
					if (X + 1 < Patch.Width && !Moves(X + 1, Y))
					{
						OutEnds.Add(Here);
						OutEnds.Add(ToWorld.TransformPosition(Patch.At(X + 1, Y).Pos));
					}
					if (Y + 1 < Patch.Height && !Moves(X, Y + 1))
					{
						OutEnds.Add(Here);
						OutEnds.Add(ToWorld.TransformPosition(Patch.At(X, Y + 1).Pos));
					}
				}
			}
		});
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			if (bSameGroup && MovingBrushes && MovingBrushes->Contains(Brush.Id))
			{
				return;
			}
			FSurfHull Hull;
			if (!SurfBrushMath::DeriveHull(Brush, Hull))
			{
				return;
			}
			for (const FSurfHullFace& Face : Hull.Faces)
			{
				const int32 Count = Face.VertexIndices.Num();
				for (int32 I = 0; I < Count; ++I)
				{
					const int32 A = Face.VertexIndices[I];
					const int32 B = Face.VertexIndices[(I + 1) % Count];
					// Each edge is shared by two faces: kept once, the lower index first.
					if (A < B && Hull.Vertices.IsValidIndex(A) && Hull.Vertices.IsValidIndex(B))
					{
						OutEnds.Add(ToWorld.TransformPosition(Hull.Vertices[A]));
						OutEnds.Add(ToWorld.TransformPosition(Hull.Vertices[B]));
					}
				}
			}
		});
	}
}

bool USurfEdMode::FindCursorSnapAmong(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
	const TArray<FVector>& Targets, const TArray<FVector>& Moving, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const
{
	OutCorrection = FVector::ZeroVector;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !SurfSnapState::On() || !(Settings->bSnapToPoints || Settings->bSnapToEdgeCentres) || !ViewportClient || !Viewport || Moving.Num() == 0 || Targets.Num() == 0)
	{
		return false;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
		Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	const FSceneView* View = ViewportClient->CalcSceneView(&Family);
	if (!View)
	{
		return false;
	}

	// A hand's width on screen, in the viewport's own pixels.
	const double ReachPx = 16.0;
	double BestPx = ReachPx * ReachPx;
	FVector Target = FVector::ZeroVector;
	bool bFound = false;
	for (const FVector& At : Targets)
	{
		if (CameFrom(At, MovingFrom))
		{
			continue;
		}
		FVector2D Px;
		if (!View->WorldToPixel(At, Px))
		{
			continue;
		}
		const double D = FVector2D::DistSquared(Px, CursorPx);
		if (D < BestPx)
		{
			BestPx = D;
			Target = At;
			bFound = true;
		}
	}
	if (!bFound)
	{
		return false;
	}

	// The moving point nearest the cursor on screen is the one that lands.
	double NearestPx = MAX_dbl;
	FVector Lead = FVector::ZeroVector;
	for (const FVector& At : Moving)
	{
		FVector2D Px;
		const double D = View->WorldToPixel(At, Px) ? FVector2D::DistSquared(Px, CursorPx) : MAX_dbl;
		if (D < NearestPx)
		{
			NearestPx = D;
			Lead = At;
		}
	}
	if (NearestPx == MAX_dbl)
	{
		return false;
	}
	OutCorrection = Allowed(Target - Lead, SnapAlongAxis, SnapExcludedAxis);
	SnapMarkerWorld = Target;
	SnapMarkerKind = ESurfSnapKind::Point;
	return true;
}

bool USurfEdMode::FindSurfaceSnap(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
	const AActor* Ignore, const TArray<FVector>& Moving, FVector& OutCorrection) const
{
	OutCorrection = FVector::ZeroVector;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	UWorld* World = GetWorld();
	if (!Settings || !SurfSnapState::On() || !Settings->bSnapToSurfaces || !ViewportClient || !Viewport || !World || Moving.Num() == 0)
	{
		return false;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(
		Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
	const FSceneView* View = ViewportClient->CalcSceneView(&Family);
	if (!View)
	{
		return false;
	}
	FVector Origin, Direction;
	View->DeprojectFVector2D(CursorPx, Origin, Direction);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SurfSurfaceSnap), /*bTraceComplex*/ true);
	if (Ignore)
	{
		Params.AddIgnoredActor(Ignore);
	}
	// Every actor being moved (S227a): a placed mesh under the cursor must
	// not snap onto itself, nor onto another of the moving ones.
	for (const TWeakObjectPtr<AActor>& Weak : ModalActors)
	{
		if (const AActor* Each = Weak.Get()) { Params.AddIgnoredActor(Each); }
	}
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 1.0e6, ECC_Visibility, Params) || !Hit.GetActor())
	{
		return false;
	}

	double NearestPx = MAX_dbl;
	FVector Lead = FVector::ZeroVector;
	for (const FVector& At : Moving)
	{
		FVector2D Px;
		const double D = View->WorldToPixel(At, Px) ? FVector2D::DistSquared(Px, CursorPx) : MAX_dbl;
		if (D < NearestPx)
		{
			NearestPx = D;
			Lead = At;
		}
	}
	if (NearestPx == MAX_dbl)
	{
		return false;
	}
	OutCorrection = Allowed(Hit.ImpactPoint - Lead, SnapAlongAxis, SnapExcludedAxis);
	SnapMarkerWorld = Hit.ImpactPoint;
	SnapMarkerNormal = Hit.ImpactNormal.GetSafeNormal(1.e-6, FVector::UpVector);
	SnapMarkerKind = ESurfSnapKind::Face;
	return true;
}

bool USurfEdMode::FindReachSnapAmong(const TArray<FVector>& Targets, const TArray<FVector>& MovingIn, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const
{
	// Snap with (S205a).
	TArray<FVector> Moving = MovingIn;
	SurfSnapState::ReduceForSnapWith(Moving);
	OutCorrection = FVector::ZeroVector;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !SurfSnapState::On() || !(Settings->bSnapToPoints || Settings->bSnapToEdgeCentres) || Moving.Num() == 0)
	{
		return false;
	}
	const double Radius = FMath::Max(0.1, Settings->PointSnapRadiusCm);
	double BestSquared = Radius * Radius;
	FVector BestCorrection = FVector::ZeroVector;
	FVector BestTarget = FVector::ZeroVector;
	bool bFound = false;
	for (const FVector& Target : Targets)
	{
		if (CameFrom(Target, MovingFrom))
		{
			continue;
		}
		for (const FVector& From : Moving)
		{
			const FVector Correction = Allowed(Target - From, SnapAlongAxis, SnapExcludedAxis);
			const double Squared = Correction.SizeSquared();
			if (Squared < BestSquared)
			{
				BestSquared = Squared;
				BestCorrection = Correction;
				BestTarget = Target;
				bFound = true;
			}
		}
	}
	if (!bFound)
	{
		// A far point's axis line instead (S171b).
		return FindAlignSnapAmong(Targets, Moving, MovingFrom, OutCorrection);
	}
	OutCorrection = BestCorrection;
	SnapMarkerWorld = BestTarget;
	SnapMarkerKind = ESurfSnapKind::Point;
	return true;
}

bool USurfEdMode::FindCursorSnap(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
	const ASurfGroup& MovingGroup, const FSurfPatch& MovingPatch, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const
{
	OutCorrection = FVector::ZeroVector;
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !ViewportClient || !Viewport || PointSelection.Num() == 0)
	{
		return false;
	}
	const FTransform MovingToWorld = MovingGroup.GetActorTransform();
	TArray<FVector> Moving;
	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (Ref.PatchId == MovingPatch.Id && Ref.X >= 0 && Ref.X < MovingPatch.Width && Ref.Y >= 0 && Ref.Y < MovingPatch.Height)
		{
			Moving.Add(MovingToWorld.TransformPosition(MovingPatch.At(Ref.X, Ref.Y).Pos));
		}
	}
	if (Moving.Num() == 0)
	{
		return false;
	}
	TArray<FVector> Targets;
	GatherSnapTargets(&MovingGroup, &MovingPatch, nullptr, Targets);
	if (FindCursorSnapAmong(ViewportClient, Viewport, CursorPx, Targets, Moving, MovingFrom, OutCorrection))
	{
		return true;
	}
	return FindSurfaceSnap(ViewportClient, Viewport, CursorPx, &MovingGroup, Moving, OutCorrection);
}

void USurfEdMode::ForEachSelectedPatch(TFunctionRef<void(ASurfGroup&, FSurfPatch&, const TArray<FSurfPointRef>&)> Body)
{
	/**
	 * The selection may span patches now, so every transform runs once per
	 * patch it touches. Grouped here, in the order the patches were first
	 * picked, and every group touched is rebuilt once at the end rather than
	 * once per point.
	 */
	TArray<TPair<FSurfPatchRef, TArray<FSurfPointRef>>> ByPatch;

	for (const FSurfPointRef& Ref : PointSelection)
	{
		if (!Ref.IsValid())
		{
			continue;
		}

		FSurfPatchRef Key;
		Key.Group = Ref.Group;
		Key.PatchId = Ref.PatchId;

		TArray<FSurfPointRef>* Bucket = nullptr;
		for (TPair<FSurfPatchRef, TArray<FSurfPointRef>>& Pair : ByPatch)
		{
			if (Pair.Key == Key)
			{
				Bucket = &Pair.Value;
				break;
			}
		}

		if (!Bucket)
		{
			ByPatch.Emplace(Key, TArray<FSurfPointRef>());
			Bucket = &ByPatch.Last().Value;
		}

		Bucket->Add(Ref);
	}

	TArray<ASurfGroup*> Touched;

	for (TPair<FSurfPatchRef, TArray<FSurfPointRef>>& Pair : ByPatch)
	{
		ASurfGroup* Group = Pair.Key.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Pair.Key.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}

		Body(*Group, *Patch, Pair.Value);

		SurfPatchSurfacing::Reapply(*Patch);
		++Patch->EditSerial;
		Touched.AddUnique(Group);
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}
}

void USurfEdMode::MoveSelectedPoints(const FVector& WorldDelta)
{
	// "UVs follow edits" off: the texture stays where it was (S185b).
	const SurfPatchSurfacing::FHoldUVs HoldUVs(GetDefault<USurfSettings>() && !GetDefault<USurfSettings>()->bUVsFollowEdits);
	// A brush's corners move with the patch points: one selection, one
	// gesture, one undo step (S113a). It refuses on its own when the move
	// would open the solid, and says so.
	MoveBrushCorners(WorldDelta);

	if (PointSelection.Num() == 0 || WorldDelta.IsNearlyZero())
	{
		return;
	}

	// Primitives picked with the patches (a group of both, S249b) follow
	// where the first point actually went, snap and all.
	auto FirstPointWorld = [this](FVector& Out) -> bool
	{
		const FSurfPointRef& Ref = PointSelection[0];
		const ASurfGroup* PointGroup = Ref.Group.Get();
		const FSurfPatch* PointPatch = PointGroup ? PointGroup->FindPatch(Ref.PatchId) : nullptr;
		if (!PointPatch || Ref.X < 0 || Ref.X >= PointPatch->Width || Ref.Y < 0 || Ref.Y >= PointPatch->Height)
		{
			return false;
		}
		Out = PointGroup->GetActorTransform().TransformPosition(PointPatch->At(Ref.X, Ref.Y).Pos);
		return true;
	};
	FVector CarryFrom = FVector::ZeroVector;
	const bool bCarry = FirstPointWorld(CarryFrom);

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	double Snap = 0.0;
	if (Settings && SurfSnapState::On())
	{
		// Control points snap on their own setting, independently of the brush
		// grid - coupling them is the classic annoyance at fine detail (D9).
		switch (Settings->PointSnap)
		{
		case ESurfPointSnap::Fine:       Snap = Settings->FinePointSnapCm; break;
		case ESurfPointSnap::SameAsGrid: Snap = Settings->GetGridStepCm(Settings->DefaultGridIndex); break;
		default:                         Snap = 0.0; break;
		}
	}

	// Where every selected point is before the move, world space, so a point
	// cannot be snapped back onto the one it is leaving.
	TArray<FVector> MovingFrom;
	MovingFrom.Reserve(PointSelection.Num());
	for (const FSurfPointRef& Ref : PointSelection)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (Patch && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
		{
			MovingFrom.Add(Group->GetActorTransform().TransformPosition(Patch->At(Ref.X, Ref.Y).Pos));
		}
	}

	// The snap correction is one vector for the whole selection, worked out
	// against the active patch, so an edge dragged onto an edge arrives as an
	// edge and a selection across two patches moves as one thing.
	FVector Correction = FVector::ZeroVector;
	{
		// Not the helper in SurfEdModeOps.cpp: it lives in that file's
		// anonymous namespace and only reached here while the two files
		// shared a unity chunk, which they stopped doing when one grew.
		ASurfGroup* ActiveGroup = SelectedPatch.Group.Get();
		const FSurfPatch* Active = ActiveGroup ? ActiveGroup->FindPatch(SelectedPatch.PatchId) : nullptr;
		if (Active && ActiveGroup)
		{
			// Measured after the move: build a moved copy of the active
			// patch's selected points to test against.
			FSurfPatch Moved = *Active;
			const FVector ActiveLocal = ActiveGroup->GetActorTransform().InverseTransformVector(WorldDelta);
			for (const FSurfPointRef& Ref : PointSelection)
			{
				if (Ref.PatchId == Active->Id && Ref.X >= 0 && Ref.X < Moved.Width && Ref.Y >= 0 && Ref.Y < Moved.Height)
				{
					Moved.At(Ref.X, Ref.Y).Pos = USurfSettings::SnapToStep(Moved.At(Ref.X, Ref.Y).Pos + ActiveLocal, Snap);
				}
			}
			Correction = FindPointSnap(*ActiveGroup, Moved, MovingFrom);
		}
	}

	ForEachSelectedPatch([&](ASurfGroup& Group, FSurfPatch& Patch, const TArray<FSurfPointRef>& Refs)
	{
		const FTransform ToWorld = Group.GetActorTransform();
		const FVector LocalDelta = ToWorld.InverseTransformVector(WorldDelta);
		const FVector LocalCorrection = ToWorld.InverseTransformVector(Correction);

		for (const FSurfPointRef& Ref : Refs)
		{
			if (Ref.X < 0 || Ref.X >= Patch.Width || Ref.Y < 0 || Ref.Y >= Patch.Height)
			{
				continue;
			}

			FSurfControlPoint& Point = Patch.At(Ref.X, Ref.Y);
			Point.Pos = USurfSettings::SnapToStep(Point.Pos + LocalDelta, Snap) + LocalCorrection;
		}

		// Soft selection: everything inside the falloff comes too, by its
		// weight. The falloff belongs to the active patch only. Unsnapped,
		// because snapping a partial move would step the falloff into
		// terraces instead of a curve.
		if (Patch.Id == SelectedPatch.PatchId && SoftInfluence.Num() == Patch.NumPoints())
		{
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					const float Influence = SoftInfluence[Y * Patch.Width + X];
					if (Influence > 0.0f)
					{
						Patch.At(X, Y).Pos += LocalDelta * Influence;
					}
				}
			}
		}
	});

	FVector CarryTo = FVector::ZeroVector;
	if (bCarry && FirstPointWorld(CarryTo) && !CarryTo.Equals(CarryFrom))
	{
		CarryPickedBrushes(FTranslationMatrix(CarryTo - CarryFrom));
	}
}

// --- Gizmo -----------------------------------------------------------------

FVector USurfEdMode::GetWidgetLocation() const
{
	// In object mode the widget sits at the patch's centre and moves all of
	// it; in the others, at the selection's.
	if (SubMode == ESurfSubMode::Object && SelectedPatch.IsValid())
	{
		const ASurfGroup* Group = SelectedPatch.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;
		if (Patch && Patch->NumPoints() > 0)
		{
			FVector Sum = FVector::ZeroVector;
			for (const FSurfControlPoint& Point : Patch->Points)
			{
				Sum += Point.Pos;
			}
			return Group->GetActorTransform().TransformPosition(Sum / Patch->NumPoints());
		}
	}

	if (SubMode != ESurfSubMode::Object && PointSelection.Num() > 0)
	{
		// The centroid of the selection, world space, across every patch it
		// touches - so a row drags from its middle and two edges on two
		// patches drag from the middle of both.
		FVector Sum = FVector::ZeroVector;
		int32 Count = 0;

		for (const FSurfPointRef& Ref : PointSelection)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (Patch && Ref.X >= 0 && Ref.X < Patch->Width && Ref.Y >= 0 && Ref.Y < Patch->Height)
			{
				Sum += Group->GetActorTransform().TransformPosition(Patch->At(Ref.X, Ref.Y).Pos);
				++Count;
			}
		}

		if (Count > 0)
		{
			return Sum / Count;
		}
	}

	/**
	 * Corners picked on a brush: the gizmo stands on them (S148a).
	 *
	 * "cant move selected verts on a box !  (and maybe any kind of brush
	 * too?!)  make sure its possible to work in vert mode for brushes. in
	 * other viewports too" - and part of it was this: the handles sat in
	 * the middle of the brush, so the arrow under the hand belonged to the
	 * brush and not to the four corners lit up at the top of it.
	 */
	if (PointSelection.Num() == 0 && BrushCornerSelection.Num() > 0)
	{
		FVector OnCorners = FVector::ZeroVector;
		if (BrushCornersPivot(OnCorners))
		{
			return OnCorners;
		}
	}

	// A brush, or its sides, and nothing of a patch: the gizmo stands on what
	// is picked - the middle of the picked sides in face sub-mode, the middle
	// of the brushes otherwise (S72g).
	if (PointSelection.Num() == 0 && !SelectedPatch.IsValid() && (Selection.Num() > 0 || ActiveFace.IsValid()))
	{
		FVector Pivot = FVector::ZeroVector;
		if (SubMode == ESurfSubMode::Face && Selection.Num() > 0 && SelectedFacesPivot(Pivot))
		{
			return Pivot;
		}
		if (BrushSelectionPivot(Pivot))
		{
			return Pivot;
		}
	}

	return Super::GetWidgetLocation();
}

bool USurfEdMode::ShouldDrawWidget() const
{
	// Off entirely when asked: G, R and S are the whole interface then, as in
	// Blender, and nothing sits on top of the point being clicked.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (Settings && !Settings->bShowGizmo)
	{
		return false;
	}
	// Nor while the brush is out (S247f): a press on a handle was the
	// editor's drag, and its release a click that picked a patch - "shift+
	// click in paint mode is a Smooth" is what the hand meant.
	if (bPaintArmed)
	{
		return false;
	}

	// Ours instead: the editor's is kept, for its drag maths, but never drawn
	// (S120a). It used to be drawn over a selected actor, because ours did
	// not draw there - and that was the whole of the difference. Ours draws
	// there now, so there is one gizmo in the viewport whatever is picked.
	if (OwnGizmoOn())
	{
		return false;
	}

	// A brush picked and nothing of a patch: the editor's widget is what
	// drags it when ours is turned off (S72g).
	if (PointSelection.Num() == 0 && !SelectedPatch.IsValid() && (Selection.Num() > 0 || ActiveFace.IsValid()))
	{
		return true;
	}

	// The same actor case, for the hand that turned our own gizmo off.
	if (GEditor && GEditor->GetSelectedActorCount() > 0
		&& !SelectedPatch.IsValid() && PointSelection.Num() == 0)
	{
		return true;
	}

	if (SubMode != ESurfSubMode::Object)
	{
		return PointSelection.Num() > 0;
	}
	return SelectedPatch.IsValid();
}

bool USurfEdMode::UsesTransformWidget() const
{
	// The brush out: no widget to take the press (S247f).
	if (bPaintArmed)
	{
		return false;
	}
	if (Selection.Num() > 0 || ActiveFace.IsValid())
	{
		return true;
	}

	/**
	 * Nothing of ours picked, but the editor has an actor - a prefab
	 * instance, a light, a mesh (S93a).
	 *
	 * This said no, and a mode that says no here gets no widget at all: no
	 * origin drawn, no handle to drag, and no sign that the click had landed
	 * on anything. Which is exactly what both of his reports were - "ok then
	 * tried to move some prefab, i couldnt. i dont see its origin either?"
	 * and "im trying to select in 4 mode while im here it dont work", both
	 * out in the level. The click was selecting the whole time; there was
	 * nothing to show for it. In Object sub-mode, picking an actor is the
	 * normal thing to be doing.
	 */
	if (GEditor && GEditor->GetSelectedActorCount() > 0
		&& !SelectedPatch.IsValid() && PointSelection.Num() == 0)
	{
		return true;
	}

	return SubMode != ESurfSubMode::Object ? true : SelectedPatch.IsValid();
}

bool USurfEdMode::StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
	// One transaction per drag, opened here and closed in EndTracking. Opening
	// it per frame would pay D21's chunk snapshot sixty times a second and
	// leave undo with sixty steps to walk back through.
	// Edge sub-mode holds control points too, so a gizmo drag started from it
	// is the same drag - refusing it here is why shift-drag extrude did
	// nothing in the sub-mode the edges are actually selected in.
	const bool bAltHeld = InViewport && (InViewport->KeyState(EKeys::LeftAlt) || InViewport->KeyState(EKeys::RightAlt));
	const bool bShift = InViewport && (InViewport->KeyState(EKeys::LeftShift) || InViewport->KeyState(EKeys::RightShift));
	const bool bOnGizmo = !bAltHeld && InViewportClient && InViewportClient->GetCurrentWidgetAxis() != EAxisList::None;

	// Object mode drags the whole patch: every point becomes the selection
	// for the length of the drag, and is let go of when it ends.
	if (bOnGizmo && SubMode == ESurfSubMode::Object && (PointSelection.Num() == 0 || bShift) && SelectedPatch.IsValid())
	{
		DragTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("EditPoints", "Edit Control Points"));
		DragStartSeconds = FPlatformTime::Seconds();
		bDragWasExtrude = false;
		bDragWasDuplicate = false;
		DragMovedCm = 0.0;
		BeginGizmoDrag(InViewportClient, InViewport, bShift);

		// Shift: the drag moves a copy and leaves the patches where they are.
		// Made inside the drag's own transaction, so one undo takes the copy
		// and the move together.
		if (bShift && DuplicateSelection())
		{
			bDragWasDuplicate = true;
			SurfSession::Gesture(TEXT("shift_drag_duplicate"));
		}

		SelectAllPoints();
		bObjectDragTookAllPoints = true;
		return true;
	}

	if (bOnGizmo && PointSelection.Num() > 0)
	{
		DragTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("EditPoints", "Edit Control Points"));
		DragStartSeconds = FPlatformTime::Seconds();
		bDragWasExtrude = false;
		bDragWasDuplicate = false;
		DragMovedCm = 0.0;
		BeginGizmoDrag(InViewportClient, InViewport, bShift);

		// Shift-dragging faces moves a copy of them, as a patch of their own.
		// Shift-dragging a line extrudes first and then drags the new line: a
		// grid cannot carry a loose line, so the two-line sheet the extrude
		// makes is what a copy of a line is here. The keyboard path (E, or
		// Shift+D) is the seamless one - it hands the new lines straight to
		// the modal move, with no gizmo to reach for - and this is the same
		// thing for anyone whose hand is already on the gizmo.
		//
		// In place: the new lines start exactly on the ones they came from, so
		// the pivot does not move at the instant the drag begins. Offsetting
		// them moved the pivot mid-drag, and Unreal measures a drag against
		// the pivot - which is why holding the Z handle sent the line off in
		// the direction of the cursor instead of straight up.
		if (bShift)
		{
			if (SubMode == ESurfSubMode::Face)
			{
				if (DuplicateSelection())
				{
					bDragWasDuplicate = true;
			SurfSession::Gesture(TEXT("shift_drag_duplicate"));
				}
			}
			else if (ExtrudeSelectedEdge(/*bInPlace*/ true))
			{
				// The drag pulls only the new lines; soft is back at EndTracking.
				bSoftSuspended = true;
				bDragWasExtrude = true;
				RefreshSoftSelection();
				SurfSession::Gesture(TEXT("shift_drag_extrude"));
			}
		}

		return true;
	}

	/**
	 * Corners picked on a brush drag like control points do (S148a).
	 *
	 * This has to come before the brush branch below, and that is the whole
	 * bug: a corner can only be picked while a side of the same brush is
	 * active, so the test below was always true too and every drag went to
	 * the branch that writes the brush's **planes**. The box moved; the
	 * corners that were picked did nothing.
	 */
	if (bOnGizmo && PointSelection.Num() == 0 && BrushCornerSelection.Num() > 0)
	{
		DragTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("EditBrushCorners", "Move Primitive Corners"));
		DragStartSeconds = FPlatformTime::Seconds();
		bDragWasExtrude = false;
		bDragWasDuplicate = false;
		DragMovedCm = 0.0;
		BeginGizmoDrag(InViewportClient, InViewport, bShift);
		return true;
	}

	// A brush has no control points for the point drag to write, so the same
	// handles write its planes (S72g). In face sub-mode the arrows push the
	// picked sides, which is what G does there.
	if (bOnGizmo && PointSelection.Num() == 0 && !SelectedPatch.IsValid() && (Selection.Num() > 0 || ActiveFace.IsValid()))
	{
		FVector Pivot = FVector::ZeroVector;
		if (BrushSelectionPivot(Pivot))
		{
			bBrushDragPushesFaces = SubMode == ESurfSubMode::Face && PickedFaces().Num() > 0
				&& InViewportClient && InViewportClient->GetWidgetMode() == UE::Widget::WM_Translate;
			DragTransaction = MakeUnique<FScopedTransaction>(bBrushDragPushesFaces
				? LOCTEXT("PushFacesDrag", "Push Faces")
				: LOCTEXT("EditBrushDrag", "Edit Primitive"));
			DragStartSeconds = FPlatformTime::Seconds();
			DragMovedCm = 0.0;
			bBrushDrag = true;
			BrushDragPivot = Pivot;
			BrushDragMoved = FVector::ZeroVector;
			BeginGizmoDrag(InViewportClient, InViewport, bShift);
			return true;
		}
	}

	return Super::StartTracking(InViewportClient, InViewport);
}

bool USurfEdMode::EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
	/**
	 * A press on the rotate ball that turned nothing is a click (S139a).
	 *
	 * "due to the rotation sphere in the gizmo i cant click to select side
	 * and mode . problem . Bug" - and he is right. The ball is the size of
	 * the rotate rings and it sits over the middle of whatever is selected,
	 * which is exactly where the next click lands. **A handle that swallows
	 * clicks is worse than no handle**, however good the thing it does when
	 * you actually drag it.
	 *
	 * So a press that goes nowhere falls through to the pick, as though the
	 * ball had not been there: it costs a drag and never a selection. The
	 * ball is smaller than the rings now as well, so the rings are reached
	 * without crossing it.
	 */
	if (InViewportClient && InViewport && OwnGizmoOn()
		&& InViewportClient->GetWidgetMode() == UE::Widget::WM_Rotate
		&& InViewportClient->GetCurrentWidgetAxis() == EAxisList::Screen
		&& DragTotalRotation.IsNearlyZero(0.01) && DragMovedCm < 0.01)
	{
		DragTransaction.Reset();
		bBrushDrag = false;
		bBrushDragPushesFaces = false;
		bDragWasDuplicate = false;
		bDragWasExtrude = false;

		HandleClickWith(FSurfPickContext::FromCursor(InViewportClient, InViewport), nullptr, /*bKeepOnMiss*/ true);
		SurfSession::Gesture(TEXT("ball_press_was_a_click"));
		if (GEditor)
		{
			RedrawViewports();
		}
		return true;
	}

	// A brush drag: one step, noted for the adjust frame as G, R and S are.
	if (DragTransaction && bBrushDrag)
	{
		DragTransaction.Reset();
		NoteGizmoDrag();
		if (Inspector)
		{
			if (!BrushDragMoved.IsNearlyZero())
			{
				Inspector->SetResult(bBrushDragPushesFaces
					? FString::Printf(TEXT("pushed the picked sides %.0f cm"), BrushDragMoved.Size())
					: FString::Printf(TEXT("the primitive moved %.0f %.0f %.0f cm"), BrushDragMoved.X, BrushDragMoved.Y, BrushDragMoved.Z));
			}
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Gesture(TEXT("brush_drag"), FString::Printf(TEXT("push=%d cm=%.0f ms=%.0f"),
			bBrushDragPushesFaces ? 1 : 0, DragMovedCm, (FPlatformTime::Seconds() - DragStartSeconds) * 1000.0));
		bBrushDrag = false;
		bBrushDragPushesFaces = false;
		return true;
	}

	if (DragTransaction)
	{
		// A Shift-drag extrude that was never pulled anywhere leaves walls of
		// no height and lids on what they came from - geometry with edges
		// and no surface. Nothing moved, so nothing was wanted: the whole
		// drag, extrude included, is taken back.
		// Undone, not cancelled: cancelling a transaction drops its record
		// and keeps the edit - the lid and the flat wall stayed, unpickable
		// and un-undoable, which is where the "wire-only patches" came from.
		// A Shift-drag copy that never went anywhere is a second patch on
		// top of the first, invisible and in the way - which is how a
		// Shift-click on a face with a twitch in it piled up copies ("why
		// are there so many of this patch"). Nothing moved, so no copy was
		// wanted: undone, the copy with it.
		if (bDragWasDuplicate && DragMovedCm < 1.0)
		{
			DragTransaction.Reset();
			if (GEditor)
			{
				GEditor->UndoTransaction();
			}
			bDragWasDuplicate = false;
			bDragWasExtrude = false;
			if (bSoftSuspended)
			{
				bSoftSuspended = false;
				RefreshSoftSelection();
			}
			if (Inspector)
			{
				Inspector->SetResult(TEXT("duplicate: nothing moved, so no copy was kept"));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Gesture(TEXT("shift_drag_duplicate_nowhere"));
			if (GEditor)
			{
				RedrawViewports();
			}
			return true;
		}
		bDragWasDuplicate = false;

		if (bDragWasExtrude && DragMovedCm < 0.01)
		{
			DragTransaction.Reset();
			if (GEditor)
			{
				GEditor->UndoTransaction();
			}
			bDragWasExtrude = false;
			ExtrudeWallFacing.Reset();
			ExtrudeClosingPieces.Reset();
			if (bSoftSuspended)
			{
				bSoftSuspended = false;
				RefreshSoftSelection();
			}
			if (Inspector)
			{
				Inspector->SetResult(TEXT("extrude: nothing moved, so nothing was made"));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Gesture(TEXT("shift_drag_extrude_nowhere"));
			if (GEditor)
			{
				RedrawViewports();
			}
			return true;
		}
		bDragWasExtrude = false;

		// A Shift-drag extrude's walls were made in place, facing a guess;
		// now they have height, turned to face out - inside the drag's
		// transaction, as the modal's confirm does.
		if (ExtrudeWallFacing.Num() > 0 || ExtrudeClosingPieces.Num() > 0)
		{
			TurnExtrudedWallsOut();
			if (bExtrudePulledAlongSurface)
			{
				// The lid slid along its surface: a block with no height.
				// The whole drag, extrude included, is taken back - undone,
				// once the step is closed.
				DragTransaction.Reset();
				bDragWasExtrude = false;
				if (GEditor)
				{
					GEditor->UndoTransaction();
				}
				if (bSoftSuspended)
				{
					bSoftSuspended = false;
					RefreshSoftSelection();
				}
				if (Inspector)
				{
					Inspector->SetResult(TEXT("extrude: pulled along the surface, not out of it - a block with no height, so nothing was made"));
					Inspector->RefreshSelectionCount();
				}
				SurfSession::Friction(TEXT("extrude_pulled_along_surface"));
				if (GEditor)
				{
					RedrawViewports();
				}
				return true;
			}
		}

		// A line dragged fills what it stretched, when asked to (S150d).
		if (DragMovedCm > 0.01)
		{
			DensifyStretchedCells();
		}

		DragTransaction.Reset();

		// A plain drag is noted for the adjust frame, as G, R and S are.
		NoteGizmoDrag();

		// A Shift-drag extrude kept soft selection off for the drag.
		if (bSoftSuspended)
		{
			bSoftSuspended = false;
			RefreshSoftSelection();
		}

		if (bObjectDragTookAllPoints)
		{
			bObjectDragTookAllPoints = false;
			ClearPointSelection();
		}

		SnapAlongAxis = FVector::ZeroVector;
		SnapExcludedAxis = FVector::ZeroVector;

		SurfSession::Gesture(TEXT("point_drag"), FString::Printf(TEXT("points=%d ms=%.0f"),
			PointSelection.Num(), (FPlatformTime::Seconds() - DragStartSeconds) * 1000.0));

		return true;
	}

	return Super::EndTracking(InViewportClient, InViewport);
}

bool USurfEdMode::InputDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale)
{
	// A brush drag writes planes, not points (S72g).
	if (bBrushDrag)
	{
		return BrushGizmoDelta(InViewportClient, InViewport, InDrag, InRot, InScale);
	}

	/**
	 * An actor turns by our maths as well (S134a).
	 *
	 * The editor works its rotation out from its own widget, and its widget
	 * is not drawn while ours is - so a ring of ours moved points and
	 * brushes and left a selected actor sitting still, and the ball under
	 * the cursor handed the editor a screen-axis spin rather than the turn
	 * the hand had asked for. Both are the same gap, and it closes here:
	 * while our gizmo is in rotate with a handle held, the turn is ours and
	 * the actors take it.
	 */
	if (PointSelection.Num() == 0 && BrushCornerSelection.Num() == 0 && OwnGizmoOn() && InViewportClient
		&& InViewportClient->GetWidgetMode() == UE::Widget::WM_Rotate
		&& InViewportClient->GetCurrentWidgetAxis() != EAxisList::None
		&& GEditor && GEditor->GetSelectedActorCount() > 0)
	{
		// Alt is the camera, whatever the drag started on - the same rule
		// every other drag here follows.
		const bool bAltCamera = InViewport
			&& (InViewport->KeyState(EKeys::LeftAlt) || InViewport->KeyState(EKeys::RightAlt));
		if (bAltCamera)
		{
			return false;
		}

		FRotator Turn = FRotator::ZeroRotator;
		if (GizmoRingRotation(InViewportClient, InViewport, Turn) && !Turn.IsNearlyZero())
		{
			if (RotateSelectedActorsAbout(Turn, PivotOr(GetWidgetLocation())) > 0)
			{
				DragTotalRotation = (Turn.Quaternion() * DragTotalRotation.Quaternion()).Rotator();
				bDragRotated = true;
			}
		}

		// Handled either way: a delta left unhandled with a handle held is
		// applied by the editor to the same actors, twice over.
		return true;
	}

	// Corners picked on a brush take the same road as control points: the
	// move below calls MoveBrushCorners on its way through (S148a). Handing
	// this delta back to the editor is what left them sitting still.
	if (PointSelection.Num() == 0 && BrushCornerSelection.Num() == 0)
	{
		return Super::InputDelta(InViewportClient, InViewport, InDrag, InRot, InScale);
	}

	// Alt is the camera, whatever the drag started on. The gizmo is large
	// enough that an Alt-drag to orbit often began on one of its handles,
	// and the editor then read it as an Alt-duplicate-drag of the selection:
	// orbiting moved the points. Alt never edits here.
	const bool bAlt = InViewport && (InViewport->KeyState(EKeys::LeftAlt) || InViewport->KeyState(EKeys::RightAlt));
	if (bAlt)
	{
		return false;
	}

	// The bug this guard fixes: without it any drag with a delta moved the
	// selection, so orbiting the camera with a row selected dragged the row.
	// A gizmo drag is the one with an axis under the cursor; everything else
	// belongs to the viewport.
	if (!InViewportClient || InViewportClient->GetCurrentWidgetAxis() == EAxisList::None)
	{
		// Refusing is right - this is the camera, not a gizmo drag. Logged once
		// per run of refusals rather than per frame: at sixty a second it
		// buried every other line in the record.
		if (!bRefusingDrag && (!InDrag.IsNearlyZero() || !InRot.IsNearlyZero()))
		{
			bRefusingDrag = true;
			SurfSession::Friction(TEXT("drag_without_widget_axis"),
				FString::Printf(TEXT("points=%d corners=%d"), PointSelection.Num(), BrushCornerSelection.Num()));
		}

		return false;
	}

	bRefusingDrag = false;

	/**
	 * A handle is a lock too (S150a): an arrow drag moves along one axis and
	 * a plane square within one plane, so the snap is bound the same way the
	 * modal's X, Y and Z bind it.
	 */
	{
		const EAxisList::Type Held = InViewportClient->GetCurrentWidgetAxis();
		SnapAlongAxis = Held == EAxisList::X ? FVector::ForwardVector
			: Held == EAxisList::Y ? FVector::RightVector
			: Held == EAxisList::Z ? FVector::UpVector
			: FVector::ZeroVector;
		SnapExcludedAxis = Held == EAxisList::XY ? FVector::UpVector
			: Held == EAxisList::XZ ? FVector::RightVector
			: Held == EAxisList::YZ ? FVector::ForwardVector
			: FVector::ZeroVector;
	}

	if (!InDrag.IsNearlyZero())
	{
		MoveSelectedPoints(InDrag);
		DragMovedCm += InDrag.Size();
		DragTotalDelta += InDrag;
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
		RotateSelectedPoints(Rotation);
		DragTotalRotation = (Rotation.Quaternion() * DragTotalRotation.Quaternion()).Rotator();
		bDragRotated = true;
	}

	if (!InScale.IsNearlyZero())
	{
		// Unreal hands scale as a per-drag increment, not a factor.
		const FVector Factor = FVector::OneVector + InScale * 0.01;
		ScaleSelectedPoints(Factor);
		DragTotalScale *= Factor;
		bDragScaled = true;
	}

	// Handled whether or not this frame's delta came to anything: a gizmo
	// drag is ours from press to release, and a delta left unhandled with an
	// axis under the cursor is applied by the editor to the selected actors
	// - the group - instead.
	return true;
}

#undef LOCTEXT_NAMESPACE

// --- Marquee selection -----------------------------------------------------

namespace
{
	/** Every control point of a patch, as selectable refs with world positions. */
	void GatherPoints(ASurfGroup& Group, const FSurfPatch& Patch, TArray<TPair<FSurfPointRef, FVector>>& Out)
	{
		const FTransform ToWorld = Group.GetActorTransform();

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				FSurfPointRef Ref;
				Ref.Group = &Group;
				Ref.PatchId = Patch.Id;
				Ref.X = X;
				Ref.Y = Y;

				Out.Emplace(Ref, ToWorld.TransformPosition(Patch.At(X, Y).Pos));
			}
		}
	}

	/**
	 * The patches a marquee may touch: the selected one if there is one, so a
	 * drag across a crowded scene cannot silently grab a neighbour's points.
	 */
	bool GatherCandidates(USurfEdMode& Mode, TArray<TPair<FSurfPointRef, FVector>>& Out)
	{
		const FSurfPatchRef& Ref = Mode.GetSelectedPatch();
		ASurfGroup* Group = Ref.Group.Get();

		if (!Group)
		{
			return false;
		}

		const FSurfPatch* Patch = Group->FindPatch(Ref.PatchId);
		if (!Patch)
		{
			return false;
		}

		GatherPoints(*Group, *Patch, Out);
		return true;
	}
}

bool USurfEdMode::BoxSelect(FBox& InBox, bool InSelect)
{
	if (SubMode != ESurfSubMode::Vertex)
	{
		return false;
	}

	TArray<TPair<FSurfPointRef, FVector>> Candidates;
	if (!GatherCandidates(*this, Candidates))
	{
		return false;
	}

	return ApplyMarquee(Candidates, [&InBox](const FVector& World) { return InBox.IsInsideOrOn(World); }, InSelect);
}

bool USurfEdMode::FrustumSelect(const FConvexVolume& InFrustum, FEditorViewportClient* InViewportClient, bool InSelect)
{
	if (SubMode != ESurfSubMode::Vertex)
	{
		return false;
	}

	TArray<TPair<FSurfPointRef, FVector>> Candidates;
	if (!GatherCandidates(*this, Candidates))
	{
		return false;
	}

	return ApplyMarquee(Candidates, [&InFrustum](const FVector& World) { return InFrustum.IntersectPoint(World); }, InSelect);
}

bool USurfEdMode::ApplyMarquee(const TArray<TPair<FSurfPointRef, FVector>>& Candidates, TFunctionRef<bool(const FVector&)> Contains, bool bSelect)
{
	bool bChanged = false;

	for (const TPair<FSurfPointRef, FVector>& Candidate : Candidates)
	{
		if (!Contains(Candidate.Value))
		{
			continue;
		}

		const int32 Existing = PointSelection.IndexOfByKey(Candidate.Key);

		// A marquee adds; holding the deselect modifier takes away. Either
		// way it never clears what is already selected, so a big region can
		// be built up in several drags.
		if (bSelect && Existing == INDEX_NONE)
		{
			PointSelection.Add(Candidate.Key);
			bChanged = true;
		}
		else if (!bSelect && Existing != INDEX_NONE)
		{
			PointSelection.RemoveAt(Existing);
			bChanged = true;
		}
	}

	if (bChanged)
	{
		// A marquee is not a click, so the row/column cycle starts fresh.
		LastRowColumnPoint = FSurfPointRef();
		RowColumnStage = 2;

		RefreshSoftSelection();

		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
	}

	return bChanged;
}

// --- Wrappers: a live viewport in, a context out ------------------------------
// Everything above takes a context; these exist for callers that still hold a
// viewport and a click. They build the context once and hand it on.

bool USurfEdMode::PickPatchQuad(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfQuadRef& Out, FVector* OutWorldHit) const
{
	return PickPatchQuadWith(FSurfPickContext::FromClick(ViewportClient, Click), Out, OutWorldHit);
}

bool USurfEdMode::PickPatchCycling(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPatchRef& Out, FVector* OutWorldHit)
{
	return PickPatchCyclingWith(FSurfPickContext::FromClick(ViewportClient, Click), Out, OutWorldHit);
}

bool USurfEdMode::PickPatch(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPatchRef& Out, FVector* OutWorldHit) const
{
	return PickPatchWith(FSurfPickContext::FromClick(ViewportClient, Click), Out, OutWorldHit);
}

bool USurfEdMode::PickPoint(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPointRef& Out) const
{
	return PickPointWith(FSurfPickContext::FromClick(ViewportClient, Click), Out);
}

bool USurfEdMode::SelectEdgeAt(FEditorViewportClient* ViewportClient, const FViewportClick& Click, bool bAdd, bool bWholeLine)
{
	return SelectEdgeWith(FSurfPickContext::FromClick(ViewportClient, Click), bAdd, bWholeLine);
}
