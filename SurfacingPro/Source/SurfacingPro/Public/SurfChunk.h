// Copyright MapModelers. All Rights Reserved.
//
// Storage granularity for undo (D21).
//
// UE serializes a whole UObject on Modify(), so holding every primitive in
// one actor made the cost of a single edit scale with the size of the level:
// 1 MB and 79 ms at a thousand primitives, and by measurement 16 MB and well
// over a second at fifteen thousand. Splitting storage into small chunks
// bounds that cost to the chunk, which is the whole point - a chunk is the
// unit of transaction, nothing else.
//
// Tools never see chunks. ASurfGroup's accessors hide them completely.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"
#include "SurfChunk.generated.h"

/**
 * A bounded bag of primitives. Transactional by design: this is the object
 * Modify() is called on, and its size is what one undo step costs.
 */
UCLASS()
class SURFACINGPRO_API USurfChunk : public UObject
{
	GENERATED_BODY()

public:
	USurfChunk();

	UPROPERTY()
	TArray<FSurfBrush> Brushes;

	UPROPERTY()
	TArray<FSurfPatch> Patches;

	int32 Num() const { return Brushes.Num() + Patches.Num(); }

	bool IsEmpty() const { return Num() == 0; }

	FSurfBrush* FindBrush(const FGuid& Id)
	{
		return Brushes.FindByPredicate([&Id](const FSurfBrush& B) { return B.Id == Id; });
	}

	FSurfPatch* FindPatch(const FGuid& Id)
	{
		return Patches.FindByPredicate([&Id](const FSurfPatch& P) { return P.Id == Id; });
	}

	// --- Selection through undo ------------------------------------------------

	/**
	 * What was selected on this chunk's patches just before its last edit.
	 * Written by the editor from Modify(), restored with the rest of the chunk
	 * by undo, and read back by the editor afterwards. Empty outside the
	 * editor; the runtime never looks at it.
	 */
	UPROPERTY()
	FSurfSelectionSnapshot SavedSelection;

	/**
	 * Set when undo or redo has just restored this chunk, cleared by the
	 * editor once it has read SavedSelection. Not saved, not transacted.
	 */
	bool bSelectionRestorePending = false;

	/**
	 * Bound by the editor while its mode is active: fills SavedSelection for
	 * the chunk about to be edited. Unbound, Modify() records nothing.
	 */
	static TFunction<void(USurfChunk&)> SelectionSnapshotProvider;

	/**
	 * Editor only, both of them (S150e): outside the editor UObject::Modify
	 * is a non-virtual stub and PostEditUndo does not exist, so neither is
	 * anything to override. The first cook of the project found this - the
	 * runtime module had never been compiled as a game target.
	 */
#if WITH_EDITOR
	virtual bool Modify(bool bAlwaysMarkDirty = true) override;
	virtual void PostEditUndo() override;
#endif
};
