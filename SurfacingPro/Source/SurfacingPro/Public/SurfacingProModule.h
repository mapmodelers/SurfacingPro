// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

SURFACINGPRO_API DECLARE_LOG_CATEGORY_EXTERN(LogSurfacingPro, Log, All);

class FSurfacingProModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
