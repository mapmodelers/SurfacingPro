// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialInfo.h"

#include "Materials/MaterialInterface.h"
#include "Interfaces/Interface_AssetUserData.h"

const USurfMaterialInfo* USurfMaterialInfo::Find(const UMaterialInterface* Material)
{
	if (!Material)
	{
		return nullptr;
	}

	// The interface's lookup is not const; reading through it changes nothing.
	UMaterialInterface* Mutable = const_cast<UMaterialInterface*>(Material);
	return Cast<USurfMaterialInfo>(Mutable->GetAssetUserDataOfClass(USurfMaterialInfo::StaticClass()));
}

USurfMaterialInfo* USurfMaterialInfo::FindOrAdd(UMaterialInterface* Material)
{
	if (!Material)
	{
		return nullptr;
	}

	if (USurfMaterialInfo* Existing = Cast<USurfMaterialInfo>(Material->GetAssetUserDataOfClass(USurfMaterialInfo::StaticClass())))
	{
		return Existing;
	}

	USurfMaterialInfo* Made = NewObject<USurfMaterialInfo>(Material, USurfMaterialInfo::StaticClass(), NAME_None, RF_Transactional);
	Material->AddAssetUserData(Made);
	return Made;
}

FVector2D USurfMaterialInfo::ScaleFor(const UMaterialInterface* Material)
{
	const USurfMaterialInfo* Info = Find(Material);
	if (!Info || FMath::IsNearlyZero(Info->DefaultScale.X) || FMath::IsNearlyZero(Info->DefaultScale.Y))
	{
		return FVector2D(1.0, 1.0);
	}
	return Info->DefaultScale;
}

// --- The stamp ----------------------------------------------------------------

const USurfMaterialStamp* USurfMaterialStamp::Find(const UObject* Asset)
{
	// The interface's lookup is not const; reading through it changes nothing.
	IInterface_AssetUserData* Holder = Asset ? Cast<IInterface_AssetUserData>(const_cast<UObject*>(Asset)) : nullptr;
	return Holder ? Cast<USurfMaterialStamp>(Holder->GetAssetUserDataOfClass(USurfMaterialStamp::StaticClass())) : nullptr;
}

USurfMaterialStamp* USurfMaterialStamp::FindOrAdd(UObject* Asset)
{
	IInterface_AssetUserData* Holder = Asset ? Cast<IInterface_AssetUserData>(Asset) : nullptr;
	if (!Holder)
	{
		return nullptr;
	}

	if (USurfMaterialStamp* Existing = Cast<USurfMaterialStamp>(Holder->GetAssetUserDataOfClass(USurfMaterialStamp::StaticClass())))
	{
		return Existing;
	}

	USurfMaterialStamp* Made = NewObject<USurfMaterialStamp>(Asset, USurfMaterialStamp::StaticClass(), NAME_None, RF_Transactional);
	Holder->AddAssetUserData(Made);
	return Made;
}
