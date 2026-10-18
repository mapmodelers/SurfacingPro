// Copyright MapModelers. All Rights Reserved.

#include "SurfLattice.h"

namespace
{
	/** The Bernstein weights of Count points at T: a cage line's share of the place. */
	void LatticeWeights(int32 Count, double T, double* Out)
	{
		const int32 N = Count - 1;
		for (int32 I = 0; I <= N; ++I)
		{
			double Choose = 1.0;
			for (int32 K = 1; K <= I; ++K)
			{
				Choose = Choose * static_cast<double>(N - K + 1) / static_cast<double>(K);
			}
			Out[I] = Choose * FMath::Pow(T, static_cast<double>(I)) * FMath::Pow(1.0 - T, static_cast<double>(N - I));
		}
	}
}

namespace SurfLattice
{
	bool Bind(FSurfPatch& Patch, int32 ResX, int32 ResY, int32 ResZ)
	{
		if (Patch.Points.Num() == 0)
		{
			return false;
		}
		ResX = FMath::Clamp(ResX, MinRes, MaxRes);
		ResY = FMath::Clamp(ResY, MinRes, MaxRes);
		ResZ = FMath::Clamp(ResZ, MinRes, MaxRes);

		FBox Box(ForceInit);
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			Box += Point.Pos;
		}
		FVector Min = Box.Min;
		FVector Size = Box.GetSize();

		// A flat patch still gets a box with depth: a tenth of its size, ten
		// centimetres at least, half each side of it.
		const double Least = FMath::Max(10.0, FMath::Max3(Size.X, Size.Y, Size.Z) * 0.1);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Size[Axis] < Least)
			{
				Min[Axis] -= (Least - Size[Axis]) * 0.5;
				Size[Axis] = Least;
			}
		}

		FSurfLattice& Lattice = Patch.Lattice;
		Lattice.ResX = ResX;
		Lattice.ResY = ResY;
		Lattice.ResZ = ResZ;
		Lattice.Cage.SetNum(ResX * ResY * ResZ);
		for (int32 Z = 0; Z < ResZ; ++Z)
		{
			for (int32 Y = 0; Y < ResY; ++Y)
			{
				for (int32 X = 0; X < ResX; ++X)
				{
					const FVector Place(
						static_cast<double>(X) / (ResX - 1),
						static_cast<double>(Y) / (ResY - 1),
						static_cast<double>(Z) / (ResZ - 1));
					Lattice.Cage[Lattice.Index(X, Y, Z)] = Min + Size * Place;
				}
			}
		}

		Lattice.Params.SetNum(Patch.Points.Num());
		for (int32 I = 0; I < Patch.Points.Num(); ++I)
		{
			Lattice.Params[I] = (Patch.Points[I].Pos - Min) / Size;
		}
		NoteRest(Patch);
		return true;
	}

	bool IsBound(const FSurfPatch& Patch)
	{
		return Patch.Lattice.HasCage() && Patch.Lattice.Params.Num() == Patch.Points.Num();
	}

	FVector Evaluate(const FSurfLattice& Lattice, const TArray<FVector>& CagePoints, const FVector& Place)
	{
		if (!Lattice.HasCage() || CagePoints.Num() != Lattice.Cage.Num())
		{
			return FVector::ZeroVector;
		}
		double WX[MaxRes];
		double WY[MaxRes];
		double WZ[MaxRes];
		LatticeWeights(Lattice.ResX, Place.X, WX);
		LatticeWeights(Lattice.ResY, Place.Y, WY);
		LatticeWeights(Lattice.ResZ, Place.Z, WZ);
		FVector Sum = FVector::ZeroVector;
		for (int32 Z = 0; Z < Lattice.ResZ; ++Z)
		{
			for (int32 Y = 0; Y < Lattice.ResY; ++Y)
			{
				const double WYZ = WY[Y] * WZ[Z];
				for (int32 X = 0; X < Lattice.ResX; ++X)
				{
					Sum += CagePoints[Lattice.Index(X, Y, Z)] * (WX[X] * WYZ);
				}
			}
		}
		return Sum;
	}

	void Deform(FSurfPatch& Patch, const TArray<FVector>& CageFrom, const TArray<FVector>& PointsFrom)
	{
		const FSurfLattice& Lattice = Patch.Lattice;
		if (!IsBound(Patch) || CageFrom.Num() != Lattice.Cage.Num() || PointsFrom.Num() != Patch.Points.Num())
		{
			return;
		}
		TArray<FVector> Moved;
		Moved.SetNum(Lattice.Cage.Num());
		for (int32 I = 0; I < Moved.Num(); ++I)
		{
			Moved[I] = Lattice.Cage[I] - CageFrom[I];
		}
		for (int32 I = 0; I < Patch.Points.Num(); ++I)
		{
			Patch.Points[I].Pos = PointsFrom[I] + Evaluate(Lattice, Moved, Lattice.Params[I]);
		}
	}

	FVector FollowOffset(const FSurfPatch& Patch)
	{
		const TArray<FVector>& Rest = Patch.Lattice.Rest;
		if (!Patch.Lattice.HasCage() || Rest.Num() != Patch.Points.Num() || Rest.Num() == 0)
		{
			return FVector::ZeroVector;
		}
		FVector Mean = FVector::ZeroVector;
		for (int32 I = 0; I < Rest.Num(); ++I)
		{
			Mean += Patch.Points[I].Pos - Rest[I];
		}
		Mean /= Rest.Num();
		if (Mean.IsNearlyZero(0.01))
		{
			return FVector::ZeroVector;
		}
		for (int32 I = 0; I < Rest.Num(); ++I)
		{
			if (!(Patch.Points[I].Pos - Rest[I]).Equals(Mean, 0.5))
			{
				return FVector::ZeroVector;
			}
		}
		return Mean;
	}

	void Follow(FSurfPatch& Patch)
	{
		const FVector Offset = FollowOffset(Patch);
		if (!Offset.IsZero())
		{
			for (FVector& Point : Patch.Lattice.Cage)
			{
				Point += Offset;
			}
		}
		NoteRest(Patch);
	}

	void NoteRest(FSurfPatch& Patch)
	{
		Patch.Lattice.Rest.SetNum(Patch.Points.Num());
		for (int32 I = 0; I < Patch.Points.Num(); ++I)
		{
			Patch.Lattice.Rest[I] = Patch.Points[I].Pos;
		}
	}
}
