// Copyright MapModelers. All Rights Reserved.

#include "SurfPrefabInstance.h"

#include "Components/ChildActorComponent.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Materials/MaterialInterface.h"

#include "SurfGroup.h"
#include "SurfPrefab.h"
#include "SurfMaterialSet.h"
#include "SurfSettings.h"
#include "Engine/World.h"

using namespace UE::Geometry;

ASurfPrefabInstance::ASurfPrefabInstance()
{
	PrimaryActorTick.bCanEverTick = false;

	Preview = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Preview"));
	Preview->SetMobility(EComponentMobility::Static);
	RootComponent = Preview;

	// As a group's preview: no collision by itself - the compiled mesh has
	// its own - unless the setting asks for it for play-in-editor.
	Preview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Preview->SetCollisionProfileName(TEXT("NoCollision"));
	Preview->bEnableComplexCollision = false;
}

void ASurfPrefabInstance::RefreshPreviewMaterial()
{
	if (!Preview)
	{
		return;
	}
	UMaterialInterface* Fallback = ASurfGroup::DefaultMaterialProvider.IsBound()
		? ASurfGroup::DefaultMaterialProvider.Execute()
		: nullptr;
	TArray<UMaterialInterface*> Set;
	Set.Reserve(FMath::Max(1, PreviewMaterials.Num()));
	Set.Add(Fallback);
	for (int32 Slot = 1; Slot < PreviewMaterials.Num(); ++Slot)
	{
		Set.Add(PreviewMaterials[Slot] ? PreviewMaterials[Slot].Get() : Fallback);
	}
	// The view's override - the clay, the paint view - on every slot, as on
	// a group (S174a). The ghost wins over it.
	UMaterialInterface* Override = WantedOverride();
	WornOverride = Override;
	WornFallback = Fallback;
	bWornOnce = true;
	if (Override)
	{
		for (UMaterialInterface*& Each : Set)
		{
			Each = Override;
		}
	}
	Preview->ConfigureMaterialSet(Set, /*bDeleteExtraSlots*/ true);
}

UMaterialInterface* ASurfPrefabInstance::WantedOverride() const
{
	UMaterialInterface* Override = ASurfGroup::PreviewOverrideProvider.IsBound() ? ASurfGroup::PreviewOverrideProvider.Execute() : nullptr;
	if (bGhosted && ASurfGroup::GhostMaterialProvider.IsBound())
	{
		if (UMaterialInterface* Ghost = ASurfGroup::GhostMaterialProvider.Execute())
		{
			Override = Ghost;
		}
	}
	return Override;
}

bool ASurfPrefabInstance::PreviewLookStale() const
{
	if (!Preview || !bWornOnce)
	{
		return Preview != nullptr;
	}
	UMaterialInterface* Fallback = ASurfGroup::DefaultMaterialProvider.IsBound()
		? ASurfGroup::DefaultMaterialProvider.Execute()
		: nullptr;
	return WornOverride.Get() != WantedOverride() || WornFallback.Get() != Fallback;
}

void ASurfPrefabInstance::RebuildPreview()
{
	if (bBaked && !BakeIsShowing())
	{
		DropStaleBake();
	}

	FDynamicMesh3 Mesh;
	FSurfMaterialSet Materials;

	// The fallback is the same checker a group draws, so a prefab wearing
	// nothing looks like a patch wearing nothing.
	UMaterialInterface* Fallback = ASurfGroup::DefaultMaterialProvider.IsBound()
		? ASurfGroup::DefaultMaterialProvider.Execute()
		: nullptr;
	Materials.Reset(Fallback);

	if (Prefab)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const double LayerOffsetCm = Settings ? Settings->LayerOffsetCm : 0.5;
		ASurfGroup::AppendPrefabToMesh(Mesh, *Prefab, FTransform::Identity, Materials, LayerOffsetCm, bDrawOwnGeometry, &PlacementRanges,
			bOverridePaint && PaintOverrides.Num() > 0 ? &PaintOverrides : nullptr);
	}
	else
	{
		PlacementRanges.Reset();
	}

	TriangleCount = Mesh.TriangleCount();

	PreviewMaterials.Reset(Materials.Num());
	for (UMaterialInterface* Slot : Materials.Slots)
	{
		PreviewMaterials.Add(Slot);
	}

	if (Preview)
	{
		RefreshPreviewMaterial();
		Preview->SetMesh(MoveTemp(Mesh));
		if (bBaked)
		{
			if (BakeIsShowing())
			{
				ApplyBakeLook();
			}
			else
			{
				// The bake is not there to stand in for it (S73a).
				DropStaleBake();
			}
		}

		// The same rule as a group's: the world says whether it collides,
		// and a standing bake collides in the preview's place.
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const UWorld* World = GetWorld();
		const bool bCollide = (World && World->IsGameWorld())
			? (!(bBaked && BakeIsShowing()) && (!Settings || Settings->bCollisionWhilePlaying))
			: (Settings && Settings->bPreviewCollision);
		Preview->SetCollisionProfileName(bCollide ? TEXT("BlockAll") : TEXT("NoCollision"));
		Preview->SetCollisionEnabled(bCollide ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		Preview->SetComplexAsSimpleCollisionEnabled(bCollide, /*bImmediateUpdate*/ true);
	}

	RebuildParts();
}

void ASurfPrefabInstance::RebuildParts()
{
	// Everything this instance was showing on behalf of the asset goes, and
	// is made again: the asset is the truth, and a part component is never
	// saved with the level.
	for (const TObjectPtr<USceneComponent>& Each : PartComponents)
	{
		if (USceneComponent* Component = Each.Get())
		{
			Component->DestroyComponent();
		}
	}
	PartComponents.Reset();
	PartCount = 0;

	if (!Prefab || !RootComponent || !bDrawOwnGeometry)
	{
		// While the prefab is open for editing its parts are out in the
		// level as actors, and drawing them here as well would be two of
		// everything.
		return;
	}

	// Its own parts, then every placement's, at the placement's transform -
	// the same walk the geometry takes. Depth is capped rather than guarded
	// by a set: the same prefab may be placed twice, and both times count.
	struct FOpen
	{
		const USurfPrefab* Prefab = nullptr;
		FTransform At = FTransform::Identity;
		int32 Depth = 0;
	};
	TArray<FOpen> Open;
	Open.Add({ Prefab.Get(), FTransform::Identity, 0 });

	for (int32 I = 0; I < Open.Num() && I < 4096; ++I)
	{
		const FOpen Node = Open[I];
		if (!Node.Prefab || Node.Depth > 16)
		{
			continue;
		}

		for (const FSurfPart& Part : Node.Prefab->Parts)
		{
			if (Part.IsBake())
			{
				continue;
			}
			const FTransform Local = Part.Transform * Node.At;

			if (UStaticMesh* Mesh = Part.Mesh.LoadSynchronous())
			{
				UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetupAttachment(RootComponent);
				Component->RegisterComponent();
				Component->SetStaticMesh(Mesh);
				Component->SetRelativeTransform(Local);
				for (int32 Slot = 0; Slot < Part.Materials.Num(); ++Slot)
				{
					if (UMaterialInterface* Material = Part.Materials[Slot].LoadSynchronous())
					{
						Component->SetMaterial(Slot, Material);
					}
				}
				// The part's paint, when the prefab keeps it (S237a).
				if (Node.Prefab->bMeshPaint && Part.VertexColours.Num() > 0)
				{
					FSurfPart::ApplyVertexColours(Component, Part.VertexColours);
				}
				PartComponents.Add(Component);
				++PartCount;
			}
			else if (Part.IsLevel())
			{
				// A level inside the prefab (S218a): a level instance as a
				// child actor, pointed at the level once it exists.
				UChildActorComponent* Component = NewObject<UChildActorComponent>(this, NAME_None, RF_Transient);
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetupAttachment(RootComponent);
				Component->RegisterComponent();
				Component->SetRelativeTransform(Local);
				Component->SetChildActorClass(ALevelInstance::StaticClass());
				if (ALevelInstance* Child = Cast<ALevelInstance>(Component->GetChildActor()))
				{
					if (Child->SetWorldAsset(Part.Level))
					{
						Child->LoadLevelInstance();
					}
				}
				PartComponents.Add(Component);
				++PartCount;
			}
			else if (UClass* Class = Part.ActorClass.LoadSynchronous())
			{
				UChildActorComponent* Component = NewObject<UChildActorComponent>(this, NAME_None, RF_Transient);
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetupAttachment(RootComponent);
				Component->RegisterComponent();
				Component->SetRelativeTransform(Local);
				Component->SetChildActorClass(Class);
				PartComponents.Add(Component);
				++PartCount;
			}
		}

		for (const FSurfPlacement& Placement : Node.Prefab->Placements)
		{
			if (Placement.Prefab)
			{
				Open.Add({ Placement.Prefab.Get(), Placement.Transform * Node.At, Node.Depth + 1 });
			}
		}
	}
}

FGuid ASurfPrefabInstance::PlacementAtTriangle(int32 Triangle) const
{
	for (const FSurfPlacementRange& Range : PlacementRanges)
	{
		if (Range.Holds(Triangle))
		{
			return Range.Id;
		}
	}
	return FGuid();
}

bool ASurfPrefabInstance::BakeIsShowing() const
{
	if (!IsValid(BakedActor))
	{
		return false;
	}
	const UStaticMeshComponent* Mesh = BakedActor->FindComponentByClass<UStaticMeshComponent>();
	return Mesh && Mesh->GetStaticMesh() != nullptr;
}

void ASurfPrefabInstance::DropStaleBake()
{
	bBaked = false;
	ApplyBakeLook();
}

void ASurfPrefabInstance::ApplyBakeLook()
{
	const bool bStanding = bBaked && BakeIsShowing();
	const bool bBakeSide = bStanding && ASurfGroup::bShowBakeInEditor;
	if (Preview)
	{
		Preview->SetVisibility(!bBakeSide, false);
		Preview->SetHiddenInGame(bStanding);
	}
#if WITH_EDITOR
	if (IsValid(BakedActor))
	{
		BakedActor->bHiddenEd = true;
		BakedActor->SetIsTemporarilyHiddenInEditor(!bBakeSide);
	}
#endif
}

bool ASurfPrefabInstance::DrawsNothing() const
{
	// IsVisibleInEditor, not IsVisible: a baked preview is hidden from the
	// game and still on the editor's screen (S153a).
	return !Preview || TriangleCount == 0 || !Preview->IsVisibleInEditor();
}

FBox ASurfPrefabInstance::WorldBounds() const
{
	FBox Local(ForceInit);
	if (Prefab)
	{
		Local = Prefab->LocalBounds();
	}
	if (!Local.IsValid && Preview)
	{
		Preview->ProcessMesh([&Local](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			if (Mesh.TriangleCount() > 0)
			{
				const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh.GetBounds(true);
				Local = FBox(FVector(Bounds.Min), FVector(Bounds.Max));
			}
		});
	}
	if (!Local.IsValid)
	{
		// Nothing to measure: a hand-sized box at its origin, so an empty
		// prefab is still something the cursor can reach.
		Local = FBox(FVector(-25.0), FVector(25.0));
	}
	return Local.TransformBy(GetActorTransform());
}

void ASurfPrefabInstance::Unbake()
{
	if (!bBaked && !BakedActor)
	{
		return;
	}
	if (BakedActor)
	{
		BakedActor->Destroy();
		BakedActor = nullptr;
	}
	bBaked = false;
	ApplyBakeLook();
}

void ASurfPrefabInstance::PostLoad()
{
	Super::PostLoad();
	RebuildPreview();
}

#if WITH_EDITOR
void ASurfPrefabInstance::PostActorCreated()
{
	Super::PostActorCreated();
	// Under the same one folder as the groups (S72d).
	SetFolderPath(ASurfGroup::OutlinerPrefabFolder);
}
#endif

void ASurfPrefabInstance::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildPreview();
}

#if WITH_EDITOR
void ASurfPrefabInstance::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	// The tick off: the prefab's paint again (S251j).
	if (!bOverridePaint && PaintOverrides.Num() > 0)
	{
		PaintOverrides.Reset();
	}
	RebuildPreview();
}

void ASurfPrefabInstance::PostEditUndo()
{
	Super::PostEditUndo();
	// A stroke on its own paint undone (S251j).
	RebuildPreview();
}
#endif

void ASurfPrefabInstance::ApplyPaintOverride(FSurfPatch& Patch) const
{
	if (!bOverridePaint)
	{
		return;
	}
	for (const FSurfPaintOverride& Own : PaintOverrides)
	{
		if (Own.Patch == Patch.Id && Own.Tint.Num() == Patch.Points.Num() && Own.Weight.Num() == Patch.Points.Num())
		{
			for (int32 I = 0; I < Patch.Points.Num(); ++I)
			{
				Patch.Points[I].Tint = Own.Tint[I];
				Patch.Points[I].Weight = Own.Weight[I];
			}
			return;
		}
	}
}

void ASurfPrefabInstance::StorePaintOverride(const FSurfPatch& Patch)
{
	FSurfPaintOverride* Own = PaintOverrides.FindByPredicate([&Patch](const FSurfPaintOverride& Each) { return Each.Patch == Patch.Id; });
	if (!Own)
	{
		Own = &PaintOverrides.AddDefaulted_GetRef();
		Own->Patch = Patch.Id;
	}
	Own->Tint.SetNum(Patch.Points.Num());
	Own->Weight.SetNum(Patch.Points.Num());
	for (int32 I = 0; I < Patch.Points.Num(); ++I)
	{
		Own->Tint[I] = Patch.Points[I].Tint;
		Own->Weight[I] = Patch.Points[I].Weight;
	}
}
