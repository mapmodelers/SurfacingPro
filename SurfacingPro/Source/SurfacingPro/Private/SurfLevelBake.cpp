// Copyright MapModelers. All Rights Reserved.

#include "SurfLevelBake.h"
#include "Engine/Level.h"
#include "Engine/World.h"

bool USurfLevelBake::IsOn(const UWorld* World)
{
	ULevel* Level = World ? World->PersistentLevel.Get() : nullptr;
	if (!Level)
	{
		return false;
	}
	// On unless the level says otherwise (S154a): with the bake invisible
	// there is no reason a level should not bake by itself.
	const USurfLevelBake* Data = Cast<USurfLevelBake>(Level->GetAssetUserDataOfClass(USurfLevelBake::StaticClass()));
	return !Data || Data->bAutoBake;
}

void USurfLevelBake::SetOn(UWorld* World, bool bOn)
{
	ULevel* Level = World ? World->PersistentLevel.Get() : nullptr;
	if (!Level)
	{
		return;
	}
	USurfLevelBake* Data = Cast<USurfLevelBake>(Level->GetAssetUserDataOfClass(USurfLevelBake::StaticClass()));
	if (!Data)
	{
		if (bOn)
		{
			return;
		}
		Level->Modify();
		Data = NewObject<USurfLevelBake>(Level, NAME_None, RF_Transactional);
		Level->AddAssetUserData(Data);
	}
	Data->Modify();
	Data->bAutoBake = bOn;
	Level->MarkPackageDirty();
}
