// Copyright MapModelers. All Rights Reserved.
//
// Hide, show and isolate (S58): Blender's H, Shift+H, Alt+H and local view.
//
// A hidden patch is drawn, picked and previewed as if it were not there -
// ASurfGroup::ForEachPatch skips it, and everything that draws or picks goes
// through ForEachPatch - and it is saved with the patch and undone with the
// edit, as Blender's hide is. Isolation is a view, not an edit: the other
// patches are set aside on their groups for the moment (IsolationHidden,
// neither saved nor undone), the other actors are hidden the editor's
// temporary way, the selection is framed, and pressing again puts it all
// back, the view included.

#include "SurfEdMode.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfGroup.h"
#include "SurfPrefabInstance.h"
#include "SurfSettings.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Brush.h"
#include "Engine/Light.h"
#include "GameFramework/Volume.h"
#include "GameFramework/WorldSettings.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

namespace
{
	/** An actor the isolate view sets aside: something to see, not a light, a volume, the sky or the level itself. */
	bool IsolateHides(const AActor* Actor)
	{
		// A level placed in the level is a container (S170a): hiding it hides
		// everything inside, our groups included - "isolate shows only in
		// wireframe" was a group inside a level instance, its surface gone
		// with the instance while the mode still drew its wire.
		if (!Actor || Actor->IsA<ASurfGroup>() || Actor->IsA<AWorldSettings>() || Actor->IsA<ABrush>() || Actor->IsA<ALevelInstance>()
			|| Actor->IsA<ALight>() || Actor->IsA<AVolume>() || Actor->IsTemporarilyHiddenInEditor() || Actor->IsHiddenEd())
		{
			return false;
		}

		// The sky, the fog, the atmosphere: the world, in Blender's terms, which its local view keeps.
		const FString ClassName = Actor->GetClass()->GetName();
		const FString ActorName = Actor->GetName();
		if (ClassName.Contains(TEXT("Sky")) || ClassName.Contains(TEXT("Fog")) || ClassName.Contains(TEXT("Atmosphere")) || ClassName.Contains(TEXT("Cloud"))
			|| ActorName.Contains(TEXT("Sky")) || ActorName.Contains(TEXT("Fog")))
		{
			return false;
		}

		// Something to see: a primitive that is not the editor's own.
		bool bSeen = false;
		Actor->ForEachComponent<UPrimitiveComponent>(false, [&bSeen](const UPrimitiveComponent* Component)
		{
			if (Component && !Component->IsEditorOnly() && Component->IsVisible())
			{
				bSeen = true;
			}
		});
		return bSeen;
	}
}

TArray<FSurfPatchRef> USurfEdMode::PatchesHeldBySelection() const
{
	// Object sub-mode: the patches picked. Otherwise the patches the points
	// and faces sit on; with none, the active patch - the thing being worked
	// on, as Delete reads it.
	if (SubMode == ESurfSubMode::Object)
	{
		return GetSelectedPatches();
	}

	TArray<FSurfPatchRef> Held;
	auto Add = [&Held](const TWeakObjectPtr<ASurfGroup>& Group, const FGuid& PatchId)
	{
		if (!Group.IsValid() || !PatchId.IsValid())
		{
			return;
		}
		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = PatchId;
		Held.AddUnique(Ref);
	};
	for (const FSurfPointRef& Point : PointSelection)
	{
		Add(Point.Group, Point.PatchId);
	}
	for (const FSurfQuadRef& Quad : QuadSelection)
	{
		Add(Quad.Group, Quad.PatchId);
	}
	if (Held.Num() == 0 && SelectedPatch.IsValid())
	{
		Add(SelectedPatch.Group, SelectedPatch.PatchId);
	}
	return Held;
}

bool USurfEdMode::HidePatches(bool bUnselected)
{
	const TArray<FSurfPatchRef> Held = PatchesHeldBySelection();
	TArray<FSurfPatchRef> ToHide;
	if (bUnselected)
	{
		// Every patch shown that the selection does not hold.
		for (const FSurfPatchRef& Each : GetAllPatches())
		{
			if (!Held.Contains(Each))
			{
				ToHide.Add(Each);
			}
		}
	}
	else
	{
		ToHide = Held;
	}

	// The primitives too (S184c): the picked ones, or every shown one not picked.
	TArray<TPair<ASurfGroup*, FGuid>> BrushesToHide;
	{
		TArray<FSurfPatchRef> PickedPatches;
		TArray<FSurfFaceRef> PickedBrushes;
		PiecesPicked(PickedPatches, PickedBrushes);
		TSet<TPair<ASurfGroup*, FGuid>> Picked;
		for (const FSurfFaceRef& Each : PickedBrushes)
		{
			if (ASurfGroup* Group = Each.Group.Get())
			{
				Picked.Add(TPair<ASurfGroup*, FGuid>(Group, Each.BrushId));
			}
		}
		if (bUnselected)
		{
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<ASurfGroup> It(World); It; ++It)
				{
					ASurfGroup* Group = *It;
					Group->ForEachBrush([&](const FSurfBrush& Brush)
					{
						const TPair<ASurfGroup*, FGuid> Key(Group, Brush.Id);
						if (!Picked.Contains(Key))
						{
							BrushesToHide.Add(Key);
						}
					});
				}
			}
		}
		else
		{
			BrushesToHide = Picked.Array();
		}
	}

	if (ToHide.Num() == 0 && BrushesToHide.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(bUnselected
				? TEXT("hide: every patch shown is selected - nothing to hide")
				: TEXT("hide: nothing is selected"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(bUnselected ? TEXT("hide_unselected_nothing") : TEXT("hide_nothing_selected"));
		return false;
	}

	FScopedTransaction Transaction(bUnselected ? LOCTEXT("HideUnselected", "Hide Unselected Patches") : LOCTEXT("HidePatches", "Hide Patches"));

	TArray<ASurfGroup*> Touched;
	int32 Hidden = 0;
	for (const FSurfPatchRef& Each : ToHide)
	{
		ASurfGroup* Group = Each.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Each.PatchId) : nullptr;
		if (Patch && !Patch->bHidden)
		{
			Patch->bHidden = true;
			++Hidden;
			Touched.AddUnique(Group);
		}
	}
	for (const TPair<ASurfGroup*, FGuid>& Each : BrushesToHide)
	{
		FSurfBrush* Brush = Each.Key ? Each.Key->ModifyBrush(Each.Value) : nullptr;
		if (Brush && !Brush->bHidden)
		{
			Brush->bHidden = true;
			++Brush->EditSerial;
			++Hidden;
			Touched.AddUnique(Each.Key);
		}
	}

	if (Hidden == 0)
	{
		// Nothing changed: the record can go (D49).
		Transaction.Cancel();
		return false;
	}

	// A hidden patch leaves the selection, as in Blender: what cannot be
	// seen cannot be moved by accident.
	if (!bUnselected)
	{
		ClearPointSelection();
		SelectPatch(FSurfPatchRef());
		ClearSelection();
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	if (Inspector)
	{
		Inspector->SetResult(Hidden == 1
			? FString(TEXT("hidden: 1 piece - Alt+H shows everything again"))
			: FString::Printf(TEXT("hidden: %d pieces - Alt+H shows everything again"), Hidden));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(bUnselected ? TEXT("hide_unselected") : TEXT("hide"), FString::Printf(TEXT("patches=%d"), Hidden));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::UnhideAllPatches()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	TArray<FSurfPatchRef> Hidden;
	TArray<TPair<ASurfGroup*, FGuid>> HiddenBrushes;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}
		Group->ForEachPatchIncludingHidden([&](const FSurfPatch& Patch)
		{
			if (Patch.bHidden)
			{
				FSurfPatchRef Ref;
				Ref.Group = Group;
				Ref.PatchId = Patch.Id;
				Hidden.Add(Ref);
			}
		});
		Group->ForEachBrushIncludingHidden([&](const FSurfBrush& Brush)
		{
			if (Brush.bHidden)
			{
				HiddenBrushes.Add(TPair<ASurfGroup*, FGuid>(Group, Brush.Id));
			}
		});
	}

	if (Hidden.Num() == 0 && HiddenBrushes.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("unhide: nothing is hidden"));
			Inspector->RefreshSelectionCount();
		}
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("UnhidePatches", "Unhide Patches"));

	TArray<ASurfGroup*> Touched;
	for (const FSurfPatchRef& Each : Hidden)
	{
		ASurfGroup* Group = Each.Group.Get();
		if (FSurfPatch* Patch = Group ? Group->ModifyPatch(Each.PatchId) : nullptr)
		{
			Patch->bHidden = false;
			Touched.AddUnique(Group);
		}
	}
	for (const TPair<ASurfGroup*, FGuid>& Each : HiddenBrushes)
	{
		if (FSurfBrush* Brush = Each.Key->ModifyBrush(Each.Value))
		{
			Brush->bHidden = false;
			++Brush->EditSerial;
			Touched.AddUnique(Each.Key);
		}
	}
	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	// What comes back is selected, in object sub-mode, as Blender selects
	// what it reveals: the next key acts on it.
	if (SubMode == ESurfSubMode::Object && Hidden.Num() > 0)
	{
		ClearPointSelection();
		SelectPatch(Hidden[0]);
		for (int32 I = 1; I < Hidden.Num(); ++I)
		{
			AddPatchToSelection(Hidden[I], /*bToggle*/ false);
		}
	}

	if (Inspector)
	{
		const int32 Shown = Hidden.Num() + HiddenBrushes.Num();
		Inspector->SetResult(Shown == 1
			? FString(TEXT("shown again: 1 piece"))
			: FString::Printf(TEXT("shown again: %d pieces"), Shown));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("unhide"), FString::Printf(TEXT("patches=%d"), Hidden.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::ToggleIsolate(FEditorViewportClient* ViewportClient)
{
	if (bIsolating)
	{
		EndIsolate(/*bRestoreView*/ true);
		if (Inspector)
		{
			Inspector->SetResult(TEXT("isolate: off - everything is back"));
			Inspector->RefreshSelectionCount();
		}
		if (GEditor)
		{
			RedrawViewports();
		}
		return true;
	}

	const TArray<FSurfPatchRef> Held = PatchesHeldBySelection();
	UWorld* World = GetWorld();
	if (Held.Num() == 0 || !World)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("isolate: nothing is selected"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("isolate_nothing_selected"));
		return false;
	}

	// Every other patch of every group, set aside for the moment.
	int32 PatchesAside = 0;
	IsolationGroups.Reset();
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}
		Group->IsolationHidden.Reset();
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Patch.Id;
			if (!Held.Contains(Ref))
			{
				Group->IsolationHidden.Add(Patch.Id);
			}
		});
		if (Group->IsolationHidden.Num() > 0)
		{
			PatchesAside += Group->IsolationHidden.Num();
			Group->RebuildPreview();
			IsolationGroups.Add(Group);
		}
	}

	// Every other actor with something to see, the editor's temporary way:
	// not saved, back on the second press or on leaving the mode.
	IsolationHiddenActors.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (IsolateHides(Actor))
		{
			Actor->SetIsTemporarilyHiddenInEditor(true);
			IsolationHiddenActors.Add(Actor);
		}
	}

	// The view, to come back to.
	IsolateViewClient = ViewportClient;
	if (ViewportClient)
	{
		IsolateViewLocation = ViewportClient->GetViewLocation();
		IsolateViewRotation = ViewportClient->GetViewRotation();
	}

	bIsolating = true;
	if (ViewportClient)
	{
		FocusSelection(ViewportClient);
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("isolated: %d patch%s - the same key brings everything back"),
			Held.Num(), Held.Num() == 1 ? TEXT("") : TEXT("es")));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("isolate"), FString::Printf(TEXT("kept=%d patches_aside=%d actors_aside=%d"),
		Held.Num(), PatchesAside, IsolationHiddenActors.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

// --- The room (S71a) -------------------------------------------------------------
//
// A prefab being edited is the one thing open. In place, everything else of
// ours is ghosted - there to fit against, out of reach; alone, everything
// else is out of view, the isolate view's way, so the prefab stands at the
// origin with nothing round it.

void USurfEdMode::SetRoomAside(ASurfGroup* Room, ASurfPrefabInstance* Instance, bool bInPlace)
{
	ClearRoomAside();
	UWorld* World = GetWorld();
	if (!World || !Room)
	{
		return;
	}
	if (bIsolating)
	{
		// One view at a time; the room's takes over.
		EndIsolate(/*bRestoreView*/ false);
	}

	int32 Groups = 0;
	int32 Instances = 0;
	int32 Actors = 0;
	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || Group == Room)
		{
			continue;
		}
		if (bInPlace)
		{
			// Dressed below, as the context look says (S249a).
			RoomContext.Add(Group);
		}
		else if (!Group->IsTemporarilyHiddenInEditor())
		{
			Group->SetIsTemporarilyHiddenInEditor(true);
			RoomHiddenActors.Add(Group);
		}
		++Groups;
	}
	for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
	{
		ASurfPrefabInstance* Each = *It;
		if (!Each)
		{
			continue;
		}
		if (bInPlace)
		{
			// The instance being edited keeps drawing its placements as they
			// are; every other instance is context.
			if (Each == Instance)
			{
				continue;
			}
			RoomContext.Add(Each);
		}
		else if (!Each->IsTemporarilyHiddenInEditor())
		{
			Each->SetIsTemporarilyHiddenInEditor(true);
			RoomHiddenActors.Add(Each);
		}
		++Instances;
	}
	if (bInPlace)
	{
		ApplyContextLook();
	}
	else
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && Actor != Room && IsolateHides(Actor))
			{
				Actor->SetIsTemporarilyHiddenInEditor(true);
				RoomHiddenActors.Add(Actor);
				++Actors;
			}
		}
	}
	SurfSession::Verb(TEXT("room_aside"), FString::Printf(TEXT("in_place=%d groups=%d instances=%d actors=%d"), bInPlace ? 1 : 0, Groups, Instances, Actors));
}

void USurfEdMode::ApplyContextLook()
{
	if (RoomContext.Num() == 0)
	{
		return;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const ESurfContextLook Look = Settings ? Settings->ContextLook : ESurfContextLook::Ghost;
	const bool bGhost = Look == ESurfContextLook::Ghost;
	const bool bWire = Look == ESurfContextLook::Wire;
	for (const TWeakObjectPtr<AActor>& Weak : RoomContext)
	{
		AActor* Actor = Weak.Get();
		if (!Actor)
		{
			continue;
		}
		if (ASurfGroup* Group = Cast<ASurfGroup>(Actor))
		{
			if (Group->bGhosted != bGhost)
			{
				Group->bGhosted = bGhost;
				Group->RefreshPreviewMaterial();
			}
		}
		else if (ASurfPrefabInstance* Each = Cast<ASurfPrefabInstance>(Actor))
		{
			if (Each->bGhosted != bGhost)
			{
				Each->bGhosted = bGhost;
				Each->RebuildPreview();
			}
		}
		// Wires: out of view, its lines drawn instead. One the hand had
		// hidden stays hidden, and undrawn.
		const bool bWiredNow = RoomWiredHidden.Contains(Weak);
		if (bWire && !bWiredNow && !Actor->IsTemporarilyHiddenInEditor())
		{
			Actor->SetIsTemporarilyHiddenInEditor(true);
			RoomWiredHidden.Add(Weak);
		}
		else if (!bWire && bWiredNow)
		{
			Actor->SetIsTemporarilyHiddenInEditor(false);
			RoomWiredHidden.Remove(Weak);
		}
	}
	SurfSession::Verb(TEXT("context_look"), bGhost ? TEXT("ghost") : bWire ? TEXT("wire") : TEXT("regular"));
	RedrawViewports();
}

bool USurfEdMode::IsRoomContext(const AActor* Actor) const
{
	return Actor && RoomContext.ContainsByPredicate([Actor](const TWeakObjectPtr<AActor>& Weak) { return Weak.Get() == Actor; });
}

bool USurfEdMode::IsRoomContextWired(const AActor* Actor) const
{
	return Actor && RoomWiredHidden.ContainsByPredicate([Actor](const TWeakObjectPtr<AActor>& Weak) { return Weak.Get() == Actor; });
}

void USurfEdMode::ClearRoomAside()
{
	for (const TWeakObjectPtr<AActor>& Weak : RoomContext)
	{
		if (ASurfGroup* Group = Cast<ASurfGroup>(Weak.Get()))
		{
			if (Group->bGhosted)
			{
				Group->bGhosted = false;
				Group->RefreshPreviewMaterial();
			}
		}
		else if (ASurfPrefabInstance* Each = Cast<ASurfPrefabInstance>(Weak.Get()))
		{
			if (Each->bGhosted)
			{
				Each->bGhosted = false;
				Each->RebuildPreview();
			}
		}
	}
	for (const TWeakObjectPtr<AActor>& Weak : RoomWiredHidden)
	{
		if (AActor* Actor = Weak.Get())
		{
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}
	RoomContext.Reset();
	RoomWiredHidden.Reset();
	for (const TWeakObjectPtr<AActor>& Weak : RoomHiddenActors)
	{
		if (AActor* Actor = Weak.Get())
		{
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}
	RoomHiddenActors.Reset();
}

void USurfEdMode::EndIsolate(bool bRestoreView)
{
	if (!bIsolating)
	{
		return;
	}

	for (const TWeakObjectPtr<ASurfGroup>& Weak : IsolationGroups)
	{
		if (ASurfGroup* Group = Weak.Get())
		{
			Group->IsolationHidden.Reset();
			Group->RebuildPreview();
		}
	}
	IsolationGroups.Reset();

	for (const TWeakObjectPtr<AActor>& Weak : IsolationHiddenActors)
	{
		if (AActor* Actor = Weak.Get())
		{
			Actor->SetIsTemporarilyHiddenInEditor(false);
		}
	}
	IsolationHiddenActors.Reset();

	// The view as it was, if the viewport is still there.
	if (bRestoreView && IsolateViewClient && GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client == IsolateViewClient)
			{
				Client->SetViewLocation(IsolateViewLocation);
				Client->SetViewRotation(IsolateViewRotation);
				Client->Invalidate();
				break;
			}
		}
	}
	IsolateViewClient = nullptr;

	bIsolating = false;
	SurfSession::Verb(TEXT("isolate"), TEXT("off"));
}

#undef LOCTEXT_NAMESPACE
