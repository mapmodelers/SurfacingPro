// Copyright MapModelers. All Rights Reserved.
//
// The material verbs of the inspector (M-A). Kept apart from the rest of the
// inspector so the material work and the modelling work do not edit one file.
//
// The material is the unit of surfacing (M3): a patch or a brush face wears
// one UMaterialInterface, any the user owns, nothing to convert. Assigning
// is one transaction over the patches the selection spans and the brush
// faces selected; the selection is kept (D16.1); the result is on the bar.

#include "SurfInspector.h"
#include "SurfEdMode.h"

#include "ScopedTransaction.h"
#include "Editor.h"
#include "Materials/MaterialInterface.h"

#include "SurfGroup.h"
#include "SurfTypes.h"
#include "SurfMaterialInfo.h"
#include "SurfPatchSurfacing.h"
#include "SurfSession.h"

#define LOCTEXT_NAMESPACE "SurfacingProInspectorMaterials"

namespace
{
	/**
	 * The patches a selection spans: those a point or a face is selected on,
	 * the patches picked as wholes in object sub-mode, and - when nothing
	 * else says and no brush face is selected - the active patch, as every
	 * patch verb takes it.
	 */
	TArray<FSurfPatchRef> PatchesSpannedBySelection(const USurfEdMode& Mode)
	{
		TArray<FSurfPatchRef> Out;
		if (Mode.GetSubMode() == ESurfSubMode::Object)
		{
			Out = Mode.GetSelectedPatches();
		}

		auto Reach = [&Out](const TWeakObjectPtr<ASurfGroup>& Group, const FGuid& PatchId)
		{
			FSurfPatchRef Ref;
			Ref.Group = Group;
			Ref.PatchId = PatchId;
			if (Ref.IsValid())
			{
				Out.AddUnique(Ref);
			}
		};
		for (const FSurfPointRef& Point : Mode.GetPointSelection())
		{
			Reach(Point.Group, Point.PatchId);
		}
		for (const FSurfQuadRef& Quad : Mode.GetQuadSelection())
		{
			Reach(Quad.Group, Quad.PatchId);
		}

		if (Out.Num() == 0 && Mode.GetSelection().Num() == 0)
		{
			Out = Mode.GetSelectedPatches();
		}
		return Out;
	}
}

void USurfInspector::AssignMaterial()
{
	SetMaterialOnSelection(MaterialToAssign.Get());
}

bool USurfInspector::SetMaterialOnSelection(UMaterialInterface* Material)
{
	if (!Mode.IsValid())
	{
		return false;
	}

	const TArray<FSurfPatchRef> Spanned = PatchesSpannedBySelection(*Mode);
	if (Spanned.Num() == 0 && Mode->GetSelection().Num() == 0)
	{
		SetResult(TEXT("material: nothing picked to put it on"));
		SurfSession::Friction(TEXT("material_nothing_selected"));
		return false;
	}

	// A material's own scale goes on the first time it is put somewhere, so
	// the tile reads at the size its maker meant; the same material again
	// leaves a tuned scale alone.
	const USurfMaterialInfo* Info = USurfMaterialInfo::Find(Material);
	const bool bHasInfo = Info != nullptr;
	const FVector2D DefaultScale = USurfMaterialInfo::ScaleFor(Material);

	FScopedTransaction Transaction(LOCTEXT("AssignMaterial", "Assign Material"));

	TSet<ASurfGroup*> Touched;
	int32 PatchesDone = 0;
	int32 FacesDone = 0;
	int32 Rescaled = 0;
	int32 LowestLevel = TNumericLimits<int32>::Max();

	for (const FSurfPatchRef& Ref : Spanned)
	{
		ASurfGroup* Group = Ref.Group.Get();
		FSurfPatch* Patch = Group ? Group->ModifyPatch(Ref.PatchId) : nullptr;
		if (!Patch)
		{
			continue;
		}

		const bool bChanged = Patch->Material.Get() != Material;
		Patch->Material = Material;
		if (bChanged && bHasInfo)
		{
			Patch->Scale = DefaultScale;
			Patch->Projection.Scale = DefaultScale;
			SurfPatchSurfacing::Reapply(*Patch);
			++Rescaled;
		}
		++Patch->EditSerial;
		++PatchesDone;
		LowestLevel = FMath::Min(LowestLevel, Patch->StackLevel);
		Touched.Add(Group);
	}

	Mode->ForEachSelectedFace([&](ASurfGroup& Group, FSurfBrush& Brush, FSurfFace& Face)
	{
		const bool bChanged = Face.Material.Get() != Material;
		Face.Material = Material;
		if (bChanged && bHasInfo)
		{
			Face.UV.Scale = DefaultScale;
			++Rescaled;
		}
		++Brush.EditSerial;
		++FacesDone;
		Touched.Add(&Group);
	});

	for (ASurfGroup* Group : Touched)
	{
		Group->RebuildPreview();
	}

	// The result, in the past tense; the selection is kept (D16.1).
	FString What;
	if (PatchesDone > 0)
	{
		What += FString::Printf(TEXT("%d patch%s"), PatchesDone, PatchesDone == 1 ? TEXT("") : TEXT("es"));
	}
	if (FacesDone > 0)
	{
		if (!What.IsEmpty())
		{
			What += TEXT(", ");
		}
		What += FString::Printf(TEXT("%d face%s"), FacesDone, FacesDone == 1 ? TEXT("") : TEXT("s"));
	}

	const FString Name = Material ? Material->GetName() : FString();
	FString Line = Material
		? FString::Printf(TEXT("assigned %s to %s"), *Name, *What)
		: FString::Printf(TEXT("cleared the material on %s"), *What);
	if (Rescaled > 0)
	{
		Line += FString::Printf(TEXT(", scale %g x %g m"), DefaultScale.X, DefaultScale.Y);
	}
	// A layer material's mask is its paint: on level 0 it has nothing beneath
	// to blend into, so the line says what lifts it (D14, D64).
	if (Info && Info->Use == ESurfMaterialUse::Layer && PatchesDone > 0 && LowestLevel == 0)
	{
		Line += TEXT(" - a layer material on level 0; Layer lifts it");
	}
	SetResult(Line);
	SurfSession::Verb(TEXT("material"), FString::Printf(TEXT("name=%s patches=%d faces=%d"), *Name, PatchesDone, FacesDone));

	RefreshSelectionCount();
	if (GEditor)
	{
		Mode->RedrawViewports();
	}
	return PatchesDone + FacesDone > 0;
}

#undef LOCTEXT_NAMESPACE
