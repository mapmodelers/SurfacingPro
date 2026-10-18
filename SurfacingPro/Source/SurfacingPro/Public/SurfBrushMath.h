// Copyright MapModelers. All Rights Reserved.
//
// Brush geometry. A brush is a set of half-spaces; its vertices are derived
// on demand by intersecting plane triples and discarding anything outside
// the remaining half-spaces. Convexity is therefore a property of the
// representation, not something a verb has to preserve by hand.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

/** One face of a derived hull: indices into FSurfHull::Vertices, wound CCW around the outward normal. */
struct SURFACINGPRO_API FSurfHullFace
{
	/** Index of the source FSurfBrush::Faces entry. */
	int32 SourceFaceIndex = INDEX_NONE;

	TArray<int32> VertexIndices;
};

struct SURFACINGPRO_API FSurfHull
{
	TArray<FVector> Vertices;

	/** Only faces that actually bound the hull. A redundant plane produces no entry. */
	TArray<FSurfHullFace> Faces;

	/** EditSerial of the brush this was derived from, for cache validation. */
	int32 EditSerial = INDEX_NONE;

	bool IsValid() const { return Vertices.Num() >= 4 && Faces.Num() >= 4; }

	void Reset()
	{
		Vertices.Reset();
		Faces.Reset();
		EditSerial = INDEX_NONE;
	}
};

namespace SurfBrushMath
{
	/** Distance tolerance in centimetres. Half a millimetre: finer than any grid step. */
	inline constexpr double DefaultEpsCm = 0.05;

	/**
	 * Derive vertices and wound faces from the brush's planes.
	 * Returns false and leaves Out reset if the planes bound no volume.
	 */
	SURFACINGPRO_API bool DeriveHull(const TArray<FSurfFace>& Faces, FSurfHull& Out, double EpsCm = DefaultEpsCm);

	/** Convenience overload that also stamps EditSerial for cache validation. */
	SURFACINGPRO_API bool DeriveHull(const FSurfBrush& Brush, FSurfHull& Out, double EpsCm = DefaultEpsCm);

	/** True when the point is inside or on every half-space. */
	SURFACINGPRO_API bool IsPointInside(const TArray<FSurfFace>& Faces, const FVector& Point, double EpsCm = DefaultEpsCm);

	/** Intersection of three planes. False when they are parallel or share a line. */
	SURFACINGPRO_API bool IntersectThreePlanes(const FPlane& A, const FPlane& B, const FPlane& C, FVector& Out);

	/** Axis-aligned box as six planes with outward normals. */
	SURFACINGPRO_API FSurfBrush MakeBox(const FBox& Box);

	/**
	 * A cylinder standing on Z inside the box: Sides planes round the
	 * box's inscribed ellipse, each tangent to it, so a four-sided one is
	 * the box and the flats of an eight-sided one lie on the box's sides -
	 * on the grid, where a brush belongs. Top and bottom from the box.
	 */
	SURFACINGPRO_API FSurfBrush MakeCylinder(const FBox& Box, int32 Sides);

	/**
	 * Split a brush with a plane. OutBack keeps the half behind the plane's
	 * normal, OutFront the half in front of it. Either can come back invalid
	 * when the plane misses the brush - check the return values.
	 *
	 * This is the whole of brush shaping. There is no CSG: a concave shape is
	 * several convex brushes, which is why a brush can never become invalid.
	 */
	SURFACINGPRO_API void SplitBrush(const FSurfBrush& Brush, const FPlane& SplitPlane,
		FSurfBrush& OutFront, bool& bOutFrontValid,
		FSurfBrush& OutBack, bool& bOutBackValid,
		double EpsCm = DefaultEpsCm);

	/** Signed volume in cubic centimetres. Positive for correctly wound outward faces. */
	/**
	 * A brush rebuilt from a set of corners (S112a).
	 *
	 * "we need be able to select brushes points (vertices) and edit them
	 * freely. theyre brushes ... you can make them any
	 * shape, as long it remain enclosed box."
	 *
	 * A brush is planes, not points - that is what makes it a brush and not a
	 * mesh - so moving a corner means finding the planes the moved corners
	 * imply. The convex hull of the points is exactly that: its faces are the
	 * planes, and a corner that ends up inside the hull is a corner that has
	 * stopped being one.
	 *
	 * Each new face takes its material and its projection from whichever of
	 * the old faces pointed most nearly the same way, so a textured brush
	 * comes through a drag still textured.
	 *
	 * False when the points bound no volume, or when any of them would end up
	 * inside the solid - the caller puts the brush back and says so.
	 */
	SURFACINGPRO_API bool RebuildFromPoints(const FSurfBrush& From, const TArray<FVector>& Points,
		FSurfBrush& Out, FString* OutWhy = nullptr, double EpsCm = DefaultEpsCm);

	SURFACINGPRO_API double Volume(const FSurfHull& Hull);

	/** Triangulate every hull face as a fan. Indices refer to FSurfHull::Vertices. */
	SURFACINGPRO_API void Triangulate(const FSurfHull& Hull, TArray<uint32>& OutIndices);
}
