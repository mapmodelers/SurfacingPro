// Copyright MapModelers. All Rights Reserved.
//
// Clicking, through the mode's own click path, with no viewport.
//
// Eleven sessions shipped verbs that unit tests could not press, and several
// did nothing when the author pressed them. These tests describe a camera in
// numbers, project a control point to a pixel, and hand a click at that pixel
// to exactly the code a real click reaches. If a pick breaks, this breaks
// first.

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/World.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "Toolkits/BaseToolkit.h"

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfPickContext.h"
#include "SurfSettings.h"
#include "SurfGizmo.h"
#include "SurfPreviewMaterial.h"
#include "SurfCamera.h"
#include "EngineUtils.h"
#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"
#include "Materials/Material.h"
#include "SurfPatchSurfacing.h"
#include "SurfProjection.h"
#include "SurfBrushMath.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UnrealWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfInteractionTestUtil
{
	/** A camera that looks straight down at the patch from three extents up. Flat (orthographic) when ViewWidth is set. */
	struct FCamera
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator(-90.0, 0.0, 0.0);
		float FOV = 90.0f;
		double ViewWidth = 0.0;
		FIntPoint Size = FIntPoint(1000, 1000);
		UWorld* World = nullptr;

		/** A flat view straight down, four extents wide. */
		static FCamera Above(ASurfGroup* Group, const FSurfPatch& Patch)
		{
			FCamera Camera = Over(Group, Patch);
			Camera.ViewWidth = (Camera.Location.Z - Camera.Extent(Group, Patch).GetCenter().Z) / 3.0 * 4.0;
			return Camera;
		}

		/** A flat view at forty-five degrees, from the -X side, four extents wide. */
		static FCamera Oblique(ASurfGroup* Group, const FSurfPatch& Patch)
		{
			FCamera Camera = Over(Group, Patch);
			const FBox Bounds = Camera.Extent(Group, Patch);
			const double Extent = FMath::Max(100.0, Bounds.GetExtent().GetMax());
			Camera.Rotation = FRotator(-45.0, 0.0, 0.0);
			Camera.Location = Bounds.GetCenter() - Camera.Rotation.Vector() * (Extent * 3.0);
			Camera.ViewWidth = Extent * 4.0;
			return Camera;
		}

		static FBox Extent(ASurfGroup* Group, const FSurfPatch& Patch)
		{
			FBox Bounds(ForceInit);
			const FTransform ToWorld = Group ? Group->GetActorTransform() : FTransform::Identity;
			for (const FSurfControlPoint& Point : Patch.Points)
			{
				Bounds += ToWorld.TransformPosition(Point.Pos);
			}
			return Bounds;
		}

		static FCamera Over(ASurfGroup* Group, const FSurfPatch& Patch)
		{
			FCamera Camera;
			Camera.World = Group ? Group->GetWorld() : nullptr;

			FBox Bounds(ForceInit);
			const FTransform ToWorld = Group ? Group->GetActorTransform() : FTransform::Identity;
			for (const FSurfControlPoint& Point : Patch.Points)
			{
				Bounds += ToWorld.TransformPosition(Point.Pos);
			}

			const double Extent = FMath::Max(100.0, Bounds.GetExtent().GetMax());
			Camera.Location = Bounds.GetCenter() + FVector(0.0, 0.0, Extent * 3.0);
			return Camera;
		}

		bool Project(const FVector& Point, FVector2D& OutPixel) const
		{
			return ViewWidth > 0.0
				? FSurfPickContext::ProjectSyntheticFlat(Location, Rotation, ViewWidth, Size, Point, OutPixel)
				: FSurfPickContext::ProjectSynthetic(Location, Rotation, FOV, Size, Point, OutPixel);
		}

		FSurfPickContext ClickAt(const FVector2D& Pixel, bool bCtrl = false, bool bShift = false, bool bAlt = false) const
		{
			FSurfPickContext Ctx = ViewWidth > 0.0
				? FSurfPickContext::SyntheticFlat(World, Location, Rotation, ViewWidth, Size, Pixel)
				: FSurfPickContext::Synthetic(World, Location, Rotation, FOV, Size, Pixel);
			Ctx.bCtrl = bCtrl;
			Ctx.bShift = bShift;
			Ctx.bAlt = bAlt;
			return Ctx;
		}

		/** A click on the pixel a world point lands on. */
		FSurfPickContext ClickOn(const FVector& Point, bool bCtrl = false, bool bShift = false, bool bAlt = false) const
		{
			FVector2D Pixel;
			Project(Point, Pixel);
			return ClickAt(Pixel, bCtrl, bShift, bAlt);
		}
	};

	static USurfEdMode* EnterMode()
	{
		// The tests were written against the defaults. The settings on disk
		// are the user's - proportional editing left on with a wide radius,
		// say - and since settings persist (D51) the test editor loads them
		// too; the harness puts the file back after the run. In memory, what
		// the tests assume.
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->bSoftSelection = false;
			Settings->NewPatchSizeCm = 1000.0;
			Settings->NewBrushSizeCm = 200.0;
			Settings->NewCylinderSides = 8;
			Settings->PointSnapRadiusCm = 10.0;
			// Clay would stand in for every material the tests look for (S173a).
			Settings->bMatcap = false;
		}
		// A test starts from an empty stage. The map the harness opens is the
		// user's, and anything of ours left in it - a group he made while
		// testing, or one a previous test made through New Patch - is in the
		// world every later test counts and clicks in. Only what the level
		// saved is swept: a test's own actors are transient and are made
		// after this. Nothing here is ever saved, so it costs him nothing
		// (S76c).
		if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
		{
			TArray<AActor*> Leftovers;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || Actor->HasAnyFlags(RF_Transient))
				{
					continue;
				}
				if (Actor->IsA(ASurfGroup::StaticClass()) || Actor->IsA(ASurfPrefabInstance::StaticClass()))
				{
					Leftovers.Add(Actor);
				}
			}
			for (AActor* Each : Leftovers)
			{
				Each->Destroy();
			}
		}

		GLevelEditorModeTools().ActivateMode(USurfEdMode::EM_SurfacingPro);
		USurfEdMode* Entered = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
		if (Entered)
		{
			Entered->RefreshPaintView();
			// The tests were written for a mode that opened in faces (S175a: it opens in Object now).
			Entered->SetSubMode(ESurfSubMode::Face);
		}
		return Entered;
	}

	/** A fresh 5 x 4 sheet, selected, with the mode in the given sub-mode. */
	static const FSurfPatch* MakeSheet(USurfEdMode* Mode, ASurfGroup*& OutGroup, FGuid& OutId, ESurfSubMode SubMode)
	{
		Mode->ClearSelection();
		Mode->ClearPointSelection();
		Mode->Inspector->SheetWidth = 5;
		Mode->Inspector->SheetHeight = 4;
		Mode->Inspector->NewPatch();

		OutGroup = Mode->GetSelectedPatch().Group.Get();
		OutId = Mode->GetSelectedPatch().PatchId;
		Mode->SetSubMode(SubMode);

		return OutGroup ? OutGroup->FindPatch(OutId) : nullptr;
	}

	static FVector WorldOf(ASurfGroup* Group, const FSurfPatch& Patch, int32 X, int32 Y)
	{
		return Group->GetActorTransform().TransformPosition(Patch.At(X, Y).Pos);
	}
}

using namespace SurfInteractionTestUtil;

// --- The projection agrees with itself -----------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSyntheticCameraTest, "SurfacingPro.Interaction.SyntheticCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSyntheticCameraTest::RunTest(const FString& Parameters)
{
	// The ray through a pixel has to pass through the point that projects to
	// that pixel, or every test below is testing the camera and not the tool.
	FCamera Camera;
	Camera.Location = FVector(0.0, 0.0, 1000.0);
	Camera.Rotation = FRotator(-90.0, 0.0, 0.0);

	const FVector Points[] = {
		FVector(0.0, 0.0, 0.0),
		FVector(300.0, -200.0, 50.0),
		FVector(-450.0, 120.0, -30.0),
	};

	for (const FVector& Point : Points)
	{
		FVector2D Pixel;
		TestTrue(TEXT("a point in front of the camera projects"), Camera.Project(Point, Pixel));

		const FSurfPickContext Ctx = FSurfPickContext::Synthetic(nullptr, Camera.Location, Camera.Rotation, Camera.FOV, Camera.Size, Pixel);

		// Nearest approach of the ray to the point, which should be zero.
		const FVector ToPoint = Point - Ctx.Origin;
		const double Along = FVector::DotProduct(ToPoint, Ctx.Direction);
		const double Miss = (ToPoint - Ctx.Direction * Along).Size();

		TestTrue(FString::Printf(TEXT("the ray through the pixel passes through the point (miss %.4f cm)"), Miss), Miss < 0.01);
		TestTrue(TEXT("and the point is in front of the ray"), Along > 0.0);
	}

	// The centre of the screen is straight ahead.
	FVector2D Centre;
	TestTrue(TEXT("the point straight ahead projects"), Camera.Project(FVector(0.0, 0.0, 0.0), Centre));
	TestTrue(TEXT("and lands in the middle of the screen"),
		FMath::IsNearlyEqual(Centre.X, 500.0, 0.01) && FMath::IsNearlyEqual(Centre.Y, 500.0, 0.01));

	// Behind the camera does not project.
	FVector2D Behind;
	TestFalse(TEXT("a point behind the camera does not project"), Camera.Project(FVector(0.0, 0.0, 2000.0), Behind));

	return true;
}

// --- Vertex mode ------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfClickPointTest, "SurfacingPro.Interaction.ClickPoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfClickPointTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Over(Group, *Patch);

	// Click a point: that point, and only it.
	TestTrue(TEXT("a click on a control point is handled"),
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr));
	TestEqual(TEXT("one point is selected"), Mode->GetPointSelection().Num(), 1);
	if (Mode->GetPointSelection().Num() == 1)
	{
		TestTrue(TEXT("and it is the one clicked"),
			Mode->GetPointSelection()[0].X == 1 && Mode->GetPointSelection()[0].Y == 1);
	}

	// Click another: replaces, does not add.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 3, 2)), nullptr);
	TestEqual(TEXT("a plain click replaces the selection"), Mode->GetPointSelection().Num(), 1);
	if (Mode->GetPointSelection().Num() == 1)
	{
		TestTrue(TEXT("with the new point"),
			Mode->GetPointSelection()[0].X == 3 && Mode->GetPointSelection()[0].Y == 2);
	}

	// Ctrl-click takes the points between the last click and this one,
	// along the grid: from (3,2) to (0,0) is three steps and two, six points.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 0, 0), /*bCtrl*/ true), nullptr);
	TestEqual(TEXT("Ctrl-click takes the points between"), Mode->GetPointSelection().Num(), 6);

	// Shift-click toggles one point, as it does in Blender: a point of the
	// path comes off, a point off it goes on.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 0, 0), /*bCtrl*/ false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Shift-click on a selected point removes it"), Mode->GetPointSelection().Num(), 5);

	// (3,0) is off any path from (3,2) to (0,0) that steps towards its end.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 3, 0), /*bCtrl*/ false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Shift-click adds a point"), Mode->GetPointSelection().Num(), 6);

	// Alt-click takes the row; again the column; again the point.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1), false, false, /*bAlt*/ true), nullptr);
	TestEqual(TEXT("Alt-click takes the row"), Mode->GetPointSelection().Num(), Patch->Width);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1), false, false, true), nullptr);
	TestEqual(TEXT("Alt-click again takes the column"), Mode->GetPointSelection().Num(), Patch->Height);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1), false, false, true), nullptr);
	TestEqual(TEXT("and again just the point"), Mode->GetPointSelection().Num(), 1);

	// Shift+Alt adds a row to what is there.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 3, 2), false, /*bShift*/ true, /*bAlt*/ true), nullptr);
	TestEqual(TEXT("Shift+Alt adds a row"), Mode->GetPointSelection().Num(), 1 + Patch->Width);

	// A click on nothing clears.
	Mode->HandleClickWith(Camera.ClickAt(FVector2D(5.0, 5.0)), nullptr);
	TestEqual(TEXT("a click on empty space clears the points"), Mode->GetPointSelection().Num(), 0);

	Group->Destroy();
	return true;
}

// --- Edge mode --------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfClickEdgeTest, "SurfacingPro.Interaction.ClickEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfClickEdgeTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Edge);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Over(Group, *Patch);

	// The middle of the segment between (1,1) and (2,1): one edge, two points.
	const FVector Mid = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 1)) * 0.5;

	TestTrue(TEXT("a click on an edge is handled"), Mode->HandleClickWith(Camera.ClickOn(Mid), nullptr));
	TestEqual(TEXT("one edge is two points"), Mode->GetPointSelection().Num(), 2);

	for (const FSurfPointRef& Ref : Mode->GetPointSelection())
	{
		TestTrue(TEXT("both points are on row 1"), Ref.Y == 1);
		TestTrue(TEXT("and are the segment's ends"), Ref.X == 1 || Ref.X == 2);
	}

	// Alt-click takes the whole line, as it does in Blender.
	Mode->HandleClickWith(Camera.ClickOn(Mid, /*bCtrl*/ false, /*bShift*/ false, /*bAlt*/ true), nullptr);
	TestEqual(TEXT("Alt-click takes the whole row"), Mode->GetPointSelection().Num(), Patch->Width);

	// Shift-click adds an edge to it.
	const FVector Other = (WorldOf(Group, *Patch, 1, 2) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
	Mode->HandleClickWith(Camera.ClickOn(Other, /*bCtrl*/ false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Shift-click adds an edge"), Mode->GetPointSelection().Num(), Patch->Width + 2);

	// With nothing selected at all, a click on an edge picks the patch and
	// the edge in one go - refusing that was most of "clicking an edge does
	// nothing".
	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Mode->SetSubMode(ESurfSubMode::Edge);

	TestTrue(TEXT("an edge click with no patch selected still works"),
		Mode->HandleClickWith(Camera.ClickOn(Mid), nullptr));
	TestTrue(TEXT("and selects the patch"), Mode->GetSelectedPatch().PatchId == PatchId);
	TestEqual(TEXT("and the edge"), Mode->GetPointSelection().Num(), 2);

	Group->Destroy();
	return true;
}

// --- Face mode --------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfClickFaceTest, "SurfacingPro.Interaction.ClickFace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfClickFaceTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Over(Group, *Patch);

	// The centre of cell (1,1).
	const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;

	TestTrue(TEXT("a click on a face is handled"), Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr));
	TestEqual(TEXT("one face is picked"), Mode->GetQuadSelection().Num(), 1);
	TestEqual(TEXT("and its four corners"), Mode->GetPointSelection().Num(), 4);

	if (Mode->GetQuadSelection().Num() == 1)
	{
		TestTrue(TEXT("it is cell 1,1"),
			Mode->GetQuadSelection()[0].X == 1 && Mode->GetQuadSelection()[0].Y == 1);
	}

	// The next cell over, with Ctrl: two faces, six points.
	const FVector Next = (WorldOf(Group, *Patch, 2, 1) + WorldOf(Group, *Patch, 3, 2)) * 0.5;
	Mode->HandleClickWith(Camera.ClickOn(Next, /*bCtrl*/ true), nullptr);
	TestEqual(TEXT("Ctrl-click adds a face"), Mode->GetQuadSelection().Num(), 2);
	TestEqual(TEXT("neighbours share an edge"), Mode->GetPointSelection().Num(), 6);

	Group->Destroy();
	return true;
}

// --- Object mode and switching patches -----------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfClickPatchTest, "SurfacingPro.Interaction.ClickPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfClickPatchTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Object);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Over(Group, *Patch);
	const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;

	// ClearSelection clears brush faces; the patch is dropped by selecting
	// none. (Worth knowing: there is no single "deselect everything".)
	Mode->SelectPatch(FSurfPatchRef());
	TestFalse(TEXT("starts with nothing selected"), Mode->GetSelectedPatch().IsValid());

	TestTrue(TEXT("a click on the surface is handled"), Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr));
	TestTrue(TEXT("and selects the patch"), Mode->GetSelectedPatch().PatchId == PatchId);

	// Delete is the verb that did nothing when pressed, because the key was
	// the editor's. The toolkit has to map it, or the key never arrives.
	TSharedPtr<FModeToolkit> Toolkit = Mode->GetToolkit().Pin();
	TestTrue(TEXT("the mode has a toolkit"), Toolkit.IsValid());
	if (Toolkit.IsValid())
	{
		TestTrue(TEXT("Delete is bound on the mode, ahead of the editor's own"),
			Toolkit->GetToolkitCommands()->IsActionMapped(FGenericCommands::Get().Delete));
		TestTrue(TEXT("and so is Select All"),
			Toolkit->GetToolkitCommands()->IsActionMapped(FGenericCommands::Get().SelectAll));
	}

	Group->Destroy();
	return true;
}

// --- Keys ------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfKeysTest, "SurfacingPro.Interaction.Keys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfKeysTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	const ESurfKeyScheme WasScheme = Settings ? Settings->KeyScheme : ESurfKeyScheme::Blender;
	if (Settings)
	{
		Settings->KeyScheme = ESurfKeyScheme::Blender;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Over(Group, *Patch);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr);
	TestEqual(TEXT("a point is selected to transform"), Mode->GetPointSelection().Num(), 1);

	// G starts a move. The same press arriving twice must not turn it into
	// anything else; a release and a second tap frees whatever lock is on
	// (S162a) - the axis keys name an axis.
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Pressed);
	TestTrue(TEXT("G starts a move"), Mode->GetModal() == ESurfModal::Move);
	TestTrue(TEXT("free at first"), Mode->GetAxisLock() == ESurfAxisLock::None);

	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Pressed);
	TestTrue(TEXT("the same press again changes nothing"), Mode->GetAxisLock() == ESurfAxisLock::None);

	Mode->InputKey(nullptr, nullptr, EKeys::X, IE_Pressed);
	TestTrue(TEXT("X locks the axis"), Mode->GetAxisLock() == ESurfAxisLock::X);

	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Released);
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Pressed);
	TestTrue(TEXT("a second tap of G frees it"), Mode->GetAxisLock() == ESurfAxisLock::None);
	TestTrue(TEXT("and it is still a move"), Mode->GetModal() == ESurfModal::Move);

	// With nothing locked, the second tap slides (S188a); a third moves again.
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Released);
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Pressed);
	TestTrue(TEXT("G G with nothing locked slides"), Mode->GetModal() == ESurfModal::Slide);
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Released);
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Pressed);
	TestTrue(TEXT("a third G moves again"), Mode->GetModal() == ESurfModal::Move);

	// Z, then Z again, then Z a third time: axis, plane, free.
	Mode->InputKey(nullptr, nullptr, EKeys::Z, IE_Pressed);
	TestTrue(TEXT("Z locks the axis"), Mode->GetAxisLock() == ESurfAxisLock::Z);
	Mode->InputKey(nullptr, nullptr, EKeys::Z, IE_Pressed);
	TestTrue(TEXT("Z again locks the plane"), Mode->GetAxisLock() == ESurfAxisLock::NotZ);
	Mode->InputKey(nullptr, nullptr, EKeys::Z, IE_Pressed);
	TestTrue(TEXT("Z a third time frees it"), Mode->GetAxisLock() == ESurfAxisLock::None);

	// Digits type a number and the status line says so.
	Mode->InputKey(nullptr, nullptr, EKeys::Two, IE_Pressed);
	Mode->InputKey(nullptr, nullptr, EKeys::Five, IE_Pressed);
	TestTrue(TEXT("typed digits show in the status"), Mode->DescribeModal().Contains(TEXT("25")));

	// Escape cancels, and the point is where it was.
	const FVector Before = Group->FindPatch(PatchId)->At(1, 1).Pos;
	Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed);
	TestTrue(TEXT("Escape cancels the move"), Mode->GetModal() == ESurfModal::None);
	TestTrue(TEXT("and the point is untouched"), Group->FindPatch(PatchId)->At(1, 1).Pos.Equals(Before, 1.e-3));

	// The sub-mode keys.
	Mode->InputKey(nullptr, nullptr, EKeys::G, IE_Released);
	Mode->InputKey(nullptr, nullptr, EKeys::Two, IE_Pressed);
	TestTrue(TEXT("2 is edge mode"), Mode->GetSubMode() == ESurfSubMode::Edge);
	Mode->InputKey(nullptr, nullptr, EKeys::Three, IE_Pressed);
	TestTrue(TEXT("3 is face mode"), Mode->GetSubMode() == ESurfSubMode::Face);
	Mode->InputKey(nullptr, nullptr, EKeys::Four, IE_Pressed);
	TestTrue(TEXT("4 is object mode"), Mode->GetSubMode() == ESurfSubMode::Object);
	Mode->InputKey(nullptr, nullptr, EKeys::One, IE_Pressed);
	TestTrue(TEXT("1 is vertex mode"), Mode->GetSubMode() == ESurfSubMode::Vertex);

	if (Settings)
	{
		Settings->KeyScheme = WasScheme;
	}

	Group->Destroy();
	return true;
}

// --- The verbs the author pressed that did nothing --------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfVerbsByClickTest, "SurfacingPro.Interaction.VerbsByClick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfVerbsByClickTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	const ESurfKeyScheme WasScheme = Settings ? Settings->KeyScheme : ESurfKeyScheme::Blender;
	if (Settings)
	{
		Settings->KeyScheme = ESurfKeyScheme::Blender;
	}

	// --- Separate, from a face click ------------------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr);
		TestEqual(TEXT("a face is picked"), Mode->GetQuadSelection().Num(), 1);

		// An interior cell of a 4 x 3 cell grid: the piece, two strips beside,
		// one above, one below. Five.
		TestTrue(TEXT("Separate runs from a clicked face"), Mode->SeparateSelectedFaces());
		TestEqual(TEXT("and the grid comes apart into five"), Group->NumPrimitives(), Before + 4);
		TestTrue(TEXT("the original is gone"), Group->FindPatch(PatchId) == nullptr);

		Group->Destroy();
	}

	// --- Weld, from two point clicks and M ---------------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);

		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 2, 1), /*bCtrl*/ true), nullptr);
		TestEqual(TEXT("two points are picked"), Mode->GetPointSelection().Num(), 2);

		const FVector Expected = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 1)) * 0.5;

		Mode->InputKey(nullptr, nullptr, EKeys::M, IE_Pressed);
		Mode->InputKey(nullptr, nullptr, EKeys::M, IE_Released);

		const FSurfPatch* Welded = Group->FindPatch(PatchId);
		TestTrue(TEXT("M welds the two picked points"),
			Welded && WorldOf(Group, *Welded, 1, 1).Equals(Expected, 1.e-3)
			&& WorldOf(Group, *Welded, 2, 1).Equals(Expected, 1.e-3));

		// And the click on the welded spot cycles to the one underneath.
		Mode->ClearPointSelection();
		Mode->HandleClickWith(Camera.ClickOn(Expected), nullptr);
		const FSurfPointRef FirstPick = Mode->GetPointSelection().Num() == 1 ? Mode->GetPointSelection()[0] : FSurfPointRef();
		Mode->HandleClickWith(Camera.ClickOn(Expected), nullptr);
		const FSurfPointRef SecondPick = Mode->GetPointSelection().Num() == 1 ? Mode->GetPointSelection()[0] : FSurfPointRef();

		TestTrue(TEXT("clicking a welded spot twice reaches the point underneath"),
			FirstPick.IsValid() && SecondPick.IsValid() && !(FirstPick == SecondPick));

		Group->Destroy();
	}

	// --- Extrude, from an edge click and E -------------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Edge);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 WidthBefore = Patch->Width;

		// The whole far column, by Alt-click on one of its segments.
		const FVector Mid = (WorldOf(Group, *Patch, WidthBefore - 1, 0) + WorldOf(Group, *Patch, WidthBefore - 1, 1)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Mid, /*bCtrl*/ false, /*bShift*/ false, /*bAlt*/ true), nullptr);
		TestEqual(TEXT("the far column is picked"), Mode->GetPointSelection().Num(), Patch->Height);

		Mode->InputKey(nullptr, nullptr, EKeys::E, IE_Pressed);
		Mode->InputKey(nullptr, nullptr, EKeys::E, IE_Released);

		const FSurfPatch* Grown = Group->FindPatch(PatchId);
		TestTrue(TEXT("E grows the patch"), Grown && Grown->Width == WidthBefore + 1);
		if (Grown && Grown->Width == WidthBefore + 1)
		{
			// In place: the new line starts on the edge it came from, and the
			// move that follows supplies the distance.
			TestTrue(TEXT("the new line starts on the old edge"),
				Grown->At(Grown->Width - 1, 0).Pos.Equals(Grown->At(Grown->Width - 2, 0).Pos, 1.e-3));
		}
		TestTrue(TEXT("and the new line follows the mouse"), Mode->GetModal() == ESurfModal::Move);
		TestTrue(TEXT("locked to the way the surface continues"), Mode->GetAxisLock() == ESurfAxisLock::Custom);
		TestTrue(TEXT("which the status line says"), Mode->DescribeModal().Contains(TEXT("outward")));

		Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed);
		TestTrue(TEXT("Escape ends the move"), Mode->GetModal() == ESurfModal::None);

		Group->Destroy();
	}

	// --- Box select replaces; Shift extends ------------------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);

		auto BoxAround = [&](int32 X, int32 Y, bool bExtend)
		{
			FVector2D Pixel;
			Camera.Project(WorldOf(Group, *Patch, X, Y), Pixel);

			Mode->BeginSelectGesture();
			Mode->AppendSelectPath(Pixel - FVector2D(6.0, 6.0));
			Mode->AppendSelectPath(Pixel + FVector2D(6.0, 6.0));

			return Mode->CommitSelectGestureWith(Camera.ClickAt(Pixel), /*bSelect*/ true, bExtend);
		};

		TestTrue(TEXT("a box around one point takes it"), BoxAround(1, 1, false));
		TestEqual(TEXT("one point"), Mode->GetPointSelection().Num(), 1);

		BoxAround(3, 2, false);
		TestEqual(TEXT("a second bare box replaces, it does not add"), Mode->GetPointSelection().Num(), 1);
		if (Mode->GetPointSelection().Num() == 1)
		{
			TestTrue(TEXT("with the second point"),
				Mode->GetPointSelection()[0].X == 3 && Mode->GetPointSelection()[0].Y == 2);
		}

		BoxAround(0, 0, true);
		TestEqual(TEXT("a Shift box extends"), Mode->GetPointSelection().Num(), 2);

		Group->Destroy();
	}

	if (Settings)
	{
		Settings->KeyScheme = WasScheme;
	}

	return true;
}

// --- Deleting faces, solidifying faces, and pulling a weld apart -------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfFaceCutsTest, "SurfacingPro.Interaction.FaceCuts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfFaceCutsTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	// --- Delete with faces picked drops them and the grid comes apart ----------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr);
		TestEqual(TEXT("a face is picked"), Mode->GetQuadSelection().Num(), 1);

		// An interior cell out of a 4 x 3 grid of cells: four pieces remain.
		TestTrue(TEXT("Delete with a face picked deletes the face"), Mode->DeleteSelection());
		TestEqual(TEXT("and leaves the rest as four patches"), Group->NumPrimitives(), Before + 3);
		TestTrue(TEXT("the original is gone"), Group->FindPatch(PatchId) == nullptr);
		TestFalse(TEXT("nothing is selected after a delete"), Mode->GetSelectedPatch().IsValid());

		Group->Destroy();
	}

	// --- Solidify with faces picked thickens only them ---------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr);

		// The piece becomes six patches (front, back, four skirts); the
		// remainder is four. One in, ten out.
		TestTrue(TEXT("Solidify with a face picked thickens the face"), Mode->SolidifySelectedPatch(20.0));
		TestEqual(TEXT("six for the solid, four for the rest"), Group->NumPrimitives(), Before - 1 + 6 + 4);

		Group->Destroy();
	}

	// --- A welded point can be pulled back off ---------------------------------------
	{
		USurfSettings* Settings = GetMutableDefault<USurfSettings>();
		const bool bWasSnapping = Settings ? Settings->bSnapToPoints : true;
		const bool bWasWithin = Settings ? Settings->bSnapWithinPatch : false;
		const double WasRadius = Settings ? Settings->PointSnapRadiusCm : 10.0;

		// The worst case: snapping on, within the patch on, a wide reach.
		// Pulling apart has to work even then, or a weld is permanent.
		if (Settings)
		{
			Settings->bSnapToPoints = true;
			Settings->bSnapWithinPatch = true;
			Settings->PointSnapRadiusCm = 50.0;
		}

		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);

		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 2, 1), /*bCtrl*/ true), nullptr);
		TestTrue(TEXT("two points weld"), Mode->WeldSelectedPoints(true));

		const FVector Welded = WorldOf(Group, *Group->FindPatch(PatchId), 1, 1);

		// Pick one of the two and pull it 30 cm away: within the snap reach
		// of the one it is leaving, which must not pull it back.
		Mode->ClearPointSelection();
		Mode->HandleClickWith(Camera.ClickOn(Welded), nullptr);
		TestEqual(TEXT("one of the welded pair is picked"), Mode->GetPointSelection().Num(), 1);

		Mode->MoveSelectedPoints(FVector(30.0, 0.0, 0.0));

		const FSurfPatch* After = Group->FindPatch(PatchId);
		const FSurfPointRef Moved = Mode->GetPointSelection().Num() == 1 ? Mode->GetPointSelection()[0] : FSurfPointRef();
		const double Apart = After && Moved.IsValid()
			? FVector::Dist(WorldOf(Group, *After, Moved.X, Moved.Y), Welded)
			: 0.0;

		TestTrue(FString::Printf(TEXT("the moved point stays away from the one it left (%.1f cm apart)"), Apart),
			Apart > 25.0);

		if (Settings)
		{
			Settings->bSnapToPoints = bWasSnapping;
			Settings->bSnapWithinPatch = bWasWithin;
			Settings->PointSnapRadiusCm = WasRadius;
		}

		Group->Destroy();
	}

	return true;
}

// --- Bridge, and object mode moving the whole patch -----------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBridgeAndObjectTest, "SurfacingPro.Interaction.BridgeAndObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBridgeAndObjectTest::RunTest(const FString& Parameters)
{
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	// --- Bridge: two Alt-clicked rows become a new 2 x N patch --------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		// Row 0 by Alt-click, row 3 added by Shift+Alt.
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 0), false, false, /*bAlt*/ true), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 3), false, /*bShift*/ true, /*bAlt*/ true), nullptr);
		TestEqual(TEXT("two rows are selected"), Mode->GetPointSelection().Num(), Patch->Width * 2);

		// The two opposite borders of one patch (S208a): the patch closes on
		// itself - one line more, lying on the first - rather than a bridge
		// patch, whose facing an average normal could not settle on a tube.
		const int32 HeightBefore = Patch->Height;
		const int32 WidthBefore = Patch->Width;
		TestTrue(TEXT("two lines bridge"), Mode->BridgeSelectedLines());
		TestEqual(TEXT("no new patch: the patch closed on itself"), Group->NumPrimitives(), Before);

		const FSurfPatch* Closed = Group->FindPatch(PatchId);
		TestTrue(TEXT("the patch is selected"), Closed && Mode->GetSelectedPatch().PatchId == PatchId);
		if (Closed)
		{
			TestEqual(TEXT("one line taller"), Closed->Height, HeightBefore + 1);
			TestEqual(TEXT("as wide as before"), Closed->Width, WidthBefore);
			TestTrue(TEXT("its last line lies on its first"),
				Closed->At(1, Closed->Height - 1).Pos.Equals(Closed->At(1, 0).Pos, 1.e-3));
			TestEqual(TEXT("the new line is selected"), Mode->GetPointSelection().Num(), Closed->Width);
		}

		// One line is not a bridge.
		Mode->SelectPatch(FSurfPatchRef());
		FSurfPatchRef Back;
		Back.Group = Group;
		Back.PatchId = PatchId;
		Mode->SelectPatch(Back);
		Mode->ClearPointSelection();
		Patch = Group->FindPatch(PatchId);
		if (!Patch)
		{
			AddError(TEXT("the patch is gone"));
			return false;
		}
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1), false, false, true), nullptr);
		TestFalse(TEXT("one line does not bridge"), Mode->BridgeSelectedLines());

		Group->Destroy();
	}

	// --- Object mode: G moves every point of the patch ------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Object);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FVector Before00 = Patch->At(0, 0).Pos;
		const FVector Before33 = Patch->At(3, 3).Pos;

		TestTrue(TEXT("G in object mode starts a move with nothing picked inside the patch"),
			Mode->BeginModal(ESurfModal::Move, nullptr, nullptr));
		TestEqual(TEXT("and takes every point"), Mode->GetPointSelection().Num(), Patch->NumPoints());

		Mode->CancelModal();
		TestEqual(TEXT("cancelling gives the points back"), Mode->GetPointSelection().Num(), 0);
		TestTrue(TEXT("and nothing moved"),
			Group->FindPatch(PatchId)->At(0, 0).Pos.Equals(Before00, 1.e-3)
			&& Group->FindPatch(PatchId)->At(3, 3).Pos.Equals(Before33, 1.e-3));

		Group->Destroy();
	}

	return true;
}


// --- Extrude takes any selection ------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfExtrudeAnythingTest, "SurfacingPro.Interaction.ExtrudeAnything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfExtrudeAnythingTest::RunTest(const FString& Parameters)
{
	// Twenty-eight times in one session E was pressed on an edge that was not
	// a whole border line, and twenty-eight times nothing happened. Every one
	// of those is a new patch now, and the move that follows pulls it out.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	// --- An interior segment, clicked in edge sub-mode, then E ----------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Edge);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		// The segment between (2,1) and (2,2): a column segment in the middle.
		const FVector Mid = (WorldOf(Group, *Patch, 2, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Mid), nullptr);
		TestEqual(TEXT("one segment is two points"), Mode->GetPointSelection().Num(), 2);

		Mode->InputKey(nullptr, nullptr, EKeys::E, IE_Pressed);
		Mode->InputKey(nullptr, nullptr, EKeys::E, IE_Released);

		TestEqual(TEXT("E on an interior segment makes one new patch"), Group->NumPrimitives(), Before + 1);
		TestTrue(TEXT("the original is untouched"), Group->FindPatch(PatchId) && Group->FindPatch(PatchId)->Width == 5);

		const FGuid MadeId = Mode->GetSelectedPatch().PatchId;
		const FSurfPatch* Made = Group->FindPatch(MadeId);
		TestTrue(TEXT("the new patch is the selected one"), Made && MadeId != PatchId);
		if (Made)
		{
			TestTrue(TEXT("two lines wide, two points long"), Made->Width == 2 && Made->Height == 2);
			TestTrue(TEXT("it starts on the segment it came from"),
				Made->At(0, 0).Pos.Equals(Group->FindPatch(PatchId)->At(2, 1).Pos, 1.e-3)
				&& Made->At(0, 1).Pos.Equals(Group->FindPatch(PatchId)->At(2, 2).Pos, 1.e-3));
			TestTrue(TEXT("in place: the far line starts on the near one"),
				Made->At(1, 0).Pos.Equals(Made->At(0, 0).Pos, 1.e-3));
		}

		TestEqual(TEXT("the far line is what is selected"), Mode->GetPointSelection().Num(), 2);
		for (const FSurfPointRef& Ref : Mode->GetPointSelection())
		{
			TestTrue(TEXT("on the new patch, its outer line"), Ref.PatchId == MadeId && Ref.X == 1);
		}

		TestTrue(TEXT("and it follows the mouse"), Mode->GetModal() == ESurfModal::Move);
		TestTrue(TEXT("along the surface normal"), Mode->GetAxisLock() == ESurfAxisLock::Custom);

		Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed);
		Group->Destroy();
	}

	// --- Two segments at once: two new patches, both outer lines selected -----------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Edge);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		const FVector First = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 1)) * 0.5;
		const FVector Second = (WorldOf(Group, *Patch, 3, 2) + WorldOf(Group, *Patch, 3, 3)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(First), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(Second, /*bCtrl*/ false, /*bShift*/ true), nullptr);
		TestEqual(TEXT("two segments are four points"), Mode->GetPointSelection().Num(), 4);

		TestTrue(TEXT("both extrude at once"), Mode->ExtrudeSelectedEdge(/*bInPlace*/ true));
		TestEqual(TEXT("into two new patches"), Group->NumPrimitives(), Before + 2);
		TestEqual(TEXT("with both outer lines selected"), Mode->GetPointSelection().Num(), 4);

		TSet<FGuid> Touched;
		for (const FSurfPointRef& Ref : Mode->GetPointSelection())
		{
			Touched.Add(Ref.PatchId);
			TestTrue(TEXT("none of them on the original"), Ref.PatchId != PatchId);
		}
		TestEqual(TEXT("on two different patches"), Touched.Num(), 2);

		Group->Destroy();
	}

	// --- The whole border at once: one wall that meets itself ----------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const int32 Before = Group->NumPrimitives();
		const int32 W = Patch->Width;
		const int32 H = Patch->Height;

		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (X == 0 || Y == 0 || X == W - 1 || Y == H - 1)
				{
					FSurfPointRef Ref;
					Ref.Group = Group;
					Ref.PatchId = PatchId;
					Ref.X = X;
					Ref.Y = Y;
					Mode->AddPointToSelection(Ref);
				}
			}
		}

		TestTrue(TEXT("the border extrudes"), Mode->ExtrudeSelectedEdge(/*bInPlace*/ true));
		TestEqual(TEXT("as one patch, not four"), Group->NumPrimitives(), Before + 1);

		const FSurfPatch* Wall = Group->FindPatch(Mode->GetSelectedPatch().PatchId);
		TestTrue(TEXT("a wall all the way round"), Wall && Wall->Width == 2 * (W - 1) + 2 * (H - 1) + 1 && Wall->Height == 2);
		TestEqual(TEXT("its top line is the selection"), Mode->GetPointSelection().Num(), 2 * (W - 1) + 2 * (H - 1) + 1);

		Group->Destroy();
	}

	// --- Faces: a lid and one wall, moving as one, undone as one -------------------
	// With the faces beneath kept, so the source patch can be checked whole.
	USurfSettings* ExtrudePrefs = GetMutableDefault<USurfSettings>();
	const ESurfExtrudeUnder UnderWas = ExtrudePrefs ? ExtrudePrefs->ExtrudeUnder : ESurfExtrudeUnder::Drop;
	if (ExtrudePrefs)
	{
		ExtrudePrefs->ExtrudeUnder = ESurfExtrudeUnder::Keep;
	}
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		auto CentreOfQuad = [&](int32 X, int32 Y)
		{
			return (WorldOf(Group, *Patch, X, Y) + WorldOf(Group, *Patch, X + 1, Y)
				+ WorldOf(Group, *Patch, X, Y + 1) + WorldOf(Group, *Patch, X + 1, Y + 1)) * 0.25;
		};

		Mode->HandleClickWith(Camera.ClickOn(CentreOfQuad(1, 1)), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(CentreOfQuad(2, 1), false, /*bShift*/ true), nullptr);
		TestEqual(TEXT("two faces side by side"), Mode->GetQuadSelection().Num(), 2);
		TestEqual(TEXT("which is six points"), Mode->GetPointSelection().Num(), 6);

		const FVector SourceCorner = Group->FindPatch(PatchId)->At(1, 1).Pos;

		TestTrue(TEXT("faces extrude"), Mode->ExtrudeSelectedEdge(/*bInPlace*/ true));
		TestEqual(TEXT("a lid and one wall round it"), Group->NumPrimitives(), Before + 2);

		const FGuid LidId = Mode->GetSelectedPatch().PatchId;
		const FSurfPatch* Lid = Group->FindPatch(LidId);
		TestTrue(TEXT("the lid is selected"), Lid && LidId != PatchId);
		if (Lid)
		{
			TestTrue(TEXT("the lid is the block's size"), Lid->Width == 3 && Lid->Height == 2);
			TestTrue(TEXT("and starts on the faces it came from"), Lid->At(0, 0).Pos.Equals(SourceCorner, 1.e-3));
		}

		// Lid corners plus the wall's top line: 6 + 7 (six round the block,
		// and the first again where the wall meets itself).
		TestEqual(TEXT("the lid and the wall's top are selected"), Mode->GetPointSelection().Num(), 13);
		TestEqual(TEXT("in face terms, the lid's faces"), Mode->GetQuadSelection().Num(), 2);

		// Move them up: the lid rises, the wall's top with it, its bottom stays.
		Mode->MoveSelectedPoints(FVector(0.0, 0.0, 50.0));

		Lid = Group->FindPatch(LidId);
		TestTrue(TEXT("the lid rose"), Lid && FMath::IsNearlyEqual(Lid->At(0, 0).Pos.Z, SourceCorner.Z + 50.0, 1.e-2));

		int32 Walls = 0;
		Group->ForEachPatch([&](const FSurfPatch& Each)
		{
			if (Each.Id == PatchId || Each.Id == LidId)
			{
				return;
			}
			++Walls;

			// One sheet round the block that meets itself: seven along, two up.
			TestTrue(TEXT("the wall goes all the way round"), Each.Width == 7 && Each.Height == 2);
			if (Each.Width == 7 && Each.Height == 2)
			{
				TestTrue(TEXT("and meets itself"), Each.At(0, 0).Pos.Equals(Each.At(6, 0).Pos, 1.e-3));
				for (int32 X = 0; X < 7; ++X)
				{
					TestTrue(TEXT("its bottom stayed"), FMath::IsNearlyEqual(Each.At(X, 0).Pos.Z, SourceCorner.Z, 1.e-2));
					TestTrue(TEXT("and its top rose with the lid"), FMath::IsNearlyEqual(Each.At(X, 1).Pos.Z, SourceCorner.Z + 50.0, 1.e-2));
				}
			}
		});
		TestEqual(TEXT("one wall"), Walls, 1);

		TestTrue(TEXT("the source patch still has every face"), Group->FindPatch(PatchId) && Group->FindPatch(PatchId)->Width == 5);

		// Undo takes both patches away and hands the two faces back.
		GEditor->UndoTransaction();
		TestEqual(TEXT("undo removes the lid and the wall"), Group->NumPrimitives(), Before);
		TestEqual(TEXT("and the faces are selected again"), Mode->GetQuadSelection().Num(), 2);
		TestEqual(TEXT("as are their corners"), Mode->GetPointSelection().Num(), 6);
		TestTrue(TEXT("on the source patch"), Mode->GetSelectedPatch().PatchId == PatchId);

		Group->Destroy();
	}
	if (ExtrudePrefs)
	{
		ExtrudePrefs->ExtrudeUnder = UnderWas;
	}

	// --- Faces, with the faces beneath dropped: a hollow block ---------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const FCamera Camera = FCamera::Over(Group, *Patch);
		const int32 Before = Group->NumPrimitives();

		auto CentreOfQuad = [&](int32 X, int32 Y)
		{
			return (WorldOf(Group, *Patch, X, Y) + WorldOf(Group, *Patch, X + 1, Y)
				+ WorldOf(Group, *Patch, X, Y + 1) + WorldOf(Group, *Patch, X + 1, Y + 1)) * 0.25;
		};

		// Two cells in the middle of a 4 x 3 cell grid: four pieces round the hole.
		Mode->HandleClickWith(Camera.ClickOn(CentreOfQuad(1, 1)), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(CentreOfQuad(2, 1), false, /*bShift*/ true), nullptr);
		TestEqual(TEXT("two faces side by side"), Mode->GetQuadSelection().Num(), 2);

		TestTrue(TEXT("faces extrude, dropping the faces beneath"), Mode->ExtrudeSelectedEdge(/*bInPlace*/ true, USurfEdMode::ExtrudeBySpacing, /*Under*/ static_cast<int32>(ESurfExtrudeUnder::Drop)));
		TestEqual(TEXT("a lid, a wall, and four pieces where the source was"), Group->NumPrimitives(), Before - 1 + 4 + 2);
		TestFalse(TEXT("the source patch is gone"), Group->FindPatch(PatchId) != nullptr);
		TestEqual(TEXT("the lid and the wall's top are selected"), Mode->GetPointSelection().Num(), 13);
		TestEqual(TEXT("the lid's faces"), Mode->GetQuadSelection().Num(), 2);

		// One undo takes it all back: the source whole, its two faces selected.
		GEditor->UndoTransaction();
		TestEqual(TEXT("undo restores the source and removes the new pieces"), Group->NumPrimitives(), Before);
		TestTrue(TEXT("the source is back"), Group->FindPatch(PatchId) != nullptr);
		TestEqual(TEXT("with its faces selected again"), Mode->GetQuadSelection().Num(), 2);

		Group->Destroy();
	}

	// --- Undo after growing an edge gives back the edge ---------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		const int32 WidthBefore = Patch->Width;
		const int32 HeightBefore = Patch->Height;
		for (int32 Y = 0; Y < HeightBefore; ++Y)
		{
			FSurfPointRef Ref;
			Ref.Group = Group;
			Ref.PatchId = PatchId;
			Ref.X = WidthBefore - 1;
			Ref.Y = Y;
			Mode->AddPointToSelection(Ref);
		}

		TestTrue(TEXT("the border grows"), Mode->ExtrudeSelectedEdge());
		TestEqual(TEXT("by one column"), Group->FindPatch(PatchId)->Width, WidthBefore + 1);

		GEditor->UndoTransaction();
		TestEqual(TEXT("undo takes the column away"), Group->FindPatch(PatchId)->Width, WidthBefore);
		TestEqual(TEXT("and gives back the line it grew from"), Mode->GetPointSelection().Num(), HeightBefore);
		for (const FSurfPointRef& Ref : Mode->GetPointSelection())
		{
			TestTrue(TEXT("the old border"), Ref.PatchId == PatchId && Ref.X == WidthBefore - 1);
		}

		// Redo, and the selection is still something rather than nothing.
		GEditor->RedoTransaction();
		TestEqual(TEXT("redo grows it again"), Group->FindPatch(PatchId)->Width, WidthBefore + 1);
		TestTrue(TEXT("with a selection to carry on from"), Mode->GetPointSelection().Num() > 0);

		Group->Destroy();
	}

	// --- Object sub-mode says what it needs ----------------------------------------
	{
		ASurfGroup* Group = nullptr;
		FGuid PatchId;
		const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Object);
		if (!Patch)
		{
			AddError(TEXT("no patch"));
			return false;
		}

		TestFalse(TEXT("object sub-mode has nothing to pull"), Mode->ExtrudeSelectedEdge(true));
		TestTrue(TEXT("and says so where it can be read"), Mode->Inspector->LastResultWasRefusal()
			&& Mode->Inspector->LastResult.StartsWith(TEXT("extrude:")));

		Group->Destroy();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPressKeepsSelectionTest, "SurfacingPro.Interaction.PressKeepsSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPressKeepsSelectionTest::RunTest(const FString& Parameters)
{
	/**
	 * "selecting faces. rotating or moving. the selection is gone and i have
	 * to select again (was on that brush)" (S149a).
	 *
	 * The gizmo's ball falls through to a pick when a press turns nothing,
	 * so it cannot swallow a click (S139a) - but it stands over the middle
	 * of whatever is selected, so that pick finds nothing more often than
	 * not, and dropping the selection there loses what the hand was reaching
	 * for. A press that found nothing keeps the selection; an ordinary click
	 * on nothing still drops it.
	 */
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Camera = FCamera::Above(Group, *Patch);

	FVector2D Min(1.e9, 1.e9);
	for (int32 Y = 0; Y < Patch->Height; ++Y)
	{
		for (int32 X = 0; X < Patch->Width; ++X)
		{
			FVector2D Pixel;
			Camera.Project(WorldOf(Group, *Patch, X, Y), Pixel);
			Min = FVector2D::Min(Min, Pixel);
		}
	}
	const FVector2D Nothing = Min - FVector2D(80.0, 80.0);

	Mode->SelectAllPoints();
	const int32 Was = Mode->GetPointSelection().Num();
	TestTrue(TEXT("there is a selection to lose"), Was > 0);

	Mode->HandleClickWith(Camera.ClickAt(Nothing), nullptr, /*bKeepOnMiss*/ true);
	TestEqual(TEXT("a press that found nothing keeps it"), Mode->GetPointSelection().Num(), Was);

	Mode->HandleClickWith(Camera.ClickAt(Nothing), nullptr, /*bKeepOnMiss*/ false);
	TestEqual(TEXT("and an ordinary click on nothing still drops it"), Mode->GetPointSelection().Num(), 0);

	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGestureStaysOnPickedTest, "SurfacingPro.Interaction.GestureStaysOnPicked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGestureStaysOnPickedTest::RunTest(const FString& Parameters)
{
	/**
	 * "when working on the brush box and selecting its vertices im also
	 * selecting other object verts. should be able to select (overall!) for
	 * selected objects only" (S149a).
	 *
	 * With a piece picked, a box in vertex, edge or face is about that
	 * piece's parts. With nothing picked it reaches everything, because that
	 * is how the first piece is picked.
	 */
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* GroupA = nullptr;
	FGuid IdA;
	if (!MakeSheet(Mode, GroupA, IdA, ESurfSubMode::Vertex))
	{
		AddError(TEXT("no first patch"));
		return false;
	}
	ASurfGroup* GroupB = nullptr;
	FGuid IdB;
	if (!MakeSheet(Mode, GroupB, IdB, ESurfSubMode::Vertex))
	{
		AddError(TEXT("no second patch"));
		return false;
	}

	// Re-found after the second was added: a patch array can move.
	const FSurfPatch* A = GroupA ? GroupA->FindPatch(IdA) : nullptr;
	const FSurfPatch* B = GroupB ? GroupB->FindPatch(IdB) : nullptr;
	if (!A || !B || GroupA != GroupB)
	{
		AddError(TEXT("two patches in one group were expected"));
		return false;
	}

	// A flat view far enough up to hold both.
	FBox Both(ForceInit);
	for (const FSurfControlPoint& Point : A->Points)
	{
		Both += GroupA->GetActorTransform().TransformPosition(Point.Pos);
	}
	for (const FSurfControlPoint& Point : B->Points)
	{
		Both += GroupB->GetActorTransform().TransformPosition(Point.Pos);
	}
	const double Extent = FMath::Max(100.0, Both.GetExtent().GetMax());

	FCamera Camera;
	Camera.World = GroupA->GetWorld();
	Camera.Location = Both.GetCenter() + FVector(0.0, 0.0, Extent * 3.0);
	Camera.Rotation = FRotator(-90.0, 0.0, 0.0);
	Camera.ViewWidth = Extent * 4.0;

	FVector2D Min(1.e9, 1.e9);
	FVector2D Max(-1.e9, -1.e9);
	for (const FVector& Corner : { Both.Min, Both.Max,
		FVector(Both.Min.X, Both.Max.Y, Both.Min.Z), FVector(Both.Max.X, Both.Min.Y, Both.Min.Z) })
	{
		FVector2D Pixel;
		Camera.Project(Corner, Pixel);
		Min = FVector2D::Min(Min, Pixel);
		Max = FVector2D::Max(Max, Pixel);
	}

	auto BoxEverything = [&]()
	{
		Mode->SetSelectTool(ESurfSelectTool::Box);
		Mode->BeginSelectGesture();
		Mode->AppendSelectPath(Min - FVector2D(20.0, 20.0));
		Mode->AppendSelectPath(Max + FVector2D(20.0, 20.0));
		Mode->CommitSelectGestureWith(Camera.ClickAt(Max), /*bSelect*/ true, /*bExtend*/ false);
	};

	auto PointsOn = [Mode](const FGuid& PatchId)
	{
		int32 Count = 0;
		for (const FSurfPointRef& Ref : Mode->GetPointSelection())
		{
			Count += Ref.PatchId == PatchId ? 1 : 0;
		}
		return Count;
	};

	// Nothing picked: the box reaches both.
	Mode->ClearPointSelection();
	Mode->ClearSelection();
	Mode->SelectPatch(FSurfPatchRef());
	BoxEverything();
	TestTrue(TEXT("with nothing picked a box reaches the first"), PointsOn(IdA) > 0);
	TestTrue(TEXT("and the second"), PointsOn(IdB) > 0);

	// One picked: the box stays on it.
	Mode->ClearPointSelection();
	Mode->ClearSelection();
	{
		FSurfPatchRef Ref;
		Ref.Group = GroupA;
		Ref.PatchId = IdA;
		Mode->SelectPatch(Ref);
	}
	BoxEverything();
	TestTrue(TEXT("with one picked the box still takes its points"), PointsOn(IdA) > 0);
	TestEqual(TEXT("and none of the other's"), PointsOn(IdB), 0);

	Mode->ClearPointSelection();
	Mode->ClearSelection();
	Mode->SelectPatch(FSurfPatchRef());
	Mode->CancelSelectGesture();
	GroupA->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfBoxTakesBrushCornersTest, "SurfacingPro.Interaction.BoxTakesBrushCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfBoxTakesBrushCornersTest::RunTest(const FString& Parameters)
{
	/**
	 * A box, a lasso or the brush takes a brush's corners (S149a).
	 *
	 * The gesture had never known what a brush corner was: boxing a box's
	 * corners took the patch points behind it and left the corners alone,
	 * which from the chair reads as one thing going wrong rather than two.
	 * A corner is drawn where it can be clicked (S113a) - it is taken where
	 * it can be boxed too.
	 */
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Mode->SelectPatch(FSurfPatchRef());

	Mode->Inspector->NewBox();
	const FSurfFaceRef Face = Mode->GetActiveFace();
	ASurfGroup* Group = Face.Group.Get();
	const FSurfBrush* Brush = Group ? Group->FindBrush(Face.BrushId) : nullptr;
	FSurfHull Hull;
	if (!Group || !Brush || !SurfBrushMath::DeriveHull(*Brush, Hull) || Hull.Vertices.Num() != 8)
	{
		AddError(TEXT("a box with eight corners was expected"));
		return false;
	}

	FBox Bounds(ForceInit);
	for (const FVector& Vertex : Hull.Vertices)
	{
		Bounds += Group->GetActorTransform().TransformPosition(Vertex);
	}
	const double Extent = FMath::Max(100.0, Bounds.GetExtent().GetMax());

	FCamera Camera;
	Camera.World = Group->GetWorld();
	Camera.Location = Bounds.GetCenter() + FVector(0.0, 0.0, Extent * 4.0);
	Camera.Rotation = FRotator(-90.0, 0.0, 0.0);
	Camera.ViewWidth = Extent * 4.0;

	FVector2D Min(1.e9, 1.e9);
	FVector2D Max(-1.e9, -1.e9);
	for (const FVector& Vertex : Hull.Vertices)
	{
		FVector2D Pixel;
		Camera.Project(Group->GetActorTransform().TransformPosition(Vertex), Pixel);
		Min = FVector2D::Min(Min, Pixel);
		Max = FVector2D::Max(Max, Pixel);
	}

	Mode->SetSubMode(ESurfSubMode::Vertex);
	Mode->SetSelectTool(ESurfSelectTool::Box);
	Mode->BeginSelectGesture();
	Mode->AppendSelectPath(Min - FVector2D(20.0, 20.0));
	Mode->AppendSelectPath(Max + FVector2D(20.0, 20.0));
	Mode->CommitSelectGestureWith(Camera.ClickAt(Max), /*bSelect*/ true, /*bExtend*/ false);

	// Seen from straight above, the four on top are not behind anything.
	TestTrue(TEXT("a box over a brush takes its corners"),
		Mode->GetBrushCornerSelection().Num() >= 4);
	for (const FSurfBrushCornerRef& Corner : Mode->GetBrushCornerSelection())
	{
		TestTrue(TEXT("and every one of them is this brush's"), Corner.BrushId == Face.BrushId);
	}

	Mode->CancelSelectGesture();
	Mode->ClearPointSelection();
	Mode->ClearSelection();
	Mode->SetSubMode(ESurfSubMode::Face);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfSelectFromBehindTest, "SurfacingPro.Interaction.SelectFromBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfSelectFromBehindTest::RunTest(const FString& Parameters)
{
	// A sheet's points are its surface: seen from behind, its own surface
	// must not hide them. Clicks and a box from below take what a box from
	// above takes.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	FCamera Above = FCamera::Over(Group, *Patch);
	FCamera Below = Above;
	Below.Location.Z = Above.Location.Z - 2.0 * (Above.Location.Z - WorldOf(Group, *Patch, 0, 0).Z);
	Below.Rotation = FRotator(90.0, 0.0, 0.0);

	// Two clicks from below: two points.
	Mode->ClearPointSelection();
	TestTrue(TEXT("a click from behind picks a point"), Mode->HandleClickWith(Below.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr));
	Mode->HandleClickWith(Below.ClickOn(WorldOf(Group, *Patch, 3, 2), false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("a second click from behind adds"), Mode->GetPointSelection().Num(), 2);

	// A box from below takes the whole patch, as one from above does.
	auto BoxAll = [&](const FCamera& Camera)
	{
		Mode->ClearPointSelection();
		FVector2D Min(1.e9, 1.e9);
		FVector2D Max(-1.e9, -1.e9);
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				FVector2D Pixel;
				Camera.Project(WorldOf(Group, *Patch, X, Y), Pixel);
				Min = FVector2D::Min(Min, Pixel);
				Max = FVector2D::Max(Max, Pixel);
			}
		}
		Mode->SetSelectTool(ESurfSelectTool::Box);
		Mode->AppendSelectPath(Min - FVector2D(5.0, 5.0));
		Mode->AppendSelectPath(Max + FVector2D(5.0, 5.0));
		Mode->CommitSelectGestureWith(Camera.ClickAt(Max), /*bSelect*/ true, /*bExtend*/ false);
		return Mode->GetPointSelection().Num();
	};

	TestEqual(TEXT("a box from above takes every point"), BoxAll(Above), Patch->NumPoints());
	TestEqual(TEXT("a box from below takes every point too"), BoxAll(Below), Patch->NumPoints());

	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMultiPatchSelectionTest, "SurfacingPro.Interaction.MultiPatchSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMultiPatchSelectionTest::RunTest(const FString& Parameters)
{
	// A selection spans patches: Shift-click adds a point on a second patch
	// and keeps the first's; a Shift-click that lands on the second patch
	// but misses its points adds the patch instead of dropping everything;
	// a box takes both; every patch the points reach into is selected.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid IdA;
	const FSurfPatch* A = MakeSheet(Mode, Group, IdA, ESurfSubMode::Vertex);
	if (!A)
	{
		AddError(TEXT("no first patch"));
		return false;
	}

	// A second sheet, placed clear of the first by the verb itself.
	Mode->Inspector->NewPatch();
	const FGuid IdB = Mode->GetSelectedPatch().PatchId;
	const FSurfPatch* B = Group->FindPatch(IdB);
	A = Group->FindPatch(IdA);
	if (!B || !A || IdB == IdA)
	{
		AddError(TEXT("no second patch"));
		return false;
	}
	Mode->SetSubMode(ESurfSubMode::Vertex);

	// A camera over both.
	FBox Bounds(ForceInit);
	for (const FSurfControlPoint& Point : A->Points) { Bounds += Group->GetActorTransform().TransformPosition(Point.Pos); }
	for (const FSurfControlPoint& Point : B->Points) { Bounds += Group->GetActorTransform().TransformPosition(Point.Pos); }
	FCamera Camera = FCamera::Over(Group, *A);
	Camera.Location = Bounds.GetCenter() + FVector(0.0, 0.0, FMath::Max(100.0, Bounds.GetExtent().GetMax()) * 3.0);

	FSurfPatchRef RefA;
	RefA.Group = Group;
	RefA.PatchId = IdA;
	Mode->SelectPatch(RefA);

	// A point on A, then Shift a point on B: two points, two patches.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *A, 1, 1)), nullptr);
	TestEqual(TEXT("one point on the first patch"), Mode->GetPointSelection().Num(), 1);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *B, 1, 1), false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Shift-click on the second patch adds its point"), Mode->GetPointSelection().Num(), 2);
	TestEqual(TEXT("both patches are selected"), Mode->GetSelectedPatches().Num(), 2);

	// Shift-click on the second patch's surface, between its points: the
	// selection stays.
	const FVector Between = (WorldOf(Group, *B, 1, 1) + WorldOf(Group, *B, 2, 2)) * 0.5;
	Mode->HandleClickWith(Camera.ClickOn(Between, false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("a Shift-click that misses a point keeps the selection"), Mode->GetPointSelection().Num(), 2);
	TestEqual(TEXT("and both patches"), Mode->GetSelectedPatches().Num(), 2);

	// A plain click on the second patch's surface starts over on it.
	Mode->HandleClickWith(Camera.ClickOn(Between), nullptr);
	TestEqual(TEXT("a plain click on a patch starts over"), Mode->GetPointSelection().Num(), 0);
	TestTrue(TEXT("on that patch"), Mode->GetSelectedPatch().PatchId == IdB);

	// A box over both takes every point of both.
	FVector2D Min(1.e9, 1.e9);
	FVector2D Max(-1.e9, -1.e9);
	for (const FSurfPatch* Patch : { A, B })
	{
		for (int32 Y = 0; Y < Patch->Height; ++Y)
		{
			for (int32 X = 0; X < Patch->Width; ++X)
			{
				FVector2D Pixel;
				Camera.Project(WorldOf(Group, *Patch, X, Y), Pixel);
				Min = FVector2D::Min(Min, Pixel);
				Max = FVector2D::Max(Max, Pixel);
			}
		}
	}
	Mode->SetSelectTool(ESurfSelectTool::Box);
	Mode->BeginSelectGesture();
	Mode->AppendSelectPath(Min - FVector2D(5.0, 5.0));
	Mode->AppendSelectPath(Max + FVector2D(5.0, 5.0));
	Mode->CommitSelectGestureWith(Camera.ClickAt(Max), /*bSelect*/ true, /*bExtend*/ false);

	/**
	 * A bare box stays on the piece being worked on (S149a): "when working
	 * on the brush box and selecting its vertices im also selecting other
	 * object verts. should be able to select (overall!) for selected objects
	 * only." The plain click above left the second patch the picked one, so
	 * that is what the box is about.
	 */
	TestEqual(TEXT("a bare box stays on the patch that is picked"),
		Mode->GetPointSelection().Num(), B->NumPoints());

	// And Shift reaches out, which is how points on a second piece are added.
	Mode->BeginSelectGesture();
	Mode->AppendSelectPath(Min - FVector2D(5.0, 5.0));
	Mode->AppendSelectPath(Max + FVector2D(5.0, 5.0));
	Mode->CommitSelectGestureWith(Camera.ClickAt(Max), /*bSelect*/ true, /*bExtend*/ true);
	TestEqual(TEXT("a box held with Shift takes both"), Mode->GetPointSelection().Num(), A->NumPoints() + B->NumPoints());
	TestEqual(TEXT("and selects both patches"), Mode->GetSelectedPatches().Num(), 2);

	// Select all keeps to the patches the selection spans.
	Mode->SelectAllPoints();
	TestEqual(TEXT("select all takes every point of both"), Mode->GetPointSelection().Num(), A->NumPoints() + B->NumPoints());

	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfObjectModeAndEdgesTest, "SurfacingPro.Interaction.ObjectModeAndEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfObjectModeAndEdgesTest::RunTest(const FString& Parameters)
{
	// Object sub-mode: G moves the patch, a click on nothing deselects,
	// select all then delete takes every patch. Edge sub-mode: an edge on a
	// patch that is not the active one is picked, not the patch.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector || !GEditor)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid IdA;
	const FSurfPatch* A = MakeSheet(Mode, Group, IdA, ESurfSubMode::Object);
	if (!A)
	{
		AddError(TEXT("no first patch"));
		return false;
	}
	Mode->Inspector->NewPatch();
	const FGuid IdB = Mode->GetSelectedPatch().PatchId;
	const FSurfPatch* B = Group->FindPatch(IdB);
	A = Group->FindPatch(IdA);
	if (!B || !A || IdB == IdA)
	{
		AddError(TEXT("no second patch"));
		return false;
	}

	FBox Bounds(ForceInit);
	for (const FSurfControlPoint& Point : A->Points) { Bounds += Group->GetActorTransform().TransformPosition(Point.Pos); }
	for (const FSurfControlPoint& Point : B->Points) { Bounds += Group->GetActorTransform().TransformPosition(Point.Pos); }
	FCamera Camera = FCamera::Over(Group, *A);
	Camera.Location = Bounds.GetCenter() + FVector(0.0, 0.0, FMath::Max(100.0, Bounds.GetExtent().GetMax()) * 3.0);

	FSurfPatchRef RefA;
	RefA.Group = Group;
	RefA.PatchId = IdA;

	// --- Object sub-mode: G moves the patch ---
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(RefA);
	TestTrue(TEXT("G in object sub-mode starts a move"), Mode->HandleAction(ESurfAction::Translate, nullptr, nullptr));
	TestTrue(TEXT("and a move is running"), Mode->GetModal() == ESurfModal::Move);
	Mode->CancelModal();
	TestTrue(TEXT("cancel ends it"), Mode->GetModal() == ESurfModal::None);

	// --- A click on nothing deselects ---
	Mode->SelectPatch(RefA);
	TestTrue(TEXT("a patch is selected"), Mode->GetSelectedPatch().IsValid());
	Mode->HandleClickWith(Camera.ClickAt(FVector2D(3.0, 3.0)), nullptr);
	TestFalse(TEXT("a click on nothing deselects in object sub-mode"), Mode->GetSelectedPatch().IsValid());

	// --- Edge sub-mode: an edge on the other patch ---
	Mode->SelectPatch(RefA);
	Mode->SetSubMode(ESurfSubMode::Edge);
	const FVector EdgeOfB = (WorldOf(Group, *B, 1, 0) + WorldOf(Group, *B, 2, 0)) * 0.5;
	TestTrue(TEXT("a click on the other patch's edge is handled"), Mode->HandleClickWith(Camera.ClickOn(EdgeOfB), nullptr));
	TestEqual(TEXT("it picks the edge, two points"), Mode->GetPointSelection().Num(), 2);
	TestTrue(TEXT("and that patch is active"), Mode->GetSelectedPatch().PatchId == IdB);

	const FVector EdgeOfA = (WorldOf(Group, *A, 1, 0) + WorldOf(Group, *A, 2, 0)) * 0.5;
	Mode->HandleClickWith(Camera.ClickOn(EdgeOfA, false, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Shift adds an edge on the first patch"), Mode->GetPointSelection().Num(), 4);
	TestEqual(TEXT("both patches are selected"), Mode->GetSelectedPatches().Num(), 2);

	// --- Select all patches, then delete ---
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectAllPatches();
	TestEqual(TEXT("select all takes both patches"), Mode->GetSelectedPatches().Num(), 2);
	TestTrue(TEXT("delete takes them"), Mode->DeleteSelection());
	TestEqual(TEXT("no patches left"), Group->NumPatches(), 0);

	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfTouchSelectionTest, "SurfacingPro.Interaction.TouchSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfTouchSelectionTest::RunTest(const FString& Parameters)
{
	// A box or lasso takes what it touches: a face with no corner inside
	// it, a segment it crosses, a patch it lands on - not only what it
	// covers whole. Points are still those inside.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Face);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}
	const FCamera Camera = FCamera::Over(Group, *Patch);

	// The middle of face (1,1) on the screen, and a box of a few pixels
	// round it: no corner inside.
	const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
	FVector2D Pixel;
	Camera.Project(Centre, Pixel);

	auto Box = [&](const FVector2D& From, const FVector2D& To, ESurfSelectTool Tool = ESurfSelectTool::Box)
	{
		Mode->SetSelectTool(Tool);
		Mode->BeginSelectGesture();
		Mode->AppendSelectPath(From);
		if (Tool == ESurfSelectTool::Lasso)
		{
			Mode->AppendSelectPath(FVector2D(To.X, From.Y));
			Mode->AppendSelectPath(To);
			Mode->AppendSelectPath(FVector2D(From.X, To.Y));
		}
		else
		{
			Mode->AppendSelectPath(To);
		}
		return Mode->CommitSelectGestureWith(Camera.ClickAt(To), /*bSelect*/ true, /*bExtend*/ false);
	};

	// --- Faces ---------------------------------------------------------------------
	TestTrue(TEXT("a small box inside a face takes it"), Box(Pixel - FVector2D(4.0, 4.0), Pixel + FVector2D(4.0, 4.0)));
	TestEqual(TEXT("one face, with no corner inside the box"), Mode->GetQuadSelection().Num(), 1);
	if (Mode->GetQuadSelection().Num() == 1)
	{
		TestTrue(TEXT("face 1,1"), Mode->GetQuadSelection()[0].X == 1 && Mode->GetQuadSelection()[0].Y == 1);
	}

	// A thin box across a whole row of faces, through their middles.
	FVector2D Left;
	FVector2D Right;
	Camera.Project((WorldOf(Group, *Patch, 0, 1) + WorldOf(Group, *Patch, 0, 2)) * 0.5, Left);
	Camera.Project((WorldOf(Group, *Patch, 4, 1) + WorldOf(Group, *Patch, 4, 2)) * 0.5, Right);
	const FVector2D Along = (Right - Left).GetSafeNormal();
	const FVector2D Across(-Along.Y, Along.X);
	TestTrue(TEXT("a thin box across the row takes what it crosses"),
		Box(Left + Along * 3.0 - Across * 2.0, Right - Along * 3.0 + Across * 2.0));
	TestEqual(TEXT("the four faces of the row"), Mode->GetQuadSelection().Num(), 4);

	// A lasso the same way.
	TestTrue(TEXT("a lasso across the row takes what it crosses"),
		Box(Left + Along * 3.0 - Across * 2.0, Right - Along * 3.0 + Across * 2.0, ESurfSelectTool::Lasso));
	TestEqual(TEXT("the four faces again"), Mode->GetQuadSelection().Num(), 4);

	// --- Edges ---------------------------------------------------------------------
	Mode->SetSubMode(ESurfSubMode::Edge);
	Mode->ClearPointSelection();
	// Across the segment from (1,1) to (2,1), through its middle, no end inside.
	FVector2D SegMid;
	Camera.Project((WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 1)) * 0.5, SegMid);
	FVector2D SegEnd;
	Camera.Project(WorldOf(Group, *Patch, 2, 1), SegEnd);
	const FVector2D SegAlong = (SegEnd - SegMid).GetSafeNormal();
	const FVector2D SegAcross(-SegAlong.Y, SegAlong.X);
	TestTrue(TEXT("a thin box across a segment takes it"),
		Box(SegMid - SegAcross * 6.0 - SegAlong * 2.0, SegMid + SegAcross * 6.0 + SegAlong * 2.0));
	TestEqual(TEXT("both ends of the segment"), Mode->GetPointSelection().Num(), 2);

	// --- Points stay inside-only -----------------------------------------------------
	Mode->SetSubMode(ESurfSubMode::Vertex);
	Mode->ClearPointSelection();
	TestFalse(TEXT("a small box inside a face takes no point"), Box(Pixel - FVector2D(4.0, 4.0), Pixel + FVector2D(4.0, 4.0)));
	TestEqual(TEXT("none"), Mode->GetPointSelection().Num(), 0);

	// --- Patches -------------------------------------------------------------------
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->ClearSelection();
	Mode->ClearPointSelection();
	TestTrue(TEXT("a small box inside a face takes the patch"), Box(Pixel - FVector2D(4.0, 4.0), Pixel + FVector2D(4.0, 4.0)));
	TestEqual(TEXT("the patch"), Mode->GetSelectedPatches().Num(), 1);

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfGridPathTest, "SurfacingPro.Interaction.GridPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfGridPathTest::RunTest(const FString& Parameters)
{
	// Ctrl+Shift-click: the points between the last one clicked and this
	// one, along the grid - a run on a row, a staircase across.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}
	const FCamera Camera = FCamera::Over(Group, *Patch);

	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 0, 1)), nullptr);
	TestEqual(TEXT("one point clicked"), Mode->GetPointSelection().Num(), 1);

	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 4, 1), /*bCtrl*/ true, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Ctrl+Shift on the same row takes the run"), Mode->GetPointSelection().Num(), 5);

	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 4, 3), /*bCtrl*/ true, /*bShift*/ true), nullptr);
	TestEqual(TEXT("and on up the column"), Mode->GetPointSelection().Num(), 7);

	// Across the grid: a staircase, connected step by step.
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 0, 0)), nullptr);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 3, 3), /*bCtrl*/ true, /*bShift*/ true), nullptr);
	TestEqual(TEXT("a diagonal is a staircase of 7 points"), Mode->GetPointSelection().Num(), 7);
	bool bConnected = true;
	const TArray<FSurfPointRef> Path = Mode->GetPointSelection();
	for (int32 I = 1; I < Path.Num(); ++I)
	{
		if (FMath::Abs(Path[I].X - Path[I - 1].X) + FMath::Abs(Path[I].Y - Path[I - 1].Y) != 1)
		{
			bConnected = false;
		}
	}
	TestTrue(TEXT("every step is one edge"), bConnected);

	// Edge sub-mode: the segments along the path.
	Mode->SetSubMode(ESurfSubMode::Edge);
	Mode->ClearPointSelection();
	Mode->HandleClickWith(Camera.ClickOn((WorldOf(Group, *Patch, 0, 2) + WorldOf(Group, *Patch, 1, 2)) * 0.5), nullptr);
	TestEqual(TEXT("an edge clicked"), Mode->GetPointSelection().Num(), 2);
	Mode->HandleClickWith(Camera.ClickOn((WorldOf(Group, *Patch, 3, 2) + WorldOf(Group, *Patch, 4, 2)) * 0.5, /*bCtrl*/ true, /*bShift*/ true), nullptr);
	TestEqual(TEXT("Ctrl+Shift on an edge along the row takes the edges between"), Mode->GetPointSelection().Num(), 5);

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

// --- Our own gizmo ----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfOwnGizmoTest, "SurfacingPro.Interaction.OwnGizmo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfOwnGizmoTest::RunTest(const FString& Parameters)
{
	// The gizmo's pieces that stand without a viewport: its material and
	// tints, a handle as the editor sees it, a ring's turn, and the editor's
	// widget standing down while ours is up.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	UMaterialInterface* Base = SurfPreviewMaterial::GetGizmoMaterial();
	TestNotNull(TEXT("the gizmo material compiles"), Base);
	UMaterialInterface* Red = SurfPreviewMaterial::GetGizmoMaterial(SurfPreviewMaterial::ESurfGizmoTint::X, FLinearColor(1.0f, 0.0f, 0.0f, 0.4f));
	TestNotNull(TEXT("a tint of it is made"), Red);
	if (Base && Red)
	{
		const UMaterialInstanceDynamic* Instance = Cast<UMaterialInstanceDynamic>(Red);
		TestNotNull(TEXT("as an instance"), Instance);
		TestTrue(TEXT("of the gizmo material"), Instance && Instance->Parent == Base);
		TestTrue(TEXT("and the same instance next time, whatever the colour"),
			SurfPreviewMaterial::GetGizmoMaterial(SurfPreviewMaterial::ESurfGizmoTint::X, FLinearColor(0.0f, 1.0f, 0.0f, 1.0f)) == Red);
	}

	// A handle is the editor's own kind of hit proxy, carrying its mode.
	{
		HSurfGizmoAxis Handle(EAxisList::Y, UE::Widget::WM_Rotate);
		TestTrue(TEXT("a handle is a widget axis to the editor"), Handle.IsA(HWidgetAxis::StaticGetType()));
		TestTrue(TEXT("and one of ours"), Handle.IsA(HSurfGizmoAxis::StaticGetType()));
		TestTrue(TEXT("with its axis"), Handle.Axis == EAxisList::Y);
		TestTrue(TEXT("and its mode"), Handle.Mode == UE::Widget::WM_Rotate);
	}

	// A ring's turn: clockwise on the screen (pixel rows grow downwards) is
	// a positive sweep, and turns the selection clockwise to the viewer
	// whichever way the ring's axis points.
	TestTrue(TEXT("right to below is a quarter turn clockwise"),
		FMath::IsNearlyEqual(SurfGizmo::SweptRadians(FVector2D(1.0, 0.0), FVector2D(0.0, 1.0)), UE_DOUBLE_HALF_PI, 1.e-6));
	TestTrue(TEXT("below to right is the same turn back"),
		FMath::IsNearlyEqual(SurfGizmo::SweptRadians(FVector2D(0.0, 1.0), FVector2D(1.0, 0.0)), -UE_DOUBLE_HALF_PI, 1.e-6));
	TestTrue(TEXT("nothing from the pivot itself"), SurfGizmo::SweptRadians(FVector2D::ZeroVector, FVector2D(1.0, 0.0)) == 0.0);
	const FVector FromAbove = SurfGizmo::RingRotation(FVector::ZAxisVector, FVector::ZAxisVector, 90.0).RotateVector(FVector::XAxisVector);
	TestTrue(TEXT("seen from above, a quarter turn takes X to Y"), FromAbove.Equals(FVector::YAxisVector, 1.e-4));
	const FVector FromBelow = SurfGizmo::RingRotation(FVector::ZAxisVector, -FVector::ZAxisVector, 90.0).RotateVector(FVector::XAxisVector);
	TestTrue(TEXT("seen from below, the same sweep takes X to -Y"), FromBelow.Equals(-FVector::YAxisVector, 1.e-4));

	/**
	 * The ball (S130a). A camera looking along +X, with +Y to the right and
	 * +Z up - so the front of the ball, under the middle of the screen,
	 * faces -X, back at the camera.
	 */
	{
		const FVector Right = FVector::YAxisVector;
		const FVector Up = FVector::ZAxisVector;
		const FVector Forward = FVector::XAxisVector;
		const double Radius = 100.0;

		const FVector Middle = SurfGizmo::BallPoint(FVector2D::ZeroVector, Radius, Right, Up, Forward);
		TestTrue(TEXT("the middle of the ball faces the camera"), Middle.Equals(-Forward, 1.e-4));

		const FVector Rim = SurfGizmo::BallPoint(FVector2D(Radius, 0.0), Radius, Right, Up, Forward);
		TestTrue(TEXT("its right edge is the way right"), Rim.Equals(Right, 1.e-4));

		const FVector Past = SurfGizmo::BallPoint(FVector2D(Radius * 3.0, 0.0), Radius, Right, Up, Forward);
		TestTrue(TEXT("and past the edge stays on the rim"), Past.Equals(Right, 1.e-4));

		const FVector Above = SurfGizmo::BallPoint(FVector2D(0.0, -Radius), Radius, Right, Up, Forward);
		TestTrue(TEXT("up the screen is up, though pixel rows go down"), Above.Equals(Up, 1.e-4));

		// The whole of what a trackball promises: the point grabbed ends up
		// under the cursor. Everything else about it is this one property.
		const FRotator Turn = SurfGizmo::BallRotation(Middle, Rim);
		TestTrue(TEXT("a drag from the middle to the edge carries the point grabbed to the cursor"),
			Turn.RotateVector(Middle).Equals(Rim, 1.e-4));
		TestTrue(TEXT("and it is a quarter turn"), FMath::IsNearlyEqual(Turn.Quaternion().GetAngle(), UE_DOUBLE_HALF_PI, 1.e-4));

		// Half way out is thirty degrees, not forty-five: the cursor is
		// walking over a sphere, not along a ruler.
		const FVector Half = SurfGizmo::BallPoint(FVector2D(Radius * 0.5, 0.0), Radius, Right, Up, Forward);
		TestTrue(TEXT("half way out is thirty degrees"),
			FMath::IsNearlyEqual(FMath::RadiansToDegrees(SurfGizmo::BallRotation(Middle, Half).Quaternion().GetAngle()), 30.0, 1.e-3));

		TestTrue(TEXT("and standing still turns nothing"),
			SurfGizmo::BallRotation(Middle, Middle).IsNearlyZero());

		// It turns about whatever axis the drag asks for, which is the
		// point of it: sideways turns about one, upwards about another.
		const FVector Sideways = SurfGizmo::BallRotation(Middle, Rim).Quaternion().GetRotationAxis();
		const FVector Upwards = SurfGizmo::BallRotation(Middle, Above).Quaternion().GetRotationAxis();
		TestTrue(TEXT("sideways and upwards are not the same axis"),
			FMath::Abs(FVector::DotProduct(Sideways, Upwards)) < 0.01);
	}

	// With something selected, the editor's widget stands down while ours
	// is up, stays in use for its drag maths, and is back when ours is off.
	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}
	const FCamera Camera = FCamera::Over(Group, *Patch);
	Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr);
	TestEqual(TEXT("a point selected"), Mode->GetPointSelection().Num(), 1);

	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	const bool bWasShown = Settings->bShowGizmo;
	const bool bWasOwn = Settings->bOwnGizmo;
	Settings->bShowGizmo = true;
	Settings->bOwnGizmo = true;
	TestFalse(TEXT("the editor's widget is not drawn with ours on"), Mode->ShouldDrawWidget());
	TestTrue(TEXT("but stays in use, for its drag maths"), Mode->UsesTransformWidget());
	Settings->bOwnGizmo = false;
	TestTrue(TEXT("the editor's widget is drawn with ours off"), Mode->ShouldDrawWidget());
	Settings->bShowGizmo = false;
	TestFalse(TEXT("and neither with the gizmo off altogether"), Mode->ShouldDrawWidget());
	Settings->bShowGizmo = bWasShown;
	Settings->bOwnGizmo = bWasOwn;

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

// --- Flat views --------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfFlatViewTest, "SurfacingPro.Interaction.FlatView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfFlatViewTest::RunTest(const FString& Parameters)
{
	// The four-pane layout's top, front and side panes are flat views: no
	// perspective, an eye nowhere in particular. Every pick and gesture has
	// to work through one as it does through a camera.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode"));
		return false;
	}

	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Vertex);
	if (!Patch)
	{
		AddError(TEXT("no patch"));
		return false;
	}

	const FCamera Cameras[2] = { FCamera::Above(Group, *Patch), FCamera::Oblique(Group, *Patch) };
	const TCHAR* Names[2] = { TEXT("above"), TEXT("oblique") };

	for (int32 C = 0; C < 2; ++C)
	{
		const FCamera& Camera = Cameras[C];
		const FString Where = Names[C];

		// The projection agrees with itself: the ray through a point's pixel passes through the point.
		{
			const FVector Point = WorldOf(Group, *Patch, 2, 1);
			const FSurfPickContext Ctx = Camera.ClickOn(Point);
			const FVector Nearest = Ctx.Origin + Ctx.Direction * FVector::DotProduct(Point - Ctx.Origin, Ctx.Direction);
			TestTrue(*FString::Printf(TEXT("%s: the ray through a pixel passes through its point"), *Where), FVector::Dist(Nearest, Point) < 1.0);
		}

		// A point.
		Mode->SetSubMode(ESurfSubMode::Vertex);
		Mode->ClearPointSelection();
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 1, 1)), nullptr);
		TestEqual(*FString::Printf(TEXT("%s: a click on a point takes it"), *Where), Mode->GetPointSelection().Num(), 1);
		if (Mode->GetPointSelection().Num() == 1)
		{
			TestTrue(*FString::Printf(TEXT("%s: the point clicked"), *Where), Mode->GetPointSelection()[0].X == 1 && Mode->GetPointSelection()[0].Y == 1);
		}

		// A click forty pixels from it does not take it: a flat view has no
		// angle to measure a miss in, and the far-off eye made every point a hit.
		{
			FVector2D Pixel;
			Camera.Project(WorldOf(Group, *Patch, 1, 1), Pixel);
			Mode->HandleClickWith(Camera.ClickAt(Pixel + FVector2D(40.0, 0.0)), nullptr);
			const TArray<FSurfPointRef>& Sel = Mode->GetPointSelection();
			TestFalse(*FString::Printf(TEXT("%s: a click forty pixels off a point does not take it"), *Where),
				Sel.Num() == 1 && Sel[0].X == 1 && Sel[0].Y == 1);
		}

		// An edge.
		Mode->SetSubMode(ESurfSubMode::Edge);
		Mode->ClearPointSelection();
		Mode->HandleClickWith(Camera.ClickOn((WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 1)) * 0.5), nullptr);
		TestEqual(*FString::Printf(TEXT("%s: a click on a segment takes its two points"), *Where), Mode->GetPointSelection().Num(), 2);

		// A face.
		Mode->SetSubMode(ESurfSubMode::Face);
		Mode->ClearPointSelection();
		const FVector Centre = (WorldOf(Group, *Patch, 1, 1) + WorldOf(Group, *Patch, 2, 2)) * 0.5;
		Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr);
		TestEqual(*FString::Printf(TEXT("%s: a click on a face takes it"), *Where), Mode->GetQuadSelection().Num(), 1);
		if (Mode->GetQuadSelection().Num() == 1)
		{
			TestTrue(*FString::Printf(TEXT("%s: face 1,1"), *Where), Mode->GetQuadSelection()[0].X == 1 && Mode->GetQuadSelection()[0].Y == 1);
		}

		// A box across a row of faces takes what it crosses.
		{
			FVector2D Left;
			FVector2D Right;
			Camera.Project((WorldOf(Group, *Patch, 0, 1) + WorldOf(Group, *Patch, 0, 2)) * 0.5, Left);
			Camera.Project((WorldOf(Group, *Patch, 4, 1) + WorldOf(Group, *Patch, 4, 2)) * 0.5, Right);
			const FVector2D Along = (Right - Left).GetSafeNormal();
			const FVector2D Across(-Along.Y, Along.X);
			const FVector2D From = Left + Along * 3.0 - Across * 2.0;
			const FVector2D To = Right - Along * 3.0 + Across * 2.0;
			Mode->SetSelectTool(ESurfSelectTool::Box);
			Mode->BeginSelectGesture();
			Mode->AppendSelectPath(From);
			Mode->AppendSelectPath(To);
			TestTrue(*FString::Printf(TEXT("%s: a thin box across the row takes what it crosses"), *Where),
				Mode->CommitSelectGestureWith(Camera.ClickAt(To), /*bSelect*/ true, /*bExtend*/ false));
			TestEqual(*FString::Printf(TEXT("%s: the four faces of the row"), *Where), Mode->GetQuadSelection().Num(), 4);
		}

		// The patch.
		Mode->SetSubMode(ESurfSubMode::Object);
		Mode->ClearSelection();
		Mode->ClearPointSelection();
		Mode->SelectPatch(FSurfPatchRef());
		Mode->HandleClickWith(Camera.ClickOn(Centre), nullptr);
		TestTrue(*FString::Printf(TEXT("%s: a click on the surface selects the patch"), *Where), Mode->GetSelectedPatch().PatchId == PatchId);

		// A path along the grid.
		Mode->SetSubMode(ESurfSubMode::Vertex);
		Mode->ClearPointSelection();
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 0, 1)), nullptr);
		Mode->HandleClickWith(Camera.ClickOn(WorldOf(Group, *Patch, 4, 1), /*bCtrl*/ true, /*bShift*/ true), nullptr);
		TestEqual(*FString::Printf(TEXT("%s: Ctrl+Shift-click takes the run between"), *Where), Mode->GetPointSelection().Num(), 5);
	}

	// The flat views the number keys give, from the way they look.
	TestTrue(TEXT("down is the top view"), FSurfCamera::FlatViewFor(-FVector::ZAxisVector) == LVT_OrthoXY);
	TestTrue(TEXT("up is the bottom view"), FSurfCamera::FlatViewFor(FVector::ZAxisVector) == LVT_OrthoNegativeXY);
	TestTrue(TEXT("along +Y is the view from the front"), FSurfCamera::FlatViewFor(FVector::YAxisVector) == LVT_OrthoNegativeXZ);
	TestTrue(TEXT("along -X is the view from the right"), FSurfCamera::FlatViewFor(-FVector::XAxisVector) == LVT_OrthoNegativeYZ);

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfCopySurfaceClickTest, "SurfacingPro.Interaction.CopySurfaceClick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfCopySurfaceClickTest::RunTest(const FString& Parameters)
{
	// The middle-click copies a surface onto the selection, from a brush
	// face or a patch, onto patches and faces alike (S68d): material,
	// projection, scale, turn. A patch keeps its layer. With nothing
	// selected, the line says what to do.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector || !GEditor)
	{
		AddError(TEXT("could not set up mode and inspector"));
		return false;
	}

	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Mode->SelectPatch(FSurfPatchRef());
	Mode->Inspector->NewBox();
	const FSurfFaceRef Top = Mode->GetActiveFace();
	ASurfGroup* Group = Top.Group.Get();
	if (!Group || !Top.IsValid())
	{
		AddError(TEXT("a box with its top selected was expected"));
		return false;
	}

	UMaterial* Marble = NewObject<UMaterial>(GetTransientPackage());
	UMaterial* Stone = NewObject<UMaterial>(GetTransientPackage());
	FSurfFaceRef Side = Top;
	FVector TopCentre = FVector::ZeroVector;
	{
		FSurfBrush* Brush = Group->ModifyBrush(Top.BrushId);
		FSurfFace& Face = Brush->Faces[Top.FaceIndex];
		Face.Material = Marble;
		Face.UV.Scale = FVector2D(0.5, 0.5);
		SurfProjection::SetRotationDegrees(Face.UV, FVector(Face.Plane.X, Face.Plane.Y, Face.Plane.Z), 30.0);
		for (int32 I = 0; I < Brush->Faces.Num(); ++I)
		{
			if (Brush->Faces[I].Plane.X > 0.9) { Side.FaceIndex = I; }
		}
		FSurfHull Hull;
		FBox Bounds(ForceInit);
		if (SurfBrushMath::DeriveHull(*Brush, Hull)) { for (const FVector& V : Hull.Vertices) { Bounds += V; } }
		TopCentre = Group->GetActorTransform().TransformPosition(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z));
	}

	// A patch, selected; the face selection dropped so the patch is the target.
	Mode->Inspector->SheetWidth = 3;
	Mode->Inspector->SheetHeight = 3;
	Mode->Inspector->NewPatch();
	const FSurfPatchRef PatchRef = Mode->GetSelectedPatch();
	Mode->ClearSelection();
	if (FSurfPatch* Patch = Group->ModifyPatch(PatchRef.PatchId)) { Patch->StackLevel = 2; }
	{
		// The patch lands clear of the box (S68d): a click on it from above
		// must reach it, not the box's top through it.
		const FSurfPatch* Placed = Group->FindPatch(PatchRef.PatchId);
		FBox PatchBounds(ForceInit);
		for (const FSurfControlPoint& Point : Placed->Points) { PatchBounds += Point.Pos; }
		FSurfHull Hull;
		FBox BoxBounds(ForceInit);
		if (const FSurfBrush* Brush = Group->FindBrush(Top.BrushId)) { if (SurfBrushMath::DeriveHull(*Brush, Hull)) { for (const FVector& V : Hull.Vertices) { BoxBounds += V; } } }
		TestFalse(TEXT("a new patch lands clear of the box"), PatchBounds.Intersect(BoxBounds));
	}

	// Middle-click the box's top from straight above it.
	FCamera OverBox;
	OverBox.World = Group->GetWorld();
	OverBox.Location = TopCentre + FVector(0.0, 0.0, 600.0);
	FSurfPickContext Click = OverBox.ClickOn(TopCentre);
	Click.Key = EKeys::MiddleMouseButton;
	TestTrue(TEXT("the middle-click is taken"), Mode->HandleClickWith(Click, nullptr));
	const FSurfPatch* P = Group->FindPatch(PatchRef.PatchId);
	TestTrue(TEXT("the patch wears the face's material"), P && P->Material == Marble);
	TestTrue(TEXT("at the face's scale"), P && P->Scale.Equals(FVector2D(0.5, 0.5), 1.e-6));
	TestTrue(TEXT("with the face's projection"), P && P->Mapping == ESurfPatchMapping::Planar);
	TestTrue(TEXT("and keeps its layer"), P && P->StackLevel == 2);
	TestTrue(TEXT("the line says so"), Mode->Inspector->LastResult.Contains(TEXT("onto 1 patch")));

	// The other way: the patch's surface onto a selected face.
	if (FSurfPatch* Patch = Group->ModifyPatch(PatchRef.PatchId))
	{
		Patch->Material = Stone;
		SurfPatchSurfacing::SetScale(*Patch, FVector2D(0.25, 0.25));
		Patch->UVRotation = 15.0;
		SurfPatchSurfacing::Reapply(*Patch);
	}
	Mode->SelectPatch(FSurfPatchRef());
	Mode->ClearPointSelection();
	Mode->SelectFace(Side);
	P = Group->FindPatch(PatchRef.PatchId);
	FCamera OverPatch = FCamera::Over(Group, *P);
	FSurfPickContext Click2 = OverPatch.ClickOn(WorldOf(Group, *P, 1, 1));
	Click2.Key = EKeys::MiddleMouseButton;
	TestTrue(TEXT("the middle-click on a patch is taken"), Mode->HandleClickWith(Click2, nullptr));
	const FSurfBrush* Brush = Group->FindBrush(Side.BrushId);
	const FSurfFace* S = Brush && Brush->Faces.IsValidIndex(Side.FaceIndex) ? &Brush->Faces[Side.FaceIndex] : nullptr;
	TestTrue(TEXT("the face wears the patch's material"), S && S->Material == Stone);
	TestTrue(TEXT("at the patch's scale"), S && S->UV.Scale.Equals(FVector2D(0.25, 0.25), 1.e-6));
	TestTrue(TEXT("turned as the patch is"), S && FMath::IsNearlyEqual(
		SurfProjection::GetRotationDegrees(S->UV, FVector(S->Plane.X, S->Plane.Y, S->Plane.Z)), 15.0, 1.e-3));
	TestTrue(TEXT("the line says so"), Mode->Inspector->LastResult.Contains(TEXT("onto 1 face")));

	// Nothing selected: nothing changes, the line says what to do.
	Mode->ClearSelection();
	Mode->SelectPatch(FSurfPatchRef());
	TestTrue(TEXT("taken, with nothing to give it to"), Mode->HandleClickWith(Click, nullptr));
	TestTrue(TEXT("and it says to select first"), Mode->Inspector->LastResult.Contains(TEXT("select what should take")));

	Group->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfPrefabClickTest, "SurfacingPro.Interaction.PrefabClick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfPrefabClickTest::RunTest(const FString& Parameters)
{
	// The prefab made here is saved to disk (S71c); off it again after.
	const FString PrefabDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/SurfacingPro/Prefabs/"));
	TSet<FString> FilesBefore;
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(PrefabDir / TEXT("*.uasset")), true, false);
		for (const FString& File : Files) { FilesBefore.Add(File); }
	}
	ON_SCOPE_EXIT
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(PrefabDir / TEXT("*.uasset")), true, false);
		for (const FString& File : Files)
		{
			if (!FilesBefore.Contains(File)) { IFileManager::Get().Delete(*(PrefabDir / File), false, true, true); }
		}
	};
	// "just made a prefab. i cannot select it anymore": a click on the
	// instance, through the click path, takes it into the editor's
	// selection and the line says how to edit it; a double-click enters it.
	USurfEdMode* Mode = EnterMode();
	if (!Mode || !Mode->Inspector || !GEditor)
	{
		AddError(TEXT("could not set up mode and inspector"));
		return false;
	}
	ASurfGroup* Group = nullptr;
	FGuid PatchId;
	const FSurfPatch* Patch = MakeSheet(Mode, Group, PatchId, ESurfSubMode::Object);
	if (!Patch || !Group)
	{
		AddError(TEXT("a patch was expected"));
		return false;
	}
	// Somewhere to click once the patch has become a prefab: the same spot.
	const FVector Middle = WorldOf(Group, *Patch, 2, 2);
	FCamera Camera = FCamera::Over(Group, *Patch);

	TestTrue(TEXT("the patch becomes a prefab"), Mode->MakePrefabFromSelection(TEXT("Clickable")));
	ASurfPrefabInstance* Instance = nullptr;
	for (TActorIterator<ASurfPrefabInstance> It(Group->GetWorld()); It; ++It)
	{
		if (It->Prefab && It->Prefab->GetName().StartsWith(TEXT("Clickable"))) { Instance = *It; }
	}
	if (!Instance)
	{
		AddError(TEXT("an instance should stand where the patch was"));
		return false;
	}
	TestTrue(TEXT("the instance draws"), Instance->TriangleCount > 0);

	// Drop every selection, then click the instance from above.
	Mode->ClearSelection();
	Mode->ClearPointSelection();
	Mode->SelectPatch(FSurfPatchRef());
	GEditor->SelectNone(false, true);
	TestTrue(TEXT("the click is taken"), Mode->HandleClickWith(Camera.ClickOn(Middle), nullptr));
	TestTrue(TEXT("and the instance is the editor's selection"), Instance->IsSelected());
	TestTrue(TEXT("and the line says how to edit it"), Mode->Inspector->LastResult.Contains(TEXT("Open")));

	// A double-click enters it.
	FSurfPickContext Twice = Camera.ClickOn(Middle);
	Twice.bDoubleClick = true;
	TestTrue(TEXT("the double-click is taken"), Mode->HandleClickWith(Twice, nullptr));
	TestTrue(TEXT("and the prefab is open for editing"), Mode->IsInsidePrefab());
	Mode->ExitPrefab(false);
	TestFalse(TEXT("dropped again"), Mode->IsInsidePrefab());

	Instance->Destroy();
	Group->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
