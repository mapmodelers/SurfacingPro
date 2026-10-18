// Copyright MapModelers. All Rights Reserved.
//
// Material instances as files (S194b, trimmed S194c): instances are authored
// in the Material Manager; a file is an export of one - the parent, the
// parameters it overrides, its surfacing defaults - kept under
// <SourceRoot>/Library/Materials/, and can be imported again (made, or an
// existing instance brought in line).

#pragma once

#include "CoreMinimal.h"
#include "SurfTextureLib.h"

class UMaterialInterface;
class UMaterialInstanceConstant;

namespace SurfLibrary
{
	struct SURFACINGPROEDITOR_API FSurfMaterialFile
	{
		FString Name;
		/** A master by name (looked for under the library root, then among the plugin's), or a full asset path. */
		FString Parent;
		TMap<FString, double> Scalars;
		TMap<FString, FLinearColor> Vectors;
		TMap<FString, bool> Switches;
		/** Parameter -> texture asset path. */
		TMap<FString, FString> Textures;
		FVector2D TileM = FVector2D::ZeroVector;
		FString Use;
		FVector2D TrimCells = FVector2D::ZeroVector;
		/** Written by SurfacingPro: the instance, and when file and instance last agreed (UTC, ISO 8601). */
		FString Asset;
		FString SyncedAt;
		/** The .material.json it was read from or written to; not part of the file. */
		FString File;
	};

	enum class EMaterialSync : uint8
	{
		/** No instance yet. */
		NotCreated,
		/** File and instance agree. */
		InSync,
		/** The file changed since: Update from file. */
		FileNewer,
		/** The instance changed since: Export to file. */
		AssetNewer,
		/** Both changed: the hand chooses. */
		Both,
		/** The parent it names cannot be found. */
		NoParent,
	};
	SURFACINGPROEDITOR_API const TCHAR* SyncName(EMaterialSync Sync);

	/** <SourceRoot>/Library */
	SURFACINGPROEDITOR_API FString LibraryDir();
	/** <Library>/Materials */
	SURFACINGPROEDITOR_API FString MaterialsDir(const FString& Library);
	/** <Dir>/<Name>.material.json */
	SURFACINGPROEDITOR_API FString MaterialFileFor(const FString& Dir, const FString& Name);

	SURFACINGPROEDITOR_API bool LoadMaterial(const FString& File, FSurfMaterialFile& Out, FString& OutError);
	SURFACINGPROEDITOR_API bool SaveMaterial(FSurfMaterialFile& Material, const FString& File, FString& OutError);
	SURFACINGPROEDITOR_API void ListMaterials(const FString& Dir, TArray<FSurfMaterialFile>& Out);

	SURFACINGPROEDITOR_API UMaterialInterface* ResolveParent(const FString& Parent, const FString& LibraryRoot);
	SURFACINGPROEDITOR_API UMaterialInstanceConstant* FindInstance(const FSurfMaterialFile& Material, const FString& LibraryRoot);
	SURFACINGPROEDITOR_API EMaterialSync SyncOf(const FSurfMaterialFile& Material, const FString& LibraryRoot);

	/**
	 * Make the instance the file describes, or bring the one it names in
	 * line: parent, parameters, surfacing defaults. The file then records the
	 * instance and the time they agreed. OutLine says what happened.
	 */
	SURFACINGPROEDITOR_API bool ApplyMaterial(FSurfMaterialFile& Material, const FString& LibraryRoot, bool bSave, FString& OutLine);

	/** Write an instance out as its file in Dir. */
	SURFACINGPROEDITOR_API bool ExportMaterial(UMaterialInstanceConstant* Instance, const FString& Dir, const FString& LibraryRoot, FSurfMaterialFile& Out, FString& OutError);

}
