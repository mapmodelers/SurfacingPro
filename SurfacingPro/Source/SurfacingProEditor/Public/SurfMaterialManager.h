// Copyright MapModelers. All Rights Reserved.
//
// The Material Manager window (M-E).
//
// The Materials window is the picker: light, a click puts a material on the
// selection. This is where a material is looked at and edited, with a list of
// its own - the same tree as the picker, a pick here changing nothing in the
// level: the material picked, on the preview mesh - the author's
// calibration cube, two material slots by design, 0 for opaque geometry and
// 1 for alpha geometry, so the instance goes on the slot its blend mode
// says and the other half stays the cube - under a long lens, with its
// parameters in a column beside it to edit, MapLink's way, saved with one
// button; Apply puts it on the level's selection when that is wanted. The
// Textures window dresses the material picked last, here or in the picker.

#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"
#include "SurfMaterialsView.h"
#include "SurfTextureSuggest.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/SCompoundWidget.h"
#include "EditorUndoClient.h"

class FAdvancedPreviewScene;
class FAssetThumbnailPool;
class FSurfMaterialPreviewViewportClient;
class IDetailsView;
class ITableRow;
class SScrollBox;
class SSearchBox;
class SVerticalBox;
class FMenuBuilder;
class UMaterial;
class STableViewBase;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UStaticMesh;
class FScopedTransaction;
class UStaticMeshComponent;

namespace SurfMaterialPreview
{
	/** Slot 0 for opaque and masked, 1 for the translucent family (translucent, additive, modulate, composite, holdout). Null: 0. */
	int32 SlotFor(const UMaterialInterface* Material);

	/** The preview mesh's package path from the settings, or the plugin's cube. */
	FSoftObjectPath MeshPath();

	/** The preview mesh, loaded: the settings' one, else the plugin's cube, else the engine's (one slot: opaque only). */
	UStaticMesh* LoadMesh();

	/** The lens and the stance the preview and the thumbnails share: 30 degrees, from front-left above. */
	float FOV();
	FVector ViewLocation();
	FRotator ViewRotation();

	/** MapLink's rule: a material with displacement is one with "Dsp" in its name or its master's. */
	bool UsesDisplacement(const UMaterialInterface* Material);

	/** The displacement mesh's package path from the settings, or the plugin's denser cube (mat_cube_dsp). */
	FSoftObjectPath DisplacementMeshPath();

	/** The mesh a material is shown on: the displacement cube for one with displacement, the cube otherwise. */
	FSoftObjectPath MeshPathFor(const UMaterialInterface* Material);
	UStaticMesh* LoadMeshFor(const UMaterialInterface* Material);
}

/** The preview: one material on the cube, live. */
class SSurfMaterialPreviewViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SSurfMaterialPreviewViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSurfMaterialPreviewViewport() override;

	/** The material on the slot its blend mode says, on the mesh its kind says; the other slot reverts to the mesh's own. Null clears both. */
	void SetMaterial(UMaterialInterface* Material);
	UMaterialInterface* GetMaterial() const { return Shown; }

	/** The material changed under it: draw again. */
	void Refresh();

	/** The mesh's bounds in the scene; invalid without a mesh. */
	FBox MeshBox() const;
	/** Frame the mesh (F); back to the stance (Home). */
	void FrameMesh();
	void HomeView();

	/** True when a mesh was found to show it on. */
	bool HasMesh() const { return MeshComponent != nullptr; }

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SSurfMaterialPreviewViewport"); }

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FSurfMaterialPreviewViewportClient> ViewportClient;
	TObjectPtr<UStaticMeshComponent> MeshComponent = nullptr;
	TObjectPtr<UMaterialInterface> Shown = nullptr;
};

/**
 * The tab's contents, MapLink's way: the library's tree on the left, the
 * inspector in the middle - a column of the instance's parameters, each with
 * its override tick, its name and its editor, the parent above them, the
 * instance's other properties folded under "More" - and the preview on the
 * right. A texture picked on one parameter offers its siblings to the others.
 */
class SSurfMaterialManagerView : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SSurfMaterialManagerView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSurfMaterialManagerView();

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;

	// Ctrl+Z / Ctrl+Y anywhere in the window go to the editor's undo.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;

	// FEditorUndoClient: an undo or a redo touched what is shown - draw it again.
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

	/** This material shown and picked in the tree (S251a); a change of selection moves it again. */
	void ShowMaterial(UMaterialInterface* Material);

private:
	using FItemPtr = TSharedPtr<FSurfMaterialItem>;
	using FNodePtr = TSharedPtr<FSurfMaterialNode>;

	// The tree, the manager's own.
	void Refresh();
	void ApplyFilter();
	void ExpandAll(const TArray<FNodePtr>& Nodes);
	TSharedRef<ITableRow> MakeRow(FNodePtr Node, const TSharedRef<STableViewBase>& Owner);
	void ChildrenOf(FNodePtr Node, TArray<FNodePtr>& Out);
	void OnRowSelected(FNodePtr Node, ESelectInfo::Type How);
	void OnQueryChanged(const FText& Text);
	UMaterialInterface* MaterialOf(const FNodePtr& Node) const;
	void OnAssetChanged(const FAssetData& Asset);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldPath);
	void MarkDirty();
	/** Select the row of this name, when the tree has it. */
	void SelectByName(const FString& Name);
	/** The Material menu (S252e): what the button strip held, the row menu's rows too. */
	TSharedRef<SWidget> MakeMaterialMenu();
	void FillMaterialMenu(class FMenuBuilder& Menu);
	/** The tree's shape: pictures (none, rendered, the first texture), the order (name, newest), parents as headings or a flat list. Remembered. */
	TSharedRef<SWidget> MakeViewMenu();
	void SaveView();
	/** The row's picture, by the choice: the manager's own render, or the instance's first texture. */
	TSharedRef<SWidget> MakeRowPicture(const FSurfMaterialItem& Item);
	TSharedPtr<SWidget> MakeContextMenu();
	FReply OnTreeKeyDown(const FGeometry& Geometry, const FKeyEvent& Event);
	void OnRowDoubleClicked(FNodePtr Node);

	// The actions, on the material picked.
	UMaterialInterface* DefaultParent() const;
	/** The New menu: an instance of the default parent, of any parent, a child of the picked instance, a new parent from a master. */
	TSharedRef<SWidget> MakeNewMenu();
	void FillNewMenu(FMenuBuilder& Menu);
	void NewInstanceOfDefault();
	/** Asks for the name (a box with the proposed one, the existing names to pick from), then makes it. */
	void NewInstanceOf(TWeakObjectPtr<UMaterialInterface> Parent);
	void MakeInstanceNamed(TWeakObjectPtr<UMaterialInterface> Parent, const FString& Name);
	void NewChildOfPicked();
	void NewParentFrom(TWeakObjectPtr<UMaterial> Master);
	void DuplicatePicked();
	void RenamePicked();
	void RenamePickedTo(const FString& NewName);
	void DeletePicked();
	void ShowPickedInContentBrowser();
	void ShowPickedInExplorer();
	/** A new instance just made here: listed, picked, shown. */
	void TakeMade(UMaterialInstanceConstant* Made, const FString& Line);

	/** Show this material: on the cube, and its parameters when it is an instance. */
	void Show(UMaterialInterface* Material);

	// The inspector.
	/** The parameter rows of the instance shown, built afresh. */
	void BuildParameters();
	/** A row: the override tick, the name (bold when overridden), the editor. */
	void AddRow(const FString& Name, TFunction<bool()> IsOverridden, TFunction<void(bool)> SetOverridden, const TSharedRef<SWidget>& Editor, const FText& Tip = FText());
	void AddHeading(const FText& Title, const TSharedPtr<SWidget>& Beside = nullptr);
	/** The instance changed here: the preview follows, Save lights up. */
	void MarkChanged();
	/** One edit of the instance shown, as a transaction the editor can undo: What names it in the history. */
	void Edit(UMaterialInstanceConstant* Instance, const FText& What, TFunctionRef<void()> Change);
	/** A slider drag is one transaction from the first move to the release. */
	void BeginDrag(UMaterialInstanceConstant* Instance, const FText& What);
	void EndDrag();
	bool Dragging() const { return Drag.IsValid(); }
	TUniquePtr<FScopedTransaction> Drag;
	void OnParentPicked(const FAssetData& Asset);
	/** A texture was put on the parameter: suggestions for the others. */
	void OnTexturePicked(FName Parameter);
	bool ShouldFilterTexture(const FAssetData& Asset) const;
	void OnConformOnlyToggled(ECheckBoxState NewState);
	ECheckBoxState ConformOnlyState() const;
	void RebuildSuggestions();
	void UseSuggestion(int32 Index);
	void UseAllSuggestions();
	void OnMoreChanged(const FPropertyChangedEvent& Event);

	void SaveShown();
	void RevertShown();
	void OpenShown();
	void ApplyShown();
	bool CanSave() const;
	bool HasShown() const;
	bool InstanceShown() const;
	UMaterialInstanceConstant* ShownInstance() const;

	void Say(const FString& Line);
	FText HeaderText() const;
	FText FootText() const;
	FText EmptyText() const;
	EVisibility EmptyVisibility() const;
	EVisibility InstanceVisibility() const;
	EVisibility ParentOnlyVisibility() const;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<STreeView<FNodePtr>> Tree;
	TSharedPtr<SSurfMaterialPreviewViewport> Preview;
	/** The parameter rows. */
	TSharedPtr<SScrollBox> ParamsBox;
	/** The offered siblings, under the textures. */
	TSharedPtr<SVerticalBox> SuggestBox;
	/** The instance's other properties (the base property overrides, the physical material...), the parameter arrays hidden. */
	TSharedPtr<IDetailsView> More;
	TSharedPtr<FAssetThumbnailPool> Thumbnails;

	TArray<FSurfMaterialItem> Library;
	TArray<FNodePtr> Roots;
	FNodePtr Selected;
	FString Query;
	FString RootRead;
	SurfMaterialsLib::ERootState State = SurfMaterialsLib::ERootState::Unset;
	bool bDirty = true;
	double LastRead = 0.0;

	/** The material shown. */
	TWeakObjectPtr<UMaterialInterface> Target;
	bool bChanged = false;

	/** What the selection wears (S197a): shown when the selection changes, marked in the list. */
	TArray<FString> WornNames;
	FString WornKey;
	double WornCheckedAt = 0.0;
	void FollowSelection();

	/** The texture picker lists conformed textures only; remembered per user and project. */
	bool bConformOnly = false;

	/** The base name of the texture picked last (T_Sp_Brick for T_Sp_Brick_N): the next picker opens searching for it. */
	FString LastPickBase;

	/** The tree's shape. */
	int32 PicturesMode = 0;
	int32 SortMode = 0;
	bool bFlat = false;
	TArray<SurfTextureSuggest::FSurfTextureSuggestion> Suggestions;

	FString Notice;
	double NoticeAt = -100.0;
};

namespace SurfMaterialManagerTab
{
	/** "SurfacingProMaterialManager", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward if it is already open. */
	void Open();

	/** Open it on this material (S251a): the viewport's right-click and the Material Library's. */
	void OpenWith(UMaterialInterface* Material);
}
