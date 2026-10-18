// Copyright MapModelers. All Rights Reserved.

#include "SurfBrushMath.h"
#include "CompGeom/ConvexHull3.h"

namespace SurfBrushMath
{
namespace
{
	/** Signed distance from the plane. Negative is inside. */
	FORCEINLINE double PlaneDistance(const FPlane& Plane, const FVector& Point)
	{
		return FVector::DotProduct(FVector(Plane.X, Plane.Y, Plane.Z), Point) - Plane.W;
	}

	FORCEINLINE FVector PlaneNormal(const FPlane& Plane)
	{
		return FVector(Plane.X, Plane.Y, Plane.Z);
	}

	/** Two in-plane axes, right-handed with the normal, for angular sorting. */
	void PlaneBasis(const FVector& Normal, FVector& OutU, FVector& OutV)
	{
		const FVector Seed = FMath::Abs(Normal.Z) < 0.9 ? FVector(0.0, 0.0, 1.0) : FVector(1.0, 0.0, 0.0);
		OutU = FVector::CrossProduct(Seed, Normal).GetSafeNormal();
		OutV = FVector::CrossProduct(Normal, OutU).GetSafeNormal();
	}
}

bool IntersectThreePlanes(const FPlane& A, const FPlane& B, const FPlane& C, FVector& Out)
{
	const FVector NA = PlaneNormal(A);
	const FVector NB = PlaneNormal(B);
	const FVector NC = PlaneNormal(C);

	const FVector CrossBC = FVector::CrossProduct(NB, NC);
	const double Denom = FVector::DotProduct(NA, CrossBC);

	// Scale-relative threshold: the normals are unit length, so this is a
	// direct measure of how close to coplanar the triple is.
	if (FMath::Abs(Denom) < 1.e-6)
	{
		return false;
	}

	const FVector CrossCA = FVector::CrossProduct(NC, NA);
	const FVector CrossAB = FVector::CrossProduct(NA, NB);

	Out = (A.W * CrossBC + B.W * CrossCA + C.W * CrossAB) / Denom;
	return !Out.ContainsNaN();
}

bool IsPointInside(const TArray<FSurfFace>& Faces, const FVector& Point, double EpsCm)
{
	for (const FSurfFace& Face : Faces)
	{
		if (PlaneDistance(Face.Plane, Point) > EpsCm)
		{
			return false;
		}
	}
	return true;
}

bool DeriveHull(const TArray<FSurfFace>& Faces, FSurfHull& Out, double EpsCm)
{
	Out.Reset();

	const int32 NumFaces = Faces.Num();
	if (NumFaces < 4)
	{
		return false;
	}

	// Candidate vertices: every plane triple that meets at a point which is
	// inside all the other half-spaces. O(n^3) in the face count, which for a
	// brush is single digits; the constant matters more than the exponent here.
	TArray<FVector> Verts;
	Verts.Reserve(NumFaces * 3);

	// Which planes each vertex lies on, so faces can be assembled without a
	// second distance pass over every vertex.
	TArray<TArray<int32, TInlineAllocator<8>>> VertFaces;
	VertFaces.Reserve(NumFaces * 3);

	const double WeldEpsSq = FMath::Square(EpsCm * 2.0);

	for (int32 I = 0; I < NumFaces - 2; ++I)
	{
		for (int32 J = I + 1; J < NumFaces - 1; ++J)
		{
			for (int32 K = J + 1; K < NumFaces; ++K)
			{
				FVector Point;
				if (!IntersectThreePlanes(Faces[I].Plane, Faces[J].Plane, Faces[K].Plane, Point))
				{
					continue;
				}

				if (!IsPointInside(Faces, Point, EpsCm))
				{
					continue;
				}

				// Weld against what we already have; three planes meeting at a
				// corner of a box produce the same point from several triples.
				int32 Existing = INDEX_NONE;
				for (int32 V = 0; V < Verts.Num(); ++V)
				{
					if (FVector::DistSquared(Verts[V], Point) <= WeldEpsSq)
					{
						Existing = V;
						break;
					}
				}

				if (Existing == INDEX_NONE)
				{
					Existing = Verts.Add(Point);
					VertFaces.AddDefaulted();
				}

				for (const int32 FaceIndex : { I, J, K })
				{
					VertFaces[Existing].AddUnique(FaceIndex);
				}
			}
		}
	}

	if (Verts.Num() < 4)
	{
		return false;
	}

	// A vertex can also lie on a plane it was not derived from - a pyramid
	// apex, or any point where more than three planes meet. Catch those.
	for (int32 V = 0; V < Verts.Num(); ++V)
	{
		for (int32 F = 0; F < NumFaces; ++F)
		{
			if (FMath::Abs(PlaneDistance(Faces[F].Plane, Verts[V])) <= EpsCm)
			{
				VertFaces[V].AddUnique(F);
			}
		}
	}

	Out.Vertices = MoveTemp(Verts);
	Out.Faces.Reserve(NumFaces);

	TArray<int32> OnPlane;
	TArray<double> Angles;

	for (int32 F = 0; F < NumFaces; ++F)
	{
		OnPlane.Reset();
		for (int32 V = 0; V < Out.Vertices.Num(); ++V)
		{
			if (VertFaces[V].Contains(F))
			{
				OnPlane.Add(V);
			}
		}

		// Fewer than three points means the plane does not bound the hull.
		// That is legal - a slice can leave a redundant plane behind - so it
		// is silently dropped rather than failing the derivation.
		if (OnPlane.Num() < 3)
		{
			continue;
		}

		const FVector Normal = PlaneNormal(Faces[F].Plane);

		FVector Centroid = FVector::ZeroVector;
		for (const int32 V : OnPlane)
		{
			Centroid += Out.Vertices[V];
		}
		Centroid /= static_cast<double>(OnPlane.Num());

		FVector AxisU, AxisV;
		PlaneBasis(Normal, AxisU, AxisV);

		Angles.Reset();
		Angles.AddUninitialized(OnPlane.Num());
		for (int32 Idx = 0; Idx < OnPlane.Num(); ++Idx)
		{
			const FVector D = Out.Vertices[OnPlane[Idx]] - Centroid;
			Angles[Idx] = FMath::Atan2(FVector::DotProduct(D, AxisV), FVector::DotProduct(D, AxisU));
		}

		// Sort indices by angle, counter-clockwise around the outward normal.
		TArray<int32> Order;
		Order.Reserve(OnPlane.Num());
		for (int32 Idx = 0; Idx < OnPlane.Num(); ++Idx)
		{
			Order.Add(Idx);
		}
		Order.Sort([&Angles](const int32 A, const int32 B) { return Angles[A] < Angles[B]; });

		FSurfHullFace& HullFace = Out.Faces.AddDefaulted_GetRef();
		HullFace.SourceFaceIndex = F;
		HullFace.VertexIndices.Reserve(Order.Num());
		for (const int32 Idx : Order)
		{
			HullFace.VertexIndices.Add(OnPlane[Idx]);
		}
	}

	if (!Out.IsValid())
	{
		Out.Reset();
		return false;
	}

	return true;
}

bool DeriveHull(const FSurfBrush& Brush, FSurfHull& Out, double EpsCm)
{
	const bool bOk = DeriveHull(Brush.Faces, Out, EpsCm);
	if (bOk)
	{
		Out.EditSerial = Brush.EditSerial;
	}
	return bOk;
}

void SplitBrush(const FSurfBrush& Brush, const FPlane& SplitPlane,
	FSurfBrush& OutFront, bool& bOutFrontValid,
	FSurfBrush& OutBack, bool& bOutBackValid,
	double EpsCm)
{
	bOutFrontValid = false;
	bOutBackValid = false;

	const FVector Normal = PlaneNormal(SplitPlane).GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		return;
	}

	const FPlane Forward(Normal, SplitPlane.W);
	const FPlane Backward(-Normal, -SplitPlane.W);

	// Inside is Dot(N, X) - W <= 0, so adding a face keeps the half behind
	// that face's normal.
	auto BuildHalf = [&Brush, EpsCm](const FPlane& Cut, FSurfBrush& Out) -> bool
	{
		Out = Brush;
		Out.Id = FGuid();
		Out.EnsureId();

		FSurfFace& NewFace = Out.Faces.AddDefaulted_GetRef();
		NewFace.Plane = Cut;
		NewFace.Material = Brush.Faces.Num() > 0 ? Brush.Faces[0].Material : nullptr;
		NewFace.SmoothingGroup = Brush.Faces.Num() > 0 ? Brush.Faces[0].SmoothingGroup : 0;

		FSurfHull Hull;
		if (!DeriveHull(Out.Faces, Hull, EpsCm))
		{
			return false;
		}

		// A cut that only grazes the brush leaves a slab with no volume.
		if (Volume(Hull) <= EpsCm * EpsCm * EpsCm)
		{
			return false;
		}

		// Only the planes that bound this half stay: the plane a cut left
		// on the far side is no face of this piece, and a brush sliced ten
		// times should not carry ten dead planes. Order kept, so the cut
		// face is still the last.
		TSet<int32> Bounding;
		for (const FSurfHullFace& HullFace : Hull.Faces)
		{
			Bounding.Add(HullFace.SourceFaceIndex);
		}
		if (Bounding.Num() < Out.Faces.Num())
		{
			TArray<FSurfFace> Kept;
			Kept.Reserve(Bounding.Num());
			for (int32 I = 0; I < Out.Faces.Num(); ++I)
			{
				if (Bounding.Contains(I))
				{
					Kept.Add(Out.Faces[I]);
				}
			}
			Out.Faces = MoveTemp(Kept);
		}
		return true;
	};

	bOutBackValid = BuildHalf(Forward, OutBack);
	bOutFrontValid = BuildHalf(Backward, OutFront);
}

FSurfBrush MakeBox(const FBox& Box)
{
	FSurfBrush Brush;
	Brush.EnsureId();
	Brush.Faces.Reserve(6);

	const FVector Min = Box.Min;
	const FVector Max = Box.Max;

	// Outward normals, W = Dot(N, PointOnPlane).
	Brush.Faces.Emplace(FPlane(FVector( 1.0,  0.0,  0.0),  Max.X));
	Brush.Faces.Emplace(FPlane(FVector(-1.0,  0.0,  0.0), -Min.X));
	Brush.Faces.Emplace(FPlane(FVector( 0.0,  1.0,  0.0),  Max.Y));
	Brush.Faces.Emplace(FPlane(FVector( 0.0, -1.0,  0.0), -Min.Y));
	Brush.Faces.Emplace(FPlane(FVector( 0.0,  0.0,  1.0),  Max.Z));
	Brush.Faces.Emplace(FPlane(FVector( 0.0,  0.0, -1.0), -Min.Z));

	return Brush;
}

FSurfBrush MakeCylinder(const FBox& Box, int32 Sides)
{
	Sides = FMath::Clamp(Sides, 3, 64);

	FSurfBrush Brush;
	Brush.EnsureId();
	Brush.Faces.Reserve(Sides + 2);

	const FVector Centre = Box.GetCenter();
	const FVector Half = Box.GetExtent();
	const double A = FMath::Max(Half.X, UE_DOUBLE_SMALL_NUMBER);
	const double B = FMath::Max(Half.Y, UE_DOUBLE_SMALL_NUMBER);

	// The first side faces +X, so the flats land on the axes when the count
	// allows; the plane through the point of the ellipse whose normal is N
	// sits at the ellipse's support distance sqrt(a^2 nx^2 + b^2 ny^2).
	for (int32 I = 0; I < Sides; ++I)
	{
		const double Angle = 2.0 * UE_DOUBLE_PI * I / Sides;
		const FVector Normal(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
		const double Support = FMath::Sqrt(A * A * Normal.X * Normal.X + B * B * Normal.Y * Normal.Y);
		Brush.Faces.Emplace(FPlane(Normal, FVector::DotProduct(Normal, Centre) + Support));
	}

	Brush.Faces.Emplace(FPlane(FVector(0.0, 0.0,  1.0),  Box.Max.Z));
	Brush.Faces.Emplace(FPlane(FVector(0.0, 0.0, -1.0), -Box.Min.Z));

	return Brush;
}

bool RebuildFromPoints(const FSurfBrush& From, const TArray<FVector>& Points,
	FSurfBrush& Out, FString* OutWhy, double EpsCm)
{
	auto No = [OutWhy](const TCHAR* Why)
	{
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return false;
	};

	if (Points.Num() < 4)
	{
		return No(TEXT("a solid needs four corners"));
	}

	UE::Geometry::FConvexHull3d Hull;
	if (!Hull.Solve(Points.Num(), [&Points](int32 I) { return FVector3d(Points[I]); }))
	{
		return No(TEXT("the corners are flat - there is no solid in them"));
	}

	const TArray<UE::Geometry::FIndex3i>& Triangles = Hull.GetTriangles();
	if (Triangles.Num() < 4)
	{
		return No(TEXT("the corners are flat - there is no solid in them"));
	}

	/**
	 * Every corner has to still be a corner. A point the hull did not use is
	 * one that has gone inside the solid, and a brush quietly losing a corner
	 * is how a drag turns into a shape nobody asked for.
	 */
	TArray<bool> Used;
	Used.Init(false, Points.Num());
	for (const UE::Geometry::FIndex3i& Tri : Triangles)
	{
		Used[Tri.A] = true;
		Used[Tri.B] = true;
		Used[Tri.C] = true;
	}
	int32 Lost = 0;
	for (const bool bUsed : Used)
	{
		Lost += bUsed ? 0 : 1;
	}
	if (Lost > 0)
	{
		if (OutWhy)
		{
			*OutWhy = FString::Printf(TEXT("%d corner%s would end up inside the solid"),
				Lost, Lost == 1 ? TEXT("") : TEXT("s"));
		}
		return false;
	}

	// The triangles of one flat face all carry the same plane; gather them
	// into one, within half a degree and half a millimetre.
	const double SameNormal = FMath::Cos(FMath::DegreesToRadians(0.5));

	TArray<FPlane> Planes;
	for (const UE::Geometry::FIndex3i& Tri : Triangles)
	{
		const FVector A = Points[Tri.A];
		const FVector B = Points[Tri.B];
		const FVector C = Points[Tri.C];
		const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			continue;
		}
		const double W = FVector::DotProduct(Normal, A);

		bool bHave = false;
		for (const FPlane& Each : Planes)
		{
			if (FVector::DotProduct(PlaneNormal(Each), Normal) > SameNormal
				&& FMath::Abs(Each.W - W) <= EpsCm)
			{
				bHave = true;
				break;
			}
		}
		if (!bHave)
		{
			Planes.Emplace(Normal, W);
		}
	}

	if (Planes.Num() < 4)
	{
		return No(TEXT("fewer than four sides came out of it"));
	}

	// The hull's own normals point away from its interior, and a brush's
	// planes are outward too - but say it rather than assume it.
	FVector Centre = FVector::ZeroVector;
	for (const FVector& Point : Points)
	{
		Centre += Point;
	}
	Centre /= static_cast<double>(Points.Num());

	Out = From;
	Out.Faces.Reset();
	Out.Faces.Reserve(Planes.Num());

	for (FPlane Plane : Planes)
	{
		if (FVector::DotProduct(PlaneNormal(Plane), Centre) - Plane.W > 0.0)
		{
			Plane = FPlane(-PlaneNormal(Plane), -Plane.W);
		}

		// The old face that pointed most nearly this way keeps its material
		// and its projection: a drag should not repaint anything.
		const FVector Normal = PlaneNormal(Plane);
		int32 Nearest = INDEX_NONE;
		double Best = -2.0;
		for (int32 I = 0; I < From.Faces.Num(); ++I)
		{
			const double D = FVector::DotProduct(PlaneNormal(From.Faces[I].Plane), Normal);
			if (D > Best)
			{
				Best = D;
				Nearest = I;
			}
		}

		FSurfFace Face = From.Faces.IsValidIndex(Nearest) ? From.Faces[Nearest] : FSurfFace();
		Face.Plane = Plane;
		Out.Faces.Add(MoveTemp(Face));
	}

	// And it has to still derive: the planes are the brush, and a set of
	// planes that bounds nothing is not one.
	FSurfHull Check;
	if (!DeriveHull(Out.Faces, Check, EpsCm) || Volume(Check) <= EpsCm * EpsCm * EpsCm)
	{
		return No(TEXT("the sides that came out of it bound nothing"));
	}

	++Out.EditSerial;
	return true;
}

double Volume(const FSurfHull& Hull)
{
	double Six = 0.0;

	for (const FSurfHullFace& Face : Hull.Faces)
	{
		for (int32 I = 1; I + 1 < Face.VertexIndices.Num(); ++I)
		{
			const FVector& A = Hull.Vertices[Face.VertexIndices[0]];
			const FVector& B = Hull.Vertices[Face.VertexIndices[I]];
			const FVector& C = Hull.Vertices[Face.VertexIndices[I + 1]];

			Six += FVector::DotProduct(A, FVector::CrossProduct(B, C));
		}
	}

	return FMath::Abs(Six) / 6.0;
}

void Triangulate(const FSurfHull& Hull, TArray<uint32>& OutIndices)
{
	int32 TriCount = 0;
	for (const FSurfHullFace& Face : Hull.Faces)
	{
		TriCount += FMath::Max(0, Face.VertexIndices.Num() - 2);
	}

	OutIndices.Reset();
	OutIndices.Reserve(TriCount * 3);

	// The hull orders a face counter-clockwise about its outward normal;
	// Unreal's front is (C - A) x (B - A), so the fan goes out backwards
	// (S108a). The preview does the same, and has to: a brush that bakes the
	// other way round from the way it draws is worse than one that is wrong
	// twice.
	for (const FSurfHullFace& Face : Hull.Faces)
	{
		for (int32 I = 1; I + 1 < Face.VertexIndices.Num(); ++I)
		{
			OutIndices.Add(static_cast<uint32>(Face.VertexIndices[0]));
			OutIndices.Add(static_cast<uint32>(Face.VertexIndices[I + 1]));
			OutIndices.Add(static_cast<uint32>(Face.VertexIndices[I]));
		}
	}
}

} // namespace SurfBrushMath
