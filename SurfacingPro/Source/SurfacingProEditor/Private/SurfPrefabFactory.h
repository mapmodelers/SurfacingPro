// Copyright MapModelers. All Rights Reserved.
//
// Dragging a prefab asset from the Content Browser into a level places an
// instance of it (S65a). The engine finds this factory by the asset's class.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "SurfPrefabFactory.generated.h"

UCLASS()
class USurfPrefabActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	USurfPrefabActorFactory(const FObjectInitializer& ObjectInitializer);

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* Instance) override;
};

namespace SurfPrefabFactory
{
	/** Add the factory to the editor's list, once, when the editor exists. */
	void Register();
}
