// Copyright MapModelers. All Rights Reserved.
//
// Texture suggestions (M-E4): the siblings of a picked texture offered to the
// other parameters - learned from the library's other instances first, read
// off the parameter's name otherwise.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfTextureSuggest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfTextureSuggestTestUtil
{
	static const TCHAR* TestRoot = TEXT("/Game/SurfacingPro/_SurfSuggestTest");

	static IAssetRegistry& Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	template<typename T>
	static T* MakeAsset(const FString& Folder, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(Folder / Name));
		T* Asset = Package ? NewObject<T>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}
		return Asset;
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

	static void AddTextureParameter(UMaterial* Master, const TCHAR* Name)
	{
		UMaterialExpressionTextureSampleParameter2D* Expression = Cast<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		if (Expression)
		{
			Expression->ParameterName = FName(Name);
		}
	}

	static void Put(UMaterialInstanceConstant* Instance, const TCHAR* Parameter, UTexture* Texture)
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(Parameter)), Texture);
	}

	static const SurfTextureSuggest::FSurfTextureSuggestion* For(const TArray<SurfTextureSuggest::FSurfTextureSuggestion>& Suggestions, const TCHAR* Parameter)
	{
		return Suggestions.FindByPredicate([Parameter](const SurfTextureSuggest::FSurfTextureSuggestion& S) { return S.Parameter == FName(Parameter); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureSuggestTest, "SurfacingPro.Materials.Suggest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureSuggestTest::RunTest(const FString& Parameters)
{
	using namespace SurfTextureSuggest;

	// The name split.
	{
		FString Base;
		FString Suffix;
		Split(TEXT("T_Sp_Brick_N"), Base, Suffix);
		TestEqual(TEXT("base of _N"), Base, FString(TEXT("T_Sp_Brick")));
		TestEqual(TEXT("suffix N"), Suffix, FString(TEXT("N")));
		Split(TEXT("T_Sp_Brick_orm"), Base, Suffix);
		TestEqual(TEXT("suffix upper-cased"), Suffix, FString(TEXT("ORM")));
		TestEqual(TEXT("base of _orm"), Base, FString(TEXT("T_Sp_Brick")));
		Split(TEXT("T_Sp_Brick"), Base, Suffix);
		TestEqual(TEXT("no suffix: the name"), Base, FString(TEXT("T_Sp_Brick")));
		TestTrue(TEXT("no suffix: empty"), Suffix.IsEmpty());
		Split(TEXT("T_Sp_Wall_01"), Base, Suffix);
		TestEqual(TEXT("a number is not a suffix"), Base, FString(TEXT("T_Sp_Wall_01")));
	}

	// The name's say.
	{
		TArray<FString> Suffixes;
		SuffixesFor(FName(TEXT("Normal")), Suffixes);
		TestTrue(TEXT("Normal -> N first"), Suffixes.Num() > 0 && Suffixes[0] == TEXT("N"));
		SuffixesFor(FName(TEXT("Roughness")), Suffixes);
		TestTrue(TEXT("Roughness -> R first"), Suffixes.Num() > 0 && Suffixes[0] == TEXT("R"));
		SuffixesFor(FName(TEXT("BaseColor")), Suffixes);
		TestTrue(TEXT("BaseColor -> _D first (the diffuse), the bare name next"), Suffixes.Num() > 1 && Suffixes[0] == TEXT("D") && Suffixes[1].IsEmpty());
		SuffixesFor(FName(TEXT("MAT_01_DiffuseMap")), Suffixes);
		TestTrue(TEXT("his DiffuseMap -> _D first"), Suffixes.Num() > 0 && Suffixes[0] == TEXT("D"));
		SuffixesFor(FName(TEXT("MAT_01_NormalMap")), Suffixes);
		TestTrue(TEXT("his NormalMap -> N first"), Suffixes.Num() > 0 && Suffixes[0] == TEXT("N"));
		SuffixesFor(FName(TEXT("MAT_01_DisplacementMap")), Suffixes);
		TestTrue(TEXT("his DisplacementMap -> H first"), Suffixes.Num() > 0 && Suffixes[0] == TEXT("H"));
		SuffixesFor(FName(TEXT("Foo")), Suffixes);
		TestEqual(TEXT("an unknown name says nothing"), Suffixes.Num(), 0);
	}

	// A library: a master with three texture parameters, two sets of textures, an instance dressed in one of them.
	const FString Root = FString(SurfTextureSuggestTestUtil::TestRoot);
	const FString Textures = Root / TEXT("Textures");
	const FString Materials = Root / TEXT("Materials");

	UTexture2D* Brick = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Brick"));
	UTexture2D* BrickN = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Brick_N"));
	UTexture2D* BrickR = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Brick_R"));
	UTexture2D* Wood = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Wood"));
	UTexture2D* WoodN = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Wood_N"));
	UTexture2D* WoodORM = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Wood_ORM"));
	UMaterial* Master = SurfTextureSuggestTestUtil::MakeAsset<UMaterial>(Materials, TEXT("M_SurfSuggest"));
	UMaterialInstanceConstant* WoodInstance = SurfTextureSuggestTestUtil::MakeAsset<UMaterialInstanceConstant>(Materials, TEXT("MI_SurfWood"));
	UMaterialInstanceConstant* BrickInstance = SurfTextureSuggestTestUtil::MakeAsset<UMaterialInstanceConstant>(Materials, TEXT("MI_SurfBrick"));

	TArray<UObject*> All = { Brick, BrickN, BrickR, Wood, WoodN, WoodORM, Master, WoodInstance, BrickInstance };
	auto Cleanup = [&All, &Root, &Textures, &Materials]()
	{
		for (UObject* Asset : All)
		{
			SurfTextureSuggestTestUtil::Drop(Asset);
		}
		IAssetRegistry& Reg = SurfTextureSuggestTestUtil::Registry();
		Reg.RemovePath(Textures);
		Reg.RemovePath(Materials);
		Reg.RemovePath(Root);
	};
	if (All.Contains(nullptr))
	{
		AddError(TEXT("could not make the library"));
		Cleanup();
		return false;
	}

	SurfTextureSuggestTestUtil::AddTextureParameter(Master, TEXT("BaseColor"));
	SurfTextureSuggestTestUtil::AddTextureParameter(Master, TEXT("Normal"));
	SurfTextureSuggestTestUtil::AddTextureParameter(Master, TEXT("Roughness"));
	Master->PostEditChange();

	WoodInstance->SetParentEditorOnly(Master, false);
	SurfTextureSuggestTestUtil::Put(WoodInstance, TEXT("BaseColor"), Wood);
	SurfTextureSuggestTestUtil::Put(WoodInstance, TEXT("Normal"), WoodN);
	SurfTextureSuggestTestUtil::Put(WoodInstance, TEXT("Roughness"), WoodORM);
	WoodInstance->PostEditChange();

	BrickInstance->SetParentEditorOnly(Master, false);
	SurfTextureSuggestTestUtil::Put(BrickInstance, TEXT("BaseColor"), Brick);
	BrickInstance->PostEditChange();

	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Guids;
	BrickInstance->GetAllTextureParameterInfo(Infos, Guids);
	TestEqual(TEXT("the instance has the three texture parameters"), Infos.Num(), 3);

	// The brick colour was picked: the normal as the wood instance pairs it, the roughness by its name (the set has no _ORM).
	TArray<FSurfTextureSuggestion> Suggestions;
	Suggest(BrickInstance, FName(TEXT("BaseColor")), Root, Suggestions);
	TestEqual(TEXT("two siblings offered"), Suggestions.Num(), 2);
	const FSurfTextureSuggestion* ForNormal = SurfTextureSuggestTestUtil::For(Suggestions, TEXT("Normal"));
	TestTrue(TEXT("Normal <- T_Sp_Brick_N"), ForNormal && ForNormal->Texture.AssetName == FName(TEXT("T_Sp_Brick_N")));
	TestTrue(TEXT("as the wood instance pairs it"), ForNormal && ForNormal->Why.Contains(TEXT("MI_SurfWood")));
	const FSurfTextureSuggestion* ForRoughness = SurfTextureSuggestTestUtil::For(Suggestions, TEXT("Roughness"));
	TestTrue(TEXT("Roughness <- T_Sp_Brick_R"), ForRoughness && ForRoughness->Texture.AssetName == FName(TEXT("T_Sp_Brick_R")));
	TestTrue(TEXT("by its name, the set having no _ORM"), ForRoughness && ForRoughness->Why == TEXT("by its name"));
	TestNull(TEXT("nothing for the parameter just set"), SurfTextureSuggestTestUtil::For(Suggestions, TEXT("BaseColor")));

	// The normal put on: the colour is of the set already, only the roughness is left to offer.
	SurfTextureSuggestTestUtil::Put(BrickInstance, TEXT("Normal"), BrickN);
	BrickInstance->PostEditChange();
	Suggest(BrickInstance, FName(TEXT("Normal")), Root, Suggestions);
	TestEqual(TEXT("one left to offer"), Suggestions.Num(), 1);
	TestTrue(TEXT("and it is the roughness"), Suggestions.Num() == 1 && Suggestions[0].Parameter == FName(TEXT("Roughness")));

	// A texture of no set: nothing to offer.
	UTexture2D* Lone = SurfTextureSuggestTestUtil::MakeAsset<UTexture2D>(Textures, TEXT("T_Sp_Lone"));
	All.Add(Lone);
	SurfTextureSuggestTestUtil::Put(BrickInstance, TEXT("BaseColor"), Lone);
	BrickInstance->PostEditChange();
	Suggest(BrickInstance, FName(TEXT("BaseColor")), Root, Suggestions);
	TestEqual(TEXT("a texture with no siblings offers nothing"), Suggestions.Num(), 0);

	Cleanup();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
