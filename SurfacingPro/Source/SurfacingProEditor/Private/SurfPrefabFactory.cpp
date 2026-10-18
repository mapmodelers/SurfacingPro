// Copyright MapModelers. All Rights Reserved.

#include "SurfPrefabFactory.h"

#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"

#include "SurfPrefab.h"
#include "SurfPrefabInstance.h"

#define LOCTEXT_NAMESPACE "SurfacingPro"

USurfPrefabActorFactory::USurfPrefabActorFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = LOCTEXT("PrefabFactory", "SurfacingPro Prefab");
	NewActorClass = ASurfPrefabInstance::StaticClass();
	bUseSurfaceOrientation = false;
}

bool USurfPrefabActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(USurfPrefab::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("NotAPrefab", "Not a SurfacingPro prefab.");
		return false;
	}
	return true;
}

void USurfPrefabActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);

	if (ASurfPrefabInstance* Instance = Cast<ASurfPrefabInstance>(NewActor))
	{
		Instance->Prefab = Cast<USurfPrefab>(Asset);
		Instance->RebuildPreview();
	}
}

UObject* USurfPrefabActorFactory::GetAssetFromActorInstance(AActor* Instance)
{
	const ASurfPrefabInstance* Placed = Cast<ASurfPrefabInstance>(Instance);
	return Placed ? Placed->Prefab.Get() : nullptr;
}

namespace SurfPrefabFactory
{
	static void AddToEditor()
	{
		if (!GEditor)
		{
			return;
		}
		for (const TObjectPtr<UActorFactory>& Existing : GEditor->ActorFactories)
		{
			if (Existing && Existing->IsA<USurfPrefabActorFactory>())
			{
				return;
			}
		}
		GEditor->ActorFactories.Add(NewObject<USurfPrefabActorFactory>(GEditor));
	}

	void Register()
	{
		// The editor collects its factories as it starts; a module that loads
		// before it exists adds its own once it does.
		if (GEditor)
		{
			AddToEditor();
		}
		else
		{
			FCoreDelegates::GetOnPostEngineInit().AddStatic(&AddToEditor);
		}
	}
}

#undef LOCTEXT_NAMESPACE
