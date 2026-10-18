// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

/**
 * The Assets tab of the SidePanel (S210a): prefabs and static meshes as
 * tiles, a search, a folder, placing by double-click or drag; on a tile's
 * right-click Place, Replace the picked with it, Details and Materials of
 * the picked, Open, Show in the content browser. "Its minimalist and can
 * display items and user may place them."
 */
namespace SurfAssetsTab
{
	TSharedRef<class SWidget> MakePanel();

	/** Every prefab and static mesh under Folder (recursive), by name. */
	void Gather(const FString& Folder, bool bPrefabs, bool bMeshes, bool bLevels, TArray<FAssetData>& Out);

	/** Words match the start of any word of the name; "p:" and "sm:" pick a kind. */
	bool Matches(const FAssetData& Asset, const FString& Typed);

	/** The picked actors' lines for any menu (S228a): Replace the picked with... (a picker), Details, Materials. */
	void AddActorLines(class FMenuBuilder& Menu);
	bool HasPickedActors();
}
