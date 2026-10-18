// Copyright MapModelers. All Rights Reserved.
//
// The editor mode. Selection is ours (D13): our own hit test against the
// brush hulls, our own selection set, our own highlight. Nothing is
// registered with Typed Elements, the Outliner never shows a primitive, and
// no verb can drop the selection because no framework owns it.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Tools/LegacyEdModeWidgetHelpers.h"
#include "SurfTypes.h"
#include "SurfKeymap.h"
#include "SurfPickContext.h"
#include "SurfSettings.h"
#include "SurfPrefab.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Misc/ITransaction.h"
#include "SurfCamera.h"   // complete type: the generated destructor deletes the TUniquePtr<FSurfCamera>
#include "SurfEdMode.generated.h"

class ASurfGroup;
class USurfInspector;
class FLevelEditorViewportClient;

// A bake in progress, held while it runs a slice at a time (S106a). Declared
// rather than included: the bake is the editor module's own business and its
// header is private to it.
namespace SurfBake { struct FJob; }

/**
 * A modal transform, the way a modelling package means it: press the key and
 * the selection follows the mouse until you confirm or cancel. No gizmo, no
 * axis to grab first.
 */
UENUM()
enum class ESurfModal : uint8
{
	None,
	Move,
	Rotate,
	Scale,
	/** Second press of the move key: points slide along their own grid lines. */
	Slide
};

UENUM()
enum class ESurfAxisLock : uint8
{
	None,
	X,
	Y,
	Z,

	// A second press of the same key locks to the plane instead of the axis:
	// "not X" is as often what is wanted as "only X", and it is one keypress
	// away rather than two.
	NotX,
	NotY,
	NotZ,

	// A direction the gesture supplied rather than the user: extrude locks
	// the move to the way the surface continues, so pulling an edge out
	// pulls it outward rather than wherever the view plane happens to lie.
	Custom
};

/** Which kind of thing clicking selects. Keys 1, 2 and 3 (D10). */
UENUM()
enum class ESurfSubMode : uint8
{
	Object,
	Face,
	/**
	 * A whole grid line at a time. Split, extrude and bevel all take a line,
	 * so selecting one directly is less work than selecting every point on it
	 * and hoping they add up to the same thing.
	 */
	Edge,
	Vertex
};

/**
 * The grid segment nearest the cursor, measured on screen in pixels (S83a).
 * Split out of the selecting half so one Edit mode can ask what is under the
 * cursor without selecting it.
 */
struct FSurfEdgeHit
{
	class ASurfGroup* Group = nullptr;
	FGuid PatchId;
	double Distance = TNumericLimits<double>::Max();
	bool bColumn = false;
	int32 Line = INDEX_NONE;
	int32 Along = INDEX_NONE;

	bool IsValid() const { return Group != nullptr && Line != INDEX_NONE; }
};

/** Every verb whose result can be adjusted after the fact. */
enum class ESurfOpKind : uint8
{
	None,
	Move,
	Rotate,
	Scale,
	Extrude,
	Bevel,
	Smooth,
	Inflate,
	Flatten,
	Solidify,
	Duplicate,
	WeldByDistance,

	/** Each cluster of selected points within a reach, to its own centre. */
	WeldClusters,

	/** A line between every pair of lines, rows and/or columns, once or more. */
	Subdivide,

	/** A mapping set on every patch the selection holds: Flow, Align, Project or Fit. */
	Surface,

	/** A staircase between two edges: how many steps, or how tall one is. */
	Steps,

	/** A patch made from nothing: how many points, how big, what shape. */
	NewSheet,

	/** Every selected point pushed a random way, up to a distance, on chosen axes or along the normal (S157a, A22). */
	Random,

	/** A lattice round the selected patches: its points across, deep and up (S189a). */
	Lattice
};

/**
 * Which mapping a surfacing verb sets. The public names, which are what the
 * bar says: Flow follows the surface (Natural), Align projects from each
 * patch's own normal (Planar), Project gives every patch in the selection one
 * projection along a world axis - so a wall that bends has no seam - and Fit
 * picks the scale that spans a whole number of tiles.
 */
enum class ESurfSurfaceVerb : uint8
{
	Flow,
	Align,
	Project,
	Fit,

	/** Every patch to the same texels per metre, by its own scale. */
	Normalise,

	/** Even (S69c): every cell the same size in the texture, one tile a cell to start with. */
	Even
};

/**
 * The last thing done, with the numbers it was done with - so they can be
 * changed afterwards and the thing done again. Adjusting undoes the operation
 * and runs it once more with the new numbers, against the selection undo
 * hands back; so it is exact, and it is only offered while the operation is
 * still the last thing in the undo history.
 */
struct FSurfOperation
{
	ESurfOpKind Kind = ESurfOpKind::None;

	/** Move: world centimetres. */
	FVector Delta = FVector::ZeroVector;

	/** Rotate: about the selection's centre. */
	FRotator Rotation = FRotator::ZeroRotator;

	/** Scale: a multiplier per axis, about the selection's centre. */
	FVector Scale = FVector::OneVector;

	/** Extrude offset, inflate distance, solidify thickness, duplicate offset, weld reach - centimetres. */
	double Distance = 0.0;

	/** Smooth and flatten strength; bevel roundness. */
	double Strength = 0.5;
	int32 Iterations = 4;

	/** Lattice: the points up (Iterations across, Segments deep). */
	int32 Layers = 2;
	bool bBorder = true;

	/** Bevel. WidthLimit is how far it can go before meeting the next line, measured when it ran. bHard keeps the crease and only adds lines either side. */
	double Width = 50.0;
	double WidthLimit = 500.0;
	int32 Segments = 2;
	/** The most segments the grid has room for, over every line the bevel takes, measured when it ran. */
	int32 SegmentsLimit = 8;
	bool bHard = false;

	/** Subdivide: which way, and how many times over (each doubles the lines). */
	bool bRows = true;
	bool bColumns = true;

	/** Surface: which mapping, and the numbers it used. */
	ESurfSurfaceVerb SurfaceVerb = ESurfSurfaceVerb::Flow;

	/** Metres per tile. Zero or less means "leave each patch's own alone", and is filled in with the active patch's when the verb runs, so the knob starts where the surface is. */
	FVector2D Tiles = FVector2D::ZeroVector;

	/** The mapping's turn, in degrees. Set, not accumulated: the number is what you get. */
	double SurfaceRotation = 0.0;

	/** Fit: how many tiles across the patch each way. */
	int32 RepeatU = 1;
	int32 RepeatV = 1;

	/** Project: 0 the selection's own direction, 1 X, 2 Y, 3 Z. */
	int32 Axis = 0;

	/** Normalise: texels per metre to match. Zero means "the active patch's", filled in when the verb runs. */
	double Density = 0.0;

	/**
	 * How the knobs reach the other patches (§2c). Relative, the default:
	 * each patch changes from its own value by what the knob did to the
	 * reference's - twice the metres a tile on the reference is twice on
	 * every patch, a turn of ten degrees is ten more on each. Match: every
	 * patch takes the knob's number outright, for hard alignment.
	 */
	bool bMatchFirst = false;

	/** The reference patch's scale and turn before the verb, so a relative knob has something to be relative to. Filled in the first time the verb runs. */
	bool bReferenceKnown = false;
	FVector2D ReferenceTiles = FVector2D(1.0, 1.0);
	double ReferenceRotation = 0.0;

	/** Solidify: which side the shell grows (-1 behind, 0 half each way, +1 in front), whether it has its rim, whether the thickness is kept even at folds. */
	int32 Side = -1;
	bool bRim = true;
	bool bEven = false;

	/** Extrude: what becomes of the faces beneath a lid; and whether the extrude came with a move (E), in Delta. */
	ESurfExtrudeUnder Under = ESurfExtrudeUnder::Drop;
	bool bMoved = false;

	/** How many undo steps the operation took (an extrude followed by a move is two until it is replayed as one). */
	int32 UndoSteps = 1;

	/** New patch: which of ESurfSheetPreset it starts as (S111a). */
	int32 Preset = 0;

	/** How far a knob moves per pixel of drag, in cm: from the cell size of the patch the operation ran on. 0 until noted. */
	double SliderRange = 0.0;

	/** The undo history as it stood right after the operation. Anything else happening since makes it stale. */
	int32 QueueLengthAfter = -1;
	int32 UndoCountAfter = -1;
};

/** One patch. */
struct FSurfPatchRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid PatchId;

	bool IsValid() const { return Group.IsValid() && PatchId.IsValid(); }
	bool operator==(const FSurfPatchRef& Other) const { return Group == Other.Group && PatchId == Other.PatchId; }
};

/** One control point of one patch. */
/**
 * One corner of one brush (S112a): which brush, and which of the hull's
 * vertices.
 *
 * The index is into the hull derived from the brush's planes, and the hull is
 * derived fresh every time it is asked for, so the index means what it meant
 * only while the brush has not been rebuilt. Every use here derives and reads
 * in the same breath, which is the only way an index into a derived thing is
 * safe to hold at all.
 */
struct FSurfBrushCornerRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid BrushId;
	int32 Corner = INDEX_NONE;

	bool IsValid() const { return Group.IsValid() && BrushId.IsValid() && Corner >= 0; }
	bool operator==(const FSurfBrushCornerRef& Other) const
	{
		return Group == Other.Group && BrushId == Other.BrushId && Corner == Other.Corner;
	}
};

struct FSurfPointRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid PatchId;
	int32 X = INDEX_NONE;
	int32 Y = INDEX_NONE;

	bool IsValid() const { return Group.IsValid() && PatchId.IsValid() && X >= 0 && Y >= 0; }
	bool operator==(const FSurfPointRef& Other) const
	{
		return Group == Other.Group && PatchId == Other.PatchId && X == Other.X && Y == Other.Y;
	}
};

FORCEINLINE uint32 GetTypeHash(const FSurfPatchRef& Ref)
{
	return HashCombine(GetTypeHash(Ref.Group), GetTypeHash(Ref.PatchId));
}

FORCEINLINE uint32 GetTypeHash(const FSurfPointRef& Ref)
{
	return HashCombine(HashCombine(GetTypeHash(Ref.PatchId), GetTypeHash(Ref.X)), GetTypeHash(Ref.Y));
}

/**
 * One cell of a patch's control grid - the patch equivalent of a brush face.
 *
 * Named by the corner it starts at, so the cell (X, Y) is bounded by the
 * control points (X, Y), (X+1, Y), (X+1, Y+1) and (X, Y+1). There is no
 * stored list of faces on a patch to index into: a grid's faces are implied
 * by its dimensions, and naming them by corner survives rows and columns
 * being inserted either side.
 */
struct FSurfQuadRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid PatchId;
	int32 X = INDEX_NONE;
	int32 Y = INDEX_NONE;

	bool IsValid() const { return Group.IsValid() && PatchId.IsValid() && X >= 0 && Y >= 0; }
	bool operator==(const FSurfQuadRef& Other) const
	{
		return Group == Other.Group && PatchId == Other.PatchId && X == Other.X && Y == Other.Y;
	}
};

FORCEINLINE uint32 GetTypeHash(const FSurfQuadRef& Ref)
{
	return HashCombine(HashCombine(GetTypeHash(Ref.PatchId), GetTypeHash(Ref.X)), GetTypeHash(Ref.Y));
}

/** A patch taken from a group by duplicate or copy, before it is put anywhere. */
struct FSurfPatchCopy
{
	TWeakObjectPtr<ASurfGroup> Group;
	FSurfPatch Patch;
};

/** One picked brush face. Faces are identified by primitive id, not by pointer. */
struct FSurfFaceRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid BrushId;
	int32 FaceIndex = INDEX_NONE;

	bool IsValid() const { return Group.IsValid() && BrushId.IsValid() && FaceIndex != INDEX_NONE; }

	bool operator==(const FSurfFaceRef& Other) const
	{
		return Group == Other.Group && BrushId == Other.BrushId && FaceIndex == Other.FaceIndex;
	}
};

/** A patch or a primitive, whole (S249b): what a group is made of. */
struct FSurfPieceRef
{
	TWeakObjectPtr<ASurfGroup> Group;
	FGuid Id;
	bool bBrush = false;

	bool operator==(const FSurfPieceRef& Other) const { return Group == Other.Group && Id == Other.Id && bBrush == Other.bBrush; }
};

FORCEINLINE uint32 GetTypeHash(const FSurfPieceRef& Ref)
{
	return HashCombine(GetTypeHash(Ref.Id), Ref.bBrush ? 1u : 0u);
}

namespace SurfGroupPaths
{
	/** A copy's groups are groups of their own: each id swapped for a fresh one, the same fresh one across the copies. */
	void Fresh(TArray<FGuid>& Path, TMap<FGuid, FGuid>& NewIdOf);
}

UCLASS()
class SURFACINGPROEDITOR_API USurfEdMode : public UBaseLegacyWidgetEdMode, public ILegacyEdModeSelectInterface
{
	GENERATED_BODY()

public:
	static const FEditorModeID EM_SurfacingPro;

	USurfEdMode();

	virtual void Enter() override;
	virtual void Exit() override;
	virtual void CreateToolkit() override;

	// Render comes from ILegacyEdModeWidgetInterface, HandleClick from
	// ILegacyEdModeViewportInterface; UBaseLegacyWidgetEdMode supplies both.
	virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click) override;

	/**
	 * The click itself, given a context. This is what the tests call: a
	 * context built from a camera described in numbers goes through exactly
	 * the path a real click takes. The viewport client may be null; it is
	 * only used by the loop cut, which needs the live cursor.
	 */
	/**
	 * A click, from wherever it came.
	 *
	 * bKeepOnMiss: a press that found nothing leaves the selection alone
	 * rather than dropping it (S149a). For the gizmo's ball, which falls
	 * through to a pick when a press turns nothing - the ball sits over the
	 * middle of what is selected, so a press on it that goes nowhere was
	 * throwing away the very selection it was about to turn.
	 */
	bool HandleClickWith(const FSurfPickContext& Ctx, FEditorViewportClient* InViewportClient, bool bKeepOnMiss = false);
	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event) override;
	virtual bool InputDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale) override;
	virtual bool StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
	virtual bool EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
	virtual FVector GetWidgetLocation() const override;
	virtual bool ShouldDrawWidget() const override;
	virtual bool UsesTransformWidget() const override;

	/** While our camera has a drag, the editor's own camera must not act on the same mouse. */
	virtual bool DisallowMouseDeltaTracking() const override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	/** The viewport is in Game view and ours are put away (S230a): nothing of ours is drawn or shown over it. */
	bool IsGameViewOf(const FViewport* Viewport) const;
	/** Ours shown over the game view after all (S231a: no setting - "the user may just toggle it back on"); forgotten each time the game view is entered. */
	bool bOursInGameView = false;
	/** The brush armed with nothing picked (S241a): it paints whatever patch it reaches. */
	bool bPaintAnything = false;
	void ToggleOursInGameView();

	/**
	 * Marquee selection, from ILegacyEdModeSelectInterface. The editor draws
	 * the rubber band and hands us the volume; in vertex sub-mode that is how
	 * a whole region of control points gets grabbed at once, which is the only
	 * practical way to edit a large sheet.
	 */
	virtual bool BoxSelect(FBox& InBox, bool InSelect = true) override;
	virtual bool FrustumSelect(const FConvexVolume& InFrustum, FEditorViewportClient* InViewportClient, bool InSelect = true) override;

	/** The current face selection. Never cleared by a verb (D16.1). */
	/**
	 * The sides that are picked - the selection, and the active one (S145a).
	 *
	 * "s should scale only this face. its bug atm", after S141a had already
	 * made S size the picked sides. It had, and the sides were not picked:
	 * **a box fresh from the bar has its top in `ActiveFace` and nothing in
	 * `Selection` at all**, so every test of "are any sides picked" said no
	 * and every one of them fell back to the whole brush. Push had the same
	 * hole, and so did the pivot.
	 *
	 * The face under the highlight is picked. Anything that asks which sides
	 * to act on asks here.
	 */
	TArray<FSurfFaceRef> PickedFaces() const;

	const TArray<FSurfFaceRef>& GetSelection() const { return Selection; }

	/** The face a copy takes its definition from: the most recently clicked one. */
	const FSurfFaceRef& GetActiveFace() const { return ActiveFace; }

	void ClearSelection();

	// --- Sub-modes and patches ----------------------------------------------

	ESurfSubMode GetSubMode() const { return SubMode; }
	void SetSubMode(ESurfSubMode InSubMode);

	/** The kind a click settled on, which is not the hand choosing one (S83a). */
	void AdoptKind(ESurfSubMode Kind);

	const FSurfPatchRef& GetSelectedPatch() const { return SelectedPatch; }
	const TArray<FSurfPointRef>& GetPointSelection() const { return PointSelection; }

	void SelectPatch(const FSurfPatchRef& Patch, bool bKeepPoints = false);

	/**
	 * The patches taken into points, edges or faces together (S191a): "im
	 * selecting multiple patch, going edit mode edge, and as I select some,
	 * selection resets to single object. can we allow multiselection (and
	 * multi edition)". Picked in 4, kept while editing; a click on any of
	 * them edits it without dropping the others. Empty for one patch.
	 */
	TArray<FSurfPatchRef> EditSet;
	bool InEditSet(const FSurfPatchRef& Ref) const { return EditSet.Num() > 1 && EditSet.Contains(Ref); }
	const TArray<FSurfPatchRef>& GetPatchSelection() const { return PatchSelection; }

	/**
	 * More than one patch at a time, for the verbs that take several: join,
	 * delete, move as one. Shift-click in object sub-mode adds one, Ctrl-click
	 * takes it back out; the last one picked is the active patch. A plain
	 * click, anywhere, is back to one.
	 */
	void AddPatchToSelection(const FSurfPatchRef& Patch, bool bToggle);

	/** The patches picked in object sub-mode, or just the active one. */
	TArray<FSurfPatchRef> GetSelectedPatches() const;

	/** Every patch of every group in the level. */
	TArray<FSurfPatchRef> GetAllPatches() const;

	/** Every patch in the level, in object sub-mode. */
	bool SelectAllPatches();

	/** Alt-click a face: its row, again its column, again the face alone. Ctrl or Shift adds. */
	void SelectQuadRowOrColumn(const FSurfQuadRef& Quad, bool bAdd);

	/** Fold several picked patches into each other wherever two share an edge. */
	bool JoinPickedPatches(const TArray<FSurfPatchRef>& Picked, double ToleranceCm);

	/**
	 * Run a body once per patch the selection touches, with that patch's
	 * refs, and rebuild every group touched once at the end. The selection
	 * may span patches; every transform goes through this.
	 */
	void ForEachSelectedPatch(TFunctionRef<void(ASurfGroup&, FSurfPatch&, const TArray<FSurfPointRef>&)> Body);

	/**
	 * The patch faces currently picked. A face selection is also a point
	 * selection - its four corners - so every transform, the gizmo and soft
	 * selection all work on it without knowing faces exist.
	 */
	const TArray<FSurfQuadRef>& GetQuadSelection() const { return QuadSelection; }

	/** Find the patch face under the cursor, nearest first. */
	bool PickPatchQuad(FEditorViewportClient* ViewportClient, const FViewportClick& Click,
		FSurfQuadRef& Out, FVector* OutWorldHit = nullptr) const;
	bool PickPatchQuadWith(const FSurfPickContext& Ctx, FSurfQuadRef& Out, FVector* OutWorldHit = nullptr) const;

	/** Pick a patch face, or add it to what is already picked. */
	void SelectQuad(const FSurfQuadRef& Quad, bool bAddToSelection);

	/** Ctrl-click in face sub-mode: the block of faces between the last face clicked and this one. */
	bool SelectQuadRectTo(const FSurfQuadRef& To);

	/** Every face of the selected patch. */
	bool SelectAllQuads();

	/**
	 * Every face of the brushes the selection holds - or of every brush in
	 * the level when it holds none (S72e). "in brush mode, ctrl a will
	 * select even from nonselected objects": A took every patch instead,
	 * because the only Select All that knew about faces was the patch one.
	 */
	/**
	 * The group a new piece goes into (S140a): "why i get multiple
	 * surfacing groups both in viewport tags and outliner".
	 *
	 * Because every piece made with nothing selected made a group of its
	 * own. What is reused is the one this session last built in - and only
	 * while it is still in this world, in the level he is in, and not a
	 * prefab's room. Remembered rather than searched for, so it can never
	 * be a group belonging to somewhere else.
	 */
	ASurfGroup* GroupForNewGeometry();

	bool SelectAllBrushFaces();

	/**
	 * Every face of the brush a click landed on (S138a): "im in 4 mode and
	 * trying seect the cylinder and it select a single face instead."
	 *
	 * Object sub-mode means the whole thing. A patch has always been taken
	 * whole there; a brush was not, because the click fell through to
	 * SelectFace whatever the sub-mode was. Adding toggles the **brush**,
	 * not the face, so Shift on a cylinder already picked takes the whole
	 * cylinder out again.
	 */
	void SelectBrushOf(const FSurfFaceRef& Face, bool bAddToSelection);

	/** Select a control point, or add/remove it from the selection. */
	void SelectPoint(const FSurfPointRef& Point, bool bAddToSelection = false);

	/**
	 * Alt-click cycles: the point's row, then its column, then just the
	 * point. With Shift as well, added to what is already selected.
	 */
	void SelectPointRowOrColumn(const FSurfPointRef& Point, bool bAddToSelection = false);

	/**
	 * Select the grid edge nearest the click: one segment, or the whole line
	 * it belongs to when the click was a double-click.
	 */
	bool SelectEdgeAt(FEditorViewportClient* ViewportClient, const FViewportClick& Click, bool bAdd, bool bWholeLine);
	bool SelectEdgeWith(const FSurfPickContext& Ctx, bool bAdd, bool bWholeLine, bool bPath = false);

	/** The nearest grid segment to the cursor, within the pick radius, picking nothing (S83a). */
	bool NearestEdgeUnder(const FSurfPickContext& Ctx, FSurfEdgeHit& Out) const;

	/**
	 * One Edit mode (S83a). What a click at this point would be picking: a
	 * control point if one is under the cursor, else an edge, else a face.
	 * False when none of ours is under it at all.
	 */
	bool KindUnderCursor(const FSurfPickContext& Ctx, ESurfSubMode& OutKind) const;

	/** True while the click is the one deciding - off in Object, off when locked. */
	bool IsAutoKind() const;
	bool AutoAppliesNow() const { return IsAutoKind() && SubMode != ESurfSubMode::Object; }
	void SetAutoKind(bool bOn);

	/** 1, 2 or 3: lock that kind, or let go if it is already the locked one. */
	void ToggleKindLock(ESurfSubMode Kind);

	/** Leave Object for editing, with the click deciding again. */
	void EnterEditKinds();

	void ClearPointSelection();

	/** Add one point without disturbing the row/column cycle. For verbs that state a result. */
	void AddPointToSelection(const FSurfPointRef& Point);
	/** The point taken out of the selection (S224a: Ctrl-click in the UV window), the rest kept. */
	void RemovePointFromSelection(const FSurfPointRef& Point);

	/**
	 * The correction that would put the selection exactly onto the nearest
	 * control point it does not own - or the nearest corner, edge or face
	 * of a static mesh in the level (S59) - or zero when nothing is near
	 * enough. World space. Sets the snap marker.
	 */
	FVector FindPointSnap(const ASurfGroup& MovingGroup, const FSurfPatch& MovingPatch, const TArray<FVector>& MovingFrom) const;

	/**
	 * The point under the cursor is the one meant (S157a, A18): during a
	 * move, a control point of another piece within a few pixels of the
	 * cursor takes the moving point nearest the cursor, whatever the reach
	 * in centimetres - "if cursor is on it guess maybe user want to snap".
	 * The correction obeys the axis lock like the reach snap does.
	 */
	bool FindCursorSnap(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
		const ASurfGroup& MovingGroup, const FSurfPatch& MovingPatch, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const;

	/**
	 * Every point a move may land on (S157e): the control points of every
	 * patch and the corners of every brush, in world space - less the ones
	 * that are moving (the point selection, the named brushes) and, unless
	 * asked, the patch being edited. "should be able snap between
	 * patch/brushes too."
	 */
	void GatherSnapTargets(const ASurfGroup* MovingGroup, const FSurfPatch* MovingPatch, const TArray<FGuid>* MovingBrushes, TArray<FVector>& Out) const;
	/** The edges of ours a moving point may snap onto (S200a): cage lines of patches, primitives' edges; what moves is left out. Pairs of ends in world space. */
	void GatherSnapEdges(const ASurfGroup* MovingGroup, const FSurfPatch* MovingPatch, const TArray<FGuid>* MovingBrushes, TArray<FVector>& OutEnds) const;

	/** The target within a few pixels of the cursor takes the moving point nearest the cursor; the axis lock bounds the correction. */
	bool FindCursorSnapAmong(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
		const TArray<FVector>& Targets, const TArray<FVector>& Moving, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const;

	/** With the option on: the moving point nearest the cursor lands on the surface under the cursor, the moving actor ignored (S157e). */
	bool FindSurfaceSnap(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& CursorPx,
		const AActor* Ignore, const TArray<FVector>& Moving, FVector& OutCorrection) const;

	/** The nearest target within the reach in centimetres of any moving point, under the axis lock - for the pieces that have no reach snap of their own. */
	bool FindReachSnapAmong(const TArray<FVector>& Targets, const TArray<FVector>& Moving, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const;

	/** What the selection is snapped to right now, drawn while it is dragged. */
	enum class ESurfSnapKind : uint8 { None, Point, Corner, Edge, Face, Align, EdgeMid, EdgePerp, OwnEdge };
	/** Align (S171b): among the targets, the axis line through one that passes within reach of a moving point, when no point itself is in reach and no lock is on. */
	bool FindAlignSnapAmong(const TArray<FVector>& Targets, const TArray<FVector>& Moving, const TArray<FVector>& MovingFrom, FVector& OutCorrection) const;
	bool FindMeshSnap(const TArray<FVector>& Moving, FVector& OutCorrection, FVector& OutTarget, double& OutDistanceSquared, ESurfSnapKind& OutKind) const;
	void DrawSnapMarker(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;

	/**
	 * The prefabs that draw nothing of their own - baked, or empty - as grey
	 * boxes: there, out of reach until unbaked, and clickable by the box
	 * (S73a). "perhaps they should show in some grey/disabled color."
	 */
	void DrawPrefabMarkers(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;

	/** The floor grid and the axes through the origin (S172b), fading with distance, spaced by how high the eye is. */
	void DrawGrid(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;

	/** A gradient behind everything in the modelling view (S172b): a quad far out, filling the view, that any geometry covers. */
	void DrawBackdrop(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;

	/** Matcap on or off (S172b): the settings' tick, applied to every group's preview. */
	void SetMatcap(bool bOn);

	/**
	 * The view as one choice (S179a): 0 Lit, 1 Unlit, 2 Matcap. "Matcap is
	 * its own view mode… like unlit but with matcap." Matcap is the unlit
	 * view with the clay pass on; any other view mode turns it off.
	 */
	void SetViewKind(int32 Kind);

	/**
	 * A verb has run from a button (S184c): its frame shows, when it has
	 * options and the frame is not already on it - a verb that writes no
	 * adjustable operation (Smoothing, Crease...) showed its frame once,
	 * from the right-click, and never on a click.
	 */
	void AfterVerbRan(const FString& Id, bool bHasOptions);
	int32 GetViewKind() const;
	/** The camera slides, keeping its turn, until the selected piece is in view (S187a) - after something is made. */
	void KeepSelectionInView();

	// --- Lattice (S189a) ---
	/** A lattice round every selected patch, and its cage open for editing on the first. */
	bool MakeLattice(int32 ResX, int32 ResY, int32 ResZ);
	/** The Lattice verb on a patch that has one: in or out of its cage. False when there is none to go into. */
	bool ToggleLattice();
	/** The cage being edited goes; the patch keeps its shape. */
	bool RemoveLattice();
	bool IsLatticeEditing() const;
	void LeaveLattice();
	TArray<FSurfPatchRef> LatticeTargets() const;
	int32 PickLatticePoint(FEditorViewportClient* ViewportClient, FViewport* Viewport) const;
	bool BeginLatticeModal(int32 Kind, FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void UpdateLatticeModal(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void EndLatticeModal(bool bKeep);
	bool HandleLatticeKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event);
	void DrawLattice(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;
	FSurfPatchRef LatticePatch;
	TArray<int32> LatticePicked;
	bool bLatticeModal = false;
	bool bLatticeClickHeld = false;
	int32 LatticeModalKind = 0;
	int32 LatticeAxis = 0;
	FVector2D LatticeStartMouse = FVector2D::ZeroVector;
	FVector LatticePivot = FVector::ZeroVector;
	TArray<FVector> LatticeCageFrom;
	TArray<FVector> LatticePointsFrom;
	TUniquePtr<FScopedTransaction> LatticeTransaction;

	/**
	 * The grid is the editor's own Grid show flag (S180a): "a grid option
	 * already exists with UE, let's avoid redundancy". While the mode is up
	 * that flag draws our metric floor with its axes in the perspective
	 * views instead of the editor's; the flat views keep the editor's.
	 */
	bool IsGridShown() const;
	void SetGridShown(bool bOn);
	/** Set while a matcap left behind by a view-mode change is being turned off. */
	bool bMatcapLeaving = false;
	/** The view kind before Game view (S181a): Game view goes to Lit and back to this. -1: nothing to put back. */
	int32 ViewKindBeforeGame = -1;

	/** False while the mode is leaving: the matcap never outlives it. */
	bool bViewOverridesAllowed = true;

	/** The pass that puts clay on everything in the scene (S173a), registered while the mode is up. */
	TSharedPtr<class FSurfMatcapViewExtension, ESPMode::ThreadSafe> MatcapExtension;

	/** Slate's own share of a frame (S173a), timed between its pre- and post-tick, for the frame line. */
	FDelegateHandle SlatePreTickHandle;
	FDelegateHandle SlatePostTickHandle;
	/** An edit in the game's view turns it back to the editor's (S200a). */
	FDelegateHandle GameViewEditHandle;
	/**
	 * The outliner as he reads it (S202a): the level's own group named after
	 * the level - "should be named after the level ... its the level prefab
	 * we re editing" - and the prefabs placed in a level held by its group
	 * (his "nest them"). On the way in, and after a placement arrives; never
	 * inside a room.
	 */
	void TidyOutliner();
	/** Placed prefabs dressed as the view asks (S206a), looked at twice a second from Render, refreshed on the next tick. */
	void SyncInstanceLooks();
	/** The statistics line (S207a): counted twice a second, drawn in the corner by DrawHUD. */
	void DrawStats(FViewport* Viewport, FCanvas* Canvas);
	FString CountStats() const;
	FString StatsLine;
	double StatsAt = 0.0;
	double InstanceLooksAt = 0.0;
	bool bInstanceLooksPending = false;
	void OnLevelActorAdded(AActor* Actor);
	FDelegateHandle ActorAddedHandle;
	bool bTidyPending = false;
	bool bLeavingGameView = false;
	void LeaveGameViewForEdit();
	double SlateTickStartedAt = 0.0;
	double FrameSampleSlateMs = 0.0;
	/**
	 * The axis the snap is allowed to move along, while one is named (S150a).
	 *
	 * "when snap's on i shall be able to snap also with single axiss!
	 * example im on G+pressed Z (stuck on Z) : can still snap with a vertice
	 * at some other place onto his Z's!"
	 *
	 * Two things were wrong at once. The correction was applied whole, so a
	 * snap during G+Z moved X and Y as well and quietly broke the lock the
	 * hand had just set; and the search measured the whole distance, so the
	 * only targets it could ever find were ones already nearly under the
	 * point. With an axis named, both become one question - **how far along
	 * that axis** - and a vertex right across the level at the same height
	 * is a target, which is the whole use of it.
	 *
	 * Set by whatever is running the gesture, cleared when it ends. A plane
	 * lock names its own normal in Excluded and measures what is left.
	 */
	FVector SnapAlongAxis = FVector::ZeroVector;
	FVector SnapExcludedAxis = FVector::ZeroVector;

	/** The name labels drawn last, by pixel rectangle (S238a): a click on one picks what it names. */
	mutable TArray<TPair<FBox2D, TWeakObjectPtr<AActor>>> NameLabelHits;
	mutable FVector SnapMarkerWorld = FVector::ZeroVector;
	mutable ESurfSnapKind SnapMarkerKind = ESurfSnapKind::None;
	/** The face's normal where a Face snap landed (S235a), for a placed thing to stand on it. */
	mutable FVector SnapMarkerNormal = FVector::UpVector;
	/** For Align: the axis of the guide and where the moving point lands on it. */
	mutable FVector SnapMarkerAxis = FVector::ZeroVector;
	mutable FVector SnapMarkerTo = FVector::ZeroVector;
	/** Each static mesh as the snap measures it, built once and kept while the mode is up. */
	mutable TMap<TWeakObjectPtr<const class UStaticMesh>, TSharedPtr<struct FSurfMeshSnapEntry>> MeshSnapCache;

	/** Move every selected control point by a world-space delta, snapped. */
	void MoveSelectedPoints(const FVector& WorldDelta);

	/**
	 * Turn and resize the selection about its own centre. A row of points is
	 * a thing, not a list of points, so the gizmo has to act on it as one.
	 */
	void RotateSelectedPoints(const FRotator& WorldRotation);
	void ScaleSelectedPoints(const FVector& Scale);

	/**
	 * How much an unselected point follows the selection, 0 to 1. Zero when
	 * soft selection is off, or the point is selected, or it is out of range.
	 */
	float GetSoftInfluence(const FSurfPointRef& Point) const;

	/**
	 * Undo restores the geometry, and the selection has to survive it: losing
	 * what you had selected turns one undo into two actions.
	 */
	void OnUndoRedo();

	/** Record the selection on the chunk about to be edited, so undo can hand it back. */
	void SnapshotSelectionInto(class USurfChunk& Chunk);

	/** After undo or redo: take the selection back from the chunks just restored. False when none were. */
	bool RestoreSelectionAfterUndo();

	/**
	 * Put the viewport's orbit point on the selection, without moving the
	 * camera. Unreal's Alt+LMB orbit already spins around its look-at point;
	 * it only ever gets set by pressing F, which is why orbiting never
	 * orbited what was being worked on.
	 */
	void UpdateOrbitPivot();

	/**
	 * Put the camera where the cursor is pointing, stopping short of the
	 * surface and keeping the direction it was already looking. Crossing a
	 * level otherwise means flying, and flying past what you were aiming at
	 * and coming back is most of the time spent navigating.
	 */
	bool TeleportToCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/**
	 * The right-click menu: the verbs that apply to what is selected, and the
	 * options that change what a verb does. Options belong next to the verb
	 * they change, not in a settings window two clicks away.
	 */
	void OpenContextMenu(FEditorViewportClient* ViewportClient);

	/** Recompute the falloff weights. Cheap, and only when the selection changes. */
	void RefreshSoftSelection();

	/** Turn proportional editing on or off from the viewport. Returns the new state. */
	bool ToggleSoftSelection();

	/** The next (or previous) soft selection profile; choosing one turns soft selection on. */
	bool CycleSoftFalloff(int32 Direction = 1);
	bool SetSoftFalloff(ESurfFalloff Shape);

	/**
	 * The viewport between the modelling view (unlit, the editor's Alt+3:
	 * nothing of the level's lighting is paid for) and the shaded one.
	 * Applied on Enter when the setting says, put back on Exit; Shift+Z.
	 */
	void ApplyModellingView(bool bOn);
	void ToggleShadedView();
	bool IsModellingView() const;

	/**
	 * The search frame at the cursor (X): every verb that can run, by name,
	 * filtered as you type; Enter runs the first. Anything typed that names
	 * no verb is written to the session record as a note - a comment on
	 * what is on the screen, in the record's own context.
	 */
	void OpenSearch();
	/** The search for the UV window (S209a): only what serves there - the UV verbs, the mapping settings. */
	void OpenUVSearch(TSharedPtr<class SWidget> Parent);
	void AddNote(const FString& Text);

	/**
	 * Ctrl+Shift-click: the points between the last point clicked and this
	 * one, along the grid (a run when they share a row or column, the
	 * nearest staircase otherwise). In edge sub-mode the segments along it.
	 */
	bool SelectGridPathTo(const FSurfPointRef& To);
	FSurfPointRef PathAnchor;

	/** Grow or shrink the falloff radius by a step, and show it. */
	bool NudgeSoftRadius(double Factor);

	/** The snap reach, times a factor: what the bar's + and - do, on keys. */
	bool NudgeSnapReach(double Factor);

	/**
	 * The material the active patch wears, else the active brush face's;
	 * null when it wears nothing (the checker), which is a material to
	 * select by as much as any other. False when nothing is active.
	 */
	bool MaterialOfActive(UMaterialInterface*& OutMaterial) const;

	/**
	 * Same (S64a, §2b): every patch and brush face in the level wearing
	 * what the active one wears joins the selection. Adds, never clears.
	 * Returns how many joined.
	 */
	int32 SelectByMaterial();

	/**
	 * Replace (S64a, §2g): everything in the level wearing From wears To
	 * instead, every projection kept exactly - a retexture, not a reassign.
	 * Returns how many changed.
	 */
	int32 ReplaceMaterialEverywhere(UMaterialInterface* From, UMaterialInterface* To);

	/** The bar's Replace: the first picked patch's material replaced by the active patch's, level-wide. */
	bool ReplaceMaterialFromSelection();

	/**
	 * Make Prefab (S65a): the selected patches leave their groups for a new
	 * prefab asset, and one instance of it stands where they were, selected.
	 */
	bool MakePrefabFromSelection(const FString& WantedName = FString());
	/** Anything a prefab can be made of is picked (S172a): patches, primitives, or actors that are not ours - meshes, blueprints, lights. */
	bool CanMakePrefab() const;

	/** The name a new prefab would get - PF_Prefab_N, the first free - for the prompt to start from. */
	FString SuggestPrefabName() const;

	/**
	 * Drill-in (S65b). Enter the instance the editor has selected: its
	 * asset's geometry comes out into an editing group at the instance's
	 * transform, to be edited with every verb there is. Exit writes it
	 * back to the asset and every instance follows - or drops it.
	 */
	bool EnterPrefab();
	bool EnterPrefabInstance(class ASurfPrefabInstance* Instance);
	/** Out of the room: the asset written (or put back). bClimb: from a nested room, back up into the room it was entered from. */
	bool ExitPrefab(bool bKeep, bool bClimb = true);
	/** Out of every room at once, without climbing back: the mode ending, a bake. */
	void UnwindRooms(bool bKeep);
	bool IsInsidePrefab() const { return DrilledInstance.IsValid(); }
	class ASurfGroup* GetDrilledGroup() const { return DrilledGroup.Get(); }

	/**
	 * Out of the prefab and into the level (S72d): "how can I take geo out
	 * of its prefab and make it just be direct geo under current parent?"
	 * The selected instance's geometry is copied into a group where it
	 * stands and the instance goes; the asset stays in the library. Inside
	 * a room, the picked placement's geometry is unpacked into the prefab
	 * being edited instead, at where it was placed.
	 */
	bool UnpackSelectedPrefab(TArray<class AActor*>* OutSpawned = nullptr);

	/**
	 * A prefab that has outgrown geometry becomes a scene (S83a): what it
	 * holds is unpacked where it stood and made into a level of its own, so
	 * it can hold anything a level holds and be edited in the outliner.
	 */
	bool PromoteToScene(const FString& Name);
	bool CanPromoteToScene() const;

	/**
	 * The selected actors - a static mesh, a blueprint, a light, anything -
	 * become parts of the prefab being edited, at where they stand, and the
	 * actors go: the prefab carries them from then on, everywhere it is
	 * placed (S76a). Returns how many were taken in.
	 */
	int32 TakeInSelection();
	bool CanTakeIn() const;

	// --- The prefab as a document (S77) --------------------------------------
	/**
	 * A new, empty prefab, placed in front of the camera and opened: the way
	 * a level is made, rather than a thing that can only be cut out of a
	 * selection. "youd just create a prefab as u create a level. do your
	 * stuff in it. save or save as."
	 */
	bool NewPrefab(const FString& Name);

	/** Write what is open into its asset and save it, without closing. */
	bool SaveOpenPrefab();

	/** The parts of the open prefab, as actors in the level: spawned on opening, written back on closing. */
	void SpawnRoomParts(class USurfPrefab& Asset);

	/** One carried part as a real actor, where it belongs. Transient while a room is open, kept otherwise (S83a). */
	class AActor* SpawnPartActor(const struct FSurfPart& Part, const FTransform& At, bool bTransient);
	int32 CaptureRoomParts(class USurfPrefab& Asset);
	void ClearRoomParts();
	bool IsRoomPartActor(const AActor* Actor) const;
	/** What an actor is as a part (S218a): a mesh and its materials, a level, else its class; the transform is the caller's. */
	static void FillPart(struct FSurfPart& Part, const AActor* Actor);
	void WatchRoomActors(bool bOn);
	/** An actor becomes a part of the open prefab (S241a: the join, once the actor is known not to be a bake). */
	void JoinRoomPart(AActor* Actor);
	void OnActorAddedToRoom(AActor* Actor);

	// --- Scenes (S75a) -------------------------------------------------------
	/**
	 * The other kind of prefab: a level of its own, placed as the engine's
	 * Level Instance, holding anything a level holds - lights, blueprints,
	 * static meshes, our groups, piece instances (D92). A piece is our
	 * geometry and nothing else; everything the piece cannot hold is why a
	 * scene exists. The engine owns the editing session, the outliner swap
	 * and the commit; these only reach it.
	 */
	bool MakeSceneFromSelection(const FString& Name);
	bool CanMakeScene() const;
	FString SuggestSceneName() const;
	TArray<AActor*> ActorsForScene() const;
	class ALevelInstance* SelectedScene() const;
	bool EnterScene(class ALevelInstance* Scene);
	bool ExitScene(bool bKeep);
	bool IsInsideScene() const;

	/**
	 * Scenes as the Prefabs window lists them (S153e, A13): the second kind
	 * of prefab beside the first. Where they are kept, one placed, every
	 * placement found and framed, the first placement entered.
	 */
	static const TCHAR* ScenesFolder();
	class ALevelInstance* PlaceSceneAsset(const FSoftObjectPath& Scene, const FVector& At);
	int32 FindSceneInstances(const FSoftObjectPath& Scene);
	bool EditSceneAsset(const FSoftObjectPath& Scene);
	/** Every placement of the scene in the level, none transient. */
	TArray<class ALevelInstance*> SceneInstancesOf(const FSoftObjectPath& Scene) const;

	/**
	 * Boxes move, turn and size like everything else (S72c). He could not
	 * find how: G, R and S did nothing on a brush, because the modal only
	 * ever moved control points.
	 *
	 * In face sub-mode with faces picked, G pushes those faces along their
	 * own normals and the rest of the brush stands - one side out, the
	 * others where they were. Otherwise G, R and S move, turn and size the
	 * whole brush. The grid catches a move, a turn is a quarter at a time,
	 * a size is one number about the middle.
	 */
	bool TransformSelectedBrushes(const FTransform& About, const FVector& Pivot);
	/** Each picked face along its own normal, in centimetres. A push that would collapse the brush is refused. */
	int32 PushSelectedFaces(double Cm);
	/** Every brush the selection reaches, once each. */
	TArray<FSurfFaceRef> BrushesHeldBySelection() const;
	/** The middle of what the selection's brushes bound. */
	bool BrushSelectionPivot(FVector& OutPivot) const;

	/**
	 * The middle of the picked brush sides, in the world. The gizmo sits
	 * there in face sub-mode, so the handles stand on the wall being pushed
	 * rather than in the middle of the brush (S72g).
	 */
	bool SelectedFacesPivot(FVector& OutPivot) const;

	/**
	 * The middle of the corners picked on a brush (S148a): "cant move
	 * selected verts on a box !".
	 *
	 * The gizmo has to stand on what is being moved. It stood on the middle
	 * of the brush, because that is the only brush pivot there was - so the
	 * handles being dragged were the handles of something else.
	 */
	bool BrushCornersPivot(FVector& OutPivot) const;

	/**
	 * Each picked face by as much of the drag as runs along its own normal:
	 * the side facing the arrow moves, the ones across it stay. Silent - a
	 * gizmo drag calls it sixty times a second.
	 */
	int32 PushSelectedFacesAlong(const FVector& WorldDelta);

	/**
	 * Size the picked sides about a point (S141a): "when scaling a box face
	 * the whole box will scale ? bug".
	 *
	 * Move was per-face and size was whole-brush, which is the
	 * inconsistency he felt. Each picked face's plane keeps its normal and
	 * takes its distance from the pivot multiplied - so one face picked
	 * makes the box longer on that side only, and every face picked is the
	 * whole brush sized about its middle, which is what it always did.
	 *
	 * Answers how many faces moved.
	 */
	int32 ScaleSelectedFacesAbout(const FVector& Factor, const FVector& Pivot);

	/**
	 * Point the picked sides at the same sides again, after the brush has
	 * been rebuilt from its corners (S146c).
	 *
	 * A face reference is an **index** into the brush's face list, and
	 * RebuildFromPoints writes a new list - so every ref held across a
	 * corner edit points at whatever now sits at that number. The normals
	 * survive the rebuild, so they are what the refs are found again by.
	 */
	void RepickFacesByNormal(ASurfGroup* Group, const FGuid& BrushId, const TArray<FPlane>& WasPlanes);

	/** A gizmo drag on a brush: the handles do what G, R and S do (S72g). */
	bool BrushGizmoDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale);

	/**
	 * Each picked brush face becomes a patch in the same group, and the
	 * brushes stay (S72c). To Sheet replaces a brush with one primitive,
	 * which is not the same thing: "i want keep box(es) + the face
	 * selection converted to patches (for each!)".
	 */
	int32 FacesToPatches();

	/** Alt+PgDn: into the picked placement, else the selected instance. Alt+PgUp: out, keeping - up one step of the nesting. */
	bool PrefabIn();
	bool PrefabOut();

	/**
	 * Nesting (S71b). Inside a room the asset's placements - other prefabs
	 * placed in this one - are drawn by a stand-in instance at the room's
	 * frame; a click on one picks it, Alt+PgDn edits it in a room of its
	 * own, Delete takes it out. Prefab inside a room places rather than
	 * spawns: the selected patches become a prefab placed here, a selected
	 * instance is taken in as a placement.
	 */
	bool EnterPlacement(const FGuid& PlacementId);
	void PickPlacement(const FGuid& PlacementId);
	FGuid GetPickedPlacement() const { return RoomPickedPlacement; }
	bool RemovePickedPlacement();

	/**
	 * A placement moves, turns and sizes like anything else (S71d): with one
	 * picked, G, R and S drag it, X Y Z lock the axis, Enter or a click
	 * confirms and Escape puts it back; Shift+D leaves a copy under the
	 * mouse. The transform is the placement's own, in the prefab's space.
	 */
	bool SetPickedPlacementTransform(const FTransform& Local, bool bTransact = true);
	FTransform GetPickedPlacementTransform() const;
	bool DuplicatePickedPlacement();
	class ASurfPrefabInstance* GetRoomStandIn() const { return RoomStandIn.Get(); }

	/** The instance whose prefab is open, and how many of its parts are out in the level as actors (S77a). */
	class ASurfPrefabInstance* GetDrilledInstance() const { return DrilledInstance.Get(); }
	int32 NumRoomPartActors() const { return RoomPartActors.Num(); }
	/** How deep the hand is: 0 outside, 1 in a prefab, 2 in a prefab inside it, ... */
	int32 GetRoomDepth() const { return IsInsidePrefab() ? RoomTrail.Num() + 1 : 0; }

	/**
	 * The library (S71c). A prefab is saved when it is made and when a room
	 * is left keeping; Save as makes a copy under a new name and points the
	 * instance being edited (or selected) at it; the Prefabs window places,
	 * edits, finds, renames, saves and shows.
	 */
	/** The prefab the hand is on: the room's, else the selected instance's, else null. */
	class USurfPrefab* ContextPrefab() const;
	bool SavePrefabAsset(class USurfPrefab* Asset);
	/** A copy of the context prefab under WantedName, saved; the instance being edited or selected points at it. */
	bool SavePrefabAs(const FString& WantedName);
	/** A copy of Asset under WantedName, saved; the context instance points at it when Asset is the context prefab. */
	bool SavePrefabAsFrom(class USurfPrefab* Asset, const FString& WantedName);
	/** Open Asset to edit: through its first instance in the level, or alone through a stand-in at the origin when none is placed. */
	bool EditPrefabAsset(class USurfPrefab* Asset);
	/** An instance of Asset at a point, on the grid, selected. */
	class ASurfPrefabInstance* PlacePrefabAsset(class USurfPrefab* Asset, const FVector& At);
	/** A static mesh at a point (S210a): the editor's own actor, tagged as ours, under our folder and the level's group; inside a prefab it becomes a part. */
	class AStaticMeshActor* PlaceStaticMeshAsset(class UStaticMesh* Mesh, const FVector& At);
	/** Every actor in Actors (placements, static meshes) becomes With - a prefab or a mesh - where it stands; how many did. */
	int32 ReplaceActorsWith(const TArray<AActor*>& Actors, const FAssetData& With);
	/** The tag a placed mesh of ours carries. */
	static const FName PlacedMeshTag;
	/** Where the view looks on the floor, on the grid: in front of the camera, for Place. */
	FVector ViewFloorPoint() const;
	/** Select every instance of Asset in the level and frame them; how many. */
	int32 FindPrefabInstances(class USurfPrefab* Asset);
	bool RenamePrefabAsset(class USurfPrefab* Asset, const FString& WantedName);
	/** The next free name built on Base: Base itself if free, else Base_2, Base_3 ... */
	FString SuggestPrefabNameLike(const FString& Base) const;
	/** The path of names from the top room down, "PF_House / PF_Door". */
	FString DescribeRoomPath() const;

	/** The prefabs open, outermost first: the path from the level down (S81a). */
	TArray<FString> RoomPathNames() const;
	bool HasSelectedInstance() const;

	/**
	 * The room (S71a): while a prefab is being edited, its group is the one
	 * thing open - everything else is out of reach. Picks, selection, the
	 * wires and framing ask this; with no prefab open, everything is open.
	 */
	bool IsOpen(const class ASurfGroup* Group) const;

	/** The room's frame in the world: the instance's transform in place, the origin otherwise. */
	FTransform GetRoomFrame() const { return RoomFrame; }
	bool IsRoomInPlace() const { return bRoomInPlace; }

	/** The context round a prefab edited where it stands, dressed again as the settings say: ghosted, wires or as it is (S249a). */
	void ApplyContextLook();
	/** One of our groups or placed prefabs round the room, out of reach. */
	bool IsRoomContext(const AActor* Actor) const;
	/** One the wire look took out of view and draws as wires. */
	bool IsRoomContextWired(const AActor* Actor) const;

	// --- Groups (S249b) ---
	/** The picked pieces made one group; picked groups go inside it, nested. Answers whether one was made. */
	bool GroupSelection();
	/** One level of the picked groups undone; a piece picked alone leaves the group it is in. */
	bool UngroupSelection();
	/** Every member of the picked pieces' outermost groups picked too: a box touching one takes the group. */
	bool ExpandSelectionToGroups();
	/** A click in Object on a piece, by its group: plain, Shift and Ctrl take whole groups; Alt the piece alone, again the groups round it. Answers whether it was handled. */
	bool ClickGroupPiece(const FSurfPieceRef& Piece, int32 FaceIndex, bool bShift, bool bCtrl, bool bAlt);
	/** "Group of 6", "In a group of 6", "2 groups"; empty when nothing grouped is picked. */
	FString DescribePickedGroups() const;
	/** The extents of the groups picked whole, for the brackets drawn round them. */
	void PickedGroupBounds(TArray<FBox>& Out, TArray<int32>* OutCounts = nullptr) const;
	/** Every outermost group's box in the level, and how many pieces it holds (S251a). */
	void AllGroupBounds(TArray<FBox>& Out, TArray<int32>* OutCounts = nullptr) const;
	/** The picked patches and primitives, whole. */
	void GatherPickedPieces(TArray<FSurfPieceRef>& Out) const;
	int32 CountPickedPieces() const;
	/** A piece's groups, outermost first. */
	TArray<FGuid> GroupPathOf(const FSurfPieceRef& Piece) const;
	/** Primitives picked with patches in Object follow a change made to the patches' points, by this world transform. */
	void CarryPickedBrushes(const FMatrix& WorldMatrix);

	/** The selected instance gets its own copy of its asset, so editing it changes nothing else. */
	bool MakePrefabUnique();

	/** Compile (S66a): every group and instance in the level to static meshes; and back. */
	bool BakeLevel();
	bool UnbakeLevel();

	/** Where the tool is standing, in one line, for a note to carry (S117a). */
	FString DescribeStanding() const;

	/**
	 * Whether a left press in this sub-mode belongs to us rather than to the
	 * editor's own marquee (S118a).
	 *
	 * The three editing sub-modes always claim it. Object sub-mode claims it
	 * only when a box or a lasso is the armed tool - otherwise a click there
	 * is a pick and a drag is the editor's, which is what object mode has
	 * always meant.
	 */
	/**
	 * The prefab instance a ray would pick, by the same rule a click uses -
	 * the one being edited excluded (S125a). For the test that guards it.
	 */
	class ASurfPrefabInstance* InstanceUnderForTest(const FSurfPickContext& Ctx, double& OutDistance) const;
	/** A test stands in for the mouse: the running move is told how far it went. */
	void NoteModalMovedForTest(const FVector& Delta) { ModalAppliedDelta = Delta; }

	/** An actor just dropped into the viewport, put on the grid (S122a). */
	void SnapDroppedActors(const TArray<AActor*>& Dropped);

	/**
	 * Turn the selected patches so they face away from the middle of what is
	 * selected - Blender's Recalculate Outside (S121a). Answers how many
	 * turned.
	 */
	int32 FaceSelectionOutward();

	/**
	 * Duplicate whatever actors the editor has selected - a prefab instance,
	 * a mesh, a light (S119a). Answers how many were made.
	 */
	int32 DuplicateSelectedActors();

	bool ClaimsSelectPress() const;

	/** A bake is running a slice at a time (S106a). */
	bool IsBaking() const;

	/** "baking 7 of 23" while one runs, empty when none does. */
	FString BakeProgress() const;

	/** Stop the running bake where it is. What is baked stays baked. */
	void StopBake();

	/**
	 * Step to the next or previous patch in the level. Cycling needs a stable
	 * order or it is shuffling, so the order is group by group and, within a
	 * group, the order the group keeps them in.
	 */
	bool CyclePatch(int32 Direction);

	/** Every patch in the level, one per line, with the selected one marked. */
	FString DescribePatches() const;

	/**
	 * Lift the picked faces out as patches of their own; the rest of the
	 * patch comes apart round the hole.
	 *
	 * A rectangle of cells is a sub-grid, and a sub-grid is a patch, so a
	 * ragged selection - an L - becomes a piece per rectangle of it (D56),
	 * and so does the rest, since a grid cannot have a hole.
	 */
	bool SeparateSelectedFaces();

	/** The picked faces go; the grid comes apart around the hole. */
	bool DeleteSelectedFaces();

	/**
	 * A new patch between two selected edges (runs of points, on one patch
	 * or two) of the same length; two loops make a tube. Joined into the
	 * patches it meets whole when asked, so two floors with an edge each
	 * selected become one.
	 */
	bool BridgeSelectedLines(bool bJoin = true);

	/**
	 * A staircase between two selected edges, as one sheet: forward a tread,
	 * up a riser, and again (S105a). Steps of zero takes the count from the
	 * settings; a rise above zero works the count out from the drop instead.
	 */
	bool StepsBetweenSelectedLines(int32 StepsAsked = 0, double RiseCm = 0.0);

	/**
	 * Continue: the selected border meets the border across from it and the
	 * surface carries on over the seam without a crease, easing back over
	 * the soft-selection radius (S132a). One border selected finds the
	 * nearest one within the snap reach; two says which. It moves points and
	 * does not Join - the two stay two patches.
	 */
	/**
	 * Continue; with bDryRun it only looks (S159a): true when a border of
	 * another patch is within reach with the same count, OutTarget filled
	 * with that border's world points, nothing said and nothing changed.
	 */
	bool ContinueSelectedBorder(bool bDryRun = false, TArray<FVector>* OutTarget = nullptr);

	/** The border Continue would take while a move runs (S159a): drawn as a ghost; confirming the move takes it. */
	TArray<FVector> ContinueOffer;
	bool bContinueOffered = false;

	/**
	 * A new brush standing on each picked face (S142a): "shouldnt i be able
	 * to extrude off from a box face ?"
	 *
	 * "brush can be reworked and used too ! user supposed to be able to
	 * model anything off em." E has been a patch verb since it existed; a
	 * brush face had nothing. Each picked face grows a piece of its own -
	 * the face's own polygon, carried out along its normal - wearing the
	 * material and the mapping of the face it came off, so the join is
	 * invisible until it is moved.
	 *
	 * Answers how many pieces were made; their outer faces are what is
	 * selected afterwards, so the move that follows pulls them.
	 */
	int32 ExtrudeSelectedBrushFaces(double ThicknessCm);

	/** The picked faces, and only they, get thickness. */
	bool SolidifySelectedFaces(double ThicknessCm, int32 Side = -1, bool bRim = true, bool bEven = false);

private:
	/** What becomes of the block of faces that is cut out. */
	enum class ESurfCutPiece : uint8 { Keep, Drop, Solidify };

	/**
	 * Cut the picked faces out of their grids, a piece per rectangle of them
	 * (in the order SurfCells::Decompose gives), the rest of each patch in
	 * patches round the hole. OutPieces gets the pieces kept or solidified
	 * (their fronts), OutRest how many patches the rest came apart into.
	 */
	bool CutSelectedFaces(ESurfCutPiece What, double ThicknessCm, int32 Side = -1, bool bRim = true, bool bEven = false,
		TArray<FSurfPatchRef>* OutPieces = nullptr, int32* OutRest = nullptr);

public:

	/**
	 * Fold the patch that shares an edge with the selected one into it.
	 *
	 * Only defined when the shared edge carries the same number of points on
	 * both sides - a grid has no way to express a seam where five points meet
	 * seven. The joined patch keeps the selected one's id.
	 */
	bool JoinSelectedPatches(double ToleranceCm);

	/**
	 * Collapse the selected points onto one position - at their centre, or
	 * onto the first one picked. A grid keeps its dimensions, so the cells
	 * between the welded points go degenerate and are dropped at build: that
	 * is how a quad grid ends up holding a triangle.
	 */
	bool WeldSelectedPoints(bool bAtCentre = true);

	/**
	 * Collapse every group of points already closer than the tolerance.
	 * Nothing picked means the whole patch.
	 */
	bool WeldSelectionByDistance(double ToleranceCm);

	/**
	 * Weld the selection in clusters: the selected points within ReachCm of
	 * one another go to their own centre, across patches, so six corners
	 * picked in three pairs become three welds and not one.
	 */
	bool WeldSelectionInClusters(double ReachCm);

	/**
	 * Delete: the whole patch in object sub-mode or with nothing picked
	 * inside it, otherwise every row or column the selection touches. A grid
	 * cannot lose single points, so a scattered selection is refused.
	 */
	bool DeleteSelection();

	/**
	 * The brushes the picked faces belong to (S99a): "need be able delete a
	 * box ?". Delete only ever reached patches, so a box could be made and
	 * never unmade. A brush face cannot go on its own - a convex solid needs
	 * every plane it has - so the brush is what goes.
	 */
	bool DeleteSelectedBrushes();

	/**
	 * Move the picked corners of the brushes in the selection (S112a).
	 *
	 * A brush is planes, so this moves the corners, asks the convex hull of
	 * the result what planes they imply, and writes those back. A corner that
	 * would end up inside the solid is refused and nothing moves - "as long it
	 * remain enclosed". Answers how many brushes changed.
	 */
	int32 MoveBrushCorners(const FVector& WorldDelta);

	/** The world positions of every corner picked on a brush, for the gizmo and the snap. */
	void GatherBrushCorners(TArray<FVector>& Out) const;

	/** The brush corner under the cursor, among the brushes whose wires are drawn. */
	bool PickBrushCornerWith(const FSurfPickContext& Ctx, FSurfBrushCornerRef& Out) const;
	/** The hull edge nearest the cursor on the primitive under it (S166a): its two corners. */
	bool PickBrushEdgeWith(const FSurfPickContext& Ctx, FSurfBrushCornerRef& OutA, FSurfBrushCornerRef& OutB) const;

	/**
	 * Loops on a primitive (S185b): "alt shift select shall work on
	 * primitives too". An edge's loop is the rim of the many-sided face
	 * beside it (a cylinder's cap ring), else every edge parallel to it (a
	 * ring round the sides). A face's loop runs across the face's edge
	 * nearest the click: every face turning about that edge's direction.
	 */
	void SelectBrushEdgeLoop(const FSurfBrushCornerRef& A, const FSurfBrushCornerRef& B, bool bAdd);
	void SelectBrushFaceLoop(const FSurfFaceRef& Picked, const FVector& WorldHit, bool bAdd);

	/** Pick one: plain replaces, add keeps what was there and toggles this one. */
	void SelectBrushCorner(const FSurfBrushCornerRef& Corner, bool bAdd);

	const TArray<FSurfBrushCornerRef>& GetBrushCornerSelection() const { return BrushCornerSelection; }

	/** Every corner of every brush the selection holds, as the world sees them. */
	void BrushCornersOfSelection(TArray<FSurfBrushCornerRef>& Out) const;

	/**
	 * Every other group in this level folded into the one it keeps (S149b).
	 *
	 * "for some reasons i see two surfgroup in outliner, two tags
	 * Lvl_FIrstPerson in the level viewport. Cant there be only one?" - and
	 * a level built before the mark existed has as many as it has. The
	 * geometry moves in world space, so nothing shifts; the emptied actors
	 * go. A prefab's room, a baked group and anything not in this level are
	 * left alone. Answers how many groups were folded in.
	 */
	int32 MergeLevelGroups();

	/**
	 * Groups with nothing left in them, gone (S149c).
	 *
	 * A group is a home for geometry; a home with nothing in it is a row in
	 * the outliner and a name floating in the viewport over empty space,
	 * which is how "why do i get multiple surfgroup" looked from the chair
	 * long after the thing that filled it was deleted. Prefab rooms, baked
	 * groups and the group being drilled into are left alone. Answers how
	 * many went.
	 */
	int32 SweepEmptyGroups();

	/**
	 * Every Nth of what is picked, kept; the rest let go (S150b).
	 *
	 * "add some selection(add or remove) options. like one out of two, all
	 * column, all row, etc. checker select." Rows and columns are Alt-click
	 * and have been since S56; the one with nothing on it is the checker,
	 * and a checker is every Nth counted along both ways at once - so one
	 * verb and one number does what "one out of two" and "one out of three"
	 * both mean. Answers how many points are left.
	 */
	int32 ThinSelection(int32 Step);
	/** One step along the grid (S165a): outward takes every neighbour of a picked point (a cell beside a picked cell in faces); inward lets go of every picked point with an unpicked neighbour. Returns how many changed. */
	int32 GrowSelection(bool bOutward);

	/**
	 * Select linked (S172a): "for one selected patch: extend selection to
	 * any other patch connected through overlapping vert". Every patch
	 * that shares a point with a picked one - within half a centimetre -
	 * and every patch sharing one with those, and on, becomes picked as
	 * whole patches. Returns how many patches were added.
	 */
	int32 SelectLinkedPatches();

	/**
	 * The transform strip (S175a). What is picked, as a position, a turn and
	 * a size: an actor's own transform (absolute) when nothing of ours is
	 * picked; otherwise the centre of what is picked - points, faces,
	 * patches, primitives - with the turn and the size read as zero and
	 * one, because our geometry has no transform of its own: a value typed
	 * there is applied about the centre and reads zero again.
	 * Returns false when nothing is picked.
	 */
	bool GetStripTransform(FVector& OutPosition, FRotator& OutRotation, FVector& OutScale, bool& bOutAbsolute) const;

	/** Set the position (absolute), a turn and a size (about the centre; absolute for actors). One undo step. */
	bool ApplyStripTransform(const FVector* NewPosition, const FRotator* NewRotation, const FVector* NewScale);

	/**
	 * The pivot (S177a): "right click set selection as pivot - works on
	 * anything; a face or more, their centre; several verts too; best on a
	 * single vert." A point in the world that rotation and scale go about -
	 * the gizmo's rings and cubes, R and S, the SidePanel's numbers - until
	 * it is cleared. Moves still go from the selection's centre. The mode
	 * forgets it on leaving.
	 */
	bool SetPivotFromSelection();
	void SetPivot(const FVector& World);
	void ClearPivot();
	bool CanSetPivot() const;
	bool HasPivot() const { return bPivotSet; }
	FVector GetPivot() const { return PivotWorld; }
	/** The pivot when one is set, else the centre handed in. */
	FVector PivotOr(const FVector& Centre) const { return bPivotSet ? PivotWorld : Centre; }
	/** A small ring and cross where the pivot is, over everything. */
	void DrawPivot(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;
	bool bPivotSet = false;
	FVector PivotWorld = FVector::ZeroVector;

	/**
	 * Shading (S171a): the picked patches take the kind (0 smooth, 1 hard,
	 * 2 by the angle), the picked primitives the crease angle it means
	 * (180, 0, or the angle). Returns how many pieces changed.
	 */
	int32 SetShadingOnSelection(int32 Kind, double AngleDeg);

	/**
	 * Crease (S171a): the whole rows and columns picked on the selected
	 * patches become hard lines - or, when every one of them is hard
	 * already, smooth again. Returns how many lines changed; says what it
	 * needs when no whole line is picked.
	 */
	int32 ToggleCreaseOnSelectedLines();

	/**
	 * The flags on the pieces the selection reaches into (S151a): 1 when
	 * every one wears it, 0 when none does, -1 when they disagree.
	 */
	int32 PieceFlagState(ESurfPrimitiveFlags Flag) const;

	/**
	 * The flag put on, or taken off, every piece the selection reaches
	 * into (S151a): on when any is without it, off when all have it. The
	 * bake reads them; the preview does not change. Answers how many pieces
	 * were touched.
	 */
	int32 TogglePieceFlag(ESurfPrimitiveFlags Flag);

	/** The flag put on (or taken off) every piece the selection reaches into - a tick, not a toggle (S152c). Answers how many pieces were touched. */
	int32 SetPieceFlag(ESurfPrimitiveFlags Flag, bool bOn);

	/** What the picked pieces wear, for the line: "detail, no shadow" or "no flags". */
	FString PieceFlagsLine() const;

	/** Every picked piece on a line of its own - "box: no draw", "patch: detail" - for the Flags window (S154c). */
	TArray<FString> PieceFlagLines() const;

	/**
	 * Placed prefabs' own flags (S201a): the placements picked in the
	 * editor's selection; whether they override (1 all, 0 none, -1 some);
	 * an override on or off; a flag of the override (1 / 0 / -1 as
	 * PieceFlagState). A change is an edit: the placement's bake goes.
	 */
	TArray<class ASurfPrefabInstance*> PlacementsPicked() const;
	int32 PlacementOverrideState() const;
	void SetPlacementOverride(bool bOn);
	/** Paint override on the picked placed prefabs (S251j): 0 none, 1 all, -1 some. */
	int32 PlacementPaintOverrideState() const;
	void SetPlacementPaintOverride(bool bOn);
	/** The placed prefabs the brush paints: those with Paint override, the picked ones when any is picked. */
	void PaintInstanceTargets(TArray<class ASurfPrefabInstance*>& Out) const;
	int32 PlacementFlagState(ESurfPrimitiveFlags Flag) const;
	void SetPlacementFlag(ESurfPrimitiveFlags Flag, bool bOn);

	/**
	 * The pieces the flags go on (S151b): the patches picked as wholes or
	 * under the picked points and faces, and the brushes under the picked
	 * sides. **Not** the patch left active from a minute ago: with a box
	 * picked, "the active patch" is the last patch clicked, and that is
	 * where the first Flags press went - "it seems the flag applied to
	 * both the box and the patch?" It applied to the patch alone, with the
	 * box picked, which is worse.
	 */
	void PiecesPicked(TArray<FSurfPatchRef>& OutPatches, TArray<FSurfFaceRef>& OutBrushes) const;

	/** The patch pick - patches, active patch, points, faces - let go of, for a plain pick of a brush (S151b). */
	void DropPatchPick();

	/**
	 * The editor's game view and the bake's side kept together (S154f):
	 * whichever way game view was turned on - the verb, G with nothing to
	 * move, the viewport's own menu - what is stale is baked and the
	 * stand-ins shown; off, the previews come back. Checked each frame,
	 * done on the next tick, never inside a render.
	 */
	void SyncGameView();
	bool bGameViewSyncPending = false;

	/**
	 * The cells a moved line stretched, cut back to the patch's density
	 * (S150d). Runs after a move when the setting is on, inside the move's
	 * own transaction; a whole row or column in the selection names the
	 * line, the cells either side are measured against the mean cell times
	 * the density, and the widest sub-cell is halved until none is over.
	 * The selection follows the line to its new index. Answers how many
	 * lines went in.
	 */
	int32 DensifyStretchedCells();

	/**
	 * A slide's amount, snapped (S150f): "that snap is over other actions!
	 * ctrl+r may work along with this."
	 *
	 * A slide moves each picked point along its own segment by one share,
	 * -1 to 1. The points are put at the share asked for, the snap is asked
	 * along the lead segment's direction only - so a line on a neighbouring
	 * patch at the right place along the slide is a target from anywhere
	 * across - and what it answers is turned back into a share and applied
	 * to every point. On a straight loop across a regular patch that lands
	 * the whole line; on a bent one it lands the lead point and the rest
	 * follow. Answers the share the points were left at.
	 */
	double SnappedSlideAmount(ASurfGroup& Group, FSurfPatch& Patch, const TArray<FVector>& Axes,
		TFunctionRef<FVector(int32, int32)> Original, double Amount);

	/** Copy the selected patch into a new one, offset, and select it. */
	bool DuplicateSelectionToPatch(double OffsetCm = -1.0);

	/** D14: the selection copied one stack level up, every weight at zero. */
	bool MakeLayerFromSelection();

	/** How deep the stack is where the selection is, the base counting as 1. */
	int32 SelectionStackDepth();

	/**
	 * Duplicate what is selected, in place, and select the copies: the
	 * patches picked in object sub-mode, otherwise the block the selection
	 * covers on each patch it touches, as a new patch each - the whole patch
	 * when it covers the whole grid. A grid cannot carry a loose point or a
	 * loose line, so anything that is not a rectangle is declined with the
	 * reason. Joins the caller's transaction when there is one.
	 */
	bool DuplicateSelection();

	/** Duplicate, then hand the copies to the modal move; cancelling the move takes the copies back. */
	bool DuplicateAndMove(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** The same selection, copied to the clipboard in world space. Nothing is edited. */
	bool CopySelection();

	/** Copy, then delete. Faces and patches only: a grid cannot lose a block of points. */
	bool CutSelection();

	/** Put the clipboard's patches into the level where they were copied from, as new patches, and select them. */
	bool PasteClipboard();

	bool HasClipboard() const { return PatchClipboard.Num() > 0; }

	/**
	 * Extrude whatever is selected. The whole of one border line grows the
	 * patch; any other run of points along a line becomes a two-line sheet
	 * of its own, and a rectangle of faces becomes a lid with four skirts.
	 * Every new line is selected afterwards, on every patch it touched, so
	 * the move that follows pulls them out together. In place, the new lines
	 * start exactly on the ones they came from.
	 */
	/** OffsetCm for ExtrudeSelectedEdge meaning "one cell, measured on the patch". Any other value is a distance, negative included. */
	static constexpr double ExtrudeBySpacing = -1.0e9;

	/** Under: -1 the setting, else an ESurfExtrudeUnder (0 drop, 1 close, 2 keep) for the faces beneath a lid. */
	bool ExtrudeSelectedEdge(bool bInPlace = false, double OffsetCm = ExtrudeBySpacing, int32 Under = -1);

	/** Grow the patch past one whole border line and select the new line. */
	bool GrowSelectedBorder(bool bColumn, bool bFromEnd, bool bInPlace, double OffsetCm = ExtrudeBySpacing);

	/** Cut the selected patch in two along the selected row or column. */
	bool SplitSelectedPatch();

	/** Double the resolution of every selected patch: a line between every pair of rows, of columns, or both. */
	bool SubdivideSelectedPatches(bool bRows, bool bColumns, int32 Times = 1);

	/** Give the selected patch thickness, as a front, a back and four skirts. */
	bool SolidifySelectedPatch(double ThicknessCm, int32 Side = -1, bool bRim = true, bool bEven = false);

	/** Every control point of the selected patch. */
	bool SelectAllPoints();
	bool InvertPointSelection();

	/** Frame the point selection, or the patch, or the group. */
	bool FocusSelection(FEditorViewportClient* ViewportClient);

	/** The same, in every viewport at once (Shift+F). */
	bool FocusAllViewports();

	/**
	 * Hide, show and isolate (S58, Blender's H, Shift+H, Alt+H and local
	 * view). A hidden patch is drawn, picked and previewed as if absent,
	 * saved and undone with the patch; isolation is a view - the other
	 * patches set aside on their groups, the other actors hidden the
	 * editor's temporary way, the selection framed - until pressed again.
	 */
	TArray<FSurfPatchRef> PatchesHeldBySelection() const;

	/**
	 * The last verb pressed on the bar or picked in the search, so Shift+R can
	 * run it again on whatever is selected now. Not the same as adjusting the
	 * last operation, which undoes and repeats with different numbers: this
	 * runs the verb afresh, wherever the selection has moved to.
	 */
	/**
	 * Move the selection's UVs, in UV units (one unit is one tile), from the
	 * UV window. A frozen (Manual) patch moves the points that are picked -
	 * its UVs are held, so they are the ones to move. A patch whose mapping
	 * is a rule has no single point to move: its whole mapping slides
	 * instead, which is what UVShift is for. Wrap a drag in a transaction of
	 * your own; this one only edits.
	 */
	bool MoveSelectedUVs(const FVector2D& DeltaUV);

	/** Turn the selection's UVs about a pivot, by a change in angle. */
	bool TurnSelectedUVs(double DeltaDegrees, const FVector2D& PivotUV);

	/** A quarter turn about the selection's pivot - Ctrl+R in the UV window (S69b). */
	bool TurnSelectedUVsQuarter();

	/**
	 * The UV window's fields (S69b): the numbers of the active patch's
	 * mapping, and the same numbers set outright on every patch the
	 * selection holds. Metres a tile, the shift in tiles, the turn in
	 * degrees; Square makes the tile square at the across figure. A frozen
	 * patch has no rule to set and is left alone, and the line says so.
	 */
	struct FUVFields
	{
		TOptional<double> TilesU;
		TOptional<double> TilesV;
		TOptional<double> ShiftU;
		TOptional<double> ShiftV;
		TOptional<double> Turn;
		bool bSquare = false;
	};
	bool ReadActiveUVFields(FVector2D& OutTiles, FVector2D& OutShift, double& OutTurn, bool& bOutFrozen) const;
	bool SetSelectedUVFields(const FUVFields& Fields);

	/**
	 * Smooth the UVs (S69e): each picked point's UV towards the mean of its
	 * four neighbours' - all the interior points when none is picked -
	 * Strength of the way, Iterations over; the border stays. A patch with
	 * a rule is frozen first, since a rule would re-derive it; the line
	 * says so.
	 */
	bool SmoothSelectedUVs(double Strength, int32 Iterations);

	/** Scale the selection's UVs about a pivot, by a ratio on each axis. */
	bool ScaleSelectedUVs(const FVector2D& Ratio, const FVector2D& PivotUV);

	/** What the UV window turns and scales about: the middle of what is picked. */
	bool SelectedUVPivot(FVector2D& OutPivot);
	/** The picked points' UV box and centre (S208a), else the selected patches'; false with nothing. */
	bool SelectedUVExtent(FVector2D& OutLow, FVector2D& OutHigh, FVector2D& OutCentre);
	/** The UV panel (S214a): the picked points' UVs (a frozen patch's) set to their low / centre / high on one axis; Where 0 low, 1 centre, 2 high. */
	bool AlignSelectedUVs(int32 Axis, int32 Where);
	/** Mirrored about the selection's centre on one axis. */
	bool FlipSelectedUVs(int32 Axis);
	/** The border points of the selected patches picked. */
	bool SelectBorderPoints();
	/** Islands (S215a): the selected patches' nets laid into the tile, tallest first, sizes kept relative, Margin in tiles between and around. */
	bool PackSelectedUVs(double Margin);
	/** Every selected net moved onto the first's centre. */
	bool StackSelectedUVs();
	/** Each selected net an even grid inside its own box, its turn kept. */
	bool RectifySelectedUVs();
	/** The picked points of one row or column of a net put on one line along the axis they lean to. */
	bool StraightenSelectedUVs();

	/** After picking in the UV window: the patch selection follows the points, as a viewport pick leaves it. */
	void SyncSelectionAfterUVPick();

	void NoteVerb(const FString& Name, TFunction<void()> Run);
	bool RepeatLastVerb();
	const FString& GetLastVerbName() const { return LastVerbName; }
	/**
	 * The last verb undone and run again with its options as they now are
	 * (S166a): "if you tweak parameters, it applies to the last performed
	 * action - I Add a box, in the popup I set it to cylinder, it turns to
	 * a cylinder, live." Only while that verb's transactions are still the
	 * top of the undo stack; false otherwise, and nothing happens.
	 */
	bool ReplayLastVerbForOptions(const FString& Id);

	/** PatchSelection becomes every patch the points and faces span, active first. */
	void SyncPatchSelectionToPoints();

	bool HidePatches(bool bUnselected);
	bool UnhideAllPatches();
	bool ToggleIsolate(FEditorViewportClient* ViewportClient);
	void EndIsolate(bool bRestoreView);
	bool IsIsolating() const { return bIsolating; }
	bool bIsolating = false;
	TArray<TWeakObjectPtr<ASurfGroup>> IsolationGroups;
	TArray<TWeakObjectPtr<AActor>> IsolationHiddenActors;
	FEditorViewportClient* IsolateViewClient = nullptr;
	FVector IsolateViewLocation = FVector::ZeroVector;
	FRotator IsolateViewRotation = FRotator::ZeroRotator;

	/**
	 * Straighten the view (S71e): to the axis it is nearest, or to a named
	 * one. Uses the viewport being worked in. The flat panes keep their own.
	 */
	bool SnapViewToNearestAxis();
	bool SnapViewToDirection(const FVector& Direction, const FString& Named);

	/** What F frames: the points, else the patch, else the brush corners, else the brushes picked, else a selected actor, else every group. False, and a friction line, when there is nothing. */
	bool BoundsToFrame(FBox& OutBounds) const;

	/**
	 * Put a row or column through the patch, on whichever axis the cursor is
	 * nearest to crossing - the loop-cut gesture, rather than a button that
	 * always cuts down the middle.
	 */
	bool InsertLoopAtCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/**
	 * A loop cut carried on into the joined patches (S190a, backlog 18): from
	 * each end of the cut on the patch given, the sheet patch whose border
	 * shares the span the cut crossed gets the same lines, and so on round.
	 * Their new points are added to OutPoints; the slide that follows moves
	 * them with the cut. The number of patches cut.
	 */
	struct FSurfLoopLink
	{
		TWeakObjectPtr<ASurfGroup> Group;
		FGuid PatchId;
		bool bColumn = true;
		/** The line before the span the cut crosses. */
		int32 At = 0;
		/** +1 when its lines run the cut's way, -1 against. */
		int32 Sign = 1;
	};
	/** The joined sheet patches a cut at Index would carry on into, found before anything is cut. */
	TArray<FSurfLoopLink> TraceLoopChain(const ASurfGroup* Group, const FGuid& PatchId, bool bColumn, int32 Index) const;
	int32 CarryLoopAcross(const TArray<FSurfLoopLink>& Chain, int32 Inserted, TArray<FSurfPointRef>& OutPoints);
	/** For the tests and the frame: the cut the next InsertLoopAtCursor makes, without a cursor. */
	void ArmLoop(bool bColumn, int32 Index) { bLoopIsColumn = bColumn; LoopIndex = Index; }
	/** A running slide put at a share of its span (-1 to 1), the carried lines with it; false with no slide running. */
	bool ApplySlideAmount(double Amount);
	/** The cage points written and the patch following them, inside a running lattice gesture. */
	void ApplyLatticeCage(const TArray<FVector>& Cage);
	/** The preview's chain, kept while the cut under the cursor stays the same. */
	TArray<FSurfLoopLink> LoopChainPreview;
	FString LoopChainKey;

	/** The patches a carried loop cut reached: 1 when their new lines are columns, and the way they run against the cut (+1 or -1). */
	TMap<FGuid, FIntPoint> LoopSlideChain;

	/** How far along its span the running slide is (-1 to 1): the carried cuts follow it. */
	double ModalSlideAmount = 0.0;

	/**
	 * Ctrl+R arms the loop rather than cutting immediately: the line that
	 * would be added is drawn under the cursor until you click to take it or
	 * press Escape. Cutting on the keypress means guessing where it went.
	 */
	void BeginLoopPreview();
	void CancelLoopPreview();
	bool IsPreviewingLoop() const { return bPreviewingLoop; }

	/** Recompute which line the cursor is naming. Called as the mouse moves. */
	void UpdateLoopPreview(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/**
	 * The select gesture: a drag that takes control points with the active
	 * tool - box, lasso or brush. In the Blender scheme a drag starts it on
	 * its own; in the house scheme a key arms it first, because there a bare
	 * drag still belongs to the editor.
	 */
	// --- Modal transform ----------------------------------------------------

	/**
	 * Start, steer, and finish a modal transform. Confirm keeps the result as
	 * one undo step; cancel puts every point back exactly, which is the
	 * contract that makes the gesture safe to experiment with.
	 */
	bool BeginModal(ESurfModal Modal, FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** The modal drag when what is picked is a placement, not points (S71d). */
	bool BeginPlacementModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void UpdatePlacementModal(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	bool IsMovingPlacement() const { return bModalOnPlacement; }

	/** The modal drag when what is picked is a brush or its faces (S72c). */
	bool BeginBrushModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void UpdateBrushModal(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	bool IsMovingBrush() const { return bModalOnBrush; }

	/**
	 * G, R and S over whatever the editor has selected, when nothing of ours
	 * is (S94a): "could move it with unreal transform (original gizmo) but
	 * not with g key". A prefab instance, a light, a mesh - the keys that
	 * move everything else in this tool move those too.
	 */
	bool BeginActorModal(ESurfModal InModal, FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void UpdateActorModal(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	bool IsMovingActors() const { return bModalOnActors; }
	/** Tell the running placement drag that it made this placement, so cancelling takes it back. */
	void SetModalPlacementAdded(const FGuid& Id) { if (bModalOnPlacement) { ModalPlacementAdded = Id; } }
	void UpdateModal(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	/** The cursor carried round the viewport's edge during a gesture (S187a); the gesture reads through it. */
	void WrapModalCursor(FViewport* Viewport);
	/** The gesture's start set to where the cursor reads now, wraps included. */
	void ResetModalMouse(FViewport* Viewport);
	void ConfirmModal();
	void CancelModal();

	ESurfModal GetModal() const { return Modal; }
	ESurfAxisLock GetAxisLock() const { return AxisLock; }

	/**
	 * The axis a turn goes about, for any lock (S143a): "pressing R to
	 * rotate selection then shift z or shift y to only work on certain axis
	 * - didnt work - bug".
	 *
	 * A plane lock names its own normal, and **that normal is the axis to
	 * turn about** - "everything but Z" makes sense for a move and cannot
	 * mean anything else for a turn. The patch path had this right; the
	 * brush, the actor and the placement paths each fell back to world Z
	 * instead, so a plane lock there was indistinguishable from no lock,
	 * and so was an axis lock seen from the wrong side of the level.
	 *
	 * With nothing named it is the way the camera looks, not world up: a
	 * turn with no axis should be about what you are looking down.
	 */
	static FVector TurnAxis(ESurfAxisLock Lock, const FVector& Custom, const FVector& Forward);

	/**
	 * What a transform in progress is doing, in a few characters: which one,
	 * what it is locked to, and any number typed so far. Empty when nothing
	 * is running. Reading this off the screen is the difference between an
	 * exact edit and a careful one.
	 */
	FString DescribeModal() const;

	/** Constrain the modal transform to one world axis, or free it again. */
	void SetAxisLock(ESurfAxisLock Lock, FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/**
	 * Reproduce a click at the cursor. The mode claims the press, so the
	 * editor never sends a click of its own; this builds one and runs it
	 * through the same path a real click takes.
	 */
	void ClickAtCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** Turn a held press into a drag once it has moved. Shared by both move callbacks. */
	bool TrackSelectDrag(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y);

	void BeginSelectGesture();
	void CancelSelectGesture();

	/**
	 * Painting the layer weight (D14, S63b). Shift+B arms the brush; a left
	 * drag paints the weight up under it, with Ctrl paints it down; the wheel
	 * sizes it; Shift+B or Escape puts it away. One transaction a stroke.
	 */
	void TogglePaint();
	bool IsPainting() const { return bPaintArmed; }

	/**
	 * The paint view (S69a): while the brush is out, every group's preview
	 * shows the channel being painted instead of its materials, and takes
	 * them back when the brush is put away. Called by the toggle, by the
	 * channel's right-click, and on Exit.
	 */
	void RefreshPaintView();

	/**
	 * Fill (S70b): the channel set outright on the selected points - every
	 * point of the selected patches when none is picked - to full, or to
	 * nothing; the Colour channel to the paint colour, or black. One step.
	 */
	bool FillSelectedPaint(bool bFull);

	// --- Cavity (S212a) -----------------------------------------------------------
	/** The cavity of every control point of Patch, 0 ridge - 0.5 flat - 1 dent, with the settings' Smooth / Level / Invert. */
	static void CavityOf(const struct FSurfPatch& Patch, TArray<float>& Out, float Smooth, float Level, bool bInvert);
	/**
	 * The adjustments (S238a): the tab's channels of the picked points -
	 * every point of the selected pieces when none is picked - and of the
	 * picked meshes, through one of USurfSettings::PaintAdjust*: from
	 * cavity through the blend, smooth, contrast, levels, invert. One
	 * transaction; a verb's frame replays it live. How many points.
	 */
	int32 AdjustPaint(int32 Kind);
	int32 MeshAdjustPaint(int32 Kind);
	/** The two paint colours swapped. */
	void SwapPaintColours();

	// --- The brush on placed meshes (S213a) --------------------------------------
	/** The picked static mesh actors' components. */
	void PaintMeshTargets(TArray<class UStaticMeshComponent*>& Out);
	/** A stroke over the meshes: LOD 0's override colours; how many vertices changed. */
	int32 PaintMeshWeightWith(const struct FSurfPickContext& Ctx, const FVector2D& At, bool bErase, bool bSmooth);
	int32 FillMeshPaint(bool bFull);
	/** The cavity per vertex of LOD 0, seams joined by position. */
	void MeshCavityOf(const class UStaticMeshComponent* Component, TArray<float>& Out) const;
	/** Placed meshes wearing the paint view (S236a): the component and the override slots it had before, to put back. */
	TMap<TWeakObjectPtr<class UStaticMeshComponent>, TArray<TWeakObjectPtr<UMaterialInterface>>> MeshViewRestore;
	/** The paint view on the placed meshes' slots while it is on, their own back when it is not (S236a). */
	void RefreshMeshPaintView(UWorld* World, UMaterialInterface* PaintView);

	/** Paint at a pixel, over the patches the selection reaches into. Returns how many points changed. */
	/**
	 * A pass of the paint brush. Erase pulls the channel down, smooth pulls
	 * each point towards the points around it - "ok when shift paint =
	 * smoothen", which is what Shift does in every paint tool there is
	 * (S104a).
	 */
	int32 PaintWeightWith(const FSurfPickContext& Ctx, const FVector2D& At, bool bErase, bool bSmooth = false);
	bool IsSelectGestureArmed() const { return bSelectArmed; }
	bool IsSelectGestureDragging() const { return bSelectDragging; }

	/** Switch which tool a drag uses, and arm it. */
	void SetSelectTool(ESurfSelectTool Tool);

	/**
	 * The next select tool, and arm it (S144a): "make it cycle through
	 * selection modes while pressing W ? box select etc".
	 *
	 * Box, lasso, brush, round again - skipping the brush outside vertex
	 * sub-mode, because it paints points and there are none to paint. The
	 * line says which one it landed on, since a tool nobody can see the
	 * name of is a tool nobody knows they are holding.
	 */
	bool CycleSelectTool();

	/** Apply the path drawn so far. Returns whether the selection changed. */
	bool CommitSelectGesture(FEditorViewportClient* ViewportClient, FViewport* Viewport, bool bSelect);

	/** The commit given a context, so a test can box, lasso and brush. */
	bool CommitSelectGestureWith(const FSurfPickContext& Ctx, bool bSelect, bool bExtend);

	/** Feed a select gesture screen positions directly, for tests. */
	void AppendSelectPath(const FVector2D& Position) { bSelectDragging = true; SelectPath.Add(Position); }


	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y) override;
	/** A subtle cross over a placed prefab the brush does not paint (S251j). */
	virtual bool GetCursor(EMouseCursor::Type& OutCursor) const override;
	bool bPaintHoverBlocked = false;
	double PaintHoverAt = 0.0;
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y) override;
	virtual bool MouseEnter(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y) override;
	virtual void DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas) override;
	virtual bool LostFocus(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;

	/**
	 * Replace a curve with the sheet its current subdivision produces, so the
	 * tessellated surface becomes editable vertex by vertex. This is how you
	 * stop steering a cage and start moving geometry.
	 */
	bool ConvertSelectedPatchToSheet();

	/** Dispatch for a bound action. Public so the panel can run one too. */
	bool HandleAction(ESurfAction Action, FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** The patch the inspector's patch verbs act on, mutable and chunk-marked. */
	FSurfPatch* ModifySelectedPatch();


	/** Select a face, replacing the selection or adding to it. Also sets the active face. */
	void SelectFace(const FSurfFaceRef& Face, bool bAddToSelection = false);

	/** Runs Visit for every selected face, marking each owning chunk first. */
	void ForEachSelectedFace(TFunctionRef<void(ASurfGroup&, FSurfBrush&, FSurfFace&)> Visit);

	/**
	 * The middle-click (§2b): the surface under the cursor - a brush face or
	 * a patch, whichever is nearer - copied onto the selected faces and every
	 * patch the selection holds: material, projection, scale, turn,
	 * smoothing. FaceUnder and FaceHit are what the click path already
	 * picked. True when the click was taken.
	 */
	bool CopySurfaceFromClick(const FSurfPickContext& Ctx, bool bFaceHit, const FSurfFaceRef& FaceUnder, const FVector& FaceHit);
	/** The other way (S167b): the active surface's material and mapping (or the material alone) onto the surface clicked. */
	bool CopySurfaceToClick(const FSurfPickContext& Ctx, bool bFaceHit, const FSurfFaceRef& FaceUnder, const FVector& FaceHit, bool bMaterialOnly);

	/** Rebuilds every group touched by the current selection. */
	void RebuildTouchedGroups();

	// --- Slice ---------------------------------------------------------------
	// The primary shaping tool, and the only one: there is no CSG, so a concave
	// shape is several convex brushes. Slice is legitimately modal because it
	// has to collect points, so it obeys D16.3 - Enter commits, Escape abandons,
	// and no other verb is blocked while it is armed.

	/** True while the tool is collecting points. */
	bool IsSlicing() const { return bSlicing; }

	void BeginSlice();
	void CancelSlice();

	/** Keep both halves, only the front, or only the back. */
	void CommitSlice(bool bKeepFront, bool bKeepBack);

	/** Commit keeping what the setting says, Ctrl for the back only and Shift for the front only overriding it. */
	void CommitSliceKeeping(bool bCtrl, bool bShift);

	/**
	 * The bar's button: arms the slice, and once two points are down
	 * commits it as Enter would. One button for the whole gesture.
	 */
	void PressSlice();

	/**
	 * A cut point, snapped to the grid, and the face it was placed on: with
	 * nothing selected, the brushes the points are placed on are the ones
	 * cut. The click path and the tests both come through here.
	 */
	void AddSlicePoint(const FVector& World, const FSurfFaceRef& On = FSurfFaceRef());

	/** The plane the collected points imply. False until there are at least two. */
	bool GetSlicePlane(FPlane& OutPlane) const;

	/** The brushes a commit would cut: the selection's, else the ones the points were placed on. */
	TMap<ASurfGroup*, TArray<FGuid>> SliceTargets() const;

	UPROPERTY()
	TObjectPtr<USurfInspector> Inspector;


private:
	bool PickFace(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfFaceRef& Out, FVector* OutWorldHit = nullptr) const;
	bool PickPatch(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPatchRef& Out, FVector* OutWorldHit = nullptr) const;

public:
	// The context-taking picks are public so a test can click through them.
	bool PickFaceWith(const FSurfPickContext& Ctx, FSurfFaceRef& Out, FVector* OutWorldHit = nullptr) const;
	bool PickPatchWith(const FSurfPickContext& Ctx, FSurfPatchRef& Out, FVector* OutWorldHit = nullptr) const;
	bool PickPatchCyclingWith(const FSurfPickContext& Ctx, FSurfPatchRef& Out, FVector* OutWorldHit = nullptr);
	bool PickPointWith(const FSurfPickContext& Ctx, FSurfPointRef& Out) const;
	bool PickPointOn(const FSurfPickContext& Ctx, const ASurfGroup& Group, const FSurfPatch& Patch, FSurfPointRef& Out) const;

private:

	/**
	 * Every patch under the cursor, nearest first. Clicking the same spot
	 * again takes the next one down, which is the only way to reach a patch
	 * that something else is sitting on top of.
	 */
	bool PickPatchCycling(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPatchRef& Out, FVector* OutWorldHit);
	bool PickPoint(FEditorViewportClient* ViewportClient, const FViewportClick& Click, FSurfPointRef& Out) const;

	TArray<FSurfFaceRef> Selection;
	FSurfFaceRef ActiveFace;

	/**
	 * The bake running a slice at a time, if one is (S106a). The ticker that
	 * drives it holds nothing but a weak handle on this mode, and stops the
	 * moment this is empty - so leaving the mode, or stopping, ends it
	 * without anything having to be unregistered.
	 */
	TSharedPtr<SurfBake::FJob> BakeJob;

	bool bSlicing = false;
	TArray<FVector> SlicePoints;

	/** The faces the points were placed on, one per point - the target when nothing is selected. */
	TArray<FSurfFaceRef> SliceFaces;

	ESurfSubMode SubMode = ESurfSubMode::Face;
	/** The Edit kind Tab goes back to from Object (S175a). */
	ESurfSubMode LastEditKind = ESurfSubMode::Face;
	FSurfPatchRef SelectedPatch;
	TArray<FSurfPointRef> PointSelection;

	/** Open for the duration of one gizmo drag, so undo sees one step. */
	TUniquePtr<class FScopedTransaction> DragTransaction;
	double DragStartSeconds = 0.0;

	/** True while a run of non-gizmo drags is being refused, so it logs once. */
	bool bRefusingDrag = false;

	/** Select gesture: the tool, and the screen path drawn so far. */
	bool bSelectArmed = false;
	bool bSelectDragging = false;
	ESurfSelectTool ActiveSelectTool = ESurfSelectTool::Box;

	/** Two corners for a box, the whole path for a lasso, the trail for a brush. */
	TArray<FVector2D> SelectPath;

	/** Points the brush has already taken, so a stroke does not toggle twice. */
	/** The picked patch faces. Their corners are mirrored into PointSelection. */
	TArray<FSurfQuadRef> QuadSelection;

	/** The last face clicked, so Ctrl-click can take the block between. */
	FSurfQuadRef QuadPathAnchor;

	/** Drill-in: the instance entered and the group its asset is being edited in. */
	TWeakObjectPtr<class ASurfPrefabInstance> DrilledInstance;
	TWeakObjectPtr<class ASurfGroup> DrilledGroup;

	/** The room (S71a): its frame, whether it stands in place, and what was set aside for it. */
	FTransform RoomFrame = FTransform::Identity;
	bool bRoomInPlace = false;

	/** Nesting (S71b): the stand-in drawing the placements, the one picked, the placements as they were (for dropping), and the way back up. */
	TWeakObjectPtr<class ASurfPrefabInstance> RoomStandIn;
	FGuid RoomPickedPlacement;
	TArray<FSurfPlacement> RoomPlacementsBefore;

	/** The parts as they were when the room opened, so dropping the edits puts them back (S76a). */
	TArray<FSurfPart> RoomPartsBefore;

	/**
	 * The parts, out in the level as real actors while the room is open
	 * (S77a): they move and delete with the editor's own keys, and anything
	 * dropped in from the Content Browser joins them. Closing writes them
	 * back into the asset.
	 */
	TArray<TWeakObjectPtr<AActor>> RoomPartActors;
	FDelegateHandle RoomActorAdded;
	struct FSurfRoomStep
	{
		TWeakObjectPtr<class ASurfPrefabInstance> Instance;
		FGuid Placement;
		/** The room above was one of ours, made only to edit an unplaced prefab: it goes when we come back out of it. */
		bool bEditOnly = false;
	};
	TArray<FSurfRoomStep> RoomTrail;

	/** The placement being dragged (S71d), and where it stood when the drag began. */
	bool bModalOnPlacement = false;
	FGuid ModalPlacementId;
	FTransform ModalPlacementStart;
	/** A placement the gesture itself made (Shift+D): cancelling takes it back with the move. */
	FGuid ModalPlacementAdded;

	/** The brushes a drag is moving (S72c), their planes as they were, and which faces are pushed. */
	bool bModalOnBrush = false;
	bool bModalPushesFaces = false;

	/** S with faces picked in face sub-mode sizes those sides, as G pushes them (S141a). */
	bool bModalScalesFaces = false;

	/**
	 * G with sides picked moves their corners, anywhere (S147a): "ok it
	 * worked and i could scale that face down. however now i cant move it
	 * freely either : ?"
	 *
	 * Pushing a side along its own normal is the gizmo arrow's gesture and
	 * stays there; the key moves it the way every other G in the tool does,
	 * and the solid re-forms around where the corners land.
	 */
	bool bModalMovesCorners = false;
	struct FSurfBrushModalTarget
	{
		TWeakObjectPtr<ASurfGroup> Group;
		FGuid BrushId;
		TArray<FPlane> Planes;
		TArray<int32> PushedFaces;

		/** Where its corners stood when the gesture began, and which of them the picked sides hold (S146a). */
		TArray<FVector> Corners;
		TArray<int32> PickedCorners;

		/** Its whole face list as it stood, because a rebuild writes a new one (S146c). */
		TArray<FSurfFace> Faces;
	};
	TArray<FSurfBrushModalTarget> ModalBrushes;

	/** Primitives picked with the patches a points gesture moves (S249b), from their planes as they were. */
	TArray<FSurfBrushModalTarget> ModalCarried;
	/** The piece an Alt+click last went to, and how far out round its groups. */
	FSurfPieceRef AltClimbPiece;
	int32 AltClimbLevel = -1;
	const TArray<FGuid>* PathOfPiece(const FSurfPieceRef& Piece) const;
	void GatherMembersOf(const TSet<FGuid>& Ids, TMap<FGuid, TArray<FSurfPieceRef>>& Out) const;
	bool SetPiecePath(const FSurfPieceRef& Piece, const TArray<FGuid>& Path);
	/** Pieces into the selection: 0 instead of it, 1 added, 2 taken out; the active one the piece clicked. */
	void PickPieces(const TArray<FSurfPieceRef>& Pieces, const FSurfPieceRef& Active, int32 ActiveFaceIndex, int32 Op);

	/** The actors a gesture is moving, and where they stood when it began (S94a). */
	bool bModalOnActors = false;
	/** R pressed again on actors (S211a): the turn is a ball under the cursor, not a twist about the view. */
	bool bActorTrackball = false;
	TArray<TWeakObjectPtr<AActor>> ModalActors;
	TArray<FTransform> ModalActorStart;

	/** A stand-in made only to edit a prefab with no instance in the level (S71c); gone when its room closes. */
	TWeakObjectPtr<class ASurfPrefabInstance> RoomEditOnlyInstance;

	/** The scene being edited, when one is (S75a). The engine holds the session; this is only who it was. */
	TWeakObjectPtr<class ALevelInstance> EditingScene;

	/** The actor the editor's own hit proxy named for this click, so a click that misses our geometry can still pick a light or a scene (S75a). */
	TWeakObjectPtr<AActor> ClickedActor;

	/** The room's geometry into Target, whole; how many pieces. */
	int32 WriteRoomToAsset(class USurfPrefab* Target);
	TArray<TWeakObjectPtr<AActor>> RoomHiddenActors;
	/** Our groups and placed prefabs round a prefab edited where it stands (S249a): out of reach, dressed as the context look says. */
	TArray<TWeakObjectPtr<AActor>> RoomContext;
	/** Those the wire look took out of view, drawn as wires instead. */
	TArray<TWeakObjectPtr<AActor>> RoomWiredHidden;
	FEditorViewportClient* RoomViewClient = nullptr;
	FVector RoomViewLocation = FVector::ZeroVector;
	FRotator RoomViewRotation = FRotator::ZeroRotator;
	void SetRoomAside(class ASurfGroup* Room, class ASurfPrefabInstance* Instance, bool bInPlace);
	void ClearRoomAside();
	void FrameRoom(class ASurfGroup* Room);
	void LeaveRoomView();

	/** Every patch picked in object sub-mode. Empty means the active patch alone. */
	TArray<FSurfPatchRef> PatchSelection;

	/** The brush corners picked in vertex sub-mode (S112a). */
	TArray<FSurfBrushCornerRef> BrushCornerSelection;

	/** The group the last piece was made in, so the next one joins it rather than starting another. */
	TWeakObjectPtr<ASurfGroup> LastGroupUsed;

	/** The face an Alt-click last went through, and how far round the row / column / face cycle it is. */
	FSurfQuadRef LastRowColumnQuad;
	int32 QuadRowColumnStage = 2;
	/**
	 * Exactly what the last row/column click put in (S84a). The cycle used
	 * to take its previous stage back out by recomputing it, which removed
	 * faces the cycle had never added - "i just selected entire lines of
	 * faces. i had to then click individually each top of line cause they
	 * had been dropped from selection."
	 */
	TArray<FSurfQuadRef> LastRowColumnFaces;

	/** Rebuild PointSelection from QuadSelection, so transforms act on the corners. */
	void SyncQuadSelectionToPoints();
	/** The cells whose four corners are all in the point selection (S165a): the faces a point selection means. */
	TArray<FSurfQuadRef> QuadsHeldByPoints() const;

	TSet<int32> BrushTouched;

	/** Take whatever the brush covers at one point of its path, as it passes. */
	void PaintBrushAt(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& At);
	void PaintBrushWith(const FSurfPickContext& Ctx, const FVector2D& At, bool bRemove);

	/** Left button is down but has not moved far enough to be a drag yet. */
	bool bSelectPressed = false;
	FVector2D SelectPressAt = FVector2D::ZeroVector;

	/** Pixels of movement before a press becomes a drag rather than a click. */
	static constexpr double DragThresholdPx = 4.0;

	/** Viewport navigation, when the Blender scheme is in force. */
	TUniquePtr<class FSurfCamera> Camera;

	/** The editor's own gizmo size, restored when the mode exits. */
	float PreviousGizmoSize = 0.0f;

	/** Viewports whose grid floor was turned off on entering, to turn back on when leaving. */
	TArray<FLevelEditorViewportClient*> GridFloorRestore;

	/** The viewports whose own axis indicator we turned off (S110a). */
	TArray<FLevelEditorViewportClient*> AxesRestore;
	/** Viewports whose own numpad flying we turned off (S174a): the numpad orbits, and the editor's walk on the same keys fought it. */
	TArray<FLevelEditorViewportClient*> NumpadRestore;
	/** Whether the placed prefabs were last rebuilt wearing the view's override (S174a). */
	bool bInstancesOverridden = false;
	TWeakObjectPtr<UMaterialInterface> InstancesOverride;

	/** Listening for drops while the mode is up (S122a). */
	FDelegateHandle DroppedHandle;

	/** The editor's play-from setting as it was before the mode set it to the camera. */
	int32 PreviousPlayLocation = 0;
	bool bPlayLocationChanged = false;

	/** Patches copied with Ctrl+C, in world space, for as long as the mode object lives. */
	UPROPERTY(Transient)
	TArray<FSurfPatch> PatchClipboard;

	/** What duplicate and copy take, by sub-mode; false with the reason when the selection is not a patch or a rectangle. */
	bool GatherSelectionAsPatches(TArray<FSurfPatchCopy>& Out, FString& OutReason) const;

	/** Select patches just made by duplicate or paste, the way the current sub-mode selects. */
	void SelectMadePatches(const TArray<FSurfPatchRef>& Made);

	/** The gizmo size last handed to the editor, so a changed setting is noticed. */
	int32 AppliedGizmoSize = 0;

	/** Hand the gizmo size setting to the editor when it has changed. */
	void ApplyGizmoSize();

	/**
	 * Our own gizmo (setting bOwnGizmo): drawn by the mode in the editor's
	 * look at half its size, quiet when idle, all three of move, rotate and
	 * scale at once. The editor's widget is kept but not drawn; our handles'
	 * hit proxies are its own kind, so hovering sets its axis and a press
	 * runs its drag maths - one transaction, the adjust frame after.
	 */
	bool OwnGizmoOn() const;
	void DrawOwnGizmo(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI);
	void UpdateGizmoHover(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y);
	bool bGizmoHovered = false;

	/** What a gizmo drag has done so far, so EndTracking can note it for the adjust frame - a plain drag; a Shift-drag (a copy, an extrude) is more than one thing. */
	FVector DragTotalDelta = FVector::ZeroVector;
	FRotator DragTotalRotation = FRotator::ZeroRotator;
	FVector DragTotalScale = FVector::OneVector;
	bool bDragScaled = false;
	bool bDragRotated = false;
	bool bDragAdjustable = false;
	void BeginGizmoDrag(FEditorViewportClient* InViewportClient, FViewport* InViewport, bool bShift);
	void NoteGizmoDrag();

	/**
	 * A ring drag turns by the angle the cursor sweeps about the pivot on
	 * screen, following the hand as a ring should; the editor's own rotate
	 * maths reads its widget, which is not drawn while ours is. The sweep so
	 * far, how much of it has been applied, and where the cursor was last.
	 */
	bool GizmoRingRotation(FEditorViewportClient* ViewportClient, FViewport* Viewport, FRotator& OutRotation);

	/** Rotate was entered by the cursor reaching a ring, so leaving takes it away again (S130a). */
	bool bGizmoRotateFromHover = false;

	/** X, Y or Z pressed while the ball is turning: lock it to that axis, or let it go. Handled in InputKey. */
	bool LockGizmoBallToAxis(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FKey& Key);
	FVector2D GizmoDragMouse = FVector2D::ZeroVector;
	bool bGizmoDragMouseValid = false;
	double GizmoSweep = 0.0;
	double GizmoSweepApplied = 0.0;

public:
	/**
	 * Turn every selected actor about a point (S134a).
	 *
	 * The editor's rotate maths reads its own widget, which is not drawn
	 * while ours is - so a ring of ours turned points and brushes and did
	 * nothing at all to a selected actor, and the ball spun it flat about
	 * the view instead of turning it the way the hand asked. Answers how
	 * many it turned.
	 */
	int32 RotateSelectedActorsAbout(const FRotator& Turn, const FVector& Pivot);

	// --- Adjust the last operation ------------------------------------------

	/** Remember what was just done and the numbers it was done with. Called by every adjustable verb, last thing. */
	void NoteOperation(const FSurfOperation& Op);

	/** Whether the last operation is still the last thing in the undo history. */
	bool CanAdjustLastOperation() const;

	/** Undo the last operation and do it again with the numbers in LastOperation. */
	bool ReplayLastOperation(bool bFinal = false);

	/** Do an operation with the numbers given. Each verb opens its own undo step. */
	bool RunOperation(const FSurfOperation& Op);

	FString DescribeLastOperation() const;

	FSurfOperation LastOperation;

	/** The nearest thing under the cursor: our patches and faces first, then whatever the level's collision says. */
	bool FindPointUnderCursor(FEditorViewportClient* ViewportClient, FViewport* Viewport, FVector& OutPoint, FVector* OutDirection = nullptr) const;

	/** Z: the patches being edited, every patch, every patch with x-ray, and round again. */
	void CycleWireView();

	/** Every wire of ours hidden, the selection's too, or shown again (S199a, Alt+Z). Not saved. */
	void ToggleOverlay();
	bool IsOverlayHidden() const { return bOverlayHidden; }
	bool bOverlayHidden = false;
	FString DescribeWireView() const;

	/** Whether a click or a box takes points hidden behind a surface. Only in x-ray. */
	bool SelectsThroughSurfaces() const;

	/**
	 * Whether nothing of ours stands between the eye and a point. A point on
	 * the far side of a surface is a point the hand cannot mean, so picks
	 * and boxes skip it unless the wire view is x-ray. Tested against the
	 * preview meshes themselves.
	 */
	bool IsPointVisible(const FVector& Eye, const FVector& WorldPoint) const;

	/** The same, told which point is asking so a curve's own surface does not hide its net. */
	bool IsPointVisibleFrom(const FVector& Eye, const FVector& WorldPoint, const FSurfPointRef* Self) const;

private:
	/** A ray-test structure over one group's preview mesh, kept until the preview is rebuilt. */
	struct FSurfOcclusionTree
	{
		int32 Serial = -1;
		const UE::Geometry::FDynamicMesh3* Mesh = nullptr;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	};
	mutable TMap<TWeakObjectPtr<const ASurfGroup>, TSharedPtr<FSurfOcclusionTree>> OcclusionTrees;


	/** Where the last pick happened, and how deep into the stack it went. */
	FVector2D LastPickScreen = FVector2D(-1000.0, -1000.0);
	FVector LastPickOrigin = FVector(1.e9, 1.e9, 1.e9);
	int32 PickDepth = 0;

	/** Throttles the preview rebuild during a modal drag. */
	double LastModalRebuild = 0.0;

	FDelegateHandle UndoHandle;

	/** The in-viewport bar and the adjust frame: one of each in every level viewport, so a four-view layout has them everywhere. The first is the primary. */
	TSharedPtr<class SSurfViewportBar> ViewportBar;
	TSharedPtr<class SSurfAdjustFrame> AdjustFrame;
	TWeakPtr<class SLevelViewport> ViewportHost;
	TArray<TSharedPtr<class SSurfViewportBar>> ViewportBars;
	TArray<TSharedPtr<class SSurfAdjustFrame>> AdjustFrames;
	TArray<TWeakPtr<class SLevelViewport>> ViewportHosts;

public:
	/**
	 * Where the adjust frame goes: down and right of where the cursor was
	 * when the operation ran (absolute screen space), in the viewport that
	 * held the cursor; the primary viewport's bottom-left when no viewport
	 * did. Set by NoteOperation for a new operation, not for a replay.
	 */
	FVector2D AdjustFrameAnchor = FVector2D::ZeroVector;
	int32 AdjustFrameHost = -1;
	uint32 AdjustFrameSerial = 0;
	int32 HostIndexOf(const SWidget* Frame) const;

	/**
	 * The last result, shown for a moment where the cursor was when it was
	 * written - a refusal on the bar's status line went unread while the
	 * eye was on the model ("why can I not subdivide?").
	 */
	FVector2D ToastAt = FVector2D::ZeroVector;
	double ToastUntil = 0.0;
	uint32 ToastSerial = 0;
	FString ToastText;
	void ToggleMaximizeViewport(FEditorViewportClient* ViewportClient);

	/** Between the four-pane layout and one pane (Ctrl+Alt+Q); the layout left is remembered for the way back. */
	void ToggleFourViews();
	FName LastMultiPaneLayout;

	/** The bar and the frame put into every level viewport, and taken out again; done again whenever the layout changes. */
	void InstallViewportOverlays();
	void RemoveViewportOverlays();
	void OnViewportLayoutChanged();
	FDelegateHandle TabContentChangedHandle;

	/** The grid floor off in every viewport that shows it (setting), remembered for Exit. */
	void HideGridFloor();
private:

	/** The level editor's own Select All and Paste, taken over while the mode is up and put back on Exit. */
	TMap<TSharedPtr<const FUICommandInfo>, FUIAction> SavedGlobalActions;
	void TakeOverGlobalCommands();
	void RestoreGlobalCommands();

public:
	/** Select all of whatever the sub-mode selects. */
	void SelectAllBySubMode();

	/** The axis of the last edge picked: 0 none, 1 a column, 2 a row. For verbs handed points that need a line. */
	int32 GetLastEdgePickAxis() const { return LastEdgePickAxis; }

private:

	/** The editor's frame-rate settings as they were before the mode raised them. */
	float PreviousMaxFPS = 0.0f;
	bool bPreviousSmoothFrameRate = true;
	bool bFrameRateChanged = false;

	/** Whether the renderer was turned down for editing (a temporary scalability override, put back on Exit). */
	bool bQualityLowered = false;

	/** The view modes the perspective viewports had before the modelling view was applied, for Exit. */
	TMap<FLevelEditorViewportClient*, int32> PreviousViewModes;
	bool bModellingViewApplied = false;
	/** Which of the two views is wanted now: unlit unless Shift+Z asked for the shaded one. New viewports take it. */
	bool bModellingViewOn = false;

	/**
	 * A patch left with no surface - a wall never pulled, a line for a
	 * patch - is what "wire-only patches" were. Every finalized transaction
	 * asks for a check (run from Render, once no gesture is in progress),
	 * and a flat patch is written to the session record with the
	 * transaction that left it, once per patch.
	 */
	FDelegateHandle TransactionHandle;
	bool bFlatCheckPending = false;
	FString FlatCheckCause;
	TSet<FGuid> FlatPatchesReported;
	void OnTransactionState(const FTransactionContext& Context, ETransactionStateEventType State);
	void CheckForFlatPatches();
	int32 PreviousCompositingSamples = 4;
	float PreviousToolTipWrap = 1000.0f;
	bool bToolTipWrapNarrowed = false;
	int32 PreviousScreenPercentageMode = 1;
	int32 PreviousScreenPercentage = 100;
	bool bScreenPercentageChanged = false;

	/** The last operation that ran and did something, so a knob dragged to a value that does nothing can fall back to it. */
	FSurfOperation LastGoodOperation;

	/**
	 * Redraw the level viewports. During a continuous edit - a knob being
	 * dragged, a modal move, a gizmo drag - the hit proxies are left alone:
	 * an invalidated hit-proxy map is the whole scene rendered again the
	 * next time the cursor asks what it is over, once per step. They are
	 * refreshed when the edit ends.
	 */
public:
	void RedrawViewports();
private:
	int32 ContinuousEditDepth = 0;

	/** Frame timing, sampled in Render and written to the session record every couple of seconds: which thread the time goes to. */
	double FrameSampleStart = 0.0;
	double FrameSampleRenderMs = 0.0;
	double FrameSampleBarMs = 0.0;
	double LastHitchLoggedAt = 0.0;
	double FrameSampleGameMs = 0.0;
	double FrameSampleDrawMs = 0.0;
	double FrameSampleGpuMs = 0.0;
	double FrameSampleSwapMs = 0.0;
	int32 FrameSampleCount = 0;
	bool bFrameSetupLogged = false;
	void SampleFrame(double OurRenderMs, const FViewport* Viewport);

	/** Whether the gizmo drag now running began with a Shift-drag extrude, and how far it has moved anything. */
	bool bDragWasExtrude = false;
	bool bDragWasDuplicate = false;
	double DragMovedCm = 0.0;

	/** Turn the renderer down for editing, or put it back. */
	void ApplyEditingQuality(bool bOn);

	/** A setting was edited while the mode is up: what shows it refreshes. */
	void OnSettingEdited(UObject* Object, struct FPropertyChangedEvent& Event);
	FDelegateHandle SettingEditedHandle;

	/** When the soft radius last changed, so the ring can fade out of the way. */
	double SoftRadiusShownAt = 0.0;
	/** When the snap reach was last changed by the wheel or a key: its ring shows for a moment after. */
	double SnapReachShownAt = 0.0;

	// --- Modal transform state ----------------------------------------------

	/**
	 * Double-click, recognised here rather than taken from the click event.
	 * The mode claims the left press itself, so the editor never delivers a
	 * real IE_DoubleClick - the two presses have to be timed by hand.
	 */
	/**
	 * The key that started the transform currently running, held until it is
	 * released. One press of G was arriving twice; the second arrival found a
	 * move already running and promoted it to a slide.
	 */
	FKey ModalKeyHeld = EKeys::Invalid;

	/**
	 * Digits typed during a transform, one entry per axis.
	 *
	 * An exact number is what a modelling tool is for half the time, and
	 * reaching for the panel to type it means letting go of the gesture.
	 * Three entries rather than one because a move has three numbers: Tab
	 * steps between them, which is what removes the old rule that a typed
	 * move had to name an axis first.
	 *
	 * A turn and a size use the first entry only - there is one number in
	 * each of those.
	 */
	FString ModalTyped[3];

	/** The direction a Custom axis lock follows. World space, unit length. */
	FVector CustomAxis = FVector::ZeroVector;

	/** The direction the last extrude grew in, world space, for the move that follows it. */
	FVector LastExtrudeDirection = FVector::ZeroVector;
	/** The border line the last grow made (S207b), for its UVs to be carried on once it has its length. */
	TWeakObjectPtr<ASurfGroup> GrownGroup;
	FGuid GrownPatch;
	bool bGrownColumn = false;
	bool bGrownFromEnd = false;
	/** The grown line's UVs, carried on by its length from the edge it grew from; frozen patches only (the others are measured). */
	void CarryGrownUVs();

	/** Which of the three Tab is currently filling. */
	int32 ModalTypedField = 0;

	/** Anything typed at all, in any field. */
	bool HasTypedValue() const;

	/** The three typed numbers, with empty fields reading as zero. */
	FVector TypedVector() const;

	/**
	 * The point picked last, and when. A weld leaves two points in the same
	 * place, and clicking the same spot again steps to the one underneath -
	 * which is how a weld gets taken apart by hand. Mutable because picking
	 * is const and this is only a memory of it.
	 */
	mutable FSurfPointRef LastPointPicked;
	mutable double LastPointPickSeconds = 0.0;

	/** An object-mode drag selected every point for its own use; let go at its end. */
	bool bObjectDragTookAllPoints = false;

	/**
	 * A gizmo drag on a brush rather than on control points (S72g): "ok box
	 * could use some gizmo handle too?". A brush has no points for the
	 * point drag to write, so the same handles write its planes - and in
	 * face sub-mode the arrows push the picked sides along their normals.
	 */
	bool bBrushDrag = false;
	bool bBrushDragPushesFaces = false;
	FVector BrushDragPivot = FVector::ZeroVector;
	FVector BrushDragMoved = FVector::ZeroVector;

	/** Where and when the right button went down, to tell a click from a drag. */
	FVector2D RightPressAt = FVector2D::ZeroVector;
	double RightPressSeconds = 0.0;

	/** The axis of the last edge picked: 0 none, 1 a column (the edge ran down a column), 2 a row. Cleared with the selection. */
	int32 LastEdgePickAxis = 0;

	double LastEdgePressSeconds = 0.0;
	FVector2D LastEdgePressAt = FVector2D::ZeroVector;
	bool bEdgePressWasDouble = false;

	ESurfModal Modal = ESurfModal::None;
	ESurfAxisLock AxisLock = ESurfAxisLock::None;

	/** Where the mouse was when the gesture started, in pixels. */
	FVector2D ModalStartMouse = FVector2D::ZeroVector;
	/** The ray under the cursor when the modal began (S167b): a locked move follows the mouse along the axis or across the plane by rays, not by screen pixels. */
	FVector ModalStartRayOrigin = FVector::ZeroVector;
	FVector ModalStartRayDirection = FVector::ZeroVector;
	/** A locked move's delta from the two rays; false when the lock is nearly edge-on and pixels have to do. */
	bool LockedMoveFromRays(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector& Pivot, const FVector& Axis, const FVector& Excluded, FVector& OutDelta) const;
	/** The last delta a locked move's rays gave (S248a), held while the cursor is past the horizon or the line seen end-on. */
	mutable FVector ModalRayHeld = FVector::ZeroVector;
	mutable bool bModalRayHeld = false;
	/** The ray through the cursor as the gesture reads it, at a modal's start; the hold dropped. */
	void NoteModalStartRay(FEditorViewportClient* ViewportClient, FViewport* Viewport);

	/** The patch and its points as they were, for cancel and for re-deriving. */
	FGuid ModalPatchId;
	TArray<FVector> ModalOriginal;

	/** The group the active patch belongs to, for the length of the gesture. */
	TWeakObjectPtr<ASurfGroup> ModalGroup;

	/**
	 * Selected points on patches other than the active one, with where they
	 * were. Carried along by the same world transform, from their originals,
	 * so a selection across two patches moves as one thing.
	 */
	struct FModalOther
	{
		FSurfPointRef Ref;
		FVector OriginalWorld = FVector::ZeroVector;
		/** A carried loop cut's point: where a whole slide takes it, in the world (S190a). */
		FVector SlideAxisWorld = FVector::ZeroVector;
	};
	TArray<FModalOther> ModalOthers;

	/** Pivot in world space: the selection's own centre. */
	FVector ModalPivot = FVector::ZeroVector;

	/** Slide direction per selected point, in local space. */
	TArray<FVector> SlideAxes;

public:
	/** The axis each selected point slides along, in patch space: for a test to read. */
	const TArray<FVector>& GetSlideAxes() const { return SlideAxes; }

private:

	TUniquePtr<class FScopedTransaction> ModalTransaction;

	bool bPaintArmed = false;
	bool bPaintStroking = false;
	TUniquePtr<class FScopedTransaction> PaintTransaction;
	void BeginPaintStroke(FEditorViewportClient* ViewportClient, FViewport* Viewport);
	void EndPaintStroke();
	void PaintWeightAt(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FVector2D& At);
public:
	void PaintTargets(TArray<FSurfPatchRef>& Out);
private:

	/** What the running modal has applied so far, in world terms, so a confirm can record it. */
	FVector ModalAppliedDelta = FVector::ZeroVector;
	FRotator ModalAppliedRotation = FRotator::ZeroRotator;
	FVector ModalAppliedScale = FVector::OneVector;

	/** The modal was begun by an extrude or a duplicate that left its new lines on the old ones; cancelling it takes that back too. */
	bool bModalFollowsExtrude = false;
	int32 ExtrudeUndoPosition = 0;
	FString ModalFollowsVerb;

	/** Whether the last extrude dropped the faces beneath its lid, for the note the move that follows makes. */
	ESurfExtrudeUnder LastExtrudeUnder = ESurfExtrudeUnder::Drop;

	/** The pieces closing extruded blocks, and the lids they close, so the move that follows can turn them to face out of the box. */
	TArray<TPair<FSurfPatchRef, FSurfPatchRef>> ExtrudeClosingPieces;

	/**
	 * A wall or lid the last extrude made in place, and what it should face
	 * once the move that follows has given it height. A wall faces away from
	 * Reference - or toward it when the block was pulled into its surface
	 * (against Normal from Base), which makes a pit, whose walls are seen
	 * from inside. A lid of a box (the faces beneath closed or kept) faces
	 * away from the surface it was pulled from, so it is turned when pulled
	 * in. A zero Normal means the way of the pull does not matter.
	 */
	struct FExtrudeFacing
	{
		FSurfPatchRef Patch;
		FVector Reference = FVector::ZeroVector;
		/** The surface normal where the piece was made, and the point it was made at. */
		FVector Normal = FVector::ZeroVector;
		FVector Base = FVector::ZeroVector;
		/** A lid (else a wall). */
		bool bLid = false;
		/** A box's lid (faces beneath kept or closed), or the wall round one: turned when pulled in. */
		bool bBox = false;
		/** A wall along a block's outline: made facing the right way for a pull either way, never turned unless it is a box's pulled in. An edge's wall is turned by where it ends up. */
		bool bOutline = false;
	};

	/** The walls and lids the last extrude made in place, settled once the move has given them height. */
	TArray<FExtrudeFacing> ExtrudeWallFacing;

	/** Set by TurnExtrudedWallsOut when the last extrude's lid was pulled along its surface rather than out of it - the whole extrude is then taken back by the caller. */
	bool bExtrudePulledAlongSurface = false;

public:
	/** Whether a sheet's cells, on balance, face away from a point in its group's space. */
	static bool PatchFacesAwayFrom(const FSurfPatch& Patch, const FVector& Reference);

	/**
	 * Settle what the last extrude made, now that the move has given it
	 * height: walls and lids turned to face the right way; a wall pulled
	 * along itself (no surface) dropped; a lid pulled along its surface
	 * rather than out of it flagged (bExtrudePulledAlongSurface) for the
	 * caller to take the extrude back. Returns how many pieces turned.
	 */
	int32 TurnExtrudedWallsOut(bool bPreview = false);

	/** The last verb pressed, for Shift+R. */
	TFunction<void()> LastVerbRun;
	FString LastVerbName;
	int32 LastVerbQueueBefore = -1;
	/** True while ReplayLastVerbForOptions runs the verb again: the frame keeps its widgets, so a knob being dragged stays under the hand. */
	bool bReplayingOptions = false;

	/** The adjust frame shows while the last operation can be adjusted and no click in the viewport has dismissed it. */
	bool IsAdjustFrameShown() const { return !bAdjustFrameDismissed && (CanAdjustLastOperation() || bOptionsShown); }

	/**
	 * One parameter surface (S160a): the frame that pops on use also holds
	 * the verb's options, and the right-click opens the same frame on that
	 * verb - "only one param window. i prefer the popup upon use." What
	 * verb the options are for, a serial for the widget, and whether the
	 * frame is up for the options alone (no knobs to show).
	 */
	FString OptionsVerbId;
	uint32 OptionsSerial = 0;
	bool bOptionsShown = false;
	bool bOptionsOnly = false;
	/** The frame on a verb's options; at the cursor when a right-click asked (S166a), at its usual place otherwise. */
	void ShowVerbOptions(const FString& Id, bool bAtCursor = false);
	void TurnOptionsTo(const FString& Id);
	void DismissOptions() { bOptionsShown = false; bOptionsOnly = false; bOptionsAtCursor = false; }
	bool bOptionsAtCursor = false;
	FVector2D OptionsCursor = FVector2D::ZeroVector;

private:
	bool bAdjustFrameDismissed = false;

	/** Soft selection is off from an in-place extrude until the move that pulls it out has ended. */
	bool bSoftSuspended = false;

	/** Steps done and not undone in the editor's history. */
	int32 UndoPosition() const;

	/**
	 * The history's own count of steps done (S176a): +1 as a transaction is
	 * finalized, -1 on an undo, +1 on a redo. The queue's length stopped
	 * counting once the editor's undo memory filled - it drops the oldest
	 * step as each new one lands - so "what the last verb wrote" and "is
	 * the last operation still last" both read nothing, and the frame's
	 * knobs stopped replaying: "the slider in popup param didn't let me
	 * update the size".
	 */
	int32 HistoryPosition = 0;
	bool bTrackingHistory = false;
	FDelegateHandle HistoryFinalizedHandle;
	FDelegateHandle HistoryUndoHandle;
	FDelegateHandle HistoryRedoHandle;
	void TrackHistory(bool bOn);

	/** Undo until the history stands at Position again. */
	void UndoBackTo(int32 Position);

	/**
	 * Steps that undo and redo together (S244a): an extrude (or a duplicate)
	 * and the move, turn or size that pulled it out are two steps in the
	 * editor's history, and one Ctrl+Z took only the pull back, leaving the
	 * new walls flat on the lines they came from - invisible, still
	 * selected, and the next E grew out of them ("all wrong extrude"). An
	 * undo or redo that lands inside a pair carries on to its end, a frame
	 * later, since the history cannot be moved from inside its own event.
	 */
	struct FSurfUndoPair
	{
		int32 From = 0;
		int32 Top = 0;
	};
	TArray<FSurfUndoPair> UndoPairs;
	int32 UndoPairingSuspended = 0;
	bool bUndoPairPending = false;
	void SettleUndoPair(bool bWasUndo);

	/** Loop preview: armed by Ctrl+R, resolved by a click. */
	bool bPreviewingLoop = false;
	bool bLoopIsColumn = false;

	/** How many lines the armed loop cut will insert. The wheel changes it. */
	int32 LoopCount = 1;
	int32 LoopIndex = INDEX_NONE;

	/** Falloff weight per point index of the selected patch, empty when off. */
	TArray<float> SoftInfluence;

	/** Shared by BoxSelect and FrustumSelect: they differ only in the test. */
	bool ApplyMarquee(const TArray<TPair<FSurfPointRef, FVector>>& Candidates, TFunctionRef<bool(const FVector&)> Contains, bool bSelect);

	/** Tracks the shift-click row/column/point cycle: 0 row, 1 column, 2 point. */
	FSurfPointRef LastRowColumnPoint;
	int32 RowColumnStage = 2;
	/** What the last stage of the Alt-click cycle put into the selection, so the next stage takes only that out (S164a). */
	TArray<FSurfPointRef> RowColumnAdded;
};
