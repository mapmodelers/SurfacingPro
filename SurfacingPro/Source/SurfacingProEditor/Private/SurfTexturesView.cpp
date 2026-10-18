// Copyright MapModelers. All Rights Reserved.

#include "SurfTexturesView.h"
#include "SurfWindows.h"
#include "SurfTheme.h"

#include "SurfMaterialsView.h"
#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfTextureLib.h"
#include "SurfTextureSuggest.h"
#include "DragAndDrop/AssetDragDropOp.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Text/TextLayout.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IDesktopPlatform.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Async/Async.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProTextures"

// --- The disk, the pictures, the zoom, the marks ------------------------------------

namespace SurfTexturesLib
{
	FString NormalisedFolder(const FString& InFolder)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(InFolder.TrimStartAndEnd());
		FPaths::NormalizeDirectoryName(Dir);
		// A drive on its own is "C:", which means the current folder of C to the system; the slash makes it the root.
		if (Dir.Len() == 2 && Dir[1] == TEXT(':'))
		{
			Dir += TEXT("/");
		}
		return Dir;
	}

	bool ListFolder(const FString& InFolder, TArray<FSurfDiskEntry>& Out)
	{
		Out.Reset();
		const FString Dir = NormalisedFolder(InFolder);
		if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
		{
			return false;
		}

		TArray<FSurfDiskEntry> Folders;
		TArray<FSurfDiskEntry> Files;
		IFileManager::Get().IterateDirectory(*Dir, [&Folders, &Files](const TCHAR* Path, bool bIsDirectory)
		{
			FSurfDiskEntry Entry;
			Entry.Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Entry.Path);
			Entry.Name = FPaths::GetCleanFilename(Entry.Path);
			Entry.bFolder = bIsDirectory;
			Entry.Shown = Entry.Name;
			if (bIsDirectory)
			{
				if (!Entry.Name.StartsWith(TEXT(".")) && !Entry.Name.StartsWith(TEXT("$")))
				{
					Folders.Add(Entry);
				}
			}
			else if (SurfTextures::IsImageFile(Entry.Path))
			{
				Entry.Bytes = IFileManager::Get().FileSize(*Entry.Path);
				Entry.Modified = IFileManager::Get().GetTimeStamp(*Entry.Path);
				Entry.Shown = Entry.Name;
				Files.Add(Entry);
			}
			return true;
		});

		auto ByName = [](const FSurfDiskEntry& A, const FSurfDiskEntry& B) { return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0; };
		Folders.Sort(ByName);
		Files.Sort(ByName);
		Out.Append(Folders);
		Out.Append(Files);
		return true;
	}

	bool ListFolderRecursive(const FString& InFolder, TArray<FSurfDiskEntry>& Out, int32 MaxFiles, bool& bOutCapped)
	{
		Out.Reset();
		bOutCapped = false;
		const FString Dir = NormalisedFolder(InFolder);
		if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
		{
			return false;
		}
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Dir, TEXT("*.*"), /*Files*/ true, /*Directories*/ false, /*bClearFileNames*/ true);
		for (const FString& Path : Found)
		{
			if (!SurfTextures::IsImageFile(Path))
			{
				continue;
			}
			if (Out.Num() >= MaxFiles)
			{
				bOutCapped = true;
				break;
			}
			FSurfDiskEntry Entry;
			Entry.Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Entry.Path);
			Entry.Name = FPaths::GetCleanFilename(Entry.Path);
			Entry.Shown = Entry.Path;
			FPaths::MakePathRelativeTo(Entry.Shown, *(Dir + TEXT("/")));
			Entry.Bytes = IFileManager::Get().FileSize(*Entry.Path);
			Entry.Modified = IFileManager::Get().GetTimeStamp(*Entry.Path);
			Out.Add(Entry);
		}
		Out.Sort([](const FSurfDiskEntry& A, const FSurfDiskEntry& B) { return A.Shown.Compare(B.Shown, ESearchCase::IgnoreCase) < 0; });
		return true;
	}

	int32 SortOrders()
	{
		return 4;
	}

	const TCHAR* SortName(int32 Order)
	{
		switch (Order)
		{
		case 1: return TEXT("Newest first");
		case 2: return TEXT("Largest first");
		case 3: return TEXT("By kind");
		default: return TEXT("By name");
		}
	}

	void SortEntries(TArray<TSharedPtr<FSurfDiskEntry>>& Entries, int32 Order)
	{
		Entries.StableSort([Order](const TSharedPtr<FSurfDiskEntry>& PA, const TSharedPtr<FSurfDiskEntry>& PB)
		{
			if (!PA.IsValid() || !PB.IsValid())
			{
				return PA.IsValid();
			}
			const FSurfDiskEntry& A = *PA;
			const FSurfDiskEntry& B = *PB;
			if (A.bFolder != B.bFolder)
			{
				return A.bFolder;
			}
			const int32 ByName = A.Shown.Compare(B.Shown, ESearchCase::IgnoreCase);
			if (A.bFolder)
			{
				return ByName < 0;
			}
			switch (Order)
			{
			case 1: return A.Modified != B.Modified ? A.Modified > B.Modified : ByName < 0;
			case 2: return A.Bytes != B.Bytes ? A.Bytes > B.Bytes : ByName < 0;
			case 3:
			{
				const int32 KindA = static_cast<int32>(SurfTextures::KindFromName(A.Name));
				const int32 KindB = static_cast<int32>(SurfTextures::KindFromName(B.Name));
				return KindA != KindB ? KindA < KindB : ByName < 0;
			}
			default: return ByName < 0;
			}
		});
	}

	void ListSubfolders(const FString& InFolder, TArray<FString>& Out)
	{
		Out.Reset();
		const FString Dir = NormalisedFolder(InFolder);
		if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
		{
			return;
		}
		IFileManager::Get().IterateDirectory(*Dir, [&Out](const TCHAR* Path, bool bIsDirectory)
		{
			if (bIsDirectory)
			{
				FString Full = FPaths::ConvertRelativePathToFull(Path);
				FPaths::NormalizeDirectoryName(Full);
				const FString Name = FPaths::GetCleanFilename(Full);
				if (!Name.StartsWith(TEXT(".")) && !Name.StartsWith(TEXT("$")) && Name != TEXT("System Volume Information"))
				{
					Out.Add(Full);
				}
			}
			return true;
		});
		Out.Sort([](const FString& A, const FString& B) { return FPaths::GetCleanFilename(A).Compare(FPaths::GetCleanFilename(B), ESearchCase::IgnoreCase) < 0; });
	}

	void ListRoots(TArray<FString>& Out)
	{
		Out.Reset();
		auto Add = [&Out](const FString& In)
		{
			const FString Dir = NormalisedFolder(In);
			if (!Dir.IsEmpty() && IFileManager::Get().DirectoryExists(*Dir))
			{
				Out.AddUnique(Dir);
			}
		};
		Add(SurfTextures::SamplesDir());
		Add(SurfTextures::SourceRoot());
		Add(FPaths::ProjectDir());
		for (TCHAR Letter = TEXT('A'); Letter <= TEXT('Z'); ++Letter)
		{
			Add(FString::Printf(TEXT("%c:/"), Letter));
		}
	}

	FString BytesText(int64 Bytes)
	{
		if (Bytes >= 1024 * 1024)
		{
			return FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0));
		}
		if (Bytes >= 1024)
		{
			return FString::Printf(TEXT("%lld KB"), Bytes / 1024);
		}
		return FString::Printf(TEXT("%lld B"), Bytes);
	}

	bool ShrinkPicture(FImage& Image, int32 Size, FSurfSmallPicture& Out)
	{
		const int32 Width = Image.SizeX;
		const int32 Height = Image.SizeY;
		if (Width <= 0 || Height <= 0 || Image.NumSlices < 1)
		{
			return false;
		}
		if (Image.Format != ERawImageFormat::BGRA8)
		{
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		}
		if (Image.RawData.Num() < static_cast<int64>(Width) * Height * 4)
		{
			return false;
		}

		// Scaled to fit, alpha shown opaque: a picture to recognise a texture by, not the texture.
		const int32 Longest = FMath::Max(Width, Height);
		const double Scale = Longest > Size ? static_cast<double>(Size) / Longest : 1.0;
		Out.Width = FMath::Max(1, FMath::RoundToInt(Width * Scale));
		Out.Height = FMath::Max(1, FMath::RoundToInt(Height * Scale));

		TArray<FColor> Source;
		Source.SetNumUninitialized(Width * Height);
		FMemory::Memcpy(Source.GetData(), Image.RawData.GetData(), static_cast<SIZE_T>(Source.Num()) * sizeof(FColor));
		Out.Pixels.Reset();
		FImageUtils::ImageResize(Width, Height, Source, Out.Width, Out.Height, Out.Pixels, /*bLinearSpace*/ false, /*bForceOpaque*/ true);
		return Out.Pixels.Num() == Out.Width * Out.Height;
	}

	UTexture2D* TextureOf(const FSurfSmallPicture& Small)
	{
		if (Small.Pixels.Num() != Small.Width * Small.Height || Small.Width <= 0)
		{
			return nullptr;
		}
		UTexture2D* Texture = UTexture2D::CreateTransient(Small.Width, Small.Height, PF_B8G8R8A8, NAME_None,
			TConstArrayView64<uint8>(reinterpret_cast<const uint8*>(Small.Pixels.GetData()), static_cast<int64>(Small.Pixels.Num()) * sizeof(FColor)));
		if (Texture)
		{
			Texture->SRGB = true;
			Texture->UpdateResource();
		}
		return Texture;
	}

	UTexture2D* MakePicture(FImage& Image, int32 Size)
	{
		FSurfSmallPicture Small;
		return ShrinkPicture(Image, Size, Small) ? TextureOf(Small) : nullptr;
	}

	bool DecodeSmall(const FString& File, int32 Size, FSurfSmallPicture& Out)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*File);
		if (Bytes <= 0 || Bytes > 48LL * 1024 * 1024)
		{
			return false;
		}

		TArray64<uint8> Compressed;
		if (!FFileHelper::LoadFileToArray(Compressed, *File))
		{
			return false;
		}

		// Decoded in the file's own layout - 8-bit grey, 16-bit, float - and made
		// BGRA here. Asking a reader for another layout is not always graceful:
		// the engine's TGA reader asserts on a grey file asked for BGRA.
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		FImage Image;
		if (!Module.DecompressImage(Compressed.GetData(), Compressed.Num(), Image))
		{
			return false;
		}
		return ShrinkPicture(Image, Size, Out);
	}

	UTexture2D* MakeThumbnail(const FString& File, int32 Size)
	{
		FSurfSmallPicture Small;
		return DecodeSmall(File, Size, Small) ? TextureOf(Small) : nullptr;
	}

	UTexture2D* MakeAssetPicture(UTexture2D* Texture, int32 Size)
	{
		if (!Texture || !Texture->Source.IsValid())
		{
			return nullptr;
		}
		if (static_cast<int64>(Texture->Source.GetSizeX()) * Texture->Source.GetSizeY() > 8192LL * 8192LL)
		{
			return nullptr;
		}
		FImage Image;
		if (!Texture->Source.GetMipImage(Image, /*BlockIndex*/ 0, /*LayerIndex*/ 0, /*MipIndex*/ 0))
		{
			return nullptr;
		}
		return MakePicture(Image, Size);
	}

	// --- Zoom -----------------------------------------------------------------

	namespace
	{
		constexpr int32 ZoomSizes[] = { 40, 64, 96, 128, 192 };
	}

	int32 ZoomLevels()
	{
		return UE_ARRAY_COUNT(ZoomSizes);
	}

	int32 ZoomSize(int32 Level)
	{
		return ZoomSizes[FMath::Clamp(Level, 0, ZoomLevels() - 1)];
	}

	bool ZoomIsTiles(int32 Level)
	{
		return Level > 0;
	}

	int32 StepZoom(int32 Level, float WheelDelta)
	{
		const int32 Step = WheelDelta > 0.0f ? 1 : (WheelDelta < 0.0f ? -1 : 0);
		return FMath::Clamp(Level + Step, 0, ZoomLevels() - 1);
	}

	// --- The marks ---------------------------------------------------------------

	FString NormalisedFile(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path.TrimStartAndEnd());
		FPaths::NormalizeFilename(Full);
		return Full.ToLower();
	}

	void IndexLibrary(const FString& LibraryRoot, FSurfLibraryIndex& Out)
	{
		Out.ByName.Reset();
		Out.BySource.Reset();
		TArray<FAssetData> Assets;
		SurfTextures::ListProject(LibraryRoot, Assets);
		for (const FAssetData& Asset : Assets)
		{
			const FString Name = Asset.AssetName.ToString();
			Out.ByName.Add(Name, Asset.GetObjectPathString());
			const FString Source = SurfTextures::SourceFileOf(Asset);
			if (!Source.IsEmpty())
			{
				Out.BySource.Add(NormalisedFile(Source), Name);
			}
		}
	}

	FString InLibrary(const FSurfLibraryIndex& Index, const FString& File)
	{
		if (const FString* Linked = Index.BySource.Find(NormalisedFile(File)))
		{
			return *Linked;
		}
		const FString Name = SurfTextures::AssetNameFor(SurfTextures::StemFrom(FPaths::GetCleanFilename(File)));
		return Index.ByName.Contains(Name) ? Name : FString();
	}

	bool IsLinked(const FSurfLibraryIndex& Index, const FString& File)
	{
		return Index.BySource.Contains(NormalisedFile(File));
	}
}

// --- What the tabs share ---------------------------------------------------------------

namespace
{
	const TCHAR* TexturesIniSection = TEXT("SurfacingPro.Textures");
	TSharedPtr<FSurfTexturesShared> GTexturesShared;
	TWeakPtr<SSurfTextureFilesView> GTexturesFilesView;
}

FSurfTexturesShared& FSurfTexturesShared::Get()
{
	if (!GTexturesShared.IsValid())
	{
		GTexturesShared = MakeShared<FSurfTexturesShared>();
		GTexturesShared->Load();
	}
	return *GTexturesShared;
}

void FSurfTexturesShared::Load()
{
	if (!GConfig)
	{
		return;
	}
	GConfig->GetString(TexturesIniSection, TEXT("Folder"), Folder, GEditorPerProjectIni);
	GConfig->GetInt(TexturesIniSection, TEXT("Zoom"), Zoom, GEditorPerProjectIni);
	GConfig->GetBool(TexturesIniSection, TEXT("Conform"), bConform, GEditorPerProjectIni);
	GConfig->GetBool(TexturesIniSection, TEXT("HideImported"), bHideImported, GEditorPerProjectIni);
	GConfig->GetBool(TexturesIniSection, TEXT("FoldersShown"), bFoldersShown, GEditorPerProjectIni);
	GConfig->GetBool(TexturesIniSection, TEXT("Recursive"), bRecursive, GEditorPerProjectIni);
	GConfig->GetInt(TexturesIniSection, TEXT("SortBy"), SortBy, GEditorPerProjectIni);
	FString Joined;
	GConfig->GetString(TexturesIniSection, TEXT("Favorites"), Joined, GEditorPerProjectIni);
	Favorites.Reset();
	Joined.ParseIntoArray(Favorites, TEXT("|"), true);
	SortBy = FMath::Clamp(SortBy, 0, SurfTexturesLib::SortOrders() - 1);
	Zoom = FMath::Clamp(Zoom, 0, SurfTexturesLib::ZoomLevels() - 1);
}

void FSurfTexturesShared::Save() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetString(TexturesIniSection, TEXT("Folder"), *Folder, GEditorPerProjectIni);
	GConfig->SetInt(TexturesIniSection, TEXT("Zoom"), Zoom, GEditorPerProjectIni);
	GConfig->SetBool(TexturesIniSection, TEXT("Conform"), bConform, GEditorPerProjectIni);
	GConfig->SetBool(TexturesIniSection, TEXT("HideImported"), bHideImported, GEditorPerProjectIni);
	GConfig->SetBool(TexturesIniSection, TEXT("FoldersShown"), bFoldersShown, GEditorPerProjectIni);
	GConfig->SetBool(TexturesIniSection, TEXT("Recursive"), bRecursive, GEditorPerProjectIni);
	GConfig->SetInt(TexturesIniSection, TEXT("SortBy"), SortBy, GEditorPerProjectIni);
	GConfig->SetString(TexturesIniSection, TEXT("Favorites"), *FString::Join(Favorites, TEXT("|")), GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// --- Shared widget bits ------------------------------------------------------------------

namespace
{
	/** The largest picture size; a picture is decoded once at this and shown at any size below. */
	constexpr int32 TexturesThumbMax = 192;
	constexpr double TexturesNoticeSeconds = 5.0;
	constexpr double TexturesReadEverySeconds = 0.5;

	const FLinearColor TexturesMarkColour(0.25f, 0.8f, 0.35f);

	TSharedRef<SWidget> TexturesButton(const FText& Label, const FText& Tip, TFunction<void()> Run, TAttribute<bool> Enabled = true)
	{
		return SNew(SButton)
			.ButtonStyle(&SurfTheme::ToolButton())
			.Text(Label)
			.ToolTipText(Tip)
			.IsEnabled(Enabled)
			.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); });
	}

	/** A list or tile view that hands Ctrl+wheel to the window instead of scrolling. */
	template<typename BaseView>
	class SSurfZoomView : public BaseView
	{
	public:
		TFunction<void(float)> OnZoomWheel;

		virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
		{
			if (MouseEvent.IsControlDown() && OnZoomWheel)
			{
				OnZoomWheel(MouseEvent.GetWheelDelta());
				return FReply::Handled();
			}
			return BaseView::OnMouseWheel(MyGeometry, MouseEvent);
		}
	};

	using FDiskListView = SSurfZoomView<SListView<TSharedPtr<FSurfDiskEntry>>>;
	using FDiskTileView = SSurfZoomView<STileView<TSharedPtr<FSurfDiskEntry>>>;
	using FRowListView = SSurfZoomView<SListView<TSharedPtr<FSurfLibraryRow>>>;
	using FRowTileView = SSurfZoomView<STileView<TSharedPtr<FSurfLibraryRow>>>;

	/** A tile: the picture over the name, the name cut with an ellipsis when the tile is narrower. */
	TSharedRef<SWidget> TileBody(TSharedRef<SWidget> PictureWidget, const FString& Name, int32 Size)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox).WidthOverride(Size).HeightOverride(Size)
				[
					PictureWidget
				]
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(2.0f, 2.0f, 2.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(static_cast<float>(Size)).Clipping(EWidgetClipping::ClipToBounds)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Name))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			];
	}

	/** The rows or the tiles, whichever the zoom says. */
	TSharedRef<SWidget> RowsOrTiles(TSharedRef<SWidget> Rows, TSharedRef<SWidget> Tiles, TAttribute<int32> Index)
	{
		return SNew(SWidgetSwitcher)
			.WidgetIndex(Index)
			+ SWidgetSwitcher::Slot()
			[
				Rows
			]
			+ SWidgetSwitcher::Slot()
			[
				Tiles
			];
	}

	/** The import, as the shared mode says. */
	SurfTextures::FSurfTextureImportReport ImportAsShared(const TArray<FString>& Files)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		SurfTextures::FSurfTextureImportOptions Options;
		Options.LibraryRoot = Settings->MaterialLibraryRoot;
		Options.bConform = FSurfTexturesShared::Get().bConform;
		Options.MaxSize = Settings->TextureImportMaxSize;
		Options.bSave = true;
		const SurfTextures::FSurfTextureImportReport Report = SurfTextures::Import(Files, Options);
		for (int32 Entry = Report.Entries.Num() - 1; Entry >= 0; --Entry)
		{
			if (Report.Entries[Entry].Error.IsEmpty() && Report.Entries[Entry].Texture)
			{
				FSurfTexturesShared::Get().LastImported = Report.Entries[Entry].AssetPath;
				break;
			}
		}
		return Report;
	}
}

// =====================================================================================
// The Texture Files tab
// =====================================================================================

FReply SSurfTextureFilesView::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
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

void SSurfTextureFilesView::Construct(const FArguments& InArgs)
{
	SetCanTick(true);
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	ZoomShown = Shared.Zoom;

	// The tree's roots: the favorites, then the places, then the drives.
	FavoritesRoot = MakeShared<FSurfFolderNode>();
	FavoritesRoot->Name = TEXT("Favourites");
	FavoritesRoot->bScanned = true;
	RebuildFavorites();
	FolderRoots.Add(FavoritesRoot);
	TArray<FString> Roots;
	SurfTexturesLib::ListRoots(Roots);
	const FString Samples = SurfTexturesLib::NormalisedFolder(SurfTextures::SamplesDir());
	const FString SourceRoot = SurfTexturesLib::NormalisedFolder(SurfTextures::SourceRoot());
	const FString Project = SurfTexturesLib::NormalisedFolder(FPaths::ProjectDir());
	for (const FString& Root : Roots)
	{
		TSharedRef<FSurfFolderNode> Node = MakeShared<FSurfFolderNode>();
		Node->Path = Root;
		if (Root == Samples)
		{
			Node->Name = TEXT("Samples");
		}
		else if (Root == SourceRoot)
		{
			Node->Name = TEXT("Source root");
		}
		else if (Root == Project)
		{
			Node->Name = TEXT("Project");
		}
		else
		{
			const FString Clean = FPaths::GetCleanFilename(Root);
			Node->Name = Clean.IsEmpty() ? Root : Clean;
		}
		FolderRoots.Add(Node);
	}

	// The views over the folder.
	TSharedRef<FDiskListView> DiskListRef = SNew(FDiskListView)
		.ListItemsSource(&DiskEntries)
		.SelectionMode(ESelectionMode::Multi)
		.OnGenerateRow(this, &SSurfTextureFilesView::MakeDiskRow)
		.OnMouseButtonDoubleClick(this, &SSurfTextureFilesView::OnDiskDoubleClick)
		.OnSelectionChanged(this, &SSurfTextureFilesView::OnDiskSelectionChanged)
		.OnContextMenuOpening(this, &SSurfTextureFilesView::OpenDiskMenu);
	DiskListRef->OnZoomWheel = [this](float Delta) { OnZoomWheel(Delta); };
	DiskList = DiskListRef;

	TSharedRef<FDiskTileView> DiskTilesRef = SNew(FDiskTileView)
		.ListItemsSource(&DiskEntries)
		.SelectionMode(ESelectionMode::Multi)
		.ItemWidth(this, &SSurfTextureFilesView::TileWidth)
		.ItemHeight(this, &SSurfTextureFilesView::TileHeight)
		.ItemAlignment(EListItemAlignment::LeftAligned)
		.OnGenerateTile(this, &SSurfTextureFilesView::MakeDiskTile)
		.OnMouseButtonDoubleClick(this, &SSurfTextureFilesView::OnDiskDoubleClick)
		.OnSelectionChanged(this, &SSurfTextureFilesView::OnDiskSelectionChanged)
		.OnContextMenuOpening(this, &SSurfTextureFilesView::OpenDiskMenu);
	DiskTilesRef->OnZoomWheel = [this](float Delta) { OnZoomWheel(Delta); };
	DiskTiles = DiskTilesRef;

	ChildSlot
	[
		SNew(SVerticalBox)

		// Where on the disk.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked(this, &SSurfTextureFilesView::FoldersState)
				.OnCheckStateChanged(this, &SSurfTextureFilesView::OnFoldersToggled)
				.ToolTipText(LOCTEXT("FoldersTip", "The folder tree on the left: shown, or folded away."))
				[
					SNew(STextBlock).Text(LOCTEXT("Folders", "Folders"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(PathBox, SEditableTextBox)
				.HintText(LOCTEXT("PathHint", "A folder on this machine"))
				.OnTextCommitted(this, &SSurfTextureFilesView::OnPathCommitted)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				TexturesButton(LOCTEXT("Up", "Up"), LOCTEXT("UpTip", "The folder above."), [this]() { GoUp(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				TexturesButton(LOCTEXT("PickFiles", "Pick files..."), LOCTEXT("PickFilesTip", "Pick files with the system's dialog; they come in as More says - conform, or linked."), [this]() { PickFiles(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				TexturesButton(LOCTEXT("Import", "Import"), LOCTEXT("ImportTip", "Bring the picked files into the library as T_Sp_ textures, their kind read off their suffix (_N a normal map, _R _AO _M _H linear, else colour). Double-click a file does the same; a green mark means it is in the library already; Ctrl+wheel for bigger pictures."), [this]() { ImportPicked(); })
			]
			// The rest under the arrow (S252e): "two rows of mixed chips,
			// buttons, radios and ticks" became one row.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SurfWindows::MenuBarPullDown(LOCTEXT("FilesMore", "More"),
					LOCTEXT("FilesMoreTip", "Samples, this folder in Explorer, Favourite; how the files come in (conform, or linked); the filters; the order."),
					[this]() { return MakeMoreMenu(); })
			]
		]

		// The tree and the folder.
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 2.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			+ SSplitter::Slot().Value(0.28f)
			[
				SNew(SBox)
				.Visibility(this, &SSurfTextureFilesView::FoldersVisibility)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SAssignNew(FolderTree, STreeView<FFolderPtr>)
					.TreeItemsSource(&FolderRoots)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SSurfTextureFilesView::MakeFolderRow)
					.OnGetChildren(this, &SSurfTextureFilesView::FolderChildren)
					.OnSelectionChanged(this, &SSurfTextureFilesView::OnFolderSelected)
				]
			]
			+ SSplitter::Slot().Value(0.72f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					RowsOrTiles(DiskListRef, DiskTilesRef, TAttribute<int32>(this, &SSurfTextureFilesView::ZoomIndex))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(this, &SSurfTextureFilesView::EmptyDiskText)
					.Visibility(this, &SSurfTextureFilesView::EmptyDiskVisibility)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			]
		]

		// The foot.
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 6.0f)
		[
			SNew(STextBlock).Text(this, &SSurfTextureFilesView::FootText).AutoWrapText(true)
		]
	];

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.OnAssetAdded().AddSP(this, &SSurfTextureFilesView::OnAssetChanged);
	Registry.OnAssetRemoved().AddSP(this, &SSurfTextureFilesView::OnAssetChanged);
	Registry.OnAssetRenamed().AddSP(this, &SSurfTextureFilesView::OnAssetRenamed);
	Registry.OnFilesLoaded().AddSP(this, &SSurfTextureFilesView::MarkDirty);

	GTexturesFilesView = SharedThis(this);
	RefreshIndex();
	const FString Remembered = SurfTexturesLib::NormalisedFolder(Shared.Folder);
	GoTo(!Remembered.IsEmpty() && IFileManager::Get().DirectoryExists(*Remembered) ? Remembered : SurfTextures::SamplesDir());
}

SSurfTextureFilesView::~SSurfTextureFilesView()
{
	ThumbnailBrushes.Empty();
	Thumbnails.Empty();
}

// --- Dropped files ---------------------------------------------------------------------

namespace
{
	/** The files an Explorer drag carries, image files only; empty for any other drag. */
	TArray<FString> DroppedImages(const FDragDropEvent& DragDropEvent)
	{
		TArray<FString> Out;
		TSharedPtr<FExternalDragOperation> Drag = DragDropEvent.GetOperationAs<FExternalDragOperation>();
		if (Drag.IsValid() && Drag->HasFiles())
		{
			for (const FString& File : Drag->GetFiles())
			{
				if (SurfTextures::IsImageFile(File))
				{
					Out.Add(File);
				}
			}
		}
		return Out;
	}
}

FReply SSurfTextureFilesView::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	return DroppedImages(DragDropEvent).Num() > 0 ? FReply::Handled() : FReply::Unhandled();
}

FReply SSurfTextureFilesView::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TArray<FString> Files = DroppedImages(DragDropEvent);
	if (Files.Num() == 0)
	{
		return FReply::Unhandled();
	}
	PickDropped(Files);
	return FReply::Handled();
}

void SSurfTextureFilesView::PickDropped(const TArray<FString>& Files)
{
	if (Files.Num() == 0)
	{
		return;
	}
	// Their folder opens - sub-folders off, so every one has a row - and they are picked.
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	if (Shared.bRecursive)
	{
		Shared.bRecursive = false;
		Shared.Save();
	}
	GoTo(FPaths::GetPath(Files[0]));
	TSharedPtr<SListView<FDiskPtr>> View = ActiveDiskView();
	if (!View.IsValid())
	{
		return;
	}
	View->ClearSelection();
	int32 Picked = 0;
	for (const FString& File : Files)
	{
		const FString Key = SurfTexturesLib::NormalisedFile(File);
		for (const FDiskPtr& Entry : DiskEntries)
		{
			if (Entry.IsValid() && !Entry->bFolder && SurfTexturesLib::NormalisedFile(Entry->Path) == Key)
			{
				View->SetItemSelection(Entry, true, ESelectInfo::Direct);
				++Picked;
				break;
			}
		}
	}
	Shared.PickedFiles = PickedFiles();
	Say(Picked == Files.Num()
		? FString::Printf(TEXT("picked %d dropped file%s: Import brings them in"), Picked, Picked == 1 ? TEXT("") : TEXT("s"))
		: FString::Printf(TEXT("picked %d of %d dropped files - the rest are in other folders, or hidden as in the library"), Picked, Files.Num()));
}

void SSurfTextureFilesView::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	const FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	const double Now = FPlatformTime::Seconds();
	const bool bRootChanged = GetDefault<USurfSettings>()->MaterialLibraryRoot != IndexRoot;
	if (bRootChanged || (bIndexDirty && Now - LastIndexed > TexturesReadEverySeconds))
	{
		RefreshIndex();
	}
	if (Shared.Zoom != ZoomShown)
	{
		ApplyZoom();
	}
	PumpThumbnails();
}

// --- The folder ------------------------------------------------------------------------

void SSurfTextureFilesView::GoTo(const FString& InFolder, bool bFromTree)
{
	const FString Dir = SurfTexturesLib::NormalisedFolder(InFolder);
	Folder = Dir;
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	if (Shared.Folder != Dir)
	{
		Shared.Folder = Dir;
		Shared.Save();
	}

	TArray<FSurfDiskEntry> Entries;
	bCapped = false;
	bFolderReadable = Shared.bRecursive
		? SurfTexturesLib::ListFolderRecursive(Dir, Entries, 5000, bCapped)
		: SurfTexturesLib::ListFolder(Dir, Entries);
	AllEntries.Reset();
	for (FSurfDiskEntry& Entry : Entries)
	{
		AllEntries.Add(MakeShared<FSurfDiskEntry>(MoveTemp(Entry)));
	}
	ApplyFilter();
	if (PathBox.IsValid())
	{
		PathBox->SetText(FText::FromString(Folder));
	}
	if (DiskList.IsValid())
	{
		DiskList->ClearSelection();
	}
	if (DiskTiles.IsValid())
	{
		DiskTiles->ClearSelection();
	}
	Shared.PickedFiles.Reset();
	if (!bFromTree)
	{
		RevealInTree(Folder);
	}
	// Pictures of the folder just left are not worth keeping past a few
	// hundred (S208a: 96 threw a big folder's away as it scrolled); the
	// asks that never got their turn go.
	ThumbnailWanted.Reset();
	if (ThumbnailBrushes.Num() > 600)
	{
		ThumbnailBrushes.Empty();
		Thumbnails.Empty();
	}
}

void SSurfTextureFilesView::ApplyFilter()
{
	const bool bHide = FSurfTexturesShared::Get().bHideImported;
	DiskEntries.Reset();
	for (const FDiskPtr& Entry : AllEntries)
	{
		if (bHide && Entry.IsValid() && !Entry->bFolder && !SurfTexturesLib::InLibrary(Index, Entry->Path).IsEmpty())
		{
			continue;
		}
		DiskEntries.Add(Entry);
	}
	SurfTexturesLib::SortEntries(DiskEntries, FSurfTexturesShared::Get().SortBy);
	if (DiskList.IsValid())
	{
		DiskList->RequestListRefresh();
	}
	if (DiskTiles.IsValid())
	{
		DiskTiles->RequestListRefresh();
	}
}

ECheckBoxState SSurfTextureFilesView::FavoriteState() const
{
	for (const FString& Favorite : FSurfTexturesShared::Get().Favorites)
	{
		if (Favorite.Equals(Folder, ESearchCase::IgnoreCase))
		{
			return ECheckBoxState::Checked;
		}
	}
	return ECheckBoxState::Unchecked;
}

void SSurfTextureFilesView::OnFavoriteToggled(ECheckBoxState State)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	Shared.Favorites.RemoveAll([this](const FString& Favorite) { return Favorite.Equals(Folder, ESearchCase::IgnoreCase); });
	if (State == ECheckBoxState::Checked && !Folder.IsEmpty())
	{
		Shared.Favorites.Add(Folder);
	}
	Shared.Save();
	RebuildFavorites();
}

void SSurfTextureFilesView::RebuildFavorites()
{
	if (!FavoritesRoot.IsValid())
	{
		return;
	}
	FavoritesRoot->Children.Reset();
	for (const FString& Favorite : FSurfTexturesShared::Get().Favorites)
	{
		TSharedRef<FSurfFolderNode> Node = MakeShared<FSurfFolderNode>();
		Node->Path = Favorite;
		const FString Clean = FPaths::GetCleanFilename(Favorite);
		Node->Name = Clean.IsEmpty() ? Favorite : Clean;
		FavoritesRoot->Children.Add(Node);
	}
	if (FolderTree.IsValid())
	{
		FolderTree->SetItemExpansion(FavoritesRoot, true);
		FolderTree->RequestTreeRefresh();
	}
}

ECheckBoxState SSurfTextureFilesView::RecursiveState() const
{
	return FSurfTexturesShared::Get().bRecursive ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSurfTextureFilesView::OnRecursiveToggled(ECheckBoxState State)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	Shared.bRecursive = State == ECheckBoxState::Checked;
	Shared.Save();
	GoTo(Folder, /*bFromTree*/ true);
}

TSharedRef<SWidget> SSurfTextureFilesView::MakeMoreMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	Menu.AddMenuEntry(LOCTEXT("Samples", "Samples"), LOCTEXT("SamplesTip", "The plugin's sample textures, to author from."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::GoToSamples)));
	Menu.AddMenuEntry(LOCTEXT("InExplorer", "In Explorer"), LOCTEXT("InExplorerTip", "Open this folder in Explorer."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::OpenFolderInExplorer)));
	Menu.AddMenuEntry(LOCTEXT("Favorite", "Favourite"), LOCTEXT("FavoriteTip", "Pin this folder at the top of the tree, under Favourites; again to unpin it."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { OnFavoriteToggled(FavoriteState() == ECheckBoxState::Checked ? ECheckBoxState::Unchecked : ECheckBoxState::Checked); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([this]() { return FavoriteState() == ECheckBoxState::Checked; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddSeparator();
	Menu.BeginSection(NAME_None, LOCTEXT("FilesHow", "How they come in"));
	Menu.AddMenuEntry(LOCTEXT("Conform", "Conform"), LOCTEXT("ConformTip", "The file is rewritten as our TGA under the source root and the texture reads that: the file the team keeps is the truth."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]() { FSurfTexturesShared::Get().bConform = true; FSurfTexturesShared::Get().Save(); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { return FSurfTexturesShared::Get().bConform; })),
		NAME_None, EUserInterfaceActionType::RadioButton);
	Menu.AddMenuEntry(LOCTEXT("Link", "Link"), LOCTEXT("LinkTip", "The texture reads the file where it is. Portable when the file is inside the project's tree, local only when it is not."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]() { FSurfTexturesShared::Get().bConform = false; FSurfTexturesShared::Get().Save(); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { return !FSurfTexturesShared::Get().bConform; })),
		NAME_None, EUserInterfaceActionType::RadioButton);
	Menu.EndSection();
	Menu.BeginSection(NAME_None, LOCTEXT("FilesShown", "Shown"));
	Menu.AddMenuEntry(LOCTEXT("HideImported", "Hide those in the library"), LOCTEXT("HideImportedTip", "Leave out the files that are in the library already - linked from here, or named after one (T_Sp_<name>). Shown, they carry a green mark."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { OnHideImportedChanged(HideImportedState() == ECheckBoxState::Checked ? ECheckBoxState::Unchecked : ECheckBoxState::Checked); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([this]() { return HideImportedState() == ECheckBoxState::Checked; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddMenuEntry(LOCTEXT("Recursive", "Sub-folders too"), LOCTEXT("RecursiveTip", "The images of every folder under this one too, shown by their path from here (up to 5000)."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { OnRecursiveToggled(RecursiveState() == ECheckBoxState::Checked ? ECheckBoxState::Unchecked : ECheckBoxState::Checked); }),
			FCanExecuteAction(), FIsActionChecked::CreateLambda([this]() { return RecursiveState() == ECheckBoxState::Checked; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddSubMenu(LOCTEXT("FilesOrder", "Order"), LOCTEXT("SortTip", "The order of the files: by name, newest first, largest first, by kind."),
		FNewMenuDelegate::CreateLambda([this](FMenuBuilder& Sub)
		{
			for (int32 Order = 0; Order < SurfTexturesLib::SortOrders(); ++Order)
			{
				Sub.AddMenuEntry(FText::FromString(SurfTexturesLib::SortName(Order)), FText(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::SetSort, Order),
						FCanExecuteAction(), FIsActionChecked::CreateLambda([Order]() { return FSurfTexturesShared::Get().SortBy == Order; })),
					NAME_None, EUserInterfaceActionType::RadioButton);
			}
		}));
	Menu.EndSection();
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SSurfTextureFilesView::MakeSortMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	for (int32 Order = 0; Order < SurfTexturesLib::SortOrders(); ++Order)
	{
		Menu.AddMenuEntry(FText::FromString(SurfTexturesLib::SortName(Order)), FText(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::SetSort, Order)));
	}
	return Menu.MakeWidget();
}

FText SSurfTextureFilesView::SortLabel() const
{
	return FText::FromString(SurfTexturesLib::SortName(FSurfTexturesShared::Get().SortBy));
}

void SSurfTextureFilesView::SetSort(int32 Order)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	Shared.SortBy = FMath::Clamp(Order, 0, SurfTexturesLib::SortOrders() - 1);
	Shared.Save();
	ApplyFilter();
}

void SSurfTextureFilesView::GoUp()
{
	const FString Above = FPaths::GetPath(Folder);
	if (!Above.IsEmpty() && SurfTexturesLib::NormalisedFolder(Above) != Folder)
	{
		GoTo(Above);
	}
}

void SSurfTextureFilesView::GoToSamples()
{
	GoTo(SurfTextures::SamplesDir());
}

void SSurfTextureFilesView::OpenFolderInExplorer()
{
	if (Folder.IsEmpty() || !IFileManager::Get().DirectoryExists(*Folder))
	{
		Say(TEXT("explorer: no folder to open"));
		return;
	}
	FPlatformProcess::ExploreFolder(*Folder);
}

void SSurfTextureFilesView::PickFiles()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		Say(TEXT("pick files: the system's dialog is not there"));
		return;
	}
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	TArray<FString> Files;
	const bool bPicked = Desktop->OpenFileDialog(Parent, TEXT("Textures to bring in"), Folder, TEXT(""),
		TEXT("Images (*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.exr;*.tif;*.tiff;*.psd;*.hdr)|*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.exr;*.tif;*.tiff;*.psd;*.hdr|All files (*.*)|*.*"),
		EFileDialogFlags::Multiple, Files);
	if (bPicked && Files.Num() > 0)
	{
		GoTo(FPaths::GetPath(Files[0]));
		ImportFiles(Files);
	}
}

void SSurfTextureFilesView::OnPathCommitted(const FText& Text, ETextCommit::Type Commit)
{
	if (Commit != ETextCommit::OnEnter)
	{
		return;
	}
	FString Dir = Text.ToString().TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
	if (Dir.EndsWith(TEXT(":")))
	{
		Dir += TEXT("/");
	}
	if (IFileManager::Get().DirectoryExists(*Dir))
	{
		GoTo(Dir);
	}
	else
	{
		Say(FString::Printf(TEXT("folder: %s is not there"), *Dir));
	}
}

// --- The tree -----------------------------------------------------------------------------

TSharedRef<ITableRow> SSurfTextureFilesView::MakeFolderRow(FFolderPtr Node, const TSharedRef<STableViewBase>& Owner)
{
	const FString Name = Node.IsValid() ? Node->Name : FString();
	const FString Path = Node.IsValid() ? Node->Path : FString();
	return SNew(STableRow<FFolderPtr>, Owner)
		.ToolTipText(FText::FromString(Path))
		.Padding(FMargin(2.0f, 1.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("ContentBrowser.AssetTreeFolderClosed"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Name)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];
}

void SSurfTextureFilesView::FolderChildren(FFolderPtr Node, TArray<FFolderPtr>& Out)
{
	Out.Reset();
	if (!Node.IsValid())
	{
		return;
	}
	if (!Node->bScanned)
	{
		Node->bScanned = true;
		TArray<FString> Subs;
		SurfTexturesLib::ListSubfolders(Node->Path, Subs);
		for (const FString& Sub : Subs)
		{
			TSharedRef<FSurfFolderNode> Child = MakeShared<FSurfFolderNode>();
			Child->Path = Sub;
			Child->Name = FPaths::GetCleanFilename(Sub);
			Node->Children.Add(Child);
		}
	}
	Out = Node->Children;
}

void SSurfTextureFilesView::OnFolderSelected(FFolderPtr Node, ESelectInfo::Type How)
{
	if (bRevealing || How == ESelectInfo::Direct || !Node.IsValid() || Node->Path.IsEmpty())
	{
		return;
	}
	if (!Node->Path.Equals(Folder, ESearchCase::IgnoreCase))
	{
		GoTo(Node->Path, /*bFromTree*/ true);
	}
}

void SSurfTextureFilesView::RevealInTree(const FString& InFolder)
{
	if (!FolderTree.IsValid())
	{
		return;
	}

	// The root that holds it - the longest match - then down one segment at a time, reading as we go.
	FFolderPtr Node;
	for (const FFolderPtr& Root : FolderRoots)
	{
		if (Root.IsValid() && !Root->Path.IsEmpty() && (InFolder.Equals(Root->Path, ESearchCase::IgnoreCase) || InFolder.StartsWith(Root->Path.EndsWith(TEXT("/")) ? Root->Path : Root->Path + TEXT("/"), ESearchCase::IgnoreCase)))
		{
			if (!Node.IsValid() || Root->Path.Len() > Node->Path.Len())
			{
				Node = Root;
			}
		}
	}
	if (!Node.IsValid())
	{
		return;
	}

	bRevealing = true;
	FolderTree->SetItemExpansion(Node, true);
	while (!Node->Path.Equals(InFolder, ESearchCase::IgnoreCase))
	{
		TArray<FFolderPtr> Kids;
		FolderChildren(Node, Kids);
		FFolderPtr Next;
		for (const FFolderPtr& Kid : Kids)
		{
			if (Kid.IsValid() && (InFolder.Equals(Kid->Path, ESearchCase::IgnoreCase) || InFolder.StartsWith(Kid->Path + TEXT("/"), ESearchCase::IgnoreCase)))
			{
				Next = Kid;
				break;
			}
		}
		if (!Next.IsValid())
		{
			break;
		}
		Node = Next;
		FolderTree->SetItemExpansion(Node, true);
	}
	FolderTree->SetSelection(Node, ESelectInfo::Direct);
	FolderTree->RequestTreeRefresh();
	FolderTree->RequestScrollIntoView(Node);
	bRevealing = false;
}

EVisibility SSurfTextureFilesView::FoldersVisibility() const
{
	return FSurfTexturesShared::Get().bFoldersShown ? EVisibility::Visible : EVisibility::Collapsed;
}

ECheckBoxState SSurfTextureFilesView::FoldersState() const
{
	return FSurfTexturesShared::Get().bFoldersShown ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSurfTextureFilesView::OnFoldersToggled(ECheckBoxState State)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	Shared.bFoldersShown = State == ECheckBoxState::Checked;
	Shared.Save();
	if (Shared.bFoldersShown)
	{
		RevealInTree(Folder);
	}
}

// --- The files -----------------------------------------------------------------------------

const FSlateBrush* SSurfTextureFilesView::ThumbnailFor(const FString& File)
{
	if (const TSharedPtr<FSlateBrush>* Known = ThumbnailBrushes.Find(File))
	{
		return Known->Get();
	}
	// Not decoded on this thread (S208a): "textures window is laggy on large
	// texture directories" - every tile that came into view decoded its
	// whole file before the frame could go on. Asked for, decoded on the
	// pool a few at a time, and the tile picks it up when it is there.
	if (!ThumbnailJobs.Contains(File) && !ThumbnailWanted.Contains(File))
	{
		ThumbnailWanted.Add(File);
	}
	return nullptr;
}

void SSurfTextureFilesView::PumpThumbnails()
{
	// Finished decodes become textures here, on the game thread.
	for (auto It = ThumbnailJobs.CreateIterator(); It; ++It)
	{
		const TSharedPtr<FSurfThumbJob>& Job = It.Value();
		if (!Job.IsValid() || !Job->bDone)
		{
			continue;
		}
		TSharedPtr<FSlateBrush> Brush;
		if (Job->bOk)
		{
			if (UTexture2D* Texture = SurfTexturesLib::TextureOf(Job->Small))
			{
				Thumbnails.Add(It.Key(), TStrongObjectPtr<UTexture2D>(Texture));
				Brush = MakeShared<FSlateBrush>();
				Brush->SetResourceObject(Texture);
				Brush->ImageSize = FVector2D(TexturesThumbMax, TexturesThumbMax);
				Brush->DrawAs = ESlateBrushDrawType::Image;
			}
		}
		ThumbnailBrushes.Add(It.Key(), Brush);
		It.RemoveCurrent();
	}
	// A few in flight at once; the rest wait their turn, newest asks first
	// (the tiles in view now).
	while (ThumbnailJobs.Num() < 4 && ThumbnailWanted.Num() > 0)
	{
		const FString File = ThumbnailWanted.Pop();
		if (ThumbnailBrushes.Contains(File) || ThumbnailJobs.Contains(File))
		{
			continue;
		}
		TSharedPtr<FSurfThumbJob> Job = MakeShared<FSurfThumbJob>();
		ThumbnailJobs.Add(File, Job);
		Async(EAsyncExecution::ThreadPool, [Job, File]()
		{
			Job->bOk = SurfTexturesLib::DecodeSmall(File, TexturesThumbMax, Job->Small);
			Job->bDone = true;
		});
	}
}

const FSlateBrush* SSurfTextureFilesView::ShownThumbnail(const FString& File)
{
	const FSlateBrush* Picture = ThumbnailFor(File);
	return Picture ? Picture : FAppStyle::GetBrush("ClassIcon.Texture2D");
}

TSharedRef<SWidget> SSurfTextureFilesView::MarkFor(const FString& File, bool bFolder) const
{
	const FString As = bFolder ? FString() : SurfTexturesLib::InLibrary(Index, File);
	if (As.IsEmpty())
	{
		return SNullWidget::NullWidget;
	}
	const bool bLinked = SurfTexturesLib::IsLinked(Index, File);
	return SNew(SBox).WidthOverride(10.0f).HeightOverride(10.0f)
		.ToolTipText(FText::FromString(FString::Printf(TEXT("%s as %s"), bLinked ? TEXT("in the library, read from here") : TEXT("in the library"), *As)))
		[
			SNew(SImage).Image(FAppStyle::GetBrush("Icons.FilledCircle")).ColorAndOpacity(FSlateColor(TexturesMarkColour))
		];
}

TSharedRef<ITableRow> SSurfTextureFilesView::MakeDiskRow(FDiskPtr Entry, const TSharedRef<STableViewBase>& Owner)
{
	const bool bFolder = Entry.IsValid() && Entry->bFolder;
	const FString Name = Entry.IsValid() ? Entry->Name : FString();
	const FString Shown = Entry.IsValid() ? (Entry->Shown.IsEmpty() ? Entry->Name : Entry->Shown) : FString();
	const FString Path = Entry.IsValid() ? Entry->Path : FString();
	const int32 Size = SurfTexturesLib::ZoomSize(0);
	TWeakPtr<SSurfTextureFilesView> Weak = SharedThis(this);
	TAttribute<const FSlateBrush*> Picture = bFolder
		? TAttribute<const FSlateBrush*>(FAppStyle::GetBrush("ContentBrowser.AssetTreeFolderClosed"))
		: TAttribute<const FSlateBrush*>::CreateLambda([Weak, Path]() -> const FSlateBrush*
		{
			TSharedPtr<SSurfTextureFilesView> Live = Weak.Pin();
			return Live.IsValid() ? Live->ShownThumbnail(Path) : FAppStyle::GetBrush("ClassIcon.Texture2D");
		});

	FString Under;
	FSlateColor UnderColour = FSlateColor::UseSubduedForeground();
	if (!bFolder && Entry.IsValid())
	{
		Under = FString::Printf(TEXT("%s, %s"), SurfTextures::KindName(SurfTextures::KindFromName(Name)), *SurfTexturesLib::BytesText(Entry->Bytes));
		const FString As = SurfTexturesLib::InLibrary(Index, Path);
		if (!As.IsEmpty())
		{
			Under += FString::Printf(TEXT(" - %s as %s"), SurfTexturesLib::IsLinked(Index, Path) ? TEXT("read from here") : TEXT("in the library"), *As);
			UnderColour = FSlateColor(TexturesMarkColour);
		}
	}

	return SNew(STableRow<FDiskPtr>, Owner)
		.ToolTipText(FText::FromString(bFolder ? Path : FString::Printf(TEXT("%s\nwould become %s"), *Path, *SurfTextures::AssetNameFor(SurfTextures::StemFrom(Name)))))
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(Size).HeightOverride(Size)
				[
					SNew(SImage).Image(Picture)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				MarkFor(Path, bFolder)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Shown))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Under)).ColorAndOpacity(UnderColour)
			]
		];
}

TSharedRef<ITableRow> SSurfTextureFilesView::MakeDiskTile(FDiskPtr Entry, const TSharedRef<STableViewBase>& Owner)
{
	const bool bFolder = Entry.IsValid() && Entry->bFolder;
	const FString Name = Entry.IsValid() ? Entry->Name : FString();
	const FString Shown = Entry.IsValid() ? (Entry->Shown.IsEmpty() ? Entry->Name : Entry->Shown) : FString();
	const FString Path = Entry.IsValid() ? Entry->Path : FString();
	const int32 Size = SurfTexturesLib::ZoomSize(ZoomShown);
	TWeakPtr<SSurfTextureFilesView> Weak = SharedThis(this);
	TAttribute<const FSlateBrush*> Picture = bFolder
		? TAttribute<const FSlateBrush*>(FAppStyle::GetBrush("ContentBrowser.AssetTreeFolderClosed"))
		: TAttribute<const FSlateBrush*>::CreateLambda([Weak, Path]() -> const FSlateBrush*
		{
			TSharedPtr<SSurfTextureFilesView> Live = Weak.Pin();
			return Live.IsValid() ? Live->ShownThumbnail(Path) : FAppStyle::GetBrush("ClassIcon.Texture2D");
		});

	FString Tip = Path;
	if (!bFolder && Entry.IsValid())
	{
		Tip = FString::Printf(TEXT("%s\n%s, %s\nwould become %s"), *Path,
			SurfTextures::KindName(SurfTextures::KindFromName(Name)), *SurfTexturesLib::BytesText(Entry->Bytes),
			*SurfTextures::AssetNameFor(SurfTextures::StemFrom(Name)));
		const FString As = SurfTexturesLib::InLibrary(Index, Path);
		if (!As.IsEmpty())
		{
			Tip += FString::Printf(TEXT("\n%s as %s"), SurfTexturesLib::IsLinked(Index, Path) ? TEXT("read from here") : TEXT("in the library"), *As);
		}
	}

	TSharedRef<SWidget> PictureWidget = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Picture)
		]
		+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(3.0f)
		[
			MarkFor(Path, bFolder)
		];

	return SNew(STableRow<FDiskPtr>, Owner)
		.ToolTipText(FText::FromString(Tip))
		.Padding(FMargin(4.0f))
		[
			TileBody(PictureWidget, Shown, Size)
		];
}

TSharedPtr<SWidget> SSurfTextureFilesView::OpenDiskMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("DiskImport", "Import"),
		LOCTEXT("DiskImportTip", "Bring the picked files in, with the mode chosen above."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::ImportPicked)));
	Menu.AddMenuEntry(LOCTEXT("DiskShow", "Show in Explorer"),
		LOCTEXT("DiskShowTip", "Explorer, with the picked file selected."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureFilesView::ShowPickedInExplorer)));
	return Menu.MakeWidget();
}

void SSurfTextureFilesView::OnDiskDoubleClick(FDiskPtr Entry)
{
	if (!Entry.IsValid())
	{
		return;
	}
	if (Entry->bFolder)
	{
		GoTo(Entry->Path);
		return;
	}
	TArray<FString> One;
	One.Add(Entry->Path);
	ImportFiles(One);
}

void SSurfTextureFilesView::OnDiskSelectionChanged(FDiskPtr Entry, ESelectInfo::Type How)
{
	FSurfTexturesShared::Get().PickedFiles = PickedFiles();
}

void SSurfTextureFilesView::ShowPickedInExplorer()
{
	TSharedPtr<SListView<FDiskPtr>> View = ActiveDiskView();
	const TArray<FDiskPtr> Picked = View.IsValid() ? View->GetSelectedItems() : TArray<FDiskPtr>();
	if (Picked.Num() > 0 && Picked[0].IsValid())
	{
		FPlatformProcess::ExploreFolder(*Picked[0]->Path);
	}
	else
	{
		OpenFolderInExplorer();
	}
}

TSharedPtr<SListView<SSurfTextureFilesView::FDiskPtr>> SSurfTextureFilesView::ActiveDiskView() const
{
	return SurfTexturesLib::ZoomIsTiles(ZoomShown) ? TSharedPtr<SListView<FDiskPtr>>(DiskTiles) : DiskList;
}

TArray<FString> SSurfTextureFilesView::PickedFiles() const
{
	TArray<FString> Out;
	TSharedPtr<SListView<FDiskPtr>> View = ActiveDiskView();
	if (View.IsValid())
	{
		for (const FDiskPtr& Entry : View->GetSelectedItems())
		{
			if (Entry.IsValid() && !Entry->bFolder)
			{
				Out.Add(Entry->Path);
			}
		}
	}
	return Out;
}

// --- Bringing in ---------------------------------------------------------------------------

void SSurfTextureFilesView::ImportPicked()
{
	const TArray<FString> Files = PickedFiles();
	if (Files.Num() == 0)
	{
		Say(TEXT("import: pick a file or two in the folder first"));
		return;
	}
	ImportFiles(Files);
}

void SSurfTextureFilesView::ImportFiles(const TArray<FString>& Files)
{
	const SurfTextures::FSurfTextureImportReport Report = ImportAsShared(Files);
	Say(Report.Line());
	bIndexDirty = true;
}

// --- The library, for the marks --------------------------------------------------------------

void SSurfTextureFilesView::RefreshIndex()
{
	IndexRoot = GetDefault<USurfSettings>()->MaterialLibraryRoot;
	LastIndexed = FPlatformTime::Seconds();
	bIndexDirty = false;
	SurfTexturesLib::IndexLibrary(IndexRoot, Index);
	ApplyFilter();
	// The marks were made with the old index.
	if (DiskList.IsValid())
	{
		DiskList->RebuildList();
	}
	if (DiskTiles.IsValid())
	{
		DiskTiles->RebuildList();
	}
}

void SSurfTextureFilesView::OnAssetChanged(const FAssetData& Asset)
{
	bIndexDirty = true;
}

void SSurfTextureFilesView::OnAssetRenamed(const FAssetData& Asset, const FString& OldPath)
{
	bIndexDirty = true;
}

void SSurfTextureFilesView::MarkDirty()
{
	bIndexDirty = true;
}

// --- The size of the pictures ------------------------------------------------------------------

void SSurfTextureFilesView::ApplyZoom()
{
	// What is picked carries over from rows to tiles and back.
	TSharedPtr<SListView<FDiskPtr>> Before = ActiveDiskView();
	const TArray<FDiskPtr> Picked = Before.IsValid() ? Before->GetSelectedItems() : TArray<FDiskPtr>();

	ZoomShown = FSurfTexturesShared::Get().Zoom;
	if (DiskList.IsValid()) { DiskList->RebuildList(); }
	if (DiskTiles.IsValid()) { DiskTiles->RebuildList(); }

	TSharedPtr<SListView<FDiskPtr>> After = ActiveDiskView();
	if (After.IsValid() && After != Before)
	{
		After->ClearSelection();
		for (const FDiskPtr& Entry : Picked)
		{
			After->SetItemSelection(Entry, true, ESelectInfo::Direct);
		}
	}
}

void SSurfTextureFilesView::OnZoomWheel(float WheelDelta)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	const int32 New = SurfTexturesLib::StepZoom(Shared.Zoom, WheelDelta);
	if (New != Shared.Zoom)
	{
		Shared.Zoom = New;
		Shared.Save();
	}
}

int32 SSurfTextureFilesView::ZoomIndex() const
{
	return SurfTexturesLib::ZoomIsTiles(ZoomShown) ? 1 : 0;
}

float SSurfTextureFilesView::TileWidth() const
{
	return static_cast<float>(SurfTexturesLib::ZoomSize(ZoomShown) + 16);
}

float SSurfTextureFilesView::TileHeight() const
{
	return static_cast<float>(SurfTexturesLib::ZoomSize(ZoomShown) + 34);
}

// --- Texts ---------------------------------------------------------------------------------------

void SSurfTextureFilesView::Say(const FString& Line)
{
	Notice = Line;
	NoticeAt = FPlatformTime::Seconds();
}

FText SSurfTextureFilesView::FootText() const
{
	if (!Notice.IsEmpty() && FPlatformTime::Seconds() - NoticeAt < TexturesNoticeSeconds)
	{
		return FText::FromString(Notice);
	}
	return FText::GetEmpty();
}

FText SSurfTextureFilesView::ModeText() const
{
	const FString Root = SurfTextures::SourceRoot();
	return FText::FromString(FSurfTexturesShared::Get().bConform
		? FString::Printf(TEXT("as T_Sp_<name>.tga under %s"), *SurfTextures::SourceDir(Root))
		: FString(TEXT("reading the file where it is")));
}

FText SSurfTextureFilesView::EmptyDiskText() const
{
	if (bCapped)
	{
		return FText::FromString(TEXT("more than 5000 images under here: the first 5000 are shown"));
	}
	if (!bFolderReadable)
	{
		return FText::FromString(TEXT("that folder cannot be read: pick another in the tree, type one, or press Samples"));
	}
	if (AllEntries.Num() > 0 && DiskEntries.Num() == 0)
	{
		return FText::FromString(TEXT("every image here is in the library already: untick Hide those in the library to see them"));
	}
	return FText::FromString(TEXT("no images here: pick a folder in the tree, go up, or type one"));
}

EVisibility SSurfTextureFilesView::EmptyDiskVisibility() const
{
	return DiskEntries.Num() == 0 || bCapped ? EVisibility::Visible : EVisibility::Collapsed;
}

ECheckBoxState SSurfTextureFilesView::ConformState() const
{
	return FSurfTexturesShared::Get().bConform ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

ECheckBoxState SSurfTextureFilesView::LinkState() const
{
	return FSurfTexturesShared::Get().bConform ? ECheckBoxState::Unchecked : ECheckBoxState::Checked;
}

ECheckBoxState SSurfTextureFilesView::HideImportedState() const
{
	return FSurfTexturesShared::Get().bHideImported ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSurfTextureFilesView::OnHideImportedChanged(ECheckBoxState State)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	Shared.bHideImported = State == ECheckBoxState::Checked;
	Shared.Save();
	ApplyFilter();
}

// =====================================================================================
// The Texture Library tab
// =====================================================================================

namespace
{
	const FLinearColor TexturesBrokenColour(0.92f, 0.38f, 0.32f);
	const FLinearColor TexturesStaleColour(0.95f, 0.72f, 0.22f);
	constexpr double TexturesStaleCheckSeconds = 5.0;
}

FReply SSurfTextureLibraryView::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
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

void SSurfTextureLibraryView::Construct(const FArguments& InArgs)
{
	SetCanTick(true);
	ZoomShown = FSurfTexturesShared::Get().Zoom;

	TSharedRef<FRowListView> ProjectListRef = SNew(FRowListView)
		.ListItemsSource(&ProjectEntries)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SSurfTextureLibraryView::MakeAssetRow)
		.OnMouseButtonDoubleClick(this, &SSurfTextureLibraryView::OnAssetDoubleClick)
		.OnContextMenuOpening(this, &SSurfTextureLibraryView::OpenAssetMenu);
	ProjectListRef->OnZoomWheel = [this](float Delta) { OnZoomWheel(Delta); };
	ProjectList = ProjectListRef;

	TSharedRef<FRowTileView> ProjectTilesRef = SNew(FRowTileView)
		.ListItemsSource(&ProjectEntries)
		.SelectionMode(ESelectionMode::Single)
		.ItemWidth(this, &SSurfTextureLibraryView::TileWidth)
		.ItemHeight(this, &SSurfTextureLibraryView::TileHeight)
		.ItemAlignment(EListItemAlignment::LeftAligned)
		.OnGenerateTile(this, &SSurfTextureLibraryView::MakeAssetTile)
		.OnMouseButtonDoubleClick(this, &SSurfTextureLibraryView::OnAssetDoubleClick)
		.OnContextMenuOpening(this, &SSurfTextureLibraryView::OpenAssetMenu);
	ProjectTilesRef->OnZoomWheel = [this](float Delta) { OnZoomWheel(Delta); };
	ProjectTiles = ProjectTilesRef;

	ChildSlot
	[
		SNew(SVerticalBox)

		// The library and its verbs.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(this, &SSurfTextureLibraryView::CountsText).ColorAndOpacity(FSlateColor::UseSubduedForeground()).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SurfWindows::MenuBarPullDown(LOCTEXT("LibraryMore", "More"),
					LOCTEXT("LibraryMoreTip", "Conform the picked texture, or all. Click a parameter to put the picked texture on the instance; double-click a texture to open it; right-click it for Conform, Read again, Read another file; Ctrl+wheel for bigger pictures."),
					[this]() { return MakeMoreMenu(); })
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 2.0f)
		[
			RowsOrTiles(ProjectListRef, ProjectTilesRef, TAttribute<int32>(this, &SSurfTextureLibraryView::ZoomIndex))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SSurfTextureLibraryView::EmptyProjectText)
			.Visibility(this, &SSurfTextureLibraryView::EmptyProjectVisibility)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]

		// Onto the material.
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SSurfTextureLibraryView::TargetText).AutoWrapText(true)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f)
		[
			SAssignNew(ParameterBox, SWrapBox).UseAllottedSize(true)
		]

		// The foot.
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 6.0f)
		[
			SNew(STextBlock).Text(this, &SSurfTextureLibraryView::FootText).AutoWrapText(true)
		]
	];

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.OnAssetAdded().AddSP(this, &SSurfTextureLibraryView::OnAssetChanged);
	Registry.OnAssetRemoved().AddSP(this, &SSurfTextureLibraryView::OnAssetChanged);
	Registry.OnAssetUpdated().AddSP(this, &SSurfTextureLibraryView::OnAssetChanged);
	Registry.OnAssetRenamed().AddSP(this, &SSurfTextureLibraryView::OnAssetRenamed);
	Registry.OnFilesLoaded().AddSP(this, &SSurfTextureLibraryView::MarkDirty);
}

SSurfTextureLibraryView::~SSurfTextureLibraryView()
{
	PictureBrushes.Empty();
	Pictures.Empty();
}

FReply SSurfTextureLibraryView::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	return DroppedImages(DragDropEvent).Num() > 0 ? FReply::Handled() : FReply::Unhandled();
}

FReply SSurfTextureLibraryView::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const TArray<FString> Files = DroppedImages(DragDropEvent);
	if (Files.Num() == 0)
	{
		return FReply::Unhandled();
	}
	// Picked in Texture Files when it is open; brought straight in when it is not.
	if (SSurfTextureFilesView* Disk = SurfTexturesTab::FilesView())
	{
		Disk->PickDropped(Files);
		Say(FString::Printf(TEXT("%d dropped file%s picked in Files"), Files.Num(), Files.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	else
	{
		Say(ImportAsShared(Files).Line());
		bDirty = true;
	}
	return FReply::Handled();
}

void SSurfTextureLibraryView::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Now = FPlatformTime::Seconds();
	const bool bRootChanged = Settings->MaterialLibraryRoot != RootRead || SurfTextures::SourceRoot() != SourceRootRead;
	if (bRootChanged || (bDirty && Now - LastRead > TexturesReadEverySeconds))
	{
		RefreshProject();
	}
	else if (Now - LastStaleCheck > TexturesStaleCheckSeconds)
	{
		// A file can change on disk with no word to the registry: look now and then.
		LastStaleCheck = Now;
		if (StandingsMoved())
		{
			RefreshProject();
		}
	}
	if (FSurfTexturesShared::Get().Zoom != ZoomShown)
	{
		ApplyZoom();
	}

	// The material to dress follows the Materials window's selection.
	UMaterialInterface* Current = SurfMaterialsLib::CurrentTarget();
	if (Current != Target.Get())
	{
		Target = Current;
		RebuildParameterButtons();
	}
}

// --- The library -----------------------------------------------------------------------------

bool SSurfTextureLibraryView::StandingsMoved() const
{
	for (const FRowPtr& Row : ProjectEntries)
	{
		if (Row.IsValid() && !Row->IsOrphan())
		{
			if (SurfTextures::StatusOf(Row->Asset, SourceRootRead) != Row->Status || SurfTextures::IsStale(Row->Asset) != Row->bStale)
			{
				return true;
			}
		}
		else if (Row.IsValid() && !FPaths::FileExists(Row->Orphan))
		{
			return true;
		}
	}
	return false;
}

void SSurfTextureLibraryView::RefreshProject()
{
	RootRead = GetDefault<USurfSettings>()->MaterialLibraryRoot;
	SourceRootRead = SurfTextures::SourceRoot();
	LastRead = FPlatformTime::Seconds();
	LastStaleCheck = LastRead;
	bDirty = false;

	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	const FRowPtr Keep = PickedRow();
	const FString KeepKey = Keep.IsValid() ? Keep->Key() : FString();

	TArray<FAssetData> Assets;
	SurfTextures::ListProject(RootRead, Assets);
	TArray<FString> Orphans;
	SurfTextures::ListOrphans(RootRead, SourceRootRead, Orphans);

	ProjectEntries.Reset();
	Counts = FSurfLibraryCounts();
	FRowPtr Again;
	FRowPtr JustIn;
	for (FAssetData& Asset : Assets)
	{
		FRowPtr Row = MakeShared<FSurfLibraryRow>();
		Row->Asset = MoveTemp(Asset);
		Row->Status = SurfTextures::StatusOf(Row->Asset, SourceRootRead);
		Row->bStale = Row->Status == SurfTextures::ESurfTextureStatus::Conform && SurfTextures::IsStale(Row->Asset);
		++Counts.Textures;
		switch (Row->Status)
		{
		case SurfTextures::ESurfTextureStatus::Conform: ++Counts.Conform; break;
		case SurfTextures::ESurfTextureStatus::Portable: ++Counts.Portable; break;
		case SurfTextures::ESurfTextureStatus::LocalOnly: ++Counts.LocalOnly; break;
		case SurfTextures::ESurfTextureStatus::Broken: ++Counts.Broken; break;
		case SurfTextures::ESurfTextureStatus::External: ++Counts.External; break;
		default: break;
		}
		Counts.Stale += Row->bStale ? 1 : 0;

		const FString Path = Row->Asset.GetObjectPathString();
		if (!KeepKey.IsEmpty() && Row->Key() == KeepKey)
		{
			Again = Row;
		}
		if (!Shared.LastImported.IsEmpty() && Path == Shared.LastImported)
		{
			JustIn = Row;
		}
		ProjectEntries.Add(Row);
	}
	for (const FString& Orphan : Orphans)
	{
		FRowPtr Row = MakeShared<FSurfLibraryRow>();
		Row->Orphan = Orphan;
		++Counts.Orphans;
		if (!KeepKey.IsEmpty() && Row->Key() == KeepKey)
		{
			Again = Row;
		}
		ProjectEntries.Add(Row);
	}

	// The last one brought in is the one picked, ready to go on a parameter.
	if (JustIn.IsValid())
	{
		Again = JustIn;
		Shared.LastImported.Reset();
	}
	if (ProjectList.IsValid())
	{
		ProjectList->RebuildList();
		if (Again.IsValid())
		{
			ProjectList->SetSelection(Again, ESelectInfo::Direct);
		}
	}
	if (ProjectTiles.IsValid())
	{
		ProjectTiles->RebuildList();
		if (Again.IsValid())
		{
			ProjectTiles->SetSelection(Again, ESelectInfo::Direct);
		}
	}
	if (JustIn.IsValid())
	{
		TSharedPtr<SListView<FRowPtr>> View = ActiveProjectView();
		if (View.IsValid())
		{
			View->RequestScrollIntoView(JustIn);
		}
	}
}

const FSlateBrush* SSurfTextureLibraryView::PictureFor(const FSurfLibraryRow& Row)
{
	if (Row.IsOrphan())
	{
		// A file with no texture: its own picture, as the Files tab would show it.
		if (const TSharedPtr<FSlateBrush>* Known = PictureBrushes.Find(Row.Orphan))
		{
			return Known->Get();
		}
		TSharedPtr<FSlateBrush> Brush;
		if (UTexture2D* Picture = SurfTexturesLib::MakeThumbnail(Row.Orphan, TexturesThumbMax))
		{
			Pictures.Add(Row.Orphan, TStrongObjectPtr<UTexture2D>(Picture));
			Brush = MakeShared<FSlateBrush>();
			Brush->SetResourceObject(Picture);
			Brush->ImageSize = FVector2D(TexturesThumbMax, TexturesThumbMax);
			Brush->DrawAs = ESlateBrushDrawType::Image;
		}
		PictureBrushes.Add(Row.Orphan, Brush);
		return Brush.Get();
	}

	UTexture2D* Texture = Cast<UTexture2D>(Row.Asset.GetAsset());
	if (!Texture)
	{
		return nullptr;
	}
	const FString Key = Row.Asset.GetObjectPathString() + TEXT("#") + (Texture->Source.IsValid() ? Texture->Source.GetId().ToString() : FString());
	if (const TSharedPtr<FSlateBrush>* Known = PictureBrushes.Find(Key))
	{
		return Known->Get();
	}
	if (PictureBrushes.Num() > 256)
	{
		PictureBrushes.Empty();
		Pictures.Empty();
	}

	// From the source pixels, so the picture is the texture at full quality
	// whatever mip is resident; a texture with no source shows itself as it is.
	TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->ImageSize = FVector2D(TexturesThumbMax, TexturesThumbMax);
	Brush->DrawAs = ESlateBrushDrawType::Image;
	if (UTexture2D* Picture = SurfTexturesLib::MakeAssetPicture(Texture, TexturesThumbMax))
	{
		Pictures.Add(Key, TStrongObjectPtr<UTexture2D>(Picture));
		Brush->SetResourceObject(Picture);
	}
	else
	{
		Brush->SetResourceObject(Texture);
	}
	PictureBrushes.Add(Key, Brush);
	return Brush.Get();
}

FString SSurfTextureLibraryView::StandingText(const FSurfLibraryRow& Row, FSlateColor& OutColour) const
{
	OutColour = FSlateColor::UseSubduedForeground();
	if (Row.IsOrphan())
	{
		OutColour = FSlateColor(TexturesStaleColour);
		return TEXT("a TGA no texture reads");
	}
	FString Out = FString::Printf(TEXT("%s, %s"), SurfTextures::KindName(SurfTextures::KindFromName(Row.Asset.AssetName.ToString())), SurfTextures::StatusName(Row.Status));
	if (Row.Status == SurfTextures::ESurfTextureStatus::Broken)
	{
		OutColour = FSlateColor(TexturesBrokenColour);
		Out += TEXT(" - its file is gone");
	}
	else if (Row.bStale)
	{
		OutColour = FSlateColor(TexturesStaleColour);
		Out += TEXT(" - changed on disk");
	}
	return Out;
}

FString SSurfTextureLibraryView::NameOf(const FSurfLibraryRow& Row)
{
	return Row.IsOrphan() ? FPaths::GetCleanFilename(Row.Orphan) : Row.Asset.AssetName.ToString();
}

FString SSurfTextureLibraryView::TipOf(const FSurfLibraryRow& Row)
{
	if (Row.IsOrphan())
	{
		return Row.Orphan + TEXT("\nunder the source root, read by no texture: Import brings it in, or take it away");
	}
	const FString Source = SurfTextures::SourceFileOf(Row.Asset);
	return Source.IsEmpty() ? FString(TEXT("no file behind it")) : Source;
}

TSharedRef<ITableRow> SSurfTextureLibraryView::MakeAssetRow(FRowPtr Row, const TSharedRef<STableViewBase>& Owner)
{
	const FString Name = Row.IsValid() ? NameOf(*Row) : FString();
	FSlateColor UnderColour = FSlateColor::UseSubduedForeground();
	const FString Under = Row.IsValid() ? StandingText(*Row, UnderColour) : FString();
	const int32 Size = SurfTexturesLib::ZoomSize(0);
	const FSlateBrush* Picture = Row.IsValid() ? PictureFor(*Row) : nullptr;
	if (!Picture)
	{
		Picture = FAppStyle::GetBrush("ClassIcon.Texture2D");
	}

	return SNew(STableRow<FRowPtr>, Owner)
		.ToolTipText(FText::FromString(Row.IsValid() ? TipOf(*Row) : FString()))
		.OnDragDetected_Lambda([Row](const FGeometry&, const FPointerEvent&)
		{
			// Dragged as the Content Browser drags an asset: onto a manager parameter, or anywhere else that takes one.
			return Row.IsValid() && !Row->IsOrphan() && Row->Asset.IsValid()
				? FReply::Handled().BeginDragDrop(FAssetDragDropOp::New(Row->Asset))
				: FReply::Unhandled();
		})
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(Size).HeightOverride(Size)
				[
					SNew(SImage).Image(Picture)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Name))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Under)).ColorAndOpacity(UnderColour)
			]
		];
}

TSharedRef<ITableRow> SSurfTextureLibraryView::MakeAssetTile(FRowPtr Row, const TSharedRef<STableViewBase>& Owner)
{
	const FString Name = Row.IsValid() ? NameOf(*Row) : FString();
	FSlateColor UnderColour = FSlateColor::UseSubduedForeground();
	const FString Under = Row.IsValid() ? StandingText(*Row, UnderColour) : FString();
	const int32 Size = SurfTexturesLib::ZoomSize(ZoomShown);
	const FSlateBrush* Picture = Row.IsValid() ? PictureFor(*Row) : nullptr;
	if (!Picture)
	{
		Picture = FAppStyle::GetBrush("ClassIcon.Texture2D");
	}

	// A standing that is not plain shows as a dot on the picture.
	const bool bMarked = Row.IsValid() && (Row->IsOrphan() || Row->bStale || Row->Status == SurfTextures::ESurfTextureStatus::Broken);
	TSharedRef<SWidget> PictureWidget = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Picture)
		]
		+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(3.0f)
		[
			SNew(SBox).WidthOverride(10.0f).HeightOverride(10.0f).Visibility(bMarked ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Icons.FilledCircle")).ColorAndOpacity(UnderColour)
			]
		];

	return SNew(STableRow<FRowPtr>, Owner)
		.ToolTipText(FText::FromString(Row.IsValid() ? Under + TEXT("\n") + TipOf(*Row) : FString()))
		.OnDragDetected_Lambda([Row](const FGeometry&, const FPointerEvent&)
		{
			return Row.IsValid() && !Row->IsOrphan() && Row->Asset.IsValid()
				? FReply::Handled().BeginDragDrop(FAssetDragDropOp::New(Row->Asset))
				: FReply::Unhandled();
		})
		.Padding(FMargin(4.0f))
		[
			TileBody(PictureWidget, Name, Size)
		];
}

TSharedPtr<SWidget> SSurfTextureLibraryView::OpenAssetMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	const FRowPtr Row = PickedRow();
	if (Row.IsValid() && Row->IsOrphan())
	{
		Menu.AddMenuEntry(LOCTEXT("OrphanImport", "Import"),
			LOCTEXT("OrphanImportTip", "Bring this TGA into the library as a texture, in the mode chosen in Files."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ImportPickedOrphan)));
		Menu.AddMenuEntry(LOCTEXT("OrphanShow", "Show in Explorer"),
			LOCTEXT("OrphanShowTip", "Explorer, with this file selected."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ShowPickedFileInExplorer)));
		return Menu.MakeWidget();
	}

	Menu.AddMenuEntry(LOCTEXT("AssetOpen", "Open"),
		LOCTEXT("AssetOpenTip", "Open this texture in its editor."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::OpenPickedAsset)));
	Menu.AddMenuEntry(LOCTEXT("AssetBrowse", "Show in Content Browser"),
		LOCTEXT("AssetBrowseTip", "The Content Browser, with this texture selected."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ShowPickedInContentBrowser)));
	Menu.AddMenuEntry(LOCTEXT("AssetExplore", "Show file in Explorer"),
		LOCTEXT("AssetExploreTip", "Explorer, with the file this texture reads selected."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ShowPickedFileInExplorer)));
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("AssetConform", "Conform"),
		LOCTEXT("AssetConformTip", "Its own pixels written as our TGA under the source root, and read from there; renamed T_Sp_ if it is not."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ConformPicked),
			FCanExecuteAction::CreateSP(this, &SSurfTextureLibraryView::CanConformPicked)));
	Menu.AddMenuEntry(LOCTEXT("AssetReimport", "Read again"),
		LOCTEXT("AssetReimportTip", "Read its file again - for one that changed on disk."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ReimportPicked),
			FCanExecuteAction::CreateSP(this, &SSurfTextureLibraryView::CanReimportPicked)));
	Menu.AddMenuEntry(LOCTEXT("AssetLink", "Read another file..."),
		LOCTEXT("AssetLinkTip", "Pick a file for this texture to read where it is - for one whose file is gone, or to swap its source."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::LinkPicked)));
	return Menu.MakeWidget();
}

void SSurfTextureLibraryView::OnAssetDoubleClick(FRowPtr Row)
{
	if (!Row.IsValid() || !GEditor)
	{
		return;
	}
	if (Row->IsOrphan())
	{
		FPlatformProcess::ExploreFolder(*Row->Orphan);
		return;
	}
	if (UObject* Loaded = Row->Asset.GetAsset())
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Loaded);
	}
}

SSurfTextureLibraryView::FRowPtr SSurfTextureLibraryView::PickedRow() const
{
	TSharedPtr<SListView<FRowPtr>> View = ActiveProjectView();
	if (View.IsValid())
	{
		const TArray<FRowPtr> Picked = View->GetSelectedItems();
		if (Picked.Num() > 0)
		{
			return Picked[0];
		}
	}
	return nullptr;
}

UTexture2D* SSurfTextureLibraryView::PickedTexture() const
{
	const FRowPtr Row = PickedRow();
	return Row.IsValid() && !Row->IsOrphan() ? Cast<UTexture2D>(Row->Asset.GetAsset()) : nullptr;
}

void SSurfTextureLibraryView::OpenPickedAsset()
{
	const FRowPtr Row = PickedRow();
	if (!Row.IsValid() || Row->IsOrphan())
	{
		Say(TEXT("open: pick a texture in the library first"));
		return;
	}
	OnAssetDoubleClick(Row);
}

void SSurfTextureLibraryView::ShowPickedInContentBrowser()
{
	const FRowPtr Row = PickedRow();
	if (!Row.IsValid() || Row->IsOrphan() || !GEditor)
	{
		Say(TEXT("show: pick a texture in the library first"));
		return;
	}
	TArray<FAssetData> One;
	One.Add(Row->Asset);
	GEditor->SyncBrowserToObjects(One);
}

void SSurfTextureLibraryView::ShowPickedFileInExplorer()
{
	const FRowPtr Row = PickedRow();
	if (!Row.IsValid())
	{
		Say(TEXT("show: pick a texture in the library first"));
		return;
	}
	const FString Source = Row->IsOrphan() ? Row->Orphan : SurfTextures::SourceFileOf(Row->Asset);
	if (Source.IsEmpty())
	{
		Say(FString::Printf(TEXT("show: %s reads no file"), *NameOf(*Row)));
		return;
	}
	if (!IFileManager::Get().FileExists(*Source))
	{
		Say(FString::Printf(TEXT("show: %s is gone (%s)"), *Source, SurfTextures::StatusName(SurfTextures::ESurfTextureStatus::Broken)));
		return;
	}
	FPlatformProcess::ExploreFolder(*Source);
}

TSharedPtr<SListView<SSurfTextureLibraryView::FRowPtr>> SSurfTextureLibraryView::ActiveProjectView() const
{
	return SurfTexturesLib::ZoomIsTiles(ZoomShown) ? TSharedPtr<SListView<FRowPtr>>(ProjectTiles) : ProjectList;
}

void SSurfTextureLibraryView::OnAssetChanged(const FAssetData& Asset)
{
	bDirty = true;
}

void SSurfTextureLibraryView::OnAssetRenamed(const FAssetData& Asset, const FString& OldPath)
{
	bDirty = true;
}

void SSurfTextureLibraryView::MarkDirty()
{
	bDirty = true;
}

// --- Keeping it right ------------------------------------------------------------------------------

SurfTextures::FSurfTextureImportOptions SSurfTextureLibraryView::OptionsNow() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	SurfTextures::FSurfTextureImportOptions Options;
	Options.LibraryRoot = Settings->MaterialLibraryRoot;
	Options.bConform = true;
	Options.MaxSize = Settings->TextureImportMaxSize;
	Options.bSave = true;
	return Options;
}

bool SSurfTextureLibraryView::CanConformPicked() const
{
	const FRowPtr Row = PickedRow();
	return Row.IsValid() && !Row->IsOrphan() && Row->Status != SurfTextures::ESurfTextureStatus::Conform;
}

bool SSurfTextureLibraryView::CanConformAll() const
{
	return Counts.Textures - Counts.Conform > 0;
}

bool SSurfTextureLibraryView::CanReimportPicked() const
{
	const FRowPtr Row = PickedRow();
	return Row.IsValid() && !Row->IsOrphan() && Row->Status != SurfTextures::ESurfTextureStatus::Broken && Row->Status != SurfTextures::ESurfTextureStatus::External;
}

void SSurfTextureLibraryView::ConformPicked()
{
	UTexture2D* Texture = PickedTexture();
	if (!Texture)
	{
		Say(TEXT("conform: pick a texture in the library first"));
		return;
	}
	TArray<UTexture2D*> One;
	One.Add(Texture);
	const SurfTextures::FSurfTextureImportReport Report = SurfTextures::Conform(One, OptionsNow());
	Say(Report.Line());
	bDirty = true;
}

void SSurfTextureLibraryView::ConformAll()
{
	TArray<UTexture2D*> Textures;
	for (const FRowPtr& Row : ProjectEntries)
	{
		if (Row.IsValid() && !Row->IsOrphan() && Row->Status != SurfTextures::ESurfTextureStatus::Conform)
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Row->Asset.GetAsset()))
			{
				Textures.Add(Texture);
			}
		}
	}
	if (Textures.Num() == 0)
	{
		Say(TEXT("conform: every texture is conform already"));
		return;
	}
	const SurfTextures::FSurfTextureImportReport Report = SurfTextures::Conform(Textures, OptionsNow());
	Say(Report.Line());
	bDirty = true;
}

void SSurfTextureLibraryView::ReimportPicked()
{
	UTexture2D* Texture = PickedTexture();
	if (!Texture)
	{
		Say(TEXT("read again: pick a texture in the library first"));
		return;
	}
	FString Error;
	if (SurfTextures::Reimport(Texture, /*bSave*/ true, Error))
	{
		Say(FString::Printf(TEXT("read %s again"), *Texture->GetName()));
	}
	else
	{
		Say(Error);
	}
	bDirty = true;
}

void SSurfTextureLibraryView::LinkPicked()
{
	UTexture2D* Texture = PickedTexture();
	if (!Texture)
	{
		Say(TEXT("read another file: pick a texture in the library first"));
		return;
	}
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		Say(TEXT("read another file: the system's dialog is not there"));
		return;
	}
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	TArray<FString> Files;
	const FString Was = SurfTextures::SourceFileOf(FAssetData(Texture));
	const bool bPicked = Desktop->OpenFileDialog(Parent, FString::Printf(TEXT("A file for %s to read"), *Texture->GetName()),
		Was.IsEmpty() ? FSurfTexturesShared::Get().Folder : FPaths::GetPath(Was), TEXT(""),
		TEXT("Images (*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.exr;*.tif;*.tiff;*.psd;*.hdr)|*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.exr;*.tif;*.tiff;*.psd;*.hdr|All files (*.*)|*.*"),
		EFileDialogFlags::None, Files);
	if (!bPicked || Files.Num() == 0)
	{
		return;
	}
	SurfTextures::FSurfTextureImportOptions Options = OptionsNow();
	Options.bConform = false;
	const SurfTextures::FSurfTextureImportReport Report = SurfTextures::Link(Texture, Files[0], Options);
	Say(Report.Line());
	bDirty = true;
}

void SSurfTextureLibraryView::ImportPickedOrphan()
{
	const FRowPtr Row = PickedRow();
	if (!Row.IsValid() || !Row->IsOrphan())
	{
		Say(TEXT("import: pick a stray file first"));
		return;
	}
	TArray<FString> One;
	One.Add(Row->Orphan);
	const SurfTextures::FSurfTextureImportReport Report = ImportAsShared(One);
	Say(Report.Line());
	bDirty = true;
}

// --- The size of the pictures --------------------------------------------------------------------

void SSurfTextureLibraryView::ApplyZoom()
{
	TSharedPtr<SListView<FRowPtr>> Before = ActiveProjectView();
	const FRowPtr Picked = PickedRow();

	ZoomShown = FSurfTexturesShared::Get().Zoom;
	if (ProjectList.IsValid()) { ProjectList->RebuildList(); }
	if (ProjectTiles.IsValid()) { ProjectTiles->RebuildList(); }

	TSharedPtr<SListView<FRowPtr>> After = ActiveProjectView();
	if (After.IsValid() && After != Before)
	{
		After->ClearSelection();
		if (Picked.IsValid())
		{
			After->SetSelection(Picked, ESelectInfo::Direct);
		}
	}
}

void SSurfTextureLibraryView::OnZoomWheel(float WheelDelta)
{
	FSurfTexturesShared& Shared = FSurfTexturesShared::Get();
	const int32 New = SurfTexturesLib::StepZoom(Shared.Zoom, WheelDelta);
	if (New != Shared.Zoom)
	{
		Shared.Zoom = New;
		Shared.Save();
	}
}

int32 SSurfTextureLibraryView::ZoomIndex() const
{
	return SurfTexturesLib::ZoomIsTiles(ZoomShown) ? 1 : 0;
}

float SSurfTextureLibraryView::TileWidth() const
{
	return static_cast<float>(SurfTexturesLib::ZoomSize(ZoomShown) + 16);
}

float SSurfTextureLibraryView::TileHeight() const
{
	return static_cast<float>(SurfTexturesLib::ZoomSize(ZoomShown) + 34);
}

// --- Onto the material ----------------------------------------------------------------------------

void SSurfTextureLibraryView::RebuildParameterButtons()
{
	TargetParameters.Reset();
	if (ParameterBox.IsValid())
	{
		ParameterBox->ClearChildren();
	}
	UMaterialInterface* Material = Target.Get();
	if (!Material || !ParameterBox.IsValid())
	{
		return;
	}
	SurfTextures::TextureParametersOf(Material, TargetParameters);
	for (const FName& Parameter : TargetParameters)
	{
		ParameterBox->AddSlot().Padding(2.0f)
		[
			TexturesButton(FText::FromName(Parameter),
				FText::FromString(FString::Printf(TEXT("Put the picked texture on %s of %s. A file picked in Files comes in first, in the mode chosen there."),
					*Parameter.ToString(), *Material->GetName())),
				[this, Parameter]() { PutOnParameter(Parameter); })
		];
	}
	// The set: the siblings of the one just put on, for the other parameters.
	ParameterBox->AddSlot().Padding(2.0f)
	[
		SNew(SButton)
		.ButtonStyle(&SurfTheme::ToolButton())
		.Text(this, &SSurfTextureLibraryView::SetButtonText)
		.ToolTipText(LOCTEXT("PutSetTip", "The siblings of the texture just put on - the same name, another suffix - onto the parameters they fit, the way the library's other instances pair them, else by the parameter's name."))
		.Visibility(this, &SSurfTextureLibraryView::SetButtonVisibility)
		.OnClicked_Lambda([this]() { PutTheSet(); return FReply::Handled(); })
	];
}

void SSurfTextureLibraryView::PutTheSet()
{
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Target.Get());
	if (!Instance)
	{
		return;
	}
	int32 Put = 0;
	FString Error;
	for (const SurfTextureSuggest::FSurfTextureSuggestion& Suggestion : Suggestions)
	{
		UTexture* Texture = Cast<UTexture>(Suggestion.Texture.GetAsset());
		if (Texture && SurfTextures::SetTextureParameter(Instance, Suggestion.Parameter, Texture, /*bSave*/ true, Error))
		{
			++Put;
		}
	}
	Say(FString::Printf(TEXT("put %d of the set on %s"), Put, *Instance->GetName()));
	Suggestions.Reset();
}

FText SSurfTextureLibraryView::SetButtonText() const
{
	return FText::FromString(FString::Printf(TEXT("Put the set (%d)"), Suggestions.Num()));
}

EVisibility SSurfTextureLibraryView::SetButtonVisibility() const
{
	return Suggestions.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

void SSurfTextureLibraryView::PutOnParameter(FName Parameter)
{
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Target.Get());
	if (!Instance)
	{
		Say(TEXT("texture: select an instance in the Material Library first (a parent's parameters are not set here)"));
		return;
	}

	// The texture: the one picked here, else the file picked in Files, brought in first.
	UTexture* Texture = PickedTexture();
	if (!Texture)
	{
		const TArray<FString>& Files = FSurfTexturesShared::Get().PickedFiles;
		if (Files.Num() > 0)
		{
			TArray<FString> One;
			One.Add(Files[0]);
			const SurfTextures::FSurfTextureImportReport Report = ImportAsShared(One);
			bDirty = true;
			if (Report.Imported > 0 && Report.Entries.Num() > 0)
			{
				Texture = Report.Entries[0].Texture;
			}
			else
			{
				Say(Report.Line());
				return;
			}
		}
	}
	if (!Texture)
	{
		Say(TEXT("texture: pick a texture here, or a file in Files, first"));
		return;
	}

	FString Error;
	if (SurfTextures::SetTextureParameter(Instance, Parameter, Texture, /*bSave*/ true, Error))
	{
		FString Line = Error.IsEmpty() ? FString::Printf(TEXT("put %s on %s of %s"), *Texture->GetName(), *Parameter.ToString(), *Instance->GetName()) : Error;
		// Its siblings, offered for the other parameters.
		SurfTextureSuggest::Suggest(Instance, Parameter, GetDefault<USurfSettings>()->MaterialLibraryRoot, Suggestions);
		if (Suggestions.Num() > 0)
		{
			Line += FString::Printf(TEXT(" - %d of the same set fit the other parameters: Put the set"), Suggestions.Num());
		}
		Say(Line);
	}
	else
	{
		Say(Error);
	}
}

// --- Texts -----------------------------------------------------------------------------------------

void SSurfTextureLibraryView::Say(const FString& Line)
{
	Notice = Line;
	NoticeAt = FPlatformTime::Seconds();
}

FText SSurfTextureLibraryView::FootText() const
{
	if (!Notice.IsEmpty() && FPlatformTime::Seconds() - NoticeAt < TexturesNoticeSeconds)
	{
		return FText::FromString(Notice);
	}
	return FText::GetEmpty();
}

TSharedRef<SWidget> SSurfTextureLibraryView::MakeMoreMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	Menu.AddMenuEntry(LOCTEXT("ConformPicked", "Conform the picked"),
		LOCTEXT("ConformPickedTip", "Make the picked texture conform: its own pixels written as our TGA under the source root, and read from there. One not named T_Sp_ is renamed so (a redirector stays behind)."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ConformPicked), FCanExecuteAction::CreateSP(this, &SSurfTextureLibraryView::CanConformPicked)));
	Menu.AddMenuEntry(LOCTEXT("ConformAll", "Conform all"),
		LOCTEXT("ConformAllTip", "Make every texture that is not conform read our TGA: its own pixels written under the source root; those not named T_Sp_ renamed so (redirectors stay behind)."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfTextureLibraryView::ConformAll), FCanExecuteAction::CreateSP(this, &SSurfTextureLibraryView::CanConformAll)));
	return Menu.MakeWidget();
}

FText SSurfTextureLibraryView::CountsText() const
{
	FString Out = TEXT("in the library: ") + SurfTextures::ContentFolder(RootRead);
	if (Counts.Textures == 0 && Counts.Orphans == 0)
	{
		return FText::FromString(Out);
	}
	Out += FString::Printf(TEXT(" - %d texture%s"), Counts.Textures, Counts.Textures == 1 ? TEXT("") : TEXT("s"));
	TArray<FString> Parts;
	if (Counts.Conform > 0) { Parts.Add(FString::Printf(TEXT("%d conform"), Counts.Conform)); }
	if (Counts.Portable + Counts.LocalOnly > 0) { Parts.Add(FString::Printf(TEXT("%d linked"), Counts.Portable + Counts.LocalOnly)); }
	if (Counts.External > 0) { Parts.Add(FString::Printf(TEXT("%d with no file"), Counts.External)); }
	if (Counts.Broken > 0) { Parts.Add(FString::Printf(TEXT("%d broken"), Counts.Broken)); }
	if (Counts.Stale > 0) { Parts.Add(FString::Printf(TEXT("%d changed on disk"), Counts.Stale)); }
	if (Parts.Num() > 0)
	{
		Out += TEXT(" (") + FString::Join(Parts, TEXT(", ")) + TEXT(")");
	}
	if (Counts.Orphans > 0)
	{
		Out += FString::Printf(TEXT("; %d stray TGA%s under the source root"), Counts.Orphans, Counts.Orphans == 1 ? TEXT("") : TEXT("s"));
	}
	return FText::FromString(Out);
}

FText SSurfTextureLibraryView::TargetText() const
{
	const UMaterialInterface* Material = Target.Get();
	if (!Material)
	{
		return FText::FromString(TEXT("no instance selected: select one in the Material Library to dress it"));
	}
	if (TargetParameters.Num() == 0)
	{
		return FText::FromString(FString::Printf(TEXT("%s has no texture parameters"), *Material->GetName()));
	}
	return FText::FromString(FString::Printf(TEXT("put on %s:"), *Material->GetName()));
}

FText SSurfTextureLibraryView::EmptyProjectText() const
{
	return FText::FromString(TEXT("no textures in the library yet: bring some in from Files"));
}

EVisibility SSurfTextureLibraryView::EmptyProjectVisibility() const
{
	return ProjectEntries.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

// =====================================================================================
// The tabs
// =====================================================================================

namespace SurfTexturesTab
{
	const FName TabName("SurfacingProTextures");
	// The old two, still answered by name so a saved layout finds them.
	const FName FilesTabName("SurfacingProTextureFiles");
	const FName LibraryTabName("SurfacingProTextureLibrary");

	/** How the two halves are split, and which are open; kept for the session. */
	static float GFilesShare = 0.58f;
	static bool GFilesOpen = true;
	static bool GLibraryOpen = true;

	/**
	 * One window, two halves (S169a): "it has split in two tabs texture
	 * library and texture files, i'd rather have a single one, and where user
	 * may be able to choose the size of each - perhaps collapse one over the
	 * other if only one needed at a time".
	 *
	 * The files on the left, the library on the right, a splitter between
	 * them; two ticks on the line above fold either away so the other takes
	 * the whole window.
	 */
	static TSharedRef<SWidget> MakeWindow()
	{
		auto Fold = [](const FText& Label, const FText& Tip, bool* Flag, bool* Other) -> TSharedRef<SWidget>
		{
			return SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(Tip)
				.IsChecked_Lambda([Flag]() { return *Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Flag, Other](ECheckBoxState State)
				{
					*Flag = State == ECheckBoxState::Checked;
					// Never both away: folding the last one opens the other.
					if (!*Flag && !*Other)
					{
						*Other = true;
					}
				})
				.Padding(FMargin(8.0f, 2.0f))
				[
					SNew(STextBlock).Text(Label).Font(SurfTheme::Font(9))
				];
		};

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 3.0f, 4.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					Fold(LOCTEXT("FoldFiles", "Files"), LOCTEXT("FoldFilesTip", "The folders on this machine and their images. Off: the library takes the whole window."), &GFilesOpen, &GLibraryOpen)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
				[
					Fold(LOCTEXT("FoldLibrary", "Library"), LOCTEXT("FoldLibraryTip", "The textures brought in as ours. Off: the files take the whole window."), &GLibraryOpen, &GFilesOpen)
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(3.0f)
				.HitDetectionSplitterHandleSize(8.0f)
				+ SSplitter::Slot()
				.Value_Lambda([]() { return GFilesShare; })
				.OnSlotResized_Lambda([](float V) { GFilesShare = V; })
				[
					SNew(SBox)
					.Visibility_Lambda([]() { return GFilesOpen ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SSurfTextureFilesView)
					]
				]
				+ SSplitter::Slot()
				.Value_Lambda([]() { return 1.0f - GFilesShare; })
				.OnSlotResized_Lambda([](float V) { GFilesShare = 1.0f - V; })
				[
					SNew(SBox)
					.Visibility_Lambda([]() { return GLibraryOpen ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SSurfTextureLibraryView)
					]
				]
			];
	}

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TexturesTabLabel", "Textures"))
			[
				MakeWindow()
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		TSharedRef<FGlobalTabmanager> Manager = FGlobalTabmanager::Get();
		if (!Manager->HasTabSpawner(TabName))
		{
			Manager->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
				.SetDisplayName(LOCTEXT("TexturesTabTitle", "SurfacingPro Textures"))
				.SetTooltipText(LOCTEXT("TexturesTabTip", "Textures brought in as ours, from any folder on disk, and put on the selected material instance."))
				.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
				.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.texture"));
		}
		// A layout saved with the old pair opens the one window instead.
		for (const FName& Old : { FilesTabName, LibraryTabName })
		{
			if (!Manager->HasTabSpawner(Old))
			{
				Manager->RegisterNomadTabSpawner(Old, FOnSpawnTab::CreateStatic(&Spawn))
					.SetDisplayName(LOCTEXT("TexturesTabTitle", "SurfacingPro Textures"))
					.SetMenuType(ETabSpawnerMenuType::Hidden);
			}
		}
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FilesTabName);
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LibraryTabName);
		}
	}

	void Open()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		Register();
		// Open, or brought forward where it is - docked or in its own window.
		FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName));
		SurfSession::Verb(TEXT("textures_window"), TEXT("open"));
	}

	SSurfTextureFilesView* FilesView()
	{
		TSharedPtr<SSurfTextureFilesView> Live = GTexturesFilesView.Pin();
		return Live.IsValid() ? Live.Get() : nullptr;
	}
}

#undef LOCTEXT_NAMESPACE
