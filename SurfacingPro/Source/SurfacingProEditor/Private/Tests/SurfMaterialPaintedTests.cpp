// Copyright MapModelers. All Rights Reserved.
//
// The painted mask (M-C's material side): the function and the master the
// plugin ships, made by code, and Use = Layer named on the bar.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "SurfMaterialPainted.h"
#include "SurfMaterialInfo.h"
#include "SurfEdMode.h"
#include "SurfInspector.h"
#include "SurfGroup.h"
#include "SurfTypes.h"
#include "SurfPatchMath.h"
#include "SurfPatchSurfacing.h"

#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SurfMaterialPaintedTestUtil
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

	static USurfEdMode* EnterMode()
	{
		GLevelEditorModeTools().ActivateMode(USurfEdMode::EM_SurfacingPro);
		return Cast<USurfEdMode>(GLevelEditorModeTools().GetActiveScriptableMode(USurfEdMode::EM_SurfacingPro));
	}

	static void LeaveMode()
	{
		GLevelEditorModeTools().DeactivateMode(USurfEdMode::EM_SurfacingPro);
	}

	/** The parameter names of a material's expressions, of one kind. */
	template<typename T>
	static TArray<FName> ParameterNames(const UMaterial* Material)
	{
		TArray<FName> Out;
		if (Material)
		{
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (const T* Parameter = Cast<T>(Expression))
				{
					Out.Add(Parameter->GetParameterName());
				}
			}
		}
		return Out;
	}
}

// --- The function and the master ------------------------------------------------
//
// With ue/_run/author_painted.txt present they are made again and saved
// into the plugin (delete the two files first); without it, the shipped
// ones are loaded. Either way their shape is checked.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialPaintedMaskTest, "SurfacingPro.Materials.PaintedMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialPaintedMaskTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
	const FString Gate = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("_run/author_painted.txt") : FString();
	const bool bAuthor = !Gate.IsEmpty() && IFileManager::Get().FileExists(*Gate);

	FString Error;
	SurfMaterialPainted::FSurfPaintedAssets Assets = bAuthor
		? SurfMaterialPainted::Author(/*bSave*/ true, Error)
		: SurfMaterialPainted::Load();
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	AddInfo(bAuthor ? TEXT("authored and saved") : TEXT("loaded the shipped ones"));
	if (bAuthor)
	{
		// The registry reads the two files now, so the dependency check sees the disk.
		TArray<FString> Saved;
		Saved.Add(FPackageName::LongPackageNameToFilename(SurfMaterialPainted::FunctionPackage(), FPackageName::GetAssetPackageExtension()));
		Saved.Add(FPackageName::LongPackageNameToFilename(SurfMaterialPainted::MasterPackage(), FPackageName::GetAssetPackageExtension()));
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(Saved, /*bForceRescan*/ true);
	}

	TestNotNull(TEXT("the function is there"), Assets.Function);
	TestNotNull(TEXT("the master is there"), Assets.Master);
	if (!Assets.Function || !Assets.Master)
	{
		return false;
	}

	// The function: three optional inputs, two outputs, by name.
	TArray<FFunctionExpressionInput> Inputs;
	TArray<FFunctionExpressionOutput> Outputs;
	Assets.Function->GetInputsAndOutputs(Inputs, Outputs);
	TestEqual(TEXT("four inputs"), Inputs.Num(), 4);
	TestEqual(TEXT("two outputs"), Outputs.Num(), 2);
	auto HasInput = [&Inputs](const TCHAR* Name)
	{
		return Inputs.ContainsByPredicate([Name](const FFunctionExpressionInput& In) { return In.ExpressionInput && In.ExpressionInput->InputName == FName(Name); });
	};
	auto HasOutput = [&Outputs](const TCHAR* Name)
	{
		return Outputs.ContainsByPredicate([Name](const FFunctionExpressionOutput& Out) { return Out.ExpressionOutput && Out.ExpressionOutput->OutputName == FName(Name); });
	};
	TestTrue(TEXT("Channel in"), HasInput(TEXT("Channel")));
	TestTrue(TEXT("Height in"), HasInput(TEXT("Height")));
	TestTrue(TEXT("Sharpen in"), HasInput(TEXT("Sharpen")));
	TestTrue(TEXT("Contrast in"), HasInput(TEXT("Contrast")));
	TestTrue(TEXT("Mask out"), HasOutput(TEXT("Mask")));
	TestTrue(TEXT("Paint out"), HasOutput(TEXT("Paint")));
	for (const FFunctionExpressionInput& In : Inputs)
	{
		TestTrue(TEXT("every input has a default, so nothing need be plugged"), In.ExpressionInput && In.ExpressionInput->bUsePreviewValueAsDefault);
	}
	TestTrue(TEXT("the function is offered in the palette"), Assets.Function->bExposeToLibrary);

	// The master: masked, its opacity mask the function's Mask, its parameters by name, a layer by its info.
	TestTrue(TEXT("the master is masked"), Assets.Master->BlendMode == BLEND_Masked);
	const UMaterialEditorOnlyData* Editor = Assets.Master->GetEditorOnlyData();
	const UMaterialExpressionMaterialFunctionCall* Call = Editor ? Cast<UMaterialExpressionMaterialFunctionCall>(Editor->OpacityMask.Expression) : nullptr;
	TestNotNull(TEXT("its opacity mask is a function call"), Call);
	TestTrue(TEXT("to the painted mask"), Call && Call->MaterialFunction == Assets.Function);
	TestTrue(TEXT("its colour is wired"), Editor && Editor->BaseColor.Expression != nullptr);
	TestTrue(TEXT("its normal is wired"), Editor && Editor->Normal.Expression != nullptr);
	TestTrue(TEXT("its roughness is wired"), Editor && Editor->Roughness.Expression != nullptr);

	const TArray<FName> Textures = SurfMaterialPaintedTestUtil::ParameterNames<UMaterialExpressionTextureSampleParameter>(Assets.Master);
	const TArray<FName> Scalars = SurfMaterialPaintedTestUtil::ParameterNames<UMaterialExpressionParameter>(Assets.Master);
	TestTrue(TEXT("a BaseColor texture"), Textures.Contains(FName(TEXT("BaseColor"))));
	TestTrue(TEXT("a Normal texture"), Textures.Contains(FName(TEXT("Normal"))));
	TestTrue(TEXT("a Height texture"), Textures.Contains(FName(TEXT("Height"))));
	TestTrue(TEXT("a Roughness number"), Scalars.Contains(FName(TEXT("Roughness"))));
	TestTrue(TEXT("a Channel number"), Scalars.Contains(FName(TEXT("Channel"))));
	TestTrue(TEXT("a Sharpen number"), Scalars.Contains(FName(TEXT("Sharpen"))));
	TestTrue(TEXT("a Contrast number"), Scalars.Contains(FName(TEXT("Contrast"))));

	const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Assets.Master);
	TestNotNull(TEXT("the master carries surfacing info"), Info);
	TestTrue(TEXT("which says it is a layer"), Info && Info->Use == ESurfMaterialUse::Layer);

	TestTrue(TEXT("the function has its file"), FPackageName::DoesPackageExist(SurfMaterialPainted::FunctionPackage()));
	TestTrue(TEXT("the master has its file"), FPackageName::DoesPackageExist(SurfMaterialPainted::MasterPackage()));
	return true;
}

// --- Use = Layer on the bar -----------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfMaterialLayerUseTest, "SurfacingPro.Materials.LayerUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSurfMaterialLayerUseTest::RunTest(const FString& Parameters)
{
	SurfMaterialPaintedTestUtil::LeaveMode();
	USurfEdMode* Mode = SurfMaterialPaintedTestUtil::EnterMode();
	ASurfGroup* Group = SurfMaterialPaintedTestUtil::SpawnGroup();
	if (!Mode || !Mode->Inspector || !Group || !GEditor)
	{
		AddError(TEXT("could not set up the mode and a group"));
		return false;
	}

	// A material that says it is a layer, and one that says nothing.
	UMaterialInterface* Painted = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage(), TEXT("SurfTest_LayerUse"));
	if (USurfMaterialInfo* Info = USurfMaterialInfo::FindOrAdd(Painted))
	{
		Info->Use = ESurfMaterialUse::Layer;
	}
	UMaterialInterface* Plain = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage(), TEXT("SurfTest_PlainUse"));

	const FGuid PatchId = SurfMaterialPaintedTestUtil::AddSheet(*Group, FVector::ZeroVector);
	Group->RebuildPreview();
	Mode->SetSubMode(ESurfSubMode::Object);
	Mode->SelectPatch(SurfMaterialPaintedTestUtil::RefTo(Group, PatchId));

	// On level 0 the line says what a layer material wants.
	TestTrue(TEXT("assigned"), Mode->Inspector->SetMaterialOnSelection(Painted));
	TestTrue(TEXT("and the line says it is a layer material on level 0"), Mode->Inspector->LastResult.Contains(TEXT("layer material")));
	TestFalse(TEXT("which is not a refusal"), Mode->Inspector->LastResultWasRefusal());

	// A plain material says nothing of the kind.
	TestTrue(TEXT("assigned the plain one"), Mode->Inspector->SetMaterialOnSelection(Plain));
	TestFalse(TEXT("with no word about layers"), Mode->Inspector->LastResult.Contains(TEXT("layer material")));

	// Above level 0 the layer material is where it belongs: nothing to say.
	if (FSurfPatch* Patch = Group->ModifyPatch(PatchId))
	{
		Patch->StackLevel = 1;
	}
	Group->RebuildPreview();
	TestTrue(TEXT("assigned on level 1"), Mode->Inspector->SetMaterialOnSelection(Painted));
	TestFalse(TEXT("and the line has no word about it"), Mode->Inspector->LastResult.Contains(TEXT("layer material")));

	// Take the three assignments back before leaving.
	GEditor->UndoTransaction();
	GEditor->UndoTransaction();
	GEditor->UndoTransaction();

	Group->Destroy();
	SurfMaterialPaintedTestUtil::LeaveMode();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
