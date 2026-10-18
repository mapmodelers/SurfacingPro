// Copyright MapModelers. All Rights Reserved.
//
// A set of cells on a patch's grid, and the two things every verb that takes
// faces needs from it: rectangles - a grid patch is a rectangle, so a ragged
// set becomes several - and outlines, one closed chain of points round each
// region of it, so a wall can be built round the whole of an L rather than
// round each rectangle of it.

#pragma once

#include "CoreMinimal.h"

/** A rectangle of cells, inclusive. */
struct FSurfCellRect
{
	int32 X0 = 0;
	int32 Y0 = 0;
	int32 X1 = 0;
	int32 Y1 = 0;

	int32 Columns() const { return X1 - X0 + 1; }
	int32 Rows() const { return Y1 - Y0 + 1; }
};

/** Which cells of a Columns x Rows grid are in the set. */
struct FSurfCellSet
{
	int32 Columns = 0;
	int32 Rows = 0;
	TBitArray<> Cells;

	FSurfCellSet() = default;
	FSurfCellSet(int32 InColumns, int32 InRows)
		: Columns(FMath::Max(0, InColumns))
		, Rows(FMath::Max(0, InRows))
		, Cells(false, FMath::Max(0, InColumns) * FMath::Max(0, InRows))
	{
	}

	bool Has(int32 X, int32 Y) const
	{
		return X >= 0 && X < Columns && Y >= 0 && Y < Rows && Cells[Y * Columns + X];
	}

	void Set(int32 X, int32 Y, bool bOn = true)
	{
		if (X >= 0 && X < Columns && Y >= 0 && Y < Rows)
		{
			Cells[Y * Columns + X] = bOn;
		}
	}

	int32 Num() const
	{
		int32 N = 0;
		for (int32 I = 0; I < Cells.Num(); ++I)
		{
			N += Cells[I] ? 1 : 0;
		}
		return N;
	}

	/** The cells of the grid that are not in the set. */
	FSurfCellSet Inverse() const
	{
		FSurfCellSet Out = *this;
		for (int32 I = 0; I < Cells.Num(); ++I)
		{
			Out.Cells[I] = !Cells[I];
		}
		return Out;
	}
};

namespace SurfCells
{
	/**
	 * Rectangles covering the set, none overlapping: row by row, each one as
	 * wide as the run of free cells it starts on and as tall as the rows
	 * below that repeat the run whole. A rectangle is a patch; an L is two.
	 * The same set always gives the same rectangles in the same order, which
	 * the extrude relies on to pair the pieces of a cut with their lids.
	 */
	inline TArray<FSurfCellRect> Decompose(const FSurfCellSet& Set)
	{
		TArray<FSurfCellRect> Out;
		TBitArray<> Taken(false, Set.Cells.Num());
		auto Free = [&](int32 X, int32 Y)
		{
			return Set.Has(X, Y) && !Taken[Y * Set.Columns + X];
		};

		for (int32 Y = 0; Y < Set.Rows; ++Y)
		{
			for (int32 X = 0; X < Set.Columns; ++X)
			{
				if (!Free(X, Y))
				{
					continue;
				}
				int32 Wide = 1;
				while (Free(X + Wide, Y))
				{
					++Wide;
				}
				int32 Tall = 1;
				for (;;)
				{
					bool bWhole = Y + Tall < Set.Rows;
					for (int32 C = 0; C < Wide && bWhole; ++C)
					{
						bWhole = Free(X + C, Y + Tall);
					}
					if (!bWhole)
					{
						break;
					}
					++Tall;
				}
				for (int32 R = 0; R < Tall; ++R)
				{
					for (int32 C = 0; C < Wide; ++C)
					{
						Taken[(Y + R) * Set.Columns + X + C] = true;
					}
				}
				FSurfCellRect Rect;
				Rect.X0 = X;
				Rect.Y0 = Y;
				Rect.X1 = X + Wide - 1;
				Rect.Y1 = Y + Tall - 1;
				Out.Add(Rect);
			}
		}
		return Out;
	}

	/**
	 * The closed chains of grid points round the set's regions, the cells on
	 * the left of every step: a region's outline runs one way round and a
	 * hole's the other, so a wall built along either faces out of the cells.
	 * Two cells meeting at a corner alone are two regions. Each chain starts
	 * at its lowest point, lowest X first, and its last point joins back to
	 * the first (the first is not repeated). For one rectangle this is its
	 * rim: along the bottom, up the right, back along the top, down the left.
	 */
	inline TArray<TArray<FIntPoint>> Outlines(const FSurfCellSet& Set)
	{
		TArray<TArray<FIntPoint>> Out;
		if (Set.Columns <= 0 || Set.Rows <= 0)
		{
			return Out;
		}

		// Points are (Columns + 1) x (Rows + 1). Directions counter-clockwise:
		// +X, +Y, -X, -Y. The edge from point P in direction K exists when
		// the cell on its left is in the set and the one on its right is not.
		const int32 PW = Set.Columns + 1;
		const int32 PH = Set.Rows + 1;
		static const FIntPoint Step[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };
		auto Index = [PW](const FIntPoint& P, int32 K)
		{
			return (P.Y * PW + P.X) * 4 + K;
		};

		TBitArray<> Present(false, PW * PH * 4);
		for (int32 Y = 0; Y < Set.Rows; ++Y)
		{
			for (int32 X = 0; X < Set.Columns; ++X)
			{
				if (!Set.Has(X, Y))
				{
					continue;
				}
				if (!Set.Has(X, Y - 1)) { Present[Index(FIntPoint(X, Y), 0)] = true; }         // along the bottom, +X
				if (!Set.Has(X + 1, Y)) { Present[Index(FIntPoint(X + 1, Y), 1)] = true; }     // up the right, +Y
				if (!Set.Has(X, Y + 1)) { Present[Index(FIntPoint(X + 1, Y + 1), 2)] = true; } // back along the top, -X
				if (!Set.Has(X - 1, Y)) { Present[Index(FIntPoint(X, Y + 1), 3)] = true; }     // down the left, -Y
			}
		}

		TBitArray<> Used(false, Present.Num());
		auto Free = [&](const FIntPoint& P, int32 K)
		{
			return P.X >= 0 && P.X < PW && P.Y >= 0 && P.Y < PH && Present[Index(P, K)] && !Used[Index(P, K)];
		};

		for (int32 Y = 0; Y < PH; ++Y)
		{
			for (int32 X = 0; X < PW; ++X)
			{
				for (int32 K0 = 0; K0 < 4; ++K0)
				{
					const FIntPoint Start(X, Y);
					if (!Free(Start, K0))
					{
						continue;
					}

					TArray<FIntPoint> Loop;
					FIntPoint P = Start;
					int32 K = K0;
					for (int32 Guard = 0; Guard < Present.Num(); ++Guard)
					{
						Used[Index(P, K)] = true;
						Loop.Add(P);
						P = P + Step[K];
						if (P == Start)
						{
							break;
						}
						// A left turn first, then straight on, then right: where
						// two regions touch at a corner, the chain keeps to the
						// region it is on instead of crossing to the other.
						const int32 Turns[3] = { (K + 1) % 4, K, (K + 3) % 4 };
						int32 Next = -1;
						for (int32 T : Turns)
						{
							if (Free(P, T))
							{
								Next = T;
								break;
							}
						}
						if (Next < 0)
						{
							break;
						}
						K = Next;
					}

					if (Loop.Num() >= 4)
					{
						Out.Add(MoveTemp(Loop));
					}
				}
			}
		}
		return Out;
	}
}
