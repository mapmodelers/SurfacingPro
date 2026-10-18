// Copyright MapModelers. All Rights Reserved.

#include "SurfPrefabThumbnail.h"

#include "SurfGroup.h"
#include "SurfMaterialSet.h"
#include "SurfPrefab.h"
#include "SurfSettings.h"

#include "CanvasItem.h"
#include "TextureResource.h"
#include "CanvasTypes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

using UE::Geometry::FDynamicMesh3;

bool USurfPrefabThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
	return Cast<USurfPrefab>(Object) != nullptr;
}

void USurfPrefabThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
	FRenderTarget* Target, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	USurfPrefab* Prefab = Cast<USurfPrefab>(Object);
	if (!Prefab || !Canvas || Width < 8 || Height < 8)
	{
		return;
	}

	const double W = static_cast<double>(Width);
	const double H = static_cast<double>(Height);

	// The background first, so an empty prefab still reads as a prefab
	// rather than as a hole.
	Canvas->DrawTile(X, Y, W, H, 0.0f, 0.0f, 1.0f, 1.0f, FLinearColor(0.07f, 0.075f, 0.09f, 1.0f));

	// The geometry, the prefab's own and its placements', in the prefab's
	// own space - the same walk an instance draws.
	// The mesh is prepared by AppendPrefabToMesh, all of it. Enabling the
	// attributes here half-prepared it and crashed the editor (S85b).
	FDynamicMesh3 Mesh;
	FSurfMaterialSet Materials;
	Materials.Reset(nullptr);
	ASurfGroup::AppendPrefabToMesh(Mesh, *Prefab, FTransform::Identity, Materials,
		/*LayerOffsetCm*/ 0.0, /*bIncludeOwnGeometry*/ true, /*OutRanges*/ nullptr);

	const FBox Bounds = Prefab->LocalBounds();
	const int32 Triangles = Mesh.TriangleCount();

	if (Triangles > 0 && Bounds.IsValid)
	{
		// Seen from the front and above, as a hand holds a thing it is
		// about to put down. One projection, no camera: X to the right, Y
		// into the picture, Z up.
		const FVector Centre = Bounds.GetCenter();
		const double Extent = FMath::Max(1.0, Bounds.GetExtent().GetMax());
		const double Scale = (FMath::Min(W, H) * 0.40) / Extent;

		auto Project = [&](const FVector& P) -> FVector2D
		{
			const FVector Local = P - Centre;
			const double Sx = (Local.X * 0.92 - Local.Y * 0.92) * Scale;
			const double Sy = (-(Local.X + Local.Y) * 0.36 - Local.Z * 0.86) * Scale;
			return FVector2D(X + W * 0.5 + Sx, Y + H * 0.52 + Sy);
		};

		/**
		 * Filled and lit, not wires (S107a): "prefab thumbnail are wireframe -
		 * may they be regular shading render?".
		 *
		 * There is no scene and no camera here - a thumbnail renderer is handed
		 * a canvas and some pixels - so this is the whole of it: sort the
		 * triangles back to front, fill each one flat, and shade it by how
		 * square it stands to a light over the viewer's shoulder. Far to near
		 * is what makes it read as a solid; a painter did it that way for
		 * centuries before anyone had a depth buffer.
		 *
		 * Two-sided on purpose. A thumbnail has no business being the place a
		 * facing problem shows up, and half a prefab is usually open geometry
		 * anyway - patches are surfaces, not solids.
		 */
		const FVector Into = FVector(0.64, 0.64, -0.53).GetSafeNormal();
		const FVector Light = FVector(-0.35, -0.45, -0.82).GetSafeNormal();

		struct FFacet
		{
			FVector2D A, B, C;
			double Depth = 0.0;
			float Shade = 0.0f;
		};

		TArray<FFacet> Facets;
		Facets.Reserve(FMath::Min(Mesh.TriangleCount(), 20000));
		for (const int32 Tid : Mesh.TriangleIndicesItr())
		{
			if (Facets.Num() >= 20000)
			{
				break;
			}
			FVector A, B, C;
			Mesh.GetTriVertices(Tid, A, B, C);

			FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
			if (Normal.IsNearlyZero())
			{
				continue;
			}
			// Towards the eye whichever way it was wound.
			if (FVector::DotProduct(Normal, Into) > 0.0)
			{
				Normal = -Normal;
			}

			FFacet& Facet = Facets.AddDefaulted_GetRef();
			Facet.A = Project(A);
			Facet.B = Project(B);
			Facet.C = Project(C);
			Facet.Depth = FVector::DotProduct((A + B + C) / 3.0, Into);

			const double Lambert = FMath::Clamp(FVector::DotProduct(Normal, -Light), 0.0, 1.0);
			// A floor of ambient, so a face turned away is dark and not black.
			Facet.Shade = static_cast<float>(0.22 + 0.78 * Lambert);
		}

		// Furthest first.
		Facets.Sort([](const FFacet& L, const FFacet& R) { return L.Depth > R.Depth; });

		const FLinearColor Body(0.66f, 0.70f, 0.78f, 1.0f);
		for (const FFacet& Facet : Facets)
		{
			FCanvasTriangleItem Triangle(Facet.A, Facet.B, Facet.C, GWhiteTexture);
			Triangle.SetColor(FLinearColor(
				Body.R * Facet.Shade, Body.G * Facet.Shade, Body.B * Facet.Shade, 1.0f));
			Canvas->DrawItem(Triangle);
		}
	}

	// What it is made of, along the bottom, for the sizes where the picture
	// is too small to read: pieces, the things it carries, what it places.
	if (Width >= 96 && GEngine && GEngine->GetTinyFont())
	{
		const FString Line = FString::Printf(TEXT("%d piece%s%s%s"),
			Prefab->NumPrimitives(), Prefab->NumPrimitives() == 1 ? TEXT("") : TEXT("s"),
			Prefab->NumParts() > 0 ? *FString::Printf(TEXT("  %d carried"), Prefab->NumParts()) : TEXT(""),
			Prefab->Placements.Num() > 0 ? *FString::Printf(TEXT("  %d inside"), Prefab->Placements.Num()) : TEXT(""));

		Canvas->DrawTile(X, Y + H - 14.0, W, 14.0, 0.0f, 0.0f, 1.0f, 1.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.55f));
		FCanvasTextItem Text(FVector2D(X + 4.0, Y + H - 12.0), FText::FromString(Line),
			GEngine->GetTinyFont(), FLinearColor(0.78f, 0.80f, 0.84f, 1.0f));
		Canvas->DrawItem(Text);
	}
}
