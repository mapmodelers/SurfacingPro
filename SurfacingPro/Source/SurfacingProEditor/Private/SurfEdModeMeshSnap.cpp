// Copyright MapModelers. All Rights Reserved.
//
// Snap to meshes (S59).
//
// A dragged selection lands on the level's static meshes as it lands on
// other patches' control points: one correction for the whole selection,
// the nearest target within reach. A mesh's corner first - the point of
// putting a patch against a wall is almost always its corner - else the
// nearest point on an edge, else on a face. Each mesh is measured through a
// tree built once from its source geometry and kept while the mode is up;
// only the meshes whose bounds the selection could reach are asked. The
// target is drawn while the drag runs, so the hand sees what it is about to
// land on.

#include "SurfEdMode.h"
#include "SurfSnapState.h"
#include "SurfGroup.h"
#include "SurfSettings.h"
#include "SurfSession.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "PrimitiveDrawingUtils.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Distance/DistPoint3Triangle3.h"
#include "SegmentTypes.h"
#include "TriangleTypes.h"

/** One static mesh, as the snap measures it. A null tree marks a mesh that could not be read; it is not asked again. */
struct FSurfMeshSnapEntry
{
	UE::Geometry::FDynamicMesh3 Mesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
};

namespace
{
	TSharedPtr<FSurfMeshSnapEntry> BuildSnapEntry(const UStaticMesh& StaticMesh)
	{
		TSharedPtr<FSurfMeshSnapEntry> Entry = MakeShared<FSurfMeshSnapEntry>();

		// The source geometry, which the editor keeps for every mesh it can
		// edit; the render data may live on the GPU alone.
		const FMeshDescription* Description = const_cast<UStaticMesh&>(StaticMesh).GetMeshDescription(0);
		if (!Description || Description->Vertices().Num() == 0)
		{
			return Entry;
		}

		FStaticMeshConstAttributes Attributes(*Description);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

		int32 Refused = 0;
		TMap<FVertexID, int32> Ids;
		Ids.Reserve(Description->Vertices().Num());
		for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
		{
			Ids.Add(VertexId, Entry->Mesh.AppendVertex(FVector3d(Positions[VertexId])));
		}

		for (const FTriangleID TriangleId : Description->Triangles().GetElementIDs())
		{
			TArrayView<const FVertexID> Vertices = Description->GetTriangleVertices(TriangleId);
			if (Vertices.Num() != 3)
			{
				continue;
			}
			const int32* A = Ids.Find(Vertices[0]);
			const int32* B = Ids.Find(Vertices[1]);
			const int32* C = Ids.Find(Vertices[2]);
			if (A && B && C)
			{
				// A triangle the mesh refuses (a non-manifold edge) is left
				// out; a snap target or two fewer on a mesh like that.
				if (Entry->Mesh.AppendTriangle(*A, *B, *C) < 0)
				{
					++Refused;
				}
			}
		}
		// Once per mesh, in the record: what the snap has to work with.
		SurfSession::Verb(TEXT("snap_mesh_read"), FString::Printf(TEXT("%s vertices=%d triangles=%d refused=%d"),
			*StaticMesh.GetName(), Entry->Mesh.VertexCount(), Entry->Mesh.TriangleCount(), Refused));

		if (Entry->Mesh.TriangleCount() > 0)
		{
			Entry->Tree = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(&Entry->Mesh, true);
		}
		return Entry;
	}
}

bool USurfEdMode::FindMeshSnap(const TArray<FVector>& Moving, FVector& OutCorrection, FVector& OutTarget, double& OutDistanceSquared, ESurfSnapKind& OutKind) const
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	UWorld* World = GetWorld();
	if (!Settings || !SurfSnapState::On() || !Settings->bSnapToMeshes || !World || Moving.Num() == 0)
	{
		return false;
	}

	const double Radius = FMath::Max(0.1, Settings->PointSnapRadiusCm);
	const double RadiusSquared = Radius * Radius;

	// The box the selection could reach: only the meshes it overlaps are asked.
	FBox Reach(ForceInit);
	for (const FVector& From : Moving)
	{
		Reach += From;
	}
	Reach = Reach.ExpandBy(Radius);

	bool bFound = false;
	double BestScore = TNumericLimits<double>::Max();

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor->IsA<ASurfGroup>() || Actor->IsHiddenEd())
		{
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (const UStaticMeshComponent* Component : Components)
		{
			const UStaticMesh* StaticMesh = Component ? Component->GetStaticMesh() : nullptr;
			if (!StaticMesh || Component->IsEditorOnly() || !Component->IsVisible() || !Component->Bounds.GetBox().Intersect(Reach))
			{
				continue;
			}

			TSharedPtr<FSurfMeshSnapEntry>& Entry = MeshSnapCache.FindOrAdd(StaticMesh);
			if (!Entry.IsValid())
			{
				Entry = BuildSnapEntry(*StaticMesh);
			}
			if (!Entry.IsValid() || !Entry->Tree.IsValid())
			{
				continue;
			}

			const FTransform& ToWorld = Component->GetComponentTransform();
			const double MinScale = FMath::Max(1.e-6, ToWorld.GetScale3D().GetAbsMin());

			for (const FVector& From : Moving)
			{
				const FVector3d Local = ToWorld.InverseTransformPosition(From);

				UE::Geometry::FDynamicMeshAABBTree3::FQueryOptions Options;
				Options.MaxDistance = Radius / MinScale;
				double NearestSquared = 0.0;
				const int32 TriangleId = Entry->Tree->FindNearestTriangle(Local, NearestSquared, Options);
				if (TriangleId < 0)
				{
					continue;
				}

				UE::Geometry::FTriangle3d Triangle;
				Entry->Mesh.GetTriVertices(TriangleId, Triangle.V[0], Triangle.V[1], Triangle.V[2]);

				// The nearest corner, edge point and face point of that
				// triangle, in world space.
				FVector Corner = FVector::ZeroVector;
				double CornerSquared = TNumericLimits<double>::Max();
				FVector EdgePoint = FVector::ZeroVector;
				double EdgeSquared = TNumericLimits<double>::Max();
				const UE::Geometry::FIndex3i TriangleVertices = Entry->Mesh.GetTriangle(TriangleId);
				const FVector3d TriangleNormal = Entry->Mesh.GetTriNormal(TriangleId);
				for (int32 I = 0; I < 3; ++I)
				{
					const FVector CornerWorld = ToWorld.TransformPosition(Triangle.V[I]);
					const double Squared = FVector::DistSquared(CornerWorld, From);
					if (Squared < CornerSquared)
					{
						CornerSquared = Squared;
						Corner = CornerWorld;
					}

					// An edge of the mesh, not of its triangulation: a border,
					// or a fold between two triangles that do not lie flat
					// together. The diagonal across a flat quad is no edge.
					const int32 EdgeId = Entry->Mesh.FindEdge(TriangleVertices[I], TriangleVertices[(I + 1) % 3]);
					if (EdgeId >= 0)
					{
						const UE::Geometry::FIndex2i EdgeTriangles = Entry->Mesh.GetEdgeT(EdgeId);
						const int32 Other = EdgeTriangles.A == TriangleId ? EdgeTriangles.B : EdgeTriangles.A;
						if (Other >= 0 && FVector3d::DotProduct(TriangleNormal, Entry->Mesh.GetTriNormal(Other)) > 0.999)
						{
							continue;
						}
					}

					const UE::Geometry::FSegment3d Segment(Triangle.V[I], Triangle.V[(I + 1) % 3]);
					const FVector OnEdge = ToWorld.TransformPosition(Segment.NearestPoint(Local));
					const double EdgeDistance = FVector::DistSquared(OnEdge, From);
					if (EdgeDistance < EdgeSquared)
					{
						EdgeSquared = EdgeDistance;
						EdgePoint = OnEdge;
					}
				}

				UE::Geometry::FDistPoint3Triangle3d Distance(Local, Triangle);
				Distance.GetSquared();
				const FVector FacePoint = ToWorld.TransformPosition(Distance.ClosestTrianglePoint);
				const double FaceSquared = FVector::DistSquared(FacePoint, From);

				// A corner within reach counts for four times its distance, an
				// edge for twice: the hand reaching for a corner from a little
				// way off should not be handed the face under it.
				FVector Target;
				double TargetSquared;
				ESurfSnapKind Kind;
				double Score;
				if (CornerSquared <= RadiusSquared)
				{
					Target = Corner;
					TargetSquared = CornerSquared;
					Kind = ESurfSnapKind::Corner;
					Score = CornerSquared * 0.25;
				}
				else if (EdgeSquared <= RadiusSquared)
				{
					Target = EdgePoint;
					TargetSquared = EdgeSquared;
					Kind = ESurfSnapKind::Edge;
					Score = EdgeSquared * 0.5;
				}
				else if (FaceSquared <= RadiusSquared)
				{
					Target = FacePoint;
					TargetSquared = FaceSquared;
					Kind = ESurfSnapKind::Face;
					Score = FaceSquared;
				}
				else
				{
					continue;
				}

				if (Score < BestScore)
				{
					BestScore = Score;
					OutCorrection = Target - From;
					OutTarget = Target;
					OutDistanceSquared = TargetSquared;
					OutKind = Kind;
					bFound = true;
				}
			}
		}
	}

	return bFound;
}

void USurfEdMode::DrawSnapMarker(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	// The border Continue would take, as a ghost (S159a): the target border
	// in the snap green, thick, and a thin tie from each moving point to its
	// place across the seam - what will happen, drawn before it does.
	if (View && PDI && bContinueOffered && Modal == ESurfModal::Move && ContinueOffer.Num() >= 2)
	{
		const FLinearColor Green(0.3f, 1.0f, 0.4f, 0.9f);
		for (int32 I = 0; I + 1 < ContinueOffer.Num(); ++I)
		{
			PDI->DrawLine(ContinueOffer[I], ContinueOffer[I + 1], Green, SDPG_Foreground, 3.0f);
		}
		for (const FVector& P : ContinueOffer)
		{
			PDI->DrawPoint(P, Green, 7.0f, SDPG_Foreground);
		}
	}

	// Only while something is being dragged: the target the selection is
	// snapped to, a ring the size of a fingertip, green for a control
	// point and orange for a mesh, a dot at a corner.
	if (!View || !PDI || SnapMarkerKind == ESurfSnapKind::None || (!DragTransaction.IsValid() && Modal == ESurfModal::None))
	{
		return;
	}

	const double Base = View->WorldToScreen(SnapMarkerWorld).W
		* (4.0 / FMath::Max(1, View->UnscaledViewRect.Width()) / View->ViewMatrices.GetViewToClip().M[0][0]);
	if (!FMath::IsFinite(Base) || Base <= 0.0)
	{
		return;
	}

	// Align (S171b): a dotted guide from the far point to where the moving
	// point landed on its line, in the axis's own colour, a dot at each end.
	if (SnapMarkerKind == ESurfSnapKind::Align)
	{
		const FLinearColor AxisColour = FMath::Abs(SnapMarkerAxis.X) > 0.5 ? FLinearColor(1.0f, 0.25f, 0.25f)
			: FMath::Abs(SnapMarkerAxis.Y) > 0.5 ? FLinearColor(0.35f, 1.0f, 0.35f) : FLinearColor(0.35f, 0.55f, 1.0f);
		const FVector Run = SnapMarkerTo - SnapMarkerWorld;
		const double Length = Run.Size();
		if (Length > 1.0)
		{
			const FVector Step = Run / Length;
			// Dashes the size of a fingertip on screen, whatever the distance.
			const double Dash = FMath::Max(2.0, 6.0 * Base);
			for (double At = 0.0; At < Length; At += Dash * 2.0)
			{
				const double End = FMath::Min(At + Dash, Length);
				PDI->DrawLine(SnapMarkerWorld + Step * At, SnapMarkerWorld + Step * End, AxisColour, SDPG_Foreground, 1.5f);
			}
		}
		PDI->DrawPoint(SnapMarkerWorld, AxisColour, 6.0f, SDPG_Foreground);
		DrawCircle(PDI, SnapMarkerTo, View->GetViewRight(), View->GetViewUp(), AxisColour, 4.0 * Base, 20, SDPG_Foreground, 1.5f);
		return;
	}

	// Ours in green, the level's meshes in orange (S200a: edges of ours too).
	const bool bOurs = SnapMarkerKind == ESurfSnapKind::Point || SnapMarkerKind == ESurfSnapKind::EdgeMid
		|| SnapMarkerKind == ESurfSnapKind::EdgePerp || SnapMarkerKind == ESurfSnapKind::OwnEdge;
	const FLinearColor Colour = bOurs ? FLinearColor(0.3f, 1.0f, 0.4f) : FLinearColor(1.0f, 0.62f, 0.1f);
	const double Ring = 4.0 * Base;
	DrawCircle(PDI, SnapMarkerWorld, View->GetViewRight(), View->GetViewUp(), Colour, Ring, 20, SDPG_Foreground, 1.5f);
	if (SnapMarkerKind == ESurfSnapKind::EdgeMid)
	{
		// A middle: a small cross in the ring.
		const FVector R = View->GetViewRight() * Ring * 0.6;
		const FVector U = View->GetViewUp() * Ring * 0.6;
		PDI->DrawLine(SnapMarkerWorld - R, SnapMarkerWorld + R, Colour, SDPG_Foreground, 1.5f);
		PDI->DrawLine(SnapMarkerWorld - U, SnapMarkerWorld + U, Colour, SDPG_Foreground, 1.5f);
	}
	else if (SnapMarkerKind == ESurfSnapKind::EdgePerp)
	{
		// A square foot: a right angle in the ring.
		const FVector R = View->GetViewRight() * Ring * 0.55;
		const FVector U = View->GetViewUp() * Ring * 0.55;
		PDI->DrawLine(SnapMarkerWorld - R - U, SnapMarkerWorld + R - U, Colour, SDPG_Foreground, 1.5f);
		PDI->DrawLine(SnapMarkerWorld - R - U, SnapMarkerWorld - R + U, Colour, SDPG_Foreground, 1.5f);
	}
	if (SnapMarkerKind == ESurfSnapKind::Point || SnapMarkerKind == ESurfSnapKind::Corner)
	{
		PDI->DrawPoint(SnapMarkerWorld, Colour, 6.0f, SDPG_Foreground);
	}
	else if (SnapMarkerKind == ESurfSnapKind::Face)
	{
		// A face: a second, wider ring, so a landing on a surface reads apart from a corner.
		DrawCircle(PDI, SnapMarkerWorld, View->GetViewRight(), View->GetViewUp(), Colour, Ring * 1.8, 20, SDPG_Foreground, 1.0f);
	}
}
