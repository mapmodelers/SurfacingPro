// Copyright MapModelers. All Rights Reserved.
//
// Core editable types. These live in the Runtime module because actors
// serialize them into maps, so they must exist in packaged builds even
// though editing does not.
//
// Conventions pinned here and asserted by tests (see SurfMathTests.cpp);
// changing any of them later is a versioned migration, not an edit:
//   - Units are centimetres. Never any other unit anywhere in this project.
//   - FSurfFaceUV::Shift is centimetres and independent of Scale.
//   - FSurfFaceUV::Scale is METRES PER TILE: (1,1) repeats every metre.
//   - Rotation is not stored. UAxis/VAxis are the single source of truth
//     and rotation is derived for display, so texture lock is just
//     "transform the axes with the primitive".
//   - Plane convention: outward normal, Dot(N, X) == W for points on the
//     plane, Dot(N, X) - W <= 0 for points inside the brush.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/ObjectPtr.h"
#include "SurfTypes.generated.h"

class UMaterialInterface;

/** Kind of brush. The clip material zoo is gone; Clip means collision-only. */
UENUM(BlueprintType)
enum class ESurfBrushKind : uint8
{
	/** Rendered and collides. */
	Solid,
	/** Collision only, never rendered. */
	Clip,
	/** Rendered nowhere, kept for collision. */
	NoDraw
};

UENUM(BlueprintType)
enum class ESurfPatchType : uint8
{
	/** Bezier. Odd control dimensions, subdivided at bake and preview. */
	Curve,
	/** Control points are the vertices. Any dimensions, per-quad diagonal flip. */
	Sheet
};

/**
 * How a patch's UVs are derived. Stored rather than baked, so moving a
 * control point re-derives the mapping instead of stretching the texture -
 * the difference between surfacing a shape and surfacing it once.
 */
UENUM()
enum class ESurfPatchMapping : uint8
{
	/** Distance along the surface, at Scale metres per tile. For anything curved. */
	Natural,
	/** A planar projection, exactly as a brush face gets. For continuing a wall. */
	Planar,
	/** Control point UVs are held as they are. Nothing re-derives them. */
	Manual,

	/**
	 * Even (S69c, re-cut S150c): each cell its own size in the texture, in
	 * one unit for the whole patch - the mean cell - measured once when Even
	 * is pressed and kept on the lines from then on, so a long cell is a
	 * long run of tiles, a square one is one, and no point moved afterwards
	 * can stretch the net. Scale is cells a tile, not metres. Blender's
	 * "follow active quads".
	 */
	Grid
};

UENUM(BlueprintType)
enum class ESurfCollision : uint8
{
	None,
	/** Low-subdivision triangle mesh. */
	BaseGrid,
	/** The render mesh. */
	Full
};

/**
 * Persistent fill intent. Default None: the projection is a stored transform
 * and a geometry edit never moves the texture. When set, the verb is
 * re-applied after a geometry edit so a fitted face stays fitted.
 * Storing only intent and re-deriving constantly is why nudging a vertex
 * can make a texture jump elsewhere; we store the result and make
 * re-derivation opt-in per face.
 */
UENUM(BlueprintType)
enum class ESurfFill : uint8
{
	None,
	/** Whole number of tiles across the face, aspect preserved. */
	Fit,
	/** Exactly one tile across the face, aspect discarded. */
	Stretch
};

/**
 * Per-primitive flags. Distinct bits, asserted by a test (D16.4) - a flags
 * value that equals the OR of others silently does several things at once.
 */
UENUM(meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class ESurfPrimitiveFlags : uint32
{
	None			= 0,
	/** Merge freely at bake, never an occluder. */
	Detail			= 1 << 0,
	/** Rendered, no collision. */
	NonColliding	= 1 << 1,
	/** Casts no shadow. */
	NoShadow		= 1 << 2,
	/** Dropped from the render pass, kept for collision. */
	NoDraw			= 1 << 3
};
ENUM_CLASS_FLAGS(ESurfPrimitiveFlags);

/**
 * Projection state for one brush face. Never an unwrap: UVs are computed
 * from world position against two axes.
 */
USTRUCT(BlueprintType)
struct SURFACINGPRO_API FSurfFaceUV
{
	GENERATED_BODY()

	/** Live projection axes, unit length, mutually orthogonal and tangent to the face. */
	UPROPERTY(EditAnywhere, Category = "Projection")
	FVector UAxis = FVector(1.0, 0.0, 0.0);

	UPROPERTY(EditAnywhere, Category = "Projection")
	FVector VAxis = FVector(0.0, -1.0, 0.0);

	/** Centimetres. Independent of Scale: +1 moves one centimetre, never one tile. */
	UPROPERTY(EditAnywhere, Category = "Projection")
	FVector2D Shift = FVector2D::ZeroVector;

	/** Metres per tile. (1,1) repeats the material every metre. Must not be zero. */
	UPROPERTY(EditAnywhere, Category = "Projection")
	FVector2D Scale = FVector2D(1.0, 1.0);

	/** Texture lock: axes are transformed with the primitive so the texture stays put. */
	UPROPERTY(EditAnywhere, Category = "Projection")
	bool bLock = true;

	UPROPERTY(EditAnywhere, Category = "Projection")
	ESurfFill Fill = ESurfFill::None;

	bool IsValid() const
	{
		return !FMath::IsNearlyZero(Scale.X) && !FMath::IsNearlyZero(Scale.Y)
			&& !UAxis.IsNearlyZero() && !VAxis.IsNearlyZero();
	}
};

/** One plane of a convex brush, plus how it is surfaced. */
USTRUCT(BlueprintType)
struct SURFACINGPRO_API FSurfFace
{
	GENERATED_BODY()

	/** Outward normal, world space. Points inside satisfy Dot(N, X) - W <= 0. */
	UPROPERTY()
	FPlane Plane = FPlane(FVector(0.0, 0.0, 1.0), 0.0);

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FSurfFaceUV UV;

	/** The material this face wears. Empty: the group's preview material, else the checker (M3). */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	TObjectPtr<UMaterialInterface> Material;

	/** 0 means flat shaded. Faces sharing a non-zero group are smoothed together. */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	int32 SmoothingGroup = 0;

	/** ESurfPrimitiveFlags bitmask. */
	UPROPERTY(EditAnywhere, Category = "Surfacing", meta = (Bitmask, BitmaskEnum = "/Script/SurfacingPro.ESurfPrimitiveFlags"))
	int32 Flags = 0;

	FSurfFace() = default;
	explicit FSurfFace(const FPlane& InPlane) : Plane(InPlane) {}
};

/**
 * Convex by construction. Faces are planes; vertices are derived on demand
 * and cached against EditSerial. Editing a vertex re-derives the planes, so
 * a brush can never become concave.
 */
USTRUCT(BlueprintType)
struct SURFACINGPRO_API FSurfBrush
{
	GENERATED_BODY()

	/** Stable across undo/redo; also the element handle payload. */
	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	TArray<FSurfFace> Faces;

	UPROPERTY(EditAnywhere, Category = "Brush")
	ESurfBrushKind Kind = ESurfBrushKind::Solid;

	UPROPERTY(EditAnywhere, Category = "Brush")
	bool bCollision = true;

	UPROPERTY(EditAnywhere, Category = "Brush")
	bool bCastShadow = true;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (Bitmask, BitmaskEnum = "/Script/SurfacingPro.ESurfPrimitiveFlags"))
	int32 Flags = 0;

	/**
	 * How the sides shade where they meet (S171a): two sides that turn by
	 * less than this angle share one normal along their edge and read as
	 * one curved surface; more, and the edge is a crease. 0 is every edge
	 * hard - a box; a cylinder is made with enough to read round.
	 */
	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0", ClampMax = "180"))
	float CreaseAngleDeg = 0.0f;

	/** Hidden while modelling (H), as a patch is (S184c): "H to hide prim doesn't work". Alt+H shows it again. */
	UPROPERTY()
	bool bHidden = false;

	/** The groups it is in, outermost first (S249b): pieces sharing an id are picked and moved together. */
	UPROPERTY()
	TArray<FGuid> GroupPath;

	/** Bumped on every edit. Drives incremental preview rebuild and cache invalidation. */
	UPROPERTY()
	int32 EditSerial = 0;

	/**
	 * Assigned when the primitive is actually created, never by the
	 * constructor: a non-deterministic default trips UE's member
	 * initialization test, and it burns a Guid on every temporary.
	 */
	void EnsureId()
	{
		if (!Id.IsValid())
		{
			Id = FGuid::NewGuid();
		}
	}
};

/**
 * One control point. UVs live here, so moving a point drags the texture with
 * it. SavedUV holds the pre-edit values so Fit / Flow / Project are
 * revertible without spending an undo step.
 */
USTRUCT(BlueprintType)
struct SURFACINGPRO_API FSurfControlPoint
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Pos = FVector::ZeroVector;

	UPROPERTY()
	FVector2D UV = FVector2D::ZeroVector;

	UPROPERTY()
	FVector2D SavedUV = FVector2D::ZeroVector;

	/**
	 * Vertex colour, R G B: painted with the brush, a channel at a time,
	 * for any material that reads it - wear, a blend, seam hiding. Black
	 * and empty to start with: nothing is painted until it is (D71).
	 */
	UPROPERTY()
	FColor Tint = FColor(0, 0, 0, 0);

	/** The A channel of the same colour, 0..1, painted the same way. Empty to start with. */
	UPROPERTY()
	float Weight = 0.0f;

	/** Sheet only: flips this quad's diagonal. Ignored on Curve patches. */
	UPROPERTY()
	uint8 bTurnedEdge = 0;

	FSurfControlPoint() = default;
	explicit FSurfControlPoint(const FVector& InPos) : Pos(InPos) {}
};

/**
 * A placed prefab's own paint on one of the prefab's patches (S251j): the
 * colour and the A channel of every control point, in the patch's order.
 * A patch with none, or whose point count no longer matches, shows the
 * prefab's own paint.
 */
USTRUCT()
struct SURFACINGPRO_API FSurfPaintOverride
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid Patch;

	UPROPERTY()
	TArray<FColor> Tint;

	UPROPERTY()
	TArray<float> Weight;
};

/**
 * Curve (bezier, odd dims) or Sheet (points are vertices). One surface per
 * patch: layering is geometric, a layer being a patch stacked on the one
 * below at StackLevel N, separated by the compiler rather than by WPO (D14).
 */
/** How a patch is shaded across its own cells. */
UENUM()
enum class ESurfShading : uint8
{
	/** One averaged normal per control point. Right for anything curved. */
	Smooth,

	/** Every triangle flat. Right for anything that is meant to read as panels. */
	Faceted,

	/** Averaged, except where a cell turns away by more than the angle. */
	ByAngle
};

/**
 * What a new patch starts as (S111a). Flat unless he says otherwise; the rest
 * are the shapes a blockout wants before it wants anything else, each of them
 * something a grid of points can be exactly rather than approximately.
 */
UENUM()
enum class ESurfSheetPreset : uint8
{
	Flat,
	Ramp,
	Arch,
	Dome,
	Bowl,
	Count UMETA(Hidden)
};

/**
 * A lattice round a patch (S189a): a box of cage points, ResX by ResY by
 * ResZ, in the group's space, and each patch point's place in the box (0 to
 * 1 each way). A move of cage points carries the patch points by the smooth
 * blend of the moves (SurfLattice).
 */
USTRUCT()
struct SURFACINGPRO_API FSurfLattice
{
	GENERATED_BODY()

	UPROPERTY()
	int32 ResX = 0;

	UPROPERTY()
	int32 ResY = 0;

	UPROPERTY()
	int32 ResZ = 0;

	/** ResX * ResY * ResZ points: X fastest, then Y, then Z. */
	UPROPERTY()
	TArray<FVector> Cage;

	/** Each patch point's place in the box when the lattice was made. */
	UPROPERTY()
	TArray<FVector> Params;

	/** The patch points as the lattice last left them: a move of the whole patch since carries the cage along. */
	UPROPERTY()
	TArray<FVector> Rest;

	bool HasCage() const { return ResX >= 2 && ResY >= 2 && ResZ >= 2 && Cage.Num() == ResX * ResY * ResZ; }
	int32 Index(int32 X, int32 Y, int32 Z) const { return (Z * ResY + Y) * ResX + X; }
	void Reset() { ResX = 0; ResY = 0; ResZ = 0; Cage.Reset(); Params.Reset(); Rest.Reset(); }
};

USTRUCT(BlueprintType)
struct SURFACINGPRO_API FSurfPatch
{
	GENERATED_BODY()

	/** Bezier parity: a Curve needs 2n+1 control points per axis. */
	static constexpr int32 MinCurveDim = 3;
	static constexpr int32 MaxCurveDim = 15;
	static constexpr int32 MinSheetDim = 2;
	/** A sheet's points are its vertices, so the cap is about undo snapshots and editing, not tessellation: 64 lines each way (was 32; a third Subdivide of a 9-line patch needs 33). */
	static constexpr int32 MaxSheetDim = 64;
	static constexpr int32 MinSubdiv = 1;
	static constexpr int32 MaxSubdiv = 32;

	UPROPERTY()
	FGuid Id;

	UPROPERTY(EditAnywhere, Category = "Patch")
	ESurfPatchType Type = ESurfPatchType::Curve;

	UPROPERTY()
	int32 Width = 3;

	UPROPERTY()
	int32 Height = 3;

	/** Row major, Width * Height entries. */
	UPROPERTY()
	TArray<FSurfControlPoint> Points;

	UPROPERTY(EditAnywhere, Category = "Patch", meta = (ClampMin = "1", ClampMax = "32"))
	int32 SubdivU = 8;

	UPROPERTY(EditAnywhere, Category = "Patch", meta = (ClampMin = "1", ClampMax = "32"))
	int32 SubdivV = 8;

	/** The material this patch wears. Empty: the group's preview material, else the checker (M3). */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	TObjectPtr<UMaterialInterface> Material;

	/** Metres per tile, as FSurfFaceUV::Scale. */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FVector2D Scale = FVector2D(1.0, 1.0);

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	ESurfPatchMapping Mapping = ESurfPatchMapping::Natural;

	/** The projection used when Mapping is Planar. Its own Scale wins over Scale above. */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FSurfFaceUV Projection;

	/**
	 * Applied after the mapping, in tiles. Everything below is a post-step for
	 * the same reason Mapping is stored: a nudged texture must survive the
	 * next control point drag.
	 */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	FVector2D UVShift = FVector2D::ZeroVector;

	/**
	 * The control point Natural mapping measures from: its own UV is (0, 0)
	 * before the post-step, so it sits at UVShift, and every other point is
	 * measured along its row and column from there. Normally the corner at
	 * (0, 0). Growing the low edge of the grid moves the corner but not this
	 * point, which is how the surfacing stays put on the points nobody touched.
	 */
	UPROPERTY()
	int32 UVOriginX = 0;

	UPROPERTY()
	int32 UVOriginY = 0;

	/**
	 * The even mapping's rulers (S70c): the column's place across and the
	 * row's place down, in cells, one entry per line. Even lays them out
	 * 0, 1, 2 ...; a line inserted between two takes the place between
	 * them, so the points already there keep their UVs - "adding this edge
	 * has modified the uvs. it shouldnt". Empty, or the wrong length, means
	 * evenly spaced.
	 */
	UPROPERTY()
	TArray<float> GridU;

	UPROPERTY()
	TArray<float> GridV;

	UPROPERTY(EditAnywhere, Category = "Surfacing", meta = (ClampMin = "-360.0", ClampMax = "360.0"))
	double UVRotation = 0.0;

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	bool bUVSwap = false;

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	bool bUVFlipU = false;

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	bool bUVFlipV = false;

	/** 0 = base. N = Nth layer above, offset N * LayerOffsetCm along the normal at bake (D14). */
	UPROPERTY(EditAnywhere, Category = "Surfacing", meta = (ClampMin = "0", ClampMax = "8"))
	int32 StackLevel = 0;

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	int32 SmoothingGroup = 0;

	/**
	 * How the surface is shaded across its own cells.
	 *
	 * Smooth averages at every vertex, which is right for a curve and wrong
	 * for a wall with a corner in it. Faceted gives every triangle its own
	 * normal. ByAngle keeps the averaged normal except where a cell turns
	 * away by more than the angle below, which is what a fold wants: smooth
	 * along the run, hard at the crease.
	 */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	ESurfShading Shading = ESurfShading::Smooth;

	UPROPERTY(EditAnywhere, Category = "Surfacing", meta = (ClampMin = "1.0", ClampMax = "179.0"))
	double SmoothingAngleDegrees = 40.0;

	/**
	 * Lines shaded hard whatever the shading above says (S171a): a control
	 * row or column (its index) on a span boundary - the surface passes
	 * through those - where the normals do not cross. Crease sets them.
	 */
	UPROPERTY(EditAnywhere, Category = "Surfacing")
	TArray<int32> HardRows;

	UPROPERTY(EditAnywhere, Category = "Surfacing")
	TArray<int32> HardColumns;

	/** Flips winding and normals. */
	UPROPERTY(EditAnywhere, Category = "Patch")
	bool bInvert = false;

	UPROPERTY(EditAnywhere, Category = "Patch")
	bool bCastShadow = true;

	UPROPERTY(EditAnywhere, Category = "Patch")
	ESurfCollision Collision = ESurfCollision::BaseGrid;

	UPROPERTY(EditAnywhere, Category = "Patch", meta = (Bitmask, BitmaskEnum = "/Script/SurfacingPro.ESurfPrimitiveFlags"))
	int32 Flags = 0;

	UPROPERTY()
	int32 EditSerial = 0;

	/**
	 * Hidden while modelling (H): drawn, picked and previewed as if absent,
	 * shown again by Alt+H. Saved with the patch and undone with the edit,
	 * as Blender's hide is.
	 */
	UPROPERTY()
	bool bHidden = false;

	/** The groups it is in, outermost first (S249b): pieces sharing an id are picked and moved together. */
	UPROPERTY()
	TArray<FGuid> GroupPath;

	/** The lattice round it, when one was made (S189a). */
	UPROPERTY()
	FSurfLattice Lattice;

	/** See FSurfBrush::EnsureId. */
	void EnsureId()
	{
		if (!Id.IsValid())
		{
			Id = FGuid::NewGuid();
		}
	}

	int32 NumPoints() const { return Width * Height; }

	const FSurfControlPoint& At(int32 X, int32 Y) const { return Points[Y * Width + X]; }
	FSurfControlPoint& At(int32 X, int32 Y) { return Points[Y * Width + X]; }

	/** Dimensions legal for this patch type, and Points sized to match. */
	bool HasValidDimensions() const
	{
		if (Points.Num() != Width * Height)
		{
			return false;
		}

		if (Type == ESurfPatchType::Curve)
		{
			return Width >= MinCurveDim && Width <= MaxCurveDim && (Width % 2) == 1
				&& Height >= MinCurveDim && Height <= MaxCurveDim && (Height % 2) == 1;
		}

		return Width >= MinSheetDim && Width <= MaxSheetDim
			&& Height >= MinSheetDim && Height <= MaxSheetDim;
	}
};

// --- Selection, as the undo system sees it ------------------------------------

/** One control point or one face of one patch, by id: what a selection is made of once the editor is out of the picture. */
USTRUCT()
struct SURFACINGPRO_API FSurfSavedRef
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid PatchId;

	UPROPERTY()
	int32 X = -1;

	UPROPERTY()
	int32 Y = -1;
};

/**
 * The editor's selection as it stood before an edit. Stored on the chunk the
 * edit touched, so the transaction that undoes the edit also brings back
 * what was selected when it was made - a bevel undone hands back the line
 * it was made from, not nothing.
 */
USTRUCT()
struct SURFACINGPRO_API FSurfSelectionSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FGuid ActivePatchId;

	/** Vertex, edge, face or object - the editor's own numbering. */
	UPROPERTY()
	uint8 SubMode = 0;

	UPROPERTY()
	TArray<FSurfSavedRef> Points;

	UPROPERTY()
	TArray<FSurfSavedRef> Faces;

	/** The patches picked as wholes (object sub-mode), those on this chunk - so an undo hands back all of them, not the active one alone. */
	UPROPERTY()
	TArray<FGuid> Patches;
};
