// Copyright MapModelers. All Rights Reserved.

#include "SurfProjection.h"

namespace SurfProjection
{
namespace
{
	/** Centimetres to metres. UVs are in tiles, and a tile is Scale metres. */
	inline constexpr double CmToM = 0.01;

	void ProjectExtent(const FSurfFaceUV& UV, const TArray<FVector>& Verts, FVector2D& OutMin, FVector2D& OutMax)
	{
		OutMin = FVector2D(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
		OutMax = FVector2D(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());

		for (const FVector& V : Verts)
		{
			const double U = FVector::DotProduct(V, UV.UAxis);
			const double W = FVector::DotProduct(V, UV.VAxis);

			OutMin.X = FMath::Min(OutMin.X, U);
			OutMin.Y = FMath::Min(OutMin.Y, W);
			OutMax.X = FMath::Max(OutMax.X, U);
			OutMax.Y = FMath::Max(OutMax.Y, W);
		}
	}
}

void BaseAxes(const FVector& Normal, FVector& OutU, FVector& OutV)
{
	const double AX = FMath::Abs(Normal.X);
	const double AY = FMath::Abs(Normal.Y);
	const double AZ = FMath::Abs(Normal.Z);

	if (AZ >= AX && AZ >= AY)
	{
		// Floor and ceiling.
		OutU = FVector(1.0, 0.0, 0.0);
		OutV = FVector(0.0, -1.0, 0.0);
	}
	else if (AX >= AY)
	{
		// Walls facing along X.
		OutU = FVector(0.0, 1.0, 0.0);
		OutV = FVector(0.0, 0.0, -1.0);
	}
	else
	{
		// Walls facing along Y.
		OutU = FVector(1.0, 0.0, 0.0);
		OutV = FVector(0.0, 0.0, -1.0);
	}
}

FVector2D ComputeUV(const FSurfFaceUV& UV, const FVector& WorldPos)
{
	const double ScaleX = FMath::IsNearlyZero(UV.Scale.X) ? 1.0 : UV.Scale.X;
	const double ScaleY = FMath::IsNearlyZero(UV.Scale.Y) ? 1.0 : UV.Scale.Y;

	return FVector2D(
		((FVector::DotProduct(WorldPos, UV.UAxis) + UV.Shift.X) * CmToM) / ScaleX,
		((FVector::DotProduct(WorldPos, UV.VAxis) + UV.Shift.Y) * CmToM) / ScaleY);
}

void Align(FSurfFaceUV& UV, const FVector& Normal)
{
	BaseAxes(Normal, UV.UAxis, UV.VAxis);
}

double GetRotationDegrees(const FSurfFaceUV& UV, const FVector& Normal)
{
	FVector BaseU, BaseV;
	BaseAxes(Normal, BaseU, BaseV);

	const FVector Axis = Normal.GetSafeNormal();
	const FVector CurrentU = UV.UAxis.GetSafeNormal();

	// Signed angle around the normal. Handedness-agnostic: the cross product
	// lies along the axis and its sign gives the direction of rotation.
	const double CosA = FVector::DotProduct(BaseU, CurrentU);
	const double SinA = FVector::DotProduct(Axis, FVector::CrossProduct(BaseU, CurrentU));

	return FMath::RadiansToDegrees(FMath::Atan2(SinA, CosA));
}

void SetRotationDegrees(FSurfFaceUV& UV, const FVector& Normal, double Degrees)
{
	FVector BaseU, BaseV;
	BaseAxes(Normal, BaseU, BaseV);

	const FQuat Rotation(Normal.GetSafeNormal(), FMath::DegreesToRadians(Degrees));

	UV.UAxis = Rotation.RotateVector(BaseU).GetSafeNormal();
	UV.VAxis = Rotation.RotateVector(BaseV).GetSafeNormal();
}

void Fit(FSurfFaceUV& UV, const TArray<FVector>& WorldVerts, int32 RepeatU, int32 RepeatV)
{
	if (WorldVerts.Num() == 0)
	{
		return;
	}

	RepeatU = FMath::Max(1, RepeatU);
	RepeatV = FMath::Max(1, RepeatV);

	FVector2D Min, Max;
	ProjectExtent(UV, WorldVerts, Min, Max);

	const double ExtentU = Max.X - Min.X;
	const double ExtentV = Max.Y - Min.Y;

	if (ExtentU > UE_DOUBLE_SMALL_NUMBER)
	{
		UV.Scale.X = (ExtentU * CmToM) / RepeatU;
		UV.Shift.X = -Min.X;
	}

	if (ExtentV > UE_DOUBLE_SMALL_NUMBER)
	{
		UV.Scale.Y = (ExtentV * CmToM) / RepeatV;
		UV.Shift.Y = -Min.Y;
	}
}

void Stretch(FSurfFaceUV& UV, const TArray<FVector>& WorldVerts)
{
	Fit(UV, WorldVerts, 1, 1);
}

void ApplyTransform(FSurfFaceUV& UV, const FTransform& Transform)
{
	const FQuat Rotation = Transform.GetRotation();
	const FVector Translation = Transform.GetTranslation();
	const FVector Scale3D = Transform.GetScale3D();

	const FVector NewU = Rotation.RotateVector(UV.UAxis).GetSafeNormal();
	const FVector NewV = Rotation.RotateVector(UV.VAxis).GetSafeNormal();

	// UV(R*P + T) must equal UV(P), so the shift absorbs the translation
	// measured along the rotated axes.
	UV.Shift.X -= FVector::DotProduct(Translation, NewU);
	UV.Shift.Y -= FVector::DotProduct(Translation, NewV);

	UV.UAxis = NewU;
	UV.VAxis = NewV;

	const double MeanScale = (FMath::Abs(Scale3D.X) + FMath::Abs(Scale3D.Y) + FMath::Abs(Scale3D.Z)) / 3.0;
	if (!FMath::IsNearlyEqual(MeanScale, 1.0) && MeanScale > UE_DOUBLE_SMALL_NUMBER)
	{
		UV.Scale *= MeanScale;
	}
}

FVector2D TexelDensity(const FSurfFaceUV& UV, const FVector2D& TextureTexels)
{
	const double ScaleX = FMath::IsNearlyZero(UV.Scale.X) ? 1.0 : UV.Scale.X;
	const double ScaleY = FMath::IsNearlyZero(UV.Scale.Y) ? 1.0 : UV.Scale.Y;

	return FVector2D(TextureTexels.X / ScaleX, TextureTexels.Y / ScaleY);
}

void CopySurfaceDefinition(const FSurfFace& From, FSurfFace& To)
{
	// The whole definition, material included: one click makes this face
	// match that one.
	To.UV = From.UV;
	To.Material = From.Material;
	To.SmoothingGroup = From.SmoothingGroup;
}

} // namespace SurfProjection
