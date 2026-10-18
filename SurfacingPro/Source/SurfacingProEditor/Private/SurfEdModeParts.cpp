// Copyright MapModelers. All Rights Reserved.
//
// The parts of the prefab being edited, while it is open (S77a).
//
// A prefab carries parts - a mesh, a blueprint, a light (S76a) - and the way
// to work on them is the way anyone works on a level: open the prefab, put
// things in it, close it. So while a room is open its parts stand in the
// level as real actors: they select, move, duplicate and delete with the
// editor's own keys, and anything dropped in from the Content Browser while
// the room is open joins them without a verb being pressed. Closing writes
// them back into the asset; dropping the edits puts them back as they were.
//
// The rule this file exists for: **inside a prefab, what you make belongs to
// the prefab.** Anything else - a button that must be remembered, a panel
// that must be found - is a step between the hand and the thing.

#include "SurfEdMode.h"

#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "SurfSession.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Materials/MaterialInterface.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

namespace
{
	/** What marks an actor as standing in for a part while a room is open. */
	const FName PartActorTag(TEXT("SurfacingProPart"));

	/** Ours, in the sense that the room already knows what to do with it. */
	bool PartsIsOurs(const AActor* Actor)
	{
		// A level instance is a part since S218a ("theyre all containers").
		return Actor
			&& (Actor->IsA(ASurfGroup::StaticClass())
				|| Actor->IsA(ASurfPrefabInstance::StaticClass())
				|| Actor->Tags.Contains(TEXT("SurfacingProBaked")));
	}
}

void USurfEdMode::SpawnRoomParts(USurfPrefab& Asset)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	ClearRoomParts();

	for (const FSurfPart& Part : Asset.Parts)
	{
		// A bake that got in as a part is left out, and Close drops it (S241a).
		if (Part.IsBake())
		{
			SurfSession::Verb(TEXT("part_bake_dropped"), Part.Label);
			continue;
		}
		if (AActor* Actor = SpawnPartActor(Part, Part.Transform * RoomFrame, /*bTransient*/ true))
		{
			Actor->Tags.AddUnique(PartActorTag);
			RoomPartActors.Add(Actor);
		}
	}
}

AActor* USurfEdMode::SpawnPartActor(const FSurfPart& Part, const FTransform& At, bool bTransient)
{
	UWorld* World = GetWorld();
	if (!World || !Part.IsSomething())
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (bTransient)
	{
		// While a room is open the actors are a view of the asset, thrown
		// away when it closes. Unpacked, they are the level's own.
		Params.ObjectFlags |= RF_Transient;
	}

	AActor* Actor = nullptr;
	if (UStaticMesh* Mesh = Part.Mesh.LoadSynchronous())
	{
		AStaticMeshActor* Placed = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), At, Params);
		if (Placed && Placed->GetStaticMeshComponent())
		{
			Placed->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Placed->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			for (int32 Slot = 0; Slot < Part.Materials.Num(); ++Slot)
			{
				if (UMaterialInterface* Material = Part.Materials[Slot].LoadSynchronous())
				{
					Placed->GetStaticMeshComponent()->SetMaterial(Slot, Material);
				}
			}
			// Its paint, out in the room to be painted further (S237a).
			if (Part.VertexColours.Num() > 0)
			{
				FSurfPart::ApplyVertexColours(Placed->GetStaticMeshComponent(), Part.VertexColours);
			}
		}
		Actor = Placed;
	}
	else if (Part.IsLevel())
	{
		ALevelInstance* Placed = World->SpawnActor<ALevelInstance>(ALevelInstance::StaticClass(), At, Params);
		if (Placed && Placed->SetWorldAsset(Part.Level))
		{
			Placed->LoadLevelInstance();
		}
		Actor = Placed;
	}
	else if (UClass* Class = Part.ActorClass.LoadSynchronous())
	{
		Actor = World->SpawnActor<AActor>(Class, At, Params);
	}

	if (Actor && !Part.Label.IsEmpty())
	{
		Actor->SetActorLabel(Part.Label);
	}
	return Actor;
}

int32 USurfEdMode::CaptureRoomParts(USurfPrefab& Asset)
{
	// The asset's parts are exactly what stands in the room now: one was
	// moved, one was deleted, three were dropped in from the Content
	// Browser. Written whole rather than diffed - the actors are the truth
	// while the room is open.
	Asset.Modify();
	Asset.Parts.Reset();

	int32 Kept = 0;
	for (const TWeakObjectPtr<AActor>& Weak : RoomPartActors)
	{
		AActor* Actor = Weak.Get();
		if (!Actor || !IsValid(Actor))
		{
			continue;
		}

		FSurfPart Part;
		FillPart(Part, Actor);
		if (Part.IsBake() || Actor->Tags.Contains(TEXT("SurfacingProBaked")))
		{
			continue;
		}
		Part.Transform = Actor->GetActorTransform().GetRelativeTransform(RoomFrame);
		// The paint goes with the part only when the prefab keeps paint (S237a).
		if (Asset.bMeshPaint)
		{
			if (const AStaticMeshActor* Mesh = Cast<AStaticMeshActor>(Actor))
			{
				FSurfPart::ReadVertexColours(Mesh->GetStaticMeshComponent(), Part.VertexColours);
			}
		}
		Asset.AddPart(MoveTemp(Part));
		++Kept;
	}

	Asset.MarkPackageDirty();
	return Kept;
}

void USurfEdMode::FillPart(FSurfPart& Part, const AActor* Actor)
{
	// What an actor is, as a part: a mesh with what it wears, a level (S218a),
	// else its class - a blueprint, a light, a trigger - spawned again
	// wherever the prefab stands. The transform is the caller's.
	Part.Label = Actor->GetActorLabel();
	if (const AStaticMeshActor* Mesh = Cast<AStaticMeshActor>(Actor))
	{
		const UStaticMeshComponent* Component = Mesh->GetStaticMeshComponent();
		if (Component && Component->GetStaticMesh())
		{
			Part.Mesh = Component->GetStaticMesh();
			for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
			{
				Part.Materials.Add(Component->GetMaterial(Slot));
			}
		}
	}
	else if (const ALevelInstance* Level = Cast<ALevelInstance>(Actor))
	{
		Part.Level = Level->GetWorldAsset();
	}
	if (!Part.IsMesh() && !Part.IsLevel())
	{
		Part.ActorClass = Actor->GetClass();
	}
}

void USurfEdMode::ClearRoomParts()
{
	for (const TWeakObjectPtr<AActor>& Weak : RoomPartActors)
	{
		if (AActor* Actor = Weak.Get())
		{
			Actor->Destroy();
		}
	}
	RoomPartActors.Reset();
}

bool USurfEdMode::IsRoomPartActor(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	for (const TWeakObjectPtr<AActor>& Weak : RoomPartActors)
	{
		if (Weak.Get() == Actor)
		{
			return true;
		}
	}
	return false;
}

void USurfEdMode::WatchRoomActors(bool bOn)
{
	if (!GEngine)
	{
		return;
	}
	if (bOn)
	{
		if (!RoomActorAdded.IsValid())
		{
			RoomActorAdded = GEngine->OnLevelActorAdded().AddUObject(this, &USurfEdMode::OnActorAddedToRoom);
		}
		return;
	}
	if (RoomActorAdded.IsValid())
	{
		GEngine->OnLevelActorAdded().Remove(RoomActorAdded);
		RoomActorAdded.Reset();
	}
}

void USurfEdMode::OnActorAddedToRoom(AActor* Actor)
{
	// Dropped in from the Content Browser, placed from the toolbar, pasted:
	// while a prefab is open, what arrives in the level arrives in the
	// prefab. No verb, because there is nothing to decide - it was put here
	// on purpose, and Close writes it in.
	if (!IsInsidePrefab() || !Actor || !IsValid(Actor))
	{
		return;
	}
	if (Actor->GetWorld() != GetWorld())
	{
		return;
	}
	/**
	 * Not the bake's stand-in (S241a): Game view inside an open prefab
	 * spawned the room's bake into the room, the room took it as a dropped
	 * part, wrote it into the asset at Close, and every instance grew a
	 * second, baked copy of itself ("its very wrong"). The level says an
	 * actor was added before its tags are set, but the bake's actors are
	 * the only ones we spawn unlisted in the outliner, and that is set
	 * before the level speaks.
	 */
	if (PartsIsOurs(Actor) || IsRoomPartActor(Actor) || !Actor->IsListedInSceneOutliner())
	{
		return;
	}
	JoinRoomPart(Actor);
}

void USurfEdMode::JoinRoomPart(AActor* Actor)
{
	Actor->Tags.AddUnique(PartActorTag);
	RoomPartActors.Add(Actor);

	if (Inspector)
	{
		const ASurfPrefabInstance* Drilled = DrilledInstance.Get();
		Inspector->SetResult(FString::Printf(TEXT("%s is part of %s now - Close keeps it, and every instance carries it"),
			*Actor->GetActorLabel(),
			Drilled && Drilled->Prefab ? *Drilled->Prefab->GetName() : TEXT("this prefab")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("part_joined"), Actor->GetActorLabel());
}

#undef LOCTEXT_NAMESPACE
