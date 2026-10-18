// Copyright MapModelers. All Rights Reserved.
//
// A picture for a prefab (S81a).
//
// A library of assets called PF_Prefab_1 through PF_Prefab_12, all wearing
// the same grey icon, is a library nobody reads. The Content Browser asks
// every asset class for a thumbnail; this draws one - the prefab's own
// geometry, seen from the front and above, in wires - without a render
// scene, a preview world or a capture. Wires because that is what the tool
// draws anyway, and because a wire silhouette survives being 64 pixels wide
// where a lit render does not.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "SurfPrefabThumbnail.generated.h"

UCLASS()
class USurfPrefabThumbnailRenderer : public UDefaultSizedThumbnailRenderer
{
	GENERATED_BODY()

public:
	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
		FRenderTarget* Target, FCanvas* Canvas, bool bAdditionalViewFamily) override;
};
