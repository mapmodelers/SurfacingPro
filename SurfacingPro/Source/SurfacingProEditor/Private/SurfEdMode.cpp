// Copyright MapModelers. All Rights Reserved.

#include "SurfEdMode.h"
#include "SurfPatchOps.h"
#include "SurfSnapState.h"
#include "SurfAutoBake.h"
#include "SurfInspector.h"
#include "SurfToolPalette.h"
#include "UnrealClient.h"

#include "LevelEditor.h"
#include "ILevelEditor.h"
#include "SLevelViewport.h"
#include "LevelViewportActions.h"
#include "Containers/Ticker.h"
#include "EditorActorFolders.h"
#include "EditorModeManager.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "SurfKeymap.h"
#include "SurfSession.h"
#include "SurfCamera.h"
#include "SurfGizmo.h"
#include "TransformGizmoEditorSettings.h"
#include "Editor.h"
#include "Framework/Commands/GenericCommands.h"
#include "LevelEditorActions.h"
#include "FileHelpers.h"
#include "Toolkits/GlobalEditorCommonCommands.h"
#include "Settings/LevelEditorPlaySettings.h"

#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "SceneManagement.h"
#include "Toolkits/BaseToolkit.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "ScopedTransaction.h"

#include "SurfGroup.h"
#include "SurfChunk.h"
#include "SurfBrushMath.h"
#include "SurfProjection.h"
#include "SurfSettings.h"
#include "SurfPreviewMaterial.h"
#include "SurfBuild.h"
#include "SurfPickContext.h"
#include "SurfMenu.h"
#include "SurfWindows.h"
#include "SurfParamsView.h"
#include "SurfPatchMath.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"

#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Math/Ray.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Scalability.h"
#include "RenderTimer.h"
#include "RHI.h"
#include "DynamicRHI.h"
#include "Misc/ScopeExit.h"
#include "EditorModes.h"
#include "Editor/TransBuffer.h"
#include "Framework/Application/SlateApplication.h"
#include "SceneViewExtension.h"
#include "SceneView.h"
#include "Misc/App.h"

#include "SurfMatcapExtension.h"
#include "SurfSidePanel.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdMode"

const FEditorModeID USurfEdMode::EM_SurfacingPro = TEXT("EM_SurfacingPro");

namespace
{
	/**
	 * Ray against one hull face: hit the plane, then test the polygon in the
	 * plane's own basis. Cheaper and more robust than triangulating first,
	 * because hull faces are convex by construction.
	 */
	bool RayHitsHullFace(const FSurfHull& Hull, const FSurfHullFace& Face, const FPlane& Plane,
		const FVector& Origin, const FVector& Direction, double& OutDistance)
	{
		const FVector Normal(Plane.X, Plane.Y, Plane.Z);
		const double Denom = FVector::DotProduct(Normal, Direction);
		if (FMath::Abs(Denom) < 1.e-8)
		{
			return false;
		}

		const double Distance = (Plane.W - FVector::DotProduct(Normal, Origin)) / Denom;
		if (Distance <= 0.0)
		{
			return false;
		}

		const FVector Point = Origin + Direction * Distance;

		// Winding test: the point is inside when it stays on the same side of
		// every edge, which holds for a convex polygon wound consistently.
		const int32 Count = Face.VertexIndices.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector& A = Hull.Vertices[Face.VertexIndices[I]];
			const FVector& B = Hull.Vertices[Face.VertexIndices[(I + 1) % Count]];
			const FVector Edge = B - A;
			const FVector ToPoint = Point - A;

			if (FVector::DotProduct(FVector::CrossProduct(Edge, ToPoint), Normal) < -1.e-3)
			{
				return false;
			}
		}

		OutDistance = Distance;
		return true;
	}

	/** Details view of the inspector object. No hand-written Slate for the fields. */
	class FSurfEdModeToolkit : public FModeToolkit
	{
	public:
		virtual void Init(const TSharedPtr<IToolkitHost>& InitToolkitHost, TWeakObjectPtr<UEdMode> InOwningMode) override
		{
			FModeToolkit::Init(InitToolkitHost, InOwningMode);

			FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

			FDetailsViewArgs Args;
			Args.bAllowSearch = true;
			Args.bHideSelectionTip = true;
			Args.bShowOptions = false;
			Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;

			DetailsView = PropertyEditor.CreateDetailView(Args);

			if (USurfEdMode* SurfMode = Cast<USurfEdMode>(InOwningMode.Get()))
			{
				DetailsView->SetObject(SurfMode->Inspector);
			}

			// A pointer, not a second copy of the Actions window (S250e: he
			// chose it). The toolkit itself stays: its command list is what
			// gives Ctrl+S and Ctrl+A to the mode.
			Widget = SNew(SBox)
				.Padding(FMargin(10.0f, 8.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(LOCTEXT("ModesPanelPointer", "SurfacingPro works in the viewport: the sidebar, the menus along the top, Q, X to search, N for the SidePanel. Every verb is in the Actions window."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
					[
						SNew(SButton)
						.Text(LOCTEXT("ModesPanelActions", "Actions window"))
						.ToolTipText(LOCTEXT("ModesPanelActionsTip", "Every verb, by family; it docks anywhere."))
						.OnClicked_Lambda([]() { SurfActionsTab::Open(); return FReply::Handled(); })
					]
				];

			BindModeCommands(InOwningMode);
		}

		/**
		 * Ctrl+A is the level editor's Select All, registered as a global
		 * command - and a global command is handled before a viewport mode
		 * ever sees the key, which is why it kept selecting every actor in the
		 * outliner however the keymap was changed. The toolkit's command list
		 * is only live while the mode is active, so a binding here takes
		 * priority and gives the key back when the mode exits.
		 */
		void BindModeCommands(TWeakObjectPtr<UEdMode> InOwningMode)
		{
			TWeakObjectPtr<USurfEdMode> SurfMode = Cast<USurfEdMode>(InOwningMode.Get());
			if (!SurfMode.IsValid())
			{
				return;
			}

			/**
			 * Ctrl+S, inside a prefab, saves the prefab (S121a).
			 *
			 * "im editing a prefab and ctrl s didnt save it it seems. Bug" -
			 * and it did not: Ctrl+S is the level editor's Save Current
			 * Level, bound globally, and it saved the level while the thing
			 * he was editing sat unsaved in a room.
			 *
			 * Bound unconditionally rather than by CanExecute, and the
			 * decision made inside: a command that declines does not
			 * reliably fall through to the binding underneath it, and Ctrl+S
			 * failing to save anything at all would be worse than what it
			 * did before. Out in the level it calls the editor's own save,
			 * so the key means the same thing it always did.
			 */
			GetToolkitCommands()->MapAction(
				FLevelEditorCommands::Get().Save,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid() && SurfMode->IsInsidePrefab())
					{
						SurfMode->SaveOpenPrefab();
						return;
					}
					FEditorFileUtils::SaveCurrentLevel();
				}),
				FCanExecuteAction::CreateLambda([]() { return true; }));

			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().SelectAll,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (!SurfMode.IsValid())
					{
						return;
					}

					// All of whatever is being selected: faces, points, or in
					// object sub-mode every patch in the level. Never the
					// editor's own Select All, which took every actor.
					SurfMode->SelectAllBySubMode();
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					return SurfMode.IsValid();
				}));

			/**
			 * Ctrl+B is the editor's "browse to asset", registered globally,
			 * so the mode never saw the bevel key. Same fix as Ctrl+A.
			 */
			GetToolkitCommands()->MapAction(
				FGlobalEditorCommonCommands::Get().FindInContentBrowser,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid() && SurfMode->Inspector)
					{
						SurfMode->Inspector->Bevel();
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					return SurfMode.IsValid() && SurfMode->GetPointSelection().Num() > 0;
				}));

			/**
			 * Delete is the level editor's own command, registered globally,
			 * and a global command is handled before a viewport mode ever
			 * sees the key - which is why pressing Delete with a patch
			 * selected did nothing at all. Same shape as the Ctrl+A problem,
			 * same fix.
			 */
			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().Delete,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid())
					{
						SurfMode->DeleteSelection();
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					return SurfMode.IsValid() && SurfMode->GetSelectedPatch().IsValid();
				}));

			/**
			 * Ctrl+D and Ctrl+W: the editor's duplicate, which would copy the
			 * actor - the whole group - and paste it on top of itself. Here it
			 * copies what is selected and hands the copy to the move, as Shift+D
			 * does. Ctrl+C, Ctrl+X and Ctrl+V likewise: the selection, into our
			 * own clipboard, never the actor into the editor's.
			 */
			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().Duplicate,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid())
					{
						FEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
						SurfMode->DuplicateAndMove(Client, Client ? Client->Viewport : nullptr);
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					// Always ours while the mode is up: with nothing selected the
					// bar says so, rather than the editor taking the actor.
					return SurfMode.IsValid();
				}));

			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().Copy,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid())
					{
						SurfMode->CopySelection();
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					// Always ours while the mode is up: with nothing selected the
					// bar says so, rather than the editor taking the actor.
					return SurfMode.IsValid();
				}));

			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().Cut,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid())
					{
						SurfMode->CutSelection();
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					// Always ours while the mode is up: with nothing selected the
					// bar says so, rather than the editor taking the actor.
					return SurfMode.IsValid();
				}));

			GetToolkitCommands()->MapAction(
				FGenericCommands::Get().Paste,
				FExecuteAction::CreateLambda([SurfMode]()
				{
					if (SurfMode.IsValid())
					{
						SurfMode->PasteClipboard();
					}
				}),
				FCanExecuteAction::CreateLambda([SurfMode]()
				{
					// Always ours while the mode is up: with nothing copied the
					// bar says so, rather than the editor pasting an actor.
					return SurfMode.IsValid();
				}));
		}

		// No Mode Toolbar tab (S251i): with palettes "integrated" the editor
		// does not open its vertical toolbar for an empty palette.
		virtual bool HasIntegratedToolPalettes() const override { return true; }
		virtual FName GetToolkitFName() const override { return FName("SurfacingProEdMode"); }
		virtual FText GetBaseToolkitName() const override { return LOCTEXT("ToolkitName", "SurfacingPro"); }
		virtual TSharedPtr<SWidget> GetInlineContent() const override { return Widget; }

	private:
		TSharedPtr<IDetailsView> DetailsView;
		TSharedPtr<SWidget> Widget;
	};
}

USurfEdMode::USurfEdMode()
{
	Info = FEditorModeInfo(
		EM_SurfacingPro,
		LOCTEXT("ModeName", "SurfacingPro"),
		FSlateIcon(),
		true);
}

void USurfEdMode::Enter()
{
	ASurfGroup::bModeUp = true;
	GameViewEditHandle = ASurfGroup::OnEditedInGameView.AddUObject(this, &USurfEdMode::LeaveGameViewForEdit);
	// A level group still called SurfGroup takes the level's name (S202a);
	// the placements stand under it (S202b).
	TidyOutliner();
	if (GEngine)
	{
		ActorAddedHandle = GEngine->OnLevelActorAdded().AddUObject(this, &USurfEdMode::OnLevelActorAdded);
	}

	// The inspector must exist before Super::Enter, because UEdMode::Enter
	// calls CreateToolkit and Toolkit->Init inside itself - binding the
	// details view to a null object is what left the mode panel blank.
	if (!Inspector)
	{
		Inspector = NewObject<USurfInspector>(this, NAME_None, RF_Transient);
	}
	Inspector->Mode = this;

	// A reach saved past the ceiling (a kilometre, before S155d) comes back
	// inside it, once, on the way in.
	if (USurfSettings* Reach = GetMutableDefault<USurfSettings>())
	{
		const double Held = FMath::Clamp(Reach->SoftSelectionRadiusCm, USurfSettings::SoftReachMinCm, USurfSettings::SoftReachMaxCm);
		if (Held != Reach->SoftSelectionRadiusCm)
		{
			Reach->SoftSelectionRadiusCm = Held;
			Reach->SaveConfig();
		}
	}

	// Every bake in the level checked once: a level saved after a bake whose
	// meshes never reached disk comes up as wires over nothing, and nothing
	// in it would rebuild by itself (S73c).
	if (UWorld* Healing = GetWorld())
	{
		int32 Healed = 0;
		for (TActorIterator<ASurfGroup> It(Healing); It; ++It)
		{
			ASurfGroup* Group = *It;
			if (Group && Group->bBaked && !Group->BakeIsShowing())
			{
				Group->DropStaleBake();
				++Healed;
			}
		}
		for (TActorIterator<ASurfPrefabInstance> It(Healing); It; ++It)
		{
			ASurfPrefabInstance* Instance = *It;
			if (Instance && Instance->bBaked && !Instance->BakeIsShowing())
			{
				Instance->DropStaleBake();
				++Healed;
			}
		}
		if (Healed > 0)
		{
			SurfSession::Verb(TEXT("stale_bake_dropped"), FString::Printf(TEXT("things=%d"), Healed));
		}
	}

	/**
	 * And the empty groups a previous run left behind (S149c).
	 *
	 * "Why do i get multiple surfgroup" - and one of the two in his level
	 * had nothing in it at all: whatever filled it had been deleted, and
	 * the actor stayed, with the level's name floating over empty space
	 * (D106). Swept when the mode comes up, as a stale bake is.
	 */
	SweepEmptyGroups();

	// The subdivided surface used to be drawn in a blue close enough to the
	// cage to read as part of it. A copy of that old colour saved in the
	// settings moves to the new pink once; a colour the user has chosen for
	// themselves is left alone (S73b).
	if (USurfSettings* Colours = GetMutableDefault<USurfSettings>())
	{
		if (Colours->SurfaceWireframeColour.Equals(FLinearColor(0.35f, 0.55f, 0.85f, 0.6f), 0.002f))
		{
			Colours->SurfaceWireframeColour = FLinearColor(1.0f, 0.33f, 0.68f, 0.75f);
			Colours->SaveConfig();
		}
	}

	// A gizmo sized for moving actors sits on top of the control point it is
	// meant to be moving. Shrunk here, put back in Exit.
	// The viewport setting for this was deprecated in 5.6; the gizmo owns its
	// own size now.
	if (UTransformGizmoEditorSettings* Gizmo = GetMutableDefault<UTransformGizmoEditorSettings>())
	{
		PreviousGizmoSize = Gizmo->TransformGizmoSize;
	}
	AppliedGizmoSize = TNumericLimits<int32>::Min();
	ApplyGizmoSize();

	UndoHandle = FEditorDelegates::PostUndoRedo.AddUObject(this, &USurfEdMode::OnUndoRedo);

	// Every edit records what was selected when it was made, on the chunk it
	// edits, so undo hands the selection back with the geometry.
	USurfChunk::SelectionSnapshotProvider = [this](USurfChunk& Chunk)
	{
		SnapshotSelectionInto(Chunk);
	};

	// The bar goes inside the viewport, because the trip to a docked panel and
	// back is the largest cost in a modelling session and it is paid hundreds
	// of times an hour.
	// One bar and one adjust frame in every level viewport - a four-view
	// layout has them everywhere - the active one first, as the primary.
	InstallViewportOverlays();
	if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		// A layout change makes new viewports: the bars, the frames, the
		// view and the grid floor follow them.
		TabContentChangedHandle = LevelEditor->OnTabContentChanged().AddUObject(this, &USurfEdMode::OnViewportLayoutChanged);
	}

	// The level editor's Select All ("every actor") and Paste have no
	// precondition, so the viewport's own command list runs them before a
	// key ever reaches the mode - the toolkit's bindings never saw Ctrl+A.
	// Taken over here, put back on Exit.
	TakeOverGlobalCommands();

	// Every frame the machine has, while editing: the editor smooths to
	// about sixty by default. Put back on Exit.
	if (const USurfSettings* Rate = GetDefault<USurfSettings>(); Rate && Rate->EditingFrameRateCap > 0 && GEngine)
	{
		if (IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
		{
			PreviousMaxFPS = MaxFPS->GetFloat();
			MaxFPS->Set(static_cast<float>(Rate->EditingFrameRateCap), ECVF_SetByConsole);
		}
		bPreviousSmoothFrameRate = GEngine->bSmoothFrameRate;
		GEngine->bSmoothFrameRate = false;
		bFrameRateChanged = true;
	}

	// Modelling wants frames, not lighting: the renderer is turned down for
	// the length of the mode (a temporary override the editor does not save)
	// and put back on Exit. Resolution and anti-aliasing are left alone.
	ApplyEditingQuality(true);
	ApplyModellingView(true);

	// A patch with no surface is reported the moment a transaction leaves one.
	if (UTransBuffer* Trans = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr)
	{
		TransactionHandle = Trans->OnTransactionStateChanged().AddUObject(this, &USurfEdMode::OnTransactionState);
	}
	FlatPatchesReported.Reset();
	bFlatCheckPending = false;

	// What a setting shows follows the setting while the mode is up.
	if (USurfSettings* Editable = GetMutableDefault<USurfSettings>())
	{
		SettingEditedHandle = Editable->OnSettingChanged().AddUObject(this, &USurfEdMode::OnSettingEdited);
	}

	// The grid floor and a floor patch at height zero fight for the same
	// pixels. Off while editing, back on leaving; remembered per viewport so
	// a viewport that had it off stays off.
	GridFloorRestore.Reset();
	AxesRestore.Reset();
	NumpadRestore.Reset();
	HideGridFloor();

	// The checker is compiled here, at a safe moment, rather than on the
	// first rebuild that happens to need it - which could be a load, an undo,
	// or worse. See SurfPreviewMaterial.
	SurfPreviewMaterial::WarmUp();
	TrackHistory(true);
	// The mode opens in Object (S175a), Tab or 1 2 3 for Edit.
	if (SubMode != ESurfSubMode::Object)
	{
		LastEditKind = SubMode;
	}
	SubMode = ESurfSubMode::Object;
	// The matcap, if it was on when the mode last went (S172b).
	bViewOverridesAllowed = true;
	if (!MatcapExtension.IsValid())
	{
		MatcapExtension = FSceneViewExtensions::NewExtension<FSurfMatcapViewExtension>();
		MatcapExtension->bBackdrop = true;
	}
	RefreshPaintView();
	// Slate's share of the frame, for the frame line (S173a).
	if (FSlateApplication::IsInitialized())
	{
		SlatePreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([this](float) { SlateTickStartedAt = FPlatformTime::Seconds(); });
		SlatePostTickHandle = FSlateApplication::Get().OnPostTick().AddLambda([this](float)
		{
			if (SlateTickStartedAt > 0.0)
			{
				FrameSampleSlateMs += (FPlatformTime::Seconds() - SlateTickStartedAt) * 1000.0;
				SlateTickStartedAt = 0.0;
			}
		});
	}

	// Play starts where the camera is: a level is tested from where it is
	// being built. The editor's own setting, set here and put back on Exit.
	if (const USurfSettings* PlayPrefs = GetDefault<USurfSettings>(); PlayPrefs && PlayPrefs->bPlayFromCamera)
	{
		if (ULevelEditorPlaySettings* Play = GetMutableDefault<ULevelEditorPlaySettings>())
		{
			PreviousPlayLocation = Play->LastExecutedPlayModeLocation;
			bPlayLocationChanged = Play->LastExecutedPlayModeLocation != PlayLocation_CurrentCameraLocation;
			if (bPlayLocationChanged)
			{
				Play->LastExecutedPlayModeLocation = PlayLocation_CurrentCameraLocation;
				Play->PostEditChange();
				Play->SaveConfig();
			}
		}
	}

	// The camera orbits about what is under the cursor; this is how it asks.
	if (!Camera)
	{
		Camera = MakeUnique<FSurfCamera>();
	}
	Camera->UnderCursor = [this](FEditorViewportClient* Client, FViewport* InViewport, FVector& Out)
	{
		return FindPointUnderCursor(Client, InViewport, Out);
	};

	// Anything dropped into the viewport while we are up lands on the grid
	// (S122a). Registered with the mode rather than for good: outside it the
	// editor's own answer is the right one.
	if (!DroppedHandle.IsValid())
	{
		TWeakObjectPtr<USurfEdMode> Self = this;
		DroppedHandle = FEditorDelegates::OnNewActorsDropped.AddLambda(
			[Self](const TArray<UObject*>&, const TArray<AActor*>& Actors)
			{
				if (Self.IsValid())
				{
					Self->SnapDroppedActors(Actors);
				}
			});
	}

	SurfSession::Begin();
	// The bake's old drawer, empty to the eye since S219a (S251i; the
	// baked actors leave it first, S252c).
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		// The bake's actors of earlier builds still name the drawer: they
		// leave it first, else the outliner makes it again from them.
		int32 Moved = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (*It && It->Tags.Contains(TEXT("SurfacingProBaked")) && It->GetFolderPath() != NAME_None)
			{
				It->SetFolderPath(NAME_None);
				++Moved;
			}
		}
		const FFolder Drawer(FFolder::GetWorldRootFolder(World).GetRootObject(), FName(TEXT("SurfacingPro/Baked")));
		const bool bHad = FActorFolders::Get().ContainsFolder(*World, Drawer);
		if (bHad)
		{
			FActorFolders::Get().DeleteFolder(*World, Drawer);
		}
		SurfSession::Verb(TEXT("baked_drawer"), FString::Printf(TEXT("had=%d after=%d moved=%d"), bHad ? 1 : 0, FActorFolders::Get().ContainsFolder(*World, Drawer) ? 1 : 0, Moved));
	}
	if (const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
	{
		const FVector At = Client->GetViewLocation();
		SurfSession::Verb(TEXT("camera_at_enter"), FString::Printf(TEXT("at=%.0f,%.0f,%.0f pitch=%.1f yaw=%.1f"),
			At.X, At.Y, At.Z, Client->GetViewRotation().Pitch, Client->GetViewRotation().Yaw));
	}

	// A note he writes from here on carries where the tool was standing
	// (S117a).
	{
		TWeakObjectPtr<USurfEdMode> Self = this;
		SurfSession::NoteStateProvider.BindLambda([Self]()
		{
			return Self.IsValid() ? Self->DescribeStanding() : FString();
		});
	}
	SurfSession::Verb(TEXT("build"), SURF_BUILD_LABEL);

	// The panel owns soft selection now, so it has to start in step with
	// whatever the settings already say.
	if (Inspector)
	{
		Inspector->PullSoftSelection();
	}

	Super::Enter();

	Inspector->RefreshSelectionCount();
}

namespace
{
	/**
	 * The editor's own grid drawing (S180a), a protected member of the
	 * viewport client: reached through a member pointer taken inside a
	 * class derived from it, which the language allows.
	 */
	struct FSurfDrawHelperAccess : public FEditorViewportClient
	{
		static FEditorCommonDrawHelper& Of(FEditorViewportClient& Client)
		{
			return Client.*(&FSurfDrawHelperAccess::DrawHelper);
		}
	};

	FEditorCommonDrawHelper& SurfGridDrawHelper(FEditorViewportClient& Client)
	{
		return FSurfDrawHelperAccess::Of(Client);
	}
}

void USurfEdMode::Exit()
{
	ASurfGroup::bModeUp = false;
	ASurfGroup::OnEditedInGameView.Remove(GameViewEditHandle);
	GameViewEditHandle.Reset();
	if (GEngine)
	{
		GEngine->OnLevelActorAdded().Remove(ActorAddedHandle);
	}
	ActorAddedHandle.Reset();
	LeaveLattice();
	// The frames go with the mode (S156e); the docked tabs stay the editor's.
	SurfWindows::CloseAllFrames();

	// Game View is the mode's (S154e): the previews come back with the
	// editor's own view when the mode goes. Every viewport, and whether or
	// not the bake was showing (S244c: "in game view if i leave surfacing
	// mode; the whole surfacing objects/scene disappear" - out of the mode
	// the editor's Game view draws neither the previews, hidden in game,
	// nor the bake, hidden in the editor).
	if (ASurfGroup::bShowBakeInEditor)
	{
		SurfAutoBake::ShowBake(GetWorld(), false);
	}
	if (GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsInGameView())
			{
				Client->SetGameView(false);
				SurfSession::Verb(TEXT("game_view"), TEXT("off with the mode"));
			}
		}
	}

	// The brush goes away with the mode, and the materials come back.
	if (bPaintArmed)
	{
		EndPaintStroke();
		bPaintArmed = false;
	}
	TrackHistory(false);
	// The matcap too (S172b).
	bViewOverridesAllowed = false;
	RefreshPaintView();
	MatcapExtension.Reset();
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnPreTick().Remove(SlatePreTickHandle);
		FSlateApplication::Get().OnPostTick().Remove(SlatePostTickHandle);
	}
	SlatePreTickHandle.Reset();
	SlatePostTickHandle.Reset();

	ClearSelection();
	bBrushDrag = false;
	bBrushDragPushesFaces = false;
	if (UTransformGizmoEditorSettings* Gizmo = GetMutableDefault<UTransformGizmoEditorSettings>())
	{
		Gizmo->SetTransformGizmoSize(PreviousGizmoSize);
	}

	// A prefab still open is kept: the mode ending is not a reason to lose
	// an edit. The room's aside comes back with it, and a gesture still in
	// the hand is cancelled rather than left holding a transaction.
	if (Modal != ESurfModal::None)
	{
		CancelModal();
	}
	if (IsInsidePrefab())
	{
		UnwindRooms(/*bKeep*/ true);
	}

	// The isolate view is a view of the mode: it ends with it, the actors
	// and patches set aside put back.
	EndIsolate(/*bRestoreView*/ false);
	MeshSnapCache.Reset();
	SnapMarkerKind = ESurfSnapKind::None;

	if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		LevelEditor->OnTabContentChanged().Remove(TabContentChangedHandle);
	}
	TabContentChangedHandle.Reset();
	RemoveViewportOverlays();

	RestoreGlobalCommands();

	if (USurfSettings* Editable = GetMutableDefault<USurfSettings>())
	{
		Editable->OnSettingChanged().Remove(SettingEditedHandle);
	}
	SettingEditedHandle.Reset();
	ApplyEditingQuality(false);
	ApplyModellingView(false);
	if (UTransBuffer* Trans = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr)
	{
		Trans->OnTransactionStateChanged().Remove(TransactionHandle);
	}
	TransactionHandle.Reset();

	if (bFrameRateChanged)
	{
		if (IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
		{
			MaxFPS->Set(PreviousMaxFPS, ECVF_SetByConsole);
		}
		if (GEngine)
		{
			GEngine->bSmoothFrameRate = bPreviousSmoothFrameRate;
		}
		bFrameRateChanged = false;
	}

	FEditorDelegates::PostUndoRedo.Remove(UndoHandle);
	UndoHandle.Reset();
	USurfChunk::SelectionSnapshotProvider = nullptr;

	if (bPlayLocationChanged)
	{
		if (ULevelEditorPlaySettings* Play = GetMutableDefault<ULevelEditorPlaySettings>())
		{
			Play->LastExecutedPlayModeLocation = static_cast<EPlayModeLocations>(PreviousPlayLocation);
			Play->PostEditChange();
			Play->SaveConfig();
		}
		bPlayLocationChanged = false;
	}

	if (GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && GridFloorRestore.Contains(Client))
			{
				SurfGridDrawHelper(*Client).bDrawGrid = true;
			}
			if (Client && AxesRestore.Contains(Client))
			{
				Client->bDrawAxes = true;
			}
			if (Client && NumpadRestore.Contains(Client))
			{
				Client->bUseNumpadCameraControl = true;
			}
		}
	}
	GridFloorRestore.Reset();
	AxesRestore.Reset();
	NumpadRestore.Reset();

	if (DroppedHandle.IsValid())
	{
		FEditorDelegates::OnNewActorsDropped.Remove(DroppedHandle);
		DroppedHandle.Reset();
	}

	SurfSession::NoteStateProvider.Unbind();
	SurfSession::End();

	Super::Exit();
}

void USurfEdMode::CreateToolkit()
{
	Toolkit = MakeShared<FSurfEdModeToolkit>();
}

void USurfEdMode::SelectFace(const FSurfFaceRef& Face, bool bAddToSelection)
{
	if (!Face.IsValid())
	{
		return;
	}

	bool bTakenOut = false;
	if (bAddToSelection)
	{
		const int32 Existing = Selection.IndexOfByKey(Face);
		if (Existing != INDEX_NONE)
		{
			Selection.RemoveAt(Existing);
			bTakenOut = true;
		}
		else
		{
			Selection.Add(Face);
		}
	}
	else
	{
		Selection.Reset();
		Selection.Add(Face);

		/**
		 * A plain pick of a brush drops the patch pick, and a plain pick of
		 * a patch drops the brush pick (S151b): "it seems the flag applied
		 * to both the box and the patch?" - it did, because a box picked
		 * after a patch left the patch picked too, and every verb that
		 * asks "which pieces" got both. A plain click means this and
		 * nothing else; Shift and Ctrl are how a selection comes to span
		 * kinds.
		 */
		DropPatchPick();
	}

	/**
	 * A side taken out of the selection is not still the active one (S147a):
	 * "its created two instead of one when it was possible".
	 *
	 * The active face has counted as picked since S145a - which is right,
	 * because it is the one under the highlight. It made this line wrong:
	 * Ctrl-clicking a side off left it sitting in ActiveFace, so it was
	 * still picked, and a verb that acts on the picked sides made two of
	 * whatever it makes where he had asked for one.
	 */
	ActiveFace = bTakenOut
		? (Selection.Num() > 0 ? Selection.Last() : FSurfFaceRef())
		: Face;

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
		Inspector->PullFromActiveFace();
	}
}

void USurfEdMode::SelectBrushOf(const FSurfFaceRef& Face, bool bAddToSelection)
{
	ASurfGroup* Group = Face.Group.Get();
	const FSurfBrush* Brush = Group ? Group->FindBrush(Face.BrushId) : nullptr;
	if (!Brush || Brush->Faces.Num() == 0)
	{
		SelectFace(Face, bAddToSelection);
		return;
	}

	auto Same = [&Face](const FSurfFaceRef& Each)
	{
		return Each.Group == Face.Group && Each.BrushId == Face.BrushId;
	};

	if (bAddToSelection && Selection.ContainsByPredicate(Same))
	{
		// Already in: the whole brush comes out, not the one face clicked.
		Selection.RemoveAll(Same);
		ActiveFace = Selection.Num() > 0 ? Selection.Last() : FSurfFaceRef();
		if (Inspector)
		{
			Inspector->RefreshSelectionCount();
		}
		return;
	}

	if (!bAddToSelection)
	{
		Selection.Reset();
		DropPatchPick();
	}

	for (int32 I = 0; I < Brush->Faces.Num(); ++I)
	{
		FSurfFaceRef Ref;
		Ref.Group = Group;
		Ref.BrushId = Face.BrushId;
		Ref.FaceIndex = I;
		Selection.AddUnique(Ref);
	}

	// The face clicked stays the active one: it is what the surfacing panel
	// reads, and it is the face a verb that wants one should get.
	ActiveFace = Face;

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
		Inspector->PullFromActiveFace();
	}
}

TArray<FSurfFaceRef> USurfEdMode::PickedFaces() const
{
	TArray<FSurfFaceRef> Picked = Selection;
	if (ActiveFace.IsValid() && !Picked.Contains(ActiveFace))
	{
		Picked.Add(ActiveFace);
	}
	return Picked;
}

void USurfEdMode::LeaveGameViewForEdit()
{
	if (bLeavingGameView)
	{
		return;
	}
	bLeavingGameView = true;
	// Next tick, never inside the edit.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) -> bool
	{
		bLeavingGameView = false;
		if (FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
		{
			if (Client->IsInGameView())
			{
				Client->SetGameView(false);
				Client->Invalidate();
				if (Inspector)
				{
					Inspector->SetResult(TEXT("back to the editor's view to edit - G for the game's"));
				}
				SurfSession::Verb(TEXT("game_view"), TEXT("left for an edit"));
			}
		}
		return false;
	}), 0.0f);
}

bool USurfEdMode::IsGameViewOf(const FViewport* Viewport) const
{
	if (bOursInGameView)
	{
		return false;
	}
	const FEditorViewportClient* Client = Viewport ? static_cast<const FEditorViewportClient*>(Viewport->GetClient()) : nullptr;
	return Client && Client->IsInGameView();
}

void USurfEdMode::ToggleOursInGameView()
{
	bOursInGameView = !bOursInGameView;
	if (Inspector)
	{
		Inspector->SetResult(bOursInGameView ? TEXT("editing overlay shown in Game view, until Game view is entered again") : TEXT("Game view shows the game alone"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("ours_in_game"), bOursInGameView ? TEXT("on") : TEXT("off"));
	RedrawViewports();
}

void USurfEdMode::SyncGameView()
{
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client || bGameViewSyncPending)
	{
		return;
	}
	const bool bWanted = Client->IsInGameView();
	if (bWanted == ASurfGroup::bShowBakeInEditor)
	{
		return;
	}
	bGameViewSyncPending = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, bWanted](float) -> bool
	{
		bGameViewSyncPending = false;
		UWorld* World = GetWorld();
		if (bWanted)
		{
			SurfAutoBake::BeforeView(World);
		}
		SurfAutoBake::ShowBake(World, bWanted);
		// Entering the game view puts ours away every time (S231a).
		if (bWanted)
		{
			bOursInGameView = false;
		}
		SurfSession::Verb(TEXT("game_view"), bWanted ? TEXT("on") : TEXT("off"));
		if (Inspector)
		{
			Inspector->SetResult(bWanted
				? TEXT("Game view: the editing overlay is off - Overlay (X) draws it over the game")
				: TEXT("the editor's view"));
		}
		// The game is seen lit (S181a): "switching to Game view also
		// triggers the right view mode, and back to as it was on G again".
		if (bWanted)
		{
			// Remembered once: going Lit can pass through here again, and
			// the second visit read Lit as the view to come back to.
			if (ViewKindBeforeGame < 0)
			{
				ViewKindBeforeGame = GetViewKind();
			}
			if (GetViewKind() != 0)
			{
				SetViewKind(0);
			}
		}
		else if (ViewKindBeforeGame >= 0)
		{
			if (ViewKindBeforeGame != GetViewKind())
			{
				SetViewKind(ViewKindBeforeGame);
			}
			ViewKindBeforeGame = -1;
		}
		if (GEditor)
		{
			RedrawViewports();
		}
		return false;
	}), 0.0f);
}

void USurfEdMode::DropPatchPick()
{
	PatchSelection.Reset();
	SelectedPatch = FSurfPatchRef();
	PointSelection.Reset();
	QuadSelection.Reset();
	SoftInfluence.Reset();
}

void USurfEdMode::ClearSelection()
{
	Selection.Reset();
	ActiveFace = FSurfFaceRef();

	if (Inspector)
	{
		Inspector->RefreshSelectionCount();
	}
}

bool USurfEdMode::PickFaceWith(const FSurfPickContext& Ctx, FSurfFaceRef& Out, FVector* OutWorldHit) const
{
	UWorld* World = Ctx.World;
	if (!World)
	{
		return false;
	}

	const FVector Origin = Ctx.Origin;
	const FVector Direction = Ctx.Direction;

	double BestDistance = TNumericLimits<double>::Max();
	bool bFound = false;

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		ASurfGroup* Group = *It;
		if (!Group || !IsOpen(Group))
		{
			continue;
		}

		// The ray is in world space; primitives are in the actor's local space.
		const FTransform ToWorld = Group->GetActorTransform();
		const FVector LocalOrigin = ToWorld.InverseTransformPosition(Origin);
		const FVector LocalDirection = ToWorld.InverseTransformVector(Direction).GetSafeNormal();

		Group->ForEachBrush([&](const FSurfBrush& Brush)
		{
			FSurfHull Hull;
			if (!SurfBrushMath::DeriveHull(Brush, Hull))
			{
				return;
			}

			for (const FSurfHullFace& HullFace : Hull.Faces)
			{
				if (!Brush.Faces.IsValidIndex(HullFace.SourceFaceIndex))
				{
					continue;
				}

				double Distance = 0.0;
				if (RayHitsHullFace(Hull, HullFace, Brush.Faces[HullFace.SourceFaceIndex].Plane, LocalOrigin, LocalDirection, Distance))
				{
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						Out.Group = Group;
						Out.BrushId = Brush.Id;
						Out.FaceIndex = HullFace.SourceFaceIndex;
						bFound = true;

						if (OutWorldHit)
						{
							*OutWorldHit = ToWorld.TransformPosition(LocalOrigin + LocalDirection * Distance);
						}
					}
				}
			}
		});
	}

	return bFound;
}

bool USurfEdMode::PickFace(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfFaceRef& Out, FVector* OutWorldHit) const
{
	return PickFaceWith(FSurfPickContext::FromClick(ViewportClient, Click), Out, OutWorldHit);
}

namespace
{
	// The nearest prefab instance along the click's ray, by its preview mesh:
	// its bounds first, then the triangles. A mode that took every click for
	// itself left an instance unselectable (his round, 2026-09-14: "i couldnt
	// select back any of the two").
	/**
	 * Where a ray first meets a box, along the ray. False when it misses, and
	 * false when the ray starts inside it: what the eye is inside of is not
	 * what a click means to pick.
	 */
	bool RayHitsBox(const FBox& Box, const FVector& Origin, const FVector& Direction, double& OutAlong)
	{
		double Near = -TNumericLimits<double>::Max();
		double Far = TNumericLimits<double>::Max();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double D = Direction[Axis];
			if (FMath::Abs(D) < 1.e-9)
			{
				if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}
			double T0 = (Box.Min[Axis] - Origin[Axis]) / D;
			double T1 = (Box.Max[Axis] - Origin[Axis]) / D;
			if (T0 > T1)
			{
				Swap(T0, T1);
			}
			Near = FMath::Max(Near, T0);
			Far = FMath::Min(Far, T1);
			if (Near > Far)
			{
				return false;
			}
		}
		if (Near <= 0.0)
		{
			return false;
		}
		OutAlong = Near;
		return true;
	}

	ASurfPrefabInstance* PickPrefabInstance(UWorld* World, const FSurfPickContext& Ctx, double& OutDistance,
		int32* OutTriangle = nullptr, const ASurfPrefabInstance* Editing = nullptr, const TArray<TWeakObjectPtr<AActor>>* Aside = nullptr)
	{
		OutDistance = TNumericLimits<double>::Max();
		if (OutTriangle)
		{
			*OutTriangle = -1;
		}
		if (!World || !Ctx.IsValid())
		{
			return nullptr;
		}

		ASurfPrefabInstance* Best = nullptr;

		for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
		{
			ASurfPrefabInstance* Instance = *It;
			if (!Instance || !Instance->Preview || Instance->IsHidden() || Instance->IsTemporarilyHiddenInEditor() || Instance->bGhosted
				|| (Aside && Aside->Contains(TWeakObjectPtr<AActor>(Instance))))
			{
				continue;
			}

			/**
			 * Not the one being edited (S125a).
			 *
			 * "selecting objects in prefab edit mode doesnt work ." - and this
			 * is why. Inside a room the instance draws nothing of its own, so
			 * the pick falls back to its **bounding box**, and that box
			 * encloses everything in the room. Its front face is nearer to the
			 * camera than any geometry inside it, so it won every comparison:
			 * every click anywhere in the room selected the instance rather
			 * than the thing under the cursor.
			 *
			 * The fallback is right - a prefab has to stay clickable when it
			 * draws nothing (S73a) - and so is skipping it here. While you are
			 * inside it, the room *is* it; there is nothing to select.
			 */
			if (Editing && Instance == Editing)
			{
				continue;
			}
			const FTransform ToWorld = Instance->GetActorTransform();
			const FVector LocalOrigin = ToWorld.InverseTransformPosition(Ctx.Origin);
			const FVector LocalDirection = ToWorld.InverseTransformVectorNoScale(Ctx.Direction).GetSafeNormal();
			const FVector LocalEnd = LocalOrigin + LocalDirection * 1.0e6;

			// The bounds come from the mesh itself, not from the component's
			// cached box, and the triangles are walked by hand (S70a).
			double Hit = TNumericLimits<double>::Max();
			int32 HitTriangle = -1;
			Instance->Preview->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				if (Mesh.TriangleCount() == 0)
				{
					return;
				}
				const UE::Geometry::FAxisAlignedBox3d Local = Mesh.GetBounds(true);
				const FBox LocalBox(FVector(Local.Min), FVector(Local.Max));
				if (!FMath::LineBoxIntersection(LocalBox, LocalOrigin, LocalEnd, LocalEnd - LocalOrigin))
				{
					return;
				}

				// Every triangle, two-sided, the way a patch is picked: the
				// AABB tree's ray query answered nothing for a flat prefab
				// seen from above, and a prefab is a surface you can be
				// behind. A few thousand triangles a click is nothing.
				double Nearest = TNumericLimits<double>::Max();
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					FVector A, B, C;
					Mesh.GetTriVertices(Tid, A, B, C);
					const FVector E1 = B - A;
					const FVector E2 = C - A;
					const FVector P = FVector::CrossProduct(LocalDirection, E2);
					const double Det = FVector::DotProduct(E1, P);
					if (FMath::Abs(Det) < 1.e-9)
					{
						continue;
					}
					const double Inv = 1.0 / Det;
					const FVector T = LocalOrigin - A;
					const double U = FVector::DotProduct(T, P) * Inv;
					if (U < 0.0 || U > 1.0)
					{
						continue;
					}
					const FVector Q = FVector::CrossProduct(T, E1);
					const double V = FVector::DotProduct(LocalDirection, Q) * Inv;
					if (V < 0.0 || U + V > 1.0)
					{
						continue;
					}
					const double Along = FVector::DotProduct(E2, Q) * Inv;
					if (Along > 0.0 && Along < Nearest)
					{
						Nearest = Along;
						HitTriangle = Tid;
					}
				}
				if (Nearest < TNumericLimits<double>::Max())
				{
					// Back to world distance, so it compares with a patch hit.
					const FVector WorldHit = ToWorld.TransformPosition(LocalOrigin + LocalDirection * Nearest);
					Hit = FVector::Dist(Ctx.Origin, WorldHit);
				}
			});

			// Nothing of its own is drawn - baked, or the asset lost its
			// geometry - so no triangle could answer the ray. Its box does,
			// and a prefab stays clickable whatever state it is in: "cant
			// select prefabs while editing" (S73a).
			if (Hit == TNumericLimits<double>::Max() && Instance->DrawsNothing())
			{
				FBox LocalBox = Instance->Prefab ? Instance->Prefab->LocalBounds() : FBox(ForceInit);
				if (!LocalBox.IsValid)
				{
					LocalBox = FBox(FVector(-25.0), FVector(25.0));
				}
				double Along = 0.0;
				if (RayHitsBox(LocalBox, LocalOrigin, LocalDirection, Along))
				{
					const FVector WorldAt = ToWorld.TransformPosition(LocalOrigin + LocalDirection * Along);
					Hit = FVector::Dist(Ctx.Origin, WorldAt);
				}
			}

			if (Hit < OutDistance)
			{
				OutDistance = Hit;
				Best = Instance;
				if (OutTriangle)
				{
					*OutTriangle = HitTriangle;
				}
			}
		}
		return Best;
	}
}

bool USurfEdMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click)
{
	// A click on a handle of our gizmo is a drag that went nowhere, not a
	// pick of whatever sits under the handle.
	if (HitProxy && HitProxy->IsA(HSurfGizmoAxis::StaticGetType()))
	{
		return true;
	}

	// What the editor's own hit proxy says is under the cursor, kept for the
	// click below: our mode takes every click in the viewport, so without
	// this a light, a blueprint or a scene could not be selected at all
	// while SurfacingPro is up (S75a).
	ClickedActor = nullptr;
	if (HitProxy && HitProxy->IsA(HActor::StaticGetType()))
	{
		ClickedActor = static_cast<HActor*>(HitProxy)->Actor;
	}

	// (A click on a baked stand-in, S73a, is gone with S153a: the stand-ins
	// are hidden in the editor and the preview is what is clicked.)

	// One context for the whole click, built once: every pick below reads
	// the same ray and the same projection.
	return HandleClickWith(FSurfPickContext::FromClick(InViewportClient, Click), InViewportClient);
}

ASurfPrefabInstance* USurfEdMode::InstanceUnderForTest(const FSurfPickContext& Ctx, double& OutDistance) const
{
	return PickPrefabInstance(GetWorld(), Ctx, OutDistance, nullptr, DrilledInstance.Get(), &RoomContext);
}

bool USurfEdMode::HandleClickWith(const FSurfPickContext& Ctx, FEditorViewportClient* InViewportClient, bool bKeepOnMiss)
{
	// A loop is armed: the click takes the line that is being drawn, and does
	// nothing else.
	if (bPreviewingLoop && Ctx.Key == EKeys::LeftMouseButton)
	{
		return InsertLoopAtCursor(InViewportClient, InViewportClient ? InViewportClient->Viewport : nullptr);
	}

	/**
	 * A click on a name label picks what it names (S238a: "clicking a name
	 * label in viewport im expecting to select it"): a placed prefab, mesh
	 * or level as a click on it would, a group by its patches.
	 */
	if (Ctx.Key == EKeys::LeftMouseButton && !Ctx.bDoubleClick && !bPaintArmed && Modal == ESurfModal::None)
	{
		for (const TPair<FBox2D, TWeakObjectPtr<AActor>>& Hit : NameLabelHits)
		{
			AActor* Named = Hit.Value.Get();
			if (!Named || !Hit.Key.IsInside(Ctx.Pixel))
			{
				continue;
			}
			if (ASurfGroup* Group = Cast<ASurfGroup>(Named))
			{
				TArray<FSurfPatchRef> Refs;
				Group->ForEachPatch([&Refs, Group](const FSurfPatch& Patch)
				{
					FSurfPatchRef Ref;
					Ref.Group = Group;
					Ref.PatchId = Patch.Id;
					Refs.Add(Ref);
				});
				if (Refs.Num() == 0)
				{
					continue;
				}
				if (SubMode != ESurfSubMode::Object)
				{
					SetSubMode(ESurfSubMode::Object);
				}
				if (!Ctx.bShift && !Ctx.bCtrl)
				{
					ClearPointSelection();
					ClearSelection();
					SelectPatch(Refs[0]);
					for (int32 I = 1; I < Refs.Num(); ++I) { AddPatchToSelection(Refs[I], /*bToggle*/ false); }
				}
				else
				{
					for (const FSurfPatchRef& Ref : Refs) { AddPatchToSelection(Ref, /*bToggle*/ Ctx.bCtrl); }
				}
				SurfSession::Gesture(TEXT("label_click"), TEXT("group"));
				if (Inspector) { Inspector->RefreshSelectionCount(); }
				return true;
			}
			if (GEditor)
			{
				ClearPointSelection();
				SelectPatch(FSurfPatchRef());
				ClearSelection();
				if (Ctx.bCtrl && Named->IsSelected())
				{
					GEditor->SelectActor(Named, /*bInSelected*/ false, /*bNotify*/ true);
				}
				else
				{
					if (!Ctx.bShift && !Ctx.bCtrl)
					{
						GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
					}
					GEditor->SelectActor(Named, /*bInSelected*/ true, /*bNotify*/ true);
				}
				SurfSession::Gesture(TEXT("label_click"), TEXT("actor"));
				if (Inspector) { Inspector->RefreshSelectionCount(); }
				return true;
			}
		}
	}

	FSurfFaceRef Picked;
	FVector WorldHit = FVector::ZeroVector;
	const bool bHit = PickFaceWith(Ctx, Picked, &WorldHit);

	// While slicing, a left click places a cut point on the surface under the
	// cursor, snapped to the grid. Nothing else is blocked meanwhile (D16.3).
	if (bSlicing && Ctx.Key == EKeys::LeftMouseButton)
	{
		if (!bHit)
		{
			return false;
		}

		AddSlicePoint(WorldHit, Picked);
		return true;
	}

	// Middle-click copies the whole surface definition onto the selection.
	// This is the gesture that gets used five hundred times a day, so it is
	// in before anything else in the inspector.
	if (Ctx.Key == EKeys::MiddleMouseButton)
	{
		// The other way round with Shift (S167b): "middle click on another
		// patch/primitive transfers the UVs from one to the other (selected
		// → middle-clicked); shift to copy the material without UV. Middle
		// click itself is taken by the orbit, so ctrl shift middle click."
		// Ctrl+Shift: mapping and material; Shift alone: the material.
		if (Ctx.bShift)
		{
			return CopySurfaceToClick(Ctx, bHit, Picked, WorldHit, /*bMaterialOnly*/ !Ctx.bCtrl);
		}
		return CopySurfaceFromClick(Ctx, bHit, Picked, WorldHit);
	}

	// The right button is the menu (S79): every verb there is, by family,
	// without the SurfacingPro window being open - "i want be able to right
	// click and-or click some menu instead". Shift+right-click is the
	// editor's own menu, untouched.
	if (Ctx.Key == EKeys::RightMouseButton)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (Settings && Settings->bRightClickMenu && !Ctx.bShift)
		{
			SurfMenu::OpenAtCursor(this, InViewportClient);
			return true;
		}
		return false;
	}

	if (Ctx.Key != EKeys::LeftMouseButton)
	{
		return false;
	}

	// A prefab instance under the cursor, and how far: it competes with a
	// patch and a brush face for the click by distance, as they do with
	// each other.
	double InstanceDistance = TNumericLimits<double>::Max();
	int32 InstanceTriangle = -1;
	ASurfPrefabInstance* Instance = PickPrefabInstance(GetWorld(), Ctx, InstanceDistance, &InstanceTriangle,
		DrilledInstance.Get(), &RoomContext);

	// Inside a room (S71b) the stand-in draws the placements: a click on it
	// picks the placement under the cursor, a double-click edits it. Any
	// other click lets the picked placement go.
	if (IsInsidePrefab())
	{
		ASurfPrefabInstance* StandIn = RoomStandIn.Get();
		if (Instance && Instance == StandIn && (!bHit || InstanceDistance < FVector::Dist(Ctx.Origin, WorldHit)))
		{
			const FGuid Hit = StandIn->PlacementAtTriangle(InstanceTriangle);
			if (Hit.IsValid())
			{
				if (Ctx.bDoubleClick)
				{
					EnterPlacement(Hit);
					return true;
				}
				PickPlacement(Hit);
				return true;
			}
		}
		if (Instance == StandIn)
		{
			// And its distance with it (S243a: "clicking the prefab it
			// doesnt select it"): the stand-in's box wraps the room, so its
			// hit stood nearer than every patch of the room and the patch
			// pick lost to an instance that had just been let go of.
			Instance = nullptr;
			InstanceDistance = TNumericLimits<double>::Max();
		}
		RoomPickedPlacement.Invalidate();
	}

	// One Edit mode (S83a): before anything is picked, the click says which
	// kind it means - a control point under the cursor, else an edge, else
	// a face - and the tool follows it. Nothing of ours under the cursor
	// and the kind is left alone, so a click on empty space does not throw
	// the hand into face editing.
	if (AutoAppliesNow())
	{
		ESurfSubMode Wanted = SubMode;
		if (KindUnderCursor(Ctx, Wanted))
		{
			AdoptKind(Wanted);
		}
	}

	// Face sub-mode picks a cell of the control grid. It used to pick only
	// brush faces, which meant the sub-mode did nothing at all on a patch -
	// and patches are what this tool is for. Brush faces are still picked
	// when no patch cell is in the way.
	if (SubMode == ESurfSubMode::Face)
	{
		FSurfQuadRef Quad;
		FVector QuadHit = FVector::ZeroVector;

		if (PickPatchQuadWith(Ctx, Quad, &QuadHit))
		{
			const double QuadDistance = FVector::Dist(Ctx.Origin, QuadHit);
			const double BrushDistance = bHit ? FVector::Dist(Ctx.Origin, WorldHit) : TNumericLimits<double>::Max();

			if (QuadDistance <= BrushDistance)
			{
				// Ctrl: the block of faces between the last one clicked and
				// this one - two corners name a rectangle.
				if (Ctx.bCtrl && !Ctx.bAlt && SelectQuadRectTo(Quad))
				{
					return true;
				}

				// Alt takes the row of faces, again the column, again the
				// one face - the loop select every modelling package has.
				if (Ctx.bAlt)
				{
					SelectQuadRowOrColumn(Quad, Ctx.bCtrl || Ctx.bShift);
				}
				else
				{
					SelectQuad(Quad, Ctx.bCtrl || Ctx.bShift);
				}
				return true;
			}
		}
	}

	// Edge sub-mode: a click takes one edge, a double-click takes the whole
	// grid line - which is what split, extrude and bevel want, and what every
	// click used to do whether it was asked for or not.
	if (SubMode == ESurfSubMode::Edge)
	{
		// Alt takes the whole line (Blender's loop select, possible again now
		// that the middle button orbits); a double-click does too. Shift adds
		// to what is already there; Ctrl adds everything between the last
		// edge clicked and this one.
		const bool bWholeLine = bEdgePressWasDouble
			|| Ctx.bAlt
			|| Ctx.bDoubleClick;

		// A primitive's edge first (S166a): its two corners, moved as corners.
		{
			FSurfBrushCornerRef A;
			FSurfBrushCornerRef B;
			if (PickBrushEdgeWith(Ctx, A, B))
			{
				const bool bAdd = Ctx.bCtrl || Ctx.bShift;
				if (bWholeLine)
				{
					SelectBrushEdgeLoop(A, B, bAdd);
					bEdgePressWasDouble = false;
					return true;
				}
				if (!bAdd)
				{
					BrushCornerSelection.Reset();
				}
				BrushCornerSelection.AddUnique(A);
				BrushCornerSelection.AddUnique(B);
				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("primitive edge: %d corner%s picked - G moves them, and the sides follow"),
						BrushCornerSelection.Num(), BrushCornerSelection.Num() == 1 ? TEXT("") : TEXT("s")));
					Inspector->RefreshSelectionCount();
				}
				SurfSession::Gesture(TEXT("brush_edge_pick"), FString::Printf(TEXT("%d"), BrushCornerSelection.Num()));
				bEdgePressWasDouble = false;
				RedrawViewports();
				return true;
			}
		}

		if (SelectEdgeWith(Ctx, Ctx.bCtrl || Ctx.bShift, bWholeLine, /*bPath*/ Ctx.bCtrl))
		{
			bEdgePressWasDouble = false;
			return true;
		}

		bEdgePressWasDouble = false;
	}

	// Vertex sub-mode picks control points of the selected patch. Shift cycles
	// row, column, point; Ctrl adds and removes.
	if (SubMode == ESurfSubMode::Vertex)
	{
		// A brush's corners first: they are only offered on a brush already
		// being worked on, so a click near one meant that one (S113a).
		FSurfBrushCornerRef Corner;
		if (PickBrushCornerWith(Ctx, Corner))
		{
			SelectBrushCorner(Corner, Ctx.bCtrl || Ctx.bShift);
			return true;
		}

		FSurfPointRef Point;
		if (PickPointWith(Ctx, Point))
		{
			// Ctrl: the points between the last one clicked and this one,
			// along the grid - Blender's shortest path, on the click it is
			// on there. Shift alone toggles a point, so Ctrl is free for it.
			if (Ctx.bCtrl && !Ctx.bAlt && SelectGridPathTo(Point))
			{
				return true;
			}

			// Blender's modifiers, now that Alt is free of the camera: Alt
			// takes the row, again the column, again the point; Shift adds;
			// Shift+Alt adds the row or column; Ctrl toggles. Shift used to
			// be the row/column cycle, which left no way to add one point.
			if (Ctx.bAlt)
			{
				// Ctrl or Shift adds. Ctrl is the one to reach for with Alt
				// down: Shift+Alt is the Windows key for switching keyboard
				// language, and it fires whatever has focus.
				SelectPointRowOrColumn(Point, /*bAddToSelection*/ Ctx.bCtrl || Ctx.bShift);
			}
			else
			{
				SelectPoint(Point, Ctx.bCtrl || Ctx.bShift);
			}
			return true;
		}

		// Missing a control point used to end here, which left no way back to
		// another patch: you were locked inside whichever one you were last
		// editing. A miss falls through to picking, so clicking another patch
		// switches to it.
	}

	// Whichever of a brush face or a patch is nearer the eye.
	{
		FSurfPatchRef PickedPatch;
		FVector PatchHit = FVector::ZeroVector;
		const bool bPatchHit = PickPatchCyclingWith(Ctx, PickedPatch, &PatchHit);
		// What the picked primitive's container wears, and in which level
		// (S251a: "why that primitive is still showing the checker while
		// all the rest is matcap?").
		FString Wears = TEXT("-");
		if (bHit && Picked.Group.IsValid() && Picked.Group->Preview)
		{
			const UMaterialInterface* Worn = Picked.Group->Preview->GetMaterial(0);
			const ULevel* Within = Picked.Group->GetLevel();
			Wears = FString::Printf(TEXT("%s@%s%s"), Worn ? *Worn->GetName() : TEXT("none"),
				Within && Within->GetOuter() ? *Within->GetOuter()->GetName() : TEXT("-"), Picked.Group->bGhosted ? TEXT(":g") : TEXT(""));
		}
		SurfSession::Gesture(TEXT("patch_pick"), FString::Printf(TEXT("hit=%d dist=%.0f face=%d facedist=%.0f instance=%.0f wears=%s"),
			bPatchHit ? 1 : 0, bPatchHit ? FVector::Dist(Ctx.Origin, PatchHit) : -1.0, bHit ? 1 : 0,
			bHit ? FVector::Dist(Ctx.Origin, WorldHit) : -1.0, InstanceDistance < 1.e12 ? InstanceDistance : -1.0, *Wears));

		if (bPatchHit)
		{
			const double PatchDistance = FVector::Dist(Ctx.Origin, PatchHit);
			const double FaceDistance = bHit ? FVector::Dist(Ctx.Origin, WorldHit) : TNumericLimits<double>::Max();

			if (PatchDistance <= FaceDistance && PatchDistance <= InstanceDistance)
			{
				const bool bSamePatch = PickedPatch == SelectedPatch;

				// In object sub-mode Shift adds a patch and Ctrl takes one
				// out, so several can be joined, deleted or moved as one.
				// In the other sub-modes a Shift-click that lands on a patch
				// rather than on one of its points adds the patch and keeps
				// what was picked - a miss by a few pixels used to drop the
				// whole selection and switch patches.
				// A piece of a group picks the group (S249b); Alt+click one alone.
				FSurfPieceRef ClickedPiece;
				ClickedPiece.Group = PickedPatch.Group;
				ClickedPiece.Id = PickedPatch.PatchId;
				if (SubMode == ESurfSubMode::Object && ClickGroupPiece(ClickedPiece, INDEX_NONE, Ctx.bShift, Ctx.bCtrl, Ctx.bAlt))
				{
					return true;
				}

				if (SubMode == ESurfSubMode::Object && (Ctx.bCtrl || Ctx.bShift))
				{
					AddPatchToSelection(PickedPatch, /*bToggle*/ Ctx.bCtrl);
				}
				else if ((Ctx.bCtrl || Ctx.bShift) && (PointSelection.Num() > 0 || QuadSelection.Num() > 0))
				{
					AddPatchToSelection(PickedPatch, /*bToggle*/ false);
					if (Inspector)
					{
						Inspector->RefreshSelectionCount();
					}
					SurfSession::Gesture(TEXT("shift_click_added_patch"));
					return true;
				}
				else if (SubMode != ESurfSubMode::Object && InEditSet(PickedPatch))
				{
					// One of the patches being edited together: it becomes the
					// active one, the others stay in the edit (S191a).
					SelectPatch(PickedPatch, /*bKeepPoints*/ true);
					PointSelection.Reset();
					QuadSelection.Reset();
					SyncPatchSelectionToPoints();
				}
				else
				{
					SelectPatch(PickedPatch);
				}
				// Shift and Ctrl in Object keep the primitives picked with the
				// patches (S249b: a group may hold both).
				if (!(SubMode == ESurfSubMode::Object && (Ctx.bCtrl || Ctx.bShift)))
				{
					Selection.Reset();
					ActiveFace = FSurfFaceRef();
				}

				// A click picks; the sub-mode stays what Tab or 1 2 3 chose
				// (the mode opens in Object, S175a). The one-time jump to
				// Vertex on the first click went in S247c.
				SurfSession::Selection(FString::Printf(TEXT("patch %s"),
					bSamePatch ? TEXT("reselected") : TEXT("switched")));

				if (Inspector)
				{
					Inspector->RefreshSelectionCount();
				}
				return true;
			}
		}
	}

	// A prefab instance, nearer than any face of ours: the editor's selection
	// takes it, as it takes any actor - from there it moves, copies and
	// deletes with the editor's own widget and keys - and a double-click
	// enters it. Ours is dropped, so the two selections never point at once.
	if (Instance && (!bHit || InstanceDistance < FVector::Dist(Ctx.Origin, WorldHit)))
	{
		// Not while the brush is out (S238a: "disable enter/leave prefab
		// click shortcut in paint mode"): a second click there is a stroke.
		if (Ctx.bDoubleClick && !bPaintArmed)
		{
			EnterPrefabInstance(Instance);
			return true;
		}

		ClearPointSelection();
		SelectPatch(FSurfPatchRef());
		ClearSelection();
		if (GEditor)
		{
			// Shift adds to the editor's selection, Ctrl takes one out (S211a):
			// "expecting to be able to multi select staticmesh or
			// staticmesh+surfacingpro assets" - placements and meshes together.
			if (Ctx.bCtrl && Instance->IsSelected())
			{
				GEditor->SelectActor(Instance, /*bInSelected*/ false, /*bNotify*/ true);
			}
			else
			{
				if (!Ctx.bShift && !Ctx.bCtrl)
				{
					GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
				}
				GEditor->SelectActor(Instance, /*bInSelected*/ true, /*bNotify*/ true);
			}
		}
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("placed prefab %s - Open (Alt+PgDn) or a double-click edits it; it moves and copies as any actor"),
				Instance->Prefab ? *Instance->Prefab->GetName() : TEXT("(empty)")));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Selection(FString::Printf(TEXT("prefab instance %s"), *Instance->GetActorLabel()));
		return true;
	}

	// Nothing of ours under the cursor, but something of the level's: the
	// editor selects it, as it would with no mode up - a light, a blueprint,
	// a static mesh, a scene - and a double-click on a scene edits it
	// (S75a). Ours is dropped, so the two selections never point at once.
	if (!bHit && ClickedActor.IsValid() && !ClickedActor->IsA(ASurfGroup::StaticClass()) && !ClickedActor->IsA(ASurfPrefabInstance::StaticClass()))
	{
		AActor* Actor = ClickedActor.Get();
		// An actor inside a placed level stands for the level instance
		// (S227a: "i couldnt move it with G" - the click took the mesh
		// inside, in a level that cannot be edited from here); the
		// outermost one not being edited, as the editor's own click does.
		if (UWorld* ClickWorld = Actor->GetWorld())
		{
			if (ULevelInstanceSubsystem* Levels = ClickWorld->GetSubsystem<ULevelInstanceSubsystem>())
			{
				for (AActor* Parent : Levels->GetParentLevelInstanceActors(Actor->GetLevel()))
				{
					ILevelInstanceInterface* Holder = Cast<ILevelInstanceInterface>(Parent);
					if (!Holder || Levels->IsEditingLevelInstance(Holder))
					{
						break;
					}
					Actor = Parent;
				}
			}
		}
		ALevelInstance* Scene = Cast<ALevelInstance>(Actor);
		if (Scene && Ctx.bDoubleClick)
		{
			return EnterScene(Scene);
		}

		ClearPointSelection();
		SelectPatch(FSurfPatchRef());
		ClearSelection();
		if (GEditor)
		{
			if (Ctx.bCtrl && Actor->IsSelected())
			{
				GEditor->SelectActor(Actor, /*bInSelected*/ false, /*bNotify*/ true);
			}
			else
			{
				if (!Ctx.bShift && !Ctx.bCtrl)
				{
					GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
				}
				GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);
			}
		}
		if (Inspector)
		{
			Inspector->SetResult(Scene
				? FString::Printf(TEXT("level %s - Alt+PgDn or double-click edits it, and the outliner becomes its own"), *Actor->GetActorLabel())
				: FString::Printf(TEXT("%s - it moves and copies with the editor's own keys"), *Actor->GetActorLabel()));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Selection(FString::Printf(TEXT("actor %s"), *Actor->GetActorLabel()));
		return true;
	}

	// Nothing under the cursor at all: drop everything, in every sub-mode.
	// Keeping the cage up over empty space made it impossible to tell what
	// a verb would act on.
	if (!bHit)
	{
		/**
		 * And twice on nothing comes back out a level (S136a): "Clicking
		 * twice out of the model to exit / drill up?"
		 *
		 * The gesture a hand tries first, because it is how you put a thing
		 * down: click away from it, twice. One level, keeping, exactly as
		 * Alt+PgUp does - this is a second door to that verb and not a
		 * second verb.
		 *
		 * Only on a plain double-click. With Ctrl or Shift held the click is
		 * part of a selection, and leaving the room in the middle of making
		 * one is the kind of surprise that makes a tool feel haunted.
		 */
		if (Ctx.bDoubleClick && !Ctx.bCtrl && !Ctx.bShift && !bPaintArmed && (IsInsidePrefab() || IsInsideScene()))
		{
			SurfSession::Gesture(TEXT("double_click_out"));
			return PrefabOut();
		}
		// At the top, out of the mode (S251i), on the next tick: the mode
		// is not taken down from inside its own click.
		if (Ctx.bDoubleClick && !Ctx.bCtrl && !Ctx.bShift && !bPaintArmed && !IsInsidePrefab() && !IsInsideScene() && !GIsAutomationTesting)
		{
			SurfSession::Gesture(TEXT("double_click_leave_mode"));
			ClearPointSelection();
			SelectPatch(FSurfPatchRef());
			ClearSelection();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				FEditorModeTools& Tools = GLevelEditorModeTools();
				if (Tools.IsModeActive(USurfEdMode::EM_SurfacingPro))
				{
					Tools.DeactivateMode(USurfEdMode::EM_SurfacingPro);
				}
				return false;
			}));
			return true;
		}

		/**
		 * A press on the gizmo that found nothing is not a click on nothing
		 * (S149a): "selecting faces. rotating or moving. the selection is
		 * gone and i have to select again (was on that brush)."
		 *
		 * The ball's press falls through to a pick so it cannot swallow a
		 * click (S139a) - but the ball stands over the middle of whatever is
		 * selected, so the pick under it finds nothing more often than not,
		 * and dropping the selection there loses exactly what the hand was
		 * reaching for.
		 */
		if (bKeepOnMiss)
		{
			SurfSession::Gesture(TEXT("press_found_nothing_selection_kept"));
			return false;
		}

		if (!Ctx.bCtrl && !Ctx.bShift)
		{
			ClearPointSelection();
			SelectPatch(FSurfPatchRef());
			ClearSelection();
			SurfSession::Selection(TEXT("cleared - clicked nothing"));
			// What the miss looked like (S242a): the pixel, the viewport, and
			// how far the nearest control point of ours projected from it.
			{
				double Nearest = TNumericLimits<double>::Max();
				FVector2D NearestPx = FVector2D::ZeroVector;
				int32 Seen = 0;
				if (Ctx.World && Ctx.WorldToPixel)
				{
					for (TActorIterator<ASurfGroup> It(Ctx.World); It; ++It)
					{
						const ASurfGroup* Group = *It;
						if (!Group || !IsOpen(Group)) { continue; }
						const FTransform ToWorld = Group->GetActorTransform();
						Group->ForEachPatch([&](const FSurfPatch& Patch)
						{
							for (const FSurfControlPoint& Point : Patch.Points)
							{
								FVector2D Px;
								if (Ctx.WorldToPixel(ToWorld.TransformPosition(Point.Pos), Px))
								{
									++Seen;
									const double D = FVector2D::Distance(Px, Ctx.Pixel);
									if (D < Nearest) { Nearest = D; NearestPx = Px; }
								}
							}
						});
					}
				}
				// And what each pick said of the same ray (S242c).
				FSurfPatchRef DiagPatch;
				FVector DiagHit = FVector::ZeroVector;
				const bool bDiagPatch = PickPatchWith(Ctx, DiagPatch, &DiagHit);
				FSurfPointRef DiagPoint;
				const bool bDiagPoint = PickPointWith(Ctx, DiagPoint);
				int32 Triangles = 0;
				int32 Groups = 0;
				if (Ctx.World)
				{
					for (TActorIterator<ASurfGroup> It(Ctx.World); It; ++It)
					{
						if (!*It || !IsOpen(*It)) { continue; }
						++Groups;
						It->ForEachPatch([&Triangles](const FSurfPatch& Patch)
						{
							FSurfTessellation Tess;
							if (SurfPatchMath::Tessellate(Patch, Tess)) { Triangles += Tess.Indices.Num() / 3; }
						});
					}
				}
				// Placed prefabs whose box the ray crosses, and their state
				// (S250c): p preview, h hidden, g ghosted, c context.
				FString Boxes;
				int32 BoxesHit = 0;
				if (Ctx.World)
				{
					for (TActorIterator<ASurfPrefabInstance> It(Ctx.World); It; ++It)
					{
						const ASurfPrefabInstance* Each = *It;
						const FBox Box = Each ? Each->WorldBounds() : FBox(ForceInit);
						if (!Box.IsValid || !FMath::LineBoxIntersection(Box, Ctx.Origin, Ctx.Origin + Ctx.Direction * 1.0e7, Ctx.Direction * 1.0e7))
						{
							continue;
						}
						if (++BoxesHit <= 3)
						{
							Boxes += FString::Printf(TEXT("%s%s:%s%s%s%s"), Boxes.IsEmpty() ? TEXT("") : TEXT(","), *Each->GetActorLabel(),
								Each->Preview ? TEXT("p") : TEXT("-"), Each->IsTemporarilyHiddenInEditor() ? TEXT("h") : TEXT(""),
								Each->bGhosted ? TEXT("g") : TEXT(""), IsRoomContext(Each) ? TEXT("c") : TEXT(""));
						}
					}
				}
				SurfSession::Friction(TEXT("click_nothing_where"), FString::Printf(TEXT("px=%.0f,%.0f size=%dx%d points=%d nearest=%.0fpx at=%.0f,%.0f origin=%.0f,%.0f,%.0f dir=%.2f,%.2f,%.2f patch=%d point=%d face=%d instance=%s at=%.0f groups=%d tris=%d submode=%d boxes=%d %s"),
					Ctx.Pixel.X, Ctx.Pixel.Y, Ctx.Size.X, Ctx.Size.Y, Seen, Seen > 0 ? Nearest : -1.0, NearestPx.X, NearestPx.Y,
					Ctx.Origin.X, Ctx.Origin.Y, Ctx.Origin.Z, Ctx.Direction.X, Ctx.Direction.Y, Ctx.Direction.Z,
					bDiagPatch ? 1 : 0, bDiagPoint ? 1 : 0, bHit ? 1 : 0, Instance ? *Instance->GetActorLabel() : TEXT("-"), Instance ? InstanceDistance : -1.0, Groups, Triangles, static_cast<int32>(SubMode),
					BoxesHit, *Boxes));
			}
			if (IsInsidePrefab() && Inspector)
			{
				const ASurfPrefabInstance* Drilled = DrilledInstance.Get();
				Inspector->SetResult(FString::Printf(TEXT("inside %s - only the prefab can be picked here; Alt+PgUp leaves it, keeping"),
					Drilled && Drilled->Prefab ? *Drilled->Prefab->GetName() : TEXT("the prefab")));
			}
		}
		return true;
	}

	if (SubMode == ESurfSubMode::Vertex)
	{
		if (!Ctx.bCtrl && !Ctx.bShift)
		{
			ClearPointSelection();
		}
		return true;
	}

	if (!bHit)
	{
		if (!Ctx.bCtrl && !Ctx.bShift)
		{
			ClearSelection();
		}
		return false;
	}

	/**
	 * In object sub-mode the brush is the thing, not the face it was
	 * clicked on (S138a). The drag only pushes faces along their normals in
	 * **face** sub-mode, so a brush held by all of its faces here moves,
	 * turns and scales as one solid rather than inflating.
	 */
	if (SubMode == ESurfSubMode::Object)
	{
		// A piece of a group picks the group (S249b); Alt+click one alone.
		FSurfPieceRef ClickedPiece;
		ClickedPiece.Group = Picked.Group;
		ClickedPiece.Id = Picked.BrushId;
		ClickedPiece.bBrush = true;
		if (!ClickGroupPiece(ClickedPiece, Picked.FaceIndex, Ctx.bShift, Ctx.bCtrl, Ctx.bAlt))
		{
			SelectBrushOf(Picked, Ctx.bCtrl || Ctx.bShift);
		}
	}
	else if (Ctx.bAlt && SubMode == ESurfSubMode::Face)
	{
		// The loop of faces, as on a patch (S185b).
		SelectBrushFaceLoop(Picked, WorldHit, Ctx.bCtrl || Ctx.bShift);
	}
	else
	{
		SelectFace(Picked, Ctx.bCtrl || Ctx.bShift);
	}
	return true;
}

void USurfEdMode::SampleFrame(double OurRenderMs, const FViewport* Viewport)
{
	// Where a frame's time goes - the game thread, the render thread, the
	// GPU, the swap - so "30 fps" can be read against the thread that
	// costs it, from the session record, without anyone reading stats off
	// the screen. Once every couple of seconds; the first line says how the
	// editor is set up.
	if (!bFrameSetupLogged)
	{
		bFrameSetupLogged = true;
		auto CVar = [](const TCHAR* Name) -> FString
		{
			const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
			return Var ? Var->GetString() : FString(TEXT("?"));
		};
		const Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
		SurfSession::Verb(TEXT("frame_setup"), FString::Printf(
			TEXT("maxfps=%s vsync=%s vsync_editor=%s msaa_composite=%s spmode=%s sp=%s smooth=%d realtime=%d res=%.0f shadow=%d gi=%d refl=%d pp=%d aa=%d view=%d tex=%d fx=%d shading=%d temporary=%d"),
			*CVar(TEXT("t.MaxFPS")), *CVar(TEXT("r.VSync")), *CVar(TEXT("r.VSyncEditor")), *CVar(TEXT("r.MSAA.CompositingSampleCount")),
			*CVar(TEXT("r.Editor.Viewport.ScreenPercentageMode.RealTime")), *CVar(TEXT("r.Editor.Viewport.ScreenPercentage")),
			GEngine && GEngine->bSmoothFrameRate ? 1 : 0,
			GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsRealtime() ? 1 : 0,
			Levels.ResolutionQuality, Levels.ShadowQuality, Levels.GlobalIlluminationQuality, Levels.ReflectionQuality,
			Levels.PostProcessQuality, Levels.AntiAliasingQuality, Levels.ViewDistanceQuality, Levels.TextureQuality,
			Levels.EffectsQuality, Levels.ShadingQuality, Scalability::IsTemporaryQualityLevelActive() ? 1 : 0));
	}

	const double Now = FPlatformTime::Seconds();
	if (FrameSampleStart <= 0.0)
	{
		FrameSampleStart = Now;
	}

	FrameSampleRenderMs += OurRenderMs;
	FrameSampleBarMs += SurfTakeBarPaintMs();
	// A frame whose game thread worked for more than 40 ms is a hitch the
	// hand feels (S172b): recorded on its own, with the last verb, so the
	// frame line's two-second average does not hide it.
	{
		const double GameMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
		// Only with the editor in front (S173a): in the background it sleeps
		// on purpose, and those sleeps read as seconds-long frames.
		if (GameMs > 40.0 && Now - LastHitchLoggedAt > 0.5 && FApp::HasFocus())
		{
			LastHitchLoggedAt = Now;
			SurfSession::Verb(TEXT("hitch"), FString::Printf(TEXT("game=%.0f after=%s"), GameMs, LastVerbName.IsEmpty() ? TEXT("-") : *LastVerbName));
		}
	}
	FrameSampleGameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
	FrameSampleDrawMs += FPlatformTime::ToMilliseconds(GRenderThreadTime);
	FrameSampleGpuMs += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	FrameSampleSwapMs += FPlatformTime::ToMilliseconds(GSwapBufferTime);
	++FrameSampleCount;

	const double Elapsed = Now - FrameSampleStart;
	if (Elapsed >= 2.0 && FrameSampleCount > 0)
	{
		const double N = FrameSampleCount;
		SurfSession::Verb(TEXT("frame"), FString::Printf(
			TEXT("fps=%.0f ms=%.1f game=%.1f draw=%.1f gpu=%.1f swap=%.1f ours=%.2f bar=%.2f slate=%.1f fg=%d back=%d"),
			N / Elapsed, 1000.0 * Elapsed / N,
			FrameSampleGameMs / N, FrameSampleDrawMs / N, FrameSampleGpuMs / N, FrameSampleSwapMs / N, FrameSampleRenderMs / N, FrameSampleBarMs / N,
			FrameSampleSlateMs / N, FApp::HasFocus() ? 1 : 0, MatcapExtension.IsValid() ? MatcapExtension->TakeBackdropCount() : -1));
		FrameSampleStart = Now;
		FrameSampleRenderMs = FrameSampleGameMs = FrameSampleDrawMs = FrameSampleGpuMs = FrameSampleSwapMs = FrameSampleBarMs = FrameSampleSlateMs = 0.0;
		FrameSampleCount = 0;
	}
}

void USurfEdMode::ApplyGizmoSize()
{
	// Applied whenever the setting differs from what was last applied, so a
	// change in the settings window takes effect on the next frame rather
	// than on the next time the mode is entered.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const int32 Wanted = Settings ? Settings->GizmoSizeAdjustment : -5;
	if (Wanted == AppliedGizmoSize)
	{
		return;
	}

	if (UTransformGizmoEditorSettings* Gizmo = GetMutableDefault<UTransformGizmoEditorSettings>())
	{
		Gizmo->SetTransformGizmoSize(static_cast<float>(Wanted));
		AppliedGizmoSize = Wanted;
	}
}

void USurfEdMode::SelectAllBySubMode()
{
	if (SubMode == ESurfSubMode::Face)
	{
		SelectAllQuads();
	}
	else if (SubMode == ESurfSubMode::Object)
	{
		SelectAllPatches();
	}
	else
	{
		SelectAllPoints();
	}
}

void USurfEdMode::TakeOverGlobalCommands()
{
	FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (!LevelEditor)
	{
		return;
	}

	TSharedRef<FUICommandList> Global = LevelEditor->GetGlobalLevelEditorActions();
	TWeakObjectPtr<USurfEdMode> Self = this;

	auto TakeOver = [&](TSharedPtr<const FUICommandInfo> Command, FExecuteAction Execute)
	{
		if (!Command.IsValid() || SavedGlobalActions.Contains(Command))
		{
			return;
		}
		if (const FUIAction* Existing = Global->GetActionForCommand(Command))
		{
			SavedGlobalActions.Add(Command, *Existing);
		}
		else
		{
			SavedGlobalActions.Add(Command, FUIAction());
		}
		Global->MapAction(Command, Execute, FCanExecuteAction::CreateLambda([Self]() { return Self.IsValid(); }));
	};

	TakeOver(FGenericCommands::Get().SelectAll, FExecuteAction::CreateLambda([Self]()
	{
		if (Self.IsValid())
		{
			Self->SelectAllBySubMode();
		}
	}));
	TakeOver(FGenericCommands::Get().Paste, FExecuteAction::CreateLambda([Self]()
	{
		if (Self.IsValid())
		{
			Self->PasteClipboard();
		}
	}));
}

void USurfEdMode::RestoreGlobalCommands()
{
	FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditor)
	{
		TSharedRef<FUICommandList> Global = LevelEditor->GetGlobalLevelEditorActions();
		for (const TPair<TSharedPtr<const FUICommandInfo>, FUIAction>& Saved : SavedGlobalActions)
		{
			if (Saved.Value.ExecuteAction.IsBound())
			{
				Global->MapAction(Saved.Key, Saved.Value);
			}
			else
			{
				Global->UnmapAction(Saved.Key);
			}
		}
	}
	SavedGlobalActions.Reset();
}

void USurfEdMode::ApplyEditingQuality(bool bOn)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();

	// Tips wrap at a third of a screen while the mode is up (S170a): "the
	// current lines are too long... it needs return line more often so it
	// makes not too wide lines". The editor's own is a thousand pixels;
	// put back when the mode goes. Independent of the quality drop below.
	if (IConsoleVariable* Wrap = IConsoleManager::Get().FindConsoleVariable(TEXT("Slate.ToolTipWrapWidth")))
	{
		if (bOn && !bToolTipWrapNarrowed)
		{
			PreviousToolTipWrap = Wrap->GetFloat();
			Wrap->Set(340.0f, ECVF_SetByConsole);
			bToolTipWrapNarrowed = true;
		}
		else if (!bOn && bToolTipWrapNarrowed)
		{
			Wrap->Set(PreviousToolTipWrap, ECVF_SetByConsole);
			bToolTipWrapNarrowed = false;
		}
	}

	if (bOn)
	{
		if (bQualityLowered || !Settings || !Settings->bLowerQualityWhileEditing || Scalability::IsTemporaryQualityLevelActive())
		{
			return;
		}

		// Everything that costs pixels and shows nothing about the model:
		// shadows, global illumination, reflections, post-processing, effects,
		// foliage and shading at their lowest. The resolution, the
		// anti-aliasing, the view distance and the textures stay as they are,
		// so the model itself is as sharp as it was.
		Scalability::FQualityLevels Lowered = Scalability::GetQualityLevels();
		Lowered.ShadowQuality = 0;
		Lowered.GlobalIlluminationQuality = 0;
		Lowered.ReflectionQuality = 0;
		Lowered.PostProcessQuality = 0;
		Lowered.EffectsQuality = 0;
		Lowered.FoliageQuality = 0;
		Lowered.ShadingQuality = 0;
		Scalability::ToggleTemporaryQualityLevels(true, Lowered);

		// The wires, points and gizmo are "editor primitives", which the
		// renderer draws into a separate target with 4x multisampling and
		// composites over the scene - a full-resolution pass that only runs
		// while something is drawn, which is while something is selected:
		// a hundred frames a second fell to thirty the moment a patch was
		// picked. One sample while editing; the wires are crisp already.
		if (IConsoleVariable* Samples = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MSAA.CompositingSampleCount")))
		{
			PreviousCompositingSamples = Samples->GetInt();
			Samples->Set(1, ECVF_SetByConsole);
		}

		// Fewer pixels: the viewport is the model and nothing else while
		// modelling, and the model reads as well at two thirds of them.
		// The editor's own default for real-time viewports is put back on
		// Exit.
		const int32 Percentage = FMath::Clamp(Settings->EditingScreenPercentage, 25, 100);
		IConsoleVariable* Mode_ = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Editor.Viewport.ScreenPercentageMode.RealTime"));
		IConsoleVariable* Percent = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Editor.Viewport.ScreenPercentage"));
		if (Percentage < 100 && Mode_ && Percent)
		{
			PreviousScreenPercentageMode = Mode_->GetInt();
			PreviousScreenPercentage = Percent->GetInt();
			Mode_->Set(0, ECVF_SetByConsole);
			Percent->Set(Percentage, ECVF_SetByConsole);
			bScreenPercentageChanged = true;
		}

		bQualityLowered = true;
	}
	else if (bQualityLowered)
	{
		if (IConsoleVariable* Samples = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MSAA.CompositingSampleCount")))
		{
			Samples->Set(PreviousCompositingSamples, ECVF_SetByConsole);
		}
		if (bScreenPercentageChanged)
		{
			if (IConsoleVariable* Mode_ = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Editor.Viewport.ScreenPercentageMode.RealTime")))
			{
				Mode_->Set(PreviousScreenPercentageMode, ECVF_SetByConsole);
			}
			if (IConsoleVariable* Percent = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Editor.Viewport.ScreenPercentage")))
			{
				Percent->Set(PreviousScreenPercentage, ECVF_SetByConsole);
			}
			bScreenPercentageChanged = false;
		}
		Scalability::ToggleTemporaryQualityLevels(false);
		bQualityLowered = false;
	}
}

void USurfEdMode::OnSettingEdited(UObject* Object, FPropertyChangedEvent& Event)
{
	const FName Name = Event.Property ? Event.Property->GetFName() : NAME_None;

	// The clay and the backdrop follow their colours, and the tick (S172b).
	if (Name == GET_MEMBER_NAME_CHECKED(USurfSettings, MatcapColour) || Name == GET_MEMBER_NAME_CHECKED(USurfSettings, BackdropTop)
		|| Name == GET_MEMBER_NAME_CHECKED(USurfSettings, BackdropBottom) || Name == GET_MEMBER_NAME_CHECKED(USurfSettings, bMatcap)
		|| (Event.MemberProperty && (Event.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(USurfSettings, MatcapColour)
			|| Event.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(USurfSettings, BackdropTop)
			|| Event.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(USurfSettings, BackdropBottom))))
	{
		SurfPreviewMaterial::RefreshViewColours();
		RefreshPaintView();
	}

	// What was set, with its value, in the record: the numbers settled on
	// in the settings window are read from here.
	if (Object && Event.MemberProperty && Event.ChangeType != EPropertyChangeType::Interactive)
	{
		FString Value;
		Event.MemberProperty->ExportText_InContainer(0, Value, Object, Object, Object, PPF_None);
		SurfSession::Verb(TEXT("setting"), FString::Printf(TEXT("%s=%s"), *Event.MemberProperty->GetName(), *Value));
	}

	if (Name == GET_MEMBER_NAME_CHECKED(USurfSettings, bLowerQualityWhileEditing) || Name == GET_MEMBER_NAME_CHECKED(USurfSettings, EditingScreenPercentage))
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		ApplyEditingQuality(false);
		ApplyEditingQuality(true);
	}

	if (Name == GET_MEMBER_NAME_CHECKED(USurfSettings, CheckerDarkest) || Name == GET_MEMBER_NAME_CHECKED(USurfSettings, CheckerLightest))
	{
		SurfPreviewMaterial::RefreshLevels();
	}

	if (Name == GET_MEMBER_NAME_CHECKED(USurfSettings, PreviewShade))
	{
		SurfPreviewMaterial::RefreshShade();
	}

	if (Name == GET_MEMBER_NAME_CHECKED(USurfSettings, bModellingViewWhileEditing))
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		ApplyModellingView(false);
		ApplyModellingView(Settings && Settings->bModellingViewWhileEditing);
	}

	if (GEditor)
	{
		RedrawViewports();
	}
}

void USurfEdMode::ApplyModellingView(bool bOn)
{
	if (!GEditor)
	{
		return;
	}

	if (bOn)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const bool bFirst = !bModellingViewApplied;
		if (bFirst && (!Settings || !Settings->bModellingViewWhileEditing))
		{
			return;
		}
		if (bFirst)
		{
			bModellingViewOn = true;
		}

		// Every perspective viewport not seen yet - all of them on Enter, the
		// new ones after a layout change - takes the view chosen: unlit,
		// unless Shift+Z asked for the shaded one. Flat panes keep their
		// wireframe.
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective() && !PreviousViewModes.Contains(Client))
			{
				PreviousViewModes.Add(Client, static_cast<int32>(Client->GetViewMode()));
				Client->SetViewMode(bModellingViewOn ? VMI_Unlit : VMI_Lit);
			}
		}
		bModellingViewApplied = true;
		if (bFirst)
		{
			SurfSession::Verb(TEXT("view"), TEXT("modelling (unlit)"));
		}
	}
	else if (bModellingViewApplied)
	{
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		for (const TPair<FLevelEditorViewportClient*, int32>& Pair : PreviousViewModes)
		{
			if (Clients.Contains(Pair.Key))
			{
				Pair.Key->SetViewMode(static_cast<EViewModeIndex>(Pair.Value));
			}
		}
		PreviousViewModes.Reset();
		bModellingViewApplied = false;
	}
}

bool USurfEdMode::IsModellingView() const
{
	if (!GEditor)
	{
		return false;
	}
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client && Client->IsPerspective())
		{
			return Client->GetViewMode() == VMI_Unlit;
		}
	}
	return false;
}

void USurfEdMode::ToggleShadedView()
{
	if (!GEditor)
	{
		return;
	}

	// Between the modelling view and the shaded one, on every perspective
	// viewport. What they showed before the mode is remembered the first
	// time either way, so Exit puts it back.
	const bool bToModelling = !IsModellingView();
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client && Client->IsPerspective())
		{
			if (!PreviousViewModes.Contains(Client))
			{
				PreviousViewModes.Add(Client, static_cast<int32>(Client->GetViewMode()));
			}
			Client->SetViewMode(bToModelling ? VMI_Unlit : VMI_Lit);
		}
	}
	bModellingViewApplied = true;
	bModellingViewOn = bToModelling;

	if (Inspector)
	{
		Inspector->SetResult(bToModelling
			? TEXT("modelling view: unlit, nothing of the level's lighting is paid for - Shift+Z for the shaded view")
			: TEXT("shaded view: the level's lighting - Shift+Z for the modelling view"));
		Inspector->RefreshSelectionCount();
	}
	SurfSession::Verb(TEXT("view"), bToModelling ? TEXT("modelling (unlit)") : TEXT("shaded (lit)"));
	RedrawViewports();
}

void USurfEdMode::OpenSearch()
{
	// The bar of the viewport under the cursor, so the frame opens in it.
	const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
	for (int32 I = 0; I < ViewportHosts.Num(); ++I)
	{
		TSharedPtr<SLevelViewport> Host = ViewportHosts[I].Pin();
		if (Host.IsValid() && Host->GetCachedGeometry().IsUnderLocation(Cursor) && ViewportBars.IsValidIndex(I) && ViewportBars[I].IsValid())
		{
			ViewportBars[I]->OpenSearch(Cursor);
			return;
		}
	}
	if (ViewportBar.IsValid())
	{
		ViewportBar->OpenSearch(Cursor);
	}
}

void USurfEdMode::OpenUVSearch(TSharedPtr<SWidget> Parent)
{
	TSharedPtr<SSurfViewportBar> Bar = ViewportBar;
	for (const TSharedPtr<SSurfViewportBar>& Each : ViewportBars)
	{
		if (Each.IsValid()) { Bar = Each; break; }
	}
	if (!Bar.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	// "if the results are use-able in uv mode too only right. no useless
	// results": the UV family's verbs and the Surface settings.
	Bar->OpenSearch(FSlateApplication::Get().GetCursorPos(), [](const FSurfSearchEntry& Entry)
	{
		if (Entry.bSetting)
		{
			return Entry.Section.ToString().Contains(TEXT("Surface"));
		}
		if (Entry.Verb.IsValid())
		{
			return Entry.Verb->Group == ESurfVerbGroup::UV;
		}
		const FString Label = Entry.Label.ToString();
		return Label.Contains(TEXT("UV")) || Label.StartsWith(TEXT("Select")) || Label.StartsWith(TEXT("Invert"));
	}, Parent);
}

int32 USurfEdMode::HostIndexOf(const SWidget* Frame) const
{
	for (int32 I = 0; I < AdjustFrames.Num(); ++I)
	{
		if (AdjustFrames[I].Get() == Frame)
		{
			return I;
		}
	}
	return -1;
}

void USurfEdMode::ToggleMaximizeViewport(FEditorViewportClient* ViewportClient)
{
	// The viewport the key came from fills the layout, or goes back.
	for (const TWeakPtr<SLevelViewport>& Weak : ViewportHosts)
	{
		TSharedPtr<SLevelViewport> Host = Weak.Pin();
		if (Host.IsValid() && (!ViewportClient || Host->GetViewportClient().Get() == ViewportClient))
		{
			Host->MakeMaximized(!Host->IsMaximized());
			SurfSession::Verb(TEXT("maximize_viewport"), Host->IsMaximized() ? TEXT("maximized") : TEXT("restored"));
			return;
		}
	}
}

void USurfEdMode::ToggleFourViews()
{
	TSharedPtr<SLevelViewport> Host;
	for (const TWeakPtr<SLevelViewport>& Weak : ViewportHosts)
	{
		Host = Weak.Pin();
		if (Host.IsValid())
		{
			break;
		}
	}
	if (!Host.IsValid())
	{
		return;
	}

	// Which layout is up: the editor answers by name, one name at a time.
	static const FName Layouts[] = {
		LevelViewportConfigurationNames::OnePane,
		LevelViewportConfigurationNames::TwoPanesHoriz, LevelViewportConfigurationNames::TwoPanesVert,
		LevelViewportConfigurationNames::ThreePanesLeft, LevelViewportConfigurationNames::ThreePanesRight,
		LevelViewportConfigurationNames::ThreePanesTop, LevelViewportConfigurationNames::ThreePanesBottom,
		LevelViewportConfigurationNames::FourPanesLeft, LevelViewportConfigurationNames::FourPanesRight,
		LevelViewportConfigurationNames::FourPanesTop, LevelViewportConfigurationNames::FourPanesBottom,
		LevelViewportConfigurationNames::FourPanes2x2 };
	FName Current = NAME_None;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	for (const FName& Name : Layouts)
	{
		if (Host->IsViewportConfigurationSet(Name))
		{
			Current = Name;
			break;
		}
	}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	FName Next;
	if (Current.IsNone() || Current == LevelViewportConfigurationNames::OnePane)
	{
		Next = LastMultiPaneLayout.IsNone() ? LevelViewportConfigurationNames::FourPanes2x2 : LastMultiPaneLayout;
	}
	else
	{
		LastMultiPaneLayout = Current;
		Next = LevelViewportConfigurationNames::OnePane;
	}

	// Switching destroys the viewports, the one the key came from included,
	// so it waits for the key to be done with: next tick.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [Host, Next](float) -> bool
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Host->OnSetViewportConfiguration(Next);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		return false;
	}), 0.0f);
	SurfSession::Verb(TEXT("four_views"), Next.ToString());
}

void USurfEdMode::InstallViewportOverlays()
{
	// One bar and one adjust frame in every level viewport - a four-view
	// layout has them everywhere - the active one first, as the primary.
	FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (!LevelEditor)
	{
		return;
	}

	TArray<TSharedPtr<SLevelViewport>> Hosts;
	if (TSharedPtr<SLevelViewport> Active = LevelEditor->GetFirstActiveLevelViewport())
	{
		Hosts.Add(Active);
	}
	if (TSharedPtr<ILevelEditor> Instance = LevelEditor->GetLevelEditorInstance().Pin())
	{
		for (const TSharedPtr<SLevelViewport>& Each : Instance->GetViewports())
		{
			if (Each.IsValid() && !Hosts.Contains(Each))
			{
				Hosts.Add(Each);
			}
		}
	}
	// The main viewport first: the perspective one, else the active one.
	// The bar goes there and only there ("can't have the toolbar in all of
	// them, only in main" - the flat panes are for looking); the adjust
	// frame goes everywhere, since it opens at the cursor.
	for (int32 I = 1; I < Hosts.Num(); ++I)
	{
		TSharedPtr<FEditorViewportClient> Client = Hosts[I]->GetViewportClient();
		TSharedPtr<FEditorViewportClient> First = Hosts[0]->GetViewportClient();
		if (Client.IsValid() && Client->IsPerspective() && (!First.IsValid() || !First->IsPerspective()))
		{
			Hosts.Swap(0, I);
			break;
		}
	}
	for (int32 I = 0; I < Hosts.Num(); ++I)
	{
		const TSharedPtr<SLevelViewport>& Host = Hosts[I];
		TSharedPtr<SSurfViewportBar> Bar;
		if (I == 0)
		{
			Bar = SNew(SSurfViewportBar).Mode(this);
			Host->AddOverlayWidget(Bar.ToSharedRef());
		}
		TSharedPtr<SSurfAdjustFrame> Frame = SNew(SSurfAdjustFrame).Mode(this);
		Host->AddOverlayWidget(Frame.ToSharedRef());
		ViewportBars.Add(Bar);
		AdjustFrames.Add(Frame);
		ViewportHosts.Add(Host);
	}
	if (Hosts.Num() > 0)
	{
		ViewportBar = ViewportBars[0];
		AdjustFrame = AdjustFrames[0];
		ViewportHost = Hosts[0];
	}
}

void USurfEdMode::RemoveViewportOverlays()
{
	for (int32 I = 0; I < ViewportHosts.Num(); ++I)
	{
		if (TSharedPtr<SLevelViewport> Host = ViewportHosts[I].Pin())
		{
			if (ViewportBars.IsValidIndex(I) && ViewportBars[I].IsValid())
			{
				Host->RemoveOverlayWidget(ViewportBars[I].ToSharedRef());
			}
			if (AdjustFrames.IsValidIndex(I) && AdjustFrames[I].IsValid())
			{
				Host->RemoveOverlayWidget(AdjustFrames[I].ToSharedRef());
			}
		}
	}
	ViewportBars.Reset();
	AdjustFrames.Reset();
	ViewportHosts.Reset();
	ViewportBar.Reset();
	AdjustFrame.Reset();
	ViewportHost.Reset();
	AdjustFrameHost = -1;
}

void USurfEdMode::OnViewportLayoutChanged()
{
	// The viewports are new: the overlays go where they are now, and the
	// view and the grid floor are applied to the ones not seen yet.
	RemoveViewportOverlays();
	InstallViewportOverlays();
	HideGridFloor();
	ApplyModellingView(true);
	SurfSession::Verb(TEXT("viewport_layout"), FString::Printf(TEXT("viewports=%d"), ViewportHosts.Num()));
	if (GEditor)
	{
		RedrawViewports();
	}
}

FString USurfEdMode::DescribeStanding() const
{
	/**
	 * Everything a note is likely to have been about, in one line (S117a).
	 *
	 * Not everything the mode knows - a line nobody reads is worse than no
	 * line. These are the things that turn "it doesn't work" into "it doesn't
	 * work *there*": which sub-mode, whether he was inside a prefab, what was
	 * picked, and which tool a drag would have used.
	 */
	const USurfSettings* Settings = GetDefault<USurfSettings>();

	const TCHAR* Kind = TEXT("object");
	switch (SubMode)
	{
	case ESurfSubMode::Vertex: Kind = TEXT("vertex"); break;
	case ESurfSubMode::Edge:   Kind = TEXT("edge"); break;
	case ESurfSubMode::Face:   Kind = TEXT("face"); break;
	default: break;
	}

	FString Where = TEXT("level");
	if (IsInsidePrefab())
	{
		const USurfPrefab* Prefab = ContextPrefab();
		Where = FString::Printf(TEXT("prefab %s"), Prefab ? *Prefab->GetName() : TEXT("?"));
	}
	else if (IsInsideScene())
	{
		Where = TEXT("level");
	}

	return FString::Printf(
		TEXT("build=%s in=%s submode=%s patch=%d patches=%d points=%d faces=%d corners=%d actors=%d modal=%d baking=%d wire=%d scheme=%s"),
		SURF_BUILD_LABEL,
		*Where,
		Kind,
		SelectedPatch.IsValid() ? 1 : 0,
		PatchSelection.Num(),
		PointSelection.Num(),
		Selection.Num(),
		BrushCornerSelection.Num(),
		GEditor ? GEditor->GetSelectedActorCount() : 0,
		static_cast<int32>(Modal),
		IsBaking() ? 1 : 0,
		Settings ? static_cast<int32>(Settings->WireView) : -1,
		Settings && Settings->KeyScheme == ESurfKeyScheme::Blender ? TEXT("blender") : TEXT("house"));
}

void USurfEdMode::SnapDroppedActors(const TArray<AActor*>& Dropped)
{
	/**
	 * A dropped asset lands on the grid (S122a).
	 *
	 * The editor drops an actor at the exact point the cursor's ray met
	 * something - a good answer for dressing a finished level and the wrong
	 * one for a blockout, where everything else in the scene is on the
	 * lattice and the new thing is the only piece that is not.
	 *
	 * Only the position, and only the axes the grid has: the drop's own
	 * rotation is whatever the surface under the cursor implied, and taking
	 * that away would be answering a question he did not ask.
	 */
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !Settings->bSnapDroppedActors || Dropped.Num() == 0)
	{
		return;
	}

	const double Step = FMath::Max(1.0, Settings->GetGridStepCm(Settings->DefaultGridIndex));

	int32 Moved = 0;
	for (AActor* Actor : Dropped)
	{
		if (!Actor)
		{
			continue;
		}
		const FVector Was = Actor->GetActorLocation();
		const FVector Now(
			FMath::RoundToDouble(Was.X / Step) * Step,
			FMath::RoundToDouble(Was.Y / Step) * Step,
			FMath::RoundToDouble(Was.Z / Step) * Step);

		if (!Was.Equals(Now, 0.01))
		{
			Actor->Modify();
			Actor->SetActorLocation(Now);
			Actor->PostEditMove(true);
			++Moved;
		}
	}

	if (Moved > 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(FString::Printf(TEXT("dropped on the grid: %d thing%s to the nearest %.0f cm"),
				Moved, Moved == 1 ? TEXT("") : TEXT("s"), Step));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Verb(TEXT("drop_snapped"), FString::Printf(TEXT("%d step=%.0f"), Moved, Step));
		RedrawViewports();
	}
}

void USurfEdMode::HideGridFloor()
{
	// The grid floor and a floor patch at height zero fight for the same
	// pixels. Off while editing, back on leaving; remembered per viewport
	// so a viewport that had it off stays off.
	/**
	 * The numpad is ours (S174a): "keypad orbiting keys are fighting with
	 * some UE keys?" - they were. The editor walks the camera on held
	 * numpad keys (8 2 4 6, 9 7, 1 3) by polling them every frame, whatever
	 * a mode does with the press; ours turns the view. Its walk is off
	 * while the mode is up, back on leaving.
	 */
	if (GEditor)
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->bUseNumpadCameraControl && !NumpadRestore.Contains(Client))
			{
				Client->bUseNumpadCameraControl = false;
				NumpadRestore.Add(Client);
			}
		}
	}

	if (!GEditor)
	{
		return;
	}
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		/**
		 * The editor's Grid show flag stays the switch (S180a) - "a grid
		 * option already exists with UE, let's avoid redundancy" - but in
		 * the perspective views it draws our floor: the editor's own
		 * drawing of it is off while the mode is up, back on leaving.
		 */
		if (Client && Client->IsPerspective() && SurfGridDrawHelper(*Client).bDrawGrid && !GridFloorRestore.Contains(Client))
		{
			SurfGridDrawHelper(*Client).bDrawGrid = false;
			GridFloorRestore.Add(Client);
		}

		/**
		 * And the editor's axis indicator (S110a): "the stats/info text collide
		 * with the xyz gizmo."
		 *
		 * It is drawn in the bottom-left corner by the viewport client itself,
		 * which is the corner the readout lives in - and there is no setting
		 * for where it goes. Ours is in the top right, clickable, and does the
		 * same job better, so the editor's comes off while the mode is up and
		 * goes back on leaving.
		 */
		if (Client && Client->bDrawAxes && !AxesRestore.Contains(Client))
		{
			Client->bDrawAxes = false;
			AxesRestore.Add(Client);
		}
	}
}

bool USurfEdMode::MouseEnter(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	// The pane under the cursor takes the keys, as Blender's areas do: F,
	// the numbers, G R S act where the hand is rather than where it last
	// clicked. Only when the keys were a pane's already - a text box being
	// typed into keeps them.
	if (ViewportClient && Modal == ESurfModal::None && !DragTransaction.IsValid() && FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
		// The UVs window is a pane of ours too (S252a): it takes the keys on
		// hover, so the viewport takes them back the same way - X over the
		// viewport opened the UV search after a pass over the nets.
		bool bFocusOnAPane = !Focused.IsValid() || Focused->GetTypeAsString() == TEXT("SSurfUVView");
		TSharedPtr<SLevelViewport> Entered;
		for (const TWeakPtr<SLevelViewport>& Weak : ViewportHosts)
		{
			TSharedPtr<SLevelViewport> Host = Weak.Pin();
			if (!Host.IsValid())
			{
				continue;
			}
			if (Focused.IsValid() && Host->GetViewportWidget().Pin() == Focused)
			{
				bFocusOnAPane = true;
			}
			if (Host->GetViewportClient().Get() == ViewportClient)
			{
				Entered = Host;
			}
		}
		if (bFocusOnAPane && Entered.IsValid())
		{
			if (Entered->GetViewportWidget().Pin() != Focused)
			{
				Entered->SetKeyboardFocusToThisViewport();
			}
			if (FLevelEditorViewportClient* Level = static_cast<FLevelEditorViewportClient*>(ViewportClient); Level != GCurrentLevelEditingViewportClient)
			{
				Level->SetCurrentViewport();
			}
		}
	}

	return Super::MouseEnter(ViewportClient, Viewport, X, Y);
}

void USurfEdMode::AddNote(const FString& Text)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}
	SurfSession::Note(Trimmed);

	/**
	 * A picture beside the note (S165a): "make sure you're tracking my
	 * sessions as well as possible". The viewport as it stands when the
	 * note is typed, saved next to the session file under the same stem
	 * with the note's second - the line in the record and the picture
	 * share a name, so what he saw is on file with what he said.
	 */
	{
		const FString Session = SurfSession::CurrentPath();
		if (!Session.IsEmpty())
		{
			const FString Stem = FPaths::GetBaseFilename(Session);
			const FString Picture = FPaths::Combine(FPaths::GetPath(Session),
				FString::Printf(TEXT("%s-note-%s.png"), *Stem, *FDateTime::Now().ToString(TEXT("%H.%M.%S"))));
			FScreenshotRequest::RequestScreenshot(Picture, /*bInShowUI*/ true, /*bAddFilenameSuffix*/ false);
			if (GEditor)
			{
				RedrawViewports();
			}
		}
	}

	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("noted: %s"), *Trimmed));
		Inspector->RefreshSelectionCount();
	}
}

void USurfEdMode::OnTransactionState(const FTransactionContext& Context, ETransactionStateEventType State)
{
	if (State == ETransactionStateEventType::TransactionFinalized)
	{
		bFlatCheckPending = true;
		FlatCheckCause = Context.Title.ToString();
	}
}

void USurfEdMode::CheckForFlatPatches()
{
	// Not in the middle of a gesture: an extrude in place is flat until
	// the move that follows gives it height, and that is not a fault.
	if (!bFlatCheckPending || Modal != ESurfModal::None || DragTransaction.IsValid())
	{
		return;
	}
	bFlatCheckPending = false;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ASurfGroup> It(World); It; ++It)
	{
		const ASurfGroup* Group = *It;
		if (!Group)
		{
			continue;
		}
		Group->ForEachPatch([&](const FSurfPatch& Patch)
		{
			if (!Patch.HasValidDimensions() || FlatPatchesReported.Contains(Patch.Id))
			{
				return;
			}
			// The surface's area, cell by cell; a patch with none is a line
			// or a point drawn as wires, not a surface.
			const double Area = SurfPatchOps::NetArea(Patch);
			double Extent = 0.0;
			for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
			{
				for (int32 X = 0; X + 1 < Patch.Width; ++X)
				{
					Extent = FMath::Max(Extent, FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X + 1, Y + 1).Pos));
				}
			}
			if (Area < 0.5 && Extent > 1.0)
			{
				FlatPatchesReported.Add(Patch.Id);
				SurfSession::Friction(TEXT("flat_patch"), FString::Printf(TEXT("by=%s patch=%dx%d extent=%.0fcm"),
					*FlatCheckCause, Patch.Width, Patch.Height, Extent));
				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("a patch with no surface was left by \"%s\" (%d x %d) - undo, or delete it"),
						*FlatCheckCause, Patch.Width, Patch.Height));
					Inspector->RefreshSelectionCount();
				}
			}
		});
	}
}

void USurfEdMode::ToggleOverlay()
{
	bOverlayHidden = !bOverlayHidden;
	if (Inspector)
	{
		Inspector->SetResult(bOverlayHidden ? TEXT("overlay off: no wires (Alt+Z brings them back)") : TEXT("overlay on"));
	}
	SurfSession::Verb(TEXT("overlay"), bOverlayHidden ? TEXT("off") : TEXT("on"));
	RedrawViewports();
}

void USurfEdMode::CycleWireView()
{
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings)
	{
		return;
	}

	switch (Settings->WireView)
	{
	case ESurfWireView::Selected: Settings->WireView = ESurfWireView::All; break;
	case ESurfWireView::All:      Settings->WireView = ESurfWireView::XRay; break;
	default:                      Settings->WireView = ESurfWireView::Selected; break;
	}
	Settings->SaveConfig();

	// No line on the status frame: "dont comment 'wires: blablabla'". The
	// Wires button carries the state, and the viewport shows it - a
	// sentence explaining what you can already see is noise (his note,
	// 2026-09-19).
	SurfSession::Verb(TEXT("wire_view"), DescribeWireView());

	if (GEditor)
	{
		RedrawViewports();
	}
}

FString USurfEdMode::DescribeWireView() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	switch (Settings ? Settings->WireView : ESurfWireView::Selected)
	{
	case ESurfWireView::All:  return TEXT("wires: all");
	case ESurfWireView::XRay: return TEXT("wires: all, x-ray");
	default:                  return TEXT("wires: edited");
	}
}

bool USurfEdMode::SelectsThroughSurfaces() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (Settings && Settings->WireView == ESurfWireView::XRay)
	{
		return true;
	}
	// The editor's own wireframe (Alt+2) sees through too (S157a, A21):
	// "while in alt 2 mode im expecting be able select the things in the
	// back too." What is drawn as lines is picked as lines.
	if (GCurrentLevelEditingViewportClient)
	{
		const EViewModeIndex ViewMode = GCurrentLevelEditingViewportClient->GetViewMode();
		return ViewMode == VMI_Wireframe || ViewMode == VMI_BrushWireframe;
	}
	return false;
}

namespace
{
	/**
	 * A wire at the wire strength (S249a: "also expose wire strength?").
	 * Full strength is the crisp line it always was; under it, a one-pixel
	 * strip blended over what is behind - the plain line is drawn opaque
	 * whatever its alpha says.
	 */
	void SurfWire(FPrimitiveDrawInterface* PDI, const FVector& A, const FVector& B, const FLinearColor& Colour, uint8 Depth, float Width, float Strength)
	{
		if (Strength >= 0.999f)
		{
			PDI->DrawLine(A, B, Colour, Depth, Width, 0.0f, /*bScreenSpace*/ true);
			return;
		}
		PDI->DrawTranslucentLine(A, B, Colour.CopyWithNewOpacity(FMath::Clamp(Strength, 0.02f, 1.0f)), Depth, FMath::Max(Width, 1.0f), 0.0f, /*bScreenSpace*/ true);
	}

	/**
	 * The quiet wires of what is not being edited: patches' cages,
	 * primitives' edges, placed prefabs' both and those placed inside them,
	 * lifted towards the eye as the cage is.
	 */
	struct FSurfQuietWires
	{
		FPrimitiveDrawInterface* PDI = nullptr;
		const FSceneView* View = nullptr;
		FLinearColor Colour = FLinearColor::White;
		uint8 Depth = SDPG_World;
		float Width = 0.0f;
		float Strength = 1.0f;
		double LiftBy = 0.004;

		FVector Raise(const FVector& World) const
		{
			// A flat view has no eye to lift towards: straight at the viewer,
			// by the same share of the view's width.
			if (!View->IsPerspectiveProjection())
			{
				const double ViewWidth = 2.0 / FMath::Max(1.e-9, View->ViewMatrices.GetViewToClip().M[0][0]);
				return World - View->GetViewDirection() * (ViewWidth * LiftBy);
			}
			const FVector Towards = View->ViewMatrices.GetViewOrigin() - World;
			const double Distance = Towards.Size();
			return Distance < KINDA_SMALL_NUMBER ? World : World + (Towards / Distance) * (Distance * LiftBy);
		}

		void DrawPatch(const FSurfPatch& Patch, const FTransform& ToWorld) const
		{
			if (!Patch.HasValidDimensions())
			{
				return;
			}
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 0; X < Patch.Width; ++X)
				{
					const FVector P = Raise(ToWorld.TransformPosition(Patch.At(X, Y).Pos));
					if (X + 1 < Patch.Width)
					{
						SurfWire(PDI, P, Raise(ToWorld.TransformPosition(Patch.At(X + 1, Y).Pos)), Colour, Depth, Width, Strength);
					}
					if (Y + 1 < Patch.Height)
					{
						SurfWire(PDI, P, Raise(ToWorld.TransformPosition(Patch.At(X, Y + 1).Pos)), Colour, Depth, Width, Strength);
					}
				}
			}
		}

		void DrawBrush(const FSurfBrush& Brush, const FTransform& ToWorld) const
		{
			FSurfHull Hull;
			if (!SurfBrushMath::DeriveHull(Brush, Hull))
			{
				return;
			}
			for (const FSurfHullFace& HullFace : Hull.Faces)
			{
				const int32 Count = HullFace.VertexIndices.Num();
				for (int32 I = 0; I < Count; ++I)
				{
					SurfWire(PDI, Raise(ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[I]])),
						Raise(ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[(I + 1) % Count]])),
						Colour, Depth, Width, Strength);
				}
			}
		}

		void DrawPrefab(const USurfPrefab& Prefab, const FTransform& ToWorld, int32 Nesting) const
		{
			Prefab.ForEachPatch([&](const FSurfPatch& Patch) { DrawPatch(Patch, ToWorld); });
			Prefab.ForEachBrush([&](const FSurfBrush& Brush) { DrawBrush(Brush, ToWorld); });
			if (Nesting >= 8)
			{
				return;
			}
			for (const FSurfPlacement& Placement : Prefab.Placements)
			{
				const USurfPrefab* Inner = Placement.Prefab.Get();
				if (Inner && Inner != &Prefab)
				{
					DrawPrefab(*Inner, Placement.Transform * ToWorld, Nesting + 1);
				}
			}
		}
	};
}

void USurfEdMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	const double RenderStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		SampleFrame((FPlatformTime::Seconds() - RenderStart) * 1000.0, Viewport);
	};

	ApplyGizmoSize();
	CheckForFlatPatches();
	SyncGameView();
	SyncInstanceLooks();

	// The game view shows the game (S230a): nothing of ours drawn in it,
	// unless the setting keeps it.
	if (IsGameViewOf(Viewport))
	{
		return;
	}
	DrawLattice(View, PDI);

	// Matcap is a view of its own (S179a): picked Lit, or any other view
	// mode, from the editor's menu, the clay goes - on the next tick, never
	// inside the draw.
	{
		const USurfSettings* ViewSettings = GetDefault<USurfSettings>();
		if (ViewSettings && ViewSettings->bMatcap && !bMatcapLeaving && !IsModellingView())
		{
			bMatcapLeaving = true;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) -> bool
			{
				bMatcapLeaving = false;
				const USurfSettings* Now = GetDefault<USurfSettings>();
				if (Now && Now->bMatcap && !IsModellingView())
				{
					SetMatcap(false);
				}
				return false;
			}), 0.0f);
		}
	}

	// Behind everything, then the floor (S172b).
	DrawBackdrop(View, PDI);
	DrawGrid(View, PDI);

	// Ours, over everything; the editor's is not drawn while it is on.
	DrawOwnGizmo(View, Viewport, PDI);
	DrawSnapMarker(View, PDI);
	DrawPivot(View, PDI);
	DrawPrefabMarkers(View, PDI);

	// The room (S71a): the prefab's origin as an axis cross - where a hand
	// puts it down - and its footprint on the floor, so what is being
	// edited and what it is measured from both read.
	if (PDI && IsInsidePrefab())
	{
		if (const ASurfGroup* Room = DrilledGroup.Get())
		{
			const FTransform& Frame = RoomFrame;
			const FVector O = Frame.GetLocation();
			const double Arm = 60.0;
			PDI->DrawLine(O, Frame.TransformPosition(FVector(Arm, 0.0, 0.0)), FLinearColor(0.95f, 0.25f, 0.2f), SDPG_Foreground, 2.0f);
			PDI->DrawLine(O, Frame.TransformPosition(FVector(0.0, Arm, 0.0)), FLinearColor(0.35f, 0.9f, 0.25f), SDPG_Foreground, 2.0f);
			PDI->DrawLine(O, Frame.TransformPosition(FVector(0.0, 0.0, Arm)), FLinearColor(0.3f, 0.5f, 1.0f), SDPG_Foreground, 2.0f);

			FBox Local(ForceInit);
			Room->ForEachPatch([&Local](const FSurfPatch& Patch)
			{
				for (const FSurfControlPoint& Point : Patch.Points)
				{
					Local += Point.Pos;
				}
			});
			if (Local.IsValid)
			{
				const double Z = Local.Min.Z;
				const FVector A = Frame.TransformPosition(FVector(Local.Min.X, Local.Min.Y, Z));
				const FVector B = Frame.TransformPosition(FVector(Local.Max.X, Local.Min.Y, Z));
				const FVector C = Frame.TransformPosition(FVector(Local.Max.X, Local.Max.Y, Z));
				const FVector D = Frame.TransformPosition(FVector(Local.Min.X, Local.Max.Y, Z));
				const FLinearColor Amber(1.0f, 0.75f, 0.2f, 0.9f);
				PDI->DrawLine(A, B, Amber, SDPG_World, 1.5f);
				PDI->DrawLine(B, C, Amber, SDPG_World, 1.5f);
				PDI->DrawLine(C, D, Amber, SDPG_World, 1.5f);
				PDI->DrawLine(D, A, Amber, SDPG_World, 1.5f);
			}

			// The placements (S71b): a small cross at each one's origin, and
			// the picked one's box, so what is inside this prefab and what
			// Alt+PgDn would open both read.
			const ASurfPrefabInstance* Drilled = DrilledInstance.Get();
			const USurfPrefab* RoomAsset = Drilled ? Drilled->Prefab.Get() : nullptr;
			if (RoomAsset)
			{
				for (const FSurfPlacement& Placement : RoomAsset->Placements)
				{
					const FTransform At = Placement.Transform * Frame;
					const FVector PO = At.GetLocation();
					const bool bPicked = Placement.Id == RoomPickedPlacement;
					const FLinearColor Mark = bPicked ? FLinearColor(1.0f, 0.75f, 0.2f) : FLinearColor(0.75f, 0.6f, 0.25f, 0.6f);
					const double Half = bPicked ? 30.0 : 18.0;
					PDI->DrawLine(At.TransformPosition(FVector(-Half, 0.0, 0.0)), At.TransformPosition(FVector(Half, 0.0, 0.0)), Mark, SDPG_Foreground, 1.5f);
					PDI->DrawLine(At.TransformPosition(FVector(0.0, -Half, 0.0)), At.TransformPosition(FVector(0.0, Half, 0.0)), Mark, SDPG_Foreground, 1.5f);
					PDI->DrawLine(PO, At.TransformPosition(FVector(0.0, 0.0, Half)), Mark, SDPG_Foreground, 1.5f);
					if (bPicked && Placement.Prefab)
					{
						const FBox Box = Placement.Prefab->LocalBounds();
						if (Box.IsValid)
						{
							FVector Corners[8];
							Box.GetVertices(Corners);
							for (FVector& Corner : Corners)
							{
								Corner = At.TransformPosition(Corner);
							}
							static const int32 Edges[12][2] = { {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4}, {0,4},{1,5},{2,6},{3,7} };
							for (const auto& Edge : Edges)
							{
								PDI->DrawLine(Corners[Edge[0]], Corners[Edge[1]], Mark, SDPG_World, 1.5f);
							}
						}
					}
				}
			}
		}
	}

	if (bSlicing)
	{
		const FLinearColor SliceColour(0.2f, 0.9f, 1.0f);

		for (const FVector& Point : SlicePoints)
		{
			// A cross rather than a dot, so the exact snapped position reads
			// at any zoom.
			const double R = 12.0;
			PDI->DrawLine(Point - FVector(R, 0, 0), Point + FVector(R, 0, 0), SliceColour, SDPG_Foreground, 2.0f, 0.0f, true);
			PDI->DrawLine(Point - FVector(0, R, 0), Point + FVector(0, R, 0), SliceColour, SDPG_Foreground, 2.0f, 0.0f, true);
			PDI->DrawLine(Point - FVector(0, 0, R), Point + FVector(0, 0, R), SliceColour, SDPG_Foreground, 2.0f, 0.0f, true);
		}

		for (int32 I = 0; I + 1 < SlicePoints.Num(); ++I)
		{
			PDI->DrawLine(SlicePoints[I], SlicePoints[I + 1], SliceColour, SDPG_Foreground, 2.0f, 0.0f, true);
		}

		// The cut itself, drawn where the plane crosses every brush it would
		// split: a convex face meets a plane in one segment, and the
		// segments close round the brush. What Enter will do, before it does.
		FPlane Cut;
		if (GetSlicePlane(Cut))
		{
			for (const TPair<ASurfGroup*, TArray<FGuid>>& Pair : SliceTargets())
			{
				const FTransform ToWorld = Pair.Key->GetActorTransform();
				for (const FGuid& Id : Pair.Value)
				{
					const FSurfBrush* Brush = Pair.Key->FindBrush(Id);
					FSurfHull Hull;
					if (!Brush || !SurfBrushMath::DeriveHull(*Brush, Hull))
					{
						continue;
					}
					for (const FSurfHullFace& HullFace : Hull.Faces)
					{
						TArray<FVector, TInlineAllocator<4>> Crossings;
						const int32 Count = HullFace.VertexIndices.Num();
						for (int32 I = 0; I < Count; ++I)
						{
							const FVector A = ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[I]]);
							const FVector B = ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[(I + 1) % Count]]);
							const double DA = Cut.PlaneDot(A);
							const double DB = Cut.PlaneDot(B);
							if ((DA < 0.0) != (DB < 0.0) && !FMath::IsNearlyEqual(DA, DB))
							{
								Crossings.Add(FMath::Lerp(A, B, DA / (DA - DB)));
							}
						}
						if (Crossings.Num() >= 2)
						{
							PDI->DrawLine(Crossings[0], Crossings[1], SliceColour, SDPG_Foreground, 3.0f, 0.0f, true);
						}
					}
				}
			}
		}
	}

	// The overlay off (S199a): "show-hide wireframe? even selection wire
	// toggles off" - the surfaces alone from here on.
	if (bOverlayHidden)
	{
		return;
	}

	// The active patch and every patch the selection reaches into: the
	// control grid, and the points when editing them. A selection may span
	// patches now, and a selected point on a patch whose cage is not drawn
	// is a selected point nobody can see.
	TArray<FSurfPatchRef> ToDraw;
	if (SelectedPatch.IsValid())
	{
		ToDraw.Add(SelectedPatch);
	}
	for (const FSurfPatchRef& Ref : PatchSelection)
	{
		if (Ref.IsValid() && !ToDraw.Contains(Ref))
		{
			ToDraw.Add(Ref);
		}
	}
	for (const FSurfPointRef& Ref : PointSelection)
	{
		FSurfPatchRef Key;
		Key.Group = Ref.Group;
		Key.PatchId = Ref.PatchId;
		if (Key.IsValid() && !ToDraw.Contains(Key))
		{
			ToDraw.Add(Key);
		}
	}

	// Wires and points are depth-tested like the surfaces they sit on, so
	// what is behind a surface is hidden with it and what can be seen is
	// what can be picked - unless asked to see through, or in x-ray, where
	// selection reaches through too.
	const USurfSettings* Wires = GetDefault<USurfSettings>();
	const ESceneDepthPriorityGroup WireDepth =
		(Wires && (Wires->bSeeThroughWires || Wires->WireView == ESurfWireView::XRay)) ? SDPG_Foreground : SDPG_World;
	// How strong the wires are (S249a); what is picked keeps its full colour.
	const float WireStrength = Wires ? FMath::Clamp(Wires->WireStrength, 0.05f, 1.0f) : 1.0f;

	// Where points of other patches sit, in millimetre cells, so a point
	// that has met another - on any patch - can be drawn as met.
	TMap<FIntVector, int32> Occupancy;
	auto CellOf = [](const FVector& World)
	{
		return FIntVector(FMath::RoundToInt(World.X * 10.0), FMath::RoundToInt(World.Y * 10.0), FMath::RoundToInt(World.Z * 10.0));
	};
	if (Wires && Wires->bShowOverlaps && (SubMode == ESurfSubMode::Vertex || SubMode == ESurfSubMode::Edge))
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				const ASurfGroup* Group = *It;
				if (!Group || !IsOpen(Group))
				{
					continue;
				}
				const FTransform ToWorld = Group->GetActorTransform();
				Group->ForEachPatch([&](const FSurfPatch& Patch)
				{
					for (const FSurfControlPoint& Point : Patch.Points)
					{
						++Occupancy.FindOrAdd(CellOf(ToWorld.TransformPosition(Point.Pos)));
					}
				});
			}
		}
	}

	/**
	 * Every brush's edges, in the brush colour (S100a).
	 *
	 * A brush is a solid described by planes; a patch is a grid of points.
	 * They are edited by different verbs and they break in different ways,
	 * and until now they looked identical - "Brushes (box, cylinder, etc.)
	 * integration is really off. it should be visible its a different type."
	 *
	 * Lines only, and nothing pickable: the picking is the mesh's, as it was.
	 * The wire view decides how many of them are drawn, the same way it does
	 * for patches, so a level full of blockout does not become a cage.
	 */
	if (Wires && View)
	{
		if (UWorld* World = GetWorld())
		{
			const bool bEveryBrush = Wires->WireView != ESurfWireView::Selected;
			const FLinearColor BrushColour = Wires->BrushWireColour;
			// Lifted towards the eye as the patch cage is (S225a: "the
			// primitive edges almost look like they flickering against the
			// model") - the edge lies in the face it outlines.
			const FVector BrushEye = View->ViewMatrices.GetViewOrigin();
			const double BrushLiftBy = Wires->CageLift;
			auto BrushLift = [&BrushEye, BrushLiftBy, View](const FVector& World) -> FVector
			{
				if (!View->IsPerspectiveProjection())
				{
					const double ViewWidth = 2.0 / FMath::Max(1.e-9, View->ViewMatrices.GetViewToClip().M[0][0]);
					return World - View->GetViewDirection() * (ViewWidth * BrushLiftBy);
				}
				const FVector Towards = BrushEye - World;
				const double Distance = Towards.Size();
				return Distance < KINDA_SMALL_NUMBER ? World : World + (Towards / Distance) * (Distance * BrushLiftBy);
			};

			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				const ASurfGroup* Group = *It;
				if (!Group || !IsOpen(Group))
				{
					continue;
				}
				const FTransform ToWorld = Group->GetActorTransform();

				Group->ForEachBrush([&](const FSurfBrush& Brush)
				{
					// The ones being worked on: a face of theirs is picked or
					// is the active one, or a corner of theirs is picked.
					bool bWorked = ActiveFace.IsValid() && ActiveFace.BrushId == Brush.Id;
					for (const FSurfFaceRef& Ref : Selection)
					{
						if (Ref.BrushId == Brush.Id) { bWorked = true; break; }
					}
					for (const FSurfBrushCornerRef& Ref : BrushCornerSelection)
					{
						if (Ref.BrushId == Brush.Id) { bWorked = true; break; }
					}
					if (!bEveryBrush && !bWorked)
					{
						return;
					}

					FSurfHull Hull;
					if (!SurfBrushMath::DeriveHull(Brush, Hull))
					{
						return;
					}

					for (const FSurfHullFace& HullFace : Hull.Faces)
					{
						const int32 Count = HullFace.VertexIndices.Num();
						for (int32 I = 0; I < Count; ++I)
						{
							const FVector A = BrushLift(ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[I]]));
							const FVector B = BrushLift(ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[(I + 1) % Count]]));
							SurfWire(PDI, A, B, BrushColour, WireDepth, 1.0f, bWorked ? 1.0f : WireStrength);
						}
					}

					/**
					 * And its corners, in vertex sub-mode (S113a): "we need be
					 * able to select brushes points (vertices) and edit them
					 * freely."
					 *
					 * Drawn only where they can be clicked, which is the rule
					 * that keeps a viewport honest - a handle you can see and
					 * cannot grab is worse than no handle.
					 */
					// In 2 as well (S166a): the picked edge's corners are drawn
					// so the edge reads as picked. Only on a primitive being
					// worked on (S225a: "why are the primitive verts staying
					// on screen even if its unselected") - the wire view may
					// show every primitive's edges, never every one's corners.
					if (bWorked && (SubMode == ESurfSubMode::Vertex || (SubMode == ESurfSubMode::Edge && BrushCornerSelection.Num() > 0)))
					{
						for (int32 I = 0; I < Hull.Vertices.Num(); ++I)
						{
							FSurfBrushCornerRef Corner;
							Corner.Group = const_cast<ASurfGroup*>(Group);
							Corner.BrushId = Brush.Id;
							Corner.Corner = I;

							const bool bPicked = BrushCornerSelection.Contains(Corner);
							PDI->DrawPoint(BrushLift(ToWorld.TransformPosition(Hull.Vertices[I])),
								bPicked ? FLinearColor(1.0f, 0.62f, 0.12f) : BrushColour,
								bPicked ? 9.0f : 6.0f, WireDepth);
						}
					}
				});
			}
		}
	}

	// Every other patch's wires, in a quiet tint, when the wire view asks
	// for them: the shape of the whole level, with the pieces being edited
	// standing out from it. Lines only - no points, nothing pickable.
	FSurfQuietWires Quiet;
	if (Wires && View)
	{
		Quiet.PDI = PDI;
		Quiet.View = View;
		Quiet.Colour = Wires->InactiveGridColour;
		Quiet.Depth = WireDepth;
		Quiet.Width = Wires->ControlGridLineWidth <= 1.0f ? 0.0f : Wires->ControlGridLineWidth;
		Quiet.Strength = WireStrength;
		Quiet.LiftBy = Wires->CageLift;
	}
	if (Quiet.PDI && Wires->WireView != ESurfWireView::Selected)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ASurfGroup> It(World); It; ++It)
			{
				const ASurfGroup* Group = *It;
				if (!Group || !IsOpen(Group))
				{
					continue;
				}
				const FTransform ToWorld = Group->GetActorTransform();
				Group->ForEachPatch([&](const FSurfPatch& Patch)
				{
					FSurfPatchRef Key;
					Key.Group = const_cast<ASurfGroup*>(Group);
					Key.PatchId = Patch.Id;
					if (!ToDraw.Contains(Key))
					{
						Quiet.DrawPatch(Patch, ToWorld);
					}
				});
			}

			// Placed prefabs too (S248a: "pressing Z don't show wire on
			// prefabs"): their patches and primitives, and those of every
			// prefab placed inside them, in the same quiet tint.
			for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
			{
				const ASurfPrefabInstance* Instance = *It;
				if (!Instance || !Instance->Prefab || Instance->IsHiddenEd() || Instance->bGhosted
					|| Instance == DrilledInstance.Get() || IsRoomContext(Instance))
				{
					continue;
				}
				Quiet.DrawPrefab(*Instance->Prefab, Instance->GetActorTransform(), 0);
			}
		}
	}

	// The context round a prefab edited where it stands, as wires (S249a:
	// "it may be wire only"), whatever the wire view says.
	if (Quiet.PDI)
	{
		for (const TWeakObjectPtr<AActor>& Weak : RoomWiredHidden)
		{
			if (const ASurfGroup* Group = Cast<ASurfGroup>(Weak.Get()))
			{
				const FTransform ToWorld = Group->GetActorTransform();
				Group->ForEachPatch([&](const FSurfPatch& Patch) { Quiet.DrawPatch(Patch, ToWorld); });
				Group->ForEachBrush([&](const FSurfBrush& Brush) { Quiet.DrawBrush(Brush, ToWorld); });
			}
			else if (const ASurfPrefabInstance* Instance = Cast<ASurfPrefabInstance>(Weak.Get()))
			{
				if (Instance->Prefab)
				{
					Quiet.DrawPrefab(*Instance->Prefab, Instance->GetActorTransform(), 0);
				}
			}
		}
	}

	// The groups picked whole, bracketed at their corners (S249b: "have
	// some subtle visual feedback for grouped").
	if (View)
	{
		TArray<FBox> Bracketed;
		PickedGroupBounds(Bracketed);
		TArray<FLinearColor> BracketTints;
		BracketTints.Init(FLinearColor(0.80f, 0.85f, 0.95f, 0.55f), Bracketed.Num());
		// Every group, fainter (S251a: "expose the bbox display for groups
		// too? off by default?").
		const USurfSettings* GroupMarks = GetDefault<USurfSettings>();
		if (GroupMarks && GroupMarks->bShowGroupBounds)
		{
			TArray<FBox> AllGroups;
			AllGroupBounds(AllGroups);
			for (const FBox& EachGroup : AllGroups)
			{
				if (!Bracketed.Contains(EachGroup))
				{
					Bracketed.Add(EachGroup);
					BracketTints.Add(FLinearColor(0.80f, 0.85f, 0.95f, 0.22f));
				}
			}
		}
		for (int32 BracketIndex = 0; BracketIndex < Bracketed.Num(); ++BracketIndex)
		{
			const FBox& Raw = Bracketed[BracketIndex];
			const FLinearColor& Bracket = BracketTints[BracketIndex];
			const FBox Box = Raw.ExpandBy(FMath::Max(2.0, Raw.GetSize().GetMax() * 0.03));
			const FVector Size = Box.GetSize();
			const FVector Reach(FMath::Min(Size.X * 0.2, 80.0), FMath::Min(Size.Y * 0.2, 80.0), FMath::Min(Size.Z * 0.2, 80.0));
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector At((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
				const FVector Inward((Corner & 1) ? -1.0 : 1.0, (Corner & 2) ? -1.0 : 1.0, (Corner & 4) ? -1.0 : 1.0);
				PDI->DrawTranslucentLine(At, At + FVector(Inward.X * Reach.X, 0.0, 0.0), Bracket, SDPG_World, 1.5f, 0.0f, true);
				PDI->DrawTranslucentLine(At, At + FVector(0.0, Inward.Y * Reach.Y, 0.0), Bracket, SDPG_World, 1.5f, 0.0f, true);
				PDI->DrawTranslucentLine(At, At + FVector(0.0, 0.0, Inward.Z * Reach.Z), Bracket, SDPG_World, 1.5f, 0.0f, true);
			}
		}
	}

	for (const FSurfPatchRef& DrawRef : ToDraw)
	{
		const ASurfGroup* PatchGroup = DrawRef.Group.Get();
		const FSurfPatch* Patch = PatchGroup ? PatchGroup->FindPatch(DrawRef.PatchId) : nullptr;
		if (PatchGroup && Patch)
		{
			const FTransform ToWorld = PatchGroup->GetActorTransform();

			// Weight and colour come from settings, defaulting to a thin dark
			// cage: the surface should read as the geometry, and the cage as
			// something laid over it.
			const USurfSettings* Display = GetDefault<USurfSettings>();
			FLinearColor GridColour = Display ? Display->ControlGridColour : FLinearColor(0.05f, 0.05f, 0.05f);

			// A colour a patch, from its id (S72e): the same patch is the
			// same colour every session, and two patches beside each other
			// are told apart at a glance.
			if (Display && Display->bColourIdWires)
			{
				const uint32 Hash = GetTypeHash(Patch->Id);
				GridColour = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash & 0xFF), 190, 235);
			}

			// Thickness in pixels, not centimetres. Without bScreenSpace a line
			// is a world-space tube: a hairline from across the level and a
			// slab from up close, wide enough to swallow the control points.
			// At one pixel and under, a hairline: thickness zero asks for the
			// hardware line, one crisp pixel wide, where any thickness above
			// zero is drawn as a smoothed strip that reads as bold. Selected
			// segments keep the width and change colour, so the cage never
			// changes weight when something is picked.
			const float Configured = Display ? Display->ControlGridLineWidth : 1.0f;
			const float GridWidth = Configured <= 1.0f ? 0.0f : Configured;

			/**
			 * Lifted towards the camera before drawing.
			 *
			 * On a sheet the control points ARE the surface vertices, so the
			 * cage lies exactly in the surface and the two fight for the same
			 * depth - which is the shimmer along every line as the view moves.
			 * The foreground draw group was not enough on its own. A lift
			 * proportional to distance is the same size on screen wherever the
			 * camera is, so it never becomes a visible gap.
			 */
			const FVector ViewOrigin = View ? View->ViewMatrices.GetViewOrigin() : FVector::ZeroVector;
			const double LiftFraction = Display ? Display->CageLift : 0.004f;

			auto Lift = [&ViewOrigin, LiftFraction, View](const FVector& World) -> FVector
			{
				// A flat view: straight at the viewer, by a share of the view's width.
				if (View && !View->IsPerspectiveProjection())
				{
					const double ViewWidth = 2.0 / FMath::Max(1.e-9, View->ViewMatrices.GetViewToClip().M[0][0]);
					return World - View->GetViewDirection() * (ViewWidth * LiftFraction);
				}

				const FVector Towards = ViewOrigin - World;
				const double Distance = Towards.Size();

				if (Distance < KINDA_SMALL_NUMBER)
				{
					return World;
				}

				return World + (Towards / Distance) * (Distance * LiftFraction);
			};

			// A curve's real surface, as thin lines under the cage. The cage
			// of a curve is not its surface, and editing the one without
			// seeing the other is guesswork.
			if (Display && Display->bShowSurfaceWireframe && Patch->Type == ESurfPatchType::Curve)
			{
				FSurfTessellation Tess;
				if (SurfPatchMath::Tessellate(*Patch, Tess))
				{
					const FLinearColor Wire = Display->SurfaceWireframeColour;

					for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
					{
						const FVector A = Lift(ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I]]));
						const FVector B = Lift(ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I + 1]]));
						const FVector C = Lift(ToWorld.TransformPosition(Tess.Positions[Tess.Indices[I + 2]]));

						// Two edges of each triangle: the third is another
						// triangle's first, so this covers the grid once.
						SurfWire(PDI, A, B, Wire, WireDepth, 0.0f, WireStrength);
						SurfWire(PDI, A, C, Wire, WireDepth, 0.0f, WireStrength);
					}
				}
			}

			// The corners of every picked face, four at a time, filled in one
			// pass once the grid has been walked.
			TArray<FVector> FaceFill;

			/**
			 * The seam of a closed patch, in a quieter tint (S162a): "on a
			 * shape like the sides of a box, or a cylinder, the edge of the
			 * seam should be in a very subtle color so I know where the seam
			 * edge is." A patch whose first and last column (or row) stand on
			 * the same points is closed there; the two borders that meet are
			 * one line to the eye and nothing said which. The border's
			 * segments take a warmer grey, so the seam reads without a word.
			 */
			bool bSeamAcross = Patch->Width >= 3;
			for (int32 Y = 0; bSeamAcross && Y < Patch->Height; ++Y)
			{
				bSeamAcross = Patch->At(0, Y).Pos.Equals(Patch->At(Patch->Width - 1, Y).Pos, 0.05);
			}
			bool bSeamDown = Patch->Height >= 3;
			for (int32 X = 0; bSeamDown && X < Patch->Width; ++X)
			{
				bSeamDown = Patch->At(X, 0).Pos.Equals(Patch->At(X, Patch->Height - 1).Pos, 0.05);
			}
			// Fainter, not warmer (S188a): "the overlapping edges ... in a much more
			// faded wireframe color so we see it's the overlap one".
			const FLinearColor SeamColour(GridColour.R * 0.38f, GridColour.G * 0.38f, GridColour.B * 0.40f, GridColour.A);
			auto OnSeam = [&](int32 AX, int32 AY, int32 BX, int32 BY)
			{
				const bool bAlongColumn = AX == BX;
				return (bSeamAcross && bAlongColumn && (AX == 0 || AX == Patch->Width - 1))
					|| (bSeamDown && !bAlongColumn && (AY == 0 || AY == Patch->Height - 1));
			};

			// The control grid, not the tessellated surface: what you drag is
			// what you see.
			for (int32 Y = 0; Y < Patch->Height; ++Y)
			{
				for (int32 X = 0; X < Patch->Width; ++X)
				{
					const FVector P = Lift(ToWorld.TransformPosition(Patch->At(X, Y).Pos));

					// A segment counts as selected when both its ends are, which
					// is what makes an edge selection visible as a line rather
					// than as two dots.
					auto Chosen = [this, Patch, &DrawRef](int32 AX, int32 AY, int32 BX, int32 BY)
					{
						FSurfPointRef One;
						One.Group = DrawRef.Group;
						One.PatchId = DrawRef.PatchId;
						One.X = AX;
						One.Y = AY;

						FSurfPointRef Two = One;
						Two.X = BX;
						Two.Y = BY;

						return PointSelection.Contains(One) && PointSelection.Contains(Two);
					};

					const FLinearColor Hot = Display ? Display->SelectedControlPointColour : FLinearColor(1.0f, 0.45f, 0.0f);
					const FLinearColor SoftColour = Display ? Display->SoftSelectionColour : FLinearColor(1.0f, 0.1f, 0.05f);

					// A segment inside the falloff is tinted by how far its ends
					// follow, in every sub-mode - so soft selection reads the
					// same whether the points are drawn or not.
					auto SegmentColour = [&](int32 AX, int32 AY, int32 BX, int32 BY, bool bOn) -> FLinearColor
					{
						if (bOn)
						{
							return Hot;
						}

						FSurfPointRef A;
						A.Group = DrawRef.Group;
						A.PatchId = DrawRef.PatchId;
						A.X = AX;
						A.Y = AY;

						FSurfPointRef B = A;
						B.X = BX;
						B.Y = BY;

						const float Influence = FMath::Max(GetSoftInfluence(A), GetSoftInfluence(B));
						const FLinearColor Rest = OnSeam(AX, AY, BX, BY) ? SeamColour : GridColour;
						return Influence > 0.0f ? FMath::Lerp(Rest, SoftColour, Influence) : Rest;
					};

					if (X + 1 < Patch->Width)
					{
						const bool bOn = Chosen(X, Y, X + 1, Y);
						SurfWire(PDI, P, Lift(ToWorld.TransformPosition(Patch->At(X + 1, Y).Pos)),
							SegmentColour(X, Y, X + 1, Y, bOn), WireDepth,
							GridWidth, bOn ? 1.0f : WireStrength);
					}
					if (Y + 1 < Patch->Height)
					{
						const bool bOn = Chosen(X, Y, X, Y + 1);
						SurfWire(PDI, P, Lift(ToWorld.TransformPosition(Patch->At(X, Y + 1).Pos)),
							SegmentColour(X, Y, X, Y + 1, bOn), WireDepth,
							GridWidth, bOn ? 1.0f : WireStrength);
					}

					/**
					 * A picked face is shaded, not crossed.
					 *
					 * The first version drew the two diagonals, which reads as
					 * two more lines on a surface already made of lines - not
					 * as "this quad is selected". A wash of colour over the
					 * cell is what every other tool does and what the eye
					 * reads without being told.
					 */
					if (SubMode == ESurfSubMode::Face && X + 1 < Patch->Width && Y + 1 < Patch->Height)
					{
						FSurfQuadRef Quad;
						Quad.Group = DrawRef.Group;
						Quad.PatchId = DrawRef.PatchId;
						Quad.X = X;
						Quad.Y = Y;

						if (QuadSelection.Contains(Quad))
						{
							const FLinearColor Base = Display ? Display->FaceSelectionColour : FLinearColor(1.0f, 0.45f, 0.0f);

							const FVector A = P;
							const FVector B = Lift(ToWorld.TransformPosition(Patch->At(X + 1, Y).Pos));
							const FVector C = Lift(ToWorld.TransformPosition(Patch->At(X + 1, Y + 1).Pos));
							const FVector D = Lift(ToWorld.TransformPosition(Patch->At(X, Y + 1).Pos));

							// Collected and filled after the loop, as one mesh.
							// The first version hatched the cell with closely
							// spaced lines, because the draw interface cannot
							// fill a polygon - and hatching on a surface
							// already made of lines reads as more lines, which
							// is exactly what it looked like.
							FaceFill.Add(A);
							FaceFill.Add(B);
							FaceFill.Add(C);
							FaceFill.Add(D);

							// An outline as well, so the edge of the selection
							// is crisp against the wash.
							PDI->DrawLine(A, B, Base, WireDepth, GridWidth * 2.0f, 0.0f, true);
							PDI->DrawLine(B, C, Base, WireDepth, GridWidth * 2.0f, 0.0f, true);
							PDI->DrawLine(C, D, Base, WireDepth, GridWidth * 2.0f, 0.0f, true);
							PDI->DrawLine(D, A, Base, WireDepth, GridWidth * 2.0f, 0.0f, true);
						}
					}

					// Vertex mode shows vertices, edge mode shows edges, face
					// mode shows faces. Drawing the points in every sub-mode
					// meant three sub-modes all looked like vertex mode, so
					// there was no way to tell from the screen which one was
					// running.
					if (SubMode == ESurfSubMode::Vertex)
					{
						FSurfPointRef Ref;
						Ref.Group = DrawRef.Group;
						Ref.PatchId = DrawRef.PatchId;
						Ref.X = X;
						Ref.Y = Y;

						const bool bSelected = PointSelection.Contains(Ref);

						// A point inside the falloff is tinted by how far it
						// follows, so the soft selection is visible before it
						// is dragged rather than after.
						const float Influence = bSelected ? 1.0f : GetSoftInfluence(Ref);

						FLinearColor PointColour = Display ? Display->ControlPointColour : FLinearColor(0.05f, 0.05f, 0.05f);
						float PointSize = Display ? Display->ControlPointSize : 5.0f;

						if (bSelected)
						{
							PointColour = Display ? Display->SelectedControlPointColour : FLinearColor(1.0f, 0.45f, 0.0f);
							PointSize = Display ? Display->SelectedControlPointSize : 8.0f;
						}
						else if (Influence > 0.0f)
						{
							const FLinearColor Soft = Display ? Display->SoftSelectionColour : FLinearColor(1.0f, 0.1f, 0.05f);
							PointColour = FMath::Lerp(PointColour, Soft, Influence);
							PointSize = FMath::Lerp(PointSize, Display ? Display->SelectedControlPointSize : 8.0f, Influence);
						}
						else if (Occupancy.Num() > 0)
						{
							// Met by another point: the whole overlap colour,
							// and a little larger - a point that sits on
							// another is what a join or a weld is looking for.
							const int32* Count = Occupancy.Find(CellOf(ToWorld.TransformPosition(Patch->At(X, Y).Pos)));
							const int32 Others = Count ? *Count - 1 : 0;
							if (Others > 0)
							{
								PointColour = Wires ? Wires->OverlapColour : FLinearColor(0.0f, 1.0f, 0.0f);
								PointSize = PointSize * 1.3f;
							}
						}

						PDI->DrawPoint(P, PointColour, PointSize, WireDepth);
					}
				}
			}

			/**
			 * The wash over the picked faces, as one translucent mesh.
			 *
			 * The draw interface a mode is given can draw lines and points
			 * but not filled polygons, so this needs a material and a mesh
			 * builder rather than a call. Built once for every picked face
			 * together, because a builder per quad would be a draw call per
			 * quad.
			 */
			if (FaceFill.Num() >= 4 && View && GEngine && GEngine->GeomMaterial)
			{
				/**
				 * The editor's own translucent geometry material, tinted per
				 * draw, the way the level viewport shades a volume. Nothing
				 * is built here: the first version built a material of its
				 * own on the first face ever picked - from inside this draw -
				 * and compiling it ran the garbage collector in the middle of
				 * the frame. That was the 2026-09-12 crash.
				 */
				{
					const FLinearColor Base = Display ? Display->FaceSelectionColour : FLinearColor(1.0f, 0.45f, 0.0f);
					const float Opacity = Display ? Display->FaceSelectionOpacity : 0.25f;

					// Registered with the draw interface, which deletes it once
					// the frame is done with it.
					FDynamicColoredMaterialRenderProxy* Wash = new FDynamicColoredMaterialRenderProxy(
						GEngine->GeomMaterial->GetRenderProxy(), Base.CopyWithNewOpacity(Opacity));
					PDI->RegisterDynamicResource(Wash);

					FDynamicMeshBuilder Builder(View->GetFeatureLevel());

					for (int32 I = 0; I + 3 < FaceFill.Num(); I += 4)
					{
						const int32 Base0 = I;

						for (int32 Corner = 0; Corner < 4; ++Corner)
						{
							Builder.AddVertex(FDynamicMeshVertex(
								FVector3f(FaceFill[Base0 + Corner]),
								FVector2f(0.0f, 0.0f),
								FColor::White));
						}

						Builder.AddTriangle(Base0 + 0, Base0 + 1, Base0 + 2);
						Builder.AddTriangle(Base0 + 0, Base0 + 2, Base0 + 3);
					}

					// Two-sided, so a face picked from behind is still shaded.
					Builder.Draw(PDI, FMatrix::Identity, Wash,
						WireDepth, /*bDisableBackfaceCulling*/ true, /*bReceivesDecals*/ false);
				}
			}
		}
	}

	// The loop about to be inserted, drawn where it would land.
	if (bPreviewingLoop && LoopIndex != INDEX_NONE)
	{
		if (const ASurfGroup* LoopGroup = SelectedPatch.Group.Get())
		{
			if (const FSurfPatch* LoopPatch = LoopGroup->FindPatch(SelectedPatch.PatchId))
			{
				const FTransform ToWorld = LoopGroup->GetActorTransform();
				const FLinearColor LoopColour(1.0f, 0.85f, 0.1f);

				// Midway between the line the cursor named and the next one:
				// exactly where InsertRow and InsertColumn put it.
				const int32 Span = bLoopIsColumn ? LoopPatch->Height : LoopPatch->Width;

				for (int32 I = 0; I + 1 < Span; ++I)
				{
					auto Midpoint = [&](int32 Along) -> FVector
					{
						const FVector A = bLoopIsColumn
							? LoopPatch->At(LoopIndex, Along).Pos
							: LoopPatch->At(Along, LoopIndex).Pos;

						const FVector B = bLoopIsColumn
							? LoopPatch->At(FMath::Min(LoopIndex + 1, LoopPatch->Width - 1), Along).Pos
							: LoopPatch->At(Along, FMath::Min(LoopIndex + 1, LoopPatch->Height - 1)).Pos;

						return ToWorld.TransformPosition((A + B) * 0.5);
					};

					PDI->DrawLine(Midpoint(I), Midpoint(I + 1), LoopColour, SDPG_Foreground, 3.0f, 0.0f, true);
				}

				// And on into the joined patches it will carry into (S190a),
				// found again only when the line under the cursor changes.
				const FString Key = FString::Printf(TEXT("%s %d %d %d"), *SelectedPatch.PatchId.ToString(), LoopIndex, bLoopIsColumn ? 1 : 0, LoopPatch->EditSerial);
				if (Key != LoopChainKey)
				{
					LoopChainKey = Key;
					LoopChainPreview = TraceLoopChain(LoopGroup, SelectedPatch.PatchId, bLoopIsColumn, LoopIndex);
				}
				for (const FSurfLoopLink& Link : LoopChainPreview)
				{
					const ASurfGroup* LinkGroup = Link.Group.Get();
					const FSurfPatch* LinkPatch = LinkGroup ? LinkGroup->FindPatch(Link.PatchId) : nullptr;
					const int32 LinkLines = LinkPatch ? (Link.bColumn ? LinkPatch->Width : LinkPatch->Height) : 0;
					if (!LinkPatch || Link.At + 1 >= LinkLines)
					{
						continue;
					}
					const FTransform LinkToWorld = LinkGroup->GetActorTransform();
					const int32 Ends = Link.bColumn ? LinkPatch->Height : LinkPatch->Width;
					auto Middle = [&](int32 Along)
					{
						const FVector A = Link.bColumn ? LinkPatch->At(Link.At, Along).Pos : LinkPatch->At(Along, Link.At).Pos;
						const FVector B = Link.bColumn ? LinkPatch->At(Link.At + 1, Along).Pos : LinkPatch->At(Along, Link.At + 1).Pos;
						return LinkToWorld.TransformPosition((A + B) * 0.5);
					};
					for (int32 I = 0; I + 1 < Ends; ++I)
					{
						PDI->DrawLine(Middle(I), Middle(I + 1), LoopColour, SDPG_Foreground, 3.0f, 0.0f, true);
					}
				}
			}
		}
	}

	// The soft-selection radius, as a circle around the selection, facing the
	// camera. A number in a panel does not tell you what it reaches; this does.
	// In every kind (S166a): "in mode 1 i can see the soft radius appear
	// while scrolling. in 3 it didnt show." Faces and edges are points too.
	if (SubMode != ESurfSubMode::Object && (PointSelection.Num() > 0 || Selection.Num() > 0) && View)
	{
		const USurfSettings* Soft = GetDefault<USurfSettings>();

		// Only while it is being changed. A ring that never leaves stops being
		// read and starts being scenery.
		const double Now = FPlatformTime::Seconds();
		const bool bRecentlyChanged = Soft && (Now - SoftRadiusShownAt) < Soft->SoftRadiusHintSeconds;
		// The snap reach the same way (S170a): "add a visible radius when
		// ctrl + scroll, like the one for softselection" - a dotted ring
		// in the snap's own colour, for as long as the soft one stays.
		const bool bSnapRecentlyChanged = Soft && (Now - SnapReachShownAt) < Soft->SoftRadiusHintSeconds;

		auto Ring = [&](double Radius, const FLinearColor& Colour, bool bDotted)
		{
			const FVector Centre = GetWidgetLocation();
			const FMatrix ViewMatrix = View->ViewMatrices.GetWorldToView();
			const FVector Right(ViewMatrix.M[0][0], ViewMatrix.M[1][0], ViewMatrix.M[2][0]);
			const FVector Up(ViewMatrix.M[0][1], ViewMatrix.M[1][1], ViewMatrix.M[2][1]);
			constexpr int32 Steps = 48;
			FVector Previous = Centre + Right * Radius;
			for (int32 I = 1; I <= Steps; ++I)
			{
				const double Angle = (2.0 * PI * I) / Steps;
				const FVector Point = Centre + (Right * FMath::Cos(Angle) + Up * FMath::Sin(Angle)) * Radius;
				if (!bDotted || (I % 2) == 0)
				{
					PDI->DrawLine(Previous, Point, Colour, SDPG_Foreground, 1.0f, 0.0f, /*bScreenSpace*/ true);
				}
				Previous = Point;
			}
		};

		if (Soft && Soft->bSoftSelection && bRecentlyChanged && PointSelection.Num() > 0)
		{
			Ring(FMath::Max(1.0, Soft->SoftSelectionRadiusCm), Soft->SoftSelectionColour, false);
		}
		if (Soft && bSnapRecentlyChanged)
		{
			Ring(FMath::Max(1.0, Soft->PointSnapRadiusCm), FLinearColor(0.35f, 0.85f, 1.0f), true);
		}
	}

	TSet<FGuid> WholeDrawn;
	for (const FSurfFaceRef& Ref : Selection)
	{
		const ASurfGroup* Group = Ref.Group.Get();
		if (!Group)
		{
			continue;
		}

		const FSurfBrush* Brush = Group->FindBrush(Ref.BrushId);
		if (!Brush)
		{
			continue;
		}

		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Brush, Hull))
		{
			continue;
		}

		// In object sub-mode the whole piece is what is picked; one side's
		// edges lit up on top of it read as a face pick (S154a): "while
		// selecting a primitive (box) in 4 mode. it still highlight one
		// face edges. confusing." Every edge of it, then, once (S174a):
		// "in 4 mode selecting a primitive cylinder, i dont see its
		// selected" - nothing was drawn at all.
		if (SubMode == ESurfSubMode::Object)
		{
			if (WholeDrawn.Contains(Ref.BrushId))
			{
				continue;
			}
			WholeDrawn.Add(Ref.BrushId);
			const FTransform WholeToWorld = Group->GetActorTransform();
			const USurfSettings* WholeDisplay = GetDefault<USurfSettings>();
			const FLinearColor WholeColour = WholeDisplay ? WholeDisplay->FaceSelectionColour : FLinearColor(1.0f, 0.45f, 0.0f);
			for (const FSurfHullFace& HullFace : Hull.Faces)
			{
				const int32 Count = HullFace.VertexIndices.Num();
				for (int32 I = 0; I < Count; ++I)
				{
					const int32 IA = HullFace.VertexIndices[I];
					const int32 IB = HullFace.VertexIndices[(I + 1) % Count];
					if (IA > IB)
					{
						continue; // each edge once: it belongs to two faces, wound opposite ways
					}
					PDI->DrawLine(WholeToWorld.TransformPosition(Hull.Vertices[IA]), WholeToWorld.TransformPosition(Hull.Vertices[IB]),
						WholeColour, SDPG_Foreground, 2.0f, 0.0f, /*bScreenSpace*/ true);
				}
			}
			continue;
		}

		const FTransform ToWorld = Group->GetActorTransform();
		const bool bActive = (Ref == ActiveFace);
		const USurfSettings* FaceDisplay = GetDefault<USurfSettings>();
		const FLinearColor Colour = bActive
			? (FaceDisplay ? FaceDisplay->ActiveFaceColour : FLinearColor(1.0f, 0.85f, 0.1f))
			: (FaceDisplay ? FaceDisplay->FaceSelectionColour : FLinearColor(1.0f, 0.45f, 0.0f));

		for (const FSurfHullFace& HullFace : Hull.Faces)
		{
			if (HullFace.SourceFaceIndex != Ref.FaceIndex)
			{
				continue;
			}

			const int32 Count = HullFace.VertexIndices.Num();
			for (int32 I = 0; I < Count; ++I)
			{
				const FVector A = ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[I]]);
				const FVector B = ToWorld.TransformPosition(Hull.Vertices[HullFace.VertexIndices[(I + 1) % Count]]);
				PDI->DrawLine(A, B, Colour, SDPG_Foreground, bActive ? 3.0f : 2.0f, 0.0f, /*bScreenSpace*/ true);
			}
		}
	}
}

// --- Slice -----------------------------------------------------------------

void USurfEdMode::BeginSlice()
{
	bSlicing = true;
	SlicePoints.Reset();
	SliceFaces.Reset();

	// The gesture, said once, where the eye is: the tool was silent before,
	// and a silent tool that wants two clicks and a key looks broken.
	if (Inspector)
	{
		Inspector->SetResult(FString::Printf(TEXT("cut: click two points on a primitive for a vertical cut through both, three for any plane; %s or Enter cuts (Ctrl keeps the back only, Shift the front); Escape puts it away"),
			*SurfKeymap::Describe(ESurfAction::Slice)));
	}
	SurfSession::Verb(TEXT("slice"), TEXT("armed"));
}

void USurfEdMode::CancelSlice()
{
	bSlicing = false;
	SlicePoints.Reset();
	SliceFaces.Reset();
}

void USurfEdMode::AddSlicePoint(const FVector& World, const FSurfFaceRef& On)
{
	if (!bSlicing)
	{
		return;
	}
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Step = Settings ? Settings->GetGridStepCm(Settings->DefaultGridIndex) : 100.0;
	SlicePoints.Add(USurfSettings::SnapToStep(World, Step));
	SliceFaces.Add(On);

	if (Inspector)
	{
		FPlane Cut;
		if (GetSlicePlane(Cut))
		{
			int32 Brushes = 0;
			for (const TPair<ASurfGroup*, TArray<FGuid>>& Pair : SliceTargets())
			{
				Brushes += Pair.Value.Num();
			}
			Inspector->SetResult(Brushes > 0
				? FString::Printf(TEXT("cut: the cut is drawn across %d primitive%s - %s or Enter cuts, a third point tilts the plane"),
					Brushes, Brushes == 1 ? TEXT("") : TEXT("s"), *SurfKeymap::Describe(ESurfAction::Slice))
				: FString(TEXT("cut: the cut crosses no primitive - select faces, or place the points on the primitive to cut")));
		}
		else
		{
			Inspector->SetResult(TEXT("cut: one point down, one more for a vertical cut through both"));
		}
	}
}

TMap<ASurfGroup*, TArray<FGuid>> USurfEdMode::SliceTargets() const
{
	// The brushes the selected faces belong to; with nothing selected, the
	// brushes the points were placed on - so a cut is two clicks on the
	// brush and Enter, no selecting first.
	TMap<ASurfGroup*, TArray<FGuid>> Targets;
	auto Take = [&Targets](const FSurfFaceRef& Ref)
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (Group && Ref.BrushId.IsValid() && Group->FindBrush(Ref.BrushId))
		{
			Targets.FindOrAdd(Group).AddUnique(Ref.BrushId);
		}
	};
	for (const FSurfFaceRef& Ref : Selection)
	{
		Take(Ref);
	}
	if (Targets.Num() == 0)
	{
		for (const FSurfFaceRef& Ref : SliceFaces)
		{
			Take(Ref);
		}
	}
	return Targets;
}

void USurfEdMode::CommitSliceKeeping(bool bCtrl, bool bShift)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const ESurfSliceKeep Keep = Settings ? Settings->SliceKeep : ESurfSliceKeep::Both;
	const bool bKeepFront = bShift ? true : bCtrl ? false : Keep != ESurfSliceKeep::Back;
	const bool bKeepBack = bCtrl ? true : bShift ? false : Keep != ESurfSliceKeep::Front;
	CommitSlice(bKeepFront, bKeepBack);
}

void USurfEdMode::PressSlice()
{
	if (!bSlicing)
	{
		BeginSlice();
		return;
	}
	FPlane Cut;
	if (!GetSlicePlane(Cut))
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("cut: click two points on the primitive first, then press again"));
		}
		return;
	}
	CommitSliceKeeping(false, false);
}

bool USurfEdMode::GetSlicePlane(FPlane& OutPlane) const
{
	if (SlicePoints.Num() == 2)
	{
		// Two points imply a vertical cut through both, which is the common
		// case: slicing a wall or a floor plan seen from any angle.
		const FVector Along = SlicePoints[1] - SlicePoints[0];
		const FVector Normal = FVector::CrossProduct(Along, FVector(0.0, 0.0, 1.0)).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return false;
		}
		OutPlane = FPlane(Normal, FVector::DotProduct(Normal, SlicePoints[0]));
		return true;
	}

	if (SlicePoints.Num() >= 3)
	{
		const FVector Normal = FVector::CrossProduct(
			SlicePoints[1] - SlicePoints[0],
			SlicePoints[2] - SlicePoints[0]).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return false;
		}
		OutPlane = FPlane(Normal, FVector::DotProduct(Normal, SlicePoints[0]));
		return true;
	}

	return false;
}

void USurfEdMode::CommitSlice(bool bKeepFront, bool bKeepBack)
{
	FPlane Cut;
	if (!bSlicing || !GetSlicePlane(Cut))
	{
		if (Inspector && bSlicing)
		{
			Inspector->SetResult(TEXT("cut: two points are needed before it can cut"));
		}
		CancelSlice();
		return;
	}

	// Slice acts on the brushes the selected faces belong to, else on the
	// brushes the points were placed on.
	TMap<ASurfGroup*, TArray<FGuid>> Targets = SliceTargets();

	if (Targets.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("cut: nothing to cut - select faces, or place the points on the primitive"));
		}
		SurfSession::Friction(TEXT("slice_no_target"));
		CancelSlice();
		return;
	}

	// The cut face wears what the active face wears when the active face
	// is on the brush being cut - so a marble block cut in two shows marble
	// on the cut, not the checker - else the brush's first face's, as it
	// always did.
	const ASurfGroup* ActiveGroup = ActiveFace.Group.Get();
	const FSurfBrush* ActiveBrush = ActiveGroup ? ActiveGroup->FindBrush(ActiveFace.BrushId) : nullptr;
	const FSurfFace* Wearing = ActiveBrush && ActiveBrush->Faces.IsValidIndex(ActiveFace.FaceIndex)
		? &ActiveBrush->Faces[ActiveFace.FaceIndex] : nullptr;
	const FGuid WearingBrush = Wearing ? ActiveFace.BrushId : FGuid();
	const TObjectPtr<UMaterialInterface> WearingMaterial = Wearing ? Wearing->Material : nullptr;
	const FVector2D WearingScale = Wearing ? Wearing->UV.Scale : FVector2D(1.0, 1.0);
	const int32 WearingSmoothing = Wearing ? Wearing->SmoothingGroup : 0;

	FScopedTransaction Transaction(NSLOCTEXT("SurfacingProEdMode", "Slice", "Slice"));

	TArray<FSurfFaceRef> NewSelection;
	int32 Cut_Count = 0;
	int32 Pieces = 0;
	int32 Missed = 0;

	for (TPair<ASurfGroup*, TArray<FGuid>>& Pair : Targets)
	{
		ASurfGroup* Group = Pair.Key;
		const FTransform ToWorld = Group->GetActorTransform();

		// Primitives are in the actor's local space; the cut was picked in world.
		const FVector LocalPoint = ToWorld.InverseTransformPosition(Cut.GetOrigin());
		const FVector LocalNormal = ToWorld.InverseTransformVector(FVector(Cut.X, Cut.Y, Cut.Z)).GetSafeNormal();
		const FPlane LocalCut(LocalNormal, FVector::DotProduct(LocalNormal, LocalPoint));

		for (const FGuid& BrushId : Pair.Value)
		{
			const FSurfBrush* Original = Group->FindBrush(BrushId);
			if (!Original)
			{
				continue;
			}

			FSurfBrush Front, Back;
			bool bFrontValid = false, bBackValid = false;
			SurfBrushMath::SplitBrush(*Original, LocalCut, Front, bFrontValid, Back, bBackValid);

			// A cut that misses the brush leaves it alone rather than deleting it.
			if (!bFrontValid || !bBackValid)
			{
				++Missed;
				continue;
			}

			const bool bWearing = BrushId == WearingBrush;
			Group->RemoveBrush(BrushId);

			auto AddHalf = [&](FSurfBrush& Half)
			{
				// The cut face is the one just added, and it is the face you
				// almost always want to surface next.
				const int32 CutFaceIndex = Half.Faces.Num() - 1;
				FSurfFace& CutFace = Half.Faces[CutFaceIndex];
				if (bWearing)
				{
					CutFace.Material = WearingMaterial;
					CutFace.UV.Scale = WearingScale;
					CutFace.SmoothingGroup = WearingSmoothing;
				}
				SurfProjection::Align(CutFace.UV, FVector(CutFace.Plane.X, CutFace.Plane.Y, CutFace.Plane.Z));

				const FGuid NewId = Half.Id;
				Group->AddBrush(MoveTemp(Half));

				FSurfFaceRef Ref;
				Ref.Group = Group;
				Ref.BrushId = NewId;
				Ref.FaceIndex = CutFaceIndex;
				NewSelection.Add(Ref);
				++Pieces;
			};

			if (bKeepFront) { AddHalf(Front); }
			if (bKeepBack)  { AddHalf(Back); }
			++Cut_Count;
		}

		Group->RebuildPreview();
	}

	if (Cut_Count == 0)
	{
		Transaction.Cancel();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("cut: the cut misses every primitive it was given - place the points across the primitive"));
		}
		SurfSession::Friction(TEXT("slice_missed"));
		CancelSlice();
		return;
	}

	// A verb states its resulting selection (D16.1). After a cut, that is the
	// cut face on each surviving piece.
	Selection = MoveTemp(NewSelection);
	ActiveFace = Selection.Num() > 0 ? Selection[0] : FSurfFaceRef();

	if (Inspector)
	{
		const TCHAR* Kept = bKeepFront && bKeepBack ? TEXT("both halves") : bKeepFront ? TEXT("the front half only") : TEXT("the back half only");
		Inspector->SetResult(FString::Printf(TEXT("cut %d primitive%s into %d, %s kept, the cut face%s selected%s"),
			Cut_Count, Cut_Count == 1 ? TEXT("") : TEXT("s"), Pieces, Kept, Pieces == 1 ? TEXT("") : TEXT("s"),
			Missed > 0 ? *FString::Printf(TEXT(" - %d missed"), Missed) : TEXT("")));
		Inspector->RefreshSelectionCount();
		Inspector->PullFromActiveFace();
	}
	SurfSession::Verb(TEXT("slice"), FString::Printf(TEXT("cut=%d pieces=%d front=%d back=%d missed=%d"),
		Cut_Count, Pieces, bKeepFront ? 1 : 0, bKeepBack ? 1 : 0, Missed));

	CancelSlice();
}

bool USurfEdMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!Camera)
	{
		Camera = MakeUnique<FSurfCamera>();
	}

	// X, Y or Z while the rotate ball is being dragged locks the turn to
	// that axis, before those keys mean anything else (S130a).
	if (Event == IE_Pressed && LockGizmoBallToAxis(ViewportClient, Viewport, Key))
	{
		return true;
	}

	// The wheel belongs to whichever gesture is running before it belongs to
	// the camera: the falloff during a soft transform, the count while a loop
	// cut is armed, the brush size while a stroke is armed. Handing it to the
	// camera first zoomed the view when a radius was being changed.
	const bool bWheel = Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown;

	// A left click anywhere in the viewport puts the adjust frame away: the
	// numbers were accepted as they stand. The operation stays; only the
	// frame goes. The other buttons are the camera - orbit, fly, the
	// context menu - and looking around the result is not accepting it.
	if (Event == IE_Pressed && Key == EKeys::LeftMouseButton && Modal == ESurfModal::None)
	{
		bAdjustFrameDismissed = true;
		DismissOptions();
		SurfParamsTab::Dismiss();
	}

	// The wheel while the right button flies the camera is the editor's own
	// speed control, as it is in every Unreal viewport; nothing here takes it.
	if (bWheel && Viewport && Viewport->KeyState(EKeys::RightMouseButton))
	{
		return false;
	}

	const bool bWheelIsAGesture = bWheel && (
		Modal != ESurfModal::None
		|| bPreviewingLoop
		|| bPaintArmed
		|| ((bSelectArmed || bSelectDragging) && ActiveSelectTool == ESurfSelectTool::Brush));

	if (!bWheelIsAGesture && Camera->HandleKey(ViewportClient, Viewport, Key, Event))
	{
		return true;
	}

	// The lattice's cage, while it is being edited, takes the clicks and G R S (S189a).
	if (LatticePatch.IsValid() && HandleLatticeKey(ViewportClient, Viewport, Key, Event))
	{
		return true;
	}

	if (Event == IE_Released && Key == EKeys::LeftMouseButton)
	{
		const bool bWasPressed = bSelectPressed;
		bSelectPressed = false;

		if (bWasPressed && Viewport)
		{
			Viewport->CaptureMouse(false);
		}

		// An Alt-drag orbit ends with its button, and gives the capture back.
		if (Camera && Camera->IsNavigating())
		{
			Camera->EndDrag(ViewportClient);
			if (Viewport)
			{
				Viewport->CaptureMouse(false);
			}
			return true;
		}

		if (bPaintStroking)
		{
			EndPaintStroke();
			if (Viewport)
			{
				Viewport->CaptureMouse(false);
			}
			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		if (bSelectDragging)
		{
			// Ctrl subtracts, as it does everywhere else here.
			const bool bSubtract = Viewport
				&& (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));

			CommitSelectGesture(ViewportClient, Viewport, !bSubtract);

			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		// Not a drag, so it was a click. The editor will not send one - we
		// claimed the press - so it is reproduced here against the same code
		// path a real click takes.
		if (bWasPressed)
		{
			ClickAtCursor(ViewportClient, Viewport);
			return true;
		}

		return false;
	}

	/**
	 * A right CLICK - press and release without moving - opens the context
	 * menu. A right DRAG is still the camera, untouched: the press is never
	 * claimed, only the release is looked at, and only if the mouse stayed
	 * put. A modal transform takes the right button first, as its cancel.
	 */
	if (Key == EKeys::RightMouseButton && Modal == ESurfModal::None && Viewport)
	{
		FIntPoint Cursor;
		Viewport->GetMousePos(Cursor);

		if (Event == IE_Pressed)
		{
			RightPressAt = FVector2D(Cursor.X, Cursor.Y);
			RightPressSeconds = FPlatformTime::Seconds();
			return false;
		}

		if (Event == IE_Released)
		{
			const bool bStayedPut = FVector2D::Distance(RightPressAt, FVector2D(Cursor.X, Cursor.Y)) < 4.0
				&& (FPlatformTime::Seconds() - RightPressSeconds) < 0.5;

			/**
			 * The verb menu, in every sub-mode (S87a).
			 *
			 * It used to refuse in Object, which is the sub-mode a hand
			 * spends half its time in - "i want be able to right click
			 * and-or click some menu instead of having the whole
			 * surfacingpro window open", and there it simply did nothing.
			 * And it opened the old selection menu rather than the pool of
			 * verbs, so two right-click menus existed and the one nobody
			 * asked for won.
			 */
			if (bStayedPut)
			{
				const USurfSettings* MenuSettings = GetDefault<USurfSettings>();
				const bool bShiftHeld = Viewport
					&& (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift));

				if (!bShiftHeld && (!MenuSettings || MenuSettings->bRightClickMenu))
				{
					SurfMenu::OpenAtCursor(this, ViewportClient);
					return true;
				}
				if (!bShiftHeld && SubMode != ESurfSubMode::Object)
				{
					OpenContextMenu(ViewportClient);
					return true;
				}
			}
		}
	}

	// A key that has been let go can start a fresh gesture. Without this, one
	// press of G arrived twice - the second arrival found a move already
	// running and promoted it to a slide, so a plain move was unreachable and
	// every slide looked like it only went one way.
	if (Event == IE_Released)
	{
		if (Key == ModalKeyHeld)
		{
			ModalKeyHeld = EKeys::Invalid;
		}
		return false;
	}

	if (Event != IE_Pressed)
	{
		return false;
	}

	const bool bShift = Viewport && (Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift));
	const bool bCtrl  = Viewport && (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl));
	const bool bAlt   = Viewport && (Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt));

	// A modal transform owns everything until it is confirmed or cancelled.
	// This block is first for exactly that reason.
	if (Modal != ESurfModal::None)
	{
		if (Key == EKeys::Escape || Key == EKeys::RightMouseButton)
		{
			CancelModal();
			return true;
		}

		if (Key == EKeys::Enter || Key == EKeys::LeftMouseButton)
		{
			ConfirmModal();
			return true;
		}

		// The wheel sizes the falloff while a transform is running, which is
		// where proportional editing is actually adjusted - reaching for a
		// bracket key mid-gesture is what made it "too far away". With soft
		// selection off, the first turn of the wheel switches it on: a wheel
		// turned during a move means the falloff, and handing it to the
		// camera instead sent the view off into space mid-gesture.
		if (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown)
		{
			if (Event != IE_Pressed)
			{
				return true;
			}

			// Ctrl+wheel sizes the snap reach instead (S156a): "what's the
			// shortcut for snap radius - like scrolling for the other radius?"
			if (Viewport && (Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl)))
			{
				SurfSnapState::NoteReachWheel();
				NudgeSnapReach(Key == EKeys::MouseScrollUp ? 1.5 : (1.0 / 1.5));
				UpdateModal(ViewportClient, Viewport);
				return true;
			}

			// Wheel up while off turns it on (NudgeSoftRadius does, at the
			// smallest radius); wheel down while off leaves it off - turning
			// it on to shrink it again flickered on and off with every notch.
			const USurfSettings* Settings = GetDefault<USurfSettings>();
			if (Settings && !Settings->bSoftSelection && Key != EKeys::MouseScrollUp)
			{
				// It used to do this in silence, which reads as broken -
				// "wait scrolling while editing dont edit softselection
				// radius no more?" (his note, 2026-09-19).
				if (Inspector)
				{
					Inspector->SetResult(TEXT("soft selection is off - the wheel up turns it on and sets its reach"));
				}
				return true;
			}

			if (NudgeSoftRadius(Key == EKeys::MouseScrollUp ? 1.25 : (1.0 / 1.25)) && Inspector && Settings && Settings->bSoftSelection)
			{
				Inspector->SetResult(FString::Printf(TEXT("soft selection: reach %.0f cm - the wheel sets it, down past the smallest turns it off"), Settings->SoftSelectionRadiusCm));
			}
			UpdateModal(ViewportClient, Viewport);
			return true;
		}

		// Digits, sign and point build an exact value; backspace takes one
		// back, and Escape on an empty entry still cancels the gesture.
		{
			// Keys are named, not lettered: EKeys::Two is "Two", not "2". The
			// first version compared the name against a digit and never
			// matched, so typing a number during a transform did nothing -
			// which the interaction harness caught on its first run.
			static const TPair<FKey, TCHAR> DigitKeys[] = {
				{ EKeys::Zero, TEXT('0') }, { EKeys::One, TEXT('1') }, { EKeys::Two, TEXT('2') },
				{ EKeys::Three, TEXT('3') }, { EKeys::Four, TEXT('4') }, { EKeys::Five, TEXT('5') },
				{ EKeys::Six, TEXT('6') }, { EKeys::Seven, TEXT('7') }, { EKeys::Eight, TEXT('8') },
				{ EKeys::Nine, TEXT('9') },
				{ EKeys::NumPadZero, TEXT('0') }, { EKeys::NumPadOne, TEXT('1') }, { EKeys::NumPadTwo, TEXT('2') },
				{ EKeys::NumPadThree, TEXT('3') }, { EKeys::NumPadFour, TEXT('4') }, { EKeys::NumPadFive, TEXT('5') },
				{ EKeys::NumPadSix, TEXT('6') }, { EKeys::NumPadSeven, TEXT('7') }, { EKeys::NumPadEight, TEXT('8') },
				{ EKeys::NumPadNine, TEXT('9') },
			};

			FString Typed;
			for (const TPair<FKey, TCHAR>& Digit : DigitKeys)
			{
				if (Key == Digit.Key)
				{
					Typed.AppendChar(Digit.Value);
					break;
				}
			}

			if (Typed.IsEmpty())
			{
				if (Key == EKeys::Period || Key == EKeys::Decimal)
				{
					Typed = TEXT(".");
				}
				else if (Key == EKeys::Hyphen || Key == EKeys::Subtract)
				{
					Typed = TEXT("-");
				}
			}

			if (!Typed.IsEmpty())
			{
				ModalTyped[FMath::Clamp(ModalTypedField, 0, 2)] += Typed;
				UpdateModal(ViewportClient, Viewport);
				return true;
			}

			// Tab steps to the next axis, as it does in every numeric entry
			// worth using. A turn and a size have one number each, so Tab is
			// only meaningful during a move.
			if (Key == EKeys::Tab && Modal == ESurfModal::Move)
			{
				ModalTypedField = (ModalTypedField + 1) % 3;
				UpdateModal(ViewportClient, Viewport);
				return true;
			}

			if (Key == EKeys::BackSpace)
			{
				FString& Field = ModalTyped[FMath::Clamp(ModalTypedField, 0, 2)];
				if (Field.Len() > 0)
				{
					Field.LeftChopInline(1);
					UpdateModal(ViewportClient, Viewport);
					return true;
				}
			}
		}

		if (Key == EKeys::X) { SetAxisLock(ESurfAxisLock::X, ViewportClient, Viewport); return true; }
		if (Key == EKeys::Y) { SetAxisLock(ESurfAxisLock::Y, ViewportClient, Viewport); return true; }
		if (Key == EKeys::Z) { SetAxisLock(ESurfAxisLock::Z, ViewportClient, Viewport); return true; }

		/**
		 * Tapping the same key again frees it (S162a): "when pressing a
		 * second time on the key, it enables free editing (free rotation
		 * etc). for a specific axis lock i gotta hit the axis key - g then
		 * y, r then r - and if i hit it again it just cycles through." So
		 * the second tap drops whatever lock is on - a turn with nothing
		 * named is the ball - and X, Y, Z name an axis, then its plane,
		 * then free again (SetAxisLock). It used to walk X, Y, Z itself.
		 */
		const ESurfAction Pressed = SurfKeymap::Match(Key, bCtrl, bShift, bAlt);
		if (Pressed == ESurfAction::Translate || Pressed == ESurfAction::Rotate || Pressed == ESurfAction::Scale)
		{
			// The same press arriving twice is not a second press: it used to
			// be read as one, which turned every move into a slide.
			if (Key == ModalKeyHeld)
			{
				return true;
			}

			ModalKeyHeld = Key;

			const ESurfModal Wanted =
				Pressed == ESurfAction::Translate ? ESurfModal::Move :
				Pressed == ESurfAction::Rotate    ? ESurfModal::Rotate : ESurfModal::Scale;

			/**
			 * A second G with nothing locked slides (S188a): "select edge
			 * ring, press g to move, pressed g again to slide ... it did not
			 * work" - freeing a move that was already free did nothing. With
			 * an axis named, the second tap still frees it (S162a); a third
			 * G goes back to the move.
			 */
			if (Modal == Wanted && Wanted == ESurfModal::Move && AxisLock == ESurfAxisLock::None
				&& !bModalOnBrush && !bModalOnPlacement && !bModalFollowsExtrude)
			{
				BeginModal(ESurfModal::Move, ViewportClient, Viewport);
				SurfSession::Gesture(TEXT("modal_slide"), TEXT("second G"));
				return true;
			}

			if (Modal == Wanted)
			{
				AxisLock = ESurfAxisLock::None;
				UpdateModal(ViewportClient, Viewport);
				SurfSession::Gesture(TEXT("modal_axis"), TEXT("0 (freed by the same key)"));
				return true;
			}

			/**
			 * Out of an extrude's pull into a turn or a size (S184c): "if
			 * extruding and want to go within using scale, then it shall
			 * cancel any previous move". The pull goes back to nothing
			 * first - the extrude stays where it was made - so S sizes the
			 * new faces in place, an inset.
			 */
			if (bModalFollowsExtrude && Modal == ESurfModal::Move && Wanted != ESurfModal::Move && Viewport)
			{
				AxisLock = ESurfAxisLock::None;
				CustomAxis = FVector::ZeroVector;
				ModalTyped[0].Reset();
				ModalTyped[1].Reset();
				ModalTyped[2].Reset();
				ResetModalMouse(Viewport);
				UpdateModal(ViewportClient, Viewport);
				SurfSession::Gesture(TEXT("extrude_pull"), TEXT("back to nothing for a turn or a size"));
			}

			BeginModal(Wanted, ViewportClient, Viewport);
			return true;
		}

		// Slide keeps its own key now that the second tap of G means something
		// else. It is the gesture for moving a loop without leaving the
		// surface it sits on, so it is worth a key of its own.
		if (Pressed == ESurfAction::SlideEdge)
		{
			BeginModal(ESurfModal::Slide, ViewportClient, Viewport);
			return true;
		}

		// Everything else is swallowed, so no stray key escapes mid-gesture.
		return true;
	}

	// The whole press-drag-release belongs to selection, and is claimed here.
	// Declining the press handed it to the editor, which with no widget under
	// the cursor falls back to moving the camera - and with Ctrl held, drags
	// what is selected. Claiming it means no move events arrive on their own,
	// so the uncaptured MouseMove callback carries the drag instead, and a
	// press that never moves is turned back into a click on release.
	// Alt+LMB is the editor's orbit only while Unreal's camera is driving.
	// With the Blender camera the middle button orbits, so Alt+click is free
	// to be what it is in Blender: the whole row or column.
	const bool bAltIsOurs = FSurfCamera::IsBlenderScheme();
	const bool bEditingSubMode = SubMode == ESurfSubMode::Vertex || SubMode == ESurfSubMode::Edge || SubMode == ESurfSubMode::Face;

	// With the brush out, the left button paints: the press is the start of
	// the stroke, and it never becomes a pick. Alt keeps the orbit.
	if (bPaintArmed && Key == EKeys::LeftMouseButton && Event == IE_Pressed && Viewport && !bAlt)
	{
		BeginPaintStroke(ViewportClient, Viewport);
		Viewport->CaptureMouse(true);
		return true;
	}
	// Shift+X while the brush is out swaps the two colours (S238a: "x
	// already busy with xsearch. cant be color swap too") - X stays the
	// search, brush out or not.
	if (Key == EKeys::X && Event == IE_Pressed && !bCtrl && !bAlt && bShift)
	{
		SwapPaintColours();
		if (Inspector)
		{
			Inspector->SetResult(TEXT("paint colours swapped"));
		}
		return true;
	}

	// Alt+LMB is claimed in every sub-mode under the Blender camera, gizmo
	// handle or not: a click picks a line and a drag orbits, and neither is
	// ever the editor's orbit, whose re-centring swing was "the snap".
	if (((ClaimsSelectPress() && (!bAlt || bAltIsOurs)) || (bAlt && bAltIsOurs))
		&& Key == EKeys::LeftMouseButton && Viewport)
	{
		const bool bOnWidget = ViewportClient && ViewportClient->GetCurrentWidgetAxis() != EAxisList::None
			&& !(bAlt && bAltIsOurs);

		// The press on the record (S242a): whether it was ours, and why not.
		if (Event == IE_Pressed)
		{
			SurfSession::Gesture(TEXT("select_press"), FString::Printf(TEXT("claimed=%d widget_axis=%d armed=%d dragging=%d tool=%d submode=%d loop=%d"),
				(!bOnWidget && !bPreviewingLoop) ? 1 : 0, ViewportClient ? static_cast<int32>(ViewportClient->GetCurrentWidgetAxis()) : -1,
				bSelectArmed ? 1 : 0, bSelectDragging ? 1 : 0, static_cast<int32>(ActiveSelectTool), static_cast<int32>(SubMode), bPreviewingLoop ? 1 : 0));
		}

		if (!bOnWidget && !bPreviewingLoop)
		{
			FIntPoint Cursor;
			Viewport->GetMousePos(Cursor);
			SelectPressAt = FVector2D(Cursor.X, Cursor.Y);
			bSelectPressed = true;

			// Two presses close together in time and place are a double-click.
			// The editor would have told us, but only if we had let it deliver
			// the press - and claiming the press is what makes box select work.
			const double Now = FPlatformTime::Seconds();
			bEdgePressWasDouble = (Now - LastEdgePressSeconds) < 0.4
				&& FVector2D::Distance(LastEdgePressAt, SelectPressAt) < 6.0;
			LastEdgePressSeconds = Now;
			LastEdgePressAt = SelectPressAt;

			// Claiming the press stops the editor capturing the mouse, and
			// without capture no move events arrive at all while a button is
			// held - so the path stayed one point wide and every box selected
			// nothing. Take the capture ourselves.
			Viewport->CaptureMouse(true);
			return true;
		}

		return false;
	}

	if (bPaintArmed)
	{
		// The wheel is the camera's unless the brush is being sized on
		// purpose - Ctrl+wheel, or the wheel during a stroke - and [ and ]
		// size it too. Taking every wheel for the radius left no way to zoom
		// with the brush out ("i need be able to move around").
		const bool bWheelHere = (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown) && (bCtrl || bPaintStroking);
		const bool bBracket = (Key == EKeys::LeftBracket || Key == EKeys::RightBracket) && Event == IE_Pressed && !bShift && !bCtrl;
		if (bWheelHere || bBracket)
		{
			const bool bUp = Key == EKeys::MouseScrollUp || Key == EKeys::RightBracket;
			if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
			{
				Settings->BrushSelectRadiusPx = FMath::Clamp(
					Settings->BrushSelectRadiusPx * (bUp ? 1.2 : (1.0 / 1.2)),
					4.0, 400.0);
				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("paint brush %.0f px - drag paints up, Ctrl-drag paints down"), Settings->BrushSelectRadiusPx));
				}
			}
			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		if (Key == EKeys::Escape && Event == IE_Pressed)
		{
			TogglePaint();
			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}
	}

	if (bSelectArmed || bSelectDragging)
	{
		// [ and ] size the select brush whether or not a stroke is running,
		// the same two keys that size the paint brush.
		if (ActiveSelectTool == ESurfSelectTool::Brush
			&& (Key == EKeys::LeftBracket || Key == EKeys::RightBracket)
			&& Event == IE_Pressed && !bShift && !bCtrl)
		{
			if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
			{
				Settings->BrushSelectRadiusPx = FMath::Clamp(
					Settings->BrushSelectRadiusPx * (Key == EKeys::RightBracket ? 1.2 : (1.0 / 1.2)),
					4.0, 400.0);
				Settings->SaveConfig();
				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("select brush %.0f px - Ctrl+wheel, or the wheel while dragging, sizes it too"), Settings->BrushSelectRadiusPx));
				}
			}
			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		/**
		 * The wheel sizes the brush while the brush is being used, and is the
		 * camera's the rest of the time (S96a).
		 *
		 * It used to take every turn of the wheel for as long as the tool was
		 * armed - and the select brush stays armed until it is put away, so
		 * there was no zoom left anywhere in the tool: "cant zoom in zoom
		 * out", "i need somehow be able to still move around zoom etc, but
		 * here the size of the brush is preventing me". Ctrl+wheel, or the
		 * wheel during a stroke, sizes it; [ and ] size it too. The paint
		 * brush learned the same rule in S69.
		 */
		if (ActiveSelectTool == ESurfSelectTool::Brush
			&& (bCtrl || bSelectDragging)
			&& (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown))
		{
			if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
			{
				Settings->BrushSelectRadiusPx = FMath::Clamp(
					Settings->BrushSelectRadiusPx * (Key == EKeys::MouseScrollUp ? 1.2 : (1.0 / 1.2)),
					4.0, 400.0);

				if (Inspector)
				{
					Inspector->SetResult(FString::Printf(TEXT("brush %.0f px"), Settings->BrushSelectRadiusPx));
					Inspector->RefreshSelectionCount();
				}
			}

			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		if (Key == EKeys::Escape)
		{
			CancelSelectGesture();
			if (ViewportClient) { ViewportClient->Invalidate(); }
			return true;
		}
	}

	if (bPreviewingLoop)
	{
		// The wheel sets how many lines the cut will make, which is where the
		// count is decided in every tool that has one - and it costs nothing,
		// because the zoom is not what the hand is doing at that moment.
		if (Key == EKeys::MouseScrollUp || Key == EKeys::MouseScrollDown)
		{
			LoopCount = FMath::Clamp(LoopCount + (Key == EKeys::MouseScrollUp ? 1 : -1), 1, 8);

			if (Inspector)
			{
				Inspector->SetResult(FString::Printf(TEXT("loop cut: %d line%s"),
					LoopCount, LoopCount == 1 ? TEXT("") : TEXT("s")));
				Inspector->RefreshSelectionCount();
			}

			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		if (Key == EKeys::Escape)
		{
			CancelLoopPreview();
			if (ViewportClient)
			{
				ViewportClient->Invalidate();
			}
			return true;
		}

		if (Key == EKeys::Enter)
		{
			return InsertLoopAtCursor(ViewportClient, Viewport);
		}
	}

	// Escape leaves Edit UI (S165a): "while in edit ui mode there shall be
	// a shortcut to exit it." The chips become chips again.
	if (Key == EKeys::Escape && Event == IE_Pressed && SurfVerbLayout::IsEditing() && Modal == ESurfModal::None && !bSlicing)
	{
		SurfVerbLayout::SetEditing(false);
		if (Inspector)
		{
			Inspector->SetResult(TEXT("edit UI: off"));
		}
		return true;
	}

	// A slice in progress owns Enter and Escape; everything else still routes
	// normally, so the mode never traps the user inside a gesture.
	if (bSlicing)
	{
		if (Key == EKeys::Escape)
		{
			CancelSlice();
			return true;
		}

		if (Key == EKeys::Enter)
		{
			// Enter keeps what the setting says; Shift+Enter keeps the
			// front, Ctrl+Enter the back, whatever it says.
			CommitSliceKeeping(bCtrl, bShift);
			return true;
		}
	}

	// While the right button is held the viewport is flying, and W A S D Q E
	// belong to the camera. Select-all firing because the camera was being
	// strafed left is the bug this prevents: a mode may read the keyboard, but
	// not while the user is steering.
	if (Viewport && (Viewport->KeyState(EKeys::RightMouseButton) || Viewport->KeyState(EKeys::MiddleMouseButton)))
	{
		return false;
	}

	const ESurfAction Action = SurfKeymap::Match(Key, bCtrl, bShift, bAlt);
	if (Action == ESurfAction::None)
	{
		return false;
	}

	SurfSession::Key(SurfKeymap::ActionName(Action), SurfKeymap::Describe(Action));

	if (Action == ESurfAction::Translate || Action == ESurfAction::Rotate || Action == ESurfAction::Scale)
	{
		ModalKeyHeld = Key;
	}

	return HandleAction(Action, ViewportClient, Viewport);
}

bool USurfEdMode::HandleAction(ESurfAction Action, FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	switch (Action)
	{
	// 1, 2 and 3 lock the kind the click would otherwise decide; pressing
	// the locked one lets go again. 4 is Object, a different question (S83a).
	case ESurfAction::SubModeObject:
		if (SubMode != ESurfSubMode::Object)
		{
			LastEditKind = SubMode;
		}
		SetSubMode(ESurfSubMode::Object);
		return true;
	// Tab (S175a): "user may switch with tab, or directly with 1 2 3, and
	// tab again to go back". Object <-> the Edit kind last used.
	case ESurfAction::ToggleObjectEdit:
		if (SubMode == ESurfSubMode::Object)
		{
			SetSubMode(LastEditKind == ESurfSubMode::Object ? ESurfSubMode::Face : LastEditKind);
			if (Inspector)
			{
				Inspector->SetResult(IsAutoKind() ? TEXT("edit - the click decides: a vertex, an edge or a face; Tab for Object") : TEXT("edit - Tab for Object"));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Verb(TEXT("object_edit"), TEXT("edit"));
		}
		else
		{
			LastEditKind = SubMode;
			SetSubMode(ESurfSubMode::Object);
			if (Inspector)
			{
				Inspector->SetResult(TEXT("pieces - whole patches and primitives; Tab, or 1 2 3, for Edit"));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Verb(TEXT("object_edit"), TEXT("object"));
		}
		return true;
	case ESurfAction::SubModeFace:   ToggleKindLock(ESurfSubMode::Face);   return true;
	case ESurfAction::FaceOutward:
		FaceSelectionOutward();
		return true;

	case ESurfAction::SubModeVertex: ToggleKindLock(ESurfSubMode::Vertex); return true;
	case ESurfAction::SubModeEdge:   ToggleKindLock(ESurfSubMode::Edge);   return true;

	case ESurfAction::CycleWireView:
		CycleWireView();
		return true;

	case ESurfAction::FocusSelection:
		return FocusSelection(ViewportClient);

	case ESurfAction::SelectAll:
		// Select all means all of whatever is being selected - and when what
		// is being selected is a brush face, that is the brushes' faces, not
		// every patch in the level (S72e).
		// In 4, every piece there is - patches and primitives both (S170a).
		if (SubMode == ESurfSubMode::Object)
		{
			return SelectAllPatches();
		}
		// No patch in the level, only primitives: A takes the primitives, in
		// every kind (S176a) - "A to select all should work regardless patch
		// or no; I got some prim box, in 1, A doesn't work".
		if (((Selection.Num() > 0 || ActiveFace.IsValid()) && !SelectedPatch.IsValid() && PointSelection.Num() == 0)
			|| GetAllPatches().Num() == 0)
		{
			return SelectAllBrushFaces();
		}
		if (SubMode == ESurfSubMode::Face)
		{
			return SelectAllQuads();
		}
		return SelectAllPoints();
	case ESurfAction::InvertSelection: return InvertPointSelection();
	case ESurfAction::GrowSelection:   GrowSelection(true);  return true;
	case ESurfAction::ShrinkSelection: GrowSelection(false); return true;
	case ESurfAction::SelectLinked:    SelectLinkedPatches(); return true;
	case ESurfAction::GroupPieces:
		SurfSession::Gesture(TEXT("key_group"));
		GroupSelection();
		return true;
	case ESurfAction::UngroupPieces:
		SurfSession::Gesture(TEXT("key_ungroup"));
		UngroupSelection();
		return true;
	case ESurfAction::ToggleSidePanel: SurfSidePanel::Toggle(); return true;
	case ESurfAction::ToggleOverlay:
		ToggleOverlay();
		return true;
	case ESurfAction::ToggleSnap:
		if (USurfSettings* Snapping = GetMutableDefault<USurfSettings>())
		{
			Snapping->bSnapEnabled = !Snapping->bSnapEnabled;
			Snapping->SaveConfig();
			if (Inspector) { Inspector->SetResult(Snapping->bSnapEnabled ? TEXT("snap on") : TEXT("snap off")); }
			SurfSession::Verb(TEXT("snap"), Snapping->bSnapEnabled ? TEXT("on") : TEXT("off"));
		}
		return true;

	case ESurfAction::SelectNone:
		ClearPointSelection();
		ClearSelection();
		return true;

	case ESurfAction::Translate:
	case ESurfAction::Rotate:
	case ESurfAction::Scale:
	{
		const USurfSettings* Scheme = GetDefault<USurfSettings>();
		const bool bModalScheme = Scheme && Scheme->KeyScheme == ESurfKeyScheme::Blender;

		// In the Blender scheme these start a modal transform; in the house
		// scheme they pick the gizmo, because that is what W/E/R mean to
		// someone already using this editor.
		// Object sub-mode too: the patch is the thing being moved, and
		// BeginModal takes every point of it.
		// A placement picked inside a prefab is what G, R and S move (S71d).
		if (bModalScheme && IsInsidePrefab() && GetPickedPlacement().IsValid() && GetPointSelection().Num() == 0)
		{
			return BeginModal(
				Action == ESurfAction::Translate ? ESurfModal::Move :
				Action == ESurfAction::Rotate    ? ESurfModal::Rotate : ESurfModal::Scale,
				ViewportClient, Viewport);
		}

		// A brush or its faces picked, and no points: the same keys move the
		// brush, or push the faces (S72c) - "i cant find how to edit them".
		if (bModalScheme && GetPointSelection().Num() == 0 && !GetSelectedPatch().IsValid()
			&& (GetSelection().Num() > 0 || GetActiveFace().IsValid()))
		{
			return BeginModal(
				Action == ESurfAction::Translate ? ESurfModal::Move :
				Action == ESurfAction::Rotate    ? ESurfModal::Rotate : ESurfModal::Scale,
				ViewportClient, Viewport);
		}

		// Nothing of ours picked at all, and the editor holding an actor -
		// a prefab instance, a light, a mesh: the same keys move it (S94a).
		if (bModalScheme && GetPointSelection().Num() == 0 && !GetSelectedPatch().IsValid()
			&& GetSelection().Num() == 0 && !GetActiveFace().IsValid()
			&& GEditor && GEditor->GetSelectedActorCount() > 0)
		{
			return BeginModal(
				Action == ESurfAction::Translate ? ESurfModal::Move :
				Action == ESurfAction::Rotate    ? ESurfModal::Rotate : ESurfModal::Scale,
				ViewportClient, Viewport);
		}

		const bool bCanBeModal = SubMode == ESurfSubMode::Vertex
			|| SubMode == ESurfSubMode::Edge
			|| SubMode == ESurfSubMode::Face
			|| (SubMode == ESurfSubMode::Object && SelectedPatch.IsValid());

		if (bModalScheme && bCanBeModal
			&& (PointSelection.Num() > 0 || BrushCornerSelection.Num() > 0 || SubMode == ESurfSubMode::Object))
		{
			return BeginModal(
				Action == ESurfAction::Translate ? ESurfModal::Move :
				Action == ESurfAction::Rotate    ? ESurfModal::Rotate : ESurfModal::Scale,
				ViewportClient, Viewport);
		}

		// Nothing to transform. G with nothing to move used to fall through
		// to the editor's Game view (S154f); no more (S241a: "i was pressing
		// G to move something i ended up toggling game view. thats wrong") -
		// a missed click and G threw the level into Game view, and inside an
		// open prefab that baked the room into itself. Game view is the Game
		// chip. R and S decline rather than consume, so they still pick the
		// editor's own gizmo.
		// Back again at his word (S251c): "I expect G to move if I have
		// anything selected but if nothing is selected then switch to game
		// view" - the Game chip's own switch.
		if (bModalScheme)
		{
			if (Action == ESurfAction::Translate)
			{
				if (ViewportClient)
				{
					ViewportClient->SetGameView(!ViewportClient->IsInGameView());
					ViewportClient->Invalidate();
				}
				const bool bGame = ViewportClient && ViewportClient->IsInGameView();
				if (Inspector)
				{
					Inspector->SetResult(bGame ? TEXT("Game view - G again for the editor's view") : TEXT("the editor's view"));
					Inspector->RefreshSelectionCount();
				}
				SurfSession::Verb(TEXT("game_view"), FString::Printf(TEXT("%s by=G"), bGame ? TEXT("on") : TEXT("off")));
				return true;
			}
			return false;
		}

		if (ViewportClient)
		{
			ViewportClient->SetWidgetMode(
				Action == ESurfAction::Translate ? UE::Widget::WM_Translate :
				Action == ESurfAction::Rotate    ? UE::Widget::WM_Rotate    : UE::Widget::WM_Scale);
			return true;
		}
		return false;
	}

	case ESurfAction::Slice:
		if (bSlicing) { CancelSlice(); } else { BeginSlice(); }
		return true;

	case ESurfAction::Separate:
		// The same verb the button runs, and it says for itself when there
		// is nothing picked to cut out.
		if (Inspector)
		{
			Inspector->Separate();
			return true;
		}
		return false;

	case ESurfAction::InsertLoop:
		// Arm rather than cut: pressing the key and finding out afterwards
		// where the line went is the thing this replaces.
		if (bPreviewingLoop)
		{
			return InsertLoopAtCursor(ViewportClient, Viewport);
		}
		BeginLoopPreview();
		UpdateLoopPreview(ViewportClient, Viewport);
		// The Parameters window turns to Loop (A19): "where's popup param
		// frame for ctrl r?" - a key is a verb pressed another way.
		SurfParamsTab::Follow(TEXT("Loop"));
		if (ViewportClient)
		{
			ViewportClient->Invalidate();
		}
		return true;

	case ESurfAction::ConvertToSheet:
		return ConvertSelectedPatchToSheet();

	case ESurfAction::SlideEdge:
		return BeginModal(ESurfModal::Slide, ViewportClient, Viewport);

	case ESurfAction::Delete:
		return DeleteSelection();

	case ESurfAction::Weld:
		// Each cluster to its own centre when asked, within the weld
		// distance; otherwise everything picked to one centre.
		if (const USurfSettings* WeldPrefs = GetDefault<USurfSettings>(); WeldPrefs && WeldPrefs->bWeldEachCluster && Inspector)
		{
			return WeldSelectionInClusters(Inspector->WeldToleranceCm);
		}
		return WeldSelectedPoints(/*bAtCentre*/ true);

	case ESurfAction::TeleportToCursor:
		return TeleportToCursor(ViewportClient, Viewport);

	case ESurfAction::Bevel:
		// Runs at once; its width and segments are then in the bar's adjust
		// section, live.
		if (Inspector)
		{
			Inspector->Bevel();
			return true;
		}
		return false;

	case ESurfAction::NextPatch:
		return CyclePatch(1);

	case ESurfAction::PreviousPatch:
		return CyclePatch(-1);

	case ESurfAction::Extrude:
		// Extrude then move, as one gesture: the new line is handed straight
		// to the modal transform and follows the mouse until it is confirmed.
		// Extruding and then stopping dead - leaving the new line exactly on
		// top of the old one, waiting for a gizmo - is what felt wrong.
		// In place: the new line starts exactly on the edge and the move that
		// follows supplies the distance. Offsetting it first put the new line
		// a whole cell away before the mouse had moved, which from a distance
		// looked like it had appeared somewhere else entirely.
		{
			/**
			 * A brush face grows a piece of its own (S142a). In place, a
			 * centimetre out, and handed to the move - the same gesture the
			 * patch side has: extrude and then pull, not extrude and wait.
			 */
			if (GetPointSelection().Num() == 0 && !GetSelectedPatch().IsValid() && GetSelection().Num() > 0)
			{
				const int32 WasAt = UndoPosition();
				{
					FScopedTransaction Growing(NSLOCTEXT("SurfacingProEdMode", "ExtrudeBrush", "Extrude Primitive Face"));
					if (ExtrudeSelectedBrushFaces(1.0) == 0)
					{
						Growing.Cancel();
						return true;
					}
				}

				BeginModal(ESurfModal::Move, ViewportClient, Viewport);

				/**
				 * The pull goes along the side's own normal (S189a): "pressed E
				 * on this face extruded as a triangle". The new piece's lid
				 * followed the mouse freely, so any sideways drift sheared it
				 * into a sliver. Now it pulls straight out; X Y Z still send it
				 * along an axis.
				 */
				if (bModalOnBrush && Modal == ESurfModal::Move)
				{
					bModalPushesFaces = true;
					bModalMovesCorners = false;
				}

				// The move carries the extrude with it: cancelled, or
				// confirmed without moving, and the new piece goes too - a
				// centimetre of brush nobody asked for is a ghost.
				bModalFollowsExtrude = Modal == ESurfModal::Move;
				ExtrudeUndoPosition = WasAt;
				ModalFollowsVerb = TEXT("extrude");
				return true;
			}

			const int32 Before = UndoPosition();
			if (!ExtrudeSelectedEdge(/*bInPlace*/ true))
			{
				return true;
			}

			// The move pulls only the new lines: the falloff would drag the
			// lines they came from along, and those must stay.
			bSoftSuspended = true;
			RefreshSoftSelection();

			BeginModal(ESurfModal::Move, ViewportClient, Viewport);

			// The move that follows carries the extrude with it: cancelled, or
			// confirmed without moving, and the extrude goes too - its new
			// lines sit exactly on the old ones and would stay as ghosts.
			bModalFollowsExtrude = Modal == ESurfModal::Move;
			ExtrudeUndoPosition = Before;
			ModalFollowsVerb = TEXT("extrude");

			// Locked to the way the surface continues, so the new line pulls
			// outward rather than sliding about in the view plane - which
			// from most angles looked like it went off in a wrong direction.
			// X, Y or Z still override it; a second tap of G frees it.
			if (Modal == ESurfModal::Move && !LastExtrudeDirection.IsNearlyZero())
			{
				CustomAxis = LastExtrudeDirection.GetSafeNormal();
				AxisLock = ESurfAxisLock::Custom;
				UpdateModal(ViewportClient, Viewport);
			}
			return true;
		}

	case ESurfAction::Duplicate:
		// A placement picked inside a prefab: the copy is another placement,
		// and it follows the mouse as a duplicated patch does (S71d).
		if (IsInsidePrefab() && GetPickedPlacement().IsValid() && GetPointSelection().Num() == 0)
		{
			const FGuid Copy = DuplicatePickedPlacement() ? GetPickedPlacement() : FGuid();
			if (Copy.IsValid() && BeginModal(ESurfModal::Move, ViewportClient, Viewport))
			{
				// Escape takes the copy back as well as the move, which is
				// what the line promises.
				SetModalPlacementAdded(Copy);
			}
			return true;
		}
		return DuplicateAndMove(ViewportClient, Viewport);

	case ESurfAction::ToggleSoftSelection:
		ToggleSoftSelection();
		return true;

	case ESurfAction::CycleSoftFalloff:
		return CycleSoftFalloff(1);

	case ESurfAction::ToggleShadedView:
		ToggleShadedView();
		return true;

	case ESurfAction::Search:
		OpenSearch();
		return true;

	case ESurfAction::ToggleMaximizeViewport:
		ToggleMaximizeViewport(ViewportClient);
		return true;

	case ESurfAction::FocusAllViewports:
		return FocusAllViewports();

	case ESurfAction::ToggleFourViews:
		ToggleFourViews();
		return true;

	case ESurfAction::HideSelected:
		HidePatches(/*bUnselected*/ false);
		return true;

	case ESurfAction::HideUnselected:
		HidePatches(/*bUnselected*/ true);
		return true;

	case ESurfAction::UnhideAll:
		UnhideAllPatches();
		return true;

	case ESurfAction::IsolateSelection:
		ToggleIsolate(ViewportClient);
		return true;

	case ESurfAction::PrefabIn:
		PrefabIn();
		return true;

	case ESurfAction::PrefabOut:
		PrefabOut();
		return true;

	case ESurfAction::RepeatLastVerb:
		return RepeatLastVerb();

	case ESurfAction::QuickFavorites:
		if (!SurfMenu::OpenQuickFavorites(this, ViewportClient) && Inspector)
		{
			Inspector->SetResult(TEXT("Q: nothing starred yet - right-click a verb, then the star"));
		}
		return true;

	case ESurfAction::SoftRadiusUp:
		return NudgeSoftRadius(1.25);

	case ESurfAction::SoftRadiusDown:
		return NudgeSoftRadius(1.0 / 1.25);

	case ESurfAction::SnapReachUp:
		return NudgeSnapReach(1.5);

	case ESurfAction::SnapReachDown:
		return NudgeSnapReach(1.0 / 1.5);

	case ESurfAction::PaintWeight:
		TogglePaint();
		return true;

	// These pick the tool a drag uses, the way Blender's toolbar does, rather
	// than each being a separate one-shot gesture.
	case ESurfAction::CycleSelectTool:
		return CycleSelectTool();

	case ESurfAction::BoxSelect:
	case ESurfAction::LassoSelect:
	case ESurfAction::BrushSelect:
		// A box or a lasso takes what it covers in every sub-mode - patches
		// in object, faces and edges in theirs (his note: "i need the box
		// select in 4 mode too"). The brush paints points, so it stays with
		// the points.
		if (Action == ESurfAction::BrushSelect && SubMode != ESurfSubMode::Vertex)
		{
			return false;
		}
		SetSelectTool(
			Action == ESurfAction::LassoSelect ? ESurfSelectTool::Lasso :
			Action == ESurfAction::BrushSelect ? ESurfSelectTool::Brush : ESurfSelectTool::Box);
		return true;

	default:
		return false;
	}
}

void USurfEdMode::ForEachSelectedFace(TFunctionRef<void(ASurfGroup&, FSurfBrush&, FSurfFace&)> Visit)
{
	for (const FSurfFaceRef& Ref : Selection)
	{
		ASurfGroup* Group = Ref.Group.Get();
		if (!Group)
		{
			continue;
		}

		// ModifyBrush marks only the owning chunk (D21).
		FSurfBrush* Brush = Group->ModifyBrush(Ref.BrushId);
		if (!Brush || !Brush->Faces.IsValidIndex(Ref.FaceIndex))
		{
			continue;
		}

		Visit(*Group, *Brush, Brush->Faces[Ref.FaceIndex]);
	}
}

void USurfEdMode::RebuildTouchedGroups()
{
	TSet<ASurfGroup*> Touched;
	for (const FSurfFaceRef& Ref : Selection)
	{
		if (ASurfGroup* Group = Ref.Group.Get())
		{
			Touched.Add(Group);
		}
	}
	/**
	 * Corners picked with no side picked (a click on a corner, then A) are
	 * a selection too - the primitive's surface has to follow its corners,
	 * else the cage walks off and the primitive stands ("the edges/verts
	 * moved not the model itself", S232a).
	 */
	for (const FSurfBrushCornerRef& Ref : BrushCornerSelection)
	{
		if (ASurfGroup* Group = Ref.Group.Get())
		{
			Touched.Add(Group);
		}
	}
	for (const FSurfBrushModalTarget& Target : ModalBrushes)
	{
		if (ASurfGroup* Group = Target.Group.Get())
		{
			Touched.Add(Group);
		}
	}

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}
}

#undef LOCTEXT_NAMESPACE
