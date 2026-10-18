// Copyright MapModelers. All Rights Reserved.
//
// Seeding (D55/M2): a library copied with every reference between its
// assets re-pointed to the copies, and the one-off import of the source
// library into the plugin's content, which is the same operation with a
// mount point in front of it.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfMaterialSeed.h"
#include "SurfMaterialInfo.h"
#include "SurfMaterialsView.h"
#include "SurfMaterialManager.h"
#include "SurfSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture2D.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "PackageTools.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfMaterialSeedTestUtil
{
	static const TCHAR* TestRoot = TEXT("/Game/SurfacingPro/_SurfSeedTest");

	static IAssetRegistry& Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	template<typename T>
	static T* MakeAsset(const FString& Folder, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(Folder / Name));
		return Package ? NewObject<T>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	}

	static void Announce(UObject* Asset)
	{
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}
	}

	static void Drop(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		UPackage* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (Package && Package != GetTransientPackage())
		{
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	/** A four by four texture with a source, so a copy of it has something to copy. */
	static UTexture2D* MakeTexture(const FString& Folder, const TCHAR* Name)
	{
		UTexture2D* Texture = MakeAsset<UTexture2D>(Folder, Name);
		if (Texture)
		{
			TArray<uint8> Pixels;
			Pixels.Init(128, 4 * 4 * 4);
			Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
			Texture->SRGB = false;
			Texture->MipGenSettings = TMGS_NoMipmaps;
			Texture->PostEditChange();
		}
		return Texture;
	}

	/** The first texture-sample expression of a material, or null. */
	static UMaterialExpressionTextureBase* FirstTextureExpression(const UMaterial* Material)
	{
		if (!Material)
		{
			return nullptr;
		}
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (UMaterialExpressionTextureBase* AsTexture = Cast<UMaterialExpressionTextureBase>(Expression))
			{
				return AsTexture;
			}
		}
		return nullptr;
	}
}

// --- A library copied, references re-pointed --------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialSeedCopyTest, "SurfacingPro.Materials.SeedCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialSeedCopyTest::RunTest(const FString& Parameters)
{
	const FString Src = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("Src");
	const FString Dst = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("Dst");

	// A texture, a master reading it, an instance of the master overriding it - the shape of a library.
	UTexture2D* Texture = SurfMaterialSeedTestUtil::MakeTexture(Src, TEXT("T_SurfSeed"));
	UMaterial* Master = SurfMaterialSeedTestUtil::MakeAsset<UMaterial>(Src, TEXT("M_SurfSeed"));
	UMaterialInstanceConstant* Instance = SurfMaterialSeedTestUtil::MakeAsset<UMaterialInstanceConstant>(Src / TEXT("Instances"), TEXT("MI_SurfSeed"));
	if (!Texture || !Master || !Instance)
	{
		AddError(TEXT("could not make the source library"));
		SurfMaterialSeedTestUtil::Drop(Texture); SurfMaterialSeedTestUtil::Drop(Master); SurfMaterialSeedTestUtil::Drop(Instance);
		return false;
	}

	UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(
		UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionTextureSample::StaticClass()));
	if (Sample)
	{
		Sample->Texture = Texture;
	}
	Master->PostEditChange();
	Instance->SetParentEditorOnly(Master, false);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tex")), Texture);
	Instance->PostEditChange();
	SurfMaterialSeedTestUtil::Announce(Texture);
	SurfMaterialSeedTestUtil::Announce(Master);
	SurfMaterialSeedTestUtil::Announce(Instance);

	// The plan: everything under the source, at the same relative paths.
	TArray<SurfMaterialSeed::FSurfSeedEntry> Plan;
	SurfMaterialSeed::Plan(Src + TEXT("/"), Dst, Plan);
	TestEqual(TEXT("the plan holds the three"), Plan.Num(), 3);
	const bool bInstanceKept = Plan.ContainsByPredicate([&Dst](const SurfMaterialSeed::FSurfSeedEntry& E) { return E.DestPackage == Dst / TEXT("Instances/MI_SurfSeed"); });
	TestTrue(TEXT("a sub-folder is kept"), bInstanceKept);

	SurfMaterialSeed::FSurfSeedOptions Options;
	Options.bOverwrite = false;
	Options.bSave = false;
	Options.bStamp = true;
	SurfMaterialSeed::FSurfSeedResult Result = SurfMaterialSeed::Copy(Plan, Options);
	for (const FString& Line : Result.Failed)
	{
		AddError(Line);
	}
	TestEqual(TEXT("three copied"), Result.Copied, 3);

	UTexture2D* TextureCopy = FindObject<UTexture2D>(nullptr, *(Dst / TEXT("T_SurfSeed.T_SurfSeed")));
	UMaterial* MasterCopy = FindObject<UMaterial>(nullptr, *(Dst / TEXT("M_SurfSeed.M_SurfSeed")));
	UMaterialInstanceConstant* InstanceCopy = FindObject<UMaterialInstanceConstant>(nullptr, *(Dst / TEXT("Instances/MI_SurfSeed.MI_SurfSeed")));
	TestNotNull(TEXT("the texture copy exists"), TextureCopy);
	TestNotNull(TEXT("the master copy exists"), MasterCopy);
	TestNotNull(TEXT("the instance copy exists, in its sub-folder"), InstanceCopy);

	if (TextureCopy && MasterCopy && InstanceCopy)
	{
		TestTrue(TEXT("the instance copy's parent is the master copy"), InstanceCopy->Parent == MasterCopy);
		const UTexture* Overridden = nullptr;
		for (const FTextureParameterValue& Value : InstanceCopy->TextureParameterValues)
		{
			if (Value.ParameterInfo.Name == FName(TEXT("Tex")))
			{
				Overridden = Value.ParameterValue;
			}
		}
		TestTrue(TEXT("the instance copy's texture is the texture copy"), Overridden == TextureCopy);
		const UMaterialExpressionTextureBase* SampleCopy = SurfMaterialSeedTestUtil::FirstTextureExpression(MasterCopy);
		TestTrue(TEXT("the master copy reads the texture copy"), SampleCopy && SampleCopy->Texture == TextureCopy);
		TestTrue(TEXT("the master copy's expression is its own"), SampleCopy && SampleCopy->IsIn(MasterCopy));

		// The sources are untouched.
		TestTrue(TEXT("the source instance still has the source master"), Instance->Parent == Master);
		TestTrue(TEXT("the source master still reads the source texture"), Sample && Sample->Texture == Texture);

		// The stamp.
		const USurfMaterialStamp* Mark = USurfMaterialStamp::Find(InstanceCopy);
		TestNotNull(TEXT("a copy is stamped"), Mark);
		TestTrue(TEXT("with where it came from"), Mark && Mark->Source == Src / TEXT("Instances/MI_SurfSeed"));
		TestNull(TEXT("and the source is not"), USurfMaterialStamp::Find(Instance));

		// Copy again without overwrite: nothing copied, everything kept.
		Result = SurfMaterialSeed::Copy(Plan, Options);
		TestEqual(TEXT("a second copy skips what is there"), Result.Skipped, 3);
		TestEqual(TEXT("and copies nothing"), Result.Copied, 0);
	}

	SurfMaterialSeedTestUtil::Drop(InstanceCopy); SurfMaterialSeedTestUtil::Drop(MasterCopy); SurfMaterialSeedTestUtil::Drop(TextureCopy);
	SurfMaterialSeedTestUtil::Drop(Instance); SurfMaterialSeedTestUtil::Drop(Master); SurfMaterialSeedTestUtil::Drop(Texture);
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.RemovePath(Src / TEXT("Instances"));
	Reg.RemovePath(Dst / TEXT("Instances"));
	Reg.RemovePath(Src);
	Reg.RemovePath(Dst);
	Reg.RemovePath(FString(SurfMaterialSeedTestUtil::TestRoot));
	return true;
}

// --- The content of an asset as a number (M2, Update masters) --------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialMasterContentTest, "SurfacingPro.Materials.MasterContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialMasterContentTest::RunTest(const FString& Parameters)
{
	const FString Folder = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("Content");
	const FString Path = Folder / TEXT("M_SurfContent.M_SurfContent");

	UMaterial* Master = SurfMaterialSeedTestUtil::MakeAsset<UMaterial>(Folder, TEXT("M_SurfContent"));
	if (!Master)
	{
		AddError(TEXT("could not make the master"));
		return false;
	}
	UMaterialExpressionScalarParameter* Param = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionScalarParameter::StaticClass()));
	if (Param)
	{
		Param->ParameterName = FName(TEXT("Tiling"));
		Param->DefaultValue = 2.0f;
	}
	Master->PostEditChange();
	SurfMaterialSeedTestUtil::Announce(Master);

	const uint32 A = SurfMaterialSeed::ContentOf(Master);
	TestTrue(TEXT("content is a number"), A != 0);
	TestEqual(TEXT("the same twice"), SurfMaterialSeed::ContentOf(Master), A);
	Master->PostEditChange();
	TestEqual(TEXT("unchanged by a PostEditChange"), SurfMaterialSeed::ContentOf(Master), A);

	SurfMaterialSeed::Stamp(Master, TEXT("/SurfacingPro/Materials/M_SurfContent"));
	const USurfMaterialStamp* Mark = USurfMaterialStamp::Find(Master);
	TestTrue(TEXT("the stamp records the content"), Mark && Mark->Content == A);
	TestEqual(TEXT("the stamp does not count"), SurfMaterialSeed::ContentOf(Master), A);
	TestTrue(TEXT("and stays on"), USurfMaterialStamp::Find(Master) == Mark);

	// Saved and loaded back: the same number.
	UPackage* Package = Master->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*File, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
	if (SurfMaterialSeed::SaveAsset(Master))
	{
		TArray<UPackage*> Packages = { Package };
		FText ReloadError;
		UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumePositive);
		UMaterial* Back = FindObject<UMaterial>(nullptr, *Path);
		TestNotNull(TEXT("loaded back"), Back);
		if (Back)
		{
			TestEqual(TEXT("the same number loaded back"), SurfMaterialSeed::ContentOf(Back), A);
			const USurfMaterialStamp* BackMark = USurfMaterialStamp::Find(Back);
			TestTrue(TEXT("with its stamp"), BackMark && BackMark->Content == A);
			Master = Back;
		}
		// The file goes; the loader is let go of it first. The package object was replaced by the reload.
		if (Back)
		{
			ResetLoaders(Back->GetOutermost());
		}
		TestTrue(TEXT("the file is removed again"), IFileManager::Get().Delete(*File, false, true, true));
	}
	else
	{
		AddError(TEXT("could not save the master"));
	}

	// An edit changes it.
	Master->TwoSided = !Master->TwoSided;
	Master->PostEditChange();
	TestTrue(TEXT("an edit changes it"), SurfMaterialSeed::ContentOf(Master) != A);

	SurfMaterialSeedTestUtil::Drop(Master);
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.RemovePath(Folder);
	Reg.RemovePath(FString(SurfMaterialSeedTestUtil::TestRoot));
	// The folders the save made go too (S207b): an empty _SurfSeedTest was
	// left in his content - "do we need directories under surfacingpro".
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(FString(SurfMaterialSeedTestUtil::TestRoot) + TEXT("/")), false, true);
	return true;
}

// --- Update masters (M2) ------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialUpdateMastersTest, "SurfacingPro.Materials.UpdateMasters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialUpdateMastersTest::RunTest(const FString& Parameters)
{
	const FString Src = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("UpSrc");
	const FString Lib = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("UpLib");
	const FString Dst = Lib / TEXT("Materials");

	// The plugin's side: a master and a ready-made instance of it.
	UMaterial* Master = SurfMaterialSeedTestUtil::MakeAsset<UMaterial>(Src, TEXT("M_SurfUp"));
	UMaterialInstanceConstant* Instance = SurfMaterialSeedTestUtil::MakeAsset<UMaterialInstanceConstant>(Src / TEXT("Instances"), TEXT("MI_SurfUp"));
	if (!Master || !Instance)
	{
		AddError(TEXT("could not make the source"));
		SurfMaterialSeedTestUtil::Drop(Master); SurfMaterialSeedTestUtil::Drop(Instance);
		return false;
	}
	UMaterialExpressionScalarParameter* Param = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionScalarParameter::StaticClass()));
	if (Param)
	{
		Param->ParameterName = FName(TEXT("Tiling"));
	}
	Master->PostEditChange();
	Instance->SetParentEditorOnly(Master, false);
	Instance->PostEditChange();
	SurfMaterialSeedTestUtil::Announce(Master);
	SurfMaterialSeedTestUtil::Announce(Instance);

	// First time: nothing in the library, everything added.
	SurfMaterialSeed::FSurfUpdateResult First = SurfMaterialSeed::UpdateMasters(Lib, /*bSave*/ false, /*bReparent*/ false, Src);
	for (const FString& Line : First.Failed) { AddError(Line); }
	TestEqual(TEXT("first: two added"), First.Added, 2);
	TestEqual(TEXT("first: none updated"), First.Updated, 0);

	UMaterial* MasterCopy = FindObject<UMaterial>(nullptr, *(Dst / TEXT("M_SurfUp.M_SurfUp")));
	// Instances land beside the masters' folder (S208a).
	UMaterialInstanceConstant* InstanceCopy = FindObject<UMaterialInstanceConstant>(nullptr, *(Lib / TEXT("MaterialInstances/MI_SurfUp.MI_SurfUp")));
	TestNotNull(TEXT("the master copy"), MasterCopy);
	TestNotNull(TEXT("the instance copy"), InstanceCopy);
	if (!MasterCopy || !InstanceCopy)
	{
		SurfMaterialSeedTestUtil::Drop(MasterCopy); SurfMaterialSeedTestUtil::Drop(InstanceCopy);
		SurfMaterialSeedTestUtil::Drop(Master); SurfMaterialSeedTestUtil::Drop(Instance);
		return false;
	}
	TestTrue(TEXT("the instance copy's parent is the master copy"), InstanceCopy->Parent == MasterCopy);

	// Checked: both stamped, neither newer nor edited (no files, no edits).
	TArray<SurfMaterialSeed::FSurfMasterState> States;
	SurfMaterialSeed::CheckMasters(Lib, States);
	TestEqual(TEXT("two stamped copies"), States.Num(), 2);
	for (const SurfMaterialSeed::FSurfMasterState& State : States)
	{
		TestFalse(TEXT("not newer"), State.bNewer);
		TestFalse(TEXT("not edited"), State.bEdited);
		TestFalse(TEXT("not gone"), State.bGone);
	}

	// Again: nothing to do.
	SurfMaterialSeed::FSurfUpdateResult Again = SurfMaterialSeed::UpdateMasters(Lib, false, false, Src);
	TestEqual(TEXT("again: nothing updated"), Again.Updated, 0);
	TestEqual(TEXT("again: nothing added"), Again.Added, 0);
	TestEqual(TEXT("again: both kept"), Again.Kept, 2);
	TestTrue(TEXT("again: the line says so"), Again.Line().StartsWith(TEXT("masters up to date")));

	// The plugin's master is newer and the copy was left alone: replaced in place, the instance keeps its parent.
	Master->TwoSided = !Master->TwoSided;
	Master->PostEditChange();
	if (USurfMaterialStamp* Mark = USurfMaterialStamp::FindOrAdd(MasterCopy))
	{
		Mark->Hash = TEXT("an older file");
	}
	SurfMaterialSeed::CheckMasters(Lib, States);
	const SurfMaterialSeed::FSurfMasterState* MasterState = States.FindByPredicate([&](const SurfMaterialSeed::FSurfMasterState& S) { return S.Copy.AssetName == FName(TEXT("M_SurfUp")); });
	TestTrue(TEXT("the master copy is behind"), MasterState && MasterState->bNewer && !MasterState->bEdited);

	SurfMaterialSeed::FSurfUpdateResult Third = SurfMaterialSeed::UpdateMasters(Lib, false, false, Src);
	for (const FString& Line : Third.Failed) { AddError(Line); }
	TestEqual(TEXT("third: one updated"), Third.Updated, 1);
	TestEqual(TEXT("third: nothing beside"), Third.Beside.Num(), 0);
	UMaterial* MasterCopyNow = FindObject<UMaterial>(nullptr, *(Dst / TEXT("M_SurfUp.M_SurfUp")));
	TestTrue(TEXT("third: the same object, in place"), MasterCopyNow == MasterCopy);
	TestTrue(TEXT("third: with the plugin's change"), MasterCopyNow && MasterCopyNow->TwoSided == Master->TwoSided);
	TestTrue(TEXT("third: the instance keeps its parent"), InstanceCopy->Parent == MasterCopy);
	const USurfMaterialStamp* NowMark = USurfMaterialStamp::Find(MasterCopy);
	TestTrue(TEXT("third: stamped afresh"), NowMark && NowMark->Hash.IsEmpty() && NowMark->Content == SurfMaterialSeed::ContentOf(MasterCopy));

	// The copy edited here and the plugin's newer: kept, the new one beside it, the instance moved over.
	UMaterialEditingLibrary::CreateMaterialExpression(MasterCopy, UMaterialExpressionScalarParameter::StaticClass());
	MasterCopy->PostEditChange();
	if (USurfMaterialStamp* Mark = USurfMaterialStamp::FindOrAdd(MasterCopy))
	{
		Mark->Hash = TEXT("an older file");
	}
	SurfMaterialSeed::CheckMasters(Lib, States);
	MasterState = States.FindByPredicate([&](const SurfMaterialSeed::FSurfMasterState& S) { return S.Copy.AssetName == FName(TEXT("M_SurfUp")); });
	TestTrue(TEXT("the master copy is behind and edited"), MasterState && MasterState->bNewer && MasterState->bEdited);

	SurfMaterialSeed::FSurfUpdateResult Fourth = SurfMaterialSeed::UpdateMasters(Lib, false, /*bReparent*/ true, Src);
	for (const FString& Line : Fourth.Failed) { AddError(Line); }
	TestEqual(TEXT("fourth: nothing updated in place"), Fourth.Updated, 0);
	TestEqual(TEXT("fourth: one beside"), Fourth.Beside.Num(), 1);
	TestEqual(TEXT("fourth: the instance moved"), Fourth.Reparented, 1);
	TestTrue(TEXT("fourth: the edited copy is still there"), FindObject<UMaterial>(nullptr, *(Dst / TEXT("M_SurfUp.M_SurfUp"))) == MasterCopy);
	TestEqual(TEXT("fourth: with its edit"), MasterCopy->GetExpressions().Num(), 2);
	UMaterial* NewCopy = InstanceCopy->Parent ? Cast<UMaterial>(InstanceCopy->Parent) : nullptr;
	TestTrue(TEXT("fourth: the instance's parent is the new one"), NewCopy && NewCopy != MasterCopy && NewCopy->GetName().StartsWith(TEXT("M_SurfUp_")));
	TestTrue(TEXT("fourth: beside the old one"), NewCopy && NewCopy->GetOutermost()->GetName().StartsWith(Dst + TEXT("/M_SurfUp_")));
	TestTrue(TEXT("fourth: the new one is the plugin's"), NewCopy && NewCopy->GetExpressions().Num() == 1);
	TestTrue(TEXT("fourth: the line names it"), Fourth.Beside.Num() == 1 && Fourth.Beside[0].StartsWith(TEXT("M_SurfUp -> M_SurfUp_")));

	SurfMaterialSeedTestUtil::Drop(NewCopy); SurfMaterialSeedTestUtil::Drop(InstanceCopy); SurfMaterialSeedTestUtil::Drop(MasterCopy);
	SurfMaterialSeedTestUtil::Drop(Instance); SurfMaterialSeedTestUtil::Drop(Master);
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.RemovePath(Src / TEXT("Instances"));
	Reg.RemovePath(Dst / TEXT("Instances"));
	Reg.RemovePath(Lib / TEXT("MaterialInstances"));
	Reg.RemovePath(Src);
	Reg.RemovePath(Dst);
	Reg.RemovePath(Lib);
	Reg.RemovePath(FString(SurfMaterialSeedTestUtil::TestRoot));
	return true;
}

// --- The import of the source library into the plugin -----------------------------
//
// Runs only when ue/_run/seed_source.txt names a folder: the Content folder of
// the library the parents come from. It is mounted where its own references
// point, read, and copied into the plugin's content with the references
// re-pointed. Without the file there is nothing to do.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialImportSeedSourceTest, "SurfacingPro.Materials.ImportSeedSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialImportSeedSourceTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
	if (!Plugin.IsValid())
	{
		AddError(TEXT("the plugin is not loaded"));
		return false;
	}

	const FString Gate = Plugin->GetBaseDir() / TEXT("_run/seed_source.txt");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Gate))
	{
		AddInfo(TEXT("no seed source named; nothing to import"));
		return true;
	}
	Source = Source.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
	if (!Source.EndsWith(TEXT("/")))
	{
		Source += TEXT("/");
	}
	if (!IFileManager::Get().DirectoryExists(*Source))
	{
		AddError(FString::Printf(TEXT("the seed source is not a folder: %s"), *Source));
		return false;
	}

	// The source's own references say /MapLink/..., so that is where it is mounted.
	const FString Mount = TEXT("/MapLink/");
	if (!FPackageName::MountPointExists(Mount))
	{
		FPackageName::RegisterMountPoint(Mount, Source);
	}
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	TArray<FString> Scan;
	Scan.Add(TEXT("/MapLink/Materials"));
	Reg.ScanPathsSynchronous(Scan, /*bForceRescan*/ true);

	TArray<SurfMaterialSeed::FSurfSeedEntry> Plan;
	SurfMaterialSeed::Plan(TEXT("/MapLink/Materials"), SurfMaterialSeed::PluginParentsRoot(), Plan,
		[](const FAssetData& Asset, FString& Relative)
		{
			const FString Name = Asset.AssetName.ToString();
			// Debug and scratch stay where they are; the landscape entry is a redirector.
			if (Name.StartsWith(TEXT("DBG_")) || Name.StartsWith(TEXT("mat_cube")) || Name.StartsWith(TEXT("M_Landscape")))
			{
				return false;
			}
			// The ready-made instances are the library's own, not debug.
			if (Relative.StartsWith(TEXT("DBG_MI/")))
			{
				Relative = TEXT("Instances/") + Relative.Mid(7);
			}
			return true;
		});
	TestTrue(TEXT("the source has parents"), Plan.Num() > 0);

	// The masters lean on one function of an engine plugin; it comes along,
	// so the library stands without that plugin.
	TArray<SurfMaterialSeed::FSurfSeedEntry> Borrowed;
	SurfMaterialSeed::Plan(TEXT("/DatasmithContent/Materials/Water/MaterialFunctions"), SurfMaterialSeed::PluginParentsRoot() + TEXT("/Functions"), Borrowed,
		[](const FAssetData& Asset, FString& Relative)
		{
			return Asset.AssetName == FName(TEXT("MF_NormalStrength_WS"));
		});
	TestEqual(TEXT("the borrowed function is found"), Borrowed.Num(), 1);
	Plan.Append(Borrowed);

	// The blocking material reads the template project's checker; it comes
	// along too, so the material stands anywhere.
	TArray<SurfMaterialSeed::FSurfSeedEntry> Checker;
	SurfMaterialSeed::Plan(TEXT("/Game/LevelPrototyping/Textures"), SurfMaterialSeed::PluginParentsRoot(), Checker,
		[](const FAssetData& Asset, FString& Relative)
		{
			return Asset.AssetName == FName(TEXT("T_GridChecker_A"));
		});
	TestEqual(TEXT("the checker is found"), Checker.Num(), 1);
	Plan.Append(Checker);

	for (const SurfMaterialSeed::FSurfSeedEntry& Entry : Plan)
	{
		AddInfo(FString::Printf(TEXT("plan: %s -> %s"), *Entry.Source.PackageName.ToString(), *Entry.DestPackage));
	}

	SurfMaterialSeed::FSurfSeedOptions Options;
	Options.bOverwrite = true;
	Options.bSave = true;
	Options.bStamp = false;
	const SurfMaterialSeed::FSurfSeedResult Result = SurfMaterialSeed::Copy(Plan, Options);
	for (const FString& Line : Result.Failed)
	{
		AddError(Line);
	}
	TestEqual(TEXT("every planned asset was copied"), Result.Copied, Plan.Num());

	// An instance that read a texture of some other project reads the
	// library's own noise instead, so it stands anywhere.
	UObject* const* NoisePtr = Result.Copies.Find(TEXT("/MapLink/Materials/128_noise"));
	UTexture* Noise = NoisePtr ? Cast<UTexture>(*NoisePtr) : nullptr;
	TestNotNull(TEXT("the noise texture came along"), Noise);
	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Pair.Value);
		if (!Instance || !Noise)
		{
			continue;
		}
		bool bChanged = false;
		for (FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			const UTexture* Texture = Value.ParameterValue;
			const FString Package = Texture ? Texture->GetOutermost()->GetName() : FString(TEXT("nothing (its texture was in another project)"));
			if (!Texture || (!Package.StartsWith(TEXT("/SurfacingPro/")) && !Package.StartsWith(TEXT("/Engine/"))))
			{
				AddInfo(FString::Printf(TEXT("%s: %s read %s, now the library's noise"), *Pair.Key, *Value.ParameterInfo.Name.ToString(), *Package));
				Value.ParameterValue = Noise;
				bChanged = true;
			}
		}
		if (bChanged)
		{
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			TestTrue(TEXT("the mended instance is saved"), SurfMaterialSeed::SaveAsset(Instance));
		}
	}

	// A texture's import record names the file it was imported from on the
	// machine that did it; the copies carry none.
	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		if (UTexture* Texture = Cast<UTexture>(Pair.Value))
		{
			Texture->AssetImportData = NewObject<UAssetImportData>(Texture);
			Texture->MarkPackageDirty();
			TestTrue(TEXT("the texture is saved without its import record"), SurfMaterialSeed::SaveAsset(Texture));
		}
	}

	// Nothing a copy points at may still be in the source library.
	int32 Leftovers = 0;
	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Pair.Value))
		{
			if (Instance->Parent && Instance->Parent->GetOutermost()->GetName().StartsWith(Mount))
			{
				++Leftovers;
				AddError(FString::Printf(TEXT("%s: parent still %s"), *Pair.Key, *Instance->Parent->GetPathName()));
			}
		}
		if (const UMaterial* Material = Cast<UMaterial>(Pair.Value))
		{
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				const UObject* Points = nullptr;
				if (const UMaterialExpressionTextureBase* AsTexture = Cast<UMaterialExpressionTextureBase>(Expression))
				{
					Points = AsTexture->Texture;
				}
				else if (const UMaterialExpressionMaterialFunctionCall* AsCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					Points = AsCall->MaterialFunction;
				}
				if (Points && Points->GetOutermost()->GetName().StartsWith(Mount))
				{
					++Leftovers;
					AddError(FString::Printf(TEXT("%s: still reads %s"), *Pair.Key, *Points->GetPathName()));
				}
			}
		}
	}
	TestEqual(TEXT("no copy still points into the source"), Leftovers, 0);

	// The registry reads the saved files now, so the dependency check that
	// follows sees what is on disk rather than what was announced.
	TArray<FString> Saved;
	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		if (Pair.Value)
		{
			Saved.Add(FPackageName::LongPackageNameToFilename(Pair.Value->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
		}
	}
	Reg.ScanFilesSynchronous(Saved, /*bForceRescan*/ true);

	AddInfo(FString::Printf(TEXT("imported %d, skipped %d, failed %d"), Result.Copied, Result.Skipped, Result.Failed.Num()));
	return true;
}

// --- What the plugin ships ----------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialPluginParentsTest, "SurfacingPro.Materials.PluginParents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialPluginParentsTest::RunTest(const FString& Parameters)
{
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.WaitForCompletion();

	TArray<FAssetData> Assets;
	Reg.GetAssetsByPath(FName(*SurfMaterialSeed::PluginParentsRoot()), Assets, /*bRecursive*/ true);
	TestTrue(TEXT("the plugin ships its parents"), Assets.Num() >= 30);

	// Every parent leans only on the plugin or the engine, and what it leans on in the plugin is there.
	int32 Wrong = 0;
	for (const FAssetData& Asset : Assets)
	{
		TArray<FName> Dependencies;
		Reg.GetDependencies(Asset.PackageName, Dependencies);
		for (const FName& Dependency : Dependencies)
		{
			const FString Name = Dependency.ToString();
			if (Name.StartsWith(TEXT("/Script/")) || Name.StartsWith(TEXT("/Engine/")))
			{
				continue;
			}
			if (!Name.StartsWith(TEXT("/SurfacingPro/")))
			{
				AddError(FString::Printf(TEXT("%s leans on %s, outside the plugin"), *Asset.PackageName.ToString(), *Name));
				++Wrong;
				continue;
			}
			if (!FPackageName::DoesPackageExist(Name))
			{
				AddError(FString::Printf(TEXT("%s leans on %s, which has no file"), *Asset.PackageName.ToString(), *Name));
				++Wrong;
			}
		}
	}
	TestEqual(TEXT("every dependency is in the plugin or the engine, and present"), Wrong, 0);
	return true;
}

// --- Seeding into a library ----------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialSeedParentsTest, "SurfacingPro.Materials.SeedParents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialSeedParentsTest::RunTest(const FString& Parameters)
{
	SurfMaterialSeedTestUtil::Registry().WaitForCompletion();
	const FString Root = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("Seeded");

	SurfMaterialSeed::FSurfSeedResult Result = SurfMaterialSeed::SeedParents(Root + TEXT("/"), /*bSave*/ false);
	for (const FString& Line : Result.Failed)
	{
		AddError(Line);
	}
	TestTrue(TEXT("the parents were seeded"), Result.Copied >= 30);
	TestEqual(TEXT("nothing was there to skip"), Result.Skipped, 0);

	// Every copy that can carry a stamp carries one, naming its source.
	int32 Stampable = 0;
	int32 Stamped = 0;
	int32 UnderRoot = 0;
	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		// Masters under Materials, the ready-made instances under MaterialInstances (S208a).
		if (Pair.Value && (Pair.Value->GetOutermost()->GetName().StartsWith(Root + TEXT("/Materials"))
			|| Pair.Value->GetOutermost()->GetName().StartsWith(Root + TEXT("/MaterialInstances"))))
		{
			++UnderRoot;
		}
		if (const USurfMaterialStamp* Mark = USurfMaterialStamp::Find(Pair.Value))
		{
			++Stamped;
			if (Mark->Source != Pair.Key)
			{
				AddError(FString::Printf(TEXT("%s is stamped as from %s"), *Pair.Key, *Mark->Source));
			}
			if (Mark->Version.IsEmpty())
			{
				AddError(FString::Printf(TEXT("%s has no version"), *Pair.Key));
			}
		}
		if (Cast<UMaterialInterface>(Pair.Value) || Cast<UTexture>(Pair.Value))
		{
			++Stampable;
		}
	}
	TestEqual(TEXT("every copy is under the root's Materials folder"), UnderRoot, Result.Copies.Num());
	TestEqual(TEXT("every material, instance and texture is stamped"), Stamped, Stampable);

	// A seeded instance's parent is the seeded master, not the plugin's.
	UObject* const* RedPtr = Result.Copies.Find(SurfMaterialSeed::PluginParentsRoot() + TEXT("/Instances/MI_OpqSimple_Red"));
	const UMaterialInstanceConstant* Red = RedPtr ? Cast<UMaterialInstanceConstant>(*RedPtr) : nullptr;
	TestNotNull(TEXT("the red instance was seeded"), Red);
	TestTrue(TEXT("with the seeded master as its parent"), Red && Red->Parent && Red->Parent->GetOutermost()->GetName() == Root + TEXT("/Materials/M_Opq_Simple"));

	// Seeding again leaves it all alone.
	const SurfMaterialSeed::FSurfSeedResult Again = SurfMaterialSeed::SeedParents(Root, /*bSave*/ false);
	TestEqual(TEXT("a second seed copies nothing"), Again.Copied, 0);
	TestEqual(TEXT("and keeps everything"), Again.Skipped, Result.Copied);

	for (const TPair<FString, UObject*>& Pair : Result.Copies)
	{
		SurfMaterialSeedTestUtil::Drop(Pair.Value);
	}
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.RemovePath(Root / TEXT("Materials/Functions"));
	Reg.RemovePath(Root / TEXT("Materials/Instances"));
	Reg.RemovePath(Root / TEXT("MaterialInstances"));
	Reg.RemovePath(Root / TEXT("Materials/MPC"));
	Reg.RemovePath(Root / TEXT("Materials"));
	Reg.RemovePath(Root);
	return true;
}

// --- The calibration cube, from MapLink -----------------------------------------
//
// Runs only when ue/_run/seed_source.txt names MapLink's Content folder and the
// plugin has no cube yet: the cube is a static mesh, not a library asset, and
// it leans on nothing but the engine's grid material, so it is copied whole.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialImportPreviewMeshTest, "SurfacingPro.Materials.ImportPreviewMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialImportPreviewMeshTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
	if (!Plugin.IsValid())
	{
		AddError(TEXT("the plugin is not loaded"));
		return false;
	}
	const FString Gate = Plugin->GetBaseDir() / TEXT("_run/seed_source.txt");
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Gate))
	{
		AddInfo(TEXT("no seed source named; nothing to import"));
		return true;
	}
	Source = Source.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
	if (!Source.EndsWith(TEXT("/")))
	{
		Source += TEXT("/");
	}
	const FString Mount = TEXT("/MapLink/");
	if (!FPackageName::MountPointExists(Mount))
	{
		FPackageName::RegisterMountPoint(Mount, Source);
	}

	// The two cubes: the calibration cube, and the denser one for displacement. Each once.
	struct FCube
	{
		const TCHAR* Name;
		int32 MinSlots;
	};
	const FCube Cubes[] = { { TEXT("mat_cube"), 2 }, { TEXT("mat_cube_dsp"), 1 } };
	for (const FCube& Cube : Cubes)
	{
		const FString Dest = FString(TEXT("/SurfacingPro/Preview/")) + Cube.Name;
		if (FPackageName::DoesPackageExist(Dest))
		{
			AddInfo(FString::Printf(TEXT("%s is there already"), Cube.Name));
			continue;
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/MapLink/Materials/%s.%s"), Cube.Name, Cube.Name));
		if (!Mesh)
		{
			AddError(FString::Printf(TEXT("MapLink's %s could not be loaded from the seed source"), Cube.Name));
			continue;
		}
		UPackage* Package = CreatePackage(*Dest);
		UStaticMesh* Copy = Cast<UStaticMesh>(StaticDuplicateObject(Mesh, Package, Cube.Name));
		if (!Copy)
		{
			AddError(FString::Printf(TEXT("%s could not be copied"), Cube.Name));
			continue;
		}
		Copy->SetFlags(RF_Public | RF_Standalone);
		Copy->PostEditChange();
		Copy->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Copy);
		TestTrue(FString::Printf(TEXT("%s saved into the plugin"), Cube.Name), SurfMaterialSeed::SaveAsset(Copy));
		TestTrue(FString::Printf(TEXT("%s has its slots"), Cube.Name), Copy->GetStaticMaterials().Num() >= Cube.MinSlots);
	}
	return true;
}

// --- The mesh a material is shown on ----------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialPreviewMeshChoiceTest, "SurfacingPro.Materials.PreviewMeshChoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialPreviewMeshChoiceTest::RunTest(const FString& Parameters)
{
	const FString Folder = FString(SurfMaterialSeedTestUtil::TestRoot) / TEXT("Mesh");
	UMaterial* Plain = SurfMaterialSeedTestUtil::MakeAsset<UMaterial>(Folder, TEXT("M_Opq_SurfTest"));
	UMaterial* Dsp = SurfMaterialSeedTestUtil::MakeAsset<UMaterial>(Folder, TEXT("M_OpqDsp_SurfTest"));
	UMaterialInstanceConstant* OfDsp = SurfMaterialSeedTestUtil::MakeAsset<UMaterialInstanceConstant>(Folder, TEXT("MI_Stone_SurfTest"));
	UMaterialInstanceConstant* OfPlain = SurfMaterialSeedTestUtil::MakeAsset<UMaterialInstanceConstant>(Folder, TEXT("MI_Wall_SurfTest"));
	if (!Plain || !Dsp || !OfDsp || !OfPlain)
	{
		AddError(TEXT("could not make the materials"));
		SurfMaterialSeedTestUtil::Drop(Plain); SurfMaterialSeedTestUtil::Drop(Dsp); SurfMaterialSeedTestUtil::Drop(OfDsp); SurfMaterialSeedTestUtil::Drop(OfPlain);
		return false;
	}
	OfDsp->SetParentEditorOnly(Dsp, false);
	OfPlain->SetParentEditorOnly(Plain, false);

	TestFalse(TEXT("a plain master: the cube"), SurfMaterialPreview::UsesDisplacement(Plain));
	TestTrue(TEXT("a Dsp master: the displacement cube"), SurfMaterialPreview::UsesDisplacement(Dsp));
	TestTrue(TEXT("an instance of a Dsp master, whatever its own name"), SurfMaterialPreview::UsesDisplacement(OfDsp));
	TestFalse(TEXT("an instance of a plain master"), SurfMaterialPreview::UsesDisplacement(OfPlain));
	TestFalse(TEXT("null: the cube"), SurfMaterialPreview::UsesDisplacement(nullptr));

	if (GetDefault<USurfSettings>()->PreviewMeshDisplacement.IsNull() && GetDefault<USurfSettings>()->PreviewMesh.IsNull())
	{
		TestEqual(TEXT("the plugin's denser cube for displacement"), SurfMaterialPreview::MeshPathFor(OfDsp).ToString(), FString(TEXT("/SurfacingPro/Preview/mat_cube_dsp.mat_cube_dsp")));
		TestEqual(TEXT("the plugin's cube otherwise"), SurfMaterialPreview::MeshPathFor(OfPlain).ToString(), FString(TEXT("/SurfacingPro/Preview/mat_cube.mat_cube")));
	}

	SurfMaterialSeedTestUtil::Drop(OfDsp); SurfMaterialSeedTestUtil::Drop(OfPlain); SurfMaterialSeedTestUtil::Drop(Dsp); SurfMaterialSeedTestUtil::Drop(Plain);
	IAssetRegistry& Reg = SurfMaterialSeedTestUtil::Registry();
	Reg.RemovePath(Folder);
	Reg.RemovePath(FString(SurfMaterialSeedTestUtil::TestRoot));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
