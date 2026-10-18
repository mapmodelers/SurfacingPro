// Copyright MapModelers. All Rights Reserved.
//
// Materials on the preview (M-A): every patch and brush face wears a
// material of its own, rendered through one slot per distinct material,
// with the checker under anything that has none. And the one verb that
// puts a material on the selection.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfTypes.h"
#include "SurfMaterialInfo.h"
#include "SurfMaterialSet.h"
#include "SurfPatchMath.h"
#include "SurfPatchPrimitives.h"
#include "SurfPatchSurfacing.h"
#include "SurfProjection.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/World.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::Geometry;

namespace SurfMaterialTestUtil
{
	static ASurfGroup* SpawnGroup()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return nullptr;
		}

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<ASurfGroup>(ASurfGroup::StaticClass(), FTransform::Identity, Params);
	}

	/** A flat three-by-three sheet, a metre each way, at Origin. */
	static FGuid AddSheet(ASurfGroup& Group, const FVector& Origin)
	{
		FSurfPatch Patch = SurfPatchMath::MakeFlatPatch(
			ESurfPatchType::Sheet, 3, 3, Origin, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
		Patch.EnsureId();
		SurfPatchSurfacing::Reapply(Patch);
		const FGuid Id = Patch.Id;
		Group.AddPatch(MoveTemp(Patch));
		return Id;
	}

	static FSurfPatchRef RefTo(ASurfGroup* Group, const FGuid& Id)
	{
		FSurfPatchRef Ref;
		Ref.Group = Group;
		Ref.PatchId = Id;
		return Ref;
	}

	static FSurfFaceRef FaceOf(ASurfGroup* Group, int32 FaceIndex)
	{
		FSurfFaceRef Ref;
		Group->ForEachBrush([&Ref, Group, FaceIndex](const FSurfBrush& Brush)
		{
			if (!Ref.BrushId.IsValid())
			{
				Ref.Group = Group;
				Ref.BrushId = Brush.Id;
				Ref.FaceIndex = FaceIndex;
			}
		});
		return Ref;
	}

	/** A material that is not the checker and not the engine default: a dynamic instance of the default, made per test. */
	static UMaterialInterface* MakeMaterial(const TCHAR* Name)
	{
		return UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage(), FName(Name));
	}

	static USurfEdMode* EnterMode()
	{
		// His ini leaks in: the view overrides off, or every slot wears the
		// clay or the paint view (S242a).
		if (USurfSettings* Settings = GetMutableDefault<USurfSettings>())
		{
			Settings->bMatcap = false;
			Settings->bChannelView = false;
		}
		GLevelEditorModeTools().ActivateMode(USurfEdMode::EM_SurfacingPro);
		if (USurfEdMode* Mode = Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro)))
		{
			Mode->RefreshPaintView();
			return Mode;
		}
		return nullptr;
	}

	static void LeaveMode()
	{
		GLevelEditorModeTools().DeactivateMode(USurfEdMode::EM_SurfacingPro);
	}

	/** Slot counts over the preview's triangles: per triangle group (patches) and for the brushes (group 0). */
	static TMap<int32, TMap<int32, int32>> SlotCountsByGroup(const ASurfGroup& Group)
	{
		TMap<int32, TMap<int32, int32>> Out;
		if (!Group.Preview)
		{
			return Out;
		}

		Group.Preview->ProcessMesh([&Out](const FDynamicMesh3& Mesh)
		{
			const FDynamicMeshMaterialAttribute* MaterialIDs =
				Mesh.HasAttributes() && Mesh.Attributes()->HasMaterialID() ? Mesh.Attributes()->GetMaterialID() : nullptr;

			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				int32 Slot = -1;
				if (MaterialIDs)
				{
					MaterialIDs->GetValue(Tid, &Slot);
				}
				Out.FindOrAdd(Mesh.GetTriangleGroup(Tid)).FindOrAdd(Slot)++;
			}
		});
		return Out;
	}

	static int32 OnlySlotOf(const TMap<int32, int32>& Counts)
	{
		return Counts.Num() == 1 ? Counts.CreateConstIterator().Key() : -2;
	}
}

// --- The set --------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialSetTest, "SurfacingPro.Materials.Set",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialSetTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Fallback = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInterface* A = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_SetA"));
	UMaterialInterface* B = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_SetB"));

	FSurfMaterialSet Set;
	Set.Reset(Fallback);

	TestEqual(TEXT("the fallback is slot 0"), Set.Num(), 1);
	TestEqual(TEXT("null goes to slot 0"), Set.SlotFor(nullptr), 0);
	TestEqual(TEXT("the first material takes slot 1"), Set.SlotFor(A), 1);
	TestEqual(TEXT("the second takes slot 2"), Set.SlotFor(B), 2);
	TestEqual(TEXT("the first again is still slot 1"), Set.SlotFor(A), 1);
	TestEqual(TEXT("three slots in all"), Set.Num(), 3);
	TestTrue(TEXT("slot 2 is the second material"), Set.Slots[2] == B);

	Set.Reset(nullptr);
	TestEqual(TEXT("reset starts over"), Set.Num(), 1);
	TestEqual(TEXT("and the order is by first use again"), Set.SlotFor(B), 1);

	return true;
}

// --- Slots on the preview -------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialSlotsTest, "SurfacingPro.Materials.Slots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialSlotsTest::RunTest(const FString& Parameters)
{
	ASurfGroup* Group = SurfMaterialTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		return false;
	}

	UMaterialInterface* Brick = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_Brick"));
	UMaterialInterface* Stone = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_Stone"));

	// A box with two faces in brick, a patch in stone, a patch with nothing.
	FSurfBrush& Box = Group->AppendBox(FBox(FVector(0.0), FVector(200.0)));
	const FGuid BoxId = Box.Id;
	Box.Faces[0].Material = Brick;
	Box.Faces[1].Material = Brick;

	const FGuid StoneId = SurfMaterialTestUtil::AddSheet(*Group, FVector(500.0, 0.0, 0.0));
	const FGuid BareId = SurfMaterialTestUtil::AddSheet(*Group, FVector(1000.0, 0.0, 0.0));
	if (FSurfPatch* Stone_ = Group->ModifyPatch(StoneId))
	{
		Stone_->Material = Stone;
	}

	Group->RebuildPreview();

	UDynamicMeshComponent* Preview = Group->Preview;
	if (!Preview)
	{
		AddError(TEXT("the group has no preview component"));
		Group->Destroy();
		return false;
	}

	TestEqual(TEXT("one slot per distinct material, plus the fallback"), Preview->GetNumMaterials(), 3);
	TestEqual(TEXT("the group remembers its slots"), Group->PreviewMaterials.Num(), 3);
	TestTrue(TEXT("slot 0 is the group's effective material"), Preview->GetMaterial(0) == Group->GetEffectiveMaterial());
	TestTrue(TEXT("brushes are built first, so brick is slot 1"), Preview->GetMaterial(1) == Brick);
	TestTrue(TEXT("stone is slot 2"), Preview->GetMaterial(2) == Stone);

	const TMap<int32, TMap<int32, int32>> Counts = SurfMaterialTestUtil::SlotCountsByGroup(*Group);

	const int32* StoneGroup = Group->PatchTriangleGroups.Find(StoneId);
	const int32* BareGroup = Group->PatchTriangleGroups.Find(BareId);
	TestNotNull(TEXT("the stone patch has a triangle group"), StoneGroup);
	TestNotNull(TEXT("the bare patch has a triangle group"), BareGroup);

	if (StoneGroup && Counts.Contains(*StoneGroup))
	{
		TestEqual(TEXT("every stone triangle is in the stone slot"), SurfMaterialTestUtil::OnlySlotOf(Counts[*StoneGroup]), 2);
	}
	if (BareGroup && Counts.Contains(*BareGroup))
	{
		TestEqual(TEXT("every bare triangle is in the fallback slot"), SurfMaterialTestUtil::OnlySlotOf(Counts[*BareGroup]), 0);
	}
	// A brush's triangles carry its own group since S151a (they were all
	// group 0 before, which told the bake nothing apart) - asked for by the
	// brush's id, as the patches' are. Edited by the modelling session with
	// the Materials session on hold; the assertion is unchanged.
	const int32* BoxGroup = Group->BrushTriangleGroups.Find(BoxId);
	if (const TMap<int32, int32>* BrushCounts = BoxGroup ? Counts.Find(*BoxGroup) : nullptr)
	{
		// Two quads in brick: four triangles; the other four faces: eight.
		TestEqual(TEXT("the two brick faces are four triangles in slot 1"), BrushCounts->FindRef(1), 4);
		TestEqual(TEXT("the other faces are in the fallback slot"), BrushCounts->FindRef(0), 8);
		TestFalse(TEXT("no brush triangle is in the stone slot"), BrushCounts->Contains(2));
	}
	else
	{
		AddError(TEXT("the brush triangles are missing from the preview"));
	}

	// Taking a material away drops its slot at the next rebuild.
	if (FSurfPatch* Stone_ = Group->ModifyPatch(StoneId))
	{
		Stone_->Material = nullptr;
	}
	Group->RebuildPreview();
	TestEqual(TEXT("a material no longer worn loses its slot"), Preview->GetNumMaterials(), 2);

	Group->Destroy();
	return true;
}

// --- The fallback ---------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialFallbackTest, "SurfacingPro.Materials.Fallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialFallbackTest::RunTest(const FString& Parameters)
{
	ASurfGroup* Group = SurfMaterialTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		return false;
	}

	UMaterialInterface* Brick = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_FallbackBrick"));
	UMaterialInterface* GroupWide = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_GroupWide"));

	const FGuid BrickId = SurfMaterialTestUtil::AddSheet(*Group, FVector::ZeroVector);
	SurfMaterialTestUtil::AddSheet(*Group, FVector(500.0, 0.0, 0.0));
	if (FSurfPatch* Patch = Group->ModifyPatch(BrickId))
	{
		Patch->Material = Brick;
	}
	Group->RebuildPreview();

	UDynamicMeshComponent* Preview = Group->Preview;
	if (!Preview)
	{
		AddError(TEXT("the group has no preview component"));
		Group->Destroy();
		return false;
	}

	TestEqual(TEXT("two slots: the fallback and brick"), Preview->GetNumMaterials(), 2);

	// The group's own material is the fallback, and changing it reaches the
	// unassigned patch without a rebuild and without touching brick.
	Group->PreviewMaterial = GroupWide;
	Group->RefreshPreviewMaterial();
	TestTrue(TEXT("the group material is slot 0"), Preview->GetMaterial(0) == GroupWide);
	TestTrue(TEXT("brick is untouched by a refresh"), Preview->GetMaterial(1) == Brick);
	TestEqual(TEXT("a refresh keeps the slot count"), Preview->GetNumMaterials(), 2);

	Group->PreviewMaterial = nullptr;
	Group->RefreshPreviewMaterial();
	TestTrue(TEXT("with no group material, slot 0 is the editor's default"),
		Preview->GetMaterial(0) == Group->GetEffectiveMaterial());
	TestTrue(TEXT("and it is never the assigned material"), Preview->GetMaterial(0) != Brick);

	Group->Destroy();
	return true;
}

// --- The verb -------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialAssignTest, "SurfacingPro.Materials.Assign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialAssignTest::RunTest(const FString& Parameters)
{
	SurfMaterialTestUtil::LeaveMode();
	USurfEdMode* Mode = SurfMaterialTestUtil::EnterMode();
	if (!Mode || !Mode->Inspector)
	{
		AddError(TEXT("could not enter the mode with an inspector"));
		return false;
	}

	UMaterialInterface* Brick = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_AssignBrick"));

	// Nothing selected: a refusal in the one voice, nothing changed.
	TestFalse(TEXT("assign declines with nothing selected"), Mode->Inspector->SetMaterialOnSelection(Brick));
	TestTrue(TEXT("the refusal names the verb"), Mode->Inspector->LastResult.StartsWith(TEXT("material:")));
	TestTrue(TEXT("and reads as a refusal"), Mode->Inspector->LastResultWasRefusal());

	ASurfGroup* Group = SurfMaterialTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		SurfMaterialTestUtil::LeaveMode();
		return false;
	}

	const FGuid FirstId = SurfMaterialTestUtil::AddSheet(*Group, FVector::ZeroVector);
	const FGuid SecondId = SurfMaterialTestUtil::AddSheet(*Group, FVector(500.0, 0.0, 0.0));
	Group->AppendBox(FBox(FVector(1000.0, 0.0, 0.0), FVector(1200.0, 200.0, 200.0)));
	Group->RebuildPreview();

	// Object sub-mode: the patches picked as wholes.
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialTestUtil::RefTo(Group, FirstId));
	Mode->AddPatchToSelection(SurfMaterialTestUtil::RefTo(Group, SecondId), /*bToggle*/ false);
	TestEqual(TEXT("two patches are picked"), Mode->GetSelectedPatches().Num(), 2);

	TestTrue(TEXT("assign runs on the picked patches"), Mode->Inspector->SetMaterialOnSelection(Brick));
	TestTrue(TEXT("the result speaks in the past tense"), Mode->Inspector->LastResult.StartsWith(TEXT("assigned")));
	TestFalse(TEXT("and is not a refusal"), Mode->Inspector->LastResultWasRefusal());
	TestEqual(TEXT("the selection is kept (D16.1)"), Mode->GetSelectedPatches().Num(), 2);

	const FSurfPatch* First = Group->FindPatch(FirstId);
	const FSurfPatch* Second = Group->FindPatch(SecondId);
	TestTrue(TEXT("the first patch wears brick"), First && First->Material == Brick);
	TestTrue(TEXT("the second patch wears brick"), Second && Second->Material == Brick);
	TestEqual(TEXT("the preview has the fallback and brick"), Group->Preview ? Group->Preview->GetNumMaterials() : -1, 2);

	// Face sub-mode on a brush: the selected faces, and not the active patch.
	Mode->SetSubMode(ESurfSubMode::Face);
	Mode->SelectFace(SurfMaterialTestUtil::FaceOf(Group, 2));
	Mode->SelectFace(SurfMaterialTestUtil::FaceOf(Group, 3), /*bAddToSelection*/ true);
	UMaterialInterface* Stone = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_AssignStone"));

	TestTrue(TEXT("assign runs on the selected faces"), Mode->Inspector->SetMaterialOnSelection(Stone));
	TestEqual(TEXT("the faces stay selected"), Mode->GetSelection().Num(), 2);

	int32 StoneFaces = 0;
	Group->ForEachBrush([&StoneFaces, Stone](const FSurfBrush& Brush)
	{
		for (const FSurfFace& Face : Brush.Faces)
		{
			if (Face.Material == Stone) { ++StoneFaces; }
		}
	});
	TestEqual(TEXT("two faces wear stone"), StoneFaces, 2);
	First = Group->FindPatch(FirstId);
	TestTrue(TEXT("the patches still wear brick"), First && First->Material == Brick);
	TestEqual(TEXT("three slots now"), Group->Preview ? Group->Preview->GetNumMaterials() : -1, 3);

	// Clearing: an empty material puts the patch back on the group's fallback.
	Mode->ClearSelection();
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialTestUtil::RefTo(Group, FirstId));
	TestTrue(TEXT("assign accepts an empty material"), Mode->Inspector->SetMaterialOnSelection(nullptr));
	TestTrue(TEXT("the result says cleared"), Mode->Inspector->LastResult.StartsWith(TEXT("cleared")));
	First = Group->FindPatch(FirstId);
	TestTrue(TEXT("the first patch wears nothing"), First && First->Material == nullptr);

	Group->Destroy();
	SurfMaterialTestUtil::LeaveMode();
	return true;
}

// --- A material's own scale -----------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialDefaultScaleTest, "SurfacingPro.Materials.DefaultScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialDefaultScaleTest::RunTest(const FString& Parameters)
{
	SurfMaterialTestUtil::LeaveMode();
	USurfEdMode* Mode = SurfMaterialTestUtil::EnterMode();
	ASurfGroup* Group = SurfMaterialTestUtil::SpawnGroup();
	if (!Mode || !Mode->Inspector || !Group)
	{
		AddError(TEXT("could not set up the mode and a group"));
		return false;
	}

	UMaterialInterface* Tiles = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_Tiles"));
	USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Tiles);
	TestNotNull(TEXT("a material takes surfacing info"), Info);
	if (Info)
	{
		Info->DefaultScale = FVector2D(2.0, 4.0);
	}
	TestTrue(TEXT("the info is found again"), USurfMaterialInfo::Find(Tiles) == Info);
	TestTrue(TEXT("the scale the material asks for"), USurfMaterialInfo::ScaleFor(Tiles).Equals(FVector2D(2.0, 4.0)));
	TestTrue(TEXT("a material without info asks for a metre"), USurfMaterialInfo::ScaleFor(nullptr).Equals(FVector2D(1.0, 1.0)));

	const FGuid PatchId = SurfMaterialTestUtil::AddSheet(*Group, FVector::ZeroVector);
	Group->RebuildPreview();
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialTestUtil::RefTo(Group, PatchId));

	Mode->Inspector->SetMaterialOnSelection(Tiles);
	const FSurfPatch* Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("the first assignment takes the material's scale"), Patch && Patch->Scale.Equals(FVector2D(2.0, 4.0)));
	TestTrue(TEXT("and the result says so"), Mode->Inspector->LastResult.Contains(TEXT("scale")));

	// A tuned scale survives re-assigning the same material.
	if (FSurfPatch* Mutable = Group->ModifyPatch(PatchId))
	{
		Mutable->Scale = FVector2D(3.0, 3.0);
	}
	Mode->Inspector->SetMaterialOnSelection(Tiles);
	Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("the same material again leaves the scale alone"), Patch && Patch->Scale.Equals(FVector2D(3.0, 3.0)));
	TestFalse(TEXT("and the result does not claim a scale"), Mode->Inspector->LastResult.Contains(TEXT("scale")));

	// A material with no info of its own leaves the scale alone too.
	UMaterialInterface* Plain = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_Plain"));
	Mode->Inspector->SetMaterialOnSelection(Plain);
	Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("a material without info keeps the patch's scale"), Patch && Patch->Scale.Equals(FVector2D(3.0, 3.0)));

	Group->Destroy();
	SurfMaterialTestUtil::LeaveMode();
	return true;
}

// --- Undo -----------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialUndoTest, "SurfacingPro.Materials.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialUndoTest::RunTest(const FString& Parameters)
{
	SurfMaterialTestUtil::LeaveMode();
	USurfEdMode* Mode = SurfMaterialTestUtil::EnterMode();
	ASurfGroup* Group = SurfMaterialTestUtil::SpawnGroup();
	if (!Mode || !Mode->Inspector || !Group || !GEditor)
	{
		AddError(TEXT("could not set up the mode and a group"));
		return false;
	}

	UMaterialInterface* Brick = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_UndoBrick"));
	const FGuid PatchId = SurfMaterialTestUtil::AddSheet(*Group, FVector::ZeroVector);
	Group->RebuildPreview();

	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialTestUtil::RefTo(Group, PatchId));
	Mode->Inspector->SetMaterialOnSelection(Brick);

	const FSurfPatch* Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("assigned"), Patch && Patch->Material == Brick);
	TestEqual(TEXT("two slots after the assignment"), Group->Preview ? Group->Preview->GetNumMaterials() : -1, 2);

	GEditor->UndoTransaction();
	Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("undo takes the material back"), Patch && Patch->Material == nullptr);
	TestEqual(TEXT("and the preview is rebuilt without its slot"), Group->Preview ? Group->Preview->GetNumMaterials() : -1, 1);

	GEditor->RedoTransaction();
	Patch = Group->FindPatch(PatchId);
	TestTrue(TEXT("redo puts it back"), Patch && Patch->Material == Brick);
	TestEqual(TEXT("with its slot"), Group->Preview ? Group->Preview->GetNumMaterials() : -1, 2);

	// Leave the undo stack where it was found.
	GEditor->UndoTransaction();

	Group->Destroy();
	SurfMaterialTestUtil::LeaveMode();
	return true;
}

// --- Copies carry the material --------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialCopiesTest, "SurfacingPro.Materials.Copies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialCopiesTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Brick = SurfMaterialTestUtil::MakeMaterial(TEXT("SurfTest_CopyBrick"));

	// Face to face: the middle-click gesture.
	FSurfFace From(FPlane(FVector(0.0, 0.0, 1.0), 0.0));
	From.Material = Brick;
	FSurfFace To(FPlane(FVector(1.0, 0.0, 0.0), 0.0));
	SurfProjection::CopySurfaceDefinition(From, To);
	TestTrue(TEXT("a face copy carries the material"), To.Material == Brick);

	// Patch to patch.
	FSurfPatch Source = SurfPatchMath::MakeFlatPatch(ESurfPatchType::Sheet, 3, 3, FVector::ZeroVector, FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	Source.EnsureId();
	Source.Material = Brick;
	FSurfPatch Target = SurfPatchMath::MakeFlatPatch(ESurfPatchType::Sheet, 3, 3, FVector(500.0, 0.0, 0.0), FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	Target.EnsureId();
	SurfPatchSurfacing::CopySurfaceDefinition(Source, Target);
	TestTrue(TEXT("a patch copy carries the material"), Target.Material == Brick);

	// Face onto a patch: continuing a wall.
	FSurfPatch Continued = SurfPatchMath::MakeFlatPatch(ESurfPatchType::Sheet, 3, 3, FVector(1000.0, 0.0, 0.0), FVector(200.0, 0.0, 0.0), FVector(0.0, 200.0, 0.0));
	Continued.EnsureId();
	SurfPatchSurfacing::CopyFaceSurface(From, Continued);
	TestTrue(TEXT("a patch taking a face's surface takes its material"), Continued.Material == Brick);

	// Solidify: front, back and skirts all wear what the source wore.
	TArray<FSurfPatch> Pieces;
	if (SurfPatchPrimitives::Solidify(Source, 20.0, Pieces))
	{
		TestTrue(TEXT("solidify makes pieces"), Pieces.Num() > 0);
		for (const FSurfPatch& Piece : Pieces)
		{
			TestTrue(TEXT("every solidified piece wears the source's material"), Piece.Material == Brick);
		}
	}
	else
	{
		AddError(TEXT("solidify refused a flat sheet"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
