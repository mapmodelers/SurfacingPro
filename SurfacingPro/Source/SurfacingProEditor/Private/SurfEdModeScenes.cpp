// Copyright MapModelers. All Rights Reserved.
//
// Scenes (S75a): the other kind of prefab.
//
// A piece is our geometry and nothing else - a data asset, placed by the
// thousand, edited in our own room. A scene is a level of its own, placed as
// the engine's Level Instance, and it holds anything a level holds: lights,
// blueprints, static meshes, our groups, piece instances. "Can a prefab hold
// a BP / static mesh / light? No -- perhaps we have a design problem."
//
// The engine already has all of it - the level asset, the edit session, the
// outliner swapping to the level being edited, the commit - so this file is
// glue and nothing else: which actors go in, where the asset lands, and the
// two verbs (in, out) reaching the engine's own. What it must not do is
// invent a second way to edit a level.

#include "SurfEdMode.h"

#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfPrefabInstance.h"
#include "SurfSession.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "SurfAutoBake.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "LevelEditorViewport.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

namespace
{
	ULevelInstanceSubsystem* ScenesOf(UWorld* World)
	{
		return World ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
	}

	/** A name an asset can carry: letters, digits and underscores. */
	FString CleanSceneName(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			Out.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
		}
		Out.TrimStartAndEndInline();
		Out = Out.IsEmpty() ? FString(TEXT("Level")) : Out;
		// LI_ in front, once (S154h): the same prefix MapLink gives a level
		// instance, so a level of ours reads as one in any list.
		return Out.StartsWith(TEXT("LI_")) ? Out : FString(TEXT("LI_")) + Out;
	}

	const TCHAR* SceneFolder = TEXT("/Game/SurfacingPro/Levels");

	bool PackageFree(const FString& Path)
	{
		return !FPackageName::DoesPackageExist(Path) && !FindPackage(nullptr, *Path);
	}
}

ALevelInstance* USurfEdMode::SelectedScene() const
{
	if (!GEditor)
	{
		return nullptr;
	}
	USelection* Selected = GEditor->GetSelectedActors();
	if (!Selected)
	{
		return nullptr;
	}
	for (FSelectionIterator It(*Selected); It; ++It)
	{
		if (ALevelInstance* Scene = Cast<ALevelInstance>(*It))
		{
			return Scene;
		}
	}
	return nullptr;
}

bool USurfEdMode::IsInsideScene() const
{
	const ULevelInstanceSubsystem* Scenes = ScenesOf(const_cast<USurfEdMode*>(this)->GetWorld());
	return Scenes && Scenes->GetEditingLevelInstance() != nullptr;
}

TArray<AActor*> USurfEdMode::ActorsForScene() const
{
	// Everything the hand has hold of: the actors the editor has selected,
	// the group of the patch being edited, and the prefab instance picked in
	// our own selection - each once. A scene is made of what is on screen and
	// selected, whatever kind of thing it is.
	TArray<AActor*> Actors;
	auto Add = [&Actors](AActor* Actor)
	{
		if (Actor && IsValid(Actor) && !Actors.Contains(Actor))
		{
			Actors.Add(Actor);
		}
	};

	if (GEditor)
	{
		if (USelection* Selected = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*Selected); It; ++It)
			{
				Add(Cast<AActor>(*It));
			}
		}
	}
	Add(SelectedPatch.Group.Get());
	for (const FSurfFaceRef& Ref : Selection)
	{
		Add(Ref.Group.Get());
	}
	for (const FSurfPointRef& Ref : PointSelection)
	{
		Add(Ref.Group.Get());
	}

	// Never the room's own furniture: a stand-in belongs to an open room and
	// would be carried into the scene as a ghost of itself.
	Actors.RemoveAll([this](AActor* Actor)
	{
		return Actor == RoomStandIn.Get() || Actor == DrilledGroup.Get();
	});
	return Actors;
}

bool USurfEdMode::CanMakeScene() const
{
	return !IsInsidePrefab() && ActorsForScene().Num() > 0;
}

FString USurfEdMode::SuggestSceneName() const
{
	for (int32 N = 1; N < 10000; ++N)
	{
		// MapLink's convention (S154h): a level instance is LI_<name>.
		const FString Name = FString::Printf(TEXT("LI_Level_%d"), N);
		if (PackageFree(FString(SceneFolder) / Name))
		{
			return Name;
		}
	}
	return TEXT("Level");
}

bool USurfEdMode::MakeSceneFromSelection(const FString& Name)
{
	UWorld* World = GetWorld();
	ULevelInstanceSubsystem* Scenes = ScenesOf(World);
	if (!Scenes)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("level: this level cannot hold level instances"));
		}
		SurfSession::Friction(TEXT("scene_no_subsystem"));
		return false;
	}

	if (IsInsidePrefab())
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("level: leave the prefab first (Alt+PgUp) - a level is made in the level you are in"));
		}
		SurfSession::Friction(TEXT("scene_inside_prefab"));
		return false;
	}

	TArray<AActor*> Actors = ActorsForScene();

	/**
	 * What was picked, not the whole group it sits in (S174a): the level
	 * takes what Prefab takes. A group holds every piece made near each
	 * other; picking two walls of a room used to carry the room's whole
	 * group into the level. Picked patches and primitives that share a
	 * group with pieces left behind move to a group of their own first,
	 * at the same place, and that group goes in.
	 */
	{
		TArray<FSurfPatchRef> Patches = GetSelectedPatches();
		for (const FSurfPatchRef& Held : PatchesHeldBySelection())
		{
			Patches.AddUnique(Held);
		}
		TMap<ASurfGroup*, TArray<FGuid>> PatchesOf;
		TMap<ASurfGroup*, TArray<FGuid>> BrushesOf;
		for (const FSurfPatchRef& Ref : Patches)
		{
			if (ASurfGroup* Group = Ref.Group.Get())
			{
				PatchesOf.FindOrAdd(Group).AddUnique(Ref.PatchId);
			}
		}
		for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
		{
			if (ASurfGroup* Group = Ref.Group.Get())
			{
				BrushesOf.FindOrAdd(Group).AddUnique(Ref.BrushId);
			}
		}
		TSet<ASurfGroup*> Touched;
		for (const TPair<ASurfGroup*, TArray<FGuid>>& Each : PatchesOf) { Touched.Add(Each.Key); }
		for (const TPair<ASurfGroup*, TArray<FGuid>>& Each : BrushesOf) { Touched.Add(Each.Key); }

		TUniquePtr<FScopedTransaction> Split;
		for (ASurfGroup* Group : Touched)
		{
			const TArray<FGuid>* PickedPatches = PatchesOf.Find(Group);
			const TArray<FGuid>* PickedBrushes = BrushesOf.Find(Group);
			const int32 Picked = (PickedPatches ? PickedPatches->Num() : 0) + (PickedBrushes ? PickedBrushes->Num() : 0);
			if (Picked >= Group->NumPatches() + Group->NumBrushes())
			{
				Actors.AddUnique(Group);
				continue;
			}
			if (!Split.IsValid())
			{
				Split = MakeUnique<FScopedTransaction>(LOCTEXT("SplitForLevel", "Pieces Apart for the Level"));
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ASurfGroup* Own = World ? World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), Group->GetActorTransform(), Params) : nullptr;
			if (!Own)
			{
				continue;
			}
			Own->PreviewMaterial = Group->PreviewMaterial;
			Group->Modify();
			if (PickedPatches)
			{
				for (const FGuid& Id : *PickedPatches)
				{
					if (const FSurfPatch* Patch = Group->FindPatch(Id))
					{
						FSurfPatch Copy = *Patch;
						Group->RemovePatch(Id);
						Own->AddPatch(MoveTemp(Copy));
					}
				}
			}
			if (PickedBrushes)
			{
				for (const FGuid& Id : *PickedBrushes)
				{
					if (const FSurfBrush* Brush = Group->FindBrush(Id))
					{
						FSurfBrush Copy = *Brush;
						Group->RemoveBrush(Id);
						Own->AddBrush(MoveTemp(Copy));
					}
				}
			}
			Group->RebuildPreview();
			Own->RebuildPreview();
			Actors.Remove(Group);
			Actors.AddUnique(Own);
		}
		Split.Reset();
	}

	if (Actors.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("level: select what goes in it first - geometry, a light, a blueprint, anything"));
		}
		SurfSession::Friction(TEXT("scene_nothing_selected"));
		return false;
	}

	FText Reason;
	if (!Scenes->CanCreateLevelInstanceFrom(Actors, &Reason))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("level: %s"), *Reason.ToString()));
		}
		SurfSession::Friction(TEXT("scene_refused"), Reason.ToString());
		return false;
	}

	// The first free name under our own folder: a scene is one file, as a
	// piece is, which is what makes two people on two scenes never collide.
	const FString Wanted = CleanSceneName(Name.IsEmpty() ? SuggestSceneName() : Name);
	FString Package = FString(SceneFolder) / Wanted;
	for (int32 N = 2; !PackageFree(Package) && N < 10000; ++N)
	{
		Package = FString(SceneFolder) / FString::Printf(TEXT("%s_%d"), *Wanted, N);
	}

	FNewLevelInstanceParams Params;
	Params.Type = ELevelInstanceCreationType::LevelInstance;
	// The same origin a piece gets: the middle of the footprint, on the
	// floor, so a scene is placed by where it stands rather than by where it
	// happened to be built.
	Params.PivotType = ELevelInstancePivotType::CenterMinZ;
	Params.bAlwaysShowDialog = false;
	Params.bPromptForSave = false;
	Params.LevelPackageName = Package;

	ILevelInstanceInterface* Made = Scenes->CreateLevelInstanceFrom(Actors, Params);
	if (!Made)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("level: it could not be made"));
		}
		SurfSession::Friction(TEXT("scene_not_made"));
		return false;
	}

	AActor* Actor = Cast<AActor>(Made);
	if (Actor)
	{
		Actor->SetActorLabel(FPackageName::GetShortName(Package));
		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);
		}
	}

	ClearPointSelection();
	SelectPatch(FSurfPatchRef());
	ClearSelection();

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("level %s made from %d thing%s - Alt+PgDn opens it, and the outliner becomes its own"),
			*FPackageName::GetShortName(Package), Actors.Num(), Actors.Num() == 1 ? TEXT("") : TEXT("s")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("scene_made"), FString::Printf(TEXT("%s actors=%d"), *Package, Actors.Num()));
	return true;
}

bool USurfEdMode::EnterScene(ALevelInstance* Scene)
{
	ULevelInstanceSubsystem* Scenes = ScenesOf(GetWorld());
	if (!Scenes || !Scene)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("open: click a level first"));
		}
		return false;
	}

	// A piece room and a scene session are not both open: one place at a
	// time, as a room is.
	if (IsInsidePrefab())
	{
		UnwindRooms(/*bKeep*/ true);
	}

	FText Reason;
	if (!Scenes->CanEditLevelInstance(Scene, &Reason))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("level: %s"), *Reason.ToString()));
		}
		SurfSession::Friction(TEXT("scene_cannot_edit"), Reason.ToString());
		return false;
	}

	Scenes->EditLevelInstance(Scene);
	EditingScene = Scene;

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("editing the level %s - the outliner is its own now; everything in it is an actor; Alt+PgUp keeps it"),
			*Scene->GetActorLabel()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("scene_enter"), Scene->GetActorLabel());
	return true;
}

bool USurfEdMode::ExitScene(bool bKeep)
{
	ULevelInstanceSubsystem* Scenes = ScenesOf(GetWorld());
	ILevelInstanceInterface* Editing = Scenes ? Scenes->GetEditingLevelInstance() : nullptr;
	if (!Editing)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("close: not inside a level"));
		}
		SurfSession::Friction(TEXT("scene_out_not_inside"));
		return false;
	}

	FText Reason;
	if (!Scenes->CanCommitLevelInstance(Editing, /*bDiscardEdits*/ !bKeep, &Reason))
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("level: %s"), *Reason.ToString()));
		}
		SurfSession::Friction(TEXT("scene_cannot_commit"), Reason.ToString());
		return false;
	}

	// Kept: baked first, while its groups are still here to bake (S153c).
	if (bKeep)
	{
		SurfAutoBake::BeforeSceneKept(GetWorld(), Scenes->GetLevelInstanceLevel(Editing));
	}

	const bool bDone = Scenes->CommitLevelInstance(Editing, /*bDiscardEdits*/ !bKeep);
	EditingScene = nullptr;

	if (Inspector)
	{
		Inspector->SetResult(bDone
			? (bKeep ? TEXT("level kept - every place it stands follows") : TEXT("level left as it was"))
			: TEXT("level: the edit could not be closed"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("scene_exit"), FString::Printf(TEXT("keep=%d ok=%d"), bKeep ? 1 : 0, bDone ? 1 : 0));
	return bDone;
}

const TCHAR* USurfEdMode::ScenesFolder()
{
	return SceneFolder;
}

TArray<ALevelInstance*> USurfEdMode::SceneInstancesOf(const FSoftObjectPath& Scene) const
{
	TArray<ALevelInstance*> Out;
	UWorld* World = const_cast<USurfEdMode*>(this)->GetWorld();
	if (!World || !Scene.IsValid())
	{
		return Out;
	}
	for (TActorIterator<ALevelInstance> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->HasAnyFlags(RF_Transient) && It->GetWorldAsset().ToSoftObjectPath() == Scene)
		{
			Out.Add(*It);
		}
	}
	return Out;
}

ALevelInstance* USurfEdMode::PlaceSceneAsset(const FSoftObjectPath& Scene, const FVector& At)
{
	UWorld* World = GetWorld();
	if (!World || !Scene.IsValid())
	{
		return nullptr;
	}
	FScopedTransaction Transaction(LOCTEXT("PlaceScene", "Place Level"));
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALevelInstance* Actor = World->SpawnActor<ALevelInstance>(ALevelInstance::StaticClass(), FTransform(At), Params);
	if (!Actor)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("level: could not be placed"));
		}
		SurfSession::Friction(TEXT("scene_place_failed"));
		return nullptr;
	}
	if (!Actor->SetWorldAsset(TSoftObjectPtr<UWorld>(Scene)))
	{
		// The open level, a partitioned one, a loop: the engine says no (S217a).
		World->EditorDestroyActor(Actor, false);
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s cannot be a level instance here"), *Scene.GetAssetName()));
		}
		SurfSession::Friction(TEXT("scene_place_refused"));
		return nullptr;
	}
	Actor->SetActorLabel(Scene.GetAssetName());
	Actor->LoadLevelInstance();
	// Tagged and filed as a placed mesh is (S217a): the Assets drawer, the
	// origin mark; inside an open prefab the room takes it as a part.
	Actor->Tags.AddUnique(PlacedMeshTag);
	Actor->SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
	if (!IsInsidePrefab())
	{
		TidyOutliner();
	}
	if (GEditor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);
	}
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("%s placed - G moves it; Alt+PgDn edits it"), *Scene.GetAssetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("place_scene"), Scene.GetAssetName());
	return Actor;
}

int32 USurfEdMode::FindSceneInstances(const FSoftObjectPath& Scene)
{
	if (!GEditor)
	{
		return 0;
	}
	TArray<ALevelInstance*> Found = SceneInstancesOf(Scene);
	ClearSelection();
	ClearPointSelection();
	GEditor->SelectNone(false, true);
	FBox Bounds(ForceInit);
	for (ALevelInstance* Each : Found)
	{
		GEditor->SelectActor(Each, true, false);
		Bounds += Each->GetComponentsBoundingBox(true);
	}
	GEditor->NoteSelectionChange();
	if (Found.Num() > 0 && Bounds.IsValid && GCurrentLevelEditingViewportClient)
	{
		GCurrentLevelEditingViewportClient->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent().GetMax() * 0.25 + 25.0), /*bInstant*/ true);
	}
	if (Inspector)
	{
		Inspector->SetResult(Found.Num() == 0
			? FString::Printf(TEXT("%s is not placed in this level - double-click its tile to place it"), *Scene.GetAssetName())
			: FString::Printf(TEXT("%d placement%s of %s selected and framed"), Found.Num(), Found.Num() == 1 ? TEXT("") : TEXT("s"), *Scene.GetAssetName()));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("find_scene"), FString::Printf(TEXT("scene=%s found=%d"), *Scene.GetAssetName(), Found.Num()));
	return Found.Num();
}

bool USurfEdMode::EditSceneAsset(const FSoftObjectPath& Scene)
{
	TArray<ALevelInstance*> Found = SceneInstancesOf(Scene);
	if (Found.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("%s is not placed in this level - double-click its tile to place it, then Open"), *Scene.GetAssetName()));
		}
		SurfSession::Friction(TEXT("scene_edit_not_placed"));
		return false;
	}
	return EnterScene(Found[0]);
}

#undef LOCTEXT_NAMESPACE
