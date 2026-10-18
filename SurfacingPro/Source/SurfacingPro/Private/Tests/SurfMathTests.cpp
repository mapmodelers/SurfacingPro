// Copyright MapModelers. All Rights Reserved.
//
// S1 test batch. Run all of them with one command:
//   UnrealEditor-Cmd.exe <project>.uproject -ExecCmds="Automation RunTests SurfacingPro; Quit" -unattended -nopause -nosplash -log
//
// Several of these pin conventions rather than verify logic. That is
// deliberate: a projection tangent or an offset sign cannot change later
// without breaking existing content, so they are fixed here on day one.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfTypes.h"
#include "SurfSettings.h"
#include "SurfBrushMath.h"
#include "SurfPatchMath.h"
#include "SurfPatchPrimitives.h"
#include "SurfProjection.h"
#include "SurfPatchSurfacing.h"
#include "SurfKeymap.h"
#include "SurfLattice.h"
#include "SurfPatchOps.h"
#include "SurfacingProModule.h"
#include "SurfLog.h"
#include "SurfGroup.h"
#include "SurfPrefab.h"
#include "SurfMaterialSet.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR   // editor-context tests; they read editor config

namespace SurfTestUtil
{
	static constexpr double Tol = 1.e-3;

	static bool NearlyEqual(double A, double B, double Tolerance = Tol)
	{
		return FMath::Abs(A - B) <= Tolerance;
	}

	static bool VectorNearlyEqual(const FVector& A, const FVector& B, double Tolerance = Tol)
	{
		return FVector::DistSquared(A, B) <= Tolerance * Tolerance;
	}

	/** A 200 cm cube centred on the origin. */
	static FSurfBrush MakeTestCube()
	{
		return SurfBrushMath::MakeBox(FBox(FVector(-100.0), FVector(100.0)));
	}
}

// ---------------------------------------------------------------------------
// Brush hull derivation
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHullCubeTest, "SurfacingPro.Math.Hull.Cube",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHullCubeTest::RunTest(const FString& Parameters)
{
	const FSurfBrush Cube = SurfTestUtil::MakeTestCube();

	FSurfHull Hull;
	TestTrue(TEXT("cube derives"), SurfBrushMath::DeriveHull(Cube, Hull));
	TestEqual(TEXT("cube has 8 vertices"), Hull.Vertices.Num(), 8);
	TestEqual(TEXT("cube has 6 faces"), Hull.Faces.Num(), 6);

	for (const FSurfHullFace& Face : Hull.Faces)
	{
		TestEqual(TEXT("each cube face is a quad"), Face.VertexIndices.Num(), 4);
	}

	// 200^3 = 8,000,000 cubic centimetres.
	TestTrue(TEXT("cube volume"), SurfTestUtil::NearlyEqual(SurfBrushMath::Volume(Hull), 8000000.0, 1.0));

	TArray<uint32> Indices;
	SurfBrushMath::Triangulate(Hull, Indices);
	TestEqual(TEXT("cube triangulates to 12 triangles"), Indices.Num(), 36);

	TestTrue(TEXT("centre is inside"), SurfBrushMath::IsPointInside(Cube.Faces, FVector::ZeroVector));
	TestFalse(TEXT("far point is outside"), SurfBrushMath::IsPointInside(Cube.Faces, FVector(500.0, 0.0, 0.0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHullSliceTest, "SurfacingPro.Math.Hull.Slice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHullSliceTest::RunTest(const FString& Parameters)
{
	FSurfBrush Brush = SurfTestUtil::MakeTestCube();

	FSurfHull Before;
	SurfBrushMath::DeriveHull(Brush, Before);
	const double VolumeBefore = SurfBrushMath::Volume(Before);

	// Slice a corner off with a 45-degree plane through (100, 100, 0)-ish.
	const FVector Normal = FVector(1.0, 1.0, 0.0).GetSafeNormal();
	Brush.Faces.Emplace(FPlane(Normal, FVector::DotProduct(Normal, FVector(100.0, 0.0, 0.0))));

	FSurfHull After;
	TestTrue(TEXT("sliced brush derives"), SurfBrushMath::DeriveHull(Brush, After));
	TestEqual(TEXT("slice adds a face"), After.Faces.Num(), 7);
	TestTrue(TEXT("slice removes volume"), SurfBrushMath::Volume(After) < VolumeBefore);
	TestTrue(TEXT("slice leaves volume"), SurfBrushMath::Volume(After) > 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHullRedundantPlaneTest, "SurfacingPro.Math.Hull.RedundantPlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHullRedundantPlaneTest::RunTest(const FString& Parameters)
{
	FSurfBrush Brush = SurfTestUtil::MakeTestCube();

	// A plane that bounds nothing. A slice can leave one behind, so it must be
	// dropped silently rather than failing the derivation.
	Brush.Faces.Emplace(FPlane(FVector(1.0, 0.0, 0.0), 1000.0));

	FSurfHull Hull;
	TestTrue(TEXT("redundant plane still derives"), SurfBrushMath::DeriveHull(Brush, Hull));
	TestEqual(TEXT("redundant plane contributes no face"), Hull.Faces.Num(), 6);
	TestEqual(TEXT("redundant plane adds no vertices"), Hull.Vertices.Num(), 8);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHullDegenerateTest, "SurfacingPro.Math.Hull.Degenerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHullDegenerateTest::RunTest(const FString& Parameters)
{
	FSurfBrush Brush;
	Brush.Faces.Emplace(FPlane(FVector(0.0, 0.0, 1.0), 0.0));
	Brush.Faces.Emplace(FPlane(FVector(0.0, 0.0, -1.0), 0.0));
	Brush.Faces.Emplace(FPlane(FVector(1.0, 0.0, 0.0), 0.0));

	FSurfHull Hull;
	TestFalse(TEXT("three planes bound no volume"), SurfBrushMath::DeriveHull(Brush, Hull));
	TestFalse(TEXT("failed derivation leaves the hull invalid"), Hull.IsValid());

	// Two parallel planes and one other: no intersection point at all.
	FVector Unused;
	TestFalse(TEXT("parallel planes do not intersect"),
		SurfBrushMath::IntersectThreePlanes(
			FPlane(FVector(0.0, 0.0, 1.0), 0.0),
			FPlane(FVector(0.0, 0.0, 1.0), 100.0),
			FPlane(FVector(1.0, 0.0, 0.0), 0.0),
			Unused));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHullSplitTest, "SurfacingPro.Math.Hull.Split",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHullSplitTest::RunTest(const FString& Parameters)
{
	// Slice is the whole of brush shaping, so its arithmetic is worth pinning.
	const FSurfBrush Cube = SurfTestUtil::MakeTestCube();   // 200 cm, centred

	FSurfHull WholeHull;
	SurfBrushMath::DeriveHull(Cube, WholeHull);
	const double WholeVolume = SurfBrushMath::Volume(WholeHull);

	// Cut clean through the middle on X.
	{
		FSurfBrush Front, Back;
		bool bFront = false, bBack = false;
		SurfBrushMath::SplitBrush(Cube, FPlane(FVector(1.0, 0.0, 0.0), 0.0), Front, bFront, Back, bBack);

		TestTrue(TEXT("a central cut yields a front half"), bFront);
		TestTrue(TEXT("a central cut yields a back half"), bBack);

		FSurfHull FrontHull, BackHull;
		SurfBrushMath::DeriveHull(Front, FrontHull);
		SurfBrushMath::DeriveHull(Back, BackHull);

		const double Sum = SurfBrushMath::Volume(FrontHull) + SurfBrushMath::Volume(BackHull);
		TestTrue(TEXT("the halves add back up to the whole"), SurfTestUtil::NearlyEqual(Sum, WholeVolume, 1.0));
		TestTrue(TEXT("a central cut halves the volume"),
			SurfTestUtil::NearlyEqual(SurfBrushMath::Volume(FrontHull), WholeVolume * 0.5, 1.0));

		// Each half gains the cut face and drops the plane the cut made
		// redundant - the face on the far side - so a half carries only
		// what bounds it (S67b). The halves get fresh ids.
		TestEqual(TEXT("the front half gains the cut face and drops the far one"), Front.Faces.Num(), Cube.Faces.Num());
		TestEqual(TEXT("so does the back half"), Back.Faces.Num(), Cube.Faces.Num());
		TestTrue(TEXT("halves get new ids"), Front.Id != Cube.Id && Back.Id != Cube.Id && Front.Id != Back.Id);
	}

	// A plane that misses the brush entirely leaves one side empty.
	{
		FSurfBrush Front, Back;
		bool bFront = false, bBack = false;
		SurfBrushMath::SplitBrush(Cube, FPlane(FVector(1.0, 0.0, 0.0), 500.0), Front, bFront, Back, bBack);

		TestFalse(TEXT("a miss produces no front half"), bFront);
		TestTrue(TEXT("a miss leaves the back half whole"), bBack);
	}

	// A plane exactly on a face grazes it and must not produce a zero-volume slab.
	{
		FSurfBrush Front, Back;
		bool bFront = false, bBack = false;
		SurfBrushMath::SplitBrush(Cube, FPlane(FVector(1.0, 0.0, 0.0), 100.0), Front, bFront, Back, bBack);

		TestFalse(TEXT("a grazing cut produces no degenerate slab"), bFront);
		TestTrue(TEXT("a grazing cut leaves the brush intact"), bBack);
	}

	// An angled cut still closes: the pieces must both be valid hulls.
	{
		FSurfBrush Front, Back;
		bool bFront = false, bBack = false;
		const FVector N = FVector(1.0, 1.0, 0.0).GetSafeNormal();
		SurfBrushMath::SplitBrush(Cube, FPlane(N, 0.0), Front, bFront, Back, bBack);

		TestTrue(TEXT("an angled cut yields two halves"), bFront && bBack);

		FSurfHull FrontHull;
		TestTrue(TEXT("the angled front half is a valid hull"), SurfBrushMath::DeriveHull(Front, FrontHull));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Patch tessellation
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfCurveSubdivTest, "SurfacingPro.Math.Curve.Subdivision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfCurveSubdivTest::RunTest(const FString& Parameters)
{
	const int32 Subdivs[] = { 1, 8, 32 };

	for (const int32 Subdiv : Subdivs)
	{
		FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
			ESurfPatchType::Curve, 3, 3,
			FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
		Patch.SubdivU = Subdiv;
		Patch.SubdivV = Subdiv;

		FSurfTessellation Tess;
		TestTrue(*FString::Printf(TEXT("3x3 curve tessellates at subdiv %d"), Subdiv),
			SurfPatchMath::Tessellate(Patch, Tess));

		const int32 Expected = Subdiv + 1;
		TestEqual(*FString::Printf(TEXT("subdiv %d vertex grid U"), Subdiv), Tess.NumU, Expected);
		TestEqual(*FString::Printf(TEXT("subdiv %d vertex grid V"), Subdiv), Tess.NumV, Expected);
		TestEqual(*FString::Printf(TEXT("subdiv %d vertex count"), Subdiv), Tess.NumVertices(), Expected * Expected);
		TestEqual(*FString::Printf(TEXT("subdiv %d triangle count"), Subdiv), Tess.NumTriangles(), Subdiv * Subdiv * 2);

		// A flat control net must stay flat at every subdivision.
		for (const FVector& P : Tess.Positions)
		{
			TestTrue(TEXT("flat curve stays planar"), SurfTestUtil::NearlyEqual(P.Z, 0.0));
		}

		// Corners interpolate the control net exactly.
		TestTrue(TEXT("first corner"), SurfTestUtil::VectorNearlyEqual(Tess.Positions[0], FVector::ZeroVector));
		TestTrue(TEXT("last corner"), SurfTestUtil::VectorNearlyEqual(Tess.Positions.Last(), FVector(200.0, 200.0, 0.0)));
	}

	// A 5-wide curve is two sub-patches, so it tessellates to twice the span.
	FSurfPatch Wide = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 5, 3,
		FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	Wide.SubdivU = 4;
	Wide.SubdivV = 4;

	FSurfTessellation WideTess;
	TestTrue(TEXT("5x3 curve tessellates"), SurfPatchMath::Tessellate(Wide, WideTess));
	TestEqual(TEXT("5-wide curve spans two sub-patches"), WideTess.NumU, 4 * 2 + 1);
	TestEqual(TEXT("3-high curve spans one sub-patch"), WideTess.NumV, 4 + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfCurveBezierTest, "SurfacingPro.Math.Curve.Bezier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfCurveBezierTest::RunTest(const FString& Parameters)
{
	// Quadratic Bezier: the surface reaches only a quarter of the way to a
	// raised middle control point, at 0.25*P0 + 0.5*P1 + 0.25*P2. A patch that
	// passed through the control point would mean we had built an
	// interpolating spline instead, which is a different tool.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 3, 3,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	for (int32 Y = 0; Y < 3; ++Y)
	{
		Patch.At(1, Y).Pos.Z = 100.0;
	}

	const FVector Mid = SurfPatchMath::Evaluate(Patch, 0.5, 0.5);
	TestTrue(TEXT("bezier midpoint is half the control height"), SurfTestUtil::NearlyEqual(Mid.Z, 50.0));
	TestTrue(TEXT("bezier midpoint X"), SurfTestUtil::NearlyEqual(Mid.X, 100.0));

	// Ends still interpolate.
	TestTrue(TEXT("bezier start"), SurfTestUtil::NearlyEqual(SurfPatchMath::Evaluate(Patch, 0.0, 0.5).Z, 0.0));
	TestTrue(TEXT("bezier end"), SurfTestUtil::NearlyEqual(SurfPatchMath::Evaluate(Patch, 1.0, 0.5).Z, 0.0));

	// Basis partition of unity.
	for (const double T : { 0.0, 0.25, 0.5, 0.75, 1.0 })
	{
		const double Sum = SurfPatchMath::Bernstein2(0, T) + SurfPatchMath::Bernstein2(1, T) + SurfPatchMath::Bernstein2(2, T);
		TestTrue(TEXT("bernstein basis sums to one"), SurfTestUtil::NearlyEqual(Sum, 1.0));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSheetTest, "SurfacingPro.Math.Sheet.PassThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSheetTest::RunTest(const FString& Parameters)
{
	FSurfPatch Sheet = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 4, 3,
		FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	// Displace one point so pass-through is actually observable.
	Sheet.At(2, 1).Pos.Z = 75.0;

	FSurfTessellation Tess;
	TestTrue(TEXT("sheet tessellates"), SurfPatchMath::Tessellate(Sheet, Tess));
	TestEqual(TEXT("sheet vertex grid U"), Tess.NumU, 4);
	TestEqual(TEXT("sheet vertex grid V"), Tess.NumV, 3);
	TestEqual(TEXT("sheet vertex count equals control count"), Tess.NumVertices(), Sheet.NumPoints());

	for (int32 Y = 0; Y < Sheet.Height; ++Y)
	{
		for (int32 X = 0; X < Sheet.Width; ++X)
		{
			TestTrue(TEXT("sheet control points pass through unchanged"),
				SurfTestUtil::VectorNearlyEqual(Tess.Positions[Y * Sheet.Width + X], Sheet.At(X, Y).Pos));
		}
	}

	// Subdivision is ignored on sheets: the control points are the vertices.
	Sheet.SubdivU = 16;
	FSurfTessellation Resubdivided;
	SurfPatchMath::Tessellate(Sheet, Resubdivided);
	TestEqual(TEXT("sheet ignores subdivision"), Resubdivided.NumU, 4);

	// Turned edge changes the triangulation without changing the vertices.
	FSurfPatch Turned = Sheet;
	Turned.At(0, 0).bTurnedEdge = 1;

	FSurfTessellation TurnedTess;
	SurfPatchMath::Tessellate(Turned, TurnedTess);
	TestEqual(TEXT("turned edge keeps the vertex count"), TurnedTess.NumVertices(), Tess.NumVertices());
	TestEqual(TEXT("turned edge keeps the triangle count"), TurnedTess.NumTriangles(), Tess.NumTriangles());
	TestTrue(TEXT("turned edge changes the index buffer"), TurnedTess.Indices != Tess.Indices);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchValidationTest, "SurfacingPro.Math.Patch.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchValidationTest::RunTest(const FString& Parameters)
{
	// Curves need odd dimensions - bezier parity, 2n+1 control points.
	FSurfPatch Even = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 4, 3,
		FVector::ZeroVector, FVector(100.0, 0.0, 0.0), FVector(0.0, 100.0, 0.0));
	TestFalse(TEXT("even-width curve is invalid"), Even.HasValidDimensions());

	FSurfTessellation Tess;
	TestFalse(TEXT("invalid patch does not tessellate"), SurfPatchMath::Tessellate(Even, Tess));

	// Sheets take any dimensions from 2 up.
	FSurfPatch EvenSheet = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 4, 2,
		FVector::ZeroVector, FVector(100.0, 0.0, 0.0), FVector(0.0, 100.0, 0.0));
	TestTrue(TEXT("even sheet is valid"), EvenSheet.HasValidDimensions());

	// A point array out of step with the dimensions is invalid whatever the type.
	FSurfPatch Mismatched = EvenSheet;
	Mismatched.Points.Pop();
	TestFalse(TEXT("mismatched point count is invalid"), Mismatched.HasValidDimensions());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfIdentityTest, "SurfacingPro.Math.Patch.Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfIdentityTest::RunTest(const FString& Parameters)
{
	// Ids are assigned at creation, not by the constructor. A default
	// constructed primitive must be id-less, or UE's member initialization
	// test flags every struct as non-deterministic and every temporary burns
	// a Guid.
	const FSurfBrush DefaultBrush;
	const FSurfPatch DefaultPatch;
	TestFalse(TEXT("default brush has no id"), DefaultBrush.Id.IsValid());
	TestFalse(TEXT("default patch has no id"), DefaultPatch.Id.IsValid());

	const FSurfBrush BoxA = SurfBrushMath::MakeBox(FBox(FVector(0.0), FVector(100.0)));
	const FSurfBrush BoxB = SurfBrushMath::MakeBox(FBox(FVector(0.0), FVector(100.0)));
	TestTrue(TEXT("a created brush has an id"), BoxA.Id.IsValid());
	TestTrue(TEXT("created brushes have distinct ids"), BoxA.Id != BoxB.Id);

	const FSurfPatch PatchA = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 3, 3, FVector::ZeroVector, FVector(100.0, 0.0, 0.0), FVector(0.0, 100.0, 0.0));
	TestTrue(TEXT("a created patch has an id"), PatchA.Id.IsValid());

	// EnsureId is idempotent - it must never re-roll an id that already exists.
	FSurfBrush Keep = BoxA;
	const FGuid Original = Keep.Id;
	Keep.EnsureId();
	TestEqual(TEXT("EnsureId does not replace an existing id"), Keep.Id, Original);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfStackOffsetTest, "SurfacingPro.Math.Layering.StackOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfStackOffsetTest::RunTest(const FString& Parameters)
{
	// D14: stacked layers are separated by the compiler, not by world position
	// offset in a material. Layer N sits N * LayerOffsetCm along the normal.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	FSurfTessellation Base;
	SurfPatchMath::Tessellate(Patch, Base);

	FSurfTessellation Layer2 = Base;
	SurfPatchMath::ApplyStackOffset(Layer2, 2, 0.5);

	for (int32 I = 0; I < Base.Positions.Num(); ++I)
	{
		const double Delta = (Layer2.Positions[I] - Base.Positions[I]).Length();
		TestTrue(TEXT("layer 2 sits two offsets above the base"), SurfTestUtil::NearlyEqual(Delta, 1.0));
	}

	FSurfTessellation Untouched = Base;
	SurfPatchMath::ApplyStackOffset(Untouched, 0, 0.5);
	TestTrue(TEXT("base layer is not displaced"),
		SurfTestUtil::VectorNearlyEqual(Untouched.Positions[0], Base.Positions[0]));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPrimitivesTest, "SurfacingPro.Math.Patch.Primitives",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPrimitivesTest::RunTest(const FString& Parameters)
{
	// A 200 cm box centred on the origin: radius 100, heights -100 / 0 / 100.
	const FBox Box(FVector(-100.0), FVector(100.0));

	struct FCase { ESurfPrimitiveKind Kind; int32 W; int32 H; const TCHAR* Name; };
	const FCase Cases[] =
	{
		{ ESurfPrimitiveKind::Bevel,    3, 3, TEXT("bevel") },
		{ ESurfPrimitiveKind::Endcap,   5, 3, TEXT("endcap") },
		{ ESurfPrimitiveKind::Cylinder, 9, 3, TEXT("cylinder") },
		{ ESurfPrimitiveKind::Cone,     9, 3, TEXT("cone") }
	};

	for (const FCase& Case : Cases)
	{
		const FSurfPatch Patch = SurfPatchPrimitives::Make(Case.Kind, Box);

		TestEqual(*FString::Printf(TEXT("%s width"), Case.Name), Patch.Width, Case.W);
		TestEqual(*FString::Printf(TEXT("%s height"), Case.Name), Patch.Height, Case.H);
		TestTrue(*FString::Printf(TEXT("%s has legal dimensions"), Case.Name), Patch.HasValidDimensions());
		TestTrue(*FString::Printf(TEXT("%s has an id"), Case.Name), Patch.Id.IsValid());

		FSurfTessellation Tess;
		TestTrue(*FString::Printf(TEXT("%s tessellates"), Case.Name), SurfPatchMath::Tessellate(Patch, Tess));
		TestTrue(*FString::Printf(TEXT("%s produces triangles"), Case.Name), Tess.NumTriangles() > 0);

		// Every control point sits inside the box it was built from - that is
		// the property that makes the primitives predictable.
		for (const FSurfControlPoint& Point : Patch.Points)
		{
			// Expand the box by a hair rather than nudging the point: a control
			// point sitting exactly on a corner is outside a shifted box in
			// both directions at once.
			TestTrue(*FString::Printf(TEXT("%s stays inside its box"), Case.Name),
				Box.ExpandBy(0.01).IsInsideOrOn(Point.Pos));
		}
	}

	// A cylinder closes: first and last column coincide.
	{
		const FSurfPatch Cyl = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Cylinder, Box);
		for (int32 Y = 0; Y < Cyl.Height; ++Y)
		{
			TestTrue(TEXT("the cylinder wraps closed"),
				SurfTestUtil::VectorNearlyEqual(Cyl.At(0, Y).Pos, Cyl.At(Cyl.Width - 1, Y).Pos));
		}
	}

	// A cone's top row is a single point.
	{
		const FSurfPatch Cone = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Cone, Box);
		const FVector Apex = Cone.At(0, 2).Pos;
		for (int32 X = 1; X < Cone.Width; ++X)
		{
			TestTrue(TEXT("the cone collapses to an apex"), SurfTestUtil::VectorNearlyEqual(Cone.At(X, 2).Pos, Apex));
		}
		TestTrue(TEXT("the apex is at the top of the box"), SurfTestUtil::NearlyEqual(Apex.Z, 100.0));
	}

	// The arc reaches the box on its axes: a quarter bevel starts at +X and
	// ends at +Y, both at full radius.
	{
		const FSurfPatch Bevel = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Bevel, Box);
		TestTrue(TEXT("the bevel starts on +X"), SurfTestUtil::NearlyEqual(Bevel.At(0, 0).Pos.X, 100.0));
		TestTrue(TEXT("the bevel ends on +Y"), SurfTestUtil::NearlyEqual(Bevel.At(2, 0).Pos.Y, 100.0));
		TestTrue(TEXT("the bevel's middle point is the box corner"),
			SurfTestUtil::NearlyEqual(Bevel.At(1, 0).Pos.X, 100.0) && SurfTestUtil::NearlyEqual(Bevel.At(1, 0).Pos.Y, 100.0));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfRowColumnTest, "SurfacingPro.Math.Patch.RowsAndColumns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfRowColumnTest::RunTest(const FString& Parameters)
{
	const FBox Box(FVector(-100.0), FVector(100.0));

	// Curves gain and lose two at a time so the dimensions stay odd.
	{
		FSurfPatch Curve = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Endcap, Box);   // 5 x 3
		const FVector BeforeStart = Curve.At(0, 0).Pos;
		const FVector BeforeEnd = Curve.At(4, 0).Pos;

		TestTrue(TEXT("insert a column into a curve"), SurfPatchPrimitives::InsertColumn(Curve, 1));
		TestEqual(TEXT("a curve gains two columns"), Curve.Width, 7);
		TestTrue(TEXT("it stays odd and legal"), Curve.HasValidDimensions());
		TestTrue(TEXT("the ends do not move"),
			SurfTestUtil::VectorNearlyEqual(Curve.At(0, 0).Pos, BeforeStart) &&
			SurfTestUtil::VectorNearlyEqual(Curve.At(Curve.Width - 1, 0).Pos, BeforeEnd));

		TestTrue(TEXT("remove a column from a curve"), SurfPatchPrimitives::RemoveColumn(Curve, 1));
		TestEqual(TEXT("a curve loses two columns"), Curve.Width, 5);
		TestTrue(TEXT("it is still legal"), Curve.HasValidDimensions());

		// A 3-wide curve is at the minimum and must refuse to shrink.
		FSurfPatch Bevel = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Bevel, Box);
		TestFalse(TEXT("a minimum curve refuses to lose a column"), SurfPatchPrimitives::RemoveColumn(Bevel, 0));
		TestEqual(TEXT("and is left untouched"), Bevel.Width, 3);
	}

	// Sheets move one at a time.
	{
		FSurfPatch Sheet = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Sheet, Box, 4, 4);
		TestEqual(TEXT("a sheet takes the size asked for"), Sheet.Width, 4);

		TestTrue(TEXT("insert a row into a sheet"), SurfPatchPrimitives::InsertRow(Sheet, 1));
		TestEqual(TEXT("a sheet gains one row"), Sheet.Height, 5);
		TestEqual(TEXT("the point array keeps pace"), Sheet.Points.Num(), Sheet.Width * Sheet.Height);

		TestTrue(TEXT("remove a row from a sheet"), SurfPatchPrimitives::RemoveRow(Sheet, 1));
		TestEqual(TEXT("a sheet loses one row"), Sheet.Height, 4);
	}

	// Respace evens the interior out and leaves the corners alone.
	{
		FSurfPatch Sheet = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Sheet, Box, 3, 3);
		const FVector Corner = Sheet.At(0, 0).Pos;
		Sheet.At(1, 0).Pos.X = 95.0;   // bunch the middle point against one end

		SurfPatchPrimitives::Respace(Sheet);

		TestTrue(TEXT("respace centres the middle point"), SurfTestUtil::NearlyEqual(Sheet.At(1, 0).Pos.X, 0.0));
		TestTrue(TEXT("respace leaves the corner alone"), SurfTestUtil::VectorNearlyEqual(Sheet.At(0, 0).Pos, Corner));
	}

	// A closed row - a tube, where the ends are the same point - keeps its
	// shape. The first version measured along the chord between the ends,
	// which for a closed row is a point, and the whole patch became a line.
	{
		FSurfPatch Tube;
		Tube.Type = ESurfPatchType::Sheet;
		Tube.EnsureId();
		Tube.Width = 9;
		Tube.Height = 2;
		Tube.Points.SetNum(Tube.Width * Tube.Height);

		const double Radius = 100.0;
		for (int32 Y = 0; Y < 2; ++Y)
		{
			for (int32 X = 0; X < 9; ++X)
			{
				// Unevenly spaced round the circle, ends coincident.
				const double Fraction = (X == 8) ? 1.0 : FMath::Pow(static_cast<double>(X) / 8.0, 1.5);
				const double Angle = Fraction * 2.0 * PI;
				Tube.At(X, Y).Pos = FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Y * 100.0);
			}
		}

		SurfPatchPrimitives::Respace(Tube);

		double WorstRadius = 0.0;
		for (int32 X = 0; X < 9; ++X)
		{
			const FVector& P = Tube.At(X, 0).Pos;
			WorstRadius = FMath::Max(WorstRadius, FMath::Abs(FVector2D(P.X, P.Y).Size() - Radius));
		}

		// Along the polyline, not the true circle, so a little inside the
		// radius is expected; collapsed to a point would be 100 out.
		TestTrue(FString::Printf(TEXT("a closed row keeps its shape after respace (worst %.1f cm off the circle)"), WorstRadius),
			WorstRadius < 15.0);

		// And the spacing is even: neighbouring gaps agree.
		double MinGap = TNumericLimits<double>::Max();
		double MaxGap = 0.0;
		for (int32 X = 1; X < 9; ++X)
		{
			const double Gap = FVector::Dist(Tube.At(X, 0).Pos, Tube.At(X - 1, 0).Pos);
			MinGap = FMath::Min(MinGap, Gap);
			MaxGap = FMath::Max(MaxGap, Gap);
		}
		TestTrue(FString::Printf(TEXT("and the spacing is even (%.1f to %.1f cm)"), MinGap, MaxGap),
			MaxGap - MinGap < 0.35 * MaxGap);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchNaturalTest, "SurfacingPro.Math.Patch.Natural",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchNaturalTest::RunTest(const FString& Parameters)
{
	// Two metres square, one metre per tile: two tiles across, whatever the
	// patch is made of. This is the number a mapper reads off the panel.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	Patch.Scale = FVector2D(1.0, 1.0);
	SurfPatchSurfacing::SetNatural(Patch);

	TestTrue(TEXT("the near corner is the origin of the mapping"),
		Patch.At(0, 0).UV.Equals(FVector2D::ZeroVector, 1.e-4));
	TestTrue(TEXT("two metres at one metre per tile is two tiles"),
		FMath::IsNearlyEqual(Patch.At(2, 0).UV.X, 2.0, 1.e-4));
	TestTrue(TEXT("and the same down the other axis"),
		FMath::IsNearlyEqual(Patch.At(0, 2).UV.Y, 2.0, 1.e-4));
	TestTrue(TEXT("the midpoint is halfway"),
		FMath::IsNearlyEqual(Patch.At(1, 0).UV.X, 1.0, 1.e-4));

	// Scale is metres per tile, so doubling it halves the tile count.
	SurfPatchSurfacing::SetScale(Patch, FVector2D(2.0, 2.0));
	TestTrue(TEXT("two metres per tile halves the count"),
		FMath::IsNearlyEqual(Patch.At(2, 0).UV.X, 1.0, 1.e-4));

	// The property that makes this worth storing: stretch the geometry and
	// the texture keeps its density instead of smearing.
	SurfPatchSurfacing::SetScale(Patch, FVector2D(1.0, 1.0));
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		Patch.At(2, Y).Pos.X = 400.0;
		Patch.At(1, Y).Pos.X = 200.0;
	}
	SurfPatchSurfacing::Reapply(Patch);

	TestTrue(TEXT("doubling the patch doubles the tiles, not the stretch"),
		FMath::IsNearlyEqual(Patch.At(2, 0).UV.X, 4.0, 1.e-4));

	const FVector2D Density = SurfPatchSurfacing::TexelDensity(Patch, FVector2D(1024.0, 1024.0));
	TestTrue(TEXT("one metre per tile at 1024 is 1024 texels per metre"),
		FMath::IsNearlyEqual(Density.X, 1024.0, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchNaturalCurvedTest, "SurfacingPro.Math.Patch.NaturalFollowsCurve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchNaturalCurvedTest::RunTest(const FString& Parameters)
{
	// The case a planar projection cannot serve: a surface that bends away
	// from its own normal. Natural must charge the bend its real length.
	FSurfPatch Flat = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 2,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 100.0, 0.0));
	Flat.Scale = FVector2D(1.0, 1.0);
	SurfPatchSurfacing::SetNatural(Flat);

	const double FlatSpan = Flat.At(2, 0).UV.X;

	FSurfPatch Bent = Flat;
	Bent.EnsureId();
	for (int32 Y = 0; Y < Bent.Height; ++Y)
	{
		Bent.At(1, Y).Pos.Z = 100.0;  // a peak in the middle: the row got longer
	}
	SurfPatchSurfacing::Reapply(Bent);

	const double BentSpan = Bent.At(2, 0).UV.X;

	TestTrue(TEXT("a bent row spans more tiles than a flat one"), BentSpan > FlatSpan + 0.1);

	// Each half is a 100 by 100 right triangle's hypotenuse, so the row is
	// 2 * sqrt(2) metres and the mapping should say so.
	TestTrue(TEXT("the bend is charged its real arc length"),
		FMath::IsNearlyEqual(BentSpan, 2.0 * FMath::Sqrt(2.0), 1.e-3));

	// A planar projection, by contrast, measures the shadow - which is the
	// whole reason both mappings exist.
	SurfPatchSurfacing::SetPlanar(Bent);
	TestTrue(TEXT("the same patch projected is not the same mapping"),
		!FMath::IsNearlyEqual(FMath::Abs(Bent.At(2, 0).UV.X - Bent.At(0, 0).UV.X), BentSpan, 1.e-2));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchSurfaceVerbsTest, "SurfacingPro.Math.Patch.SurfaceVerbs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchSurfaceVerbsTest::RunTest(const FString& Parameters)
{
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 500.0, 0.0));

	Patch.Scale = FVector2D(1.0, 1.0);
	SurfPatchSurfacing::SetNatural(Patch);

	// Fit chooses the scale, so the answer survives a later edit. Three
	// metres across in two tiles is 1.5 metres per tile.
	SurfPatchSurfacing::FitScale(Patch, 2, 1);
	TestTrue(TEXT("fit picks the scale that spans the tiles asked for"),
		FMath::IsNearlyEqual(Patch.Scale.X, 1.5, 1.e-4));
	TestTrue(TEXT("and spans exactly that many"),
		FMath::IsNearlyEqual(Patch.At(2, 0).UV.X, 2.0, 1.e-4));
	TestTrue(TEXT("the other axis spans one"),
		FMath::IsNearlyEqual(Patch.At(0, 2).UV.Y, 1.0, 1.e-4));

	// Shift is stored, not baked, so it is still there after a re-derive.
	SurfPatchSurfacing::Shift(Patch, 0.25, 0.0);
	TestTrue(TEXT("shift moves the mapping by whole tiles"),
		FMath::IsNearlyEqual(Patch.At(0, 0).UV.X, 0.25, 1.e-4));

	SurfPatchSurfacing::Reapply(Patch);
	TestTrue(TEXT("and survives a re-derive"),
		FMath::IsNearlyEqual(Patch.At(0, 0).UV.X, 0.25, 1.e-4));

	// Swap exchanges the axes; doing it twice is doing nothing. Spans rather
	// than absolute values, because shift is applied after the swap - it moves
	// the texture along the axis the mapper sees, which is the useful order.
	const FVector2D Before = Patch.At(2, 0).UV;
	const double USpanBeforeSwap = Patch.At(2, 0).UV.X - Patch.At(0, 0).UV.X;

	SurfPatchSurfacing::SwapUV(Patch);
	TestTrue(TEXT("swap exchanges the axes"),
		FMath::IsNearlyEqual(Patch.At(2, 0).UV.Y - Patch.At(0, 0).UV.Y, USpanBeforeSwap, 1.e-3));

	SurfPatchSurfacing::SwapUV(Patch);
	TestTrue(TEXT("swapping twice is a no-op"), Patch.At(2, 0).UV.Equals(Before, 1.e-4));

	// Flip reverses the axis within its own span, leaving the span alone.
	const double SpanBefore = Patch.At(2, 0).UV.X - Patch.At(0, 0).UV.X;
	SurfPatchSurfacing::FlipU(Patch);
	const double SpanAfter = Patch.At(0, 0).UV.X - Patch.At(2, 0).UV.X;
	TestTrue(TEXT("flip reverses without rescaling"), FMath::IsNearlyEqual(SpanBefore, SpanAfter, 1.e-3));
	SurfPatchSurfacing::FlipU(Patch);

	// Rotating by a right angle turns a U span into a V span.
	SurfPatchSurfacing::Rotate(Patch, 90.0);
	TestTrue(TEXT("a right angle turns U into V"),
		FMath::IsNearlyEqual(FMath::Abs(Patch.At(2, 0).UV.Y - Patch.At(0, 0).UV.Y), SpanBefore, 1.e-3));
	SurfPatchSurfacing::Rotate(Patch, -90.0);

	// Freezing stops the rule from running, which is the point of freezing.
	SurfPatchSurfacing::SetManual(Patch);
	const FVector2D Frozen = Patch.At(1, 1).UV;
	Patch.At(1, 1).Pos.Z = 400.0;
	SurfPatchSurfacing::Reapply(Patch);
	TestTrue(TEXT("a frozen mapping ignores geometry changes"), Patch.At(1, 1).UV.Equals(Frozen, 1.e-6));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchInheritSurfaceTest, "SurfacingPro.Math.Patch.InheritFaceSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchInheritSurfaceTest::RunTest(const FString& Parameters)
{
	// The seam case: a patch that meets a wall has to be mapped by the wall's
	// own projection, or the texture steps at the join.
	FSurfFace Face;
	Face.Plane = FPlane(FVector(0.0, 0.0, 1.0), 0.0);
	Face.UV.Scale = FVector2D(2.0, 2.0);
	Face.UV.Shift = FVector2D(37.0, -11.0);
	SurfProjection::Align(Face.UV, FVector::UpVector);

	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector(500.0, 250.0, 0.0), FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	SurfPatchSurfacing::CopyFaceSurface(Face, Patch);

	TestTrue(TEXT("the patch takes the face's mapping mode"), Patch.Mapping == ESurfPatchMapping::Planar);
	TestTrue(TEXT("and its scale"), Patch.Scale.Equals(FVector2D(2.0, 2.0), 1.e-6));

	// Every control point must land exactly where the face's projection puts
	// that same world position. Anything less and the seam is visible.
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		for (int32 X = 0; X < Patch.Width; ++X)
		{
			const FSurfControlPoint& Point = Patch.At(X, Y);
			const FVector2D Expected = SurfProjection::ComputeUV(Face.UV, Point.Pos);

			if (!Point.UV.Equals(Expected, 1.e-4))
			{
				AddError(FString::Printf(
					TEXT("control point %d,%d maps to %s but the face maps the same position to %s"),
					X, Y, *Point.UV.ToString(), *Expected.ToString()));
				return false;
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchPrimitiveSurfacedTest, "SurfacingPro.Math.Patch.PrimitivesArriveSurfaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchPrimitiveSurfacedTest::RunTest(const FString& Parameters)
{
	// A curved primitive with parametric UVs is wrong on arrival, so every
	// primitive is surfaced as it is built.
	const FBox Box(FVector(0.0), FVector(200.0, 200.0, 300.0));

	const ESurfPrimitiveKind Kinds[] = {
		ESurfPrimitiveKind::Simple, ESurfPrimitiveKind::Cylinder, ESurfPrimitiveKind::Bevel,
		ESurfPrimitiveKind::Endcap, ESurfPrimitiveKind::Cone,     ESurfPrimitiveKind::Sheet };

	for (const ESurfPrimitiveKind Kind : Kinds)
	{
		FSurfPatch Patch = SurfPatchPrimitives::Make(Kind, Box, 5, 5);

		double RowCm = 0.0;
		double ColumnCm = 0.0;
		SurfPatchSurfacing::MeasureExtent(Patch, RowCm, ColumnCm);

		const double SpanU = FMath::Abs(Patch.At(Patch.Width - 1, 0).UV.X - Patch.At(0, 0).UV.X);

		// One metre per tile by default, so the tile count is the metre count.
		TestTrue(*FString::Printf(TEXT("kind %d spans its own length in tiles"), static_cast<int32>(Kind)),
			FMath::IsNearlyEqual(SpanU, RowCm * 0.01, 1.e-3));
		TestTrue(*FString::Printf(TEXT("kind %d is not left parametric"), static_cast<int32>(Kind)),
			RowCm < 50.0 || SpanU > 1.0001);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSettingsPersistTest, "SurfacingPro.Settings.Persist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSettingsPersistTest::RunTest(const FString& Parameters)
{
	// A setting saved reaches the user's per-project editor config. As a
	// defaultconfig class writing the project's Game config, nothing set in
	// the settings window ever reached the disk: the editor writes that
	// file only for the sections it lists, and ours was not one.
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	if (!Settings || !GConfig)
	{
		AddError(TEXT("no settings or no config"));
		return false;
	}

	const FString Section = TEXT("/Script/SurfacingPro.SurfSettings");
	const float Before = Settings->PointPickRadiusPx;
	const float Marker = 23.5f;

	Settings->PointPickRadiusPx = Marker;
	Settings->SaveConfig();

	FString Written;
	const bool bFound = GConfig->GetString(*Section, TEXT("PointPickRadiusPx"), Written, GEditorPerProjectIni);
	TestTrue(TEXT("the setting is in the per-project user config after SaveConfig"), bFound);
	TestTrue(TEXT("with the value saved"), bFound && FMath::IsNearlyEqual(FCString::Atof(*Written), Marker, 0.01f));
	TestTrue(TEXT("the config file is the editor's per-project one"), GEditorPerProjectIni.Contains(TEXT("EditorPerProjectUserSettings")));

	Settings->PointPickRadiusPx = Before;
	Settings->SaveConfig();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfKeymapTest, "SurfacingPro.Settings.Keymap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfKeymapTest::RunTest(const FString& Parameters)
{
	// Both schemes must bind every action they claim, or a key silently does
	// nothing and the user concludes the feature is missing.
	const ESurfKeyScheme Schemes[] = { ESurfKeyScheme::SurfacingPro, ESurfKeyScheme::Blender };

	for (const ESurfKeyScheme Scheme : Schemes)
	{
		TMap<FString, ESurfAction> Seen;

		for (int32 I = 1; I < static_cast<int32>(ESurfAction::Count); ++I)
		{
			const ESurfAction Action = static_cast<ESurfAction>(I);
			const FSurfKeyBinding Binding = SurfKeymap::Default(Scheme, Action);

			// The brush has no key of its own in the Blender scheme (S158d):
			// W cycles the tools and reaches it.
			const bool bMayBeUnbound = Scheme == ESurfKeyScheme::Blender && Action == ESurfAction::BrushSelect;
			if (bMayBeUnbound && !Binding.Key.IsValid())
			{
				continue;
			}
			if (!TestTrue(*FString::Printf(TEXT("scheme %d binds %s"),
				static_cast<int32>(Scheme), *SurfKeymap::ActionName(Action)), Binding.Key.IsValid()))
			{
				continue;
			}

			// Two actions on one chord means one of them is unreachable.
			const FString Chord = FString::Printf(TEXT("%s%s%s%s"),
				Binding.bCtrl ? TEXT("C") : TEXT(""), Binding.bShift ? TEXT("S") : TEXT(""),
				Binding.bAlt ? TEXT("A") : TEXT(""), *Binding.Key.ToString());

			if (const ESurfAction* Clash = Seen.Find(Chord))
			{
				AddError(FString::Printf(TEXT("scheme %d gives %s to both %s and %s"),
					static_cast<int32>(Scheme), *Chord,
					*SurfKeymap::ActionName(*Clash), *SurfKeymap::ActionName(Action)));
			}

			Seen.Add(Chord, Action);
		}
	}

	// Blender's modelling keys are the reason that scheme exists.
	TestTrue(TEXT("Blender puts move on G"),
		SurfKeymap::Default(ESurfKeyScheme::Blender, ESurfAction::Translate).Key == EKeys::G);
	TestFalse(TEXT("Blender gives the brush no key of its own - W cycles to it (S158d)"),
		SurfKeymap::Default(ESurfKeyScheme::Blender, ESurfAction::BrushSelect).Key.IsValid());
	TestTrue(TEXT("so the slice moves to the knife key"),
		SurfKeymap::Default(ESurfKeyScheme::Blender, ESurfAction::Slice).Key == EKeys::K);

	// Focus is F in both, because that is what this editor has trained
	// everyone to press, whichever package they came from.
	for (const ESurfKeyScheme Scheme : Schemes)
	{
		TestTrue(TEXT("focus is F in every scheme"),
			SurfKeymap::Default(Scheme, ESurfAction::FocusSelection).Key == EKeys::F);
	}

	// A modified chord must beat the bare key, or Ctrl+R fires plain R.
	const FSurfKeyBinding Loop = SurfKeymap::Resolve(ESurfAction::InsertLoop);
	TestTrue(TEXT("insert loop is a Ctrl chord"), Loop.bCtrl);
	TestTrue(TEXT("and the modified chord wins over the bare key"),
		SurfKeymap::Match(Loop.Key, true, false, false) == ESurfAction::InsertLoop);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfConvertToSheetTest, "SurfacingPro.Math.Patch.ConvertToSheet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfConvertToSheetTest::RunTest(const FString& Parameters)
{
	// "How do I apply this and edit the actual geometry?" - a curve's control
	// points steer the surface from outside it; a sheet's points are the
	// surface. This is the conversion between the two.
	const FBox Box(FVector(0.0), FVector(200.0, 200.0, 300.0));
	FSurfPatch Curve = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Cylinder, Box);
	Curve.SubdivU = 8;
	Curve.SubdivV = 8;

	FSurfPatch Sheet;
	int32 Subdivision = 0;
	TestTrue(TEXT("a curve converts"), SurfPatchPrimitives::ConvertToSheet(Curve, Sheet, &Subdivision));

	TestTrue(TEXT("the result is a sheet"), Sheet.Type == ESurfPatchType::Sheet);
	TestTrue(TEXT("the sheet is valid"), Sheet.HasValidDimensions());
	TestTrue(TEXT("it is denser than the cage it came from"),
		Sheet.NumPoints() > Curve.NumPoints());
	TestTrue(TEXT("it has a new identity"), Sheet.Id.IsValid() && Sheet.Id != Curve.Id);

	// A sheet's points are its vertices, so subdivision no longer applies -
	// which is exactly why it appears to "do nothing" on a sheet.
	TestEqual(TEXT("subdivision is retired"), Sheet.SubdivU, 1);

	// Dimensions are capped, and the chosen subdivision must be honest about it.
	TestTrue(TEXT("width fits a sheet"), Sheet.Width <= FSurfPatch::MaxSheetDim);
	TestTrue(TEXT("height fits a sheet"), Sheet.Height <= FSurfPatch::MaxSheetDim);
	TestTrue(TEXT("the subdivision used is reported"), Subdivision >= FSurfPatch::MinSubdiv);

	// The shape has to survive: every sheet point sits on the curve's surface.
	FSurfTessellation Reference;
	FSurfPatch AtUsed = Curve;
	AtUsed.SubdivU = Subdivision;
	AtUsed.SubdivV = Subdivision;

	if (SurfPatchMath::Tessellate(AtUsed, Reference) && Reference.NumU == Sheet.Width)
	{
		double Worst = 0.0;
		for (int32 I = 0; I < Sheet.NumPoints() && I < Reference.Positions.Num(); ++I)
		{
			Worst = FMath::Max(Worst, FVector::Dist(Sheet.Points[I].Pos, Reference.Positions[I]));
		}
		TestTrue(*FString::Printf(TEXT("the surface is unchanged (worst %.4f cm)"), Worst), Worst < 1.e-3);
	}

	// Converting a sheet is refused rather than silently doing nothing twice.
	FSurfPatch Again;
	TestFalse(TEXT("a sheet does not convert again"), SurfPatchPrimitives::ConvertToSheet(Sheet, Again));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchNormalTest, "SurfacingPro.Math.Patch.Normals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchNormalTest::RunTest(const FString& Parameters)
{
	// A flat patch built on the top of a box has to face up. And inverting it
	// has to actually invert it: the winding flip and a normal negation used
	// to cancel, so an inverted patch was lit as though it were not.
	const FBox Box(FVector(0.0), FVector(200.0, 200.0, 100.0));

	FSurfPatch Up = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Sheet, Box, 3, 3, FVector::UpVector);

	FSurfTessellation Tess;
	if (!SurfPatchMath::Tessellate(Up, Tess) || Tess.Normals.Num() == 0)
	{
		AddError(TEXT("a flat sheet did not tessellate"));
		return false;
	}

	for (const FVector& Normal : Tess.Normals)
	{
		if (Normal.Z < 0.9)
		{
			AddError(FString::Printf(TEXT("a sheet made on an upward face points %s"), *Normal.ToString()));
			break;
		}
	}

	// It also has to sit on that face rather than on the box floor - a sheet
	// made from the top of a block belongs on top of the block.
	for (const FSurfControlPoint& Point : Up.Points)
	{
		TestTrue(TEXT("an upward sheet sits on the top of the box"),
			FMath::IsNearlyEqual(Point.Pos.Z, Box.Max.Z, 1.e-3));
	}

	FSurfPatch Down = SurfPatchPrimitives::Make(ESurfPrimitiveKind::Sheet, Box, 3, 3, -FVector::UpVector);
	FSurfTessellation DownTess;
	if (SurfPatchMath::Tessellate(Down, DownTess) && DownTess.Normals.Num() > 0)
	{
		TestTrue(TEXT("a sheet made on a downward face points down"), DownTess.Normals[0].Z < -0.9);
		TestTrue(TEXT("and sits on the bottom of the box"),
			FMath::IsNearlyEqual(Down.Points[0].Pos.Z, Box.Min.Z, 1.e-3));
	}

	// Invert flips the normals, rather than flipping the winding and then
	// undoing it on the normals.
	FSurfPatch Inverted = Up;
	Inverted.bInvert = true;

	FSurfTessellation InvertedTess;
	if (!SurfPatchMath::Tessellate(Inverted, InvertedTess) || InvertedTess.Normals.Num() == 0)
	{
		AddError(TEXT("an inverted sheet did not tessellate"));
		return false;
	}

	TestTrue(TEXT("inverting actually inverts the normal"), InvertedTess.Normals[0].Z < -0.9);

	// Winding and normal are deliberately opposite under the right-hand rule,
	// because Unreal treats that order as front-facing. Agreeing here would
	// mean the surface is culled from the side it is lit on.
	const FVector& P0 = InvertedTess.Positions[InvertedTess.Indices[0]];
	const FVector& P1 = InvertedTess.Positions[InvertedTess.Indices[1]];
	const FVector& P2 = InvertedTess.Positions[InvertedTess.Indices[2]];
	const FVector Wound = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();

	TestTrue(TEXT("winding is Unreal's front-face order for this normal"),
		FVector::DotProduct(Wound, InvertedTess.Normals[0]) < -0.9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfFalloffTest, "SurfacingPro.Settings.Falloff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfFalloffTest::RunTest(const FString& Parameters)
{
	// Whatever the curve, the ends are fixed: full pull at the selection,
	// nothing at the radius. A falloff that leaks past its radius is a drag
	// that moves the whole patch.
	const ESurfFalloff Shapes[] = {
		ESurfFalloff::Smooth, ESurfFalloff::Sphere, ESurfFalloff::Root,
		ESurfFalloff::Sharp,  ESurfFalloff::Linear, ESurfFalloff::Constant, ESurfFalloff::InverseSquare };

	// Random is its own thing: a fixed share per point of a linear
	// falloff, the same share every time for the same point, never past 1
	// and nothing at the radius.
	TestTrue(TEXT("random is the same for the same point"),
		FMath::IsNearlyEqual(USurfSettings::Falloff(ESurfFalloff::Random, 0.3, 7), USurfSettings::Falloff(ESurfFalloff::Random, 0.3, 7), 1.e-9));
	TestTrue(TEXT("and differs between points"),
		!FMath::IsNearlyEqual(USurfSettings::Falloff(ESurfFalloff::Random, 0.3, 7), USurfSettings::Falloff(ESurfFalloff::Random, 0.3, 8), 1.e-3));
	TestTrue(TEXT("random stays inside 0..1"),
		USurfSettings::Falloff(ESurfFalloff::Random, 0.0, 3) >= 0.0 && USurfSettings::Falloff(ESurfFalloff::Random, 0.0, 3) <= 1.0);
	TestTrue(TEXT("random is 0 at the radius"), FMath::IsNearlyEqual(USurfSettings::Falloff(ESurfFalloff::Random, 1.0, 3), 0.0, 1.e-9));
	TestTrue(TEXT("inverse square falls fast near the selection"),
		USurfSettings::Falloff(ESurfFalloff::InverseSquare, 0.5) > USurfSettings::Falloff(ESurfFalloff::Linear, 0.5));
	TestTrue(TEXT("every falloff has a name"), USurfSettings::FalloffName(ESurfFalloff::Random) == TEXT("random"));

	for (const ESurfFalloff Shape : Shapes)
	{
		const int32 Id = static_cast<int32>(Shape);

		TestTrue(*FString::Printf(TEXT("falloff %d is 1 at the selection"), Id),
			FMath::IsNearlyEqual(USurfSettings::Falloff(Shape, 0.0), 1.0, 1.e-3));

		TestTrue(*FString::Printf(TEXT("falloff %d is 0 at the radius"), Id),
			Shape == ESurfFalloff::Constant || FMath::IsNearlyEqual(USurfSettings::Falloff(Shape, 1.0), 0.0, 1.e-3));

		TestTrue(*FString::Printf(TEXT("falloff %d stays inside 0..1"), Id),
			USurfSettings::Falloff(Shape, 0.5) >= 0.0 && USurfSettings::Falloff(Shape, 0.5) <= 1.0);

		// Past the radius nothing follows, however the curve is written.
		TestTrue(*FString::Printf(TEXT("falloff %d does not leak past the radius"), Id),
			FMath::IsNearlyEqual(USurfSettings::Falloff(Shape, 2.0), Shape == ESurfFalloff::Constant ? 0.0 : 0.0, 1.e-3));
	}

	// Monotonic, or a point further away would follow more than a near one.
	for (const ESurfFalloff Shape : Shapes)
	{
		double Previous = 2.0;
		for (double D = 0.0; D <= 1.0; D += 0.1)
		{
			const double Value = USurfSettings::Falloff(Shape, D);
			if (Value > Previous + 1.e-6)
			{
				AddError(FString::Printf(TEXT("falloff %d rises at %.1f"), static_cast<int32>(Shape), D));
				break;
			}
			Previous = Value;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPatchOpsTest, "SurfacingPro.Math.Patch.ShapeOperators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPatchOpsTest::RunTest(const FString& Parameters)
{
	// The property live preview depends on: an operator is a pure function of
	// its input, so running it from the original twice gives the same answer.
	// Anything that accumulates cannot be scrubbed with a slider.
	auto MakeBumpy = []()
	{
		FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
			ESurfPatchType::Sheet, 5, 5,
			FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0));

		// A spike in the middle, which is what smoothing has to take down.
		Patch.At(2, 2).Pos.Z = 200.0;
		return Patch;
	};

	const FSurfPatch Original = MakeBumpy();

	FSurfPatch A = Original;
	FSurfPatch B = Original;
	SurfPatchOps::Smooth(A, {}, 0.5, 4, true);
	SurfPatchOps::Smooth(B, {}, 0.5, 4, true);

	for (int32 I = 0; I < A.NumPoints(); ++I)
	{
		if (!A.Points[I].Pos.Equals(B.Points[I].Pos, 1.e-6))
		{
			AddError(TEXT("smoothing is not deterministic, so it cannot be previewed"));
			break;
		}
	}

	// It has to actually smooth: the spike comes down.
	TestTrue(TEXT("smoothing lowers a spike"), A.At(2, 2).Pos.Z < Original.At(2, 2).Pos.Z - 1.0);
	TestTrue(TEXT("but does not invert it"), A.At(2, 2).Pos.Z > -1.0);

	// Strength zero is a no-op, which is what makes the slider's low end safe.
	FSurfPatch Zero = Original;
	SurfPatchOps::Smooth(Zero, {}, 0.0, 10, true);
	TestTrue(TEXT("zero strength changes nothing"), Zero.At(2, 2).Pos.Equals(Original.At(2, 2).Pos, 1.e-6));

	// The border is held, so a smoothed patch still meets its neighbours.
	FSurfPatch Held = Original;
	SurfPatchOps::Smooth(Held, {}, 1.0, 20, true);
	TestTrue(TEXT("a held border does not move"), Held.At(0, 0).Pos.Equals(Original.At(0, 0).Pos, 1.e-6));

	FSurfPatch Free = Original;
	Free.At(0, 0).Pos.Z = 150.0;
	const FVector FreeCornerBefore = Free.At(0, 0).Pos;
	SurfPatchOps::Smooth(Free, {}, 1.0, 8, false);
	TestTrue(TEXT("a free border does move"), !Free.At(0, 0).Pos.Equals(FreeCornerBefore, 1.e-3));

	// Only the given points, when points are given.
	FSurfPatch Partial = Original;
	const FVector UntouchedBefore = Partial.At(1, 1).Pos;
	SurfPatchOps::Smooth(Partial, { 2 * 5 + 2 }, 1.0, 4, false);
	TestTrue(TEXT("an unselected point is left alone"), Partial.At(1, 1).Pos.Equals(UntouchedBefore, 1.e-6));
	TestTrue(TEXT("the selected one is not"), !Partial.At(2, 2).Pos.Equals(Original.At(2, 2).Pos, 1.e-3));

	// Flatten pulls onto the best-fit plane; at full strength it is flat.
	FSurfPatch Flat = MakeBumpy();
	SurfPatchOps::Flatten(Flat, {}, 1.0);

	double MinZ = TNumericLimits<double>::Max();
	double MaxZ = -TNumericLimits<double>::Max();
	for (const FSurfControlPoint& Point : Flat.Points)
	{
		MinZ = FMath::Min(MinZ, Point.Pos.Z);
		MaxZ = FMath::Max(MaxZ, Point.Pos.Z);
	}
	TestTrue(*FString::Printf(TEXT("full flatten leaves no relief (%.3f cm)"), MaxZ - MinZ), (MaxZ - MinZ) < 1.e-2);

	// Inflate moves along the normal, and reversing it comes back.
	FSurfPatch Flat2 = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3, FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	const FVector Before = Flat2.At(1, 1).Pos;
	SurfPatchOps::Inflate(Flat2, {}, 50.0);
	TestTrue(TEXT("inflate lifts along the normal"),
		FMath::IsNearlyEqual(Flat2.At(1, 1).Pos.Z, Before.Z + 50.0, 1.e-3));

	SurfPatchOps::Inflate(Flat2, {}, -50.0);
	TestTrue(TEXT("and reverses exactly"), Flat2.At(1, 1).Pos.Equals(Before, 1.e-3));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBevelTest, "SurfacingPro.Math.Patch.BevelLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBevelTest::RunTest(const FString& Parameters)
{
	// Bevel on a grid means rounding the crease along one row or column: the
	// line is replaced by several, spread either side of where it was.
	auto MakeCreased = []()
	{
		FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
			ESurfPatchType::Sheet, 5, 3,
			FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

		// Fold the middle column up: a hard crease to round off.
		for (int32 Y = 0; Y < Patch.Height; ++Y)
		{
			Patch.At(2, Y).Pos.Z = 150.0;
		}
		return Patch;
	};

	FSurfPatch Patch = MakeCreased();
	const int32 WidthBefore = Patch.Width;
	const double CreaseZ = Patch.At(2, 0).Pos.Z;

	TestTrue(TEXT("a column bevels"), SurfPatchOps::BevelLine(Patch, /*bColumn*/ true, 2, 60.0, 2, 1.0));

	TestEqual(TEXT("one line becomes four"), Patch.Width, WidthBefore + 3);
	TestEqual(TEXT("the other axis is untouched"), Patch.Height, 3);
	TestTrue(TEXT("the patch is still valid"), Patch.HasValidDimensions());
	TestEqual(TEXT("and the points match the dimensions"), Patch.Points.Num(), Patch.Width * Patch.Height);

	// The crease is gone: no single column still stands at the full height
	// while its neighbours sit flat.
	double Highest = -TNumericLimits<double>::Max();
	for (const FSurfControlPoint& Point : Patch.Points)
	{
		Highest = FMath::Max(Highest, Point.Pos.Z);
	}
	TestTrue(TEXT("the bevel stays within the crease it rounded"), Highest <= CreaseZ + 1.e-3);

	// The ends of the patch are where they were - a bevel is local.
	FSurfPatch Reference = MakeCreased();
	TestTrue(TEXT("the far edge does not move"),
		Patch.At(Patch.Width - 1, 0).Pos.Equals(Reference.At(Reference.Width - 1, 0).Pos, 1.e-3));
	TestTrue(TEXT("nor the near one"),
		Patch.At(0, 0).Pos.Equals(Reference.At(0, 0).Pos, 1.e-3));

	// Rows work the same way, on the other axis.
	FSurfPatch Rows = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 5, FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0));

	TestTrue(TEXT("a row bevels"), SurfPatchOps::BevelLine(Rows, /*bColumn*/ false, 2, 40.0, 1, 1.0));
	TestEqual(TEXT("the row axis grows"), Rows.Height, 6);
	TestEqual(TEXT("and the column axis does not"), Rows.Width, 3);
	TestTrue(TEXT("still valid"), Rows.HasValidDimensions());

	// A border line has no crease to round, and is refused rather than
	// producing something arbitrary.
	FSurfPatch Border = MakeCreased();
	TestFalse(TEXT("the first line cannot bevel"), SurfPatchOps::BevelLine(Border, true, 0, 50.0, 2, 1.0));
	TestFalse(TEXT("nor the last"), SurfPatchOps::BevelLine(Border, true, Border.Width - 1, 50.0, 2, 1.0));

	// Growing past a curve's maximum is refused, not truncated.
	FSurfPatch Curve = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 15, 3, FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	TestFalse(TEXT("a full curve refuses rather than truncating"),
		SurfPatchOps::BevelLine(Curve, true, 7, 40.0, 4, 1.0));

	// Deterministic, so it can be previewed like the other operators.
	FSurfPatch A = MakeCreased();
	FSurfPatch B = MakeCreased();
	SurfPatchOps::BevelLine(A, true, 2, 60.0, 2, 0.5);
	SurfPatchOps::BevelLine(B, true, 2, 60.0, 2, 0.5);

	TestEqual(TEXT("bevel is deterministic in size"), A.Points.Num(), B.Points.Num());
	for (int32 I = 0; I < A.Points.Num(); ++I)
	{
		if (!A.Points[I].Pos.Equals(B.Points[I].Pos, 1.e-6))
		{
			AddError(TEXT("bevel is not deterministic, so it cannot be previewed"));
			break;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfExtrudeTest, "SurfacingPro.Math.Patch.Extrude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfExtrudeTest::RunTest(const FString& Parameters)
{
	// Extrude is how a patch grows: continue past an edge at the spacing the
	// last two lines already had, so repeating it walks a surface outwards
	// instead of piling lines onto the border.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 4, 3,
		FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	const double SpacingBefore = Patch.At(3, 0).Pos.X - Patch.At(2, 0).Pos.X;
	const FVector FarEdgeBefore = Patch.At(3, 0).Pos;

	TestTrue(TEXT("a column extrudes"), SurfPatchPrimitives::ExtrudeEdge(Patch, true, true, 1));

	TestEqual(TEXT("one line is added"), Patch.Width, 5);
	TestEqual(TEXT("the other axis is untouched"), Patch.Height, 3);
	TestTrue(TEXT("the patch is still valid"), Patch.HasValidDimensions());

	// The new line continues the spacing rather than sitting on the old edge.
	TestTrue(TEXT("the new line continues the spacing"),
		FMath::IsNearlyEqual(Patch.At(4, 0).Pos.X - Patch.At(3, 0).Pos.X, SpacingBefore, 1.e-3));
	TestTrue(TEXT("the old edge did not move"), Patch.At(3, 0).Pos.Equals(FarEdgeBefore, 1.e-4));

	// The other side grows in the other direction, and everything shifts up.
	const FVector NearEdgeBefore = Patch.At(0, 0).Pos;
	TestTrue(TEXT("the near side extrudes too"), SurfPatchPrimitives::ExtrudeEdge(Patch, true, false, 1));

	TestEqual(TEXT("and adds a line"), Patch.Width, 6);
	TestTrue(TEXT("the old near edge is now one in"), Patch.At(1, 0).Pos.Equals(NearEdgeBefore, 1.e-4));
	TestTrue(TEXT("and the new line is beyond it"), Patch.At(0, 0).Pos.X < NearEdgeBefore.X - 1.0);

	// Rows extrude on the other axis.
	FSurfPatch Rows = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 4, FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 300.0, 0.0));

	TestTrue(TEXT("a row extrudes"), SurfPatchPrimitives::ExtrudeEdge(Rows, false, true, 1));
	TestEqual(TEXT("on the row axis"), Rows.Height, 5);
	TestEqual(TEXT("leaving the columns alone"), Rows.Width, 3);

	// A curve has to stay odd, so it grows two at a time.
	FSurfPatch Curve = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 3, 3, FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	TestTrue(TEXT("a curve extrudes"), SurfPatchPrimitives::ExtrudeEdge(Curve, true, true, 1));
	TestEqual(TEXT("by two, to stay odd"), Curve.Width, 5);
	TestTrue(TEXT("and is still a legal curve"), Curve.HasValidDimensions());

	// Refused rather than truncated once there is no room.
	FSurfPatch Full = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, FSurfPatch::MaxSheetDim, 3, FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	TestFalse(TEXT("a full patch refuses to grow"), SurfPatchPrimitives::ExtrudeEdge(Full, true, true, 1));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSplitTest, "SurfacingPro.Math.Patch.Split",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSplitTest::RunTest(const FString& Parameters)
{
	// Both halves must keep the line they were cut on. Two patches that merely
	// end in the same place drift apart the first time either is edited, and
	// the seam opens.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 5, 3,
		FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	FSurfPatch A, B;
	TestTrue(TEXT("a sheet splits"), SurfPatchPrimitives::Split(Patch, true, 2, A, B));

	TestEqual(TEXT("the near half keeps the cut line"), A.Width, 3);
	TestEqual(TEXT("and so does the far half"), B.Width, 3);
	TestEqual(TEXT("the other axis is untouched"), A.Height, 3);
	TestTrue(TEXT("both halves are valid"), A.HasValidDimensions() && B.HasValidDimensions());
	TestTrue(TEXT("and have their own identities"), A.Id != B.Id && A.Id != Patch.Id);

	// The shared line is the same geometry on both sides.
	for (int32 Y = 0; Y < Patch.Height; ++Y)
	{
		TestTrue(TEXT("the seam is shared exactly"),
			A.At(A.Width - 1, Y).Pos.Equals(B.At(0, Y).Pos, 1.e-6));
		TestTrue(TEXT("and matches where the cut was"),
			A.At(A.Width - 1, Y).Pos.Equals(Patch.At(2, Y).Pos, 1.e-6));
	}

	// A border cut would leave one half empty.
	FSurfPatch X, Y2;
	TestFalse(TEXT("the first line cannot be cut"), SurfPatchPrimitives::Split(Patch, true, 0, X, Y2));
	TestFalse(TEXT("nor the last"), SurfPatchPrimitives::Split(Patch, true, Patch.Width - 1, X, Y2));

	// A curve may only be cut where both halves stay odd.
	FSurfPatch Curve = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 5, 3, FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	TestTrue(TEXT("a curve splits at its middle"), SurfPatchPrimitives::Split(Curve, true, 2, A, B));
	TestTrue(TEXT("leaving two legal curves"), A.HasValidDimensions() && B.HasValidDimensions());
	TestFalse(TEXT("but not where a half would come out even"),
		SurfPatchPrimitives::Split(Curve, true, 1, A, B));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfAppendPreparedTest, "SurfacingPro.Group.AppendPrepared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfAppendPreparedTest::RunTest(const FString& Parameters)
{
	// A mesh handed in half-prepared used to take the editor down: the
	// setup guard was all-or-nothing, so a caller who had already enabled
	// attributes got no colour overlay and the first append wrote through a
	// null pointer. Both shapes must come out with everything (S85b).
	USurfPrefab* Prefab = NewObject<USurfPrefab>(GetTransientPackage());
	if (!Prefab)
	{
		AddError(TEXT("a prefab was expected"));
		return false;
	}
	Prefab->AddPatch(SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 3,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0)));

	FSurfMaterialSet Materials;
	Materials.Reset(nullptr);

	// Handed in bare, as the group does.
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		ASurfGroup::AppendPrefabToMesh(Mesh, *Prefab, FTransform::Identity, Materials, 0.0, true, nullptr);
		TestTrue(TEXT("a bare mesh comes back with triangles"), Mesh.TriangleCount() > 0);
		TestTrue(TEXT("and with every overlay"), Mesh.HasAttributes()
			&& Mesh.Attributes()->PrimaryColors() != nullptr
			&& Mesh.Attributes()->HasMaterialID());
	}

	// Handed in with the attributes already on, as the thumbnail did.
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		Mesh.EnableAttributes();
		ASurfGroup::AppendPrefabToMesh(Mesh, *Prefab, FTransform::Identity, Materials, 0.0, true, nullptr);
		TestTrue(TEXT("a half-prepared mesh comes back with triangles too"), Mesh.TriangleCount() > 0);
		TestTrue(TEXT("and the colour overlay it was missing"), Mesh.Attributes()->PrimaryColors() != nullptr);
		TestTrue(TEXT("and the material IDs"), Mesh.Attributes()->HasMaterialID());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSolidifyTest, "SurfacingPro.Math.Patch.Solidify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSolidifyTest::RunTest(const FString& Parameters)
{
	// A surface given thickness, in the only thing this tool has: more
	// surfaces. Front, back, and a skirt at each border.
	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 4, 3,
		FVector::ZeroVector, FVector(300.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));

	TArray<FSurfPatch> Made;
	TestTrue(TEXT("a sheet solidifies"), SurfPatchPrimitives::Solidify(Patch, 25.0, Made));
	TestEqual(TEXT("into a front, a back and four skirts"), Made.Num(), 6);

	for (const FSurfPatch& One : Made)
	{
		TestTrue(TEXT("every piece is valid"), One.HasValidDimensions());
		TestTrue(TEXT("and has its own identity"), One.Id.IsValid());
	}

	// The back sits a thickness behind the front, and faces the other way so
	// the solid is closed rather than inside out.
	const FSurfPatch& Front = Made[0];
	const FSurfPatch& Back = Made[1];

	TestEqual(TEXT("the back is the same grid"), Back.Width, Front.Width);
	TestTrue(TEXT("offset by the thickness"),
		FMath::IsNearlyEqual(FVector::Dist(Front.At(0, 0).Pos, Back.At(0, 0).Pos), 25.0, 1.e-3));
	TestTrue(TEXT("and wound the other way"), Back.bInvert != Front.bInvert);

	// Each skirt joins a front border to the matching back border, so nothing
	// is left open.
	for (int32 I = 2; I < Made.Num(); ++I)
	{
		TestEqual(TEXT("a skirt is two lines wide"), Made[I].Width, 2);
		TestTrue(TEXT("and spans the thickness"),
			FMath::IsNearlyEqual(FVector::Dist(Made[I].At(0, 0).Pos, Made[I].At(1, 0).Pos), 25.0, 1.e-2));
	}

	// Every id must be distinct, or removing one piece removes another.
	TSet<FGuid> Ids;
	for (const FSurfPatch& One : Made)
	{
		Ids.Add(One.Id);
	}
	TestEqual(TEXT("all six are distinct"), Ids.Num(), 6);

	TestFalse(TEXT("zero thickness is refused"), SurfPatchPrimitives::Solidify(Patch, 0.0, Made));

	// The options: which side the shell grows, whether it has a rim, and
	// an even thickness at folds.
	const FVector Was = Patch.At(1, 1).Pos;
	SurfPatchPrimitives::FSurfSolidifyOptions InFront;
	InFront.Side = 1;
	TestTrue(TEXT("grows in front"), SurfPatchPrimitives::Solidify(Patch, 25.0, Made, InFront));
	TestTrue(TEXT("the front moved off the surface by the thickness"), FMath::IsNearlyEqual(FVector::Dist(Made[0].At(1, 1).Pos, Was), 25.0, 1.e-3));
	TestTrue(TEXT("and the back stayed on it"), Made[1].At(1, 1).Pos.Equals(Was, 1.e-3));

	SurfPatchPrimitives::FSurfSolidifyOptions Centred;
	Centred.Side = 0;
	TestTrue(TEXT("grows half each way"), SurfPatchPrimitives::Solidify(Patch, 25.0, Made, Centred));
	TestTrue(TEXT("front half a thickness off"), FMath::IsNearlyEqual(FVector::Dist(Made[0].At(1, 1).Pos, Was), 12.5, 1.e-3));
	TestTrue(TEXT("back half a thickness off the other way"), FMath::IsNearlyEqual(FVector::Dist(Made[1].At(1, 1).Pos, Was), 12.5, 1.e-3));
	TestTrue(TEXT("a thickness apart"), FMath::IsNearlyEqual(FVector::Dist(Made[0].At(1, 1).Pos, Made[1].At(1, 1).Pos), 25.0, 1.e-3));

	SurfPatchPrimitives::FSurfSolidifyOptions NoRim;
	NoRim.bRim = false;
	TestTrue(TEXT("without a rim"), SurfPatchPrimitives::Solidify(Patch, 25.0, Made, NoRim));
	TestEqual(TEXT("front and back only"), Made.Num(), 2);

	// A folded sheet: a V. Pushing along the averaged normal at the fold
	// leaves the two halves less than a thickness apart; even thickness
	// pushes further there.
	FSurfPatch Fold = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Sheet, 3, 2,
		FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	Fold.At(0, 0).Pos.Z = 100.0;
	Fold.At(0, 1).Pos.Z = 100.0;
	Fold.At(2, 0).Pos.Z = 100.0;
	Fold.At(2, 1).Pos.Z = 100.0;
	SurfPatchPrimitives::FSurfSolidifyOptions Even;
	Even.bEven = true;
	TArray<FSurfPatch> Plain;
	TArray<FSurfPatch> Kept;
	TestTrue(TEXT("a fold solidifies"), SurfPatchPrimitives::Solidify(Fold, 20.0, Plain));
	TestTrue(TEXT("and with an even thickness"), SurfPatchPrimitives::Solidify(Fold, 20.0, Kept, Even));
	const double PlainPush = FVector::Dist(Plain[1].At(1, 0).Pos, Fold.At(1, 0).Pos);
	const double EvenPush = FVector::Dist(Kept[1].At(1, 0).Pos, Fold.At(1, 0).Pos);
	TestTrue(TEXT("the fold's point is pushed further to keep the thickness"), EvenPush > PlainPush + 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfProjectionAxesTest, "SurfacingPro.Math.Projection.BaseAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfProjectionAxesTest::RunTest(const FString& Parameters)
{
	// Convention pin. If these change, every existing level's surfacing moves.
	struct FCase { FVector Normal; FVector U; FVector V; const TCHAR* Name; };

	const FCase Cases[] =
	{
		{ FVector( 0,  0,  1), FVector(1,  0, 0), FVector(0, -1,  0), TEXT("up") },
		{ FVector( 0,  0, -1), FVector(1,  0, 0), FVector(0, -1,  0), TEXT("down") },
		{ FVector( 1,  0,  0), FVector(0,  1, 0), FVector(0,  0, -1), TEXT("+X") },
		{ FVector(-1,  0,  0), FVector(0,  1, 0), FVector(0,  0, -1), TEXT("-X") },
		{ FVector( 0,  1,  0), FVector(1,  0, 0), FVector(0,  0, -1), TEXT("+Y") },
		{ FVector( 0, -1,  0), FVector(1,  0, 0), FVector(0,  0, -1), TEXT("-Y") }
	};

	for (const FCase& Case : Cases)
	{
		FVector U, V;
		SurfProjection::BaseAxes(Case.Normal, U, V);
		TestTrue(*FString::Printf(TEXT("%s base U"), Case.Name), SurfTestUtil::VectorNearlyEqual(U, Case.U));
		TestTrue(*FString::Printf(TEXT("%s base V"), Case.Name), SurfTestUtil::VectorNearlyEqual(V, Case.V));
		TestTrue(*FString::Printf(TEXT("%s axes are orthogonal"), Case.Name),
			SurfTestUtil::NearlyEqual(FVector::DotProduct(U, V), 0.0));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfProjectionShiftScaleTest, "SurfacingPro.Math.Projection.ShiftAndScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfProjectionShiftScaleTest::RunTest(const FString& Parameters)
{
	FSurfFaceUV UV;
	SurfProjection::Align(UV, FVector(0.0, 0.0, 1.0));
	UV.Scale = FVector2D(1.0, 1.0); // one metre per tile

	// 100 cm along U is exactly one tile at 1 m per tile.
	TestTrue(TEXT("one metre is one tile"),
		SurfTestUtil::NearlyEqual(SurfProjection::ComputeUV(UV, FVector(100.0, 0.0, 0.0)).X, 1.0));

	// Two metres per tile halves the repeat.
	UV.Scale = FVector2D(2.0, 2.0);
	TestTrue(TEXT("two metres per tile halves the repeat"),
		SurfTestUtil::NearlyEqual(SurfProjection::ComputeUV(UV, FVector(100.0, 0.0, 0.0)).X, 0.5));

	// The shift rule: +1 shift moves the texture one centimetre, at any scale,
	// never by one texture width.
	for (const double Scale : { 0.5, 1.0, 4.0 })
	{
		FSurfFaceUV Probe;
		SurfProjection::Align(Probe, FVector(0.0, 0.0, 1.0));
		Probe.Scale = FVector2D(Scale, Scale);

		const double Before = SurfProjection::ComputeUV(Probe, FVector::ZeroVector).X;
		Probe.Shift.X += 100.0; // one metre
		const double After = SurfProjection::ComputeUV(Probe, FVector::ZeroVector).X;

		// One metre of shift is exactly one metre in world terms, which is
		// 1/Scale tiles - never one tile regardless of scale.
		TestTrue(TEXT("shift is in world units, independent of scale"),
			SurfTestUtil::NearlyEqual(After - Before, 1.0 / Scale));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfProjectionFitTest, "SurfacingPro.Math.Projection.Fit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfProjectionFitTest::RunTest(const FString& Parameters)
{
	// A 300 x 200 cm floor quad.
	const TArray<FVector> Quad =
	{
		FVector(0.0,   0.0, 0.0),
		FVector(300.0, 0.0, 0.0),
		FVector(300.0, 200.0, 0.0),
		FVector(0.0,   200.0, 0.0)
	};

	for (const int32 Repeat : { 1, 2, 5 })
	{
		FSurfFaceUV UV;
		SurfProjection::Align(UV, FVector(0.0, 0.0, 1.0));
		SurfProjection::Fit(UV, Quad, Repeat, Repeat);

		double MinU = TNumericLimits<double>::Max();
		double MaxU = TNumericLimits<double>::Lowest();
		double MinV = TNumericLimits<double>::Max();
		double MaxV = TNumericLimits<double>::Lowest();

		for (const FVector& P : Quad)
		{
			const FVector2D T = SurfProjection::ComputeUV(UV, P);
			MinU = FMath::Min(MinU, T.X);
			MaxU = FMath::Max(MaxU, T.X);
			MinV = FMath::Min(MinV, T.Y);
			MaxV = FMath::Max(MaxV, T.Y);
		}

		TestTrue(TEXT("fit starts at zero on U"), SurfTestUtil::NearlyEqual(MinU, 0.0));
		TestTrue(TEXT("fit starts at zero on V"), SurfTestUtil::NearlyEqual(MinV, 0.0));
		TestTrue(TEXT("fit spans the requested repeats on U"), SurfTestUtil::NearlyEqual(MaxU, Repeat));
		TestTrue(TEXT("fit spans the requested repeats on V"), SurfTestUtil::NearlyEqual(MaxV, Repeat));
	}

	// Stretch is Fit at one repeat, aspect discarded: a 300x200 face gets
	// different scales per axis.
	FSurfFaceUV Stretched;
	SurfProjection::Align(Stretched, FVector(0.0, 0.0, 1.0));
	SurfProjection::Stretch(Stretched, Quad);
	TestTrue(TEXT("stretch scales the axes independently"),
		!SurfTestUtil::NearlyEqual(Stretched.Scale.X, Stretched.Scale.Y));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfProjectionRotationTest, "SurfacingPro.Math.Projection.Rotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfProjectionRotationTest::RunTest(const FString& Parameters)
{
	const FVector Normal(0.0, 0.0, 1.0);

	for (const double Degrees : { 0.0, 37.0, 90.0, -45.0, 179.0 })
	{
		FSurfFaceUV UV;
		SurfProjection::SetRotationDegrees(UV, Normal, Degrees);

		TestTrue(TEXT("rotation round-trips"),
			SurfTestUtil::NearlyEqual(SurfProjection::GetRotationDegrees(UV, Normal), Degrees, 1.e-2));

		TestTrue(TEXT("rotated axes stay unit length"), SurfTestUtil::NearlyEqual(UV.UAxis.Length(), 1.0));
		TestTrue(TEXT("rotated axes stay orthogonal"),
			SurfTestUtil::NearlyEqual(FVector::DotProduct(UV.UAxis, UV.VAxis), 0.0));
		TestTrue(TEXT("rotated axes stay in the plane"),
			SurfTestUtil::NearlyEqual(FVector::DotProduct(UV.UAxis, Normal), 0.0));
	}

	// Align resets the rotation to zero.
	FSurfFaceUV UV;
	SurfProjection::SetRotationDegrees(UV, Normal, 62.0);
	SurfProjection::Align(UV, Normal);
	TestTrue(TEXT("align resets rotation"), SurfTestUtil::NearlyEqual(SurfProjection::GetRotationDegrees(UV, Normal), 0.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTextureLockTest, "SurfacingPro.Math.Projection.TextureLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTextureLockTest::RunTest(const FString& Parameters)
{
	// Texture lock means the texture stays where it is in the world while the
	// brush moves under it. Carrying the projection through the transform must
	// therefore leave every point's UV unchanged.
	FSurfFaceUV UV;
	SurfProjection::Align(UV, FVector(0.0, 0.0, 1.0));
	UV.Scale = FVector2D(2.0, 2.0);
	UV.Shift = FVector2D(13.0, -7.0);

	const TArray<FVector> Points =
	{
		FVector(0.0, 0.0, 0.0),
		FVector(150.0, 40.0, 0.0),
		FVector(-90.0, 220.0, 0.0)
	};

	TArray<FVector2D> Expected;
	for (const FVector& P : Points)
	{
		Expected.Add(SurfProjection::ComputeUV(UV, P));
	}

	const FTransform Move(FQuat(FVector(0.0, 0.0, 1.0), FMath::DegreesToRadians(31.0)), FVector(250.0, -80.0, 15.0), FVector::OneVector);

	FSurfFaceUV Locked = UV;
	SurfProjection::ApplyTransform(Locked, Move);

	for (int32 I = 0; I < Points.Num(); ++I)
	{
		const FVector2D After = SurfProjection::ComputeUV(Locked, Move.TransformPosition(Points[I]));
		TestTrue(TEXT("texture lock holds under rotation and translation"),
			SurfTestUtil::NearlyEqual(After.X, Expected[I].X, 1.e-2) &&
			SurfTestUtil::NearlyEqual(After.Y, Expected[I].Y, 1.e-2));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfCopySurfaceTest, "SurfacingPro.Math.Projection.CopySurfaceDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfCopySurfaceTest::RunTest(const FString& Parameters)
{
	FSurfFace Source(FPlane(FVector(0.0, 0.0, 1.0), 0.0));
	SurfProjection::SetRotationDegrees(Source.UV, FVector(0.0, 0.0, 1.0), 45.0);
	Source.UV.Scale = FVector2D(3.0, 4.0);
	Source.UV.Shift = FVector2D(11.0, 22.0);
	Source.SmoothingGroup = 7;

	FSurfFace Target(FPlane(FVector(1.0, 0.0, 0.0), 100.0));
	SurfProjection::CopySurfaceDefinition(Source, Target);

	TestTrue(TEXT("copy carries the U axis"), SurfTestUtil::VectorNearlyEqual(Target.UV.UAxis, Source.UV.UAxis));
	TestTrue(TEXT("copy carries the scale"), SurfTestUtil::NearlyEqual(Target.UV.Scale.X, 3.0));
	TestTrue(TEXT("copy carries the shift"), SurfTestUtil::NearlyEqual(Target.UV.Shift.Y, 22.0));
	TestEqual(TEXT("copy carries the smoothing group"), Target.SmoothingGroup, 7);

	// The plane is the target's own geometry and must survive the copy.
	TestTrue(TEXT("copy does not touch the plane"), SurfTestUtil::NearlyEqual(Target.Plane.W, 100.0));

	return true;
}

// ---------------------------------------------------------------------------
// Settings and conventions
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGridLadderTest, "SurfacingPro.Settings.GridLadder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGridLadderTest::RunTest(const FString& Parameters)
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();

	TestEqual(TEXT("ladder has eight steps"), Settings->GridLadderCm.Num(), 8);
	TestTrue(TEXT("default grid is one metre"), SurfTestUtil::NearlyEqual(Settings->GetGridStepCm(Settings->DefaultGridIndex), 100.0));

	// Metric only. Any legacy unit would show up as a step that is not a whole
	// number of centimetres, or as 2.54 and its multiples.
	for (const double Step : Settings->GridLadderCm)
	{
		TestTrue(TEXT("every ladder step is a whole centimetre"), SurfTestUtil::NearlyEqual(Step, FMath::RoundToDouble(Step)));
		TestTrue(TEXT("no legacy unit in the ladder"), !SurfTestUtil::NearlyEqual(FMath::Fmod(Step, 2.54), 0.0) || SurfTestUtil::NearlyEqual(Step, 0.0));
	}

	// Above 1 cm, every step is a multiple of 5, so any two steps share a lattice.
	for (const double Step : Settings->GridLadderCm)
	{
		if (Step > 1.0)
		{
			TestTrue(TEXT("steps above 1 cm share a 5 cm lattice"), SurfTestUtil::NearlyEqual(FMath::Fmod(Step, 5.0), 0.0));
		}
	}

	// Index stepping clamps rather than wrapping.
	TestEqual(TEXT("stepping below zero clamps"), Settings->StepGridIndex(0, -1), 0);
	TestEqual(TEXT("stepping past the end clamps"), Settings->StepGridIndex(Settings->GridLadderCm.Num() - 1, 1), Settings->GridLadderCm.Num() - 1);

	// Snapping.
	TestTrue(TEXT("snap rounds to the nearest step"), SurfTestUtil::NearlyEqual(USurfSettings::SnapToStep(137.0, 25.0), 125.0));
	TestTrue(TEXT("snap handles negatives"), SurfTestUtil::NearlyEqual(USurfSettings::SnapToStep(-137.0, 25.0), -125.0));
	TestTrue(TEXT("a zero step is a no-op"), SurfTestUtil::NearlyEqual(USurfSettings::SnapToStep(137.0, 0.0), 137.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfFlagsDistinctTest, "SurfacingPro.Settings.FlagsAreDistinctBits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfFlagsDistinctTest::RunTest(const FString& Parameters)
{
	// D16.4. Asking for one thing must never silently do four. This test
	// makes that impossible here.
	const uint32 Flags[] =
	{
		static_cast<uint32>(ESurfPrimitiveFlags::Detail),
		static_cast<uint32>(ESurfPrimitiveFlags::NonColliding),
		static_cast<uint32>(ESurfPrimitiveFlags::NoShadow),
		static_cast<uint32>(ESurfPrimitiveFlags::NoDraw)
	};

	const int32 Count = UE_ARRAY_COUNT(Flags);

	for (int32 I = 0; I < Count; ++I)
	{
		TestTrue(TEXT("each flag is a single bit"), Flags[I] != 0 && (Flags[I] & (Flags[I] - 1)) == 0);

		for (int32 J = I + 1; J < Count; ++J)
		{
			TestTrue(TEXT("flags do not overlap"), (Flags[I] & Flags[J]) == 0);
		}
	}

	// No flag equals the OR of any subset of the others.
	for (int32 I = 0; I < Count; ++I)
	{
		uint32 OthersOr = 0;
		for (int32 J = 0; J < Count; ++J)
		{
			if (I != J)
			{
				OthersOr |= Flags[J];
			}
		}
		TestTrue(TEXT("no flag is the OR of the others"), (Flags[I] & OthersOr) == 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Performance floor
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPerfBaselineTest, "SurfacingPro.Perf.MathBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPerfBaselineTest::RunTest(const FString& Parameters)
{
	// Measured and logged, never asserted - except for a ceiling loose enough
	// that only a catastrophic regression trips it, so the rebuild cost
	// cannot grow unnoticed.
	constexpr int32 BrushCount = 1000;

	TArray<FSurfBrush> Brushes;
	Brushes.Reserve(BrushCount);
	for (int32 I = 0; I < BrushCount; ++I)
	{
		const double X = (I % 50) * 300.0;
		const double Y = (I / 50) * 300.0;
		Brushes.Add(SurfBrushMath::MakeBox(FBox(FVector(X, Y, 0.0), FVector(X + 200.0, Y + 200.0, 300.0))));
	}

	FSurfHull Hull;
	const double HullStart = FPlatformTime::Seconds();
	int32 Derived = 0;
	for (const FSurfBrush& Brush : Brushes)
	{
		if (SurfBrushMath::DeriveHull(Brush, Hull))
		{
			++Derived;
		}
	}
	const double HullMs = (FPlatformTime::Seconds() - HullStart) * 1000.0;

	TestEqual(TEXT("every brush derives"), Derived, BrushCount);

	FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
		ESurfPatchType::Curve, 5, 5,
		FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0));
	Patch.SubdivU = 8;
	Patch.SubdivV = 8;

	FSurfTessellation Tess;
	const double PatchStart = FPlatformTime::Seconds();
	for (int32 I = 0; I < BrushCount; ++I)
	{
		SurfPatchMath::Tessellate(Patch, Tess);
	}
	const double PatchMs = (FPlatformTime::Seconds() - PatchStart) * 1000.0;

	SURF_METRIC_N("math.hull_derive_ms", HullMs, "ms", BrushCount);
	SURF_METRIC_N("math.patch_tessellate_ms", PatchMs, "ms", BrushCount);
	SURF_METRIC("math.patch_tris", Tess.NumTriangles(), "tris");

	// S2's budget is a single-primitive edit under 16 ms at 1000 primitives.
	// The maths alone must sit far under that or nothing downstream can hold.
	TestTrue(TEXT("hull derivation stays under 1 ms per brush"), (HullMs / BrushCount) < 1.0);
	TestTrue(TEXT("patch tessellation stays under 1 ms per patch"), (PatchMs / BrushCount) < 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfJoinTest, "SurfacingPro.Math.JoinPatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfJoinTest::RunTest(const FString& Parameters)
{
	// Two sheets built side by side, sharing a column exactly. Either may be
	// turned or reversed before joining, so the caller never has to have made
	// them in a particular order.
	auto MakeSheet = [](int32 Width, int32 Height, const FVector& Origin, double StepCm)
	{
		FSurfPatch Patch;
		Patch.Type = ESurfPatchType::Sheet;
		Patch.EnsureId();
		Patch.Width = Width;
		Patch.Height = Height;
		Patch.Points.SetNum(Width * Height);

		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Patch.At(X, Y).Pos = Origin + FVector(X * StepCm, Y * StepCm, 0.0);
			}
		}

		return Patch;
	};

	const double Step = 100.0;

	const FSurfPatch Left = MakeSheet(3, 4, FVector::ZeroVector, Step);
	const FSurfPatch Right = MakeSheet(3, 4, FVector(2.0 * Step, 0.0, 0.0), Step);

	FSurfPatch Joined;
	TestTrue(TEXT("two sheets sharing a column join"),
		SurfPatchPrimitives::Join(Joined, Left, Right, 0.1));

	// The shared line is kept once: 3 + 3 - 1.
	TestEqual(TEXT("the shared column is kept once"), Joined.Width, 5);
	TestEqual(TEXT("the height is unchanged"), Joined.Height, 4);

	// And the points are where they were, in order.
	double Worst = 0.0;
	for (int32 Y = 0; Y < Joined.Height; ++Y)
	{
		for (int32 X = 0; X < Joined.Width; ++X)
		{
			const FVector Expected(X * Step, Y * Step, 0.0);
			Worst = FMath::Max(Worst, FVector::Dist(Joined.At(X, Y).Pos, Expected));
		}
	}

	TestTrue(FString::Printf(TEXT("the joined grid is continuous (worst %.4f cm)"), Worst), Worst < 1.e-3);

	// The same pair, given the other way round and with one of them turned:
	// the join has to find the orientation itself.
	FSurfPatch Turned = Right;
	Turned.Width = Right.Height;
	Turned.Height = Right.Width;
	Turned.Points.SetNum(Turned.Width * Turned.Height);
	for (int32 Y = 0; Y < Right.Height; ++Y)
	{
		for (int32 X = 0; X < Right.Width; ++X)
		{
			Turned.At(Y, X) = Right.At(X, Y);
		}
	}

	FSurfPatch Either;
	TestTrue(TEXT("a turned patch still joins"),
		SurfPatchPrimitives::Join(Either, Turned, Left, 0.1));
	TestEqual(TEXT("and gives the same size"), Either.Width * Either.Height, 20);

	// Patches that do not touch do not join.
	const FSurfPatch Away = MakeSheet(3, 4, FVector(0.0, 0.0, 5000.0), Step);

	FSurfPatch Nothing;
	TestFalse(TEXT("patches that do not meet are refused"),
		SurfPatchPrimitives::Join(Nothing, Left, Away, 0.1));

	// Nor do patches whose shared edge carries different numbers of points.
	const FSurfPatch Mismatched = MakeSheet(3, 5, FVector(2.0 * Step, 0.0, 0.0), Step);

	TestFalse(TEXT("a seam where four points meet five is refused"),
		SurfPatchPrimitives::Join(Nothing, Left, Mismatched, 0.1));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfLatticeTest, "SurfacingPro.Math.Lattice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfLatticeTest::RunTest(const FString& Parameters)
{
	FSurfPatch Patch;
	Patch.Type = ESurfPatchType::Sheet;
	Patch.Width = 3;
	Patch.Height = 3;
	for (int32 Y = 0; Y < 3; ++Y)
	{
		for (int32 X = 0; X < 3; ++X)
		{
			FSurfControlPoint Point;
			Point.Pos = FVector(X * 100.0, Y * 100.0, 0.0);
			Patch.Points.Add(Point);
		}
	}
	TestTrue(TEXT("binds"), SurfLattice::Bind(Patch, 3, 3, 2));
	TestTrue(TEXT("bound"), SurfLattice::IsBound(Patch));
	TestEqual(TEXT("cage points"), Patch.Lattice.Cage.Num(), 18);
	for (int32 I = 0; I < Patch.Points.Num(); ++I)
	{
		TestTrue(TEXT("an untouched cage is the patch"),
			SurfLattice::Evaluate(Patch.Lattice, Patch.Lattice.Cage, Patch.Lattice.Params[I]).Equals(Patch.Points[I].Pos, 0.01));
	}

	const TArray<FVector> CageFrom = Patch.Lattice.Cage;
	TArray<FVector> PointsFrom;
	for (const FSurfControlPoint& Point : Patch.Points)
	{
		PointsFrom.Add(Point.Pos);
	}

	// The whole cage up: the whole patch up.
	for (FVector& Point : Patch.Lattice.Cage)
	{
		Point.Z += 50.0;
	}
	SurfLattice::Deform(Patch, CageFrom, PointsFrom);
	TestTrue(TEXT("a lifted cage lifts the patch"), FMath::IsNearlyEqual(Patch.Points[4].Pos.Z, 50.0, 0.01) && FMath::IsNearlyEqual(Patch.Points[0].Pos.Z, 50.0, 0.01));

	// The middle column of the cage up 100: the middle point rises a
	// quarter of it, the corners not at all.
	Patch.Lattice.Cage = CageFrom;
	for (int32 Z = 0; Z < 2; ++Z)
	{
		Patch.Lattice.Cage[Patch.Lattice.Index(1, 1, Z)].Z += 100.0;
	}
	SurfLattice::Deform(Patch, CageFrom, PointsFrom);
	TestTrue(TEXT("the middle rises by its share"), FMath::IsNearlyEqual(Patch.Points[4].Pos.Z, 25.0, 0.01));
	TestTrue(TEXT("a corner stays"), FMath::IsNearlyEqual(Patch.Points[0].Pos.Z, 0.0, 0.01));

	// A move of the whole patch carries the cage.
	SurfLattice::NoteRest(Patch);
	for (FSurfControlPoint& Point : Patch.Points)
	{
		Point.Pos.X += 30.0;
	}
	TestTrue(TEXT("a whole move is seen"), SurfLattice::FollowOffset(Patch).Equals(FVector(30.0, 0.0, 0.0), 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfHardLinesFollowTest, "SurfacingPro.Math.HardLinesFollow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfHardLinesFollowTest::RunTest(const FString& Parameters)
{
	FSurfPatch Patch;
	Patch.Type = ESurfPatchType::Sheet;
	Patch.Width = 5;
	Patch.Height = 5;
	for (int32 Y = 0; Y < 5; ++Y)
	{
		for (int32 X = 0; X < 5; ++X)
		{
			FSurfControlPoint Point;
			Point.Pos = FVector(X * 100.0, Y * 100.0, 0.0);
			Patch.Points.Add(Point);
		}
	}
	Patch.HardColumns = { 3 };
	Patch.HardRows = { 1 };
	TestTrue(TEXT("a column goes in before the hard one"), SurfPatchPrimitives::InsertColumn(Patch, 1));
	TestTrue(TEXT("the hard column moved along"), Patch.HardColumns.Num() == 1 && Patch.HardColumns[0] == 4);
	TestTrue(TEXT("still the same line"), FMath::IsNearlyEqual(Patch.At(Patch.HardColumns[0], 0).Pos.X, 300.0, 0.01));
	TestTrue(TEXT("a row goes in after the hard one"), SurfPatchPrimitives::InsertRow(Patch, 2));
	TestTrue(TEXT("the hard row stays"), Patch.HardRows.Num() == 1 && Patch.HardRows[0] == 1);
	TestTrue(TEXT("the inserted column comes out"), SurfPatchPrimitives::RemoveColumn(Patch, 2));
	TestTrue(TEXT("the hard column moved back"), Patch.HardColumns.Num() == 1 && Patch.HardColumns[0] == 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
