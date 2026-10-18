// Copyright MapModelers. All Rights Reserved.
//
// Seeding the material library (D55/M2).
//
// The parents - the masters, the functions and collections they are built
// from, the textures they read, and a few ready-made instances - ship in the
// plugin's content and are copied into the project's own library on request,
// so an instance the user makes never depends on the plugin being installed.
// Copying a library is one operation: duplicate every asset, re-point every
// reference between them to the copies, stamp, save. The same operation
// brought the parents into the plugin in the first place.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

namespace SurfMaterialSeed
{
	/** One asset to copy: what it is and the package its copy will be. */
	struct FSurfSeedEntry
	{
		FAssetData Source;
		FString DestPackage;

		/** The copy's own name; empty, the source's. */
		FString DestName;

		/** Leave an asset already at the destination as it is, whatever the options say. */
		bool bKeep = false;
	};

	struct FSurfSeedOptions
	{
		/** Replace a copy that is already there; otherwise it is kept and counted as skipped. */
		bool bOverwrite = false;

		/** Write the copies to disk. Off in tests, which keep everything in memory. */
		bool bSave = true;

		/** Mark each copy with where it came from (USurfMaterialStamp). */
		bool bStamp = false;
	};

	struct FSurfSeedResult
	{
		int32 Copied = 0;
		int32 Skipped = 0;

		/** What went wrong, one line each: "what: why". Empty when everything went. */
		TArray<FString> Failed;

		/** Source package name -> the copy (or the existing asset that was kept). */
		TMap<FString, UObject*> Copies;
	};

	/** Materials, instances, functions, parameter collections and textures: what a library is made of. */
	bool IsLibraryAsset(const FAssetData& Asset);

	/**
	 * Every library asset under SourceRoot and its sub-folders, each with its
	 * copy's package under DestRoot at the same relative path. Keep sees each
	 * asset and its relative path; it may change the path, or return false to
	 * leave the asset out.
	 */
	void Plan(const FString& SourceRoot, const FString& DestRoot, TArray<FSurfSeedEntry>& Out,
		TFunctionRef<bool(const FAssetData& Asset, FString& Relative)> Keep);
	void Plan(const FString& SourceRoot, const FString& DestRoot, TArray<FSurfSeedEntry>& Out);

	/** Copy the plan: duplicate, re-point every reference between the sources to the copies, stamp, save. */
	FSurfSeedResult Copy(const TArray<FSurfSeedEntry>& Plan, const FSurfSeedOptions& Options);

	/** Stamp a copy with where it came from. An asset that cannot carry user data is left as it is. */
	void Stamp(UObject* Copy, const FString& SourcePackage);

	/** Where the plugin keeps its parents. */
	FString PluginParentsRoot();

	/** Copy the plugin's parents into <LibraryRoot>/Materials, stamped, leaving what is already there alone. */
	FSurfSeedResult SeedParents(const FString& LibraryRoot, bool bSave = true);

	/** Write one asset's package to disk where its name says. */
	bool SaveAsset(UObject* Asset);

	// --- Update masters (M2) ----------------------------------------------------

	/**
	 * What an asset holds, as one number: its properties and everything it
	 * owns, its stamp left out. The same content gives the same number, fresh
	 * from a copy or loaded back from disk; an edit changes it.
	 */
	uint32 ContentOf(UObject* Asset);

	/** How one seeded copy stands against the plugin asset it came from. */
	struct FSurfMasterState
	{
		FAssetData Copy;

		/** The plugin package it came from. */
		FString Source;

		/** The plugin no longer has it. */
		bool bGone = false;

		/** The plugin's file differs from the one the copy was made from. */
		bool bNewer = false;

		/** The copy's content differs from what was copied: edited, despite the rule. */
		bool bEdited = false;
	};

	/** Every stamped asset under <LibraryRoot>/Materials, checked against the plugin. */
	void CheckMasters(const FString& LibraryRoot, TArray<FSurfMasterState>& Out);

	struct FSurfUpdateResult
	{
		/** Copies replaced in place: stamped, left alone, and the plugin's is newer. */
		int32 Updated = 0;

		/** In the plugin, not in the library before. */
		int32 Added = 0;

		/** Left as they are: up to date, the user's own, or no longer in the plugin. */
		int32 Kept = 0;

		/** Edited copies kept, the new version put beside each: "M_X -> M_X_v0_2". */
		TArray<FString> Beside;

		/** Instances moved from an edited copy to the new version beside it. */
		int32 Reparented = 0;

		TArray<FString> Failed;

		/** One line for the bar. */
		FString Line() const;
	};

	/**
	 * Update masters (M2): a stamped copy the user left alone is replaced in
	 * place by the plugin's newer one (its instances keep their parent); a
	 * copy whose content changed is kept and the new version lands beside it,
	 * its instances moved over when bReparent says; what the plugin has and
	 * the library lacks is added; the user's own assets are never touched.
	 * SourceRoot is the plugin's parents unless a test says otherwise.
	 */
	FSurfUpdateResult UpdateMasters(const FString& LibraryRoot, bool bSave, bool bReparent, const FString& SourceRoot = FString());
}
