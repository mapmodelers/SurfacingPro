// Copyright MapModelers. All Rights Reserved.

// Groups (S249b). "Group Patches / Ungroup Patches ... so grouped select
// together and edit together" - and then: "Group is right word", primitives
// too, "Alt click is good", "grouping multi select automatically. i guess
// nested is interesting."
//
// A group is a mark on the pieces, not a container: each patch and each
// primitive carries the ids of the groups it is in, outermost first. Pieces
// of one group may stand in several of our actors; nothing else of the level
// changes, and a level with no groups loads as it did.

#include "SurfEdMode.h"
#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfBrushMath.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SurfacingProGroups"

namespace SurfGroupPaths
{
	void Fresh(TArray<FGuid>& Path, TMap<FGuid, FGuid>& NewIdOf)
	{
		for (FGuid& Id : Path)
		{
			if (const FGuid* Made = NewIdOf.Find(Id))
			{
				Id = *Made;
			}
			else
			{
				const FGuid New = FGuid::NewGuid();
				NewIdOf.Add(Id, New);
				Id = New;
			}
		}
	}
}

namespace
{
	/** The groups among these whose every member is picked. */
	void WholeGroups(const TSet<FSurfPieceRef>& Picked, const TMap<FGuid, TArray<FSurfPieceRef>>& Members, TSet<FGuid>& Out)
	{
		for (const TPair<FGuid, TArray<FSurfPieceRef>>& Pair : Members)
		{
			bool bAll = Pair.Value.Num() > 0;
			for (const FSurfPieceRef& Each : Pair.Value)
			{
				if (!Picked.Contains(Each))
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				Out.Add(Pair.Key);
			}
		}
	}
}

const TArray<FGuid>* USurfEdMode::PathOfPiece(const FSurfPieceRef& Piece) const
{
	const ASurfGroup* Group = Piece.Group.Get();
	if (!Group)
	{
		return nullptr;
	}
	if (Piece.bBrush)
	{
		const FSurfBrush* Brush = Group->FindBrush(Piece.Id);
		return Brush ? &Brush->GroupPath : nullptr;
	}
	const FSurfPatch* Patch = Group->FindPatch(Piece.Id);
	return Patch ? &Patch->GroupPath : nullptr;
}

TArray<FGuid> USurfEdMode::GroupPathOf(const FSurfPieceRef& Piece) const
{
	const TArray<FGuid>* Path = PathOfPiece(Piece);
	return Path ? *Path : TArray<FGuid>();
}

void USurfEdMode::GatherPickedPieces(TArray<FSurfPieceRef>& Out) const
{
	Out.Reset();
	for (const FSurfPatchRef& Ref : GetSelectedPatches())
	{
		FSurfPieceRef Piece;
		Piece.Group = Ref.Group;
		Piece.Id = Ref.PatchId;
		Out.AddUnique(Piece);
	}
	auto AddBrush = [&Out](const FSurfFaceRef& Ref)
	{
		if (!Ref.Group.IsValid() || !Ref.BrushId.IsValid())
		{
			return;
		}
		FSurfPieceRef Piece;
		Piece.Group = Ref.Group;
		Piece.Id = Ref.BrushId;
		Piece.bBrush = true;
		Out.AddUnique(Piece);
	};
	for (const FSurfFaceRef& Ref : Selection)
	{
		AddBrush(Ref);
	}
	if (ActiveFace.IsValid())
	{
		AddBrush(ActiveFace);
	}
}

int32 USurfEdMode::CountPickedPieces() const
{
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	return Picked.Num();
}

void USurfEdMode::GatherMembersOf(const TSet<FGuid>& Ids, TMap<FGuid, TArray<FSurfPieceRef>>& Out) const
{
	Out.Reset();
	UWorld* World = GetWorld();
	if (!World || Ids.Num() == 0)
	{
		return;
	}
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			for (const FGuid& Id : Patch.GroupPath)
			{
				if (Ids.Contains(Id))
				{
					FSurfPieceRef Piece;
					Piece.Group = Group;
					Piece.Id = Patch.Id;
					Out.FindOrAdd(Id).Add(Piece);
				}
			}
		});
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			for (const FGuid& Id : Brush.GroupPath)
			{
				if (Ids.Contains(Id))
				{
					FSurfPieceRef Piece;
					Piece.Group = Group;
					Piece.Id = Brush.Id;
					Piece.bBrush = true;
					Out.FindOrAdd(Id).Add(Piece);
				}
			}
		});
	}
}

bool USurfEdMode::SetPiecePath(const FSurfPieceRef& Piece, const TArray<FGuid>& Path)
{
	ASurfGroup* Group = Piece.Group.Get();
	if (!Group)
	{
		return false;
	}
	if (Piece.bBrush)
	{
		if (FSurfBrush* Brush = Group->ModifyBrush(Piece.Id))
		{
			Brush->GroupPath = Path;
			return true;
		}
		return false;
	}
	if (FSurfPatch* Patch = Group->ModifyPatch(Piece.Id))
	{
		Patch->GroupPath = Path;
		return true;
	}
	return false;
}

void USurfEdMode::PickPieces(const TArray<FSurfPieceRef>& Pieces, const FSurfPieceRef& Active, int32 ActiveFaceIndex, int32 Op)
{
	if (Op == 0)
	{
		ClearPointSelection();
		DropPatchPick();
		Selection.Reset();
		ActiveFace = FSurfFaceRef();
		EditSet.Reset();
	}

	FSurfPatchRef LastPatch;
	for (const FSurfPieceRef& Piece : Pieces)
	{
		ASurfGroup* Group = Piece.Group.Get();
		if (!Group)
		{
			continue;
		}
		if (Piece.bBrush)
		{
			const FSurfBrush* Brush = Group->FindBrush(Piece.Id);
			if (!Brush)
			{
				continue;
			}
			auto Same = [&Piece](const FSurfFaceRef& Each) { return Each.Group == Piece.Group && Each.BrushId == Piece.Id; };
			if (Op == 2)
			{
				Selection.RemoveAll(Same);
				if (ActiveFace.IsValid() && Same(ActiveFace))
				{
					ActiveFace = Selection.Num() > 0 ? Selection.Last() : FSurfFaceRef();
				}
				continue;
			}
			for (int32 I = 0; I < Brush->Faces.Num(); ++I)
			{
				FSurfFaceRef Ref;
				Ref.Group = Group;
				Ref.BrushId = Piece.Id;
				Ref.FaceIndex = I;
				Selection.AddUnique(Ref);
			}
			continue;
		}

		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Piece.Id;
		if (Op == 2)
		{
			if (PatchSelection.Contains(Ref) || SelectedPatch == Ref)
			{
				AddPatchToSelection(Ref, /*bToggle*/ true);
			}
			continue;
		}
		if (PatchSelection.Num() == 0 && SelectedPatch.IsValid())
		{
			PatchSelection.Add(SelectedPatch);
		}
		PatchSelection.AddUnique(Ref);
		LastPatch = Ref;
	}

	if (Op != 2)
	{
		// The piece clicked is the active one; with a primitive clicked, a
		// patch of the pick still holds the patch side of it, so G and the
		// gizmo move the patches and carry the primitives.
		if (Active.Id.IsValid() && !Active.bBrush)
		{
			SelectedPatch.Group = Active.Group;
			SelectedPatch.PatchId = Active.Id;
		}
		else
		{
			if (Active.Id.IsValid() && Active.bBrush)
			{
				FSurfFaceRef Face;
				Face.Group = Active.Group;
				Face.BrushId = Active.Id;
				Face.FaceIndex = FMath::Max(0, ActiveFaceIndex);
				ActiveFace = Face;
			}
			if (!SelectedPatch.IsValid() && LastPatch.IsValid())
			{
				SelectedPatch = LastPatch;
			}
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
		if (ActiveFace.IsValid())
		{
			Inspector->PullFromActiveFace();
		}
	}
	if (GEditor)
	{
		RedrawViewports();
	}
}

bool USurfEdMode::ClickGroupPiece(const FSurfPieceRef& Piece, int32 FaceIndex, bool bShift, bool bCtrl, bool bAlt)
{
	const TArray<FGuid>* PathFound = PathOfPiece(Piece);
	const bool bAltAlone = bAlt && !bShift && !bCtrl;
	if (!bAltAlone || !(AltClimbPiece == Piece))
	{
		AltClimbLevel = -1;
	}
	if (!PathFound || PathFound->Num() == 0)
	{
		AltClimbPiece = FSurfPieceRef();
		return false;
	}
	const TArray<FGuid> Path = *PathFound;

	if (bAltAlone)
	{
		// The piece alone; again, the group round it; again, the one round
		// that; past the outermost, the piece again.
		int32 Level = Path.Num();
		if (AltClimbLevel >= 0)
		{
			Level = AltClimbLevel - 1;
			if (Level < 0)
			{
				Level = Path.Num();
			}
		}
		AltClimbPiece = Piece;
		AltClimbLevel = Level;

		TArray<FSurfPieceRef> Pick;
		if (Level == Path.Num())
		{
			Pick.Add(Piece);
		}
		else
		{
			TMap<FGuid, TArray<FSurfPieceRef>> Found;
			GatherMembersOf(TSet<FGuid>{ Path[Level] }, Found);
			Pick = Found.FindRef(Path[Level]);
		}
		PickPieces(Pick, Piece, FaceIndex, 0);
		if (Inspector)
		{
			Inspector->SetResult(Level == Path.Num()
				? FString(TEXT("one piece of its group - Alt+click it again for the group round it"))
				: FString::Printf(TEXT("a group of %d%s - Alt+click again for %s"), Pick.Num(),
					Level > 0 ? TEXT(", inside another") : TEXT(""), Level > 0 ? TEXT("the one round it") : TEXT("the piece alone")));
		}
		SurfSession::Selection(FString::Printf(TEXT("group_alt level=%d of=%d picked=%d"), Level, Path.Num(), Pick.Num()));
		return true;
	}

	AltClimbPiece = FSurfPieceRef();
	TMap<FGuid, TArray<FSurfPieceRef>> Found;
	GatherMembersOf(TSet<FGuid>{ Path[0] }, Found);
	const TArray<FSurfPieceRef> Members = Found.FindRef(Path[0]);
	int32 Op = bShift ? 1 : 0;
	if (bCtrl)
	{
		// Ctrl takes the group out when the piece is in, as it takes one piece.
		TArray<FSurfPieceRef> Picked;
		GatherPickedPieces(Picked);
		Op = Picked.Contains(Piece) ? 2 : 1;
	}
	PickPieces(Members, Piece, FaceIndex, Op);
	SurfSession::Selection(FString::Printf(TEXT("group op=%d members=%d depth=%d"), Op, Members.Num(), Path.Num()));
	return true;
}

bool USurfEdMode::ExpandSelectionToGroups()
{
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	TSet<FGuid> Outer;
	for (const FSurfPieceRef& Piece : Picked)
	{
		const TArray<FGuid>* Path = PathOfPiece(Piece);
		if (Path && Path->Num() > 0)
		{
			Outer.Add((*Path)[0]);
		}
	}
	if (Outer.Num() == 0)
	{
		return false;
	}
	TMap<FGuid, TArray<FSurfPieceRef>> Members;
	GatherMembersOf(Outer, Members);
	TArray<FSurfPieceRef> Adding;
	for (const TPair<FGuid, TArray<FSurfPieceRef>>& Pair : Members)
	{
		for (const FSurfPieceRef& Each : Pair.Value)
		{
			if (!Picked.Contains(Each))
			{
				Adding.AddUnique(Each);
			}
		}
	}
	if (Adding.Num() == 0)
	{
		return false;
	}
	PickPieces(Adding, FSurfPieceRef(), INDEX_NONE, 1);
	return true;
}

bool USurfEdMode::GroupSelection()
{
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	if (Picked.Num() < 2)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("group: pick two pieces or more - patches, primitives, or groups"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("group_refused"), FString::Printf(TEXT("pieces=%d"), Picked.Num()));
		return false;
	}

	TArray<TArray<FGuid>> Paths;
	TSet<FGuid> Ids;
	for (const FSurfPieceRef& Piece : Picked)
	{
		const TArray<FGuid>* Path = PathOfPiece(Piece);
		Paths.Add(Path ? *Path : TArray<FGuid>());
		for (const FGuid& Id : Paths.Last())
		{
			Ids.Add(Id);
		}
	}

	// What they all share stays round the new group: grouping pieces inside
	// a group makes a group inside it.
	int32 Common = Paths[0].Num();
	for (const TArray<FGuid>& Path : Paths)
	{
		int32 Same = 0;
		while (Same < Common && Same < Path.Num() && Path[Same] == Paths[0][Same])
		{
			++Same;
		}
		Common = Same;
	}

	// A group picked whole goes inside the new one; a group picked in part
	// lets the picked pieces go.
	TMap<FGuid, TArray<FSurfPieceRef>> Members;
	GatherMembersOf(Ids, Members);
	TSet<FGuid> Whole;
	WholeGroups(TSet<FSurfPieceRef>(Picked), Members, Whole);

	TSet<FString> Units;
	for (int32 I = 0; I < Picked.Num(); ++I)
	{
		FString Unit = Picked[I].Id.ToString();
		for (int32 J = Common; J < Paths[I].Num(); ++J)
		{
			if (Whole.Contains(Paths[I][J]))
			{
				Unit = Paths[I][J].ToString();
				break;
			}
		}
		Units.Add(Unit);
	}
	if (Units.Num() < 2)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("group: that is one group already - Alt+click pieces inside it to group some of them"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("group_refused"), TEXT("one group already"));
		return false;
	}

	const FGuid New = FGuid::NewGuid();
	FScopedTransaction Transaction(LOCTEXT("GroupPieces", "Group"));
	int32 Done = 0;
	for (int32 I = 0; I < Picked.Num(); ++I)
	{
		TArray<FGuid> Path;
		for (int32 J = 0; J < Common; ++J)
		{
			Path.Add(Paths[I][J]);
		}
		Path.Add(New);
		for (int32 J = Common; J < Paths[I].Num(); ++J)
		{
			if (Whole.Contains(Paths[I][J]))
			{
				Path.Add(Paths[I][J]);
			}
		}
		Done += SetPiecePath(Picked[I], Path) ? 1 : 0;
	}
	if (Done == 0)
	{
		Transaction.Cancel();
		return false;
	}
	AltClimbPiece = FSurfPieceRef();
	AltClimbLevel = -1;
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("grouped %d pieces%s - a click picks them all, Alt+click one alone; Ctrl+Alt+G ungroups"),
			Done, Common > 0 ? TEXT(", inside the group they were in") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("group"), FString::Printf(TEXT("pieces=%d units=%d depth=%d"), Done, Units.Num(), Common + 1));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::UngroupSelection()
{
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	TArray<TArray<FGuid>> Paths;
	TSet<FGuid> Ids;
	for (const FSurfPieceRef& Piece : Picked)
	{
		const TArray<FGuid>* Path = PathOfPiece(Piece);
		Paths.Add(Path ? *Path : TArray<FGuid>());
		for (const FGuid& Id : Paths.Last())
		{
			Ids.Add(Id);
		}
	}
	if (Ids.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("ungroup: nothing picked is in a group"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("ungroup_refused"), FString::Printf(TEXT("pieces=%d"), Picked.Num()));
		return false;
	}

	TMap<FGuid, TArray<FSurfPieceRef>> Members;
	GatherMembersOf(Ids, Members);
	TSet<FGuid> Whole;
	WholeGroups(TSet<FSurfPieceRef>(Picked), Members, Whole);

	// A group picked whole comes apart one level; with none picked whole,
	// the picked pieces leave the group they are in.
	FScopedTransaction Transaction(LOCTEXT("UngroupPieces", "Ungroup"));
	TSet<FGuid> Undone;
	int32 Done = 0;
	for (int32 I = 0; I < Picked.Num(); ++I)
	{
		TArray<FGuid> Path = Paths[I];
		int32 Drop = INDEX_NONE;
		if (Whole.Num() > 0)
		{
			for (int32 J = 0; J < Path.Num(); ++J)
			{
				if (Whole.Contains(Path[J]))
				{
					Drop = J;
					break;
				}
			}
		}
		else if (Path.Num() > 0)
		{
			Drop = Path.Num() - 1;
		}
		if (Drop == INDEX_NONE)
		{
			continue;
		}
		Undone.Add(Path[Drop]);
		Path.RemoveAt(Drop);
		Done += SetPiecePath(Picked[I], Path) ? 1 : 0;
	}
	if (Done == 0)
	{
		Transaction.Cancel();
		return false;
	}
	AltClimbPiece = FSurfPieceRef();
	AltClimbLevel = -1;
	if (Inspector)
	{
		Inspector->SetResult(Whole.Num() > 0
			? FString::Printf(TEXT("ungrouped %d group%s, one level - %d piece%s"), Undone.Num(), Undone.Num() == 1 ? TEXT("") : TEXT("s"), Done, Done == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(TEXT("%d piece%s out of %s group"), Done, Done == 1 ? TEXT("") : TEXT("s"), Done == 1 ? TEXT("its") : TEXT("their")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("ungroup"), FString::Printf(TEXT("pieces=%d groups=%d whole=%d"), Done, Undone.Num(), Whole.Num() > 0 ? 1 : 0));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

FString USurfEdMode::DescribePickedGroups() const
{
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	TSet<FGuid> Outer;
	for (const FSurfPieceRef& Piece : Picked)
	{
		const TArray<FGuid>* Path = PathOfPiece(Piece);
		if (Path && Path->Num() > 0)
		{
			Outer.Add((*Path)[0]);
		}
	}
	if (Outer.Num() == 0)
	{
		return FString();
	}
	if (Outer.Num() > 1)
	{
		return FString::Printf(TEXT("%d groups"), Outer.Num());
	}
	TMap<FGuid, TArray<FSurfPieceRef>> Members;
	GatherMembersOf(Outer, Members);
	TSet<FGuid> Whole;
	WholeGroups(TSet<FSurfPieceRef>(Picked), Members, Whole);
	const FGuid Id = *Outer.CreateConstIterator();
	const int32 Count = Members.FindRef(Id).Num();
	return Whole.Contains(Id) ? FString::Printf(TEXT("Group of %d"), Count) : FString::Printf(TEXT("In a group of %d"), Count);
}

void USurfEdMode::PickedGroupBounds(TArray<FBox>& Out, TArray<int32>* OutCounts) const
{
	Out.Reset();
	if (OutCounts)
	{
		OutCounts->Reset();
	}
	TArray<FSurfPieceRef> Picked;
	GatherPickedPieces(Picked);
	TArray<TArray<FGuid>> Paths;
	TSet<FGuid> Ids;
	for (const FSurfPieceRef& Piece : Picked)
	{
		const TArray<FGuid>* Path = PathOfPiece(Piece);
		Paths.Add(Path ? *Path : TArray<FGuid>());
		for (const FGuid& Id : Paths.Last())
		{
			Ids.Add(Id);
		}
	}
	if (Ids.Num() == 0)
	{
		return;
	}
	TMap<FGuid, TArray<FSurfPieceRef>> Members;
	GatherMembersOf(Ids, Members);
	TSet<FGuid> Whole;
	WholeGroups(TSet<FSurfPieceRef>(Picked), Members, Whole);

	// Each piece's largest group picked whole.
	TSet<FGuid> Shown;
	for (const TArray<FGuid>& Path : Paths)
	{
		for (const FGuid& Id : Path)
		{
			if (Whole.Contains(Id))
			{
				Shown.Add(Id);
				break;
			}
		}
	}
	for (const FGuid& Id : Shown)
	{
		FBox Box(ForceInit);
		for (const FSurfPieceRef& Each : Members.FindRef(Id))
		{
			const ASurfGroup* Group = Each.Group.Get();
			if (!Group)
			{
				continue;
			}
			const FTransform ToWorld = Group->GetActorTransform();
			if (Each.bBrush)
			{
				const FSurfBrush* Brush = Group->FindBrush(Each.Id);
				FSurfHull Hull;
				if (Brush && SurfBrushMath::DeriveHull(*Brush, Hull))
				{
					for (const FVector& Corner : Hull.Vertices)
					{
						Box += ToWorld.TransformPosition(Corner);
					}
				}
			}
			else if (const FSurfPatch* Patch = Group->FindPatch(Each.Id))
			{
				for (const FSurfControlPoint& Point : Patch->Points)
				{
					Box += ToWorld.TransformPosition(Point.Pos);
				}
			}
		}
		if (Box.IsValid)
		{
			Out.Add(Box);
			if (OutCounts)
			{
				OutCounts->Add(Members.FindRef(Id).Num());
			}
		}
	}
}

void USurfEdMode::AllGroupBounds(TArray<FBox>& Out, TArray<int32>* OutCounts) const
{
	Out.Reset();
	if (OutCounts)
	{
		OutCounts->Reset();
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	TArray<FGuid> Order;
	TMap<FGuid, FBox> Boxes;
	TMap<FGuid, int32> Counts;
	auto Add = [&Order, &Boxes, &Counts](const FGuid& Id, const FBox& Box)
	{
		if (!Box.IsValid)
		{
			return;
		}
		if (FBox* Found = Boxes.Find(Id))
		{
			*Found += Box;
			++Counts.FindOrAdd(Id);
			return;
		}
		Order.Add(Id);
		Boxes.Add(Id, Box);
		Counts.Add(Id, 1);
	};
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (Patch.GroupPath.Num() == 0)
			{
				return;
			}
			FBox Box(ForceInit);
			for (const FSurfControlPoint& Point : Patch.Points)
			{
				Box += ToWorld.TransformPosition(Point.Pos);
			}
			Add(Patch.GroupPath[0], Box);
		});
		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			if (Brush.GroupPath.Num() == 0)
			{
				return;
			}
			FBox Box(ForceInit);
			FSurfHull Hull;
			if (SurfBrushMath::DeriveHull(Brush, Hull))
			{
				for (const FVector& Corner : Hull.Vertices)
				{
					Box += ToWorld.TransformPosition(Corner);
				}
			}
			Add(Brush.GroupPath[0], Box);
		});
	}
	for (const FGuid& Id : Order)
	{
		Out.Add(Boxes.FindRef(Id));
		if (OutCounts)
		{
			OutCounts->Add(Counts.FindRef(Id));
		}
	}
}

void USurfEdMode::CarryPickedBrushes(const FMatrix& WorldMatrix)
{
	if (SubMode != ESurfSubMode::Object || !SelectedPatch.IsValid() || (Selection.Num() == 0 && !ActiveFace.IsValid()))
	{
		return;
	}
	TArray<FSurfPieceRef> Pieces;
	GatherPickedPieces(Pieces);
	TArray<ASurfGroup*> Touched;
	for (const FSurfPieceRef& Piece : Pieces)
	{
		if (!Piece.bBrush)
		{
			continue;
		}
		ASurfGroup* Group = Piece.Group.Get();
		FSurfBrush* Brush = Group ? Group->ModifyBrush(Piece.Id) : nullptr;
		if (!Brush)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		const FMatrix Local = ToWorld.ToMatrixWithScale() * WorldMatrix * ToWorld.Inverse().ToMatrixWithScale();
		const TArray<FSurfFace> Was = Brush->Faces;
		for (FSurfFace& Face : Brush->Faces)
		{
			Face.Plane = Face.Plane.TransformBy(Local);
		}
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			Brush->Faces = Was;
			continue;
		}
		++Brush->EditSerial;
		Touched.AddUnique(Group);
	}
	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}
}

#undef LOCTEXT_NAMESPACE
