// Copyright MapModelers. All Rights Reserved.
//
// Material thumbnails rendered by the manager (M-E2).
//
// The same picture the Material Manager shows - the material on the
// calibration cube, on the slot its blend mode says, under the 30-degree
// lens - rendered small and kept: on disk as a PNG under
// <SourceRoot>/Thumbnails/<Name>.png, read back while the asset's file is
// older than it, and in memory for the session. The Materials window shows
// them beside its rows when asked. Nothing is rendered until a picture is
// asked for, and a machine that cannot render (the test editor) gets none
// and says so once.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailHelpers.h"

class UMaterialInterface;
class UStaticMeshComponent;
class UTexture2D;
struct FSlateBrush;

/** The manager's scene, for pictures: the cube with the material on the slot its blend mode says. */
class FSurfMaterialThumbnailScene : public FThumbnailPreviewScene
{
public:
	FSurfMaterialThumbnailScene();

	/** The material on its slot, the other slot the cube's own; null clears both. Centres the mesh in frame. */
	void SetMaterial(UMaterialInterface* Material);

	bool HasMesh() const { return MeshComponent != nullptr; }

protected:
	virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override;
	virtual float GetFOV() const override;

private:
	UStaticMeshComponent* MeshComponent = nullptr;
};

namespace SurfMaterialThumbnails
{
	/** The size a picture is rendered at. */
	int32 Size();

	/** <SourceRoot>/Thumbnails/<AssetName>.png */
	FString CachePathFor(const FString& Root, const FString& AssetName);

	/** True on a machine that can render at all (not the test editor's null RHI). */
	bool CanRender();

	/** Render the material on the cube into pixels, Size by Size, BGRA top-down. False, with the reason, when it cannot. */
	bool Render(UMaterialInterface* Material, int32 InSize, TArray<FColor>& OutPixels, FString& OutError);

	/**
	 * The picture of a material: from memory, else from its PNG under the
	 * source root when that is newer than the asset's file, else rendered now
	 * and written there. Null when it cannot be had; the reason in OutError.
	 */
	UTexture2D* PictureFor(UMaterialInterface* Material, FString& OutError);

	/** The picture as a brush for Slate, kept for the session; null when there is none. */
	const FSlateBrush* BrushFor(UMaterialInterface* Material, FString& OutError);

	/** Forget the pictures held in memory (the files stay); a changed asset is re-read on its own. */
	void Forget();

	/** Forget the pictures and delete their files under the source root, so every one is rendered afresh. Returns how many files went. */
	int32 ForgetFiles();

	/** True while the material's shaders are still compiling: a picture taken now would be blank or the default material, so none is. */
	bool IsCompiling(const UMaterialInterface* Material);
}
