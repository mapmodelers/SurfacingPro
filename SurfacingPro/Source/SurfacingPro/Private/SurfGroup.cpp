// Copyright MapModelers. All Rights Reserved.

#include "SurfGroup.h"

#include "Components/DynamicMeshComponent.h"
#include "Engine/World.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"

#include "SurfChunk.h"
#include "SurfSession.h"
#include "SurfPatchOps.h"
#include "SurfBrushMath.h"
#include "SurfPatchMath.h"
#include "SurfProjection.h"
#include "SurfSettings.h"
#include "SurfLog.h"
#include "SurfMaterialSet.h"
#include "SurfPrefab.h"

using namespace UE::Geometry;

namespace
{
	/**
	 * Appends one brush. Faces are appended independently so each keeps its
	 * own normal and its own projection - a brush is flat shaded by nature and
	 * two faces almost never share UVs.
	 */
	/**
	 * Point a triangle the way we mean it to face (S109a).
	 *
	 * Unreal reads a triangle (P, Q, R) as facing (R - P) x (Q - P). Laying
	 * the corners out in that order at append time turned out not to be
	 * enough - the mesh does not promise to keep the order it was handed -
	 * so the facing is **checked after the fact and corrected**, which needs
	 * no promise from anybody. Reversing carries the attribute overlays with
	 * it, so this goes after the normals, UVs and colours are set, not
	 * before.
	 *
	 * Asking is cheap and the answer is certain, which is the whole reason
	 * this reads back rather than arranging.
	 */
	void PointTriangle(FDynamicMesh3& Mesh, int32 Tid, const FVector& Want)
	{
		if (Want.IsNearlyZero() || !Mesh.IsTriangle(Tid))
		{
			return;
		}
		FVector P, Q, R;
		Mesh.GetTriVertices(Tid, P, Q, R);
		const FVector Facing = FVector::CrossProduct(R - P, Q - P);
		if (FVector::DotProduct(Facing, Want) < 0.0)
		{
			Mesh.ReverseTriOrientation(Tid);
		}
	}

	void AppendBrush(FDynamicMesh3& Mesh, FDynamicMeshNormalOverlay* NormalOverlay, FDynamicMeshUVOverlay* UVOverlay, FDynamicMeshColorOverlay* ColourOverlay, FDynamicMeshMaterialAttribute* MaterialIDs, FSurfMaterialSet& Materials, const FSurfBrush& Brush, int32 TriangleGroup = 0)
	{
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(Brush, Hull))
		{
			return;
		}

		/**
		 * Sides that meet at less than the crease angle share a normal
		 * along their edge (S171a): at a corner, this side's normal is
		 * the average of every side at that corner that turns from it by
		 * less than the angle - so a cylinder's sides read round while its
		 * caps stay hard, and a box stays a box at zero.
		 */
		const double CosCrease = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Brush.CreaseAngleDeg, 0.0f, 180.0f)));
		const bool bSmoothing = Brush.CreaseAngleDeg > 0.5f;
		TMap<int32, TArray<int32, TInlineAllocator<8>>> FacesAtVertex;
		if (bSmoothing)
		{
			for (int32 F = 0; F < Hull.Faces.Num(); ++F)
			{
				for (const int32 VertexIndex : Hull.Faces[F].VertexIndices)
				{
					FacesAtVertex.FindOrAdd(VertexIndex).Add(F);
				}
			}
		}
		auto NormalOfHullFace = [&Hull, &Brush](int32 F) -> FVector
		{
			const int32 Source = Hull.Faces[F].SourceFaceIndex;
			return Brush.Faces.IsValidIndex(Source) ? FVector(Brush.Faces[Source].Plane.X, Brush.Faces[Source].Plane.Y, Brush.Faces[Source].Plane.Z).GetSafeNormal() : FVector::ZeroVector;
		};

		for (int32 HullFaceIndex = 0; HullFaceIndex < Hull.Faces.Num(); ++HullFaceIndex)
		{
			const FSurfHullFace& HullFace = Hull.Faces[HullFaceIndex];
			const int32 Count = HullFace.VertexIndices.Num();
			if (Count < 3 || !Brush.Faces.IsValidIndex(HullFace.SourceFaceIndex))
			{
				continue;
			}

			const FSurfFace& Face = Brush.Faces[HullFace.SourceFaceIndex];
			const FVector FlatNormal(Face.Plane.X, Face.Plane.Y, Face.Plane.Z);
			const int32 MaterialSlot = Materials.SlotFor(Face.Material.Get());

			TArray<int32, TInlineAllocator<16>> Vids;
			TArray<int32, TInlineAllocator<16>> Nids;
			TArray<int32, TInlineAllocator<16>> Uids;
			TArray<int32, TInlineAllocator<16>> Cids;
			Vids.Reserve(Count);
			Nids.Reserve(Count);
			Uids.Reserve(Count);
			Cids.Reserve(Count);

			for (const int32 VertexIndex : HullFace.VertexIndices)
			{
				const FVector Position = Hull.Vertices[VertexIndex];
				const FVector2D UV = SurfProjection::ComputeUV(Face.UV, Position);

				Vids.Add(Mesh.AppendVertex(Position));
				FVector Normal = FlatNormal;
				if (bSmoothing)
				{
					if (const TArray<int32, TInlineAllocator<8>>* Around = FacesAtVertex.Find(VertexIndex))
					{
						FVector Sum = FVector::ZeroVector;
						for (const int32 Other : *Around)
						{
							const FVector N = NormalOfHullFace(Other);
							if (Other == HullFaceIndex || FVector::DotProduct(N, FlatNormal.GetSafeNormal()) >= CosCrease)
							{
								Sum += N;
							}
						}
						if (!Sum.IsNearlyZero())
						{
							Normal = Sum.GetSafeNormal();
						}
					}
				}
				Nids.Add(NormalOverlay->AppendElement(FVector3f(Normal)));
				Uids.Add(UVOverlay->AppendElement(FVector2f(static_cast<float>(UV.X), static_cast<float>(UV.Y))));

				// Black and empty, as an unpainted patch is: nothing painted.
				Cids.Add(ColourOverlay->AppendElement(FVector4f(0.0f, 0.0f, 0.0f, 0.0f)));
			}

			/**
			 * Fan from the first vertex, then turned to face out (S108a/S109a).
			 *
			 * "the box has normal flipped by default!" - and it was: every
			 * brush in the level was a solid seen from the inside. The face
			 * already carries the answer, in its plane's normal, so the fan
			 * goes out in hull order and each triangle is pointed at it
			 * afterwards. Hull faces are convex by construction, so the fan
			 * itself is safe whichever way round it runs.
			 */
			const FVector Outward(Face.Plane.X, Face.Plane.Y, Face.Plane.Z);
			for (int32 I = 1; I + 1 < Count; ++I)
			{
				const int32 Tid = Mesh.AppendTriangle(Vids[0], Vids[I], Vids[I + 1]);
				if (Tid >= 0 && TriangleGroup != 0)
				{
					Mesh.SetTriangleGroup(Tid, TriangleGroup);
				}
				if (Tid < 0)
				{
					continue;
				}

				NormalOverlay->SetTriangle(Tid, FIndex3i(Nids[0], Nids[I], Nids[I + 1]));
				UVOverlay->SetTriangle(Tid, FIndex3i(Uids[0], Uids[I], Uids[I + 1]));
				ColourOverlay->SetTriangle(Tid, FIndex3i(Cids[0], Cids[I], Cids[I + 1]));
				MaterialIDs->SetValue(Tid, &MaterialSlot);

				PointTriangle(Mesh, Tid, Outward);
			}
		}
	}

	void AppendPatch(FDynamicMesh3& Mesh, FDynamicMeshNormalOverlay* NormalOverlay, FDynamicMeshUVOverlay* UVOverlay, FDynamicMeshColorOverlay* ColourOverlay, const FSurfPatch& Patch, double LayerOffsetCm, int32 TriangleGroup, FDynamicMeshMaterialAttribute* MaterialIDs, int32 MaterialSlot)
	{
		FSurfTessellation Tess;
		if (!SurfPatchMath::Tessellate(Patch, Tess))
		{
			return;
		}

		SurfPatchMath::ApplyStackOffset(Tess, Patch.StackLevel, LayerOffsetCm);

		const int32 Count = Tess.Positions.Num();
		TArray<int32> Vids;
		TArray<int32> Nids;
		TArray<int32> Uids;
		TArray<int32> Cids;
		Vids.Reserve(Count);
		Nids.Reserve(Count);
		Uids.Reserve(Count);
		Cids.Reserve(Count);

		for (int32 I = 0; I < Count; ++I)
		{
			Vids.Add(Mesh.AppendVertex(Tess.Positions[I]));

			// The tessellation has already turned over if the patch is
			// inverted - winding and normals together (S115a) - so there is
			// nothing to undo here.
			Nids.Add(NormalOverlay->AppendElement(FVector3f(Tess.Normals[I])));
			Uids.Add(UVOverlay->AppendElement(FVector2f(
				static_cast<float>(Tess.UVs[I].X),
				static_cast<float>(Tess.UVs[I].Y))));

			// RGB the tint, alpha this patch's opacity where it is stacked
			// over another (D14). Both were already interpolated across the
			// tessellation from the control net and had nowhere to go, which
			// is why a painted weight changed nothing anyone could see.
			const FColor Tint = Tess.Tints.IsValidIndex(I) ? Tess.Tints[I] : FColor(0, 0, 0, 0);
			const float Weight = Tess.Weights.IsValidIndex(I) ? Tess.Weights[I] : 0.0f;
			Cids.Add(ColourOverlay->AppendElement(FVector4f(
				Tint.R / 255.0f, Tint.G / 255.0f, Tint.B / 255.0f, Weight)));
		}

		// Shading is decided per triangle corner, because that is the only
		// place it can be: two cells meeting at a hard fold need two different
		// normals at the same position, and a vertex only has one.
		const bool bPerTriangle = Patch.Shading != ESurfShading::Smooth;
		const double CosLimit = FMath::Cos(FMath::DegreesToRadians(
			FMath::Clamp(Patch.SmoothingAngleDegrees, 1.0, 179.0)));

		/**
		 * Hard lines (S171a): a control row or column marked hard is a
		 * crease whatever the shading says. The surface passes through the
		 * control lines on span boundaries, which sit at sample row
		 * (Row / 2) * SubdivV; at a corner on such a line the normal is
		 * the average of the triangles on that corner's own side of it,
		 * so the two sides shade apart and the run along the line stays
		 * smooth.
		 */
		TSet<int32> HardSampleRows;
		TSet<int32> HardSampleColumns;
		if (Patch.Type != ESurfPatchType::Sheet)
		{
			const int32 SubdivU = FMath::Clamp(Patch.SubdivU, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
			const int32 SubdivV = FMath::Clamp(Patch.SubdivV, FSurfPatch::MinSubdiv, FSurfPatch::MaxSubdiv);
			for (int32 Row : Patch.HardRows)
			{
				if (Row > 0 && Row + 1 < Patch.Height && (Row % 2) == 0) { HardSampleRows.Add((Row / 2) * SubdivV); }
			}
			for (int32 Column : Patch.HardColumns)
			{
				if (Column > 0 && Column + 1 < Patch.Width && (Column % 2) == 0) { HardSampleColumns.Add((Column / 2) * SubdivU); }
			}
		}
		else
		{
			for (int32 Row : Patch.HardRows) { if (Row > 0 && Row + 1 < Patch.Height) { HardSampleRows.Add(Row); } }
			for (int32 Column : Patch.HardColumns) { if (Column > 0 && Column + 1 < Patch.Width) { HardSampleColumns.Add(Column); } }
		}
		const bool bHardLines = HardSampleRows.Num() > 0 || HardSampleColumns.Num() > 0;
		// Per vertex on a hard line: the side-averaged normals, keyed by
		// (vertex, side) with side 0 = below/left of the line, 1 = above/right.
		TMap<int64, FVector> SideNormals;
		auto SideKey = [](int32 Vertex, int32 SideRow, int32 SideColumn) -> int64 { return (static_cast<int64>(Vertex) << 2) | (SideRow << 1) | SideColumn; };
		auto OnHardRow = [&](int32 Vertex) { return bHardLines && Tess.NumU > 0 && HardSampleRows.Contains(Vertex / Tess.NumU); };
		auto OnHardColumn = [&](int32 Vertex) { return bHardLines && Tess.NumU > 0 && HardSampleColumns.Contains(Vertex % Tess.NumU); };
		auto SideOfTriangle = [&](int32 Vertex, int32 A, int32 B, int32 C, int32& OutSideRow, int32& OutSideColumn)
		{
			// Which side of the line(s) through Vertex this triangle lies on:
			// its other corners are all at or past the line, one way.
			const int32 Row = Vertex / Tess.NumU;
			const int32 Column = Vertex % Tess.NumU;
			OutSideRow = 0;
			OutSideColumn = 0;
			for (const int32 Other : { A, B, C })
			{
				if (Other == Vertex) { continue; }
				if (Other / Tess.NumU > Row) { OutSideRow = 1; }
				if (Other % Tess.NumU > Column) { OutSideColumn = 1; }
			}
		};
		if (bHardLines)
		{
			for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
			{
				const int32 Tri[3] = { static_cast<int32>(Tess.Indices[I]), static_cast<int32>(Tess.Indices[I + 1]), static_cast<int32>(Tess.Indices[I + 2]) };
				const FVector& PA = Tess.Positions[Tri[0]];
				const FVector& PB = Tess.Positions[Tri[1]];
				const FVector& PC = Tess.Positions[Tri[2]];
				const FVector Cross = FVector::CrossProduct(PC - PA, PB - PA);
				if (Cross.SizeSquared() < 1.e-6) { continue; }
				const FVector Face = Cross.GetSafeNormal();
				for (const int32 V : Tri)
				{
					const bool bRow = OnHardRow(V);
					const bool bColumn = OnHardColumn(V);
					if (!bRow && !bColumn) { continue; }
					int32 SideRow = 0, SideColumn = 0;
					SideOfTriangle(V, Tri[0], Tri[1], Tri[2], SideRow, SideColumn);
					// Unweighted average, area-weighted by the cross itself.
					SideNormals.FindOrAdd(SideKey(V, bRow ? SideRow : 0, bColumn ? SideColumn : 0)) += Cross;
				}
			}
		}

		for (int32 I = 0; I + 2 < Tess.Indices.Num(); I += 3)
		{
			const int32 A = static_cast<int32>(Tess.Indices[I]);
			const int32 B = static_cast<int32>(Tess.Indices[I + 1]);
			const int32 C = static_cast<int32>(Tess.Indices[I + 2]);

			// Welded points sit on top of each other, so the cells between
			// them collapse. Those triangles have no area, carry no normal
			// worth having, and are the difference between a quad grid that
			// can hold a triangle and one that only pretends to.
			const FVector& PA = Tess.Positions[A];
			const FVector& PB = Tess.Positions[B];
			const FVector& PC = Tess.Positions[C];

			if (FVector::CrossProduct(PB - PA, PC - PA).SizeSquared() < 1.e-6)
			{
				continue;
			}

			/**
			 * A patch keeps the side it has always been drawn on (S115a).
			 *
			 * S108a turned every patch over, on the reasoning that Unreal
			 * reads a triangle's front the other way from the tool's own
			 * U x V. The reasoning was sound and the change was wrong: **the
			 * levels were built looking at the old answer**, so turning it
			 * over reversed every surface he had already made - "its face were
			 * reversed i dont think i saved it this way?", and he did not.
			 *
			 * A convention is only worth arguing about where nothing is built
			 * on it yet. Brushes were the case where the answer was checkable
			 * against something outside the tool - Unreal's own box - and they
			 * keep that fix. A patch is a surface, and which side of a surface
			 * is the front is a choice; his levels already made it.
			 *
			 * What bInvert does is the part that was genuinely broken: it used
			 * to turn the faceted shading normal and nothing else, so Flip did
			 * nothing at all on a smooth patch - "why using flip dont work
			 * here ?". It turns the drawn side now, which is what flipping a
			 * surface means.
			 */
			const int32 Tid = Mesh.AppendTriangle(Vids[A], Vids[B], Vids[C]);
			if (Tid < 0)
			{
				continue;
			}

			// Which patch a triangle came from, for the editor's visibility
			// test: a curve's control points sit off its surface, and must
			// not be hidden by it.
			Mesh.SetTriangleGroup(Tid, TriangleGroup);

			if (bHardLines && (OnHardRow(A) || OnHardRow(B) || OnHardRow(C) || OnHardColumn(A) || OnHardColumn(B) || OnHardColumn(C)))
			{
				const FVector Face = FVector::CrossProduct(PC - PA, PB - PA).GetSafeNormal();
				FIndex3i Corners(Nids[A], Nids[B], Nids[C]);
				const int32 Source[3] = { A, B, C };
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 V = Source[Corner];
					const bool bRow = OnHardRow(V);
					const bool bColumn = OnHardColumn(V);
					FVector Use = Tess.Normals[V];
					if (bRow || bColumn)
					{
						int32 SideRow = 0, SideColumn = 0;
						SideOfTriangle(V, A, B, C, SideRow, SideColumn);
						const FVector* Side = SideNormals.Find(SideKey(V, bRow ? SideRow : 0, bColumn ? SideColumn : 0));
						Use = Side && !Side->IsNearlyZero() ? Side->GetSafeNormal() : Face;
					}
					else if (bPerTriangle && (Patch.Shading == ESurfShading::Faceted || FVector::DotProduct(Use, Face) < CosLimit))
					{
						Use = Face;
					}
					if (!Use.Equals(Tess.Normals[V], 1.e-4))
					{
						Corners[Corner] = NormalOverlay->AppendElement(FVector3f(Use));
					}
				}
				NormalOverlay->SetTriangle(Tid, Corners);
			}
			else if (bPerTriangle)
			{
				// From the winding, which is already the flipped one when the
				// patch is inverted.
				const FVector Face = FVector::CrossProduct(PC - PA, PB - PA).GetSafeNormal();

				// Faceted takes the face normal outright. ByAngle keeps the
				// averaged normal wherever the face agrees with it, and takes
				// the face normal where it does not - smooth along the run,
				// hard at the crease.
				FIndex3i Corners(Nids[A], Nids[B], Nids[C]);

				const int32 Source[3] = { A, B, C };
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector Averaged = Tess.Normals[Source[Corner]];

					const bool bUseFace = Patch.Shading == ESurfShading::Faceted
						|| FVector::DotProduct(Averaged, Face) < CosLimit;

					if (bUseFace)
					{
						Corners[Corner] = NormalOverlay->AppendElement(FVector3f(Face));
					}
				}

				NormalOverlay->SetTriangle(Tid, Corners);
			}
			else
			{
				NormalOverlay->SetTriangle(Tid, FIndex3i(Nids[A], Nids[B], Nids[C]));
			}

			UVOverlay->SetTriangle(Tid, FIndex3i(Uids[A], Uids[B], Uids[C]));
			ColourOverlay->SetTriangle(Tid, FIndex3i(Cids[A], Cids[B], Cids[C]));
			MaterialIDs->SetValue(Tid, &MaterialSlot);
		}
	}
}

// --- Prefabs -----------------------------------------------------------------

static void AppendPrefabWalk(FDynamicMesh3& Mesh, const USurfPrefab& Prefab, const FTransform& Local,
	FSurfMaterialSet& Materials, double LayerOffsetCm, TSet<const USurfPrefab*>& Stack, bool bIncludeOwnGeometry, TArray<FSurfPlacementRange>* OutRanges,
	const TArray<FSurfPaintOverride>* Paint)
{
	// A prefab on the way down cannot appear again below itself - the asset
	// refuses the placement (USurfPrefab::CanPlace), and this is the belt to
	// that brace, for an asset edited by hand.
	if (Stack.Contains(&Prefab) || Stack.Num() > 32)
	{
		return;
	}
	Stack.Add(&Prefab);

	/**
	 * Each piece on its own, not "if there are no attributes at all" (S85b).
	 *
	 * The old guard read `if (!Mesh.HasAttributes())` and set up all five
	 * things inside it. A caller who had already called `EnableAttributes()`
	 * therefore got a mesh with no colour overlay and no material IDs, and
	 * the first `AppendElement` wrote through a null pointer - an access
	 * violation at offset 0x38, which took the editor down while the Content
	 * Browser was drawing a prefab thumbnail.
	 *
	 * A setup guard that is all-or-nothing is a trap for the next caller.
	 * This one ensures what is missing and nothing else.
	 */
	if (!Mesh.HasAttributes())
	{
		Mesh.EnableAttributes();
	}
	if (!Mesh.HasTriangleGroups())
	{
		Mesh.EnableTriangleGroups(0);
	}
	if (Mesh.Attributes()->NumUVLayers() < 1)
	{
		Mesh.Attributes()->SetNumUVLayers(1);
	}
	if (!Mesh.Attributes()->HasMaterialID())
	{
		Mesh.Attributes()->EnableMaterialID();
	}
	if (!Mesh.Attributes()->HasPrimaryColors())
	{
		Mesh.Attributes()->EnablePrimaryColors();
	}

	FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
	FDynamicMeshNormalOverlay* NormalOverlay = Attributes->PrimaryNormals();
	FDynamicMeshUVOverlay* UVOverlay = Attributes->PrimaryUV();
	FDynamicMeshColorOverlay* ColourOverlay = Attributes->PrimaryColors();
	FDynamicMeshMaterialAttribute* MaterialIDs = Attributes->GetMaterialID();

	// And if any of it is still missing, nothing is appended. A picture in
	// the Content Browser is never worth the editor.
	if (!NormalOverlay || !UVOverlay || !ColourOverlay || !MaterialIDs)
	{
		Stack.Remove(&Prefab);
		return;
	}

	// Carried, not re-derived: the positions move with the placement and the
	// UVs stay what they were, so the texture is part of the prefab and
	// travels with it. A brush's planes move the same way.
	const FMatrix LocalMatrix = Local.ToMatrixWithScale();

	if (bIncludeOwnGeometry)
	{
	Prefab.ForEachBrush([&](const FSurfBrush& Brush)
	{
		FSurfBrush Carried = Brush;
		for (FSurfFace& Face : Carried.Faces)
		{
			Face.Plane = Face.Plane.TransformBy(LocalMatrix);
		}
		AppendBrush(Mesh, NormalOverlay, UVOverlay, ColourOverlay, MaterialIDs, Materials, Carried);
	});

	Prefab.ForEachPatch([&](const FSurfPatch& Patch)
	{
		FSurfPatch Carried = Patch;
		for (FSurfControlPoint& Point : Carried.Points)
		{
			Point.Pos = Local.TransformPosition(Point.Pos);
		}
		// The placed prefab's own paint over the prefab's (S251j).
		if (Paint)
		{
			for (const FSurfPaintOverride& Own : *Paint)
			{
				if (Own.Patch == Carried.Id && Own.Tint.Num() == Carried.Points.Num() && Own.Weight.Num() == Carried.Points.Num())
				{
					for (int32 I = 0; I < Carried.Points.Num(); ++I)
					{
						Carried.Points[I].Tint = Own.Tint[I];
						Carried.Points[I].Weight = Own.Weight[I];
					}
					break;
				}
			}
		}
		AppendPatch(Mesh, NormalOverlay, UVOverlay, ColourOverlay, Carried, LayerOffsetCm, 0, MaterialIDs, Materials.SlotFor(Carried.Material.Get()));
	});
	}

	for (const FSurfPlacement& Placement : Prefab.Placements)
	{
		if (const USurfPrefab* Child = Placement.Prefab.Get())
		{
			// Where this placement's triangles start and end (S71b), for the
			// top level only: a click maps to the placement it landed on.
			const int32 First = Mesh.MaxTriangleID();
			AppendPrefabWalk(Mesh, *Child, Placement.Transform * Local, Materials, LayerOffsetCm, Stack, /*bIncludeOwnGeometry*/ true, nullptr, nullptr);
			if (OutRanges)
			{
				FSurfPlacementRange& Range = OutRanges->AddDefaulted_GetRef();
				Range.Id = Placement.Id;
				Range.FirstTriangle = First;
				Range.NumTriangles = Mesh.MaxTriangleID() - First;
			}
		}
	}

	Stack.Remove(&Prefab);
}

void ASurfGroup::AppendPrefabToMesh(FDynamicMesh3& Mesh, const USurfPrefab& Prefab, const FTransform& Local,
	FSurfMaterialSet& Materials, double LayerOffsetCm, bool bIncludeOwnGeometry, TArray<FSurfPlacementRange>* OutRanges,
	const TArray<FSurfPaintOverride>* Paint)
{
	TSet<const USurfPrefab*> Stack;
	if (OutRanges)
	{
		OutRanges->Reset();
	}
	AppendPrefabWalk(Mesh, Prefab, Local, Materials, LayerOffsetCm, Stack, bIncludeOwnGeometry, OutRanges, Paint);
}

ASurfGroup::ASurfGroup()
{
	PrimaryActorTick.bCanEverTick = false;

	Preview = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Preview"));
	Preview->SetMobility(EComponentMobility::Static);
	RootComponent = Preview;

	// No collision unless asked for: the preview is the thing being edited,
	// rebuilt on every drag, and a complex collision mesh rebuilt with it is
	// cost for nothing. The setting turns it on for walking a level in
	// play-in-editor; the compiled mesh has its own.
	Preview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Preview->SetCollisionProfileName(TEXT("NoCollision"));
	Preview->bEnableComplexCollision = false;

	GroupId = FGuid::NewGuid();
}

#if WITH_EDITOR
void ASurfGroup::TidyOutlinerFolder()
{
	// Said once here so a group made today and a group loaded from a level
	// made last week end up in the same place.
	// The geometry sits in the folder itself rather than a room of its own
	// (S128a): a folder holding one folder is a click that buys nothing.
	if (GetFolderPath() != OutlinerFolder)
	{
		SetFolderPath(OutlinerFolder);
	}
}

void ASurfGroup::PostActorCreated()
{
	Super::PostActorCreated();
	// The author: "can we make sure theyre always stack under a surfacing
	// directory". One folder, so the outliner stays the level's.
	TidyOutlinerFolder();
}
#endif

// --- Storage ---------------------------------------------------------------

USurfChunk* ASurfGroup::GetChunkWithRoom()
{
	const int32 Capacity = FMath::Max(1, ChunkCapacity);

	// Chunks fill in order; the last one is almost always the open one.
	for (int32 I = Chunks.Num() - 1; I >= 0; --I)
	{
		if (Chunks[I] && Chunks[I]->Num() < Capacity)
		{
			return Chunks[I];
		}
	}

	// A new chunk changes the actor's pointer array, which is the only time
	// the actor itself has to be transacted for a geometry edit.
	Modify();

	USurfChunk* Chunk = NewObject<USurfChunk>(this, NAME_None, RF_Transactional);
	Chunks.Add(Chunk);
	return Chunk;
}

int32 ASurfGroup::NumBrushes() const
{
	int32 Total = 0;
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (Chunk)
		{
			Total += Chunk->Brushes.Num();
		}
	}
	return Total;
}

int32 ASurfGroup::NumPatches() const
{
	int32 Total = 0;
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (Chunk)
		{
			Total += Chunk->Patches.Num();
		}
	}
	return Total;
}

void ASurfGroup::ForEachBrush(TFunctionRef<void(const FSurfBrush&)> Visit) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfBrush& Brush : Chunk->Brushes)
		{
			if (!Brush.bHidden)
			{
				Visit(Brush);
			}
		}
	}
}

void ASurfGroup::ForEachBrushIncludingHidden(TFunctionRef<void(const FSurfBrush&)> Visit) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfBrush& Brush : Chunk->Brushes)
		{
			Visit(Brush);
		}
	}
}

void ASurfGroup::ForEachPatch(TFunctionRef<void(const FSurfPatch&)> Visit) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfPatch& Patch : Chunk->Patches)
		{
			if (Patch.bHidden || (IsolationHidden.Num() > 0 && IsolationHidden.Contains(Patch.Id)))
			{
				continue;
			}
			Visit(Patch);
		}
	}
}

void ASurfGroup::ForEachPatchIncludingHidden(TFunctionRef<void(const FSurfPatch&)> Visit) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfPatch& Patch : Chunk->Patches)
		{
			Visit(Patch);
		}
	}
}

int32 ASurfGroup::NumHiddenPatches() const
{
	int32 Count = 0;
	ForEachPatchIncludingHidden([&Count](const FSurfPatch& Patch)
	{
		if (Patch.bHidden)
		{
			++Count;
		}
	});
	return Count;
}

const FSurfBrush* ASurfGroup::FindBrush(const FGuid& Id) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfBrush& Brush : Chunk->Brushes)
		{
			if (Brush.Id == Id)
			{
				return &Brush;
			}
		}
	}
	return nullptr;
}

const FSurfPatch* ASurfGroup::FindPatch(const FGuid& Id) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (const FSurfPatch& Patch : Chunk->Patches)
		{
			if (Patch.Id == Id)
			{
				return &Patch;
			}
		}
	}
	return nullptr;
}

USurfChunk* ASurfGroup::FindChunkForBrush(const FGuid& Id) const
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (Chunk && const_cast<USurfChunk*>(Chunk.Get())->FindBrush(Id))
		{
			return Chunk;
		}
	}
	return nullptr;
}

FSurfBrush* ASurfGroup::ModifyBrush(const FGuid& Id)
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}

		if (FSurfBrush* Brush = Chunk->FindBrush(Id))
		{
			// Only this chunk enters the transaction. That is D21 in one line.
			Chunk->Modify();
			return Chunk->FindBrush(Id);
		}
	}
	return nullptr;
}

FSurfPatch* ASurfGroup::ModifyPatch(const FGuid& Id)
{
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}

		if (Chunk->FindPatch(Id))
		{
			Chunk->Modify();
			return Chunk->FindPatch(Id);
		}
	}
	return nullptr;
}

FSurfBrush* ASurfGroup::ModifyBrushAt(int32 LinearIndex)
{
	int32 Remaining = LinearIndex;
	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}

		if (Remaining < Chunk->Brushes.Num())
		{
			Chunk->Modify();
			return &Chunk->Brushes[Remaining];
		}

		Remaining -= Chunk->Brushes.Num();
	}
	return nullptr;
}

FSurfBrush& ASurfGroup::AddBrush(FSurfBrush&& Brush)
{
	USurfChunk* Chunk = GetChunkWithRoom();
	Chunk->Modify();

	const int32 Index = Chunk->Brushes.Add(MoveTemp(Brush));
	return Chunk->Brushes[Index];
}

FSurfPatch& ASurfGroup::AddPatch(FSurfPatch&& Patch)
{
	USurfChunk* Chunk = GetChunkWithRoom();
	Chunk->Modify();

	const int32 Index = Chunk->Patches.Add(MoveTemp(Patch));
	return Chunk->Patches[Index];
}

bool ASurfGroup::RemoveBrush(const FGuid& Id)
{
	for (int32 C = 0; C < Chunks.Num(); ++C)
	{
		USurfChunk* Chunk = Chunks[C];
		if (!Chunk)
		{
			continue;
		}

		const int32 Index = Chunk->Brushes.IndexOfByPredicate([&Id](const FSurfBrush& B) { return B.Id == Id; });
		if (Index == INDEX_NONE)
		{
			continue;
		}

		Chunk->Modify();
		Chunk->Brushes.RemoveAt(Index);

		if (Chunk->IsEmpty())
		{
			Modify();
			Chunks.RemoveAt(C);
		}
		return true;
	}
	return false;
}

bool ASurfGroup::RemovePatch(const FGuid& Id)
{
	for (int32 C = 0; C < Chunks.Num(); ++C)
	{
		USurfChunk* Chunk = Chunks[C];
		if (!Chunk)
		{
			continue;
		}

		const int32 Index = Chunk->Patches.IndexOfByPredicate([&Id](const FSurfPatch& B) { return B.Id == Id; });
		if (Index == INDEX_NONE)
		{
			continue;
		}

		Chunk->Modify();
		Chunk->Patches.RemoveAt(Index);

		if (Chunk->IsEmpty())
		{
			Modify();
			Chunks.RemoveAt(C);
		}
		return true;
	}
	return false;
}

void ASurfGroup::ClearPrimitives()
{
	Modify();

	for (const TObjectPtr<USurfChunk>& Chunk : Chunks)
	{
		if (Chunk)
		{
			Chunk->Modify();
			Chunk->Brushes.Reset();
			Chunk->Patches.Reset();
		}
	}

	Chunks.Reset();
}

// --- Build -----------------------------------------------------------------

TDelegate<UMaterialInterface*()> ASurfGroup::DefaultMaterialProvider;
TDelegate<UMaterialInterface*()> ASurfGroup::PreviewOverrideProvider;
TDelegate<UMaterialInterface*()> ASurfGroup::GhostMaterialProvider;
const FName ASurfGroup::OutlinerFolder(TEXT("SurfacingPro"));
// Assets since S220a: prefabs, meshes and levels placed in the level, one drawer - "use the outliner as an outliner".
const FName ASurfGroup::OutlinerPrefabFolder(TEXT("SurfacingPro/Assets"));

UMaterialInterface* ASurfGroup::GetEffectiveMaterial() const
{
	if (PreviewMaterial)
	{
		return PreviewMaterial.Get();
	}

	return DefaultMaterialProvider.IsBound() ? DefaultMaterialProvider.Execute() : nullptr;
}

void ASurfGroup::RefreshPreviewMaterial()
{
	if (!Preview)
	{
		return;
	}

	// Slot 0 is the fallback, resolved again here so a rebuilt checker or a
	// changed group material reaches every face that wears nothing; the other
	// slots are the materials the primitives wear, fixed at the last rebuild.
	TArray<UMaterialInterface*> Set;
	Set.Reserve(FMath::Max(1, PreviewMaterials.Num()));
	Set.Add(GetEffectiveMaterial());
	for (int32 Slot = 1; Slot < PreviewMaterials.Num(); ++Slot)
	{
		Set.Add(PreviewMaterials[Slot] ? PreviewMaterials[Slot].Get() : GetEffectiveMaterial());
	}

	// The override, when there is one, on every slot: the slots stay as
	// they are so the mesh's material ids still resolve, and every one of
	// them shows the same thing.
	UMaterialInterface* Override = PreviewOverrideProvider.IsBound() ? PreviewOverrideProvider.Execute() : nullptr;
	if (bGhosted && GhostMaterialProvider.IsBound())
	{
		// The ghost over everything, the paint view included: a group out
		// of reach shows nothing that asks to be painted.
		if (UMaterialInterface* Ghost = GhostMaterialProvider.Execute())
		{
			Override = Ghost;
		}
	}
	if (Override)
	{
		for (UMaterialInterface*& Each : Set)
		{
			Each = Override;
		}
	}
	Preview->ConfigureMaterialSet(Set, /*bDeleteExtraSlots*/ true);

	// The shaded checker is a modelling view: it neither takes nor throws
	// shadows, which is most of what a mesh costs a lit level. A group that
	// wears materials of its own is lit like any mesh.
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const bool bOwnMaterials = PreviewMaterial != nullptr || Set.Num() > 1;
	const bool bCastShadow = bOwnMaterials || !Settings || Settings->PreviewShade == ESurfPreviewShade::Lit;
	if (Preview->CastShadow != bCastShadow)
	{
		Preview->SetCastShadow(bCastShadow);
	}
}

// --- The incremental rebuild (S66b) -----------------------------------------------
//
// A drag rebuilt the whole group on every move, which is what made a big
// group feel heavy: a thousand patches tessellated so that one could move.
// The mesh in the component already holds every patch's triangles under
// that patch's group id; when the group's shape is what it was - the same
// patches in the same order, the same brushes, the same slots - and only a
// few fingerprints differ, those patches' triangles are taken out and put
// back from a fresh tessellation, and nothing else is touched.
//
// The fingerprint reads the state itself, not only EditSerial, so a path
// that forgot to bump the serial still gets its patch rebuilt: a stale
// preview is the one failure this must never have.

uint32 ASurfGroup::FingerprintOf(const FSurfPatch& Patch)
{
	uint32 H = GetTypeHash(Patch.EditSerial);
	H = HashCombine(H, GetTypeHash(Patch.Points.Num()));
	H = HashCombine(H, GetTypeHash(Patch.Width));
	H = HashCombine(H, GetTypeHash(Patch.Height));
	H = HashCombine(H, GetTypeHash(Patch.SubdivU));
	H = HashCombine(H, GetTypeHash(Patch.SubdivV));
	H = HashCombine(H, GetTypeHash(Patch.StackLevel));
	H = HashCombine(H, GetTypeHash(static_cast<int32>(Patch.Mapping)));
	H = HashCombine(H, GetTypeHash(static_cast<int32>(Patch.Shading)));
	H = HashCombine(H, GetTypeHash(Patch.bInvert ? 1 : 0));
	H = HashCombine(H, GetTypeHash(Patch.SmoothingAngleDegrees));
	for (int32 Row : Patch.HardRows) { H = HashCombine(H, GetTypeHash(Row * 2 + 1)); }
	for (int32 Column : Patch.HardColumns) { H = HashCombine(H, GetTypeHash(Column * 2)); }
	H = HashCombine(H, GetTypeHash(Patch.Scale));
	H = HashCombine(H, GetTypeHash(Patch.UVRotation));
	H = HashCombine(H, GetTypeHash(Patch.UVShift));
	H = HashCombine(H, GetTypeHash((Patch.bUVSwap ? 1 : 0) | (Patch.bUVFlipU ? 2 : 0) | (Patch.bUVFlipV ? 4 : 0)));
	H = HashCombine(H, GetTypeHash(Patch.Material.Get()));
	for (const FSurfControlPoint& Point : Patch.Points)
	{
		H = HashCombine(H, GetTypeHash(Point.Pos));
		H = HashCombine(H, GetTypeHash(Point.UV));
		H = HashCombine(H, GetTypeHash(Point.Weight));
		H = HashCombine(H, GetTypeHash(Point.Tint.DWColor()));
		H = HashCombine(H, GetTypeHash(static_cast<int32>(Point.bTurnedEdge)));
	}
	return H;
}

uint32 ASurfGroup::FingerprintOfBrushes() const
{
	uint32 H = GetTypeHash(NumBrushes());
	ForEachBrush([&H](const FSurfBrush& Brush)
	{
		H = HashCombine(H, GetTypeHash(Brush.EditSerial));
		H = HashCombine(H, GetTypeHash(Brush.CreaseAngleDeg));
		H = HashCombine(H, GetTypeHash(Brush.Faces.Num()));
		for (const FSurfFace& Face : Brush.Faces)
		{
			H = HashCombine(H, GetTypeHash(FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z)));
			H = HashCombine(H, GetTypeHash(Face.Plane.W));
			H = HashCombine(H, GetTypeHash(Face.UV.Scale));
			H = HashCombine(H, GetTypeHash(Face.UV.Shift));
			H = HashCombine(H, GetTypeHash(Face.Material.Get()));
		}
	});
	return H;
}

bool ASurfGroup::TryIncrementalRebuild(double Start)
{
	if (!Preview || !bPreviewBuilt)
	{
		return false;
	}

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double LayerOffsetCm = Settings ? Settings->LayerOffsetCm : 0.5;
	if (!FMath::IsNearlyEqual(LayerOffsetCm, BuiltLayerOffsetCm))
	{
		return false;
	}
	if (BuiltSlots.Num() == 0 || BuiltSlots[0].Get() != GetEffectiveMaterial())
	{
		return false;
	}
	if (FingerprintOfBrushes() != BuiltBrushFingerprint)
	{
		return false;
	}

	// The same patches, in the same order, or it is a different mesh.
	TArray<FGuid> Order;
	TArray<const FSurfPatch*> Changed;
	TArray<uint32> ChangedFingerprints;
	Order.Reserve(BuiltPatchOrder.Num());
	bool bSameShape = true;
	ForEachPatch([&](const FSurfPatch& Patch)
	{
		Order.Add(Patch.Id);
		const uint32* Was = BuiltPatchFingerprints.Find(Patch.Id);
		if (!Was)
		{
			bSameShape = false;
			return;
		}
		const uint32 Now = FingerprintOf(Patch);
		if (Now != *Was)
		{
			Changed.Add(&Patch);
			ChangedFingerprints.Add(Now);
		}
	});
	if (!bSameShape || Order != BuiltPatchOrder)
	{
		return false;
	}
	if (Changed.Num() == 0)
	{
		LastRebuildReplaced = 0;
		return true;
	}

	// Past a quarter of the patches, tessellating everything is the cheaper
	// path and the simpler one.
	if (Changed.Num() > FMath::Max(4, Order.Num() / 4))
	{
		return false;
	}

	// The slots exactly as the full build would make them - brushes first,
	// then the patches in order. Any difference, a material newly worn or
	// one no longer worn, is a full build, so the component's slots are
	// never stale (the Materials tests check that a dropped material loses
	// its slot).
	FSurfMaterialSet Materials;
	Materials.Reset(BuiltSlots[0].Get());
	ForEachBrush([&Materials](const FSurfBrush& Brush)
	{
		for (const FSurfFace& Face : Brush.Faces)
		{
			Materials.SlotFor(Face.Material.Get());
		}
	});
	ForEachPatch([&Materials](const FSurfPatch& Patch)
	{
		Materials.SlotFor(Patch.Material.Get());
	});
	if (Materials.Num() != BuiltSlots.Num())
	{
		return false;
	}
	for (int32 Slot = 0; Slot < BuiltSlots.Num(); ++Slot)
	{
		if (Materials.Slots[Slot] != BuiltSlots[Slot].Get())
		{
			return false;
		}
	}
	TArray<int32> Slots;
	for (const FSurfPatch* Patch : Changed)
	{
		Slots.Add(Materials.SlotFor(Patch->Material.Get()));
	}

	Preview->EditMesh([&](FDynamicMesh3& Mesh)
	{
		FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
		if (!Attributes)
		{
			return;
		}
		FDynamicMeshNormalOverlay* NormalOverlay = Attributes->PrimaryNormals();
		FDynamicMeshUVOverlay* UVOverlay = Attributes->PrimaryUV();
		FDynamicMeshColorOverlay* ColourOverlay = Attributes->PrimaryColors();
		FDynamicMeshMaterialAttribute* MaterialIDs = Attributes->GetMaterialID();

		for (int32 I = 0; I < Changed.Num(); ++I)
		{
			const FSurfPatch& Patch = *Changed[I];
			const int32* Group = PatchTriangleGroups.Find(Patch.Id);
			if (!Group)
			{
				continue;
			}

			// Out with the old triangles of this patch - and the vertices
			// they alone used, since a patch shares none with its
			// neighbours - and in with the new, under the same group id,
			// so the visibility test and the next rebuild still find them.
			TArray<int32> Gone;
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				if (Mesh.GetTriangleGroup(Tid) == *Group)
				{
					Gone.Add(Tid);
				}
			}
			for (const int32 Tid : Gone)
			{
				Mesh.RemoveTriangle(Tid, /*bRemoveIsolatedVertices*/ true);
			}

			AppendPatch(Mesh, NormalOverlay, UVOverlay, ColourOverlay, Patch, LayerOffsetCm, *Group, MaterialIDs, Slots[I]);
		}
	});

	for (int32 I = 0; I < Changed.Num(); ++I)
	{
		BuiltPatchFingerprints.Add(Changed[I]->Id, ChangedFingerprints[I]);
	}

	++RebuildSerial;
	LastRebuildReplaced = Changed.Num();
	Preview->ProcessMesh([this](const FDynamicMesh3& Mesh) { TriangleCount = Mesh.TriangleCount(); });
	PrimitiveCount = NumPrimitives();
	ChunkCount = Chunks.Num();

	if (bBaked)
	{
		// Only while the bake is really standing there (S73a).
		if (BakeIsShowing())
		{
			ApplyBakeLook();
		}
		else
		{
			DropStaleBake();
		}
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	LastRebuildMs = static_cast<float>(ElapsedMs);
	SURF_METRIC_N("preview.incremental_rebuild_ms", ElapsedMs, "ms", Changed.Num());
	return true;
}

double ASurfGroup::RebuildPreview()
{
	const double Start = FPlatformTime::Seconds();

	// A bake that is not standing there is not a bake: the geometry comes
	// back rather than the level showing wires over nothing. First, because
	// a rebuild with nothing to do returns without touching the preview.
	if (bBaked && !BakeIsShowing())
	{
		DropStaleBake();
	}

	if (TryIncrementalRebuild(Start))
	{
		return LastRebuildMs;
	}

	FDynamicMesh3 Mesh;
	Mesh.EnableAttributes();
	Mesh.EnableTriangleGroups(0);

	FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
	Attributes->SetNumUVLayers(1);

	FDynamicMeshNormalOverlay* NormalOverlay = Attributes->PrimaryNormals();
	FDynamicMeshUVOverlay* UVOverlay = Attributes->PrimaryUV();

	// The channel a layer is painted into. Without it the weight lives on the
	// control net and reaches nothing: the mask a stacked layer is read
	// through is vertex alpha, at bake and in the preview alike (D14).
	Attributes->EnablePrimaryColors();
	FDynamicMeshColorOverlay* ColourOverlay = Attributes->PrimaryColors();

	// One material slot per distinct material the primitives wear, slot 0
	// the fallback: the component renders each slot with its own material.
	Attributes->EnableMaterialID();
	FDynamicMeshMaterialAttribute* MaterialIDs = Attributes->GetMaterialID();
	FSurfMaterialSet Materials;
	Materials.Reset(GetEffectiveMaterial());

	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const double LayerOffsetCm = Settings ? Settings->LayerOffsetCm : 0.5;

	BrushTriangleGroups.Reset();
	int32 NextBrushGroup = BrushTriangleGroupBase;
	ForEachBrush([&](const FSurfBrush& Brush)
	{
		BrushTriangleGroups.Add(Brush.Id, NextBrushGroup);
		AppendBrush(Mesh, NormalOverlay, UVOverlay, ColourOverlay, MaterialIDs, Materials, Brush, NextBrushGroup);
		++NextBrushGroup;
	});

	PatchTriangleGroups.Reset();
	BuiltPatchOrder.Reset();
	BuiltPatchFingerprints.Reset();
	int32 NextGroup = 1;
	ForEachPatch([&](const FSurfPatch& Patch)
	{
		PatchTriangleGroups.Add(Patch.Id, NextGroup);
		BuiltPatchOrder.Add(Patch.Id);
		BuiltPatchFingerprints.Add(Patch.Id, FingerprintOf(Patch));
		AppendPatch(Mesh, NormalOverlay, UVOverlay, ColourOverlay, Patch, LayerOffsetCm, NextGroup, MaterialIDs, Materials.SlotFor(Patch.Material.Get()));
		++NextGroup;
	});

	// What this build was, for the next one to compare against.
	BuiltBrushFingerprint = FingerprintOfBrushes();
	BuiltSlots.Reset(Materials.Num());
	for (UMaterialInterface* Slot : Materials.Slots)
	{
		BuiltSlots.Add(Slot);
	}
	BuiltLayerOffsetCm = LayerOffsetCm;
	bPreviewBuilt = Preview != nullptr;
	LastRebuildReplaced = -1;

	++RebuildSerial;

	TriangleCount = Mesh.TriangleCount();
	PrimitiveCount = NumPrimitives();
	ChunkCount = Chunks.Num();

	PreviewMaterials.Reset(Materials.Num());
	for (UMaterialInterface* Slot : Materials.Slots)
	{
		PreviewMaterials.Add(Slot);
	}

	if (Preview)
	{
		// The slots first, so the proxy the new mesh gets is built with the
		// right number of materials; a triangle whose slot the component
		// does not have is not drawn.
		RefreshPreviewMaterial();
		Preview->SetMesh(MoveTemp(Mesh));

		// Baked: the meshes stand in and the preview stays hidden, whatever
		// rebuilt it - a load, an undo. An edit un-bakes through the chunk.
		// Unless the bake is not there to stand in: then the geometry comes
		// back rather than the level coming up as wires over nothing (S73a).
		if (bBaked)
		{
			if (BakeIsShowing())
			{
				ApplyBakeLook();
			}
			else
			{
				DropStaleBake();
			}
		}

		const bool bCollide = WantsCollision();
		if (bCollide != (Preview->GetCollisionEnabled() != ECollisionEnabled::NoCollision))
		{
			SetPreviewCollision(bCollide);
		}
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	LastRebuildMs = static_cast<float>(ElapsedMs);

	SURF_METRIC_N("preview.full_rebuild_ms", ElapsedMs, "ms", FMath::Max(1, PrimitiveCount));
	return ElapsedMs;
}

FSurfBrush& ASurfGroup::AppendBox(const FBox& LocalBox)
{
	FSurfBrush Brush = SurfBrushMath::MakeBox(LocalBox);

	// Axial projection at one metre per tile, so a textured material reads
	// correctly the moment it is assigned.
	for (FSurfFace& Face : Brush.Faces)
	{
		SurfProjection::Align(Face.UV, FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z));
	}

	return AddBrush(MoveTemp(Brush));
}

// --- Buttons ---------------------------------------------------------------

void ASurfGroup::AddBox()
{
	const double Offset = NumBrushes() * 150.0;
	AppendBox(FBox(FVector(Offset, 0.0, 0.0), FVector(Offset + 100.0, 100.0, 100.0)));
	RebuildPreview();
}

void ASurfGroup::AddCurve()
{
	const double Offset = NumPatches() * 250.0;
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 3, 3,
		FVector(Offset, 200.0, 0.0),
		FVector(200.0, 0.0, 0.0),
		FVector(0.0, 200.0, 0.0));

	// Raise the middle column so the bezier is visible rather than a flat quad.
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		Patch.At(1, Y).Pos.Z += 150.0;
	}

	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		for (int32 X = 0; X < Patch.Width; ++X)
		{
			FSurfControlPoint& Point = Patch.At(X, Y);
			Point.UV = FVector2D(X, Y);
			Point.SavedUV = Point.UV;
		}
	}

	AddPatch(MoveTemp(Patch));
	RebuildPreview();
}

void ASurfGroup::AddSheet()
{
	const double Offset = NumPatches() * 250.0;
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 5, 5,
		FVector(Offset, -450.0, 0.0),
		FVector(400.0, 0.0, 0.0),
		FVector(0.0, 400.0, 0.0));

	// A gentle mound, so the pass-through tessellator has something to show.
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		for (int32 X = 0; X < Patch.Width; ++X)
		{
			FSurfControlPoint& Point = Patch.At(X, Y);
			const double DX = (X - 2.0) / 2.0;
			const double DY = (Y - 2.0) / 2.0;
			Point.Pos.Z += 120.0 * FMath::Max(0.0, 1.0 - (DX * DX + DY * DY));
			Point.UV = FVector2D(X, Y);
			Point.SavedUV = Point.UV;
		}
	}

	AddPatch(MoveTemp(Patch));
	RebuildPreview();
}

void ASurfGroup::ClearAll()
{
	ClearPrimitives();
	RebuildPreview();
}

void ASurfGroup::Rebuild()
{
	RebuildPreview();
}

void ASurfGroup::StressTest()
{
	ClearPrimitives();

	const int32 Count = FMath::Max(1, StressCount);
	const int32 Side = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<double>(Count))));

	for (int32 I = 0; I < Count; ++I)
	{
		const double X = (I % Side) * 300.0;
		const double Y = (I / Side) * 300.0;
		AppendBox(FBox(FVector(X, Y, 0.0), FVector(X + 200.0, Y + 200.0, 300.0)));
	}

	const double Ms = RebuildPreview();

	SURF_DIAG("stress", TEXT("primitives=%d chunks=%d tris=%d rebuild_ms=%.2f"),
		NumPrimitives(), Chunks.Num(), TriangleCount, Ms);
}

void ASurfGroup::MeasureEditCost()
{
	if (NumBrushes() == 0)
	{
		SURF_DIAG("editcost", TEXT("skipped: no brushes"));
		return;
	}

	const int32 Iterations = FMath::Max(1, EditIterations);
	const double Start = FPlatformTime::Seconds();

	for (int32 I = 0; I < Iterations; ++I)
	{
		if (FSurfBrush* Brush = ModifyBrushAt(0))
		{
			Brush->Faces[0].Plane.W += (I % 2 == 0) ? 1.0 : -1.0;
			++Brush->EditSerial;
		}

		RebuildPreview();
	}

	const double TotalMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	SURF_METRIC_N("preview.edit_roundtrip_ms", TotalMs, "ms", Iterations);
	SURF_DIAG("editcost", TEXT("primitives=%d iterations=%d per_edit_ms=%.3f"),
		NumPrimitives(), Iterations, TotalMs / Iterations);
}

// --- Lifecycle -------------------------------------------------------------

void ASurfGroup::SetPreviewCollision(bool bOn)
{
	if (!Preview)
	{
		return;
	}

	// The whole surface, as it is drawn, is what collides: a patch is walked
	// on as it looks. Turned on, the collision mesh is cooked from the
	// preview; turned off, it is dropped and editing pays nothing for it.
	Preview->SetCollisionProfileName(bOn ? TEXT("BlockAll") : TEXT("NoCollision"));
	Preview->SetCollisionEnabled(bOn ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	Preview->SetComplexAsSimpleCollisionEnabled(bOn, /*bImmediateUpdate*/ true);
}

bool ASurfGroup::WantsCollision() const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	const UWorld* World = GetWorld();

	// A play world is a copy of the editor's, rebuilt here as it is
	// registered, and nothing set in the editor survives that copy - which is
	// why switching collision on just before play did nothing. The world
	// itself says which rule applies.
	if (World && World->IsGameWorld())
	{
		// The bake has its own collision; the preview behind it has none.
		if (bBaked && BakeIsShowing())
		{
			return false;
		}
		return !Settings || Settings->bCollisionWhilePlaying;
	}
	return Settings && Settings->bPreviewCollision;
}

bool ASurfGroup::BakeIsShowing() const
{
	for (const TObjectPtr<AActor>& Actor : BakedActors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}
		if (const UStaticMeshComponent* Mesh = Actor->FindComponentByClass<UStaticMeshComponent>())
		{
			if (Mesh->GetStaticMesh())
			{
				return true;
			}
		}
	}
	return false;
}

void ASurfGroup::DropStaleBake()
{
	bBaked = false;
	ApplyBakeLook();
}

bool ASurfGroup::bShowBakeInEditor = false;
FSimpleMulticastDelegate ASurfGroup::OnEditedInGameView;
bool ASurfGroup::bModeUp = false;

void ASurfGroup::ApplyBakeLook()
{
	const bool bStanding = bBaked && BakeIsShowing();
	// Game View (S154e): the editor's own game view cannot draw an actor
	// hidden in the editor - a hidden actor has no render state at all in
	// an editor world - so showing the bake means un-hiding the stand-ins
	// and hiding the preview, for as long as that view is on.
	const bool bBakeSide = bStanding && bShowBakeInEditor;
	if (Preview)
	{
		Preview->SetVisibility(!bBakeSide, false);
		Preview->SetHiddenInGame(bStanding);
	}
#if WITH_EDITOR
	// Editor-only flags, and only the editor has stand-ins to hide: in the
	// game they are what is drawn.
	for (const TObjectPtr<AActor>& Actor : BakedActors)
	{
		if (IsValid(Actor))
		{
			// bHiddenEd is the saved flag, read at load; the temporary one is
			// what the viewport reads now.
			Actor->bHiddenEd = true;
			Actor->SetIsTemporarilyHiddenInEditor(!bBakeSide);
		}
	}
#endif
}

void ASurfGroup::Unbake()
{
	if (!bBaked && BakedActors.Num() == 0)
	{
		return;
	}
	for (TObjectPtr<AActor>& Actor : BakedActors)
	{
		if (Actor)
		{
			Actor->Destroy();
		}
	}
	BakedActors.Reset();
	bBaked = false;
	ApplyBakeLook();
}

void ASurfGroup::NoteChunkEdited()
{
	// An edit in the game's view (S200a): "good idea for switching back to
	// editor view first" - the mode hears it and turns the view back.
	if (bShowBakeInEditor)
	{
		OnEditedInGameView.Broadcast();
	}

	// A bake of geometry that has just changed is a bake of something else;
	// it goes, and the preview is back for the edit to be seen.
	if (bBaked)
	{
		Modify();
		Unbake();
	}
}

int32 ASurfGroup::FlagsOfTriangleGroup(int32 TriangleGroup) const
{
	if (TriangleGroup >= BrushTriangleGroupBase)
	{
		for (const TPair<FGuid, int32>& Pair : BrushTriangleGroups)
		{
			if (Pair.Value == TriangleGroup)
			{
				const FSurfBrush* Brush = FindBrush(Pair.Key);
				return Brush ? Brush->Flags : 0;
			}
		}
		return 0;
	}
	for (const TPair<FGuid, int32>& Pair : PatchTriangleGroups)
	{
		if (Pair.Value == TriangleGroup)
		{
			const FSurfPatch* Patch = FindPatch(Pair.Key);
			return Patch ? Patch->Flags : 0;
		}
	}
	return 0;
}

void ASurfGroup::RefreshIdentity()
{
	GroupId = FGuid::NewGuid();

	// A copy is geometry, not the level's home for geometry (S149b): two
	// groups both answering to the mark is the bug it exists to stop.
	bLevelGroup = false;

	for (USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		for (FSurfBrush& Brush : Chunk->Brushes)
		{
			Brush.Id = FGuid::NewGuid();
		}
		for (FSurfPatch& Patch : Chunk->Patches)
		{
			Patch.Id = FGuid::NewGuid();
		}
	}
}

void ASurfGroup::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);

	// A copy for play keeps its identity - it is the same level, running. Any
	// other copy is a new group and must not answer to its source's ids.
	if (DuplicateMode != EDuplicateMode::PIE)
	{
		RefreshIdentity();
	}
}

#if WITH_EDITOR
void ASurfGroup::PostEditImport()
{
	Super::PostEditImport();

	// Paste, and the editor's own duplicate, arrive here with the chunks
	// already copied (they are instanced). The components were registered
	// before the primitives existed, so the preview has to be built now.
	RefreshIdentity();
	RebuildPreview();
}
#endif

int32 ASurfGroup::DropEmptyPatches()
{
	int32 Dropped = 0;
	for (USurfChunk* Chunk : Chunks)
	{
		if (!Chunk)
		{
			continue;
		}
		Chunk->ConditionalPostLoad();
		for (int32 I = Chunk->Patches.Num() - 1; I >= 0; --I)
		{
			if (SurfPatchOps::HasNoSurface(Chunk->Patches[I]))
			{
				Chunk->Patches.RemoveAt(I);
				++Dropped;
			}
		}
	}
	if (Dropped > 0)
	{
		SurfSession::Verb(TEXT("group_empty_dropped"), FString::Printf(TEXT("%s patches=%d"), *GetName(), Dropped));
	}
	return Dropped;
}

void ASurfGroup::PostLoad()
{
	Super::PostLoad();

	if (!GroupId.IsValid())
	{
		GroupId = FGuid::NewGuid();
	}

	// A patch with no surface is nobody's (S245a): a line or a point left
	// by an extrude undone halfway and saved with the level.
#if WITH_EDITOR
	if (DropEmptyPatches() > 0)
	{
		MarkPackageDirty();
	}
#endif

	/**
	 * And a level made before this existed (S126a).
	 *
	 * "atm im not seeing my current level surfacing in the outliner" - the
	 * folder was only ever set in PostActorCreated, which fires when an actor
	 * is spawned and never when one is loaded. Every group in every level he
	 * had already built sat loose at the root, which is the one place a tidy
	 * outliner does not look.
	 */
#if WITH_EDITOR
	TidyOutlinerFolder();
#endif
}

void ASurfGroup::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// A saved level must show its geometry without anyone pressing a button.
	if (NumPrimitives() > 0)
	{
		RebuildPreview();
	}
}

#if WITH_EDITOR
void ASurfGroup::PostEditUndo()
{
	Super::PostEditUndo();

	// The chunk's data has already been restored; the preview is a build
	// product and has to follow it.
	RebuildPreview();
}
#endif

#if WITH_EDITOR
void ASurfGroup::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASurfGroup, PreviewMaterial))
	{
		RefreshPreviewMaterial();
	}
}
#endif
