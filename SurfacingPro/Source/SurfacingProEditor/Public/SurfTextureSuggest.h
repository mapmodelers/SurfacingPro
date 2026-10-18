// Copyright MapModelers. All Rights Reserved.
//
// Texture suggestions (M-E4): a texture put on one parameter of an instance
// names its set - T_Sp_Brick_N goes with T_Sp_Brick and T_Sp_Brick_R - so
// the other texture parameters can be offered the siblings that fit them.
// Which sibling fits which parameter is learned from the library's other
// instances first (an instance of the same parent with a _N on Normal says
// so), and read off the parameter's name when nothing has been seen yet.
// Suggestions are offered, never applied on their own.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class UMaterialInstanceConstant;

namespace SurfTextureSuggest
{
	/** "T_Sp_Brick_N" -> "T_Sp_Brick" and "N"; a name ending in no known suffix -> itself and "". The suffix comes back upper-case. */
	void Split(const FString& AssetName, FString& OutBase, FString& OutSuffix);

	/** The suffixes a parameter's name calls for, likeliest first ("Normal" -> N; "Roughness" -> R, ORM, RMA; "BaseColor" -> none, C, BC ...); empty when the name says nothing. */
	void SuffixesFor(FName Parameter, TArray<FString>& Out);

	struct FSurfTextureSuggestion
	{
		/** The parameter that could take it. */
		FName Parameter;
		/** The sibling texture. */
		FAssetData Texture;
		/** Why: "as in MI_Wood", or "by its name". */
		FString Why;
	};

	/** Every texture under the library root, by upper-cased asset name. */
	void IndexTextures(const FString& LibraryRoot, TMap<FString, FAssetData>& Out);

	/**
	 * The suggestions for the other texture parameters of Instance, from the
	 * texture on SetParameter: for each, the sibling of that texture (same
	 * base, another suffix) that fits it - by how the library's other
	 * instances pair suffixes with that parameter (those of the same parent
	 * counting most), else by the parameter's name - when the library has it
	 * and the parameter does not hold one of the set already.
	 */
	void Suggest(UMaterialInstanceConstant* Instance, FName SetParameter, const FString& LibraryRoot, TArray<FSurfTextureSuggestion>& Out);
}
