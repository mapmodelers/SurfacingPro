// Copyright MapModelers. All Rights Reserved.
//
// The blockout set (M-F2): drawn by code, shipped in the plugin, each one
// the size it prints.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfMaterialBlockout.h"
#include "SurfMaterialInfo.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialBlockoutDrawTest, "SurfacingPro.Materials.BlockoutDraw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialBlockoutDrawTest::RunTest(const FString& Parameters)
{
	using namespace SurfMaterialBlockout;
	const int32 Side = TextureSize();
	const TArray<FSurfBlockoutKind>& All = Kinds();
	TestEqual(TEXT("seven kinds"), All.Num(), 7);

	// The font: a 1 lights the middle column, a 0 does not; the point is narrow.
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor::Black, Side * Side);
		const int32 Used = PrintGlyph(TEXT('1'), 10, 10, 2, FColor::White, Pixels, Side);
		TestEqual(TEXT("a digit is six cells wide"), Used, 12);
		TestTrue(TEXT("the 1's stem is lit"), Pixels[(10 + 3 * 2) * Side + (10 + 2 * 2)] == FColor::White);
		TestEqual(TEXT("the point is four cells wide"), PrintGlyph(TEXT('.'), 100, 10, 2, FColor::White, Pixels, Side), 8);
		TestEqual(TEXT("an unknown glyph prints nothing"), PrintGlyph(TEXT('x'), 100, 10, 2, FColor::White, Pixels, Side), 0);
	}

	for (const FSurfBlockoutKind& Kind : All)
	{
		TArray<FColor> Pixels;
		Draw(Kind, Pixels);
		TestEqual(*FString::Printf(TEXT("%s: the right size"), *Kind.Stem), Pixels.Num(), Side * Side);
		const FColor Base = Kind.Colour.ToFColor(true);
		TSet<uint32> Distinct;
		for (const FColor& C : Pixels)
		{
			Distinct.Add(C.DWColor());
		}
		switch (Kind.Shape)
		{
		case FSurfBlockoutKind::EShape::Fill:
			TestEqual(*FString::Printf(TEXT("%s: one colour"), *Kind.Stem), Distinct.Num(), 1);
			break;
		case FSurfBlockoutKind::EShape::Strip:
			TestTrue(*FString::Printf(TEXT("%s: bands, edges"), *Kind.Stem), Distinct.Num() >= 3);
			// The first band and the one after it differ.
			{
				const int32 Band = FMath::RoundToInt(Side * Kind.BandMetres / Kind.TileMetres);
				TestTrue(*FString::Printf(TEXT("%s: the bands alternate"), *Kind.Stem),
					Pixels[(Band / 2) * Side + Side / 2] != Pixels[(Band + Band / 2) * Side + Side / 2]);
			}
			break;
		case FSurfBlockoutKind::EShape::Grid:
			TestTrue(*FString::Printf(TEXT("%s: lines, a label"), *Kind.Stem), Distinct.Num() >= 4);
			TestTrue(*FString::Printf(TEXT("%s: the edge is inked"), *Kind.Stem), Pixels[0] != Base);
			// Somewhere in the middle third a glyph pixel is inked (the label).
			{
				bool bInk = false;
				const FColor Ink = (Kind.Colour * 0.28f).ToFColor(true);
				for (int32 Y = Side / 3; Y < Side * 2 / 3 && !bInk; ++Y)
				{
					for (int32 X = Side / 3; X < Side * 2 / 3; ++X)
					{
						if (Pixels[Y * Side + X] == Ink)
						{
							bInk = true;
							break;
						}
					}
				}
				TestTrue(*FString::Printf(TEXT("%s: the size is printed"), *Kind.Stem), bInk);
			}
			break;
		}
	}
	return true;
}

// --- The shipped set: authored once behind a gate, loaded every run ------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialBlockoutSetTest, "SurfacingPro.Materials.BlockoutSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialBlockoutSetTest::RunTest(const FString& Parameters)
{
	using namespace SurfMaterialBlockout;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
	const FString Gate = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("_run/author_blockout.txt") : FString();
	const bool bAuthor = !Gate.IsEmpty() && IFileManager::Get().FileExists(*Gate);

	FString Error;
	FSurfBlockoutAssets Assets = bAuthor ? Author(/*bSave*/ true, Error) : Load();
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	AddInfo(bAuthor ? TEXT("authored and saved") : TEXT("loaded the shipped set"));
	if (bAuthor)
	{
		TArray<FString> Saved;
		Saved.Add(FPackageName::LongPackageNameToFilename(MasterPackage(), FPackageName::GetAssetPackageExtension()));
		for (UTexture2D* T : Assets.Textures) { if (T) { Saved.Add(FPackageName::LongPackageNameToFilename(T->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())); } }
		for (UMaterialInstanceConstant* I : Assets.Instances) { if (I) { Saved.Add(FPackageName::LongPackageNameToFilename(I->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())); } }
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(Saved, /*bForceRescan*/ true);
	}

	TestNotNull(TEXT("the master"), Assets.Master);
	const TArray<FSurfBlockoutKind>& All = Kinds();
	TestEqual(TEXT("a texture a kind"), Assets.Textures.Num(), All.Num());
	TestEqual(TEXT("an instance a kind"), Assets.Instances.Num(), All.Num());
	for (int32 Index = 0; Index < All.Num(); ++Index)
	{
		const FSurfBlockoutKind& Kind = All[Index];
		UTexture2D* Texture = Assets.Textures.IsValidIndex(Index) ? Assets.Textures[Index] : nullptr;
		UMaterialInstanceConstant* Instance = Assets.Instances.IsValidIndex(Index) ? Assets.Instances[Index] : nullptr;
		TestNotNull(*FString::Printf(TEXT("%s: the texture"), *Kind.Stem), Texture);
		TestNotNull(*FString::Printf(TEXT("%s: the instance"), *Kind.Stem), Instance);
		if (!Texture || !Instance)
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: square, the size drawn"), *Kind.Stem), Texture->Source.GetSizeX() == TextureSize() && Texture->Source.GetSizeY() == TextureSize());
		TestTrue(*FString::Printf(TEXT("%s: an instance of the master"), *Kind.Stem), Instance->Parent == Assets.Master);
		UTexture* OnGrid = nullptr;
		Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Grid")), OnGrid);
		TestTrue(*FString::Printf(TEXT("%s: wears its texture"), *Kind.Stem), OnGrid == Texture);
		const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Instance);
		TestTrue(*FString::Printf(TEXT("%s: the tile is the size it prints"), *Kind.Stem), Info && FMath::IsNearlyEqual(Info->DefaultScale.X, Kind.TileMetres) && FMath::IsNearlyEqual(Info->DefaultScale.Y, Kind.TileMetres));
		TestTrue(*FString::Printf(TEXT("%s: in the plugin's Blockout folder"), *Kind.Stem), Instance->GetOutermost()->GetName().StartsWith(Folder()));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
