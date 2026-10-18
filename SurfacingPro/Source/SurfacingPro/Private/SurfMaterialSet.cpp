// Copyright MapModelers. All Rights Reserved.

#include "SurfMaterialSet.h"

#include "Materials/MaterialInterface.h"

void FSurfMaterialSet::Reset(UMaterialInterface* Fallback)
{
	Slots.Reset();
	SlotByMaterial.Reset();
	Slots.Add(Fallback);
}

int32 FSurfMaterialSet::SlotFor(UMaterialInterface* Material)
{
	// Slot 0 is only ever the fallback, so a refresh can swap it without
	// touching what a face chose on purpose - even a face that chose the
	// very material the group falls back to gets a slot of its own.
	if (!Material)
	{
		return 0;
	}

	if (Slots.Num() == 0)
	{
		Slots.Add(nullptr);
	}

	if (const int32* Found = SlotByMaterial.Find(Material))
	{
		return *Found;
	}

	const int32 Slot = Slots.Add(Material);
	SlotByMaterial.Add(Material, Slot);
	return Slot;
}
