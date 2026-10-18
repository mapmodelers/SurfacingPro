// Copyright MapModelers. All Rights Reserved.
//
// The Materials window (M-B0).
//
// The material is the unit of surfacing (D55): a patch or a brush face wears
// one, any the user owns. This is where they are picked from - the project's
// own library under one root, its instances under the parent each was made
// from, narrowed by a search, and put on the selection with a click. The
// parents themselves are chosen, not shown (D68): New MI asks which. New
// instances are made here, copied here, and the surfacing info a material
// carries is read and set here. The window reads the library through the asset
// registry and the selection through the mode as it ticks; nothing is
// pushed to it, so it can be opened before the mode is entered and is still
// right after the mode is left and entered again (D59's shape).
//
// The listing, the filter, the tree, the making and the assignment live in
// SurfMaterialsLib, apart from the widget, so they are tested rather than
// eyeballed.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class IDetailsView;
class ITableRow;
class SSearchBox;
class STableViewBase;
class UMaterial;
class UMaterialInstanceConstant;
class UMaterialInterface;
class USurfEdMode;
class USurfMaterialInfo;
struct FPropertyChangedEvent;

/** One material of the library, as the window lists it. */
struct FSurfMaterialItem
{
	FAssetData Asset;

	/** The asset's own name: what the list shows and the search matches. */
	FString Name;

	/** An instance's parent, by name; empty for a material of its own. */
	FString Parent;

	bool bInstance = false;
};

/** A row of the tree: an instance of the library, or the heading its instances sit under, named after their parent. */
struct FSurfMaterialNode
{
	/** Null for a heading. */
	TSharedPtr<FSurfMaterialItem> Item;
	FString Label;
	TArray<TSharedPtr<FSurfMaterialNode>> Children;

	bool IsGroupOnly() const { return !Item.IsValid(); }
};

namespace SurfMaterialsLib
{
	/** Why a listing came back the way it did. */
	enum class ERootState : uint8
	{
		/** The setting is empty. */
		Unset,
		/** The registry is still reading the project. */
		Loading,
		/** The root names a folder the project does not have. */
		Missing,
		/** The folder is there and holds no material. */
		Empty,
		/** Materials were found. */
		Listed,
	};

	/** The root as the setting gives it, trimmed, without a trailing slash; empty when unset. */
	FString NormalisedRoot(const FString& Root);

	/** Every material and instance under Root and its sub-folders, by name, case blind. */
	ERootState List(const FString& Root, TArray<FSurfMaterialItem>& Out);

	/** The items whose name or parent holds every word of Query, case blind; all of them for an empty query. */
	void Filter(const TArray<FSurfMaterialItem>& Items, const FString& Query, TArray<FSurfMaterialItem>& Out);

	/**
	 * The tree: the instances, each under a heading named after its parent -
	 * or under the instance it was made from, when that one is listed - and
	 * at the top when it has none. The parents themselves are not rows. By
	 * name at every level.
	 */
	void BuildTree(const TArray<FSurfMaterialItem>& Items, TArray<TSharedPtr<FSurfMaterialNode>>& Out);

	/** The materials of their own among the items - what a new instance can be made from - in the order given. */
	void Masters(const TArray<FSurfMaterialItem>& Items, TArray<FSurfMaterialItem>& Out);

	/** The master New MI uses when nothing else is chosen: the one the settings name, else the first; INDEX_NONE for none. */
	int32 DefaultMasterIndex(const TArray<FSurfMaterialItem>& Masters);

	/** <Root>/Materials - where the parents live. */
	FString MastersFolder(const FString& Root);

	/**
	 * A new parent: a copy of a master in Folder, named after it and numbered,
	 * keeping its info and dropping any seed stamp - the user's own to edit,
	 * never touched by an update of the seeded ones. Null with the reason.
	 */
	UMaterial* CopyMaster(UMaterial* Source, const FString& Folder, bool bSave, FString& OutError);

	/** What the window says in place of the list, in one line, for a state that listed nothing. */
	FString EmptyLine(ERootState State, const FString& Root);

	/** The parent's name out of an instance's Parent tag: an object path, with or without the class and quotes round it. */
	FString ParentNameFromTag(const FString& Tag);

	/** Put the material on the selection through the mode's inspector; false with the reason on the bar. */
	bool Assign(USurfEdMode& Mode, UMaterialInterface* Material);

	/** The line at the foot of the window: what is picked, or what to pick. */
	FString SelectionLine(const USurfEdMode* Mode);

	/** Where new instances go: <Root>/MI. */
	FString InstancesFolder(const FString& Root);
	/**
	 * Instances left in the older folders - Root/MI and Root/Materials/Instances
	 * - moved into Root/MaterialInstances (S208a), redirectors fixed up.
	 * Returns how many moved; OutError says what would not.
	 */
	SURFACINGPROEDITOR_API int32 TidyInstanceFolders(const FString& Root, FString& OutError);

	/** The name an instance of Parent starts from: MI_<the parent's name without its M_ or MI_>. */
	FString InstanceBaseName(const UMaterialInterface* Parent);

	/** A name no asset in Folder has: Base, else Base_02, Base_03 ... */
	FString UniqueAssetName(const FString& Folder, const FString& Base);

	/**
	 * A new instance of Parent in Folder, its surfacing info copied from the
	 * parent's, saved when asked. Null, with the reason in the house voice,
	 * when it cannot be made.
	 */
	UMaterialInstanceConstant* NewInstance(UMaterialInterface* Parent, const FString& Folder, bool bSave, FString& OutError);

	/** A copy of an instance in Folder, keeping its info and dropping any seed stamp: the copy is the user's own. */
	UMaterialInstanceConstant* CopyInstance(UMaterialInstanceConstant* Source, const FString& Folder, bool bSave, FString& OutError);

	/** As NewInstance, named as asked (made unique in Folder when the name is taken). */
	UMaterialInstanceConstant* NewInstanceNamed(UMaterialInterface* Parent, const FString& Folder, const FString& Name, bool bSave, FString& OutError);

	/** The material on the selection: the first patch the selection reaches into, else the first selected face; null for none. */
	UMaterialInterface* SelectionMaterial(const USurfEdMode* Mode);

	/**
	 * Every material the selection wears, first seen first (S197a): the
	 * patches it reaches, the primitive faces, and - with none of ours
	 * picked - the meshes of the other selected actors. What the Materials
	 * window and the Manager follow.
	 */
	void SelectionMaterials(const USurfEdMode* Mode, TArray<UMaterialInterface*>& Out);

	/** The names of the instances among the items, sorted, for a name to be picked from. */
	void InstanceNames(const TArray<FSurfMaterialItem>& Items, TArray<FString>& Out);

	/** The material the Materials window has selected, for the Textures window to dress; null when none. */
	UMaterialInterface* CurrentTarget();
	void SetCurrentTarget(UMaterialInterface* Material);
}

/** The tab's contents: the search and the verbs, the tree, the info, and the line at the foot. */
class SSurfMaterialsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfMaterialsView) : _InPanel(false) {}
		/** In the SidePanel (S251c): the tree a fixed height, no info block. */
		SLATE_ARGUMENT(bool, InPanel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Ctrl+Z / Ctrl+Y anywhere in the window go to the editor's undo.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;

private:
	using FItemPtr = TSharedPtr<FSurfMaterialItem>;
	using FNodePtr = TSharedPtr<FSurfMaterialNode>;

	USurfEdMode* LiveMode() const;

	/** Read the library again and rebuild the tree. */
	void Refresh();

	/** Narrow the tree to the query, keeping the library as read. */
	void ApplyFilter();
	void ExpandAll(const TArray<FNodePtr>& Nodes);

	TSharedRef<ITableRow> MakeRow(FNodePtr Node, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeNewMenu();
	void ChildrenOf(FNodePtr Node, TArray<FNodePtr>& Out);
	void OnRowClicked(FNodePtr Node);
	void OnRowSelected(FNodePtr Node, ESelectInfo::Type How);
	TSharedPtr<SWidget> OpenRowMenu();
	void OnQueryChanged(const FText& Text);
	void OnQueryCommitted(const FText& Text, ETextCommit::Type Commit);

	/** Put a row's material on the selection, with the result at the foot too. */
	void AssignNode(FNodePtr Node);

	/** The verbs. */
	void NewInstanceOf(FItemPtr Master);
	void NewInstanceOfDefault();
	void NewParentFrom(FItemPtr Master);
	void NewInstanceBesideSelected();
	/** An instance of Parent, saved, put on the selection; ToOpen (else the instance) opens for editing. */
	UMaterialInstanceConstant* MakeInstance(UMaterialInterface* Parent, UObject* ToOpen = nullptr);
	void CopySelected();
	void OpenSelected();
	void AddInfoToSelected();
	void SeedParents();
	void TidyFolders();
	/** Every picture rendered afresh: the files under <SourceRoot>/Thumbnails deleted, the memory dropped. */
	void RenderPicturesAgain();
	/** The plugin's newer masters over the copies left alone; a changed copy kept, the new one beside it, its instances moved if he says so. */
	void UpdateMasters();
	bool HasSelectedMaterial() const;
	bool HasSelectedInstance() const;
	bool NeedsSeeding() const;

	/** The selected row's material, loaded; null for a group-only row or nothing selected. */
	UMaterialInterface* SelectedMaterial() const;

	/** Show the selected material's info in the panel, or nothing. */
	void ShowInfoOf(UMaterialInterface* Material);
	void OnInfoChanged(const FPropertyChangedEvent& Event);

	/** The registry changed under the window: read it again on the next tick. */
	void OnAssetChanged(const FAssetData& Asset);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldPath);
	void MarkDirty();

	void Say(const FString& Line);

	FText FootText() const;
	FText EmptyText() const;
	EVisibility EmptyVisibility() const;
	FText InfoHeaderText() const;
	EVisibility InfoVisibility() const;
	EVisibility AddInfoVisibility() const;
	EVisibility SeedVisibility() const;
	EVisibility UpdateVisibility() const;

	/** Pictures beside the rows: the manager's thumbnails, on or off, remembered per user and project. */
	ECheckBoxState PicturesState() const;
	void OnPicturesToggled(ECheckBoxState NewState);
	bool bPictures = false;
	bool bInPanel = false;
	/** Smaller rows and type, remembered per user and project. */
	bool bSmall = false;
	void OnSmallToggled(ECheckBoxState NewState);
	ECheckBoxState SmallState() const;
	EVisibility FootVisibility() const;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<STreeView<FNodePtr>> Tree;
	TSharedPtr<IDetailsView> InfoView;

	/** The library as last read, and the tree the query leaves of it. */
	TArray<FSurfMaterialItem> Library;
	TArray<FNodePtr> Roots;
	SurfMaterialsLib::ERootState State = SurfMaterialsLib::ERootState::Unset;
	FString RootRead;
	FString Query;

	/** The row last selected, and the material it stands for once loaded. */
	FNodePtr Selected;
	TWeakObjectPtr<UMaterialInterface> InfoMaterial;
	TWeakObjectPtr<USurfMaterialInfo> InfoShown;

	bool bDirty = true;
	double LastRead = 0.0;
	/** A picture was not taken because its material was still compiling: the list is read again a moment later. */
	double PicturesPendingAt = -1.0;

	/** The last thing the window did, shown at the foot for a moment. */
	FString Notice;
	double NoticeAt = -100.0;

	/** What the selection wears (S197a): marked in the list, the first selected. */
	TArray<FString> WornNames;
	FString WornKey;
	double WornCheckedAt = 0.0;
	void FollowSelection();
	void SelectByName(const FString& Name);
	TSharedRef<SWidget> MakeMaterialMenu();
	TSharedRef<SWidget> MakeViewOptionsMenu();
};

namespace SurfMaterialsTab
{
	/** "SurfacingProMaterials", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward if it is already open. */
	void Open();

	/** The SidePanel's Materials tab (S251c): the same list, compact. */
	TSharedRef<SWidget> MakePanel();
}
