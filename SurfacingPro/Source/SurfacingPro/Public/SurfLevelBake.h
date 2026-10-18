// Copyright MapModelers. All Rights Reserved.
//
// The level's own bake tick (S153b): "the tick bake should be per-level and
// recursive to all surfgroup contained". Kept on the persistent level as
// asset user data, so it is saved with the level and travels with it, and
// read by the automatic bake for every group and prefab instance the world
// holds.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "SurfLevelBake.generated.h"

class UWorld;

UCLASS()
class SURFACINGPRO_API USurfLevelBake : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Bake this level by itself before Play and before a save. */
	UPROPERTY(EditAnywhere, Category = "SurfacingPro", meta = (ToolTip = "The level bakes by itself before Play and before a save."))
	bool bAutoBake = true;

	/** The tick as the world's persistent level has it; on when it has none (S154a). */
	static bool IsOn(const UWorld* World);

	/** Set it, making the data if the level has none yet; the level is marked for saving. */
	static void SetOn(UWorld* World, bool bOn);
};
