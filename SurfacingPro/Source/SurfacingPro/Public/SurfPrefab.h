// Copyright MapModelers. All Rights Reserved.
//
// A prefab (S65a, D72). Geometry that is placed many times lives once, in
// this asset - the same chunked storage a group has (D21) - and each
// placement in a map is one light actor pointing at it (ASurfPrefabInstance).
// A prefab can hold placements of other prefabs, so a house is a prefab of
// rooms is a prefab of walls: nesting is a reference graph between assets,
// and the one thing it may not do is contain itself.
//
// Level Instances are left to the map scale, where a building holds lights,
// blueprints and imported meshes beside a few of ours. Our own asset exists
// for the count: a door frame placed a thousand times is a thousand light
// actors and one build, not a thousand level loads.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SurfTypes.h"
#include "SurfPrefab.generated.h"

class USurfChunk;
class USurfPrefab;

/** One prefab placed inside another, at a transform in the parent's space. */
USTRUCT()
struct SURFACINGPRO_API FSurfPlacement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The prefab placed here."))
	TObjectPtr<USurfPrefab> Prefab;

	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "Where it stands inside the prefab."))
	FTransform Transform;

	UPROPERTY()
	FGuid Id;
};

/**
 * One thing a prefab carries that is not our geometry (S76a): a static mesh,
 * or an actor class - a blueprint, a light, a trigger - at a transform in the
 * prefab's space. "id rather be able to include other elements (sm. bp. etc.)
 * with prefabs too."
 *
 * A part is carried, not contained: the asset holds what it is and where it
 * stands, and every instance shows it as a component of its own. That is the
 * difference from a scene, where the same things are actors in a level and
 * are edited in the outliner - a prefab is for the piece placed a thousand
 * times, a scene for the room you walk into.
 */
USTRUCT()
struct SURFACINGPRO_API FSurfPart
{
	GENERATED_BODY()

	/** A mesh, drawn by a component on every instance. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The static mesh this part places."))
	TSoftObjectPtr<class UStaticMesh> Mesh;

	/** Or a class - a blueprint, a light, anything an actor can be - spawned as a child actor on every instance. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The kind of actor this part places, when it is not a mesh."))
	TSoftClassPtr<AActor> ActorClass;

	/** Or a level (S218a), placed as a level instance on every instance - a container inside a container. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The level this part places as a level instance."))
	TSoftObjectPtr<class UWorld> Level;

	/** What the mesh wears, slot by slot. An empty slot keeps the mesh's own. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The part's own materials, slot by slot; empty keeps the mesh's."))
	TArray<TSoftObjectPtr<class UMaterialInterface>> Materials;

	/** Where it stands, in the prefab's space. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "Where the part stands inside the prefab."))
	FTransform Transform;

	/** What it was called where it came from, so a part reads as itself. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The part's name."))
	FString Label;

	UPROPERTY()
	FGuid Id;

	/**
	 * The mesh's vertex paint, a colour per vertex of LOD 0 (S237a), kept
	 * only when the prefab has Painted meshes on; every instance wears it.
	 * Empty: the mesh's own colours.
	 */
	UPROPERTY()
	TArray<FColor> VertexColours;

	/** Put the kept paint on a component's LOD 0 as its override colours; nothing when the count does not match. */
	static bool ApplyVertexColours(class UStaticMeshComponent* Component, const TArray<FColor>& Colours);
	/** The override colours a component wears, if any. */
	static bool ReadVertexColours(const class UStaticMeshComponent* Component, TArray<FColor>& Out);

	bool IsMesh() const { return !Mesh.IsNull(); }
	/**
	 * A bake's own mesh taken in as a part (S241a): Game view inside an open
	 * prefab spawned the room's stand-in into the room, and the room took it
	 * as a dropped part. Such a part is never spawned, drawn or kept.
	 */
	bool IsBake() const { return !Mesh.IsNull() && (Mesh.ToString().Contains(TEXT("/SurfacingProBaked/")) || Mesh.GetAssetName().StartsWith(TEXT("SM_SP_"))); }
	bool IsActor() const { return !ActorClass.IsNull(); }
	bool IsLevel() const { return !Level.IsNull(); }
	bool IsSomething() const { return IsMesh() || IsActor() || IsLevel(); }
};

/** Where one placement's triangles sit in an instance's mesh (S71b), so a click on it knows which placement it hit. */
struct FSurfPlacementRange
{
	FGuid Id;
	int32 FirstTriangle = 0;
	int32 NumTriangles = 0;
	bool Holds(int32 Triangle) const { return Triangle >= FirstTriangle && Triangle < FirstTriangle + NumTriangles; }
};

UCLASS(BlueprintType)
class SURFACINGPRO_API USurfPrefab : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The geometry, in the prefab's own space, chunked as a group's is. */
	UPROPERTY(Instanced)
	TArray<TObjectPtr<USurfChunk>> Chunks;

	/** Other prefabs placed inside this one. */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "Prefabs placed inside this one."))
	TArray<FSurfPlacement> Placements;

	/** Meshes, blueprints and other actors carried by this prefab (S76a). */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "Meshes, levels and other actors carried by this prefab."))
	TArray<FSurfPart> Parts;

	UPROPERTY(EditAnywhere, Category = "Prefab|Advanced", meta = (ToolTip = "How many pieces are stored together; leave as it is.", ClampMin = "1", ClampMax = "512"))
	int32 ChunkCapacity = 32;

	/**
	 * Painted meshes (S237a): the meshes inside take the brush while the
	 * prefab is open, and the paint is kept with the part - every instance
	 * wears it. Off, a mesh inside is not paintable and the parts carry no
	 * paint.
	 */
	UPROPERTY(EditAnywhere, Category = "Prefab", meta = (ToolTip = "The meshes inside take the paint brush, and the paint is kept with the prefab."))
	bool bMeshPaint = false;

	int32 NumPatches() const;
	int32 NumBrushes() const;
	int32 NumPrimitives() const { return NumPatches() + NumBrushes(); }

	void ForEachPatch(TFunctionRef<void(const FSurfPatch&)> Visit) const;
	void ForEachBrush(TFunctionRef<void(const FSurfBrush&)> Visit) const;

	FSurfPatch& AddPatch(FSurfPatch&& Patch);
	FSurfBrush& AddBrush(FSurfBrush&& Brush);

	const FSurfPatch* FindPatch(const FGuid& Id) const;
	FSurfPatch* ModifyPatch(const FGuid& Id);

	/** Whether Other is placed anywhere inside this prefab, at any depth. */
	bool Contains(const USurfPrefab* Other) const;

	/** Whether Child could be placed here without a prefab containing itself. */
	bool CanPlace(const USurfPrefab* Child) const;

	/** Place Child at a transform; false, and nothing done, when it would make a cycle. */
	bool Place(USurfPrefab* Child, const FTransform& Transform);

	/** One placement by id; null when there is none. */
	const FSurfPlacement* FindPlacement(const FGuid& Id) const;

	/** The same, to change - marks the asset dirty for undo first. Call inside a transaction. */
	FSurfPlacement* ModifyPlacement(const FGuid& Id);

	/** Take a placement out. False when there is none by that id. */
	bool RemovePlacement(const FGuid& Id);

	int32 NumParts() const { return Parts.Num(); }

	/** Carry a part; its id is made here and returned. */
	FGuid AddPart(FSurfPart&& Part);
	/** A bake's mesh that got in as a part is dropped on load (S241a); how many. */
	int32 DropBakeParts();
	/** Patches with no surface, out (S245a): lines and points saved in by an extrude undone halfway. */
	int32 DropEmptyPatches();
	virtual void PostLoad() override;

	const FSurfPart* FindPart(const FGuid& Id) const;

	/** The same, to change - marks the asset dirty for undo first. Call inside a transaction. */
	FSurfPart* ModifyPart(const FGuid& Id);

	/** Stop carrying one. False when there is none by that id. */
	bool RemovePart(const FGuid& Id);

	/** The bounds of the geometry and every placement, in this prefab's space. */
	FBox LocalBounds() const;

private:
	FBox LocalBoundsWalk(TSet<const USurfPrefab*>& Seen) const;

public:

private:
	USurfChunk* GetChunkWithRoom();
	bool ContainsWalk(const USurfPrefab* Other, TSet<const USurfPrefab*>& Seen) const;
};
