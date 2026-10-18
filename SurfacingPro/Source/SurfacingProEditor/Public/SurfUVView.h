// Copyright MapModelers. All Rights Reserved.
//
// The UV view (S62).
//
// The mapping is a rule, not a result (D24/D27), so there has never been
// anywhere to look at what the rule produced. This is that place: the
// selected patches' UV nets, over the tile grid, live - move a control point
// in the viewport and the net moves here, press Flow or Fit and the whole net
// changes shape. Points are picked and dragged here too, and the selection is
// the one thing: a point picked in the window is picked in the viewport.
//
// It is a dockable tab rather than a viewport pane, because a level viewport's
// panes host only level viewports. Drag it beside the perspective view and it
// stays where it was put.
//
// The maths lives in SurfUV, apart from the widget, so the framing, the zoom
// and the picking are tested rather than eyeballed.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "SurfEdMode.h"

class FScopedTransaction;
class USurfEdMode;

/** One patch's UV net, as the view draws it. */
struct FSurfUVNet
{
	FSurfPatchRef Patch;
	int32 Width = 0;
	int32 Height = 0;

	/** Row major, Width * Height, exactly as the control points are. */
	TArray<FVector2D> UVs;
	TArray<bool> Selected;

	/** A frozen (Manual) patch: its UVs are held, so they are the ones that can be moved point by point. */
	bool bFrozen = false;

	/** The active patch, drawn brighter: the one every surfacing verb reads its numbers from. */
	bool bActive = false;

	/**
	 * Per cell, (Width - 1) * (Height - 1), row major: how far the cell's
	 * share of the texture strays from its share of the surface, as log2 of
	 * the ratio against the net's median - 0 even, -1 half (tight), +1
	 * twice (stretched). Empty when the net has no cells.
	 */
	TArray<float> Stretch;

	const FVector2D& At(int32 X, int32 Y) const { return UVs[Y * Width + X]; }
	bool IsSelected(int32 X, int32 Y) const { return Selected[Y * Width + X]; }
};

namespace SurfUV
{
	/** The UV nets of every patch the selection reaches into, active first. */
	void GatherNets(const USurfEdMode& Mode, TArray<FSurfUVNet>& Out);

	/** The box the nets cover; the unit tile when there is nothing to cover. */
	FBox2D BoundsOf(const TArray<FSurfUVNet>& Nets);

	/** The stretch colour for a cell's log2 ratio: blue at -1, green at 0, red at +1. */
	FLinearColor StretchColour(float Log);

	/**
	 * How the UV plane sits in the widget: the UV at the top left pixel, and
	 * how many pixels a UV unit is. V grows downward, as it does on a texture.
	 */
	struct FView
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Scale = 240.0;

		FVector2D ToPixel(const FVector2D& UV) const { return (UV - Origin) * Scale; }
		FVector2D ToUV(const FVector2D& Pixel) const { return Origin + Pixel / FMath::Max(1.e-6, Scale); }
	};

	/** The view that fits Bounds into a widget of this size, with a margin in pixels. */
	FView Frame(const FBox2D& Bounds, const FVector2D& WidgetSize, double MarginPixels = 24.0);

	/** Zoom about a pixel, so the UV under the cursor stays under it. */
	FView ZoomAt(const FView& From, const FVector2D& Pixel, double Factor);

	/** The net point nearest a pixel, within a radius; false when nothing is near enough. */
	bool PickPoint(const TArray<FSurfUVNet>& Nets, const FView& View, const FVector2D& Pixel,
		double RadiusPixels, FSurfPointRef& OutPoint);

	/** Every net point inside a pixel rectangle. */
	void PointsInBox(const TArray<FSurfUVNet>& Nets, const FView& View, const FBox2D& PixelBox,
		TArray<FSurfPointRef>& Out);

	/**
	 * The net line nearest a pixel, within a radius (S234a): its two points,
	 * A the lower index. False when nothing is near enough.
	 */
	bool PickEdge(const TArray<FSurfUVNet>& Nets, const FView& View, const FVector2D& Pixel,
		double RadiusPixels, FSurfPointRef& OutA, FSurfPointRef& OutB);
}

/** The tab's contents: the nets, the tile grid, the picking, and the drag. */
class SSurfUVView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfUVView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSurfUVView();

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	/** The keys follow the cursor (S207a): "in uv mode im trying to G (move) selection it doesnt work" - G went to the viewport, which had the focus. */
	virtual void OnMouseEnter(const FGeometry& Geometry, const FPointerEvent& Event) override;
	/** Whether the keys may come to the view from what holds them: not from a field being typed in. */
	bool TakeKeysFrom(const TSharedPtr<SWidget>& Focused) const;

	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;

private:
	USurfEdMode* LiveMode() const;

	/** Put the selection in view; done once when it changes, and on F. */
	void FrameSelection(const FVector2D& WidgetSize);

	/** Close the drag's transaction, if one is open. */
	void EndDrag();

	/**
	 * G, R and S, as in the viewport: press, move the mouse, click or Enter to
	 * keep it, Escape to drop it. X and Y hold it to one axis; a typed number
	 * says it exactly.
	 */
	enum class EUVModal : uint8 { None, Move, Turn, Scale };

	void BeginModal(EUVModal Kind, const FGeometry& Geometry);
	void UpdateModal(const FVector2D& Pixel);
	void ApplyTypedModal();
	void EndModal(bool bCancel);
	void ReportModal() const;

	/** The texture behind the nets (S207a) and its brush, made again when the texture changes. */
	mutable TWeakObjectPtr<class UTexture> BackdropTexture;
	mutable TSharedPtr<struct FSlateBrush> BackdropBrush;

	mutable SurfUV::FView View;
	mutable FVector2D LastSize = FVector2D(400.0, 400.0);

	/** What the nets looked like when the view was last framed, so a new selection frames itself once. */
	uint32 FramedOn = 0;

	bool bPanning = false;
	FVector2D PanFrom = FVector2D::ZeroVector;

	/** A left drag on a point moves UVs; one on empty space draws a box. */
	bool bDragging = false;
	bool bBoxing = false;
	bool bBoxAdds = false;
	bool bBoxTakes = false;
	FVector2D DragFrom = FVector2D::ZeroVector;
	/** The point in hand during a drag, and where it would be with no snap (S230a). */
	FSurfPointRef DragAnchor;
	FVector2D DragAnchorFree = FVector2D::ZeroVector;
	FVector2D BoxFrom = FVector2D::ZeroVector;
	FVector2D BoxTo = FVector2D::ZeroVector;
	TUniquePtr<FScopedTransaction> DragTransaction;

	EUVModal Modal = EUVModal::None;
	FVector2D ModalFrom = FVector2D::ZeroVector;
	/** How far the cursor was carried round the window during G R S (S188a): added to where it is. */
	FVector2D ModalWrap = FVector2D::ZeroVector;
	FVector2D ModalPivotPixel = FVector2D::ZeroVector;
	FVector2D ModalPivotUV = FVector2D::ZeroVector;

	/** How far the modal has got: tiles moved, degrees turned, or the factor. */
	FVector2D ModalMoved = FVector2D::ZeroVector;
	double ModalTurned = 0.0;
	double ModalScaled = 1.0;

	/** -1 both, 0 U, 1 V. */
	int32 ModalAxis = -1;
	FString ModalTyped;
	TUniquePtr<FScopedTransaction> ModalTransaction;
};

namespace SurfUVTab
{
	/** "SurfacingProUV", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward if it is already open. */
	void Open();
}
