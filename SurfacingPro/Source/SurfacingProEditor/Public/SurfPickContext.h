// Copyright MapModelers. All Rights Reserved.
//
// Everything a pick needs, and nothing it does not.
//
// Picking used to take a viewport client and a click, and reach into both for
// a ray, a cursor position and a way to project points to the screen. That
// tied every pick to a live Slate viewport - which the automation tests run
// without, so no test could ever click anything, and the first time a verb
// was clicked was the first time the author clicked it.
//
// A pick now takes this. The mode builds one from the real view; a test
// builds one from a camera it describes in numbers. Same code either way.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class FEditorViewportClient;
class FViewport;
struct FViewportClick;

struct SURFACINGPROEDITOR_API FSurfPickContext
{
	/** The level the geometry lives in. */
	UWorld* World = nullptr;

	/** The click ray, world space; Direction is unit length. */
	FVector Origin = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;

	/** Where the click or cursor is, in viewport pixels. */
	FVector2D Pixel = FVector2D::ZeroVector;

	/** Viewport size in pixels, for anything that reasons about the screen. */
	FIntPoint Size = FIntPoint(1920, 1080);

	/** World to viewport pixels. False when the point is behind the camera. */
	TFunction<bool(const FVector& World, FVector2D& OutPixel)> WorldToPixel;

	/** The camera, for things drawn or measured against the view. */
	FVector ViewOrigin = FVector::ZeroVector;
	FVector ViewForward = FVector::ForwardVector;

	/** Modifier state at the moment of the pick. */
	bool bCtrl = false;
	bool bShift = false;
	bool bAlt = false;

	FKey Key = EKeys::LeftMouseButton;
	bool bDoubleClick = false;

	bool IsValid() const { return World != nullptr && static_cast<bool>(WorldToPixel); }

	/**
	 * From a live viewport and the click the editor delivered. Builds a scene
	 * view once for the projection, which is what every pick used to do for
	 * itself.
	 */
	static FSurfPickContext FromClick(FEditorViewportClient* ViewportClient, const FViewportClick& Click);

	/** From a live viewport at the current cursor position, for gestures that follow the mouse. */
	static FSurfPickContext FromCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/**
	 * From a live viewport at a pixel that may lie past its edges (S248a): a
	 * locked move follows the cursor round the screen's edges, and a ray
	 * through a pixel outside the view is still a ray.
	 */
	static FSurfPickContext FromPixel(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& Pixel);

	/**
	 * From a camera described in numbers: where it is, which way it looks,
	 * its field of view and size, and the pixel being clicked. Pure
	 * arithmetic, so it works with no viewport at all - which is what lets a
	 * test click.
	 */
	static FSurfPickContext Synthetic(UWorld* World, const FVector& CameraLocation, const FRotator& CameraRotation,
		float FOVDegrees, const FIntPoint& Size, const FVector2D& Pixel);

	/** The pixel a world point lands on, for a synthetic camera. False when behind it. */
	static bool ProjectSynthetic(const FVector& CameraLocation, const FRotator& CameraRotation,
		float FOVDegrees, const FIntPoint& Size, const FVector& World, FVector2D& OutPixel);

	/**
	 * From a flat (orthographic) camera described in numbers: where it is,
	 * which way it looks, how wide the view is in centimetres, its size and
	 * the pixel clicked. The ray runs the camera's way from the pixel's
	 * place on the view plane, as the editor's flat views do.
	 */
	static FSurfPickContext SyntheticFlat(UWorld* World, const FVector& CameraLocation, const FRotator& CameraRotation,
		double ViewWidthCm, const FIntPoint& Size, const FVector2D& Pixel);

	/** The pixel a world point lands on, for a synthetic flat camera. False when behind its plane. */
	static bool ProjectSyntheticFlat(const FVector& CameraLocation, const FRotator& CameraRotation,
		double ViewWidthCm, const FIntPoint& Size, const FVector& World, FVector2D& OutPixel);
};
