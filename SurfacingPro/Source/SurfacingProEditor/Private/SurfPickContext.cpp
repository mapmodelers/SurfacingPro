// Copyright MapModelers. All Rights Reserved.

#include "SurfPickContext.h"

#include "EditorViewportClient.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace
{
	/**
	 * A scene view for the projection, kept alive in the closure.
	 *
	 * WorldToPixel needs the view family the view was built from to still
	 * exist, and the pick may run long after the builder has returned, so
	 * both live in a shared block that the closure owns.
	 */
	struct FProjection
	{
		TUniquePtr<FSceneViewFamilyContext> Family;
		FSceneView* View = nullptr;
	};

	TSharedPtr<FProjection> MakeProjection(FEditorViewportClient* ViewportClient, FViewport* Viewport)
	{
		if (!ViewportClient || !Viewport)
		{
			return nullptr;
		}

		TSharedPtr<FProjection> Projection = MakeShared<FProjection>();

		Projection->Family = MakeUnique<FSceneViewFamilyContext>(FSceneViewFamily::ConstructionValues(
			Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags)
			.SetRealtimeUpdate(ViewportClient->IsRealtime()));

		Projection->View = ViewportClient->CalcSceneView(Projection->Family.Get());

		return Projection->View ? Projection : nullptr;
	}

	void FillFromViewport(FSurfPickContext& Out, FEditorViewportClient* ViewportClient, FViewport* Viewport,
		const TSharedPtr<FProjection>& Projection)
	{
		Out.World = ViewportClient->GetWorld();
		Out.Size = Viewport ? Viewport->GetSizeXY() : FIntPoint(1920, 1080);
		Out.ViewOrigin = ViewportClient->GetViewLocation();
		Out.ViewForward = ViewportClient->GetViewRotation().Vector();

		Out.bCtrl = ViewportClient->IsCtrlPressed();
		Out.bShift = ViewportClient->IsShiftPressed();
		Out.bAlt = ViewportClient->IsAltPressed();

		if (Projection.IsValid())
		{
			Out.WorldToPixel = [Projection](const FVector& World, FVector2D& OutPixel) -> bool
			{
				return Projection->View && Projection->View->WorldToPixel(World, OutPixel);
			};
		}
	}
}

FSurfPickContext FSurfPickContext::FromClick(FEditorViewportClient* ViewportClient, const FViewportClick& Click)
{
	FSurfPickContext Out;
	if (!ViewportClient)
	{
		return Out;
	}

	FillFromViewport(Out, ViewportClient, ViewportClient->Viewport, MakeProjection(ViewportClient, ViewportClient->Viewport));

	Out.Origin = Click.GetOrigin();
	Out.Direction = Click.GetDirection().GetSafeNormal();
	Out.Pixel = FVector2D(Click.GetCursorPos().X, Click.GetCursorPos().Y);
	Out.Key = Click.GetKey();
	Out.bDoubleClick = Click.GetEvent() == IE_DoubleClick;

	// The click's own record of the modifiers, which is what was true at the
	// moment it happened rather than now.
	Out.bCtrl = Click.IsControlDown();
	Out.bShift = Click.IsShiftDown();
	Out.bAlt = Click.IsAltDown();

	return Out;
}

FSurfPickContext FSurfPickContext::FromCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	FSurfPickContext Out;
	if (!ViewportClient || !Viewport)
	{
		return Out;
	}

	TSharedPtr<FProjection> Projection = MakeProjection(ViewportClient, Viewport);
	FillFromViewport(Out, ViewportClient, Viewport, Projection);

	FIntPoint Cursor;
	Viewport->GetMousePos(Cursor);
	Out.Pixel = FVector2D(Cursor.X, Cursor.Y);

	if (Projection.IsValid() && Projection->View)
	{
		const FViewportCursorLocation Location(Projection->View, ViewportClient, Cursor.X, Cursor.Y);
		Out.Origin = Location.GetOrigin();
		Out.Direction = Location.GetDirection().GetSafeNormal();
	}

	return Out;
}

FSurfPickContext FSurfPickContext::FromPixel(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& Pixel)
{
	FSurfPickContext Out;
	if (!ViewportClient || !Viewport)
	{
		return Out;
	}

	TSharedPtr<FProjection> Projection = MakeProjection(ViewportClient, Viewport);
	FillFromViewport(Out, ViewportClient, Viewport, Projection);
	Out.Pixel = Pixel;

	if (Projection.IsValid() && Projection->View)
	{
		const FViewportCursorLocation Location(Projection->View, ViewportClient, FMath::RoundToInt(Pixel.X), FMath::RoundToInt(Pixel.Y));
		Out.Origin = Location.GetOrigin();
		Out.Direction = Location.GetDirection().GetSafeNormal();
	}

	return Out;
}

bool FSurfPickContext::ProjectSynthetic(const FVector& CameraLocation, const FRotator& CameraRotation,
	float FOVDegrees, const FIntPoint& Size, const FVector& World, FVector2D& OutPixel)
{
	// Into the camera's own frame: X forward, Y right, Z up.
	const FVector Local = FRotationMatrix(CameraRotation).InverseTransformVector(World - CameraLocation);
	if (Local.X <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const double HalfTan = FMath::Tan(FMath::DegreesToRadians(FOVDegrees * 0.5f));
	const double Aspect = static_cast<double>(FMath::Max(1, Size.X)) / FMath::Max(1, Size.Y);

	// The horizontal field of view is the one given, as in Unreal's own view.
	const double NdcX = (Local.Y / Local.X) / HalfTan;
	const double NdcY = (Local.Z / Local.X) / (HalfTan / Aspect);

	OutPixel.X = (NdcX + 1.0) * 0.5 * Size.X;
	OutPixel.Y = (1.0 - NdcY) * 0.5 * Size.Y;
	return true;
}

FSurfPickContext FSurfPickContext::Synthetic(UWorld* World, const FVector& CameraLocation, const FRotator& CameraRotation,
	float FOVDegrees, const FIntPoint& Size, const FVector2D& Pixel)
{
	FSurfPickContext Out;
	Out.World = World;
	Out.Size = Size;
	Out.Pixel = Pixel;
	Out.ViewOrigin = CameraLocation;
	Out.ViewForward = CameraRotation.Vector();

	// The ray through the pixel: the exact inverse of ProjectSynthetic, so a
	// point that projects to a pixel is hit by the ray through that pixel.
	const double HalfTan = FMath::Tan(FMath::DegreesToRadians(FOVDegrees * 0.5f));
	const double Aspect = static_cast<double>(FMath::Max(1, Size.X)) / FMath::Max(1, Size.Y);

	const double NdcX = (Pixel.X / Size.X) * 2.0 - 1.0;
	const double NdcY = 1.0 - (Pixel.Y / Size.Y) * 2.0;

	const FVector Local(1.0, NdcX * HalfTan, NdcY * (HalfTan / Aspect));

	Out.Origin = CameraLocation;
	Out.Direction = FRotationMatrix(CameraRotation).TransformVector(Local).GetSafeNormal();

	Out.WorldToPixel = [CameraLocation, CameraRotation, FOVDegrees, Size](const FVector& World, FVector2D& OutPixel) -> bool
	{
		return ProjectSynthetic(CameraLocation, CameraRotation, FOVDegrees, Size, World, OutPixel);
	};

	return Out;
}

bool FSurfPickContext::ProjectSyntheticFlat(const FVector& CameraLocation, const FRotator& CameraRotation,
	double ViewWidthCm, const FIntPoint& Size, const FVector& World, FVector2D& OutPixel)
{
	// Into the camera's own frame: X forward, Y right, Z up. No perspective:
	// a centimetre is the same number of pixels at any depth.
	const FVector Local = FRotationMatrix(CameraRotation).InverseTransformVector(World - CameraLocation);
	if (Local.X <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const double HalfWidth = FMath::Max(1.0, ViewWidthCm * 0.5);
	const double Aspect = static_cast<double>(FMath::Max(1, Size.X)) / FMath::Max(1, Size.Y);

	const double NdcX = Local.Y / HalfWidth;
	const double NdcY = Local.Z / (HalfWidth / Aspect);

	OutPixel.X = (NdcX + 1.0) * 0.5 * Size.X;
	OutPixel.Y = (1.0 - NdcY) * 0.5 * Size.Y;
	return true;
}

FSurfPickContext FSurfPickContext::SyntheticFlat(UWorld* World, const FVector& CameraLocation, const FRotator& CameraRotation,
	double ViewWidthCm, const FIntPoint& Size, const FVector2D& Pixel)
{
	FSurfPickContext Out;
	Out.World = World;
	Out.Size = Size;
	Out.Pixel = Pixel;
	Out.ViewOrigin = CameraLocation;
	Out.ViewForward = CameraRotation.Vector();

	// The ray through the pixel: from the pixel's place on the view plane,
	// the camera's way - the exact inverse of ProjectSyntheticFlat.
	const double HalfWidth = FMath::Max(1.0, ViewWidthCm * 0.5);
	const double Aspect = static_cast<double>(FMath::Max(1, Size.X)) / FMath::Max(1, Size.Y);

	const double NdcX = (Pixel.X / Size.X) * 2.0 - 1.0;
	const double NdcY = 1.0 - (Pixel.Y / Size.Y) * 2.0;

	const FVector Local(0.0, NdcX * HalfWidth, NdcY * (HalfWidth / Aspect));

	Out.Origin = CameraLocation + FRotationMatrix(CameraRotation).TransformVector(Local);
	Out.Direction = Out.ViewForward.GetSafeNormal();

	Out.WorldToPixel = [CameraLocation, CameraRotation, ViewWidthCm, Size](const FVector& World, FVector2D& OutPixel) -> bool
	{
		return ProjectSyntheticFlat(CameraLocation, CameraRotation, ViewWidthCm, Size, World, OutPixel);
	};

	return Out;
}
