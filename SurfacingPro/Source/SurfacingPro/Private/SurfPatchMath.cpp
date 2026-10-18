// Copyright MapModelers. All Rights Reserved.

#include "SurfPatchMath.h"

namespace SurfPatchMath
{
namespace
{
	/** Sub-patches along one axis. A 3-wide curve is one, a 5-wide is two. */
	FORCEINLINE int32 NumSubPatches(int32 ControlDim)
	{
		return FMath::Max(1, (ControlDim - 1) / 2);
	}

	/** Map a tessellated index to its sub-patch and local parameter. */
	FORCEINLINE void SplitIndex(int32 Index, int32 Subdiv, int32 NumSub, int32& OutSub, double& OutT)
	{
		OutSub = FMath::Clamp(Index / Subdiv, 0, NumSub - 1);
		const int32 Local = Index - OutSub * Subdiv;
		OutT = static_cast<double>(Local) / static_cast<double>(Subdiv);
	}

	struct FBlend
	{
		FVector Pos = FVector::ZeroVector;
		FVector2D UV = FVector2D::ZeroVector;
		double Weight = 0.0;
		double R = 0.0, G = 0.0, B = 0.0, A = 0.0;

		void Accumulate(const FSurfControlPoint& P, double W)
		{
			Pos += P.Pos * W;
			UV += P.UV * W;
			Weight += P.Weight * W;
			R += P.Tint.R * W;
			G += P.Tint.G * W;
			B += P.Tint.B * W;
			A += P.Tint.A * W;
		}

		FColor GetTint() const
		{
			return FColor(
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A), 0, 255)));
		}
	};

	/** Evaluate one biquadratic sub-patch of a Curve. */
	FBlend EvaluateCurveSubPatch(const FSurfPatch& Patch, int32 SubU, int32 SubV, double TU, double TV)
	{
		FBlend Blend;

		const int32 BaseX = SubU * 2;
		const int32 BaseY = SubV * 2;

		for (int32 J = 0; J < 3; ++J)
		{
			const double BV = Bernstein2(J, TV);
			if (BV == 0.0)
			{
				continue;
			}

			for (int32 I = 0; I < 3; ++I)
			{
				const double BU = Bernstein2(I, TU);
				if (BU == 0.0)
				{
					continue;
				}

				Blend.Accumulate(Patch.At(BaseX + I, BaseY + J), BU * BV);
			}
		}

		return Blend;
	}

	/** Area-weighted vertex normals from the index buffer. */
	/**
	 * From the winding, and only the winding. An earlier version also negated
	 * by bInvert, which cancelled the index flip above and left an inverted
	 * patch lit as though it were not.
	 */
	void ComputeNormals(FSurfTessellation& Tess)
	{
		Tess.Normals.Reset();
		Tess.Normals.AddZeroed(Tess.Positions.Num());

		for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
		{
			const uint32 I0 = Tess.Indices[I];
			const uint32 I1 = Tess.Indices[I + 1];
			const uint32 I2 = Tess.Indices[I + 2];

			const FVector& P0 = Tess.Positions[I0];
			const FVector& P1 = Tess.Positions[I1];
			const FVector& P2 = Tess.Positions[I2];

			// Unnormalised, so the cross product's magnitude weights by area.
			const FVector FaceNormal = FVector::CrossProduct(P1 - P0, P2 - P0);

			Tess.Normals[I0] += FaceNormal;
			Tess.Normals[I1] += FaceNormal;
			Tess.Normals[I2] += FaceNormal;
		}

		for (FVector& Normal : Tess.Normals)
		{
			Normal = Normal.GetSafeNormal();
			if (Normal.IsNearlyZero())
			{
				Normal = FVector(0.0, 0.0, 1.0);
			}
		}
	}
}

int32 TessellatedNumU(const FSurfPatch& Patch)
{
	if (Patch.Type == ESurfPatchType::Sheet)
	{
		return Patch.Width;
	}

	const int32 Subdiv = FMath::Clamp(Patch.SubdivU, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
	return NumSubPatches(Patch.Width) * Subdiv + 1;
}

int32 TessellatedNumV(const FSurfPatch& Patch)
{
	if (Patch.Type == ESurfPatchType::Sheet)
	{
		return Patch.Height;
	}

	const int32 Subdiv = FMath::Clamp(Patch.SubdivV, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
	return NumSubPatches(Patch.Height) * Subdiv + 1;
}

FVector Evaluate(const FSurfPatch& Patch, double U, double V)
{
	if (!Patch.HasValidDimensions())
	{
		return FVector::ZeroVector;
	}

	U = FMath::Clamp(U, 0.0, 1.0);
	V = FMath::Clamp(V, 0.0, 1.0);

	if (Patch.Type == ESurfPatchType::Sheet)
	{
		const double FX = U * (Patch.Width - 1);
		const double FY = V * (Patch.Height - 1);

		const int32 X0 = FMath::Clamp(static_cast<int32>(FX), 0, Patch.Width - 1);
		const int32 Y0 = FMath::Clamp(static_cast<int32>(FY), 0, Patch.Height - 1);
		const int32 X1 = FMath::Min(X0 + 1, Patch.Width - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, Patch.Height - 1);

		const double TX = FX - X0;
		const double TY = FY - Y0;

		const FVector A = FMath::Lerp(Patch.At(X0, Y0).Pos, Patch.At(X1, Y0).Pos, TX);
		const FVector B = FMath::Lerp(Patch.At(X0, Y1).Pos, Patch.At(X1, Y1).Pos, TX);
		return FMath::Lerp(A, B, TY);
	}

	const int32 NumSubU = NumSubPatches(Patch.Width);
	const int32 NumSubV = NumSubPatches(Patch.Height);

	const double GlobalU = U * NumSubU;
	const double GlobalV = V * NumSubV;

	const int32 SubU = FMath::Clamp(static_cast<int32>(GlobalU), 0, NumSubU - 1);
	const int32 SubV = FMath::Clamp(static_cast<int32>(GlobalV), 0, NumSubV - 1);

	return EvaluateCurveSubPatch(Patch, SubU, SubV, GlobalU - SubU, GlobalV - SubV).Pos;
}

bool Tessellate(const FSurfPatch& Patch, FSurfTessellation& Out)
{
	Out.Reset();

	if (!Patch.HasValidDimensions())
	{
		return false;
	}

	const int32 NumU = TessellatedNumU(Patch);
	const int32 NumV = TessellatedNumV(Patch);
	const int32 VertCount = NumU * NumV;

	Out.NumU = NumU;
	Out.NumV = NumV;
	Out.Positions.Reserve(VertCount);
	Out.UVs.Reserve(VertCount);
	Out.Tints.Reserve(VertCount);
	Out.Weights.Reserve(VertCount);

	if (Patch.Type == ESurfPatchType::Sheet)
	{
		// Control points are the vertices. Nothing to evaluate.
		for (int32 Y = 0; Y < NumV; ++Y)
		{
			for (int32 X = 0; X < NumU; ++X)
			{
				const FSurfControlPoint& P = Patch.At(X, Y);
				Out.Positions.Add(P.Pos);
				Out.UVs.Add(P.UV);
				Out.Tints.Add(P.Tint);
				Out.Weights.Add(P.Weight);
			}
		}
	}
	else
	{
		const int32 SubdivU = FMath::Clamp(Patch.SubdivU, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
		const int32 SubdivV = FMath::Clamp(Patch.SubdivV, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
		const int32 NumSubU = NumSubPatches(Patch.Width);
		const int32 NumSubV = NumSubPatches(Patch.Height);

		for (int32 Y = 0; Y < NumV; ++Y)
		{
			int32 SubV;
			double TV;
			SplitIndex(Y, SubdivV, NumSubV, SubV, TV);

			for (int32 X = 0; X < NumU; ++X)
			{
				int32 SubU;
				double TU;
				SplitIndex(X, SubdivU, NumSubU, SubU, TU);

				const FBlend Blend = EvaluateCurveSubPatch(Patch, SubU, SubV, TU, TV);
				Out.Positions.Add(Blend.Pos);
				Out.UVs.Add(Blend.UV);
				Out.Tints.Add(Blend.GetTint());
				Out.Weights.Add(static_cast<float>(FMath::Clamp(Blend.Weight, 0.0, 1.0)));
			}
		}
	}

	// Indices. Sheets honour the per-quad diagonal flip stored on the quad's
	// lower-left control point; curves are uniform so the diagonal is fixed.
	Out.Indices.Reserve((NumU - 1) * (NumV - 1) * 6);

	for (int32 Y = 0; Y + 1 < NumV; ++Y)
	{
		for (int32 X = 0; X + 1 < NumU; ++X)
		{
			const uint32 V00 = static_cast<uint32>(Y * NumU + X);
			const uint32 V10 = V00 + 1;
			const uint32 V01 = static_cast<uint32>((Y + 1) * NumU + X);
			const uint32 V11 = V01 + 1;

			const bool bTurned = Patch.Type == ESurfPatchType::Sheet && Patch.At(X, Y).bTurnedEdge != 0;

			if (bTurned)
			{
				Out.Indices.Add(V00); Out.Indices.Add(V10); Out.Indices.Add(V01);
				Out.Indices.Add(V10); Out.Indices.Add(V11); Out.Indices.Add(V01);
			}
			else
			{
				Out.Indices.Add(V00); Out.Indices.Add(V10); Out.Indices.Add(V11);
				Out.Indices.Add(V00); Out.Indices.Add(V11); Out.Indices.Add(V01);
			}
		}
	}

	if (Patch.bInvert)
	{
		for (int32 I = 0; I + 2 < Out.Indices.Num(); I += 3)
		{
			Swap(Out.Indices[I + 1], Out.Indices[I + 2]);
		}
	}

	/**
	 * The winding is not touched by bInvert, and that is deliberate (S115b).
	 *
	 * SurfacingPro.Math.Patch.Normals has said so since it was written: the
	 * winding and the shading normal are **opposite on purpose**, because the
	 * order Unreal reads as front-facing is the one the right-hand rule calls
	 * the back. Agreeing would mean the surface is culled from the side it is
	 * lit on. Inverting therefore turns the normal over and leaves the winding
	 * where it is, so the pair stays opposite.
	 *
	 * I spent three builds re-deriving that from scratch and breaking it twice
	 * on the way. **The test was right and I did not read it.**
	 */

	// Normals come from the winding above, while it still reads as the plain
	// right-hand rule: a grid ascending in X and Y gives +Z.
	ComputeNormals(Out);

	// Then the winding is flipped once more, and only the winding. Unreal
	// treats the opposite order as front-facing, so without this a patch is
	// lit as though it faces you and culled as though it faces away - which
	// looks exactly like a surface that has been built upside down, and was
	// reported as one twice.
	for (int32 I = 0; I + 2 < Out.Indices.Num(); I += 3)
	{
		Swap(Out.Indices[I + 1], Out.Indices[I + 2]);
	}

	return true;
}

FSurfPatch MakeFlatPatch(ESurfPatchType Type, int32 Width, int32 Height, const FVector& Origin, const FVector& SizeU, const FVector& SizeV)
{
	FSurfPatch Patch;
	Patch.EnsureId();
	Patch.Type = Type;
	Patch.Width = Width;
	Patch.Height = Height;
	Patch.Points.Reserve(Width * Height);

	const double DenomU = FMath::Max(1, Width - 1);
	const double DenomV = FMath::Max(1, Height - 1);

	for (int32 Y = 0; Y < Height; ++Y)
	{
		const double V = static_cast<double>(Y) / DenomV;
		for (int32 X = 0; X < Width; ++X)
		{
			const double U = static_cast<double>(X) / DenomU;

			FSurfControlPoint Point(Origin + SizeU * U + SizeV * V);
			Point.UV = FVector2D(U, V);
			Point.SavedUV = Point.UV;
			Patch.Points.Add(Point);
		}
	}

	return Patch;
}

void ApplyStackOffset(FSurfTessellation& Tess, int32 StackLevel, double OffsetCm)
{
	if (StackLevel <= 0 || OffsetCm <= 0.0)
	{
		return;
	}

	const double Distance = StackLevel * OffsetCm;
	const int32 Count = FMath::Min(Tess.Positions.Num(), Tess.Normals.Num());

	for (int32 I = 0; I < Count; ++I)
	{
		Tess.Positions[I] += Tess.Normals[I] * Distance;
	}
}

} // namespace SurfPatchMath
