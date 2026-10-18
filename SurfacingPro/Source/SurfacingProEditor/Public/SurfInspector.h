// Copyright MapModelers. All Rights Reserved.
//
// The surface inspector, as a plain UObject shown in a details view.
//
// Every button acts on the live selection and returns - no accept, no cancel,
// and the selection is never cleared by a verb (D16.1). Multi-selection is
// Relative by default: an edit applies per face from its own current value,
// rather than flattening every face to the first one's, which is what
// mappers complain about elsewhere.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SurfTypes.h"
#include "SurfSettings.h"
#include "SurfInspector.generated.h"

class USurfEdMode;
class UMaterialInterface;

UENUM()
enum class ESurfMultiEditMode : uint8
{
	/** Each edit applies per face from its own value. */
	Relative,
	/** The first selected face's values are copied onto all of them. */
	MatchFirst
};

UCLASS()
class SURFACINGPROEDITOR_API USurfInspector : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<USurfEdMode> Mode;

	UPROPERTY(VisibleAnywhere, Category = "Selection")
	int32 SelectedFaces = 0;

	UPROPERTY(VisibleAnywhere, Category = "Selection")
	int32 SelectedPoints = 0;

	UPROPERTY(VisibleAnywhere, Category = "Selection")
	FString PatchInfo;

	/**
	 * What the last verb did, or why it declined. A verb that refuses in
	 * silence is indistinguishable from one that is broken.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Selection")
	FString LastResult;

	/** Counts every result, so the viewport bar can tell a repeated refusal from the one it already showed. */
	uint32 ResultSerial = 0;

	/** When the last result was written, in FPlatformTime seconds; the bar shows it for a few seconds after. */
	double ResultAt = 0.0;

	/** Every verb reports through this; the bar and the panel both read it. */
	void SetResult(const FString& Text)
	{
		LastResult = Text;
		++ResultSerial;
		ResultAt = FPlatformTime::Seconds();
		// The last two hundred, for the Notes window (S153f): what the
		// corner shows for a few seconds, kept.
		ResultHistory.Add(Text);
		if (ResultHistory.Num() > 200)
		{
			ResultHistory.RemoveAt(0, ResultHistory.Num() - 200);
		}
	}

	/** Every result in order, oldest first, capped; the Notes window reads it. */
	TArray<FString> ResultHistory;

	/**
	 * Whether the last result was a verb declining. Results are written in
	 * one voice: a refusal names the verb and a colon ("extrude: ..."), a
	 * result speaks in the past tense ("extruded ..."), so the text says.
	 */
	bool LastResultWasRefusal() const
	{
		return IsRefusal(LastResult);
	}

	/** Whether a line is a verb declining, by its voice; the Notes window reads the history with it (S153f). */
	static bool IsRefusal(const FString& Line)
	{
		static const TCHAR* Refusals[] =
		{
			TEXT("bridge:"), TEXT("delete:"), TEXT("extrude:"), TEXT("flatten:"), TEXT("join:"),
			TEXT("select all:"), TEXT("separate:"), TEXT("solidify:"), TEXT("split:"), TEXT("weld:"),
			TEXT("weld by distance:"), TEXT("nothing"), TEXT("convert:"), TEXT("insert loop:"), TEXT("bevel:"),
			TEXT("duplicate:"), TEXT("copy:"), TEXT("cut:"), TEXT("paste:"), TEXT("material:")
		};

		for (const TCHAR* Prefix : Refusals)
		{
			if (Line.StartsWith(Prefix))
			{
				return true;
			}
		}

		return false;
	}

	UPROPERTY(EditAnywhere, Category = "Selection")
	ESurfMultiEditMode MultiEdit = ESurfMultiEditMode::Relative;

	/** Centimetres, independent of Scale: +1 moves one centimetre, never one tile. */
	UPROPERTY(EditAnywhere, Category = "Projection")
	FVector2D ShiftStepCm = FVector2D(10.0, 10.0);

	/** Metres per tile. (1,1) repeats the material every metre. */
	UPROPERTY(EditAnywhere, Category = "Projection", meta = (ClampMin = "0.01"))
	FVector2D Scale = FVector2D(1.0, 1.0);

	UPROPERTY(EditAnywhere, Category = "Projection", meta = (ClampMin = "-360.0", ClampMax = "360.0"))
	double RotationDegrees = 0.0;

	UPROPERTY(EditAnywhere, Category = "Projection", meta = (ClampMin = "1", ClampMax = "64"))
	int32 FitRepeatU = 1;

	UPROPERTY(EditAnywhere, Category = "Projection", meta = (ClampMin = "1", ClampMax = "64"))
	int32 FitRepeatV = 1;

	// --- Verbs --------------------------------------------------------------

	/** Reset to the axial projection for each face's own normal. */
	UFUNCTION(CallInEditor, Category = "Projection")
	void Align();

	/** Whole number of tiles across each face, aspect preserved. */
	UFUNCTION(CallInEditor, Category = "Projection")
	void Fit();

	/** One tile across each face, aspect discarded. */
	UFUNCTION(CallInEditor, Category = "Projection")
	void Stretch();

	/** Apply Scale and RotationDegrees to the selection. */
	UFUNCTION(CallInEditor, Category = "Projection")
	void ApplyScaleAndRotation();

	UFUNCTION(CallInEditor, Category = "Shift")
	void ShiftLeft();

	UFUNCTION(CallInEditor, Category = "Shift")
	void ShiftRight();

	UFUNCTION(CallInEditor, Category = "Shift")
	void ShiftUp();

	UFUNCTION(CallInEditor, Category = "Shift")
	void ShiftDown();

	/** Read the active face's values back into the panel. */
	// --- Create -------------------------------------------------------------

	/** A flat sheet in front of the camera. No blockout box needed. */
	UFUNCTION(CallInEditor, Category = "Create")
	void NewPatch();

	/**
	 * A new patch, said out loud (S111a): how many points across and down,
	 * how big, and what shape to start from.
	 *
	 * NewPatch is this with the settings' answers. Both note the operation, so
	 * the adjust frame comes up over the new patch and every number in it is
	 * still a number - "The popup param window shows (its a simple patch by
	 * default) but some parameter exposed. and a dropdown for some custom
	 * preset shapes."
	 */
	void NewPatchWith(int32 Columns, int32 Rows, double SizeCm, int32 Preset);

	/** The Lattice verb (S189a): into a patch's cage, out of it, or a new one with the last numbers. */
	void MakeLattice();

	/**
	 * A box in the level and a face selected, in one press. Everything else
	 * here acts on a selection, so without this the panel cannot start a
	 * shape - and hunting for the actor in the outliner to press a button on
	 * it is not a workflow.
	 */
	UFUNCTION(CallInEditor, Category = "Create")
	void NewBox();

	/** A cylinder brush the same way: in front of the camera, on the grid, its top selected. */
	UFUNCTION(CallInEditor, Category = "Create")
	void NewCylinder();

	/** Both of the above: the brush placed, surfaced, added and its top selected. */
	void MakeNewBrush(bool bCylinder);

	// --- Patches ------------------------------------------------------------
	// Select a brush face and press one of these: the brush is replaced by a
	// patch filling the same space. Every curved thing is one of them.

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void MakeBevel();

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void MakeEndcap();

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void MakeCylinder();

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void MakeCone();

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void MakeSheet();

	/** The old Sheet: the whole brush replaced by one flat grid over its box. */
	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void ReplaceWithSheet();

	UPROPERTY(EditAnywhere, Category = "Patch|Create", meta = (ClampMin = "2", ClampMax = "32"))
	int32 SheetWidth = 5;

	UPROPERTY(EditAnywhere, Category = "Patch|Create", meta = (ClampMin = "2", ClampMax = "32"))
	int32 SheetHeight = 5;

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void InsertRow();

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void InsertColumn();

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void RemoveRow();

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void RemoveColumn();

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Respace();

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Invert();

	UPROPERTY(EditAnywhere, Category = "Patch|Edit", meta = (ClampMin = "1", ClampMax = "32"))
	int32 Subdivision = 8;

	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void ApplySubdivision();

	/**
	 * Apply: replace the curve with the vertex grid its subdivision
	 * produces. A curve's control points steer the surface from outside it,
	 * which is right for blocking a form and wrong once the work is moving
	 * individual vertices. Subdivision is retired by the conversion, because
	 * a sheet's points already are its vertices.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Edit", meta = (DisplayName = "Apply"))
	void ConvertToSheet();

	/** Grow the patch past the edge the selected row or column sits on. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Extrude();

	/** Copy the patch, offset along its own normal, and select the copy. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Duplicate();

	/** Cut the patch in two along the selected row or column. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Split();

	/** Double the resolution of the selected patches, along the axes ticked below. */
	UFUNCTION(CallInEditor, Category = "Patch|Geometry")
	void Subdivide();

	UPROPERTY(EditAnywhere, Category = "Patch|Geometry")
	bool bSubdivideRows = true;

	UPROPERTY(EditAnywhere, Category = "Patch|Geometry")
	bool bSubdivideColumns = true;

	/** Front, back and four skirts - a surface given thickness. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Solidify();

	UPROPERTY(EditAnywhere, Category = "Patch|Edit", meta = (ClampMin = "0.1"))
	double ThicknessCm = 20.0;

	/** Which side of the surface the shell grows: -1 behind it (the front stays), 0 half each way, 1 in front. */
	UPROPERTY(EditAnywhere, Category = "Patch|Edit", meta = (ClampMin = "-1", ClampMax = "1"))
	int32 SolidifySide = -1;

	/** The four skirts joining front and back. Off: front and back only. */
	UPROPERTY(EditAnywhere, Category = "Patch|Edit")
	bool bSolidifyRim = true;

	/** Keep the shell as thick at a fold as on a flat run. */
	UPROPERTY(EditAnywhere, Category = "Patch|Edit")
	bool bSolidifyEven = false;

	/** Turn the surface inside out. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void FlipNormals();

	/**
	 * Collapse the selected points onto their centre. The cells between them
	 * go degenerate and are dropped at build, which is how a quad grid ends
	 * up holding a triangle.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Weld();

	/** Collapse everything already closer together than the tolerance. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void WeldByDistance();

	UPROPERTY(EditAnywhere, Category = "Patch|Edit", meta = (ClampMin = "0.01", ClampMax = "500.0"))
	double WeldToleranceCm = 1.0;

	/**
	 * How far apart two edges may be and still be one edge, for Join. Two
	 * patches are one when their shared edge matches point for point; this
	 * is only the slack for numbers that are not quite exact.
	 */
	UPROPERTY(EditAnywhere, Category = "Patch|Edit", meta = (ClampMin = "0.0001", ClampMax = "100.0"))
	double JoinToleranceCm = 0.01;

	/**
	 * Fold whichever patch shares an edge with this one into it. The shared
	 * line has to carry the same number of points on both sides.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Join();

	/**
	 * Lift the picked block of faces out as its own patch. Face sub-mode, and
	 * the picked cells have to form a rectangle.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Separate();

	/** A new patch between two selected lines of the same length. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Bridge();

	/** A staircase between two selected lines of the same length. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Steps();

	/** The selected border meets the one across from it, and the surface carries on without a crease. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void Continue();

	/** A new brush standing on each picked brush face, a grid step out. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void ExtrudeBrushFace();

	/** Turn the selected patches to face away from the middle of the selection. */
	UFUNCTION(CallInEditor, Category = "Patch|Edit")
	void FaceOutward();

	// --- Layering -----------------------------------------------------------
	// A layer is an ordinary patch lifted a hair along its normal at build
	// time, so the ordering is geometric rather than a material trick (D14).

	UPROPERTY(EditAnywhere, Category = "Patch|Layering", meta = (ClampMin = "0", ClampMax = "4"))
	int32 Layer = 0;

	UFUNCTION(CallInEditor, Category = "Patch|Layering")
	void ApplyLayer();

	/** The selection copied one layer up: lifted a hair at build, so it draws over what is beneath. */
	UFUNCTION(CallInEditor, Category = "Patch|Layering")
	void MakeLayer();

	/** The selected patches to the next level, 0 to 3 and round again: what the bar's Layer does. */
	UFUNCTION(CallInEditor, Category = "Patch|Layering")
	void CycleLayer();

	/** Set every selected patch's level outright, as the bar's right-click does. */
	void SetLayer(int32 Level);

	/** The active patch's level, for the bar. -1 with no patch. */
	int32 GetActiveLayer() const;

	// --- Shading ------------------------------------------------------------
	// Smooth is right for a curve and wrong for a wall with a corner in it.

	UPROPERTY(EditAnywhere, Category = "Patch|Shading")
	ESurfShading Shading = ESurfShading::Smooth;

	/** Only used by ByAngle: how far a cell may turn before the fold goes hard. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shading", meta = (ClampMin = "1.0", ClampMax = "179.0"))
	double SmoothingAngleDegrees = 40.0;

	UFUNCTION(CallInEditor, Category = "Patch|Shading")
	void ApplyShading();

	/**
	 * Every patch in the level, with the selected one marked. Which patch a
	 * verb is about to act on was previously something you found out by
	 * pressing the verb.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Patch|Create")
	FString Patches;

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void NextPatch();

	UFUNCTION(CallInEditor, Category = "Patch|Create")
	void PreviousPatch();

	/** What the current keyboard scheme binds, so the shortcuts need no manual. */
	UPROPERTY(VisibleAnywhere, Category = "Controls")
	FString Shortcuts;

	// --- Patch surfacing ----------------------------------------------------
	// The mapping is stored on the patch, so these choose a rule rather than
	// write UVs once. Move a control point afterwards and the rule re-runs.

	/** Distance along the surface at Scale metres per tile. The mapping for curves. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void Natural();

	/** Even (S69c): every cell the same size in the texture, one tile a cell to start with - square UVs, no stretch across the net. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void Even();

	/** Planar projection from the patch's own average normal - what a brush face gets. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void Planar();

	/**
	 * One projection, along a world axis, shared by every patch the selection
	 * holds - so a wall that bends across several patches has no seam where
	 * they meet. ProjectAxis chooses the axis.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void Project();

	/** Project: 0 the selection's own direction, 1 X, 2 Y, 3 Z. */
	UPROPERTY(EditAnywhere, Category = "Patch|Surface", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ProjectAxis = 0;

	/** Take the last selected brush face's exact projection, so a seam disappears. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void InheritFaceSurface();

	/** Hold the current UVs and stop re-deriving them. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void Freeze();

	/** Pick the scale that spans FitRepeatU by FitRepeatV tiles across the patch. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void FitPatch();

	/** Push the panel's Scale onto the patch, in metres per tile. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void ApplyPatchScale();

	/** Turn the mapping by RotationDegrees. */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void RotatePatchSurface();

	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void FlipPatchU();

	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void FlipPatchV();

	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void SwapPatchUV();

	/** Tiles, not centimetres: a patch has no single plane to measure against. */
	UPROPERTY(EditAnywhere, Category = "Patch|Surface")
	FVector2D PatchShiftTiles = FVector2D(0.25, 0.25);

	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void ShiftPatchU();

	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void ShiftPatchV();

	/**
	 * Every patch the selection holds to the same texels per metre, by
	 * changing its scale: the number that says whether a wall matches its
	 * neighbours. The active patch's density is the one to match unless one
	 * is typed in the frame.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Surface")
	void NormaliseDensity();

	/** Texels per metre across the selected patch, read from TextureTexels. */
	UPROPERTY(VisibleAnywhere, Category = "Patch|Surface")
	FVector2D PatchDensity = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Patch|Surface")
	FVector2D TextureTexels = FVector2D(1024.0, 1024.0);

	// --- Soft selection -----------------------------------------------------
	// These write through to the project settings, but they live here because
	// this is where they get switched on and off - not in a settings dialog.

	UPROPERTY(EditAnywhere, Category = "Soft Selection")
	bool bSoftSelection = false;

	UPROPERTY(EditAnywhere, Category = "Soft Selection", meta = (ClampMin = "1.0",
		EditCondition = "bSoftSelection", EditConditionHides))
	double SoftRadiusCm = 200.0;

	UPROPERTY(EditAnywhere, Category = "Soft Selection", meta = (
		EditCondition = "bSoftSelection", EditConditionHides))
	ESurfFalloff SoftFalloff = ESurfFalloff::Smooth;

	/** Copy the panel's soft-selection values into the settings and re-derive. */
	void PushSoftSelection();

	/** Read the settings back into the panel, so the two never disagree. */
	void PullSoftSelection();

	// --- Shape operators ----------------------------------------------------
	// Each one runs the moment it is pressed, with the numbers below; the
	// viewport bar then offers those numbers to adjust, and every change
	// undoes the operation and runs it again. There is nothing to apply.

	UFUNCTION(CallInEditor, Category = "Patch|Shape")
	void Smooth();

	UFUNCTION(CallInEditor, Category = "Patch|Shape")
	void Inflate();
	void Random();

	UFUNCTION(CallInEditor, Category = "Patch|Shape")
	void Flatten();

	/** Flatten along one world axis: 0 X, 1 Y, 2 Z. Every selected point's coordinate becomes their mean. */
	void FlattenAlong(int32 Axis);

	/** How far the bevel can spread before it reaches the next line. Drives the slider's range. */
	double BevelWidthLimitCm() const;

	/** Bridge: joined into the patches it meets whole, so two edges make one patch when they can. */
	UPROPERTY(EditAnywhere, Category = "Bridge")
	bool bBridgeJoins = true;

	/**
	 * Round off the crease along the selected row or column: the line is
	 * replaced by BevelSegments lines spread BevelWidthCm either side.
	 */
	UFUNCTION(CallInEditor, Category = "Patch|Shape")
	void Bevel();

	/** Run one shape operation with the numbers in Op, as one undo step. Smooth, inflate, flatten, bevel. */
	bool ApplyShape(const struct FSurfOperation& Op);

	/**
	 * The one place a mapping is set: Flow, Align, Project or Fit, over every
	 * patch the selection holds, in one step, noted so the metres per tile and
	 * the turn can be dragged in the adjust frame afterwards.
	 */
	bool ApplySurface(const struct FSurfOperation& Op);

	UPROPERTY(EditAnywhere, Category = "Patch|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double Strength = 0.5;

	UPROPERTY(EditAnywhere, Category = "Patch|Shape", meta = (ClampMin = "1", ClampMax = "50"))
	int32 Iterations = 4;

	/** Hold the outer ring still, so a smoothed patch still meets its neighbours. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shape")
	bool bPreserveBorder = true;

	/** Centimetres along the surface normal, for Inflate. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shape")
	double DistanceCm = 10.0;

	/** How far back from the crease the bevel starts, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shape", meta = (ClampMin = "0.0"))
	double BevelWidthCm = 50.0;

	/** Rows or columns the bevel is built from. More is rounder. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shape", meta = (ClampMin = "1", ClampMax = "8"))
	int32 BevelSegments = 2;

	/** Keep the crease sharp and only add the lines either side of it - support lines, not a round. */
	UPROPERTY(EditAnywhere, Category = "Patch|Shape")
	bool bBevelKeepsCrease = false;

	// --- Material -------------------------------------------------------------
	// The material is the unit of surfacing (M3): a patch or a brush face
	// wears one, any the user owns. Assigning goes through here; the bar's
	// picker and the tests call the verb directly.

	/** What Assign Material puts on the selection. Empty clears it: the group's material, else the checker. */
	UPROPERTY(EditAnywhere, Category = "Material")
	TObjectPtr<UMaterialInterface> MaterialToAssign;

	/** Put MaterialToAssign on every patch the selection spans, and on the selected brush faces. The selection is kept. */
	UFUNCTION(CallInEditor, Category = "Material")
	void AssignMaterial();

	/**
	 * The verb itself: the patches the selection spans (the active one when
	 * nothing else says) and the selected brush faces take the material, in
	 * one transaction; a material's own default scale is applied wherever the
	 * material changed. False, with the reason on the bar, when nothing is
	 * selected.
	 */
	bool SetMaterialOnSelection(UMaterialInterface* Material);

	void PullFromActiveFace();

	void RefreshSelectionCount();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:

	void ApplyShift(const FVector2D& DeltaCm);
	void RunVerb(const FText& TransactionText, TFunctionRef<void(struct FSurfFace&)> Apply);
};
