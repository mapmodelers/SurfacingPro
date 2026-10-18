// Copyright MapModelers. All Rights Reserved.
//
// The group actor: the unit that holds editable geometry and renders it.
//
// Brushes and patches are level-resident data inside this actor - there is no
// asset per primitive, and saving the level saves the geometry. The preview
// component is a build product, rebuilt from the source data, never the other
// way round.
//
// Storage is chunked (D21) so that the cost of one undo step is bounded by
// the chunk rather than by the size of the level. Nothing outside this class
// needs to know that: use the accessors, and call ModifyBrush / ModifyPatch
// inside a transaction before mutating a primitive.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SurfTypes.h"
#include "SurfGroup.generated.h"

class UDynamicMeshComponent;
class UMaterialInterface;
class USurfChunk;
class USurfPrefab;
struct FSurfMaterialSet;
namespace UE { namespace Geometry { class FDynamicMesh3; } }

UCLASS(BlueprintType)
class SURFACINGPRO_API ASurfGroup : public AActor
{
	GENERATED_BODY()

public:
	ASurfGroup();

	/** Rebuilt from the chunks; never edited directly. */
	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro", meta = (ToolTip = "What the editor draws of the geometry."))
	TObjectPtr<UDynamicMeshComponent> Preview;

	/**
	 * Storage. Transactional only as a pointer array, so adding or removing a
	 * chunk undoes correctly while a primitive edit never touches the actor.
	 */
	/**
	 * Instanced, so a copied or duplicated group gets chunks of its own. Without
	 * it a paste shared the source's chunks: editing the copy edited the
	 * original, and the two could not be told apart.
	 */
	UPROPERTY(Instanced)
	TArray<TObjectPtr<USurfChunk>> Chunks;

	UPROPERTY()
	FGuid GroupId;

	/**
	 * Primitives per chunk. Smaller means cheaper undo and more UObjects;
	 * the measurement in SurfacingPro.Perf.UndoSnapshot is what should move
	 * this number, not intuition.
	 */
	UPROPERTY(EditAnywhere, Category = "SurfacingPro|Advanced", meta = (ToolTip = "How many pieces are stored together; leave as it is.", ClampMin = "1", ClampMax = "512"))
	int32 ChunkCapacity = 32;

	/**
	 * Material for every primitive that wears none of its own. Leave it empty
	 * and the editor supplies its checker, so UVs are readable the moment a
	 * group exists.
	 */
	UPROPERTY(EditAnywhere, Category = "SurfacingPro", meta = (ToolTip = "The material the geometry wears where a piece has none of its own."))
	TObjectPtr<UMaterialInterface> PreviewMaterial;

	/**
	 * The slots the preview wears since its last rebuild: slot 0 the fallback
	 * (resolved again on every refresh), then one per distinct material the
	 * primitives wear, in the order first met. A build product, never saved.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> PreviewMaterials;

	/**
	 * What to use when PreviewMaterial is empty. The editor module binds this
	 * at start-up; the runtime module must not depend on the editor, and a
	 * default material is not worth inverting that for.
	 */
	static TDelegate<UMaterialInterface*()> DefaultMaterialProvider;

	/** PreviewMaterial, or the editor's default, or null. */
	UMaterialInterface* GetEffectiveMaterial() const;

	/**
	 * A material for every slot at once, while it is bound and answers: the
	 * paint view, which shows the channel being painted on every surface
	 * for as long as the brush is out (S69a). The editor binds and clears
	 * it; the runtime module knows nothing of what it draws.
	 */
	static TDelegate<UMaterialInterface*()> PreviewOverrideProvider;

	/**
	 * The ghost: what a group wears while a prefab is being edited in place
	 * around it (S71a) - there, but out of reach. The editor binds it.
	 */
	static TDelegate<UMaterialInterface*()> GhostMaterialProvider;

	/** Ghosted: every slot wears the ghost. Not saved; the mode sets and clears it. */
	bool bGhosted = false;

	/** The outliner folder everything of ours stands in, so a level's own actors are not mixed with it (S72d). */
	/**
	 * Where our actors live in the outliner (S126a): "Fix our file hierarchy
	 * so we may rely on unreal outliner window for our tool."
	 *
	 * One root, two rooms under it - the geometry he is building, and the
	 * prefabs standing in it. Set when an actor is made **and** when a level
	 * that predates this is loaded, because a folder only new actors get is a
	 * folder half his level is missing from.
	 */
	static const FName OutlinerFolder;
	/** The placed assets' drawer (S220a): prefab instances, placed meshes, placed levels. */
	static const FName OutlinerPrefabFolder;

	/** Put this actor where it belongs in the outliner. Safe to call twice. */
	void TidyOutlinerFolder();

	/** Hand the preview its slots again (slot 0 re-resolved), and whether it casts a shadow: the default checker, the group material or the shade setting changed. */
	void RefreshPreviewMaterial();

	/**
	 * Baked (S66a): the static meshes stand in for the preview, attached so
	 * they move with the group, and the preview is hidden. An edit of any
	 * chunk un-bakes, since the bake would be stale; Bake again when done.
	 */
	/**
	 * The one group a level makes for itself (S149b).
	 *
	 * "for some reasons i see two surfgroup in outliner, two tags
	 * Lvl_FIrstPerson in the level viewport. Cant there be only one?"
	 *
	 * S140a stopped new geometry from walking the world for a group to join
	 * - rightly, because the first actor the iterator hands back is chosen
	 * by nothing the hand did - and remembered the last one used instead.
	 * But that memory lives on the mode, so it is gone by the next editor
	 * run, and the next piece made starts another group: one a session,
	 * every one of them labelled with the level's name.
	 *
	 * A mark is the difference between a group the tool made for this level
	 * and one that happens to stand in it. It is set when the tool spawns a
	 * group for the level, never on a prefab's room, and dropped by a copy
	 * (a duplicate is a piece of geometry, not a second home for it).
	 */
	UPROPERTY()
	bool bLevelGroup = false;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Bake", meta = (ToolTip = "Baked: the game draws static meshes in its place."))
	bool bBaked = false;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Bake", meta = (ToolTip = "The baked meshes standing in for it."))
	TArray<TObjectPtr<AActor>> BakedActors;

	/** Back to the preview: the baked actors removed, the preview shown. The assets stay. */
	void Unbake();

	/**
	 * Whether the bake is actually standing there: an actor of it alive,
	 * with a mesh in it. A level saved after a bake whose mesh assets never
	 * reached disk came back with the preview hidden and nothing in its
	 * place - wires and no surface ("i still have wire only patches", S73a).
	 */
	bool BakeIsShowing() const;

	/** The bake is gone: the flag dropped and the preview shown, the actors left for Unbake to clear. */
	void DropStaleBake();

	/**
	 * The bake is the game's copy (S153a): the editor draws the preview and
	 * never the stand-ins, the game runs the stand-ins and never the
	 * preview. Applied whenever either side changes - a bake, an unbake, a
	 * rebuild, a load. "Couldnt the Bake part be invisible to user?"
	 */
	void ApplyBakeLook();

	/**
	 * Which side the editor shows (S154e): the previews, or the bake as
	 * the game would. Game View sets it; the mode leaving puts it back.
	 * Not saved - a level always opens showing its previews.
	 */
	static bool bShowBakeInEditor;

	/** Something of ours was edited while the game's view is shown (S200a): the mode goes back to the editor's view. */
	static FSimpleMulticastDelegate OnEditedInGameView;

	/**
	 * Set by the mode while it is up (S183a): "surfacing related assets
	 * shouldn't be editable (transform) when not in surfacing mode" - a
	 * group stands still under the editor's own gizmo outside the mode.
	 */
	static bool bModeUp;
#if WITH_EDITOR
	virtual bool IsLockLocation() const override { return !bModeUp || Super::IsLockLocation(); }
#endif

	/** Called by a chunk as it is edited: a baked group un-bakes. */
	void NoteChunkEdited();

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "Primitives held."))
	int32 PrimitiveCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "Triangles drawn."))
	int32 TriangleCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "Storage chunks."))
	int32 ChunkCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "SurfacingPro|Stats", meta = (ToolTip = "How long the last rebuild of the drawing took, in milliseconds."))
	float LastRebuildMs = 0.0f;

	// --- Test helpers: called by the tests and the code, not buttons in the
	// Details panel any more (S250c) - Clear All sat there in the shipped tool.

	UFUNCTION(Category = "SurfacingPro|Build")
	void AddBox();

	UFUNCTION(Category = "SurfacingPro|Build")
	void AddCurve();

	UFUNCTION(Category = "SurfacingPro|Build")
	void AddSheet();

	UFUNCTION(Category = "SurfacingPro|Build")
	void ClearAll();

	UFUNCTION(Category = "SurfacingPro|Build")
	void Rebuild();

	UFUNCTION(Category = "SurfacingPro|Bench")
	void StressTest();

	UFUNCTION(Category = "SurfacingPro|Bench")
	void MeasureEditCost();

	UPROPERTY(meta = (ClampMin = "1", ClampMax = "50000"))
	int32 StressCount = 1000;

	UPROPERTY(meta = (ClampMin = "1", ClampMax = "500"))
	int32 EditIterations = 60;

	// --- Storage access -----------------------------------------------------

	int32 NumBrushes() const;
	int32 NumPatches() const;
	int32 NumPrimitives() const { return NumBrushes() + NumPatches(); }

	/** Every primitive that is shown (not hidden with H, S184c). */
	void ForEachBrush(TFunctionRef<void(const FSurfBrush&)> Visit) const;
	void ForEachBrushIncludingHidden(TFunctionRef<void(const FSurfBrush&)> Visit) const;

	/** Every patch that is shown: not hidden (H), not set aside by the isolate view. Drawing, picking and the preview go through this. */
	void ForEachPatch(TFunctionRef<void(const FSurfPatch&)> Visit) const;

	/** Every patch, the hidden ones too - for unhiding and counting. */
	void ForEachPatchIncludingHidden(TFunctionRef<void(const FSurfPatch&)> Visit) const;

	/** Patches hidden by H, not by the isolate view. */
	int32 NumHiddenPatches() const;

	/**
	 * Patches set aside by the isolate view for the moment: ForEachPatch
	 * skips them like the hidden ones. Not saved, not undone - a view, not
	 * an edit; the mode clears it when the view ends.
	 */
	TSet<FGuid> IsolationHidden;

	/** Read-only lookup. Null when the id is not in this group. */
	const FSurfBrush* FindBrush(const FGuid& Id) const;
	const FSurfPatch* FindPatch(const FGuid& Id) const;

	/**
	 * Mutable lookup that first marks the owning chunk dirty, so the edit is
	 * undoable at chunk cost. Call inside a transaction; this is the only
	 * supported way to change a primitive.
	 */
	FSurfBrush* ModifyBrush(const FGuid& Id);
	FSurfPatch* ModifyPatch(const FGuid& Id);

	/** Positional access for benchmarks and tests. Also marks the chunk dirty. */
	FSurfBrush* ModifyBrushAt(int32 LinearIndex);

	FSurfBrush& AddBrush(FSurfBrush&& Brush);
	FSurfPatch& AddPatch(FSurfPatch&& Patch);

	/** Remove a brush by id. Marks the owning chunk; drops the chunk when it empties. */
	bool RemoveBrush(const FGuid& Id);
	bool RemovePatch(const FGuid& Id);
	/** Patches with no surface, out (S245a): lines and points left by an extrude undone halfway. */
	int32 DropEmptyPatches();

	void ClearPrimitives();

	/** The chunk holding this id, or null. Exposed for measurement only. */
	USurfChunk* FindChunkForBrush(const FGuid& Id) const;

	// --- Build --------------------------------------------------------------

	/** Rebuild the preview from source. Returns milliseconds taken. */
	double RebuildPreview();

	/**
	 * Append a prefab's geometry - its own and its placements', recursively -
	 * into a mesh, each primitive carried through Local (S65a). The same
	 * tessellation and the same overlays as the group's own preview, so a
	 * prefab looks exactly as its patches did before they became one. Sets
	 * the mesh's attributes up if it has none. Shared by the prefab instance
	 * and, later, the bake.
	 */
	static void AppendPrefabToMesh(UE::Geometry::FDynamicMesh3& Mesh, const USurfPrefab& Prefab, const FTransform& Local,
		FSurfMaterialSet& Materials, double LayerOffsetCm, bool bIncludeOwnGeometry = true, TArray<struct FSurfPlacementRange>* OutRanges = nullptr,
		const TArray<struct FSurfPaintOverride>* Paint = nullptr);

	/** Give the preview collision, or take it away; the collision mesh is built here when turned on. */
	void SetPreviewCollision(bool bOn);

	/**
	 * Whether the preview should collide where it is: in play the geometry
	 * is walked on (CollisionWhilePlaying); in the editor only when asked
	 * for (PreviewCollision), because the mesh is rebuilt on every drag.
	 */
	bool WantsCollision() const;

	/**
	 * New ids for the group and every primitive in it, so a group made by
	 * paste or duplicate shares no identity with the one it came from.
	 */
	void RefreshIdentity();

	/** Bumped by every rebuild, so anything derived from the preview mesh knows it is stale. */
	int32 RebuildSerial = 0;

	/** The triangle group each patch's triangles carry in the preview mesh, from the last rebuild: 1 upward. */
	TMap<FGuid, int32> PatchTriangleGroups;

	/**
	 * And each brush's, from BrushTriangleGroupBase upward (S151a) - so the
	 * bake can ask which piece a triangle came from and what flags it wears.
	 * Brushes were all group 0 before, which told nothing apart.
	 */
	static constexpr int32 BrushTriangleGroupBase = 1000000;
	TMap<FGuid, int32> BrushTriangleGroups;

	/**
	 * The flags the piece behind a triangle group wears (S151a): Detail,
	 * NonColliding, NoShadow, NoDraw, as ESurfPrimitiveFlags bits. Zero for a
	 * group nothing answers to.
	 */
	int32 FlagsOfTriangleGroup(int32 TriangleGroup) const;

	/**
	 * The last build's shape, for the incremental rebuild (S66b): the
	 * patches in their order with a fingerprint each, the brushes' one, the
	 * material slots and the layer offset. A rebuild that finds the same
	 * shape with a few fingerprints changed replaces those patches'
	 * triangles in the mesh in hand rather than tessellating everything.
	 */
	TArray<FGuid> BuiltPatchOrder;
	TMap<FGuid, uint32> BuiltPatchFingerprints;
	uint32 BuiltBrushFingerprint = 0;
	TArray<TWeakObjectPtr<UMaterialInterface>> BuiltSlots;
	double BuiltLayerOffsetCm = 0.0;
	bool bPreviewBuilt = false;

	/** What the last rebuild did: -1 everything, else how many patches were replaced in place. */
	int32 LastRebuildReplaced = -1;

	/** Everything, whatever the fingerprints say. The next rebuild is a full one. */
	void ForceFullRebuild() { bPreviewBuilt = false; }

	/** Everything that decides what a patch or the brushes look like, folded into a number. */
	static uint32 FingerprintOf(const FSurfPatch& Patch);
	uint32 FingerprintOfBrushes() const;

private:
	bool TryIncrementalRebuild(double Start);
public:

	/** Append a box brush in local space, axially projected. */
	FSurfBrush& AppendBox(const FBox& LocalBox);

	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostActorCreated() override;
#endif
	virtual void PostRegisterAllComponents() override;
#if WITH_EDITOR
	virtual void PostEditUndo() override;
#endif
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;

#if WITH_EDITOR
	virtual void PostEditImport() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** A chunk with room, creating one when needed. Marks the actor dirty only when a chunk is added. */
	USurfChunk* GetChunkWithRoom();
};
