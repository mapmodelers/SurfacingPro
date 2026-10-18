// Copyright MapModelers. All Rights Reserved.
//
// Textures for the library (T-a).
//
// A texture comes in from anywhere on disk and leaves as one of ours: named
// T_Sp_<Stem>, its kind read off its suffix, and - when conformed, which is
// the default - its source rewritten as a byte-exact TGA under the source
// root (<Root>/Textures/T_Sp_<Stem>.tga) that the asset then reads from, so
// the file the team keeps is the truth and resolves for everyone. Linked
// instead, the asset reads the file where it was found; the window says
// whether that file is one the team can reach (Portable) or only this
// machine can (LocalOnly). The same words, statuses, container bytes and
// policy record as MapLink's texture manager, so the two libraries can be
// merged one day (D55/M7).

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
class UTexture;
class UTexture2D;

namespace SurfTextures
{
	// --- Naming -----------------------------------------------------------

	/** "T_Sp_" */
	FString Prefix();

	/** The stem a file or asset name gives: prefixes dropped, anything that is not a letter, a digit or _ made _, runs of _ folded, at most 48 long; "Texture" for nothing. */
	FString StemFrom(const FString& FileOrAssetName);

	/** "T_Sp_<Stem>" */
	FString AssetNameFor(const FString& Stem);

	/** True when the name is exactly T_Sp_ and a stem. */
	bool IsOurs(const FString& AssetName);

	/** What a texture is for, read off its suffix: _N a normal map; _R _AO _M _H _DISP _ORM _RMA _MASK _A linear data; anything else colour (_D is the diffuse). */
	enum class ESurfTextureKind : uint8
	{
		Colour,
		Normal,
		Linear,
	};
	ESurfTextureKind KindFromName(const FString& Name);
	const TCHAR* KindName(ESurfTextureKind Kind);

	/** sRGB and compression for the kind, and a rebuild. */
	void ApplyKind(UTexture2D* Texture, ESurfTextureKind Kind);

	// --- Where things are ---------------------------------------------------

	/** The source root: the setting, else <Project>/SurfacingPro; absolute, no trailing slash. */
	FString SourceRoot();

	/** <Root>/Textures */
	FString SourceDir(const FString& Root);

	/** <Root>/Textures/<AssetName>.tga */
	FString SourcePathFor(const FString& Root, const FString& AssetName);

	/** <LibraryRoot>/Textures - where the assets go. */
	FString ContentFolder(const FString& LibraryRoot);

	/** The plugin's sample textures on disk, to author from. */
	FString SamplesDir();

	// --- Status -------------------------------------------------------------

	enum class ESurfTextureStatus : uint8
	{
		/** Ours, named right, reading its TGA under the source root. */
		Conform,
		/** Reading a file inside the project's tree: the team can reach it. */
		Portable,
		/** Reading a file outside the project's tree: only this machine can. */
		LocalOnly,
		/** Its file is gone. */
		Broken,
		/** Not imported from a file at all (made in the editor, or the engine's). */
		External,
	};
	const TCHAR* StatusName(ESurfTextureStatus Status);

	/** The file a texture asset reads, resolved to an absolute path from the registry's record; empty when it has none. */
	FString SourceFileOf(const FAssetData& Asset);

	ESurfTextureStatus StatusOf(const FAssetData& Asset, const FString& Root);

	// --- The record ---------------------------------------------------------

	/** One line of <Root>/texture_policy.json: what the team decided about one texture. */
	struct FSurfTexturePolicyEntry
	{
		FString AssetPath;
		FString Class = TEXT("SurfacingPro");
		bool bSRGB = true;
		FString Compression;
		FString LastConform;
		bool bLossyReduction = false;
		FString Note;
	};

	/** The record, read and written whole; a missing file is an empty record. */
	class FSurfTexturePolicy
	{
	public:
		static FString PathFor(const FString& Root);

		bool Load(const FString& Root);
		bool Save(const FString& Root) const;

		const FSurfTexturePolicyEntry* Find(const FString& AssetPath) const;
		void Set(const FSurfTexturePolicyEntry& Entry);
		bool Remove(const FString& AssetPath);
		const TArray<FSurfTexturePolicyEntry>& Entries() const { return Lines; }

	private:
		TArray<FSurfTexturePolicyEntry> Lines;
	};

	// --- Import --------------------------------------------------------------

	struct FSurfTextureImportOptions
	{
		/** The library root the assets go under (<LibraryRoot>/Textures). */
		FString LibraryRoot;

		/** The source root the TGAs go under; SourceRoot() when empty. */
		FString SourceRootOverride;

		/** Conform (the default): rewrite the source as our TGA and read from it. Off: link the file where it is. */
		bool bConform = true;

		/** Longest edge a conformed texture keeps; 0 keeps every pixel. Larger sources are scaled down on the way in, and the record says so. */
		int32 MaxSize = 0;

		/** Write the assets to disk. Off in tests. */
		bool bSave = true;
	};

	struct FSurfTextureImportEntry
	{
		FString File;
		FString AssetPath;
		FString SourceAfter;
		ESurfTextureStatus Status = ESurfTextureStatus::Broken;
		ESurfTextureKind Kind = ESurfTextureKind::Colour;
		bool bLossy = false;
		bool bResized = false;
		/** Non-uniform alpha went into its own texture. */
		FString AlphaAssetPath;
		TArray<FString> Warnings;
		FString Error;
		UTexture2D* Texture = nullptr;
	};

	struct FSurfTextureImportReport
	{
		TArray<FSurfTextureImportEntry> Entries;
		int32 Imported = 0;
		int32 Failed = 0;

		/** The verb the line uses: import / imported by default; conform / conformed, link / linked for those. */
		FString Verb = TEXT("import");
		FString Done = TEXT("imported");

		/** One line for the foot: "imported 2 textures, conformed" / "import: <why>". */
		FString Line() const;
	};

	/** Bring files in: import, name, kind, conform or link, record. */
	FSurfTextureImportReport Import(const TArray<FString>& Files, const FSurfTextureImportOptions& Options);

	/** The image files among these, by extension (tga png jpg jpeg bmp exr tif tiff psd hdr). */
	bool IsImageFile(const FString& Path);

	// --- The library's textures ---------------------------------------------

	/** Every texture asset under <LibraryRoot>/Textures, by name. */
	void ListProject(const FString& LibraryRoot, TArray<FAssetData>& Out);

	// --- Onto a material ------------------------------------------------------

	/** The texture parameters a material offers, by name. */
	void TextureParametersOf(UMaterialInterface* Material, TArray<FName>& Out);

	/** Put the texture on the instance's parameter; saved when asked. False with the reason. */
	bool SetTextureParameter(UMaterialInstanceConstant* Instance, FName Parameter, UTexture* Texture, bool bSave, FString& OutError);

	// --- Keeping the library right (T-b) ---------------------------------------

	/** True when the file a texture reads is newer than what it read last (its import record's timestamp). Gone is Broken, not stale. */
	bool IsStale(const FAssetData& Asset);

	/** Read the file again, the kind kept; saved when asked. False with the reason. */
	bool Reimport(UTexture2D* Texture, bool bSave, FString& OutError);

	/**
	 * Make library textures conform: each one's own pixels written as our TGA
	 * under the source root and read from there, a varying alpha split off as
	 * on import. One not named T_Sp_ is renamed so first (a redirector stays
	 * behind) when it can be saved; kept as it is, with a warning, otherwise.
	 * SourceRootOverride, MaxSize and bSave apply; LibraryRoot is not needed.
	 */
	FSurfTextureImportReport Conform(const TArray<UTexture2D*>& Textures, const FSurfTextureImportOptions& Options);

	/** Point a texture at a file where it is and read from it - for one whose file is gone, or to swap its source. The kind is kept; recorded as a link. */
	FSurfTextureImportReport Link(UTexture2D* Texture, const FString& File, const FSurfTextureImportOptions& Options);

	/** The TGAs under <Root>/Textures that no library texture reads, by name: the strays. */
	void ListOrphans(const FString& LibraryRoot, const FString& Root, TArray<FString>& Out);
}
