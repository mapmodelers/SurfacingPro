// Copyright MapModelers. All Rights Reserved.
//
// Surfacing verbs for patches.
//
// A brush face is flat, so one projection describes it completely. A patch is
// not, so its UVs live on the control points and are interpolated by the same
// basis that interpolates position - which is what makes the mapping follow
// the surface instead of sliding across it.
//
// The mapping is stored on the patch, not baked into the UVs, and Reapply
// re-derives them. That is the whole design: drag a control point and the
// texture keeps its density, because the mapping is a rule rather than a
// result. Two rules matter, and they answer different questions:
//
//   Natural - accumulate real distance along the control net, so a metre of
//             surface is a metre of texture wherever it bends. The mapping
//             for anything curved: pipes, arches, terrain.
//   Planar  - the same projection a brush face gets. A patch that meets a
//             wall must share the wall's projection or the seam shows, and no
//             distance-following mapping can do that.
//
// Shift, rotation, flips and swap are stored separately and applied after the
// mapping, so a nudged texture survives the next edit too. Scale is metres
// per tile throughout, matching FSurfFaceUV exactly, so density transfers
// between brushes and patches.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

namespace SurfPatchSurfacing
{
	/** Area-weighted average normal of the control net. Zero when degenerate. */
	SURFACINGPRO_API FVector AverageNormal(const FSurfPatch& Patch);

	/** Longest row and longest column of the control net, in centimetres. */
	SURFACINGPRO_API void MeasureExtent(const FSurfPatch& Patch, double& OutMaxRowCm, double& OutMaxColumnCm);

	/**
	 * Recompute control point UVs from the patch's stored mapping. Call after
	 * anything that moves a control point; Manual patches are left alone.
	 */
	SURFACINGPRO_API void Reapply(FSurfPatch& Patch);

	/**
	 * While held, Reapply leaves the UVs as they stand (S185b): "moved some
	 * of it with soft selection, and it also warped the UVs - a tick param
	 * toggle?" - the setting "UVs follow edits" off holds them through a
	 * move, a turn or a size.
	 */
	SURFACINGPRO_API int32& HoldUVsCount();
	struct FHoldUVs
	{
		bool bHeld;
		explicit FHoldUVs(bool bHold) : bHeld(bHold) { if (bHeld) { ++HoldUVsCount(); } }
		~FHoldUVs() { if (bHeld) { --HoldUVsCount(); } }
	};

	/** Switch to distance-following UVs. */
	SURFACINGPRO_API void SetNatural(FSurfPatch& Patch);

	/**
	 * Switch to the even mapping (S69c): every cell the same size in the
	 * texture, Scale meaning cells a tile. The patch arrives with one tile
	 * per cell; the knobs go on from there.
	 */
	SURFACINGPRO_API void SetEven(FSurfPatch& Patch);

	/** A mapping that measures along the control net - Natural or Even - and so carries its UV origin and shift from a source. */
	inline bool FollowsTheNet(ESurfPatchMapping Mapping)
	{
		return Mapping == ESurfPatchMapping::Natural || Mapping == ESurfPatchMapping::Grid;
	}

	/** Switch to a planar projection, axial from the patch's own average normal. */
	SURFACINGPRO_API void SetPlanar(FSurfPatch& Patch);

	/** Switch to a planar projection copied from elsewhere - a face, or another patch. */
	SURFACINGPRO_API void SetPlanarWith(FSurfPatch& Patch, const FSurfFaceUV& Projection);

	/** Freeze the current UVs and stop re-deriving them. */
	SURFACINGPRO_API void SetManual(FSurfPatch& Patch);

	/** Metres per tile. Values at or below zero are ignored. */
	SURFACINGPRO_API void SetScale(FSurfPatch& Patch, const FVector2D& MetresPerTile);

	/** Choose the scale that makes the patch span exactly RepeatU by RepeatV tiles. */
	SURFACINGPRO_API void FitScale(FSurfPatch& Patch, int32 RepeatU = 1, int32 RepeatV = 1);

	/**
	 * Fit to a trim sheet (§2f): the material declares how many cells a tile
	 * holds each way, and the patch spans a whole number of cells rather
	 * than tiles, its shift landed on a cell boundary so it sits on a cell.
	 */
	SURFACINGPRO_API void FitToTrim(FSurfPatch& Patch, int32 CellsU, int32 CellsV, const FVector2D& CellsPerTile);

	/** Slide the mapping, in tiles. */
	SURFACINGPRO_API void Shift(FSurfPatch& Patch, double TilesU, double TilesV);

	/** Turn the mapping about its own centre. */
	SURFACINGPRO_API void Rotate(FSurfPatch& Patch, double Degrees);

	SURFACINGPRO_API void FlipU(FSurfPatch& Patch);
	SURFACINGPRO_API void FlipV(FSurfPatch& Patch);

	/** Swap the two axes - the fix for a mapping that came out ninety degrees off. */
	SURFACINGPRO_API void SwapUV(FSurfPatch& Patch);

	/** Copy the whole surface definition from one patch onto another. */
	SURFACINGPRO_API void CopySurfaceDefinition(const FSurfPatch& From, FSurfPatch& To);

	/** Copy a brush face's surface onto a patch, projection included. */
	SURFACINGPRO_API void CopyFaceSurface(const FSurfFace& From, FSurfPatch& To);

	/**
	 * Texels per metre on each axis, measured across the patch as mapped.
	 * This is the number that says whether a surface matches its neighbours.
	 */
	SURFACINGPRO_API FVector2D TexelDensity(const FSurfPatch& Patch, const FVector2D& TextureTexels);
}
