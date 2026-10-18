// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialSeed.h"

#include "SurfMaterialInfo.h"
#include "SurfMaterialsView.h"
#include "SurfSession.h"
#include "SurfacingProModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace SurfMaterialSeed
{
	namespace
	{
		IAssetRegistry& SeedRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		FString SeedPluginVersion()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
			return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
		}
	}

	bool IsLibraryAsset(const FAssetData& Asset)
	{
		const UClass* Class = Asset.GetClass();
		if (!Class || Class->IsChildOf(UObjectRedirector::StaticClass()))
		{
			return false;
		}
		return Class->IsChildOf(UMaterialInterface::StaticClass())
			|| Class->IsChildOf(UMaterialFunctionInterface::StaticClass())
			|| Class->IsChildOf(UMaterialParameterCollection::StaticClass())
			|| Class->IsChildOf(UTexture::StaticClass());
	}

	void Plan(const FString& SourceRoot, const FString& DestRoot, TArray<FSurfSeedEntry>& Out,
		TFunctionRef<bool(const FAssetData& Asset, FString& Relative)> Keep)
	{
		Out.Reset();

		const FString Source = SurfMaterialsLib::NormalisedRoot(SourceRoot);
		const FString Dest = SurfMaterialsLib::NormalisedRoot(DestRoot);
		if (Source.IsEmpty() || Dest.IsEmpty())
		{
			return;
		}

		TArray<FAssetData> Assets;
		SeedRegistry().GetAssetsByPath(FName(*Source), Assets, /*bRecursive*/ true);

		for (const FAssetData& Asset : Assets)
		{
			if (!IsLibraryAsset(Asset))
			{
				continue;
			}

			// The package's path under the root: "Functions/Opq_MAT_01".
			FString Relative = Asset.PackageName.ToString();
			if (!Relative.RemoveFromStart(Source + TEXT("/")))
			{
				continue;
			}
			if (!Keep(Asset, Relative))
			{
				continue;
			}

			FSurfSeedEntry& Entry = Out.AddDefaulted_GetRef();
			Entry.Source = Asset;
			Entry.DestPackage = Dest + TEXT("/") + Relative;
			// The plugin's ready-made instances land beside the masters'
			// folder, not under it (S208a): "keep MaterialInstances, and not
			// under /Materials/. so parent and child are distinct".
			FString Rest;
			if (Dest.EndsWith(TEXT("/Materials")) && Relative.Split(TEXT("Instances/"), nullptr, &Rest) && Relative.StartsWith(TEXT("Instances/")))
			{
				Entry.DestPackage = Dest.LeftChop(FString(TEXT("/Materials")).Len()) + TEXT("/MaterialInstances/") + Rest;
			}
		}

		Out.Sort([](const FSurfSeedEntry& A, const FSurfSeedEntry& B)
		{
			return A.DestPackage.Compare(B.DestPackage, ESearchCase::IgnoreCase) < 0;
		});
	}

	void Plan(const FString& SourceRoot, const FString& DestRoot, TArray<FSurfSeedEntry>& Out)
	{
		Plan(SourceRoot, DestRoot, Out, [](const FAssetData&, FString&) { return true; });
	}

	void Stamp(UObject* Copy, const FString& SourcePackage)
	{
		// The content first: the stamp itself is left out of it either way,
		// but it must be the settled copy that is measured.
		const uint32 Content = ContentOf(Copy);

		USurfMaterialStamp* Made = USurfMaterialStamp::FindOrAdd(Copy);
		if (!Made)
		{
			return;
		}

		Made->Source = SourcePackage;
		Made->Version = SeedPluginVersion();
		Made->Content = Content;

		FString SourceFile;
		Made->Hash.Reset();
		if (FPackageName::DoesPackageExist(SourcePackage, &SourceFile))
		{
			const FMD5Hash FileHash = FMD5Hash::HashFile(*SourceFile);
			if (FileHash.IsValid())
			{
				Made->Hash = LexToString(FileHash);
			}
		}
	}

	FSurfSeedResult Copy(const TArray<FSurfSeedEntry>& Plan, const FSurfSeedOptions& Options)
	{
		FSurfSeedResult Result;

		// Source -> copy, for every asset in the plan, so that pass two can
		// turn a reference to a source into a reference to its copy.
		TMap<UObject*, UObject*> Replace;
		TArray<UObject*> Copies;
		TArray<FString> CopiedFrom;

		// Pass one: load and duplicate. A copy already in place is kept (and
		// still stands in for its source) unless the caller says to replace it.
		for (const FSurfSeedEntry& Entry : Plan)
		{
			const FString SourcePackage = Entry.Source.PackageName.ToString();
			UObject* Source = Entry.Source.GetAsset();
			if (!Source)
			{
				Result.Failed.Add(SourcePackage + TEXT(": could not load"));
				continue;
			}

			const FString Name = Entry.DestName.IsEmpty() ? Entry.Source.AssetName.ToString() : Entry.DestName;
			const FString DestPath = Entry.DestPackage + TEXT(".") + Name;
			UObject* Existing = FindObject<UObject>(nullptr, *DestPath);
			if (!Existing && FPackageName::DoesPackageExist(Entry.DestPackage))
			{
				Existing = LoadObject<UObject>(nullptr, *DestPath);
			}
			if (Existing && (!Options.bOverwrite || Entry.bKeep))
			{
				++Result.Skipped;
				Replace.Add(Source, Existing);
				Result.Copies.Add(SourcePackage, Existing);
				continue;
			}

			UPackage* Package = CreatePackage(*Entry.DestPackage);
			if (!Package)
			{
				Result.Failed.Add(Entry.DestPackage + TEXT(": could not make the package"));
				continue;
			}
			Package->FullyLoad();

			UObject* Duplicate = StaticDuplicateObject(Source, Package, FName(*Name));
			if (!Duplicate)
			{
				Result.Failed.Add(Entry.DestPackage + TEXT(": could not duplicate"));
				continue;
			}
			Duplicate->SetFlags(RF_Public | RF_Standalone);

			Replace.Add(Source, Duplicate);
			Copies.Add(Duplicate);
			CopiedFrom.Add(SourcePackage);
			Result.Copies.Add(SourcePackage, Duplicate);
			++Result.Copied;
		}

		// Pass two: inside every copy, and everything it owns (a material's
		// expressions, an instance's parameter values), a reference to a
		// source becomes a reference to its copy.
		for (UObject* Duplicate : Copies)
		{
			FArchiveReplaceObjectRef<UObject> Replacer(Duplicate, Replace,
				EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		}

		// Pass three: let each copy settle on its new references, stamp it,
		// tell the registry, save.
		for (int32 Index = 0; Index < Copies.Num(); ++Index)
		{
			UObject* Duplicate = Copies[Index];

			// A material's texture streaming record names its textures by
			// soft path - the source's, not the copies'. It is derived data;
			// dropped here, the editor builds it again for the copy.
			if (UMaterialInterface* AsMaterial = Cast<UMaterialInterface>(Duplicate))
			{
				AsMaterial->SetTextureStreamingData(TArray<FMaterialTextureInfo>());
			}

			Duplicate->PostEditChange();
			if (Options.bStamp)
			{
				Stamp(Duplicate, CopiedFrom[Index]);
			}
			Duplicate->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Duplicate);

			if (Options.bSave && !SaveAsset(Duplicate))
			{
				Result.Failed.Add(Duplicate->GetOutermost()->GetName() + TEXT(": could not save"));
			}
		}

		UE_LOG(LogSurfacingPro, Display, TEXT("SP_METRIC name=seed.copied value=%d unit=assets n=1"), Result.Copied);
		for (const FString& Line : Result.Failed)
		{
			UE_LOG(LogSurfacingPro, Warning, TEXT("SP_DIAG tag=seed_failed %s"), *Line);
		}
		return Result;
	}

	bool SaveAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}
		UPackage* Package = Asset->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *File, Args);
	}

	FString PluginParentsRoot()
	{
		return TEXT("/SurfacingPro/Materials");
	}

	FSurfSeedResult SeedParents(const FString& LibraryRoot, bool bSave)
	{
		FSurfSeedResult Result;

		const FString Root = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
		if (Root.IsEmpty())
		{
			Result.Failed.Add(TEXT("no library root"));
			return Result;
		}

		TArray<FSurfSeedEntry> Entries;
		Plan(PluginParentsRoot(), Root + TEXT("/Materials"), Entries);
		if (Entries.Num() == 0)
		{
			Result.Failed.Add(TEXT("the plugin has no parents to seed"));
			return Result;
		}

		FSurfSeedOptions Options;
		Options.bOverwrite = false;
		Options.bSave = bSave;
		Options.bStamp = true;
		Result = Copy(Entries, Options);

		SurfSession::Verb(TEXT("materials_seed"), FString::Printf(TEXT("root=%s copied=%d skipped=%d failed=%d"),
			*Root, Result.Copied, Result.Skipped, Result.Failed.Num()));
		return Result;
	}

	// --- Update masters (M2) ----------------------------------------------------

	namespace
	{
		/**
		 * The measure, blind to what is identity rather than content: a
		 * GUID (a material's StateId, an instance's ParameterStateId, a
		 * texture's LightingGuid - remade on any PostEditChange) and what
		 * is transient or deprecated.
		 */
		class FSurfContentArchive : public FArchiveObjectCrc32
		{
		public:
			virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
			{
				if (InProperty->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated | CPF_SkipSerialization))
				{
					return true;
				}
				const FStructProperty* AsStruct = CastField<FStructProperty>(InProperty);
				return AsStruct && AsStruct->Struct == TBaseStructure<FGuid>::Get();
			}
		};
	}

	uint32 ContentOf(UObject* Asset)
	{
		if (!Asset)
		{
			return 0;
		}

		// The stamp is taken off while the asset is measured, so that stamping
		// changes nothing and one made before this was recorded measures the same.
		IInterface_AssetUserData* Holder = Cast<IInterface_AssetUserData>(Asset);
		USurfMaterialStamp* Held = Holder ? Cast<USurfMaterialStamp>(Holder->GetAssetUserDataOfClass(USurfMaterialStamp::StaticClass())) : nullptr;
		if (Held)
		{
			Holder->RemoveUserDataOfClass(USurfMaterialStamp::StaticClass());
		}

		FSurfContentArchive Archive;
		const uint32 Crc = Archive.Crc32(Asset);

		if (Held)
		{
			Holder->AddAssetUserData(Held);
		}
		return Crc;
	}

	namespace
	{
		/** The hash of a package's file as the stamp records it; empty when there is no file. */
		FString FileHashOf(const FString& PackageName)
		{
			FString File;
			if (FPackageName::DoesPackageExist(PackageName, &File))
			{
				const FMD5Hash Hash = FMD5Hash::HashFile(*File);
				if (Hash.IsValid())
				{
					return LexToString(Hash);
				}
			}
			return FString();
		}

		bool PackageIsThere(const FString& PackageName)
		{
			const FString Path = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			return FindObject<UObject>(nullptr, *Path) != nullptr
				|| SeedRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid()
				|| FPackageName::DoesPackageExist(PackageName);
		}

		/** A name beside Name in Folder that nothing has: Name_v<version>, then numbered. */
		FString BesideName(const FString& Folder, const FString& Name)
		{
			FString Version = SeedPluginVersion();
			for (TCHAR& C : Version)
			{
				if (!FChar::IsAlnum(C))
				{
					C = TEXT('_');
				}
			}
			const FString Base = Version.IsEmpty() ? Name + TEXT("_new") : Name + TEXT("_v") + Version;
			FString Candidate = Base;
			for (int32 N = 2; PackageIsThere(Folder / Candidate); ++N)
			{
				Candidate = FString::Printf(TEXT("%s_%d"), *Base, N);
			}
			return Candidate;
		}

		/** Every instance under the root whose parent is Parent. */
		void InstancesOf(const FString& Root, UMaterialInterface* Parent, TArray<UMaterialInstanceConstant*>& Out)
		{
			TArray<FAssetData> Assets;
			SeedRegistry().GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true);
			for (const FAssetData& Asset : Assets)
			{
				const UClass* Class = Asset.GetClass();
				if (!Class || !Class->IsChildOf(UMaterialInstanceConstant::StaticClass()))
				{
					continue;
				}
				UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset.GetAsset());
				if (Instance && Instance->Parent == Parent)
				{
					Out.AddUnique(Instance);
				}
			}
		}
	}

	void CheckMasters(const FString& LibraryRoot, TArray<FSurfMasterState>& Out)
	{
		Out.Reset();
		const FString Root = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
		if (Root.IsEmpty())
		{
			return;
		}

		TArray<FAssetData> Assets;
		SeedRegistry().GetAssetsByPath(FName(*(Root + TEXT("/Materials"))), Assets, /*bRecursive*/ true);
		// The seeded instances beside the masters (S208a), and any still in
		// the older MI folder.
		SeedRegistry().GetAssetsByPath(FName(*(Root + TEXT("/MaterialInstances"))), Assets, /*bRecursive*/ true);
		SeedRegistry().GetAssetsByPath(FName(*(Root + TEXT("/MI"))), Assets, /*bRecursive*/ true);
		for (const FAssetData& Asset : Assets)
		{
			if (!IsLibraryAsset(Asset))
			{
				continue;
			}
			UObject* Object = Asset.GetAsset();
			const USurfMaterialStamp* Mark = USurfMaterialStamp::Find(Object);
			if (!Mark || Mark->Source.IsEmpty())
			{
				continue;
			}

			FSurfMasterState& State = Out.AddDefaulted_GetRef();
			State.Copy = Asset;
			State.Source = Mark->Source;
			State.bGone = !PackageIsThere(Mark->Source);
			State.bNewer = !State.bGone && FileHashOf(Mark->Source) != Mark->Hash;
			// A copy made before the content was recorded counts as left alone: the rule is that nobody edits a seeded master.
			State.bEdited = Mark->Content != 0 && ContentOf(Object) != Mark->Content;
		}
	}

	FString FSurfUpdateResult::Line() const
	{
		if (Failed.Num() > 0 && Updated + Added + Beside.Num() == 0)
		{
			return TEXT("update: ") + Failed[0];
		}
		if (Updated + Added + Beside.Num() == 0)
		{
			return FString::Printf(TEXT("masters up to date (%d kept)"), Kept);
		}

		TArray<FString> Parts;
		if (Updated > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d master%s updated"), Updated, Updated == 1 ? TEXT("") : TEXT("s")));
		}
		if (Added > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d added"), Added));
		}
		for (const FString& Pair : Beside)
		{
			Parts.Add(Pair + TEXT(" (changed here, kept; the new one beside it)"));
		}
		if (Reparented > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d instance%s moved to the new one"), Reparented, Reparented == 1 ? TEXT("") : TEXT("s")));
		}
		if (Kept > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d kept"), Kept));
		}
		if (Failed.Num() > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d failed"), Failed.Num()));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	FSurfUpdateResult UpdateMasters(const FString& LibraryRoot, bool bSave, bool bReparent, const FString& SourceRoot)
	{
		FSurfUpdateResult Result;

		const FString Root = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
		if (Root.IsEmpty())
		{
			Result.Failed.Add(TEXT("no library root"));
			return Result;
		}
		const FString Dest = Root + TEXT("/Materials");

		TArray<FSurfMasterState> States;
		CheckMasters(Root, States);
		TMap<FString, const FSurfMasterState*> BySource;
		for (const FSurfMasterState& State : States)
		{
			BySource.Add(State.Source, &State);
		}

		TArray<FSurfSeedEntry> Entries;
		Plan(SourceRoot.IsEmpty() ? PluginParentsRoot() : SourceRoot, Dest, Entries);
		if (Entries.Num() == 0)
		{
			Result.Failed.Add(TEXT("the plugin has no parents to update from"));
			return Result;
		}

		struct FBeside
		{
			FString Source;
			FString OldPath;
			UObject* Old = nullptr;
		};
		TArray<FBeside> Besides;
		TArray<FString> UpdatedSources;
		TArray<FString> AddedSources;

		for (FSurfSeedEntry& Entry : Entries)
		{
			const FString SourcePackage = Entry.Source.PackageName.ToString();
			const FString Name = Entry.Source.AssetName.ToString();
			const FSurfMasterState* State = BySource.FindRef(SourcePackage);
			if (!State)
			{
				// Nothing stamped from this source: new to the library, or an asset of the user's own by that name.
				if (PackageIsThere(Entry.DestPackage))
				{
					Entry.bKeep = true;
					++Result.Kept;
				}
				else
				{
					AddedSources.Add(SourcePackage);
				}
				continue;
			}

			// The copy is wherever the user keeps it now; renamed, it is his.
			Entry.DestPackage = State->Copy.PackageName.ToString();
			if (State->Copy.AssetName.ToString() != Name || !State->bNewer)
			{
				Entry.bKeep = true;
				++Result.Kept;
				continue;
			}

			if (State->bEdited)
			{
				const FString Folder = FPackageName::GetLongPackagePath(Entry.DestPackage);
				const FString NewName = BesideName(Folder, Name);
				Entry.DestPackage = Folder / NewName;
				Entry.DestName = NewName;
				Besides.Add({ SourcePackage, State->Copy.GetObjectPathString(), State->Copy.GetAsset() });
			}
			else
			{
				UpdatedSources.Add(SourcePackage);
			}
		}

		FSurfSeedOptions Options;
		Options.bOverwrite = true;
		Options.bSave = bSave;
		Options.bStamp = true;
		const FSurfSeedResult Copied = Copy(Entries, Options);
		Result.Failed.Append(Copied.Failed);

		for (const FString& Source : UpdatedSources)
		{
			if (UObject* const* Made = Copied.Copies.Find(Source))
			{
				++Result.Updated;
				// The instances of a master replaced under them settle on it again.
				if (UMaterialInterface* AsParent = Cast<UMaterialInterface>(*Made))
				{
					TArray<UMaterialInstanceConstant*> Instances;
					InstancesOf(Root, AsParent, Instances);
					for (UMaterialInstanceConstant* Instance : Instances)
					{
						Instance->PostEditChange();
					}
				}
			}
		}
		for (const FString& Source : AddedSources)
		{
			if (Copied.Copies.Contains(Source))
			{
				++Result.Added;
			}
		}
		for (const FBeside& Beside : Besides)
		{
			UObject* const* Made = Copied.Copies.Find(Beside.Source);
			if (!Made || !*Made)
			{
				continue;
			}
			Result.Beside.Add(FPackageName::ObjectPathToObjectName(Beside.OldPath) + TEXT(" -> ") + (*Made)->GetName());

			UMaterialInterface* OldParent = Cast<UMaterialInterface>(Beside.Old);
			UMaterialInterface* NewParent = Cast<UMaterialInterface>(*Made);
			if (!bReparent || !OldParent || !NewParent)
			{
				continue;
			}
			TArray<UMaterialInstanceConstant*> Instances;
			InstancesOf(Root, OldParent, Instances);
			for (UMaterialInstanceConstant* Instance : Instances)
			{
				Instance->SetParentEditorOnly(NewParent, /*RecacheShader*/ false);
				Instance->PostEditChange();
				Instance->MarkPackageDirty();
				if (bSave && !SaveAsset(Instance))
				{
					Result.Failed.Add(Instance->GetOutermost()->GetName() + TEXT(": could not save"));
				}
				++Result.Reparented;
			}
		}

		SurfSession::Verb(TEXT("materials_update"), FString::Printf(TEXT("root=%s updated=%d added=%d beside=%d reparented=%d kept=%d failed=%d"),
			*Root, Result.Updated, Result.Added, Result.Beside.Num(), Result.Reparented, Result.Kept, Result.Failed.Num()));
		return Result;
	}
}
