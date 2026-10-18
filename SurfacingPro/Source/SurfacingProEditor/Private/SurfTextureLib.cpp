// Copyright MapModelers. All Rights Reserved.

#include "SurfTextureLib.h"

#include "ScopedTransaction.h"

#include "SurfMaterialSeed.h"
#include "SurfMaterialsView.h"
#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfTgaWriter.h"
#include "SurfacingProModule.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace SurfTextures
{
	// --- Naming -----------------------------------------------------------

	FString Prefix()
	{
		return TEXT("T_Sp_");
	}

	FString StemFrom(const FString& FileOrAssetName)
	{
		FString Stem = FPaths::GetBaseFilename(FileOrAssetName);

		// Ours, MapLink's, or a plain texture prefix: dropped, so a re-import
		// of a conformed file does not grow T_Sp_T_Sp_.
		for (const TCHAR* Known : { TEXT("T_Sp_"), TEXT("T_Mk_"), TEXT("T_") })
		{
			if (Stem.StartsWith(Known, ESearchCase::IgnoreCase) && Stem.Len() > FCString::Strlen(Known))
			{
				Stem.RightChopInline(FCString::Strlen(Known));
				break;
			}
		}

		FString Clean;
		Clean.Reserve(Stem.Len());
		for (const TCHAR C : Stem)
		{
			const bool bWord = (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9'));
			const TCHAR Next = bWord ? C : TEXT('_');
			if (Next == TEXT('_') && (Clean.IsEmpty() || Clean[Clean.Len() - 1] == TEXT('_')))
			{
				continue;
			}
			Clean.AppendChar(Next);
		}
		while (Clean.EndsWith(TEXT("_")))
		{
			Clean.LeftChopInline(1);
		}
		if (Clean.Len() > 48)
		{
			Clean.LeftInline(48);
			while (Clean.EndsWith(TEXT("_")))
			{
				Clean.LeftChopInline(1);
			}
		}
		return Clean.IsEmpty() ? FString(TEXT("Texture")) : Clean;
	}

	FString AssetNameFor(const FString& Stem)
	{
		return Prefix() + Stem;
	}

	bool IsOurs(const FString& AssetName)
	{
		if (!AssetName.StartsWith(Prefix(), ESearchCase::CaseSensitive) || AssetName.Len() <= Prefix().Len())
		{
			return false;
		}
		for (int32 Index = Prefix().Len(); Index < AssetName.Len(); ++Index)
		{
			const TCHAR C = AssetName[Index];
			const bool bWord = (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_');
			if (!bWord)
			{
				return false;
			}
		}
		return true;
	}

	ESurfTextureKind KindFromName(const FString& Name)
	{
		// The last word after an underscore says what the texture is.
		FString Stem = FPaths::GetBaseFilename(Name);
		int32 Cut = INDEX_NONE;
		const FString Last = Stem.FindLastChar(TEXT('_'), Cut) ? Stem.Mid(Cut + 1).ToUpper() : Stem.ToUpper();

		static const TCHAR* Normals[] = { TEXT("N"), TEXT("NRM"), TEXT("NORMAL"), TEXT("NOR"), TEXT("NORM") };
		static const TCHAR* Linears[] = { TEXT("R"), TEXT("ROUGH"), TEXT("ROUGHNESS"), TEXT("AO"), TEXT("M"), TEXT("METAL"), TEXT("METALLIC"),
			TEXT("H"), TEXT("HEIGHT"), TEXT("DISP"), TEXT("DISPLACEMENT"), TEXT("ORM"), TEXT("RMA"), TEXT("MASK"), TEXT("MASKS"), TEXT("A"), TEXT("ALPHA"), TEXT("OP"), TEXT("OPACITY") };
		for (const TCHAR* Word : Normals)
		{
			if (Last == Word)
			{
				return ESurfTextureKind::Normal;
			}
		}
		for (const TCHAR* Word : Linears)
		{
			if (Last == Word)
			{
				return ESurfTextureKind::Linear;
			}
		}
		return ESurfTextureKind::Colour;
	}

	const TCHAR* KindName(ESurfTextureKind Kind)
	{
		switch (Kind)
		{
		case ESurfTextureKind::Normal: return TEXT("normal");
		case ESurfTextureKind::Linear: return TEXT("linear");
		default: return TEXT("colour");
		}
	}

	void ApplyKind(UTexture2D* Texture, ESurfTextureKind Kind)
	{
		if (!Texture)
		{
			return;
		}
		switch (Kind)
		{
		case ESurfTextureKind::Normal:
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Normalmap;
			Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
			break;
		case ESurfTextureKind::Linear:
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Masks;
			break;
		default:
			Texture->SRGB = true;
			Texture->CompressionSettings = TC_Default;
			break;
		}
		Texture->PostEditChange();
	}

	// --- Where things are ---------------------------------------------------

	FString SourceRoot()
	{
		FString Root = GetDefault<USurfSettings>()->TextureSourceRoot.TrimStartAndEnd();
		if (Root.IsEmpty())
		{
			Root = FPaths::ProjectDir() / TEXT("SurfacingPro");
		}
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	FString SourceDir(const FString& Root)
	{
		return Root / TEXT("Textures");
	}

	FString SourcePathFor(const FString& Root, const FString& AssetName)
	{
		return SourceDir(Root) / (AssetName + TEXT(".tga"));
	}

	FString ContentFolder(const FString& LibraryRoot)
	{
		const FString Dir = SurfMaterialsLib::NormalisedRoot(LibraryRoot);
		return Dir.IsEmpty() ? FString() : Dir + TEXT("/Textures");
	}

	FString SamplesDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/TextureSamples"));
		FPaths::NormalizeDirectoryName(Dir);
		return Dir;
	}

	// --- Status -------------------------------------------------------------

	const TCHAR* StatusName(ESurfTextureStatus Status)
	{
		switch (Status)
		{
		case ESurfTextureStatus::Conform: return TEXT("conform");
		case ESurfTextureStatus::Portable: return TEXT("portable");
		case ESurfTextureStatus::LocalOnly: return TEXT("local only");
		case ESurfTextureStatus::Broken: return TEXT("broken");
		default: return TEXT("external");
		}
	}

	FString SourceFileOf(const FAssetData& Asset)
	{
		FString Json;
		if (!Asset.GetTagValue(UObject::SourceFileTagName(), Json) || Json.IsEmpty())
		{
			return FString();
		}
		const TOptional<FAssetImportInfo> Info = FAssetImportInfo::FromJson(Json);
		if (!Info.IsSet() || Info->SourceFiles.Num() == 0)
		{
			return FString();
		}
		FString Resolved = UAssetImportData::ResolveImportFilename(Info->SourceFiles[0].RelativeFilename, Asset.PackageName.ToString());
		FPaths::NormalizeFilename(Resolved);
		return Resolved;
	}

	namespace
	{
		bool SamePath(const FString& A, const FString& B)
		{
			FString X = FPaths::ConvertRelativePathToFull(A);
			FString Y = FPaths::ConvertRelativePathToFull(B);
			FPaths::NormalizeFilename(X);
			FPaths::NormalizeFilename(Y);
			return X.Equals(Y, ESearchCase::IgnoreCase);
		}

		bool UnderDir(const FString& File, const FString& Dir)
		{
			FString X = FPaths::ConvertRelativePathToFull(File);
			FString D = FPaths::ConvertRelativePathToFull(Dir);
			FPaths::NormalizeFilename(X);
			FPaths::NormalizeDirectoryName(D);
			return X.StartsWith(D + TEXT("/"), ESearchCase::IgnoreCase);
		}
	}

	ESurfTextureStatus StatusOf(const FAssetData& Asset, const FString& Root)
	{
		const FString File = SourceFileOf(Asset);
		if (File.IsEmpty())
		{
			return ESurfTextureStatus::External;
		}
		if (!FPaths::FileExists(File))
		{
			return ESurfTextureStatus::Broken;
		}
		const FString Name = Asset.AssetName.ToString();
		if (IsOurs(Name) && SamePath(File, SourcePathFor(Root, Name)))
		{
			return ESurfTextureStatus::Conform;
		}
		return UnderDir(File, FPaths::ProjectDir()) ? ESurfTextureStatus::Portable : ESurfTextureStatus::LocalOnly;
	}

	// --- The record ---------------------------------------------------------

	FString FSurfTexturePolicy::PathFor(const FString& Root)
	{
		return Root / TEXT("texture_policy.json");
	}

	bool FSurfTexturePolicy::Load(const FString& Root)
	{
		Lines.Reset();

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *PathFor(Root)))
		{
			// No record yet is not an error: nothing has been decided.
			return true;
		}

		TSharedPtr<FJsonObject> Doc;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Doc) || !Doc.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Doc->TryGetArrayField(TEXT("textures"), Items) || !Items)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Item) || !Item || !Item->IsValid())
			{
				continue;
			}
			FSurfTexturePolicyEntry Entry;
			(*Item)->TryGetStringField(TEXT("asset_path"), Entry.AssetPath);
			(*Item)->TryGetStringField(TEXT("class"), Entry.Class);
			(*Item)->TryGetBoolField(TEXT("srgb"), Entry.bSRGB);
			(*Item)->TryGetStringField(TEXT("compression"), Entry.Compression);
			(*Item)->TryGetStringField(TEXT("last_conform"), Entry.LastConform);
			(*Item)->TryGetBoolField(TEXT("lossy_reduction"), Entry.bLossyReduction);
			(*Item)->TryGetStringField(TEXT("note"), Entry.Note);
			if (!Entry.AssetPath.IsEmpty())
			{
				Lines.Add(Entry);
			}
		}
		return true;
	}

	bool FSurfTexturePolicy::Save(const FString& Root) const
	{
		TArray<FSurfTexturePolicyEntry> Sorted = Lines;
		Sorted.Sort([](const FSurfTexturePolicyEntry& A, const FSurfTexturePolicyEntry& B) { return A.AssetPath < B.AssetPath; });

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FSurfTexturePolicyEntry& Entry : Sorted)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), Entry.AssetPath);
			Item->SetStringField(TEXT("class"), Entry.Class);
			Item->SetBoolField(TEXT("srgb"), Entry.bSRGB);
			Item->SetStringField(TEXT("compression"), Entry.Compression);
			Item->SetStringField(TEXT("last_conform"), Entry.LastConform);
			Item->SetBoolField(TEXT("lossy_reduction"), Entry.bLossyReduction);
			Item->SetStringField(TEXT("note"), Entry.Note);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
		Doc->SetNumberField(TEXT("schema_version"), 1);
		Doc->SetArrayField(TEXT("textures"), Items);

		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Doc.ToSharedRef(), Writer))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*Root, true);
		return FFileHelper::SaveStringToFile(Text, *PathFor(Root), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	const FSurfTexturePolicyEntry* FSurfTexturePolicy::Find(const FString& AssetPath) const
	{
		return Lines.FindByPredicate([&AssetPath](const FSurfTexturePolicyEntry& Entry) { return Entry.AssetPath == AssetPath; });
	}

	void FSurfTexturePolicy::Set(const FSurfTexturePolicyEntry& Entry)
	{
		if (FSurfTexturePolicyEntry* Existing = Lines.FindByPredicate([&Entry](const FSurfTexturePolicyEntry& Line) { return Line.AssetPath == Entry.AssetPath; }))
		{
			*Existing = Entry;
			return;
		}
		Lines.Add(Entry);
	}

	bool FSurfTexturePolicy::Remove(const FString& AssetPath)
	{
		return Lines.RemoveAll([&AssetPath](const FSurfTexturePolicyEntry& Line) { return Line.AssetPath == AssetPath; }) > 0;
	}

	// --- Import --------------------------------------------------------------

	bool IsImageFile(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path).ToLower();
		static const TCHAR* Known[] = { TEXT("tga"), TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("bmp"), TEXT("exr"), TEXT("tif"), TEXT("tiff"), TEXT("psd"), TEXT("hdr"), TEXT("dds") };
		for (const TCHAR* K : Known)
		{
			if (Ext == K)
			{
				return true;
			}
		}
		return false;
	}

	FString FSurfTextureImportReport::Line() const
	{
		if (Imported == 0)
		{
			for (const FSurfTextureImportEntry& Entry : Entries)
			{
				if (!Entry.Error.IsEmpty())
				{
					return Verb + TEXT(": ") + Entry.Error;
				}
			}
			return Verb + TEXT(": nothing to do");
		}

		int32 Conformed = 0;
		int32 Lossy = 0;
		int32 Split = 0;
		for (const FSurfTextureImportEntry& Entry : Entries)
		{
			if (Entry.Status == ESurfTextureStatus::Conform)
			{
				++Conformed;
			}
			Lossy += Entry.bLossy || Entry.bResized ? 1 : 0;
			Split += Entry.AlphaAssetPath.IsEmpty() ? 0 : 1;
		}

		FString Out = FString::Printf(TEXT("%s %d texture%s"), *Done, Imported, Imported == 1 ? TEXT("") : TEXT("s"));
		if (Imported == 1 && Entries.Num() > 0)
		{
			for (const FSurfTextureImportEntry& Entry : Entries)
			{
				if (Entry.Error.IsEmpty())
				{
					Out = Done + TEXT(" ") + FPackageName::ObjectPathToObjectName(Entry.AssetPath);
					break;
				}
			}
		}
		Out += Conformed == Imported ? TEXT(", conformed") : (Conformed == 0 ? TEXT(", linked") : FString::Printf(TEXT(", %d conformed"), Conformed));
		if (Lossy > 0)
		{
			Out += FString::Printf(TEXT(", %d reduced"), Lossy);
		}
		if (Split > 0)
		{
			Out += FString::Printf(TEXT(", %d alpha split off"), Split);
		}
		if (Failed > 0)
		{
			Out += FString::Printf(TEXT(", %d failed"), Failed);
		}
		return Out;
	}

	namespace
	{
		UTexture2D* ImportOne(const FString& File, const FString& Folder, const FString& AssetName, FString& OutError)
		{
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->AddToRoot();
			Task->Filename = File;
			Task->DestinationPath = Folder;
			Task->DestinationName = AssetName;
			Task->bReplaceExisting = false;
			Task->bAutomated = true;
			Task->bSave = false;

			TArray<UAssetImportTask*> Tasks;
			Tasks.Add(Task);
			FAssetToolsModule::GetModule().Get().ImportAssetTasks(Tasks);

			UTexture2D* Texture = nullptr;
			for (UObject* Object : Task->GetObjects())
			{
				Texture = Cast<UTexture2D>(Object);
				if (Texture)
				{
					break;
				}
			}
			Task->RemoveFromRoot();
			if (!Texture)
			{
				OutError = FString::Printf(TEXT("%s did not import as a texture"), *FPaths::GetCleanFilename(File));
			}
			return Texture;
		}

		/** Scale the surface down so its longest edge is MaxSize at most. */
		bool ShrinkTo(FSurfTextureSurface& Surface, int32 MaxSize)
		{
			const int32 Longest = FMath::Max(Surface.Width, Surface.Height);
			if (MaxSize <= 0 || Longest <= MaxSize)
			{
				return false;
			}
			const double Scale = static_cast<double>(MaxSize) / Longest;
			const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(Surface.Width * Scale));
			const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(Surface.Height * Scale));

			TArray<FColor> Source;
			Source.SetNumUninitialized(Surface.Width * Surface.Height);
			FMemory::Memcpy(Source.GetData(), Surface.BGRA.GetData(), Source.Num() * sizeof(FColor));
			TArray<FColor> Small;
			FImageUtils::ImageResize(Surface.Width, Surface.Height, Source, NewWidth, NewHeight, Small, /*bLinearSpace*/ false, /*bForceOpaque*/ false);

			Surface.Width = NewWidth;
			Surface.Height = NewHeight;
			Surface.BGRA.SetNumUninitialized(static_cast<int64>(NewWidth) * NewHeight * 4);
			FMemory::Memcpy(Surface.BGRA.GetData(), Small.GetData(), Small.Num() * sizeof(FColor));
			return true;
		}

		FString Now()
		{
			return FDateTime::UtcNow().ToIso8601();
		}

		/**
		 * The texture's own pixels as our TGA under Root, read from there; the
		 * alpha that varies split into a texture of its own beside it in Folder.
		 * False with the reason in Entry.Error.
		 */
		bool ConformTexture(UTexture2D* Texture, const FString& AssetName, const FString& Folder, const FString& Root,
			const FSurfTextureImportOptions& Options, FSurfTexturePolicy& Policy, bool& bPolicyChanged, FSurfTextureImportEntry& Entry)
		{
			FSurfTextureSurface Surface;
			FString Why;
			if (!FSurfTgaWriter::ExtractSurface(Texture, /*bAllowBitDepthReduction*/ true, Surface, Why))
			{
				Entry.Error = FString::Printf(TEXT("%s: %s"), *AssetName, *Why);
				return false;
			}
			Entry.bLossy = Surface.bReducedBitDepth;
			Entry.bResized = ShrinkTo(Surface, Options.MaxSize);

			const FString Tga = SourcePathFor(Root, AssetName);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Tga), true);
			const FSurfTgaWriteResult Written = FSurfTgaWriter::WriteSurface(Surface, Tga, Surface.bSingleChannelSource);
			if (!Written.bSuccess)
			{
				Entry.Error = FString::Printf(TEXT("%s: %s"), *AssetName, *Written.Error);
				return false;
			}
			if (Written.bGrayscaleCandidate)
			{
				Entry.Warnings.Add(TEXT("its channels are identical; a grayscale texture would do"));
			}

			// Alpha that varies goes into a texture of its own; the TGA
			// carries none, as the container says.
			if (Written.bHasNonUniformAlpha)
			{
				FSurfTextureSurface Alpha;
				if (FSurfTgaWriter::ExtractAlphaSurface(Surface, Alpha))
				{
					const FString AlphaName = SurfMaterialsLib::UniqueAssetName(Folder, AssetName + TEXT("_A"));
					const FString AlphaTga = SourcePathFor(Root, AlphaName);
					const FSurfTgaWriteResult AlphaWritten = FSurfTgaWriter::WriteSurface(Alpha, AlphaTga, /*bForceGrayscale*/ true);
					FString AlphaError;
					UTexture2D* AlphaTexture = AlphaWritten.bSuccess ? ImportOne(AlphaTga, Folder, AlphaName, AlphaError) : nullptr;
					if (AlphaTexture)
					{
						ApplyKind(AlphaTexture, ESurfTextureKind::Linear);
						Entry.AlphaAssetPath = AlphaTexture->GetPathName();
						FSurfTexturePolicyEntry AlphaLine;
						AlphaLine.AssetPath = Entry.AlphaAssetPath;
						AlphaLine.bSRGB = false;
						AlphaLine.Compression = TEXT("TC_Masks");
						AlphaLine.LastConform = Now();
						AlphaLine.Note = TEXT("alpha of ") + AssetName;
						Policy.Set(AlphaLine);
						bPolicyChanged = true;
						if (Options.bSave)
						{
							SurfMaterialSeed::SaveAsset(AlphaTexture);
						}
					}
					else
					{
						Entry.Warnings.Add(TEXT("its alpha could not be split off: ") + (AlphaError.IsEmpty() ? AlphaWritten.Error : AlphaError));
					}
				}
			}

			// The asset now reads the TGA: the file the team keeps is the truth.
			if (Texture->AssetImportData)
			{
				Texture->AssetImportData->Update(Tga);
			}
			FReimportManager::Instance()->Reimport(Texture, /*bAskForNewFileIfMissing*/ false, /*bShowNotification*/ false, Tga,
				nullptr, INDEX_NONE, /*bForceNewFile*/ false, /*bAutomated*/ true);
			ApplyKind(Texture, Entry.Kind);
			Entry.SourceAfter = Tga;
			Entry.Status = ESurfTextureStatus::Conform;
			return true;
		}

		/** The record's line for a texture as it stands, after a conform or a link. */
		void RecordTexture(UTexture2D* Texture, const FSurfTextureImportEntry& Entry, const FSurfTextureImportOptions& Options, bool bConformed, FSurfTexturePolicy& Policy)
		{
			FSurfTexturePolicyEntry Line;
			Line.AssetPath = Entry.AssetPath;
			Line.bSRGB = Texture->SRGB != 0;
			Line.Compression = UEnum::GetValueAsString(Texture->CompressionSettings.GetValue());
			Line.LastConform = bConformed ? Now() : FString();
			Line.bLossyReduction = Entry.bLossy || Entry.bResized;
			Line.Note = bConformed ? (Entry.bResized ? FString::Printf(TEXT("conform, scaled to %d"), Options.MaxSize) : FString(TEXT("conform"))) : FString(TEXT("link"));
			Policy.Set(Line);
		}
	}

	FSurfTextureImportReport Import(const TArray<FString>& Files, const FSurfTextureImportOptions& Options)
	{
		FSurfTextureImportReport Report;

		const FString Folder = ContentFolder(Options.LibraryRoot);
		const FString Root = Options.SourceRootOverride.IsEmpty() ? SourceRoot() : Options.SourceRootOverride;
		if (Folder.IsEmpty())
		{
			FSurfTextureImportEntry& Entry = Report.Entries.AddDefaulted_GetRef();
			Entry.Error = TEXT("no library root");
			++Report.Failed;
			return Report;
		}

		FSurfTexturePolicy Policy;
		Policy.Load(Root);
		bool bPolicyChanged = false;

		for (const FString& File : Files)
		{
			FSurfTextureImportEntry& Entry = Report.Entries.AddDefaulted_GetRef();
			Entry.File = File;

			if (!FPaths::FileExists(File))
			{
				Entry.Error = FString::Printf(TEXT("%s is not there"), *FPaths::GetCleanFilename(File));
				++Report.Failed;
				continue;
			}
			if (!IsImageFile(File))
			{
				Entry.Error = FString::Printf(TEXT("%s is not an image"), *FPaths::GetCleanFilename(File));
				++Report.Failed;
				continue;
			}

			const FString AssetName = SurfMaterialsLib::UniqueAssetName(Folder, AssetNameFor(StemFrom(File)));
			Entry.Kind = KindFromName(AssetName);

			UTexture2D* Texture = ImportOne(File, Folder, AssetName, Entry.Error);
			if (!Texture)
			{
				++Report.Failed;
				continue;
			}
			Entry.Texture = Texture;
			Entry.AssetPath = Texture->GetPathName();
			ApplyKind(Texture, Entry.Kind);

			if (Options.bConform)
			{
				// The source becomes our TGA under the root, and the asset reads from it.
				if (!ConformTexture(Texture, AssetName, Folder, Root, Options, Policy, bPolicyChanged, Entry))
				{
					++Report.Failed;
					continue;
				}
			}
			else
			{
				Entry.SourceAfter = FPaths::ConvertRelativePathToFull(File);
				Entry.Status = UnderDir(File, FPaths::ProjectDir()) ? ESurfTextureStatus::Portable : ESurfTextureStatus::LocalOnly;
			}

			RecordTexture(Texture, Entry, Options, Options.bConform, Policy);
			bPolicyChanged = true;

			Texture->MarkPackageDirty();
			if (Options.bSave)
			{
				SurfMaterialSeed::SaveAsset(Texture);
			}
			++Report.Imported;

			SurfSession::Verb(TEXT("texture_import"), FString::Printf(TEXT("name=%s kind=%s status=%s"), *AssetName, KindName(Entry.Kind), StatusName(Entry.Status)));
		}

		if (bPolicyChanged)
		{
			Policy.Save(Root);
		}
		return Report;
	}

	// --- Keeping the library right (T-b) ---------------------------------------

	bool IsStale(const FAssetData& Asset)
	{
		FString Json;
		if (!Asset.GetTagValue(UObject::SourceFileTagName(), Json) || Json.IsEmpty())
		{
			return false;
		}
		const TOptional<FAssetImportInfo> Info = FAssetImportInfo::FromJson(Json);
		if (!Info.IsSet() || Info->SourceFiles.Num() == 0 || Info->SourceFiles[0].Timestamp.GetTicks() == 0)
		{
			return false;
		}
		const FString Resolved = UAssetImportData::ResolveImportFilename(Info->SourceFiles[0].RelativeFilename, Asset.PackageName.ToString());
		const FDateTime OnDisk = IFileManager::Get().GetTimeStamp(*Resolved);
		if (OnDisk == FDateTime::MinValue())
		{
			return false;
		}
		return OnDisk > Info->SourceFiles[0].Timestamp + FTimespan::FromSeconds(2.0);
	}

	bool Reimport(UTexture2D* Texture, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Texture)
		{
			OutError = TEXT("reimport: no texture");
			return false;
		}
		const FString File = Texture->AssetImportData ? Texture->AssetImportData->GetFirstFilename() : FString();
		if (File.IsEmpty() || !FPaths::FileExists(File))
		{
			OutError = FString::Printf(TEXT("reimport: %s has no file to read (%s)"), *Texture->GetName(), *File);
			return false;
		}
		if (!FReimportManager::Instance()->Reimport(Texture, /*bAskForNewFileIfMissing*/ false, /*bShowNotification*/ false, TEXT(""),
			nullptr, INDEX_NONE, /*bForceNewFile*/ false, /*bAutomated*/ true))
		{
			OutError = FString::Printf(TEXT("reimport: %s could not be read again from %s"), *Texture->GetName(), *File);
			return false;
		}
		ApplyKind(Texture, KindFromName(Texture->GetName()));
		Texture->MarkPackageDirty();
		if (bSave)
		{
			SurfMaterialSeed::SaveAsset(Texture);
		}
		SurfSession::Verb(TEXT("texture_reimport"), FString::Printf(TEXT("name=%s"), *Texture->GetName()));
		return true;
	}

	FSurfTextureImportReport Conform(const TArray<UTexture2D*>& Textures, const FSurfTextureImportOptions& Options)
	{
		FSurfTextureImportReport Report;
		Report.Verb = TEXT("conform");
		Report.Done = TEXT("conformed");
		const FString Root = Options.SourceRootOverride.IsEmpty() ? SourceRoot() : Options.SourceRootOverride;

		FSurfTexturePolicy Policy;
		Policy.Load(Root);
		bool bPolicyChanged = false;

		for (UTexture2D* Texture : Textures)
		{
			FSurfTextureImportEntry& Entry = Report.Entries.AddDefaulted_GetRef();
			if (!Texture)
			{
				Entry.Error = TEXT("no texture");
				++Report.Failed;
				continue;
			}
			Entry.Texture = Texture;
			Entry.File = Texture->AssetImportData ? Texture->AssetImportData->GetFirstFilename() : FString();
			if (!Texture->Source.IsValid())
			{
				Entry.Error = FString::Printf(TEXT("%s has no pixels of its own to write"), *Texture->GetName());
				++Report.Failed;
				continue;
			}

			// Ours by name first: the TGA is named after the asset.
			const FString Folder = FPackageName::GetLongPackagePath(Texture->GetOutermost()->GetName());
			FString AssetName = Texture->GetName();
			if (!IsOurs(AssetName))
			{
				const FString Wanted = SurfMaterialsLib::UniqueAssetName(Folder, AssetNameFor(StemFrom(AssetName)));
				if (Options.bSave)
				{
					TArray<FAssetRenameData> Renames;
					Renames.Emplace(TWeakObjectPtr<UObject>(Texture), Folder, Wanted);
					if (FAssetToolsModule::GetModule().Get().RenameAssets(Renames))
					{
						AssetName = Texture->GetName();
					}
					else
					{
						Entry.Warnings.Add(FString::Printf(TEXT("%s could not be renamed %s; it keeps its name"), *AssetName, *Wanted));
					}
				}
				else
				{
					Entry.Warnings.Add(FString::Printf(TEXT("%s keeps its name (not saved, so not renamed %s)"), *AssetName, *Wanted));
				}
			}
			Entry.Kind = KindFromName(AssetName);
			Entry.AssetPath = Texture->GetPathName();

			if (!ConformTexture(Texture, AssetName, Folder, Root, Options, Policy, bPolicyChanged, Entry))
			{
				++Report.Failed;
				continue;
			}
			RecordTexture(Texture, Entry, Options, /*bConformed*/ true, Policy);
			bPolicyChanged = true;

			Texture->MarkPackageDirty();
			if (Options.bSave)
			{
				SurfMaterialSeed::SaveAsset(Texture);
			}
			++Report.Imported;
			SurfSession::Verb(TEXT("texture_conform"), FString::Printf(TEXT("name=%s kind=%s"), *AssetName, KindName(Entry.Kind)));
		}

		if (bPolicyChanged)
		{
			Policy.Save(Root);
		}
		return Report;
	}

	FSurfTextureImportReport Link(UTexture2D* Texture, const FString& File, const FSurfTextureImportOptions& Options)
	{
		FSurfTextureImportReport Report;
		Report.Verb = TEXT("link");
		Report.Done = TEXT("linked");
		const FString Root = Options.SourceRootOverride.IsEmpty() ? SourceRoot() : Options.SourceRootOverride;
		FSurfTextureImportEntry& Entry = Report.Entries.AddDefaulted_GetRef();
		Entry.File = File;
		if (!Texture)
		{
			Entry.Error = TEXT("no texture");
			++Report.Failed;
			return Report;
		}
		Entry.Texture = Texture;
		Entry.AssetPath = Texture->GetPathName();
		if (!FPaths::FileExists(File))
		{
			Entry.Error = FString::Printf(TEXT("%s is not there"), *FPaths::GetCleanFilename(File));
			++Report.Failed;
			return Report;
		}
		if (!IsImageFile(File))
		{
			Entry.Error = FString::Printf(TEXT("%s is not an image"), *FPaths::GetCleanFilename(File));
			++Report.Failed;
			return Report;
		}

		FString Full = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Full);
		Entry.Kind = KindFromName(Texture->GetName());
		if (Texture->AssetImportData)
		{
			Texture->AssetImportData->Update(Full);
		}
		if (!FReimportManager::Instance()->Reimport(Texture, /*bAskForNewFileIfMissing*/ false, /*bShowNotification*/ false, Full,
			nullptr, INDEX_NONE, /*bForceNewFile*/ false, /*bAutomated*/ true))
		{
			Entry.Error = FString::Printf(TEXT("%s could not be read from %s"), *Texture->GetName(), *Full);
			++Report.Failed;
			return Report;
		}
		ApplyKind(Texture, Entry.Kind);
		Entry.SourceAfter = Full;
		Entry.Status = UnderDir(Full, FPaths::ProjectDir()) ? ESurfTextureStatus::Portable : ESurfTextureStatus::LocalOnly;

		FSurfTexturePolicy Policy;
		Policy.Load(Root);
		RecordTexture(Texture, Entry, Options, /*bConformed*/ false, Policy);
		Policy.Save(Root);

		Texture->MarkPackageDirty();
		if (Options.bSave)
		{
			SurfMaterialSeed::SaveAsset(Texture);
		}
		++Report.Imported;
		SurfSession::Verb(TEXT("texture_link"), FString::Printf(TEXT("name=%s status=%s"), *Texture->GetName(), StatusName(Entry.Status)));
		return Report;
	}

	void ListOrphans(const FString& LibraryRoot, const FString& Root, TArray<FString>& Out)
	{
		Out.Reset();
		const FString Dir = SourceDir(Root);
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*.tga")), /*Files*/ true, /*Directories*/ false);
		if (Names.Num() == 0)
		{
			return;
		}

		auto Key = [](const FString& Path)
		{
			FString Full = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Full);
			return Full.ToLower();
		};
		TSet<FString> Read;
		TArray<FAssetData> Assets;
		ListProject(LibraryRoot, Assets);
		for (const FAssetData& Asset : Assets)
		{
			const FString Source = SourceFileOf(Asset);
			if (!Source.IsEmpty())
			{
				Read.Add(Key(Source));
			}
		}
		for (const FString& Name : Names)
		{
			const FString Full = Dir / Name;
			if (!Read.Contains(Key(Full)))
			{
				Out.Add(FPaths::ConvertRelativePathToFull(Full));
			}
		}
		Out.Sort([](const FString& A, const FString& B) { return FPaths::GetCleanFilename(A).Compare(FPaths::GetCleanFilename(B), ESearchCase::IgnoreCase) < 0; });
	}

	// --- The library's textures ---------------------------------------------

	void ListProject(const FString& LibraryRoot, TArray<FAssetData>& Out)
	{
		Out.Reset();
		const FString Folder = ContentFolder(LibraryRoot);
		if (Folder.IsEmpty())
		{
			return;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive*/ true);
		for (const FAssetData& Asset : Assets)
		{
			const UClass* Class = Asset.GetClass();
			if (Class && Class->IsChildOf(UTexture::StaticClass()))
			{
				Out.Add(Asset);
			}
		}
		Out.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.AssetName.ToString().Compare(B.AssetName.ToString(), ESearchCase::IgnoreCase) < 0;
		});
	}

	// --- Onto a material ------------------------------------------------------

	void TextureParametersOf(UMaterialInterface* Material, TArray<FName>& Out)
	{
		Out.Reset();
		if (!Material)
		{
			return;
		}
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Material->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			Out.AddUnique(Info.Name);
		}
		Out.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	}

	bool SetTextureParameter(UMaterialInstanceConstant* Instance, FName Parameter, UTexture* Texture, bool bSave, FString& OutError)
	{
		OutError.Reset();
		if (!Instance)
		{
			OutError = TEXT("texture: pick an instance first");
			return false;
		}
		if (Parameter.IsNone())
		{
			OutError = TEXT("texture: pick a parameter first");
			return false;
		}
		if (!Texture)
		{
			OutError = TEXT("texture: pick a texture first");
			return false;
		}

		{
			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Set %s"), *Parameter.ToString())));
			Instance->Modify();
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Parameter), Texture);
			Instance->PostEditChange();
		}
		Instance->MarkPackageDirty();
		if (bSave && !SurfMaterialSeed::SaveAsset(Instance))
		{
			OutError = FString::Printf(TEXT("texture: %s took %s but could not be saved"), *Instance->GetName(), *Texture->GetName());
		}
		SurfSession::Verb(TEXT("texture_set"), FString::Printf(TEXT("instance=%s parameter=%s texture=%s"), *Instance->GetName(), *Parameter.ToString(), *Texture->GetName()));
		return true;
	}
}
