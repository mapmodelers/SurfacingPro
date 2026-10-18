// Copyright MapModelers. All Rights Reserved.

#include "SurfOutliner.h"

#include "SurfSettings.h"
#include "SurfGroup.h"
#include "SurfacingProModule.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "SSceneOutliner.h"
#include "ISceneOutlinerTreeItem.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Widgets/SWidget.h"

namespace SurfOutliner
{
	namespace
	{
		FDelegateHandle MapOpenedHandle;

		/**
		 * Walk the widget tree looking for outliners.
		 *
		 * There is no registry of open outliners to ask, and the tab's content
		 * is a stack of wrappers rather than the outliner itself, so the tree
		 * is what there is. Matching on the type name keeps this honest: a
		 * blind cast down a wrapper would be a crash waiting for a layout
		 * change.
		 */
		void Gather(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SSceneOutliner>>& Out)
		{
			static const FName OutlinerType(TEXT("SSceneOutliner"));

			if (Widget->GetType() == OutlinerType)
			{
				Out.Add(StaticCastSharedRef<SSceneOutliner>(Widget));
				return;
			}

			FChildren* Children = Widget->GetChildren();
			if (!Children)
			{
				return;
			}

			for (int32 I = 0; I < Children->Num(); ++I)
			{
				Gather(Children->GetChildAt(I), Out);
			}
		}
	}

	int32 CollapseAll()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return 0;
		}

		TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetTopLevelWindows();

		TArray<TSharedRef<SSceneOutliner>> Found;
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			Gather(Window, Found);
		}

		for (const TSharedRef<SSceneOutliner>& Outliner : Found)
		{
			Outliner->CollapseAll();
		}

		return Found.Num();
	}

	int32 Tidy()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return 0;
		}

		TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetTopLevelWindows();

		TArray<TSharedRef<SSceneOutliner>> Found;
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			Gather(Window, Found);
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

		// The level being worked in, which in a partitioned map is the
		// persistent one - there is no other. One level open is what "tidy"
		// should mean; everything shut hides the thing you came for.
		ULevel* Current = World ? World->GetCurrentLevel() : nullptr;
		if (!Current && World)
		{
			Current = World->PersistentLevel;
		}

		/**
		 * Collapsed, with the top row open (S108b).
		 *
		 * "i need it collapsed BUT the first asset of outliner (level itself)
		 * is open." It was written that way and it was not happening: the row
		 * was looked up by the current ULevel alone, and in a partitioned map
		 * the row at the top is the **world**, not a level, so the lookup
		 * found nothing and said nothing. **A best-effort that cannot report
		 * failing is a best-effort that never gets fixed.**
		 *
		 * Every identity the top row might have is tried now, and the count of
		 * outliners that actually opened one is what comes back - so the
		 * retry above can tell the difference between "done" and "not yet".
		 */
		int32 Opened = 0;
		for (const TSharedRef<SSceneOutliner>& Outliner : Found)
		{
			Outliner->CollapseAll();

			// Opened one deep, not recursively: the point is to see what the
			// level holds, not to be back where we started.
			bool bAny = false;
			auto Open = [&](const FSceneOutlinerTreeItemID& Id)
			{
				if (bAny)
				{
					return;
				}
				if (FSceneOutlinerTreeItemPtr Item = Outliner->GetTreeItem(Id))
				{
					Outliner->SetItemExpansion(Item, true);
					bAny = true;
				}
			};

			if (Current)
			{
				Open(FSceneOutlinerTreeItemID(Current));
			}
			/**
			 * And the folder our own actors are in (S126a). Collapsing
			 * everything and then hiding his geometry inside a shut folder is
			 * a tidy outliner with the one thing he wants out of view.
			 */
			if (Current)
			{
				const FFolder Ours(FFolder::GetWorldRootFolder(World).GetRootObject(), ASurfGroup::OutlinerFolder);
				if (FSceneOutlinerTreeItemPtr Folder = Outliner->GetTreeItem(FSceneOutlinerTreeItemID(Ours)))
				{
					Outliner->SetItemExpansion(Folder, true);
				}
			}

			if (World)
			{
				Open(FSceneOutlinerTreeItemID(World));
				ULevel* Persistent = World->PersistentLevel;
				if (Persistent && Persistent != Current)
				{
					Open(FSceneOutlinerTreeItemID(Persistent));
				}
			}
			Opened += bAny ? 1 : 0;
		}

		return Opened;
	}

	void RegisterMapOpenHook()
	{
		if (MapOpenedHandle.IsValid())
		{
			return;
		}

		MapOpenedHandle = FEditorDelegates::OnMapOpened.AddLambda(
			[](const FString&, bool)
			{
				const USurfSettings* Settings = GetDefault<USurfSettings>();
				if (!Settings || !Settings->bCollapseOutlinerOnMapOpen)
				{
					return;
				}

				// Not this frame: the outliner repopulates after the map is
				// reported open, and collapsing a tree that is about to be
				// rebuilt collapses nothing.
				/**
				 * Not this frame, and not only once (S108b): the outliner
				 * repopulates after the map is reported open, and how long
				 * that takes depends on the map. Tidy answers how many
				 * outliners it actually opened the top row of, so this keeps
				 * asking until one says yes, or until it has asked eight
				 * times over three seconds and can stop pretending.
				 */
				TSharedRef<int32> Tries = MakeShared<int32>(0);
				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([Tries](float) -> bool
					{
						const int32 Opened = Tidy();
						++(*Tries);
						if (Opened > 0 || *Tries >= 8)
						{
							UE_LOG(LogSurfacingPro, Verbose,
								TEXT("Tidied the outliner after map open: %d opened, %d tries."), Opened, *Tries);
							return false;
						}
						return true;
					}),
					0.4f);
			});
	}

	void UnregisterMapOpenHook()
	{
		if (MapOpenedHandle.IsValid())
		{
			FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
			MapOpenedHandle.Reset();
		}
	}
}
