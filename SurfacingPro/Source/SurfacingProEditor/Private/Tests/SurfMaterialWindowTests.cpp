// Copyright MapModelers. All Rights Reserved.
//
// The Materials window (M-B0): the library under the root, the search that
// narrows it, and assigning through the window. The widget itself needs a
// screen; what it does lives in SurfMaterialsLib and is tested here.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfMaterialsView.h"
#include "SurfMaterialManager.h"
#include "SurfMaterialThumbnails.h"
#include "SurfTextureLib.h"
#include "HAL/FileManager.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfTypes.h"
#include "SurfPatchMath.h"
#include "SurfPatchSurfacing.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "SurfMaterialInfo.h"
#include "SurfMaterialSeed.h"
#include "SurfSettings.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfMaterialWindowTestUtil
{
	/** A folder of the test's own under the default root, so nothing of the user's is read or touched. */
	static const TCHAR* TestRoot = TEXT("/Game/SurfacingPro/_SurfWindowTest");

	static IAssetRegistry& Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** An asset in a package under Folder, not yet announced to the registry, so a parent can be set first. */
	template<typename T>
	static T* MakeAsset(const FString& Folder, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(Folder / Name));
		if (!Package)
		{
			return nullptr;
		}
		return NewObject<T>(Package, FName(Name), RF_Public | RF_Standalone | RF_Transactional);
	}

	/** Tell the registry: from here on the asset is listed as if it were on disk. */
	static void Announce(UObject* Asset)
	{
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}
	}

	/** Take it out of the registry and let it be collected; nothing is written to disk. */
	static void Drop(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		UPackage* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (Package && Package != GetTransientPackage())
		{
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	static int32 IndexOf(const TArray<FSurfMaterialItem>& Items, const TCHAR* Name)
	{
		return Items.IndexOfByPredicate([Name](const FSurfMaterialItem& Item) { return Item.Name == Name; });
	}

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
		GLevelEditorModeTools().ActivateMode(USurfEdMode::EM_SurfacingPro);
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	static void LeaveMode()
	{
		GLevelEditorModeTools().DeactivateMode(USurfEdMode::EM_SurfacingPro);
	}
}

// --- The listing ----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialWindowListTest, "SurfacingPro.Materials.WindowList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialWindowListTest::RunTest(const FString& Parameters)
{
	using SurfMaterialsLib::ERootState;

	IAssetRegistry& Reg = SurfMaterialWindowTestUtil::Registry();
	Reg.WaitForCompletion();

	TArray<FSurfMaterialItem> Items;

	// The states before anything exists, each with its line.
	TestTrue(TEXT("an empty root is unset"), SurfMaterialsLib::List(TEXT("   "), Items) == ERootState::Unset);
	TestTrue(TEXT("and the line says where to set it"), SurfMaterialsLib::EmptyLine(ERootState::Unset, TEXT("")).Contains(TEXT("settings")));

	const FString Nowhere = FString(SurfMaterialWindowTestUtil::TestRoot) / TEXT("Nowhere");
	TestTrue(TEXT("a root the project does not have is missing"), SurfMaterialsLib::List(Nowhere + TEXT("/"), Items) == ERootState::Missing);
	TestEqual(TEXT("and lists nothing"), Items.Num(), 0);
	TestTrue(TEXT("and the line names the root without its slash"),
		SurfMaterialsLib::EmptyLine(ERootState::Missing, Nowhere + TEXT("/")).Contains(Nowhere + TEXT(" yet")));

	const FString EmptyFolder = FString(SurfMaterialWindowTestUtil::TestRoot) / TEXT("Empty");
	Reg.AddPath(EmptyFolder);
	TestTrue(TEXT("a folder with no material in it is empty"), SurfMaterialsLib::List(EmptyFolder, Items) == ERootState::Empty);

	// Three materials the registry knows of - one in a sub-folder - and one
	// thing under the root that is not a material.
	UMaterial* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInstanceConstant* Brick = SurfMaterialWindowTestUtil::MakeAsset<UMaterialInstanceConstant>(SurfMaterialWindowTestUtil::TestRoot, TEXT("MI_SurfTest_brick"));
	UMaterialInstanceConstant* Wall = SurfMaterialWindowTestUtil::MakeAsset<UMaterialInstanceConstant>(FString(SurfMaterialWindowTestUtil::TestRoot) / TEXT("Sub"), TEXT("MI_SurfTest_Wall"));
	UMaterial* Stone = SurfMaterialWindowTestUtil::MakeAsset<UMaterial>(SurfMaterialWindowTestUtil::TestRoot, TEXT("M_SurfTest_Stone"));
	UMaterialFunction* NotOne = SurfMaterialWindowTestUtil::MakeAsset<UMaterialFunction>(SurfMaterialWindowTestUtil::TestRoot, TEXT("MF_SurfTest_NotAMaterial"));
	if (!Brick || !Wall || !Stone || !NotOne || !Parent)
	{
		AddError(TEXT("could not make the test assets"));
		SurfMaterialWindowTestUtil::Drop(Brick); SurfMaterialWindowTestUtil::Drop(Wall); SurfMaterialWindowTestUtil::Drop(Stone); SurfMaterialWindowTestUtil::Drop(NotOne);
		return false;
	}
	Brick->SetParentEditorOnly(Parent, false);
	Wall->SetParentEditorOnly(Parent, false);
	SurfMaterialWindowTestUtil::Announce(Brick); SurfMaterialWindowTestUtil::Announce(Wall); SurfMaterialWindowTestUtil::Announce(Stone); SurfMaterialWindowTestUtil::Announce(NotOne);

	TestTrue(TEXT("the root lists its materials"), SurfMaterialsLib::List(FString(SurfMaterialWindowTestUtil::TestRoot) + TEXT("/"), Items) == ERootState::Listed);
	TestEqual(TEXT("three of them, the sub-folder's included, the function not"), Items.Num(), 3);

	const int32 AtBrick = SurfMaterialWindowTestUtil::IndexOf(Items, TEXT("MI_SurfTest_brick"));
	const int32 AtWall = SurfMaterialWindowTestUtil::IndexOf(Items, TEXT("MI_SurfTest_Wall"));
	const int32 AtStone = SurfMaterialWindowTestUtil::IndexOf(Items, TEXT("M_SurfTest_Stone"));
	TestTrue(TEXT("brick is listed"), AtBrick != INDEX_NONE);
	TestTrue(TEXT("the wall in the sub-folder is listed"), AtWall != INDEX_NONE);
	TestTrue(TEXT("the material is listed"), AtStone != INDEX_NONE);
	TestTrue(TEXT("the function is not"), SurfMaterialWindowTestUtil::IndexOf(Items, TEXT("MF_SurfTest_NotAMaterial")) == INDEX_NONE);
	TestTrue(TEXT("the order is by name, case blind"), AtBrick != INDEX_NONE && AtWall != INDEX_NONE && AtBrick < AtWall);
	if (AtBrick != INDEX_NONE && AtStone != INDEX_NONE)
	{
		TestTrue(TEXT("an instance is marked as one"), Items[AtBrick].bInstance);
		TestFalse(TEXT("a material is not"), Items[AtStone].bInstance);
		TestEqual(TEXT("an instance names its parent"), Items[AtBrick].Parent, Parent->GetName());
	}

	// The sub-folder on its own.
	TestTrue(TEXT("a sub-folder lists what is under it"), SurfMaterialsLib::List(FString(SurfMaterialWindowTestUtil::TestRoot) / TEXT("Sub"), Items) == ERootState::Listed);
	TestEqual(TEXT("one material there"), Items.Num(), 1);

	SurfMaterialWindowTestUtil::Drop(Brick); SurfMaterialWindowTestUtil::Drop(Wall); SurfMaterialWindowTestUtil::Drop(Stone); SurfMaterialWindowTestUtil::Drop(NotOne);
	Reg.RemovePath(FString(SurfMaterialWindowTestUtil::TestRoot) / TEXT("Sub"));
	Reg.RemovePath(EmptyFolder);
	Reg.RemovePath(FString(SurfMaterialWindowTestUtil::TestRoot));

	TestTrue(TEXT("gone from the registry, gone from the list"),
		SurfMaterialsLib::List(FString(SurfMaterialWindowTestUtil::TestRoot), Items) != ERootState::Listed);
	return true;
}

// --- The filter -----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialWindowFilterTest, "SurfacingPro.Materials.WindowFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialWindowFilterTest::RunTest(const FString& Parameters)
{
	TArray<FSurfMaterialItem> Items;
	auto Add = [&Items](const TCHAR* Name, const TCHAR* Parent, bool bInstance)
	{
		FSurfMaterialItem& Item = Items.AddDefaulted_GetRef();
		Item.Name = Name;
		Item.Parent = Parent;
		Item.bInstance = bInstance;
	};
	Add(TEXT("M_Brick"), TEXT(""), false);
	Add(TEXT("MI_Brick_Red"), TEXT("M_Brick"), true);
	Add(TEXT("MI_Stone_Wall"), TEXT("M_Stone"), true);
	Add(TEXT("M_Plaster"), TEXT(""), false);

	TArray<FSurfMaterialItem> Out;
	SurfMaterialsLib::Filter(Items, TEXT(""), Out);
	TestEqual(TEXT("an empty query keeps everything"), Out.Num(), 4);

	SurfMaterialsLib::Filter(Items, TEXT("brick"), Out);
	TestEqual(TEXT("a word narrows by name, case blind"), Out.Num(), 2);

	SurfMaterialsLib::Filter(Items, TEXT("BRICK red"), Out);
	TestEqual(TEXT("two words both have to match"), Out.Num(), 1);
	TestTrue(TEXT("and the one left is the red brick"), Out.Num() == 1 && Out[0].Name == TEXT("MI_Brick_Red"));

	SurfMaterialsLib::Filter(Items, TEXT("m_stone"), Out);
	TestEqual(TEXT("a parent's name matches too"), Out.Num(), 1);
	TestTrue(TEXT("through the instance"), Out.Num() == 1 && Out[0].Name == TEXT("MI_Stone_Wall"));

	SurfMaterialsLib::Filter(Items, TEXT("   plaster  "), Out);
	TestEqual(TEXT("spaces round the query do not count"), Out.Num(), 1);

	SurfMaterialsLib::Filter(Items, TEXT("zzz"), Out);
	TestEqual(TEXT("nothing matches, nothing is shown"), Out.Num(), 0);

	// The parent's name out of the tag, in the shapes the registry writes it.
	TestEqual(TEXT("a bare object path"), SurfMaterialsLib::ParentNameFromTag(TEXT("/Game/A/M_Foo.M_Foo")), FString(TEXT("M_Foo")));
	TestEqual(TEXT("a quoted one with its class"), SurfMaterialsLib::ParentNameFromTag(TEXT("Material'/Game/A/M_Foo.M_Foo'")), FString(TEXT("M_Foo")));
	TestEqual(TEXT("a script class path"),
		SurfMaterialsLib::ParentNameFromTag(TEXT("/Script/Engine.Material'/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial'")),
		FString(TEXT("WorldGridMaterial")));
	TestEqual(TEXT("nothing from nothing"), SurfMaterialsLib::ParentNameFromTag(TEXT("")), FString());

	// The root, as the setting may hold it.
	TestEqual(TEXT("a trailing slash is dropped"), SurfMaterialsLib::NormalisedRoot(TEXT("/Game/SurfacingPro/")), FString(TEXT("/Game/SurfacingPro")));
	TestEqual(TEXT("spaces are dropped"), SurfMaterialsLib::NormalisedRoot(TEXT("  /Game/SurfacingPro  ")), FString(TEXT("/Game/SurfacingPro")));
	TestEqual(TEXT("empty stays empty"), SurfMaterialsLib::NormalisedRoot(TEXT("  ")), FString());
	return true;
}

// --- Assigning through the window -----------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialWindowAssignTest, "SurfacingPro.Materials.WindowAssign",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialWindowAssignTest::RunTest(const FString& Parameters)
{

	TestTrue(TEXT("no mode: the foot says so"), SurfMaterialsLib::SelectionLine(nullptr).StartsWith(TEXT("mode not up")));

	SurfMaterialWindowTestUtil::LeaveMode();
	USurfEdMode* Mode = SurfMaterialWindowTestUtil::EnterMode();
	if (!Mode || !Mode->Inspector || !GEditor)
	{
		AddError(TEXT("could not enter the mode with an inspector"));
		return false;
	}

	UMaterialInterface* Brick = SurfMaterialWindowTestUtil::MakeMaterial(TEXT("SurfTest_WindowBrick"));

	// Nothing selected: the foot says what to pick, the click is refused on the bar.
	TestTrue(TEXT("nothing selected: the foot says what to pick"), SurfMaterialsLib::SelectionLine(Mode).StartsWith(TEXT("nothing selected")));
	TestFalse(TEXT("a click assigns nothing"), SurfMaterialsLib::Assign(*Mode, Brick));
	TestEqual(TEXT("and the bar says why"), Mode->Inspector->LastResult, FString(TEXT("material: nothing picked to put it on")));

	ASurfGroup* Group = SurfMaterialWindowTestUtil::SpawnGroup();
	if (!Group)
	{
		AddError(TEXT("could not spawn a group in the editor world"));
		SurfMaterialWindowTestUtil::LeaveMode();
		return false;
	}

	const FGuid FirstId = SurfMaterialWindowTestUtil::AddSheet(*Group, FVector::ZeroVector);
	const FGuid SecondId = SurfMaterialWindowTestUtil::AddSheet(*Group, FVector(500.0, 0.0, 0.0));
	Group->AppendBox(FBox(FVector(1000.0, 0.0, 0.0), FVector(1200.0, 200.0, 200.0)));
	Group->RebuildPreview();

	// Two patches picked as wholes: the click puts the material on both.
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialWindowTestUtil::RefTo(Group, FirstId));
	Mode->AddPatchToSelection(SurfMaterialWindowTestUtil::RefTo(Group, SecondId), /*bToggle*/ false);
	TestEqual(TEXT("the foot counts the patches"), SurfMaterialsLib::SelectionLine(Mode), FString(TEXT("2 patches picked - click a material to put it on")));

	TestTrue(TEXT("a click assigns to the selection"), SurfMaterialsLib::Assign(*Mode, Brick));
	TestTrue(TEXT("the bar says what happened"), Mode->Inspector->LastResult.StartsWith(TEXT("assigned SurfTest_WindowBrick to 2 patches")));
	const FSurfPatch* First = Group->FindPatch(FirstId);
	const FSurfPatch* Second = Group->FindPatch(SecondId);
	TestTrue(TEXT("the first patch wears it"), First && First->Material == Brick);
	TestTrue(TEXT("the second patch wears it"), Second && Second->Material == Brick);
	TestEqual(TEXT("the selection is kept"), Mode->GetSelectedPatches().Num(), 2);

	// Undo takes it off both; redo puts it back.
	GEditor->UndoTransaction();
	First = Group->FindPatch(FirstId);
	Second = Group->FindPatch(SecondId);
	TestTrue(TEXT("undo takes it off the first"), First && First->Material == nullptr);
	TestTrue(TEXT("and off the second"), Second && Second->Material == nullptr);
	GEditor->RedoTransaction();
	First = Group->FindPatch(FirstId);
	TestTrue(TEXT("redo puts it back"), First && First->Material == Brick);
	GEditor->UndoTransaction();

	// Brush faces: the foot counts them and the click reaches them, not the active patch.
	Mode->SetSubMode(ESurfSubMode::Face);
	Mode->SelectFace(SurfMaterialWindowTestUtil::FaceOf(Group, 2));
	Mode->SelectFace(SurfMaterialWindowTestUtil::FaceOf(Group, 3), /*bAddToSelection*/ true);
	TestEqual(TEXT("the foot counts the faces"), SurfMaterialsLib::SelectionLine(Mode), FString(TEXT("2 faces picked - click a material to put it on")));
	TestTrue(TEXT("a click assigns to the faces"), SurfMaterialsLib::Assign(*Mode, Brick));
	int32 BrickFaces = 0;
	Group->ForEachBrush([&BrickFaces, Brick](const FSurfBrush& Brush)
	{
		for (const FSurfFace& Face : Brush.Faces)
		{
			if (Face.Material == Brick)
			{
				++BrickFaces;
			}
		}
	});
	TestEqual(TEXT("two faces wear it"), BrickFaces, 2);
	First = Group->FindPatch(FirstId);
	TestTrue(TEXT("the patch does not"), First && First->Material == nullptr);
	GEditor->UndoTransaction();

	Group->Destroy();
	SurfMaterialWindowTestUtil::LeaveMode();
	return true;
}

// --- The tree ---------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialWindowTreeTest, "SurfacingPro.Materials.WindowTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialWindowTreeTest::RunTest(const FString& Parameters)
{
	TArray<FSurfMaterialItem> Items;
	auto Add = [&Items](const TCHAR* Name, const TCHAR* Parent, bool bInstance)
	{
		FSurfMaterialItem& Item = Items.AddDefaulted_GetRef();
		Item.Name = Name;
		Item.Parent = Parent;
		Item.bInstance = bInstance;
	};
	Add(TEXT("MI_Brick_Red_Dark"), TEXT("MI_Brick_Red"), true);
	Add(TEXT("M_Plaster"), TEXT(""), false);
	Add(TEXT("MI_Stone"), TEXT("M_Stone"), true);
	Add(TEXT("MI_Brick_Red"), TEXT("M_Brick"), true);
	Add(TEXT("M_Brick"), TEXT(""), false);
	Add(TEXT("MI_Lost"), TEXT(""), true);

	TArray<TSharedPtr<FSurfMaterialNode>> Roots;
	SurfMaterialsLib::BuildTree(Items, Roots);

	// The top: a heading per parent that has instances, and the orphan -
	// by name. A material of its own is not a row (D68): plaster, with no
	// instances, is nowhere; brick is a heading.
	TestEqual(TEXT("three rows at the top"), Roots.Num(), 3);
	if (Roots.Num() == 3)
	{
		TestEqual(TEXT("brick's heading first"), Roots[0]->Label, FString(TEXT("M_Brick")));
		TestTrue(TEXT("a heading, not a material"), Roots[0]->IsGroupOnly());
		TestEqual(TEXT("stone's heading"), Roots[1]->Label, FString(TEXT("M_Stone")));
		TestTrue(TEXT("a heading too, though stone is not listed"), Roots[1]->IsGroupOnly());
		TestEqual(TEXT("the orphan instance"), Roots[2]->Label, FString(TEXT("MI_Lost")));
		TestFalse(TEXT("which is a material"), Roots[2]->IsGroupOnly());

		TestEqual(TEXT("brick has one instance"), Roots[0]->Children.Num(), 1);
		if (Roots[0]->Children.Num() == 1)
		{
			TestEqual(TEXT("the red one"), Roots[0]->Children[0]->Label, FString(TEXT("MI_Brick_Red")));
			TestEqual(TEXT("which has the dark one under it"), Roots[0]->Children[0]->Children.Num(), 1);
		}
		TestEqual(TEXT("stone's heading holds its instance"), Roots[1]->Children.Num(), 1);
	}

	// The parents are what New MI offers.
	TArray<FSurfMaterialItem> Parents;
	SurfMaterialsLib::Masters(Items, Parents);
	TestEqual(TEXT("two parents to choose from"), Parents.Num(), 2);
	TestTrue(TEXT("plaster among them"), Parents.ContainsByPredicate([](const FSurfMaterialItem& I) { return I.Name == TEXT("M_Plaster"); }));
	TestTrue(TEXT("brick among them"), Parents.ContainsByPredicate([](const FSurfMaterialItem& I) { return I.Name == TEXT("M_Brick"); }));
	TestFalse(TEXT("no instance among them"), Parents.ContainsByPredicate([](const FSurfMaterialItem& I) { return I.bInstance; }));

	// A filter that keeps an instance and not its parent still shows it, under a heading for the parent.
	TArray<FSurfMaterialItem> Kept;
	SurfMaterialsLib::Filter(Items, TEXT("dark"), Kept);
	SurfMaterialsLib::BuildTree(Kept, Roots);
	TestEqual(TEXT("one heading for the dark brick's parent"), Roots.Num(), 1);
	TestTrue(TEXT("named after it"), Roots.Num() == 1 && Roots[0]->Label == TEXT("MI_Brick_Red") && Roots[0]->IsGroupOnly());

	// Nothing: nothing. Only parents: nothing.
	SurfMaterialsLib::BuildTree(TArray<FSurfMaterialItem>(), Roots);
	TestEqual(TEXT("an empty list is an empty tree"), Roots.Num(), 0);
	SurfMaterialsLib::BuildTree(Parents, Roots);
	TestEqual(TEXT("parents alone make no rows"), Roots.Num(), 0);
	return true;
}

// --- Making instances -------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialNewInstanceTest, "SurfacingPro.Materials.NewInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialNewInstanceTest::RunTest(const FString& Parameters)
{
	const FString Root = TEXT("/Game/SurfacingPro/_SurfNewTest");
	const FString Folder = SurfMaterialsLib::InstancesFolder(Root + TEXT("/"));
	TestEqual(TEXT("instances go under MaterialInstances"), Folder, Root + TEXT("/MaterialInstances"));

	FString Error;
	TestNull(TEXT("no parent, no instance"), SurfMaterialsLib::NewInstance(nullptr, Folder, false, Error));
	TestTrue(TEXT("and the line says to pick one"), Error.StartsWith(TEXT("new instance: pick")));

	UMaterial* Master = SurfMaterialWindowTestUtil::MakeAsset<UMaterial>(Root / TEXT("Materials"), TEXT("M_SurfNew"));
	if (!Master)
	{
		AddError(TEXT("could not make the master"));
		return false;
	}
	USurfMaterialInfo* MasterInfo = USurfMaterialInfo::FindOrAdd(Master);
	MasterInfo->DefaultScale = FVector2D(2.0, 3.0);
	MasterInfo->Use = ESurfMaterialUse::Layer;
	SurfMaterialWindowTestUtil::Announce(Master);

	TestEqual(TEXT("the base name drops the M_"), SurfMaterialsLib::InstanceBaseName(Master), FString(TEXT("MI_SurfNew")));

	UMaterialInstanceConstant* First = SurfMaterialsLib::NewInstance(Master, Folder, false, Error);
	TestNotNull(TEXT("an instance is made"), First);
	TestTrue(TEXT("with no complaint"), Error.IsEmpty());
	if (First)
	{
		TestEqual(TEXT("named after its parent"), First->GetName(), FString(TEXT("MI_SurfNew")));
		TestTrue(TEXT("in the folder"), First->GetOutermost()->GetName() == Folder / TEXT("MI_SurfNew"));
		TestTrue(TEXT("with the parent"), First->Parent == Master);
		const USurfMaterialInfo* Info = USurfMaterialInfo::Find(First);
		TestNotNull(TEXT("carrying the parent's info"), Info);
		TestTrue(TEXT("the scale"), Info && Info->DefaultScale.Equals(FVector2D(2.0, 3.0)));
		TestTrue(TEXT("the use"), Info && Info->Use == ESurfMaterialUse::Layer);
		TestTrue(TEXT("its own copy, not the parent's"), Info != MasterInfo);
	}

	UMaterialInstanceConstant* Second = SurfMaterialsLib::NewInstance(Master, Folder, false, Error);
	TestTrue(TEXT("a second takes the next number"), Second && Second->GetName() == TEXT("MI_SurfNew_02"));

	// An instance of an instance keeps the family name.
	UMaterialInstanceConstant* Third = SurfMaterialsLib::NewInstance(First, Folder, false, Error);
	TestTrue(TEXT("an instance of an instance is numbered on"), Third && Third->GetName() == TEXT("MI_SurfNew_03"));
	TestTrue(TEXT("with the instance as its parent"), Third && Third->Parent == First);

	// A copy keeps the info and takes the next number; a seed stamp does not survive it.
	UMaterialInstanceConstant* Copy = nullptr;
	if (First)
	{
		if (USurfMaterialStamp* Mark = USurfMaterialStamp::FindOrAdd(First))
		{
			Mark->Source = TEXT("/SurfacingPro/Materials/Instances/MI_Something");
		}
		Copy = SurfMaterialsLib::CopyInstance(First, Folder, false, Error);
		TestNotNull(TEXT("a copy is made"), Copy);
		if (Copy)
		{
			TestEqual(TEXT("numbered after the others"), Copy->GetName(), FString(TEXT("MI_SurfNew_04")));
			TestTrue(TEXT("with the same parent"), Copy->Parent == Master);
			const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Copy);
			TestTrue(TEXT("keeping the info"), Info && Info->DefaultScale.Equals(FVector2D(2.0, 3.0)));
			TestNull(TEXT("and no stamp: a copy is the user's own"), USurfMaterialStamp::Find(Copy));
		}
	}
	TestNull(TEXT("no source, no copy"), SurfMaterialsLib::CopyInstance(nullptr, Folder, false, Error));
	TestTrue(TEXT("and the line says to pick one"), Error.StartsWith(TEXT("copy: pick")));

	SurfMaterialWindowTestUtil::Drop(Copy);
	SurfMaterialWindowTestUtil::Drop(Third);
	SurfMaterialWindowTestUtil::Drop(Second);
	SurfMaterialWindowTestUtil::Drop(First);
	SurfMaterialWindowTestUtil::Drop(Master);
	IAssetRegistry& Reg = SurfMaterialWindowTestUtil::Registry();
	Reg.RemovePath(Folder);
	Reg.RemovePath(Root / TEXT("Materials"));
	Reg.RemovePath(Root);
	return true;
}

// --- The default parent, and a new one ----------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialDefaultParentTest, "SurfacingPro.Materials.DefaultParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialDefaultParentTest::RunTest(const FString& Parameters)
{
	const FString Root = TEXT("/Game/SurfacingPro/_SurfDefaultTest");
	const FString Folder = SurfMaterialsLib::MastersFolder(Root + TEXT("/"));
	TestEqual(TEXT("parents live under Materials"), Folder, Root + TEXT("/Materials"));

	// The default: the one the settings name, else the first, else none.
	USurfSettings* Settings = GetMutableDefault<USurfSettings>();
	const FString Was = Settings->DefaultParent;
	TArray<FSurfMaterialItem> None;
	TestEqual(TEXT("no masters, no default"), SurfMaterialsLib::DefaultMasterIndex(None), (int32)INDEX_NONE);
	TArray<FSurfMaterialItem> Items;
	Items.AddDefaulted(3);
	Items[0].Name = TEXT("M_SurfA");
	Items[1].Name = TEXT("M_SurfB");
	Items[2].Name = TEXT("M_SurfC");
	Settings->DefaultParent = TEXT(" m_surfb ");
	TestEqual(TEXT("the named one, whatever the case or the spaces"), SurfMaterialsLib::DefaultMasterIndex(Items), 1);
	Settings->DefaultParent = TEXT("M_NotThere");
	TestEqual(TEXT("not there: the first"), SurfMaterialsLib::DefaultMasterIndex(Items), 0);
	Settings->DefaultParent = Was;

	// A new parent: a numbered copy beside the source, its info kept, its stamp dropped.
	FString Error;
	TestNull(TEXT("no source, no parent"), SurfMaterialsLib::CopyMaster(nullptr, Folder, false, Error));
	TestTrue(TEXT("and the line says to pick one"), Error.StartsWith(TEXT("new parent: pick")));

	UMaterial* Master = SurfMaterialWindowTestUtil::MakeAsset<UMaterial>(Folder, TEXT("M_SurfBase"));
	if (!Master)
	{
		AddError(TEXT("could not make the master"));
		return false;
	}
	USurfMaterialInfo* MasterInfo = USurfMaterialInfo::FindOrAdd(Master);
	MasterInfo->DefaultScale = FVector2D(4.0, 5.0);
	if (USurfMaterialStamp* Mark = USurfMaterialStamp::FindOrAdd(Master))
	{
		Mark->Source = TEXT("/SurfacingPro/Materials/M_SurfBase");
	}
	SurfMaterialWindowTestUtil::Announce(Master);

	UMaterial* Copy = SurfMaterialsLib::CopyMaster(Master, Folder, false, Error);
	TestNotNull(TEXT("a parent is made"), Copy);
	TestTrue(TEXT("with no complaint"), Error.IsEmpty());
	if (Copy)
	{
		TestEqual(TEXT("numbered after its source"), Copy->GetName(), FString(TEXT("M_SurfBase_02")));
		TestTrue(TEXT("in the Materials folder"), Copy->GetOutermost()->GetName() == Folder / TEXT("M_SurfBase_02"));
		const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Copy);
		TestTrue(TEXT("keeping the info"), Info && Info->DefaultScale.Equals(FVector2D(4.0, 5.0)));
		TestTrue(TEXT("its own copy of it"), Info != MasterInfo);
		TestNull(TEXT("and no stamp: the user's own"), USurfMaterialStamp::Find(Copy));
	}

	// An instance of the new parent is named after it, as any instance is.
	UMaterialInstanceConstant* Instance = Copy ? SurfMaterialsLib::NewInstance(Copy, SurfMaterialsLib::InstancesFolder(Root + TEXT("/")), false, Error) : nullptr;
	TestTrue(TEXT("an instance of it takes its name"), Instance && Instance->GetName() == TEXT("MI_SurfBase_02"));

	SurfMaterialWindowTestUtil::Drop(Instance);
	SurfMaterialWindowTestUtil::Drop(Copy);
	SurfMaterialWindowTestUtil::Drop(Master);
	IAssetRegistry& Reg = SurfMaterialWindowTestUtil::Registry();
	Reg.RemovePath(Root / TEXT("MI"));
	Reg.RemovePath(Folder);
	Reg.RemovePath(Root);
	return true;
}

// --- The preview's slot --------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialPreviewSlotTest, "SurfacingPro.Materials.PreviewSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialPreviewSlotTest::RunTest(const FString& Parameters)
{
	// Slot 0 is the cube's opaque geometry, 1 its alpha geometry: a material goes where its blend mode says.
	TestEqual(TEXT("nothing: the opaque slot"), SurfMaterialPreview::SlotFor(nullptr), 0);
	auto Slot = [](const TCHAR* Path) -> int32
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path);
		return Material ? SurfMaterialPreview::SlotFor(Material) : -1;
	};
	TestEqual(TEXT("an opaque master"), Slot(TEXT("/SurfacingPro/Materials/M_Opq_Simple.M_Opq_Simple")), 0);
	TestEqual(TEXT("a masked master is opaque geometry too"), Slot(TEXT("/SurfacingPro/Materials/M_SurfPainted.M_SurfPainted")), 0);
	TestEqual(TEXT("a translucent master"), Slot(TEXT("/SurfacingPro/Materials/M_AlphaBlend_Simple.M_AlphaBlend_Simple")), 1);
	TestTrue(TEXT("the mesh path is the plugin's cube unless set"), SurfMaterialPreview::MeshPath().ToString().EndsWith(TEXT("mat_cube.mat_cube")) || !GetDefault<USurfSettings>()->PreviewMesh.IsNull());
	TestTrue(TEXT("the lens is long"), SurfMaterialPreview::FOV() < 45.0f);
	return true;
}

// --- The thumbnails --------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialThumbnailTest, "SurfacingPro.Materials.Thumbnail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialThumbnailTest::RunTest(const FString& Parameters)
{
	// Where a picture lives, and what happens where nothing can be rendered (this editor has no RHI).
	TestEqual(TEXT("under the source root, by name"), SurfMaterialThumbnails::CachePathFor(TEXT("D:/Proj/SurfacingPro"), TEXT("MI_Brick")), FString(TEXT("D:/Proj/SurfacingPro/Thumbnails/MI_Brick.png")));
	TestTrue(TEXT("a picture is small but not tiny"), SurfMaterialThumbnails::Size() >= 64 && SurfMaterialThumbnails::Size() <= 256);

	FString Why;
	TArray<FColor> Pixels;
	TestFalse(TEXT("no material, no render"), SurfMaterialThumbnails::Render(nullptr, 32, Pixels, Why));
	TestTrue(TEXT("with the reason"), Why.StartsWith(TEXT("render: ")));

	UMaterialInterface* Instance = LoadObject<UMaterialInterface>(nullptr, TEXT("/SurfacingPro/Materials/Instances/MI_OpqSimple_Red.MI_OpqSimple_Red"));
	TestNotNull(TEXT("a shipped instance to try"), Instance);
	if (Instance)
	{
		const bool bRendered = SurfMaterialThumbnails::Render(Instance, 32, Pixels, Why);
		if (SurfMaterialThumbnails::CanRender())
		{
			TestTrue(TEXT("rendered where it can be: ") + Why, bRendered);
			TestEqual(TEXT("32 by 32"), Pixels.Num(), 32 * 32);
		}
		else
		{
			TestFalse(TEXT("not rendered where it cannot be"), bRendered);
			TestTrue(TEXT("and it says so"), Why.Contains(TEXT("cannot render")));
			// A picture kept on disk by an editor that could render is still
			// read (S250e: his session had left one, and this failed).
			const bool bKept = IFileManager::Get().FileExists(*SurfMaterialThumbnails::CachePathFor(SurfTextures::SourceRoot(), Instance->GetName()));
			if (!bKept)
			{
				TestNull(TEXT("no picture either"), SurfMaterialThumbnails::PictureFor(Instance, Why));
			}
		}
	}
	SurfMaterialThumbnails::Forget();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
