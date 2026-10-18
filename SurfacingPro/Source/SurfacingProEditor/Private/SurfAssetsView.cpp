// Copyright MapModelers. All Rights Reserved.

#include "SurfAssetsView.h"
#include "SurfEdMode.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "SurfSettings.h"
#include "SurfSession.h"
#include "SurfTheme.h"
#include "SurfMaterialsView.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ObjectTools.h"
#include "EditorModeManager.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Components/StaticMeshComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STileView.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

#define LOCTEXT_NAMESPACE "SurfacingProAssets"

namespace
{
	USurfEdMode* AssetsMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** 0 is the list (S211a: "once scrolled all the way down, turn it to a List view"). */
	constexpr int32 TileSizes[] = { 20, 48, 64, 88, 120 };

	bool IsPrefab(const FAssetData& Asset) { return Asset.AssetClassPath == USurfPrefab::StaticClass()->GetClassPathName(); }
	bool IsMesh(const FAssetData& Asset) { return Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName(); }
	/** A level, to place as a level instance (S217a). */
	bool IsLevel(const FAssetData& Asset) { return Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName(); }

	/**
	 * The tile's tooltip (S247a): the name and the folder, and for a prefab
	 * what the Prefabs window's columns said - its pieces, the parts it
	 * carries, how many are placed - and whether it is unsaved. Read on
	 * hover, so no asset is loaded for a tile nobody looks at.
	 */
	FText TipFor(const TSharedPtr<FAssetData>& Row)
	{
		if (!Row.IsValid())
		{
			return FText::GetEmpty();
		}
		FString Tip = FString::Printf(TEXT("%s\n%s"), *Row->AssetName.ToString(), *Row->PackagePath.ToString());
		if (IsPrefab(*Row))
		{
			if (const USurfPrefab* Asset = Cast<USurfPrefab>(Row->GetAsset()))
			{
				int32 Placed = 0;
				if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
				{
					for (TActorIterator<ASurfPrefabInstance> It(World); It; ++It)
					{
						if (*It && It->Prefab == Asset && !It->HasAnyFlags(RF_Transient)) { ++Placed; }
					}
				}
				Tip += FString::Printf(TEXT("\n%d piece%s, %d part%s, placed %d"),
					Asset->NumPrimitives(), Asset->NumPrimitives() == 1 ? TEXT("") : TEXT("s"),
					Asset->Parts.Num(), Asset->Parts.Num() == 1 ? TEXT("") : TEXT("s"), Placed);
				if (const UPackage* Package = Asset->GetOutermost(); Package && Package->IsDirty())
				{
					Tip += TEXT(" - unsaved");
				}
			}
		}
		return FText::FromString(Tip);
	}

	/** The name on the tile (S228a: "if assetname start with some SM_ or PF_ then dont show it"): the kind's prefix dropped, the full name in the tooltip. */
	FString ShownName(const FAssetData& Asset)
	{
		FString Name = Asset.AssetName.ToString();
		for (const TCHAR* Prefix : { TEXT("SM_"), TEXT("PF_"), TEXT("LI_"), TEXT("S_"), TEXT("BP_") })
		{
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase) && Name.Len() > FCString::Strlen(Prefix))
			{
				return Name.Mid(FCString::Strlen(Prefix));
			}
		}
		return Name;
	}

	/** The picked actors that an asset can stand in for: our placements and static meshes. */
	void PickedActors(TArray<AActor*>& Out)
	{
		Out.Reset();
		if (!GEditor)
		{
			return;
		}
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (Actor && (Actor->IsA<ASurfPrefabInstance>() || Actor->IsA<AStaticMeshActor>() || Actor->IsA<ALevelInstance>()))
			{
				Out.Add(Actor);
			}
		}
	}
}

namespace SurfAssetsTab
{
	void Gather(const FString& Folder, bool bPrefabs, bool bMeshes, bool bLevels, TArray<FAssetData>& Out)
	{
		Out.Reset();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.bRecursiveClasses = true;
		const FString Path = SurfMaterialsLib::NormalisedRoot(Folder);
		Filter.PackagePaths.Add(FName(*(Path.IsEmpty() ? FString(TEXT("/Game")) : Path)));
		if (bPrefabs) { Filter.ClassPaths.Add(USurfPrefab::StaticClass()->GetClassPathName()); }
		if (bMeshes) { Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName()); }
		if (bLevels) { Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName()); }
		if (Filter.ClassPaths.Num() == 0)
		{
			return;
		}
		Registry.GetAssets(Filter, Out);
		// The tests' scratch and the engine's own stay out.
		// The tests' scratch and the engine's own stay out; so does the level that is open - it cannot be an instance of itself.
		const FName OpenLevel = GEditor && GEditor->GetEditorWorldContext().World() ? GEditor->GetEditorWorldContext().World()->GetPackage()->GetFName() : NAME_None;
		Out.RemoveAll([OpenLevel](const FAssetData& A) { return A.PackagePath.ToString().Contains(TEXT("/_Surf")) || (IsLevel(A) && A.PackageName == OpenLevel); });
		Out.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });
	}

	bool Matches(const FAssetData& Asset, const FString& Typed)
	{
		TArray<FString> Words;
		Typed.ToLower().ParseIntoArrayWS(Words);
		if (Words.Num() == 0)
		{
			return true;
		}
		TArray<FString> NameWords;
		Asset.AssetName.ToString().ToLower().Replace(TEXT("_"), TEXT(" ")).ParseIntoArrayWS(NameWords);
		for (const FString& Word : Words)
		{
			if (Word == TEXT("p:")) { if (!IsPrefab(Asset)) { return false; } continue; }
			if (Word == TEXT("sm:")) { if (!IsMesh(Asset)) { return false; } continue; }
			if (Word == TEXT("li:")) { if (!IsLevel(Asset)) { return false; } continue; }
			bool bAny = false;
			for (const FString& NameWord : NameWords)
			{
				if (NameWord.StartsWith(Word))
				{
					bAny = true;
					break;
				}
			}
			if (!bAny)
			{
				return false;
			}
		}
		return true;
	}
}

/** The tab's body. */
class SSurfAssetsView : public SCompoundWidget
{
public:
	using FRowPtr = TSharedPtr<FAssetData>;

	SLATE_BEGIN_ARGS(SSurfAssetsView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ThumbnailPool = MakeShared<FAssetThumbnailPool>(128);
		const USurfSettings* S = GetDefault<USurfSettings>();
		Zoom = S ? FMath::Clamp(S->AssetsZoom, 0, UE_ARRAY_COUNT(TileSizes) - 1) : 2;
		Kinds = S ? S->AssetsKinds : 3;
		Folder = S && !S->AssetsFolder.IsEmpty() ? S->AssetsFolder : FString(TEXT("/Game"));

		auto KindTick = [this](const FText& Label, const FText& Tip, int32 Bit) -> TSharedRef<SWidget>
		{
			return SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.Padding(FMargin(5.0f, 1.0f))
				.ToolTipText(Tip)
				.IsChecked_Lambda([this, Bit]() { return (Kinds & Bit) != 0 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Bit](ECheckBoxState State)
				{
					Kinds = State == ECheckBoxState::Checked ? (Kinds | Bit) : (Kinds & ~Bit);
					if (Kinds == 0) { Kinds = Bit == 1 ? 2 : 1; }
					Save();
					Refresh();
				})
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Font(8))
				];
		};
		auto Small = [](const FText& Label, const FText& Tip, TFunction<void()> Run) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(3.0f, 0.0f))
				.ToolTipText(Tip)
				.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); })
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Font(9)).ColorAndOpacity(SurfTheme::Dim())
				];
		};

		ChildSlot
		[
			SNew(SVerticalBox)
			// The search, and the kinds.
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 3.0f, 4.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("AssetsSearchHint", "search - p: sm: li: pick a kind"))
					.OnTextChanged_Lambda([this](const FText& Text) { Typed = Text.ToString(); Refresh(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					KindTick(LOCTEXT("KindPrefabs", "P"), LOCTEXT("KindPrefabsTip", "Prefabs."), 1)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f, 0.0f, 0.0f)
				[
					KindTick(LOCTEXT("KindMeshes", "SM"), LOCTEXT("KindMeshesTip", "Static meshes."), 2)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(1.0f, 0.0f, 0.0f, 0.0f)
				[
					KindTick(LOCTEXT("KindLevels", "LI"), LOCTEXT("KindLevelsTip", "Levels, placed as level instances."), 4)
				]
			]
			// The folder, the zoom.
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 1.0f, 4.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SComboButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(2.0f, 0.0f))
					.ToolTipText(LOCTEXT("FolderTip", "Which content folder, and everything under it."))
					.ButtonContent()
					[
						SNew(STextBlock)
						.Font(SurfTheme::Font(8))
						.ColorAndOpacity(SurfTheme::Dim())
						.Text_Lambda([this]() { return FText::FromString(Folder); })
					]
					.OnGetMenuContent_Lambda([this]() { return MakeFolderMenu(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(SurfTheme::Font(8)).ColorAndOpacity(SurfTheme::Dim())
					.Text_Lambda([this]() { return FText::Format(LOCTEXT("AssetsCount", "{0}"), FText::AsNumber(Rows.Num())); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					Small(LOCTEXT("ZoomOut", "−"), LOCTEXT("ZoomOutTip", "Smaller tiles (Ctrl+wheel over the tiles too)."), [this]() { SetZoom(Zoom - 1); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					Small(LOCTEXT("ZoomIn", "+"), LOCTEXT("ZoomInTip", "Bigger tiles."), [this]() { SetZoom(Zoom + 1); })
				]
			]
			// The tiles.
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride(360.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					.Padding(2.0f)
					[
						SAssignNew(Switcher, SWidgetSwitcher)
						.WidgetIndex_Lambda([this]() { return Zoom == 0 ? 0 : 1; })
						+ SWidgetSwitcher::Slot()
						[
							SAssignNew(List, SListView<FRowPtr>)
							.ListItemsSource(&Rows)
							.SelectionMode(ESelectionMode::Multi)
							.OnGenerateRow(this, &SSurfAssetsView::MakeListRow)
							.OnMouseButtonDoubleClick_Lambda([this](FRowPtr Row) { Place(Row); })
							.OnContextMenuOpening_Lambda([this]() { return MakeTileMenu(); })
						]
						+ SWidgetSwitcher::Slot()
						[
							SAssignNew(Tiles, STileView<FRowPtr>)
							.ListItemsSource(&Rows)
							.ItemWidth_Lambda([this]() { return static_cast<float>(TileSizes[Zoom] + 6); })
							.ItemHeight_Lambda([this]() { return static_cast<float>(TileSizes[Zoom] + 22); })
							.SelectionMode(ESelectionMode::Multi)
							.OnGenerateTile(this, &SSurfAssetsView::MakeTile)
							.OnMouseButtonDoubleClick_Lambda([this](FRowPtr Row) { Place(Row); })
							.OnContextMenuOpening_Lambda([this]() { return MakeTileMenu(); })
						]
					]
				]
			]
			// The foot: what a click does.
			+ SVerticalBox::Slot().AutoHeight().Padding(6.0f, 2.0f, 6.0f, 3.0f)
			[
				SNew(STextBlock)
				.Font(SurfTheme::Font(8))
				.ColorAndOpacity(SurfTheme::Dim())
				.AutoWrapText(true)
				.Text(LOCTEXT("AssetsFoot", "Double-click or drag into the viewport to place; right-click for more."))
			]
		];
		Refresh();
	}

	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.IsControlDown())
		{
			SetZoom(Zoom + (Event.GetWheelDelta() > 0.0f ? 1 : -1));
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

private:
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<STileView<FRowPtr>> Tiles;
	TSharedPtr<SListView<FRowPtr>> List;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
	TArray<TSharedPtr<FAssetThumbnail>> Thumbnails;
	TArray<FRowPtr> Rows;
	TArray<FAssetData> All;
	FString Typed;
	FString Folder;
	int32 Kinds = 3;
	int32 Zoom = 1;
	double GatheredAt = -100.0;

	void Save()
	{
		if (USurfSettings* S = GetMutableDefault<USurfSettings>())
		{
			S->AssetsZoom = Zoom;
			S->AssetsKinds = Kinds;
			S->AssetsFolder = Folder;
			S->SaveConfig();
		}
	}

	void SetZoom(int32 Want)
	{
		Zoom = FMath::Clamp(Want, 0, UE_ARRAY_COUNT(TileSizes) - 1);
		Save();
		Thumbnails.Reset();
		if (Tiles.IsValid()) { Tiles->RebuildList(); }
		if (List.IsValid()) { List->RebuildList(); }
	}

	void Refresh(bool bGatherAgain = true)
	{
		if (bGatherAgain)
		{
			SurfAssetsTab::Gather(Folder, (Kinds & 1) != 0, (Kinds & 2) != 0, (Kinds & 4) != 0, All);
			GatheredAt = FPlatformTime::Seconds();
		}
		Rows.Reset();
		for (const FAssetData& Asset : All)
		{
			if (SurfAssetsTab::Matches(Asset, Typed))
			{
				Rows.Add(MakeShared<FAssetData>(Asset));
			}
		}
		Thumbnails.Reset();
		if (Tiles.IsValid())
		{
			Tiles->RequestListRefresh();
		}
		if (List.IsValid())
		{
			List->RequestListRefresh();
		}
	}

	/** Whichever view is up. */
	TSharedPtr<STableViewBase> Shown() const { return Zoom == 0 ? StaticCastSharedPtr<STableViewBase>(List) : StaticCastSharedPtr<STableViewBase>(Tiles); }

	TArray<FRowPtr> PickedRows() const
	{
		if (Zoom == 0 && List.IsValid()) { return List->GetSelectedItems(); }
		if (Tiles.IsValid()) { return Tiles->GetSelectedItems(); }
		return {};
	}

	TArray<FAssetData> Picked() const
	{
		TArray<FAssetData> Out;
		for (const FRowPtr& Row : PickedRows())
		{
			if (Row.IsValid()) { Out.Add(*Row); }
		}
		return Out;
	}

	TSharedRef<ITableRow> MakeListRow(FRowPtr Row, const TSharedRef<STableViewBase>& Owner)
	{
		const int32 Size = TileSizes[0];
		TSharedPtr<SWidget> Picture = SNullWidget::NullWidget;
		if (Row.IsValid() && Row->IsValid())
		{
			TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(*Row, Size, Size, ThumbnailPool);
			Thumbnails.Add(Thumbnail);
			FAssetThumbnailConfig Config;
			Config.bAllowFadeIn = false;
			Config.bAllowHintText = false;
			Picture = Thumbnail->MakeThumbnailWidget(Config);
		}
		const FString Name = Row.IsValid() ? Row->AssetName.ToString() : FString();
		const bool bPrefab = Row.IsValid() && IsPrefab(*Row);
		const bool bLevel = Row.IsValid() && IsLevel(*Row);
		TWeakPtr<FAssetData> Weak = Row;
		return SNew(STableRow<FRowPtr>, Owner)
			.Padding(FMargin(3.0f, 1.0f))
			.ToolTipText_Lambda([Weak]() { return TipFor(Weak.Pin()); })
			.OnDragDetected_Lambda([Weak](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				TSharedPtr<FAssetData> Data = Weak.Pin();
				if (!Data.IsValid() || !Event.IsMouseButtonDown(EKeys::LeftMouseButton))
				{
					return FReply::Unhandled();
				}
				SurfSession::Gesture(TEXT("asset_row_drag"));
				return FReply::Handled().BeginDragDrop(FAssetDragDropOp::New(*Data));
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(static_cast<float>(Size)).HeightOverride(static_cast<float>(Size))
					[
						Picture.ToSharedRef()
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(bPrefab ? LOCTEXT("MarkPrefab", "P") : (bLevel ? LOCTEXT("MarkLevel", "LI") : LOCTEXT("MarkMesh", "SM"))).Font(SurfTheme::Font(7)).ColorAndOpacity(bPrefab ? SurfTheme::On() : SurfTheme::Dim())
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Row.IsValid() ? ShownName(*Row) : Name)).Font(SurfTheme::Font(8)).OverflowPolicy(ETextOverflowPolicy::Ellipsis).ColorAndOpacity(SurfTheme::Quiet())
				]
			];
	}

	TSharedRef<ITableRow> MakeTile(FRowPtr Row, const TSharedRef<STableViewBase>& Owner)
	{
		const int32 Size = TileSizes[Zoom];
		TSharedPtr<SWidget> Picture = SNullWidget::NullWidget;
		if (Row.IsValid() && Row->IsValid())
		{
			TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(*Row, Size, Size, ThumbnailPool);
			Thumbnails.Add(Thumbnail);
			FAssetThumbnailConfig Config;
			Config.bAllowFadeIn = false;
			Config.bAllowHintText = false;
			Picture = Thumbnail->MakeThumbnailWidget(Config);
		}
		const FString Name = Row.IsValid() ? Row->AssetName.ToString() : FString();
		const FString Shown = Row.IsValid() ? ShownName(*Row) : FString();
		const bool bPrefab = Row.IsValid() && IsPrefab(*Row);
		const bool bLevel = Row.IsValid() && IsLevel(*Row);
		TWeakPtr<FAssetData> Weak = Row;

		// Tighter and lighter (S228a: "brighter background for staticmesh
		// thumbnails ... more compact less space between thumbnails").
		return SNew(STableRow<FRowPtr>, Owner)
			.Padding(FMargin(1.0f))
			.ToolTipText_Lambda([Weak]() { return TipFor(Weak.Pin()); })
			.OnDragDetected_Lambda([Weak](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				TSharedPtr<FAssetData> Data = Weak.Pin();
				if (!Data.IsValid() || !Event.IsMouseButtonDown(EKeys::LeftMouseButton))
				{
					return FReply::Unhandled();
				}
				// The same drop the content browser's drag makes: a prefab
				// through our factory, a mesh through the editor's.
				SurfSession::Gesture(TEXT("asset_tile_drag"));
				return FReply::Handled().BeginDragDrop(FAssetDragDropOp::New(*Data));
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FLinearColor(0.30f, 0.30f, 0.32f))
						.Padding(0.0f)
						[
							SNew(SBox).WidthOverride(static_cast<float>(Size)).HeightOverride(static_cast<float>(Size))
							[
								Picture.ToSharedRef()
							]
						]
					]
					// The kind, a small mark in the corner.
					+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(2.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
						.Padding(FMargin(3.0f, 0.0f))
						[
							SNew(STextBlock).Text(bPrefab ? LOCTEXT("MarkPrefab", "P") : (bLevel ? LOCTEXT("MarkLevel", "LI") : LOCTEXT("MarkMesh", "SM"))).Font(SurfTheme::Font(7)).ColorAndOpacity(bPrefab ? SurfTheme::On() : SurfTheme::Dim())
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(1.0f, 1.0f, 1.0f, 0.0f)
				[
					SNew(SBox).WidthOverride(static_cast<float>(Size + 4)).Clipping(EWidgetClipping::ClipToBounds)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Shown))
						.Font(SurfTheme::Font(8))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.ColorAndOpacity(SurfTheme::Quiet())
					]
				]
			];
	}

	/** The content folders as a tree (S228a: "instead of a dropdown ... something closer to the outliner, selecting among the actual hierarchy"). */
	TSharedRef<SWidget> MakeFolderMenu()
	{
		FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		FPathPickerConfig Config;
		Config.DefaultPath = Folder;
		Config.bAllowContextMenu = false;
		Config.bAllowClassesFolder = false;
		Config.bAllowReadOnlyFolders = true;
		Config.bAddDefaultPath = false;
		Config.bFocusSearchBoxWhenOpened = false;
		Config.OnPathSelected = FOnPathSelected::CreateLambda([this](const FString& Path)
		{
			if (Path.IsEmpty() || Path == Folder) { return; }
			Folder = Path;
			Save();
			Refresh();
			SurfSession::Verb(TEXT("assets_folder"), Path);
		});
		return SNew(SBox).WidthOverride(260.0f).HeightOverride(320.0f)
			[
				Browser.Get().CreatePathPicker(Config)
			];
	}

	void Place(FRowPtr Row)
	{
		USurfEdMode* Mode = AssetsMode();
		if (!Mode || !Row.IsValid())
		{
			return;
		}
		const FVector Where = Mode->ViewFloorPoint();
		PlaceAt(*Row, Where);
	}

	/**
	 * Where the tile menu's Place puts it (S250e: "right click place should
	 * default to 0 0 0 - or same pos as selection (center) if any selection
	 * active?"): the middle of what is picked - our geometry's, else the
	 * picked actors' origins - and with nothing picked, the origin.
	 */
	FVector MenuPlacePoint() const
	{
		USurfEdMode* Mode = AssetsMode();
		if (Mode && (Mode->CountPickedPieces() > 0 || Mode->GetPointSelection().Num() > 0))
		{
			return Mode->GetWidgetLocation();
		}
		if (GEditor && GEditor->GetSelectedActorCount() > 0)
		{
			FVector Sum = FVector::ZeroVector;
			int32 Count = 0;
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (const AActor* Actor = Cast<AActor>(*It))
				{
					Sum += Actor->GetActorLocation();
					++Count;
				}
			}
			if (Count > 0)
			{
				return Sum / Count;
			}
		}
		return FVector::ZeroVector;
	}

	void PlaceAt(const FAssetData& Data, const FVector& Where)
	{
		USurfEdMode* Mode = AssetsMode();
		if (!Mode)
		{
			return;
		}
		if (IsLevel(Data))
		{
			Mode->PlaceSceneAsset(Data.GetSoftObjectPath(), Where);
		}
		else if (USurfPrefab* Prefab = Cast<USurfPrefab>(Data.GetAsset()))
		{
			Mode->PlacePrefabAsset(Prefab, Where);
		}
		else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Data.GetAsset()))
		{
			Mode->PlaceStaticMeshAsset(Mesh, Where);
		}
	}

	TSharedPtr<SWidget> MakeTileMenu()
	{
		const TArray<FAssetData> Chosen = Picked();
		TArray<AActor*> Actors;
		PickedActors(Actors);
		FMenuBuilder Menu(true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), false);
		if (Chosen.Num() > 0)
		{
			Menu.AddMenuEntry(LOCTEXT("TilePlace", "Place"), LOCTEXT("TilePlaceTip", "An instance at the middle of what is picked, or at the origin with nothing picked."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this]()
				{
					const FVector Where = MenuPlacePoint();
					for (const FRowPtr& Row : PickedRows()) { if (Row.IsValid()) { PlaceAt(*Row, Where); } }
				})));
			Menu.AddMenuEntry(
				FText::Format(LOCTEXT("TileReplace", "Replace the picked with this ({0})"), FText::AsNumber(Actors.Num())),
				LOCTEXT("TileReplaceTip", "Every picked prefab, mesh or level instance in the level becomes this one, where it stands - any kind for any other."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, First = Chosen[0]]() { Replace(First); }),
					FCanExecuteAction::CreateLambda([Actors]() { return Actors.Num() > 0; })));
			Menu.AddSeparator();
			// A prefab's own lines (S247a, redundancy W1): what the Prefabs
			// window had, here - Edit, Find, Painted meshes, Save, Delete.
			{
				TArray<USurfPrefab*> Prefabs;
				for (const FAssetData& Data : Chosen)
				{
					if (IsPrefab(Data))
					{
						if (USurfPrefab* Asset = Cast<USurfPrefab>(Data.GetAsset())) { Prefabs.Add(Asset); }
					}
				}
				if (Prefabs.Num() > 0)
				{
					TArray<TWeakObjectPtr<USurfPrefab>> Weak;
					for (USurfPrefab* Asset : Prefabs) { Weak.Add(Asset); }
					const TWeakObjectPtr<USurfPrefab> First = Prefabs[0];
					Menu.AddMenuEntry(LOCTEXT("TileEdit", "Open"), LOCTEXT("TileEditTip", "Open the prefab to edit it; Close writes it back and every instance follows."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([First]() { if (USurfEdMode* Mode = AssetsMode()) { if (First.IsValid()) { Mode->EditPrefabAsset(First.Get()); } } })));
					Menu.AddMenuEntry(LOCTEXT("TileFind", "Find in the level"), LOCTEXT("TileFindTip", "Select every instance of it placed in this level."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([First]() { if (USurfEdMode* Mode = AssetsMode()) { if (First.IsValid()) { Mode->FindPrefabInstances(First.Get()); } } })));
					Menu.AddMenuEntry(LOCTEXT("TileMeshPaint", "Painted meshes"), LOCTEXT("TileMeshPaintTip", "The meshes inside it take the paint brush, and the paint is kept with the prefab."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([Weak]()
						{
							for (const TWeakObjectPtr<USurfPrefab>& Each : Weak)
							{
								if (USurfPrefab* Asset = Each.Get())
								{
									Asset->Modify();
									Asset->bMeshPaint = !Asset->bMeshPaint;
									Asset->MarkPackageDirty();
									SurfSession::Verb(TEXT("prefab_mesh_paint"), Asset->bMeshPaint ? TEXT("on") : TEXT("off"));
								}
							}
						}), FCanExecuteAction(), FIsActionChecked::CreateLambda([First]() { return First.IsValid() && First->bMeshPaint; })),
						NAME_None, EUserInterfaceActionType::ToggleButton);
					Menu.AddMenuEntry(LOCTEXT("TileSave", "Save"), LOCTEXT("TileSaveTip", "Write the prefab asset to disk."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([Weak]()
						{
							USurfEdMode* Mode = AssetsMode();
							for (const TWeakObjectPtr<USurfPrefab>& Each : Weak)
							{
								if (Mode && Each.IsValid()) { Mode->SavePrefabAsset(Each.Get()); }
							}
						})));
					Menu.AddMenuEntry(LOCTEXT("TileDelete", "Delete"), LOCTEXT("TileDeleteTip", "Delete the asset; the editor says what still uses it."), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([Weak]()
						{
							// The room is closed first if the one being deleted is open.
							if (USurfEdMode* Mode = AssetsMode()) { if (Mode->IsInsidePrefab()) { Mode->UnwindRooms(/*bKeep*/ true); } }
							TArray<FAssetData> Assets;
							for (const TWeakObjectPtr<USurfPrefab>& Each : Weak) { if (Each.IsValid()) { Assets.Emplace(Each.Get()); } }
							if (Assets.Num() > 0)
							{
								const int32 Gone = ObjectTools::DeleteAssets(Assets, /*bShowConfirmation*/ true);
								SurfSession::Verb(TEXT("prefab_delete"), FString::Printf(TEXT("picked=%d gone=%d"), Assets.Num(), Gone));
							}
						})));
					Menu.AddSeparator();
				}
			}
			// Not for a level: opening one would swap the map out under him.
			Menu.AddMenuEntry(LOCTEXT("TileOpen", "Open in the asset editor"), LOCTEXT("TileOpenTip", "The editor's own window for the asset."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Chosen]()
				{
					if (GEditor)
					{
						for (const FAssetData& Data : Chosen)
						{
							if (IsLevel(Data)) { continue; }
							if (UObject* Asset = Data.GetAsset()) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset); }
						}
					}
				}), FCanExecuteAction::CreateLambda([Chosen]() { for (const FAssetData& D : Chosen) { if (!IsLevel(D)) { return true; } } return false; })));
			Menu.AddMenuEntry(LOCTEXT("TileBrowse", "Show in the content browser"), LOCTEXT("TileBrowseTip", "The content browser, at the asset."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([Chosen]()
				{
					FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
					Browser.Get().SyncBrowserToAssets(Chosen);
				})));
			Menu.AddSeparator();
		}
		// The picked actors' own lines - Replace with, Details, Materials - whatever tile.
		SurfAssetsTab::AddActorLines(Menu);
		return Menu.MakeWidget();
	}

	static void AddActorLinesOf(FMenuBuilder& Menu, TArray<AActor*> Actors)
	{
		if (Actors.Num() > 0)
		{
			// A picker to swap the picked for any prefab, mesh or level
			// (S228a: "right clicking asset i shall be able to Replace and
			// swap it with something else - popup search field").
			Menu.AddSubMenu(
				FText::Format(LOCTEXT("PickedReplace", "Replace the picked ({0}) with..."), FText::AsNumber(Actors.Num())),
				LOCTEXT("PickedReplaceTip", "Every picked prefab, mesh or level becomes the asset chosen, where it stands - any kind for any other."),
				FNewMenuDelegate::CreateLambda([Actors](FMenuBuilder& Pick)
				{
					FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
					FAssetPickerConfig Config;
					Config.Filter.ClassPaths.Add(USurfPrefab::StaticClass()->GetClassPathName());
					Config.Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
					Config.Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
					Config.Filter.bRecursiveClasses = true;
					Config.InitialAssetViewType = EAssetViewType::List;
					Config.bAllowNullSelection = false;
					Config.bFocusSearchBoxWhenOpened = true;
					Config.bAllowDragging = false;
					Config.OnAssetSelected = FOnAssetSelected::CreateLambda([Actors](const FAssetData& Data)
					{
						USurfEdMode* Mode = AssetsMode();
						if (!Mode) { return; }
						const int32 Done = Mode->ReplaceActorsWith(Actors, Data);
						SurfSession::Verb(TEXT("assets_replace"), FString::Printf(TEXT("%s -> %d (viewport)"), *Data.AssetName.ToString(), Done));
						FSlateApplication::Get().DismissAllMenus();
					});
					Pick.AddWidget(SNew(SBox).WidthOverride(320.0f).HeightOverride(360.0f)[ Browser.Get().CreateAssetPicker(Config) ], FText::GetEmpty(), true);
				}));
		}
		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("PickedDetails", "Details of the picked ({0})"), FText::AsNumber(Actors.Num())),
			LOCTEXT("PickedDetailsTip", "The editor's Details panel for what is picked in the level, in a window of its own."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Actors]()
			{
				TArray<UObject*> Objects;
				for (AActor* Actor : Actors) { Objects.Add(Actor); }
				FPropertyEditorModule& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
				Properties.CreateFloatingDetailsView(Objects, /*bIsLockable*/ false);
				SurfSession::Verb(TEXT("assets_details"), FString::Printf(TEXT("actors=%d"), Objects.Num()));
			}), FCanExecuteAction::CreateLambda([Actors]() { return Actors.Num() > 0; })));
		TArray<AStaticMeshActor*> Meshes;
		for (AActor* Actor : Actors)
		{
			if (AStaticMeshActor* M = Cast<AStaticMeshActor>(Actor)) { Meshes.Add(M); }
		}
		if (Meshes.Num() > 0)
		{
			Menu.AddSubMenu(LOCTEXT("PickedMaterials", "Materials"), LOCTEXT("PickedMaterialsTip", "The picked mesh's slots: pick another material for one, or its own back."),
				FNewMenuDelegate::CreateLambda([Meshes](FMenuBuilder& Sub) { FillMaterials(Sub, Meshes); }));
		}
	}
	friend void SurfAssetsTab::AddActorLines(FMenuBuilder& Menu);

	static void FillMaterials(FMenuBuilder& Sub, TArray<AStaticMeshActor*> Meshes)
	{
		UStaticMeshComponent* First = Meshes.Num() > 0 && Meshes[0] ? Meshes[0]->GetStaticMeshComponent() : nullptr;
		if (!First || !First->GetStaticMesh())
		{
			Sub.AddMenuEntry(LOCTEXT("NoSlots", "no mesh"), FText::GetEmpty(), FSlateIcon(), FUIAction());
			return;
		}
		const int32 Slots = First->GetNumMaterials();
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			UMaterialInterface* Worn = First->GetMaterial(Slot);
			const FName SlotName = First->GetStaticMesh()->GetStaticMaterials().IsValidIndex(Slot) ? First->GetStaticMesh()->GetStaticMaterials()[Slot].MaterialSlotName : NAME_None;
			const FText Label = FText::Format(LOCTEXT("SlotLine", "{0}  {1}   {2}"), FText::AsNumber(Slot),
				FText::FromName(SlotName), FText::FromString(Worn ? Worn->GetName() : TEXT("-")));
			Sub.AddSubMenu(Label, LOCTEXT("SlotTip", "Pick a material for this slot; the mesh's own puts it back."),
				FNewMenuDelegate::CreateLambda([Meshes, Slot](FMenuBuilder& Pick)
				{
					Pick.AddMenuEntry(LOCTEXT("SlotOwn", "The mesh's own"), FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([Meshes, Slot]()
						{
							FScopedTransaction Transaction(LOCTEXT("SlotOwnTx", "Reset Material"));
							for (AStaticMeshActor* M : Meshes)
							{
								if (M && M->GetStaticMeshComponent())
								{
									M->GetStaticMeshComponent()->Modify();
									M->GetStaticMeshComponent()->SetMaterial(Slot, nullptr);
								}
							}
						})));
					Pick.AddSeparator();
					FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
					FAssetPickerConfig Config;
					Config.Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
					Config.Filter.bRecursiveClasses = true;
					Config.InitialAssetViewType = EAssetViewType::List;
					Config.bAllowNullSelection = false;
					Config.bFocusSearchBoxWhenOpened = true;
					Config.bAllowDragging = false;
					Config.OnAssetSelected = FOnAssetSelected::CreateLambda([Meshes, Slot](const FAssetData& Data)
					{
						UMaterialInterface* Material = Cast<UMaterialInterface>(Data.GetAsset());
						if (!Material)
						{
							return;
						}
						FScopedTransaction Transaction(LOCTEXT("SlotPickTx", "Assign Material"));
						for (AStaticMeshActor* M : Meshes)
						{
							if (M && M->GetStaticMeshComponent())
							{
								M->GetStaticMeshComponent()->Modify();
								M->GetStaticMeshComponent()->SetMaterial(Slot, Material);
							}
						}
						SurfSession::Verb(TEXT("assets_material"), FString::Printf(TEXT("slot=%d %s"), Slot, *Material->GetName()));
						FSlateApplication::Get().DismissAllMenus();
					});
					Pick.AddWidget(SNew(SBox).WidthOverride(320.0f).HeightOverride(360.0f)[ Browser.Get().CreateAssetPicker(Config) ], FText::GetEmpty(), true);
				}));
		}
	}

	void Replace(const FAssetData& With)
	{
		USurfEdMode* Mode = AssetsMode();
		TArray<AActor*> Actors;
		PickedActors(Actors);
		if (!Mode || Actors.Num() == 0)
		{
			return;
		}
		const int32 Done = Mode->ReplaceActorsWith(Actors, With);
		SurfSession::Verb(TEXT("assets_replace"), FString::Printf(TEXT("%s -> %d"), *With.AssetName.ToString(), Done));
	}
};

namespace SurfAssetsTab
{
	TSharedRef<SWidget> MakePanel()
	{
		return SNew(SSurfAssetsView);
	}

	void AddActorLines(FMenuBuilder& Menu)
	{
		TArray<AActor*> Actors;
		PickedActors(Actors);
		SSurfAssetsView::AddActorLinesOf(Menu, Actors);
	}

	bool HasPickedActors()
	{
		TArray<AActor*> Actors;
		PickedActors(Actors);
		return Actors.Num() > 0;
	}
}

#undef LOCTEXT_NAMESPACE
