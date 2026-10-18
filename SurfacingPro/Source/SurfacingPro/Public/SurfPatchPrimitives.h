// Copyright MapModelers. All Rights Reserved.
//
// Patch primitives, built from a bounding box.
//
// Every curved thing in this workflow is one of these. The control points sit
// on the box's corners and edge midpoints, which is what makes a quadratic
// bezier through them approximate an arc - and what makes the primitives
// predictable: the patch always fits the box you drew.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

UENUM(BlueprintType)
enum class ESurfPrimitiveKind : uint8
{
	/** Flat grid across the box footprint. */
	Simple,
	/** Full tube, 9 x 3. Wraps closed. */
	Cylinder,
	/** Quarter arc, 3 x 3. The corner piece everything is built from. */
	Bevel,
	/** Half arc, 5 x 3. */
	Endcap,
	/** Cylinder with the top row collapsed to a point. */
	Cone,
	/** Vertex grid across the box footprint, points are vertices. */
	Sheet
};

namespace SurfPatchPrimitives
{
	/**
	 * Build a primitive sized to the box, in the same space the box is in.
	 * Width and Height are used by Simple and Sheet only; the arc primitives
	 * have fixed control dimensions because their shape depends on it.
	 */
	/**
	 * FaceNormal is the face the primitive was made from: a flat grid lies on
	 * that face rather than on the box floor, and winds towards it.
	 */
	SURFACINGPRO_API FSurfPatch Make(ESurfPrimitiveKind Kind, const FBox& Box, int32 Width = 3, int32 Height = 3, const FVector& FaceNormal = FVector::UpVector);

	/** Control dimensions a kind produces, so callers can size UI and tests. */
	SURFACINGPRO_API void GetDimensions(ESurfPrimitiveKind Kind, int32& OutWidth, int32& OutHeight, int32 RequestedWidth = 3, int32 RequestedHeight = 3);

	/** Insert a row or column next to Index, duplicating and averaging. Curves gain two to stay odd. */
	SURFACINGPRO_API bool InsertRow(FSurfPatch& Patch, int32 AtRow);
	SURFACINGPRO_API bool InsertColumn(FSurfPatch& Patch, int32 AtColumn);

	/** Remove a row or column. Curves lose two to stay odd. Refuses below the minimum. */
	SURFACINGPRO_API bool RemoveRow(FSurfPatch& Patch, int32 AtRow);
	SURFACINGPRO_API bool RemoveColumn(FSurfPatch& Patch, int32 AtColumn);

	/** Even out control point spacing along each row and column. */
	SURFACINGPRO_API void Respace(FSurfPatch& Patch);

	/**
	 * Extend the grid past one of its four edges, by mirroring the spacing of
	 * the last two lines. This is how a patch grows: extrude the edge, drag it,
	 * extrude again - rather than building a big grid and pushing points about.
	 *
	 * bColumn extends along X, bFromEnd chooses which side. Returns false when
	 * the patch has no room left.
	 */
	/**
	 * Grow the patch past one of its edges.
	 *
	 * bInPlace puts the new line exactly on the edge it came from instead of
	 * one spacing further out. A drag that extrudes needs that: moving the
	 * pivot at the moment the drag starts is what sent the new line off in
	 * the direction of the cursor rather than along the axis being held.
	 */
	/**
	 * Join two patches into one along the edge they share.
	 *
	 * Only defined when an edge of each lies on an edge of the other, within
	 * the tolerance, and the two carry the same number of points - a grid has
	 * no way to express a seam where five points meet seven. The shared line
	 * is kept once, so joining an A x N to a B x N gives an (A + B - 1) x N.
	 *
	 * Either patch may be turned or reversed to make the edges meet: the
	 * caller should not have to have built them in a particular order.
	 * Returns false, having changed nothing, when no edge pair works.
	 */
	SURFACINGPRO_API bool Join(FSurfPatch& Out, const FSurfPatch& A, const FSurfPatch& B, double ToleranceCm);

	SURFACINGPRO_API bool ExtrudeEdge(FSurfPatch& Patch, bool bColumn, bool bFromEnd, int32 Count = 1, bool bInPlace = false);

	/**
	 * Cut a patch in two along one of its own lines. Both halves keep that
	 * line, so the seam is shared rather than merely adjacent - two patches
	 * that happen to end at the same place drift apart the first time either
	 * one is edited.
	 *
	 * Refused on a border line, where one half would be empty.
	 */
	SURFACINGPRO_API bool Split(const FSurfPatch& Patch, bool bColumn, int32 Index,
		FSurfPatch& OutA, FSurfPatch& OutB);

	/**
	 * Give a patch thickness: the original, a copy offset along the reversed
	 * normal, and a skirt joining them at each of the four borders.
	 *
	 * Six patches rather than one solid, because a patch is a surface - this
	 * is the same shape a modeller would get by extruding a plane, expressed
	 * in the only thing this tool has.
	 */
	/**
	 * How a solidify is built. Side: -1 the shell grows behind the surface
	 * (the front stays where it was), +1 in front of it, 0 half each way.
	 * Rim: the four skirts; without them, front and back only. Even: each
	 * point is pushed so the shell keeps its thickness at corners, where
	 * pushing along an averaged normal thins it (the wall of a box would be
	 * thinner at its edges).
	 */
	struct FSurfSolidifyOptions
	{
		int32 Side = -1;
		bool bRim = true;
		bool bEven = false;
	};

	SURFACINGPRO_API bool Solidify(const FSurfPatch& Patch, double ThicknessCm, TArray<FSurfPatch>& Out,
		const FSurfSolidifyOptions& Options = FSurfSolidifyOptions());

	/**
	 * Replace a curve with the sheet its tessellation produces, so the surface
	 * being looked at becomes the surface being edited. A curve's control
	 * points steer the shape from outside it - useful for blocking a form,
	 * useless once the work is moving individual vertices.
	 *
	 * The subdivision is reduced if the result would exceed a sheet's maximum
	 * dimensions; OutSubdivisionUsed says what it settled on, because silently
	 * producing a coarser surface than asked for is worse than saying so.
	 */
	SURFACINGPRO_API bool ConvertToSheet(const FSurfPatch& Patch, FSurfPatch& Out, int32* OutSubdivisionUsed = nullptr);
}
