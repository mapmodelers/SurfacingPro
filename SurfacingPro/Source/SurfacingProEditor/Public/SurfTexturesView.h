// Copyright MapModelers. All Rights Reserved.
//
// The Textures windows (T-a).
//
// Two tabs, opened side by side and docked anywhere from there. Texture Files:
// any folder on the machine, read without importing a thing - a folder tree on
// the left, the folder's image files with a picture of each on the right, those
// already in the library marked (and hidden, when asked) - picked and brought
// in as ours, conformed by default so the file the team keeps is the truth, or
// linked where they are. Texture Library: the library's own textures with their
// standing (conform, portable, local only, broken, changed on disk), the stray
// TGAs under the source root, the verbs that put them right (Conform, Read
// again, Read another file, Import), and under them the material
// the Materials window has selected with its texture parameters as buttons: a
// texture picked on either side goes on a parameter with one click. Ctrl+wheel
// over any list goes from rows to tiles and up in size, as Explorer does; a
// double-click opens a texture, a right-click shows it in Explorer or the
// Content Browser. The folder, the zoom and the mode are remembered per user
// and project. Nothing is pushed to the windows; they read the disk, the
// registry, each other's pick and the Materials window's selection as they go.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "AssetRegistry/AssetData.h"
#include "SurfTextureLib.h"
#include "SurfTextureSuggest.h"
#include "Styling/SlateColor.h"
#include "Types/SlateEnums.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STileView.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class SEditableTextBox;
class STableViewBase;
class SWrapBox;
class UTexture2D;
struct FImage;
struct FSlateBrush;

/** One thing in a folder on disk: a sub-folder, or an image file. */
struct FSurfDiskEntry
{
	FString Path;
	FString Name;
	bool bFolder = false;
	int64 Bytes = 0;
	FDateTime Modified;
	/** The name as shown: the file's, or its path from the folder when the sub-folders are read too. */
	FString Shown;
};

/** A folder in the tree on the left, its children read when it is opened. */
struct FSurfFolderNode
{
	FString Path;
	FString Name;
	TArray<TSharedPtr<FSurfFolderNode>> Children;
	bool bScanned = false;
};

/** A row of the Texture Library tab: a texture with its standing, or a stray TGA under the source root that no texture reads. */
struct FSurfLibraryRow
{
	FAssetData Asset;
	FString Orphan;
	SurfTextures::ESurfTextureStatus Status = SurfTextures::ESurfTextureStatus::External;
	bool bStale = false;

	bool IsOrphan() const { return !Orphan.IsEmpty(); }

	/** What names the row across a refresh: the object path, or the file. */
	FString Key() const { return IsOrphan() ? Orphan : Asset.GetObjectPathString(); }
};

/** How the library stands, for the line above it. */
struct FSurfLibraryCounts
{
	int32 Textures = 0;
	int32 Conform = 0;
	int32 Portable = 0;
	int32 LocalOnly = 0;
	int32 Broken = 0;
	int32 External = 0;
	int32 Stale = 0;
	int32 Orphans = 0;
};

namespace SurfTexturesLib
{
	/** A folder path made whole: absolute, forward slashes, no trailing slash - except a drive, which keeps its one (C:/). */
	FString NormalisedFolder(const FString& Folder);

	/** The sub-folders, then the image files, of a folder on disk, by name. False when the folder cannot be read. */
	bool ListFolder(const FString& Folder, TArray<FSurfDiskEntry>& Out);

	/** The image files of a folder and every folder under it, at most MaxFiles, shown by their path from the folder. False when the folder cannot be read. */
	bool ListFolderRecursive(const FString& Folder, TArray<FSurfDiskEntry>& Out, int32 MaxFiles, bool& bOutCapped);

	/** The sort orders: 0 name, 1 newest first, 2 largest first, 3 kind then name. Folders stay first. */
	int32 SortOrders();
	const TCHAR* SortName(int32 Order);
	void SortEntries(TArray<TSharedPtr<FSurfDiskEntry>>& Entries, int32 Order);

	/** The sub-folders of a folder, by name, hidden and system ones left out. */
	void ListSubfolders(const FString& Folder, TArray<FString>& Out);

	/** Where the tree starts: the samples, the source root, the project, then every drive that is there. */
	void ListRoots(TArray<FString>& Out);

	/** Bytes as people read them: 1.2 MB, 340 KB. */
	FString BytesText(int64 Bytes);

	/** A picture's pixels, small (S208a): what a pool thread can make; the texture is made from it on the game thread. */
	struct FSurfSmallPicture
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;
	};
	/** Decode a file and shrink it to fit Size - safe off the game thread. */
	bool DecodeSmall(const FString& File, int32 Size, FSurfSmallPicture& Out);
	/** The texture of a small picture; game thread. */
	UTexture2D* TextureOf(const FSurfSmallPicture& Small);

	/** A picture from a decoded image, made BGRA and scaled to fit Size, alpha shown opaque; the image is converted in place. */
	UTexture2D* MakePicture(FImage& Image, int32 Size);

	/** A picture of an image file - decoded in its own layout. Null when the file cannot be read as an image or is too big to bother (over 48 MB). */
	UTexture2D* MakeThumbnail(const FString& File, int32 Size);

	/** A picture of a texture asset from its source pixels. Null when it has no source, or the source is over 8192 on a side. */
	UTexture2D* MakeAssetPicture(UTexture2D* Texture, int32 Size);

	/** The zoom levels: 0 is rows with a small picture; 1 and up are tiles, larger each step. */
	int32 ZoomLevels();
	int32 ZoomSize(int32 Level);
	bool ZoomIsTiles(int32 Level);

	/** The level after a wheel step (up is larger), kept within the levels. */
	int32 StepZoom(int32 Level, float WheelDelta);

	/** What the library holds, for marking files on disk: every texture by name, and the file each linked one reads. */
	struct FSurfLibraryIndex
	{
		/** Asset name -> object path. */
		TMap<FString, FString> ByName;
		/** Normalised file (NormalisedFile) -> asset name, for textures reading a file where it is. */
		TMap<FString, FString> BySource;
	};

	/** A file path as a key: absolute, forward slashes, lower case. */
	FString NormalisedFile(const FString& Path);

	void IndexLibrary(const FString& LibraryRoot, FSurfLibraryIndex& Out);

	/** The library texture a file on disk is already in as - linked from it, or named after it (T_Sp_<Stem>); empty when none. */
	FString InLibrary(const FSurfLibraryIndex& Index, const FString& File);

	/** True when a library texture reads this very file. */
	bool IsLinked(const FSurfLibraryIndex& Index, const FString& File);
}

/** What the two tabs share: the folder, the pick, the mode, the zoom; remembered per user and project. */
struct FSurfTexturesShared
{
	FString Folder;
	int32 Zoom = 0;
	bool bConform = true;
	bool bHideImported = false;
	bool bFoldersShown = true;
	bool bRecursive = false;
	int32 SortBy = 0;

	/** Folders pinned at the top of the tree. */
	TArray<FString> Favorites;

	/** The image files picked in the Texture Files tab, for the Library tab to bring in and put on a parameter. */
	TArray<FString> PickedFiles;

	/** The last texture brought in, by object path, for the Library tab to pick once it lists it. */
	FString LastImported;

	static FSurfTexturesShared& Get();

	void Load();
	void Save() const;
};

/** The Texture Files tab: the disk. */
class SSurfTextureFilesView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfTextureFilesView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Ctrl+Z / Ctrl+Y anywhere in the window go to the editor's undo.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
	virtual ~SSurfTextureFilesView();

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

	/** Files dropped on either tab: the folder of the first is opened here and they are picked. */
	void PickDropped(const TArray<FString>& Files);

private:
	using FDiskPtr = TSharedPtr<FSurfDiskEntry>;
	using FFolderPtr = TSharedPtr<FSurfFolderNode>;

	// The folder.
	void GoTo(const FString& Folder, bool bFromTree = false);
	void GoUp();
	void GoToSamples();
	void OpenFolderInExplorer();
	void PickFiles();
	void OnPathCommitted(const FText& Text, ETextCommit::Type Commit);
	void ApplyFilter();
	ECheckBoxState FavoriteState() const;
	void OnFavoriteToggled(ECheckBoxState State);
	void RebuildFavorites();
	ECheckBoxState RecursiveState() const;
	void OnRecursiveToggled(ECheckBoxState State);
	TSharedRef<SWidget> MakeSortMenu();
	FText SortLabel() const;
	/** The rest of the Files toolbar (S252e): Samples, In Explorer, Favourite, how they come in, the filters, the order. */
	TSharedRef<SWidget> MakeMoreMenu();
	void SetSort(int32 Order);

	// The tree.
	TSharedRef<ITableRow> MakeFolderRow(FFolderPtr Node, const TSharedRef<STableViewBase>& Owner);
	void FolderChildren(FFolderPtr Node, TArray<FFolderPtr>& Out);
	void OnFolderSelected(FFolderPtr Node, ESelectInfo::Type How);
	void RevealInTree(const FString& Folder);
	EVisibility FoldersVisibility() const;
	ECheckBoxState FoldersState() const;
	void OnFoldersToggled(ECheckBoxState State);

	// The files.
	TSharedRef<ITableRow> MakeDiskRow(FDiskPtr Entry, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeDiskTile(FDiskPtr Entry, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MarkFor(const FString& File, bool bFolder) const;
	TSharedPtr<SWidget> OpenDiskMenu();
	void OnDiskDoubleClick(FDiskPtr Entry);
	void OnDiskSelectionChanged(FDiskPtr Entry, ESelectInfo::Type How);
	void ShowPickedInExplorer();
	const FSlateBrush* ThumbnailFor(const FString& File);
	TArray<FString> PickedFiles() const;
	TSharedPtr<SListView<FDiskPtr>> ActiveDiskView() const;

	// Bringing in.
	void ImportPicked();
	void ImportFiles(const TArray<FString>& Files);

	// The library, for the marks.
	void RefreshIndex();
	void OnAssetChanged(const FAssetData& Asset);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldPath);
	void MarkDirty();

	// The size of the pictures.
	void ApplyZoom();
	void OnZoomWheel(float WheelDelta);
	int32 ZoomIndex() const;
	float TileWidth() const;
	float TileHeight() const;

	void Say(const FString& Line);
	FText FootText() const;
	FText ModeText() const;
	FText EmptyDiskText() const;
	EVisibility EmptyDiskVisibility() const;
	ECheckBoxState ConformState() const;
	ECheckBoxState LinkState() const;
	ECheckBoxState HideImportedState() const;
	void OnHideImportedChanged(ECheckBoxState State);

	TSharedPtr<SEditableTextBox> PathBox;
	TSharedPtr<STreeView<FFolderPtr>> FolderTree;
	TSharedPtr<SListView<FDiskPtr>> DiskList;
	TSharedPtr<STileView<FDiskPtr>> DiskTiles;

	TArray<FFolderPtr> FolderRoots;
	FFolderPtr FavoritesRoot;
	bool bCapped = false;
	bool bRevealing = false;

	FString Folder;
	TArray<FDiskPtr> AllEntries;
	TArray<FDiskPtr> DiskEntries;
	bool bFolderReadable = true;

	SurfTexturesLib::FSurfLibraryIndex Index;
	FString IndexRoot;
	bool bIndexDirty = true;
	double LastIndexed = 0.0;

	/** The zoom the rows and tiles were made at; the shared one may have moved since. */
	int32 ZoomShown = 0;

	/** Pictures of files, decoded once at the largest size and shown at any, kept while the window lives. */
	TMap<FString, TStrongObjectPtr<UTexture2D>> Thumbnails;
	TMap<FString, TSharedPtr<FSlateBrush>> ThumbnailBrushes;
	/** Decodes on the pool (S208a): asked for by the tiles, a few in flight, finished ones picked up each tick. */
	struct FSurfThumbJob
	{
		std::atomic<bool> bDone { false };
		bool bOk = false;
		SurfTexturesLib::FSurfSmallPicture Small;
	};
	TMap<FString, TSharedPtr<FSurfThumbJob>> ThumbnailJobs;
	TArray<FString> ThumbnailWanted;
	void PumpThumbnails();
	/** The picture, or the placeholder while it is on its way. */
	const FSlateBrush* ShownThumbnail(const FString& File);

	FString Notice;
	double NoticeAt = -100.0;
};

/** The Texture Library tab: the library's textures with their standing, the strays, the verbs that put them right, and the instance to dress. */
class SSurfTextureLibraryView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfTextureLibraryView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Ctrl+Z / Ctrl+Y anywhere in the window go to the editor's undo.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
	virtual ~SSurfTextureLibraryView();

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	using FRowPtr = TSharedPtr<FSurfLibraryRow>;

	// The library.
	void RefreshProject();
	bool StandingsMoved() const;
	TSharedRef<ITableRow> MakeAssetRow(FRowPtr Row, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeAssetTile(FRowPtr Row, const TSharedRef<STableViewBase>& Owner);
	const FSlateBrush* PictureFor(const FSurfLibraryRow& Row);
	FString StandingText(const FSurfLibraryRow& Row, FSlateColor& OutColour) const;
	static FString NameOf(const FSurfLibraryRow& Row);
	static FString TipOf(const FSurfLibraryRow& Row);
	TSharedPtr<SWidget> OpenAssetMenu();
	void OnAssetDoubleClick(FRowPtr Row);
	FRowPtr PickedRow() const;
	UTexture2D* PickedTexture() const;
	void OpenPickedAsset();
	void ShowPickedInContentBrowser();
	void ShowPickedFileInExplorer();
	TSharedPtr<SListView<FRowPtr>> ActiveProjectView() const;
	void OnAssetChanged(const FAssetData& Asset);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldPath);
	void MarkDirty();

	// Keeping it right.
	SurfTextures::FSurfTextureImportOptions OptionsNow() const;
	bool CanConformPicked() const;
	bool CanConformAll() const;
	/** Conform picked / Conform all, under the library's arrow (S252e). */
	TSharedRef<SWidget> MakeMoreMenu();
	bool CanReimportPicked() const;
	void ConformPicked();
	void ConformAll();
	void ReimportPicked();
	void LinkPicked();
	void ImportPickedOrphan();

	// The size of the pictures.
	void ApplyZoom();
	void OnZoomWheel(float WheelDelta);
	int32 ZoomIndex() const;
	float TileWidth() const;
	float TileHeight() const;

	// Onto the material.
	void PutOnParameter(FName Parameter);
	void RebuildParameterButtons();
	/** The siblings of the texture just put on, that fit the other parameters (M-E4's suggester); Put the set puts them all. */
	TArray<SurfTextureSuggest::FSurfTextureSuggestion> Suggestions;
	void PutTheSet();
	FText SetButtonText() const;
	EVisibility SetButtonVisibility() const;

	void Say(const FString& Line);
	FText FootText() const;
	FText CountsText() const;
	FText TargetText() const;
	FText EmptyProjectText() const;
	EVisibility EmptyProjectVisibility() const;

	TSharedPtr<SListView<FRowPtr>> ProjectList;
	TSharedPtr<STileView<FRowPtr>> ProjectTiles;
	TSharedPtr<SWrapBox> ParameterBox;

	TArray<FRowPtr> ProjectEntries;
	FSurfLibraryCounts Counts;
	FString RootRead;
	FString SourceRootRead;
	bool bDirty = true;
	double LastRead = 0.0;
	double LastStaleCheck = 0.0;

	int32 ZoomShown = 0;

	/** Pictures: a texture's from its source pixels, keyed by path and source id; a stray's from its file. Decoded once at the largest size. */
	TMap<FString, TStrongObjectPtr<UTexture2D>> Pictures;
	TMap<FString, TSharedPtr<FSlateBrush>> PictureBrushes;

	/** The material the Materials window has selected, as last seen, and its parameters. */
	TWeakObjectPtr<class UMaterialInterface> Target;
	TArray<FName> TargetParameters;

	FString Notice;
	double NoticeAt = -100.0;
};

namespace SurfTexturesTab
{
	/** "SurfacingProTextures", in the Window menu under the level editor: the files on the left, the library on the right, either foldable. */
	extern const FName TabName;
	/** The old pair's names, answered by the one window so a saved layout still opens. */
	extern const FName FilesTabName;
	extern const FName LibraryTabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward where it is. */
	void Open();

	/** The Files tab, if open: dropped files go to it. */
	class SSurfTextureFilesView* FilesView();
}
