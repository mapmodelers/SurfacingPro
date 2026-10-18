// Copyright MapModelers. All Rights Reserved.

#include "SurfacingProModule.h"

DEFINE_LOG_CATEGORY(LogSurfacingPro);

#define LOCTEXT_NAMESPACE "FSurfacingProModule"

void FSurfacingProModule::StartupModule()
{
	UE_LOG(LogSurfacingPro, Log, TEXT("SurfacingPro runtime module started."));
}

void FSurfacingProModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSurfacingProModule, SurfacingPro)
