// Copyright MapModelers. All Rights Reserved.
//
// Material instances as files (S194b-c): an instance made from its file,
// written back out, and the two told apart when one of them changes.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfMaterialFiles.h"
#include "SurfMaterialInfo.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfMaterialFilesTestUtil
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialFilesTest, "SurfacingPro.Library.MaterialFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialFilesTest::RunTest(const FString& Parameters)
{
	using namespace SurfLibrary;
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SurfLibTest/materials"));
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	const FString Root = TEXT("/Game/SurfacingPro/_SurfLibTestMI/");

	UMaterialInterface* Parent = ResolveParent(TEXT("/SurfacingPro/Materials/M_Opq_Simple"), Root);
	TestNotNull(TEXT("the parent by its path"), Parent);
	if (!Parent)
	{
		return false;
	}
	TestTrue(TEXT("and by its name"), ResolveParent(TEXT("M_Opq_Simple"), Root) == Parent);
	TestNull(TEXT("no parent by a name nobody has"), ResolveParent(TEXT("M_Nobody_Has_This"), Root));

	TArray<UObject*> Made;

	// A file: a scalar, the surfacing defaults.
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> Guids;
	Parent->GetAllScalarParameterInfo(ScalarInfos, Guids);
	FSurfMaterialFile File;
	File.Name = TEXT("MI_LibFileTest");
	File.Parent = TEXT("/SurfacingPro/Materials/M_Opq_Simple");
	if (ScalarInfos.Num() > 0)
	{
		File.Scalars.Add(ScalarInfos[0].Name.ToString(), 0.37);
	}
	File.Scalars.Add(TEXT("NoSuchParameter"), 1.0);
	File.TileM = FVector2D(3.0, 3.0);
	File.Use = TEXT("Opaque");
	FString Error;
	TestTrue(TEXT("the file written"), SaveMaterial(File, MaterialFileFor(Scratch, File.Name), Error));
	TestTrue(TEXT("not made yet"), SyncOf(File, Root) == EMaterialSync::NotCreated);

	FString Line;
	TestTrue(TEXT("made from its file"), ApplyMaterial(File, Root, /*bSave*/ false, Line));
	AddInfo(Line);
	TestTrue(TEXT("a parameter the parent lacks is named"), Line.Contains(TEXT("NoSuchParameter")));
	UMaterialInstanceConstant* Instance = FindInstance(File, Root);
	TestNotNull(TEXT("the instance"), Instance);
	if (!Instance)
	{
		return false;
	}
	Made.Add(Instance);
	TestTrue(TEXT("of its parent"), Instance->Parent == Parent);
	TestEqual(TEXT("named as the file says"), Instance->GetName(), File.Name);
	if (ScalarInfos.Num() > 0)
	{
		float Value = 0.0f;
		TestTrue(TEXT("the scalar set"), Instance->GetScalarParameterValue(ScalarInfos[0], Value) && FMath::IsNearlyEqual(Value, 0.37f, 1.e-4f));
	}
	const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Instance);
	TestTrue(TEXT("the tile size"), Info && Info->DefaultScale.Equals(FVector2D(3.0, 3.0)));
	TestFalse(TEXT("the file records the instance"), File.Asset.IsEmpty() || File.SyncedAt.IsEmpty());

	Instance->GetOutermost()->SetDirtyFlag(false);
	TestTrue(TEXT("in step once made"), SyncOf(File, Root) == EMaterialSync::InSync);

	// Out again.
	FSurfMaterialFile Out;
	TestTrue(TEXT("exported"), ExportMaterial(Instance, Scratch / TEXT("out"), Root, Out, Error));
	TestEqual(TEXT("the parent by name"), Out.Parent, FString(TEXT("M_Opq_Simple")));
	TestTrue(TEXT("the tile size out"), Out.TileM.Equals(FVector2D(3.0, 3.0)));
	TestEqual(TEXT("the use out"), Out.Use, FString(TEXT("Opaque")));
	if (ScalarInfos.Num() > 0)
	{
		TestTrue(TEXT("the scalar out"), FMath::IsNearlyEqual(Out.Scalars.FindRef(ScalarInfos[0].Name.ToString()), 0.37, 1.e-4));
	}
	TestFalse(TEXT("what the parent lacks is not out"), Out.Scalars.Contains(TEXT("NoSuchParameter")));
	FSurfMaterialFile Back;
	TestTrue(TEXT("and the exported file reads"), LoadMaterial(Out.File, Back, Error) && Back.Name == Out.Name && Back.Scalars.Num() == Out.Scalars.Num());

	// The two told apart.
	IFileManager::Get().SetTimeStamp(*File.File, FDateTime::UtcNow() + FTimespan::FromMinutes(5.0));
	TestTrue(TEXT("a newer file is the file's change"), SyncOf(File, Root) == EMaterialSync::FileNewer);
	Instance->MarkPackageDirty();
	TestTrue(TEXT("and with the instance changed too, both"), SyncOf(File, Root) == EMaterialSync::Both);
	IFileManager::Get().SetTimeStamp(*File.File, FDateTime::UtcNow() - FTimespan::FromMinutes(5.0));
	TestTrue(TEXT("the instance alone"), SyncOf(File, Root) == EMaterialSync::AssetNewer);

	for (UObject* Asset : Made)
	{
		SurfMaterialFilesTestUtil::Drop(Asset);
	}
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	return true;
}

#endif
