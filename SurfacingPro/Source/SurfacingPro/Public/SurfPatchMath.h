// Copyright MapModelers. All Rights Reserved.
//
// Patch tessellation. Uniform, never adaptive: even clusters and no sliver
// triangles is exactly what Nanite wants, and it means a patch tessellates
// the same way in the preview and at bake.

#pragma once

#include "CoreMinimal.h"
#include "SurfTypes.h"

struct SURFACINGPRO_API FSurfTessellation
{
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Tints;

	/** Layer opacity per vertex (D14). Interpolated from control point Weight. */
	TArray<float> Weights;

	TArray<uint32> Indices;

	/** Vertex grid dimensions of the tessellated surface. */
	int32 NumU = 0;
	int32 NumV = 0;

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Indices.Num() / 3; }

	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		UVs.Reset();
		Tints.Reset();
		Weights.Reset();
		Indices.Reset();
		NumU = 0;
		NumV = 0;
	}
};

namespace SurfPatchMath
{
	/**
	 * Nine stacked layers is the hard maximum and three the rule of thumb,
	 * both from overdraw: every layer shades its overlap region again (D14).
	 * The bar warns past the setting; the verb stops at this.
	 */
	static constexpr int32 MaxStackDepth = 9;

	/** Vertex count along U for this patch at its current subdivision. */
	SURFACINGPRO_API int32 TessellatedNumU(const FSurfPatch& Patch);
	SURFACINGPRO_API int32 TessellatedNumV(const FSurfPatch& Patch);

	/**
	 * Tessellate into a vertex grid plus indices.
	 * Returns false when the patch dimensions are illegal for its type.
	 */
	SURFACINGPRO_API bool Tessellate(const FSurfPatch& Patch, FSurfTessellation& Out);

	/**
	 * Position at normalised (U, V) across the whole patch, both in [0, 1].
	 * Curve patches evaluate the biquadratic net; Sheet patches interpolate
	 * the control grid bilinearly.
	 */
	SURFACINGPRO_API FVector Evaluate(const FSurfPatch& Patch, double U, double V);

	/** Quadratic Bezier basis, the degree the control grid's odd parity implies. */
	FORCEINLINE double Bernstein2(int32 Index, double T)
	{
		const double OneMinusT = 1.0 - T;
		switch (Index)
		{
		case 0:  return OneMinusT * OneMinusT;
		case 1:  return 2.0 * T * OneMinusT;
		default: return T * T;
		}
	}

	/** A flat rectangular patch of the given control dimensions, for tests and defaults. */
	SURFACINGPRO_API FSurfPatch MakeFlatPatch(ESurfPatchType Type, int32 Width, int32 Height, const FVector& Origin, const FVector& SizeU, const FVector& SizeV);

	/**
	 * Displace a tessellation along its normals by StackLevel * OffsetCm.
	 * This is how stacked layers are separated (D14): at bake and in the
	 * preview, never as world position offset in a material.
	 */
	SURFACINGPRO_API void ApplyStackOffset(FSurfTessellation& Tess, int32 StackLevel, double OffsetCm);
}
