// Copyright MapModelers. All Rights Reserved.
//
// Blender viewport navigation, reproduced.
//
// This is not a key rebind. Unreal and Blender disagree about what almost
// every mouse button means in a viewport, so the only coherent thing is to
// take navigation over and implement the other package's model:
//
//   Middle drag          orbit (turntable: yaw about world up, pitch about
//                        the camera's own right, which is why a Blender view
//                        never rolls) - around the selection when there is one
//   Shift + middle       pan, at a rate that matches the distance to the pivot
//   Ctrl + middle        dolly
//   Wheel                zoom in steps towards the pivot
//   Numpad 1 / 3 / 7     front / right / top, Ctrl for the opposite side
//   Numpad 4 / 6 / 8 / 2 orbit by a fixed step
//   Numpad 9             turn the view all the way round
//   Numpad 5             orthographic and back
//   Home                 frame everything
//
// Unreal's own right-drag fly with WASD is left exactly as it is, and Space
// (in the mode) puts the camera where the cursor points. Between the two,
// the viewport is Blender's for looking and Unreal's for walking.
//
// The editor lets a mode see input first and stops if the mode claims it, so
// all of this is done by claiming: the viewport's own camera code never runs
// for these gestures. The one thing not claimed is the middle-button press
// itself, because letting the editor capture the mouse is what keeps the
// cursor inside the viewport for the length of a drag.
//
// Everything here moves the camera RELATIVE to where it is. The first version
// re-aimed the camera at the pivot on every gesture, which is why taking the
// pivot from the selection made the view jump: the selection was rarely on
// the view axis. An orbit now rotates the camera's position about the centre
// and turns the camera by the same amount, so a view that was looking past
// the selection keeps looking past it as it swings round.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "SurfSettings.h"
#include "EditorViewportClient.h"

class FEditorViewportClient;
class FViewport;

/**
 * Navigation state for one mode. Holds the pivot, because "what am I
 * orbiting around" is a property of the session rather than of the camera,
 * and Unreal's viewport does not keep one in a form this can use.
 */
class SURFACINGPROEDITOR_API FSurfCamera
{
public:
	~FSurfCamera();

	/** True when this scheme should be driving, given the settings. */
	static bool IsBlenderScheme();

	/** Returns true if the key was navigation and has been dealt with. */
	bool HandleKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FKey& Key, EInputEvent Event);

	/** Returns true if this drove the camera, which stops the viewport doing so as well. */
	bool HandleCapturedMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y);

	/** Point the view at a box, keeping the current direction. A flat pane frames by its centre and zoom. */
	void Frame(FEditorViewportClient* ViewportClient, const FBox& Bounds);

	/**
	 * The nearest straight view (S71e): the axis the camera is closest to
	 * already - top, bottom, front, back or either side - keeping the pivot
	 * and the distance, and staying a perspective view so the hand can keep
	 * moving. Alt with the middle button does this; so does the search.
	 */
	void SnapToNearestAxis(FEditorViewportClient* ViewportClient);

	/** One named straight view: Direction is where the camera looks. */
	void SnapToDirection(FEditorViewportClient* ViewportClient, const FVector& Direction);

	/** A flat pane: top, front, side or their opposites - fixed views whose rotation the editor never reads. */
	static bool IsFlatPane(const FEditorViewportClient* ViewportClient);

	/** The way a viewport looks, a flat pane included. */
	static FVector ViewDirectionOf(const FEditorViewportClient* ViewportClient);

	/** The editor's flat view that looks the way given: top for down, and so on. */
	static ELevelViewportType FlatViewFor(const FVector& Direction);

	/**
	 * The point an orbit turns around while there is a selection. Set by the
	 * mode as the selection changes; taken at the start of each drag, so a
	 * selection that changes mid-drag does not move the centre under the
	 * hand.
	 */
	void SetPivot(const FVector& InPivot) { SelectionPivot = InPivot; bHasSelectionPivot = true; }
	void ClearSelectionPivot() { bHasSelectionPivot = false; }

	/**
	 * How to find the point under the cursor, bound by the mode. An orbit
	 * turns about whatever was under the cursor when the drag began - the
	 * surface you are looking at - and about the view's own pivot when
	 * nothing is there.
	 */
	TFunction<bool(FEditorViewportClient*, FViewport*, FVector&)> UnderCursor;

	bool IsNavigating() const { return Drag != EDrag::None; }

	/** Alt with the left button held and moving: orbit, the same way the middle button does. */
	void BeginAltOrbit(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** The button came up. */
	void EndDrag(FEditorViewportClient* ViewportClient = nullptr);

private:
	enum class EDrag : uint8 { None, Orbit, Pan, Dolly };

	/** Latch everything a drag needs at the moment it starts. */
	void BeginDrag(FEditorViewportClient* ViewportClient, FViewport* Viewport, EDrag InDrag, const FKey& Button);

	/** Which button holds the drag; its release ends it. */
	FKey DragButton = EKeys::MiddleMouseButton;

	/** The point the current gesture turns or zooms about. */
	FVector ResolveCentre(FEditorViewportClient* ViewportClient);

	/** Turn the camera about the centre by yaw (world up) and pitch (camera right). */
	void Orbit(FEditorViewportClient* ViewportClient, double DeltaYaw, double DeltaPitch, bool bAroundSelection = false);

	/** Zoom: move the camera towards or away from the centre. Blender's VIEW3D_OT_zoom. */
	void Zoom(FEditorViewportClient* ViewportClient, double Factor);

	/** Slide the camera and its pivot together, by a world-space delta. Pan and dolly. */
	void Slide(FEditorViewportClient* ViewportClient, const FVector& Delta);

	/** Turn to a fixed direction, keeping the centre where it is on screen. Smoothed. */
	void SnapTo(FEditorViewportClient* ViewportClient, const FRotator& Rotation);

	/** Write a transform and remember it, so an external change is detectable. */
	void Write(FEditorViewportClient* ViewportClient, const FVector& Location, const FRotator& Rotation, const FVector& LookAt);

	/**
	 * Pick the pivot back up if anything else moved the camera. Unreal owns
	 * ViewLocation and many paths write it - focus, bookmarks, Alt-drag, the
	 * fly camera - and a pivot that does not notice is how the mode and the
	 * viewport start disagreeing.
	 */
	void ReconcileWithViewport(FEditorViewportClient* ViewportClient);

	/** Run the smooth view, if one is in flight. Called from the ticker. */
	bool TickSmoothView(float DeltaSeconds);

	EDrag Drag = EDrag::None;
	FIntPoint LastMouse = FIntPoint::ZeroValue;

	/** A drag has begun and no move has arrived yet: the first one only says where the drag starts. */
	bool bAwaitingFirstMove = false;

	/** The view's own pivot: a point ahead of the camera along its axis. */
	FVector Pivot = FVector::ZeroVector;
	bool bHasPivot = false;
	double Distance = 500.0;

	/** The selection's centre, when there is one. Wins over the view pivot for orbit. */
	FVector SelectionPivot = FVector::ZeroVector;
	bool bHasSelectionPivot = false;

	/** The centre latched at the start of the drag in progress. */
	FVector DragCentre = FVector::ZeroVector;

	/**
	 * Alt with the middle button: the orbit snaps to the axis views (top,
	 * bottom, front, back, left, right) as the hand turns - Blender's
	 * Alt-MMB. The free rotation is kept so the snap follows the hand
	 * rather than the last snapped view.
	 */
	bool bSnappingOrbit = false;
	FRotator SnapFreeRotation = FRotator::ZeroRotator;
	FRotator SnapLastApplied = FRotator::ZeroRotator;

	/** Re-aim the camera at a centre from a rotation, at its current distance from it. */
	void AimAt(FEditorViewportClient* ViewportClient, const FVector& Centre, const FRotator& Rotation);

	/** What we last wrote, so an external change to the camera is detectable. */
	FVector LastWrittenLocation = FVector::ZeroVector;
	FRotator LastWrittenRotation = FRotator::ZeroRotator;

	/**
	 * Which way a horizontal drag turns, latched when the drag starts. Blender
	 * reverses it when the view is upside down so the scene still follows the
	 * hand, and latches it so dragging through the pole does not flip halfway.
	 */
	double YawSign = 1.0;

	/** Pan speed latched at the start of the drag: Blender's zfac, taken once. */
	double PanPerPixel = 1.0;

	// --- Smooth view -------------------------------------------------------
	// Blender eases a snapped view over 200 ms with smoothstep. An instant
	// snap reads as a glitch rather than a move.
	FEditorViewportClient* SmoothClient = nullptr;
	FVector SmoothFromLocation = FVector::ZeroVector;
	FVector SmoothToLocation = FVector::ZeroVector;
	FQuat SmoothFromRotation = FQuat::Identity;
	FQuat SmoothToRotation = FQuat::Identity;
	FVector SmoothLookAt = FVector::ZeroVector;
	double SmoothElapsed = 0.0;
	double SmoothDuration = 0.0;
	FTSTicker::FDelegateHandle SmoothTicker;
};
