// Copyright MapModelers. All Rights Reserved.
//
// Textures (T-a): the container, the naming, the record, and a file brought
// in from disk as one of ours - conformed, or linked where it is.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfTextureLib.h"
#include "SurfTgaWriter.h"
#include "SurfMaterialsView.h"
#include "SurfTexturesView.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfTextureTestUtil
{
	static FString ScratchDir()
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SurfTexTest"));
		FPaths::NormalizeDirectoryName(Dir);
		return Dir;
	}

	/** A small PNG of one colour, with or without a varying alpha, written to Path. */
	static bool WritePng(const FString& Path, int32 Width, int32 Height, FColor Colour, bool bVaryAlpha)
	{
		TArray<uint8> Pixels;
		Pixels.SetNumUninitialized(Width * Height * 4);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				uint8* P = Pixels.GetData() + (Y * Width + X) * 4;
				P[0] = Colour.B;
				P[1] = Colour.G;
				P[2] = Colour.R;
				P[3] = bVaryAlpha ? static_cast<uint8>((X * 255) / FMath::Max(1, Width - 1)) : 255;
			}
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num(), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8> Bytes = Png->GetCompressed();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
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
}

// --- The container ----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureTgaWriterTest, "SurfacingPro.Textures.TgaWriter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureTgaWriterTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the writer's own round trip holds"), FSurfTgaWriter::RunSelfTest());

	// A surface written twice: the second write is skipped, byte for byte.
	FSurfTextureSurface Surface;
	Surface.Width = 4;
	Surface.Height = 4;
	Surface.BGRA.SetNumZeroed(4 * 4 * 4);
	for (int64 I = 0; I < 16; ++I)
	{
		Surface.BGRA[I * 4 + 2] = static_cast<uint8>(I * 16);
		Surface.BGRA[I * 4 + 3] = 255;
	}
	const FString Path = SurfTextureTestUtil::ScratchDir() / TEXT("writer/T_Sp_Ramp.tga");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FSurfTgaWriteResult First = FSurfTgaWriter::WriteSurface(Surface, Path, false);
	TestTrue(TEXT("written"), First.bSuccess);
	FString Why;
	TestTrue(TEXT("and it is the locked container"), FSurfTgaWriter::IsConformContainer(Path, Why));
	const FSurfTgaWriteResult Second = FSurfTgaWriter::WriteSurface(Surface, Path, false);
	TestTrue(TEXT("a second write is skipped, the bytes being the same"), Second.bSuccess && Second.bSkippedIdentical);

	// A PNG in a .tga's clothes is not a container.
	const FString Fake = SurfTextureTestUtil::ScratchDir() / TEXT("writer/fake.tga");
	SurfTextureTestUtil::WritePng(Fake, 2, 2, FColor::Red, false);
	TestFalse(TEXT("a renamed PNG is refused"), FSurfTgaWriter::IsConformContainer(Fake, Why));

	IFileManager::Get().DeleteDirectory(*(SurfTextureTestUtil::ScratchDir() / TEXT("writer")), false, true);
	return true;
}

// --- The naming -------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureNamingTest, "SurfacingPro.Textures.Naming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureNamingTest::RunTest(const FString& Parameters)
{
	using namespace SurfTextures;
	TestEqual(TEXT("a file name is its stem"), StemFrom(TEXT("C:/Tex/Bricks Old_N.png")), FString(TEXT("Bricks_Old_N")));
	TestEqual(TEXT("our prefix is not doubled"), StemFrom(TEXT("T_Sp_Bricks.tga")), FString(TEXT("Bricks")));
	TestEqual(TEXT("MapLink's is dropped too"), StemFrom(TEXT("T_Mk_Bricks.tga")), FString(TEXT("Bricks")));
	TestEqual(TEXT("a plain T_ is dropped"), StemFrom(TEXT("T_Bricks.tga")), FString(TEXT("Bricks")));
	TestEqual(TEXT("odd characters fold into one underscore"), StemFrom(TEXT("wall  (2)--top.jpg")), FString(TEXT("wall_2_top")));
	TestEqual(TEXT("nothing is Texture"), StemFrom(TEXT("___.png")), FString(TEXT("Texture")));
	TestEqual(TEXT("the asset name"), AssetNameFor(TEXT("Bricks")), FString(TEXT("T_Sp_Bricks")));
	TestTrue(TEXT("ours"), IsOurs(TEXT("T_Sp_Bricks_N")));
	TestFalse(TEXT("not ours: the bare prefix"), IsOurs(TEXT("T_Sp_")));
	TestFalse(TEXT("not ours: MapLink's"), IsOurs(TEXT("T_Mk_Bricks")));
	TestFalse(TEXT("not ours: a dash"), IsOurs(TEXT("T_Sp_Bricks-N")));

	TestTrue(TEXT("_N is a normal map"), KindFromName(TEXT("T_Sp_Bricks_N")) == ESurfTextureKind::Normal);
	TestTrue(TEXT("_Normal too"), KindFromName(TEXT("Bricks_Normal.png")) == ESurfTextureKind::Normal);
	TestTrue(TEXT("_R is linear"), KindFromName(TEXT("T_Sp_Bricks_R")) == ESurfTextureKind::Linear);
	TestTrue(TEXT("_AO is linear"), KindFromName(TEXT("Bricks_AO")) == ESurfTextureKind::Linear);
	TestTrue(TEXT("_H is linear"), KindFromName(TEXT("Bricks_H")) == ESurfTextureKind::Linear);
	TestTrue(TEXT("_ORM is linear"), KindFromName(TEXT("Bricks_ORM")) == ESurfTextureKind::Linear);
	TestTrue(TEXT("_D is colour"), KindFromName(TEXT("Bricks_D")) == ESurfTextureKind::Colour);
	TestTrue(TEXT("no suffix is colour"), KindFromName(TEXT("Bricks")) == ESurfTextureKind::Colour);
	TestTrue(TEXT("the source path"), SourcePathFor(TEXT("C:/P/SurfacingPro"), TEXT("T_Sp_X")).EndsWith(TEXT("SurfacingPro/Textures/T_Sp_X.tga")));
	TestEqual(TEXT("the content folder"), ContentFolder(TEXT("/Game/SurfacingPro/")), FString(TEXT("/Game/SurfacingPro/Textures")));
	return true;
}

// --- The record -------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTexturePolicyTest, "SurfacingPro.Textures.Policy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTexturePolicyTest::RunTest(const FString& Parameters)
{
	using namespace SurfTextures;
	const FString Root = SurfTextureTestUtil::ScratchDir() / TEXT("policy");

	FSurfTexturePolicy Policy;
	TestTrue(TEXT("no record is an empty record"), Policy.Load(Root));
	TestEqual(TEXT("with nothing in it"), Policy.Entries().Num(), 0);

	FSurfTexturePolicyEntry Line;
	Line.AssetPath = TEXT("/Game/X/T_Sp_B.T_Sp_B");
	Line.bSRGB = false;
	Line.Compression = TEXT("TC_Normalmap");
	Line.LastConform = TEXT("2026-09-14T20:00:00Z");
	Line.bLossyReduction = true;
	Line.Note = TEXT("conform");
	Policy.Set(Line);
	Line.AssetPath = TEXT("/Game/X/T_Sp_A.T_Sp_A");
	Line.bSRGB = true;
	Line.bLossyReduction = false;
	Line.Note = TEXT("link");
	Policy.Set(Line);
	Line.Note = TEXT("link again");
	Policy.Set(Line);
	TestEqual(TEXT("setting the same path again replaces"), Policy.Entries().Num(), 2);
	TestTrue(TEXT("saved"), Policy.Save(Root));
	TestTrue(TEXT("as texture_policy.json"), FPaths::FileExists(FSurfTexturePolicy::PathFor(Root)));

	FSurfTexturePolicy Again;
	TestTrue(TEXT("read back"), Again.Load(Root));
	TestEqual(TEXT("both lines"), Again.Entries().Num(), 2);
	const FSurfTexturePolicyEntry* B = Again.Find(TEXT("/Game/X/T_Sp_B.T_Sp_B"));
	TestNotNull(TEXT("B is there"), B);
	TestTrue(TEXT("with its values"), B && !B->bSRGB && B->Compression == TEXT("TC_Normalmap") && B->bLossyReduction && B->Note == TEXT("conform"));
	const FSurfTexturePolicyEntry* A = Again.Find(TEXT("/Game/X/T_Sp_A.T_Sp_A"));
	TestTrue(TEXT("A took the last note"), A && A->Note == TEXT("link again"));
	TestTrue(TEXT("sorted by path"), Again.Entries()[0].AssetPath < Again.Entries()[1].AssetPath);
	TestTrue(TEXT("removed"), Again.Remove(TEXT("/Game/X/T_Sp_A.T_Sp_A")));
	TestNull(TEXT("and gone"), Again.Find(TEXT("/Game/X/T_Sp_A.T_Sp_A")));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

// --- Import ------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureImportTest, "SurfacingPro.Textures.Import",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureImportTest::RunTest(const FString& Parameters)
{
	using namespace SurfTextures;
	const FString Scratch = SurfTextureTestUtil::ScratchDir();
	const FString Root = Scratch / TEXT("root");
	const FString LibraryRoot = TEXT("/Game/SurfacingPro/_SurfTexTest");

	// Two files: a normal map with a space in its name, and a colour with a varying alpha.
	const FString NormalFile = Scratch / TEXT("src/Bricks Old_N.png");
	const FString ColourFile = Scratch / TEXT("src/Bricks_D.png");
	if (!SurfTextureTestUtil::WritePng(NormalFile, 8, 8, FColor(128, 128, 255), false)
		|| !SurfTextureTestUtil::WritePng(ColourFile, 8, 4, FColor(200, 90, 40), true))
	{
		AddError(TEXT("could not write the source files"));
		return false;
	}

	FSurfTextureImportOptions Options;
	Options.LibraryRoot = LibraryRoot;
	Options.SourceRootOverride = Root;
	Options.bConform = true;
	Options.MaxSize = 4;
	Options.bSave = false;

	TArray<FString> Files;
	Files.Add(NormalFile);
	Files.Add(ColourFile);
	FSurfTextureImportReport Report = Import(Files, Options);
	for (const FSurfTextureImportEntry& Entry : Report.Entries)
	{
		if (!Entry.Error.IsEmpty())
		{
			AddError(Entry.Error);
		}
		for (const FString& Warning : Entry.Warnings)
		{
			AddInfo(Warning);
		}
	}
	TestEqual(TEXT("two came in"), Report.Imported, 2);
	TestEqual(TEXT("none failed"), Report.Failed, 0);
	AddInfo(Report.Line());

	TArray<UObject*> Made;
	if (Report.Entries.Num() == 2 && Report.Imported == 2)
	{
		const FSurfTextureImportEntry& Normal = Report.Entries[0];
		const FSurfTextureImportEntry& Colour = Report.Entries[1];
		Made.Add(Normal.Texture);
		Made.Add(Colour.Texture);

		TestEqual(TEXT("named as ours, from the file's stem"), Normal.AssetPath, LibraryRoot + TEXT("/Textures/T_Sp_Bricks_Old_N.T_Sp_Bricks_Old_N"));
		TestTrue(TEXT("read as a normal map"), Normal.Kind == ESurfTextureKind::Normal);
		TestTrue(TEXT("and set as one"), Normal.Texture && !Normal.Texture->SRGB && Normal.Texture->CompressionSettings == TC_Normalmap);
		TestTrue(TEXT("conformed"), Normal.Status == ESurfTextureStatus::Conform);
		TestTrue(TEXT("its TGA is under the root"), Normal.SourceAfter == SourcePathFor(Root, TEXT("T_Sp_Bricks_Old_N")) && FPaths::FileExists(Normal.SourceAfter));
		FString Why;
		TestTrue(TEXT("in the locked container"), FSurfTgaWriter::IsConformContainer(Normal.SourceAfter, Why));
		FSurfTgaHeaderInfo Header;
		TestTrue(TEXT("scaled to the cap"), FSurfTgaWriter::ReadHeader(Normal.SourceAfter, Header) && Header.Width == 4 && Header.Height == 4);
		TestTrue(TEXT("scaling is recorded"), Normal.bResized);
		TestTrue(TEXT("and the asset reads the TGA"), Normal.Texture && Normal.Texture->AssetImportData
			&& FPaths::ConvertRelativePathToFull(Normal.Texture->AssetImportData->GetFirstFilename()).Equals(Normal.SourceAfter, ESearchCase::IgnoreCase));
		TestTrue(TEXT("so the registry sees it conform"), Normal.Texture && StatusOf(FAssetData(Normal.Texture), Root) == ESurfTextureStatus::Conform);

		TestTrue(TEXT("the colour is colour"), Colour.Kind == ESurfTextureKind::Colour && Colour.Texture && Colour.Texture->SRGB);
		TestTrue(TEXT("its alpha went into a texture of its own"), !Colour.AlphaAssetPath.IsEmpty());
		if (!Colour.AlphaAssetPath.IsEmpty())
		{
			UTexture2D* Alpha = FindObject<UTexture2D>(nullptr, *Colour.AlphaAssetPath);
			TestNotNull(TEXT("which exists"), Alpha);
			TestTrue(TEXT("named after it"), Colour.AlphaAssetPath.EndsWith(TEXT("T_Sp_Bricks_D_A.T_Sp_Bricks_D_A")));
			TestTrue(TEXT("as linear data"), Alpha && !Alpha->SRGB);
			Made.Add(Alpha);
		}

		// The record.
		FSurfTexturePolicy Policy;
		Policy.Load(Root);
		const FSurfTexturePolicyEntry* Line = Policy.Find(Normal.AssetPath);
		TestNotNull(TEXT("the record has the normal map"), Line);
		TestTrue(TEXT("as conformed and scaled"), Line && Line->Note.StartsWith(TEXT("conform")) && Line->bLossyReduction && !Line->bSRGB);

		// Gone from disk: broken.
		IFileManager::Get().Delete(*Normal.SourceAfter);
		TestTrue(TEXT("its file gone, the texture is broken"), Normal.Texture && StatusOf(FAssetData(Normal.Texture), Root) == ESurfTextureStatus::Broken);
	}

	// Linked: the file stays where it is; outside the project it is local only.
	const FString Outside = FPaths::ConvertRelativePathToFull(FString(FPlatformProcess::UserTempDir())) / TEXT("SurfTexTest/Loose_R.png");
	if (SurfTextureTestUtil::WritePng(Outside, 4, 4, FColor(60, 60, 60), false))
	{
		Options.bConform = false;
		TArray<FString> Linked;
		Linked.Add(Outside);
		const FSurfTextureImportReport LinkReport = Import(Linked, Options);
		TestEqual(TEXT("the linked one came in"), LinkReport.Imported, 1);
		if (LinkReport.Entries.Num() == 1 && LinkReport.Entries[0].Texture)
		{
			const FSurfTextureImportEntry& Loose = LinkReport.Entries[0];
			Made.Add(Loose.Texture);
			TestTrue(TEXT("read as linear"), Loose.Kind == ESurfTextureKind::Linear && !Loose.Texture->SRGB);
			TestTrue(TEXT("local only, outside the project"), Loose.Status == ESurfTextureStatus::LocalOnly);
			TestTrue(TEXT("the registry agrees"), StatusOf(FAssetData(Loose.Texture), Root) == ESurfTextureStatus::LocalOnly);
			TestTrue(TEXT("reading the file where it was"), FPaths::ConvertRelativePathToFull(Loose.Texture->AssetImportData->GetFirstFilename()).Equals(Outside, ESearchCase::IgnoreCase));
			TestFalse(TEXT("no TGA was written for it"), FPaths::FileExists(SourcePathFor(Root, TEXT("T_Sp_Loose_R"))));
		}
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Outside), false, true);
	}

	// A file that is not an image, and one that is not there.
	Options.bConform = true;
	TArray<FString> Bad;
	Bad.Add(Scratch / TEXT("src/notes.txt"));
	Bad.Add(Scratch / TEXT("src/missing.png"));
	FFileHelper::SaveStringToFile(TEXT("not an image"), *Bad[0]);
	const FSurfTextureImportReport BadReport = Import(Bad, Options);
	TestEqual(TEXT("nothing came in"), BadReport.Imported, 0);
	TestEqual(TEXT("both refused"), BadReport.Failed, 2);
	TestTrue(TEXT("the line says why"), BadReport.Line().StartsWith(TEXT("import: ")));

	// Onto an instance.
	UPackage* Package = CreatePackage(*(LibraryRoot + TEXT("/MI/MI_SurfTexTest")));
	UMaterialInstanceConstant* Instance = Package ? NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_SurfTexTest"), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (Instance && Made.Num() > 0 && Made[0])
	{
		Instance->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface), false);
		FString Error;
		TestTrue(TEXT("a texture goes on a parameter"), SetTextureParameter(Instance, FName(TEXT("BaseColor")), Cast<UTexture>(Made[0]), false, Error));
		bool bFound = false;
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			bFound |= Value.ParameterInfo.Name == FName(TEXT("BaseColor")) && Value.ParameterValue == Made[0];
		}
		TestTrue(TEXT("and is there"), bFound);
		TestFalse(TEXT("no instance, no setting"), SetTextureParameter(nullptr, FName(TEXT("BaseColor")), Cast<UTexture>(Made[0]), false, Error));
		TestTrue(TEXT("with the reason"), Error.StartsWith(TEXT("texture: pick")));
		Made.Add(Instance);
	}

	for (UObject* Object : Made)
	{
		SurfTextureTestUtil::Drop(Object);
	}
	IAssetRegistry& Reg = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Reg.RemovePath(LibraryRoot + TEXT("/Textures"));
	Reg.RemovePath(LibraryRoot + TEXT("/MI"));
	Reg.RemovePath(LibraryRoot);
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	return true;
}

// --- Zoom ---------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureZoomTest, "SurfacingPro.Textures.Zoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureZoomTest::RunTest(const FString& Parameters)
{
	// Rows first, then tiles that grow; a wheel step moves one level and stops at the ends.
	const int32 Levels = SurfTexturesLib::ZoomLevels();
	TestTrue(TEXT("a few levels"), Levels >= 3);
	TestFalse(TEXT("level 0 is rows"), SurfTexturesLib::ZoomIsTiles(0));
	TestTrue(TEXT("level 1 is tiles"), SurfTexturesLib::ZoomIsTiles(1));
	for (int32 Level = 1; Level < Levels; ++Level)
	{
		TestTrue(TEXT("each level is larger than the last"), SurfTexturesLib::ZoomSize(Level) > SurfTexturesLib::ZoomSize(Level - 1));
	}
	TestEqual(TEXT("below the levels reads as the first"), SurfTexturesLib::ZoomSize(-3), SurfTexturesLib::ZoomSize(0));
	TestEqual(TEXT("above the levels reads as the last"), SurfTexturesLib::ZoomSize(99), SurfTexturesLib::ZoomSize(Levels - 1));
	TestEqual(TEXT("wheel up is one level bigger"), SurfTexturesLib::StepZoom(0, 1.0f), 1);
	TestEqual(TEXT("wheel down is one level smaller"), SurfTexturesLib::StepZoom(2, -1.0f), 1);
	TestEqual(TEXT("no wheel, no step"), SurfTexturesLib::StepZoom(2, 0.0f), 2);
	TestEqual(TEXT("down from rows stays on rows"), SurfTexturesLib::StepZoom(0, -1.0f), 0);
	TestEqual(TEXT("up from the largest stays there"), SurfTexturesLib::StepZoom(Levels - 1, 1.0f), Levels - 1);
	return true;
}

// --- The marks ------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureMarksTest, "SurfacingPro.Textures.Marks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureMarksTest::RunTest(const FString& Parameters)
{
	// A file on disk is "in the library" when a texture reads it where it is, or one is named after it.
	SurfTexturesLib::FSurfLibraryIndex Index;
	Index.ByName.Add(TEXT("T_Sp_Brick_N"), TEXT("/Game/SurfacingPro/Textures/T_Sp_Brick_N.T_Sp_Brick_N"));
	Index.BySource.Add(SurfTexturesLib::NormalisedFile(TEXT("C:/Art/Loose/moss.png")), TEXT("T_Sp_moss"));

	TestEqual(TEXT("a file a texture reads is linked, whatever its case"), SurfTexturesLib::InLibrary(Index, TEXT("c:\\Art\\LOOSE\\Moss.PNG")), FString(TEXT("T_Sp_moss")));
	TestTrue(TEXT("and says so"), SurfTexturesLib::IsLinked(Index, TEXT("C:/Art/Loose/moss.png")));
	TestEqual(TEXT("a file a texture is named after is in, from any folder"), SurfTexturesLib::InLibrary(Index, TEXT("D:/elsewhere/brick_N.tga")), FString(TEXT("T_Sp_Brick_N")));
	TestFalse(TEXT("but not linked"), SurfTexturesLib::IsLinked(Index, TEXT("D:/elsewhere/brick_N.tga")));
	TestTrue(TEXT("a stranger is not in"), SurfTexturesLib::InLibrary(Index, TEXT("D:/elsewhere/stone.png")).IsEmpty());

	// Folders: a drive keeps its slash, anything else loses it.
	TestEqual(TEXT("a drive root"), SurfTexturesLib::NormalisedFolder(TEXT("C:\\")), FString(TEXT("C:/")));
	TestEqual(TEXT("a bare drive"), SurfTexturesLib::NormalisedFolder(TEXT("C:")), FString(TEXT("C:/")));
	TestEqual(TEXT("a folder"), SurfTexturesLib::NormalisedFolder(TEXT("C:\\Art\\Loose\\")), FString(TEXT("C:/Art/Loose")));
	return true;
}

// --- Keeping the library right (T-b) --------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureConformTest, "SurfacingPro.Textures.Conform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureConformTest::RunTest(const FString& Parameters)
{
	using namespace SurfTextures;
	const FString Scratch = SurfTextureTestUtil::ScratchDir() + TEXT("_conform");
	const FString Root = Scratch / TEXT("root");
	const FString LibraryRoot = TEXT("/Game/SurfacingPro/_SurfConformTest");

	// A linked texture, its file inside the project: portable.
	FString Loose = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SurfConformTest/wall_D.png"));
	FPaths::NormalizeFilename(Loose);
	if (!SurfTextureTestUtil::WritePng(Loose, 8, 8, FColor(120, 100, 80), false))
	{
		AddError(TEXT("could not write the source file"));
		return false;
	}

	FSurfTextureImportOptions Options;
	Options.LibraryRoot = LibraryRoot;
	Options.SourceRootOverride = Root;
	Options.bConform = false;
	Options.bSave = false;
	TArray<FString> Files;
	Files.Add(Loose);
	const FSurfTextureImportReport In = Import(Files, Options);
	TestEqual(TEXT("one came in, linked"), In.Imported, 1);
	UTexture2D* Texture = In.Entries.Num() > 0 ? In.Entries[0].Texture : nullptr;
	if (!Texture)
	{
		AddError(TEXT("no texture to work on: ") + In.Line());
		return false;
	}
	TestTrue(TEXT("portable, inside the project"), StatusOf(FAssetData(Texture), Root) == ESurfTextureStatus::Portable);
	TestFalse(TEXT("nothing is stale yet"), IsStale(FAssetData(Texture)));

	// Conform: its own pixels become our TGA under the root, read from there.
	TArray<UTexture2D*> Textures;
	Textures.Add(Texture);
	const FSurfTextureImportReport Done = Conform(Textures, Options);
	for (const FSurfTextureImportEntry& Entry : Done.Entries)
	{
		if (!Entry.Error.IsEmpty())
		{
			AddError(Entry.Error);
		}
		for (const FString& Warning : Entry.Warnings)
		{
			AddInfo(Warning);
		}
	}
	TestEqual(TEXT("one conformed"), Done.Imported, 1);
	TestTrue(TEXT("the line says so"), Done.Line().StartsWith(TEXT("conformed ")));
	const FString Tga = SourcePathFor(Root, TEXT("T_Sp_wall_D"));
	TestTrue(TEXT("its TGA is under the root"), FPaths::FileExists(Tga));
	TestTrue(TEXT("and it reads it: conform"), StatusOf(FAssetData(Texture), Root) == ESurfTextureStatus::Conform);
	FSurfTexturePolicy Policy;
	Policy.Load(Root);
	const FSurfTexturePolicyEntry* Line = Policy.Find(Texture->GetPathName());
	TestTrue(TEXT("recorded as conform"), Line && Line->Note.StartsWith(TEXT("conform")));

	// The TGA changed on disk: stale; read again: current.
	IFileManager::Get().SetTimeStamp(*Tga, FDateTime::UtcNow() + FTimespan::FromHours(1.0));
	TestTrue(TEXT("a newer file is stale"), IsStale(FAssetData(Texture)));
	FString Error;
	TestTrue(TEXT("read again"), Reimport(Texture, false, Error));
	TestTrue(TEXT("with no complaint"), Error.IsEmpty());
	TestFalse(TEXT("and current"), IsStale(FAssetData(Texture)));

	// A stray TGA under the root is an orphan; the one read is not.
	const FString Stray = SourceDir(Root) / TEXT("T_Sp_Stray.tga");
	TestTrue(TEXT("a stray is written"), IFileManager::Get().Copy(*Stray, *Tga) == COPY_OK);
	TArray<FString> Orphans;
	ListOrphans(LibraryRoot, Root, Orphans);
	TestEqual(TEXT("one orphan"), Orphans.Num(), 1);
	TestTrue(TEXT("the stray"), Orphans.Num() == 1 && FPaths::GetCleanFilename(Orphans[0]) == TEXT("T_Sp_Stray.tga"));

	// Read another file: back to one where it is; its TGA is then a stray too.
	const FSurfTextureImportReport Linked = Link(Texture, Loose, Options);
	TestEqual(TEXT("linked again"), Linked.Imported, 1);
	TestTrue(TEXT("the line says so"), Linked.Line().StartsWith(TEXT("linked ")));
	TestTrue(TEXT("portable again"), StatusOf(FAssetData(Texture), Root) == ESurfTextureStatus::Portable);
	ListOrphans(LibraryRoot, Root, Orphans);
	TestEqual(TEXT("two strays now"), Orphans.Num(), 2);
	Policy.Load(Root);
	Line = Policy.Find(Texture->GetPathName());
	TestTrue(TEXT("recorded as a link"), Line && Line->Note == TEXT("link"));

	// Refusals say why.
	TArray<UTexture2D*> None;
	None.Add(nullptr);
	TestEqual(TEXT("no texture, nothing conformed"), Conform(None, Options).Failed, 1);
	const FSurfTextureImportReport Missing = Link(Texture, Scratch / TEXT("nope.png"), Options);
	TestEqual(TEXT("a missing file is not linked"), Missing.Failed, 1);
	TestTrue(TEXT("and the line says so"), Missing.Line().StartsWith(TEXT("link: ")));
	TestFalse(TEXT("nothing to read again"), Reimport(nullptr, false, Error));
	TestTrue(TEXT("with the reason"), Error.StartsWith(TEXT("reimport: ")));

	SurfTextureTestUtil::Drop(Texture);
	IAssetRegistry& Reg = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Reg.RemovePath(LibraryRoot + TEXT("/Textures"));
	Reg.RemovePath(LibraryRoot);
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Loose), false, true);
	return true;
}

// --- Sort ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureSortTest, "SurfacingPro.Textures.Sort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureSortTest::RunTest(const FString& Parameters)
{
	auto Make = [](const TCHAR* Name, bool bFolder, int64 Bytes, int32 Day)
	{
		TSharedRef<FSurfDiskEntry> E = MakeShared<FSurfDiskEntry>();
		E->Name = Name;
		E->Shown = Name;
		E->bFolder = bFolder;
		E->Bytes = Bytes;
		E->Modified = FDateTime(2026, 9, Day);
		return E;
	};
	TArray<TSharedPtr<FSurfDiskEntry>> Entries;
	Entries.Add(Make(TEXT("wall_N.png"), false, 300, 3));
	Entries.Add(Make(TEXT("Sub"), false, 0, 1));
	Entries.Add(Make(TEXT("brick.png"), false, 900, 1));
	Entries.Add(Make(TEXT("Alpha"), true, 0, 2));
	Entries[1]->bFolder = true;
	auto NameAt = [&Entries](int32 I) { return Entries[I]->Name; };

	SurfTexturesLib::SortEntries(Entries, 0);
	TestTrue(TEXT("folders first, by name"), NameAt(0) == TEXT("Alpha") && NameAt(1) == TEXT("Sub") && NameAt(2) == TEXT("brick.png") && NameAt(3) == TEXT("wall_N.png"));
	SurfTexturesLib::SortEntries(Entries, 1);
	TestTrue(TEXT("newest first"), NameAt(2) == TEXT("wall_N.png") && NameAt(3) == TEXT("brick.png"));
	SurfTexturesLib::SortEntries(Entries, 2);
	TestTrue(TEXT("largest first"), NameAt(2) == TEXT("brick.png") && NameAt(3) == TEXT("wall_N.png"));
	SurfTexturesLib::SortEntries(Entries, 3);
	TestTrue(TEXT("by kind: colour before normal"), NameAt(2) == TEXT("brick.png") && NameAt(3) == TEXT("wall_N.png"));
	TestTrue(TEXT("folders still first"), Entries[0]->bFolder && Entries[1]->bFolder);
	TestEqual(TEXT("four orders"), SurfTexturesLib::SortOrders(), 4);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
