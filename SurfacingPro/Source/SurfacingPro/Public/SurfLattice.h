// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

/**
 * The lattice round a patch (S189a): "add an option to create a lattice for
 * any patch, and user choose the lattice res x y z". A box of cage points;
 * each patch point keeps its place in the box; a move of the cage carries
 * the points by the smooth blend of how far the cage points went.
 */
namespace SurfLattice
{
	constexpr int32 MinRes = 2;
	constexpr int32 MaxRes = 8;

	/** A box of cage points round the patch's points, and each point's place in it. False with no points. */
	SURFACINGPRO_API bool Bind(FSurfPatch& Patch, int32 ResX, int32 ResY, int32 ResZ);

	/** A cage, and one place per point. */
	SURFACINGPRO_API bool IsBound(const FSurfPatch& Patch);

	/** The blend of the given cage points (or cage moves) at a place in the box. */
	SURFACINGPRO_API FVector Evaluate(const FSurfLattice& Lattice, const TArray<FVector>& CagePoints, const FVector& Place);

	/** Every point = where it stood + the blend of how far the cage has moved from CageFrom. */
	SURFACINGPRO_API void Deform(FSurfPatch& Patch, const TArray<FVector>& CageFrom, const TArray<FVector>& PointsFrom);

	/** How far every point moved together since the lattice last noted them - zero unless all moved alike (the whole patch moved). */
	SURFACINGPRO_API FVector FollowOffset(const FSurfPatch& Patch);

	/** The cage carried by that, and the points noted as they stand. */
	SURFACINGPRO_API void Follow(FSurfPatch& Patch);

	/** The points noted as they stand. */
	SURFACINGPRO_API void NoteRest(FSurfPatch& Patch);
}
