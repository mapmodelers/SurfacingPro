// Copyright MapModelers. All Rights Reserved.

#include "SurfAutoBake.h"
#include "SurfBake.h"
#include "SurfGroup.h"
#include "SurfLevelBake.h"
#include "SurfLog.h"
#include "SurfPrefabInstance.h"
#include "SurfSession.h"
#include "SurfSettings.h"

#include "Editor.h"
#include "Containers/Ticker.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Misc/ScopedSlowTask.h"
#include "EditorModeManager.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"

namespace SurfAutoBake
{
	namespace
	{
		FDelegateHandle PlayHandle;
		FDelegateHandle SaveHandle;
		FTSTicker::FDelegateHandle GameViewHandle;
		bool bBusy = false;

		/**
		 * Bake what is not standing; answer how many meshes were made. The
		 * bake is skipped entirely when nothing needs it, so a level that is
		 * current costs nothing at Play.
		 */
		int32 BakeWhatIsStale(UWorld* World, const TCHAR* Why, const ULevel* OnlyLevel = nullptr)
		{
			// The level's own tick (S153b), for every group and instance the
			// world holds - sublevels and prefab scenes included.
			if (!World || !USurfLevelBake::IsOn(World) || bBusy || World->WorldType != EWorldType::Editor)
			{
				return 0;
			}
			TGuardValue<bool> Guard(bBusy, true);
			TSharedRef<SurfBake::FJob> Plan = SurfBake::PlanLevelBake(World, /*bSkipStanding*/ true, OnlyLevel);
			if (Plan->Total() == 0)
			{
				return 0;
			}
			// Seen while it runs (S154a): "I need to know when bake's in
			// progress" - the editor's own progress window, and a line
			// after, in the corner and in Notes.
			FScopedSlowTask Task(1.0f, FText::Format(NSLOCTEXT("SurfacingPro", "AutoBakeTask", "SurfacingPro: baking {0} {0}|plural(one=piece,other=pieces) before {1}"),
				Plan->Total(), FText::FromString(Why)));
			Task.MakeDialog();
			const SurfBake::FReport Report = SurfBake::BakeLevel(World, /*bSkipStanding*/ true, OnlyLevel);
			Task.EnterProgressFrame(1.0f);
			UE_LOG(LogSurfacingPro, Log, TEXT("auto bake (%s): %d groups, %d instances, %d meshes, %d triangles in %.1f s%s"),
				Why, Report.Groups, Report.Instances, Report.Meshes, Report.Triangles, Report.Seconds,
				Report.Problems.Num() > 0 ? *FString::Printf(TEXT(" - %d would not bake"), Report.Problems.Num()) : TEXT(""));
			SurfSession::Verb(TEXT("auto_bake"), FString::Printf(TEXT("%s meshes=%d tris=%d problems=%d"), Why, Report.Meshes, Report.Triangles, Report.Problems.Num()));
			if (USurfEdMode* Mode = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro)))
			{
				if (Mode->Inspector)
				{
					Mode->Inspector->SetResult(FString::Printf(TEXT("baked by itself before %s: %d mesh%s, %.1f s%s"),
						Why, Report.Meshes, Report.Meshes == 1 ? TEXT("") : TEXT("es"), Report.Seconds,
						Report.Problems.Num() > 0 ? *FString::Printf(TEXT(" - %d would not bake"), Report.Problems.Num()) : TEXT("")));
				}
			}
			return Report.Meshes;
		}

		void OnPreBeginPIE(const bool /*bIsSimulating*/)
		{
			if (GEditor)
			{
				BakeWhatIsStale(GEditor->GetEditorWorldContext().World(), TEXT("play"));
			}
		}

		/**
		 * Before the level is written: the save that is under way gathered
		 * its packages before this ran, so what the bake just dirtied - the
		 * groups' own files and the stand-ins' - is saved here, by hand, so
		 * the level on disk is whole. Autosaves and cooks are left alone.
		 */
		void OnPreSaveWorld(UWorld* World, FObjectPreSaveContext Context)
		{
			if (!World || Context.IsCooking() || Context.IsProceduralSave() || (Context.GetSaveFlags() & SAVE_FromAutosave))
			{
				return;
			}
			// The camera the level keeps (S187b): "super far from all and not
			// where camera was when i last saved".
			if (const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
			{
				const FVector At = Client->GetViewLocation();
				const FRotator Turn = Client->GetViewRotation();
				SurfSession::Verb(TEXT("camera_at_save"), FString::Printf(TEXT("%s at=%.0f,%.0f,%.0f pitch=%.1f yaw=%.1f"),
					*World->GetName(), At.X, At.Y, At.Z, Turn.Pitch, Turn.Yaw));
			}
			if (BakeWhatIsStale(World, TEXT("save")) == 0)
			{
				return;
			}

			TArray<UPackage*> Dirty;
			auto Gather = [&Dirty](const AActor* Actor)
			{
				UPackage* Package = Actor ? Actor->GetExternalPackage() : nullptr;
				if (Package && Package->IsDirty())
				{
					Dirty.AddUnique(Package);
				}
			};
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				Gather(*It);
				for (const TObjectPtr<AActor>& Actor : It->BakedActors)
				{
					Gather(Actor);
				}
			}
			for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
			{
				Gather(*It);
				Gather(It->BakedActor);
			}
			if (Dirty.Num() > 0)
			{
				UEditorLoadingAndSavingUtils::SavePackages(Dirty, /*bOnlyDirty*/ true);
			}
		}
	}

	int32 BeforeView(UWorld* EditorWorld)
	{
		return BakeWhatIsStale(EditorWorld, TEXT("view"));
	}

	void ShowBake(UWorld* EditorWorld, bool bShow)
	{
		ASurfGroup::bShowBakeInEditor = bShow;
		if (!EditorWorld)
		{
			return;
		}
		for (TActorIterator<ASurfGroup> It(EditorWorld); It; ++It)
		{
			if (IsValid(*It)) { It->ApplyBakeLook(); }
		}
		for (TActorIterator<ASurfPrefabInstance> It(EditorWorld); It; ++It)
		{
			if (IsValid(*It)) { It->ApplyBakeLook(); }
		}
	}

	void BeforeSceneKept(UWorld* EditorWorld, const ULevel* SceneLevel)
	{
		if (EditorWorld && SceneLevel)
		{
			BakeWhatIsStale(EditorWorld, TEXT("scene"), SceneLevel);
		}
	}

	/**
	 * The editor's own Game view, with the mode closed (S244c): "in game
	 * view if i leave surfacing mode; the whole surfacing objects/scene
	 * disappear". Out of the mode nothing followed the view - the previews
	 * are hidden in game and the bake hidden in the editor, so Game view
	 * showed neither. Now the bake follows the view whether the mode is up
	 * or not; the mode keeps its own, richer sync while it is.
	 */
	void FollowGameView()
	{
		if (!GEditor || !GEditor->IsInitialized() || GEditor->PlayWorld)
		{
			return;
		}
		if (GLevelEditorModeTools().IsModeActive(USurfEdMode::EM_SurfacingPro))
		{
			return;
		}
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!Client || !World)
		{
			return;
		}
		const bool bWanted = Client->IsInGameView();
		if (bWanted == ASurfGroup::bShowBakeInEditor)
		{
			return;
		}
		if (bWanted)
		{
			BeforeView(World);
		}
		ShowBake(World, bWanted);
		SurfSession::Verb(TEXT("game_view"), bWanted ? TEXT("on (editor)") : TEXT("off (editor)"));
		GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies*/ false);
	}

	void Register()
	{
		if (!PlayHandle.IsValid())
		{
			PlayHandle = FEditorDelegates::PreBeginPIE.AddStatic(&OnPreBeginPIE);
		}
		if (!SaveHandle.IsValid())
		{
			SaveHandle = FEditorDelegates::PreSaveWorldWithContext.AddStatic(&OnPreSaveWorld);
		}
		if (!GameViewHandle.IsValid())
		{
			GameViewHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
			{
				FollowGameView();
				return true;
			}), 0.25f);
		}
	}

	void Unregister()
	{
		if (GameViewHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GameViewHandle);
			GameViewHandle.Reset();
		}
		if (PlayHandle.IsValid())
		{
			FEditorDelegates::PreBeginPIE.Remove(PlayHandle);
			PlayHandle.Reset();
		}
		if (SaveHandle.IsValid())
		{
			FEditorDelegates::PreSaveWorldWithContext.Remove(SaveHandle);
			SaveHandle.Reset();
		}
	}
}
