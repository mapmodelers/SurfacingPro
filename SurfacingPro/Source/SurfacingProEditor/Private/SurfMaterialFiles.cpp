// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialFiles.h"
#include "SurfMaterialInfo.h"
#include "SurfMaterialSeed.h"
#include "SurfMaterialsView.h"
#include "SurfSession.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticParameterSet.h"
#include "UObject/Package.h"

namespace SurfLibrary
{
	namespace
	{
		const TCHAR* MaterialSchema = TEXT("surfacingpro.material/1");
		const TCHAR* MaterialExtension = TEXT(".material.json");

		FString Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			FString Out;
			Object->TryGetStringField(Field, Out);
			return Out;
		}

		FVector2D Pair(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Object->TryGetArrayField(Field, Array) || !Array || Array->Num() == 0)
			{
				return FVector2D::ZeroVector;
			}
			const double U = (*Array)[0].IsValid() ? (*Array)[0]->AsNumber() : 0.0;
			const double V = Array->Num() >= 2 && (*Array)[1].IsValid() ? (*Array)[1]->AsNumber() : U;
			return FVector2D(U, V);
		}

		TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (double Value : Values)
			{
				Out.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Out;
		}

		FDateTime StampOf(const FString& File)
		{
			return File.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*File);
		}

		FString ObjectPathOf(const FString& Path)
		{
			// "/Game/X/M_Y" -> "/Game/X/M_Y.M_Y"
			if (Path.Contains(TEXT(".")))
			{
				return Path;
			}
			return Path + TEXT(".") + FPackageName::GetShortName(Path);
		}

		template <typename T>
		T* LoadQuiet(const FString& Path)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			const FString Object = ObjectPathOf(Path);
			// Looked for inside its package, and loaded only when on disk: a failed
			// load leaves an empty package behind, and its name taken.
			const FString PackageName = FPackageName::ObjectPathToPackageName(Object);
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				if (T* Found = FindObject<T>(Package, *FPackageName::ObjectPathToObjectName(Object)))
				{
					return Found;
				}
			}
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}
			return LoadObject<T>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		TSet<FName> NamesOf(UMaterialInterface* Parent, int32 Kind)
		{
			TSet<FName> Out;
			if (!Parent)
			{
				return Out;
			}
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			switch (Kind)
			{
			case 0: Parent->GetAllScalarParameterInfo(Infos, Guids); break;
			case 1: Parent->GetAllVectorParameterInfo(Infos, Guids); break;
			case 2: Parent->GetAllStaticSwitchParameterInfo(Infos, Guids); break;
			default: Parent->GetAllTextureParameterInfo(Infos, Guids); break;
			}
			for (const FMaterialParameterInfo& Info : Infos)
			{
				Out.Add(Info.Name);
			}
			return Out;
		}
	}

	const TCHAR* SyncName(EMaterialSync Sync)
	{
		switch (Sync)
		{
		case EMaterialSync::NotCreated: return TEXT("not made yet");
		case EMaterialSync::InSync:     return TEXT("in step");
		case EMaterialSync::FileNewer:  return TEXT("file changed");
		case EMaterialSync::AssetNewer: return TEXT("instance changed");
		case EMaterialSync::Both:       return TEXT("changed on both");
		case EMaterialSync::NoParent:   return TEXT("no such parent");
		default:                        return TEXT("?");
		}
	}

	FString LibraryDir()
	{
		return SurfTextures::SourceRoot() / TEXT("Library");
	}

	FString MaterialsDir(const FString& Library)
	{
		return Library / TEXT("Materials");
	}

	FString MaterialFileFor(const FString& Dir, const FString& Name)
	{
		FString Clean = Name;
		for (TCHAR& C : Clean)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_'))
			{
				C = TEXT('_');
			}
		}
		return Dir / (Clean + MaterialExtension);
	}

	bool LoadMaterial(const FString& File, FSurfMaterialFile& Out, FString& OutError)
	{
		Out = FSurfMaterialFile();
		Out.File = FPaths::ConvertRelativePathToFull(File);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			OutError = FString::Printf(TEXT("%s could not be read"), *FPaths::GetCleanFilename(File));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutError = FString::Printf(TEXT("%s is not JSON: %s"), *FPaths::GetCleanFilename(File), *Reader->GetErrorMessage());
			return false;
		}
		const FString Schema = Str(Object, TEXT("schema"));
		if (!Schema.IsEmpty() && !Schema.StartsWith(TEXT("surfacingpro.material/")))
		{
			OutError = FString::Printf(TEXT("%s is not a material (%s)"), *FPaths::GetCleanFilename(File), *Schema);
			return false;
		}
		Out.Name = Str(Object, TEXT("name"));
		if (Out.Name.IsEmpty())
		{
			Out.Name = FPaths::GetCleanFilename(File).Replace(MaterialExtension, TEXT(""));
		}
		Out.Parent = Str(Object, TEXT("parent"));
		Out.Asset = Str(Object, TEXT("asset"));
		Out.SyncedAt = Str(Object, TEXT("synced_at"));

		const TSharedPtr<FJsonObject>* Section = nullptr;
		if (Object->TryGetObjectField(TEXT("scalars"), Section) && Section && Section->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Section)->Values)
			{
				double Value = 0.0;
				if (P.Value.IsValid() && P.Value->TryGetNumber(Value))
				{
					Out.Scalars.Add(P.Key, Value);
				}
			}
		}
		if (Object->TryGetObjectField(TEXT("vectors"), Section) && Section && Section->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Section)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (P.Value.IsValid() && P.Value->TryGetArray(Array) && Array && Array->Num() >= 3)
				{
					FLinearColor Colour((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber(), Array->Num() >= 4 ? (*Array)[3]->AsNumber() : 1.0);
					Out.Vectors.Add(P.Key, Colour);
				}
			}
		}
		if (Object->TryGetObjectField(TEXT("switches"), Section) && Section && Section->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Section)->Values)
			{
				bool bValue = false;
				if (P.Value.IsValid() && P.Value->TryGetBool(bValue))
				{
					Out.Switches.Add(P.Key, bValue);
				}
			}
		}
		if (Object->TryGetObjectField(TEXT("textures"), Section) && Section && Section->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Section)->Values)
			{
				FString Value;
				if (P.Value.IsValid() && P.Value->TryGetString(Value))
				{
					Out.Textures.Add(P.Key, Value);
				}
			}
		}
		if (Object->TryGetObjectField(TEXT("surfacing"), Section) && Section && Section->IsValid())
		{
			Out.TileM = Pair(*Section, TEXT("tile_m"));
			Out.Use = Str(*Section, TEXT("use"));
			Out.TrimCells = Pair(*Section, TEXT("trim_cells"));
		}
		return true;
	}

	bool SaveMaterial(FSurfMaterialFile& Material, const FString& File, FString& OutError)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), MaterialSchema);
		Object->SetStringField(TEXT("name"), Material.Name);
		Object->SetStringField(TEXT("parent"), Material.Parent);
		auto Sorted = [](auto& Map) { Map.KeySort([](const FString& A, const FString& B) { return A < B; }); };
		if (Material.Scalars.Num() > 0)
		{
			Sorted(Material.Scalars);
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			for (const TPair<FString, double>& P : Material.Scalars)
			{
				Section->SetNumberField(P.Key, P.Value);
			}
			Object->SetObjectField(TEXT("scalars"), Section);
		}
		if (Material.Vectors.Num() > 0)
		{
			Sorted(Material.Vectors);
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			for (const TPair<FString, FLinearColor>& P : Material.Vectors)
			{
				Section->SetArrayField(P.Key, Numbers({ P.Value.R, P.Value.G, P.Value.B, P.Value.A }));
			}
			Object->SetObjectField(TEXT("vectors"), Section);
		}
		if (Material.Switches.Num() > 0)
		{
			Sorted(Material.Switches);
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			for (const TPair<FString, bool>& P : Material.Switches)
			{
				Section->SetBoolField(P.Key, P.Value);
			}
			Object->SetObjectField(TEXT("switches"), Section);
		}
		if (Material.Textures.Num() > 0)
		{
			Sorted(Material.Textures);
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& P : Material.Textures)
			{
				Section->SetStringField(P.Key, P.Value);
			}
			Object->SetObjectField(TEXT("textures"), Section);
		}
		if (!Material.TileM.IsZero() || !Material.Use.IsEmpty() || !Material.TrimCells.IsZero())
		{
			TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
			if (!Material.TileM.IsZero())
			{
				Section->SetArrayField(TEXT("tile_m"), Numbers({ Material.TileM.X, Material.TileM.Y }));
			}
			if (!Material.Use.IsEmpty())
			{
				Section->SetStringField(TEXT("use"), Material.Use);
			}
			if (!Material.TrimCells.IsZero())
			{
				Section->SetArrayField(TEXT("trim_cells"), Numbers({ Material.TrimCells.X, Material.TrimCells.Y }));
			}
			Object->SetObjectField(TEXT("surfacing"), Section);
		}
		if (!Material.Asset.IsEmpty())
		{
			Object->SetStringField(TEXT("asset"), Material.Asset);
			Object->SetStringField(TEXT("synced_at"), Material.SyncedAt);
		}

		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			OutError = TEXT("the material could not be written as JSON");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("%s could not be written"), *FPaths::GetCleanFilename(File));
			return false;
		}
		Material.File = FPaths::ConvertRelativePathToFull(File);
		return true;
	}

	void ListMaterials(const FString& Dir, TArray<FSurfMaterialFile>& Out)
	{
		Out.Reset();
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / (FString(TEXT("*")) + MaterialExtension)), true, false);
		Names.Sort();
		for (const FString& Name : Names)
		{
			FSurfMaterialFile Material;
			FString Error;
			if (LoadMaterial(Dir / Name, Material, Error))
			{
				Out.Add(MoveTemp(Material));
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SurfacingPro: %s"), *Error);
			}
		}
	}

	UMaterialInterface* ResolveParent(const FString& Parent, const FString& LibraryRoot)
	{
		if (Parent.IsEmpty())
		{
			return nullptr;
		}
		if (Parent.StartsWith(TEXT("/")))
		{
			return LoadQuiet<UMaterialInterface>(Parent);
		}
		// By name: the library first, then the plugin's own masters.
		TArray<FSurfMaterialItem> Items;
		SurfMaterialsLib::List(LibraryRoot, Items);
		for (const FSurfMaterialItem& Item : Items)
		{
			if (Item.Name.Equals(Parent, ESearchCase::IgnoreCase))
			{
				return Cast<UMaterialInterface>(Item.Asset.GetAsset());
			}
		}
		return LoadQuiet<UMaterialInterface>(FString(TEXT("/SurfacingPro/Materials/")) + Parent);
	}

	UMaterialInstanceConstant* FindInstance(const FSurfMaterialFile& Material, const FString& LibraryRoot)
	{
		if (UMaterialInstanceConstant* Named = LoadQuiet<UMaterialInstanceConstant>(Material.Asset))
		{
			return Named;
		}
		const FString Folder = SurfMaterialsLib::InstancesFolder(LibraryRoot);
		return Folder.IsEmpty() || Material.Name.IsEmpty() ? nullptr : LoadQuiet<UMaterialInstanceConstant>(Folder / Material.Name);
	}

	EMaterialSync SyncOf(const FSurfMaterialFile& Material, const FString& LibraryRoot)
	{
		UMaterialInstanceConstant* Instance = FindInstance(Material, LibraryRoot);
		if (!Instance)
		{
			return ResolveParent(Material.Parent, LibraryRoot) ? EMaterialSync::NotCreated : EMaterialSync::NoParent;
		}
		FDateTime At;
		if (Material.SyncedAt.IsEmpty() || !FDateTime::ParseIso8601(*Material.SyncedAt, At))
		{
			// An instance of that name, never matched with this file.
			return EMaterialSync::Both;
		}
		const FDateTime Slack = At + FTimespan::FromSeconds(2.0);
		const bool bFile = StampOf(Material.File) > Slack;
		UPackage* Package = Instance->GetOutermost();
		const FString PackageFile = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		const bool bAsset = Package->IsDirty() || StampOf(PackageFile) > Slack;
		if (bFile && bAsset)
		{
			return EMaterialSync::Both;
		}
		return bFile ? EMaterialSync::FileNewer : (bAsset ? EMaterialSync::AssetNewer : EMaterialSync::InSync);
	}

	bool ApplyMaterial(FSurfMaterialFile& Material, const FString& LibraryRoot, bool bSave, FString& OutLine)
	{
		UMaterialInterface* Parent = ResolveParent(Material.Parent, LibraryRoot);
		if (!Parent)
		{
			OutLine = FString::Printf(TEXT("%s: no parent called %s"), *Material.Name, *Material.Parent);
			return false;
		}

		UMaterialInstanceConstant* Instance = FindInstance(Material, LibraryRoot);
		const bool bMade = Instance == nullptr;
		if (!Instance)
		{
			FString Error;
			Instance = SurfMaterialsLib::NewInstanceNamed(Parent, SurfMaterialsLib::InstancesFolder(LibraryRoot), Material.Name, /*bSave*/ false, Error);
			if (!Instance)
			{
				OutLine = FString::Printf(TEXT("%s: %s"), *Material.Name, *Error);
				return false;
			}
		}
		Instance->Modify();
		if (Instance->Parent != Parent)
		{
			Instance->SetParentEditorOnly(Parent);
		}

		// The file is the whole of the instance's overrides: what it does not name goes back to the parent.
		Instance->ClearParameterValuesEditorOnly();
		TArray<FString> Unknown;
		int32 Set = 0;

		const TSet<FName> Scalars = NamesOf(Parent, 0);
		const TSet<FName> Vectors = NamesOf(Parent, 1);
		const TSet<FName> Switches = NamesOf(Parent, 2);
		const TSet<FName> Textures = NamesOf(Parent, 3);
		for (const TPair<FString, FString>& Pair : Material.Textures)
		{
			if (!Textures.Contains(FName(*Pair.Key)))
			{
				Unknown.Add(Pair.Key);
				continue;
			}
			if (UTexture* Texture = LoadQuiet<UTexture>(Pair.Value))
			{
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), Texture);
				++Set;
			}
			else
			{
				Unknown.Add(Pair.Value);
			}
		}
		for (const TPair<FString, double>& Pair : Material.Scalars)
		{
			if (!Scalars.Contains(FName(*Pair.Key)))
			{
				Unknown.Add(Pair.Key);
				continue;
			}
			Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), static_cast<float>(Pair.Value));
			++Set;
		}
		for (const TPair<FString, FLinearColor>& Pair : Material.Vectors)
		{
			if (!Vectors.Contains(FName(*Pair.Key)))
			{
				Unknown.Add(Pair.Key);
				continue;
			}
			Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(*Pair.Key)), Pair.Value);
			++Set;
		}
		{
			FStaticParameterSet Statics = Instance->GetStaticParameters();
			Statics.StaticSwitchParameters.Reset();
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Guids;
			Parent->GetAllStaticSwitchParameterInfo(Infos, Guids);
			for (const TPair<FString, bool>& Pair : Material.Switches)
			{
				const int32 Index = Infos.IndexOfByPredicate([&Pair](const FMaterialParameterInfo& Info) { return Info.Name == FName(*Pair.Key); });
				if (Index == INDEX_NONE)
				{
					Unknown.Add(Pair.Key);
					continue;
				}
				Statics.StaticSwitchParameters.Add(FStaticSwitchParameter(Infos[Index], Pair.Value, /*bOverride*/ true, Guids.IsValidIndex(Index) ? Guids[Index] : FGuid()));
				++Set;
			}
			Instance->UpdateStaticPermutation(Statics);
		}

		if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Instance))
		{
			if (!Material.TileM.IsZero())
			{
				Info->DefaultScale = Material.TileM;
			}
			if (!Material.Use.IsEmpty())
			{
				const int64 Value = StaticEnum<ESurfMaterialUse>()->GetValueByNameString(Material.Use);
				if (Value != INDEX_NONE)
				{
					Info->Use = static_cast<ESurfMaterialUse>(Value);
				}
				else
				{
					Unknown.Add(Material.Use);
				}
			}
			Info->TrimCellsPerTile = Material.TrimCells;
		}

		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		if (bSave)
		{
			SurfMaterialSeed::SaveAsset(Instance);
		}

		Material.Asset = Instance->GetPathName();
		Material.SyncedAt = FDateTime::UtcNow().ToIso8601();
		if (!Material.File.IsEmpty())
		{
			FString Error;
			SaveMaterial(Material, Material.File, Error);
		}

		OutLine = FString::Printf(TEXT("%s %s: %d parameter%s"), bMade ? TEXT("made") : TEXT("updated"), *Instance->GetName(), Set, Set == 1 ? TEXT("") : TEXT("s"));
		if (Unknown.Num() > 0)
		{
			OutLine += FString::Printf(TEXT("; not found: %s"), *FString::Join(Unknown, TEXT(", ")));
		}
		SurfSession::Verb(TEXT("material_file_apply"), FString::Printf(TEXT("name=%s made=%d set=%d unknown=%d"), *Instance->GetName(), bMade ? 1 : 0, Set, Unknown.Num()));
		return true;
	}

	bool ExportMaterial(UMaterialInstanceConstant* Instance, const FString& Dir, const FString& LibraryRoot, FSurfMaterialFile& Out, FString& OutError)
	{
		if (!Instance)
		{
			OutError = TEXT("export: no instance");
			return false;
		}
		const FString File = MaterialFileFor(Dir, Instance->GetName());

		Out = FSurfMaterialFile();
		Out.Name = Instance->GetName();
		UMaterialInterface* Parent = Instance->Parent;
		if (Parent)
		{
			// By name when the name finds it again, else the full path.
			Out.Parent = ResolveParent(Parent->GetName(), LibraryRoot) == Parent ? Parent->GetName() : Parent->GetPathName();
		}
		for (const FScalarParameterValue& Value : Instance->ScalarParameterValues)
		{
			Out.Scalars.Add(Value.ParameterInfo.Name.ToString(), Value.ParameterValue);
		}
		for (const FVectorParameterValue& Value : Instance->VectorParameterValues)
		{
			Out.Vectors.Add(Value.ParameterInfo.Name.ToString(), Value.ParameterValue);
		}
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			const FString Path = Value.ParameterValue ? Value.ParameterValue->GetPathName() : FString();
			Out.Textures.Add(Value.ParameterInfo.Name.ToString(), Path);
		}
		for (const FStaticSwitchParameter& Switch : Instance->GetStaticParameters().StaticSwitchParameters)
		{
			if (Switch.bOverride)
			{
				Out.Switches.Add(Switch.ParameterInfo.Name.ToString(), Switch.Value);
			}
		}
		if (const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Instance))
		{
			Out.TileM = Info->DefaultScale;
			Out.Use = StaticEnum<ESurfMaterialUse>()->GetNameStringByValue(static_cast<int64>(Info->Use));
			Out.TrimCells = Info->TrimCellsPerTile;
		}
		Out.Asset = Instance->GetPathName();
		Out.SyncedAt = FDateTime::UtcNow().ToIso8601();
		if (!SaveMaterial(Out, File, OutError))
		{
			return false;
		}
		SurfSession::Verb(TEXT("material_file_export"), FString::Printf(TEXT("name=%s"), *Out.Name));
		return true;
	}

}
