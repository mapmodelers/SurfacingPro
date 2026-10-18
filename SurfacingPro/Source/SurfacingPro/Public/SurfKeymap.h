// Copyright MapModelers. All Rights Reserved.
//
// Named actions, and the chords that trigger them.
//
// Nothing in the mode tests a key directly. It asks whether an action fired,
// and this decides - which is what makes a second scheme a table rather than
// a rewrite, and what lets a mapper rebind one action without touching the
// rest.
//
// Two schemes ship. SurfacingPro is the house layout, chosen to stay clear of
// the Unreal viewport defaults. Blender mirrors the modelling keys a 3D artist
// already has in their fingers, which is worth more than internal consistency
// for anyone coming from that side.
//
// Where the two would disagree with Unreal itself, Unreal wins on camera and
// the house wins on editing: Focus is F in both schemes because that is what
// the engine has trained everyone to press, and no scheme rebinds Alt-drag.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "SurfKeymap.generated.h"

UENUM(BlueprintType)
enum class ESurfKeyScheme : uint8
{
	/** The house layout. Clear of the Unreal viewport defaults. */
	SurfacingPro,
	/** The modelling keys a Blender artist already knows. */
	Blender
};

/**
 * Everything the mode can be asked to do from the keyboard. Add here, bind in
 * both tables, and the panel's shortcut readout updates itself.
 */
UENUM(BlueprintType)
enum class ESurfAction : uint8
{
	None,

	SlideEdge,
	Weld,
	NextPatch,
	PreviousPatch,
	TeleportToCursor,
	Bevel,
	Delete,

	/** Which patches show their wires, and whether selection reaches through surfaces. */
	CycleWireView,
	SubModeObject,
	SubModeFace,
	SubModeEdge,
	SubModeVertex,

	/** Frame the selection, or the whole group when nothing is selected. */
	FocusSelection,

	SelectAll,
	SelectNone,
	InvertSelection,
	/** One step outward or inward along the grid (S165a): Ctrl + numpad plus / minus. */
	GrowSelection,
	ShrinkSelection,

	/** Rubber band, freehand loop, and paint-over-to-select. */
	BoxSelect,
	LassoSelect,
	BrushSelect,

	/** Put a row or column through the patch under the cursor. */
	InsertLoop,

	/** Gizmo modes, so G/R/S land where a Blender user reaches for them. */
	Translate,
	Rotate,
	Scale,

	/** Cut a brush with a plane. */
	Slice,

	/** Turn the tessellated surface into an editable vertex grid. */
	ConvertToSheet,

	/** Grow the patch past the edge the selection sits on. */
	Extrude,

	/** Proportional editing: on and off, and the radius, without leaving the viewport. */
	ToggleSoftSelection,
	SoftRadiusUp,
	SoftRadiusDown,

	/** Copy the selection in place and move the copy. */
	Duplicate,

	/** The viewport between the modelling view (unlit, cheap) and the shaded one. */
	ToggleShadedView,

	/** The search frame at the cursor: every verb that can run, by name; anything else typed is a note in the session record. */
	Search,

	/** The next soft selection profile: smooth, sphere, root, inverse square, sharp, linear, constant, random. */
	CycleSoftFalloff,

	/** The viewport under the cursor fills the layout, or goes back to its place. */
	ToggleMaximizeViewport,

	/** Frame the selection in every viewport at once. */
	FocusAllViewports,

	/** Between the four-pane layout and one pane, as Blender's Ctrl+Alt+Q. */
	ToggleFourViews,

	/** Hide the patches the selection holds; hide every other patch; show everything hidden again. */
	HideSelected,
	HideUnselected,
	UnhideAll,

	/** Only the selection, framed, until pressed again: Blender's local view. */
	IsolateSelection,

	/** Run the last verb pressed on the bar (or picked in the search) again, on whatever is selected now. */
	RepeatLastVerb,

	/** The verbs the user starred, as a menu at the cursor (S156b): Blender's Quick Favorites. */
	QuickFavorites,

	/** Paint the layer weight with a brush. */
	PaintWeight,

	/** The snap reach, longer and shorter: the bar's + and - on keys. */
	SnapReachUp,
	SnapReachDown,

	/** Into the selected prefab instance to edit its asset; out again, keeping - one step down or up the nesting. */
	PrefabIn,
	PrefabOut,

	/**
	 * Cut the picked block of faces out as a patch of its own; the rest of
	 * the patch comes apart around the hole (S98a). "p to separate ?" - P in
	 * the Blender scheme, where it is free and where it is Blender's own key
	 * for exactly this; Shift+P in the house scheme, where P already picks
	 * with the brush.
	 */
	Separate,

	/**
	 * Blender's Recalculate Outside, on the key Blender puts it on: Shift+N
	 * in both schemes, where N is free (S121a).
	 */
	FaceOutward,

	/**
	 * Box, lasso, brush, and round again (S144a): "make it cycle through
	 * selection modes while pressing W ? box select etc" - Blender's key
	 * for exactly this, and free in the Blender scheme. W is Move in the
	 * house scheme, so the cycle takes Shift+W there - every action needs a
	 * key in every scheme, or that scheme cannot reach it.
	 */
	CycleSelectTool,

	/** Every patch joined to the picked ones through a shared point, and on (S172a): Ctrl+L, Blender's select linked. */
	SelectLinked,

	/** Object and Edit (S175a): Tab, as in Blender - Object is whole pieces, Edit is 1 2 3. */
	ToggleObjectEdit,

	/** The SidePanel (S177a): N, as in Blender - Item, Paint, Flags, View down the viewport's edge. */
	ToggleSidePanel,

	/** Snapping on and off (S198a): Shift+Tab, as the snap menu says. */
	ToggleSnap,

	/** Every wire of ours off and on (S199a): Alt+Z - Shift+Alt stays the editor's. */
	ToggleOverlay,

	/** The picked pieces made one group (S249b): Ctrl+G, as the editor groups actors. */
	GroupPieces,

	/** One level of the picked groups undone (S249b): Ctrl+Alt+G - Shift+G is Slide here. */
	UngroupPieces,

	Count
};

USTRUCT()
struct SURFACINGPRO_API FSurfKeyBinding
{
	GENERATED_BODY()

	UPROPERTY(config, EditAnywhere, Category = "Keys")
	ESurfAction Action = ESurfAction::None;

	UPROPERTY(config, EditAnywhere, Category = "Keys")
	FKey Key;

	UPROPERTY(config, EditAnywhere, Category = "Keys")
	bool bCtrl = false;

	UPROPERTY(config, EditAnywhere, Category = "Keys")
	bool bShift = false;

	UPROPERTY(config, EditAnywhere, Category = "Keys")
	bool bAlt = false;
};

namespace SurfKeymap
{
	/** The chord this scheme gives the action, before any user override. */
	SURFACINGPRO_API FSurfKeyBinding Default(ESurfKeyScheme Scheme, ESurfAction Action);

	/** The chord in force: the scheme's, unless the user has overridden it. */
	SURFACINGPRO_API FSurfKeyBinding Resolve(ESurfAction Action);

	/** Which action this chord fires, or None. */
	SURFACINGPRO_API ESurfAction Match(const FKey& Key, bool bCtrl, bool bShift, bool bAlt);

	/** "Ctrl+R" - for the panel, so the shortcut is discoverable without a manual. */
	SURFACINGPRO_API FString Describe(ESurfAction Action);

	SURFACINGPRO_API FString ActionName(ESurfAction Action);
}
