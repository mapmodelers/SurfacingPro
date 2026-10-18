// Copyright MapModelers. All Rights Reserved.

#include "SurfPrefab.h"

#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "Rendering/ColorVertexBuffer.h"
#include "RenderingThread.h"

#include "SurfChunk.h"
#include "SurfSession.h"
#include "SurfPatchOps.h"
#include "SurfBrushMath.h"

int32 USurfPrefab::NumPatches() const
{
	int32 Count = 0;
	for (const USurfChunk* Chunk : Chunks)
	{
		Count += Chunk ? Chunk->Patches.Num() : 0;
	}
	return Count;
}

int32 USurfPrefab::NumBrushes() const
{
	int32 Count = 0;
	for (const USurfChunk* Chunk : Chunks)
	{
		Count += Chunk ? Chunk->Brushes.Num() : 0;
	}
	return Count;
}

void USurfPrefab::ForEachPatch(TFunctionRef<void(const FSurfPatch&)> Visit) const
{
	for (const USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfPatch& Patch : Chunk->Patches)
		{
			Visit(Patch);
		}
	}
}

void USurfPrefab::ForEachBrush(TFunctionRef<void(const FSurfBrush&)> Visit) const
{
	for (const USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfBrush& Brush : Chunk->Brushes)
		{
			Visit(Brush);
		}
	}
}

USurfChunk* USurfPrefab::GetChunkWithRoom()
{
	const int32 Capacity = FMath::Max(1, ChunkCapacity);
	for (USurfChunk* Chunk : Chunks)
	{
		if (Chunk && Chunk->Num() < Capacity)
		{
			return Chunk;
		}
	}

	Modify();
	USurfChunk* Chunk = NewObject<USurfChunk>(this, NAME_None, RF_Transactional);
	Chunks.Add(Chunk);
	return Chunk;
}

FSurfPatch& USurfPrefab::AddPatch(FSurfPatch&& Patch)
{
	USurfChunk* Chunk = GetChunkWithRoom();
	Chunk->Modify();
	const int32 Index = Chunk->Patches.Add(MoveTemp(Patch));
	return Chunk->Patches[Index];
}

FSurfBrush& USurfPrefab::AddBrush(FSurfBrush&& Brush)
{
	USurfChunk* Chunk = GetChunkWithRoom();
	Chunk->Modify();
	const int32 Index = Chunk->Brushes.Add(MoveTemp(Brush));
	return Chunk->Brushes[Index];
}

const FSurfPatch* USurfPrefab::FindPatch(const FGuid& Id) const
{
	for (const USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		if (const FSurfPatch* Found = Chunk->Patches.FindByPredicate([&Id](const FSurfPatch& P) { return P.Id == Id; }))
		{
			return Found;
		}
	}
	return nullptr;
}

FSurfPatch* USurfPrefab::ModifyPatch(const FGuid& Id)
{
	for (USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		if (FSurfPatch* Found = Chunk->FindPatch(Id))
		{
			Chunk->Modify();
			return Found;
		}
	}
	return nullptr;
}

bool USurfPrefab::ContainsWalk(const USurfPrefab* Other, TSet<const USurfPrefab*>& Seen) const
{
	// A visited set rather than a depth count: a prefab reached twice by two
	// paths is walked once, and a cycle that slipped in cannot spin forever.
	if (Seen.Contains(this))
	{
		return false;
	}
	Seen.Add(this);

	for (const FSurfPlacement& Placement : Placements)
	{
		const USurfPrefab* Child = Placement.Prefab.Get();
		if (!Child)
		{
			continue;
		}
		if (Child == Other || Child->ContainsWalk(Other, Seen))
		{
			return true;
		}
	}
	return false;
}

bool USurfPrefab::Contains(const USurfPrefab* Other) const
{
	if (!Other)
	{
		return false;
	}
	TSet<const USurfPrefab*> Seen;
	return ContainsWalk(Other, Seen);
}

bool USurfPrefab::CanPlace(const USurfPrefab* Child) const
{
	// A prefab inside itself, however many steps round, is the one thing a
	// reference graph must refuse - the engine refuses it for levels for the
	// same reason.
	return Child && Child != this && !Child->Contains(this);
}

bool USurfPrefab::Place(USurfPrefab* Child, const FTransform& Transform)
{
	if (!CanPlace(Child))
	{
		return false;
	}
	Modify();
	FSurfPlacement& Placement = Placements.AddDefaulted_GetRef();
	Placement.Prefab = Child;
	Placement.Transform = Transform;
	Placement.Id = FGuid::NewGuid();
	return true;
}

const FSurfPlacement* USurfPrefab::FindPlacement(const FGuid& Id) const
{
	for (const FSurfPlacement& Placement : Placements)
	{
		if (Placement.Id == Id)
		{
			return &Placement;
		}
	}
	return nullptr;
}

FSurfPlacement* USurfPrefab::ModifyPlacement(const FGuid& Id)
{
	for (FSurfPlacement& Placement : Placements)
	{
		if (Placement.Id == Id)
		{
			Modify();
			return &Placement;
		}
	}
	return nullptr;
}

bool USurfPrefab::RemovePlacement(const FGuid& Id)
{
	for (int32 I = 0; I < Placements.Num(); ++I)
	{
		if (Placements[I].Id == Id)
		{
			Modify();
			Placements.RemoveAt(I);
			return true;
		}
	}
	return false;
}

FBox USurfPrefab::LocalBounds() const
{
	TSet<const USurfPrefab*> Seen;
	return LocalBoundsWalk(Seen);
}

bool FSurfPart::ApplyVertexColours(UStaticMeshComponent* Component, const TArray<FColor>& Colours)
{
	const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
	if (!Mesh || Colours.Num() == 0 || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.Num() == 0)
	{
		return false;
	}
	if (Mesh->GetRenderData()->LODResources[0].GetNumVertices() != Colours.Num())
	{
		return false;
	}
	if (Component->LODData.Num() < 1)
	{
		Component->SetLODDataCount(1, Component->LODData.Num());
	}
	FStaticMeshComponentLODInfo& Info = Component->LODData[0];
	if (Info.OverrideVertexColors)
	{
		Info.ReleaseOverrideVertexColorsAndBlock();
	}
	Info.OverrideVertexColors = new FColorVertexBuffer();
	Info.OverrideVertexColors->InitFromColorArray(Colours);
	BeginInitResource(Info.OverrideVertexColors);
#if WITH_EDITOR
	Component->CachePaintedDataIfNecessary();   // editor-only API; game builds keep the override colours as set
#endif
	Component->MarkRenderStateDirty();
	return true;
}

bool FSurfPart::ReadVertexColours(const UStaticMeshComponent* Component, TArray<FColor>& Out)
{
	Out.Reset();
	if (!Component || Component->LODData.Num() < 1 || !Component->LODData[0].OverrideVertexColors)
	{
		return false;
	}
	Component->LODData[0].OverrideVertexColors->GetVertexColors(Out);
	return Out.Num() > 0;
}

int32 USurfPrefab::DropBakeParts()
{
	int32 Dropped = 0;
	for (int32 I = Parts.Num() - 1; I >= 0; --I)
	{
		if (Parts[I].IsBake())
		{
			Parts.RemoveAt(I);
			++Dropped;
		}
	}
	return Dropped;
}

int32 USurfPrefab::DropEmptyPatches()
{
	int32 Dropped = 0;
	for (USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		Chunk->ConditionalPostLoad();
		for (int32 I = Chunk->Patches.Num() - 1; I >= 0; --I)
		{
			if (SurfPatchOps::HasNoSurface(Chunk->Patches[I]))
			{
				Chunk->Patches.RemoveAt(I);
				++Dropped;
			}
		}
	}
	if (Dropped > 0)
	{
		SurfSession::Verb(TEXT("prefab_empty_dropped"), FString::Printf(TEXT("%s patches=%d"), *GetName(), Dropped));
	}
	return Dropped;
}

void USurfPrefab::PostLoad()
{
	Super::PostLoad();
	// A bake that got in as a part (S241a) is not the asset's to keep, and
	// neither is a patch with no surface (S245a).
	if (DropBakeParts() + DropEmptyPatches() > 0)
	{
		MarkPackageDirty();
	}
}

FGuid USurfPrefab::AddPart(FSurfPart&& Part)
{
	if (!Part.Id.IsValid())
	{
		Part.Id = FGuid::NewGuid();
	}
	const FGuid Id = Part.Id;
	Parts.Add(MoveTemp(Part));
	return Id;
}

const FSurfPart* USurfPrefab::FindPart(const FGuid& Id) const
{
	return Parts.FindByPredicate([&Id](const FSurfPart& Each) { return Each.Id == Id; });
}

FSurfPart* USurfPrefab::ModifyPart(const FGuid& Id)
{
	FSurfPart* Found = Parts.FindByPredicate([&Id](const FSurfPart& Each) { return Each.Id == Id; });
	if (Found)
	{
		Modify();
	}
	return Found;
}

bool USurfPrefab::RemovePart(const FGuid& Id)
{
	const int32 At = Parts.IndexOfByPredicate([&Id](const FSurfPart& Each) { return Each.Id == Id; });
	if (At == INDEX_NONE)
	{
		return false;
	}
	Modify();
	Parts.RemoveAt(At);
	return true;
}

FBox USurfPrefab::LocalBoundsWalk(TSet<const USurfPrefab*>& Seen) const
{
	if (Seen.Contains(this) || Seen.Num() > 32)
	{
		return FBox(ForceInit);
	}
	Seen.Add(this);

	FBox Bounds(ForceInit);
	ForEachPatch([&Bounds](const FSurfPatch& Patch)
	{
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			Bounds += Point.Pos;
		}
	});
	ForEachBrush([&Bounds](const FSurfBrush& Brush)
	{
		FSurfHull Hull;
		if (SurfBrushMath::DeriveHull(Brush, Hull))
		{
			for (const FVector& Vertex : Hull.Vertices)
			{
				Bounds += Vertex;
			}
		}
	});

	// A part counts towards the bounds: a prefab that is only a mesh and a
	// light still has a size, and that is what places it on the grid (S76a).
	for (const FSurfPart& Part : Parts)
	{
		if (!Part.IsSomething())
		{
			continue;
		}
		FBox Local(FVector(-25.0), FVector(25.0));
		if (const UStaticMesh* Mesh = Part.Mesh.Get())
		{
			const FBox FromMesh = Mesh->GetBoundingBox();
			if (FromMesh.IsValid)
			{
				Local = FromMesh;
			}
		}
		Bounds += Local.TransformBy(Part.Transform);
	}

	for (const FSurfPlacement& Placement : Placements)
	{
		const USurfPrefab* Child = Placement.Prefab.Get();
		if (!Child || Seen.Contains(Child))
		{
			continue;
		}
		// The set is threaded through, as the other walks thread theirs: a
		// fresh one at each step guarded only against a prefab placing
		// itself directly, and A in B in A - which the Details panel can
		// still make by hand - went round forever.
		const FBox ChildBounds = Child->LocalBoundsWalk(Seen);
		if (ChildBounds.IsValid)
		{
			Bounds += ChildBounds.TransformBy(Placement.Transform);
		}
	}
	return Bounds;
}
