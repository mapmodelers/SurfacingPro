// Copyright MapModelers. All Rights Reserved.
//
// Extrude, for any selection.
//
// A grid patch can grow by one whole line and in no other way: it cannot
// carry half a row, a wall, or a lid. So the whole of one border line grows
// the patch, and everything else becomes new patches - as few as the
// selection allows. Every chain of selected neighbours along the grid, corners
// included, becomes one two-line sheet; a closed chain becomes one sheet that
// meets itself; a block of faces becomes a lid and one wall around it, and a
// ragged set of faces - an L - a lid per rectangle of it under one wall round
// the whole. All the new lines are selected afterwards and the move that
// follows pulls them out together.

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfSession.h"
#include "SurfGroup.h"
#include "SurfPatchSurfacing.h"
#include "SurfPatchPrimitives.h"
#include "SurfCellSet.h"
#include "SurfBrushMath.h"
#include "SurfSettings.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "SurfacingProEdModeExtrude"

namespace
{
	/** A chain of selected points, each a grid neighbour of the next. */
	struct FSurfChain
	{
		TArray<FIntPoint> Points;

		/** The last point joins back to the first. */
		bool bClosed = false;

		/** Every point shares an X: the chain is part of one column. */
		bool IsColumn() const
		{
			for (const FIntPoint& P : Points)
			{
				if (P.X != Points[0].X)
				{
					return false;
				}
			}
			return true;
		}

		/** Points in the sheet made from it: one more when it closes on itself. */
		int32 Length() const { return Points.Num() + (bClosed ? 1 : 0); }
	};

	/** A rectangle of faces, by the points at its corners, inclusive. */
	struct FSurfBlock
	{
		int32 X0 = 0;
		int32 Y0 = 0;
		int32 X1 = 0;
		int32 Y1 = 0;

		int32 Columns() const { return X1 - X0 + 1; }
		int32 Rows() const { return Y1 - Y0 + 1; }
	};

	/** One patch's share of the selection, and what it asks for. */
	struct FSurfExtrudePlan
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		TArray<FIntPoint> Points;

		/** Rectangles of whole cells in the selection: each gets a lid. */
		TArray<FSurfBlock> Blocks;

		/** The outline of each region the blocks make, cells on the left: each gets one wall. */
		TArray<FSurfChain> Outlines;

		TArray<FSurfChain> Chains;

		/** Selected points with no selected neighbour: a point on its own has nothing to pull. */
		int32 Lone = 0;
	};

	/** The whole of one border line, and which border. */
	bool IsWholeBorder(const FSurfPatch& Patch, const FSurfChain& Chain, bool& bOutColumn, bool& bOutFromEnd)
	{
		if (Chain.bClosed || Chain.Points.Num() < 2)
		{
			return false;
		}

		const bool bColumn = Chain.IsColumn();
		bool bRow = true;
		for (const FIntPoint& P : Chain.Points)
		{
			bRow &= (P.Y == Chain.Points[0].Y);
		}

		if (!bColumn && !bRow)
		{
			return false;
		}

		const int32 Line = bColumn ? Chain.Points[0].X : Chain.Points[0].Y;
		const int32 Lines = bColumn ? Patch.Width : Patch.Height;
		const int32 Along = bColumn ? Patch.Height : Patch.Width;

		if (Chain.Points.Num() != Along || (Line != 0 && Line != Lines - 1))
		{
			return false;
		}

		bOutColumn = bColumn;
		bOutFromEnd = Line != 0;
		return true;
	}

	/** The set's rectangles as blocks (point coordinates) and its outlines as closed chains. */
	void BlocksAndOutlines(const FSurfCellSet& Cells, FSurfExtrudePlan& Out)
	{
		for (const FSurfCellRect& Rect : SurfCells::Decompose(Cells))
		{
			FSurfBlock Block;
			Block.X0 = Rect.X0;
			Block.Y0 = Rect.Y0;
			Block.X1 = Rect.X1 + 1;
			Block.Y1 = Rect.Y1 + 1;
			Out.Blocks.Add(Block);
		}
		for (TArray<FIntPoint>& Loop : SurfCells::Outlines(Cells))
		{
			FSurfChain& Chain = Out.Outlines.AddDefaulted_GetRef();
			Chain.Points = MoveTemp(Loop);
			Chain.bClosed = true;
		}
	}

	/**
	 * Sort a patch's selected points into blocks and outlines, chains and
	 * strays. Whole cells in the selection are faces: rectangles of them are
	 * blocks, and one outline goes round each region of them. Otherwise
	 * every stretch of selected neighbours along rows and columns is
	 * followed through its corners: a point with one, or three or more,
	 * selected neighbours ends a chain, a point with two carries it on, and
	 * what is left after that is closed loops.
	 */
	void Plan(const FSurfPatch& Patch, FSurfExtrudePlan& Out)
	{
		const int32 W = Patch.Width;
		const int32 H = Patch.Height;

		TBitArray<> Selected(false, W * H);
		FIntPoint Min(MAX_int32, MAX_int32);
		FIntPoint Max(-1, -1);

		for (const FIntPoint& P : Out.Points)
		{
			if (P.X < 0 || P.X >= W || P.Y < 0 || P.Y >= H)
			{
				continue;
			}

			Selected[P.Y * W + P.X] = true;
			Min.X = FMath::Min(Min.X, P.X);
			Min.Y = FMath::Min(Min.Y, P.Y);
			Max.X = FMath::Max(Max.X, P.X);
			Max.Y = FMath::Max(Max.Y, P.Y);
		}

		if (Max.X < 0)
		{
			return;
		}

		auto Is = [&](int32 X, int32 Y)
		{
			return X >= 0 && X < W && Y >= 0 && Y < H && Selected[Y * W + X];
		};

		// Whole cells in the selection - all four corners picked - are faces,
		// and faces get a lid and a wall, not a two-point wall per edge: a
		// whole line picked with a face beside it used to come out as four
		// slivers. Rectangles of them, and one outline round each region of
		// them - an L is two lids under one wall, not two boxes sharing a
		// wall; what is left is lines.
		FSurfCellSet Cells(W - 1, H - 1);
		for (int32 Y = 0; Y + 1 < H; ++Y)
		{
			for (int32 X = 0; X + 1 < W; ++X)
			{
				Cells.Set(X, Y, Is(X, Y) && Is(X + 1, Y) && Is(X, Y + 1) && Is(X + 1, Y + 1));
			}
		}
		BlocksAndOutlines(Cells, Out);

		const FIntPoint Steps[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };

		auto Degree = [&](const FIntPoint& P)
		{
			int32 D = 0;
			for (const FIntPoint& S : Steps)
			{
				D += Is(P.X + S.X, P.Y + S.Y) ? 1 : 0;
			}
			return D;
		};

		// A segment is used once. Keyed on its low end and its direction.
		TBitArray<> Used(false, W * H * 2);
		auto SegmentIndex = [&](const FIntPoint& A, const FIntPoint& B)
		{
			const FIntPoint Low(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
			const int32 Axis = A.X != B.X ? 0 : 1;
			return (Low.Y * W + Low.X) * 2 + Axis;
		};

		// Every segment inside or around a block is the block's, not a line's.
		for (const FSurfBlock& Block : Out.Blocks)
		{
			for (int32 Y = Block.Y0; Y <= Block.Y1; ++Y)
			{
				for (int32 X = Block.X0; X <= Block.X1; ++X)
				{
					if (X < Block.X1) { Used[SegmentIndex(FIntPoint(X, Y), FIntPoint(X + 1, Y))] = true; }
					if (Y < Block.Y1) { Used[SegmentIndex(FIntPoint(X, Y), FIntPoint(X, Y + 1))] = true; }
				}
			}
		}

		auto Walk = [&](FIntPoint From, FIntPoint To, FSurfChain& Chain)
		{
			// Follow the chain from From through To until a point that is
			// not a plain link (degree other than two), or until it closes.
			Chain.Points.Add(From);
			for (;;)
			{
				Used[SegmentIndex(From, To)] = true;

				if (To == Chain.Points[0])
				{
					Chain.bClosed = true;
					return;
				}

				Chain.Points.Add(To);

				if (Degree(To) != 2)
				{
					return;
				}

				FIntPoint Next = To;
				bool bFound = false;
				for (const FIntPoint& S : Steps)
				{
					const FIntPoint Candidate(To.X + S.X, To.Y + S.Y);
					if (Is(Candidate.X, Candidate.Y) && !Used[SegmentIndex(To, Candidate)])
					{
						Next = Candidate;
						bFound = true;
						break;
					}
				}

				if (!bFound)
				{
					return;
				}

				From = To;
				To = Next;
			}
		};

		// Open chains first, from every end and every junction.
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const FIntPoint P(X, Y);
				if (!Is(X, Y) || Degree(P) == 2)
				{
					continue;
				}

				for (const FIntPoint& S : Steps)
				{
					const FIntPoint Q(X + S.X, Y + S.Y);
					if (Is(Q.X, Q.Y) && !Used[SegmentIndex(P, Q)])
					{
						FSurfChain& Chain = Out.Chains.AddDefaulted_GetRef();
						Walk(P, Q, Chain);
					}
				}
			}
		}

		// Whatever is left is loops: every point on them has two neighbours.
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const FIntPoint P(X, Y);
				if (!Is(X, Y))
				{
					continue;
				}

				for (const FIntPoint& S : Steps)
				{
					const FIntPoint Q(X + S.X, Y + S.Y);
					if (Is(Q.X, Q.Y) && !Used[SegmentIndex(P, Q)])
					{
						FSurfChain& Chain = Out.Chains.AddDefaulted_GetRef();
						Walk(P, Q, Chain);
					}
				}
			}
		}

		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (Is(X, Y) && Degree(FIntPoint(X, Y)) == 0)
				{
					++Out.Lone;
				}
			}
		}
	}

	/** The surface normal at one control point, from its neighbours on the net. Patch space. */
	FVector PointNormal(const FSurfPatch& Patch, int32 X, int32 Y)
	{
		const FVector& P = Patch.At(X, Y).Pos;
		const FVector DU = X + 1 < Patch.Width ? Patch.At(X + 1, Y).Pos - P : P - Patch.At(X - 1, Y).Pos;
		const FVector DV = Y + 1 < Patch.Height ? Patch.At(X, Y + 1).Pos - P : P - Patch.At(X, Y - 1).Pos;

		FVector Normal = FVector::CrossProduct(DU, DV);
		if (Patch.bInvert)
		{
			Normal = -Normal;
		}

		return Normal.GetSafeNormal();
	}

	/** Mean normal over a set of points, or the patch's if they cancel out. */
	FVector NormalOver(const FSurfPatch& Patch, const TArray<FIntPoint>& Points)
	{
		FVector Sum = FVector::ZeroVector;
		for (const FIntPoint& P : Points)
		{
			Sum += PointNormal(Patch, P.X, P.Y);
		}

		const FVector Normal = Sum.GetSafeNormal();
		return Normal.IsNearlyZero() ? SurfPatchSurfacing::AverageNormal(Patch) : Normal;
	}

	/** Mean distance between neighbours among a set of points: how far one cell is, here. */
	double SpacingOver(const FSurfPatch& Patch, const TArray<FIntPoint>& Points)
	{
		double Sum = 0.0;
		int32 Count = 0;

		for (const FIntPoint& P : Points)
		{
			const FVector& Here = Patch.At(P.X, P.Y).Pos;
			if (P.X + 1 < Patch.Width && Points.Contains(FIntPoint(P.X + 1, P.Y)))
			{
				Sum += FVector::Dist(Here, Patch.At(P.X + 1, P.Y).Pos);
				++Count;
			}
			if (P.Y + 1 < Patch.Height && Points.Contains(FIntPoint(P.X, P.Y + 1)))
			{
				Sum += FVector::Dist(Here, Patch.At(P.X, P.Y + 1).Pos);
				++Count;
			}
		}

		if (Count == 0)
		{
			for (int32 Y = 0; Y < Patch.Height; ++Y)
			{
				for (int32 X = 1; X < Patch.Width; ++X)
				{
					Sum += FVector::Dist(Patch.At(X, Y).Pos, Patch.At(X - 1, Y).Pos);
					++Count;
				}
			}
		}

		return Count > 0 ? FMath::Max(1.0, Sum / Count) : 100.0;
	}

	/** The distance asked for, or the spacing when none was; never nothing, which would make walls of no height. */
	double DistanceOr(double OffsetCm, double SpacingCm)
	{
		if (OffsetCm <= USurfEdMode::ExtrudeBySpacing * 0.5)
		{
			return SpacingCm;
		}
		return FMath::Abs(OffsetCm) < 0.1 ? (OffsetCm < 0.0 ? -0.1 : 0.1) : OffsetCm;
	}

	/** A sheet that inherits everything of Source but its shape and identity. */
	FSurfPatch DeriveSheet(const FSurfPatch& Source, int32 Width, int32 Height)
	{
		FSurfPatch Made = Source;
		Made.Id = FGuid::NewGuid();
		Made.Type = ESurfPatchType::Sheet;
		Made.Width = Width;
		Made.Height = Height;
		Made.Points.Init(FSurfControlPoint(), Width * Height);
		Made.UVOriginX = 0;
		Made.UVOriginY = 0;
		Made.EditSerial = 0;
		return Made;
	}

	/** Whether a sheet's cells, on balance, face away from a point. */
	bool FacesAwayFrom(const FSurfPatch& Made, const FVector& Reference)
	{
		double Sum = 0.0;
		for (int32 Y = 0; Y + 1 < Made.Height; ++Y)
		{
			for (int32 X = 0; X + 1 < Made.Width; ++X)
			{
				const FVector& P00 = Made.At(X, Y).Pos;
				const FVector& P10 = Made.At(X + 1, Y).Pos;
				const FVector& P01 = Made.At(X, Y + 1).Pos;
				const FVector& P11 = Made.At(X + 1, Y + 1).Pos;

				FVector Normal = FVector::CrossProduct(P10 - P00, P01 - P00);
				if (Made.bInvert)
				{
					Normal = -Normal;
				}

				const FVector Mid = (P00 + P10 + P01 + P11) * 0.25;
				Sum += FVector::DotProduct(Normal, Mid - Reference);
			}
		}
		return Sum >= 0.0;
	}

	/**
	 * A two-line sheet along a chain: one line on the chain's points, the
	 * other pushed by Offset. A chain that is part of one column keeps the
	 * column's direction (two wide, as tall as the chain), so a wall pulled
	 * from a column folds the surfacing over the edge; any other chain runs
	 * along its width. A closed chain repeats its first point at the end, so
	 * the sheet meets itself. Turned to face away from Reference when asked
	 * (an outline is walked with its cells on the left, so a wall along it
	 * already faces out of them when pushed out of the surface, and into
	 * them when pushed in), then collapsed onto the chain when the move that
	 * follows supplies the distance.
	 */
	FSurfPatch MakeWall(const FSurfPatch& Source, const FSurfChain& Chain, const FVector& Offset,
		const FVector& Reference, bool bInPlace, bool& bOutColumn, bool bOrientByReference = true)
	{
		TArray<FIntPoint> Along = Chain.Points;
		if (Chain.bClosed)
		{
			Along.Add(Chain.Points[0]);
		}

		const int32 N = Along.Num();
		bOutColumn = !Chain.bClosed && Chain.IsColumn();

		FSurfPatch Made = DeriveSheet(Source, bOutColumn ? 2 : N, bOutColumn ? N : 2);

		for (int32 I = 0; I < N; ++I)
		{
			const FSurfControlPoint& Near = Source.At(Along[I].X, Along[I].Y);

			FSurfControlPoint Far = Near;
			Far.Pos = Near.Pos + Offset;

			if (bOutColumn)
			{
				Made.At(0, I) = Near;
				Made.At(1, I) = Far;
			}
			else
			{
				Made.At(I, 0) = Near;
				Made.At(I, 1) = Far;
			}
		}

		// The new patch measures its own UVs from its own corner, which put it
		// at zero while the patch it came from was somewhere else - the seam
		// showed. Start it at the source's UV on the shared corner instead.
		// A wall of a frozen (Manual) source has no UVs of its own to keep -
		// its far line would carry the near line's, and the texture would be
		// pulled across the wall - so it measures its own, from that corner.
		if (Made.Mapping == ESurfPatchMapping::Manual)
		{
			Made.Mapping = ESurfPatchMapping::Natural;
		}
		if (SurfPatchSurfacing::FollowsTheNet(Made.Mapping))
		{
			Made.UVShift = Source.At(Along[0].X, Along[0].Y).UV;
		}

		if (bOrientByReference && !FacesAwayFrom(Made, Reference))
		{
			Made.bInvert = !Made.bInvert;
		}

		if (bInPlace)
		{
			for (int32 I = 0; I < N; ++I)
			{
				if (bOutColumn)
				{
					Made.At(1, I).Pos = Made.At(0, I).Pos;
				}
				else
				{
					Made.At(I, 1).Pos = Made.At(I, 0).Pos;
				}
			}
		}

		return Made;
	}

	FVector CentreOf(const FSurfPatch& Patch, const TArray<FIntPoint>& Points)
	{
		FVector Sum = FVector::ZeroVector;
		for (const FIntPoint& P : Points)
		{
			Sum += Patch.At(P.X, P.Y).Pos;
		}
		return Points.Num() > 0 ? Sum / Points.Num() : FVector::ZeroVector;
	}

	FVector Centroid(const FSurfPatch& Patch)
	{
		FVector Sum = FVector::ZeroVector;
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			Sum += Point.Pos;
		}
		return Patch.Points.Num() > 0 ? Sum / Patch.Points.Num() : FVector::ZeroVector;
	}

	/** One run of selected points: where it lives, and where it is. */
	struct FSurfLineRun
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		FSurfChain Chain;
		TArray<FVector> World;
	};

	/**
	 * The runs of selected points, per patch - the chains the extrude planner
	 * finds - on the active patch, another, or one each.
	 *
	 * A single point is not an edge (S103a). Two patches that meet at a corner
	 * share that corner's point and a selection travels through shared points,
	 * so picking an edge on each of two patches also picks the one point they
	 * both touch on a third. The lone chains are dropped and counted, so the
	 * verb can say one was there rather than refusing over it.
	 *
	 * Both Bridge and Steps start here: two edges, and what is between them.
	 */
	void GatherLineRuns(const TArray<FSurfPointRef>& Selection,
		TArray<FSurfLineRun>& Runs, int32& OutFaces, int32& OutStray)
	{
		OutFaces = 0;
		OutStray = 0;

		TArray<FSurfExtrudePlan> Plans;
		for (const FSurfPointRef& Ref : Selection)
		{
			ASurfGroup* Group = Ref.Group.Get();
			const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
			if (!Patch || Ref.X < 0 || Ref.X >= Patch->Width || Ref.Y < 0 || Ref.Y >= Patch->Height)
			{
				continue;
			}
			FSurfExtrudePlan* Found = Plans.FindByPredicate([&](const FSurfExtrudePlan& P)
			{
				return P.Group == Group && P.PatchId == Ref.PatchId;
			});
			if (!Found)
			{
				Found = &Plans.AddDefaulted_GetRef();
				Found->Group = Group;
				Found->PatchId = Ref.PatchId;
			}
			Found->Points.AddUnique(FIntPoint(Ref.X, Ref.Y));
		}

		for (FSurfExtrudePlan& Each : Plans)
		{
			const FSurfPatch* Patch = Each.Group->FindPatch(Each.PatchId);
			if (!Patch)
			{
				continue;
			}
			Plan(*Patch, Each);
			OutFaces += Each.Blocks.Num();
			const FTransform ToWorld = Each.Group->GetActorTransform();
			for (FSurfChain& Chain : Each.Chains)
			{
				FSurfLineRun& Run = Runs.AddDefaulted_GetRef();
				Run.Group = Each.Group;
				Run.PatchId = Each.PatchId;
				Run.Chain = MoveTemp(Chain);
				for (const FIntPoint& P : Run.Chain.Points)
				{
					Run.World.Add(ToWorld.TransformPosition(Patch->At(P.X, P.Y).Pos));
				}
			}
		}

		for (int32 I = Runs.Num() - 1; I >= 0; --I)
		{
			if (Runs[I].Chain.Length() < 2)
			{
				Runs.RemoveAt(I);
				++OutStray;
			}
		}

		/**
		 * Two whole lines side by side (S167b): "i dont understand how to
		 * use steps. ive selected a top set of edges and a down set of
		 * edges - it doesnt act." Adjacent lines are a block of cells to
		 * the planner, so they were refused as faces. Two full lines, and
		 * nothing else, on one patch are two edges whatever lies between.
		 */
		if (OutFaces > 0 && Runs.Num() == 0 && Plans.Num() == 1)
		{
			FSurfExtrudePlan& Only = Plans[0];
			const FSurfPatch* Patch = Only.Group->FindPatch(Only.PatchId);
			if (Patch)
			{
				TSet<int32> Columns;
				TSet<int32> Rows;
				for (const FIntPoint& P : Only.Points)
				{
					Columns.Add(P.X);
					Rows.Add(P.Y);
				}
				const bool bTwoColumns = Columns.Num() == 2 && Only.Points.Num() == 2 * Patch->Height;
				const bool bTwoRows = Rows.Num() == 2 && Only.Points.Num() == 2 * Patch->Width;
				if (bTwoColumns || bTwoRows)
				{
					const FTransform ToWorld = Only.Group->GetActorTransform();
					for (const int32 Line : (bTwoColumns ? Columns : Rows))
					{
						FSurfLineRun& Run = Runs.AddDefaulted_GetRef();
						Run.Group = Only.Group;
						Run.PatchId = Only.PatchId;
						const int32 Across = bTwoColumns ? Patch->Height : Patch->Width;
						for (int32 B = 0; B < Across; ++B)
						{
							const FIntPoint P = bTwoColumns ? FIntPoint(Line, B) : FIntPoint(B, Line);
							Run.Chain.Points.Add(P);
							Run.World.Add(ToWorld.TransformPosition(Patch->At(P.X, P.Y).Pos));
						}
					}
					OutFaces = 0;
				}
			}
		}
	}

	/**
	 * The two edges paired so the piece between them does not twist (S105a,
	 * out of Bridge; the indices split out in S132a). Two edges may run
	 * opposite ways and a loop may start anywhere; the pairing with the
	 * least total distance is the one that lies flat.
	 *
	 * Answered as **which** point of the far edge each near point goes with,
	 * because Bridge wants the position and Continue wants to go on asking
	 * the far patch questions about the point it paired with - the line
	 * behind it, in particular, which is the whole of what it continues.
	 */
	void PairIndicesAcross(const FSurfLineRun& A, const FSurfLineRun& B, TArray<int32>& OutFar)
	{
		const int32 N = A.Chain.Points.Num();
		const bool bClosed = A.Chain.bClosed;
		double Best = TNumericLimits<double>::Max();
		int32 BestOffset = 0;
		bool bBestReversed = false;
		for (int32 Offset = 0; Offset < (bClosed ? N : 1); ++Offset)
		{
			for (int32 Reversed = 0; Reversed < 2; ++Reversed)
			{
				double Sum = 0.0;
				for (int32 I = 0; I < N; ++I)
				{
					const int32 J = bClosed
						? (Reversed ? (Offset - I + N) % N : (Offset + I) % N)
						: (Reversed ? N - 1 - I : I);
					Sum += FVector::DistSquared(A.World[I], B.World[J]);
				}
				if (Sum < Best)
				{
					Best = Sum;
					BestOffset = Offset;
					bBestReversed = Reversed != 0;
				}
			}
		}

		OutFar.Reset();
		OutFar.Reserve(N);
		for (int32 I = 0; I < N; ++I)
		{
			OutFar.Add(bClosed
				? (bBestReversed ? (BestOffset - I + N) % N : (BestOffset + I) % N)
				: (bBestReversed ? N - 1 - I : I));
		}
	}

	/** The same pairing, read off as the far edge's points in the near edge's order. */
	void PairAcross(const FSurfLineRun& A, const FSurfLineRun& B, TArray<FVector>& OutFar)
	{
		TArray<int32> Indices;
		PairIndicesAcross(A, B, Indices);
		for (int32 Index : Indices)
		{
			OutFar.Add(B.World[Index]);
		}
	}

	/**
	 * The whole of one border of a patch, as a run (S132a). Side 0 is the
	 * column at X 0, 1 the column at the far X, 2 the row at Y 0, 3 the row
	 * at the far Y - so Side < 2 is a column, and an odd Side is the far
	 * one, which is what IsWholeBorder answers about a selection.
	 */
	void BorderRun(ASurfGroup* Group, const FSurfPatch& Patch, int32 Side, FSurfLineRun& Out)
	{
		Out.Group = Group;
		Out.PatchId = Patch.Id;
		Out.Chain.Points.Reset();
		Out.Chain.bClosed = false;
		Out.World.Reset();

		const bool bColumn = Side < 2;
		const int32 Line = (Side % 2) == 0 ? 0 : ((bColumn ? Patch.Width : Patch.Height) - 1);
		const int32 Along = bColumn ? Patch.Height : Patch.Width;
		const FTransform ToWorld = Group ? Group->GetActorTransform() : FTransform::Identity;

		for (int32 I = 0; I < Along; ++I)
		{
			const FIntPoint At = bColumn ? FIntPoint(Line, I) : FIntPoint(I, Line);
			Out.Chain.Points.Add(At);
			Out.World.Add(ToWorld.TransformPosition(Patch.At(At.X, At.Y).Pos));
		}
	}

	/** The points around a block's edge, in order, starting at its low corner. */
	FSurfChain BlockRim(const FSurfBlock& Block)
	{
		FSurfChain Rim;
		Rim.bClosed = true;
		for (int32 X = Block.X0; X < Block.X1; ++X) { Rim.Points.Add(FIntPoint(X, Block.Y0)); }
		for (int32 Y = Block.Y0; Y < Block.Y1; ++Y) { Rim.Points.Add(FIntPoint(Block.X1, Y)); }
		for (int32 X = Block.X1; X > Block.X0; --X) { Rim.Points.Add(FIntPoint(X, Block.Y1)); }
		for (int32 Y = Block.Y1; Y > Block.Y0; --Y) { Rim.Points.Add(FIntPoint(Block.X0, Y)); }
		return Rim;
	}
}

bool USurfEdMode::PatchFacesAwayFrom(const FSurfPatch& Patch, const FVector& Reference)
{
	return FacesAwayFrom(Patch, Reference);
}

bool USurfEdMode::ExtrudeSelectedEdge(bool bInPlace, double OffsetCm, int32 Under)
{
	// A grow below says which line it made; any other extrude made none (S207b).
	GrownGroup.Reset();
	// What becomes of the faces beneath a lid: the setting, unless the
	// caller (the adjust frame) says.
	const USurfSettings* DropPrefs = GetDefault<USurfSettings>();
	const ESurfExtrudeUnder Beneath = Under < 0
		? (DropPrefs ? DropPrefs->ExtrudeUnder : ESurfExtrudeUnder::Drop)
		: static_cast<ESurfExtrudeUnder>(FMath::Clamp(Under, 0, 2));
	LastExtrudeUnder = Beneath;

	// Object sub-mode has no part of a patch to pull; the faces, an edge or
	// the points have to be named first.
	if (SubMode == ESurfSubMode::Object || PointSelection.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(SubMode == ESurfSubMode::Object
				? TEXT("extrude: pick faces (3), an edge (2) or points (1) first")
				: TEXT("extrude: select some faces, an edge or two points next to each other first"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_nothing_selected"));
		return false;
	}

	// One plan per patch, in the order the patches were first picked.
	TArray<FSurfExtrudePlan> Plans;

	for (const FSurfPointRef& Ref : PointSelection)
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfPatch* Patch = Group ? Group->FindPatch(Ref.PatchId) : nullptr;
		if (!Patch || Ref.X < 0 || Ref.X >= Patch->Width || Ref.Y < 0 || Ref.Y >= Patch->Height)
		{
			continue;
		}

		FSurfExtrudePlan* Found = Plans.FindByPredicate([&](const FSurfExtrudePlan& P)
		{
			return P.Group == Group && P.PatchId == Ref.PatchId;
		});

		if (!Found)
		{
			Found = &Plans.AddDefaulted_GetRef();
			Found->Group = Group;
			Found->PatchId = Ref.PatchId;
		}

		Found->Points.AddUnique(FIntPoint(Ref.X, Ref.Y));
	}

	if (Plans.Num() == 0)
	{
		return false;
	}

	// In face sub-mode the faces are what was picked, whatever their shape:
	// a lid per rectangle of them, one wall round each region of them.
	if (SubMode == ESurfSubMode::Face && QuadSelection.Num() > 0)
	{
		for (FSurfExtrudePlan& Each : Plans)
		{
			const FSurfPatch* Patch = Each.Group->FindPatch(Each.PatchId);
			if (!Patch)
			{
				continue;
			}

			FSurfCellSet Cells(Patch->Width - 1, Patch->Height - 1);
			int32 Quads = 0;
			for (const FSurfQuadRef& Quad : QuadSelection)
			{
				if (Quad.Group.Get() != Each.Group || Quad.PatchId != Each.PatchId)
				{
					continue;
				}
				Cells.Set(Quad.X, Quad.Y);
				++Quads;
			}

			if (Quads == 0)
			{
				continue;
			}
			BlocksAndOutlines(Cells, Each);
		}
	}

	int32 Lone = 0;
	int32 Chains = 0;
	int32 Blocks = 0;

	for (FSurfExtrudePlan& Each : Plans)
	{
		if (const FSurfPatch* Patch = Each.Group->FindPatch(Each.PatchId))
		{
			if (Each.Blocks.Num() == 0)
			{
				Plan(*Patch, Each);
			}
		}

		Lone += Each.Lone;
		Chains += Each.Chains.Num();
		Blocks += Each.Blocks.Num();
	}

	if (Chains == 0 && Blocks == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(Lone == 1
				? TEXT("extrude: one point on its own has nothing to pull - select two next to each other, an edge or faces")
				: TEXT("extrude: the selected points are not next to each other"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_gap_in_edge"));
		return false;
	}

	// The whole of one border line, and nothing else: the patch grows -
	// unless a new patch was asked for (S185b), which the walls below make.
	const USurfSettings* NewPatchPrefs = GetDefault<USurfSettings>();
	const bool bNewPatch = NewPatchPrefs && NewPatchPrefs->bExtrudeNewPatch;
	if (!bNewPatch && Plans.Num() == 1 && Blocks == 0 && Chains == 1 && Lone == 0)
	{
		const FSurfPatch* Patch = Plans[0].Group->FindPatch(Plans[0].PatchId);
		bool bColumn = false;
		bool bFromEnd = false;
		if (Patch && IsWholeBorder(*Patch, Plans[0].Chains[0], bColumn, bFromEnd))
		{
			return GrowSelectedBorder(bColumn, bFromEnd, bInPlace, OffsetCm);
		}

		/**
		 * A whole line inside the patch (S158d): by default a wall of its
		 * own stands on it, as below; with the option on, the patch is cut
		 * there and the near half grows - one surface, no fin. His answer:
		 * "default 1 [the wall]. but you may popup for that."
		 */
		const USurfSettings* Inside = GetDefault<USurfSettings>();
		if (Patch && Inside && Inside->bExtrudeInsideCuts)
		{
			const FSurfChain& Chain = Plans[0].Chains[0];
			const bool bChainColumn = Chain.IsColumn();
			bool bChainRow = true;
			for (const FIntPoint& P : Chain.Points)
			{
				bChainRow &= (P.Y == Chain.Points[0].Y);
			}
			const int32 Along = bChainColumn ? Patch->Height : Patch->Width;
			if ((bChainColumn || bChainRow) && Chain.Points.Num() == Along && !Chain.bClosed)
			{
				const FVector Mark = Plans[0].Group->GetActorTransform().TransformPosition(Patch->At(Chain.Points[0].X, Chain.Points[0].Y).Pos);
				ASurfGroup* Group = Plans[0].Group;
				if (SplitSelectedPatch())
				{
					// The near half carries the line as one of its borders: the
					// border whose first point stands where the line stood.
					const FSurfPatch* Half = Group ? Group->FindPatch(SelectedPatch.PatchId) : nullptr;
					if (Half)
					{
						const FTransform ToWorld = Group->GetActorTransform();
						int32 Line = INDEX_NONE;
						const int32 Lines = bChainColumn ? Half->Width : Half->Height;
						for (const int32 Candidate : { 0, Lines - 1 })
						{
							const FVector At = ToWorld.TransformPosition((bChainColumn ? Half->At(Candidate, 0) : Half->At(0, Candidate)).Pos);
							if (FVector::DistSquared(At, Mark) < 1.0)
							{
								Line = Candidate;
							}
						}
						if (Line != INDEX_NONE)
						{
							PointSelection.Reset();
							const int32 HalfAlong = bChainColumn ? Half->Height : Half->Width;
							for (int32 I = 0; I < HalfAlong; ++I)
							{
								FSurfPointRef Ref;
								Ref.Group = Group;
								Ref.PatchId = Half->Id;
								Ref.X = bChainColumn ? Line : I;
								Ref.Y = bChainColumn ? I : Line;
								PointSelection.Add(Ref);
							}
							SyncPatchSelectionToPoints();
							return GrowSelectedBorder(bChainColumn, Line != 0, bInPlace, OffsetCm);
						}
					}
				}
			}
		}
	}

	// Everything else is new patches, made in one transaction.
	FScopedTransaction Transaction(LOCTEXT("ExtrudeSelection", "Extrude Selection"));
	ExtrudeWallFacing.Reset();
	ExtrudeClosingPieces.Reset();

	TArray<FSurfPointRef> NewSelection;
	TArray<FSurfQuadRef> NewQuads;
	FSurfPatchRef FirstMade;

	/** The lids made for each source patch's blocks, in the blocks' order, for the pieces that close them. */
	TMap<FGuid, TArray<FSurfPatchRef>> LidsOf;
	FVector DirectionSum = FVector::ZeroVector;
	int32 Made = 0;
	int32 Lids = 0;
	int32 Walls = 0;
	double OffsetUsed = 0.0;

	for (FSurfExtrudePlan& Each : Plans)
	{
		ASurfGroup* Group = Each.Group;
		const FSurfPatch* Found = Group->FindPatch(Each.PatchId);
		if (!Found)
		{
			continue;
		}

		const FSurfPatch Source = *Found;
		const FTransform ToWorld = Group->GetActorTransform();

		Group->Modify();

		auto Add = [&](FSurfPatch&& Patch, TFunctionRef<void(const FGuid&)> Select)
		{
			const FGuid Id = Patch.Id;
			FSurfPatch& Added = Group->AddPatch(MoveTemp(Patch));
			SurfPatchSurfacing::Reapply(Added);
			++Made;

			if (!FirstMade.IsValid())
			{
				FirstMade.Group = Group;
				FirstMade.PatchId = Id;
			}

			Select(Id);
		};

		auto SelectPointOn = [&](const FGuid& Id, int32 X, int32 Y)
		{
			FSurfPointRef Ref;
			Ref.Group = Group;
			Ref.PatchId = Id;
			Ref.X = X;
			Ref.Y = Y;
			NewSelection.AddUnique(Ref);
		};

		// A wall along a chain, its far line selected. In place it has no
		// height yet, so its facing is a guess from the surface normal; the
		// move that follows checks it again against the same reference. An
		// outline's wall is made facing the right way for a pull either way
		// - the outline runs with its cells on the left, so pushed out of
		// the surface the wall faces out of them, and pushed in, into them:
		// a pit's walls are seen from inside. A box pulled in hangs under
		// its surface, walls out like any box, so its wall is turned as its
		// lid is. An edge's wall faces away from its patch either way - a
		// zero Normal says the way of the pull does not matter.
		auto AddWall = [&](const FSurfChain& Chain, const FVector& Offset, const FVector& Reference,
			const FVector& Normal, double Along, const FVector& Base, bool bOutline, bool bBox)
		{
			bool bColumn = false;
			FSurfPatch Wall = MakeWall(Source, Chain, Offset, Reference, bInPlace, bColumn, /*bOrientByReference*/ !bOutline);
			if (!bInPlace)
			{
				if (bOutline)
				{
					if (bBox && Along < 0.0)
					{
						Wall.bInvert = !Wall.bInvert;
					}
				}
				else
				{
					// An edge's wall pulled within the surface (a floor grown
					// past its edge) faces as the surface does; "away from
					// the patch" means nothing for a piece in the same plane.
					const FVector WallNormal = SurfPatchSurfacing::AverageNormal(Wall);
					const double Agreement = FVector::DotProduct(WallNormal, Normal);
					if (FMath::Abs(Agreement) > 0.7 && Agreement < 0.0)
					{
						Wall.bInvert = !Wall.bInvert;
					}
				}
			}
			const int32 N = Chain.Length();

			Add(MoveTemp(Wall), [&](const FGuid& Id)
			{
				for (int32 I = 0; I < N; ++I)
				{
					SelectPointOn(Id, bColumn ? 1 : I, bColumn ? I : 1);
				}
				if (bInPlace)
				{
					FExtrudeFacing Facing;
					Facing.Patch.Group = Group;
					Facing.Patch.PatchId = Id;
					Facing.Reference = Reference;
					Facing.Normal = Normal;
					Facing.Base = Base;
					Facing.bOutline = bOutline;
					Facing.bBox = bBox;
					ExtrudeWallFacing.Add(Facing);
				}
			});
			++Walls;
		};

		if (Each.Blocks.Num() > 0)
		{
			// The union of the blocks: one normal, one distance, one centre,
			// so the lids and the wall round them meet exactly.
			TArray<FIntPoint> UnionPoints;
			for (const FSurfBlock& Block : Each.Blocks)
			{
				for (int32 Y = Block.Y0; Y <= Block.Y1; ++Y)
				{
					for (int32 X = Block.X0; X <= Block.X1; ++X)
					{
						UnionPoints.AddUnique(FIntPoint(X, Y));
					}
				}
			}

			const FVector Normal = NormalOver(Source, UnionPoints);
			const double Along = DistanceOr(OffsetCm, SpacingOver(Source, UnionPoints));
			const FVector Offset = Normal * Along;
			const FVector UnionCentre = CentreOf(Source, UnionPoints);
			OffsetUsed = Along;

			DirectionSum += ToWorld.TransformVector(Normal).GetSafeNormal();

			// A dropped block's lid is the surface itself, moved, and keeps
			// its facing: a pit's floor still faces up. A box's lid (the
			// faces beneath closed, or kept) faces away from the surface it
			// was pulled from, so pulled in it faces the other way from its
			// source. In place the pull is not known yet; the move decides.
			const bool bBoxLid = Beneath != ESurfExtrudeUnder::Drop;

			for (const FSurfBlock& Block : Each.Blocks)
			{
				TArray<FIntPoint> BlockPoints;
				for (int32 Y = Block.Y0; Y <= Block.Y1; ++Y)
				{
					for (int32 X = Block.X0; X <= Block.X1; ++X)
					{
						BlockPoints.Add(FIntPoint(X, Y));
					}
				}
				const FVector BlockCentre = CentreOf(Source, BlockPoints);

				// The lid: the block's points, pushed along the normal. Same
				// point order as the source, so it faces the same way.
				FSurfPatch Lid = DeriveSheet(Source, Block.Columns(), Block.Rows());
				for (int32 Y = Block.Y0; Y <= Block.Y1; ++Y)
				{
					for (int32 X = Block.X0; X <= Block.X1; ++X)
					{
						FSurfControlPoint Point = Source.At(X, Y);
						Point.Pos += bInPlace ? FVector::ZeroVector : Offset;
						Lid.At(X - Block.X0, Y - Block.Y0) = Point;
					}
				}
				if (SurfPatchSurfacing::FollowsTheNet(Lid.Mapping))
				{
					Lid.UVShift = Source.At(Block.X0, Block.Y0).UV;
				}
				if (!bInPlace && bBoxLid && Along < 0.0)
				{
					Lid.bInvert = !Lid.bInvert;
				}

				Add(MoveTemp(Lid), [&](const FGuid& Id)
				{
					FSurfPatchRef LidRef;
					LidRef.Group = Group;
					LidRef.PatchId = Id;
					LidsOf.FindOrAdd(Each.PatchId).Add(LidRef);

					if (bInPlace)
					{
						FExtrudeFacing Facing;
						Facing.Patch = LidRef;
						Facing.Reference = BlockCentre;
						Facing.Normal = Normal;
						Facing.Base = BlockCentre;
						Facing.bLid = true;
						Facing.bBox = bBoxLid;
						ExtrudeWallFacing.Add(Facing);
					}

					for (int32 Y = 0; Y < Block.Rows(); ++Y)
					{
						for (int32 X = 0; X < Block.Columns(); ++X)
						{
							SelectPointOn(Id, X, Y);
						}
					}

					for (int32 Y = 0; Y + 1 < Block.Rows(); ++Y)
					{
						for (int32 X = 0; X + 1 < Block.Columns(); ++X)
						{
							FSurfQuadRef Quad;
							Quad.Group = Group;
							Quad.PatchId = Id;
							Quad.X = X;
							Quad.Y = Y;
							NewQuads.Add(Quad);
						}
					}
				});
				++Lids;
			}

			// One wall all the way round each region the blocks make, its
			// top on the lids' edges - so lids and wall meet exactly and
			// move as one. One patch per region, not one per block or per
			// side: a sheet's cells are flat, so a corner in the chain is a
			// corner in the wall, and an L is two lids under one wall. A
			// ring is two walls, the hole's facing into the hole.
			for (const FSurfChain& Outline : Each.Outlines)
			{
				AddWall(Outline, Offset, UnionCentre, Normal, Along, CentreOf(Source, Outline.Points), /*bOutline*/ true, bBoxLid);
			}
		}

		const FVector PatchCentre = Centroid(Source);

		for (const FSurfChain& Chain : Each.Chains)
		{
			const FVector Normal = NormalOver(Source, Chain.Points);
			const double Along = DistanceOr(OffsetCm, SpacingOver(Source, Chain.Points));
			const FVector Offset = Normal * Along;
			OffsetUsed = Along;

			DirectionSum += ToWorld.TransformVector(Normal).GetSafeNormal();

			// A wall on a border faces out of the patch; one through the
			// middle has no outside, and keeps the way it was made.
			AddWall(Chain, Offset, PatchCentre, Normal, Along, CentreOf(Source, Chain.Points), /*bOutline*/ false, /*bBox*/ false);
		}
	}

	if (Made == 0)
	{
		Transaction.Cancel();
		return false;
	}

	// The faces beneath each lid: dropped when asked - the block is hollow,
	// and the rest of its patch comes apart into patches round the hole,
	// because a grid cannot have one - or cut into a patch of their own
	// that closes the box, facing out of it. A curve cannot lose faces (its
	// net is not its surface), so its faces stay and the line says so.
	int32 Dropped = 0;
	int32 Closed = 0;
	int32 ClosingPieces = 0;
	int32 Kept = 0;
	int32 PiecesAround = 0;
	if (Beneath != ESurfExtrudeUnder::Keep)
	{
		for (const FSurfExtrudePlan& Each : Plans)
		{
			if (Each.Blocks.Num() == 0 || !Each.Group->FindPatch(Each.PatchId))
			{
				continue;
			}

			// Every block's cells at once: the cut takes the whole of them
			// out, a piece per block, and the rest of the patch comes apart
			// round the hole.
			QuadSelection.Reset();
			for (const FSurfBlock& Block : Each.Blocks)
			{
				for (int32 Y = Block.Y0; Y < Block.Y1; ++Y)
				{
					for (int32 X = Block.X0; X < Block.X1; ++X)
					{
						FSurfQuadRef Quad;
						Quad.Group = Each.Group;
						Quad.PatchId = Each.PatchId;
						Quad.X = X;
						Quad.Y = Y;
						QuadSelection.Add(Quad);
					}
				}
			}
			SelectedPatch.Group = Each.Group;
			SelectedPatch.PatchId = Each.PatchId;

			const bool bClose = Beneath == ESurfExtrudeUnder::Close;
			TArray<FSurfPatchRef> Pieces;
			int32 Rest = 0;
			if (CutSelectedFaces(bClose ? ESurfCutPiece::Keep : ESurfCutPiece::Drop, 0.0, -1, true, false, &Pieces, &Rest))
			{
				PiecesAround += Rest;
				if (bClose)
				{
					// The pieces are the blocks' cells again, one per block
					// in the blocks' order (the cut takes the same cells
					// apart the same way). Each faces the way its source did
					// - into the box once the lid is pulled off it - so each
					// is turned to face away from its lid: now, when the lid
					// is already out; after the move otherwise.
					++Closed;
					ClosingPieces += Pieces.Num();
					const TArray<FSurfPatchRef>* LidRefs = LidsOf.Find(Each.PatchId);
					for (int32 I = 0; I < Pieces.Num(); ++I)
					{
						const FSurfPatchRef* LidRef = LidRefs && LidRefs->IsValidIndex(I) ? &(*LidRefs)[I] : nullptr;
						if (!Pieces[I].IsValid() || !LidRef || !LidRef->IsValid())
						{
							continue;
						}
						if (bInPlace)
						{
							ExtrudeClosingPieces.Emplace(Pieces[I], *LidRef);
						}
						else if (const FSurfPatch* LidPatch = LidRef->Group->FindPatch(LidRef->PatchId))
						{
							if (FSurfPatch* Piece = Each.Group->ModifyPatch(Pieces[I].PatchId))
							{
								const FVector LidCentre = Centroid(*LidPatch);
								if (!FacesAwayFrom(*Piece, LidCentre))
								{
									Piece->bInvert = !Piece->bInvert;
									++Piece->EditSerial;
								}
							}
						}
					}
				}
				else
				{
					++Dropped;
				}
			}
			else
			{
				++Kept;
			}
		}
	}

	for (const FSurfExtrudePlan& Each : Plans)
	{
		Each.Group->RebuildPreview();
	}

	// Which way to pull, for the move that follows: the mean of the normals,
	// or nothing when they cancel and the move is better left free.
	LastExtrudeDirection = DirectionSum.GetSafeNormal();

	// The new lines are the selection, on the new patches, so the move that
	// follows pulls what was just made and not the lines it came from.
	SelectPatch(FirstMade, /*bKeepPoints*/ true);
	PointSelection = MoveTemp(NewSelection);
	QuadSelection = SubMode == ESurfSubMode::Face ? MoveTemp(NewQuads) : TArray<FSurfQuadRef>();
	SyncPatchSelectionToPoints();
	LastRowColumnPoint = FSurfPointRef();
	RowColumnStage = 2;
	RefreshSoftSelection();
	UpdateOrbitPivot();

	if (Inspector)
	{
		FString Line;
		if (Lids > 0 && Chains == 0)
		{
			Line = Lids == 1
				? FString(TEXT("extruded the faces into a lid and one wall around them"))
				: (Plans.Num() == 1 && Walls == 1
					? FString::Printf(TEXT("extruded the faces into %d lids under one wall around them"), Lids)
					: FString::Printf(TEXT("extruded %d blocks of faces into %d new patches"), Lids, Made));
			if (Dropped > 0)
			{
				Line += FString::Printf(TEXT(" - the faces beneath are dropped, the rest is %d patch%s"),
					PiecesAround, PiecesAround == 1 ? TEXT("") : TEXT("es"));
			}
			else if (Closed > 0)
			{
				Line += FString::Printf(TEXT(" - the faces beneath close the box as %s, the rest is %d patch%s"),
					ClosingPieces == 1 ? TEXT("their own patch") : *FString::Printf(TEXT("%d patches of their own"), ClosingPieces),
					PiecesAround, PiecesAround == 1 ? TEXT("") : TEXT("es"));
			}
			else if (Kept > 0)
			{
				Line += TEXT(" - the faces beneath stay (a curve cannot lose faces)");
			}
			else
			{
				Line += TEXT(" - the faces beneath stay");
			}
		}
		else if (Lids == 0 && Walls == 1)
		{
			const FSurfChain& Only = Plans[0].Chains[0];
			const bool bColumn = !Only.bClosed && Only.IsColumn();
			Line = FString::Printf(TEXT("extruded the line into a new %d x %d patch%s"),
				bColumn ? 2 : Only.Length(), bColumn ? Only.Length() : 2,
				Only.bClosed ? TEXT(" that meets itself") : TEXT(""));
		}
		else if (Lids == 0)
		{
			Line = FString::Printf(TEXT("extruded %d lines into %d new patches"), Walls, Made);
		}
		else
		{
			Line = FString::Printf(TEXT("extruded %d lines and %d blocks into %d new patches"), Walls, Lids, Made);
		}

		if (Plans.Num() > 1)
		{
			Line += FString::Printf(TEXT(" across %d patches"), Plans.Num());
		}

		if (Lone > 0)
		{
			Line += FString::Printf(TEXT(" (%d point%s on their own skipped)"), Lone, Lone == 1 ? TEXT("") : TEXT("s"));
		}

		Inspector->SetResult(Line);
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("extrude_new"), FString::Printf(TEXT("chains=%d blocks=%d walls=%d lone=%d patches=%d made=%d dropped=%d closed=%d"),
		Chains, Lids, Walls, Lone, Plans.Num(), Made, Dropped, Closed));

	// Pushed out by a distance: that distance can be adjusted afterwards. In
	// place, the move that follows is the adjustable thing.
	if (!bInPlace)
	{
		FSurfOperation Op;
		Op.Kind = ESurfOpKind::Extrude;
		Op.Distance = OffsetUsed;
		Op.Under = Beneath;
		NoteOperation(Op);
	}

	if (GEditor)
	{
		RedrawViewports();
	}

	return true;
}

bool USurfEdMode::BridgeSelectedLines(bool bJoin)
{
	TArray<FSurfLineRun> Runs;
	int32 Faces = 0;
	int32 Stray = 0;
	GatherLineRuns(GetPointSelection(), Runs, Faces, Stray);

	auto Refuse = [&](const FString& Why, const TCHAR* Tag)
	{
		if (Inspector)
		{
			Inspector->SetResult(Why);
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(Tag);
		return false;
	};

	if (Faces > 0)
	{
		return Refuse(TEXT("bridge: select two edges (runs of points), not faces"), TEXT("bridge_faces"));
	}
	if (Runs.Num() != 2)
	{
		// And when it still cannot go, say what it found rather than only how
		// many: a run of five points and a run of two are a different problem
		// from three runs of five, and the number alone said neither.
		FString Sizes;
		for (const FSurfLineRun& Run : Runs)
		{
			Sizes += FString::Printf(TEXT("%s%d"), Sizes.IsEmpty() ? TEXT("") : TEXT(", "), Run.Chain.Length());
		}
		return Refuse(Runs.Num() == 0
			? FString(TEXT("bridge: select two edges first - an edge on each patch, or two on one"))
			: FString::Printf(TEXT("bridge: needs exactly two edges, found %d (of %s points) - Alt-click picks a whole line"),
				Runs.Num(), *Sizes), TEXT("bridge_not_two_lines"));
	}
	if (Runs[0].Chain.Length() != Runs[1].Chain.Length())
	{
		return Refuse(FString::Printf(TEXT("bridge: the two edges have %d and %d points - subdivide or insert lines so they match"),
			Runs[0].Chain.Length(), Runs[1].Chain.Length()), TEXT("bridge_lengths_differ"));
	}
	if (Runs[0].Chain.bClosed != Runs[1].Chain.bClosed)
	{
		return Refuse(TEXT("bridge: one edge is a loop and the other is not"), TEXT("bridge_loop_and_line"));
	}

	// The bridge goes into the first edge's group, in its space.
	FSurfLineRun& A = Runs[0];
	FSurfLineRun& B = Runs[1];
	ASurfGroup* Group = A.Group;
	const FSurfPatch* SourceA = Group->FindPatch(A.PatchId);
	const FSurfPatch* SourceB = B.Group->FindPatch(B.PatchId);
	if (!SourceA || !SourceB)
	{
		return false;
	}

	// The two opposite borders of one patch (S208a): the patch closes on
	// itself - one line grown past the first border and set down on the
	// other, point for point, so the tube is one patch with its last line
	// doubled. "was expecting it to create the last face - and have
	// overlapped vertices at the end." No bridge patch, nothing to face.
	{
		bool bColumnA = false, bFromEndA = false, bColumnB = false, bFromEndB = false;
		if (A.PatchId == B.PatchId && !A.Chain.bClosed && !B.Chain.bClosed
			&& IsWholeBorder(*SourceA, A.Chain, bColumnA, bFromEndA) && IsWholeBorder(*SourceB, B.Chain, bColumnB, bFromEndB)
			&& bColumnA == bColumnB && bFromEndA != bFromEndB)
		{
			FScopedTransaction Closing(LOCTEXT("BridgeClose", "Bridge Edges"));
			FSurfPatch* Mutable = Group->ModifyPatch(A.PatchId);
			if (!Mutable || !SurfPatchPrimitives::ExtrudeEdge(*Mutable, bColumnA, bFromEndA, 1, /*bInPlace*/ true))
			{
				Closing.Cancel();
				return Refuse(TEXT("bridge: the patch has no room left to close"), TEXT("bridge_close_refused"));
			}
			const int32 Lines = bColumnA ? Mutable->Width : Mutable->Height;
			const int32 NewAt = bFromEndA ? Lines - 1 : 0;
			const int32 OppositeAt = bFromEndA ? 0 : Lines - 1;
			const int32 Across = bColumnA ? Mutable->Height : Mutable->Width;
			for (int32 K = 0; K < Across; ++K)
			{
				FSurfControlPoint& New = bColumnA ? Mutable->At(NewAt, K) : Mutable->At(K, NewAt);
				const FSurfControlPoint& Other = bColumnA ? Mutable->At(OppositeAt, K) : Mutable->At(K, OppositeAt);
				New.Pos = Other.Pos;
			}
			GrownGroup = Group;
			GrownPatch = A.PatchId;
			bGrownColumn = bColumnA;
			bGrownFromEnd = bFromEndA;
			CarryGrownUVs();
			GrownGroup.Reset();
			SurfPatchSurfacing::Reapply(*Mutable);
			Group->RebuildPreview();

			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = A.PatchId;
			SelectPatch(Ref);
			ClearPointSelection();
			for (int32 K = 0; K < Across; ++K)
			{
				FSurfPointRef Point;
				Point.Group = Group;
				Point.PatchId = A.PatchId;
				Point.X = bColumnA ? NewAt : K;
				Point.Y = bColumnA ? K : NewAt;
				AddPointToSelection(Point);
			}
			if (Inspector)
			{
				Inspector->SetResult(FString::Printf(TEXT("bridged: the patch closes on itself, %d x %d - its last line lies on its first"), Mutable->Width, Mutable->Height));
				Inspector->RefreshSelectionCount();
			}
			SurfSession::Verb(TEXT("bridge"), FString::Printf(TEXT("%dx1 closed=patch"), Across));
			if (GEditor)
			{
				RedrawViewports();
			}
			return true;
		}
	}
	const FTransform ToLocal = Group->GetActorTransform().Inverse();
	const int32 N = A.Chain.Points.Num();

	TArray<FVector> Near;
	TArray<FVector> Far;
	for (int32 I = 0; I < N; ++I)
	{
		Near.Add(ToLocal.TransformPosition(A.World[I]));
	}

	PairAcross(A, B, Far);
	for (FVector& Point : Far)
	{
		Point = ToLocal.TransformPosition(Point);
	}

	// A sheet with the two edges as its two lines; a loop meets itself.
	const int32 Count = A.Chain.Length();
	FSurfPatch Made = DeriveSheet(*SourceA, Count, 2);
	for (int32 I = 0; I < Count; ++I)
	{
		const int32 K = I % N;
		FSurfControlPoint NearPoint = SourceA->At(A.Chain.Points[K].X, A.Chain.Points[K].Y);
		NearPoint.Pos = Near[K];
		FSurfControlPoint FarPoint = NearPoint;
		FarPoint.Pos = Far[K];
		Made.At(I, 0) = NearPoint;
		Made.At(I, 1) = FarPoint;
	}
	if (Made.Mapping == ESurfPatchMapping::Manual)
	{
		Made.Mapping = ESurfPatchMapping::Natural;
	}
	if (SurfPatchSurfacing::FollowsTheNet(Made.Mapping))
	{
		Made.UVShift = SourceA->At(A.Chain.Points[0].X, A.Chain.Points[0].Y).UV;
	}

	// Facing: as the patch it starts from, so a bridge between two floors
	// faces up with them. Along a border the winding says it outright
	// (S208a): two cells that share an edge run it opposite ways, and the
	// source's cell beside the edge runs it one known way - an average
	// normal says nothing on a patch bent into a tube. Elsewhere (an
	// interior line, a loop) the averages decide as before.
	{
		int32 SourceRuns = 0;
		if (!A.Chain.bClosed && A.Chain.Points.Num() >= 2)
		{
			const bool bColumn = A.Chain.IsColumn();
			bool bRow = true;
			for (const FIntPoint& P : A.Chain.Points)
			{
				bRow &= (P.Y == A.Chain.Points[0].Y);
			}
			if (bColumn || bRow)
			{
				const int32 Line = bColumn ? A.Chain.Points[0].X : A.Chain.Points[0].Y;
				const int32 Lines = bColumn ? SourceA->Width : SourceA->Height;
				if (Line == 0 || Line == Lines - 1)
				{
					// The cell beside a bottom row runs it +X, beside a top
					// row -X; beside a left column -Y, beside a right one +Y.
					const int32 Beside = bColumn ? (Line == 0 ? -1 : 1) : (Line == 0 ? 1 : -1);
					const int32 ChainWay = bColumn
						? FMath::Sign(A.Chain.Points[1].Y - A.Chain.Points[0].Y)
						: FMath::Sign(A.Chain.Points[1].X - A.Chain.Points[0].X);
					SourceRuns = Beside * ChainWay * (SourceA->bInvert ? -1 : 1);
				}
			}
		}
		if (SourceRuns != 0)
		{
			// The bridge's first row is the chain in order: uninverted it runs
			// the edge the chain's way. It has to run it the other way.
			Made.bInvert = SourceRuns > 0;
		}
		else
		{
			const FVector SourceNormal = SurfPatchSurfacing::AverageNormal(*SourceA);
			const FVector MadeNormal = SurfPatchSurfacing::AverageNormal(Made);
			if (!SourceNormal.IsNearlyZero() && !MadeNormal.IsNearlyZero()
				&& FVector::DotProduct(SourceNormal, MadeNormal) < 0.0)
			{
				Made.bInvert = !Made.bInvert;
			}
		}
	}

	// Whether each edge is the whole of a border of its patch: then the
	// bridge meets that patch point for point and can be joined into it.
	bool bColumnA = false, bFromEndA = false, bColumnB = false, bFromEndB = false;
	const bool bWholeA = !A.Chain.bClosed && IsWholeBorder(*SourceA, A.Chain, bColumnA, bFromEndA);
	const bool bWholeB = !B.Chain.bClosed && IsWholeBorder(*SourceB, B.Chain, bColumnB, bFromEndB);

	FScopedTransaction Transaction(LOCTEXT("Bridge", "Bridge Edges"));

	Group->Modify();
	const FGuid MadeId = Made.Id;
	FSurfPatch& Added = Group->AddPatch(MoveTemp(Made));
	SurfPatchSurfacing::Reapply(Added);
	Group->RebuildPreview();

	FSurfPatchRef BridgeRef;
	BridgeRef.Group = Group;
	BridgeRef.PatchId = MadeId;

	// Connected when it can be: the patches the bridge meets whole are
	// joined into one with it, the first edge's patch surviving. Two edges
	// of one patch stay a patch of their own - joined, the bridge would
	// fold the patch back onto itself.
	int32 Joined = 0;
	if (bJoin && A.PatchId != B.PatchId && (bWholeA || (bWholeB && B.Group == Group)))
	{
		FSurfPatchRef RefA;
		RefA.Group = Group;
		RefA.PatchId = A.PatchId;
		FSurfPatchRef RefB;
		RefB.Group = B.Group;
		RefB.PatchId = B.PatchId;

		TArray<FSurfPatchRef> Picked;
		if (bWholeA) { Picked.Add(RefA); }
		Picked.Add(BridgeRef);
		if (bWholeB && B.Group == Group) { Picked.Add(RefB); }

		const int32 Before = Group->NumPatches();
		SelectedPatch = bWholeA ? RefA : BridgeRef;
		PatchSelection.Reset();
		PatchSelection.Add(SelectedPatch);
		if (JoinPickedPatches(Picked, 0.01))
		{
			Joined = Before - Group->NumPatches();
		}
	}

	FSurfPatchRef Survivor;
	Survivor.Group = Group;
	Survivor.PatchId = Joined > 0 ? (bWholeA ? A.PatchId : MadeId) : MadeId;
	if (!Group->FindPatch(Survivor.PatchId))
	{
		Survivor = BridgeRef;
	}
	SelectPatch(Survivor);

	if (Inspector && Stray > 0)
	{
		// Said once, so a corner shared with a third patch stops looking like
		// a refusal that fixed itself.
		Inspector->SetResult(FString::Printf(
			TEXT("bridge: made - %d lone point%s of another patch was in the selection and ignored"),
			Stray, Stray == 1 ? TEXT("") : TEXT("s")));
	}
	else if (Inspector)
	{
		const FSurfPatch* Now = Group->FindPatch(Survivor.PatchId);
		Inspector->SetResult(Joined > 0 && Now
			? FString::Printf(TEXT("bridged and joined into one patch: %d x %d"), Now->Width, Now->Height)
			: FString::Printf(TEXT("bridged: a new %d x 2 patch between the two edges%s"), Count,
				(bJoin && !(bWholeA || bWholeB)) ? TEXT(" (not joined: neither edge is a whole border)") : TEXT("")));
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("bridge"), FString::Printf(TEXT("%dx2 closed=%d patches=%s joined=%d"), Count, A.Chain.bClosed ? 1 : 0,
		A.PatchId == B.PatchId ? TEXT("same") : (A.Group == B.Group ? TEXT("two") : TEXT("two groups")), Joined));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

bool USurfEdMode::StepsBetweenSelectedLines(int32 StepsAsked, double RiseCm)
{
	/**
	 * Steps between two edges (S105a).
	 *
	 * "a steps button? to create step (config) between selected edges?" -
	 * and, asked how it should be said: "By cm and/or amount". So both: a
	 * count, or a rise in centimetres that the count falls out of, which is
	 * how stairs are written down everywhere outside a modelling tool.
	 *
	 * The same two edges Bridge takes, and the same pairing. Where the bridge
	 * lays one sheet across the gap, this walks it: forward the width of a
	 * tread, up the height of a riser, and again, so the whole staircase is a
	 * single sheet patch of 2n+1 lines. A sheet's control points are its
	 * vertices - nothing is evaluated between them - so the corners stay
	 * corners and no step is rounded off.
	 *
	 * Tread first, from the near edge: the first surface is level with the
	 * edge it starts at, which is what you want whether that edge is the top
	 * of the stair or the bottom.
	 */
	TArray<FSurfLineRun> Runs;
	int32 Faces = 0;
	int32 Stray = 0;
	GatherLineRuns(GetPointSelection(), Runs, Faces, Stray);

	auto Refuse = [&](const FString& Why, const TCHAR* Tag)
	{
		if (Inspector)
		{
			Inspector->SetResult(Why);
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(Tag);
		return false;
	};

	if (Faces > 0)
	{
		return Refuse(TEXT("steps: pick two lines - the top and the bottom of the stair (2: Alt-click one, Shift+Alt-click the other) - not a block of faces"), TEXT("steps_faces"));
	}
	if (Runs.Num() != 2)
	{
		FString Sizes;
		for (const FSurfLineRun& Run : Runs)
		{
			Sizes += FString::Printf(TEXT("%s%d"), Sizes.IsEmpty() ? TEXT("") : TEXT(", "), Run.Chain.Length());
		}
		return Refuse(Runs.Num() == 0
			? FString(TEXT("steps: select two edges first - the top and the bottom of the stair"))
			: FString::Printf(TEXT("steps: needs exactly two edges, found %d (of %s points) - Alt-click picks a whole line"),
				Runs.Num(), *Sizes), TEXT("steps_not_two_lines"));
	}
	if (Runs[0].Chain.Length() != Runs[1].Chain.Length())
	{
		return Refuse(FString::Printf(TEXT("steps: the two edges have %d and %d points - subdivide or insert lines so they match"),
			Runs[0].Chain.Length(), Runs[1].Chain.Length()), TEXT("steps_lengths_differ"));
	}
	if (Runs[0].Chain.bClosed != Runs[1].Chain.bClosed)
	{
		return Refuse(TEXT("steps: one edge is a loop and the other is not"), TEXT("steps_loop_and_line"));
	}

	FSurfLineRun& A = Runs[0];
	FSurfLineRun& B = Runs[1];
	ASurfGroup* Group = A.Group;
	const FSurfPatch* SourceA = Group ? Group->FindPatch(A.PatchId) : nullptr;
	if (!SourceA)
	{
		return false;
	}

	TArray<FVector> Far;
	PairAcross(A, B, Far);

	const int32 N = A.Chain.Points.Num();
	const int32 Count = A.Chain.Length();

	// The drop and the run, averaged over the pairs: one staircase, not one
	// per point.
	FVector Mean = FVector::ZeroVector;
	for (int32 I = 0; I < N; ++I)
	{
		Mean += Far[I] - A.World[I];
	}
	Mean /= static_cast<double>(FMath::Max(1, N));

	const double Drop = FMath::Abs(Mean.Z);
	const double Run = FVector2D(Mean.X, Mean.Y).Size();
	if (Drop < 0.1 && Run < 0.1)
	{
		return Refuse(TEXT("steps: the two edges are in the same place"), TEXT("steps_no_gap"));
	}
	if (Drop < 0.1)
	{
		// A staircase with no drop is a floor, and Bridge already makes that.
		return Refuse(TEXT("steps: the two edges are at the same height - Bridge makes the flat piece"), TEXT("steps_no_drop"));
	}

	/**
	 * How many. A rise in centimetres wins when one is given, because it is
	 * the measurement a stair is actually specified by; the count is what is
	 * left when it is not.
	 *
	 * Neither falls back to the settings here - the caller resolves that, so
	 * that the adjust frame's "no rise" means no rise rather than "whatever
	 * the settings say", which would make the count knob do nothing.
	 */
	const double Rise = FMath::Max(0.0, RiseCm);
	int32 Steps = StepsAsked > 0 ? StepsAsked : 8;
	if (Rise > 0.1)
	{
		Steps = FMath::RoundToInt(Drop / Rise);
	}
	Steps = FMath::Clamp(Steps, 1, 128);

	const FTransform ToLocal = Group->GetActorTransform().Inverse();
	FSurfPatch Made = DeriveSheet(*SourceA, Count, 2 * Steps + 1);

	for (int32 I = 0; I < Count; ++I)
	{
		const int32 K = I % N;
		const FSurfControlPoint Carried = SourceA->At(A.Chain.Points[K].X, A.Chain.Points[K].Y);
		const FVector Near = A.World[K];
		const FVector Delta = Far[K] - Near;
		const FVector Flat(Delta.X, Delta.Y, 0.0);
		const FVector Up(0.0, 0.0, Delta.Z);

		auto Put = [&](int32 Row, const FVector& World)
		{
			FSurfControlPoint Point = Carried;
			Point.Pos = ToLocal.TransformPosition(World);
			Made.At(I, Row) = Point;
		};

		Put(0, Near);
		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			const double Ahead = static_cast<double>(Step) / static_cast<double>(Steps);
			const double Below = static_cast<double>(Step - 1) / static_cast<double>(Steps);
			Put(2 * Step - 1, Near + Flat * Ahead + Up * Below);
			Put(2 * Step,     Near + Flat * Ahead + Up * Ahead);
		}
	}

	if (Made.Mapping == ESurfPatchMapping::Manual)
	{
		Made.Mapping = ESurfPatchMapping::Natural;
	}
	if (SurfPatchSurfacing::FollowsTheNet(Made.Mapping))
	{
		Made.UVShift = SourceA->At(A.Chain.Points[0].X, A.Chain.Points[0].Y).UV;
	}

	// Facing: as the patch it starts from, so a stair down from a floor has
	// its treads up with that floor.
	{
		const FVector SourceNormal = SurfPatchSurfacing::AverageNormal(*SourceA);
		const FVector MadeNormal = SurfPatchSurfacing::AverageNormal(Made);
		if (!SourceNormal.IsNearlyZero() && !MadeNormal.IsNearlyZero()
			&& FVector::DotProduct(SourceNormal, MadeNormal) < 0.0)
		{
			Made.bInvert = !Made.bInvert;
		}
	}

	FScopedTransaction Transaction(LOCTEXT("Steps", "Steps"));

	Group->Modify();
	const FGuid MadeId = Made.Id;
	FSurfPatch& Added = Group->AddPatch(MoveTemp(Made));
	SurfPatchSurfacing::Reapply(Added);
	Group->RebuildPreview();

	FSurfPatchRef MadeRef;
	MadeRef.Group = Group;
	MadeRef.PatchId = MadeId;
	SelectPatch(MadeRef);

	if (Inspector)
	{
		FString Line = FString::Printf(TEXT("steps: %d, %.0f cm up and %.0f cm along each%s"),
			Steps, Drop / Steps, Run / Steps,
			Rise > 0.1 ? TEXT(" (from the rise asked for)") : TEXT(""));
		if (Stray > 0)
		{
			Line += FString::Printf(TEXT(" - %d lone point%s of another patch ignored"),
				Stray, Stray == 1 ? TEXT("") : TEXT("s"));
		}
		Inspector->SetResult(Line);
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("steps"), FString::Printf(TEXT("steps=%d points=%d closed=%d rise=%.1f drop=%.1f run=%.1f"),
		Steps, Count, A.Chain.bClosed ? 1 : 0, Rise, Drop, Run));

	FSurfOperation Op;
	Op.Kind = ESurfOpKind::Steps;
	Op.Iterations = Steps;
	Op.Distance = Rise;
	NoteOperation(Op);

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

/**
 * Continue: the selected border meets the one across from it, and the
 * surface goes on across the seam without a crease (S132a).
 *
 * His idea, twice, and top of the list of things nobody else's tool does:
 *
 *   "HERE FEATURE IDEA: adapt and snap suggest toward the other patch. like
 *   an auto continuity suggest. with a radius of blendadapt"
 *
 *   "if my mouse cursor goes over another one suggest a snap then"
 *
 * Everything that already reaches from one patch to another is **positional**
 * - point snap, mesh snap, Join, Bridge. Not one of them knows the shape on
 * the far side of the seam, so two patches can meet exactly and still show a
 * crease down the join, because the rows *behind* the seam do not line up.
 * Fixing that by hand, point by point, is the work this replaces.
 *
 * Three things happen, and the soft-selection radius (O, the wheel) says how
 * far back the third one reaches:
 *
 *   meet   every point of the border lands on the point it paired with, and
 *          the rows behind follow by the falloff - otherwise closing a gap
 *          tears the patch off its own second row.
 *   lean   the line leaving the seam is turned to carry on the line that
 *          arrived at it from the other side. Each point keeps its distance
 *          from the seam: it leans, it does not stretch.
 *   ease   that turn fades with distance from the seam, so the crease is not
 *          simply moved one line further in - which is what "with a radius
 *          of blendadapt" asks for, in a radius he already sets.
 *
 * Point snap is the near miss it improves on: that moves a whole selection
 * by **one** correction toward **one** target, which only lands a border of
 * five points when the two patches are already parallel - and that is the
 * case where he did not need help.
 *
 * **It does not Join.** Making two patches into one is Join's verb and it
 * has conditions of its own; this one moves points, and the result line says
 * so rather than leaving him to wonder which happened.
 */
bool USurfEdMode::ContinueSelectedBorder(bool bDryRun, TArray<FVector>* OutTarget)
{
	TArray<FSurfLineRun> Runs;
	int32 Faces = 0;
	int32 Stray = 0;
	GatherLineRuns(GetPointSelection(), Runs, Faces, Stray);

	auto Refuse = [&](const FString& Why, const TCHAR* Tag)
	{
		// A dry run is silent: nothing near is the resting state (S159a).
		if (bDryRun)
		{
			return false;
		}
		if (Inspector)
		{
			Inspector->SetResult(Why);
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(Tag);
		return false;
	};

	if (Faces > 0)
	{
		return Refuse(TEXT("continue: select a border - a run of points along one edge - not faces"), TEXT("continue_faces"));
	}
	if (Runs.Num() == 0)
	{
		return Refuse(TEXT("continue: select the whole of one border - Alt-click picks the line"), TEXT("continue_no_line"));
	}
	if (Runs.Num() > 2)
	{
		return Refuse(FString::Printf(
			TEXT("continue: found %d edges - one border is enough; two says which patch to continue into"), Runs.Num()),
			TEXT("continue_too_many"));
	}

	// The border that moves is the active patch's - the last one picked.
	int32 MovingAt = Runs.IndexOfByPredicate([this](const FSurfLineRun& Run)
	{
		return Run.Group == SelectedPatch.Group.Get() && Run.PatchId == SelectedPatch.PatchId;
	});
	if (MovingAt == INDEX_NONE)
	{
		MovingAt = 0;
	}

	const FSurfLineRun Moving = Runs[MovingAt];
	ASurfGroup* GroupA = Moving.Group;
	UWorld* World = GetWorld();
	const FSurfPatch* PatchA = GroupA ? GroupA->FindPatch(Moving.PatchId) : nullptr;
	if (!PatchA || !World)
	{
		return false;
	}

	bool bColumnA = false;
	bool bFromEndA = false;
	if (Moving.Chain.bClosed || !IsWholeBorder(*PatchA, Moving.Chain, bColumnA, bFromEndA))
	{
		return Refuse(FString::Printf(
			TEXT("continue: the %d points picked are not the whole of one border of the patch - Alt-click picks the whole line"),
			Moving.Chain.Points.Num()), TEXT("continue_not_border"));
	}

	const int32 N = Moving.Chain.Points.Num();
	const int32 Depth = bColumnA ? PatchA->Width : PatchA->Height;

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double Reach = FMath::Max(1.0, Settings ? Settings->PointSnapRadiusCm : 10.0);

	// --- which border it continues into -------------------------------------
	FSurfLineRun Target;
	ASurfGroup* GroupB = nullptr;
	FGuid PatchBId;
	bool bColumnB = false;
	bool bFromEndB = false;

	if (Runs.Num() == 2)
	{
		Target = Runs[1 - MovingAt];
		GroupB = Target.Group;
		const FSurfPatch* Named = GroupB ? GroupB->FindPatch(Target.PatchId) : nullptr;
		if (!Named)
		{
			return false;
		}
		if (GroupB == GroupA && Target.PatchId == Moving.PatchId)
		{
			return Refuse(TEXT("continue: both borders are on the same patch - there is nothing across the seam"),
				TEXT("continue_same_patch"));
		}
		if (Target.Chain.bClosed || !IsWholeBorder(*Named, Target.Chain, bColumnB, bFromEndB))
		{
			return Refuse(TEXT("continue: the other edge is not the whole of a border - the line behind it is what this one carries on"),
				TEXT("continue_target_not_border"));
		}
		if (Target.Chain.Points.Num() != N)
		{
			return Refuse(FString::Printf(
				TEXT("continue: this border has %d points and that one has %d - subdivide, or Bridge makes the piece between them"),
				N, Target.Chain.Points.Num()), TEXT("continue_counts_differ"));
		}
		PatchBId = Target.PatchId;
	}
	else
	{
		// One border picked: the nearest whole border of another patch, by
		// the paired distance, and only within the snap reach - the same
		// number that decides whether a dragged point takes a snap.
		double Best = TNumericLimits<double>::Max();
		int32 NearestWrong = 0;
		double NearestWrongAway = TNumericLimits<double>::Max();

		for (TActorIterator<ASurfGroup> It(World); It; ++It)
		{
			ASurfGroup* Each = *It;
			if (!Each)
			{
				continue;
			}
			Each->ForEachPatch([&](const FSurfPatch& Candidate)
			{
				if (Each == GroupA && Candidate.Id == Moving.PatchId)
				{
					return;
				}

				for (int32 Side = 0; Side < 4; ++Side)
				{
					FSurfLineRun Run;
					BorderRun(Each, Candidate, Side, Run);

					if (Run.Chain.Points.Num() != N)
					{
						// Measured anyway, so a refusal can say the nearest
						// border was there and had the wrong number of points
						// - which is a different problem from nothing near.
						double Worst = 0.0;
						for (int32 I = 0; I < N; ++I)
						{
							double Nearest = TNumericLimits<double>::Max();
							for (const FVector& Point : Run.World)
							{
								Nearest = FMath::Min(Nearest, FVector::Dist(Moving.World[I], Point));
							}
							Worst = FMath::Max(Worst, Nearest);
						}
						if (Worst <= Reach && Worst < NearestWrongAway)
						{
							NearestWrongAway = Worst;
							NearestWrong = Run.Chain.Points.Num();
						}
						continue;
					}

					TArray<int32> Pairing;
					PairIndicesAcross(Moving, Run, Pairing);
					double Worst = 0.0;
					double Sum = 0.0;
					for (int32 I = 0; I < N; ++I)
					{
						const double Away = FVector::Dist(Moving.World[I], Run.World[Pairing[I]]);
						Worst = FMath::Max(Worst, Away);
						Sum += Away;
					}
					if (Worst <= Reach && Sum < Best)
					{
						Best = Sum;
						Target = Run;
						GroupB = Each;
						PatchBId = Candidate.Id;
						bColumnB = Side < 2;
						bFromEndB = (Side % 2) != 0;
					}
				}
			});
		}

		if (!GroupB)
		{
			return Refuse(NearestWrong > 0
				? FString::Printf(TEXT("continue: the nearest border within %.0f cm has %d points and this one has %d - subdivide, or Bridge makes the piece between them"),
					Reach, NearestWrong, N)
				: FString::Printf(TEXT("continue: no border of another patch is within %.0f cm - move it closer, or select that border too"), Reach),
				TEXT("continue_no_target"));
		}
	}

	const FSurfPatch* Across = GroupB ? GroupB->FindPatch(PatchBId) : nullptr;
	if (!Across)
	{
		return false;
	}

	if (bDryRun)
	{
		if (OutTarget)
		{
			*OutTarget = Target.World;
		}
		return true;
	}

	TArray<int32> Pairing;
	PairIndicesAcross(Moving, Target, Pairing);

	const ESurfFalloff Shape = Settings ? Settings->SoftSelectionFalloff : ESurfFalloff::Smooth;
	const double Radius = Settings ? FMath::Max(0.0, Settings->SoftSelectionRadiusCm) : 0.0;

	const FTransform ToWorldA = GroupA->GetActorTransform();
	const FTransform ToLocalA = ToWorldA.Inverse();
	const FTransform ToWorldB = GroupB->GetActorTransform();

	// Which way is inward, on each side: away from the border, one line at a time.
	const FIntPoint InA = bColumnA ? FIntPoint(bFromEndA ? -1 : 1, 0) : FIntPoint(0, bFromEndA ? -1 : 1);
	const FIntPoint InB = bColumnB ? FIntPoint(bFromEndB ? -1 : 1, 0) : FIntPoint(0, bFromEndB ? -1 : 1);

	FScopedTransaction Transaction(LOCTEXT("Continue", "Continue Across The Seam"));

	GroupA->Modify();
	FSurfPatch* Mutable = GroupA->ModifyPatch(Moving.PatchId);
	if (!Mutable)
	{
		Transaction.Cancel();
		return false;
	}

	int32 Leaned = 0;
	int32 Folded = 0;
	double WidestTurn = 0.0;

	for (int32 I = 0; I < N; ++I)
	{
		// The line of points running in from this border point. Each border
		// point has its own, and they never overlap, so a column can be read
		// whole, worked out, and written back before the next one starts.
		TArray<FIntPoint> Where;
		TArray<FVector> Along;
		Where.Reserve(Depth);
		Along.Reserve(Depth);
		for (int32 K = 0; K < Depth; ++K)
		{
			const FIntPoint At = Moving.Chain.Points[I] + InA * K;
			Where.Add(At);
			Along.Add(ToWorldA.TransformPosition(Mutable->At(At.X, At.Y).Pos));
		}

		// --- meet ---------------------------------------------------------
		{
			const FVector Delta = Target.World[Pairing[I]] - Along[0];
			double Walked = 0.0;
			for (int32 K = 0; K < Depth; ++K)
			{
				if (K > 0)
				{
					Walked += FVector::Dist(Along[K], Along[K - 1]);
				}
				const double Weight = Radius > 0.0
					? USurfSettings::Falloff(Shape, Walked / Radius)
					: (K == 0 ? 1.0 : 0.0);
				Along[K] += Delta * Weight;
			}
		}

		// --- lean, and ease back ------------------------------------------
		const FIntPoint BehindAt = Target.Chain.Points[Pairing[I]] + InB;
		const FVector Arrived = ToWorldB.TransformPosition(Across->At(BehindAt.X, BehindAt.Y).Pos);

		const FVector Seam = Along[0];
		const FVector Leaving = (Along[1] - Seam).GetSafeNormal();
		const FVector Arriving = (Seam - Arrived).GetSafeNormal();
		if (!Leaving.IsNearlyZero() && !Arriving.IsNearlyZero())
		{
			const FVector Axis = FVector::CrossProduct(Leaving, Arriving);
			const double Turn = FMath::Acos(FMath::Clamp(FVector::DotProduct(Leaving, Arriving), -1.0, 1.0));

			if (Axis.IsNearlyZero(1.e-8))
			{
				// In line already, or doubled straight back on itself - and a
				// fold has no axis to turn about, so it is counted and left.
				if (Turn > 1.0)
				{
					++Folded;
				}
			}
			else if (Turn > 1.e-5)
			{
				const FVector Turning = Axis.GetSafeNormal();
				const double Reference = FVector::Dist(Along[1], Seam);
				WidestTurn = FMath::Max(WidestTurn, Turn);
				++Leaned;

				for (int32 K = 1; K < Depth; ++K)
				{
					// A rotation about the seam point, so every point keeps
					// its distance from the seam whatever share it takes.
					const double Away = FVector::Dist(Along[K], Seam);
					const double Share = Away <= Reference + KINDA_SMALL_NUMBER
						? 1.0
						: (Radius > 0.0 ? USurfSettings::Falloff(Shape, (Away - Reference) / Radius) : 0.0);
					if (Share <= 0.0)
					{
						continue;
					}
					Along[K] = Seam + FQuat(Turning, Turn * Share).RotateVector(Along[K] - Seam);
				}
			}
		}

		for (int32 K = 0; K < Depth; ++K)
		{
			Mutable->At(Where[K].X, Where[K].Y).Pos = ToLocalA.TransformPosition(Along[K]);
		}
	}

	++Mutable->EditSerial;
	SurfPatchSurfacing::Reapply(*Mutable);
	GroupA->RebuildPreview();
	RefreshSoftSelection();

	// Facing is a separate question from shape, and Flip is its answer: the
	// surface crosses the seam smoothly either way, but it shades wrong.
	const FVector FacingA = SurfPatchSurfacing::AverageNormal(*Mutable);
	const FVector FacingB = SurfPatchSurfacing::AverageNormal(*Across);
	const bool bOpposed = !FacingA.IsNearlyZero() && !FacingB.IsNearlyZero()
		&& FVector::DotProduct(FacingA, FacingB) < 0.0;

	if (Inspector)
	{
		FString Said = FString::Printf(
			TEXT("continued: %d points met, %d lines leaned, easing back over %.0f cm (the soft radius, O)"),
			N, Leaned, Radius);
		if (Folded > 0)
		{
			Said += FString::Printf(TEXT(" - %d doubled back and were left alone"), Folded);
		}
		if (bOpposed)
		{
			Said += TEXT(" - the two face opposite ways; Flip one if the seam shades wrong");
		}
		Said += TEXT(". Still two patches - Join makes them one.");
		Inspector->SetResult(Said);
		Inspector->RefreshSelectionCount();
	}

	SurfSession::Verb(TEXT("continue"), FString::Printf(
		TEXT("points=%d depth=%d leaned=%d folded=%d radius=%.0f turn=%.1f picked=%d"),
		N, Depth, Leaned, Folded, Radius, FMath::RadiansToDegrees(WidestTurn), Runs.Num()));

	if (GEditor)
	{
		RedrawViewports();
	}
	return true;
}

int32 USurfEdMode::ExtrudeSelectedBrushFaces(double ThicknessCm)
{
	if (Selection.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("extrude: pick a side of the primitive first (3)"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_brush_no_face"));
		return 0;
	}

	const double Thickness = FMath::Max(0.1, ThicknessCm);
	TArray<FSurfFaceRef> Lids;
	TArray<FSurfFaceRef> GrownFaces;
	TArray<ASurfGroup*> Touched;
	int32 Grown = 0;

	for (const FSurfFaceRef& Ref : BrushesHeldBySelection())
	{
		ASurfGroup* Group = Ref.Group.Get();
		const FSurfBrush* Source = Group ? Group->FindBrush(Ref.BrushId) : nullptr;
		if (!Source)
		{
			continue;
		}

		// Every side of a primitive picked (S170a): the primitive grows by
		// the amount, every plane carried out - one piece, as the hand
		// meant. A piece off each of a cylinder's twenty-eight sides was
		// "editing faces of a primitive cylinder is all bugged".
		int32 PickedHere = 0;
		for (const FSurfFaceRef& Picked : Selection)
		{
			if (Picked.Group == Ref.Group && Picked.BrushId == Ref.BrushId && Source->Faces.IsValidIndex(Picked.FaceIndex))
			{
				++PickedHere;
			}
		}
		if (PickedHere >= Source->Faces.Num())
		{
			if (!Touched.Contains(Group))
			{
				Group->Modify();
				Touched.Add(Group);
			}
			if (FSurfBrush* Whole = Group->ModifyBrush(Ref.BrushId))
			{
				const TArray<FSurfFace> Was = Whole->Faces;
				for (FSurfFace& Face : Whole->Faces)
				{
					Face.Plane.W += Thickness;
				}
				FSurfHull Check;
				if (!SurfBrushMath::DeriveHull(*Whole, Check))
				{
					Whole->Faces = Was;
				}
				else
				{
					++Whole->EditSerial;
					++Grown;
					for (const FSurfFaceRef& Picked : Selection)
					{
						if (Picked.Group == Ref.Group && Picked.BrushId == Ref.BrushId)
						{
							GrownFaces.Add(Picked);
						}
					}
				}
			}
			continue;
		}

		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(*Source, Hull))
		{
			continue;
		}

		for (const FSurfFaceRef& Picked : Selection)
		{
			if (Picked.Group != Ref.Group || Picked.BrushId != Ref.BrushId
				|| !Source->Faces.IsValidIndex(Picked.FaceIndex))
			{
				continue;
			}

			// The polygon this face actually bounds - not its plane, which
			// is infinite, and not its bounding box, which is bigger than
			// the face on anything tilted (S101a said that about patches).
			const FSurfHullFace* Poly = Hull.Faces.FindByPredicate(
				[&Picked](const FSurfHullFace& Each) { return Each.SourceFaceIndex == Picked.FaceIndex; });
			if (!Poly || Poly->VertexIndices.Num() < 3)
			{
				continue;
			}

			const FSurfFace& From = Source->Faces[Picked.FaceIndex];
			const FVector Normal = FVector(From.Plane.X, From.Plane.Y, From.Plane.Z).GetSafeNormal();
			if (Normal.IsNearlyZero())
			{
				continue;
			}

			FSurfBrush Piece;
			Piece.Id = FGuid::NewGuid();
			Piece.Kind = Source->Kind;
			Piece.bCollision = Source->bCollision;
			Piece.bCastShadow = Source->bCastShadow;

			// Every side of the new piece wears what the face wore, so it
			// reads as the same surface carried out rather than as a
			// different object that happens to touch.
			auto Add = [&Piece, &From](const FVector& Facing, double Out)
			{
				FSurfFace Face = From;
				Face.Plane = FPlane(Facing, Out);
				Piece.Faces.Add(Face);
			};

			// Its floor is the face it stands on, turned the other way; its
			// lid is that same plane carried out along the normal.
			Add(-Normal, -From.Plane.W);
			Add(Normal, From.Plane.W + Thickness);

			// And a side per edge of the polygon. The hull winds a face
			// anticlockwise about its outward normal, so the way out of the
			// polygon at an edge is the edge crossed with that normal.
			const int32 Corners = Poly->VertexIndices.Num();
			for (int32 I = 0; I < Corners; ++I)
			{
				const FVector A = Hull.Vertices[Poly->VertexIndices[I]];
				const FVector B = Hull.Vertices[Poly->VertexIndices[(I + 1) % Corners]];
				const FVector Side = FVector::CrossProduct(B - A, Normal).GetSafeNormal();
				if (Side.IsNearlyZero())
				{
					continue;
				}
				Add(Side, FVector::DotProduct(Side, A));
			}

			FSurfHull Check;
			if (Piece.Faces.Num() < 4 || !SurfBrushMath::DeriveHull(Piece, Check))
			{
				continue;
			}

			if (!Touched.Contains(Group))
			{
				Group->Modify();
				Touched.Add(Group);
			}

			const FGuid MadeId = Piece.Id;
			Group->AddBrush(MoveTemp(Piece));

			FSurfFaceRef Lid;
			Lid.Group = Group;
			Lid.BrushId = MadeId;
			Lid.FaceIndex = 1;
			Lids.Add(Lid);
		}
	}

	if (Lids.Num() == 0 && GrownFaces.Num() == 0)
	{
		if (Inspector)
		{
			Inspector->SetResult(TEXT("extrude: nothing came of it - the picked sides bound no polygon"));
			Inspector->RefreshSelectionCount();
		}
		SurfSession::Friction(TEXT("extrude_brush_nothing_made"));
		return 0;
	}

	for (ASurfGroup* Each : Touched)
	{
		Each->RebuildPreview();
	}

	// The outer faces of what was made: the move that follows pushes them,
	// which is the second half of the gesture.
	SetSubMode(ESurfSubMode::Face);
	Selection = Lids;
	Selection.Append(GrownFaces);
	ActiveFace = Selection.Last();

	if (Inspector)
	{
		const int32 Pieces = Lids.Num();
		Inspector->SetResult(Grown > 0 && Pieces == 0
			? FString::Printf(TEXT("%d primitive%s grown by %.0f cm on every side - every side was picked"), Grown, Grown == 1 ? TEXT("") : TEXT("s"), Thickness)
			: Pieces == 1
				? FString::Printf(TEXT("a new piece off that side, %.0f cm out - move it to pull it further"), Thickness)
				: FString::Printf(TEXT("%d new pieces, one off each side, %.0f cm out - move them to pull them further"), Pieces, Thickness));
		Inspector->RefreshSelectionCount();
		Inspector->PullFromActiveFace();
	}

	SurfSession::Verb(TEXT("extrude_brush"), FString::Printf(TEXT("pieces=%d grown=%d out=%.1f"), Lids.Num(), Grown, Thickness));

	if (GEditor)
	{
		RedrawViewports();
	}
	return Lids.Num() + Grown;
}

#undef LOCTEXT_NAMESPACE
