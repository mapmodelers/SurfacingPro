// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SurfKeymap.h"
#include "SurfSettings.generated.h"

/**
 * Which package's navigation the viewport answers to.
 *
 * Unreal and Blender disagree about almost every mouse button, so this cannot
 * be a per-key rebind: it is one scheme or the other, and the scheme decides
 * what the middle button and the wheel mean.
 */
UENUM(BlueprintType)
enum class ESurfCameraScheme : uint8
{
	/** Leave the viewport alone: Alt-drag orbit, right-drag fly. The default. */
	Unreal,
	/**
	 * Middle drag orbits, Shift pans, Ctrl dollies, wheel zooms, numpad snaps.
	 *
	 * Off by default, and honestly so: an editor mode can intercept input but
	 * it does not own the viewport. Camera speed, cursor capture, focus
	 * animation, orthographic handling and the drag tools all still belong to
	 * the viewport client, so what this can offer is a good imitation with
	 * seams rather than the real thing - and the seams are exactly where it
	 * feels like two systems fighting.
	 */
	Blender,
	/** Follow the key scheme. Blender keys get Blender navigation. */
	FollowKeyScheme
};

/**
 * What a drag in the viewport selects with. Blender keeps this as a tool
 * choice rather than a per-gesture decision: you pick box, lasso or brush
 * once, and from then on dragging does that.
 */
UENUM(BlueprintType)
enum class ESurfSelectTool : uint8
{
	/** A rectangle between the press and the release. */
	Box,
	/** A freehand loop; everything inside the closed path. */
	Lasso,
	/** Paint over points with a circular brush. */
	Brush
};

/** How From Cavity meets the channel already there (S212a; the masks it was made for went in S251e). */
UENUM()
enum class ESurfMaskBlend : uint8
{
	Replace,
	Add,
	Subtract,
	Multiply,
	Lighten,
	Darken,
};

/** Which channel of the vertex colour the paint brush writes. */
UENUM(BlueprintType)
enum class ESurfPaintChannel : uint8
{
	R,
	G,
	B,
	A,

	/** The three colour channels at once, towards the paint colour (S70b). */
	Colour
};

/**
 * What the brush, Fill and the adjustments write (S248a: "instead of 'Col'
 * just have R G B all three enabled"): the ticked channels. R, G and B all
 * ticked are the colour - a drag paints the swatch, Ctrl the second one -
 * any other set goes up with a drag and down with Ctrl, and A rides beside
 * the colour when it is ticked too.
 */
struct SURFACINGPRO_API FSurfPaintSet
{
	/** 1 R, 2 G, 4 B, 8 A. */
	int32 Bits = 1;

	bool IsColour() const { return (Bits & 7) == 7; }
	/** The channels moved one by one: every ticked one, less R G B when they are the colour. */
	void Singles(TArray<ESurfPaintChannel, TInlineAllocator<4>>& Out) const;
	/** Every ticked channel, R G B A in order: what the adjustments write. */
	void All(TArray<ESurfPaintChannel, TInlineAllocator<4>>& Out) const;
	/** The Paint view's kind: the colour for R G B or several channels, else the one. */
	int32 ViewIndex() const;
	/** For a line: "R", "R+G", "colour", "colour+A". */
	FString Name() const;
};

/**
 * Falloff shape for soft selection. The curve matters more than it sounds:
 * Smooth is the one that makes a dragged point look like a hill rather than
 * a spike, and it is what proportional editing means to most people.
 */
UENUM(BlueprintType)
enum class ESurfFalloff : uint8
{
	Smooth,
	Sphere,
	Root,
	Sharp,
	Linear,
	Constant,
	/** Falls away fast near the selection, then trails: Blender's inverse square. */
	InverseSquare,
	/** Each point follows by its own fixed share of a linear falloff: a rough surface, not a hill. */
	Random,

	Count UMETA(Hidden)
};

/** Which point of what moves lands on the target (S205a). */
UENUM()
enum class ESurfSnapWith : uint8
{
	/** Whichever moving point is nearest a target. */
	Closest,
	/** The middle of the moving points' box. */
	Centre,
	/** The average of the moving points. */
	Median,
	/** The point picked last. */
	Active
};

/** How patch control points snap, independently of the brush grid (D9). */
UENUM(BlueprintType)
enum class ESurfPointSnap : uint8
{
	Off,
	/** FinePointSnapCm, effectively unnoticed while sculpting. */
	Fine,
	/** The current brush grid step. */
	SameAsGrid
};

/**
 * Project settings. The grid ladder is metric and there is no other preset:
 * never any legacy unit anywhere in this project (D9).
 */
/**
 * Which patches draw their wires, and whether selection reaches through a
 * surface. Z walks through these.
 */
UENUM()
enum class ESurfWireView : uint8
{
	/** The patches being edited. Selection takes only what is visible. */
	Selected,

	/** Every patch, the others in a quiet tint. Selection takes only what is visible. */
	All,

	/** Every patch, and selection reaches through surfaces. */
	XRay
};

/**
 * How the rest of the level looks round a prefab edited where it stands
 * (S249a: "it may be ghost as current. it may be wire only. it may be
 * regular (follow the current viewmode)"). Out of reach whichever.
 */
UENUM()
enum class ESurfContextLook : uint8
{
	/** Ghosted: there to fit against. */
	Ghost,

	/** Its wires alone, the surfaces out of view. */
	Wire,

	/** As it is, in the view in use. */
	Regular
};

/** How the preview checker is shaded. */
UENUM()
enum class ESurfPreviewShade : uint8
{
	/** By the view alone: a fixed light turning with the camera, no lighting cost. */
	Shaded,

	/** By the level's lighting. */
	Lit
};

/** What becomes of the faces beneath the lid when a block of faces is extruded. */
UENUM()
enum class ESurfExtrudeUnder : uint8
{
	/** The faces go; the block is hollow and the rest of the patch comes apart round the hole. */
	Drop,

	/** The faces become a patch of their own, turned to face out of the block: a closed box. */
	Close,

	/** The source patch is left as it was, its faces still under the lid. */
	Keep
};

/** Which side of the cut a slice keeps when nothing says otherwise. */
UENUM()
enum class ESurfSliceKeep : uint8
{
	/** Both halves, the cut face selected on each. */
	Both,

	/** The half in front of the cut only - the side the plane's normal points to. */
	Front,

	/** The half behind the cut only. */
	Back
};

/**
 * One saved arrangement of the buttons across the two surfaces (S82a): what
 * is on the viewport bar, what is in the SurfacingPro window, in order.
 */
USTRUCT()
struct SURFACINGPRO_API FSurfUiLayout
{
	GENERATED_BODY()

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The arrangement's name."))
	FString Name;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The verbs on the sidebar in this arrangement, in order."))
	TArray<FString> Bar;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The verbs in the Actions window in this arrangement."))
	TArray<FString> Window;
};

/** The ball the matcap reads its clay from (S188a). */
UENUM(BlueprintType)
enum class ESurfMatcapLook : uint8
{
	Clay UMETA(DisplayName = "Clay"),
	SoftGrey UMETA(DisplayName = "Soft grey"),
	Terracotta UMETA(DisplayName = "Terracotta"),
	Studio UMETA(DisplayName = "Studio"),
	RedWax UMETA(DisplayName = "Red wax"),

	Count UMETA(Hidden)
};

/** What a button shows (S195a): its word, its icon, or both. */
UENUM(BlueprintType)
enum class ESurfIconMode : uint8
{
	Words,
	Icons,
	IconAndWord UMETA(DisplayName = "Icon and word"),
};

UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "SurfacingPro"))
class SURFACINGPRO_API USurfSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USurfSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/**
	 * Saved per user and project, under Editor Preferences: these are the
	 * hand's preferences (keys, the gizmo, colours), not the project's. The
	 * editor writes a project's Game config only for the sections it lists,
	 * so as a defaultconfig class nothing set here ever reached the disk.
	 */
	virtual FName GetContainerName() const override { return FName(TEXT("Editor")); }
	virtual void PostInitProperties() override;

	/**
	 * Which revision of the defaults this saved config has taken. Every
	 * SaveConfig writes every property, so a changed default never reaches
	 * an existing config on its own; PostInitProperties moves the few that
	 * changed on purpose, once, and bumps this.
	 */
	UPROPERTY(config)
	int32 SettingsRevision = 0;

	/**
	 * Which keyboard layout the mode answers to. Blender mirrors the modelling
	 * keys a 3D artist already has in their fingers; nothing in the mode reads
	 * a key directly, so a third scheme is a table rather than a rewrite.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Which set of keys the mode answers to."))
	ESurfKeyScheme KeyScheme = ESurfKeyScheme::Blender;

	/**
	 * Write a record of each session - keys, verbs, selections, gestures and
	 * the moments that looked like friction - to Saved/SurfacingPro/Sessions.
	 * Local only, and the way a workflow problem gets reported as a file
	 * rather than as a paragraph.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Keep a local record of each session - keys, verbs, picks and misses - in Saved/SurfacingPro/Sessions, for reporting a problem."))
	bool bRecordSessions = true;

	/** Per-action rebinds. Any entry here beats the scheme's own chord. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Your own keys for any action; an entry here wins over the scheme's key."))
	TArray<FSurfKeyBinding> KeyOverrides;

	/** Centimetres. Every step above 1 is a multiple of 5, so any two steps share a lattice. */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "The grid steps, in centimetres, that the grid size walks through."))
	TArray<double> GridLadderCm;

	/** Index into GridLadderCm. Default 100 cm. */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Which grid step is used to start with (5 is 100 cm).", ClampMin = "0"))
	int32 DefaultGridIndex = 5;

	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Where moved control points land: free, on a fine step, or on the grid."))
	ESurfPointSnap PointSnap = ESurfPointSnap::Off;

	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "The fine step for control points, in centimetres.", ClampMin = "0.01"))
	double FinePointSnapCm = 1.0;

	/**
	 * In the Blender scheme a drag selects immediately with this tool - no key
	 * to arm it first. B, L and C switch which tool that is, the way Blender's
	 * toolbar does.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "What a bare drag in the viewport selects with: box, lasso or brush."))
	ESurfSelectTool SelectTool = ESurfSelectTool::Box;

	/**
	 * Navigation is deliberately not tied to the key scheme: the editing keys
	 * are worth taking from another package, the camera is not.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "How the camera is driven, apart from the editing keys."))
	ESurfCameraScheme CameraScheme = ESurfCameraScheme::Blender;

	/** Degrees of orbit per pixel of mouse movement. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Degrees the view turns per pixel of mouse movement.", ClampMin = "0.01", ClampMax = "2.0"))
	double OrbitSensitivity = 0.4;

	/** Fraction of the distance to the pivot one wheel step covers. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "How far one wheel step moves towards the pivot, as a share of the distance.", ClampMin = "0.01", ClampMax = "0.9"))
	double ZoomStep = 0.2;

	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Turn the view the other way left and right."))
	bool bInvertOrbitX = false;

	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Turn the view the other way up and down."))
	bool bInvertOrbitY = false;

	/**
	 * Orbit around the selection rather than the point the view is centred on.
	 * Blender ships this off, which surprises people who expect it on - and
	 * on is what was asked for here: the camera should turn around the thing
	 * being worked on.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "The view turns about what is picked rather than about the point it is centred on."))
	bool bOrbitAroundSelection = false;

	/**
	 * Orbit about whatever is under the cursor when the drag begins - the
	 * surface being looked at - rather than about a fixed point. Nothing
	 * under the cursor, and the selection or the view's pivot is used.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "The view turns about the surface under the cursor when the drag begins; nothing there, the pick or the view's pivot."))
	bool bOrbitUnderCursor = true;

	/** Degrees one numpad orbit key turns the view. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Degrees one numpad turn key turns the view.", ClampMin = "1.0", ClampMax = "90.0"))
	double NumpadOrbitDegrees = 15.0;

	/**
	 * How much smaller the transform gizmo is while this mode is active.
	 * Unreal's default is sized for moving whole actors; for dragging one
	 * control point it covers the thing being dragged. Restored on exit.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How much smaller the editor's gizmo is while the mode is up; put back on leaving.", ClampMin = "-10", ClampMax = "10"))
	int32 GizmoSizeAdjustment = -8;

	/**
	 * A box, a lasso or the brush reaches only the pieces already picked
	 * (S149a): "when working on the brush box and selecting its vertices im
	 * also selecting other object verts. should be able to select (overall!)
	 * for selected objects only."
	 *
	 * With nothing picked it reaches everything, which is how a piece is
	 * picked in the first place; with something picked, the gesture is about
	 * that thing's parts and reaching into its neighbours is never wanted.
	 * Whole pieces (4) always reaches everything, because there the gesture
	 * is how a second piece is added, and so does a gesture held with Shift
	 * or Ctrl - bare is stay where you are, Shift is reach further.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "A box, lasso or brush reaches only the pieces already picked; with nothing picked it reaches everything. Shift reaches further."))
	bool bSelectStaysOnPicked = true;

	/**
	 * A line moved fills the cells it stretches with lines (S150d): "add a
	 * tick option (by default off) when moving an edge, you can 'auto
	 * subdivide' + a %slider 0 none-less 0.5= same as average of this patch
	 * 1= much more dense".
	 *
	 * Off by default, because it changes the grid behind a plain move. On,
	 * when a whole row or column is moved the cells either side of it are
	 * cut until none is wider than the patch's mean cell times the density
	 * below; cells that were not stretched are left alone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Moving a whole row or column adds lines to the cells it stretches, so the grid stays even."))
	bool bAutoSubdivideOnMove = false;

	/** The density for it: 0 leaves a cell up to four times the mean, 0.5 holds it at the mean, 1 cuts it to a quarter. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How dense those added lines are: 0 loose, 0.5 the patch's average, 1 tight.", ClampMin = "0.0", ClampMax = "1.0"))
	double AutoSubdivideDensity = 0.5;

	/** Radius of the brush tool, in pixels. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "The select brush's radius, in pixels.", ClampMin = "4.0", ClampMax = "400.0"))
	double BrushSelectRadiusPx = 40.0;

	// --- Soft selection -----------------------------------------------------

	/**
	 * Collapse the editor's outliner shortly after a map opens.
	 *
	 * The outliner opens fully expanded every time, and a map with a few
	 * hundred pieces in it opens as a wall of names. There is no editor
	 * setting for this; the widget is found and collapsed instead.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Fold the editor's outliner shortly after a map opens."))
	bool bCollapseOutlinerOnMapOpen = false;

	/**
	 * Give the preview mesh collision. Off: the preview is what is being
	 * edited, and a mesh rebuilt on every drag is a poor thing to collide
	 * with. On, for walking a level in play-in-editor before it is compiled.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Give the editable surfaces collision while editing. Off keeps editing light."))
	bool bPreviewCollision = false;

	/**
	 * Give every patch collision for the length of a play session, so a
	 * level can be walked before it is compiled, and take it away again
	 * afterwards so editing stays light. Independent of the setting above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Give every patch collision during Play, so a level can be walked before it is baked."))
	bool bCollisionWhilePlaying = true;

	/**
	 * Play starts where the camera is, not at the player start. A level is
	 * tested from where it is being built; set when the mode is entered, put
	 * back when it is left.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Play starts where the camera is rather than at the player start, while the mode is up."))
	bool bPlayFromCamera = true;

	/** Remembered across sessions; Z changes it. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Which wires are drawn: the pieces being edited, every piece, or every piece with clicks reaching through (Z)."))
	ESurfWireView WireView = ESurfWireView::Selected;

	/** How strong the wires are, 1 full; what is picked keeps its full colour (S249a: "also expose wire strength?"). */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How strong the wires are, 1 full; what is picked keeps its full colour.", ClampMin = "0.05", ClampMax = "1.0"))
	float WireStrength = 1.0f;

	/** How the rest of the level looks round a prefab edited where it stands (S249a). */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How the rest of the level looks round a prefab open where it stands: ghosted, as wires, or as it is."))
	ESurfContextLook ContextLook = ESurfContextLook::Ghost;

	/** How big a new patch is, each way, in centimetres, rounded to the grid step. Ten metres: a room's floor, not a tile. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How big a new patch is, each way, in centimetres, on the grid.", ClampMin = "100.0", ClampMax = "100000.0"))
	double NewPatchSizeCm = 1000.0;

	/** How big a new box or cylinder is, each way, in centimetres. Two metres: a block to cut, on the grid. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How big a new primitive is, each way, in centimetres.", ClampMin = "10.0", ClampMax = "100000.0"))
	double NewBrushSizeCm = 200.0;

	/** What Primitive makes (S154a): 0 box, 1 cylinder, 2 patch, 3 cone, 4 tube, 5 bevel, 6 endcap. The last one chosen. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "What Primitive makes next: 0 box, 1 cylinder, 2 patch, 3 cone, 4 tube, 5 bevel, 6 endcap. The last one chosen.", ClampMin = "0", ClampMax = "6"))
	int32 NewPrimitiveKind = 0;

	/**
	 * Bake a slice at a time, between frames, instead of locking the editor
	 * until it is done (S106a): "bake can we have some background bake option
	 * on? perhaps see the progress(es) or issue(s) somewhere still?".
	 *
	 * The count baked and anything that would not bake are in the bottom-left
	 * readout while it runs; Bake becomes Stop.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bake", meta = (ToolTip = "Bake a slice at a time between frames instead of holding the editor until it is done; Bake becomes Stop meanwhile."))
	bool bBakeInBackground = true;

	/**
	 * How long a slice may take, in milliseconds. Longer is quicker overall
	 * and jerkier to watch; sixteen is about a frame.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How long a bake slice may take, in milliseconds. Longer is quicker overall, jerkier to watch.", ClampMin = "1.0", ClampMax = "200.0"))
	double BakeSliceMs = 16.0;

	/** How many sides a new cylinder has. Eight lies flat on the grid's axes. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How many sides a new cylinder has.", ClampMin = "3", ClampMax = "64"))
	int32 NewCylinderSides = 8;

	/**
	 * How many steps Steps makes between two edges (S105a): "a steps button?
	 * to create step (config) between selected edges?" - and "by cm and/or
	 * amount", so there are two ways to say it and they do not fight.
	 *
	 * The count is used unless a rise is set. Both are draggable in the
	 * adjust frame after the verb has run.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How many steps Steps makes, unless a riser height is set.", ClampMin = "1", ClampMax = "128"))
	int32 StepCount = 8;

	/**
	 * How tall one step is, in centimetres. Zero means the count decides;
	 * anything else and the count is the drop divided by this, which is how
	 * stairs are specified everywhere outside a modelling tool.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "One step's height, in centimetres; 0 lets the count decide.", ClampMin = "0.0", ClampMax = "1000.0"))
	double StepRiseCm = 0.0;

	/**
	 * How wide the viewport bar is, in pixels. The verbs are as wide as
	 * their words and wrap, so a wider bar is fewer, longer rows: 138 is
	 * one verb a row, 280 is two or three. The bar scrolls past the bottom
	 * of the viewport whatever it is set to.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How wide the sidebar is, in pixels.", ClampMin = "30", ClampMax = "900"))
	int32 ViewportBarWidthPx = 138;

	/**
	 * How long the bar's column of verbs may be, in pixels, before it
	 * scrolls; 0 is as long as the viewport allows (S162a): "should be able
	 * to edit the sidebar/shelf length with mouse". The grip at the foot of
	 * the column sets it; a double-click there puts it back to 0.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How long the sidebar may be before it scrolls, in pixels; 0 as long as the viewport allows.", ClampMin = "0", ClampMax = "4000"))
	int32 ViewportBarLengthPx = 0;

	/** How far down from the menu line the bar's column starts, in pixels (S165a): "in edit ui, user can move the sidebar anywhere he likes... he can move it down too". Dragged with the handle in Edit UI. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How far down the sidebar starts, in pixels.", ClampMin = "0", ClampMax = "4000"))
	int32 ViewportBarTopPx = 0;

	/** How big the bar's chips are, 1 as they ship (S166a): "in edit ui mode it'll be possible to ctrl scroll the sidebar to set scale to the sidebar buttons." */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How big the sidebar's buttons are, 1 as they ship.", ClampMin = "0.7", ClampMax = "2.0"))
	float ViewportBarScale = 1.0f;

	/**
	 * Enter edits a prefab where its instance stands, the rest of the level
	 * ghosted round it and out of reach. Off: the prefab alone at the
	 * origin, nothing else in view, the camera brought to it and back on
	 * Exit. Also on Enter's right-click. On from the start (S206a): the
	 * ghosted level round it is the context he works in.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Open edits a prefab where it stands, the level round it; off, alone at the origin."))
	bool bEditPrefabsInPlace = true;

	/** The wires of patches that are not being edited, when every patch shows them. */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "The wires of the pieces not being edited, when every piece shows them."))
	FLinearColor InactiveGridColour = FLinearColor(0.45f, 0.45f, 0.5f, 0.55f);

	/**
	 * The edges of a brush, in a colour no patch uses (S100a).
	 *
	 * "Brushes (box, cylinder, etc.) integration is really off. it should be
	 * visible its a different type." A brush had no wires of its own at all -
	 * it was a surface like any other until something of it was picked - so
	 * the one kind of thing in the level that behaves differently was the one
	 * kind you could not tell apart.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The edges of primitives."))
	FLinearColor BrushWireColour = FLinearColor(0.95f, 0.38f, 0.22f, 0.7f);

	/**
	 * Draw wires and points through surfaces. Off, a wire behind a surface
	 * is hidden by it, as the surface is - what can be seen is what can be
	 * picked. The x-ray wire view draws through whatever this says.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Draw wires and points through the surfaces; off, what is behind a surface is hidden with it."))
	bool bSeeThroughWires = false;

	/**
	 * Tint a control point towards a colour when another point, on any
	 * patch, sits at the same place - so a corner that has met its
	 * neighbour reads as met, and one that has not as not.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Mark control points that sit on another point, on any patch, so a corner that has met its neighbour reads as met."))
	bool bShowOverlaps = true;

	/** The dots on control points that meet a neighbour's, in 1 and 2 (Show Overlaps). */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "The mark on control points that meet another point."))
	FLinearColor OverlapColour = FLinearColor(0.15f, 0.95f, 0.25f, 1.0f);

	/**
	 * Frames per second the editor is allowed while the mode is up (0 leaves
	 * the editor's own cap). The editor smooths to about sixty by default;
	 * editing wants every frame the machine has. Put back on leaving.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Frames per second allowed while the mode is up; 0 keeps the editor's own cap.", ClampMin = "0", ClampMax = "1000"))
	int32 EditingFrameRateCap = 120;

	/**
	 * Turn the renderer down while the mode is up - shadows, global
	 * illumination, reflections, post-processing and effects at their
	 * lowest, the resolution and anti-aliasing left alone - and put it back
	 * on leaving. Modelling wants frames, not lighting; a laptop that shows
	 * thirty frames a second up close with the level's lighting on shows
	 * sixty or more with it down.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "Turn shadows, lighting and effects down while the mode is up, and back on leaving."))
	bool bLowerQualityWhileEditing = true;

	/**
	 * The screen percentage the viewport renders at while the mode is up,
	 * when the renderer is turned down: 100 leaves it alone. The viewport
	 * is nothing but the model while modelling, and a model reads as well
	 * at two thirds of the pixels for a third of the GPU.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "The viewport's resolution while the quality is down, in percent; 100 leaves it.", ClampMin = "25", ClampMax = "100"))
	int32 EditingScreenPercentage = 70;

	/**
	 * The viewport in the modelling view - unlit, the editor's Alt+3 -
	 * while the mode is up, put back on exit. Nothing of the level's
	 * lighting is paid for, and the checker is shaded by the view on its
	 * own already. Shift+Z switches to the shaded view and back.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (ToolTip = "The viewport unlit while the mode is up, put back on leaving. Shift+Z switches."))
	bool bModellingViewWhileEditing = true;

	/**
	 * The darkest and lightest grey of the preview checker, 0 to 1. The
	 * checker keeps its gradient - dark at the tile's origin, light at its
	 * far corner - between these two, so it reads without glaring or
	 * disappearing into shadow.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The darkest grey of the preview checker, 0 to 1.", ClampMin = "0.0", ClampMax = "1.0"))
	float CheckerDarkest = 0.30f;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The lightest grey of the preview checker, 0 to 1.", ClampMin = "0.0", ClampMax = "1.0"))
	float CheckerLightest = 0.62f;

	/**
	 * How the preview checker is shaded. Shaded: by the view alone, the
	 * way a modelling package's solid view is - no lights, no shadows, no
	 * global illumination, smooth and cheap; the shape reads from a fixed
	 * light that turns with the camera. Lit: by the level's lighting, as
	 * any material is. Groups with a material of their own keep it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How the preview checker is shaded: by the view alone, or by the level's lighting."))
	ESurfPreviewShade PreviewShade = ESurfPreviewShade::Shaded;

	/** How far short of the surface Space leaves the camera. */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "How far short of the surface Space leaves the camera, in centimetres.", ClampMin = "10.0", ClampMax = "5000.0"))
	double TeleportStandoffCm = 200.0;

	/**
	 * Keep the viewport's orbit point on whatever is selected.
	 *
	 * This is the substance of the difference from Blender, where the camera
	 * orbits what you are working on rather than turning where you are
	 * standing. Unreal's own Alt+LMB orbit already spins around its look-at
	 * point - it just only gets set when you press F. Setting it as the
	 * selection changes, without moving the camera, makes Alt+LMB orbit the
	 * thing being edited from then on.
	 *
	 * This does not change the gesture: orbit is still Alt+LMB, not the
	 * middle button. Changing that means owning the camera outright.
	 *
	 * Not to be confused with bOrbitAroundSelection above, which belongs to
	 * the house camera scheme and only applies while that scheme is driving.
	 * This one works with Unreal's own camera, which is what is driving.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "Keep the editor's orbit point on what is picked, without moving the camera."))
	bool bKeepOrbitPointOnSelection = false;

	/**
	 * A free turn behaves like a ball under the cursor rather than a twist
	 * about the view direction.
	 *
	 * Unreal's rotate gizmo is three rings, which means picking the axis
	 * before the turn. A trackball turns the way the hand moves: drag left
	 * and the far side comes towards you. Naming an axis with X, Y or Z still
	 * gives an exact turn about it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "A free turn rolls like a ball under the cursor; X, Y or Z still turn about one axis."))
	bool bTrackballRotate = true;

	/** The Extrude button places the new line one cell out; E places it on the edge and moves it. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "The Extrude button puts the new line one cell out; E puts it on the edge and moves it."))
	bool bExtrudeButtonOffsets = true;

	/**
	 * What becomes of the faces beneath the lid when a block of faces is
	 * extruded. Drop: they go, the block is hollow, and the rest of the patch
	 * comes apart into patches round the hole (a grid cannot have one).
	 * Close: they become a patch of their own, turned to face out of the
	 * block - a closed box. Keep: the source patch is left as it was, the
	 * faces still under the lid. Also a choice in the adjust frame after
	 * each extrude.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "What becomes of the faces under an extruded block: dropped, closed as a patch of their own, or kept."))
	ESurfExtrudeUnder ExtrudeUnder = ESurfExtrudeUnder::Drop;

	/** A bevel wider than the next line eats the lines it reaches (S159a): they go, and the round takes their place. Off: the width stops at the next line. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A bevel wider than the next line takes that line; off, the width stops there."))
	bool bBevelEatsLines = true;

	/** While a whole border is moved near another patch's border, Continue is offered as a ghost and taken on confirm (S159a). */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Moving a whole border near another patch's border offers Continue, taken on confirm."))
	bool bContinueOffers = true;

	/** Extrude on a whole line inside a patch: cut the patch there and grow the near half, instead of a wall of its own (S158d). Off: the wall. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Extrude on a line inside a patch cuts the patch there and grows the near half; off, a wall of its own."))
	bool bExtrudeInsideCuts = false;

	/**
	 * How far along its normal a duplicate is placed. Zero is exactly on top
	 * of the original, which is where the next drag starts from - and which
	 * z-fights until that drag happens.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "How far along its normal a copy is placed, in centimetres; 0 exactly on the original.", ClampMin = "0.0", ClampMax = "1000.0"))
	double DuplicateOffsetCm = 0.0;

	/**
	 * What a slice keeps when Enter, or the bar's button, commits it: both
	 * halves, the front only, or the back only. Ctrl+Enter keeps the back
	 * and Shift+Enter the front whatever this says. Also on the Slice
	 * button's right-click.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "What Cut keeps: both halves, the front, or the back."))
	ESurfSliceKeep SliceKeep = ESurfSliceKeep::Both;

	/**
	 * Draw the transform gizmo at all. Off, G R S are the whole interface,
	 * as in Blender, and nothing sits on top of the point being clicked.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Draw the transform gizmo; off, G R S alone."))
	bool bShowGizmo = true;

	/**
	 * Our own gizmo in place of the editor's: the same arrows, plane
	 * squares and rings, coloured by axis, all three in one - move, rotate
	 * and scale without switching - at half the editor's size, translucent
	 * when idle and full on the handle under the cursor. The editor's drag
	 * maths runs underneath it unchanged. Off: the editor's own widget.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Our gizmo - move, turn and size in one - instead of the editor's."))
	bool bOwnGizmo = true;

	/**
	 * The rotate ball (S130a): "Instead of rings for x y z rotation, overlay
	 * some sphere, in transparency, and we re rotating that sphere, 360 all
	 * axis possible! Its still possible to follow some 'main axis' (where
	 * the rings were) and snap with x y z."
	 *
	 * A translucent ball inside the three rings. Drag it anywhere and the
	 * selection turns about whatever axis the drag asks for; the rings are
	 * still there for a turn about one axis; X, Y or Z during the drag locks
	 * it to that axis, and the rotation grid applies again once it is locked.
	 *
	 * It is only grabbable while the gizmo is in rotate - reached by the
	 * rings, or by R - so a ball the size of the rings is not a dead disc
	 * over the middle of the selection for the rest of the time.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "A see-through ball inside the gizmo's rings: drag it to turn freely."))
	bool bGizmoBall = true;

	/**
	 * How solid the rotate ball is, 0 to 1 (S133a): "sphere more
	 * transparent."
	 *
	 * A number rather than another build: it sits over whatever he is
	 * turning, so how much of it he wants to see through is his to set and
	 * not mine to guess twice. The handle under the cursor is drawn at
	 * twice this, so the ball still answers when it is grabbed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How solid the turning ball is, 0 to 1.", ClampMin = "0.0", ClampMax = "0.6"))
	float GizmoBallOpacity = 0.01f;

	/** How large our gizmo is, as a share of the editor's: 0.5 is half. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How large our gizmo is next to the editor's: 0.5 is half.", ClampMin = "0.2", ClampMax = "2.0"))
	float OwnGizmoScale = 0.5f;

	/** How solid our gizmo is when nothing on it is under the cursor, 0 to 1. the author's setting (S57b). */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How solid our gizmo is when the cursor is not on it, 0 to 1.", ClampMin = "0.05", ClampMax = "1.0"))
	float OwnGizmoIdleOpacity = 0.2f;

	/**
	 * Draw a curve's tessellated surface as thin lines under its cage, so the
	 * shape the control net implies is visible while the net is edited.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Draw a curve's real surface as thin lines under its cage."))
	bool bShowSurfaceWireframe = true;

	/**
	 * The subdivided surface's own colour, apart from the white cage: "i'd
	 * see the subdivided wire in another color (pink?)" (S73b). The cage
	 * steers the shape; this is the shape.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "The colour of a curve's surface lines under its cage."))
	FLinearColor SurfaceWireframeColour = FLinearColor(1.0f, 0.33f, 0.68f, 0.75f);

	/**
	 * A weld leaves the UVs where they were.
	 *
	 * Natural mapping re-measures the surface on every edit, which after a
	 * weld would slide the texture across the pinch. Keeping the UVs means
	 * freezing the mapping - the patch becomes Manual - and that is what is
	 * wanted: the texture should pinch with the surface.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Weld leaves the texture where it was, so it pinches with the surface."))
	bool bWeldKeepsUVs = true;

	/**
	 * M welds each cluster of selected points on its own - the points within
	 * the weld distance of each other go to their own centre - instead of
	 * every selected point to one centre. Six corners picked in three pairs
	 * become three welds, not one.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "M welds each cluster of picked points on its own instead of all to one centre."))
	bool bWeldEachCluster = false;

	/**
	 * Snap a dragged selection onto control points that already exist.
	 *
	 * The grid snap below keeps points on round numbers, which is not the
	 * same problem: two patches meet cleanly when their edges are on each
	 * other, wherever that happens to be. The whole selection moves by one
	 * correction rather than each point finding its own target, so the shape
	 * being dragged arrives intact.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land on existing control points within reach."))
	bool bSnapToPoints = true;

	/**
	 * The snap switch (S198a): off, nothing of ours snaps whatever targets
	 * are ticked. Shift+Tab; Ctrl held during a move flips it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Snapping on or off (Shift+Tab); Ctrl held during a move flips it."))
	bool bSnapEnabled = true;

	/** Snap onto the nearest point of an edge of ours - a patch's cage line, a primitive's edge - when no point is in reach (S200a). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land on the nearest point of an edge when no point is in reach."))
	bool bSnapToEdges = false;

	/** Snap onto the middle of an edge of ours (S200a). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land on the middle of an edge."))
	bool bSnapToEdgeCentres = false;

	/** Snap onto the foot of the perpendicular from where the point started to an edge of ours - walls meeting square (S200a). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land square to an edge, from where the point started."))
	bool bSnapToEdgePerpendicular = false;

	/** Which point of what moves is the one that lands (S205a). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Which point of what moves is the one that lands."))
	ESurfSnapWith SnapWith = ESurfSnapWith::Closest;

	/** How near a control point, or a mesh, has to be, in centimetres, to be snapped onto. */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "How near a point or a mesh has to be to be snapped onto, in centimetres.", ClampMin = "0.1", ClampMax = "500.0"))
	double PointSnapRadiusCm = 10.0;

	/** Snap onto the level's static meshes too, within the same reach: their corners first, else the nearest point on an edge or a face. */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land on the level's static meshes too: their corners, edges and faces."))
	bool bSnapToMeshes = true;

	/** During a move, with no point under the cursor, the moving point nearest the cursor lands on the surface under it - another piece's, or the level's (S157e). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "With no point under the cursor, a move lands on the surface under it."))
	bool bSnapToSurfaces = true;

	/** A placed mesh, prefab or level landing on a surface under G turns to stand on it - its up along the surface's normal (S235a). */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "A placed mesh, prefab or level landing on a surface turns to stand on it."))
	bool bSnapTurnsToFace = false;

	/**
	 * Snap onto points of the patch being edited, not only other patches.
	 *
	 * Off, because on it was how points collapsed onto their neighbours
	 * without anyone asking: drag a point a little too close to the next one
	 * and it landed exactly on it, and a grid with two points in one place
	 * has lost a cell. Snapping exists to make patches meet each other.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "Moves land on points of the patch being edited too, not only other patches."))
	bool bSnapWithinPatch = false;

	/**
	 * Align with far points (S171b): when no point is within reach, a
	 * moving point lands on the line along a world axis through any other
	 * point that passes within reach - the same X and Y as a corner across
	 * the room, say - and a dotted guide shows which. "suggesting a snap
	 * with some other vert in a same line / axis, even if further away."
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "A moving point lines up with far points along the world axes, with a dotted guide."))
	bool bSnapAlign = true;

	/**
	 * Points outside the selection follow it, by how near they are. Off by
	 * default because a hard selection is what a precise edit needs; on, it
	 * is how a surface gets shaped rather than poked.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "Points outside the selection follow it by how near they are (O)."))
	bool bSoftSelection = false;

	/**
	 * Centimetres. Nothing beyond this follows at all. Held between the
	 * smallest useful reach and 50 m (S155d): the wheel during a transform
	 * grows it a quarter a notch, and a wheel spun while scaling had taken
	 * it to a kilometre - "100,000" on the top line, and every point in the
	 * level following every move.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "How far the soft selection reaches, in centimetres.", ClampMin = "10.0", ClampMax = "5000.0"))
	double SoftSelectionRadiusCm = 200.0;

	static constexpr double SoftReachMinCm = 10.0;
	static constexpr double SoftReachMaxCm = 5000.0;
	static constexpr double SnapReachMinCm = 0.5;
	static constexpr double SnapReachMaxCm = 500.0;

	/**
	 * A reach as one digit, 0 at the smallest to 9 at the largest, evenly
	 * on a log scale (S156a): "number ain't that important - a simple 0-9
	 * remap". The centimetres stay underneath; the digit is what is read.
	 */
	static int32 ReachDigit(double Cm, double MinCm, double MaxCm)
	{
		const double T = FMath::Loge(FMath::Clamp(Cm, MinCm, MaxCm) / MinCm) / FMath::Loge(MaxCm / MinCm);
		return FMath::Clamp(FMath::RoundToInt(T * 9.0), 0, 9);
	}

	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "How the soft selection fades with distance."))
	ESurfFalloff SoftSelectionFalloff = ESurfFalloff::Smooth;

	/**
	 * Measure the reach along the grid rather than through the air (S165a):
	 * "add a mode to soft select from selection and extending it rather
	 * than actual distance". Along the grid, a point on the far side of a
	 * fold is as far as the walk to it; through the air, it is next door.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "Measure the soft reach along the grid rather than through the air."))
	bool bSoftAlongTheGrid = false;

	/** Flatten levels along these world axes (S167b), any of them; none ticked: onto the selection's own plane. "should be tick params not buttons, so they may be complementary or unique." */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Flatten along the world's X; none ticked, onto the selection's own plane."))
	bool bFlattenX = false;
	/** Flatten along the world's Y. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Flatten along the world's Y."))
	bool bFlattenY = false;
	/** Flatten along the world's Z. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Flatten along the world's Z."))
	bool bFlattenZ = false;

	/** Shading (S171a): what the verb sets on the picked patches and primitives - 0 smooth, 1 hard, 2 by the angle below. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "What Normals sets on the picked pieces: 0 smooth, 1 hard, 2 by angle.", ClampMin = "0", ClampMax = "2"))
	int32 ShadingKind = 2;

	/** Shading by angle: sides or cells that turn by less than this share a normal. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Normals by angle: sides that turn by less than this read as one smooth surface.", ClampMin = "1", ClampMax = "179"))
	double ShadingAngleDeg = 60.0;

	/** A new cylinder's crease angle: its sides read round under it, its caps stay hard. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A new cylinder's smoothing angle: its sides read round, its caps hard.", ClampMin = "0", ClampMax = "180"))
	double NewCylinderCreaseDeg = 60.0;

	/**
	 * Seconds the radius circle stays up after the last change. Blender only
	 * draws it while you are setting it; a ring that never leaves becomes
	 * scenery you stop reading.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "Seconds the soft-selection circle stays after its last change.", ClampMin = "0.0", ClampMax = "10.0"))
	double SoftRadiusHintSeconds = 1.5;

	/** 0 at the radius, 1 at the selection. The curve named by the falloff; Seed tells Random which point it is. */
	static double Falloff(ESurfFalloff Shape, double NormalisedDistance, int32 Seed = 0);

	/** The name of a falloff, for the bar and the session record. */
	static FString FalloffName(ESurfFalloff Shape);

	/**
	 * Whether this is the falloff a drag is expected to have (S137a).
	 *
	 * "BUG: turning down the soft selection produce extremely corrupted
	 * noise" and "when smooth scrolls down to 0 it leaves a super messy
	 * noisy mesh behind" - two notes, four sessions apart, both describing
	 * a mesh that has gone wrong. **Random is not a bug; it is a falloff
	 * that makes a rough surface on purpose**, one key away in the cycle,
	 * and nothing on screen said it was on. A state you can enter with one
	 * press and cannot see is indistinguishable from corruption.
	 *
	 * Only smooth is usual. Everything else makes the number on the soft
	 * chip amber, the way the density readout goes amber when the texels do
	 * not match: something is set to other than the ordinary, and the
	 * right-click says what.
	 */
	static bool FalloffIsUsual(ESurfFalloff Shape) { return Shape == ESurfFalloff::Smooth; }

	// --- Display ------------------------------------------------------------
	// Defaults match what a modelling package looks like: a thin dark cage and
	// small points, so the surface is what you see and the cage is what you
	// grab. Thick orange lines read as "selected object", not as geometry.

	/**
	 * Width of the cage lines in pixels. At 1 and below the line is drawn as
	 * a hairline - one crisp pixel, the way a modelling package draws a
	 * wire - rather than as a smoothed strip. Selected lines are the same
	 * width in a different colour, so the cage never changes weight.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The cage lines' width in pixels; 1 and below is a hairline.", ClampMin = "0.1", ClampMax = "8.0"))
	float ControlGridLineWidth = 1.0f;

	/**
	 * How far towards the camera the cage is drawn, as a fraction of its
	 * distance.
	 *
	 * On a sheet the control points ARE the surface vertices, so the cage
	 * lies exactly in the surface and the two fight for the same depth - the
	 * shimmer along every line as the view moves. Proportional to distance,
	 * so it is the same size on screen wherever the camera is and never
	 * becomes a visible gap. Raise it if lines still shimmer.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How far towards the camera the cage is drawn, as a share of its distance; raise it if lines shimmer.", ClampMin = "0.0", ClampMax = "0.02"))
	float CageLift = 0.004f;

	/**
	 * How close the cursor has to be to an edge, in pixels, for a click to
	 * take it. Measured against the line as drawn, so it means the same thing
	 * whatever the angle or distance.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How close a click has to land to a line to take it, in pixels.", ClampMin = "2.0", ClampMax = "80.0"))
	float EdgePickRadiusPx = 40.0f;

	/** How close to a control point, in pixels, a click has to land to take it. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How close a click has to land to a control point to take it, in pixels.", ClampMin = "2.0", ClampMax = "60.0"))
	float PointPickRadiusPx = 16.0f;

	/** A little smaller since S208a ("bit too big atm"): 4 and 6.5, from 5 and 8. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Control points' size, in pixels.", ClampMin = "1.0", ClampMax = "24.0"))
	float ControlPointSize = 4.0f;

	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Picked control points' size, in pixels.", ClampMin = "1.0", ClampMax = "24.0"))
	float SelectedControlPointSize = 6.5f;

	/**
	 * The X search finds these settings too, last in the list, and sets them
	 * there (S208a): "any parameter of surfacing pro may be tweaked here".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The X search finds these settings too, last in the list, and sets them there."))
	bool bSearchFindsSettings = true;

	/**
	 * The cage lines, near-white for the same reason the points are: a dark
	 * line on a surface that is usually dark, seen against a level that is
	 * usually dark, is a line you cannot find.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "The cage lines of what is being edited."))
	FLinearColor ControlGridColour = FLinearColor(0.88f, 0.88f, 0.92f, 1.0f);

	/**
	 * Near-white, not near-black. A dark point on a dark surface is the same
	 * as no point at all, which is how the cage came to look empty.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "Control points."))
	FLinearColor ControlPointColour = FLinearColor(0.92f, 0.92f, 0.95f, 1.0f);

	/** The picked control points. */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "Picked control points."))
	FLinearColor SelectedControlPointColour = FLinearColor(1.0f, 0.08f, 0.05f, 1.0f);

	/** Points inside the soft-selection falloff, tinted by how much they follow. */
	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "Points inside the soft selection, tinted by how much they follow."))
	FLinearColor SoftSelectionColour = FLinearColor(1.0f, 0.55f, 0.1f, 1.0f);

	/** How strongly a picked face is washed over. Nothing near 1: it has to stay readable. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How strongly a picked face is washed over.", ClampMin = "0.05", ClampMax = "0.8"))
	float FaceSelectionOpacity = 0.25f;

	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "Picked faces."))
	FLinearColor FaceSelectionColour = FLinearColor(1.0f, 0.08f, 0.05f, 1.0f);

	UPROPERTY(config, EditAnywhere, Category = "Colours", meta = (ToolTip = "The active face - the one the surfacing reads."))
	FLinearColor ActiveFaceColour = FLinearColor(1.0f, 0.85f, 0.1f, 1.0f);

	/**
	 * Separation between stacked layers, applied along the normal by the
	 * compiler and the preview - never by world position offset in a
	 * material, which costs on Nanite and still z-fights at grazing angles (D14).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "How far apart stacked layers stand, along the surface's normal, in centimetres.", ClampMin = "0.01"))
	double LayerOffsetCm = 0.5;

	/** Stack depth past which the inspector warns. */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "How many stacked layers before a warning.", ClampMin = "1"))
	int32 StackDepthWarn = 3;

	/**
	 * Where the Materials window looks: a content path of the project, the
	 * library the window lists, sub-folders included. Project content, never
	 * plugin content (D55).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (ToolTip = "The content folder the Material Library lists, with its sub-folders.", DisplayName = "Library Root"))
	FString MaterialLibraryRoot = TEXT("/Game/SurfacingPro/");

	/**
	 * Where conformed texture sources live on disk: <this>/Textures/T_Sp_<Stem>.tga,
	 * with the record beside them. Empty: <Project>/SurfacingPro - inside the
	 * project's tree, so the whole team reaches every file.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (ToolTip = "Where texture sources are kept on disk; empty, the project's SurfacingPro folder.", DisplayName = "Texture Source Root"))
	FString TextureSourceRoot;

	/**
	 * The longest edge a texture keeps on import; 0 keeps every pixel. A
	 * larger source is scaled down on the way in, and the record says so.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (ToolTip = "The longest edge a texture keeps on import, in pixels; 0 keeps every pixel.", DisplayName = "Texture Import Max Size", ClampMin = "0"))
	int32 TextureImportMaxSize = 0;

	/**
	 * The parent New MI makes an instance of when nothing else is chosen: a
	 * master's name under the library root. Not found there: the first master.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (ToolTip = "The master a new material instance is made from when none is chosen.", DisplayName = "Default Parent"))
	FString DefaultParent = TEXT("M_Opq_Simple");

	/**
	 * The mesh the Material Manager shows a material on: two material slots,
	 * 0 for opaque geometry and 1 for alpha geometry. Empty: the plugin's
	 * calibration cube.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (ToolTip = "The mesh the Material Manager shows materials on; empty, the plugin's cube.", DisplayName = "Preview Mesh", AllowedClasses = "/Script/Engine.StaticMesh"))
	FSoftObjectPath PreviewMesh;

	/**
	 * The mesh for a material with displacement ("Dsp" in its name or its
	 * master's): a denser cube, so the displacement has something to move.
	 * Empty: the plugin's mat_cube_dsp.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (DisplayName = "Preview Mesh (Displacement)", AllowedClasses = "/Script/Engine.StaticMesh"))
	FSoftObjectPath PreviewMeshDisplacement;

	/**
	 * How much a pass of the paint brush sets at its centre, falling off to
	 * nothing at its ring. One is a full stroke; a half builds a blend up in
	 * passes. The brush's size is the select brush's, on the wheel.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls", meta = (ToolTip = "How much one pass of the paint brush sets at its centre, 0 to 1.", ClampMin = "0.05", ClampMax = "4.0"))
	double PaintStrength = 1.0;

	/**
	 * Where each verb lives, in order: the ids on the viewport bar and the
	 * ids in the SurfacingPro window (S74a). A verb in neither list has
	 * never been moved and takes its default place, which is why no verb can
	 * be lost: it is on one surface or the other, always.
	 */
	/**
	 * One Edit mode (S83a): the click decides whether it is picking a
	 * control point, an edge or a face, and the tool follows. 1, 2 and 3
	 * lock it to one kind - pressing the one you are locked to lets go
	 * again - and 4 is Object, which is a different question entirely.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Selection", meta = (ToolTip = "A click in Edit picks a point, line or face, whichever it lands on; 1 2 3 lock it to one kind."))
	bool bClickDecidesKind = true;

	UPROPERTY(config)
	TArray<FString> BarVerbs;

	UPROPERTY(config)
	TArray<FString> WindowVerbs;

	/** Verbs that ship on the bar and were taken off it (S166a), so they do not come back by default. */
	UPROPERTY(config)
	TArray<FString> HiddenVerbs;

	/** How many of the verb renames (S184c) have been carried into the saved lists. */
	UPROPERTY(config)
	int32 VerbRenamesDone = 0;

	/** UVs re-measured when points move (S185b): on, the texture follows the new shape; off, it stays where it was. */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "Moving points re-measures the UVs, so the texture follows the shape; off, the UVs stay."))
	bool bUVsFollowEdits = true;

	/**
	 * The UVs window draws the active patch's texture behind the nets
	 * (S207a): "in uv mode need option display texture as background in uv
	 * panel. and option to have it tile".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "The UVs window shows the texture behind the nets."))
	bool bUVShowTexture = true;

	/** ... on every tile in view, not only the first. */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "... on every tile in view, not only the first."))
	bool bUVTileTexture = false;

	/** The UVs window colours each cell by its stretch (S233a): blue tight, green even, red stretched, against the net's own middle. */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "The UVs window colours each cell by its stretch: blue tight, green even, red stretched."))
	bool bUVStretchView = false;

	/** Extrude on edges makes a new patch joined at the edge instead of growing this one (S185b). */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "Extrude on lines makes a new patch joined at the line instead of growing this one."))
	bool bExtrudeNewPatch = false;

	/**
	 * A grown border's new line carries the UVs on by its real length
	 * (S207b): "add a UV param to parampopup when extruding. so it may
	 * preserve good uv continuity" - a frozen patch's new row kept one cell's
	 * worth of texture however far it was pulled, and stretched.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A grown border's new line carries the UVs on by its real length."))
	bool bExtrudeCarriesUVs = true;

	/** The verbs starred for the Q menu (S156b), by id, in the order starred. */
	UPROPERTY(config)
	TArray<FString> QuickFavorites;

	/** Random (S157a): how far at most, which axes, or along each point's normal. */
	UPROPERTY(config)
	double RandomOffsetCm = 10.0;

	UPROPERTY(config)
	bool bRandomX = true;

	UPROPERTY(config)
	bool bRandomY = true;

	UPROPERTY(config)
	bool bRandomZ = true;

	UPROPERTY(config)
	bool bRandomAlongNormal = false;

	/** Loop adds a column rather than a row; Push goes in rather than out (S156c: one verb, a direction). */
	UPROPERTY(config)
	bool bLoopIsColumn = false;

	UPROPERTY(config)
	bool bPushIn = false;

	/**
	 * Arrangements kept by name (S82a): "save/reset/load configs available
	 * from setting". One is the arrangement for building, another for
	 * texturing, and neither has to be rebuilt button by button.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "Interface arrangements kept by name, to switch between."))
	TArray<FSurfUiLayout> UiLayouts;

	/**
	 * Which columns the Prefabs window shows (S86b). "it will be minimized
	 * very small. must support it. here the columns need be togglable" - a
	 * window docked into a corner has room for the name and little else,
	 * and a column that cannot be turned off is a window that cannot be
	 * made small.
	 */
	/**
	 * Which edge of the viewport the bar sits against (S89a). Left keeps the
	 * hand where it learned the tool; right is where Blender puts its N
	 * panel and it is away from Unreal's own viewport toolbar.
	 */
	/**
	 * Whether what a verb just did is shown at all (S93a). "have the
	 * tips/note show in some other collapsable/closable popup in the
	 * viewport" - it has a panel of its own now, with a cross on it; this is
	 * the switch for the hand that wants it gone for good.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The foot line: what a verb just did and the keys that mean something now."))
	bool bShowTips = true;

	/** The bar against the viewport's right edge, clear of the editor's own toolbar; off, the left. */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The sidebar against the viewport's right edge; off, the left."))
	bool bBarOnRight = false;

	/** The line of family menus under the top line (S152b): Create, Geometry, Shape, Surface, UV, Select, Windows, Interface. */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The line of family menus under the top line."))
	bool bShowMenuLine = true;

	/** The sidebar's buttons: words, icons, or both (S195a). */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The sidebar's buttons: words, icons, or both."))
	ESurfIconMode SidebarIconMode = ESurfIconMode::Words;

	/** The Actions window's buttons: words, icons, or both. */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The Actions window's buttons: words, icons, or both."))
	ESurfIconMode ActionsIconMode = ESurfIconMode::IconAndWord;

	/** The menus' rows: words, icons, or both. */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "The menus' rows: words, icons, or both."))
	ESurfIconMode MenuIconMode = ESurfIconMode::IconAndWord;

	/**
	 * Which of the sidebar's family sections are open, one bit per family
	 * (S193a): "all collapsed, remember what was open". Set by clicking a
	 * section's header.
	 */
	UPROPERTY(config)
	int32 SidebarOpenFamilies = 0;

	/** The Actions window's folded families (S203a), a bit per ESurfVerbGroup: "make it possible to collapse/extend". */
	UPROPERTY(config)
	int32 ActionsFoldedFamilies = 0;

	/** Whether the sidebar's shelf (its family tabs) is unfolded (S193c): the small arrow under the pinned buttons. */
	UPROPERTY(config)
	bool bSidebarShelfOpen = true;

	/**
	 * Which columns of the Prefabs table are hidden (S102a). The header owns
	 * the answer now - it is Unreal's own table header, and its right-click
	 * is where a column is turned off - so this is only where that answer is
	 * kept between sessions.
	 */
	UPROPERTY(config)
	TArray<FName> PrefabHiddenColumns;

	/**
	 * The Prefabs window as tiles rather than rows (S114a): "prefab view
	 * should offer a view similar to content browser view."
	 *
	 * The table answers what a prefab is made of; the tiles answer what it
	 * looks like, which is the question you have while you are placing one.
	 * Both, and which was last used is remembered.
	 */
	/**
	 * An asset dropped into the viewport lands on the grid (S122a): "drag and
	 * dropping an asset must snap under cursor . (trying a prefab didnt
	 * snap)".
	 *
	 * The editor puts a dropped actor wherever the cursor's ray hit, to the
	 * centimetre. In a blockout that is never where you meant - you meant the
	 * corner you were pointing at.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapping", meta = (ToolTip = "An asset dropped into the viewport lands on the grid under the cursor."))
	bool bSnapDroppedActors = true;

	UPROPERTY(config)
	bool bPrefabTiles = false;

	/** How big a prefab tile is, in pixels. */
	UPROPERTY(config, meta = (ClampMin = "48", ClampMax = "256"))
	int32 PrefabTileSizePx = 96;

	/**
	 * A prefab placed in the level draws its origin as a small cross and its
	 * extent as a faint box (S81a): it says where the thing is anchored, and
	 * the faintness says it is not the thing being edited - open it (Alt+PgDn)
	 * to work on it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Placed prefabs show their origin as a small cross."))
	bool bShowPrefabMarks = true;

	/**
	 * A placed prefab's extent as a faint box (S199a): apart from its origin
	 * now - "dissociate boundbox and origin ... in settings we get to toggle
	 * bbox". The origin cross is Prefab Marks.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Placed prefabs show their extent as a faint box."))
	bool bShowPrefabBounds = true;

	/** Every group's corners, faint (S251a): "expose the bbox display for groups too? off by default?" */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Every group's corners, faint; a picked group shows its own whatever this says."))
	bool bShowGroupBounds = false;

	/**
	 * The right button opens the verb menu over the viewport (S79).
	 * Shift+right-click is the editor's own menu either way; off, the right
	 * button is the editor's as it always was.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Interface", meta = (ToolTip = "Right-click opens the SurfacingPro menu; Shift+right-click is the editor's."))
	bool bRightClickMenu = true;

	/**
	 * Names in the world: every group and prefab instance says what it is,
	 * over its middle - grey when it is baked or empty. "We may have a
	 * button to toggle a 3d name label."
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "What things are called, on a plate over their middle in the viewport."))
	bool bShowNames = false;

	/**
	 * A small line of numbers in the viewport's corner (S207a): "a Statistic
	 * display toggle? shows all / selection - tris/points/how many materials
	 * in use ... small discreet". What is picked, else the whole level.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "A small line of numbers in the viewport's corner: what is picked, else the whole level."))
	bool bShowStats = false;

	/**
	 * A colour a patch, in the cage: every patch draws its wires in a colour
	 * of its own, from its id, so a blockout reads as pieces rather than as
	 * one net. Off is the one cage colour under Colours.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Every patch's cage in a colour of its own."))
	bool bColourIdWires = false;

	/** The one channel, kept in step with PaintChannels for what reads it alone; Colour when R, G and B are all ticked. */
	UPROPERTY(config)
	ESurfPaintChannel PaintChannel = ESurfPaintChannel::R;

	/** What the Colour channel paints towards: the three colour channels at once. */
	UPROPERTY(config)
	FLinearColor PaintColour = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	/** The second colour (S212a): Ctrl-drag paints it on the Colour channel; X while the brush is out swaps the two. */
	UPROPERTY(config)
	FLinearColor PaintColourBack = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);

	/** From Cavity: how many rings of neighbours are averaged in, 0 none to 1 two rings. */
	UPROPERTY(config)
	float CavitySmooth = 0.3f;

	/** From Cavity: the gain - 1 as measured, 4 a shallow crevice already full. */
	UPROPERTY(config)
	float CavityLevel = 4.0f;

	/** From Cavity: ridges instead of dents. */
	UPROPERTY(config)
	bool bCavityInvert = false;

	/** How From Cavity meets the channel already there: replace, add, subtract, multiply, lighten, darken. */
	UPROPERTY(config)
	ESurfMaskBlend MaskBlend = ESurfMaskBlend::Replace;

	/**
	 * The adjustments (S238a: "remove any Mask option ... set from cavity
	 * like blender's mask does with similar slider (live view) ... a
	 * smooth button too. a contrast button. level button"). Each is a verb
	 * with a frame; its knobs are here.
	 */
	/** From Cavity: how much of the cavity goes in, 0 none, 1 all. */
	UPROPERTY(config)
	float PaintCavityFactor = 1.0f;

	/** Smooth: how far towards the neighbours' mean each pass goes. */
	UPROPERTY(config)
	float PaintSmoothAmount = 1.0f;

	/** Smooth: how many passes. */
	UPROPERTY(config)
	int32 PaintSmoothTimes = 1;

	/** Contrast: 0 as it is, 1 twice as far from the middle, -1 flat. */
	UPROPERTY(config)
	float PaintContrast = 0.5f;

	/** Levels: the input black point. */
	UPROPERTY(config)
	float PaintLevelsInLow = 0.0f;
	/** Levels: the input white point. */
	UPROPERTY(config)
	float PaintLevelsInHigh = 1.0f;
	/** Levels: the gamma - 1 straight, above 1 lifts the middle, below drops it. */
	UPROPERTY(config)
	float PaintLevelsGamma = 1.0f;
	/** Levels: the output black point. */
	UPROPERTY(config)
	float PaintLevelsOutLow = 0.0f;
	/** Levels: the output white point. */
	UPROPERTY(config)
	float PaintLevelsOutHigh = 1.0f;

	/** The adjustments, by number: 0 from cavity, 1 smooth, 2 contrast, 3 levels, 4 invert. */
	static constexpr int32 PaintAdjustCavity = 0;
	static constexpr int32 PaintAdjustSmooth = 1;
	static constexpr int32 PaintAdjustContrast = 2;
	static constexpr int32 PaintAdjustLevels = 3;
	static constexpr int32 PaintAdjustInvert = 4;

	/** A value through one adjustment (S238a); Mean is the neighbours' mean for Smooth, Cavity the cavity for From Cavity. */
	static float AdjustPaintValue(const USurfSettings& S, int32 Kind, float Was, float Mean, float Cavity);

	/** The Paint view (S229a: "perhaps its a view mode too"): the channel on every surface, brush or not; a view kind beside Lit, Unlit, Matcap. */
	UPROPERTY(config)
	bool bChannelView = false;

	/** The ticked channels (S229a, S248a): bits 1 R, 2 G, 4 B, 8 A; 0 means PaintChannel alone (an older config). */
	UPROPERTY(config)
	int32 PaintChannels = 0;

	/** The ticked channels as a set (S248a). */
	FSurfPaintSet PaintSet() const;
	/** Tick exactly these (1 R, 2 G, 4 B, 8 A); none ticked keeps what was. Not saved - the caller saves. */
	void SetPaintSet(int32 Bits);

	/**
	 * Show the channel being painted on every surface while the brush is
	 * out - dark where empty, the channel's colour where full (S69a). Off
	 * by default; the toggle is on the brush's right-click.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "While the paint brush is out, the Paint view shows the painted channels."))
	bool bPaintViewOn = false;

	/**
	 * The viewport's floor and backdrop (S172b): "some kind of grid, subtle,
	 * with a parameter to toggle, the axes slightly different so we know
	 * where 0 0 0 stands"; "a slight gradient by default like in modelling
	 * software"; "perhaps even a matcap mode".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The floor grid at zero."))
	bool bShowGrid = true;

	/** How strong the grid's lines are, 0 to 1; the major lines and the axes are drawn stronger. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "How strong the grid's lines are, 0 to 1.", ClampMin = "0.02", ClampMax = "1.0"))
	float GridOpacity = 0.22f;

	/** The X (red) and Y (green) lines through the origin, and a short Z (blue). */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The X (red) and Y (green) lines through the origin, and a short Z (blue)."))
	bool bShowGridAxes = true;

	/** A gradient behind everything in the modelling (unlit) view, instead of black. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "A gradient behind everything in the unlit views, instead of black."))
	bool bBackdropGradient = true;

	/** The backdrop gradient's colour at the top of the view. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The backdrop's colour at the top."))
	FLinearColor BackdropTop = FLinearColor(0.085f, 0.088f, 0.095f, 1.0f);

	/** The backdrop gradient's colour at the foot of the view. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The backdrop's colour at the bottom."))
	FLinearColor BackdropBottom = FLinearColor(0.018f, 0.018f, 0.021f, 1.0f);

	/** Clay on every patch and primitive - the shape alone, lit from the camera - instead of its materials. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Clay on every patch and primitive - the shape alone - instead of their materials."))
	bool bMatcap = false;

	/** The tint the Matcap view kind shades with. */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "The clay's colour."))
	FLinearColor MatcapColour = FLinearColor(0.60f, 0.60f, 0.60f, 1.0f);

	/** Which ball the clay is read from: a grey clay, a soft grey, terracotta, a contrasted studio ball, red wax (S188a). */
	UPROPERTY(config, EditAnywhere, Category = "Display", meta = (ToolTip = "Which ball the clay is read from."))
	ESurfMatcapLook MatcapLook = ESurfMatcapLook::Clay;

	/** A new lattice's points each way (S189a): the last ones used. */
	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A new lattice's points along X.", ClampMin = "2", ClampMax = "8"))
	int32 LatticeResX = 3;

	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A new lattice's points along Y.", ClampMin = "2", ClampMax = "8"))
	int32 LatticeResY = 3;

	UPROPERTY(config, EditAnywhere, Category = "Modelling", meta = (ToolTip = "A new lattice's points along Z.", ClampMin = "2", ClampMax = "8"))
	int32 LatticeResZ = 2;

	/** The UV window's quickpanel (S214a, tabbed S216a): open (N over the window), and which tabs are shown (bits: 1 Select, 2 Transform, 4 Islands, 8 Mapping, 16 View; 0 folded to its tabs). */
	UPROPERTY(config)
	bool bUVPanelOpen = true;
	UPROPERTY(config)
	int32 UVPanelShown = 8;
	/** Its tabs' order and its width (S219a), as the SidePanel's. */
	UPROPERTY(config)
	FString UVPanelOrder = TEXT("0,1,2,3,4");
	UPROPERTY(config)
	int32 UVPanelWidthPx = 250;

	/** Pack (S215a): the gap between islands and round the tile, in tiles. */
	UPROPERTY(config, EditAnywhere, Category = "Surfacing", meta = (ToolTip = "Pack's gap between islands and round the tile, in tiles.", ClampMin = "0", ClampMax = "0.2"))
	float UVPackMargin = 0.02f;

	/** The Assets tab (S210a): tile size, which kinds (1 prefabs, 2 meshes), the folder. */
	UPROPERTY(config)
	int32 AssetsZoom = 2;
	UPROPERTY(config)
	int32 AssetsKinds = 3;
	UPROPERTY(config)
	FString AssetsFolder;

	/** The SidePanel (S177a): open or closed (N), which tabs show - a bit each, 1 Item, 2 Paint, 4 Flags, 8 View; none is folded to the tabs (S178a) - their order, and how wide. */
	UPROPERTY(config)
	bool bSidePanelOpen = true;

	UPROPERTY(config)
	int32 SidePanelShown = 1;

	UPROPERTY(config)
	FString SidePanelOrder = TEXT("0,1,2,3");

	UPROPERTY(config)
	int32 SidePanelWidthPx = 240;

	/** Project content, never plugin content. */
	UPROPERTY(config, EditAnywhere, Category = "Bake", meta = (ToolTip = "Where baked meshes are written: a content folder of the project."))
	FString BakeFolder = TEXT("/Game/SurfacingProBaked/");

	/**
	 * A group is baked one mesh per square cell of this size on the floor
	 * plan, so a long wall streams in as the player reaches it rather than
	 * all at once. Zero: one mesh per group.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bake", meta = (ToolTip = "Baked meshes are split into square cells of this size on the floor plan, in centimetres; 0 does not split.", ClampMin = "0"))
	double BakeCellCm = 2000.0;

	/** Baked meshes are Nanite, with the fallback kept whole for collision. Off: a plain static mesh. */
	UPROPERTY(config, EditAnywhere, Category = "Bake", meta = (ToolTip = "Baked meshes are Nanite; off, plain static meshes."))
	bool bBakeNanite = true;

	// Snapping guides, in centimetres. Nothing depends on the exact numbers;
	// they exist so blockout dimensions are one click away.
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuidePlayerHeightCm = 180.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideCrouchHeightCm = 90.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideStepHeightCm = 20.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideMantleLowCm = 50.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideMantleHighCm = 120.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideDoorWidthCm = 100.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideDoorHeightCm = 220.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideWallHeightCm = 300.0;
	UPROPERTY(config, EditAnywhere, Category = "Guides") double GuideWallThicknessCm = 25.0;

	/** Clamped, so a bad config index cannot crash a tool. */
	double GetGridStepCm(int32 Index) const;

	/** Nearest ladder index in the given direction; Delta -1 finer, +1 coarser. */
	int32 StepGridIndex(int32 Index, int32 Delta) const;

	/** Snap one scalar to a step. Step <= 0 returns Value unchanged. */
	static double SnapToStep(double Value, double StepCm);

	static FVector SnapToStep(const FVector& Value, double StepCm);
};
