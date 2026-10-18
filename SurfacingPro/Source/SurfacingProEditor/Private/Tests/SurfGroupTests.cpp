// Copyright MapModelers. All Rights Reserved.
//
// Editor-side tests: correctness of the preview build, correctness and cost
// of undo, and the rebuild curve.
//
// D21's acceptance test lives here. Before chunking, one Modify() copied the
// whole level - 1079 KB and 79 ms at a thousand primitives, and by
// measurement ~16 MB and over a second at fifteen thousand. After chunking,
// `undo.snapshot_kb` must stop scaling with primitive count. That is asserted,
// not merely logged, because it is the property the whole surfacing loop
// depends on.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Serialization/ArchiveCountMem.h"

#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

#include "SurfGroup.h"
#include "SurfChunk.h"
#include "SurfBrushMath.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Generators/GridBoxMeshGenerator.h"
#include "SurfPatchMath.h"
#include "SurfPatchSurfacing.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "SurfLog.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfGroupTestUtil
{
	/** A transient group in the editor world. Never saved, always destroyed. */
	static ASurfGroup* SpawnGroup(int32 ChunkCapacity = 32)
	{
		if (!GEditor)
		{
			return nullptr;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return nullptr;
		}

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ASurfGroup* Group = World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), FTransform::Identity, Params);
		if (Group)
		{
			Group->ChunkCapacity = ChunkCapacity;
		}
		return Group;
	}

	static void FillBoxes(ASurfGroup* Group, int32 Count)
	{
		const int32 Side = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<double>(Count))));

		for (int32 I = 0; I < Count; ++I)
		{
			const double X = (I % Side) * 300.0;
			const double Y = (I / Side) * 300.0;
			Group->AppendBox(FBox(FVector(X, Y, 0.0), FVector(X + 200.0, Y + 200.0, 300.0)));
		}
	}

	/** What Modify() has to copy: the object serialized whole, arrays included. */
	static double SnapshotKb(UObject* Object)
	{
		if (!Object)
		{
			return 0.0;
		}

		FArchiveCountMem Counter(Object);
		return static_cast<double>(Counter.GetMax()) / 1024.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGroupBuildTest, "SurfacingPro.Group.PreviewBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGroupBuildTest::RunTest(const FString& Parameters)
{
	ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		return false;
	}

	TestEqual(TEXT("a new group is empty"), Group->NumPrimitives(), 0);
	TestEqual(TEXT("a new group has no chunks"), Group->Chunks.Num(), 0);

	Group->AppendBox(FBox(FVector(0.0), FVector(100.0)));
	Group->RebuildPreview();

	TestEqual(TEXT("one brush is one primitive"), Group->NumPrimitives(), 1);
	TestEqual(TEXT("one brush allocates one chunk"), Group->Chunks.Num(), 1);
	TestEqual(TEXT("a box preview is 12 triangles"), Group->TriangleCount, 12);

	Group->AppendBox(FBox(FVector(300.0), FVector(400.0)));
	Group->RebuildPreview();
	TestEqual(TEXT("two boxes are 24 triangles"), Group->TriangleCount, 24);

	// Chunks fill to capacity before another is allocated.
	Group->ChunkCapacity = 4;
	SurfGroupTestUtil::FillBoxes(Group, 6);
	TestEqual(TEXT("eight primitives across a filling chunk and new ones"), Group->NumPrimitives(), 8);
	TestTrue(TEXT("more than one chunk once capacity is exceeded"), Group->Chunks.Num() >= 2);

	Group->ClearPrimitives();
	Group->RebuildPreview();
	TestEqual(TEXT("clearing empties the preview"), Group->TriangleCount, 0);
	TestEqual(TEXT("clearing releases the chunks"), Group->Chunks.Num(), 0);

	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGroupUndoRoundTripTest, "SurfacingPro.Group.UndoRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGroupUndoRoundTripTest::RunTest(const FString& Parameters)
{
	// Cheap undo is worthless if it does not actually restore the geometry.
	ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup(8);
	if (!Group || !GEditor)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		return false;
	}

	SurfGroupTestUtil::FillBoxes(Group, 20);
	Group->RebuildPreview();

	const FSurfBrush* First = nullptr;
	Group->ForEachBrush([&First](const FSurfBrush& Brush)
	{
		if (!First)
		{
			First = &Brush;
		}
	});

	if (!First)
	{
		AddError(TEXT("no brush to edit"));
		Group->Destroy();
		return false;
	}

	const FGuid EditedId = First->Id;
	const double OriginalW = First->Faces[0].Plane.W;

	{
		FScopedTransaction Transaction(NSLOCTEXT("SurfacingPro", "SurfTestNudge", "Nudge Brush"));
		FSurfBrush* Brush = Group->ModifyBrush(EditedId);
		if (!Brush)
		{
			AddError(TEXT("ModifyBrush did not find the brush by id"));
			Group->Destroy();
			return false;
		}
		Brush->Faces[0].Plane.W = OriginalW + 50.0;
		++Brush->EditSerial;
	}

	const FSurfBrush* AfterEdit = Group->FindBrush(EditedId);
	TestTrue(TEXT("the brush survives the edit"), AfterEdit != nullptr);
	if (AfterEdit)
	{
		TestTrue(TEXT("the edit applied"), FMath::IsNearlyEqual(AfterEdit->Faces[0].Plane.W, OriginalW + 50.0, 1.e-3));
	}

	GEditor->UndoTransaction();

	const FSurfBrush* AfterUndo = Group->FindBrush(EditedId);
	TestTrue(TEXT("the brush survives undo"), AfterUndo != nullptr);
	if (AfterUndo)
	{
		TestTrue(TEXT("undo restores the plane"), FMath::IsNearlyEqual(AfterUndo->Faces[0].Plane.W, OriginalW, 1.e-3));
	}

	TestEqual(TEXT("undo does not lose primitives"), Group->NumPrimitives(), 20);

	GEditor->RedoTransaction();

	const FSurfBrush* AfterRedo = Group->FindBrush(EditedId);
	TestTrue(TEXT("the brush survives redo"), AfterRedo != nullptr);
	if (AfterRedo)
	{
		TestTrue(TEXT("redo reapplies the plane"), FMath::IsNearlyEqual(AfterRedo->Faces[0].Plane.W, OriginalW + 50.0, 1.e-3));
	}

	// Leave the undo stack where it was found.
	GEditor->UndoTransaction();

	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGroupScaleTest, "SurfacingPro.Perf.PreviewRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGroupScaleTest::RunTest(const FString& Parameters)
{
	// Rebuild cost at three sizes, so the shape of the curve is visible rather
	// than a single number that could be hiding anything.
	const int32 Sizes[] = { 100, 500, 1000 };

	for (const int32 Count : Sizes)
	{
		ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup();
		if (!Group)
		{
			AddError(TEXT("could not spawn a group in the editor world"));
			return false;
		}

		SurfGroupTestUtil::FillBoxes(Group, Count);

		const double Ms = Group->RebuildPreview();

		TestEqual(*FString::Printf(TEXT("%d brushes present"), Count), Group->NumPrimitives(), Count);
		TestEqual(*FString::Printf(TEXT("%d brushes are %d triangles"), Count, Count * 12), Group->TriangleCount, Count * 12);

		SURF_DIAG("rebuild_scale", TEXT("primitives=%d chunks=%d tris=%d total_ms=%.2f per_primitive_ms=%.4f"),
			Count, Group->Chunks.Num(), Group->TriangleCount, Ms, Ms / Count);

		Group->Destroy();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGroupUndoCostTest, "SurfacingPro.Perf.UndoSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGroupUndoCostTest::RunTest(const FString& Parameters)
{
	// The D21 acceptance test. Transactions are cancelled rather than
	// committed so this never pollutes a real undo stack.
	const int32 Sizes[] = { 1, 100, 1000 };

	double ChunkKbAt100 = 0.0;
	double ChunkKbAt1000 = 0.0;

	for (const int32 Count : Sizes)
	{
		ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup(32);
		if (!Group)
		{
			AddError(TEXT("could not spawn a group in the editor world"));
			return false;
		}

		SurfGroupTestUtil::FillBoxes(Group, Count);

		FGuid FirstId;
		Group->ForEachBrush([&FirstId](const FSurfBrush& Brush)
		{
			if (!FirstId.IsValid())
			{
				FirstId = Brush.Id;
			}
		});

		USurfChunk* Chunk = Group->FindChunkForBrush(FirstId);
		const double ChunkKb = SurfGroupTestUtil::SnapshotKb(Chunk);
		const double GroupKb = SurfGroupTestUtil::SnapshotKb(Group);

		SURF_METRIC_N("undo.snapshot_kb", ChunkKb, "kb", Count);
		SURF_METRIC_N("undo.group_kb", GroupKb, "kb", Count);

		if (Count == 100)  { ChunkKbAt100 = ChunkKb; }
		if (Count == 1000) { ChunkKbAt1000 = ChunkKb; }

		constexpr int32 Iterations = 60;
		const double Start = FPlatformTime::Seconds();

		for (int32 I = 0; I < Iterations; ++I)
		{
			FScopedTransaction Transaction(NSLOCTEXT("SurfacingPro", "SurfTestEdit", "Edit Brush"));
			if (FSurfBrush* Brush = Group->ModifyBrush(FirstId))
			{
				Brush->Faces[0].Plane.W += 1.0;
				++Brush->EditSerial;
			}
			Transaction.Cancel();
		}

		const double TotalMs = (FPlatformTime::Seconds() - Start) * 1000.0;

		SURF_METRIC_N("undo.modify_ms", TotalMs, "ms", Iterations);
		SURF_DIAG("undo_scale", TEXT("primitives=%d chunks=%d chunk_kb=%.1f group_kb=%.1f modify_ms_each=%.4f"),
			Count, Group->Chunks.Num(), ChunkKb, GroupKb, TotalMs / Iterations);

		Group->Destroy();
	}

	// The property the surfacing loop depends on: a transaction costs the
	// chunk, not the level. Ten times the primitives must not mean ten times
	// the snapshot.
	if (ChunkKbAt100 > 0.0)
	{
		TestTrue(
			*FString::Printf(TEXT("undo snapshot does not scale with level size (100: %.1f kb, 1000: %.1f kb)"), ChunkKbAt100, ChunkKbAt1000),
			ChunkKbAt1000 < ChunkKbAt100 * 2.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPreviewUVTest, "SurfacingPro.Group.PreviewUVs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPreviewUVTest::RunTest(const FString& Parameters)
{
	using namespace UE::Geometry;

	// Every other surfacing test reads control points. What ships is the UV
	// overlay on the built mesh, and nothing had checked that the mapping
	// survives tessellation - which is the one place it could silently be
	// dropped, leaving a correct panel over an untextured surface.
	ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		return false;
	}

	// Four metres by two, at one metre per tile: four tiles by two.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	Patch.Scale = FVector2D(1.0, 1.0);
	Patch.SubdivU = 4;
	Patch.SubdivV = 4;
	SurfPatchSurfacing::SetNatural(Patch);

	Group->AddPatch(MoveTemp(Patch));
	Group->RebuildPreview();

	UDynamicMeshComponent* Preview = Group->Preview;
	if (!Preview)
	{
		AddError(TEXT("the group has no preview component"));
		Group->Destroy();
		return false;
	}

	FVector2D Min(MAX_dbl, MAX_dbl);
	FVector2D Max(-MAX_dbl, -MAX_dbl);
	int32 Elements = 0;

	Preview->ProcessMesh([&Min, &Max, &Elements](const FDynamicMesh3& Mesh)
	{
		const FDynamicMeshUVOverlay* UVs = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryUV() : nullptr;
		if (!UVs)
		{
			return;
		}

		for (const int32 Id : UVs->ElementIndicesItr())
		{
			const FVector2f UV = UVs->GetElement(Id);
			Min.X = FMath::Min(Min.X, static_cast<double>(UV.X));
			Min.Y = FMath::Min(Min.Y, static_cast<double>(UV.Y));
			Max.X = FMath::Max(Max.X, static_cast<double>(UV.X));
			Max.Y = FMath::Max(Max.Y, static_cast<double>(UV.Y));
			++Elements;
		}
	});

	TestTrue(TEXT("the built mesh carries UVs at all"), Elements > 0);

	if (Elements > 0)
	{
		TestTrue(TEXT("four metres at one metre per tile is four tiles on the mesh"),
			FMath::IsNearlyEqual(Max.X - Min.X, 4.0, 1.e-3));
		TestTrue(TEXT("and two the other way"),
			FMath::IsNearlyEqual(Max.Y - Min.Y, 2.0, 1.e-3));
	}

	Group->Destroy();
	return true;
}

/**
 * Which way a brush's triangles face (S99a).
 *
 * "are you aware any brush is reversed faces? box cylinder etc.. theyre all
 * toward inside instead of outside." The planes of a box are written with
 * outward normals and the hull's loops are sorted counter-clockwise about
 * each one, which should put every triangle's geometric normal out of the
 * solid. This asks the geometry rather than the eye: build a box, build a
 * cylinder, and check that no triangle of either faces its own centre.
 *
 * If it passes and they still look inside-out on screen, the fault is in the
 * shading or the material and not in the mesh - which is worth knowing
 * before anything is reversed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBrushFacingTest, "SurfacingPro.Brush.FacesOutward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBrushFacingTest::RunTest(const FString& Parameters)
{
	auto CheckSolid = [this](const TCHAR* What, const FSurfBrush& Brush)
	{
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(Brush, Hull) || Hull.Vertices.Num() == 0)
		{
			AddError(FString::Printf(TEXT("%s has no hull"), What));
			return;
		}

		TArray<uint32> Indices;
		SurfBrushMath::Triangulate(Hull, Indices);
		if (Indices.Num() < 3)
		{
			AddError(FString::Printf(TEXT("%s has no triangles"), What));
			return;
		}

		FVector Centre = FVector::ZeroVector;
		for (const FVector& Vertex : Hull.Vertices)
		{
			Centre += Vertex;
		}
		Centre /= static_cast<double>(Hull.Vertices.Num());

		int32 Inward = 0;
		int32 Outward = 0;
		for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
		{
			const FVector A = Hull.Vertices[Indices[I]];
			const FVector B = Hull.Vertices[Indices[I + 1]];
			const FVector C = Hull.Vertices[Indices[I + 2]];
			// Unreal's front: (C - A) x (B - A). S99a measured the other one
			// and called a solid wound inward "outward" (S108a).
			const FVector Normal = FVector::CrossProduct(C - A, B - A).GetSafeNormal();
			const FVector Away = (((A + B + C) / 3.0) - Centre).GetSafeNormal();
			if (FVector::DotProduct(Normal, Away) > 0.0)
			{
				++Outward;
			}
			else
			{
				++Inward;
			}
		}

		// Consistent, which it always was. Whether consistent means *outward
		// as Unreal reads it* is SurfacingPro.Brush.WindsLikeUnreal, and that
		// is the question S99a should have asked (S108a).
		TestEqual(FString::Printf(TEXT("every triangle of a %s winds the same way"), What), Inward, 0);
		TestTrue(FString::Printf(TEXT("and a %s has triangles at all"), What), Outward > 0);
	};

	CheckSolid(TEXT("box"), SurfBrushMath::MakeBox(FBox(FVector(-100.0), FVector(100.0))));
	CheckSolid(TEXT("cylinder"), SurfBrushMath::MakeCylinder(FBox(FVector(-100.0), FVector(100.0)), 12));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchFlipTest, "SurfacingPro.Patch.Flip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchFlipTest::RunTest(const FString& Parameters)
{
	/**
	 * "why using flip dont work here ?" (S115a).
	 *
	 * It did not, and no test would have caught it, because every test asked
	 * the patch what it thought rather than asking the mesh what was drawn.
	 * bInvert turned the faceted shading normal and nothing else, so on a
	 * smooth patch - which is every patch by default - Flip changed nothing
	 * at all.
	 *
	 * Flipping a surface means the other side is the one you see. So: build
	 * the preview, remember which way its triangles face, flip, build again,
	 * and they have to face the other way. It says nothing about which side
	 * is "front" - only that flipping swaps them, which is the whole of what
	 * the verb promises.
	 */
	ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group"));
		return false;
	}

	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 300.0, 0.0));
	const FGuid PatchId = Patch.Id;
	Group->AddPatch(MoveTemp(Patch));
	Group->RebuildPreview();

	// Both halves of what "flipped" has to mean: the side that is drawn, and
	// the side that is lit. Either one alone is a patch that looks wrong.
	auto FacingZ = [Group](double& OutShading)
	{
		double Sum = 0.0;
		OutShading = 0.0;
		double& Shading = OutShading;
		if (Group->Preview)
		{
			Group->Preview->ProcessMesh([&Sum, &Shading](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				const UE::Geometry::FDynamicMeshNormalOverlay* Normals =
					Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryNormals() : nullptr;
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					FVector A, B, C;
					Mesh.GetTriVertices(Tid, A, B, C);
					Sum += FVector::CrossProduct(C - A, B - A).Z;

					if (Normals && Normals->IsSetTriangle(Tid))
					{
						const UE::Geometry::FIndex3i Elements = Normals->GetTriangle(Tid);
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							Shading += FVector(Normals->GetElement(Elements[Corner])).Z;
						}
					}
				}
			});
		}
		return Sum;
	};

	double ShadingBefore = 0.0;
	const double Before = FacingZ(ShadingBefore);
	TestTrue(TEXT("a flat patch draws with a side"), FMath::Abs(Before) > 1.0);
	TestTrue(TEXT("and is lit on one"), FMath::Abs(ShadingBefore) > 0.01);

	if (FSurfPatch* Flip = Group->ModifyPatch(PatchId))
	{
		Flip->bInvert = !Flip->bInvert;
		++Flip->EditSerial;
	}
	Group->RebuildPreview();

	double ShadingAfter = 0.0;
	const double After = FacingZ(ShadingAfter);
	AddInfo(FString::Printf(TEXT("flip: winding %.0f -> %.0f, shading %.2f -> %.2f"),
		Before, After, ShadingBefore, ShadingAfter));

	/**
	 * What Flip does, and what it does not (S115b).
	 *
	 * The normal turns over, and the winding deliberately does not - the two
	 * are kept opposite because that is the order Unreal reads as front, and
	 * SurfacingPro.Math.Patch.Normals has guarded that pairing since it was
	 * written.
	 *
	 * So this asserts the half that is real. The other half of his report -
	 * "why using flip dont work here ?" - is not this: **the preview material
	 * is unlit and two-sided**, so nothing about facing can show in it at all.
	 * Flip shows on a real material and in a bake, and is invisible in the
	 * preview by construction. That is a question about what the preview
	 * should do, not a bug in the flip, and it is his to answer.
	 */
	TestTrue(TEXT("flipped, it is lit from the other side"), (ShadingBefore > 0.0) != (ShadingAfter > 0.0));
	TestTrue(TEXT("and still draws a surface"), FMath::Abs(After) > 1.0);

	// And back: flipping twice is where it started.
	if (FSurfPatch* Flip = Group->ModifyPatch(PatchId))
	{
		Flip->bInvert = !Flip->bInvert;
		++Flip->EditSerial;
	}
	Group->RebuildPreview();
	double ShadingBack = 0.0;
	FacingZ(ShadingBack);
	TestTrue(TEXT("and flipping twice is where it started"), (ShadingBack > 0.0) == (ShadingBefore > 0.0));

	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBrushCornersTest, "SurfacingPro.Brush.Corners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBrushCornersTest::RunTest(const FString& Parameters)
{
	/**
	 * "we need be able to select brushes points (vertices) and edit them
	 * freely ... you can make them any shape, as long it remain enclosed box."
	 *
	 * A brush is planes. Moving a corner means asking what planes the moved
	 * corners imply, which is the convex hull of them - exact, so a corner
	 * dragged onto the grid lands on the grid. A corner that ends up inside
	 * the solid has stopped being a corner, and that is the one thing this
	 * refuses.
	 */
	const FSurfBrush Box = SurfBrushMath::MakeBox(FBox(FVector(-100.0), FVector(100.0)));

	FSurfHull Hull;
	if (!SurfBrushMath::DeriveHull(Box, Hull) || Hull.Vertices.Num() != 8)
	{
		AddError(TEXT("a box should derive eight corners"));
		return false;
	}
	const double Was = SurfBrushMath::Volume(Hull);

	// --- One corner pulled out: a seven-sided solid --------------------------
	{
		TArray<FVector> Corners = Hull.Vertices;
		int32 Top = 0;
		for (int32 I = 1; I < Corners.Num(); ++I)
		{
			if (Corners[I].Z > Corners[Top].Z + 1.0
				|| (FMath::IsNearlyEqual(Corners[I].Z, Corners[Top].Z, 1.0) && Corners[I].X > Corners[Top].X))
			{
				Top = I;
			}
		}
		const FVector Moved = Corners[Top] + FVector(0.0, 0.0, 200.0);
		Corners[Top] = Moved;

		FSurfBrush Out;
		FString Why;
		TestTrue(TEXT("a corner pulled outwards rebuilds"), SurfBrushMath::RebuildFromPoints(Box, Corners, Out, &Why));
		TestTrue(TEXT("with no reason to refuse"), Why.IsEmpty());
		TestTrue(TEXT("and more sides than a box has"), Out.Faces.Num() >= 6);

		FSurfHull After;
		TestTrue(TEXT("the new planes bound a solid"), SurfBrushMath::DeriveHull(Out, After));
		TestTrue(TEXT("which is bigger than the box was"), SurfBrushMath::Volume(After) > Was);

		bool bFound = false;
		for (const FVector& Vertex : After.Vertices)
		{
			bFound = bFound || Vertex.Equals(Moved, 0.1);
		}
		TestTrue(TEXT("and the corner is where it was put, exactly"), bFound);

		// Every face still carries a material slot from the box it came from.
		TestEqual(TEXT("the brush keeps what it was"), static_cast<int32>(Out.Kind), static_cast<int32>(Box.Kind));
	}

	// --- One corner pushed inside: refused, and said ------------------------
	{
		TArray<FVector> Corners = Hull.Vertices;
		Corners[0] = FVector::ZeroVector;

		FSurfBrush Out;
		FString Why;
		TestFalse(TEXT("a corner pushed inside is refused"), SurfBrushMath::RebuildFromPoints(Box, Corners, Out, &Why));
		TestTrue(TEXT("and says which"), Why.Contains(TEXT("inside")));
	}

	// --- Flattened: no solid ------------------------------------------------
	{
		TArray<FVector> Corners = Hull.Vertices;
		for (FVector& Corner : Corners)
		{
			Corner.Z = 0.0;
		}

		FSurfBrush Out;
		FString Why;
		TestFalse(TEXT("corners on one plane are refused"), SurfBrushMath::RebuildFromPoints(Box, Corners, Out, &Why));
		TestFalse(TEXT("and it says so"), Why.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBrushWindingTest, "SurfacingPro.Brush.WindsLikeUnreal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBrushWindingTest::RunTest(const FString& Parameters)
{
	/**
	 * "are you aware any brush is reversed faces? box cylinder etc.. theyre
	 * all toward inside instead of outside" - asked again, better (S107a).
	 *
	 * S99a asked whether a brush's triangles wind away from its own centre,
	 * and they do. That was the wrong question: it establishes that the
	 * winding is consistent, not that it is the winding Unreal calls the
	 * front. A solid wound consistently the wrong way is exactly what you see
	 * the inside of.
	 *
	 * So ask something that needs no convention of ours at all: build the same
	 * box with Unreal's own generator - which renders right by construction,
	 * or every geometry script in the engine would be inside out - and compare
	 * the two by signed volume. The sign of the sum over A . (B x C) is
	 * positive for a closed mesh wound outward and negative for one wound
	 * inward, whichever way round "outward" is defined, so if the two signs
	 * agree our brush faces the way Unreal's box does, and if they differ it
	 * is inside out.
	 */
	auto SignedVolumeSix = [](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		double Six = 0.0;
		for (const int32 Tid : Mesh.TriangleIndicesItr())
		{
			FVector A, B, C;
			Mesh.GetTriVertices(Tid, A, B, C);
			Six += FVector::DotProduct(A, FVector::CrossProduct(B, C));
		}
		return Six;
	};

	// Unreal's box, one quad a side, the same size as ours.
	UE::Geometry::FGridBoxMeshGenerator Generator;
	Generator.Box = UE::Geometry::FOrientedBox3d(FVector3d::Zero(), FVector3d(100.0, 100.0, 100.0));
	Generator.EdgeVertices = UE::Geometry::FIndex3i(2, 2, 2);
	Generator.Generate();
	UE::Geometry::FDynamicMesh3 Reference(&Generator);

	const double ReferenceSix = SignedVolumeSix(Reference);
	TestTrue(TEXT("Unreal's own box is a closed solid with volume"), FMath::Abs(ReferenceSix) > 1.0);

	// Ours, through the same path the preview draws: hull, then the fan.
	UE::Geometry::FDynamicMesh3 Ours;
	{
		const FSurfBrush Brush = SurfBrushMath::MakeBox(FBox(FVector(-100.0), FVector(100.0)));
		FSurfHull Hull;
		if (!SurfBrushMath::DeriveHull(Brush, Hull))
		{
			AddError(TEXT("our box has no hull"));
			return false;
		}
		TArray<int32> Vids;
		Vids.Reserve(Hull.Vertices.Num());
		for (const FVector& Vertex : Hull.Vertices)
		{
			Vids.Add(Ours.AppendVertex(Vertex));
		}

		// Through the shipping triangulation, not a copy of it here: a test
		// that fans the hull itself is testing its own arithmetic.
		TArray<uint32> Indices;
		SurfBrushMath::Triangulate(Hull, Indices);
		for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
		{
			Ours.AppendTriangle(Vids[Indices[I]], Vids[Indices[I + 1]], Vids[Indices[I + 2]]);
		}
	}

	const double OursSix = SignedVolumeSix(Ours);
	TestTrue(TEXT("our box is a closed solid with volume"), FMath::Abs(OursSix) > 1.0);

	AddInfo(FString::Printf(TEXT("winding: Unreal's box %s, ours %s"),
		ReferenceSix > 0.0 ? TEXT("positive") : TEXT("negative"),
		OursSix > 0.0 ? TEXT("positive") : TEXT("negative")));

	TestTrue(TEXT("our box winds the way Unreal's does"),
		(ReferenceSix > 0.0) == (OursSix > 0.0));

	/**
	 * And the same of the preview, which is the mesh he is actually looking at
	 * (S109a).
	 *
	 * The hull triangulation and the preview build are two different pieces of
	 * code, and only the second one draws. Signed volume answers for the
	 * preview as well as it answers for the hull: rotating a triangle's corners
	 * does not change it, reversing one does, so it says which way a closed
	 * mesh faces and nothing else.
	 */
	{
		ASurfGroup* Group = SurfGroupTestUtil::SpawnGroup();
		if (!Group)
		{
			AddError(TEXT("could not spawn a group"));
			return false;
		}

		Group->AppendBox(FBox(FVector(-100.0), FVector(100.0)));
		Group->RebuildPreview();

		double PreviewSix = 0.0;
		int32 Triangles = 0;
		if (Group->Preview)
		{
			Group->Preview->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				Triangles = Mesh.TriangleCount();
				for (const int32 Tid : Mesh.TriangleIndicesItr())
				{
					FVector A, B, C;
					Mesh.GetTriVertices(Tid, A, B, C);
					PreviewSix += FVector::DotProduct(A, FVector::CrossProduct(B, C));
				}
			});
		}

		AddInfo(FString::Printf(TEXT("preview box: %d triangles, %s"),
			Triangles, PreviewSix > 0.0 ? TEXT("positive") : TEXT("negative")));

		TestTrue(TEXT("the preview box is closed and has volume"), FMath::Abs(PreviewSix) > 1.0);
		TestTrue(TEXT("and it faces the way Unreal's box does"),
			(ReferenceSix > 0.0) == (PreviewSix > 0.0));

		Group->Destroy();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
