// Copyright MapModelers. All Rights Reserved.
//
// The materials one preview mesh wears, as slots.
//
// A dynamic mesh component has no slot names: a triangle carries a material
// index, and the component carries that many materials. Slot 0 is the
// fallback - the group's own preview material, else the editor's checker -
// so a face or patch with no material of its own, or one whose material
// could not be found, always reads as the checker and never as nothing.
// Every other slot is one distinct material, in the order it was first met
// while building, which is the order of the primitives and so stable from
// one rebuild to the next. The compiler merges by the same set later.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;

struct SURFACINGPRO_API FSurfMaterialSet
{
	/** Slot 0 is the fallback and may be null (the component then shows the engine's default). */
	TArray<UMaterialInterface*> Slots;

	/** Start over with the fallback in slot 0. */
	void Reset(UMaterialInterface* Fallback);

	/** The slot this material renders through: 0 for null, a new slot the first time a material is seen. */
	int32 SlotFor(UMaterialInterface* Material);

	int32 Num() const { return Slots.Num(); }

private:
	TMap<UMaterialInterface*, int32> SlotByMaterial;
};
