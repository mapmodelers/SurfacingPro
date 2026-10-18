// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialsView.h"
#include "SurfTheme.h"

#include "SurfEdMode.h"
#include "SurfGroup.h"
#include "SurfInspector.h"
#include "SurfMaterialInfo.h"
#include "SurfMaterialSeed.h"
#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfMaterialManager.h"
#include "SurfMaterialThumbnails.h"
#include "SurfTexturesView.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Selection.h"
#include "SurfWindows.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/ObjectRedirector.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DetailsViewArgs.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformTime.h"
#include "IDetailsView.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SurfacingProMaterials"

// --- The library ------------------------------------------------------------

namespace SurfMaterialsLib
{
	namespace
	{
		IAssetRegistry& LibRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}
	}

	FString NormalisedRoot(const FString& Root)
	{
		FString Out = Root.TrimStartAndEnd();
		while (Out.Len() > 1 && Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	FString ParentNameFromTag(const FString& Tag)
	{
		// The tag is an object path - /Game/A/M_Foo.M_Foo - sometimes wrapped
		// as Material'/Game/A/M_Foo.M_Foo'. The name is what follows the last
		// dot, without the closing quote.
		FString Path = Tag.TrimStartAndEnd();
		if (Path.EndsWith(TEXT("'")))
		{
			Path.LeftChopInline(1);
		}
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot))
		{
			Path.MidInline(Dot + 1);
		}
		int32 Slash = INDEX_NONE;
		if (Path.FindLastChar(TEXT('/'), Slash))
		{
			Path.MidInline(Slash + 1);
		}
		return Path;
	}

	ERootState List(const FString& Root, TArray<FSurfMaterialItem>& Out)
	{
		Out.Reset();

		const FString Path = NormalisedRoot(Root);
		if (Path.IsEmpty())
		{
			return ERootState::Unset;
		}

		IAssetRegistry& Registry = LibRegistry();

		// Every material or instance under the root, sub-folders included,
		// without loading any of them: the registry's own view of the project.
		FARFilter Wanted;
		Wanted.PackagePaths.Add(FName(*Path));
		Wanted.bRecursivePaths = true;
		Wanted.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
		Wanted.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		Registry.GetAssets(Wanted, Assets);

		for (const FAssetData& Asset : Assets)
		{
			FSurfMaterialItem& Item = Out.AddDefaulted_GetRef();
			Item.Asset = Asset;
			Item.Name = Asset.AssetName.ToString();
			const UClass* Class = Asset.GetClass();
			Item.bInstance = Class && Class->IsChildOf(UMaterialInstance::StaticClass());
			if (Item.bInstance)
			{
				FString ParentTag;
				if (Asset.GetTagValue(FName(TEXT("Parent")), ParentTag))
				{
					Item.Parent = ParentNameFromTag(ParentTag);
				}
			}
		}

		Out.Sort([](const FSurfMaterialItem& A, const FSurfMaterialItem& B)
		{
			return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
		});

		if (Out.Num() > 0)
		{
			return ERootState::Listed;
		}
		if (Registry.IsLoadingAssets())
		{
			return ERootState::Loading;
		}
		return Registry.PathExists(Path) ? ERootState::Empty : ERootState::Missing;
	}

	void Filter(const TArray<FSurfMaterialItem>& Items, const FString& Query, TArray<FSurfMaterialItem>& Out)
	{
		Out.Reset();

		TArray<FString> Words;
		Query.ParseIntoArrayWS(Words);

		for (const FSurfMaterialItem& Item : Items)
		{
			bool bEveryWord = true;
			for (const FString& Word : Words)
			{
				if (!Item.Name.Contains(Word, ESearchCase::IgnoreCase)
					&& !Item.Parent.Contains(Word, ESearchCase::IgnoreCase))
				{
					bEveryWord = false;
					break;
				}
			}
			if (bEveryWord)
			{
				Out.Add(Item);
			}
		}
	}

	namespace
	{
		void SortNodes(TArray<TSharedPtr<FSurfMaterialNode>>& Nodes)
		{
			Nodes.Sort([](const TSharedPtr<FSurfMaterialNode>& A, const TSharedPtr<FSurfMaterialNode>& B)
			{
				return A->Label.Compare(B->Label, ESearchCase::IgnoreCase) < 0;
			});
			for (const TSharedPtr<FSurfMaterialNode>& Node : Nodes)
			{
				SortNodes(Node->Children);
			}
		}

		void MarkReached(const TSharedPtr<FSurfMaterialNode>& Node, TSet<FSurfMaterialNode*>& Reached)
		{
			if (!Node.IsValid() || Reached.Contains(Node.Get()))
			{
				return;
			}
			Reached.Add(Node.Get());
			for (const TSharedPtr<FSurfMaterialNode>& Child : Node->Children)
			{
				MarkReached(Child, Reached);
			}
		}
	}

	void BuildTree(const TArray<FSurfMaterialItem>& Items, TArray<TSharedPtr<FSurfMaterialNode>>& Out)
	{
		Out.Reset();

		// A node per instance, findable by name. A material of its own is
		// not a row: the parents are chosen, not shown (D68), and one that
		// has instances is the heading they sit under.
		TArray<TSharedPtr<FSurfMaterialNode>> Nodes;
		TMap<FString, TSharedPtr<FSurfMaterialNode>> ByName;
		for (const FSurfMaterialItem& Item : Items)
		{
			if (!Item.bInstance)
			{
				continue;
			}
			TSharedPtr<FSurfMaterialNode> Node = MakeShared<FSurfMaterialNode>();
			Node->Item = MakeShared<FSurfMaterialItem>(Item);
			Node->Label = Item.Name;
			Nodes.Add(Node);
			ByName.Add(Item.Name.ToLower(), Node);
		}

		// An instance under the instance it was made from when that one is
		// listed, else under a heading named after its parent; at the top
		// when it has none.
		TMap<FString, TSharedPtr<FSurfMaterialNode>> Groups;
		for (const TSharedPtr<FSurfMaterialNode>& Node : Nodes)
		{
			const FSurfMaterialItem& Item = *Node->Item;
			if (Item.Parent.IsEmpty())
			{
				Out.Add(Node);
				continue;
			}

			const FString Key = Item.Parent.ToLower();
			if (const TSharedPtr<FSurfMaterialNode>* Parent = ByName.Find(Key))
			{
				if (*Parent != Node)
				{
					(*Parent)->Children.Add(Node);
					continue;
				}
			}

			TSharedPtr<FSurfMaterialNode>& Group = Groups.FindOrAdd(Key);
			if (!Group.IsValid())
			{
				Group = MakeShared<FSurfMaterialNode>();
				Group->Label = Item.Parent;
				Out.Add(Group);
			}
			Group->Children.Add(Node);
		}

		// Two assets of one name in two folders can leave a node hanging off
		// nothing that is shown; such a node is shown at the top rather than lost.
		TSet<FSurfMaterialNode*> Reached;
		for (const TSharedPtr<FSurfMaterialNode>& Root : Out)
		{
			MarkReached(Root, Reached);
		}
		for (const TSharedPtr<FSurfMaterialNode>& Node : Nodes)
		{
			if (!Reached.Contains(Node.Get()))
			{
				Out.Add(Node);
				MarkReached(Node, Reached);
			}
		}

		SortNodes(Out);
	}

	void Masters(const TArray<FSurfMaterialItem>& Items, TArray<FSurfMaterialItem>& Out)
	{
		Out.Reset();
		for (const FSurfMaterialItem& Item : Items)
		{
			if (!Item.bInstance)
			{
				Out.Add(Item);
			}
		}
	}

	int32 DefaultMasterIndex(const TArray<FSurfMaterialItem>& InMasters)
	{
		if (InMasters.Num() == 0)
		{
			return INDEX_NONE;
		}
		const FString Wanted = GetDefault<USurfSettings>()->DefaultParent.TrimStartAndEnd();
		for (int32 Index = 0; Index < InMasters.Num(); ++Index)
		{
			if (InMasters[Index].Name.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return 0;
	}

	FString EmptyLine(ERootState State, const FString& Root)
	{
		const FString Path = NormalisedRoot(Root);
		switch (State)
		{
		case ERootState::Unset:
			return TEXT("no library root: set Materials > Library Root in the settings");
		case ERootState::Loading:
			return TEXT("reading the project's assets");
		case ERootState::Missing:
			return FString::Printf(TEXT("nothing at %s yet: press Seed parents to start the library there, or set another root in the settings"), *Path);
		case ERootState::Empty:
			return FString::Printf(TEXT("no materials under %s yet: press Seed parents, then New instance"), *Path);
		default:
			return FString();
		}
	}

	bool Assign(USurfEdMode& Mode, UMaterialInterface* Material)
	{
		if (!Mode.Inspector)
		{
			return false;
		}
		const bool bDone = Mode.Inspector->SetMaterialOnSelection(Material);
		SurfSession::Verb(TEXT("materials_window"), FString::Printf(TEXT("assign name=%s done=%d"),
			Material ? *Material->GetName() : TEXT(""), bDone ? 1 : 0));
		return bDone;
	}

	namespace
	{
		/**
		 * What an assignment reaches, counted the way SetMaterialOnSelection
		 * reaches it: the patches a point or a face is selected on, the
		 * patches picked as wholes in object sub-mode, the selected brush
		 * faces - and, when nothing else says, the active patch.
		 */
		void ReachOfSelection(const USurfEdMode& Mode, int32& Patches, int32& Faces)
		{
			TArray<FSurfPatchRef> Spanned;
			if (Mode.GetSubMode() == ESurfSubMode::Object)
			{
				Spanned = Mode.GetSelectedPatches();
			}

			auto Reach = [&Spanned](const TWeakObjectPtr<ASurfGroup>& Group, const FGuid& PatchId)
			{
				FSurfPatchRef Ref;
				Ref.Group = Group;
				Ref.PatchId = PatchId;
				if (Ref.IsValid())
				{
					Spanned.AddUnique(Ref);
				}
			};
			for (const FSurfPointRef& Point : Mode.GetPointSelection())
			{
				Reach(Point.Group, Point.PatchId);
			}
			for (const FSurfQuadRef& Quad : Mode.GetQuadSelection())
			{
				Reach(Quad.Group, Quad.PatchId);
			}

			Faces = Mode.GetSelection().Num();
			if (Spanned.Num() == 0 && Faces == 0)
			{
				Spanned = Mode.GetSelectedPatches();
			}
			Patches = Spanned.Num();
		}
	}

	UMaterialInterface* SelectionMaterial(const USurfEdMode* Mode)
	{
		TArray<UMaterialInterface*> Worn;
		SelectionMaterials(Mode, Worn);
		return Worn.Num() > 0 ? Worn[0] : nullptr;
	}

	void SelectionMaterials(const USurfEdMode* Mode, TArray<UMaterialInterface*>& Out)
	{
		Out.Reset();
		if (!Mode)
		{
			return;
		}
		TArray<FSurfPatchRef> Spanned;
		if (Mode->GetSubMode() == ESurfSubMode::Object)
		{
			Spanned = Mode->GetSelectedPatches();
		}
		auto Reach = [&Spanned](const TWeakObjectPtr<ASurfGroup>& Group, const FGuid& PatchId)
		{
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = PatchId;
			if (Ref.IsValid())
			{
				Spanned.AddUnique(Ref);
			}
		};
		for (const FSurfPointRef& Point : Mode->GetPointSelection())
		{
			Reach(Point.Group, Point.PatchId);
		}
		for (const FSurfQuadRef& Quad : Mode->GetQuadSelection())
		{
			Reach(Quad.Group, Quad.PatchId);
		}
		if (Spanned.Num() == 0 && Mode->GetSelection().Num() == 0)
		{
			Spanned = Mode->GetSelectedPatches();
		}
		for (const FSurfPatchRef& Ref : Spanned)
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (Patch && Patch->Material.Get())
			{
				Out.AddUnique(Patch->Material.Get());
			}
		}
		for (const FSurfFaceRef& Ref : Mode->GetSelection())
		{
			const ASurfGroup* Group = Ref.Group.Get();
			const FSurfBrush* Brush = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
			if (Brush && Brush->Faces.IsValidIndex(Ref.FaceIndex) && Brush->Faces[Ref.FaceIndex].Material.Get())
			{
				Out.AddUnique(Brush->Faces[Ref.FaceIndex].Material.Get());
			}
		}
		if (Out.Num() > 0 || !GEditor)
		{
			return;
		}
		// None of ours: the other selected actors' meshes.
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			const AActor* Actor = Cast<AActor>(*It);
			if (!Actor || Actor->IsA<ASurfGroup>())
			{
				continue;
			}
			TArray<UPrimitiveComponent*> Parts;
			Actor->GetComponents(Parts);
			for (UPrimitiveComponent* Part : Parts)
			{
				TArray<UMaterialInterface*> Used;
				Part->GetUsedMaterials(Used);
				for (UMaterialInterface* Material : Used)
				{
					if (Material)
					{
						Out.AddUnique(Material);
					}
				}
			}
		}
	}

	void InstanceNames(const TArray<FSurfMaterialItem>& Items, TArray<FString>& Out)
	{
		Out.Reset();
		for (const FSurfMaterialItem& Item : Items)
		{
			if (Item.bInstance)
			{
				Out.Add(Item.Name);
			}
		}
		Out.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	}

	FString SelectionLine(const USurfEdMode* Mode)
	{
		if (!Mode)
		{
			return TEXT("mode not up: press SurfacingPro on the toolbar, then pick what the material goes on");
		}

		int32 Patches = 0;
		int32 Faces = 0;
		ReachOfSelection(*Mode, Patches, Faces);
		if (Patches + Faces == 0)
		{
			return TEXT("nothing selected: pick a patch or some faces, then click a material");
		}

		FString What;
		if (Patches > 0)
		{
			What += FString::Printf(TEXT("%d patch%s"), Patches, Patches == 1 ? TEXT("") : TEXT("es"));
		}
		if (Faces > 0)
		{
			if (!What.IsEmpty())
			{
				What += TEXT(", ");
			}
			What += FString::Printf(TEXT("%d face%s"), Faces, Faces == 1 ? TEXT("") : TEXT("s"));
		}
		return FString::Printf(TEXT("%s picked - click a material to put it on"), *What);
	}

	// --- Making ---------------------------------------------------------------

	FString InstancesFolder(const FString& Root)
	{
		const FString Dir = NormalisedRoot(Root);
		// Its own folder by its full name (S207b): "also one for
		// MaterialInstances. good to organize contentbrowser directories".
		return Dir.IsEmpty() ? FString() : Dir + TEXT("/MaterialInstances");
	}

	int32 TidyInstanceFolders(const FString& Root, FString& OutError)
	{
		OutError.Reset();
		const FString Dir = NormalisedRoot(Root);
		if (Dir.IsEmpty())
		{
			OutError = TEXT("no library root");
			return 0;
		}
		const FString Wanted = InstancesFolder(Root);
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetRenameData> Moves;
		for (const FString& Old : { Dir + TEXT("/MI"), Dir + TEXT("/Materials/Instances") })
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPath(FName(*Old), Assets, /*bRecursive*/ true);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName())
				{
					continue;
				}
				UObject* Object = Asset.GetAsset();
				if (!Object)
				{
					continue;
				}
				// The same name already in place: left where it is, said.
				const FString Target = Wanted / Asset.AssetName.ToString();
				if (FPackageName::DoesPackageExist(Target))
				{
					OutError += FString::Printf(TEXT("%s already in %s; "), *Asset.AssetName.ToString(), *Wanted);
					continue;
				}
				Moves.Add(FAssetRenameData(Object, Wanted, Asset.AssetName.ToString()));
			}
		}
		if (Moves.Num() == 0)
		{
			return 0;
		}
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		if (!Tools.RenameAssets(Moves))
		{
			OutError += TEXT("some could not be moved; ");
		}
		// The redirectors the moves left behind: the references pointed at
		// the new place, the redirectors gone.
		TArray<UObjectRedirector*> Redirectors;
		for (const FString& Old : { Dir + TEXT("/MI"), Dir + TEXT("/Materials/Instances") })
		{
			TArray<FAssetData> Left;
			Registry.GetAssetsByPath(FName(*Old), Left, /*bRecursive*/ true);
			for (const FAssetData& Asset : Left)
			{
				if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
				{
					Redirectors.Add(Redirector);
				}
			}
		}
		if (Redirectors.Num() > 0)
		{
			Tools.FixupReferencers(Redirectors, /*bCheckoutDialogPrompt*/ false);
		}
		return Moves.Num();
	}

	FString MastersFolder(const FString& Root)
	{
		const FString Dir = NormalisedRoot(Root);
		return Dir.IsEmpty() ? FString() : Dir + TEXT("/Materials");
	}

	FString InstanceBaseName(const UMaterialInterface* Parent)
	{
		FString Name = Parent ? Parent->GetName() : FString(TEXT("Material"));
		if (!Name.RemoveFromStart(TEXT("MI_")))
		{
			Name.RemoveFromStart(TEXT("M_"));
		}
		return TEXT("MI_") + Name;
	}

	FString UniqueAssetName(const FString& Folder, const FString& Base)
	{
		const FString Dir = NormalisedRoot(Folder);
		IAssetRegistry& Registry = LibRegistry();

		// Taken in memory, on disk, or known to the registry: any of the three.
		auto Taken = [&Dir, &Registry](const FString& Name)
		{
			const FString Package = Dir / Name;
			if (FindPackage(nullptr, *Package) != nullptr)
			{
				return true;
			}
			if (FPackageName::DoesPackageExist(Package))
			{
				return true;
			}
			return Registry.GetAssetByObjectPath(FSoftObjectPath(Package + TEXT(".") + Name)).IsValid();
		};

		if (!Taken(Base))
		{
			return Base;
		}
		for (int32 Number = 2; Number < 1000; ++Number)
		{
			const FString Candidate = FString::Printf(TEXT("%s_%02d"), *Base, Number);
			if (!Taken(Candidate))
			{
				return Candidate;
			}
		}
		return Base + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Short);
	}

	namespace
	{
		UMaterialInstanceConstant* FinishInstance(UMaterialInstanceConstant* Instance, bool bSave, FString& OutError, const TCHAR* Verb)
		{
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Instance);
			if (bSave && !SurfMaterialSeed::SaveAsset(Instance))
			{
				OutError = FString::Printf(TEXT("%s: %s was made but could not be saved"), Verb, *Instance->GetName());
			}
			return Instance;
		}
	}

	UMaterialInstanceConstant* NewInstance(UMaterialInterface* Parent, const FString& Folder, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Parent)
		{
			OutError = TEXT("new instance: pick a material or an instance first");
			return nullptr;
		}

		const FString Dir = NormalisedRoot(Folder);
		if (Dir.IsEmpty())
		{
			OutError = TEXT("new instance: no library root");
			return nullptr;
		}

		return NewInstanceNamed(Parent, Folder, InstanceBaseName(Parent), bSave, OutError);
	}

	UMaterialInstanceConstant* NewInstanceNamed(UMaterialInterface* Parent, const FString& Folder, const FString& AskedName, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Parent)
		{
			OutError = TEXT("new instance: pick a material or an instance first");
			return nullptr;
		}
		const FString Dir = NormalisedRoot(Folder);
		if (Dir.IsEmpty())
		{
			OutError = TEXT("new instance: no library root");
			return nullptr;
		}
		FString Base = AskedName.TrimStartAndEnd();
		if (Base.IsEmpty())
		{
			Base = InstanceBaseName(Parent);
		}
		FText Reason;
		if (!FName::IsValidXName(Base, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS, &Reason))
		{
			OutError = TEXT("new instance: ") + Reason.ToString();
			return nullptr;
		}

		const FString Name = UniqueAssetName(Dir, Base);
		UPackage* Package = CreatePackage(*(Dir / Name));
		if (!Package)
		{
			OutError = FString::Printf(TEXT("new instance: could not make %s"), *(Dir / Name));
			return nullptr;
		}

		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		Instance->SetParentEditorOnly(Parent);

		// The instance surfaces as its parent does until told otherwise.
		if (const USurfMaterialInfo* ParentInfo = USurfMaterialInfo::Find(Parent))
		{
			if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Instance))
			{
				Info->DefaultScale = ParentInfo->DefaultScale;
				Info->Use = ParentInfo->Use;
				Info->TrimCellsPerTile = ParentInfo->TrimCellsPerTile;
			}
		}

		SurfSession::Verb(TEXT("materials_new_instance"), FString::Printf(TEXT("name=%s parent=%s"), *Name, *Parent->GetName()));
		return FinishInstance(Instance, bSave, OutError, TEXT("new instance"));
	}

	UMaterialInstanceConstant* CopyInstance(UMaterialInstanceConstant* Source, const FString& Folder, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Source)
		{
			OutError = TEXT("copy: pick an instance first");
			return nullptr;
		}

		const FString Dir = NormalisedRoot(Folder);
		if (Dir.IsEmpty())
		{
			OutError = TEXT("copy: no library root");
			return nullptr;
		}

		const FString Name = UniqueAssetName(Dir, Source->GetName());
		UPackage* Package = CreatePackage(*(Dir / Name));
		if (!Package)
		{
			OutError = FString::Printf(TEXT("copy: could not make %s"), *(Dir / Name));
			return nullptr;
		}

		UMaterialInstanceConstant* Copy = Cast<UMaterialInstanceConstant>(StaticDuplicateObject(Source, Package, FName(*Name)));
		if (!Copy)
		{
			OutError = FString::Printf(TEXT("copy: could not copy %s"), *Source->GetName());
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);

		// A copy is the user's own, whatever the source was seeded from.
		Copy->RemoveUserDataOfClass(USurfMaterialStamp::StaticClass());

		SurfSession::Verb(TEXT("materials_copy_instance"), FString::Printf(TEXT("name=%s from=%s"), *Name, *Source->GetName()));
		return FinishInstance(Copy, bSave, OutError, TEXT("copy"));
	}

	UMaterial* CopyMaster(UMaterial* Source, const FString& Folder, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Source)
		{
			OutError = TEXT("new parent: pick a parent first");
			return nullptr;
		}

		const FString Dir = NormalisedRoot(Folder);
		if (Dir.IsEmpty())
		{
			OutError = TEXT("new parent: no library root");
			return nullptr;
		}

		const FString Name = UniqueAssetName(Dir, Source->GetName());
		UPackage* Package = CreatePackage(*(Dir / Name));
		if (!Package)
		{
			OutError = FString::Printf(TEXT("new parent: could not make %s"), *(Dir / Name));
			return nullptr;
		}

		UMaterial* Copy = Cast<UMaterial>(StaticDuplicateObject(Source, Package, FName(*Name)));
		if (!Copy)
		{
			OutError = FString::Printf(TEXT("new parent: could not copy %s"), *Source->GetName());
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);

		// The user's own: no stamp, so no update of the seeded parents reaches it.
		Copy->RemoveUserDataOfClass(USurfMaterialStamp::StaticClass());
		Copy->PostEditChange();
		Copy->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Copy);
		if (bSave && !SurfMaterialSeed::SaveAsset(Copy))
		{
			OutError = FString::Printf(TEXT("new parent: %s was made but could not be saved"), *Name);
		}
		SurfSession::Verb(TEXT("materials_new_parent"), FString::Printf(TEXT("name=%s from=%s"), *Name, *Source->GetName()));
		return Copy;
	}

	// --- For the Textures window ---------------------------------------------

	static TWeakObjectPtr<UMaterialInterface> GSurfMaterialsTarget;

	UMaterialInterface* CurrentTarget()
	{
		return GSurfMaterialsTarget.Get();
	}

	void SetCurrentTarget(UMaterialInterface* Material)
	{
		GSurfMaterialsTarget = Material;
	}
}

// --- The widget -------------------------------------------------------------

namespace
{
	USurfEdMode* MaterialsWindowMode()
	{
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	/** How long the foot keeps the last result before going back to what is picked. */
	constexpr double MaterialsNoticeSeconds = 5.0;

	/** The registry announces every asset as it scans; one read per this many seconds is plenty. */
	constexpr double MaterialsReadEverySeconds = 0.5;

	TSharedRef<SWidget> MaterialsButton(const FText& Label, const FText& Tip, TFunction<void()> Run,
		TAttribute<bool> Enabled = true, TAttribute<EVisibility> Shown = EVisibility::Visible)
	{
		return SNew(SButton)
			.ButtonStyle(&SurfTheme::ToolButton())
			.Text(Label)
			.ToolTipText(Tip)
			.IsEnabled(Enabled)
			.Visibility(Shown)
			.OnClicked_Lambda([Run]() { Run(); return FReply::Handled(); });
	}
}

FReply SSurfMaterialsView::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
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

void SSurfMaterialsView::Construct(const FArguments& InArgs)
{
	SetCanTick(true);
	bInPanel = InArgs._InPanel;

	// The info a material carries (metres per tile, its use, the trim grid),
	// shown and edited in place.
	FPropertyEditorModule& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs InfoArgs;
	InfoArgs.bHideSelectionTip = true;
	InfoArgs.bAllowSearch = false;
	InfoArgs.bShowOptions = false;
	InfoArgs.bShowScrollBar = false;
	InfoArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	InfoView = Properties.CreateDetailView(InfoArgs);
	InfoView->OnFinishedChangingProperties().AddSP(this, &SSurfMaterialsView::OnInfoChanged);
	if (GConfig)
	{
		GConfig->GetBool(TEXT("SurfacingPro.Materials"), TEXT("Pictures"), bPictures, GEditorPerProjectIni);
		GConfig->GetBool(TEXT("SurfacingPro.Materials"), TEXT("Small"), bSmall, GEditorPerProjectIni);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// Two menus and the search (S197a): "the icons in surfacing materials
		// window are unclear. perhaps menu(s) better here".
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 2.0f, 4.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SurfWindows::MenuBarPullDown(LOCTEXT("MatMenuMaterial", "Material"),
					LOCTEXT("MatMenuMaterialTip", "New instance, copy, the masters, the Material Manager and the Textures window."),
					[this]() { return MakeMaterialMenu(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SurfWindows::MenuBarPullDown(LOCTEXT("MatMenuView", "View"),
					LOCTEXT("MatMenuViewTip", "Pictures beside the instances; small rows."),
					[this]() { return MakeViewOptionsMenu(); })
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search the library"))
				.OnTextChanged(this, &SSurfMaterialsView::OnQueryChanged)
				.OnTextCommitted(this, &SSurfMaterialsView::OnQueryCommitted)
			]
		]

		// The tree.
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 2.0f)
		[
			SNew(SBox)
			.HeightOverride(bInPanel ? FOptionalSize(360.0f) : FOptionalSize())
			[
			SAssignNew(Tree, STreeView<FNodePtr>)
			.TreeItemsSource(&Roots)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SSurfMaterialsView::MakeRow)
			.OnGetChildren(this, &SSurfMaterialsView::ChildrenOf)
			.OnMouseButtonClick(this, &SSurfMaterialsView::OnRowClicked)
			.OnSelectionChanged(this, &SSurfMaterialsView::OnRowSelected)
			.OnContextMenuOpening(this, &SSurfMaterialsView::OpenRowMenu)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f)
		[
			SNew(STextBlock)
			.Text(this, &SSurfMaterialsView::EmptyText)
			.Visibility(this, &SSurfMaterialsView::EmptyVisibility)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		// The selected material's surfacing info.
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f, 8.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SSurfMaterialsView::InfoVisibility)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SSurfMaterialsView::InfoHeaderText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				MaterialsButton(LOCTEXT("AddInfo", "Add surfacing info"),
					LOCTEXT("AddInfoTip", "Give this material its own metres per tile, use and trim grid, applied wherever it is put."),
					[this]() { AddInfoToSelected(); },
					true,
					TAttribute<EVisibility>::CreateSP(this, &SSurfMaterialsView::AddInfoVisibility))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f)
		[
			SNew(SBox)
			.Visibility(this, &SSurfMaterialsView::InfoVisibility)
			[
				InfoView.ToSharedRef()
			]
		]

		// The foot: the last result, for a moment; nothing otherwise.
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SSurfMaterialsView::FootText)
			.Visibility(this, &SSurfMaterialsView::FootVisibility)
			.AutoWrapText(true)
		]
	];

	// The library is read again when the registry changes under the window -
	// a material made, deleted or renamed in the Content Browser shows up
	// without a button to press.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.OnAssetAdded().AddSP(this, &SSurfMaterialsView::OnAssetChanged);
	Registry.OnAssetRemoved().AddSP(this, &SSurfMaterialsView::OnAssetChanged);
	Registry.OnAssetRenamed().AddSP(this, &SSurfMaterialsView::OnAssetRenamed);
	Registry.OnFilesLoaded().AddSP(this, &SSurfMaterialsView::MarkDirty);
}

USurfEdMode* SSurfMaterialsView::LiveMode() const
{
	return MaterialsWindowMode();
}

void SSurfMaterialsView::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	const FString Root = GetDefault<USurfSettings>()->MaterialLibraryRoot;
	const double Now = FPlatformTime::Seconds();
	const bool bRootChanged = Root != RootRead;
	const bool bStillLoading = State == SurfMaterialsLib::ERootState::Loading;
	if (bRootChanged || ((bDirty || bStillLoading) && Now - LastRead > MaterialsReadEverySeconds))
	{
		Refresh();
	}

	// A picture waited on a compile: ask again once it has had a moment.
	if (PicturesPendingAt >= 0.0 && Now - PicturesPendingAt > 2.0)
	{
		PicturesPendingAt = -1.0;
		bDirty = true;
	}

	if (Now - WornCheckedAt > 0.25)
	{
		WornCheckedAt = Now;
		FollowSelection();
	}

	// The info panel follows the asset: user data added or removed elsewhere
	// shows here without a click.
	if (InfoMaterial.IsValid())
	{
		USurfMaterialInfo* Current = Cast<USurfMaterialInfo>(InfoMaterial->GetAssetUserDataOfClass(USurfMaterialInfo::StaticClass()));
		if (Current != InfoShown.Get())
		{
			InfoShown = Current;
			InfoView->SetObject(Current);
		}
	}
}

void SSurfMaterialsView::Refresh()
{
	RootRead = GetDefault<USurfSettings>()->MaterialLibraryRoot;
	State = SurfMaterialsLib::List(RootRead, Library);
	LastRead = FPlatformTime::Seconds();
	bDirty = false;
	ApplyFilter();
}

void SSurfMaterialsView::ApplyFilter()
{
	TArray<FSurfMaterialItem> Kept;
	SurfMaterialsLib::Filter(Library, Query, Kept);
	SurfMaterialsLib::BuildTree(Kept, Roots);

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

void SSurfMaterialsView::ExpandAll(const TArray<FNodePtr>& Nodes)
{
	for (const FNodePtr& Node : Nodes)
	{
		if (Node->Children.Num() > 0)
		{
			Tree->SetItemExpansion(Node, true);
			ExpandAll(Node->Children);
		}
	}
}

/**
 * The selection's materials, followed (S197a): marked blue in the list and
 * the first one selected, when the selection changes - a click here still
 * puts a material on the selection, which then wears it.
 */
void SSurfMaterialsView::FollowSelection()
{
	TArray<UMaterialInterface*> Worn;
	SurfMaterialsLib::SelectionMaterials(LiveMode(), Worn);
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
	if (Names.Num() > 0)
	{
		SelectByName(Names[0]);
	}
}

void SSurfMaterialsView::SelectByName(const FString& Name)
{
	TArray<FNodePtr> Stack = Roots;
	while (Stack.Num() > 0)
	{
		FNodePtr Node = Stack.Pop();
		if (!Node->IsGroupOnly() && Node->Label == Name)
		{
			if (Tree.IsValid())
			{
				Tree->SetSelection(Node, ESelectInfo::Direct);
				Tree->RequestScrollIntoView(Node);
			}
			Selected = Node;
			return;
		}
		Stack.Append(Node->Children);
	}
}

TSharedRef<SWidget> SSurfMaterialsView::MakeMaterialMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	Menu.AddMenuEntry(LOCTEXT("MatNewMI", "New instance"),
		LOCTEXT("NewMITip", "A new instance of the default parent (the settings say which; M_Opq_Simple to start), in the library's MaterialInstances folder, put on the selection and opened for editing."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::NewInstanceOfDefault)));
	Menu.AddWrapperSubMenu(LOCTEXT("MatNewMIOf", "New instance of"),
		LOCTEXT("NewMIMoreTip", "An instance of another parent, or a new parent of your own from one of them."),
		FOnGetContent::CreateSP(this, &SSurfMaterialsView::MakeNewMenu), FSlateIcon());
	Menu.AddMenuEntry(LOCTEXT("MatCopyMI", "Copy"),
		LOCTEXT("CopyMITip", "A copy of the selected instance beside the others, with its surfacing info, put on the selection and opened."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::CopySelected), FCanExecuteAction::CreateSP(this, &SSurfMaterialsView::HasSelectedInstance)));
	const bool bSeed = SeedVisibility() == EVisibility::Visible;
	const bool bUpdate = UpdateVisibility() == EVisibility::Visible;
	if (bSeed || bUpdate)
	{
		Menu.AddSeparator();
	}
	if (bSeed)
	{
		Menu.AddMenuEntry(LOCTEXT("MatSeed", "Seed parents"),
			LOCTEXT("SeedTip", "The plugin's masters, functions, textures and ready-made instances copied into the library root, once, so nothing made here depends on the plugin."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::SeedParents)));
	}
	Menu.AddMenuEntry(LOCTEXT("MatTidyFolders", "Tidy instance folders"),
		LOCTEXT("TidyFoldersTip", "Instances still in the older MI and Materials/Instances folders move into MaterialInstances, and whatever wore them follows. Parents stay in Materials."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::TidyFolders)));
	if (bUpdate)
	{
		Menu.AddMenuEntry(LOCTEXT("MatUpdate", "Update masters"),
			LOCTEXT("UpdateMastersTip", "The plugin's newer masters, functions and textures over the seeded copies left alone; a copy changed here is kept and the new one lands beside it. Your own materials are never touched."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::UpdateMasters)));
	}
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("MatOpenManager", "Material Manager"),
		LOCTEXT("ManagerTip", "The selected material on the calibration cube, its parameters beside it to edit."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { SurfMaterialManagerTab::Open(); })));
	Menu.AddMenuEntry(LOCTEXT("MatOpenTextures", "Textures"),
		LOCTEXT("TexturesTip", "Any folder on this machine, its images brought into the library as ours, and put on the selected instance."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { SurfTexturesTab::Open(); })));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SSurfMaterialsView::MakeViewOptionsMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ false, nullptr, nullptr, false, &SurfTheme::MenuStyle(), /*bSearchable*/ false);
	Menu.AddMenuEntry(LOCTEXT("MatPictures", "Pictures"),
		LOCTEXT("PicturesTip", "A picture beside each instance: the manager's own, rendered on the cube and kept under the source root. Off, the list is bare and quick."),
		FSlateIcon(), FUIAction(
			FExecuteAction::CreateLambda([this]() { OnPicturesToggled(bPictures ? ECheckBoxState::Unchecked : ECheckBoxState::Checked); }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this]() { return bPictures; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddMenuEntry(LOCTEXT("MatSmall", "Small rows"),
		LOCTEXT("SmallTip", "Smaller rows and type, so more of the library fits."),
		FSlateIcon(), FUIAction(
			FExecuteAction::CreateLambda([this]() { OnSmallToggled(bSmall ? ECheckBoxState::Unchecked : ECheckBoxState::Checked); }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this]() { return bSmall; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	return Menu.MakeWidget();
}

TSharedRef<ITableRow> SSurfMaterialsView::MakeRow(FNodePtr Node, const TSharedRef<STableViewBase>& Owner)
{
	const FString Name = Node.IsValid() ? Node->Label : FString();
	FString Under;
	FString Path;
	if (Node.IsValid() && Node->Item.IsValid())
	{
		const FSurfMaterialItem& Item = *Node->Item;
		Under = Item.bInstance ? FString(TEXT("instance")) : FString(TEXT("material"));
		Path = Item.Asset.PackagePath.ToString();
	}
	else
	{
		Under = TEXT("parent");
		Path = TEXT("the instances beneath were made from this; click to fold them");
	}

	// The picture, when asked and the row is an instance: the manager's own, from its cache or rendered now.
	const FSlateBrush* Picture = nullptr;
	if (bPictures && Node.IsValid() && Node->Item.IsValid() && Node->Item->bInstance)
	{
		FString Why;
		Picture = SurfMaterialThumbnails::BrushFor(Cast<UMaterialInterface>(Node->Item->Asset.GetAsset()), Why);
		if (!Picture && !Why.IsEmpty())
		{
			Under = Why;
			if (Why.StartsWith(TEXT("compiling")) && PicturesPendingAt < 0.0)
			{
				PicturesPendingAt = FPlatformTime::Seconds();
			}
		}
	}

	const float Side = bSmall ? 28.0f : 48.0f;
	const FSlateFontInfo Font = FAppStyle::Get().GetFontStyle(bSmall ? "SmallFont" : "NormalFont");
	return SNew(STableRow<FNodePtr>, Owner)
		.ToolTipText(FText::FromString(Path))
		.Padding(FMargin(4.0f, bSmall ? 1.0f : 3.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(Picture ? Side : 0.0f).HeightOverride(Picture ? Side : 0.0f)
				[
					Picture ? StaticCastSharedRef<SWidget>(SNew(SImage).Image(Picture)) : SNullWidget::NullWidget
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				// Blue when the selection wears it (S197a).
				SNew(STextBlock).Text(FText::FromString(Name)).Font(Font)
				.ColorAndOpacity_Lambda([this, Name]() { return WornNames.Contains(Name) ? SurfTheme::On() : FSlateColor::UseForeground(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Under)).Font(Font).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

ECheckBoxState SSurfMaterialsView::PicturesState() const
{
	return bPictures ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

ECheckBoxState SSurfMaterialsView::SmallState() const
{
	return bSmall ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSurfMaterialsView::OnSmallToggled(ECheckBoxState NewState)
{
	bSmall = NewState == ECheckBoxState::Checked;
	if (GConfig)
	{
		GConfig->SetBool(TEXT("SurfacingPro.Materials"), TEXT("Small"), bSmall, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	bDirty = true;
}

EVisibility SSurfMaterialsView::FootVisibility() const
{
	return !Notice.IsEmpty() && FPlatformTime::Seconds() - NoticeAt < MaterialsNoticeSeconds ? EVisibility::Visible : EVisibility::Collapsed;
}

void SSurfMaterialsView::OnPicturesToggled(ECheckBoxState NewState)
{
	bPictures = NewState == ECheckBoxState::Checked;
	if (GConfig)
	{
		GConfig->SetBool(TEXT("SurfacingPro.Materials"), TEXT("Pictures"), bPictures, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	if (Tree.IsValid())
	{
		Tree->RebuildList();
	}
}

void SSurfMaterialsView::ChildrenOf(FNodePtr Node, TArray<FNodePtr>& Out)
{
	if (Node.IsValid())
	{
		Out = Node->Children;
	}
}

void SSurfMaterialsView::OnRowClicked(FNodePtr Node)
{
	AssignNode(Node);
}

void SSurfMaterialsView::OnRowSelected(FNodePtr Node, ESelectInfo::Type How)
{
	Selected = Node;
	ShowInfoOf(SelectedMaterial());
	SurfMaterialsLib::SetCurrentTarget(SelectedMaterial());
}

TSharedRef<SWidget> SSurfMaterialsView::MakeNewMenu()
{
	// The parents to choose from: every material of its own under the root.
	// None yet: the one thing to do is seed them.
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	TArray<FSurfMaterialItem> Parents;
	SurfMaterialsLib::Masters(Library, Parents);
	if (Parents.Num() == 0)
	{
		Menu.AddMenuEntry(LOCTEXT("NewMISeed", "Seed parents first"),
			LOCTEXT("NewMISeedTip", "The library has no parent material yet: copy the plugin's into it, then choose one here."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::SeedParents)));
		return Menu.MakeWidget();
	}

	const int32 Default = SurfMaterialsLib::DefaultMasterIndex(Parents);
	Menu.BeginSection(NAME_None, LOCTEXT("NewMIOf", "An instance of"));
	for (int32 Index = 0; Index < Parents.Num(); ++Index)
	{
		const FSurfMaterialItem& Parent = Parents[Index];
		FItemPtr Held = MakeShared<FSurfMaterialItem>(Parent);
		const FString Label = Index == Default ? Parent.Name + TEXT(" (default)") : Parent.Name;
		Menu.AddMenuEntry(FText::FromString(Label), FText::FromString(Parent.Asset.GetObjectPathString()),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::NewInstanceOf, Held)));
	}
	Menu.EndSection();

	Menu.BeginSection(NAME_None, LOCTEXT("NewParentFrom", "A new parent from"));
	for (const FSurfMaterialItem& Parent : Parents)
	{
		FItemPtr Held = MakeShared<FSurfMaterialItem>(Parent);
		Menu.AddMenuEntry(FText::FromString(Parent.Name),
			FText::FromString(FString::Printf(TEXT("A copy of %s in the library's Materials folder, yours to edit, with a first instance of it on the selection."), *Parent.Name)),
			FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::NewParentFrom, Held)));
	}
	Menu.EndSection();
	return Menu.MakeWidget();
}

TSharedPtr<SWidget> SSurfMaterialsView::OpenRowMenu()
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("MenuNewMI", "New instance beside"),
		LOCTEXT("MenuNewMITip", "A new instance of the same parent as this one, in the library's MaterialInstances folder, put on the selection and opened."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::NewInstanceBesideSelected),
			FCanExecuteAction::CreateSP(this, &SSurfMaterialsView::HasSelectedInstance)));
	Menu.AddMenuEntry(LOCTEXT("MenuCopy", "Copy"),
		LOCTEXT("MenuCopyTip", "A copy of this instance beside the others, put on the selection and opened."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::CopySelected),
			FCanExecuteAction::CreateSP(this, &SSurfMaterialsView::HasSelectedInstance)));
	Menu.AddMenuEntry(LOCTEXT("MenuOpen", "Open in the asset editor"),
		LOCTEXT("MenuOpenTip", "Open this material in the asset editor."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::OpenSelected),
			FCanExecuteAction::CreateSP(this, &SSurfMaterialsView::HasSelectedMaterial)));
	// "+option in mat library to open in matmanager?" (S251a)
	Menu.AddMenuEntry(LOCTEXT("MenuInManager", "Open in the Material Manager"),
		LOCTEXT("MenuInManagerTip", "The Material Manager, showing this material with its parameters to edit."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
			{
				if (UMaterialInterface* Material = SelectedMaterial())
				{
					SurfMaterialManagerTab::OpenWith(Material);
				}
			}),
			FCanExecuteAction::CreateSP(this, &SSurfMaterialsView::HasSelectedMaterial)));
	Menu.AddMenuEntry(LOCTEXT("MenuPicturesAgain", "Render pictures again"),
		LOCTEXT("MenuPicturesAgainTip", "Every picture rendered afresh - for one taken while its material was still compiling."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SSurfMaterialsView::RenderPicturesAgain)));
	return Menu.MakeWidget();
}

void SSurfMaterialsView::OnQueryChanged(const FText& Text)
{
	Query = Text.ToString();
	ApplyFilter();
}

void SSurfMaterialsView::OnQueryCommitted(const FText& Text, ETextCommit::Type Commit)
{
	// Enter takes the first material the search left, so a name typed whole
	// needs no click.
	Query = Text.ToString();
	ApplyFilter();
	if (Commit == ETextCommit::OnEnter && Roots.Num() > 0)
	{
		FNodePtr First = Roots[0];
		while (First.IsValid() && First->IsGroupOnly() && First->Children.Num() > 0)
		{
			First = First->Children[0];
		}
		AssignNode(First);
	}
}

void SSurfMaterialsView::Say(const FString& Line)
{
	Notice = Line;
	NoticeAt = FPlatformTime::Seconds();
}

void SSurfMaterialsView::AssignNode(FNodePtr Node)
{
	if (!Node.IsValid())
	{
		return;
	}
	if (Node->IsGroupOnly())
	{
		// A heading folds and unfolds; the parents are not assigned (D68).
		if (Tree.IsValid())
		{
			Tree->SetItemExpansion(Node, !Tree->IsItemExpanded(Node));
		}
		return;
	}

	USurfEdMode* Mode = LiveMode();
	if (!Mode || !Mode->Inspector)
	{
		Say(TEXT("material: the mode is not up"));
		SurfSession::Friction(TEXT("materials_window_no_mode"));
		return;
	}

	// Loading here, in a click, is one of the two places a library material
	// is loaded by the window (the other is selecting a row for its info):
	// the list itself never loads anything.
	UMaterialInterface* Material = Cast<UMaterialInterface>(Node->Item->Asset.GetAsset());
	if (!Material)
	{
		Say(FString::Printf(TEXT("material: %s could not be loaded"), *Node->Label));
		SurfSession::Friction(TEXT("materials_window_load"), Node->Item->Asset.GetObjectPathString());
		return;
	}

	SurfMaterialsLib::Assign(*Mode, Material);
	Say(Mode->Inspector->LastResult);
}

bool SSurfMaterialsView::HasSelectedMaterial() const
{
	return Selected.IsValid() && !Selected->IsGroupOnly();
}

bool SSurfMaterialsView::HasSelectedInstance() const
{
	return HasSelectedMaterial() && Selected->Item->bInstance;
}

bool SSurfMaterialsView::NeedsSeeding() const
{
	if (State == SurfMaterialsLib::ERootState::Unset || State == SurfMaterialsLib::ERootState::Loading)
	{
		return false;
	}
	for (const FSurfMaterialItem& Item : Library)
	{
		if (!Item.bInstance)
		{
			return false;
		}
	}
	return true;
}

UMaterialInterface* SSurfMaterialsView::SelectedMaterial() const
{
	if (!HasSelectedMaterial())
	{
		return nullptr;
	}
	return Cast<UMaterialInterface>(Selected->Item->Asset.GetAsset());
}

void SSurfMaterialsView::ShowInfoOf(UMaterialInterface* Material)
{
	InfoMaterial = Material;
	InfoShown = Material ? Cast<USurfMaterialInfo>(Material->GetAssetUserDataOfClass(USurfMaterialInfo::StaticClass())) : nullptr;
	if (InfoView.IsValid())
	{
		InfoView->SetObject(InfoShown.Get());
	}
}

void SSurfMaterialsView::OnInfoChanged(const FPropertyChangedEvent& Event)
{
	// A value set here is meant to stay: the material is saved at once.
	UMaterialInterface* Material = InfoMaterial.Get();
	if (!Material)
	{
		return;
	}
	Material->MarkPackageDirty();
	if (SurfMaterialSeed::SaveAsset(Material))
	{
		Say(FString::Printf(TEXT("saved the surfacing info of %s"), *Material->GetName()));
	}
	else
	{
		Say(FString::Printf(TEXT("info: %s could not be saved"), *Material->GetName()));
	}
	SurfSession::Verb(TEXT("materials_info"), FString::Printf(TEXT("name=%s"), *Material->GetName()));
}

void SSurfMaterialsView::AddInfoToSelected()
{
	UMaterialInterface* Material = SelectedMaterial();
	if (!Material)
	{
		Say(TEXT("info: pick a material first"));
		return;
	}
	USurfMaterialInfo::FindOrAdd(Material);
	Material->MarkPackageDirty();
	ShowInfoOf(Material);
	if (SurfMaterialSeed::SaveAsset(Material))
	{
		Say(FString::Printf(TEXT("added surfacing info to %s"), *Material->GetName()));
	}
	else
	{
		Say(FString::Printf(TEXT("info: %s could not be saved"), *Material->GetName()));
	}
}

void SSurfMaterialsView::NewInstanceOf(FItemPtr Master)
{
	UMaterialInterface* Parent = Master.IsValid() ? Cast<UMaterialInterface>(Master->Asset.GetAsset()) : nullptr;
	if (!Parent)
	{
		Say(FString::Printf(TEXT("new instance: %s could not be loaded"), Master.IsValid() ? *Master->Name : TEXT("the parent")));
		return;
	}
	MakeInstance(Parent);
}

void SSurfMaterialsView::NewInstanceBesideSelected()
{
	const UMaterialInstance* Instance = Cast<UMaterialInstance>(SelectedMaterial());
	MakeInstance(Instance ? Instance->Parent.Get() : nullptr);
}

void SSurfMaterialsView::NewInstanceOfDefault()
{
	TArray<FSurfMaterialItem> Parents;
	SurfMaterialsLib::Masters(Library, Parents);
	const int32 Index = SurfMaterialsLib::DefaultMasterIndex(Parents);
	if (Index == INDEX_NONE)
	{
		Say(TEXT("new instance: no parent in the library yet - press Seed parents"));
		return;
	}
	NewInstanceOf(MakeShared<FSurfMaterialItem>(Parents[Index]));
}

void SSurfMaterialsView::NewParentFrom(FItemPtr Master)
{
	UMaterial* Source = Master.IsValid() ? Cast<UMaterial>(Master->Asset.GetAsset()) : nullptr;
	if (!Source)
	{
		Say(FString::Printf(TEXT("new parent: %s could not be loaded"), Master.IsValid() ? *Master->Name : TEXT("the parent")));
		return;
	}
	FString Error;
	UMaterial* Parent = SurfMaterialsLib::CopyMaster(Source, SurfMaterialsLib::MastersFolder(RootRead), /*bSave*/ true, Error);
	if (!Parent)
	{
		Say(Error);
		return;
	}

	// Its first instance, so the parent has a place in the tree and the
	// selection wears it; the parent is what opens, being the thing just made.
	if (UMaterialInstanceConstant* Made = MakeInstance(Parent, Parent))
	{
		Say(Error.IsEmpty() ? FString::Printf(TEXT("made %s from %s, and %s of it on the selection"), *Parent->GetName(), *Source->GetName(), *Made->GetName()) : Error);
	}
}

UMaterialInstanceConstant* SSurfMaterialsView::MakeInstance(UMaterialInterface* Parent, UObject* ToOpen)
{
	FString Error;
	UMaterialInstanceConstant* Made = SurfMaterialsLib::NewInstance(Parent, SurfMaterialsLib::InstancesFolder(RootRead), /*bSave*/ true, Error);
	if (!Made)
	{
		Say(Error);
		return nullptr;
	}

	FString Line = FString::Printf(TEXT("made %s in %s"), *Made->GetName(), *SurfMaterialsLib::InstancesFolder(RootRead));
	if (USurfEdMode* Mode = LiveMode())
	{
		if (SurfMaterialsLib::Assign(*Mode, Made))
		{
			Line = Mode->Inspector->LastResult;
		}
	}
	Say(Error.IsEmpty() ? Line : Error);
	bDirty = true;

	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(ToOpen ? ToOpen : Made);
	}
	return Made;
}

void SSurfMaterialsView::CopySelected()
{
	UMaterialInstanceConstant* Source = Cast<UMaterialInstanceConstant>(SelectedMaterial());
	FString Error;
	UMaterialInstanceConstant* Made = SurfMaterialsLib::CopyInstance(Source, SurfMaterialsLib::InstancesFolder(RootRead), /*bSave*/ true, Error);
	if (!Made)
	{
		Say(Error);
		return;
	}

	FString Line = FString::Printf(TEXT("copied %s to %s"), *Source->GetName(), *Made->GetName());
	if (USurfEdMode* Mode = LiveMode())
	{
		if (SurfMaterialsLib::Assign(*Mode, Made))
		{
			Line = Mode->Inspector->LastResult;
		}
	}
	Say(Error.IsEmpty() ? Line : Error);
	bDirty = true;

	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Made);
	}
}

void SSurfMaterialsView::OpenSelected()
{
	UMaterialInterface* Material = SelectedMaterial();
	if (Material && GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Material);
	}
}

void SSurfMaterialsView::RenderPicturesAgain()
{
	const int32 Gone = SurfMaterialThumbnails::ForgetFiles();
	Say(FString::Printf(TEXT("%d picture%s dropped - rendered again as the list is read"), Gone, Gone == 1 ? TEXT("") : TEXT("s")));
	bDirty = true;
}

void SSurfMaterialsView::TidyFolders()
{
	FString Error;
	const int32 Moved = SurfMaterialsLib::TidyInstanceFolders(RootRead, Error);
	FString Line = FString::Printf(TEXT("%d instance%s moved into %s"), Moved, Moved == 1 ? TEXT("") : TEXT("s"), *SurfMaterialsLib::InstancesFolder(RootRead));
	if (!Error.IsEmpty())
	{
		Line += TEXT(" - ") + Error;
	}
	Say(Line);
	SurfSession::Verb(TEXT("tidy_instances"), FString::Printf(TEXT("moved=%d"), Moved));
	bDirty = true;
}

void SSurfMaterialsView::SeedParents()
{
	const SurfMaterialSeed::FSurfSeedResult Result = SurfMaterialSeed::SeedParents(RootRead, /*bSave*/ true);
	if (Result.Failed.Num() > 0 && Result.Copied == 0)
	{
		Say(TEXT("seed: ") + Result.Failed[0]);
	}
	else
	{
		FString Line = FString::Printf(TEXT("seeded %d parent%s into %s/Materials"), Result.Copied, Result.Copied == 1 ? TEXT("") : TEXT("s"),
			*SurfMaterialsLib::NormalisedRoot(RootRead));
		if (Result.Skipped > 0)
		{
			Line += FString::Printf(TEXT(", %d already there"), Result.Skipped);
		}
		if (Result.Failed.Num() > 0)
		{
			Line += FString::Printf(TEXT(", %d failed"), Result.Failed.Num());
		}
		Say(Line);
	}
	bDirty = true;
}

void SSurfMaterialsView::UpdateMasters()
{
	TArray<SurfMaterialSeed::FSurfMasterState> States;
	SurfMaterialSeed::CheckMasters(RootRead, States);
	int32 Changed = 0;
	for (const SurfMaterialSeed::FSurfMasterState& Standing : States)
	{
		if (Standing.bNewer && Standing.bEdited)
		{
			++Changed;
		}
	}

	bool bReparent = false;
	if (Changed > 0)
	{
		const FText Question = FText::Format(LOCTEXT("UpdateReparentAsk",
			"{0} seeded {0}|plural(one=master was,other=masters were) changed here and the plugin has a newer one. Each is kept and the new one lands beside it.\n\nMove their instances to the new ones?"), Changed);
		const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel, Question);
		if (Answer == EAppReturnType::Cancel)
		{
			return;
		}
		bReparent = Answer == EAppReturnType::Yes;
	}

	const SurfMaterialSeed::FSurfUpdateResult Result = SurfMaterialSeed::UpdateMasters(RootRead, /*bSave*/ true, bReparent);
	Say(Result.Line());
	bDirty = true;
}

void SSurfMaterialsView::OnAssetChanged(const FAssetData& Asset)
{
	bDirty = true;
}

void SSurfMaterialsView::OnAssetRenamed(const FAssetData& Asset, const FString& OldPath)
{
	bDirty = true;
}

void SSurfMaterialsView::MarkDirty()
{
	bDirty = true;
}

FText SSurfMaterialsView::FootText() const
{
	if (!Notice.IsEmpty() && FPlatformTime::Seconds() - NoticeAt < MaterialsNoticeSeconds)
	{
		return FText::FromString(Notice);
	}
	return FText();
}

FText SSurfMaterialsView::EmptyText() const
{
	if (State == SurfMaterialsLib::ERootState::Listed)
	{
		return Roots.Num() == 0
			? FText::FromString(FString::Printf(TEXT("nothing in the library matches \"%s\""), *Query))
			: FText::GetEmpty();
	}
	return FText::FromString(SurfMaterialsLib::EmptyLine(State, RootRead));
}

EVisibility SSurfMaterialsView::EmptyVisibility() const
{
	return Roots.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SSurfMaterialsView::InfoHeaderText() const
{
	const UMaterialInterface* Material = InfoMaterial.Get();
	if (!Material)
	{
		return FText::GetEmpty();
	}
	return FText::FromString(InfoShown.IsValid()
		? FString::Printf(TEXT("surfacing of %s"), *Material->GetName())
		: FString::Printf(TEXT("%s carries no surfacing info: a metre a tile, opaque"), *Material->GetName()));
}

EVisibility SSurfMaterialsView::InfoVisibility() const
{
	return InfoMaterial.IsValid() && !bInPanel ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SSurfMaterialsView::AddInfoVisibility() const
{
	return InfoMaterial.IsValid() && !InfoShown.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SSurfMaterialsView::SeedVisibility() const
{
	return NeedsSeeding() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SSurfMaterialsView::UpdateVisibility() const
{
	const bool bSeeded = (State == SurfMaterialsLib::ERootState::Listed) && !NeedsSeeding();
	return bSeeded ? EVisibility::Visible : EVisibility::Collapsed;
}

// --- The tab ----------------------------------------------------------------

namespace SurfMaterialsTab
{
	const FName TabName("SurfacingProMaterials");

	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("MaterialsTabLabel", "Material Library"))
			[
				SNew(SSurfMaterialsView)
			];
	}

	void Register()
	{
		if (!FSlateApplication::IsInitialized() || FGlobalTabmanager::Get()->HasTabSpawner(TabName))
		{
			return;
		}

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&Spawn))
			.SetDisplayName(LOCTEXT("MaterialsTabTitle", "SurfacingPro Material Library"))
			.SetTooltipText(LOCTEXT("MaterialsTabTip", "The project's materials, with a search; click one to put it on the selection."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory())
			.SetIcon(FSlateIcon(TEXT("SurfacingProStyle"), "SurfacingPro.Verb.sphere"));
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
		SurfSession::Verb(TEXT("materials_window"), TEXT("open"));
	}

	TSharedRef<SWidget> MakePanel()
	{
		return SNew(SSurfMaterialsView).InPanel(true);
	}
}

#undef LOCTEXT_NAMESPACE
