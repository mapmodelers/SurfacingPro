// Copyright MapModelers. All Rights Reserved.
//
// Shape operators on a patch's control net.
//
// Each one is a pure function of an input patch, a set of point indices and
// some parameters. That is deliberate: an operator that reads no state and
// writes nothing but the patch can be re-run from the original as many times
// as a slider moves, which is what makes live preview possible at all.
//
// The rule for every operator here: given the same input it gives the same
// output, and running it twice from the original is the same as running it
// once. Anything that accumulates cannot be previewed.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

namespace SurfPatchOps
{
	/** The control net's area, cell by cell, in cm2. */
	SURFACINGPRO_API double NetArea(const FSurfPatch& Patch);

	/**
	 * A patch that is a line or a point (S245a): valid dimensions and no
	 * surface. Nothing a person wants - the leftovers of an extrude undone
	 * halfway, saved into an asset, and reported on every open since.
	 */
	SURFACINGPRO_API bool HasNoSurface(const FSurfPatch& Patch);

	/**
	 * Laplacian smoothing: each point moves towards the average of its four
	 * grid neighbours, Strength of the way, Iterations times.
	 *
	 * Indices empty means the whole patch. bPreserveBorder holds the outer
	 * ring still, which is nearly always wanted - a smoothed patch that pulls
	 * its own edges inward no longer meets whatever it was built against.
	 */
	SURFACINGPRO_API void Smooth(FSurfPatch& Patch, const TArray<int32>& Indices,
		double Strength, int32 Iterations, bool bPreserveBorder);

	/**
	 * Push points along the patch's local normal. Positive lifts away from
	 * the surface; the falloff weights are applied by the caller.
	 */
	SURFACINGPRO_API void Inflate(FSurfPatch& Patch, const TArray<int32>& Indices, double DistanceCm);

	/**
	 * Pull points towards the plane that best fits them. Strength 1 flattens
	 * completely - the fast way to make a wall out of a wobbly sheet.
	 */
	SURFACINGPRO_API void Flatten(FSurfPatch& Patch, const TArray<int32>& Indices, double Strength);

	/**
	 * Each point pushed a random way, up to AmountCm, on the axes the mask
	 * allows (1 or 0 per axis) - or along its own normal when bAlongNormal.
	 * The same seed gives the same push, so a knob can tune the amount.
	 */
	SURFACINGPRO_API void Random(FSurfPatch& Patch, const TArray<int32>& Indices, double AmountCm, const FVector& AxisMask, bool bAlongNormal, int32 Seed);

	/**
	 * Round off the crease along one row or column.
	 *
	 * The line is replaced by 2 * Segments lines, spread WidthCm either side
	 * along the surface and pulled towards the corner by Roundness - so the
	 * hard fold becomes an arc. This is bevel in the sense a grid can mean it:
	 * a patch has no mesh edges, it has rows and columns, and the thing you
	 * want rounded is where one of them creases.
	 *
	 * Returns false when the patch has no room to grow, rather than producing
	 * something smaller than asked for.
	 */
	SURFACINGPRO_API bool BevelLine(FSurfPatch& Patch, bool bColumn, int32 Index,
		double WidthCm, int32 Segments, double Roundness, bool bKeepCorner = false);

	/**
	 * Collapse the given points onto one position.
	 *
	 * A grid cannot lose a point - its dimensions are its structure - so a
	 * weld moves points on top of each other instead of removing them. The
	 * cells between them become degenerate and the mesh builder drops them,
	 * which is how a quad grid ends up with a triangle in it.
	 *
	 * bAtCentre welds to the average of the points; otherwise they go to the
	 * first one given, which is what "weld to the last one picked" wants.
	 * Returns false for fewer than two points.
	 */
	SURFACINGPRO_API bool Weld(FSurfPatch& Patch, const TArray<int32>& Indices, bool bAtCentre);

	/**
	 * Collapse every group of points already closer together than the
	 * tolerance. Returns how many points were moved.
	 *
	 * This is the tidy-up after dragging an edge onto another one by eye:
	 * the points look coincident and are not, and a seam shows wherever they
	 * are not. Indices empty means the whole patch.
	 */
	SURFACINGPRO_API int32 WeldByDistance(FSurfPatch& Patch, const TArray<int32>& Indices, double ToleranceCm);

	/** The grid normal at a control point, from its neighbours. */
	SURFACINGPRO_API FVector PointNormal(const FSurfPatch& Patch, int32 X, int32 Y);
}
