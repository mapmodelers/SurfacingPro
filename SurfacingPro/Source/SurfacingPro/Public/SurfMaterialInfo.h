// Copyright MapModelers. All Rights Reserved.
//
// The surfacing defaults a material carries with it.
//
// A material instance is the unit of surfacing (M3): a face or a patch wears
// a UMaterialInterface, any the user owns, nothing to convert. What used to
// need an asset of its own - metres per tile, the trim grid, whether the
// material is a layer over another - rides on the material itself as asset
// user data, so one asset in the library says everything about how it wants
// to be applied. Absent, the defaults below apply and nothing is written.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "SurfMaterialInfo.generated.h"

class UMaterialInterface;

UENUM(BlueprintType)
enum class ESurfMaterialUse : uint8
{
	/** Base layer. Opaque, no vertex alpha needed. */
	Opaque,
	/** Stacked over another patch; vertex alpha drives its opacity mask (D14). */
	Layer,
	/** Skipped at bake, kept for collision. */
	NoDraw,
	/** Collision only. */
	Clip
};

UCLASS(BlueprintType)
class SURFACINGPRO_API USurfMaterialInfo : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Metres per tile, applied to a face or patch the first time this material is put on it. */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FVector2D DefaultScale = FVector2D(1.0, 1.0);

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	ESurfMaterialUse Use = ESurfMaterialUse::Opaque;

	/** Trim / hotspot cells per tile. Zero means no trim grid; Fit snaps to the nearest cell when set. */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FVector2D TrimCellsPerTile = FVector2D::ZeroVector;

	bool HasTrimGrid() const
	{
		return TrimCellsPerTile.X > 0.0 && TrimCellsPerTile.Y > 0.0;
	}

	/** The info this material carries, or null when it carries none. */
	static const USurfMaterialInfo* Find(const UMaterialInterface* Material);

	/** The info this material carries, made and attached when it had none. Null only for a null material. */
	static USurfMaterialInfo* FindOrAdd(UMaterialInterface* Material);

	/** The scale a material asks for: its info's DefaultScale, or one metre per tile. */
	static FVector2D ScaleFor(const UMaterialInterface* Material);
};

/**
 * Where a seeded parent came from (D55/M2). The plugin's parents are copied
 * into the project's library so nothing of the user's depends on the plugin
 * being installed; each copy carries this, so a later version of the plugin
 * can tell a copy it made from one the user made or changed.
 */
UCLASS()
class SURFACINGPRO_API USurfMaterialStamp : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** The plugin asset this was copied from, as a package name. */
	UPROPERTY(VisibleAnywhere, Category = "Seed")
	FString Source;

	/** The plugin version that made the copy. */
	UPROPERTY(VisibleAnywhere, Category = "Seed")
	FString Version;

	/** The source file's hash when the copy was made; empty when the source had no file. */
	UPROPERTY(VisibleAnywhere, Category = "Seed")
	FString Hash;

	/** The copy's content when it was made (SurfMaterialSeed::ContentOf), so a copy edited since can be told from one left alone. Zero on copies made before this was recorded. */
	UPROPERTY(VisibleAnywhere, Category = "Seed")
	uint32 Content = 0;

	/** The stamp an asset carries, or null when it carries none or cannot carry one. */
	static const USurfMaterialStamp* Find(const UObject* Asset);

	/** The stamp, made and attached when the asset had none. Null for an asset that cannot carry user data. */
	static USurfMaterialStamp* FindOrAdd(UObject* Asset);
};
