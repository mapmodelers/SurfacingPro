// Copyright MapModelers. All Rights Reserved.
//
// One placement of a prefab in a map (S65a, D72): a light actor that points
// at the asset and draws its geometry - the asset's own and its placements'
// recursively - at this actor's transform. It picks, moves, duplicates and
// streams as any actor does, and a level designer who wants a light or some
// logic beside the geometry wraps it in a Blueprint.
//
// The geometry is edited in the asset, not here (drill-in, S65b); every
// instance shows the change when it lands.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.generated.h"

class UDynamicMeshComponent;
class UMaterialInterface;
class USurfPrefab;

UCLASS(BlueprintType, Blueprintable, hidecategories = (Input, Rendering, Replication, Collision, HLOD, Physics, Networking, Cooking, LevelInstance))
class SURFACINGPRO_API ASurfPrefabInstance : public AActor
{
	GENERATED_BODY()

public:
	ASurfPrefabInstance();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SurfacingPro", meta = (ToolTip = "The prefab this instance places."))
	TObjectPtr<USurfPrefab> Prefab;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro", meta = (ToolTip = "What the editor draws for it."))
	TObjectPtr<UDynamicMeshComponent> Preview;

	/** One slot per material the geometry wears, the fallback first. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> PreviewMaterials;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "Triangles drawn."))
	int32 TriangleCount = 0;

	/** Baked (S66a): the asset's static mesh stands in, attached, the preview hidden. Exit from a drill-in un-bakes. */
	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Bake", meta = (ToolTip = "Baked: the game draws a static mesh in its place."))
	bool bBaked = false;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Bake", meta = (ToolTip = "The baked mesh standing in for it."))
	TObjectPtr<AActor> BakedActor;

	/**
	 * Flags for this placement as a whole (S201a): "override should override
	 * all else" - on, OverrideFlags (ESurfPrimitiveFlags bits) are what its
	 * bake wears, whatever its pieces say. To differ piece by piece, save it
	 * as a new prefab or unpack it.
	 */
	UPROPERTY(EditAnywhere, Category = "SurfacingPro|Flags", meta = (ToolTip = "Its own flags over every piece inside it, for the bake."))
	bool bOverrideFlags = false;

	UPROPERTY(EditAnywhere, Category = "SurfacingPro|Flags", meta = (ToolTip = "The flags given to every piece inside it while it overrides."))
	int32 OverrideFlags = 0;

	/**
	 * Its own paint (S251j): the brush paints this placed prefab where it
	 * stands, starting from the prefab's paint; the asset and the other
	 * instances keep theirs. Off, it shows the prefab's paint again.
	 */
	UPROPERTY(EditAnywhere, Category = "SurfacingPro|Flags", meta = (ToolTip = "Paint this placed prefab on its own, from the prefab's paint; off, the prefab's paint again."))
	bool bOverridePaint = false;

	/** Its own paint, per patch of the prefab's, while bOverridePaint is on. */
	UPROPERTY()
	TArray<FSurfPaintOverride> PaintOverrides;

	/** The colours of Patch (one of the prefab's) as this instance shows them. */
	void ApplyPaintOverride(FSurfPatch& Patch) const;
	/** Keep Patch's colours as this instance's own. */
	void StorePaintOverride(const FSurfPatch& Patch);

	void Unbake();

	/** Whether the bake is actually standing there: the actor alive, with a mesh in it (S73a). */
	bool BakeIsShowing() const;

	/** The bake is gone: the flag dropped and the preview shown. */
	void DropStaleBake();

	/** The bake is the game's copy (S153a): the preview drawn in the editor, the stand-in in the game. */
	void ApplyBakeLook();

	/**
	 * Nothing of its own on screen - baked, empty, or its asset lost its
	 * geometry. Such an instance is still there and must still be clickable
	 * and visible: the mode draws it as a grey box and picks it by that box.
	 */
	bool DrawsNothing() const;

	/** Its box in the world: the asset's bounds, else what the preview holds, else a hand-sized box at its origin. */
	FBox WorldBounds() const;

	/** Build the preview from the asset. Safe to call with no asset: an empty mesh. */
	UFUNCTION(CallInEditor, Category = "SurfacingPro")
	void RebuildPreview();

	/** Only the materials again - the view's override, the ghost - over the mesh already built (S175a). */
	void RefreshPreviewMaterial();

	/**
	 * Whether what it wears is not what the view asks for now (S206a):
	 * "prefabs and active scene view mode isnt in sync. when switching
	 * matcap, theyre treated separately" - the mode looks and refreshes.
	 */
	bool PreviewLookStale() const;

private:
	/** The override and fallback the preview was last dressed with. */
	TWeakObjectPtr<UMaterialInterface> WornOverride;
	TWeakObjectPtr<UMaterialInterface> WornFallback;
	bool bWornOnce = false;
	UMaterialInterface* WantedOverride() const;

public:

	/**
	 * While this instance is being edited (drill-in, S65b) its own geometry
	 * is out in an editing group and only its placements are drawn here, so
	 * the two are never on top of each other. Not saved.
	 */
	UPROPERTY(Transient)
	bool bDrawOwnGeometry = true;

	/** Ghosted (S71a): every slot wears the group ghost while another prefab is edited in place. Not saved. */
	UPROPERTY(Transient)
	bool bGhosted = false;

	/**
	 * The parts the prefab carries, as components of this instance (S76a):
	 * a mesh as a mesh component, a class as a child actor. Made from the
	 * asset on every rebuild and never saved - the asset is the truth.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class USceneComponent>> PartComponents;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "How many parts it carries."))
	int32 PartCount = 0;

	/** Build the part components from the asset, its placements' included. */
	void RebuildParts();

	/** Where each of the asset's own placements sits in the preview mesh (S71b), from the last rebuild. */
	TArray<FSurfPlacementRange> PlacementRanges;

	/** The placement whose triangles these are, or an invalid id. */
	FGuid PlacementAtTriangle(int32 Triangle) const;

	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostActorCreated() override;
#endif
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PostEditUndo() override;
#endif
};
