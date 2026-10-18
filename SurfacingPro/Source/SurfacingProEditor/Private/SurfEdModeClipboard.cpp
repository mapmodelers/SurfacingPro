// Copyright MapModelers. All Rights Reserved.
//
// Duplicate, copy, cut and paste.
//
// What is taken is decided by the sub-mode: the patches picked in object
// sub-mode, otherwise the block the selection covers on each patch it touches
// - the whole patch when it covers the whole grid, a sheet of the block when
// it does not. A grid cannot carry a loose point or a loose line, so a
// selection that is not a rectangle at least two points each way is declined
// with the reason. A copy lands in place, on top of what it came from, and is
// selected, so the move that follows takes it off. The clipboard keeps its
// patches in world space, so a paste lands where the copy was taken from
// whichever group receives it.

#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfGroup.h"
#include "SurfPatchSurfacing.h"
#include "SurfCellSet.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModeClipboard"

namespace
{
	/** A rectangle of control points, inclusive. */
	struct FSurfRect
	{
		int32 X0 = 0;
		int32 Y0 = 0;
		int32 X1 = 0;
		int32 Y1 = 0;

		int32 Columns() const { return X1 - X0 + 1; }
		int32 Rows() const { return Y1 - Y0 + 1; }
	};

	/** The rectangle a set of points on one patch fills exactly, at least two each way. */
	bool FillsRectangle(const TArray<FIntPoint>& Points, FSurfRect& Out)
	{
		if (Points.Num() < 4)
		{
			return false;
		}

		FIntPoint Min(MAX_int32, MAX_int32);
		FIntPoint Max(-1, -1);
		for (const FIntPoint& P : Points)
		{
			Min = Min.ComponentMin(P);
			Max = Max.ComponentMax(P);
		}

		const int32 Columns = Max.X - Min.X + 1;
		const int32 Rows = Max.Y - Min.Y + 1;
		if (Columns < 2 || Rows < 2 || Points.Num() != Columns * Rows)
		{
			return false;
		}

		Out.X0 = Min.X;
		Out.Y0 = Min.Y;
		Out.X1 = Max.X;
		Out.Y1 = Max.Y;
		return true;
	}

	/** Whether a rectangle is the whole of a patch's grid. */
	bool IsWhole(const FSurfPatch& Patch, const FSurfRect& Rect)
	{
		return Rect.X0 == 0 && Rect.Y0 == 0 && Rect.Columns() == Patch.Width && Rect.Rows() == Patch.Height;
	}

	/**
	 * A copy of a patch, or of a rectangle of it. The whole grid keeps its
	 * type, so a copied curve is still a curve; a part becomes a sheet, as a
	 * part of a curve's net is not a curve of its own. A part starts its
	 * mapping at the source's UV on its first corner, so the surfacing
	 * continues across the copy.
	 */
	FSurfPatch CopyOf(const FSurfPatch& Source, const FSurfRect& Rect)
	{
		FSurfPatch Made = Source;
		Made.Id = FGuid::NewGuid();
		Made.EditSerial = 0;

		if (IsWhole(Source, Rect))
		{
			return Made;
		}

		Made.Type = ESurfPatchType::Sheet;
		// A part is not a member of the whole's groups (S249b).
		Made.GroupPath.Reset();
		Made.Width = Rect.Columns();
		Made.Height = Rect.Rows();
		Made.UVOriginX = 0;
		Made.UVOriginY = 0;
		Made.Points.Reset(Made.Width * Made.Height);
		for (int32 Y = Rect.Y0; Y <= Rect.Y1; ++Y)
		{
			for (int32 X = Rect.X0; X <= Rect.X1; ++X)
			{
				Made.Points.Add(Source.At(X, Y));
			}
		}

		if (SurfPatchSurfacing::FollowsTheNet(Made.Mapping))
		{
			Made.UVShift = Source.At(Rect.X0, Rect.Y0).UV;
		}
		return Made;
	}

	/** "3 patches" / "a 4 x 3 patch". */
	FString Describe(const TArray<FSurfPatchCopy>& Copies)
	{
		if (Copies.Num() == 1)
		{
			return FString::Printf(TEXT("a %d x %d patch"), Copies[0].Patch.Width, Copies[0].Patch.Height);
		}
		return FString::Printf(TEXT("%d patches"), Copies.Num());
	}
}

bool USurfEdMode::GatherSelectionAsPatches(TArray<FSurfPatchCopy>& Out, FString& OutReason) const
{
	Out.Reset();

	if (SubMode == ESurfSubMode::Object)
	{
		for (const FSurfPatchRef& Ref : GetSelectedPatches())
		{
			ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || !Patch->HasValidDimensions())
			{
				continue;
			}

			FSurfRect Whole;
			Whole.X1 = Patch->Width - 1;
			Whole.Y1 = Patch->Height - 1;

			FSurfPatchCopy& Copy = Out.AddDefaulted_GetRef();
			Copy.Group = Group;
			Copy.Patch = CopyOf(*Patch, Whole);
		}

		if (Out.Num() == 0)
		{
			OutReason = TEXT("pick a patch first");
			return false;
		}
		return true;
	}

	// The selected points, per patch, in the order the patches were first
	// touched. Faces are their corners, already mirrored into the points.
	struct FShare
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		TArray<FIntPoint> Points;
	};
	TArray<FShare> Shares;

	for (const FSurfPointRef& Ref : PointSelection)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch || Ref.X < 0 || Ref.X >= Patch->Width || Ref.Y < 0 || Ref.Y >= Patch->Height)
		{
			continue;
		}

		FShare* Found = Shares.FindByPredicate([&](const FShare& S)
		{
			return S.Group == Group && S.PatchId == Ref.PatchId;
		});
		if (!Found)
		{
			Found = &Shares.AddDefaulted_GetRef();
			Found->Group = Group;
			Found->PatchId = Ref.PatchId;
		}
		Found->Points.AddUnique(FIntPoint(Ref.X, Ref.Y));
	}

	if (Shares.Num() == 0)
	{
		OutReason = TEXT("select faces (3), a block of points (1), or a patch (4) first");
		return false;
	}

	for (const FShare& Share : Shares)
	{
		const FSurfPatch* Patch = Share.Group->FindPatch(Share.PatchId);
		if (!Patch)
		{
			continue;
		}
		FSurfRect Rect;
		if (FillsRectangle(Share.Points, Rect))
		{
			FSurfPatchCopy& Copy = Out.AddDefaulted_GetRef();
			Copy.Group = Share.Group;
			Copy.Patch = CopyOf(*Patch, Rect);
			continue;
		}

		/**
		 * Not one rectangle (S163b, backlog 17): the cells whose four
		 * corners are all picked, as rectangles under one rule - the same
		 * rule the extrude and the cut use (SurfCells::Decompose), so an L
		 * copies as two patches, a ring as four. A point or a line with no
		 * cell behind it carries nothing, and is left where it is.
		 */
		FSurfCellSet Cells(Patch->Width - 1, Patch->Height - 1);
		TSet<FIntPoint> Picked(Share.Points);
		int32 Held = 0;
		for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
		{
			for (int32 X = 0; X + 1 < Patch->Width; ++X)
			{
				if (Picked.Contains(FIntPoint(X, Y)) && Picked.Contains(FIntPoint(X + 1, Y))
					&& Picked.Contains(FIntPoint(X, Y + 1)) && Picked.Contains(FIntPoint(X + 1, Y + 1)))
				{
					Cells.Set(X, Y);
					++Held;
				}
			}
		}
		if (Held == 0)
		{
			OutReason = SubMode == ESurfSubMode::Face
				? TEXT("the picked faces hold no whole cell")
				: TEXT("the selected points are not a block - a grid cannot carry a loose point or line");
			if (Shares.Num() > 1)
			{
				OutReason += FString::Printf(TEXT(" (on one of %d patches)"), Shares.Num());
			}
			return false;
		}
		for (const FSurfCellRect& Piece : SurfCells::Decompose(Cells))
		{
			FSurfRect Part;
			Part.X0 = Piece.X0;
			Part.Y0 = Piece.Y0;
			Part.X1 = Piece.X1 + 1;
			Part.Y1 = Piece.Y1 + 1;
			FSurfPatchCopy& Copy = Out.AddDefaulted_GetRef();
			Copy.Group = Share.Group;
			Copy.Patch = CopyOf(*Patch, Part);
		}
	}

	return true;
}

void USurfEdMode::SelectMadePatches(const TArray<FSurfPatchRef>& Made)
{
	if (Made.Num() == 0)
	{
		return;
	}

	// The copies are the selection, the way the sub-mode selects: the
	// patches themselves, all their faces, or all their points - so the move
	// that follows takes the copies and not what they came from.
	SelectPatch(Made[0]);
	PatchSelection = Made;

	if (SubMode == ESurfSubMode::Face)
	{
		QuadSelection.Reset();
		for (const FSurfPatchRef& Ref : Made)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch)
			{
				continue;
			}
			for (int32 Y = 0; Y + 1 < Patch->Height; ++Y)
			{
				for (int32 X = 0; X + 1 < Patch->Width; ++X)
				{
					FSurfQuadRef Quad;
					Quad.Group = Ref.Group;
					Quad.PatchId = Ref.PatchId;
					Quad.X = X;
					Quad.Y = Y;
					QuadSelection.Add(Quad);
				}
			}
		}
		SyncQuadSelectionToPoints();
	}
	else if (SubMode != ESurfSubMode::Object)
	{
		SelectAllPoints();
	}

	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

int32 USurfEdMode::DuplicateSelectedActors()
{
	/**
	 * Through the editor's own duplicate (S119a).
	 *
	 * It knows how to copy an actor, keep its attachments, name the copy and
	 * leave it selected - all of which we would otherwise write worse. The
	 * same reasoning as the prefab delete: where Unreal already does the job,
	 * the work is calling it, not rewriting it.
	 */
	UWorld* World = GetWorld();
	if (!GUnrealEd || !World || !World->GetCurrentLevel() || GEditor->GetSelectedActorCount() == 0)
	{
		return 0;
	}

	const int32 Before = GEditor->GetSelectedActorCount();
	GUnrealEd->edactDuplicateSelected(World->GetCurrentLevel(), /*bOffsetLocations*/ false);

	// What is selected after a duplicate is the copies, so the count is what
	// it was - the number made, not the number left standing.
	return Before;
}

bool USurfEdMode::DuplicateSelection()
{
	/**
	 * An actor is a thing too (S119a).
	 *
	 * "couldnt shift d to duplicate a prefab" - and the record said exactly
	 * why: `submode=vertex patch=0 points=0 faces=0 actors=1`, a key
	 * Duplicate, and `friction duplicate_refused`. G had already learned to
	 * move a selected actor (S94a); Shift+D had not learned to copy one, so
	 * with a prefab instance picked and nothing of ours, it gathered no
	 * patches and refused.
	 *
	 * Ours first, always: a patch selection is the specific thing and an
	 * actor selection is what is left when there is no specific thing.
	 */
	if (GEditor && GEditor->GetSelectedActorCount() > 0
		&& !SelectedPatch.IsValid() && PointSelection.Num() == 0
		&& Selection.Num() == 0 && PatchSelection.Num() == 0)
	{
		const int32 Made = DuplicateSelectedActors();
		if (Inspector)
		{
			Inspector->SetResult(Made > 0
				? FString::Printf(TEXT("duplicated %d actor%s - it is selected, and G moves it"),
					Made, Made == 1 ? TEXT("") : TEXT("s"))
				: FString(TEXT("duplicate: nothing could be copied")));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("duplicate_actors"), FString::Printf(TEXT("%d"), Made));
		if (GEditor)
		{
			RedrawViewports();
		}
		return Made > 0;
	}

	/**
	 * A primitive is copied whole (S172a): "shift d shall be a duplicate
	 * key" - with a box picked in 4 and nothing of a patch, the key
	 * gathered no patch and refused. The copies stand where the originals
	 * do, picked the way the originals were, and the move that follows
	 * carries them off.
	 */
	if (PointSelection.Num() == 0 && QuadSelection.Num() == 0 && !SelectedPatch.IsValid() && PatchSelection.Num() == 0
		&& (Selection.Num() > 0 || ActiveFace.IsValid()))
	{
		TArray<FSurfFaceRef> Picked = Selection;
		if (ActiveFace.IsValid())
		{
			Picked.AddUnique(ActiveFace);
		}
		FScopedTransaction BrushTransaction(LOCTEXT("DuplicatePrimitives", "Duplicate Primitive"));
		TMap<FGuid, FGuid> NewIdOf;
		TMap<FGuid, FGuid> FreshGroups;
		TArray<ASurfGroup*> Touched;
		for (const FSurfFaceRef& Ref : Picked)
		{
			ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (!Brush || NewIdOf.Contains(Ref.BrushId))
			{
				continue;
			}
			if (!Touched.Contains(Group))
			{
				Group->Modify();
				Touched.Add(Group);
			}
			FSurfBrush Copy = *Brush;
			Copy.Id = FGuid::NewGuid();
			// The copies are a group of their own (S249b).
			SurfGroupPaths::Fresh(Copy.GroupPath, FreshGroups);
			NewIdOf.Add(Ref.BrushId, Copy.Id);
			Group->AddBrush(MoveTemp(Copy));
		}
		if (NewIdOf.Num() == 0)
		{
			BrushTransaction.Cancel();
			return false;
		}
		for (ASurfGroup* Each : Touched)
		{
			Each->RebuildPreview();
		}
		TArray<FSurfFaceRef> Copies;
		for (const FSurfFaceRef& Ref : Picked)
		{
			if (const FGuid* NewId = NewIdOf.Find(Ref.BrushId))
			{
				FSurfFaceRef Moved = Ref;
				Moved.BrushId = *NewId;
				Copies.Add(Moved);
			}
		}
		Selection = Copies;
		ActiveFace = Copies.Num() > 0 ? Copies.Last() : FSurfFaceRef();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("duplicated %d primitive%s in place - move %s off"),
				NewIdOf.Num(), NewIdOf.Num() == 1 ? TEXT("") : TEXT("s"), NewIdOf.Num() == 1 ? TEXT("it") : TEXT("them")));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("duplicate_brushes"), FString::Printf(TEXT("made=%d"), NewIdOf.Num()));
		if (GEditor)
		{
			RedrawViewports();
		}
		return true;
	}

	TArray<FSurfPatchCopy> Copies;
	FString Reason;
	if (!GatherSelectionAsPatches(Copies, Reason))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString(TEXT("duplicate: ")) + Reason);
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("duplicate_refused"), Reason);
		return false;
	}

	const FString What = Describe(Copies);

	FScopedTransaction Transaction(LOCTEXT("DuplicateSelection", "Duplicate Selection"));

	TArray<FSurfPatchRef> Made;
	TArray<ASurfGroup*> Touched;
	// The copies are a group of their own (S249b): the same nesting, fresh ids.
	TMap<FGuid, FGuid> FreshGroups;
	for (FSurfPatchCopy& Copy : Copies)
	{
		ASurfGroup* Group = Copy.Group.Get();
		if (!Group)
		{
			continue;
		}

		Group->Modify();
		if (SubMode == ESurfSubMode::Object)
		{
			SurfGroupPaths::Fresh(Copy.Patch.GroupPath, FreshGroups);
		}
		else
		{
			Copy.Patch.GroupPath.Reset();
		}
		const FGuid Id = Copy.Patch.Id;
		FSurfPatch& Added = Group->AddPatch(MoveTemp(Copy.Patch));
		SurfPatchSurfacing::Reapply(Added);
		Touched.AddUnique(Group);

		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Id;
		Made.Add(Ref);
	}

	// Primitives picked with them in Object (a group of both, S249b) are
	// copied too, into the same fresh groups.
	TArray<FSurfFaceRef> BrushCopies;
	if (SubMode == ESurfSubMode::Object && Made.Num() > 0)
	{
		TArray<FSurfFaceRef> PickedSides = Selection;
		if (ActiveFace.IsValid())
		{
			PickedSides.AddUnique(ActiveFace);
		}
		TSet<FGuid> Seen;
		for (const FSurfFaceRef& Ref : PickedSides)
		{
			ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (!Brush || Seen.Contains(Ref.BrushId))
			{
				continue;
			}
			Seen.Add(Ref.BrushId);
			Group->Modify();
			FSurfBrush Copy = *Brush;
			Copy.Id = FGuid::NewGuid();
			SurfGroupPaths::Fresh(Copy.GroupPath, FreshGroups);
			const FGuid NewId = Copy.Id;
			const int32 Sides = Copy.Faces.Num();
			Group->AddBrush(MoveTemp(Copy));
			Touched.AddUnique(Group);
			for (int32 I = 0; I < Sides; ++I)
			{
				FSurfFaceRef Side;
				Side.Group = Group;
				Side.BrushId = NewId;
				Side.FaceIndex = I;
				BrushCopies.Add(Side);
			}
		}
	}

	if (Made.Num() == 0)
	{
		Transaction.Cancel();
		return false;
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	SelectMadePatches(Made);
	if (BrushCopies.Num() > 0)
	{
		Selection = BrushCopies;
		ActiveFace = BrushCopies[0];
	}

	// In place: a copy on top of the original is already where the next move
	// starts from. The cost is two surfaces in one place until it is moved,
	// which looks like flickering, so the line says so.
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("duplicated %s in place - move %s off, or the surfaces flicker against each other"),
			*What, Copies.Num() == 1 ? TEXT("it") : TEXT("them")));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("duplicate_selection"), FString::Printf(TEXT("made=%d"), Made.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::DuplicateAndMove(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	const int32 Before = UndoPosition();
	if (!DuplicateSelection())
	{
		// Declined, with the reason on the bar; the key is still ours.
		return true;
	}

	if (!ViewportClient || !Viewport)
	{
		return true;
	}

	// The copy follows the mouse until it is confirmed, as after an extrude.
	// Cancelled, or confirmed without moving, and the copy goes too - it sat
	// exactly on its source and would stay there, two surfaces in one place.
	BeginModal(ESurfModal::Move, ViewportClient, Viewport);
	bModalFollowsExtrude = Modal == ESurfModal::Move;
	ExtrudeUndoPosition = Before;
	ModalFollowsVerb = TEXT("duplicate");
	return true;
}

bool USurfEdMode::CopySelection()
{
	TArray<FSurfPatchCopy> Copies;
	FString Reason;
	if (!GatherSelectionAsPatches(Copies, Reason))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString(TEXT("copy: ")) + Reason);
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("copy_refused"));
		return false;
	}

	// In world space, so the paste lands where the copy was taken from
	// whichever group it goes into.
	PatchClipboard.Reset();
	for (FSurfPatchCopy& Copy : Copies)
	{
		const ASurfGroup* Group = Copy.Group.Get();
		const FTransform ToWorld = Group ? Group->GetActorTransform() : FTransform::Identity;
		for (FSurfControlPoint& Point : Copy.Patch.Points)
		{
			Point.Pos = ToWorld.TransformPosition(Point.Pos);
		}
		PatchClipboard.Add(MoveTemp(Copy.Patch));
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("copied %s - Ctrl+V pastes it in place"), *Describe(Copies)));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("copy_selection"), FString::Printf(TEXT("patches=%d"), PatchClipboard.Num()));
	return true;
}

bool USurfEdMode::CutSelection()
{
	// Delete takes whole rows or columns from a point selection, which is
	// more than a block; faces and patches come out exactly.
	if (SubMode != ESurfSubMode::Object && SubMode != ESurfSubMode::Face)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("cut: pick faces (3) or a patch (4) to cut out - Ctrl+C copies a block of points"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("cut_refused"));
		return false;
	}

	if (!CopySelection())
	{
		return false;
	}

	const int32 Count = PatchClipboard.Num();
	if (!DeleteSelection())
	{
		return false;
	}

	if (Inspector)
	{
		Inspector->SetResult(Count == 1
			? FString(TEXT("cut a patch to the clipboard - Ctrl+V pastes it back in place"))
			: FString::Printf(TEXT("cut %d patches to the clipboard - Ctrl+V pastes them back in place"), Count));
		Inspector->RefreshSelectionCount();
	}
	return true;
}

bool USurfEdMode::PasteClipboard()
{
	if (PatchClipboard.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("paste: nothing copied yet - Ctrl+C copies the selection"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("paste_empty"));
		return false;
	}

	// Into the group being edited, else the first one there is, else a new
	// one at the origin - the clipboard is in world space, so the patches
	// land where they were copied from whichever it is.
	ASurfGroup* Group = IsInsidePrefab() ? GetDrilledGroup() : SelectedPatch.Group.Get();
	/**
	 * Paste joins the group everything else joins (S149c).
	 *
	 * It walked the world and took **whichever group the iterator handed
	 * back first** - the exact thing S76c found and S140a wrote a rule
	 * against - and, failing that, spawned one of its own that no later
	 * verb would ever find again. One question, one answer:
	 * GroupForNewGeometry is where "which group does new geometry go into"
	 * is decided, and it marks what it makes.
	 */
	UWorld* World = GetWorld();

	FScopedTransaction Transaction(LOCTEXT("PastePatches", "Paste Patches"));

	if (!Group && World)
	{
		Group = GroupForNewGeometry();
	}
	if (!Group)
	{
		Transaction.Cancel();
		return false;
	}

	Group->Modify();
	const FTransform ToLocal = Group->GetActorTransform().Inverse();

	TArray<FSurfPatchRef> Made;
	// Pasted pieces are a group of their own (S249b).
	TMap<FGuid, FGuid> FreshGroups;
	for (const FSurfPatch& Stored : PatchClipboard)
	{
		FSurfPatch Copy = Stored;
		Copy.Id = FGuid::NewGuid();
		SurfGroupPaths::Fresh(Copy.GroupPath, FreshGroups);
		Copy.EditSerial = 0;
		for (FSurfControlPoint& Point : Copy.Points)
		{
			Point.Pos = ToLocal.TransformPosition(Point.Pos);
		}

		const FGuid Id = Copy.Id;
		FSurfPatch& Added = Group->AddPatch(MoveTemp(Copy));
		SurfPatchSurfacing::Reapply(Added);

		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Id;
		Made.Add(Ref);
	}

	Group->RebuildPreview();
	SelectMadePatches(Made);

	if (Inspector)
	{
		Inspector->SetResult(Made.Num() == 1
			? FString(TEXT("pasted a patch where it was copied from - move it off, or the two surfaces flicker against each other"))
			: FString::Printf(TEXT("pasted %d patches where they were copied from - move them off, or the surfaces flicker against each other"), Made.Num()));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("paste_patches"), FString::Printf(TEXT("patches=%d"), Made.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
