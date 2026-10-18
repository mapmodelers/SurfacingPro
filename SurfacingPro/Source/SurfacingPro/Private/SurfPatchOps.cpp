// Copyright MapModelers. All Rights Reserved.

#include "SurfPatchOps.h"

namespace SurfPatchOps
{
	double NetArea(const FSurfPatch& Patch)
	{
		double Area = 0.0;
		if (!Patch.HasValidDimensions())
		{
			return Area;
		}
		for (int32 Y = 0; Y + 1 < Patch.Height; ++Y)
		{
			for (int32 X = 0; X + 1 < Patch.Width; ++X)
			{
				const FVector& P00 = Patch.At(X, Y).Pos;
				const FVector& P10 = Patch.At(X + 1, Y).Pos;
				const FVector& P01 = Patch.At(X, Y + 1).Pos;
				const FVector& P11 = Patch.At(X + 1, Y + 1).Pos;
				Area += FVector::CrossProduct(P10 - P00, P01 - P00).Size() * 0.5;
				Area += FVector::CrossProduct(P01 - P11, P10 - P11).Size() * 0.5;
			}
		}
		return Area;
	}

	bool HasNoSurface(const FSurfPatch& Patch)
	{
		return Patch.HasValidDimensions() && Patch.Width >= 2 && Patch.Height >= 2 && NetArea(Patch) < 0.5;
	}

namespace
{
	/** Indices to act on: the given set, or everything when none was given. */
	TArray<int32> Resolve(const FSurfPatch& Patch, const TArray<int32>& Indices)
	{
		if (Indices.Num() > 0)
		{
			return Indices;
		}

		TArray<int32> All;
		All.Reserve(Patch.NumPoints());
		for (int32 I = 0; I < Patch.NumPoints(); ++I)
		{
			All.Add(I);
		}
		return All;
	}

	FORCEINLINE bool IsBorder(const FSurfPatch& Patch, int32 X, int32 Y)
	{
		return X == 0 || Y == 0 || X == Patch.Width - 1 || Y == Patch.Height - 1;
	}
}

FVector PointNormal(const FSurfPatch& Patch, int32 X, int32 Y)
{
	if (Patch.Width < 2 || Patch.Height < 2)
	{
		return FVector::UpVector;
	}

	// Central differences where there are neighbours on both sides, one-sided
	// at the border. Cheaper and steadier than averaging the adjacent quads.
	const int32 X0 = FMath::Max(0, X - 1);
	const int32 X1 = FMath::Min(Patch.Width - 1, X + 1);
	const int32 Y0 = FMath::Max(0, Y - 1);
	const int32 Y1 = FMath::Min(Patch.Height - 1, Y + 1);

	const FVector DU = Patch.At(X1, Y).Pos - Patch.At(X0, Y).Pos;
	const FVector DV = Patch.At(X, Y1).Pos - Patch.At(X, Y0).Pos;

	FVector Normal = FVector::CrossProduct(DU, DV).GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		Normal = FVector::UpVector;
	}

	return Patch.bInvert ? -Normal : Normal;
}

void Smooth(FSurfPatch& Patch, const TArray<int32>& Indices, double Strength, int32 Iterations, bool bPreserveBorder)
{
	if (!Patch.HasValidDimensions() || Strength <= 0.0 || Iterations <= 0)
	{
		return;
	}

	const TArray<int32> Targets = Resolve(Patch, Indices);
	if (Targets.Num() == 0)
	{
		return;
	}

	const double Amount = FMath::Clamp(Strength, 0.0, 1.0);

	for (int32 Pass = 0; Pass < Iterations; ++Pass)
	{
		// Read from a snapshot, write to the patch: smoothing in place would
		// make the result depend on the order points happen to be visited.
		TArray<FVector> Source;
		Source.Reserve(Patch.NumPoints());
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			Source.Add(Point.Pos);
		}

		for (const int32 Index : Targets)
		{
			if (!Patch.Points.IsValidIndex(Index))
			{
				continue;
			}

			const int32 X = Index % Patch.Width;
			const int32 Y = Index / Patch.Width;

			if (bPreserveBorder && IsBorder(Patch, X, Y))
			{
				continue;
			}

			FVector Sum = FVector::ZeroVector;
			int32 Count = 0;

			if (X > 0)                 { Sum += Source[Y * Patch.Width + (X - 1)]; ++Count; }
			if (X + 1 < Patch.Width)   { Sum += Source[Y * Patch.Width + (X + 1)]; ++Count; }
			if (Y > 0)                 { Sum += Source[(Y - 1) * Patch.Width + X]; ++Count; }
			if (Y + 1 < Patch.Height)  { Sum += Source[(Y + 1) * Patch.Width + X]; ++Count; }

			if (Count == 0)
			{
				continue;
			}

			Patch.Points[Index].Pos = FMath::Lerp(Source[Index], Sum / Count, Amount);
		}
	}

	++Patch.EditSerial;
}

void Random(FSurfPatch& Patch, const TArray<int32>& Indices, double AmountCm, const FVector& AxisMask, bool bAlongNormal, int32 Seed)
{
	if (!Patch.HasValidDimensions() || FMath::IsNearlyZero(AmountCm))
	{
		return;
	}
	const TArray<int32> Targets = Resolve(Patch, Indices);
	TArray<FVector> Normals;
	if (bAlongNormal)
	{
		Normals.Reserve(Targets.Num());
		for (const int32 Index : Targets)
		{
			Normals.Add(Patch.Points.IsValidIndex(Index) ? PointNormal(Patch, Index % Patch.Width, Index / Patch.Width) : FVector::UpVector);
		}
	}
	FRandomStream Stream(Seed);
	for (int32 I = 0; I < Targets.Num(); ++I)
	{
		const int32 Index = Targets[I];
		if (!Patch.Points.IsValidIndex(Index))
		{
			continue;
		}
		if (bAlongNormal)
		{
			Patch.Points[Index].Pos += Normals[I] * (Stream.FRandRange(-1.0, 1.0) * AmountCm);
		}
		else
		{
			const FVector Push(Stream.FRandRange(-1.0, 1.0) * AxisMask.X, Stream.FRandRange(-1.0, 1.0) * AxisMask.Y, Stream.FRandRange(-1.0, 1.0) * AxisMask.Z);
			Patch.Points[Index].Pos += Push * AmountCm;
		}
	}
}

void Inflate(FSurfPatch& Patch, const TArray<int32>& Indices, double DistanceCm)
{
	if (!Patch.HasValidDimensions() || FMath::IsNearlyZero(DistanceCm))
	{
		return;
	}

	const TArray<int32> Targets = Resolve(Patch, Indices);

	// Normals from the patch as it was, so points do not chase each other
	// outwards as the loop proceeds.
	TArray<FVector> Normals;
	Normals.Reserve(Targets.Num());
	for (const int32 Index : Targets)
	{
		if (Patch.Points.IsValidIndex(Index))
		{
			Normals.Add(PointNormal(Patch, Index % Patch.Width, Index / Patch.Width));
		}
		else
		{
			Normals.Add(FVector::UpVector);
		}
	}

	for (int32 I = 0; I < Targets.Num(); ++I)
	{
		if (Patch.Points.IsValidIndex(Targets[I]))
		{
			Patch.Points[Targets[I]].Pos += Normals[I] * DistanceCm;
		}
	}

	++Patch.EditSerial;
}

void Flatten(FSurfPatch& Patch, const TArray<int32>& Indices, double Strength)
{
	if (!Patch.HasValidDimensions() || Strength <= 0.0)
	{
		return;
	}

	const TArray<int32> Targets = Resolve(Patch, Indices);
	if (Targets.Num() < 3)
	{
		return;
	}

	FVector Centre = FVector::ZeroVector;
	int32 Count = 0;
	for (const int32 Index : Targets)
	{
		if (Patch.Points.IsValidIndex(Index))
		{
			Centre += Patch.Points[Index].Pos;
			++Count;
		}
	}

	if (Count == 0)
	{
		return;
	}

	Centre /= Count;

	// Plane normal by covariance: the direction of least spread. Averaging
	// grid normals would be pulled about by whichever part of the selection
	// happens to be most curved.
	double XX = 0.0, XY = 0.0, XZ = 0.0, YY = 0.0, YZ = 0.0, ZZ = 0.0;
	for (const int32 Index : Targets)
	{
		if (!Patch.Points.IsValidIndex(Index))
		{
			continue;
		}

		const FVector D = Patch.Points[Index].Pos - Centre;
		XX += D.X * D.X; XY += D.X * D.Y; XZ += D.X * D.Z;
		YY += D.Y * D.Y; YZ += D.Y * D.Z; ZZ += D.Z * D.Z;
	}

	const FVector AxisX(YY * ZZ - YZ * YZ, XZ * YZ - XY * ZZ, XY * YZ - XZ * YY);
	const FVector AxisY(XZ * YZ - XY * ZZ, XX * ZZ - XZ * XZ, XY * XZ - YZ * XX);
	const FVector AxisZ(XY * YZ - XZ * YY, XY * XZ - YZ * XX, XX * YY - XY * XY);

	FVector Normal = AxisX;
	if (AxisY.SizeSquared() > Normal.SizeSquared()) { Normal = AxisY; }
	if (AxisZ.SizeSquared() > Normal.SizeSquared()) { Normal = AxisZ; }

	Normal = Normal.GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		// Already flat, or collinear. Either way there is nothing to pull to.
		return;
	}

	const double Amount = FMath::Clamp(Strength, 0.0, 1.0);

	for (const int32 Index : Targets)
	{
		if (!Patch.Points.IsValidIndex(Index))
		{
			continue;
		}

		FVector& Position = Patch.Points[Index].Pos;
		const double Offset = FVector::DotProduct(Position - Centre, Normal);
		Position -= Normal * (Offset * Amount);
	}

	++Patch.EditSerial;
}

bool BevelLine(FSurfPatch& Patch, bool bColumn, int32 Index, double WidthCm, int32 Segments, double Roundness, bool bKeepCorner)
{
	if (!Patch.HasValidDimensions() || WidthCm <= 0.0 || Segments < 1)
	{
		return false;
	}

	const int32 Along = bColumn ? Patch.Width : Patch.Height;
	const int32 Across = bColumn ? Patch.Height : Patch.Width;

	if (Index <= 0 || Index >= Along - 1)
	{
		// A border line has nothing on one side, so there is no crease there
		// to round - only an edge to move, which is a different verb.
		return false;
	}

	// Replacing one line with 2 * Segments lines is a net gain; a curve also
	// has to stay odd, so it grows in pairs anyway. Keeping the corner adds
	// one more: the crease itself stays, with the new lines either side of
	// it on the surface - support lines, not a bevel.
	const int32 Count = bKeepCorner ? 2 * Segments + 1 : 2 * Segments;
	const int32 Added = Count - 1;
	const int32 Result = Along + Added;
	const int32 Limit = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;

	if (Result > Limit || (Patch.Type == ESurfPatchType::Curve && (Result % 2) == 0))
	{
		return false;
	}

	// Read the crease line and its neighbours, then build the replacement from
	// them. Working from a copy keeps this a pure function of the input.
	auto Get = [&Patch, bColumn](int32 A, int32 B) -> const FSurfControlPoint&
	{
		return bColumn ? Patch.At(A, B) : Patch.At(B, A);
	};

	TArray<FSurfControlPoint> Rebuilt;
	Rebuilt.Reserve(Result * Across);

	for (int32 B = 0; B < Across; ++B)
	{
		const FSurfControlPoint& Corner = Get(Index, B);
		const FSurfControlPoint& Before = Get(Index - 1, B);
		const FSurfControlPoint& After  = Get(Index + 1, B);

		// Directions away from the crease, each capped at the distance to its
		// neighbour so a wide bevel cannot reach past the next line.
		const FVector ToBefore = Before.Pos - Corner.Pos;
		const FVector ToAfter  = After.Pos - Corner.Pos;

		// Capped just short of the neighbour. Clamping to the full gap let a
		// wide bevel land its outermost point exactly on the next line, which
		// reads as the bevel having swallowed it; a little short of it, the
		// bevel can be as wide as the gap allows (the cap was 0.45 and the
		// widest bevel looked like a fillet on a beam).
		constexpr double MaxShareOfGap = 0.95;
		const double BackCm    = FMath::Min(WidthCm, ToBefore.Size() * MaxShareOfGap);
		const double ForwardCm = FMath::Min(WidthCm, ToAfter.Size() * MaxShareOfGap);

		const FVector BackDir    = ToBefore.GetSafeNormal();
		const FVector ForwardDir = ToAfter.GetSafeNormal();

		// Everything up to the crease, unchanged.
		for (int32 A = 0; A < Index; ++A)
		{
			Rebuilt.Add(Get(A, B));
		}

		// The arc. Points walk from BackCm back, through the corner's
		// neighbourhood, to ForwardCm forward; Roundness pulls each towards
		// the corner, so 0 is a straight chamfer and 1 a full round.
		for (int32 S = 0; S < Count; ++S)
		{
			const double T = static_cast<double>(S) / static_cast<double>(Count - 1);

			const FVector Straight = T < 0.5
				? Corner.Pos + BackDir * (BackCm * (1.0 - 2.0 * T))
				: Corner.Pos + ForwardDir * (ForwardCm * (2.0 * T - 1.0));

			// Distance from the middle of the run, 1 at the ends and 0 in the
			// centre: how much of the chamfer this point keeps.
			const double FromCentre = FMath::Abs(2.0 * T - 1.0);
			const FVector Rounded = FMath::Lerp(Corner.Pos, Straight, FromCentre);

			FSurfControlPoint Point = Corner;
			Point.Pos = bKeepCorner ? Straight : FMath::Lerp(Straight, Rounded, FMath::Clamp(Roundness, 0.0, 1.0));
			Rebuilt.Add(Point);
		}

		// And everything after it.
		for (int32 A = Index + 1; A < Along; ++A)
		{
			Rebuilt.Add(Get(A, B));
		}
	}

	// Rebuilt is laid out along-then-across; the patch is row major, so a
	// column bevel writes straight through and a row bevel transposes.
	FSurfPatch Out = Patch;
	Out.Width  = bColumn ? Result : Patch.Width;
	Out.Height = bColumn ? Patch.Height : Result;
	Out.Points.SetNum(Out.Width * Out.Height);

	// The UV origin stays on its point, past the lines the crease became.
	{
		int32& Origin = bColumn ? Out.UVOriginX : Out.UVOriginY;
		if (Origin > Index)
		{
			Origin += Added;
		}
	}

	for (int32 B = 0; B < Across; ++B)
	{
		for (int32 A = 0; A < Result; ++A)
		{
			const FSurfControlPoint& Source = Rebuilt[B * Result + A];
			if (bColumn)
			{
				Out.At(A, B) = Source;
			}
			else
			{
				Out.At(B, A) = Source;
			}
		}
	}

	++Out.EditSerial;
	Patch = MoveTemp(Out);
	return true;
}

bool Weld(FSurfPatch& Patch, const TArray<int32>& Indices, bool bAtCentre)
{
	if (!Patch.HasValidDimensions() || Indices.Num() < 2)
	{
		return false;
	}

	FVector Target = FVector::ZeroVector;
	int32 Counted = 0;

	for (const int32 Index : Indices)
	{
		if (Patch.Points.IsValidIndex(Index))
		{
			Target += Patch.Points[Index].Pos;
			++Counted;
		}
	}

	if (Counted < 2)
	{
		return false;
	}

	if (bAtCentre)
	{
		Target /= Counted;
	}
	else
	{
		// The first index given wins, so a caller that wants "weld to the one
		// I picked last" simply puts it first.
		const int32 First = Indices[0];
		if (!Patch.Points.IsValidIndex(First))
		{
			return false;
		}
		Target = Patch.Points[First].Pos;
	}

	for (const int32 Index : Indices)
	{
		if (Patch.Points.IsValidIndex(Index))
		{
			Patch.Points[Index].Pos = Target;
		}
	}

	++Patch.EditSerial;
	return true;
}

int32 WeldByDistance(FSurfPatch& Patch, const TArray<int32>& Indices, double ToleranceCm)
{
	if (!Patch.HasValidDimensions() || ToleranceCm <= 0.0)
	{
		return 0;
	}

	TArray<int32> Considered = Indices;
	if (Considered.Num() == 0)
	{
		Considered.Reserve(Patch.NumPoints());
		for (int32 I = 0; I < Patch.NumPoints(); ++I)
		{
			Considered.Add(I);
		}
	}

	const double ToleranceSquared = ToleranceCm * ToleranceCm;

	// Each point joins the first cluster it is close enough to, and clusters
	// keep the position of whichever point opened them rather than drifting
	// towards their own average - a moving target would let a line of points
	// each within tolerance of the next collapse the whole line.
	TArray<int32> Leaders;
	TArray<int32> LeaderOf;
	LeaderOf.Init(INDEX_NONE, Considered.Num());

	for (int32 I = 0; I < Considered.Num(); ++I)
	{
		const int32 Index = Considered[I];
		if (!Patch.Points.IsValidIndex(Index))
		{
			continue;
		}

		const FVector& Position = Patch.Points[Index].Pos;

		for (const int32 Leader : Leaders)
		{
			if (FVector::DistSquared(Position, Patch.Points[Leader].Pos) <= ToleranceSquared)
			{
				LeaderOf[I] = Leader;
				break;
			}
		}

		if (LeaderOf[I] == INDEX_NONE)
		{
			Leaders.Add(Index);
			LeaderOf[I] = Index;
		}
	}

	int32 Moved = 0;

	for (int32 I = 0; I < Considered.Num(); ++I)
	{
		const int32 Index = Considered[I];
		const int32 Leader = LeaderOf[I];

		if (Leader == INDEX_NONE || Leader == Index || !Patch.Points.IsValidIndex(Index))
		{
			continue;
		}

		if (!Patch.Points[Index].Pos.Equals(Patch.Points[Leader].Pos, 1.e-4))
		{
			Patch.Points[Index].Pos = Patch.Points[Leader].Pos;
			++Moved;
		}
	}

	if (Moved > 0)
	{
		++Patch.EditSerial;
	}

	return Moved;
}

} // namespace SurfPatchOps
