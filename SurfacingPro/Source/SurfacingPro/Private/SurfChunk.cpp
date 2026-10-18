// Copyright MapModelers. All Rights Reserved.

#include "SurfChunk.h"

#include "SurfGroup.h"

USurfChunk::USurfChunk()
{
	// The reason this class exists: it is the object Modify() transacts, and
	// its size is what one undo step costs.
	SetFlags(RF_Transactional);
}

TFunction<void(USurfChunk&)> USurfChunk::SelectionSnapshotProvider;

#if WITH_EDITOR
bool USurfChunk::Modify(bool bAlwaysMarkDirty)
{
	// Only while a transaction is open: the snapshot goes into the transaction
	// with the rest of the chunk, and outside one there is nothing to put it
	// in. Recorded before Super so the state the transaction saves is the
	// state before the edit - selection included.
	if (SelectionSnapshotProvider && GUndo)
	{
		SelectionSnapshotProvider(*this);
	}

	// Every edit passes through here (D21), which makes it the one place a
	// baked group can learn that its bake is now stale.
	if (ASurfGroup* Group = GetTypedOuter<ASurfGroup>())
	{
		Group->NoteChunkEdited();
	}

	return Super::Modify(bAlwaysMarkDirty);
}
#endif

#if WITH_EDITOR
void USurfChunk::PostEditUndo()
{
	Super::PostEditUndo();
	bSelectionRestorePending = true;
}
#endif
