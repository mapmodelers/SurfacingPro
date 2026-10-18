// Copyright MapModelers. All Rights Reserved.

#include "SurfPatchPrimitives.h"
#include "SurfPatchSurfacing.h"
#include "SurfPatchMath.h"
#include "SurfPatchOps.h"

namespace SurfPatchPrimitives
{
namespace
{
	/**
	 * The ring of control points a bezier arc needs, on the unit circle's
	 * bounding box: axis points at radius, corner points at the box corner.
	 * A quadratic bezier through (1,0) (1,1) (0,1) is the standard quarter-arc
	 * approximation, which is why the primitives land on box corners.
	 */
	void UnitArc(int32 Count, TArray<FVector2D>& Out)
	{
		static const FVector2D Ring[9] =
		{
			FVector2D( 1.0,  0.0), FVector2D( 1.0,  1.0), FVector2D( 0.0,  1.0),
			FVector2D(-1.0,  1.0), FVector2D(-1.0,  0.0), FVector2D(-1.0, -1.0),
			FVector2D( 0.0, -1.0), FVector2D( 1.0, -1.0), FVector2D( 1.0,  0.0)
		};

		Out.Reset();
		for (int32 I = 0; I < Count && I < 9; ++I)
		{
			Out.Add(Ring[I]);
		}
	}

	void SetPoint(FSurfPatch& Patch, int32 X, int32 Y, const FVector& Pos, const FVector2D& UV)
	{
		FSurfControlPoint& Point = Patch.At(X, Y);
		Point.Pos = Pos;
		Point.UV = UV;
		Point.SavedUV = UV;
	}

	FSurfPatch Allocate(ESurfPatchType Type, int32 Width, int32 Height)
	{
		FSurfPatch Patch;
		Patch.EnsureId();
		Patch.Type = Type;
		Patch.Width = Width;
		Patch.Height = Height;
		Patch.Points.SetNum(Width * Height);
		return Patch;
	}

	/** An arc primitive: a ring swept up the box's Z extent. */
	FSurfPatch MakeArc(const FBox& Box, int32 RingCount, bool bCollapseTop)
	{
		FSurfPatch Patch = Allocate(ESurfPatchType::Curve, RingCount, 3);

		TArray<FVector2D> Ring;
		UnitArc(RingCount, Ring);

		const FVector Centre = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		const double Heights[3] = { Box.Min.Z, Centre.Z, Box.Max.Z };

		for (int32 Y = 0; Y < 3; ++Y)
		{
			// The apex of a cone is one point, reached by collapsing the ring.
			const double Shrink = (bCollapseTop && Y == 2) ? 0.0 : 1.0;

			for (int32 X = 0; X < RingCount; ++X)
			{
				const FVector Pos(
					Centre.X + Ring[X].X * Extent.X * Shrink,
					Centre.Y + Ring[X].Y * Extent.Y * Shrink,
					Heights[Y]);

				// U runs around the arc, V up it, so a texture wraps the way
				// you would expect without touching the inspector.
				const FVector2D UV(
					static_cast<double>(X) / FMath::Max(1, RingCount - 1),
					static_cast<double>(Y) * 0.5);

				SetPoint(Patch, X, Y, Pos, UV);
			}
		}

		return Patch;
	}

	FSurfPatch MakeGrid(const FBox& Box, ESurfPatchType Type, int32 Width, int32 Height, const FVector& FaceNormal)
	{
		FSurfPatch Patch = Allocate(Type, Width, Height);

		// The grid lies on the face it was made from, facing the way that face
		// faced. Building it on the box floor regardless put a sheet made from
		// the top of a block underneath the block.
		const FVector Normal = FaceNormal.IsNearlyZero() ? FVector::UpVector : FaceNormal.GetSafeNormal();

		const double AbsX = FMath::Abs(Normal.X);
		const double AbsY = FMath::Abs(Normal.Y);
		const double AbsZ = FMath::Abs(Normal.Z);

		// Axis along the normal, then the two that span the face. The pair is
		// ordered so the grid winds towards the normal rather than away.
		int32 Axis = 2;
		if (AbsX >= AbsY && AbsX >= AbsZ)      { Axis = 0; }
		else if (AbsY >= AbsX && AbsY >= AbsZ) { Axis = 1; }

		const bool bPositive = Normal[Axis] >= 0.0;
		const int32 AxisU = (Axis + 1) % 3;
		const int32 AxisV = (Axis + 2) % 3;

		const double DenomX = FMath::Max(1, Width - 1);
		const double DenomY = FMath::Max(1, Height - 1);

		for (int32 Y = 0; Y < Height; ++Y)
		{
			const double TY = static_cast<double>(Y) / DenomY;
			for (int32 X = 0; X < Width; ++X)
			{
				const double TX = static_cast<double>(X) / DenomX;

				FVector Pos = FVector::ZeroVector;
				Pos[Axis]  = bPositive ? Box.Max[Axis] : Box.Min[Axis];
				Pos[AxisU] = FMath::Lerp(Box.Min[AxisU], Box.Max[AxisU], bPositive ? TX : 1.0 - TX);
				Pos[AxisV] = FMath::Lerp(Box.Min[AxisV], Box.Max[AxisV], TY);

				SetPoint(Patch, X, Y, Pos, FVector2D(TX, TY));
			}
		}

		return Patch;
	}
}

void GetDimensions(ESurfPrimitiveKind Kind, int32& OutWidth, int32& OutHeight, int32 RequestedWidth, int32 RequestedHeight)
{
	switch (Kind)
	{
	case ESurfPrimitiveKind::Cylinder:
	case ESurfPrimitiveKind::Cone:
		OutWidth = 9; OutHeight = 3; break;
	case ESurfPrimitiveKind::Endcap:
		OutWidth = 5; OutHeight = 3; break;
	case ESurfPrimitiveKind::Bevel:
		OutWidth = 3; OutHeight = 3; break;
	case ESurfPrimitiveKind::Sheet:
		OutWidth = FMath::Clamp(RequestedWidth, FSurfPatch::MinSheetDim, FSurfPatch::MaxSheetDim);
		OutHeight = FMath::Clamp(RequestedHeight, FSurfPatch::MinSheetDim, FSurfPatch::MaxSheetDim);
		break;
	default:
		// Curves need odd dimensions; round up rather than silently refusing.
		OutWidth = FMath::Clamp(RequestedWidth | 1, FSurfPatch::MinCurveDim, FSurfPatch::MaxCurveDim);
		OutHeight = FMath::Clamp(RequestedHeight | 1, FSurfPatch::MinCurveDim, FSurfPatch::MaxCurveDim);
		break;
	}
}

FSurfPatch Make(ESurfPrimitiveKind Kind, const FBox& Box, int32 Width, int32 Height, const FVector& FaceNormal)
{
	int32 W = 3, H = 3;
	GetDimensions(Kind, W, H, Width, Height);

	FSurfPatch Patch;

	switch (Kind)
	{
	case ESurfPrimitiveKind::Cylinder: Patch = MakeArc(Box, 9, false); break;
	case ESurfPrimitiveKind::Cone:     Patch = MakeArc(Box, 9, true);  break;
	case ESurfPrimitiveKind::Endcap:   Patch = MakeArc(Box, 5, false); break;
	case ESurfPrimitiveKind::Bevel:    Patch = MakeArc(Box, 3, false); break;
	case ESurfPrimitiveKind::Sheet:    Patch = MakeGrid(Box, ESurfPatchType::Sheet, W, H, FaceNormal); break;
	default:                           Patch = MakeGrid(Box, ESurfPatchType::Curve, W, H, FaceNormal); break;
	}

	// A new patch arrives surfaced. Parametric UVs would stretch the moment
	// the shape is anything but a rectangle, and a curved primitive never is.
	SurfPatchSurfacing::Reapply(Patch);

	return Patch;
}

// --- Row and column editing ------------------------------------------------

namespace
{
	FSurfControlPoint Average(const FSurfControlPoint& A, const FSurfControlPoint& B)
	{
		FSurfControlPoint Out = A;
		Out.Pos = (A.Pos + B.Pos) * 0.5;
		Out.UV = (A.UV + B.UV) * 0.5;
		Out.SavedUV = Out.UV;
		Out.Weight = (A.Weight + B.Weight) * 0.5f;
		return Out;
	}

	/** Curves must keep odd dimensions, so they gain and lose two at a time. */
	int32 StepFor(const FSurfPatch& Patch)
	{
		return Patch.Type == ESurfPatchType::Curve ? 2 : 1;
	}

	bool Rebuild(FSurfPatch& Patch, int32 NewWidth, int32 NewHeight, TFunctionRef<FSurfControlPoint(int32, int32)> Source)
	{
		TArray<FSurfControlPoint> Points;
		Points.SetNum(NewWidth * NewHeight);

		for (int32 Y = 0; Y < NewHeight; ++Y)
		{
			for (int32 X = 0; X < NewWidth; ++X)
			{
				Points[Y * NewWidth + X] = Source(X, Y);
			}
		}

		Patch.Points = MoveTemp(Points);
		Patch.Width = NewWidth;
		Patch.Height = NewHeight;
		++Patch.EditSerial;
		return true;
	}
}

bool InsertColumn(FSurfPatch& Patch, int32 AtColumn)
{
	const int32 Step = StepFor(Patch);
	const int32 NewWidth = Patch.Width + Step;
	const int32 Max = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;

	if (NewWidth > Max || !Patch.HasValidDimensions())
	{
		return false;
	}

	AtColumn = FMath::Clamp(AtColumn, 0, Patch.Width - 2);

	const FSurfPatch Old = Patch;
	Rebuild(Patch, NewWidth, Patch.Height, [&Old, AtColumn, Step](int32 X, int32 Y) -> FSurfControlPoint
	{
		if (X <= AtColumn)
		{
			return Old.At(X, Y);
		}
		if (X > AtColumn + Step)
		{
			return Old.At(X - Step, Y);
		}
		// The inserted span interpolates the pair it was placed between, so
		// the surface does not move when resolution is added.
		return Average(Old.At(AtColumn, Y), Old.At(AtColumn + 1, Y));
	});

	// The UV origin stays on its point.
	if (Patch.UVOriginX > AtColumn)
	{
		Patch.UVOriginX += Step;
	}

	// A hard line stays on its line (S189a, backlog 21): the indices past
	// the cut move along by what was put in.
	for (int32& Column : Patch.HardColumns)
	{
		if (Column > AtColumn)
		{
			Column += Step;
		}
	}

	// The even mapping's ruler: the new lines take the places between the
	// pair they were put between, so nothing already there moves.
	if (Old.GridU.Num() == Old.Width && Old.GridU.IsValidIndex(AtColumn + 1))
	{
		Patch.GridU = Old.GridU;
		for (int32 I = 0; I < Step; ++I)
		{
			const float Place = FMath::Lerp(Old.GridU[AtColumn], Old.GridU[AtColumn + 1], static_cast<float>(I + 1) / (Step + 1));
			Patch.GridU.Insert(Place, AtColumn + 1 + I);
		}
	}
	return true;
}

bool InsertRow(FSurfPatch& Patch, int32 AtRow)
{
	const int32 Step = StepFor(Patch);
	const int32 NewHeight = Patch.Height + Step;
	const int32 Max = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;

	if (NewHeight > Max || !Patch.HasValidDimensions())
	{
		return false;
	}

	AtRow = FMath::Clamp(AtRow, 0, Patch.Height - 2);

	const FSurfPatch Old = Patch;
	Rebuild(Patch, Patch.Width, NewHeight, [&Old, AtRow, Step](int32 X, int32 Y) -> FSurfControlPoint
	{
		if (Y <= AtRow)
		{
			return Old.At(X, Y);
		}
		if (Y > AtRow + Step)
		{
			return Old.At(X, Y - Step);
		}
		return Average(Old.At(X, AtRow), Old.At(X, AtRow + 1));
	});

	if (Patch.UVOriginY > AtRow)
	{
		Patch.UVOriginY += Step;
	}

	for (int32& Row : Patch.HardRows)
	{
		if (Row > AtRow)
		{
			Row += Step;
		}
	}

	if (Old.GridV.Num() == Old.Height && Old.GridV.IsValidIndex(AtRow + 1))
	{
		Patch.GridV = Old.GridV;
		for (int32 I = 0; I < Step; ++I)
		{
			const float Place = FMath::Lerp(Old.GridV[AtRow], Old.GridV[AtRow + 1], static_cast<float>(I + 1) / (Step + 1));
			Patch.GridV.Insert(Place, AtRow + 1 + I);
		}
	}
	return true;
}

bool RemoveColumn(FSurfPatch& Patch, int32 AtColumn)
{
	const int32 Step = StepFor(Patch);
	const int32 NewWidth = Patch.Width - Step;
	const int32 Min = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MinCurveDim : FSurfPatch::MinSheetDim;

	if (NewWidth < Min || !Patch.HasValidDimensions())
	{
		return false;
	}

	AtColumn = FMath::Clamp(AtColumn, 0, Patch.Width - Step - 1);

	const FSurfPatch Old = Patch;
	Rebuild(Patch, NewWidth, Patch.Height, [&Old, AtColumn, Step](int32 X, int32 Y) -> FSurfControlPoint
	{
		return X < AtColumn ? Old.At(X, Y) : Old.At(X + Step, Y);
	});

	// The UV origin stays on its point; if its point went, the line that
	// took its place measures from now on.
	if (Patch.UVOriginX >= AtColumn + Step)
	{
		Patch.UVOriginX -= Step;
	}
	else if (Patch.UVOriginX > AtColumn)
	{
		Patch.UVOriginX = AtColumn;
	}

	// A hard line that went goes; the ones past it move back (S189a).
	Patch.HardColumns.RemoveAll([AtColumn, Step](int32 Column) { return Column >= AtColumn && Column < AtColumn + Step; });
	for (int32& Column : Patch.HardColumns)
	{
		if (Column >= AtColumn + Step)
		{
			Column -= Step;
		}
	}
	return true;
}

bool RemoveRow(FSurfPatch& Patch, int32 AtRow)
{
	const int32 Step = StepFor(Patch);
	const int32 NewHeight = Patch.Height - Step;
	const int32 Min = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MinCurveDim : FSurfPatch::MinSheetDim;

	if (NewHeight < Min || !Patch.HasValidDimensions())
	{
		return false;
	}

	AtRow = FMath::Clamp(AtRow, 0, Patch.Height - Step - 1);

	const FSurfPatch Old = Patch;
	Rebuild(Patch, Patch.Width, NewHeight, [&Old, AtRow, Step](int32 X, int32 Y) -> FSurfControlPoint
	{
		return Y < AtRow ? Old.At(X, Y) : Old.At(X, Y + Step);
	});

	if (Patch.UVOriginY >= AtRow + Step)
	{
		Patch.UVOriginY -= Step;
	}
	else if (Patch.UVOriginY > AtRow)
	{
		Patch.UVOriginY = AtRow;
	}

	Patch.HardRows.RemoveAll([AtRow, Step](int32 Row) { return Row >= AtRow && Row < AtRow + Step; });
	for (int32& Row : Patch.HardRows)
	{
		if (Row >= AtRow + Step)
		{
			Row -= Step;
		}
	}
	return true;
}

namespace
{
	/**
	 * Move the interior points of one line so they sit at even arc-length
	 * intervals ALONG the line as it is, not along the chord between its
	 * ends. The chord is what the first version used, and on a closed row -
	 * a tube, where the two ends are the same point - every interior point
	 * landed on that point and the patch became a line.
	 */
	void RespaceLine(TArray<FVector>& Points)
	{
		const int32 Count = Points.Num();
		if (Count < 3)
		{
			return;
		}

		TArray<double> Along;
		Along.SetNumZeroed(Count);
		for (int32 I = 1; I < Count; ++I)
		{
			Along[I] = Along[I - 1] + FVector::Dist(Points[I], Points[I - 1]);
		}

		const double Total = Along.Last();
		if (Total < KINDA_SMALL_NUMBER)
		{
			return;
		}

		const TArray<FVector> Original = Points;

		for (int32 I = 1; I < Count - 1; ++I)
		{
			const double Target = Total * static_cast<double>(I) / (Count - 1);

			// The segment of the original line the target distance falls in.
			int32 Segment = 0;
			while (Segment + 2 < Count && Along[Segment + 1] < Target)
			{
				++Segment;
			}

			const double SegmentLength = Along[Segment + 1] - Along[Segment];
			const double T = SegmentLength > KINDA_SMALL_NUMBER
				? (Target - Along[Segment]) / SegmentLength
				: 0.0;

			Points[I] = FMath::Lerp(Original[Segment], Original[Segment + 1], T);
		}
	}
}

void Respace(FSurfPatch& Patch)
{
	if (!Patch.HasValidDimensions())
	{
		return;
	}

	// Rows first, then columns, each along its own length. Endpoints never
	// move, so the patch keeps its footprint and a closed line stays closed.
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		TArray<FVector> Row;
		Row.Reserve(Patch.Width);
		for (int32 X = 0; X < Patch.Width; ++X)
		{
			Row.Add(Patch.At(X, Y).Pos);
		}

		RespaceLine(Row);

		for (int32 X = 0; X < Patch.Width; ++X)
		{
			Patch.At(X, Y).Pos = Row[X];
		}
	}

	for (int32 X = 0; X < Patch.Width; ++X)
	{
		TArray<FVector> Column;
		Column.Reserve(Patch.Height);
		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			Column.Add(Patch.At(X, Y).Pos);
		}

		RespaceLine(Column);

		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			Patch.At(X, Y).Pos = Column[Y];
		}
	}

	++Patch.EditSerial;
}

bool ConvertToSheet(const FSurfPatch& Patch, FSurfPatch& Out, int32* OutSubdivisionUsed)
{
	if (!Patch.HasValidDimensions())
	{
		return false;
	}

	if (Patch.Type == ESurfPatchType::Sheet)
	{
		// Already vertices. Saying so beats quietly doing nothing.
		return false;
	}

	// Find the finest subdivision whose vertex grid still fits a sheet. The
	// cap is real - a sheet's points are its vertices, and they all live in
	// one chunk, which is what an undo step costs.
	FSurfPatch Source = Patch;
	int32 Subdivision = FMath::Clamp(FMath::Max(Patch.SubdivU, Patch.SubdivV), FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);

	for (; Subdivision >= FSurfPatch::MinSubdiv; --Subdivision)
	{
		Source.SubdivU = Subdivision;
		Source.SubdivV = Subdivision;

		if (SurfPatchMath::TessellatedNumU(Source) <= FSurfPatch::MaxSheetDim
			&& SurfPatchMath::TessellatedNumV(Source) <= FSurfPatch::MaxSheetDim)
		{
			break;
		}
	}

	if (Subdivision < FSurfPatch::MinSubdiv)
	{
		return false;
	}

	FSurfTessellation Tess;
	if (!SurfPatchMath::Tessellate(Source, Tess) || Tess.NumU < 2 || Tess.NumV < 2)
	{
		return false;
	}

	Out = Patch;
	Out.Id.Invalidate();
	Out.EnsureId();
	Out.Type = ESurfPatchType::Sheet;
	Out.Width = Tess.NumU;
	Out.Height = Tess.NumV;
	Out.Points.Reset(Tess.NumU * Tess.NumV);

	// The UV origin is a control point; on the vertex grid it is that
	// point's vertex, one segment of Subdivision vertices per pair of lines.
	Out.UVOriginX = FMath::Clamp((Patch.UVOriginX / 2) * Subdivision, 0, Tess.NumU - 1);
	Out.UVOriginY = FMath::Clamp((Patch.UVOriginY / 2) * Subdivision, 0, Tess.NumV - 1);

	for (int32 I = 0; I < Tess.NumU * Tess.NumV; ++I)
	{
		FSurfControlPoint Point(Tess.Positions[I]);
		Point.UV = Tess.UVs.IsValidIndex(I) ? Tess.UVs[I] : FVector2D::ZeroVector;
		Point.SavedUV = Point.UV;
		Out.Points.Add(Point);
	}

	// A sheet's points are its vertices, so subdivision no longer applies.
	Out.SubdivU = 1;
	Out.SubdivV = 1;
	++Out.EditSerial;

	// Keep the mapping rule rather than the UVs it happened to produce: the
	// new grid is denser, and the rule re-derives correctly for it.
	SurfPatchSurfacing::Reapply(Out);

	if (OutSubdivisionUsed)
	{
		*OutSubdivisionUsed = Subdivision;
	}

	return true;
}

bool ExtrudeEdge(FSurfPatch& Patch, bool bColumn, bool bFromEnd, int32 Count, bool bInPlace)
{
	if (!Patch.HasValidDimensions() || Count < 1)
	{
		return false;
	}

	// A curve has to stay odd on both axes, so it grows two lines at a time.
	const int32 Step = Patch.Type == ESurfPatchType::Curve ? 2 : 1;
	const int32 Added = Count * Step;

	const int32 Along = bColumn ? Patch.Width : Patch.Height;
	const int32 Limit = Patch.Type == ESurfPatchType::Curve ? FSurfPatch::MaxCurveDim : FSurfPatch::MaxSheetDim;

	if (Along + Added > Limit || Along < 2)
	{
		return false;
	}

	const int32 Across = bColumn ? Patch.Height : Patch.Width;

	auto Read = [&Patch, bColumn](int32 A, int32 B) -> const FSurfControlPoint&
	{
		return bColumn ? Patch.At(A, B) : Patch.At(B, A);
	};

	FSurfPatch Out = Patch;
	Out.Width  = bColumn ? Along + Added : Patch.Width;
	Out.Height = bColumn ? Patch.Height : Along + Added;
	Out.Points.SetNum(Out.Width * Out.Height);

	auto Write = [&Out, bColumn](int32 A, int32 B) -> FSurfControlPoint&
	{
		return bColumn ? Out.At(A, B) : Out.At(B, A);
	};

	for (int32 B = 0; B < Across; ++B)
	{
		// The step to continue by: the gap between the last two lines on the
		// side being extruded, so the new lines carry on at the same spacing
		// rather than piling up on the edge.
		const FSurfControlPoint& Edge  = Read(bFromEnd ? Along - 1 : 0, B);
		const FSurfControlPoint& Inner = Read(bFromEnd ? Along - 2 : 1, B);
		const FVector Delta = bInPlace ? FVector::ZeroVector : (Edge.Pos - Inner.Pos);

		// Frozen (Manual) UVs are not re-measured, so a new line that copied
		// the edge's would sit on it in texture space and pull the texture
		// across the new cell. It carries on by the same step as the last two
		// lines, which is what re-measuring would have given.
		const bool bFrozen = Patch.Mapping == ESurfPatchMapping::Manual;
		const FVector2D UVStep = bFrozen ? (Edge.UV - Inner.UV) : FVector2D::ZeroVector;
		const FVector2D SavedStep = bFrozen ? (Edge.SavedUV - Inner.SavedUV) : FVector2D::ZeroVector;

		if (bFromEnd)
		{
			for (int32 A = 0; A < Along; ++A)
			{
				Write(A, B) = Read(A, B);
			}
			for (int32 I = 1; I <= Added; ++I)
			{
				FSurfControlPoint Point = Edge;
				Point.Pos = Edge.Pos + Delta * I;
				Point.UV = Edge.UV + UVStep * I;
				Point.SavedUV = Edge.SavedUV + SavedStep * I;
				Write(Along - 1 + I, B) = Point;
			}
		}
		else
		{
			for (int32 I = 1; I <= Added; ++I)
			{
				FSurfControlPoint Point = Edge;
				Point.Pos = Edge.Pos + Delta * I;
				Point.UV = Edge.UV + UVStep * I;
				Point.SavedUV = Edge.SavedUV + SavedStep * I;
				Write(Added - I, B) = Point;
			}
			for (int32 A = 0; A < Along; ++A)
			{
				Write(Added + A, B) = Read(A, B);
			}
		}
	}

	// Lines grown on the low side push the grid's corner out, not the point
	// the UVs are measured from: the old points keep their surfacing, and
	// the new lines measure backwards from it.
	if (!bFromEnd)
	{
		if (bColumn) { Out.UVOriginX += Added; }
		else         { Out.UVOriginY += Added; }
	}

	++Out.EditSerial;
	Patch = MoveTemp(Out);
	return true;
}

bool Split(const FSurfPatch& Patch, bool bColumn, int32 Index, FSurfPatch& OutA, FSurfPatch& OutB)
{
	if (!Patch.HasValidDimensions())
	{
		return false;
	}

	const int32 Along = bColumn ? Patch.Width : Patch.Height;
	const int32 Across = bColumn ? Patch.Height : Patch.Width;

	// A cut on the border leaves nothing on one side.
	if (Index <= 0 || Index >= Along - 1)
	{
		return false;
	}

	// Both halves include the cut line, so they share it.
	const int32 CountA = Index + 1;
	const int32 CountB = Along - Index;

	// A curve has to stay odd on both axes; an even half is not a legal curve.
	if (Patch.Type == ESurfPatchType::Curve && ((CountA % 2) == 0 || (CountB % 2) == 0))
	{
		return false;
	}

	if (CountA < 2 || CountB < 2)
	{
		return false;
	}

	auto Build = [&Patch, bColumn, Across](int32 First, int32 Count, FSurfPatch& Out)
	{
		Out = Patch;
		Out.Id.Invalidate();
		Out.EnsureId();
		Out.Width  = bColumn ? Count : Patch.Width;
		Out.Height = bColumn ? Patch.Height : Count;
		Out.Points.SetNum(Out.Width * Out.Height);

		for (int32 B = 0; B < Across; ++B)
		{
			for (int32 A = 0; A < Count; ++A)
			{
				const FSurfControlPoint& Source = bColumn ? Patch.At(First + A, B) : Patch.At(B, First + A);

				if (bColumn) { Out.At(A, B) = Source; }
				else         { Out.At(B, A) = Source; }
			}
		}

		// The half that keeps the UV origin keeps its surfacing as it was.
		// The other half measures from its own near line, and starts there
		// at the UV the whole patch had on it, so the cut shows no seam.
		int32& Origin = bColumn ? Out.UVOriginX : Out.UVOriginY;
		const int32 Moved = Origin - First;
		if (Moved >= 0 && Moved < Count)
		{
			Origin = Moved;
		}
		else
		{
			const int32 Near = Moved < 0 ? 0 : Count - 1;
			Origin = Near;
			if (SurfPatchSurfacing::FollowsTheNet(Out.Mapping))
			{
				Out.UVShift = bColumn
					? Patch.At(First + Near, FMath::Clamp(Out.UVOriginY, 0, Patch.Height - 1)).UV
					: Patch.At(FMath::Clamp(Out.UVOriginX, 0, Patch.Width - 1), First + Near).UV;
			}
		}

		++Out.EditSerial;
	};

	Build(0, CountA, OutA);
	Build(Index, CountB, OutB);
	return true;
}

bool Solidify(const FSurfPatch& Patch, double ThicknessCm, TArray<FSurfPatch>& Out, const FSurfSolidifyOptions& Options)
{
	if (!Patch.HasValidDimensions() || FMath::IsNearlyZero(ThicknessCm))
	{
		return false;
	}

	Out.Reset();

	/**
	 * A thickness dragged the other way is not a different verb (S84a).
	 *
	 * The knob goes negative as easily as positive - his was at -157 cm -
	 * and the sign used to land in FrontAlong/BackAlong while Front and
	 * Back kept their names. The shell at the +normal side was then the one
	 * called Back, which is the one turned round, so **both caps faced the
	 * wrong way**: from outside, half the solid was missing and the last
	 * face made looked flipped. Both were reported, and they were the same
	 * bug.
	 *
	 * The sign belongs on the side, not on the offsets: pulling 157 cm
	 * behind is pulling 157 cm in front from the other face. Fold it in
	 * once, here, and everything below works on a solid whose front really
	 * is in front.
	 */
	double Thickness = ThicknessCm;
	int32 SideWanted = FMath::Clamp(Options.Side, -1, 1);
	if (Thickness < 0.0)
	{
		Thickness = -Thickness;
		SideWanted = -SideWanted;
	}

	// Where the two faces go, along each point's normal: behind the surface
	// (the front stays put), in front of it, or half each way.
	const double FrontAlong = SideWanted > 0 ? Thickness : (SideWanted < 0 ? 0.0 : Thickness * 0.5);
	const double BackAlong  = FrontAlong - Thickness;

	// The push at each point: its averaged normal, and with "even" the
	// length that keeps the shell as thick at a corner as on a flat run -
	// the averaged normal at a fold leans between the two faces, so a push
	// of t along it leaves the faces less than t apart.
	auto PushAt = [&Patch, &Options](int32 X, int32 Y) -> FVector
	{
		const FVector Normal = SurfPatchOps::PointNormal(Patch, X, Y);
		if (!Options.bEven)
		{
			return Normal;
		}

		// The cells around the point: the least the normal agrees with any
		// of them says how far the fold leans.
		double Least = 1.0;
		for (int32 DY = -1; DY <= 0; ++DY)
		{
			for (int32 DX = -1; DX <= 0; ++DX)
			{
				const int32 X0 = X + DX;
				const int32 Y0 = Y + DY;
				if (X0 < 0 || Y0 < 0 || X0 + 1 >= Patch.Width || Y0 + 1 >= Patch.Height)
				{
					continue;
				}
				const FVector& P00 = Patch.At(X0, Y0).Pos;
				const FVector Cell = FVector::CrossProduct(Patch.At(X0 + 1, Y0).Pos - P00, Patch.At(X0, Y0 + 1).Pos - P00).GetSafeNormal();
				if (!Cell.IsNearlyZero())
				{
					Least = FMath::Min(Least, FMath::Abs(FVector::DotProduct(Normal, Cell)));
				}
			}
		}
		return Normal / FMath::Max(0.25, Least);
	};

	FSurfPatch Front = Patch;
	FSurfPatch Back = Patch;
	Back.Id.Invalidate();
	Back.EnsureId();
	Back.bInvert = !Back.bInvert;

	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		for (int32 X = 0; X < Patch.Width; ++X)
		{
			const FVector Push = PushAt(X, Y);
			Front.At(X, Y).Pos = Patch.At(X, Y).Pos + Push * FrontAlong;
			Back.At(X, Y).Pos  = Patch.At(X, Y).Pos + Push * BackAlong;
		}
	}
	if (!FMath::IsNearlyZero(FrontAlong))
	{
		++Front.EditSerial;
	}

	Out.Add(Front);
	Out.Add(Back);

	if (!Options.bRim)
	{
		for (FSurfPatch& Made : Out)
		{
			Made.EnsureId();
			SurfPatchSurfacing::Reapply(Made);
		}
		return true;
	}

	// One skirt per border, two points wide: front edge to back edge. Built
	// as sheets whatever the source was, because two lines is not a legal
	// curve and a skirt has no curvature of its own to preserve.
	// Where the whole thing is, so a skirt can tell outward from inward.
	FVector Centre = FVector::ZeroVector;
	for (const FSurfControlPoint& Point : Front.Points)
	{
		Centre += Point.Pos;
	}
	for (const FSurfControlPoint& Point : Back.Points)
	{
		Centre += Point.Pos;
	}
	Centre /= FMath::Max(1, Front.Points.Num() + Back.Points.Num());

	auto AddSkirt = [&Out, &Patch, &Front, &Back, &Centre](bool bColumn, int32 Line)
	{
		const int32 Across = bColumn ? Patch.Height : Patch.Width;

		FSurfPatch Skirt;
		Skirt.EnsureId();
		Skirt.Type = ESurfPatchType::Sheet;
		Skirt.Width = 2;
		Skirt.Height = Across;
		Skirt.Material = Patch.Material;
		Skirt.Scale = Patch.Scale;
		Skirt.Points.SetNum(2 * Across);

		for (int32 B = 0; B < Across; ++B)
		{
			const FSurfControlPoint& Near = bColumn ? Front.At(Line, B) : Front.At(B, Line);
			const FSurfControlPoint& Rear = bColumn ? Back.At(Line, B)  : Back.At(B, Line);

			Skirt.At(0, B) = Near;
			Skirt.At(1, B) = Rear;
		}

		// A skirt of a frozen (Manual) patch has no UVs of its own to keep:
		// both its lines would carry the front line's, and the texture would
		// be pulled across its whole height. It measures its own, continuing
		// from the front's UV where the two meet.
		Skirt.Mapping = Patch.Mapping == ESurfPatchMapping::Manual ? ESurfPatchMapping::Natural : Patch.Mapping;
		if (SurfPatchSurfacing::FollowsTheNet(Skirt.Mapping))
		{
			Skirt.UVShift = Skirt.At(0, 0).UV;
		}

		// Two of the four skirts come out wound inwards, because opposite
		// borders run in opposite directions around the patch. Left alone they
		// are culled and the solid looks like it is missing a side - which is
		// exactly how it was reported. Each one is turned to face away from
		// the middle.
		FVector SkirtCentre = FVector::ZeroVector;
		for (const FSurfControlPoint& Point : Skirt.Points)
		{
			SkirtCentre += Point.Pos;
		}
		SkirtCentre /= FMath::Max(1, Skirt.Points.Num());

		FVector Outward = SkirtCentre - Centre;
		const FVector Normal = SurfPatchSurfacing::AverageNormal(Skirt);

		// A skirt on a patch so thin that its border sits on the middle has
		// no "away" to measure - the vector is noise, and the coin-flip it
		// decided left a wall culled. The border's own direction away from
		// the opposite border is the answer then (S84a).
		if (Outward.IsNearlyZero())
		{
			const FSurfControlPoint& Here = bColumn ? Front.At(Line, 0) : Front.At(0, Line);
			const int32 Other = bColumn
				? (Line == 0 ? Patch.Width - 1 : 0)
				: (Line == 0 ? Patch.Height - 1 : 0);
			const FSurfControlPoint& Across2 = bColumn ? Front.At(Other, 0) : Front.At(0, Other);
			Outward = Here.Pos - Across2.Pos;
		}

		if (!Outward.IsNearlyZero() && !Normal.IsNearlyZero()
			&& FVector::DotProduct(Normal, Outward.GetSafeNormal()) < 0.0)
		{
			Skirt.bInvert = !Skirt.bInvert;
		}

		Out.Add(MoveTemp(Skirt));
	};

	AddSkirt(true,  0);
	AddSkirt(true,  Patch.Width - 1);
	AddSkirt(false, 0);
	AddSkirt(false, Patch.Height - 1);

	for (FSurfPatch& Made : Out)
	{
		Made.EnsureId();
		SurfPatchSurfacing::Reapply(Made);
	}

	return true;
}

namespace
{
	/** X and Y swapped. A patch has no preferred axis; the caller's does. */
	FSurfPatch Transposed(const FSurfPatch& In)
	{
		FSurfPatch Out = In;
		Out.Width = In.Height;
		Out.Height = In.Width;
		Out.Points.SetNum(Out.Width * Out.Height);
		Out.UVOriginX = In.UVOriginY;
		Out.UVOriginY = In.UVOriginX;

		for (int32 Y = 0; Y < In.Height; ++Y)
		{
			for (int32 X = 0; X < In.Width; ++X)
			{
				Out.At(Y, X) = In.At(X, Y);
			}
		}

		return Out;
	}

	FSurfPatch ReversedColumns(const FSurfPatch& In)
	{
		FSurfPatch Out = In;
		Out.UVOriginX = FMath::Clamp(In.Width - 1 - In.UVOriginX, 0, In.Width - 1);
		for (int32 Y = 0; Y < In.Height; ++Y)
		{
			for (int32 X = 0; X < In.Width; ++X)
			{
				Out.At(X, Y) = In.At(In.Width - 1 - X, Y);
			}
		}
		return Out;
	}

	FSurfPatch ReversedRows(const FSurfPatch& In)
	{
		FSurfPatch Out = In;
		Out.UVOriginY = FMath::Clamp(In.Height - 1 - In.UVOriginY, 0, In.Height - 1);
		for (int32 Y = 0; Y < In.Height; ++Y)
		{
			for (int32 X = 0; X < In.Width; ++X)
			{
				Out.At(X, Y) = In.At(X, In.Height - 1 - Y);
			}
		}
		return Out;
	}

	/**
	 * Every way a patch can be presented with one of its four edges as its
	 * last column, in the eight orientations a grid has. Joining then only
	 * ever has to handle one case.
	 */
	void Orientations(const FSurfPatch& In, TArray<FSurfPatch>& Out)
	{
		const FSurfPatch Turned = Transposed(In);

		Out.Add(In);
		Out.Add(ReversedColumns(In));
		Out.Add(ReversedRows(In));
		Out.Add(ReversedRows(ReversedColumns(In)));
		Out.Add(Turned);
		Out.Add(ReversedColumns(Turned));
		Out.Add(ReversedRows(Turned));
		Out.Add(ReversedRows(ReversedColumns(Turned)));
	}

	/** Does A's last column lie on B's first, point for point? */
	bool ColumnsMeet(const FSurfPatch& A, const FSurfPatch& B, double ToleranceCm)
	{
		if (A.Height != B.Height)
		{
			return false;
		}

		const double ToleranceSquared = ToleranceCm * ToleranceCm;

		for (int32 Y = 0; Y < A.Height; ++Y)
		{
			if (FVector::DistSquared(A.At(A.Width - 1, Y).Pos, B.At(0, Y).Pos) > ToleranceSquared)
			{
				return false;
			}
		}

		return true;
	}
}

bool Join(FSurfPatch& Out, const FSurfPatch& A, const FSurfPatch& B, double ToleranceCm)
{
	if (!A.HasValidDimensions() || !B.HasValidDimensions() || A.Type != B.Type || ToleranceCm < 0.0)
	{
		return false;
	}

	TArray<FSurfPatch> Lefts;
	TArray<FSurfPatch> Rights;
	Orientations(A, Lefts);
	Orientations(B, Rights);

	// Which of the eight orientations turn the winding over (S208a): one
	// reversal or the transpose alone does; two undo each other; all three
	// do again. Indexed as Orientations() lists them.
	static const bool Flips[8] = { false, true, true, false, true, false, false, true };

	for (int32 LeftAt = 0; LeftAt < Lefts.Num(); ++LeftAt)
	{
		const FSurfPatch& Left = Lefts[LeftAt];
		for (const FSurfPatch& Right : Rights)
		{
			if (!ColumnsMeet(Left, Right, ToleranceCm))
			{
				continue;
			}

			const int32 Width = Left.Width + Right.Width - 1;

			// A curve has to stay odd on both axes. Odd + odd - 1 is odd, so
			// two curves always fit; the check is here for the day one of
			// them is not what it claims to be.
			if (A.Type == ESurfPatchType::Curve)
			{
				if ((Width % 2) == 0 || Width > FSurfPatch::MaxCurveDim || Left.Height > FSurfPatch::MaxCurveDim)
				{
					continue;
				}
			}
			else if (Width > FSurfPatch::MaxSheetDim)
			{
				continue;
			}

			FSurfPatch Joined = Left;
			Joined.Width = Width;
			Joined.Height = Left.Height;
			Joined.Points.SetNum(Joined.Width * Joined.Height);

			for (int32 Y = 0; Y < Joined.Height; ++Y)
			{
				for (int32 X = 0; X < Left.Width; ++X)
				{
					Joined.At(X, Y) = Left.At(X, Y);
				}

				// The shared line is kept once, from the left-hand patch.
				for (int32 X = 1; X < Right.Width; ++X)
				{
					Joined.At(Left.Width - 1 + X, Y) = Right.At(X, Y);
				}
			}

			// The orientation that met may run the other way round - reversed
			// columns turn the winding over - so the result would face the
			// other way from the patch it grew out of. It faces the way A did:
			// known from the orientation itself (S208a), not from an average
			// normal, which says nothing on a patch bent round into a tube
			// ("just Join and it has flipped too").
			if (Flips[LeftAt])
			{
				Joined.bInvert = !Joined.bInvert;
			}

			++Joined.EditSerial;
			Out = MoveTemp(Joined);
			return true;
		}
	}

	return false;
}

} // namespace SurfPatchPrimitives
