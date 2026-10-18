// Copyright MapModelers. All Rights Reserved.

#include "SurfPatchSurfacing.h"
#include "SurfProjection.h"

namespace SurfPatchSurfacing
{
namespace
{
	/** Metres per tile, never zero - a zero scale would divide the mapping away. */
	FORCEINLINE FVector2D SafeScale(const FVector2D& Scale)
	{
		return FVector2D(
			FMath::IsNearlyZero(Scale.X) ? 1.0 : Scale.X,
			FMath::IsNearlyZero(Scale.Y) ? 1.0 : Scale.Y);
	}

	bool UVBounds(const FSurfPatch& Patch, FVector2D& OutMin, FVector2D& OutMax)
	{
		if (Patch.Points.Num() == 0)
		{
			return false;
		}

		OutMin = OutMax = Patch.Points[0].UV;
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			OutMin.X = FMath::Min(OutMin.X, Point.UV.X);
			OutMin.Y = FMath::Min(OutMin.Y, Point.UV.Y);
			OutMax.X = FMath::Max(OutMax.X, Point.UV.X);
			OutMax.Y = FMath::Max(OutMax.Y, Point.UV.Y);
		}
		return true;
	}

	/**
	 * Natural: U along each point's own row, V down its own column, in tiles,
	 * measured from the patch's UV origin - the corner at (0, 0) unless the
	 * grid has grown past it.
	 */
	void ApplyNatural(FSurfPatch& Patch)
	{
		const FVector2D Scale = SafeScale(Patch.Scale);
		const int32 OriginX = FMath::Clamp(Patch.UVOriginX, 0, Patch.Width - 1);
		const int32 OriginY = FMath::Clamp(Patch.UVOriginY, 0, Patch.Height - 1);

		TArray<double> U;
		TArray<double> V;
		U.SetNumZeroed(Patch.NumPoints());
		V.SetNumZeroed(Patch.NumPoints());

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			double Acc = 0.0;
			for (int32 X = 1; X < Patch.Width; ++X)
			{
				Acc += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X - 1, Y).Pos);
				U[Y * Patch.Width + X] = (Acc * 0.01) / Scale.X;
			}
		}

		for (int32 X = 0; X < Patch.Width; ++X)
		{
			double Acc = 0.0;
			for (int32 Y = 1; Y < Patch.Height; ++Y)
			{
				Acc += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X, Y - 1).Pos);
				V[Y * Patch.Width + X] = (Acc * 0.01) / Scale.Y;
			}
		}

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				const int32 I = Y * Patch.Width + X;
				Patch.At(X, Y).UV = FVector2D(
					U[I] - U[Y * Patch.Width + OriginX],
					V[I] - V[OriginY * Patch.Width + X]);
			}
		}
	}

	/**
	 * Even: U is the column's index over the cells a tile, V the row's -
	 * every cell the same size in the texture, whatever its size in the
	 * world, and nothing of the geometry in it, so no point moved can
	 * stretch the net. Measured from the patch's UV origin, as Natural is.
	 */
	void ApplyGrid(FSurfPatch& Patch)
	{
		const FVector2D Scale = SafeScale(Patch.Scale);
		const int32 OriginX = FMath::Clamp(Patch.UVOriginX, 0, Patch.Width - 1);
		const int32 OriginY = FMath::Clamp(Patch.UVOriginY, 0, Patch.Height - 1);

		// The rulers: each line's place in cells. Missing, or the wrong
		// length after an edit that did not carry them, they are laid out
		// evenly again (S70c).
		if (Patch.GridU.Num() != Patch.Width)
		{
			Patch.GridU.SetNum(Patch.Width);
			for (int32 X = 0; X < Patch.Width; ++X) { Patch.GridU[X] = static_cast<float>(X); }
		}
		if (Patch.GridV.Num() != Patch.Height)
		{
			Patch.GridV.SetNum(Patch.Height);
			for (int32 Y = 0; Y < Patch.Height; ++Y) { Patch.GridV[Y] = static_cast<float>(Y); }
		}

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				Patch.At(X, Y).UV = FVector2D(
					(Patch.GridU[X] - Patch.GridU[OriginX]) / Scale.X,
					(Patch.GridV[Y] - Patch.GridV[OriginY]) / Scale.Y);
			}
		}
	}

	void ApplyPlanar(FSurfPatch& Patch)
	{
		FSurfFaceUV Projection = Patch.Projection;
		Projection.Scale = SafeScale(Projection.Scale);

		if (Projection.UAxis.IsNearlyZero() || Projection.VAxis.IsNearlyZero())
		{
			const FVector Normal = AverageNormal(Patch);
			SurfProjection::Align(Projection, Normal.IsNearlyZero() ? FVector::UpVector : Normal);
		}

		for (FSurfControlPoint& Point : Patch.Points)
		{
			Point.UV = SurfProjection::ComputeUV(Projection, Point.Pos);
		}
	}

	/**
	 * The post-mapping step: swap, flips, rotation, shift, in that order.
	 * Rotation and the flips work about the mapping's own centre so the
	 * texture turns where it is instead of swinging off the patch.
	 */
	void ApplyPostTransform(FSurfPatch& Patch)
	{
		if (Patch.bUVSwap)
		{
			for (FSurfControlPoint& Point : Patch.Points)
			{
				Point.UV = FVector2D(Point.UV.Y, Point.UV.X);
			}
		}

		FVector2D Min, Max;
		if (!UVBounds(Patch, Min, Max))
		{
			return;
		}

		const FVector2D Centre = (Min + Max) * 0.5;

		if (Patch.bUVFlipU || Patch.bUVFlipV || !FMath::IsNearlyZero(Patch.UVRotation))
		{
			const double Radians = FMath::DegreesToRadians(Patch.UVRotation);
			const double S = FMath::Sin(Radians);
			const double C = FMath::Cos(Radians);

			for (FSurfControlPoint& Point : Patch.Points)
			{
				FVector2D Local = Point.UV - Centre;

				if (Patch.bUVFlipU) { Local.X = -Local.X; }
				if (Patch.bUVFlipV) { Local.Y = -Local.Y; }

				Point.UV = Centre + FVector2D(Local.X * C - Local.Y * S, Local.X * S + Local.Y * C);
			}
		}

		if (!Patch.UVShift.IsNearlyZero())
		{
			for (FSurfControlPoint& Point : Patch.Points)
			{
				Point.UV += Patch.UVShift;
			}
		}
	}

	/** Remember the mapping so a later edit can be measured against it. */
	FORCEINLINE void Commit(FSurfPatch& Patch)
	{
		for (FSurfControlPoint& Point : Patch.Points)
		{
			Point.SavedUV = Point.UV;
		}
		++Patch.EditSerial;
	}
}

FVector AverageNormal(const FSurfPatch& Patch)
{
	if (!Patch.HasValidDimensions() || Patch.Width < 2 || Patch.Height < 2)
	{
		return FVector::ZeroVector;
	}

	// Summed cross products over the net's cells: area-weighted without
	// needing the areas, and stable across a patch that doubles back.
	FVector Sum = FVector::ZeroVector;

	for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
	{
		for (int32 X = 0; X + 1 < Patch.Width; ++X)
		{
			const FVector& P00 = Patch.At(X, Y).Pos;
			const FVector DU = Patch.At(X + 1, Y).Pos - P00;
			const FVector DV = Patch.At(X, Y + 1).Pos - P00;
			Sum += FVector::CrossProduct(DU, DV);
		}
	}

	if (Patch.bInvert)
	{
		Sum = -Sum;
	}

	return Sum.GetSafeNormal();
}

void MeasureExtent(const FSurfPatch& Patch, double& OutMaxRowCm, double& OutMaxColumnCm)
{
	OutMaxRowCm = 0.0;
	OutMaxColumnCm = 0.0;

	if (!Patch.HasValidDimensions())
	{
		return;
	}

	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		double Length = 0.0;
		for (int32 X = 1; X < Patch.Width; ++X)
		{
			Length += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X - 1, Y).Pos);
		}
		OutMaxRowCm = FMath::Max(OutMaxRowCm, Length);
	}

	for (int32 X = 0; X < Patch.Width; ++X)
	{
		double Length = 0.0;
		for (int32 Y = 1; Y < Patch.Height; ++Y)
		{
			Length += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X, Y - 1).Pos);
		}
		OutMaxColumnCm = FMath::Max(OutMaxColumnCm, Length);
	}
}

int32& HoldUVsCount()
{
	static int32 Count = 0;
	return Count;
}

void Reapply(FSurfPatch& Patch)
{
	if (!Patch.HasValidDimensions() || Patch.Mapping == ESurfPatchMapping::Manual)
	{
		return;
	}
	if (HoldUVsCount() > 0)
	{
		++Patch.EditSerial;
		return;
	}

	if (Patch.Mapping == ESurfPatchMapping::Natural)
	{
		ApplyNatural(Patch);
	}
	else if (Patch.Mapping == ESurfPatchMapping::Grid)
	{
		ApplyGrid(Patch);
	}
	else
	{
		ApplyPlanar(Patch);
	}

	ApplyPostTransform(Patch);
	Commit(Patch);
}

void SetNatural(FSurfPatch& Patch)
{
	Patch.Mapping = ESurfPatchMapping::Natural;
	Reapply(Patch);
}

void SetEven(FSurfPatch& Patch)
{
	/**
	 * The rulers laid from the cells as they stand, once (S150c): "should
	 * be smart : get the decent size for the decent quad it has ! right now
	 * its stretching long quads !"
	 *
	 * A tile a cell (S69c) made every cell square in the texture whatever
	 * its shape in the world, so a cell four times as long as it was wide
	 * wore its tile stretched four to one. The rulers now take each line's
	 * place from the **measured** width of the cells before it - a column's
	 * width averaged down its rows, a row's height across its columns - in
	 * one unit for both ways, the mean cell, so a long cell is a long run of
	 * tiles and a square one is one. Uniform cells come out 0, 1, 2 exactly
	 * as before.
	 *
	 * Measured **here and never again**: the rulers are stored (S70c), so a
	 * point moved afterwards moves nothing in the net - which was D83's
	 * whole objection to measuring, and holds.
	 */
	Patch.Scale = FVector2D(1.0, 1.0);
	Patch.Projection.Scale = FVector2D(1.0, 1.0);
	Patch.GridU.Reset();
	Patch.GridV.Reset();

	if (Patch.Width >= 2 && Patch.Height >= 2 && Patch.Points.Num() == Patch.Width * Patch.Height)
	{
		TArray<double> Across;
		Across.SetNumZeroed(Patch.Width - 1);
		for (int32 X = 0; X + 1 < Patch.Width; ++X)
		{
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				Across[X] += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X + 1, Y).Pos);
			}
			Across[X] /= Patch.Height;
		}

		TArray<double> Down;
		Down.SetNumZeroed(Patch.Height - 1);
		for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
		{
			for (int32 X = 0; X < Patch.Width; ++X)
			{
				Down[Y] += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X, Y + 1).Pos);
			}
			Down[Y] /= Patch.Width;
		}

		double Sum = 0.0;
		for (const double Each : Across) { Sum += Each; }
		for (const double Each : Down) { Sum += Each; }
		const double Mean = Sum / (Across.Num() + Down.Num());

		// A patch with no size at all is laid out a tile a cell, as before:
		// there is nothing to measure and nothing to stretch.
		if (Mean > KINDA_SMALL_NUMBER)
		{
			Patch.GridU.SetNum(Patch.Width);
			Patch.GridU[0] = 0.0f;
			for (int32 X = 0; X + 1 < Patch.Width; ++X)
			{
				Patch.GridU[X + 1] = Patch.GridU[X] + static_cast<float>(Across[X] / Mean);
			}
			Patch.GridV.SetNum(Patch.Height);
			Patch.GridV[0] = 0.0f;
			for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
			{
				Patch.GridV[Y + 1] = Patch.GridV[Y] + static_cast<float>(Down[Y] / Mean);
			}
		}
	}

	Patch.Mapping = ESurfPatchMapping::Grid;
	Reapply(Patch);
}

void SetPlanar(FSurfPatch& Patch)
{
	const FVector Normal = AverageNormal(Patch);
	if (Normal.IsNearlyZero())
	{
		return;
	}

	Patch.Projection.Scale = SafeScale(Patch.Scale);
	Patch.Projection.Shift = FVector2D::ZeroVector;
	SurfProjection::Align(Patch.Projection, Normal);

	Patch.Mapping = ESurfPatchMapping::Planar;
	Reapply(Patch);
}

void SetPlanarWith(FSurfPatch& Patch, const FSurfFaceUV& Projection)
{
	Patch.Projection = Projection;
	Patch.Projection.Scale = SafeScale(Projection.Scale);
	Patch.Mapping = ESurfPatchMapping::Planar;
	Reapply(Patch);
}

void SetManual(FSurfPatch& Patch)
{
	Patch.Mapping = ESurfPatchMapping::Manual;
	++Patch.EditSerial;
}

void SetScale(FSurfPatch& Patch, const FVector2D& MetresPerTile)
{
	if (MetresPerTile.X <= 0.0 || MetresPerTile.Y <= 0.0)
	{
		return;
	}

	Patch.Scale = MetresPerTile;
	Patch.Projection.Scale = MetresPerTile;
	Reapply(Patch);
}

void FitScale(FSurfPatch& Patch, int32 RepeatU, int32 RepeatV)
{
	double RowCm = 0.0;
	double ColumnCm = 0.0;
	MeasureExtent(Patch, RowCm, ColumnCm);

	// An even patch is counted, not measured: the ruler's span over the
	// tiles wanted is the cells a tile.
	if (Patch.Mapping == ESurfPatchMapping::Grid)
	{
		Reapply(Patch);
		const double SpanU = Patch.GridU.Num() == Patch.Width ? Patch.GridU.Last() - Patch.GridU[0] : Patch.Width - 1;
		const double SpanV = Patch.GridV.Num() == Patch.Height ? Patch.GridV.Last() - Patch.GridV[0] : Patch.Height - 1;
		SetScale(Patch, FVector2D(
			FMath::Max(1.e-3, SpanU) / FMath::Max(1, RepeatU),
			FMath::Max(1.e-3, SpanV) / FMath::Max(1, RepeatV)));
		return;
	}

	if (RowCm <= 0.0 || ColumnCm <= 0.0)
	{
		return;
	}

	// Metres of surface divided by the tiles wanted across it.
	const FVector2D Scale(
		(RowCm * 0.01) / FMath::Max(1, RepeatU),
		(ColumnCm * 0.01) / FMath::Max(1, RepeatV));

	// Fit answers "how big is a tile", which is a Natural question; a planar
	// projection measured this way would only be right head-on.
	Patch.Mapping = ESurfPatchMapping::Natural;
	SetScale(Patch, Scale);
}

void FitToTrim(FSurfPatch& Patch, int32 CellsU, int32 CellsV, const FVector2D& CellsPerTile)
{
	// A cell is a tile divided by the cells across it: the surface over
	// CellsU cells is the surface over CellsU / CellsPerTile tiles, so the
	// metres a tile are the fitted metres a cell times the cells in a tile.
	FitScale(Patch, CellsU, CellsV);
	const FVector2D PerTile(FMath::Max(1.0, CellsPerTile.X), FMath::Max(1.0, CellsPerTile.Y));
	SetScale(Patch, FVector2D(Patch.Scale.X * PerTile.X, Patch.Scale.Y * PerTile.Y));

	// On a cell: the shift snapped to the nearest cell boundary, so the
	// mapping starts where a cell starts and not partway across one.
	Patch.UVShift = FVector2D(
		FMath::RoundToDouble(Patch.UVShift.X * PerTile.X) / PerTile.X,
		FMath::RoundToDouble(Patch.UVShift.Y * PerTile.Y) / PerTile.Y);
	Reapply(Patch);
}

void Shift(FSurfPatch& Patch, double TilesU, double TilesV)
{
	Patch.UVShift += FVector2D(TilesU, TilesV);
	Reapply(Patch);
}

void Rotate(FSurfPatch& Patch, double Degrees)
{
	Patch.UVRotation = FMath::Fmod(Patch.UVRotation + Degrees, 360.0);
	Reapply(Patch);
}

void FlipU(FSurfPatch& Patch)
{
	Patch.bUVFlipU = !Patch.bUVFlipU;
	Reapply(Patch);
}

void FlipV(FSurfPatch& Patch)
{
	Patch.bUVFlipV = !Patch.bUVFlipV;
	Reapply(Patch);
}

void SwapUV(FSurfPatch& Patch)
{
	Patch.bUVSwap = !Patch.bUVSwap;
	Reapply(Patch);
}

void CopySurfaceDefinition(const FSurfPatch& From, FSurfPatch& To)
{
	To.Material = From.Material;
	To.Scale = From.Scale;
	To.Mapping = From.Mapping;
	To.Projection = From.Projection;
	To.UVShift = From.UVShift;
	To.UVRotation = From.UVRotation;
	To.bUVSwap = From.bUVSwap;
	To.bUVFlipU = From.bUVFlipU;
	To.bUVFlipV = From.bUVFlipV;
	To.SmoothingGroup = From.SmoothingGroup;
	To.StackLevel = From.StackLevel;

	Reapply(To);
}

void CopyFaceSurface(const FSurfFace& From, FSurfPatch& To)
{
	// Taking the face's exact projection is the point: this is how a patch is
	// made to continue the wall it meets without a visible seam.
	To.Material = From.Material;
	To.Scale = From.UV.Scale;
	To.SmoothingGroup = From.SmoothingGroup;
	SetPlanarWith(To, From.UV);
}

FVector2D TexelDensity(const FSurfPatch& Patch, const FVector2D& TextureTexels)
{
	double RowCm = 0.0;
	double ColumnCm = 0.0;
	MeasureExtent(Patch, RowCm, ColumnCm);

	FVector2D Min, Max;
	if (!UVBounds(Patch, Min, Max) || RowCm <= 0.0 || ColumnCm <= 0.0)
	{
		return FVector2D::ZeroVector;
	}

	// Tiles across the patch times texels per tile, over metres of surface.
	const double TilesU = Max.X - Min.X;
	const double TilesV = Max.Y - Min.Y;

	return FVector2D(
		(TilesU * TextureTexels.X) / (RowCm * 0.01),
		(TilesV * TextureTexels.Y) / (ColumnCm * 0.01));
}

} // namespace SurfPatchSurfacing
