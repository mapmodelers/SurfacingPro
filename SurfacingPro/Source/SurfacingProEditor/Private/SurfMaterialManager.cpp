// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialManager.h"
#include "SurfWindows.h"
#include "SurfMaterialFiles.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"
#include "SurfTheme.h"

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfMaterialSeed.h"
#include "SurfMaterialsView.h"
#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfTextureLib.h"

#include "AdvancedPreviewScene.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "HAL/FileManager.h"
#include "Widgets/Images/SImage.h"
#include "SurfMaterialThumbnails.h"
#include "Widgets/SWindow.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Views/SListView.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Components/StaticMeshComponent.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformTime.h"
#include "IDetailsView.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "StaticParameterSet.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProMaterialManager"

// --- The preview's rules --------------------------------------------------------------

namespace SurfMaterialPreview
{
	int32 SlotFor(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return 0;
		}
		switch (Material->GetBlendMode())
		{
		case BLEND_Translucent:
		case BLEND_Additive:
		case BLEND_Modulate:
		case BLEND_AlphaComposite:
		case BLEND_AlphaHoldout:
			return 1;
		default:
			// Opaque and masked render on opaque geometry in the engine too.
			return 0;
		}
	}

	FSoftObjectPath MeshPath()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (Settings && !Settings->PreviewMesh.IsNull())
		{
			return Settings->PreviewMesh;
		}
		return FSoftObjectPath(TEXT("/SurfacingPro/Preview/mat_cube.mat_cube"));
	}

	UStaticMesh* LoadMesh()
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(MeshPath().TryLoad()))
		{
			return Mesh;
		}
		if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/SurfacingPro/Preview/mat_cube.mat_cube")))
		{
			return Cube;
		}
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	float FOV()
	{
		// A long lens: at 90 degrees the near face splays and the tiling - the
		// one thing the picture is for - reads differently in every corner.
		return 30.0f;
	}

	FVector ViewLocation()
	{
		return FVector(448.0, 448.0, 336.0);
	}

	FRotator ViewRotation()
	{
		return FRotator(-25.0, -135.0, 0.0);
	}

	bool UsesDisplacement(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return false;
		}
		if (Material->GetName().Contains(TEXT("Dsp")))
		{
			return true;
		}
		const UMaterial* Master = const_cast<UMaterialInterface*>(Material)->GetMaterial();
		return Master && Master != Material && Master->GetName().Contains(TEXT("Dsp"));
	}

	FSoftObjectPath DisplacementMeshPath()
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (Settings && !Settings->PreviewMeshDisplacement.IsNull())
		{
			return Settings->PreviewMeshDisplacement;
		}
		return FSoftObjectPath(TEXT("/SurfacingPro/Preview/mat_cube_dsp.mat_cube_dsp"));
	}

	FSoftObjectPath MeshPathFor(const UMaterialInterface* Material)
	{
		return UsesDisplacement(Material) ? DisplacementMeshPath() : MeshPath();
	}

	UStaticMesh* LoadMeshFor(const UMaterialInterface* Material)
	{
		if (UsesDisplacement(Material))
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(DisplacementMeshPath().TryLoad()))
			{
				return Mesh;
			}
			if (UStaticMesh* Dense = LoadObject<UStaticMesh>(nullptr, TEXT("/SurfacingPro/Preview/mat_cube_dsp.mat_cube_dsp")))
			{
				return Dense;
			}
			// No denser cube yet: the plain one, flat.
		}
		return LoadMesh();
	}
}

// --- The viewport -------------------------------------------------------------------------

class FSurfMaterialPreviewViewportClient : public FEditorViewportClient
{
public:
	FSurfMaterialPreviewViewportClient(FAdvancedPreviewScene* InScene, const TSharedRef<SSurfMaterialPreviewViewport>& InWidget)
		: FEditorViewportClient(nullptr, InScene, StaticCastSharedRef<SEditorViewport>(InWidget))
		, Scene(InScene)
	{
		SetViewMode(VMI_Lit);
		SetRealtime(true);
		ViewFOV = SurfMaterialPreview::FOV();
		FOVAngle = ViewFOV;
		EngineShowFlags.SetGrid(false);
		EngineShowFlags.SetSnap(false);
		bSetListenerPosition = false;
		// An orbit camera, as the engine's own asset editors have: the left button turns
		// about the mesh, the right button moves in and out, the middle button slides, the wheel zooms.
		bUsingOrbitCamera = true;
		Home();
	}

	/** The stance the thumbnails share: 30 degrees, from front-left above, looking at the mesh's middle. */
	void Home()
	{
		ToggleOrbitCamera(true);
		SetLookAtLocation(FVector::ZeroVector);
		SetViewLocation(SurfMaterialPreview::ViewLocation());
		SetViewRotation(SurfMaterialPreview::ViewRotation());
		Invalidate();
	}

	/** Frame what is shown, keeping the angle. */
	void Frame(const FBox& Box)
	{
		if (Box.IsValid)
		{
			FocusViewportOnBox(Box, /*bInstant*/ true);
		}
	}

	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override
	{
		if (EventArgs.Event == IE_Pressed)
		{
			if (EventArgs.Key == EKeys::F)
			{
				Frame(Widget.IsValid() ? Widget.Pin()->MeshBox() : FBox(ForceInit));
				return true;
			}
			if (EventArgs.Key == EKeys::Home)
			{
				Home();
				return true;
			}
			const bool bCtrl = IsCtrlPressed();
			const bool bShift = IsShiftPressed();
			if (bCtrl && EventArgs.Key == EKeys::Z && GEditor)
			{
				if (bShift) { GEditor->RedoTransaction(); } else { GEditor->UndoTransaction(); }
				return true;
			}
			if (bCtrl && EventArgs.Key == EKeys::Y && GEditor)
			{
				GEditor->RedoTransaction();
				return true;
			}
		}
		return FEditorViewportClient::InputKey(EventArgs);
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		if (Scene)
		{
			Scene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	void SetWidget(const TSharedRef<SSurfMaterialPreviewViewport>& InWidget) { Widget = InWidget; }

private:
	FAdvancedPreviewScene* Scene;
	TWeakPtr<SSurfMaterialPreviewViewport> Widget;
};

void SSurfMaterialPreviewViewport::Construct(const FArguments& InArgs)
{
	PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	// No floor: the cube floats, so nothing fights it.
	PreviewScene->SetFloorVisibility(false);

	if (UStaticMesh* Mesh = SurfMaterialPreview::LoadMesh())
	{
		MeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
		MeshComponent->SetStaticMesh(Mesh);
		// Centred on its bounds, so a mesh with an off-centre pivot sits where the camera looks.
		MeshComponent->UpdateBounds();
		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		PreviewScene->AddComponent(MeshComponent, FTransform(-Bounds.Origin));
	}

	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SSurfMaterialPreviewViewport::~SSurfMaterialPreviewViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Viewport = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SSurfMaterialPreviewViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FSurfMaterialPreviewViewportClient>(PreviewScene.Get(), SharedThis(this));
	ViewportClient->SetWidget(SharedThis(this));
	return ViewportClient.ToSharedRef();
}

FBox SSurfMaterialPreviewViewport::MeshBox() const
{
	return MeshComponent ? MeshComponent->Bounds.GetBox() : FBox(ForceInit);
}

void SSurfMaterialPreviewViewport::FrameMesh()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Frame(MeshBox());
	}
}

void SSurfMaterialPreviewViewport::HomeView()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Home();
	}
}

void SSurfMaterialPreviewViewport::SetMaterial(UMaterialInterface* Material)
{
	Shown = Material;
	if (!MeshComponent)
	{
		return;
	}
	// The mesh its kind says: the denser cube for displacement, centred like the first.
	if (UStaticMesh* Mesh = SurfMaterialPreview::LoadMeshFor(Material))
	{
		if (MeshComponent->GetStaticMesh() != Mesh)
		{
			MeshComponent->SetStaticMesh(Mesh);
			MeshComponent->UpdateBounds();
			MeshComponent->SetRelativeLocation(-Mesh->GetBounds().Origin);
			if (ViewportClient.IsValid())
			{
				ViewportClient->Home();
			}
		}
	}
	const int32 Slot = SurfMaterialPreview::SlotFor(Material);
	const int32 NumSlots = MeshComponent->GetNumMaterials();
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		// Null puts the mesh's own material back on the other slot.
		MeshComponent->SetMaterial(Index, Material && Index == Slot ? Material : nullptr);
	}
	MeshComponent->MarkRenderStateDirty();
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SSurfMaterialPreviewViewport::Refresh()
{
	if (MeshComponent)
	{
		MeshComponent->MarkRenderStateDirty();
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SSurfMaterialPreviewViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(MeshComponent);
	Collector.AddReferencedObject(Shown);
}

// --- The texture picker --------------------------------------------------------------------------
//
// A row: the texture's thumbnail, its name on a button that drops a list of
// the project's textures with a search box over it, and a clear. The search
// opens on what the caller says - the base of the texture picked last, so the
// siblings of a set come up on their own - and the list keeps to what the
// caller's filter lets through (Conform textures only).

class SSurfTexturePicker : public SCompoundWidget
{
public:
	using FItemPtr = TSharedPtr<FAssetData>;

	SLATE_BEGIN_ARGS(SSurfTexturePicker) {}
		SLATE_ARGUMENT(TSharedPtr<FAssetThumbnailPool>, ThumbnailPool)
		/** The texture held, as an object path; empty for none. */
		SLATE_ATTRIBUTE(FString, ObjectPath)
		/** The base name of the set to list first (T_Sp_Brick): its textures come at the top, marked; the search opens empty. */
		SLATE_ATTRIBUTE(FString, SetBase)
		/** Picked (an asset) or cleared (an empty FAssetData). */
		SLATE_EVENT(FOnSetObject, OnPicked)
		/** True to leave a texture out of the list. */
		SLATE_EVENT(FOnShouldFilterAsset, OnShouldFilterAsset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Pool = InArgs._ThumbnailPool;
		ObjectPath = InArgs._ObjectPath;
		SetBase = InArgs._SetBase;
		OnPicked = InArgs._OnPicked;
		OnShouldFilterAsset = InArgs._OnShouldFilterAsset;
		SetCanTick(true);

		ChildSlot
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SAssignNew(ThumbnailBox, SBox).WidthOverride(28.0f).HeightOverride(28.0f)
				.ToolTipText(LOCTEXT("PickerOpenTip", "Double-click to open the texture in its editor."))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(Combo, SComboButton)
				.OnGetMenuContent(this, &SSurfTexturePicker::MakeList)
				.OnComboBoxOpened(this, &SSurfTexturePicker::OnOpened)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(this, &SSurfTexturePicker::HeldName)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("PickerClearTip", "No texture: the parent's value."))
				.OnClicked_Lambda([this]() { OnPicked.ExecuteIfBound(FAssetData()); return FReply::Handled(); })
				[
					// The editor's reset arrow (S192f): back to the parent's value.
					SNew(SImage)
					.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
					.DesiredSizeOverride(FVector2D(12.0, 12.0))
				]
			]
		];
		RefreshThumbnail();
	}

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override
	{
		if (ObjectPath.Get() != ShownPath)
		{
			RefreshThumbnail();
		}
	}

	// A double click on the row opens the texture it holds in its editor.
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		const FString Path = ObjectPath.Get();
		if (Path.IsEmpty() || !GEditor)
		{
			return FReply::Unhandled();
		}
		if (UObject* Texture = LoadObject<UObject>(nullptr, *Path))
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Texture);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	// A texture dropped on the row - from the Textures window or the Content Browser - is the pick.
	virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override
	{
		return DroppedTexture(Event).IsValid() ? FReply::Handled() : FReply::Unhandled();
	}

	virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override
	{
		const FAssetData Asset = DroppedTexture(Event);
		if (!Asset.IsValid())
		{
			return FReply::Unhandled();
		}
		OnPicked.ExecuteIfBound(Asset);
		return FReply::Handled();
	}

private:
	static FAssetData DroppedTexture(const FDragDropEvent& Event)
	{
		TSharedPtr<FAssetDragDropOp> Operation = Event.GetOperationAs<FAssetDragDropOp>();
		if (Operation.IsValid())
		{
			for (const FAssetData& Asset : Operation->GetAssets())
			{
				const UClass* Class = Asset.GetClass();
				if (Class && Class->IsChildOf(UTexture::StaticClass()))
				{
					return Asset;
				}
			}
		}
		return FAssetData();
	}

	FText HeldName() const
	{
		const FString Path = ObjectPath.Get();
		if (Path.IsEmpty())
		{
			return LOCTEXT("PickerNone", "none");
		}
		return FText::FromString(FPackageName::ObjectPathToObjectName(Path));
	}

	void RefreshThumbnail()
	{
		ShownPath = ObjectPath.Get();
		if (!ThumbnailBox.IsValid())
		{
			return;
		}
		if (ShownPath.IsEmpty() || !Pool.IsValid())
		{
			ThumbnailBox->SetContent(SNullWidget::NullWidget);
			return;
		}
		const FAssetData Asset = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(ShownPath));
		if (!Asset.IsValid())
		{
			ThumbnailBox->SetContent(SNullWidget::NullWidget);
			return;
		}
		HeldThumbnail = MakeShared<FAssetThumbnail>(Asset, 28, 28, Pool);
		ThumbnailBox->SetContent(HeldThumbnail->MakeThumbnailWidget());
	}

	void OnOpened()
	{
		if (SearchBox.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(SearchBox, EFocusCause::SetDirectly);
		}
	}

	TSharedRef<SWidget> MakeList()
	{
		// Every texture the project has, less what the filter keeps out, by name.
		All.Reset();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UTexture::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.PackageName.ToString().StartsWith(TEXT("/Engine/")))
			{
				continue;
			}
			if (OnShouldFilterAsset.IsBound() && OnShouldFilterAsset.Execute(Asset))
			{
				continue;
			}
			All.Add(MakeShared<FAssetData>(Asset));
		}
		// The set of the texture picked last at the top, then the rest, by name.
		Base = SetBase.Get();
		auto InSet = [this](const FItemPtr& Item) { return !Base.IsEmpty() && Item->AssetName.ToString().StartsWith(Base, ESearchCase::IgnoreCase); };
		All.Sort([&InSet](const FItemPtr& A, const FItemPtr& B)
		{
			const bool bA = InSet(A);
			const bool bB = InSet(B);
			if (bA != bB)
			{
				return bA;
			}
			return A->AssetName.LexicalLess(B->AssetName);
		});

		Query.Reset();
		ApplyQuery();

		return SNew(SBox).WidthOverride(380.0f).HeightOverride(420.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.InitialText(FText::FromString(Query))
				.HintText(LOCTEXT("PickerSearchHint", "Search the project's textures"))
				.OnTextChanged_Lambda([this](const FText& Text) { Query = Text.ToString(); ApplyQuery(); })
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 0.0f, 4.0f, 4.0f)
			[
				SAssignNew(List, SListView<FItemPtr>)
				.ListItemsSource(&Shown)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SSurfTexturePicker::MakeRow)
				.OnSelectionChanged(this, &SSurfTexturePicker::OnRowPicked)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(6.0f, 0.0f, 6.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("%d of %d"), Shown.Num(), All.Num())); })
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
	}

	void ApplyQuery()
	{
		TArray<FString> Words;
		Query.ParseIntoArrayWS(Words);
		Shown.Reset();
		for (const FItemPtr& Item : All)
		{
			const FString Name = Item->AssetName.ToString();
			bool bAll = true;
			for (const FString& Word : Words)
			{
				if (!Name.Contains(Word))
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				Shown.Add(Item);
			}
		}
		if (List.IsValid())
		{
			List->RequestListRefresh();
		}
	}

	TSharedRef<ITableRow> MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
	{
		TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(*Item, 32, 32, Pool);
		RowThumbnails.Add(Thumbnail);
		const bool bInSet = !Base.IsEmpty() && Item->AssetName.ToString().StartsWith(Base, ESearchCase::IgnoreCase);
		return SNew(STableRow<FItemPtr>, Owner)
			.ToolTipText(FText::FromString(Item->GetObjectPathString()))
			.Padding(FMargin(2.0f, 1.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(32.0f).HeightOverride(32.0f)
					[
						Thumbnail->MakeThumbnailWidget()
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromName(Item->AssetName)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(bInSet ? LOCTEXT("PickerSameSet", "same set") : FText::FromName(Item->PackagePath))
					.ColorAndOpacity(bInSet ? SurfTheme::Good() : FSlateColor::UseSubduedForeground())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			];
	}

	void OnRowPicked(FItemPtr Item, ESelectInfo::Type How)
	{
		if (!Item.IsValid() || How == ESelectInfo::Direct)
		{
			return;
		}
		if (Combo.IsValid())
		{
			Combo->SetIsOpen(false);
		}
		OnPicked.ExecuteIfBound(*Item);
	}

	TSharedPtr<FAssetThumbnailPool> Pool;
	TAttribute<FString> ObjectPath;
	TAttribute<FString> SetBase;
	FString Base;
	FOnSetObject OnPicked;
	FOnShouldFilterAsset OnShouldFilterAsset;

	TSharedPtr<SBox> ThumbnailBox;
	TSharedPtr<FAssetThumbnail> HeldThumbnail;
	TArray<TSharedPtr<FAssetThumbnail>> RowThumbnails;
	FString ShownPath;
	TSharedPtr<SComboButton> Combo;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FItemPtr>> List;
	TArray<FItemPtr> All;
	TArray<FItemPtr> Shown;
	FString Query;
};

// --- The window ------------------------------------------------------------------------------

namespace
{
	constexpr double ManagerNoticeSeconds = 5.0;
	constexpr double ManagerReadEverySeconds = 0.5;
	const TCHAR* ManagerConfigSection = TEXT("SurfacingPro.Materials");

	/** The editor's icon for the window's own verbs (S192f), none for the rest. */
	const TCHAR* ManagerIcon(const FText& Label)
	{
		static const TMap<FString, const TCHAR*> Icons = {
			{ TEXT("Duplicate"), TEXT("Icons.Duplicate") },
			{ TEXT("Rename"), TEXT("Icons.Edit") },
			{ TEXT("Delete"), TEXT("Icons.Delete") },
			{ TEXT("Apply"), TEXT("Icons.Check") },
			{ TEXT("Save"), TEXT("Icons.Save") },
			{ TEXT("Revert"), TEXT("Icons.Refresh") },
			{ TEXT("Open in editor"), TEXT("Icons.OpenInExternalEditor") },
		};
		const TCHAR* const* Found = Icons.Find(Label.ToString());
		return Found ? *Found : nullptr;
	}

	/** A button as the editor's toolbars draw one (S192f): its icon, then the word. */
	TSharedRef<SWidget> ManagerButton(const FText& Label, const FText& Tip, TFunction<void()> Run, TAttribute<bool> Enabled = true)
	{
		const TCHAR* Icon = ManagerIcon(Label);
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		if (Icon)
		{
			Content->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(Icon))
				.DesiredSizeOverride(FVector2D(14.0, 14.0))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
		}
		Content->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Label)
		];
		return SNew(SButton)
			.ButtonStyle(&SurfTheme::ToolButton())
			.ToolTipText(Tip)
			.IsEnabled(Enabled)
			.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
			[
				Content
			];
	}

	USurfEdMode* ManagerMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** The properties "More" leaves out: the parameter arrays the rows above already edit, and the parent, picked above them. */
	bool MoreShowsProperty(const FPropertyAndParent& PropertyAndParent)
	{
		static const TSet<FName> Hidden = {
			FName(TEXT("Parent")),
			FName(TEXT("ScalarParameterValues")),
			FName(TEXT("VectorParameterValues")),
			FName(TEXT("DoubleVectorParameterValues")),
			FName(TEXT("TextureParameterValues")),
			FName(TEXT("TextureCollectionParameterValues")),
			FName(TEXT("RuntimeVirtualTextureParameterValues")),
			FName(TEXT("SparseVolumeTextureParameterValues")),
			FName(TEXT("FontParameterValues")),
			FName(TEXT("StaticParametersRuntime")),
			FName(TEXT("StaticParameters")),
		};
		if (Hidden.Contains(PropertyAndParent.Property.GetFName()))
		{
			return false;
		}
		for (const FProperty* Outer : PropertyAndParent.ParentProperties)
		{
			if (Outer && Hidden.Contains(Outer->GetFName()))
			{
				return false;
			}
		}
		return true;
	}
}

void SSurfMaterialManagerView::Construct(const FArguments& InArgs)
{
	SetCanTick(true);

	Thumbnails = MakeShared<FAssetThumbnailPool>(32);
	GConfig->GetBool(ManagerConfigSection, TEXT("ConformOnly"), bConformOnly, GEditorPerProjectIni);
	GConfig->GetInt(ManagerConfigSection, TEXT("ManagerPictures"), PicturesMode, GEditorPerProjectIni);
	GConfig->GetInt(ManagerConfigSection, TEXT("ManagerSort"), SortMode, GEditorPerProjectIni);
	GConfig->GetBool(ManagerConfigSection, TEXT("ManagerFlat"), bFlat, GEditorPerProjectIni);

	FPropertyEditorModule& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs Args;
	Args.bAllowSearch = true;
	Args.bHideSelectionTip = true;
	Args.bShowOptions = false;
	Args.bUpdatesFromSelection = false;
	Args.bLockable = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	More = Properties.CreateDetailView(Args);
	More->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateStatic(&MoreShowsProperty));
	More->OnFinishedChangingProperties().AddSP(this, &SSurfMaterialManagerView::OnMoreChanged);

	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)

		// The library, the manager's own pick.
		+ SSplitter::Slot().Value(0.26f)
		[
			SNew(SVerticalBox)
			// Two menus and the search (S252e), as the Material Library has:
			// the strip of five buttons went under Material.
			+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 2.0f, 4.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SurfWindows::MenuBarPullDown(LOCTEXT("ManagerMenuMaterial", "Material"),
						LOCTEXT("ManagerMenuMaterialTip", "New, duplicate, rename, delete; apply, save, revert; export and import as files."),
						[this]() { return MakeMaterialMenu(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SurfWindows::MenuBarPullDown(LOCTEXT("ManagerMenuView", "View"),
						LOCTEXT("ViewTip", "How the list looks: pictures (none, the rendered ones, the first texture), the order (name, newest), parents as headings or a flat list."),
						[this]() { return MakeViewMenu(); })
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search the library"))
					.OnTextChanged(this, &SSurfMaterialManagerView::OnQueryChanged)
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 2.0f)
			[
				SAssignNew(Tree, STreeView<FNodePtr>)
				.TreeItemsSource(&Roots)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SSurfMaterialManagerView::MakeRow)
				.OnGetChildren(this, &SSurfMaterialManagerView::ChildrenOf)
				.OnSelectionChanged(this, &SSurfMaterialManagerView::OnRowSelected)
				.OnContextMenuOpening(this, &SSurfMaterialManagerView::MakeContextMenu)
				.OnMouseButtonDoubleClick(this, &SSurfMaterialManagerView::OnRowDoubleClicked)
				.OnKeyDownHandler(this, &SSurfMaterialManagerView::OnTreeKeyDown)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(this, &SSurfMaterialManagerView::EmptyText)
				.Visibility(this, &SSurfMaterialManagerView::EmptyVisibility)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
		]

		// The inspector: the buttons, the parameters in a column, the rest under More.
		+ SSplitter::Slot().Value(0.40f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 2.0f)
			[
				SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 2.0f))
				+ SWrapBox::Slot()
				[
					ManagerButton(LOCTEXT("Apply", "Apply"), LOCTEXT("ApplyTip", "Put this material on the level's selection, as a click in the Material Library does."),
						[this]() { ApplyShown(); }, TAttribute<bool>::CreateSP(this, &SSurfMaterialManagerView::HasShown))
				]
				+ SWrapBox::Slot()
				[
					ManagerButton(LOCTEXT("Save", "Save"), LOCTEXT("SaveTip", "Save the instance with the parameters as they are here."),
						[this]() { SaveShown(); }, TAttribute<bool>::CreateSP(this, &SSurfMaterialManagerView::CanSave))
				]
				+ SWrapBox::Slot()
				[
					ManagerButton(LOCTEXT("Revert", "Revert"), LOCTEXT("RevertTip", "Drop the changes made here since the last save: the instance read again from disk."),
						[this]() { RevertShown(); }, TAttribute<bool>::CreateSP(this, &SSurfMaterialManagerView::CanSave))
				]
				+ SWrapBox::Slot()
				[
					ManagerButton(LOCTEXT("Open", "Open in the asset editor"), LOCTEXT("OpenTip", "The editor's own window for the material, for what is not here."),
						[this]() { OpenShown(); }, TAttribute<bool>::CreateSP(this, &SSurfMaterialManagerView::HasShown))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 4.0f)
			[
				SNew(STextBlock).Text(this, &SSurfMaterialManagerView::HeaderText).AutoWrapText(true)
				.ToolTipText(FText::FromString(TEXT("Tick a parameter to override it, untick to inherit. Ctrl+Z / Ctrl+Y undo and redo. Save keeps, Revert reloads, Apply puts it on the selection.\nCube: left drag turns, right drag zooms, middle drag pans, F frames, Home resets.")))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 0.0f, 4.0f, 2.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)
				+ SSplitter::Slot().Value(0.65f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					.Padding(4.0f)
					[
						SAssignNew(ParamsBox, SScrollBox)
					]
				]
				+ SSplitter::Slot().Value(0.35f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					.Visibility(this, &SSurfMaterialManagerView::InstanceVisibility)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 2.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("More", "More"))
							.ToolTipText(LOCTEXT("MoreTip", "The instance's other properties: the base property overrides (blend mode, two-sided...), the physical material, the surfacing info."))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot().FillHeight(1.0f)
						[
							More.ToSharedRef()
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 6.0f)
			[
				SNew(STextBlock).Text(this, &SSurfMaterialManagerView::FootText).AutoWrapText(true)
			]
		]

		// The cube, with its two camera buttons under it.
		+ SSplitter::Slot().Value(0.34f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
				.Padding(2.0f)
				[
					SAssignNew(Preview, SSurfMaterialPreviewViewport)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					ManagerButton(LOCTEXT("Frame", "Frame"), LOCTEXT("FrameTip", "Frame the mesh, keeping the angle (F over the cube)."),
						[this]() { if (Preview.IsValid()) { Preview->FrameMesh(); } })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					ManagerButton(LOCTEXT("HomeView", "Home"), LOCTEXT("HomeViewTip", "Back to the stance the thumbnails use: 30 degrees from front-left above (Home over the cube)."),
						[this]() { if (Preview.IsValid()) { Preview->HomeView(); } })
				]
			]
		]
	];

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.OnAssetAdded().AddSP(this, &SSurfMaterialManagerView::OnAssetChanged);
	Registry.OnAssetRemoved().AddSP(this, &SSurfMaterialManagerView::OnAssetChanged);
	Registry.OnAssetRenamed().AddSP(this, &SSurfMaterialManagerView::OnAssetRenamed);
	Registry.OnFilesLoaded().AddSP(this, &SSurfMaterialManagerView::MarkDirty);

	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	Refresh();

	// It opens on the selection's material - what the selected patch or face wears - else on what the picker had, and goes its own way from there.
	UMaterialInterface* First = SurfMaterialsLib::SelectionMaterial(ManagerMode());
	if (!First)
	{
		First = SurfMaterialsLib::CurrentTarget();
	}
	Show(First);
	if (First)
	{
		SelectByName(First->GetName());
	}
}

SSurfMaterialManagerView::~SSurfMaterialManagerView()
{
	Drag.Reset();
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
	if (More.IsValid())
	{
		More->SetObject(nullptr);
	}
}

FReply SSurfMaterialManagerView::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	const bool bCtrl = Event.IsControlDown() || Event.IsCommandDown();
	if (bCtrl && GEditor)
	{
		if (Event.GetKey() == EKeys::Z)
		{
			if (Event.IsShiftDown()) { GEditor->RedoTransaction(); } else { GEditor->UndoTransaction(); }
			return FReply::Handled();
		}
		if (Event.GetKey() == EKeys::Y)
		{
			GEditor->RedoTransaction();
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(Geometry, Event);
}

void SSurfMaterialManagerView::PostUndo(bool bSuccess)
{
	// The rows read the instance live; the cube and More do not.
	if (UMaterialInstanceConstant* Instance = ShownInstance())
	{
		Instance->PostEditChange();
		bChanged = Instance->GetOutermost()->IsDirty();
	}
	if (Preview.IsValid())
	{
		Preview->Refresh();
	}
	if (More.IsValid())
	{
		More->ForceRefresh();
	}
	bDirty = true;
	Say(TEXT("undone"));
}

void SSurfMaterialManagerView::PostRedo(bool bSuccess)
{
	PostUndo(bSuccess);
	Say(TEXT("redone"));
}

void SSurfMaterialManagerView::Edit(UMaterialInstanceConstant* Instance, const FText& What, TFunctionRef<void()> Change)
{
	if (!Instance)
	{
		return;
	}
	if (Dragging())
	{
		// Inside a slider drag: the drag's transaction already holds it.
		Change();
		Instance->PostEditChange();
		MarkChanged();
		return;
	}
	FScopedTransaction Transaction(What);
	Instance->Modify();
	Change();
	Instance->PostEditChange();
	MarkChanged();
}

void SSurfMaterialManagerView::BeginDrag(UMaterialInstanceConstant* Instance, const FText& What)
{
	if (!Instance || Dragging())
	{
		return;
	}
	Drag = MakeUnique<FScopedTransaction>(What);
	Instance->Modify();
}

void SSurfMaterialManagerView::EndDrag()
{
	Drag.Reset();
}

void SSurfMaterialManagerView::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	const bool bRootChanged = GetDefault<USurfSettings>()->MaterialLibraryRoot != RootRead;
	if (bRootChanged || (bDirty && Now - LastRead > ManagerReadEverySeconds))
	{
		Refresh();
	}
	if (Now - WornCheckedAt > 0.25)
	{
		WornCheckedAt = Now;
		FollowSelection();
	}
}

/**
 * The selection's material, shown as the selection changes (S197a): "matmanager
 * and mat window both should show the currently selected object's material".
 * Only a change of selection moves it; a row picked here stays until then.
 */
void SSurfMaterialManagerView::FollowSelection()
{
	TArray<UMaterialInterface*> Worn;
	SurfMaterialsLib::SelectionMaterials(ManagerMode(), Worn);
	FString Key;
	TArray<FString> Names;
	for (UMaterialInterface* Material : Worn)
	{
		Key += Material->GetPathName();
		Key += TEXT("|");
		Names.Add(Material->GetName());
	}
	if (Key == WornKey)
	{
		return;
	}
	WornKey = Key;
	WornNames = Names;
	if (Worn.Num() > 0 && Worn[0] != Target.Get())
	{
		Show(Worn[0]);
		SelectByName(Worn[0]->GetName());
	}
}

// --- The tree ------------------------------------------------------------------------------------

void SSurfMaterialManagerView::Refresh()
{
	RootRead = GetDefault<USurfSettings>()->MaterialLibraryRoot;
	State = SurfMaterialsLib::List(RootRead, Library);
	LastRead = FPlatformTime::Seconds();
	bDirty = false;
	ApplyFilter();
}

void SSurfMaterialManagerView::ApplyFilter()
{
	TArray<FSurfMaterialItem> Kept;
	SurfMaterialsLib::Filter(Library, Query, Kept);
	if (SortMode == 1)
	{
		// Newest first, by the asset file's time; what has no file yet comes first.
		TMap<FString, FDateTime> Times;
		for (const FSurfMaterialItem& Item : Kept)
		{
			FString File;
			const FString Package = Item.Asset.PackageName.ToString();
			Times.Add(Package, FPackageName::DoesPackageExist(Package, &File) ? IFileManager::Get().GetTimeStamp(*File) : FDateTime::MaxValue());
		}
		Kept.StableSort([&Times](const FSurfMaterialItem& A, const FSurfMaterialItem& B)
		{
			return Times.FindRef(A.Asset.PackageName.ToString()) > Times.FindRef(B.Asset.PackageName.ToString());
		});
	}
	if (bFlat)
	{
		Roots.Reset();
		for (const FSurfMaterialItem& Item : Kept)
		{
			FNodePtr Node = MakeShared<FSurfMaterialNode>();
			Node->Item = MakeShared<FSurfMaterialItem>(Item);
			Node->Label = Item.Name;
			Roots.Add(Node);
		}
	}
	else
	{
		SurfMaterialsLib::BuildTree(Kept, Roots);
	}

	// The row that was selected stays selected across a re-read, by name.
	FNodePtr Again;
	if (Selected.IsValid())
	{
		TArray<FNodePtr> Stack = Roots;
		while (Stack.Num() > 0 && !Again.IsValid())
		{
			FNodePtr Node = Stack.Pop();
			if (Node->Label == Selected->Label && Node->IsGroupOnly() == Selected->IsGroupOnly())
			{
				Again = Node;
			}
			Stack.Append(Node->Children);
		}
	}
	if (Tree.IsValid())
	{
		Tree->RequestTreeRefresh();
		ExpandAll(Roots);
		if (Again.IsValid())
		{
			Selected = Again;
			Tree->SetSelection(Again, ESelectInfo::Direct);
		}
	}
}

void SSurfMaterialManagerView::ExpandAll(const TArray<FNodePtr>& Nodes)
{
	for (const FNodePtr& Node : Nodes)
	{
		if (Node.IsValid() && Tree.IsValid())
		{
			Tree->SetItemExpansion(Node, true);
			ExpandAll(Node->Children);
		}
	}
}

TSharedRef<ITableRow> SSurfMaterialManagerView::MakeRow(FNodePtr Node, const TSharedRef<STableViewBase>& Owner)
{
	const FString Name = Node.IsValid() ? Node->Label : FString();
	FString Under;
	FString Tip;
	if (Node.IsValid() && Node->Item.IsValid())
	{
		Under = Node->Item->bInstance ? TEXT("instance") : TEXT("material");
		Tip = Node->Item->Asset.GetObjectPathString();
	}
	else
	{
		Under = TEXT("parent");
		Tip = TEXT("the instances beneath were made from this; click to see the parent itself");
	}
	TSharedRef<SWidget> Picture = SNullWidget::NullWidget;
	if (PicturesMode != 0 && Node.IsValid() && Node->Item.IsValid() && Node->Item->bInstance)
	{
		Picture = MakeRowPicture(*Node->Item);
	}
	return SNew(STableRow<FNodePtr>, Owner)
		.ToolTipText(FText::FromString(Tip))
		.Padding(FMargin(4.0f, PicturesMode != 0 ? 1.0f : 3.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				Picture
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				// Blue when the selection wears it (S197a).
				SNew(STextBlock).Text(FText::FromString(Name))
				.ColorAndOpacity_Lambda([this, Name]() { return WornNames.Contains(Name) ? SurfTheme::On() : FSlateColor::UseForeground(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Under)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

TSharedRef<SWidget> SSurfMaterialManagerView::MakeRowPicture(const FSurfMaterialItem& Item)
{
	const float Side = 36.0f;
	if (PicturesMode == 1)
	{
		FString Why;
		const FSlateBrush* Brush = SurfMaterialThumbnails::BrushFor(Cast<UMaterialInterface>(Item.Asset.GetAsset()), Why);
		if (Brush)
		{
			return SNew(SBox).WidthOverride(Side).HeightOverride(Side)
			[
				SNew(SImage).Image(Brush)
			];
		}
		return SNew(SBox).WidthOverride(Side).HeightOverride(Side).ToolTipText(FText::FromString(Why));
	}

	// The instance's first texture: set on it, else inherited.
	UTexture* Texture = nullptr;
	if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Item.Asset.GetAsset()))
	{
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			if (Value.ParameterValue)
			{
				Texture = Value.ParameterValue;
				break;
			}
		}
		if (!Texture)
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Instance->GetAllTextureParameterInfo(Infos, Guids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				if (Instance->GetTextureParameterValue(Info, Texture) && Texture)
				{
					break;
				}
			}
		}
	}
	if (!Texture)
	{
		return SNew(SBox).WidthOverride(Side).HeightOverride(Side).ToolTipText(LOCTEXT("NoTextureTip", "no texture on it"));
	}
	TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(FAssetData(Texture), (int32)Side, (int32)Side, Thumbnails);
	return SNew(SBox).WidthOverride(Side).HeightOverride(Side).ToolTipText(FText::FromString(Texture->GetName()))
	[
		Thumbnail->MakeThumbnailWidget()
	];
}

TSharedRef<SWidget> SSurfMaterialManagerView::MakeViewMenu()
{
	FMenuBuilder Menu(/*bCloseAfterSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	auto Radio = [this, &Menu](const FText& Label, int32* Field, int32 Value)
	{
		Menu.AddMenuEntry(Label, FText(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Field, Value]() { *Field = Value; SaveView(); ApplyFilter(); }),
				FCanExecuteAction(), FIsActionChecked::CreateLambda([Field, Value]() { return *Field == Value; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};
	Menu.BeginSection(TEXT("SurfViewPictures"), LOCTEXT("ViewPictures", "Pictures"));
	Radio(LOCTEXT("ViewPicturesNone", "None"), &PicturesMode, 0);
	Radio(LOCTEXT("ViewPicturesRendered", "Rendered on the cube"), &PicturesMode, 1);
	Radio(LOCTEXT("ViewPicturesTexture", "The first texture"), &PicturesMode, 2);
	Menu.EndSection();
	Menu.BeginSection(TEXT("SurfViewSort"), LOCTEXT("ViewSort", "Order"));
	Radio(LOCTEXT("ViewSortName", "By name"), &SortMode, 0);
	Radio(LOCTEXT("ViewSortNewest", "Newest first"), &SortMode, 1);
	Menu.EndSection();
	Menu.BeginSection(TEXT("SurfViewShape"), LOCTEXT("ViewShape", "Shape"));
	Menu.AddMenuEntry(LOCTEXT("ViewParents", "Parents as headings"), LOCTEXT("ViewParentsTip", "Off: one flat list of instances and materials."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { bFlat = !bFlat; SaveView(); ApplyFilter(); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([this]() { return !bFlat; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.EndSection();
	return Menu.MakeWidget();
}

void SSurfMaterialManagerView::SaveView()
{
	GConfig->SetInt(ManagerConfigSection, TEXT("ManagerPictures"), PicturesMode, GEditorPerProjectIni);
	GConfig->SetInt(ManagerConfigSection, TEXT("ManagerSort"), SortMode, GEditorPerProjectIni);
	GConfig->SetBool(ManagerConfigSection, TEXT("ManagerFlat"), bFlat, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SSurfMaterialManagerView::ChildrenOf(FNodePtr Node, TArray<FNodePtr>& Out)
{
	if (Node.IsValid())
	{
		Out = Node->Children;
	}
}

UMaterialInterface* SSurfMaterialManagerView::MaterialOf(const FNodePtr& Node) const
{
	if (!Node.IsValid())
	{
		return nullptr;
	}
	if (Node->Item.IsValid())
	{
		return Cast<UMaterialInterface>(Node->Item->Asset.GetAsset());
	}
	// A heading: the parent it is named after, when the library lists it.
	for (const FSurfMaterialItem& Item : Library)
	{
		if (Item.Name == Node->Label)
		{
			return Cast<UMaterialInterface>(Item.Asset.GetAsset());
		}
	}
	return nullptr;
}

void SSurfMaterialManagerView::OnRowSelected(FNodePtr Node, ESelectInfo::Type How)
{
	Selected = Node;
	if (How == ESelectInfo::Direct)
	{
		return;
	}
	UMaterialInterface* Material = MaterialOf(Node);
	Show(Material);
	// The Textures window dresses the material picked last, here or in the picker.
	SurfMaterialsLib::SetCurrentTarget(Material);
}

void SSurfMaterialManagerView::OnQueryChanged(const FText& Text)
{
	Query = Text.ToString();
	ApplyFilter();
}

void SSurfMaterialManagerView::OnAssetChanged(const FAssetData& Asset)
{
	bDirty = true;
}

void SSurfMaterialManagerView::OnAssetRenamed(const FAssetData& Asset, const FString& OldPath)
{
	bDirty = true;
}

void SSurfMaterialManagerView::MarkDirty()
{
	bDirty = true;
}

void SSurfMaterialManagerView::SelectByName(const FString& Name)
{
	TArray<FNodePtr> Stack = Roots;
	while (Stack.Num() > 0)
	{
		FNodePtr Node = Stack.Pop();
		if (!Node->IsGroupOnly() && Node->Label == Name)
		{
			Selected = Node;
			if (Tree.IsValid())
			{
				Tree->SetSelection(Node, ESelectInfo::Direct);
				Tree->RequestScrollIntoView(Node);
			}
			return;
		}
		Stack.Append(Node->Children);
	}
}

TSharedPtr<SWidget> SSurfMaterialManagerView::MakeContextMenu()
{
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	FillMaterialMenu(Menu);
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SSurfMaterialManagerView::MakeMaterialMenu()
{
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	FillMaterialMenu(Menu);
	return Menu.MakeWidget();
}

void SSurfMaterialManagerView::FillMaterialMenu(FMenuBuilder& Menu)
{
	const bool bAny = HasShown();
	const bool bInstance = InstanceShown();
	Menu.AddSubMenu(LOCTEXT("MenuNew", "New"), LOCTEXT("MenuNewTip", "A new instance of a parent of your choosing, a child of the picked instance, or a new parent from a master."),
		FNewMenuDelegate::CreateSP(this, &SSurfMaterialManagerView::FillNewMenu));
	Menu.AddMenuEntry(LOCTEXT("MenuDuplicate", "Duplicate"), LOCTEXT("MenuDuplicateTip", "A copy of the picked instance beside it, its info kept (Ctrl+D)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::DuplicatePicked), FCanExecuteAction::CreateLambda([bInstance]() { return bInstance; })));
	Menu.AddMenuEntry(LOCTEXT("MenuRename", "Rename"), LOCTEXT("RenameTip", "Another name for the picked material; what refers to it follows (F2)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::RenamePicked), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
	Menu.AddMenuEntry(LOCTEXT("MenuDelete", "Delete"), LOCTEXT("DeleteTip", "Delete the picked material, through the editor's own check of what still refers to it (Delete)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::DeletePicked), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("MenuApply", "Apply to the selection"), FText(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::ApplyShown), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
	Menu.AddMenuEntry(LOCTEXT("MenuSave", "Save"), LOCTEXT("MenuSaveTip", "Ctrl+S"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::SaveShown), FCanExecuteAction::CreateSP(this, &SSurfMaterialManagerView::CanSave)));
	Menu.AddMenuEntry(LOCTEXT("MenuRevert", "Revert"), FText(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::RevertShown), FCanExecuteAction::CreateSP(this, &SSurfMaterialManagerView::CanSave)));
	Menu.AddMenuEntry(LOCTEXT("MenuExportFile", "Export to file"), LOCTEXT("MenuExportFileTip", "Write this instance as a .material.json under SurfacingPro/Library/Materials: to keep, share or edit outside the editor."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			SurfLibrary::FSurfMaterialFile Out;
			FString Error;
			if (SurfLibrary::ExportMaterial(ShownInstance(), SurfLibrary::MaterialsDir(SurfLibrary::LibraryDir()), RootRead, Out, Error))
			{
				Say(FString::Printf(TEXT("written %s"), *FPaths::GetCleanFilename(Out.File)));
			}
			else
			{
				Say(Error);
			}
		}), FCanExecuteAction::CreateLambda([bInstance]() { return bInstance; })));
	Menu.AddMenuEntry(LOCTEXT("MenuImportFile", "Import from file..."), LOCTEXT("MenuImportFileTip", "Read .material.json files back: an instance not there yet is made, one that is is brought in line with its file."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
			if (!Desktop)
			{
				return;
			}
			const FString Dir = SurfLibrary::MaterialsDir(SurfLibrary::LibraryDir());
			IFileManager::Get().MakeDirectory(*Dir, true);
			TArray<FString> Files;
			if (!Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()), TEXT("Material files to read"), Dir, TEXT(""),
				TEXT("Material files (*.material.json)|*.material.json"), EFileDialogFlags::Multiple, Files) || Files.Num() == 0)
			{
				return;
			}
			TArray<FString> Lines;
			UMaterialInstanceConstant* Last = nullptr;
			for (const FString& File : Files)
			{
				SurfLibrary::FSurfMaterialFile Material;
				FString Line;
				if (!SurfLibrary::LoadMaterial(File, Material, Line) || !SurfLibrary::ApplyMaterial(Material, RootRead, /*bSave*/ true, Line))
				{
					Lines.Add(Line);
					continue;
				}
				Lines.Add(Line);
				Last = SurfLibrary::FindInstance(Material, RootRead);
			}
			const FString Said = FString::Join(Lines, TEXT("; "));
			if (Last)
			{
				TakeMade(Last, Said);
			}
			else
			{
				Say(Said);
			}
		})));
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("MenuOpen", "Open in the asset editor"), LOCTEXT("MenuOpenTip", "The editor's own window for the material (Enter, or a double-click)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::OpenShown), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
	Menu.AddMenuEntry(LOCTEXT("MenuBrowse", "Show in the content browser"), LOCTEXT("MenuBrowseTip", "The content browser, at the asset (Ctrl+B)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::ShowPickedInContentBrowser), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
	Menu.AddMenuEntry(LOCTEXT("MenuExplore", "Show in Explorer"), LOCTEXT("MenuExploreTip", "The asset's file, in Windows"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::ShowPickedInExplorer), FCanExecuteAction::CreateLambda([bAny]() { return bAny; })));
}

FReply SSurfMaterialManagerView::OnTreeKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	const FKey Key = Event.GetKey();
	const bool bCtrl = Event.IsControlDown() || Event.IsCommandDown();
	const bool bShift = Event.IsShiftDown();
	if (bCtrl && bShift && Key == EKeys::N) { NewChildOfPicked(); return FReply::Handled(); }
	if (bCtrl && Key == EKeys::N) { NewInstanceOfDefault(); return FReply::Handled(); }
	if (bCtrl && Key == EKeys::D) { DuplicatePicked(); return FReply::Handled(); }
	if (bCtrl && Key == EKeys::B) { ShowPickedInContentBrowser(); return FReply::Handled(); }
	if (bCtrl && Key == EKeys::S) { SaveShown(); return FReply::Handled(); }
	if (bCtrl && Key == EKeys::Z && GEditor) { if (bShift) { GEditor->RedoTransaction(); } else { GEditor->UndoTransaction(); } return FReply::Handled(); }
	if (bCtrl && Key == EKeys::Y && GEditor) { GEditor->RedoTransaction(); return FReply::Handled(); }
	if (Key == EKeys::F2) { RenamePicked(); return FReply::Handled(); }
	if (Key == EKeys::Delete) { DeletePicked(); return FReply::Handled(); }
	if (Key == EKeys::Enter) { OpenShown(); return FReply::Handled(); }
	return FReply::Unhandled();
}

void SSurfMaterialManagerView::OnRowDoubleClicked(FNodePtr Node)
{
	if (UMaterialInterface* Material = MaterialOf(Node))
	{
		Show(Material);
		SurfMaterialsLib::SetCurrentTarget(Material);
		OpenShown();
	}
}

// --- The actions ---------------------------------------------------------------------------------

UMaterialInterface* SSurfMaterialManagerView::DefaultParent() const
{
	TArray<FSurfMaterialItem> Masters;
	SurfMaterialsLib::Masters(Library, Masters);
	const int32 Index = SurfMaterialsLib::DefaultMasterIndex(Masters);
	return Masters.IsValidIndex(Index) ? Cast<UMaterialInterface>(Masters[Index].Asset.GetAsset()) : nullptr;
}

void SSurfMaterialManagerView::TakeMade(UMaterialInstanceConstant* Made, const FString& Line)
{
	if (!Made)
	{
		return;
	}
	Refresh();
	SelectByName(Made->GetName());
	Show(Made);
	SurfMaterialsLib::SetCurrentTarget(Made);
	Say(Line);
	SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("made name=%s parent=%s"), *Made->GetName(), Made->Parent ? *Made->Parent->GetName() : TEXT("none")));
}

TSharedRef<SWidget> SSurfMaterialManagerView::MakeNewMenu()
{
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	FillNewMenu(Menu);
	return Menu.MakeWidget();
}

void SSurfMaterialManagerView::FillNewMenu(FMenuBuilder& Menu)
{
	TArray<FSurfMaterialItem> Masters;
	SurfMaterialsLib::Masters(Library, Masters);
	const int32 DefaultIndex = SurfMaterialsLib::DefaultMasterIndex(Masters);
	UMaterialInstanceConstant* Picked = ShownInstance();

	if (Masters.Num() == 0)
	{
		Menu.AddMenuEntry(LOCTEXT("NewNoParents", "No parent in the library yet - Seed parents in the Material Library"), FText(), FSlateIcon(), FUIAction());
		return;
	}

	// The instance: of the default parent, under the picked instance, of any parent.
	Menu.BeginSection(TEXT("SurfNewInstance"), LOCTEXT("NewInstanceOf", "An instance of"));
	if (Masters.IsValidIndex(DefaultIndex))
	{
		Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%s (the default)"), *Masters[DefaultIndex].Name)),
			LOCTEXT("NewDefaultTip", "Ctrl+N. The settings say which parent is the default."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::NewInstanceOfDefault)));
	}
	if (Picked)
	{
		Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%s (the picked instance - a child)"), *Picked->GetName())),
			LOCTEXT("NewChildTip", "Ctrl+Shift+N. The child takes the picked instance as its parent and inherits its parameters."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::NewChildOfPicked)));
	}
	for (int32 Index = 0; Index < Masters.Num(); ++Index)
	{
		if (Index == DefaultIndex)
		{
			continue;
		}
		TWeakObjectPtr<UMaterialInterface> Parent = Cast<UMaterialInterface>(Masters[Index].Asset.GetAsset());
		Menu.AddMenuEntry(FText::FromString(Masters[Index].Name), FText::FromString(Masters[Index].Asset.GetObjectPathString()), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::NewInstanceOf, Parent)));
	}
	Menu.EndSection();

	// A parent of the user's own: a copy of a master, never touched by an update.
	Menu.BeginSection(TEXT("SurfNewParent"), LOCTEXT("NewParentFrom", "A new parent from"));
	for (const FSurfMaterialItem& Master : Masters)
	{
		TWeakObjectPtr<UMaterial> Source = Cast<UMaterial>(Master.Asset.GetAsset());
		if (!Source.IsValid())
		{
			continue;
		}
		Menu.AddMenuEntry(FText::FromString(Master.Name),
			LOCTEXT("NewParentTip", "An unstamped, numbered copy of this master in the library's Materials folder, yours to edit, with a first instance under it."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialManagerView::NewParentFrom, Source)));
	}
	Menu.EndSection();
}

void SSurfMaterialManagerView::NewInstanceOfDefault()
{
	UMaterialInterface* Parent = DefaultParent();
	if (!Parent)
	{
		Say(TEXT("new instance: no parent in the library yet - press Seed parents in the Material Library"));
		return;
	}
	NewInstanceOf(Parent);
}

void SSurfMaterialManagerView::NewInstanceOf(TWeakObjectPtr<UMaterialInterface> Parent)
{
	UMaterialInterface* Of = Parent.Get();
	if (!Of)
	{
		Say(TEXT("new instance: the parent could not be loaded"));
		return;
	}

	// The name first: the proposed one in a box, the library's instances below to pick from.
	const FString Proposed = SurfMaterialsLib::UniqueAssetName(SurfMaterialsLib::InstancesFolder(RootRead), SurfMaterialsLib::InstanceBaseName(Of));
	TArray<FString> Existing;
	SurfMaterialsLib::InstanceNames(Library, Existing);
	TArray<TSharedPtr<FString>> Names;
	for (const FString& Name : Existing)
	{
		Names.Add(MakeShared<FString>(Name));
	}
	TSharedRef<TArray<TSharedPtr<FString>>> NamesHeld = MakeShared<TArray<TSharedPtr<FString>>>(MoveTemp(Names));

	TSharedRef<TWeakPtr<SWindow>> Holder = MakeShared<TWeakPtr<SWindow>>();
	auto Close = [Holder]()
	{
		if (TSharedPtr<SWindow> Open = Holder->Pin())
		{
			Open->RequestDestroyWindow();
		}
	};
	TSharedRef<SEditableTextBox> Box = SNew(SEditableTextBox)
		.Text(FText::FromString(Proposed))
		.SelectAllTextWhenFocused(true)
		.OnTextCommitted_Lambda([this, Parent, Close](const FText& Text, ETextCommit::Type How)
		{
			if (How == ETextCommit::OnEnter)
			{
				MakeInstanceNamed(Parent, Text.ToString());
				Close();
			}
		});
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(FString::Printf(TEXT("New instance of %s"), *Of->GetName())))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBox).Padding(10.0f).MinDesiredWidth(380.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NewNameLabel", "Its name (a taken name is numbered):"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Box
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NewNameExisting", "Or start from one of the library's:")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).HeightOverride(180.0f)
					[
						SNew(SListView<TSharedPtr<FString>>)
						.ListItemsSource(&NamesHeld.Get())
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow_Lambda([](TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& Owner)
						{
							return SNew(STableRow<TSharedPtr<FString>>, Owner).Padding(FMargin(4.0f, 1.0f))
							[
								SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()))
							];
						})
						.OnSelectionChanged_Lambda([Box, NamesHeld](TSharedPtr<FString> Item, ESelectInfo::Type How)
						{
							if (Item.IsValid() && How != ESelectInfo::Direct)
							{
								Box->SetText(FText::FromString(*Item));
							}
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						ManagerButton(LOCTEXT("NewOk", "Make it"), FText(), [this, Parent, Box, Close]() { MakeInstanceNamed(Parent, Box->GetText().ToString()); Close(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						ManagerButton(LOCTEXT("NewCancel", "Cancel"), FText(), [Close]() { Close(); })
					]
				]
			]
		];
	*Holder = Window;
	FSlateApplication::Get().AddWindow(Window);
	FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);
}

void SSurfMaterialManagerView::MakeInstanceNamed(TWeakObjectPtr<UMaterialInterface> Parent, const FString& Name)
{
	UMaterialInterface* Of = Parent.Get();
	if (!Of)
	{
		Say(TEXT("new instance: the parent could not be loaded"));
		return;
	}
	FString Error;
	UMaterialInstanceConstant* Made = SurfMaterialsLib::NewInstanceNamed(Of, SurfMaterialsLib::InstancesFolder(RootRead), Name, /*bSave*/ true, Error);
	if (!Made)
	{
		Say(Error);
		return;
	}
	TakeMade(Made, FString::Printf(TEXT("made %s, %s %s"), *Made->GetName(), Of->IsA<UMaterialInstanceConstant>() ? TEXT("a child of") : TEXT("an instance of"), *Of->GetName()));
}

void SSurfMaterialManagerView::NewChildOfPicked()
{
	UMaterialInstanceConstant* Picked = ShownInstance();
	if (!Picked)
	{
		Say(TEXT("new child: pick an instance first"));
		return;
	}
	NewInstanceOf(Picked);
}

void SSurfMaterialManagerView::NewParentFrom(TWeakObjectPtr<UMaterial> Master)
{
	UMaterial* Source = Master.Get();
	if (!Source)
	{
		Say(TEXT("new parent: the master could not be loaded"));
		return;
	}
	FString Error;
	UMaterial* Parent = SurfMaterialsLib::CopyMaster(Source, SurfMaterialsLib::MastersFolder(RootRead), /*bSave*/ true, Error);
	if (!Parent)
	{
		Say(Error);
		return;
	}
	// Its first instance, so the parent has a place in the tree.
	UMaterialInstanceConstant* Made = SurfMaterialsLib::NewInstance(Parent, SurfMaterialsLib::InstancesFolder(RootRead), /*bSave*/ true, Error);
	if (!Made)
	{
		bDirty = true;
		Say(Error);
		return;
	}
	TakeMade(Made, FString::Printf(TEXT("made %s from %s, and %s of it"), *Parent->GetName(), *Source->GetName(), *Made->GetName()));
}

void SSurfMaterialManagerView::DuplicatePicked()
{
	UMaterialInstanceConstant* Source = ShownInstance();
	if (!Source)
	{
		Say(TEXT("duplicate: pick an instance"));
		return;
	}
	FString Error;
	UMaterialInstanceConstant* Made = SurfMaterialsLib::CopyInstance(Source, FPackageName::GetLongPackagePath(Source->GetOutermost()->GetName()), /*bSave*/ true, Error);
	if (!Made)
	{
		Say(Error);
		return;
	}
	TakeMade(Made, FString::Printf(TEXT("copied %s to %s"), *Source->GetName(), *Made->GetName()));
}

void SSurfMaterialManagerView::RenamePicked()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return;
	}
	TSharedRef<TWeakPtr<SWindow>> Holder = MakeShared<TWeakPtr<SWindow>>();
	auto Close = [Holder]()
	{
		if (TSharedPtr<SWindow> Open = Holder->Pin())
		{
			Open->RequestDestroyWindow();
		}
	};
	TSharedRef<SEditableTextBox> Box = SNew(SEditableTextBox)
		.Text(FText::FromString(Material->GetName()))
		.SelectAllTextWhenFocused(true)
		.OnTextCommitted_Lambda([this, Close](const FText& Text, ETextCommit::Type How)
		{
			if (How == ETextCommit::OnEnter)
			{
				RenamePickedTo(Text.ToString().TrimStartAndEnd());
				Close();
			}
		});
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("RenameTitle", "Rename"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBox).Padding(10.0f).MinDesiredWidth(360.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("A new name for %s"), *Material->GetName())))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Box
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						ManagerButton(LOCTEXT("RenameOk", "Rename"), FText(), [this, Box, Close]() { RenamePickedTo(Box->GetText().ToString().TrimStartAndEnd()); Close(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						ManagerButton(LOCTEXT("RenameCancel", "Cancel"), FText(), [Close]() { Close(); })
					]
				]
			]
		];
	*Holder = Window;
	FSlateApplication::Get().AddWindow(Window);
	FSlateApplication::Get().SetKeyboardFocus(Box, EFocusCause::SetDirectly);
}

void SSurfMaterialManagerView::RenamePickedTo(const FString& NewName)
{
	UMaterialInterface* Material = Target.Get();
	if (!Material || NewName.IsEmpty() || NewName == Material->GetName())
	{
		return;
	}
	FText Reason;
	if (!FName::IsValidXName(NewName, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS, &Reason))
	{
		Say(TEXT("rename: ") + Reason.ToString());
		return;
	}
	const FString Folder = FPackageName::GetLongPackagePath(Material->GetOutermost()->GetName());
	if (FPackageName::DoesPackageExist(Folder / NewName))
	{
		Say(FString::Printf(TEXT("rename: %s is taken in %s"), *NewName, *Folder));
		return;
	}
	TArray<FAssetRenameData> Renames;
	Renames.Add(FAssetRenameData(Material, Folder, NewName));
	const FString Old = Material->GetName();
	if (IAssetTools::Get().RenameAssets(Renames))
	{
		bDirty = true;
		Refresh();
		SelectByName(NewName);
		Say(FString::Printf(TEXT("%s is now %s"), *Old, *NewName));
		SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("rename from=%s to=%s"), *Old, *NewName));
	}
	else
	{
		Say(FString::Printf(TEXT("rename: %s could not be renamed"), *Old));
	}
}

void SSurfMaterialManagerView::DeletePicked()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return;
	}
	const FString Name = Material->GetName();
	TArray<FAssetData> Assets;
	Assets.Add(FAssetData(Material));
	// The editor's own dialog: what still refers to it is shown, and the delete can be a replace.
	const int32 Deleted = ObjectTools::DeleteAssets(Assets, /*bShowConfirmation*/ true);
	if (Deleted > 0)
	{
		Selected.Reset();
		Show(nullptr);
		SurfMaterialsLib::SetCurrentTarget(nullptr);
		bDirty = true;
		Say(FString::Printf(TEXT("deleted %s"), *Name));
		SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("delete name=%s"), *Name));
	}
}

void SSurfMaterialManagerView::ShowPickedInContentBrowser()
{
	if (UMaterialInterface* Material = Target.Get())
	{
		if (GEditor)
		{
			TArray<UObject*> Objects;
			Objects.Add(Material);
			GEditor->SyncBrowserToObjects(Objects);
		}
	}
}

void SSurfMaterialManagerView::ShowPickedInExplorer()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return;
	}
	FString File;
	if (FPackageName::DoesPackageExist(Material->GetOutermost()->GetName(), &File))
	{
		FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(File));
	}
	else
	{
		Say(FString::Printf(TEXT("%s has no file yet - save it first"), *Material->GetName()));
	}
}

// --- The material shown ------------------------------------------------------------------------

void SSurfMaterialManagerView::ShowMaterial(UMaterialInterface* Material)
{
	if (!Material)
	{
		return;
	}
	// The selection read first, so the next tick does not move it back.
	FollowSelection();
	Show(Material);
	SelectByName(Material->GetName());
}

void SSurfMaterialManagerView::Show(UMaterialInterface* Material)
{
	Target = Material;
	bChanged = false;
	Suggestions.Reset();
	if (Preview.IsValid())
	{
		Preview->SetMaterial(Material);
	}
	if (More.IsValid())
	{
		More->SetObject(Cast<UMaterialInstanceConstant>(Material), /*bForceRefresh*/ true);
	}
	BuildParameters();
}

UMaterialInstanceConstant* SSurfMaterialManagerView::ShownInstance() const
{
	return Cast<UMaterialInstanceConstant>(Target.Get());
}

void SSurfMaterialManagerView::MarkChanged()
{
	if (UMaterialInstanceConstant* Instance = ShownInstance())
	{
		Instance->MarkPackageDirty();
		bChanged = true;
		if (Preview.IsValid())
		{
			Preview->Refresh();
		}
	}
}

// --- The inspector ------------------------------------------------------------------------------

void SSurfMaterialManagerView::AddHeading(const FText& Title, const TSharedPtr<SWidget>& Beside)
{
	// A Details category header (S192f): the header grey across, the name
	// in the editor's small bold header type.
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Title).Font(SurfTheme::Bold(8)).ColorAndOpacity(SurfTheme::Header())
		];
	if (Beside.IsValid())
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			Beside.ToSharedRef()
		];
	}
	ParamsBox->AddSlot().Padding(0.0f, 8.0f, 0.0f, 3.0f)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
		.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(SBox).MinDesiredHeight(24.0f).VAlign(VAlign_Center)
			[
				Row
			]
		]
	];
}

void SSurfMaterialManagerView::AddRow(const FString& Name, TFunction<bool()> IsOverridden, TFunction<void(bool)> SetOverridden, const TSharedRef<SWidget>& Editor, const FText& Tip)
{
	const FSlateFontInfo Bold = FAppStyle::Get().GetFontStyle("BoldFont");
	const FSlateFontInfo Normal = FAppStyle::Get().GetFontStyle("NormalFont");
	ParamsBox->AddSlot().Padding(2.0f, 1.0f)
	[
		SNew(SHorizontalBox)
		.ToolTipText(Tip)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("OverrideTip", "Ticked: the instance sets this itself. Unticked: it takes the parent's value."))
			.IsChecked_Lambda([IsOverridden]() { return IsOverridden() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, SetOverridden](ECheckBoxState NewState)
			{
				SetOverridden(NewState == ECheckBoxState::Checked);
				MarkChanged();
			})
		]
		+ SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Name))
			.Font_Lambda([Bold, Normal, IsOverridden]() { return IsOverridden() ? Bold : Normal; })
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]
		+ SHorizontalBox::Slot().FillWidth(0.58f).VAlign(VAlign_Center)
		[
			SNew(SBox)
			.IsEnabled_Lambda([IsOverridden]() { return IsOverridden(); })
			[
				Editor
			]
		]
	];
}

void SSurfMaterialManagerView::BuildParameters()
{
	if (!ParamsBox.IsValid())
	{
		return;
	}
	ParamsBox->ClearChildren();
	SuggestBox.Reset();

	UMaterialInstanceConstant* Instance = ShownInstance();
	if (!Instance)
	{
		const UMaterialInterface* Material = Target.Get();
		ParamsBox->AddSlot().Padding(4.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Material
				? TEXT("Parent material - Open in editor, or make an instance to set parameters.")
				: TEXT("Pick an instance to edit its parameters.")))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
		return;
	}

	TWeakObjectPtr<UMaterialInstanceConstant> Weak = Instance;
	const FString Root = SurfMaterialsLib::NormalisedRoot(RootRead);

	// The parent, and the master under it when the parent is itself an instance.
	ParamsBox->AddSlot().Padding(2.0f, 2.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("Parent", "Parent")).Font(FAppStyle::Get().GetFontStyle("BoldFont"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(UMaterialInterface::StaticClass())
			.ThumbnailPool(Thumbnails)
			.DisplayThumbnail(true)
			.AllowClear(false)
			.OnShouldFilterAsset_Lambda([Root, Weak](const FAssetData& Asset)
			{
				// The library's own, and not itself.
				if (!Root.IsEmpty() && !Asset.PackageName.ToString().StartsWith(Root + TEXT("/")))
				{
					return true;
				}
				const UMaterialInstanceConstant* Self = Weak.Get();
				return Self && Asset.GetObjectPathString() == Self->GetPathName();
			})
			.ObjectPath_Lambda([Weak]()
			{
				const UMaterialInstanceConstant* Self = Weak.Get();
				return (Self && Self->Parent) ? Self->Parent->GetPathName() : FString();
			})
			.OnObjectChanged(this, &SSurfMaterialManagerView::OnParentPicked)
		]
	];
	if (Instance->Parent && Instance->Parent->IsA<UMaterialInstance>())
	{
		if (const UMaterial* Master = Instance->GetMaterial())
		{
			ParamsBox->AddSlot().Padding(2.0f, 0.0f, 2.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("master: %s"), *Master->GetName())))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
	}

	// Scalars.
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Instance->GetAllScalarParameterInfo(Infos, Guids);
		if (Infos.Num() > 0)
		{
			AddHeading(LOCTEXT("Scalars", "Scalars"));
		}
		for (const FMaterialParameterInfo& Info : Infos)
		{
			const FName Name = Info.Name;
			TFunction<bool()> IsOverridden = [Weak, Name]()
			{
				const UMaterialInstanceConstant* Self = Weak.Get();
				return Self && Self->ScalarParameterValues.ContainsByPredicate([Name](const FScalarParameterValue& V) { return V.ParameterInfo.Name == Name; });
			};
			TFunction<void(bool)> SetOverridden = [this, Weak, Info, Name](bool bOn)
			{
				UMaterialInstanceConstant* Self = Weak.Get();
				if (!Self)
				{
					return;
				}
				Edit(Self, FText::FromString(FString::Printf(TEXT("%s %s"), bOn ? TEXT("Set") : TEXT("Unset"), *Name.ToString())), [&]()
				{
					if (bOn)
					{
						float Value = 0.0f;
						Self->GetScalarParameterValue(Info, Value);
						Self->SetScalarParameterValueEditorOnly(Info, Value);
					}
					else
					{
						Self->ScalarParameterValues.RemoveAll([Name](const FScalarParameterValue& V) { return V.ParameterInfo.Name == Name; });
					}
				});
			};
			float SliderMin = 0.0f;
			float SliderMax = 0.0f;
			Instance->GetScalarParameterSliderMinMax(Info, SliderMin, SliderMax);
			const bool bSlider = SliderMax > SliderMin;
			auto Commit = [this, Weak, Info, Name](float NewValue)
			{
				if (UMaterialInstanceConstant* Self = Weak.Get())
				{
					Edit(Self, FText::FromString(FString::Printf(TEXT("Set %s"), *Name.ToString())), [&]()
					{
						Self->SetScalarParameterValueEditorOnly(Info, NewValue);
					});
				}
			};
			AddRow(Name.ToString(), IsOverridden, SetOverridden,
				SNew(SSpinBox<float>)
				.MinDesiredWidth(80.0f)
				.Delta(0.01f)
				.MinSliderValue(bSlider ? TOptional<float>(SliderMin) : TOptional<float>())
				.MaxSliderValue(bSlider ? TOptional<float>(SliderMax) : TOptional<float>())
				.Value_Lambda([Weak, Info]()
				{
					float Value = 0.0f;
					if (const UMaterialInstanceConstant* Self = Weak.Get())
					{
						Self->GetScalarParameterValue(Info, Value);
					}
					return Value;
				})
				.OnBeginSliderMovement_Lambda([this, Weak, Name]() { BeginDrag(Weak.Get(), FText::FromString(FString::Printf(TEXT("Set %s"), *Name.ToString()))); })
				.OnValueChanged_Lambda([Commit](float NewValue) { Commit(NewValue); })
				.OnEndSliderMovement_Lambda([this, Commit](float NewValue) { Commit(NewValue); EndDrag(); })
				.OnValueCommitted_Lambda([Commit](float NewValue, ETextCommit::Type) { Commit(NewValue); }));
		}
	}

	// Vectors: the four channels, and a swatch that opens the colour picker.
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Instance->GetAllVectorParameterInfo(Infos, Guids);
		if (Infos.Num() > 0)
		{
			AddHeading(LOCTEXT("Vectors", "Vectors (R G B A)"));
		}
		for (const FMaterialParameterInfo& Info : Infos)
		{
			const FName Name = Info.Name;
			TFunction<bool()> IsOverridden = [Weak, Name]()
			{
				const UMaterialInstanceConstant* Self = Weak.Get();
				return Self && Self->VectorParameterValues.ContainsByPredicate([Name](const FVectorParameterValue& V) { return V.ParameterInfo.Name == Name; });
			};
			TFunction<void(bool)> SetOverridden = [this, Weak, Info, Name](bool bOn)
			{
				UMaterialInstanceConstant* Self = Weak.Get();
				if (!Self)
				{
					return;
				}
				Edit(Self, FText::FromString(FString::Printf(TEXT("%s %s"), bOn ? TEXT("Set") : TEXT("Unset"), *Name.ToString())), [&]()
				{
					if (bOn)
					{
						FLinearColor Value = FLinearColor::Black;
						Self->GetVectorParameterValue(Info, Value);
						Self->SetVectorParameterValueEditorOnly(Info, Value);
					}
					else
					{
						Self->VectorParameterValues.RemoveAll([Name](const FVectorParameterValue& V) { return V.ParameterInfo.Name == Name; });
					}
				});
			};
			auto Current = [Weak, Info]()
			{
				FLinearColor Value = FLinearColor::Black;
				if (const UMaterialInstanceConstant* Self = Weak.Get())
				{
					Self->GetVectorParameterValue(Info, Value);
				}
				return Value;
			};
			auto Commit = [this, Weak, Info, Name](const FLinearColor& NewValue)
			{
				if (UMaterialInstanceConstant* Self = Weak.Get())
				{
					Edit(Self, FText::FromString(FString::Printf(TEXT("Set %s"), *Name.ToString())), [&]()
					{
						Self->SetVectorParameterValueEditorOnly(Info, NewValue);
					});
				}
			};
			auto Channel = [this, Weak, Name, Current, Commit](int32 Index) -> TSharedRef<SWidget>
			{
				return SNew(SSpinBox<float>)
					.MinDesiredWidth(40.0f)
					.Delta(0.01f)
					.Value_Lambda([Current, Index]() { return Current().Component(Index); })
					.OnBeginSliderMovement_Lambda([this, Weak, Name]() { BeginDrag(Weak.Get(), FText::FromString(FString::Printf(TEXT("Set %s"), *Name.ToString()))); })
					.OnEndSliderMovement_Lambda([this, Current, Commit, Index](float NewValue)
					{
						FLinearColor Value = Current();
						Value.Component(Index) = NewValue;
						Commit(Value);
						EndDrag();
					})
					.OnValueChanged_Lambda([Current, Commit, Index](float NewValue)
					{
						FLinearColor Value = Current();
						Value.Component(Index) = NewValue;
						Commit(Value);
					})
					.OnValueCommitted_Lambda([Current, Commit, Index](float NewValue, ETextCommit::Type)
					{
						FLinearColor Value = Current();
						Value.Component(Index) = NewValue;
						Commit(Value);
					});
			};
			AddRow(Name.ToString(), IsOverridden, SetOverridden,
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 3.0f, 0.0f)
				[
					SNew(SColorBlock)
					.Color_Lambda([Current]() { return Current(); })
					.Size(FVector2D(22.0f, 16.0f))
					.ShowBackgroundForAlpha(true)
					.ToolTipText(LOCTEXT("SwatchTip", "Click for the colour picker."))
					.OnMouseButtonDown_Lambda([this, Current, Commit](const FGeometry&, const FPointerEvent&)
					{
						FColorPickerArgs PickerArgs;
						PickerArgs.bUseAlpha = true;
						PickerArgs.InitialColor = Current();
						PickerArgs.ParentWidget = AsShared();
						PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([Commit](FLinearColor NewValue) { Commit(NewValue); });
						OpenColorPicker(PickerArgs);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(1.0f, 0.0f)[ Channel(0) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(1.0f, 0.0f)[ Channel(1) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(1.0f, 0.0f)[ Channel(2) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(1.0f, 0.0f)[ Channel(3) ]);
		}
	}

	// Switches: the static ones, a new permutation on change.
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Instance->GetAllStaticSwitchParameterInfo(Infos, Guids);
		if (Infos.Num() > 0)
		{
			AddHeading(LOCTEXT("Switches", "Switches"));
		}
		for (int32 Index = 0; Index < Infos.Num(); ++Index)
		{
			const FMaterialParameterInfo Info = Infos[Index];
			const FGuid Guid = Guids.IsValidIndex(Index) ? Guids[Index] : FGuid();
			const FName Name = Info.Name;
			TFunction<bool()> IsOverridden = [Weak, Name]()
			{
				const UMaterialInstanceConstant* Self = Weak.Get();
				if (!Self)
				{
					return false;
				}
				const FStaticParameterSet Set = Self->GetStaticParameters();
				return Set.StaticSwitchParameters.ContainsByPredicate([Name](const FStaticSwitchParameter& P) { return P.ParameterInfo.Name == Name && P.bOverride; });
			};
			auto Put = [this, Weak, Info, Name, Guid](bool bOn, bool bValue)
			{
				UMaterialInstanceConstant* Self = Weak.Get();
				if (!Self)
				{
					return;
				}
				Edit(Self, FText::FromString(FString::Printf(TEXT("Switch %s"), *Name.ToString())), [&]()
				{
					FStaticParameterSet Set = Self->GetStaticParameters();
					Set.StaticSwitchParameters.RemoveAll([Name](const FStaticSwitchParameter& P) { return P.ParameterInfo.Name == Name; });
					if (bOn)
					{
						Set.StaticSwitchParameters.Add(FStaticSwitchParameter(Info, bValue, /*bOverride*/ true, Guid));
					}
					Self->UpdateStaticPermutation(Set);
				});
			};
			auto Current = [Weak, Info]()
			{
				bool bValue = false;
				FGuid Unused;
				if (const UMaterialInstanceConstant* Self = Weak.Get())
				{
					Self->GetStaticSwitchParameterValue(Info, bValue, Unused);
				}
				return bValue;
			};
			TFunction<void(bool)> SetOverridden = [Put, Current](bool bOn) { Put(bOn, Current()); };
			AddRow(Name.ToString(), IsOverridden, SetOverridden,
				SNew(SCheckBox)
				.IsChecked_Lambda([Current]() { return Current() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Put](ECheckBoxState NewState)
				{
					Put(true, NewState == ECheckBoxState::Checked);
				}),
				LOCTEXT("SwitchTip", "A static switch: changing it compiles a new permutation of the material, which takes a moment."));
		}
	}

	// Textures, the picker filtered to conformed ones when asked, and the siblings offered under them.
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		Instance->GetAllTextureParameterInfo(Infos, Guids);
		if (Infos.Num() > 0)
		{
			AddHeading(LOCTEXT("Textures", "Textures"),
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("ConformOnlyTip", "The picker lists only textures that are ours and read their file under the source root - the Textures window's Conform. Off, every texture in the project."))
				.IsChecked(this, &SSurfMaterialManagerView::ConformOnlyState)
				.OnCheckStateChanged(this, &SSurfMaterialManagerView::OnConformOnlyToggled)
				[
					SNew(STextBlock).Text(LOCTEXT("ConformOnly", "Conform textures only"))
				]);
		}
		for (const FMaterialParameterInfo& Info : Infos)
		{
			const FName Name = Info.Name;
			TFunction<bool()> IsOverridden = [Weak, Name]()
			{
				const UMaterialInstanceConstant* Self = Weak.Get();
				return Self && Self->TextureParameterValues.ContainsByPredicate([Name](const FTextureParameterValue& V) { return V.ParameterInfo.Name == Name; });
			};
			TFunction<void(bool)> SetOverridden = [this, Weak, Info, Name](bool bOn)
			{
				UMaterialInstanceConstant* Self = Weak.Get();
				if (!Self)
				{
					return;
				}
				Edit(Self, FText::FromString(FString::Printf(TEXT("%s %s"), bOn ? TEXT("Set") : TEXT("Unset"), *Name.ToString())), [&]()
				{
					if (bOn)
					{
						UTexture* Value = nullptr;
						Self->GetTextureParameterValue(Info, Value);
						Self->SetTextureParameterValueEditorOnly(Info, Value);
					}
					else
					{
						Self->TextureParameterValues.RemoveAll([Name](const FTextureParameterValue& V) { return V.ParameterInfo.Name == Name; });
					}
				});
			};
			auto CurrentPath = [Weak, Info]()
			{
				UTexture* Value = nullptr;
				if (const UMaterialInstanceConstant* Self = Weak.Get())
				{
					Self->GetTextureParameterValue(Info, Value);
				}
				return Value ? Value->GetPathName() : FString();
			};
			AddRow(Name.ToString(), IsOverridden, SetOverridden,
				SNew(SSurfTexturePicker)
				.ThumbnailPool(Thumbnails)
				.OnShouldFilterAsset(this, &SSurfMaterialManagerView::ShouldFilterTexture)
				.ObjectPath_Lambda(CurrentPath)
				.SetBase_Lambda([this]() { return LastPickBase; })
				.OnPicked_Lambda([this, Weak, Info, Name](const FAssetData& Asset)
				{
					if (UMaterialInstanceConstant* Self = Weak.Get())
					{
						UTexture* Texture = Asset.IsValid() ? Cast<UTexture>(Asset.GetAsset()) : nullptr;
						Edit(Self, FText::FromString(FString::Printf(TEXT("Set %s"), *Name.ToString())), [&]()
						{
							Self->SetTextureParameterValueEditorOnly(Info, Texture);
						});
						if (Texture)
						{
							FString Suffix;
							SurfTextureSuggest::Split(Texture->GetName(), LastPickBase, Suffix);
							OnTexturePicked(Name);
						}
					}
				}));
		}
		if (Infos.Num() > 0)
		{
			ParamsBox->AddSlot().Padding(2.0f, 4.0f, 2.0f, 2.0f)
			[
				SAssignNew(SuggestBox, SVerticalBox)
			];
			RebuildSuggestions();
		}
	}
}

void SSurfMaterialManagerView::OnParentPicked(const FAssetData& Asset)
{
	UMaterialInstanceConstant* Instance = ShownInstance();
	UMaterialInterface* NewParent = Cast<UMaterialInterface>(Asset.GetAsset());
	if (!Instance || !NewParent || Instance->Parent == NewParent || NewParent == Instance)
	{
		return;
	}
	Edit(Instance, FText::FromString(FString::Printf(TEXT("Parent of %s"), *Instance->GetName())), [&]()
	{
		Instance->SetParentEditorOnly(NewParent);
	});
	BuildParameters();
	if (More.IsValid())
	{
		More->ForceRefresh();
	}
	bDirty = true;
	Say(FString::Printf(TEXT("%s is now an instance of %s - Save to keep it"), *Instance->GetName(), *NewParent->GetName()));
}

bool SSurfMaterialManagerView::ShouldFilterTexture(const FAssetData& Asset) const
{
	if (!bConformOnly)
	{
		return false;
	}
	return SurfTextures::StatusOf(Asset, SurfTextures::SourceRoot()) != SurfTextures::ESurfTextureStatus::Conform;
}

void SSurfMaterialManagerView::OnConformOnlyToggled(ECheckBoxState NewState)
{
	bConformOnly = NewState == ECheckBoxState::Checked;
	GConfig->SetBool(ManagerConfigSection, TEXT("ConformOnly"), bConformOnly, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

ECheckBoxState SSurfMaterialManagerView::ConformOnlyState() const
{
	return bConformOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSurfMaterialManagerView::OnTexturePicked(FName Parameter)
{
	Suggestions.Reset();
	if (UMaterialInstanceConstant* Instance = ShownInstance())
	{
		SurfTextureSuggest::Suggest(Instance, Parameter, RootRead, Suggestions);
	}
	RebuildSuggestions();
	if (Suggestions.Num() > 0)
	{
		Say(FString::Printf(TEXT("%d of the same set could go on the other parameters - see under Textures"), Suggestions.Num()));
	}
}

void SSurfMaterialManagerView::RebuildSuggestions()
{
	if (!SuggestBox.IsValid())
	{
		return;
	}
	SuggestBox->ClearChildren();
	if (Suggestions.Num() == 0)
	{
		return;
	}

	SuggestBox->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SameSet", "From the same set:"))
			.ToolTipText(LOCTEXT("SameSetTip", "Siblings of the texture just picked - the same name, another suffix - that fit the other parameters, by how the library's other instances pair them, else by the parameter's name. Offered, never put on by themselves."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			ManagerButton(LOCTEXT("UseAll", "Use all"), LOCTEXT("UseAllTip", "Put every one below on its parameter."), [this]() { UseAllSuggestions(); })
		]
	];
	for (int32 Index = 0; Index < Suggestions.Num(); ++Index)
	{
		const SurfTextureSuggest::FSurfTextureSuggestion& Suggestion = Suggestions[Index];
		SuggestBox->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(20.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s  <-  %s  (%s)"), *Suggestion.Parameter.ToString(), *Suggestion.Texture.AssetName.ToString(), *Suggestion.Why)))
				.ToolTipText(FText::FromString(Suggestion.Texture.GetObjectPathString()))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				ManagerButton(LOCTEXT("Use", "Use"), LOCTEXT("UseTip", "Put this texture on this parameter."), [this, Index]() { UseSuggestion(Index); })
			]
		];
	}
}

void SSurfMaterialManagerView::UseSuggestion(int32 Index)
{
	UMaterialInstanceConstant* Instance = ShownInstance();
	if (!Instance || !Suggestions.IsValidIndex(Index))
	{
		return;
	}
	const SurfTextureSuggest::FSurfTextureSuggestion Suggestion = Suggestions[Index];
	UTexture* Texture = Cast<UTexture>(Suggestion.Texture.GetAsset());
	if (!Texture)
	{
		Say(FString::Printf(TEXT("%s could not be loaded"), *Suggestion.Texture.AssetName.ToString()));
		return;
	}
	Edit(Instance, FText::FromString(FString::Printf(TEXT("Set %s"), *Suggestion.Parameter.ToString())), [&]()
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Suggestion.Parameter), Texture);
	});
	{
		FString Suffix;
		SurfTextureSuggest::Split(Texture->GetName(), LastPickBase, Suffix);
	}
	Suggestions.RemoveAt(Index);
	RebuildSuggestions();
	SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("suggest_used parameter=%s texture=%s"), *Suggestion.Parameter.ToString(), *Texture->GetName()));
}

void SSurfMaterialManagerView::UseAllSuggestions()
{
	while (Suggestions.Num() > 0)
	{
		const int32 Before = Suggestions.Num();
		UseSuggestion(0);
		if (Suggestions.Num() == Before)
		{
			break;
		}
	}
}

void SSurfMaterialManagerView::OnMoreChanged(const FPropertyChangedEvent& Event)
{
	MarkChanged();
}

// --- The buttons ---------------------------------------------------------------------------------

void SSurfMaterialManagerView::SaveShown()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		Say(TEXT("save: nothing is shown"));
		return;
	}
	if (SurfMaterialSeed::SaveAsset(Material))
	{
		bChanged = false;
		Say(FString::Printf(TEXT("saved %s"), *Material->GetName()));
		SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("save name=%s"), *Material->GetName()));
	}
	else
	{
		Say(FString::Printf(TEXT("save: %s could not be saved"), *Material->GetName()));
	}
}

void SSurfMaterialManagerView::RevertShown()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return;
	}
	const FString Path = Material->GetPathName();
	UPackage* Package = Material->GetOutermost();
	if (!Package || !FPackageName::DoesPackageExist(Package->GetName()))
	{
		Say(TEXT("revert: it has never been saved - nothing on disk to go back to"));
		return;
	}
	TArray<UPackage*> Packages = { Package };
	FText Error;
	if (!UPackageTools::ReloadPackages(Packages, Error, EReloadPackagesInteractionMode::AssumePositive))
	{
		Say(TEXT("revert: ") + Error.ToString());
		return;
	}
	UMaterialInterface* Back = FindObject<UMaterialInterface>(nullptr, *Path);
	if (!Back)
	{
		Back = LoadObject<UMaterialInterface>(nullptr, *Path);
	}
	Show(Back);
	SurfMaterialsLib::SetCurrentTarget(Back);
	bDirty = true;
	Say(Back ? FString::Printf(TEXT("%s read again from disk"), *Back->GetName()) : TEXT("revert: it could not be read again"));
	SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("revert path=%s"), *Path));
}

void SSurfMaterialManagerView::OpenShown()
{
	if (UMaterialInterface* Material = Target.Get())
	{
		if (GEditor)
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Material);
		}
	}
}

void SSurfMaterialManagerView::ApplyShown()
{
	UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		Say(TEXT("apply: nothing is shown"));
		return;
	}
	USurfEdMode* Mode = ManagerMode();
	if (!Mode)
	{
		Say(TEXT("apply: the mode is not active - press the SurfacingPro mode first"));
		return;
	}
	if (SurfMaterialsLib::Assign(*Mode, Material))
	{
		Say(Mode->Inspector ? Mode->Inspector->LastResult : FString::Printf(TEXT("put %s on the selection"), *Material->GetName()));
	}
	else
	{
		Say(Mode->Inspector ? Mode->Inspector->LastResult : TEXT("apply: nothing selected"));
	}
}

bool SSurfMaterialManagerView::CanSave() const
{
	return Target.IsValid() && bChanged;
}

bool SSurfMaterialManagerView::HasShown() const
{
	return Target.IsValid();
}

bool SSurfMaterialManagerView::InstanceShown() const
{
	return ShownInstance() != nullptr;
}

void SSurfMaterialManagerView::Say(const FString& Line)
{
	Notice = Line;
	NoticeAt = FPlatformTime::Seconds();
}

FText SSurfMaterialManagerView::HeaderText() const
{
	const UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return FText::FromString(TEXT("No material picked"));
	}
	// One short line (S191a): "in mat manager, too much massive text block".
	return FText::FromString(FString::Printf(TEXT("%s%s"), *Material->GetName(), bChanged ? TEXT("  (changed)") : TEXT("")));
}

FText SSurfMaterialManagerView::FootText() const
{
	if (!Notice.IsEmpty() && FPlatformTime::Seconds() - NoticeAt < ManagerNoticeSeconds)
	{
		return FText::FromString(Notice);
	}
	if (Preview.IsValid() && !Preview->HasMesh())
	{
		return FText::FromString(TEXT("No preview mesh - set one in Settings > Materials > Preview Mesh."));
	}
	// The help lives in the tooltip now; the line only speaks when something happened.
	return FText::GetEmpty();
}

FText SSurfMaterialManagerView::EmptyText() const
{
	return FText::FromString(SurfMaterialsLib::EmptyLine(State, RootRead));
}

EVisibility SSurfMaterialManagerView::EmptyVisibility() const
{
	return Roots.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SSurfMaterialManagerView::InstanceVisibility() const
{
	return ShownInstance() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SSurfMaterialManagerView::ParentOnlyVisibility() const
{
	return Target.IsValid() && !ShownInstance() ? EVisibility::Visible : EVisibility::Collapsed;
}

// --- The tab ---------------------------------------------------------------------------------

namespace SurfMaterialManagerTab
{
	const FName TabName("SurfacingProMaterialManager");

	/** The open window's view, for OpenWith. */
	static TWeakPtr<SSurfMaterialManagerView> LiveView;

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		TSharedRef<SSurfMaterialManagerView> View = SNew(SSurfMaterialManagerView);
		LiveView = View;
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("ManagerTabLabel", "Material Manager"))
			[
				View
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized() || FGlobalTabmanager::Get()->HasTabSpawner(TabName))
		{
			return;
		}
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
			.SetDisplayName(LOCTEXT("ManagerTabTitle", "SurfacingPro Material Manager"))
			.SetTooltipText(LOCTEXT("ManagerTabTip", "The picked material on a preview cube, its parameters beside it to edit and save."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.tool"));
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		}
	}

	void Open()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		Register();
		FGlobalTabmanager::Get()->TryInvokeTab(TabName);
		SurfSession::Verb(TEXT("material_manager"), TEXT("open"));
	}

	void OpenWith(UMaterialInterface* Material)
	{
		Open();
		if (TSharedPtr<SSurfMaterialManagerView> View = LiveView.Pin())
		{
			View->ShowMaterial(Material);
		}
		SurfSession::Verb(TEXT("material_manager"), FString::Printf(TEXT("open_with %s"), Material ? *Material->GetName() : TEXT("-")));
	}
}

#undef LOCTEXT_NAMESPACE
