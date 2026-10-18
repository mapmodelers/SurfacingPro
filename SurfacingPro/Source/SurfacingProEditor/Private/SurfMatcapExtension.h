// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "SceneView.h"
#include "Engine/World.h"
#include "SurfSettings.h"
#include "SurfPreviewMaterial.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"

/**
 * Clay on everything (S173a): the post-process pass from SurfPreviewMaterial,
 * added to every editor view of the editor world while the mode is up and
 * the tick is on - never to a play view, never while the paint view shows.
 */
class FSurfMatcapViewExtension : public FSceneViewExtensionBase
{
public:
	FSurfMatcapViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	bool bEnabled = false;

	/**
	 * The Paint view is showing (S248a): the family drawn gamma only - no
	 * tonemapper, no exposure, nothing that moves a colour - so a channel at
	 * 1 is full on the screen. Per frame, never saved with the viewport.
	 */
	bool bPaintView = false;

	/**
	 * The unlit view turns post-processing off altogether, so the clay pass
	 * never ran there - "matcap is still showing as all flat shading"
	 * (S181a). While the clay is on the family keeps post-processing with
	 * everything in it that changes a colour turned off - gamma only, the
	 * way the unlit view shows it - so only our pass has work to do.
	 */
	/**
	 * The mode is up (S250a): the gradient behind the unlit views is this
	 * pass's too, painted into the empty sky before the see-through things.
	 */
	bool bBackdrop = false;

	/** Views the gradient was handed to since the last count (the frame record). */
	int32 BackdropViews = 0;

	int32 TakeBackdropCount()
	{
		const int32 Count = BackdropViews;
		BackdropViews = 0;
		return Count;
	}

	/**
	 * The unlit view (S251a): "why not seeing background gradient in
	 * Unlit?" The family is set up before the view mode turns its lighting
	 * flag off, so the flag still read lit here and the pass was never
	 * switched on, while the old quad stood aside for it. The viewport's
	 * own view mode says it.
	 */
	static bool IsUnlitFamily(const FSceneViewFamily& Family)
	{
		if (!Family.EngineShowFlags.Lighting)
		{
			return true;
		}
		if (GEditor && Family.RenderTarget)
		{
			for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
			{
				if (Client && Client->Viewport && static_cast<const FRenderTarget*>(Client->Viewport) == Family.RenderTarget)
				{
					return Client->GetViewMode() == VMI_Unlit;
				}
			}
		}
		return false;
	}

	static bool WantsBackdrop(const FSceneViewFamily& Family)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		return Settings && Settings->bBackdropGradient && !Family.EngineShowFlags.Game && IsUnlitFamily(Family);
	}

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		const bool bClay = bEnabled && Settings && Settings->bMatcap;
		const bool bBack = bBackdrop && !bClay && WantsBackdrop(InViewFamily);
		if ((!bClay && !bPaintView && !bBack) || InViewFamily.EngineShowFlags.Game)
		{
			return;
		}
		const UWorld* World = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		FEngineShowFlags& Flags = InViewFamily.EngineShowFlags;
		if (bClay || bBack)
		{
			Flags.SetPostProcessing(true);
			Flags.SetPostProcessMaterial(true);
		}
		Flags.SetTonemapper(false);
		Flags.SetBloom(false);
		Flags.SetEyeAdaptation(false);
		Flags.SetLocalExposure(false);
		Flags.SetMotionBlur(false);
		Flags.SetDepthOfField(false);
		Flags.SetLensFlares(false);
		Flags.SetVignette(false);
		Flags.SetGrain(false);
		Flags.SetColorGrading(false);
	}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (!Settings || InViewFamily.EngineShowFlags.Game || !InView.State)
		{
			return;
		}
		const UWorld* World = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		if (bEnabled && Settings->bMatcap)
		{
			if (UMaterialInterface* Post = SurfPreviewMaterial::GetMatcapPost())
			{
				Post->OverrideBlendableSettings(InView, 1.0f);
			}
			return;
		}
		if (bBackdrop && WantsBackdrop(InViewFamily) && InView.IsPerspectiveProjection())
		{
			if (UMaterialInterface* Post = SurfPreviewMaterial::GetBackdropPost())
			{
				Post->OverrideBlendableSettings(InView, 1.0f);
				++BackdropViews;
			}
		}
	}

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
	{
		return bEnabled || bPaintView || bBackdrop;
	}
};
