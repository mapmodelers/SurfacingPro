// Copyright MapModelers. All Rights Reserved.
//
// Projection surfacing. UVs are computed from world position against two
// axes; nothing is ever unwrapped.
//
// Conventions, pinned by test:
//   UV.X = ((Dot(P, UAxis) + Shift.X) * 0.01) / Scale.X
//   UV.Y = ((Dot(P, VAxis) + Shift.Y) * 0.01) / Scale.Y
// P is centimetres, Shift is centimetres, Scale is metres per tile.
// Shift is therefore independent of Scale: +1 moves one centimetre, never
// one tile - which is the behaviour every mapper eventually asks for.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

namespace SurfProjection
{
	/**
	 * Axial base axes for a normal - the projection you get from Align.
	 * Chosen by the normal's dominant component. This table is a convention,
	 * not a derivation: it is pinned by test so it can never drift.
	 */
	SURFACINGPRO_API void BaseAxes(const FVector& Normal, FVector& OutU, FVector& OutV);

	/** UV for a world-space point under this projection. */
	SURFACINGPRO_API FVector2D ComputeUV(const FSurfFaceUV& UV, const FVector& WorldPos);

	/** Reset to the axial projection for this normal. Scale and Shift are kept. */
	SURFACINGPRO_API void Align(FSurfFaceUV& UV, const FVector& Normal);

	/** Signed rotation in degrees from the axial base axes, for display. */
	SURFACINGPRO_API double GetRotationDegrees(const FSurfFaceUV& UV, const FVector& Normal);

	/** Rebuild the axes as the base axes rotated by this many degrees around the normal. */
	SURFACINGPRO_API void SetRotationDegrees(FSurfFaceUV& UV, const FVector& Normal, double Degrees);

	/**
	 * Scale and shift so the material repeats a whole number of times across
	 * these vertices. RepeatU/V below 1 are clamped to 1.
	 */
	SURFACINGPRO_API void Fit(FSurfFaceUV& UV, const TArray<FVector>& WorldVerts, int32 RepeatU = 1, int32 RepeatV = 1);

	/** One tile across the face on each axis, aspect ratio discarded. */
	SURFACINGPRO_API void Stretch(FSurfFaceUV& UV, const TArray<FVector>& WorldVerts);

	/**
	 * Texture lock: carry the projection through a primitive transform so the
	 * texture stays where it is. Rotation and translation are exact; uniform
	 * scale adjusts Scale so density is preserved; non-uniform scale is
	 * approximated by its mean, which is the honest limit of a two-axis
	 * projection.
	 */
	SURFACINGPRO_API void ApplyTransform(FSurfFaceUV& UV, const FTransform& Transform);

	/** Texels per metre on each axis, given the material's texture dimensions. */
	SURFACINGPRO_API FVector2D TexelDensity(const FSurfFaceUV& UV, const FVector2D& TextureTexels);

	/** Copy the whole surface definition from one face onto another - the middle-click gesture. */
	SURFACINGPRO_API void CopySurfaceDefinition(const FSurfFace& From, FSurfFace& To);
}
