// Copyright MapModelers. All Rights Reserved.
//
// Prefabs from the mode's side (S65a): making one from the selection. The
// geometry leaves the group for the asset, and one instance stands where it
// was. Placing it again is the Content Browser's drag, or a duplicate of the
// instance; editing it is drill-in (S65b).

#include "SurfEdMode.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "LevelInstance/LevelInstanceActor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "SurfSession.h"
#include "Containers/Ticker.h"
#include "Selection.h"
#include "SurfBrushMath.h"
#include "SurfBake.h"
#include "SurfSettings.h"
#include "SurfCamera.h"
#include "LevelEditorViewport.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

namespace
{
	ASurfPrefabInstance* SelectedInstance();

	// The asset to disk, where its package says (S71c): a prefab is a
	// library asset, and a library asset that only exists until the editor
	// closes is not one. the author: "need to ask for prefab name to save when
	// creating new".
	bool SavePrefabPackage(USurfPrefab* Asset)
	{
		if (!Asset)
		{
			return false;
		}
		UPackage* Package = Asset->GetOutermost();
		if (!Package || Package == GetTransientPackage())
		{
			return false;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		SurfSession::Verb(TEXT("save_prefab"), FString::Printf(TEXT("%s ok=%d"), *Asset->GetName(), bSaved ? 1 : 0));
		return bSaved;
	}

	bool PrefabNameFree(const FString& Path)
	{
		return !FPackageName::DoesPackageExist(Path) && !FindPackage(nullptr, *Path);
	}

	// The name asked for, made safe for an asset and unique (a _2 when it
	// is taken); else Prefab_1, Prefab_2, ... under the plugin's project
	// folder, the first nothing else has, on disk or in memory. No PF_ in
	// front since S228a ("user can search them by prefab anyway").
	FString FreePrefabName(FString& OutPackagePath, const FString& Wanted = FString())
	{
		const FString Folder = TEXT("/Game/SurfacingPro/Prefabs");

		FString Clean;
		for (const TCHAR C : Wanted.TrimStartAndEnd())
		{
			Clean.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') ? C : TEXT('_'));
		}
		if (!Clean.IsEmpty())
		{
			if (FChar::IsDigit(Clean[0]))
			{
				Clean = TEXT("Prefab_") + Clean;
			}
			for (int32 N = 1; N < 10000; ++N)
			{
				const FString Name = N == 1 ? Clean : FString::Printf(TEXT("%s_%d"), *Clean, N);
				const FString Path = Folder / Name;
				if (PrefabNameFree(Path))
				{
					OutPackagePath = Path;
					return Name;
				}
			}
		}

		for (int32 N = 1; N < 10000; ++N)
		{
			const FString Name = FString::Printf(TEXT("Prefab_%d"), N);
			const FString Path = Folder / Name;
			if (PrefabNameFree(Path))
			{
				OutPackagePath = Path;
				return Name;
			}
		}
		OutPackagePath = Folder / TEXT("Prefab");
		return TEXT("Prefab");
	}
}

FString USurfEdMode::SuggestPrefabName() const
{
	FString Path;
	return FreePrefabName(Path);
}

namespace
{
	/** The actors picked in the level that a prefab carries as parts: anything that is not ours. */
	TArray<AActor*> PickedForeignActors()
	{
		TArray<AActor*> Out;
		if (!GEditor)
		{
			return Out;
		}
		if (USelection* Selected = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*Selected); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (Actor && IsValid(Actor) && !Actor->IsA(ASurfGroup::StaticClass()) && !Actor->IsA(ASurfPrefabInstance::StaticClass()))
				{
					Out.Add(Actor);
				}
			}
		}
		return Out;
	}
}

bool USurfEdMode::CanMakePrefab() const
{
	return PatchesHeldBySelection().Num() > 0 || GetSelectedPatch().IsValid() || Selection.Num() > 0 || ActiveFace.IsValid()
		|| PickedForeignActors().Num() > 0;
}

bool USurfEdMode::MakePrefabFromSelection(const FString& WantedName)
{
	TArray<FSurfPatchRef> Targets = GetSelectedPatches();
	for (const FSurfPatchRef& Held : PatchesHeldBySelection())
	{
		Targets.AddUnique(Held);
	}

	/**
	 * Primitives and anything else picked go in too (S172a): "we should be
	 * able to include primitive (and any other asset, actually, like even
	 * static meshes) to being within a prefab". The primitives become the
	 * prefab's own geometry, as the patches do; a mesh, a blueprint, a light
	 * becomes a part it carries, as Open's "Add the selection" does.
	 */
	TArray<FSurfFaceRef> BrushTargets;
	{
		TArray<FGuid> Seen;
		for (const FSurfFaceRef& Held : BrushesHeldBySelection())
		{
			if (!Seen.Contains(Held.BrushId))
			{
				Seen.Add(Held.BrushId);
				BrushTargets.Add(Held);
			}
		}
	}
	const TArray<AActor*> PartActors = PickedForeignActors();

	UWorld* World = GetWorld();

	// Inside a room with no patches picked but an instance selected: the
	// instance is taken in as a placement of the prefab being edited (S71b)
	// - the library's door placed in the house.
	if (Targets.Num() == 0 && BrushTargets.Num() == 0 && World && IsInsidePrefab())
	{
		ASurfPrefabInstance* Taken = SelectedInstance();
		ASurfPrefabInstance* Drilled = DrilledInstance.Get();
		USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
		if (Taken && RoomAsset && Taken != Drilled && Taken != RoomStandIn.Get() && Taken->Prefab)
		{
			if (!RoomAsset->CanPlace(Taken->Prefab))
			{
				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("prefab: %s cannot go inside %s - it would contain itself"),
						*Taken->Prefab->GetName(), *RoomAsset->GetName()));
				}
				SurfSession::Friction(TEXT("place_would_cycle"));
				return false;
			}
			FScopedTransaction Transaction(LOCTEXT("TakePrefabIn", "Place Prefab Inside"));
			const FTransform Relative = Taken->GetActorTransform().GetRelativeTransform(RoomFrame);
			USurfPrefab* Child = Taken->Prefab;
			RoomAsset->Place(Child, Relative);
			RoomAsset->MarkPackageDirty();
			Taken->Modify();
			Taken->Destroy();
			if (GEditor)
			{
				GEditor->SelectNone(false, true);
			}
			if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
			{
				StandIn->RebuildPreview();
			}
			if (RoomAsset->Placements.Num() > 0)
			{
				PickPlacement(RoomAsset->Placements.Last().Id);
			}
			if (Inspector)
			{
				Inspector->SetResult(FString::Printf(TEXT("%s placed inside %s - Alt+PgDn opens it, Delete takes it out"),
					*Child->GetName(), *RoomAsset->GetName()));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Verb(TEXT("place_prefab"), FString::Printf(TEXT("child=%s inside=%s"), *Child->GetName(), *RoomAsset->GetName()));
			if (GEditor)
			{
				RedrawViewports();
			}
			return true;
		}
	}

	if ((Targets.Num() == 0 && BrushTargets.Num() == 0 && PartActors.Num() == 0) || !World)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("prefab: pick what to make it from - patches, primitives, meshes, anything"));
		}
		return false;
	}

	// Where the prefab stands: the middle of its footprint, at its lowest
	// point, so a prefab placed on a floor sits on the floor and its origin
	// is where a hand would put it down. Every point is measured from there.
	FBox Bounds(ForceInit);
	for (const FSurfPatchRef& Ref : Targets)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FSurfControlPoint& Point : Patch->Points)
		{
			Bounds += ToWorld.TransformPosition(Point.Pos);
		}
	}
	for (const FSurfFaceRef& Ref : BrushTargets)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		FSurfHull Hull;
		if (!Brush || !SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();
		for (const FVector& Corner : Hull.Vertices)
		{
			Bounds += ToWorld.TransformPosition(Corner);
		}
	}
	for (const AActor* Actor : PartActors)
	{
		FVector Origin, Extent;
		Actor->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
		if (Extent.IsNearlyZero())
		{
			Bounds += Actor->GetActorLocation();
		}
		else
		{
			Bounds += FBox(Origin - Extent, Origin + Extent);
		}
	}
	if (!Bounds.IsValid)
	{
		return false;
	}
	// On the grid (S71b): the middle of the footprint is a half-cell for
	// any odd-width piece, and an origin off the grid put the asset's own
	// geometry off it, and every placement after. The nearest grid line,
	// each way, and the floor the same.
	const USurfSettings* GridSettings = GetDefault<USurfSettings>();
	const double Step = GridSettings ? FMath::Max(1.0, GridSettings->GetGridStepCm(GridSettings->DefaultGridIndex)) : 100.0;
	FVector Pivot = Bounds.GetCenter();
	Pivot.Z = Bounds.Min.Z;
	Pivot.X = FMath::RoundToDouble(Pivot.X / Step) * Step;
	Pivot.Y = FMath::RoundToDouble(Pivot.Y / Step) * Step;
	Pivot.Z = FMath::RoundToDouble(Pivot.Z / Step) * Step;
	const FTransform Standing(FQuat::Identity, Pivot);
	const FTransform ToPrefab = Standing.Inverse();

	// Inside a room, the new prefab is placed in the one being edited, not
	// spawned in the level: the pieces of a house are the house's.
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const bool bIntoRoom = IsInsidePrefab() && RoomAsset != nullptr;

	// The asset: a real package under the project, dirty until saved, so
	// nothing lands on disk that was not asked for (rule 3) and the name is
	// there to change in the Content Browser before it is.
	FString PackagePath;
	const FString Name = FreePrefabName(PackagePath, WantedName);
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return false;
	}
	Package->FullyLoad();

	// Held rather than scoped, because the asset is written to disk when it
	// is made (D89) and a save inside an open transaction records a file
	// that an undo cannot take back: the transaction closes first.
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("MakePrefab", "Make Prefab"));

	USurfPrefab* Asset = NewObject<USurfPrefab>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	if (!Asset)
	{
		Transaction->Cancel();
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();

	// The patches move: copied into the asset in its space, removed from
	// their groups. Their ids are new, since the copies are new patches.
	TArray<ASurfGroup*> Groups;
	int32 Moved = 0;
	for (const FSurfPatchRef& Ref : Targets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}
		const FTransform ToWorld = Group->GetActorTransform();

		FSurfPatch Copy = *Patch;
		Copy.Id.Invalidate();
		Copy.EnsureId();
		for (FSurfControlPoint& Point : Copy.Points)
		{
			Point.Pos = ToPrefab.TransformPosition(ToWorld.TransformPosition(Point.Pos));
		}
		Asset->AddPatch(MoveTemp(Copy));

		if (!Groups.Contains(Group))
		{
			Group->Modify();
			Groups.Add(Group);
		}
		Group->RemovePatch(Ref.PatchId);
		++Moved;
	}

	// The primitives, their planes carried into the prefab's space.
	for (const FSurfFaceRef& Ref : BrushTargets)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Brush)
		{
			continue;
		}
		const FMatrix Matrix = (Group->GetActorTransform() * ToPrefab).ToMatrixWithScale();
		FSurfBrush Copy = *Brush;
		Copy.Id = FGuid::NewGuid();
		for (FSurfFace& Face : Copy.Faces)
		{
			Face.Plane = Face.Plane.TransformBy(Matrix);
		}
		Asset->AddBrush(MoveTemp(Copy));
		if (!Groups.Contains(Group))
		{
			Group->Modify();
			Groups.Add(Group);
		}
		Group->RemoveBrush(Ref.BrushId);
		++Moved;
	}

	// Anything else picked, carried as a part where it stood.
	for (AActor* Actor : PartActors)
	{
		FSurfPart Part;
		FillPart(Part, Actor);
		Part.Transform = Actor->GetActorTransform().GetRelativeTransform(Standing);
		Asset->AddPart(MoveTemp(Part));
		Actor->Modify();
		Actor->Destroy();
		++Moved;
	}

	if (Moved == 0)
	{
		Transaction->Cancel();
		return false;
	}

	ClearSelection();
	ClearPointSelection();
	for (ASurfGroup* Group : Groups)
	{
		Group->RebuildPreview();
	}
	// Anything emptied by the move goes, as after a delete.
	SweepEmptyGroups();

	if (bIntoRoom)
	{
		// Placed in the room's asset, at where it stands measured from the
		// room's frame; the stand-in draws it from here on.
		const FTransform Relative = Standing.GetRelativeTransform(RoomFrame);
		if (!RoomAsset->Place(Asset, Relative))
		{
			Transaction->Cancel();
			return false;
		}
		RoomAsset->MarkPackageDirty();
		Transaction.Reset();
		SavePrefabPackage(Asset);
		if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
		{
			StandIn->RebuildPreview();
		}
		if (RoomAsset->Placements.Num() > 0)
		{
			PickPlacement(RoomAsset->Placements.Last().Id);
		}
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(
				TEXT("prefab %s made from %d piece%s, saved, and placed inside %s - Alt+PgDn opens it, Delete takes it out"),
				*Name, Moved, Moved == 1 ? TEXT("") : TEXT("s"), *RoomAsset->GetName()));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("make_prefab"), FString::Printf(TEXT("patches=%d name=%s inside=%s"), Moved, *Name, *RoomAsset->GetName()));
		if (GEditor)
		{
			RedrawViewports();
		}
		return true;
	}

	// One instance where the geometry was, selected as the editor selects
	// an actor: from here it is moved, copied and deleted as any actor is.
	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World->GetCurrentLevel(), ASurfPrefabInstance::StaticClass(), *Name);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASurfPrefabInstance* Instance = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), Standing, Params);
	if (Instance)
	{
		Instance->Prefab = Asset;
		Instance->SetActorLabel(Name);
		Instance->RebuildPreview();
		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			GEditor->SelectActor(Instance, /*bInSelected*/ true, /*bNotify*/ true);
		}
	}

	// Saved where it lives (S71c), and shown there (S69d): "I made a prefab
	// and had no idea after that how to see it, open it, name it". The
	// Content Browser goes to the asset, the instance stands selected, and
	// the line says what each next thing is.
	Transaction.Reset();
	SavePrefabPackage(Asset);
	if (GEditor)
	{
		TArray<UObject*> Made;
		Made.Add(Asset);
		GEditor->SyncBrowserToObjects(Made);
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(
			TEXT("prefab %s made from %d piece%s and saved under %s - the instance stands here, selected; Alt+PgDn or double-click edits it, the Assets tab places it again"),
			*Name, Moved, Moved == 1 ? TEXT("") : TEXT("s"), *FPackageName::GetLongPackagePath(PackagePath)));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("make_prefab"), FString::Printf(TEXT("patches=%d name=%s"), Moved, *Name));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

// --- Drill-in (S65b) --------------------------------------------------------
//
// The asset is edited where it stands: entering an instance copies its
// geometry into a transient group at the instance's transform, so every
// verb the mode has works on it unchanged, and the instance draws only its
// placements meanwhile. Exit writes the group back into the asset and
// rebuilds every instance of it in the level - "update on save, unless made
// unique" (the author, 2026-09-11) - or drops the edit. Nothing here is a
// second editing path; it is the one path, pointed at an asset.

namespace
{
	ASurfPrefabInstance* SelectedInstance()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		USelection* Selected = GEditor->GetSelectedActors();
		return Selected ? Selected->GetTop<ASurfPrefabInstance>() : nullptr;
	}

	void RebuildInstancesOf(UWorld* World, const USurfPrefab* Asset)
	{
		if (!World || !Asset)
		{
			return;
		}
		for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
		{
			if (It->Prefab == Asset || (It->Prefab && It->Prefab->Contains(Asset)))
			{
				// The asset changed under it: a bake of it is stale.
				It->Unbake();
				It->RebuildPreview();
			}
		}
	}
}

bool USurfEdMode::EnterPrefab()
{
	ASurfPrefabInstance* Instance = SelectedInstance();
	if (!Instance)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("open: click a placed prefab first"));
		}
		return false;
	}
	return EnterPrefabInstance(Instance);
}

bool USurfEdMode::NewPrefab(const FString& Name)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// A prefab is made the way a level is: an empty one, in front of you,
	// open. What goes in it goes in while it is open (S77).
	FString PackagePath;
	const FString AssetName = FreePrefabName(PackagePath, Name);
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("new prefab: the asset could not be made"));
		}
		return false;
	}
	Package->FullyLoad();

	USurfPrefab* Asset = NewObject<USurfPrefab>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Asset)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("new prefab: the asset could not be made"));
		}
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	SavePrefabPackage(Asset);

	ASurfPrefabInstance* Instance = PlacePrefabAsset(Asset, ViewFloorPoint());
	if (!Instance)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("new prefab: it could not be placed"));
		}
		return false;
	}

	// Made only to be edited: if nothing is put in it and it is closed, the
	// instance goes with it, as an unplaced prefab's stand-in does (S71c).
	const bool bOpened = EnterPrefabInstance(Instance);
	if (bOpened)
	{
		RoomEditOnlyInstance = Instance;
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s is open and empty - make geometry, drop in meshes and blueprints; Close keeps and saves it"), *AssetName));
			Inspector->RefreshSelectionCount();
		}
	}
	SurfSession::Verb(TEXT("new_prefab"), AssetName);
	return bOpened;
}

bool USurfEdMode::SaveOpenPrefab()
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* Asset = Drilled ? Drilled->Prefab.Get() : nullptr;
	if (!IsInsidePrefab() || !Asset)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("save: open a prefab first"));
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("SaveOpenPrefab", "Save Prefab"));
	const int32 Written = WriteRoomToAsset(Asset);
	const int32 Parts = CaptureRoomParts(*Asset);
	const bool bSaved = SavePrefabPackage(Asset);
	RebuildInstancesOf(GetWorld(), Asset);

	if (Inspector)
	{
		Inspector->SetResult(bSaved
			? FString::Printf(TEXT("%s saved - %d piece%s%s; it stays open"),
				*Asset->GetName(), Written, Written == 1 ? TEXT("") : TEXT("s"),
				Parts > 0 ? *FString::Printf(TEXT(" and %d thing%s it carries"), Parts, Parts == 1 ? TEXT("") : TEXT("s")) : TEXT(""))
			: FString::Printf(TEXT("%s could not be saved"), *Asset->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("save_prefab"), FString::Printf(TEXT("%s pieces=%d parts=%d"), *Asset->GetName(), Written, Parts));
	return bSaved;
}

bool USurfEdMode::EnterPrefabInstance(ASurfPrefabInstance* Instance)
{
	UWorld* World = GetWorld();
	if (!Instance || !Instance->Prefab || !World)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("open: this placed prefab has no asset to open"));
		}
		return false;
	}
	if (IsInsidePrefab())
	{
		// One at a time, all the way up: closing with the climb on would
		// have re-opened the room above in the middle of opening this one,
		// and left its group and stand-in behind forever.
		UnwindRooms(/*bKeep*/ true);
	}

	USurfPrefab* Asset = Instance->Prefab;

	// Where the room stands (S71a): the instance's own transform when the
	// prefab is edited in place, the origin when it is edited alone - the
	// asset's geometry is in its own space either way, so the group's
	// transform is the whole difference.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bInPlace = Settings && Settings->bEditPrefabsInPlace;
	const FTransform Frame = bInPlace ? Instance->GetActorTransform() : FTransform::Identity;

	FScopedTransaction Transaction(LOCTEXT("EnterPrefab", "Enter Prefab"));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASurfGroup* Group = World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), Frame, Params);
	if (!Group)
	{
		Transaction.Cancel();
		return false;
	}
	Group->SetActorLabel(FString::Printf(TEXT("Editing %s"), *Asset->GetName()));

	// The asset's own geometry, ids and all, into the group; the instance
	// keeps drawing its placements so the whole prefab is still in view.
	int32 Copied = 0;
	Asset->ForEachPatch([&](const FSurfPatch& Patch)
	{
		FSurfPatch Copy = Patch;
		Group->AddPatch(MoveTemp(Copy));
		++Copied;
	});
	Asset->ForEachBrush([&](const FSurfBrush& Brush)
	{
		FSurfBrush Copy = Brush;
		Group->AddBrush(MoveTemp(Copy));
		++Copied;
	});
	Group->RebuildPreview();

	Instance->Modify();
	Instance->bDrawOwnGeometry = false;
	Instance->RebuildPreview();

	DrilledInstance = Instance;
	DrilledGroup = Group;
	RoomFrame = Frame;
	bRoomInPlace = bInPlace;

	ClearSelection();
	ClearPointSelection();
	if (GEditor)
	{
		GEditor->SelectNone(false, true);
	}
	SetSubMode(ESurfSubMode::Object);

	// The rest of the level out of reach: ghosted round the room in place,
	// out of view when the prefab is edited alone - and the camera brought
	// to it, to come back on Exit.
	SetRoomAside(Group, Instance, bInPlace);
	FrameRoom(Group);

	// The placements (S71b) are drawn by a stand-in at the room's frame, so
	// they stand with the geometry wherever the room is; the instance
	// itself is out of view meanwhile, whichever way the room stands.
	if (!Instance->IsTemporarilyHiddenInEditor())
	{
		Instance->SetIsTemporarilyHiddenInEditor(true);
		RoomHiddenActors.Add(Instance);
	}
	RoomPlacementsBefore = Asset->Placements;
	RoomPartsBefore = Asset->Parts;

	// The parts stand in the level while the room is open, and anything
	// dropped in from here on joins them (S77a).
	SpawnRoomParts(*Asset);
	WatchRoomActors(true);
	RoomPickedPlacement.Invalidate();
	{
		FActorSpawnParameters StandInParams;
		StandInParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		StandInParams.ObjectFlags |= RF_Transient;
		ASurfPrefabInstance* StandIn = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), Frame, StandInParams);
		if (StandIn)
		{
			StandIn->SetActorLabel(FString::Printf(TEXT("Inside %s"), *Asset->GetName()));
			StandIn->Prefab = Asset;
			StandIn->bDrawOwnGeometry = false;
			StandIn->RebuildPreview();
			RoomStandIn = StandIn;
		}
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("editing %s%s - %d piece%s; Alt+PgUp or Close keeps it and every instance follows; Close, dropping the edits, puts it back as it was"),
			*Asset->GetName(), bInPlace ? TEXT(" where it stands") : TEXT(" alone, at the origin"), Copied, Copied == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("enter_prefab"), FString::Printf(TEXT("asset=%s pieces=%d in_place=%d"), *Asset->GetName(), Copied, bInPlace ? 1 : 0));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::ExitPrefab(bool bKeep, bool bClimb)
{
	ASurfPrefabInstance* Instance = DrilledInstance.Get();
	ASurfGroup* Group = DrilledGroup.Get();
	if (!Instance && !Group)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("close: not inside a prefab"));
		}
		return false;
	}

	// A placement being dragged belongs to this room: it goes back where it
	// was and its transaction closes, rather than outliving the room.
	if (bModalOnPlacement)
	{
		CancelModal();
	}

	USurfPrefab* Asset = Instance ? Instance->Prefab.Get() : nullptr;
	UWorld* World = GetWorld();
	const bool bNested = RoomTrail.Num() > 0;

	FScopedTransaction Transaction(bKeep ? LOCTEXT("ExitPrefabKeep", "Exit Prefab") : LOCTEXT("ExitPrefabDrop", "Exit Prefab, Dropping Edits"));

	// Dropping puts the placements back as well as the geometry (S71b).
	if (!bKeep && Asset)
	{
		Asset->Modify();
		Asset->Placements = RoomPlacementsBefore;
		Asset->Parts = RoomPartsBefore;
	}
	RoomPlacementsBefore.Reset();
	RoomPartsBefore.Reset();
	RoomPickedPlacement.Invalidate();
	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->Destroy();
	}
	RoomStandIn.Reset();

	// The parts as they stand: moved, deleted, or dropped in while the room
	// was open. Read before the actors go (S77a).
	WatchRoomActors(false);
	int32 PartsKept = 0;
	if (bKeep && Asset)
	{
		PartsKept = CaptureRoomParts(*Asset);
	}
	ClearRoomParts();

	int32 Written = 0;
	bool bSaved = false;
	if (bKeep && Asset && Group)
	{
		Written = WriteRoomToAsset(Asset);
		// Kept is saved (S71c): "update should happen upon save" was the
		// rule, and leaving means keeping. Ctrl+S is no longer a step.
		bSaved = SavePrefabPackage(Asset);
	}

	ClearSelection();
	ClearPointSelection();
	DrilledInstance.Reset();
	DrilledGroup.Reset();

	// Everything set aside for the room comes back, and the camera with it.
	ClearRoomAside();
	LeaveRoomView();
	RoomFrame = FTransform::Identity;
	bRoomInPlace = false;

	if (Group)
	{
		Group->Destroy();
	}
	if (Instance)
	{
		Instance->Modify();
		Instance->bDrawOwnGeometry = true;
		Instance->RebuildPreview();
		if (GEditor && !bNested)
		{
			GEditor->SelectNone(false, true);
			GEditor->SelectActor(Instance, true, true);
		}
	}
	if (bKeep)
	{
		RebuildInstancesOf(World, Asset);
	}

	// A stand-in made only to edit an unplaced prefab (S71c) has done its job.
	if (Instance && Instance == RoomEditOnlyInstance.Get() && !bNested)
	{
		if (GEditor)
		{
			GEditor->SelectNone(false, true);
		}
		Instance->Destroy();
		RoomEditOnlyInstance.Reset();
	}

	if (Inspector)
	{
		Inspector->SetResult(bKeep
			? FString::Printf(TEXT("%s kept%s - %d piece%s%s; every instance follows"),
				Asset ? *Asset->GetName() : TEXT("prefab"), bSaved ? TEXT(" and saved") : TEXT(""),
				Written, Written == 1 ? TEXT("") : TEXT("s"),
				PartsKept > 0 ? *FString::Printf(TEXT(" and %d thing%s it carries"), PartsKept, PartsKept == 1 ? TEXT("") : TEXT("s")) : TEXT(""))
			: TEXT("edits dropped - the prefab is as it was"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("exit_prefab"), FString::Printf(TEXT("keep=%d pieces=%d depth=%d"), bKeep ? 1 : 0, Written, RoomTrail.Num() + 1));

	// A nested room (S71b) was entered through a placement of the room
	// above: its instance was a stand-in for the placement, and goes; the
	// room above is opened again with the placement picked.
	if (bNested && bClimb)
	{
		if (Instance)
		{
			Instance->Destroy();
		}
		const FSurfRoomStep Step = RoomTrail.Pop();
		if (ASurfPrefabInstance* Above = Step.Instance.Get())
		{
			EnterPrefabInstance(Above);
			if (Step.bEditOnly)
			{
				// Ours again, and ours to destroy when this room closes.
				RoomEditOnlyInstance = Above;
			}
			if (Step.Placement.IsValid())
			{
				PickPlacement(Step.Placement);
			}
		}
	}
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

int32 USurfEdMode::WriteRoomToAsset(USurfPrefab* Target)
{
	ASurfGroup* Group = DrilledGroup.Get();
	if (!Target || !Group)
	{
		return 0;
	}
	// The group's geometry becomes the asset's, whole: new chunks, the
	// old ones let go, so the asset is exactly what was on screen.
	int32 Written = 0;
	Target->Modify();
	Target->Chunks.Reset();
	Group->ForEachPatchIncludingHidden([&](const FSurfPatch& Patch)
	{
		FSurfPatch Copy = Patch;
		Target->AddPatch(MoveTemp(Copy));
		++Written;
	});
	Group->ForEachBrush([&](const FSurfBrush& Brush)
	{
		FSurfBrush Copy = Brush;
		Target->AddBrush(MoveTemp(Copy));
		++Written;
	});
	Target->MarkPackageDirty();
	return Written;
}

USurfPrefab* USurfEdMode::ContextPrefab() const
{
	if (const ASurfPrefabInstance* Drilled = DrilledInstance.Get())
	{
		return Drilled->Prefab.Get();
	}
	const ASurfPrefabInstance* Selected = SelectedInstance();
	return Selected ? Selected->Prefab.Get() : nullptr;
}

bool USurfEdMode::SavePrefabAsset(USurfPrefab* Asset)
{
	if (!Asset)
	{
		return false;
	}
	// The room's own content goes into it first, so what is saved is what
	// is on screen; the room stays open.
	if (const ASurfPrefabInstance* Drilled = DrilledInstance.Get(); Drilled && Drilled->Prefab == Asset)
	{
		WriteRoomToAsset(Asset);
		RebuildInstancesOf(GetWorld(), Asset);
	}
	const bool bSaved = SavePrefabPackage(Asset);
	if (Inspector)
	{
		Inspector->SetResult(bSaved
			? FString::Printf(TEXT("%s saved under %s"), *Asset->GetName(), *FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName()))
			: FString::Printf(TEXT("save: %s could not be written - is the file read-only, or checked out?"), *Asset->GetName()));
	}
	return bSaved;
}

FString USurfEdMode::SuggestPrefabNameLike(const FString& Base) const
{
	FString Path;
	FString Clean = Base;
	// A copy of PF_Door_2 is PF_Door_3, not PF_Door_2_2.
	int32 Under = INDEX_NONE;
	if (Clean.FindLastChar(TEXT('_'), Under) && Under > 0 && Under + 1 < Clean.Len() && Clean.Mid(Under + 1).IsNumeric())
	{
		Clean = Clean.Left(Under);
	}
	return FreePrefabName(Path, Clean);
}

bool USurfEdMode::SavePrefabAsFrom(USurfPrefab* Asset, const FString& WantedName)
{
	UWorld* World = GetWorld();
	if (!Asset || !World)
	{
		return false;
	}
	FString PackagePath;
	const FString Name = FreePrefabName(PackagePath, WantedName);
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return false;
	}
	Package->FullyLoad();

	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("SavePrefabAs", "Save Prefab As"));

	USurfPrefab* Copy = DuplicateObject<USurfPrefab>(Asset, Package, *Name);
	if (!Copy)
	{
		Transaction->Cancel();
		return false;
	}
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Package->MarkPackageDirty();

	// The instance being edited, or the one selected, points at the copy
	// from now on: a variant, without touching what else is placed. With
	// the room open, the copy takes what is on screen and the original is
	// put back as it was on entering, placements included.
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	ASurfPrefabInstance* Selected = SelectedInstance();
	FString Who;
	if (Drilled && Drilled->Prefab == Asset)
	{
		WriteRoomToAsset(Copy);
		Asset->Modify();
		Asset->Placements = RoomPlacementsBefore;
		RoomPlacementsBefore = Copy->Placements;
		Drilled->Modify();
		Drilled->Prefab = Copy;
		Drilled->SetActorLabel(Name);
		if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
		{
			StandIn->Prefab = Copy;
			StandIn->RebuildPreview();
		}
		// Nested: the room above places this prefab, and that placement is
		// what must follow the copy - otherwise the variant is saved,
		// referenced by nothing, and the parent still holds the original.
		if (RoomTrail.Num() > 0)
		{
			const FSurfRoomStep& Above = RoomTrail.Last();
			ASurfPrefabInstance* AboveInstance = Above.Instance.Get();
			USurfPrefab* AboveAsset = AboveInstance ? AboveInstance->Prefab.Get() : nullptr;
			if (AboveAsset && Above.Placement.IsValid())
			{
				if (FSurfPlacement* Placement = AboveAsset->ModifyPlacement(Above.Placement))
				{
					Placement->Prefab = Copy;
					AboveAsset->MarkPackageDirty();
				}
			}
		}
		RebuildInstancesOf(World, Asset);
		Who = RoomTrail.Num() > 0 ? TEXT("the prefab being edited, and the one that places it") : TEXT("the prefab being edited");
	}
	else if (Selected && Selected->Prefab == Asset && Selected != RoomStandIn.Get())
	{
		Selected->Modify();
		Selected->Prefab = Copy;
		Selected->SetActorLabel(Name);
		Selected->RebuildPreview();
		Who = TEXT("the selected instance");
	}
	Transaction.Reset();
	const bool bSaved = SavePrefabPackage(Copy);

	if (Inspector)
	{
		Inspector->SetResult(Who.IsEmpty()
			? FString::Printf(TEXT("%s saved as a copy of %s%s - Place puts it in the level"), *Name, *Asset->GetName(), bSaved ? TEXT("") : TEXT(" (not written to disk)"))
			: FString::Printf(TEXT("%s saved as %s%s - %s now uses it, every other instance keeps %s"), *Asset->GetName(), *Name, bSaved ? TEXT("") : TEXT(" (not written to disk)"), *Who, *Asset->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("save_prefab_as"), FString::Printf(TEXT("from=%s to=%s repointed=%d"), *Asset->GetName(), *Name, Who.IsEmpty() ? 0 : 1));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::SavePrefabAs(const FString& WantedName)
{
	USurfPrefab* Asset = ContextPrefab();
	if (!Asset)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("save as: open a prefab, or click a placed one first"));
		}
		SurfSession::Friction(TEXT("save_as_no_prefab"));
		return false;
	}
	return SavePrefabAsFrom(Asset, WantedName);
}

FVector USurfEdMode::ViewFloorPoint() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Step = Settings ? FMath::Max(1.0, Settings->GetGridStepCm(Settings->DefaultGridIndex)) : 100.0;
	FVector At = FVector::ZeroVector;
	if (const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
	{
		const FVector Eye = Client->GetViewLocation();
		const FVector Dir = Client->GetViewRotation().Vector();
		// Where the view meets the floor, when it looks down at it; else a
		// few metres ahead.
		if (Dir.Z < -0.05 && Eye.Z > 0.0)
		{
			At = Eye + Dir * (Eye.Z / -Dir.Z);
		}
		else
		{
			At = Eye + Dir * 600.0;
			At.Z = 0.0;
		}
	}
	At.X = FMath::RoundToDouble(At.X / Step) * Step;
	At.Y = FMath::RoundToDouble(At.Y / Step) * Step;
	At.Z = FMath::RoundToDouble(At.Z / Step) * Step;
	return At;
}

const FName USurfEdMode::PlacedMeshTag(TEXT("SurfacingProMesh"));

AStaticMeshActor* USurfEdMode::PlaceStaticMeshAsset(UStaticMesh* Mesh, const FVector& At)
{
	UWorld* World = GetWorld();
	if (!Mesh || !World)
	{
		return nullptr;
	}
	FScopedTransaction Transaction(LOCTEXT("PlaceMesh", "Place Static Mesh"));
	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World->GetCurrentLevel(), AStaticMeshActor::StaticClass(), *Mesh->GetName());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FQuat::Identity, At), Params);
	if (!Actor)
	{
		Transaction.Cancel();
		return nullptr;
	}
	if (UStaticMeshComponent* Component = Actor->GetStaticMeshComponent())
	{
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
	}
	Actor->SetActorLabel(Mesh->GetName());
	Actor->Tags.AddUnique(PlacedMeshTag);
	Actor->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
	// Inside a prefab the room takes it as a part (OnActorAddedToRoom);
	// in the level it stands in the Assets drawer.
	if (!IsInsidePrefab())
	{
		TidyOutliner();
	}
	ClearSelection();
	ClearPointSelection();
	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Actor, true, true);
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("placed %s"), *Mesh->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("place_mesh"), Mesh->GetName());
	RedrawViewports();
	return Actor;
}

int32 USurfEdMode::ReplaceActorsWith(const TArray<AActor*>& Actors, const FAssetData& With)
{
	UWorld* World = GetWorld();
	const bool bLevel = With.AssetClassPath == UWorld::StaticClass()->GetClassPathName();
	USurfPrefab* Prefab = bLevel ? nullptr : Cast<USurfPrefab>(With.GetAsset());
	UStaticMesh* Mesh = bLevel ? nullptr : Cast<UStaticMesh>(With.GetAsset());
	if (!World || (!Prefab && !Mesh && !bLevel) || Actors.Num() == 0)
	{
		return 0;
	}
	FScopedTransaction Transaction(LOCTEXT("ReplaceWith", "Replace With Asset"));
	TArray<AActor*> Made;
	int32 Done = 0;
	for (AActor* Old : Actors)
	{
		if (!IsValid(Old))
		{
			continue;
		}
		const FTransform Where = Old->GetActorTransform();
		AActor* Parent = Old->GetAttachParentActor();
		AActor* New = nullptr;
		if (ASurfPrefabInstance* Instance = Cast<ASurfPrefabInstance>(Old))
		{
			if (Prefab)
			{
				// The same placement, another prefab: nothing moves.
				Instance->Modify();
				Instance->Prefab = Prefab;
				Instance->SetActorLabel(Prefab->GetName());
				Instance->RebuildPreview();
				++Done;
				continue;
			}
		}
		else if (AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Old))
		{
			if (Mesh && MeshActor->GetStaticMeshComponent())
			{
				MeshActor->GetStaticMeshComponent()->Modify();
				MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				MeshActor->GetStaticMeshComponent()->EmptyOverrideMaterials();
				MeshActor->SetActorLabel(Mesh->GetName());
				MeshActor->Tags.AddUnique(PlacedMeshTag);
				++Done;
				continue;
			}
		}
		else if (ALevelInstance* LevelActor = Cast<ALevelInstance>(Old))
		{
			if (bLevel)
			{
				// The same placement, another level (S217a).
				LevelActor->Modify();
				if (LevelActor->SetWorldAsset(TSoftObjectPtr<UWorld>(With.GetSoftObjectPath())))
				{
					LevelActor->UpdateLevelInstanceFromWorldAsset();
					LevelActor->SetActorLabel(With.AssetName.ToString());
					LevelActor->Tags.AddUnique(PlacedMeshTag);
					++Done;
				}
				continue;
			}
		}
		else
		{
			continue;
		}
		// A kind for the other: a new actor where the old one stood.
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.OverrideLevel = Old->GetLevel();
		if (Prefab)
		{
			ASurfPrefabInstance* Instance = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), Where, Params);
			if (Instance)
			{
				Instance->Prefab = Prefab;
				Instance->SetActorLabel(Prefab->GetName());
				Instance->RebuildPreview();
				New = Instance;
			}
		}
		else if (Mesh)
		{
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Where, Params);
			if (Actor && Actor->GetStaticMeshComponent())
			{
				Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor->SetActorLabel(Mesh->GetName());
				Actor->Tags.AddUnique(PlacedMeshTag);
				Actor->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
				New = Actor;
			}
		}
		else if (bLevel)
		{
			ALevelInstance* Actor = World->SpawnActor<ALevelInstance>(ALevelInstance::StaticClass(), Where, Params);
			if (Actor && Actor->SetWorldAsset(TSoftObjectPtr<UWorld>(With.GetSoftObjectPath())))
			{
				Actor->LoadLevelInstance();
				Actor->SetActorLabel(With.AssetName.ToString());
				Actor->Tags.AddUnique(PlacedMeshTag);
				Actor->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
				New = Actor;
			}
			else if (Actor)
			{
				World->EditorDestroyActor(Actor, false);
			}
		}
		if (!New)
		{
			continue;
		}
		if (Parent)
		{
			New->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		}
		Made.Add(New);
		World->EditorDestroyActor(Old, true);
		++Done;
	}
	if (GEditor && Made.Num() > 0)
	{
		GEditor->SelectNone(false, true);
		for (AActor* Actor : Made)
		{
			GEditor->SelectActor(Actor, true, true);
		}
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("replaced %d with %s"), Done, *With.AssetName.ToString()));
		Inspector->RefreshSelectionCount();
	}
	RedrawViewports();
	return Done;
}

ASurfPrefabInstance* USurfEdMode::PlacePrefabAsset(USurfPrefab* Asset, const FVector& At)
{
	UWorld* World = GetWorld();
	if (!Asset || !World)
	{
		return nullptr;
	}
	// Inside a room, placed inside the prefab being edited (S71b's rule);
	// in the level otherwise.
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	if (RoomAsset)
	{
		if (!RoomAsset->CanPlace(Asset))
		{
			if (Inspector)
			{
				Inspector->SetResult(FString::Printf(TEXT("place: %s cannot go inside %s - it would contain itself"), *Asset->GetName(), *RoomAsset->GetName()));
			}
			return nullptr;
		}
		FScopedTransaction Transaction(LOCTEXT("PlaceInside", "Place Prefab Inside"));
		RoomAsset->Place(Asset, FTransform(FQuat::Identity, At).GetRelativeTransform(RoomFrame));
		RoomAsset->MarkPackageDirty();
		if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
		{
			StandIn->RebuildPreview();
		}
		PickPlacement(RoomAsset->Placements.Last().Id);
		SurfSession::Verb(TEXT("place_prefab"), FString::Printf(TEXT("child=%s inside=%s from=window"), *Asset->GetName(), *RoomAsset->GetName()));
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("PlacePrefab", "Place Prefab"));
	FActorSpawnParameters Params;
	Params.Name = MakeUniqueObjectName(World->GetCurrentLevel(), ASurfPrefabInstance::StaticClass(), *Asset->GetName());
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASurfPrefabInstance* Instance = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), FTransform(FQuat::Identity, At), Params);
	if (!Instance)
	{
		Transaction.Cancel();
		return nullptr;
	}
	Instance->Prefab = Asset;
	Instance->SetActorLabel(Asset->GetName());
	Instance->RebuildPreview();
	ClearSelection();
	ClearPointSelection();
	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Instance, true, true);
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s placed at %.0f %.0f %.0f, selected - it moves as any actor; Alt+PgDn edits it"), *Asset->GetName(), At.X, At.Y, At.Z));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("place_prefab"), FString::Printf(TEXT("asset=%s at=%.0f,%.0f,%.0f"), *Asset->GetName(), At.X, At.Y, At.Z));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Instance;
}

bool USurfEdMode::EditPrefabAsset(USurfPrefab* Asset)
{
	UWorld* World = GetWorld();
	if (!Asset || !World)
	{
		return false;
	}
	if (IsInsidePrefab() && ContextPrefab() == Asset)
	{
		return true;
	}
	// Through its first instance in the level when it has one; else through
	// a stand-in at the origin, made for the room and gone with it.
	ASurfPrefabInstance* Through = nullptr;
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		if (It->Prefab == Asset && !It->HasAnyFlags(RF_Transient))
		{
			Through = *It;
			break;
		}
	}
	if (!Through)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		Through = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), FTransform::Identity, Params);
		if (!Through)
		{
			return false;
		}
		Through->SetActorLabel(FString::Printf(TEXT("%s (editing)"), *Asset->GetName()));
		Through->Prefab = Asset;
		Through->RebuildPreview();
	}
	if (IsInsidePrefab())
	{
		UnwindRooms(/*bKeep*/ true);
	}
	const bool bEntered = EnterPrefabInstance(Through);
	if (Through->HasAnyFlags(RF_Transient))
	{
		if (bEntered)
		{
			RoomEditOnlyInstance = Through;
		}
		else
		{
			Through->Destroy();
		}
	}
	SurfSession::Verb(TEXT("edit_prefab"), FString::Printf(TEXT("asset=%s placed=%d ok=%d"), *Asset->GetName(), Through->HasAnyFlags(RF_Transient) ? 0 : 1, bEntered ? 1 : 0));
	return bEntered;
}

int32 USurfEdMode::FindPrefabInstances(USurfPrefab* Asset)
{
	UWorld* World = GetWorld();
	if (!Asset || !World || !GEditor)
	{
		return 0;
	}
	ClearSelection();
	ClearPointSelection();
	GEditor->SelectNone(false, true);
	int32 Found = 0;
	FBox Bounds(ForceInit);
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		if (It->Prefab == Asset && !It->HasAnyFlags(RF_Transient))
		{
			GEditor->SelectActor(*It, true, false);
			Bounds += It->GetComponentsBoundingBox(true);
			++Found;
		}
	}
	GEditor->NoteSelectionChange();
	if (Found > 0 && Bounds.IsValid)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client == GCurrentLevelEditingViewportClient)
			{
				Client->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent().GetMax() * 0.25 + 25.0), /*bInstant*/ true);
			}
		}
	}
	if (Inspector)
	{
		Inspector->SetResult(Found == 0
			? FString::Printf(TEXT("%s is not placed in this level - double-click its tile to place it"), *Asset->GetName())
			: FString::Printf(TEXT("%d instance%s of %s selected and framed"), Found, Found == 1 ? TEXT("") : TEXT("s"), *Asset->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("find_prefab"), FString::Printf(TEXT("asset=%s found=%d"), *Asset->GetName(), Found));
	return Found;
}

bool USurfEdMode::RenamePrefabAsset(USurfPrefab* Asset, const FString& WantedName)
{
	if (!Asset)
	{
		return false;
	}
	FString Clean;
	for (const TCHAR C : WantedName.TrimStartAndEnd())
	{
		Clean.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') ? C : TEXT('_'));
	}

	// The name as typed (S228a: "dont auto name it PF_ something. user can
	// search them by prefab anyway. just give them a unique icon") - S107a's
	// forced prefix is withdrawn; the prefab's own icon tells it apart.
	Clean = Clean.TrimStartAndEnd();
	if (Clean.IsEmpty())
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("rename: a prefab needs a name"));
		}
		return false;
	}

	if (Clean == Asset->GetName())
	{
		return false;
	}
	const FString Folder = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	if (!PrefabNameFree(Folder / Clean))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("rename: %s is taken"), *Clean));
		}
		return false;
	}
	const FString Was = Asset->GetName();
	TArray<FAssetRenameData> Renames;
	Renames.Emplace(TWeakObjectPtr<UObject>(Asset), Folder, Clean);
	const bool bRenamed = FAssetToolsModule::GetModule().Get().RenameAssets(Renames);
	if (bRenamed && GetWorld())
	{
		for (TActorIterator<ASurfPrefabInstance> It(GetWorld()); It; ++It)
		{
			if (It->Prefab == Asset && It->GetActorLabel() == Was)
			{
				It->SetActorLabel(Asset->GetName());
			}
		}
	}
	if (Inspector)
	{
		Inspector->SetResult(bRenamed
			? FString::Printf(TEXT("%s is now %s - every instance and placement follows"), *Was, *Asset->GetName())
			: FString::Printf(TEXT("rename: %s could not be renamed"), *Was));
	}
	SurfSession::Verb(TEXT("rename_prefab"), FString::Printf(TEXT("from=%s to=%s ok=%d"), *Was, *Clean, bRenamed ? 1 : 0));
	return bRenamed;
}

void USurfEdMode::TidyOutliner()
{
	UWorld* World = GetWorld();
	if (!World || IsInsidePrefab() || IsInsideScene() || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		return;
	}
	// The level's own group per level, the oldest mark as FindOrMakeGroup has it.
	TMap<const ULevel*, ASurfGroup*> Home;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!IsValid(Group) || !Group->bLevelGroup || Group->HasAnyFlags(RF_Transient))
		{
			continue;
		}
		ASurfGroup*& Found = Home.FindOrAdd(Group->GetLevel());
		if (!Found || Group->GroupId < Found->GroupId)
		{
			Found = Group;
		}
	}
	for (const TPair<const ULevel*, ASurfGroup*>& Pair : Home)
	{
		ASurfGroup* Group = Pair.Value;
		// Its name: the level's, unless he gave it one (the default starts SurfGroup).
		const FString Wanted = FPackageName::GetShortName(Pair.Key->GetOutermost()->GetName());
		const FString Now = Group->GetActorLabel();
		// A name read from freed memory (S206a, the S202a spawn) is junk
		// characters: control codes, the replacement mark, CJK - renamed too.
		bool bJunk = false;
		for (const TCHAR C : Now)
		{
			if (static_cast<uint32>(C) < 32u || static_cast<uint32>(C) == 0xFFFDu || static_cast<uint32>(C) >= 0x3000u)
			{
				bJunk = true;
				break;
			}
		}
		if (!Wanted.IsEmpty() && Now != Wanted && (Now.StartsWith(TEXT("SurfGroup")) || bJunk))
		{
			Group->SetActorLabel(Wanted, /*bMarkDirty*/ false);
		}
	}
	// The prefabs, meshes and levels placed in a level stood under its group
	// from S202b ("nest them"); since S220a they stand in one Assets drawer
	// of the outliner instead - "in folder allow user to use the outliner
	// as an outliner and make selections from there" - so anything of
	// ours still hanging under a level group is let go, where it stands.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient) || Actor == RoomStandIn.Get() || IsRoomPartActor(Actor))
		{
			continue;
		}
		const bool bInstance = Actor->IsA(ASurfPrefabInstance::StaticClass());
		const bool bPlaced = Actor->Tags.Contains(PlacedMeshTag) && (Actor->IsA(AStaticMeshActor::StaticClass()) || Actor->IsA(ALevelInstance::StaticClass()));
		if (!bInstance && !bPlaced)
		{
			continue;
		}
		if (ASurfGroup* Parent = Cast<ASurfGroup>(Actor->GetAttachParentActor()))
		{
			if (Parent->bLevelGroup)
			{
				Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			}
		}
		if (!Actor->GetAttachParentActor() && Actor->GetFolderPath() != ASurfGroup::OutlinerPrefabFolder)
		{
			Actor->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
		}
	}
}

void USurfEdMode::SyncInstanceLooks()
{
	const double Now = FPlatformTime::Seconds();
	if (bInstanceLooksPending || Now - InstanceLooksAt < 0.5)
	{
		return;
	}
	InstanceLooksAt = Now;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	bool bAny = false;
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		if (IsValid(*It) && It->PreviewLookStale())
		{
			bAny = true;
			break;
		}
	}
	if (!bAny)
	{
		return;
	}
	// Never inside the draw: the next tick.
	bInstanceLooksPending = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) -> bool
	{
		bInstanceLooksPending = false;
		int32 Count = 0;
		if (UWorld* Live = GetWorld())
		{
			for (TActorIterator<ASurfPrefabInstance> It(Live); It; ++It)
			{
				if (IsValid(*It) && It->PreviewLookStale())
				{
					It->RefreshPreviewMaterial();
					++Count;
				}
			}
		}
		if (Count > 0)
		{
			SurfSession::Verb(TEXT("prefab_look"), FString::Printf(TEXT("refreshed=%d"), Count));
			RedrawViewports();
		}
		return false;
	}), 0.0f);
}

void USurfEdMode::OnLevelActorAdded(AActor* Actor)
{
	// A placement, or a mesh dropped in from the Assets tab / the content
	// browser while the mode is up: a static mesh arriving in the level is
	// tagged as ours and nested (S210a); a level instance the same (S217a).
	AActor* Dropped = Cast<AStaticMeshActor>(Actor);
	if (!Dropped) { Dropped = Cast<ALevelInstance>(Actor); }
	if (Dropped && !IsInsidePrefab() && !Dropped->HasAnyFlags(RF_Transient))
	{
		Dropped->Tags.AddUnique(PlacedMeshTag);
		Dropped->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
	}
	if (bTidyPending || (!Cast<ASurfPrefabInstance>(Actor) && !Dropped))
	{
		return;
	}
	bTidyPending = true;
	// Next tick: the placement is still being set up when it is announced.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) -> bool
	{
		bTidyPending = false;
		TidyOutliner();
		return false;
	}), 0.0f);
}

void USurfEdMode::DrawPrefabMarkers(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	UWorld* World = GetWorld();
	if (!PDI || !World)
	{
		return;
	}

	// Grey, because grey is what "there, but not yours to touch yet" looks
	// like; brighter when the editor has it selected, so a click on one
	// shows that it landed.
	const USurfSettings* Marks = GetDefault<USurfSettings>();
	const bool bMarks = !Marks || Marks->bShowPrefabMarks;
	const bool bBounds = !Marks || Marks->bShowPrefabBounds;

	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		const ASurfPrefabInstance* Instance = *It;
		if (!Instance || Instance == RoomStandIn.Get() || Instance->IsHidden() || Instance->IsTemporarilyHiddenInEditor())
		{
			continue;
		}

		const bool bPicked = Instance->IsSelected();

		if (Instance->DrawsNothing())
		{
			const FLinearColor Colour = bPicked ? FLinearColor(0.85f, 0.85f, 0.9f, 1.0f) : FLinearColor(0.42f, 0.42f, 0.47f, 1.0f);
			DrawWireBox(PDI, Instance->WorldBounds(), Colour, SDPG_World);
			continue;
		}

		if ((!bMarks && !bBounds) || Instance == DrilledInstance.Get())
		{
			continue;
		}

		// Where it is anchored, and how far it reaches - faint, because it is
		// placed rather than open: a prefab in the level is a thing you put
		// down, not a thing you are holding (S81a).
		// Its size on screen, not the prefab's scale (S191a): "the gizmo
		// showing with prefab mark is very good but it scales if i scale the
		// prefab too".
		const FTransform At(Instance->GetActorQuat(), Instance->GetActorLocation());
		double Arm = 40.0;
		if (View)
		{
			const double Depth = View->WorldToScreen(At.GetLocation()).W;
			const double Across = FMath::Max(1, View->UnscaledViewRect.Width());
			const double Proj = View->ViewMatrices.GetViewToClip().M[0][0];
			if (Depth > 0.0 && Proj > 0.0)
			{
				// About forty pixels.
				Arm = Depth * (2.0 / Across) / Proj * 40.0;
			}
		}
		const float Fade = bPicked ? 0.85f : 0.3f;
		if (bMarks)
		{
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(Arm, 0.0, 0.0)), FLinearColor(0.95f, 0.3f, 0.25f, Fade), SDPG_World, 1.0f);
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(0.0, Arm, 0.0)), FLinearColor(0.35f, 0.9f, 0.3f, Fade), SDPG_World, 1.0f);
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(0.0, 0.0, Arm)), FLinearColor(0.35f, 0.55f, 1.0f, Fade), SDPG_World, 1.0f);
		}

		const FBox Box = Instance->WorldBounds();
		if (bBounds && Box.IsValid)
		{
			DrawWireBox(PDI, Box, FLinearColor(0.55f, 0.58f, 0.65f, bPicked ? 0.55f : 0.16f), SDPG_World);
		}
	}

	// The meshes placed from the Assets tab wear the same mark at their
	// origin (S211a: "expecting to see the origin of a staticmesh like i do
	// for a prefab"); no bounds - the editor draws those when picked.
	if (bMarks)
	{
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			const AStaticMeshActor* Actor = *It;
			if (!Actor || !Actor->Tags.Contains(PlacedMeshTag) || Actor->IsHidden() || Actor->IsTemporarilyHiddenInEditor() || IsRoomPartActor(Actor))
			{
				continue;
			}
			const bool bPicked = Actor->IsSelected();
			const FTransform At(Actor->GetActorQuat(), Actor->GetActorLocation());
			double Arm = 40.0;
			if (View)
			{
				const double Depth = View->WorldToScreen(At.GetLocation()).W;
				const double Across = FMath::Max(1, View->UnscaledViewRect.Width());
				const double Proj = View->ViewMatrices.GetViewToClip().M[0][0];
				if (Depth > 0.0 && Proj > 0.0)
				{
					Arm = Depth * (2.0 / Across) / Proj * 40.0;
				}
			}
			const float Fade = bPicked ? 0.85f : 0.3f;
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(Arm, 0.0, 0.0)), FLinearColor(0.95f, 0.3f, 0.25f, Fade), SDPG_World, 1.0f);
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(0.0, Arm, 0.0)), FLinearColor(0.35f, 0.9f, 0.3f, Fade), SDPG_World, 1.0f);
			PDI->DrawLine(At.GetLocation(), At.TransformPosition(FVector(0.0, 0.0, Arm)), FLinearColor(0.35f, 0.55f, 1.0f, Fade), SDPG_World, 1.0f);
		}
	}
}

void USurfEdMode::UnwindRooms(bool bKeep)
{
	int32 Guard = 0;
	while (IsInsidePrefab() && Guard++ < 64)
	{
		ASurfPrefabInstance* Current = DrilledInstance.Get();
		ExitPrefab(bKeep, /*bClimb*/ false);
		if (Current && RoomTrail.Num() > 0 && Current->HasAnyFlags(RF_Transient))
		{
			Current->Destroy();
		}
	}
	// Whatever the trail still holds were stand-ins for placements: gone.
	for (const FSurfRoomStep& Step : RoomTrail)
	{
		if (ASurfPrefabInstance* Left = Step.Instance.Get())
		{
			if (Left->HasAnyFlags(RF_Transient))
			{
				Left->Destroy();
			}
		}
	}
	RoomTrail.Reset();
}

bool USurfEdMode::EnterPlacement(const FGuid& PlacementId)
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Placement = RoomAsset ? RoomAsset->FindPlacement(PlacementId) : nullptr;
	UWorld* World = GetWorld();
	if (!Placement || !Placement->Prefab || !World)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("open: click a placement first"));
		}
		return false;
	}
	USurfPrefab* Child = Placement->Prefab;
	const FString AboveName = RoomAsset->GetName();

	// Where the child stands in the world: its placement in the room's
	// instance, wherever that instance is. The room above is kept and
	// closed, remembered as the way back up; a stand-in instance for the
	// placement is what the child's room is entered through.
	const FTransform ChildWorld = Placement->Transform * Drilled->GetActorTransform();
	FSurfRoomStep Step;
	Step.Instance = Drilled;
	Step.Placement = PlacementId;

	// The room above may stand on an instance we made ourselves, to edit a
	// prefab nothing has placed. Closing would destroy it as finished - but
	// it is the way back up, so the trail takes it over until we climb out
	// of it again.
	Step.bEditOnly = Drilled == RoomEditOnlyInstance.Get();
	if (Step.bEditOnly)
	{
		RoomEditOnlyInstance.Reset();
	}
	ExitPrefab(/*bKeep*/ true, /*bClimb*/ false);
	RoomTrail.Add(Step);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASurfPrefabInstance* ChildInstance = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), ChildWorld, Params);
	if (!ChildInstance)
	{
		RoomTrail.Pop();
		return false;
	}
	ChildInstance->SetActorLabel(FString::Printf(TEXT("%s in %s"), *Child->GetName(), *AboveName));
	ChildInstance->Prefab = Child;
	ChildInstance->RebuildPreview();
	const bool bEntered = EnterPrefabInstance(ChildInstance);
	if (!bEntered)
	{
		ChildInstance->Destroy();
		RoomTrail.Pop();
		return false;
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("editing %s inside %s - Alt+PgUp keeps it and goes back up to %s"),
			*Child->GetName(), *AboveName, *AboveName));
	}
	SurfSession::Verb(TEXT("enter_placement"), FString::Printf(TEXT("child=%s above=%s depth=%d"), *Child->GetName(), *AboveName, RoomTrail.Num() + 1));
	return true;
}

void USurfEdMode::PickPlacement(const FGuid& PlacementId)
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Placement = RoomAsset ? RoomAsset->FindPlacement(PlacementId) : nullptr;
	if (!Placement)
	{
		RoomPickedPlacement.Invalidate();
		return;
	}
	ClearPointSelection();
	SelectPatch(FSurfPatchRef());
	ClearSelection();
	if (GEditor)
	{
		GEditor->SelectNone(false, true);
	}
	RoomPickedPlacement = PlacementId;
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s, placed inside %s - G R S move it, Shift+D copies it, Alt+PgDn or double-click opens it, Delete takes it out"),
			Placement->Prefab ? *Placement->Prefab->GetName() : TEXT("(empty)"), *RoomAsset->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Selection(FString::Printf(TEXT("placement %s in %s"), Placement->Prefab ? *Placement->Prefab->GetName() : TEXT("(empty)"), *RoomAsset->GetName()));
	if (GEditor)
	{
		RedrawViewports();
	}
}

FTransform USurfEdMode::GetPickedPlacementTransform() const
{
	const ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	const USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Placement = RoomAsset && RoomPickedPlacement.IsValid() ? RoomAsset->FindPlacement(RoomPickedPlacement) : nullptr;
	return Placement ? Placement->Transform : FTransform::Identity;
}

bool USurfEdMode::SetPickedPlacementTransform(const FTransform& Local, bool bTransact)
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	if (!RoomAsset || !RoomPickedPlacement.IsValid())
	{
		return false;
	}
	if (!RoomAsset->FindPlacement(RoomPickedPlacement))
	{
		return false;
	}
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransact)
	{
		Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("MovePlacement", "Move Placement"));
	}
	FSurfPlacement* Placement = RoomAsset->ModifyPlacement(RoomPickedPlacement);
	if (!Placement)
	{
		return false;
	}
	Placement->Transform = Local;
	RoomAsset->MarkPackageDirty();
	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->RebuildPreview();
	}
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::DuplicatePickedPlacement()
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Picked = RoomAsset && RoomPickedPlacement.IsValid() ? RoomAsset->FindPlacement(RoomPickedPlacement) : nullptr;
	if (!Picked)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("duplicate: click a placement inside this prefab first"));
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("DuplicatePlacement", "Duplicate Placement"));
	USurfPrefab* Child = Picked->Prefab;
	const FTransform Where = Picked->Transform;
	RoomAsset->Modify();
	FSurfPlacement& Copy = RoomAsset->Placements.AddDefaulted_GetRef();
	Copy.Prefab = Child;
	Copy.Transform = Where;
	Copy.Id = FGuid::NewGuid();
	RoomAsset->MarkPackageDirty();

	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->RebuildPreview();
	}
	PickPlacement(Copy.Id);
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s copied - it sits on the first; G moves it, Escape while moving takes the copy back"),
			Child ? *Child->GetName() : TEXT("the placement")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("duplicate_placement"), FString::Printf(TEXT("child=%s inside=%s"), Child ? *Child->GetName() : TEXT("none"), *RoomAsset->GetName()));
	return true;
}

bool USurfEdMode::RemovePickedPlacement()
{
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
	const FSurfPlacement* Placement = RoomAsset && RoomPickedPlacement.IsValid() ? RoomAsset->FindPlacement(RoomPickedPlacement) : nullptr;
	if (!Placement)
	{
		return false;
	}
	const FString ChildName = Placement->Prefab ? Placement->Prefab->GetName() : TEXT("(empty)");
	FScopedTransaction Transaction(LOCTEXT("RemovePlacement", "Remove Placement"));
	RoomAsset->RemovePlacement(RoomPickedPlacement);
	RoomAsset->MarkPackageDirty();
	RoomPickedPlacement.Invalidate();
	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s taken out of %s - the asset %s still exists in the Content Browser"),
			*ChildName, *RoomAsset->GetName(), *ChildName));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("remove_placement"), FString::Printf(TEXT("child=%s from=%s"), *ChildName, *RoomAsset->GetName()));
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

TArray<FString> USurfEdMode::RoomPathNames() const
{
	TArray<FString> Path;
	for (const FSurfRoomStep& Step : RoomTrail)
	{
		const ASurfPrefabInstance* Above = Step.Instance.Get();
		Path.Add(Above && Above->Prefab ? Above->Prefab->GetName() : FString(TEXT("?")));
	}
	if (const ASurfPrefabInstance* Drilled = DrilledInstance.Get())
	{
		Path.Add(Drilled->Prefab ? Drilled->Prefab->GetName() : FString(TEXT("prefab")));
	}
	return Path;
}

FString USurfEdMode::DescribeRoomPath() const
{
	FString Path;
	for (const FSurfRoomStep& Step : RoomTrail)
	{
		const ASurfPrefabInstance* Above = Step.Instance.Get();
		Path += (Above && Above->Prefab ? Above->Prefab->GetName() : FString(TEXT("?"))) + TEXT(" / ");
	}
	const ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	Path += Drilled && Drilled->Prefab ? Drilled->Prefab->GetName() : FString(TEXT("prefab"));
	return Path;
}

bool USurfEdMode::HasSelectedInstance() const
{
	if (!GEditor)
	{
		return false;
	}
	USelection* Selected = GEditor->GetSelectedActors();
	const ASurfPrefabInstance* Instance = Selected ? Selected->GetTop<ASurfPrefabInstance>() : nullptr;
	return Instance && Instance != DrilledInstance.Get() && Instance != RoomStandIn.Get();
}

bool USurfEdMode::PrefabIn()
{
	if (IsInsidePrefab() && RoomPickedPlacement.IsValid())
	{
		return EnterPlacement(RoomPickedPlacement);
	}

	// A scene goes in through the engine's own session (S75a).
	if (ALevelInstance* Scene = SelectedScene())
	{
		return EnterScene(Scene);
	}
	ASurfPrefabInstance* Instance = SelectedInstance();
	if (Instance && (Instance == DrilledInstance.Get() || Instance == RoomStandIn.Get()))
	{
		Instance = nullptr;
	}
	if (!Instance)
	{
		if (Inspector)
		{
			Inspector->SetResult(IsInsidePrefab()
				? TEXT("in: click a placement inside this prefab first, or a prefab instance")
				: TEXT("in: click a prefab instance first (Alt+PgDn edits it)"));
		}
		SurfSession::Friction(TEXT("prefab_in_no_instance"));
		return false;
	}
	return EnterPrefabInstance(Instance);
}

bool USurfEdMode::PrefabOut()
{
	if (IsInsideScene())
	{
		return ExitScene(/*bKeep*/ true);
	}

	if (!IsInsidePrefab())
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("close: not inside a prefab or a level"));
		}
		SurfSession::Friction(TEXT("prefab_out_not_inside"));
		return false;
	}
	return ExitPrefab(/*bKeep*/ true);
}

bool USurfEdMode::IsOpen(const ASurfGroup* Group) const
{
	if (!DrilledInstance.IsValid())
	{
		return true;
	}
	return Group && Group == DrilledGroup.Get();
}

void USurfEdMode::FrameRoom(ASurfGroup* Room)
{
	// The camera to the room, in the viewport being used, remembered so
	// Exit brings it back: the prefab alone at the origin is far from
	// wherever the hand was working.
	RoomViewClient = nullptr;
	if (!Room || bRoomInPlace || !GEditor)
	{
		return;
	}
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client)
	{
		return;
	}
	RoomViewClient = Client;
	RoomViewLocation = Client->GetViewLocation();
	RoomViewRotation = Client->GetViewRotation();

	FBox Bounds = Room->GetComponentsBoundingBox(true);
	if (!Bounds.IsValid || Bounds.GetExtent().GetMax() < 1.0)
	{
		Bounds = FBox(FVector(-100.0), FVector(100.0));
	}
	if (FSurfCamera::IsBlenderScheme())
	{
		if (!Camera)
		{
			Camera = MakeUnique<FSurfCamera>();
		}
		Camera->Frame(Client, Bounds);
	}
	else
	{
		Client->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent().GetMax() * 0.25 + 25.0), /*bInstant*/ true);
	}
}

void USurfEdMode::LeaveRoomView()
{
	if (RoomViewClient && GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client == RoomViewClient)
			{
				Client->SetViewLocation(RoomViewLocation);
				Client->SetViewRotation(RoomViewRotation);
				Client->Invalidate();
				break;
			}
		}
	}
	RoomViewClient = nullptr;
}

bool USurfEdMode::CanTakeIn() const
{
	if (!IsInsidePrefab() || !GEditor)
	{
		return false;
	}
	USelection* Selected = GEditor->GetSelectedActors();
	if (!Selected)
	{
		return false;
	}
	for (FSelectionIterator It(*Selected); It; ++It)
	{
		const AActor* Actor = Cast<AActor>(*It);
		if (Actor && !Actor->IsA(ASurfGroup::StaticClass()) && !Actor->IsA(ASurfPrefabInstance::StaticClass()))
		{
			return true;
		}
	}
	return false;
}

int32 USurfEdMode::TakeInSelection()
{
	// "id rather be able to include other elements (sm. bp. etc.) with
	// prefabs too": what is selected in the level becomes part of the prefab
	// being edited, at where it stands, and the actor goes - the prefab
	// carries it from then on, everywhere it is placed (S76a).
	ASurfPrefabInstance* Drilled = DrilledInstance.Get();
	USurfPrefab* Asset = Drilled ? Drilled->Prefab.Get() : nullptr;
	if (!IsInsidePrefab() || !Asset || !GEditor)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("take in: open a prefab first (Alt+PgDn) - then what you select goes into it"));
		}
		SurfSession::Friction(TEXT("take_in_no_room"));
		return 0;
	}

	TArray<AActor*> Taking;
	if (USelection* Selected = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*Selected); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (!Actor || !IsValid(Actor))
			{
				continue;
			}
			// Ours is not a part: geometry goes in as geometry, and a prefab
			// instance goes in as a placement (Prefab does that).
			if (Actor->IsA(ASurfGroup::StaticClass()) || Actor->IsA(ASurfPrefabInstance::StaticClass()))
			{
				continue;
			}
			Taking.Add(Actor);
		}
	}

	if (Taking.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("take in: select a mesh, a blueprint, a light - something that is not ours - first"));
		}
		SurfSession::Friction(TEXT("take_in_nothing"));
		return 0;
	}

	FScopedTransaction Transaction(LOCTEXT("TakeIn", "Take Into Prefab"));
	Asset->Modify();

	int32 Taken = 0;
	for (AActor* Actor : Taking)
	{
		FSurfPart Part;
		FillPart(Part, Actor);
		// Where it stands, in the prefab's own space.
		Part.Transform = Actor->GetActorTransform().GetRelativeTransform(RoomFrame);
		Asset->AddPart(MoveTemp(Part));
		Actor->Destroy();
		++Taken;
	}

	Asset->MarkPackageDirty();
	if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
	{
		StandIn->RebuildPreview();
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%d thing%s taken into %s - every instance of it carries them now"),
			Taken, Taken == 1 ? TEXT("") : TEXT("s"), *Asset->GetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("take_in"), FString::Printf(TEXT("parts=%d into=%s"), Taken, *Asset->GetName()));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Taken;
}

bool USurfEdMode::UnpackSelectedPrefab(TArray<AActor*>* OutSpawned)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Inside a room, a picked placement unpacks into the prefab being
	// edited: its geometry becomes the room's own, where it stood.
	if (IsInsidePrefab() && RoomPickedPlacement.IsValid())
	{
		ASurfPrefabInstance* Drilled = DrilledInstance.Get();
		USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
		ASurfGroup* Room = DrilledGroup.Get();
		const FSurfPlacement* Placement = RoomAsset ? RoomAsset->FindPlacement(RoomPickedPlacement) : nullptr;
		const USurfPrefab* Child = Placement ? Placement->Prefab.Get() : nullptr;
		if (!Room || !Child)
		{
			if (Inspector)
			{
				Inspector->SetResult(TEXT("unpack: click a placement inside this prefab, or a placed prefab in the level"));
			}
			return false;
		}
		const FTransform Where = Placement->Transform;
		const FString ChildName = Child->GetName();

		FScopedTransaction Transaction(LOCTEXT("UnpackPlacement", "Unpack Placement"));
		int32 Moved = 0;
		Child->ForEachPatch([&](const FSurfPatch& Patch)
		{
			FSurfPatch Copy = Patch;
			Copy.Id.Invalidate();
			Copy.EnsureId();
			for (FSurfControlPoint& Point : Copy.Points)
			{
				Point.Pos = Where.TransformPosition(Point.Pos);
			}
			Room->AddPatch(MoveTemp(Copy));
			++Moved;
		});
		const FMatrix Matrix = Where.ToMatrixWithScale();
		Child->ForEachBrush([&](const FSurfBrush& Brush)
		{
			FSurfBrush Copy = Brush;
			Copy.Id = FGuid::NewGuid();
			for (FSurfFace& Face : Copy.Faces)
			{
				Face.Plane = Face.Plane.TransformBy(Matrix);
			}
			Room->AddBrush(MoveTemp(Copy));
			++Moved;
		});
		RoomAsset->RemovePlacement(RoomPickedPlacement);
		RoomAsset->MarkPackageDirty();
		RoomPickedPlacement.Invalidate();
		if (ASurfPrefabInstance* StandIn = RoomStandIn.Get())
		{
			StandIn->RebuildPreview();
		}
		Room->RebuildPreview();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s unpacked into %s - %d piece%s, editable here; %s is still in the library"),
				*ChildName, *RoomAsset->GetName(), Moved, Moved == 1 ? TEXT("") : TEXT("s"), *ChildName));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("unpack_placement"), FString::Printf(TEXT("child=%s pieces=%d"), *ChildName, Moved));
		if (GEditor)
		{
			RedrawViewports();
		}
		return Moved > 0;
	}

	ASurfPrefabInstance* Instance = SelectedInstance();
	USurfPrefab* Asset = Instance ? Instance->Prefab.Get() : nullptr;
	if (!Instance || !Asset || Instance == RoomStandIn.Get() || Instance == DrilledInstance.Get())
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("unpack: click a placed prefab first"));
		}
		SurfSession::Friction(TEXT("unpack_nothing"));
		return false;
	}
	const FString AssetName = Asset->GetName();
	const FTransform Stood = Instance->GetActorTransform();

	FScopedTransaction Transaction(LOCTEXT("UnpackPrefab", "Unpack Prefab"));

	// Into a group of its own where it stood, so what comes out is plain
	// geometry under the level, not another container.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASurfGroup* Group = World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), Stood, Params);
	if (!Group)
	{
		Transaction.Cancel();
		return false;
	}
	Group->SetActorLabel(FString::Printf(TEXT("%s (unpacked)"), *AssetName));
	if (OutSpawned)
	{
		OutSpawned->Add(Group);
	}

	int32 Moved = 0;
	Asset->ForEachPatch([&](const FSurfPatch& Patch)
	{
		FSurfPatch Copy = Patch;
		Copy.Id.Invalidate();
		Copy.EnsureId();
		Group->AddPatch(MoveTemp(Copy));
		++Moved;
	});
	Asset->ForEachBrush([&](const FSurfBrush& Brush)
	{
		FSurfBrush Copy = Brush;
		Copy.Id = FGuid::NewGuid();
		Group->AddBrush(MoveTemp(Copy));
		++Moved;
	});

	// What the prefab placed inside itself stays a placement: an instance
	// of each, where it stood, so nothing is flattened that did not ask to
	// be.
	int32 Placed = 0;
	for (const FSurfPlacement& Placement : Asset->Placements)
	{
		USurfPrefab* Child = Placement.Prefab.Get();
		if (!Child)
		{
			continue;
		}
		FActorSpawnParameters ChildParams;
		ChildParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASurfPrefabInstance* Kept = World->SpawnActor<ASurfPrefabInstance>(ASurfPrefabInstance::StaticClass(), Placement.Transform * Stood, ChildParams);
		if (Kept)
		{
			Kept->Prefab = Child;
			Kept->SetActorLabel(Child->GetName());
			Kept->RebuildPreview();
			if (OutSpawned)
			{
				OutSpawned->Add(Kept);
			}
			++Placed;
		}
	}

	// And what it carried - a mesh, a blueprint, a light - comes out as the
	// level's own actor where it stood. It used to be dropped on the floor:
	// unpacking a prefab with a door mesh in it lost the door (S83a).
	int32 Carried = 0;
	for (const FSurfPart& Part : Asset->Parts)
	{
		if (AActor* Made = SpawnPartActor(Part, Part.Transform * Stood, /*bTransient*/ false))
		{
			if (OutSpawned)
			{
				OutSpawned->Add(Made);
			}
			++Carried;
		}
	}

	Group->RebuildPreview();
	Instance->Modify();
	Instance->Destroy();

	ClearSelection();
	ClearPointSelection();
	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Group, true, true);
	}
	if (Inspector)
	{
		Inspector->SetResult(Placed > 0
			? FString::Printf(TEXT("%s unpacked - %d piece%s now plain geometry in the level, %d prefab%s inside it kept as instances; %s is still in the library"),
				*AssetName, Moved, Moved == 1 ? TEXT("") : TEXT("s"), Placed, Placed == 1 ? TEXT("") : TEXT("s"), *AssetName)
			: FString::Printf(TEXT("%s unpacked - %d piece%s now plain geometry in the level; %s is still in the library"),
				*AssetName, Moved, Moved == 1 ? TEXT("") : TEXT("s"), *AssetName));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("unpack_prefab"), FString::Printf(TEXT("asset=%s pieces=%d kept=%d carried=%d"), *AssetName, Moved, Placed, Carried));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Moved > 0 || Placed > 0 || Carried > 0;
}

bool USurfEdMode::CanPromoteToScene() const
{
	const ASurfPrefabInstance* Instance = SelectedInstance();
	return Instance != nullptr
		&& Instance->Prefab != nullptr
		&& Instance != RoomStandIn.Get()
		&& Instance != DrilledInstance.Get();
}

bool USurfEdMode::PromoteToScene(const FString& Name)
{
	ASurfPrefabInstance* Instance = SelectedInstance();
	USurfPrefab* Asset = Instance ? Instance->Prefab.Get() : nullptr;
	if (!CanPromoteToScene() || !Asset)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("promote to level: click a placed prefab first"));
		}
		SurfSession::Friction(TEXT("promote_nothing"));
		return false;
	}

	const FString AssetName = Asset->GetName();

	// A promotion is one step for the hand and two for the tool: what the
	// prefab held comes out where it stood, and what came out becomes a
	// level of its own.
	TArray<AActor*> Spawned;
	if (!UnpackSelectedPrefab(&Spawned) || Spawned.Num() == 0)
	{
		return false;
	}

	if (GEditor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		for (AActor* Actor : Spawned)
		{
			if (IsValid(Actor))
			{
				GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ false);
			}
		}
		GEditor->NoteSelectionChange();
	}

	const FString SceneName = Name.IsEmpty() ? AssetName : Name;
	if (!MakeSceneFromSelection(SceneName))
	{
		// The unpack stands: what was in the prefab is in the level, which
		// is the half that worked, and undo takes both back together.
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s was unpacked but could not be made into a scene - it is in the level as plain geometry"), *AssetName));
		}
		return false;
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s is a scene now - a level of its own, holding what the prefab held; %s is still in the library"),
			*SceneName, *AssetName));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("promote_to_scene"), FString::Printf(TEXT("asset=%s scene=%s actors=%d"), *AssetName, *SceneName, Spawned.Num()));
	return true;
}

bool USurfEdMode::MakePrefabUnique()
{
	ASurfPrefabInstance* Instance = SelectedInstance();
	if (!Instance || !Instance->Prefab)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("unique: click a placed prefab first"));
		}
		return false;
	}
	if (IsInsidePrefab())
	{
		UnwindRooms(/*bKeep*/ true);
	}

	// A copy of the asset under its own name; this instance alone points at
	// it, so editing it from here changes nothing else in the level.
	FString PackagePath;
	const FString Name = FreePrefabName(PackagePath);
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return false;
	}
	Package->FullyLoad();

	FScopedTransaction Transaction(LOCTEXT("MakeUnique", "Make Prefab Unique"));

	USurfPrefab* Copy = DuplicateObject<USurfPrefab>(Instance->Prefab, Package, *Name);
	if (!Copy)
	{
		Transaction.Cancel();
		return false;
	}
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Package->MarkPackageDirty();

	Instance->Modify();
	Instance->Prefab = Copy;
	Instance->RebuildPreview();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s is now this instance's own - editing it changes nothing else; Ctrl+S to keep it"), *Name));
	}
	SurfSession::Verb(TEXT("unique_prefab"), Name);
	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

// --- Compile (S66a) --------------------------------------------------------------

namespace
{
	/** What a finished bake has to say for itself, problems and all (S106a). */
	FString BakeLine(const SurfBake::FReport& Report, bool bStopped)
	{
		FString Line;
		if (Report.Meshes == 0)
		{
			Line = bStopped
				? TEXT("bake: stopped before anything was made")
				: TEXT("bake: nothing to bake - no geometry of ours and no placed prefab");
		}
		else
		{
			Line = FString::Printf(TEXT("%s the level's geometry and %d placed prefab%s into %d mesh%s, %d triangles, in %.1f s - Game view shows them; Ctrl+S keeps them; an edit drops them"),
				bStopped ? TEXT("stopped after baking") : TEXT("baked"),
				Report.Instances, Report.Instances == 1 ? TEXT("") : TEXT("s"),
				Report.Meshes, Report.Meshes == 1 ? TEXT("") : TEXT("es"),
				Report.Triangles, Report.Seconds);
		}

		if (Report.Tidied > 0)
		{
			Line += FString::Printf(TEXT("; %d old mesh%s tidied"), Report.Tidied, Report.Tidied == 1 ? TEXT("") : TEXT("es"));
		}

		// "perhaps see the progress(es) or issue(s) somewhere still?" - named,
		// not counted: a number tells him something is wrong and nothing else.
		// Two by name, then the count, because the line has a viewport to fit
		// into.
		if (Report.Problems.Num() > 0)
		{
			Line += FString::Printf(TEXT("  |  %d would not bake: %s"),
				Report.Problems.Num(), *Report.Problems[0]);
			if (Report.Problems.Num() > 1)
			{
				Line += FString::Printf(TEXT("; %s"), *Report.Problems[1]);
			}
			if (Report.Problems.Num() > 2)
			{
				Line += FString::Printf(TEXT("; and %d more"), Report.Problems.Num() - 2);
			}
		}
		return Line;
	}
}

bool USurfEdMode::IsBaking() const
{
	return BakeJob.IsValid();
}

FString USurfEdMode::BakeProgress() const
{
	if (!BakeJob.IsValid())
	{
		return FString();
	}
	const int32 Total = BakeJob->Total();
	FString Line = FString::Printf(TEXT("baking %d of %d"), BakeJob->Done(), Total);
	if (BakeJob->Report.Problems.Num() > 0)
	{
		Line += FString::Printf(TEXT("  -  %d would not bake"), BakeJob->Report.Problems.Num());
	}
	return Line;
}

void USurfEdMode::StopBake()
{
	if (BakeJob.IsValid())
	{
		BakeJob->bStopped = true;
	}
}

bool USurfEdMode::BakeLevel()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Pressing Bake while one runs stops it: one button, and the label says
	// which of the two it is (S106a).
	if (BakeJob.IsValid())
	{
		StopBake();
		return false;
	}

	if (IsInsidePrefab())
	{
		UnwindRooms(/*bKeep*/ true);
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bBackground = Settings ? Settings->bBakeInBackground : true;

	auto Finish = [this](const SurfBake::FReport& Report, bool bStopped)
	{
		ClearSelection();
		ClearPointSelection();
		if (Inspector)
		{
			Inspector->SetResult(BakeLine(Report, bStopped));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("bake"), FString::Printf(TEXT("groups=%d instances=%d meshes=%d tris=%d s=%.2f problems=%d stopped=%d"),
			Report.Groups, Report.Instances, Report.Meshes, Report.Triangles, Report.Seconds,
			Report.Problems.Num(), bStopped ? 1 : 0));
		for (const FString& Problem : Report.Problems)
		{
			SurfSession::Friction(TEXT("bake_problem"), Problem);
		}
		if (GEditor)
		{
			RedrawViewports();
		}
	};

	if (!bBackground)
	{
		FScopedTransaction Transaction(LOCTEXT("Bake", "Bake"));
		const SurfBake::FReport Report = SurfBake::BakeLevel(World);
		Finish(Report, /*bStopped*/ false);
		return Report.Meshes > 0;
	}

	/**
	 * The background bake (S106a).
	 *
	 * A slice a tick, each slice its own transaction - a transaction cannot
	 * be held open across frames without swallowing whatever he does in the
	 * meantime, and a bake he has to sit still through is the thing he asked
	 * to be rid of. So undo takes it back a piece at a time, and Unbake,
	 * which was always the real inverse, takes all of it at once.
	 */
	BakeJob = SurfBake::PlanLevelBake(World);
	if (BakeJob->Total() == 0)
	{
		const SurfBake::FReport Empty = BakeJob->Report;
		BakeJob.Reset();
		Finish(Empty, /*bStopped*/ false);
		return false;
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(
			TEXT("baking %d piece%s in the background - carry on; Bake again stops it"),
			BakeJob->Total(), BakeJob->Total() == 1 ? TEXT("") : TEXT("s")));
	}

	const double SliceSeconds = FMath::Clamp(Settings ? Settings->BakeSliceMs : 16.0, 1.0, 200.0) / 1000.0;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this,
		[this, SliceSeconds, Finish](float) -> bool
	{
		if (!BakeJob.IsValid())
		{
			return false;
		}

		bool bMore = false;
		{
			FScopedTransaction Slice(LOCTEXT("BakeSlice", "Bake"));
			bMore = SurfBake::StepBake(*BakeJob, SliceSeconds);
		}

		if (bMore)
		{
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

		const SurfBake::FReport Report = BakeJob->Report;
		const bool bStopped = BakeJob->bStopped;
		BakeJob.Reset();
		Finish(Report, bStopped);
		return false;
	}), 0.0f);

	return true;
}

bool USurfEdMode::UnbakeLevel()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FScopedTransaction Transaction(LOCTEXT("Unbake", "Unbake"));
	const int32 Count = SurfBake::UnbakeLevel(World);
	if (Inspector)
	{
		Inspector->SetResult(Count == 0
			? TEXT("unbake: nothing is baked")
			: FString::Printf(TEXT("unbaked %d - the previews are back; the meshes stay in the Content Browser"), Count));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("unbake"), FString::Printf(TEXT("%d"), Count));
	if (GEditor)
	{
		RedrawViewports();
	}
	return Count > 0;
}

#undef LOCTEXT_NAMESPACE
