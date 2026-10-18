// Copyright MapModelers. All Rights Reserved.
//
// The tool surfaces: a labelled grid in the docked panel, and a slim vertical
// bar inside the viewport.
//
// A details view renders whatever a UPROPERTY declares and nothing else - no
// font, no colour, no grouping. That is why the panel was unreadable, and why
// this is hand-built Slate instead.
//
// Two surfaces because they answer different questions. The grid is "what can
// this tool do", read occasionally, everything visible at once with a colour
// down the left saying which family a verb belongs to. The bar is "the six
// things I am pressing right now", and it lives in the viewport because the
// trip to a docked panel and back is the single biggest cost in a modelling
// session.

#pragma once

#include "CoreMinimal.h"
#include "SurfKeymap.h"
#include "Widgets/SCompoundWidget.h"

class USurfEdMode;
enum class ESurfIconSurface : uint8;

/** What family a verb belongs to. Colour is the only thing this decides. */
enum class ESurfVerbGroup : uint8
{
	Create,
	Geometry,
	Shape,
	Surface,
	UV,
	Select,
	/** What is shown and how, not what is picked (S154h): wires, names, colour ids, hide, isolate. */
	View
};

/** One button: what it says, what it does, and whether it can run right now. */
class FMenuBuilder;

struct FSurfVerb
{
	FText Label;
	FText Tip;
	ESurfVerbGroup Group = ESurfVerbGroup::Geometry;
	TFunction<void()> Run;
	TFunction<bool()> IsEnabled;

	/**
	 * The verb's options, built into a menu on a right-click of its button.
	 * A decision that changes what a verb does belongs beside the verb, not
	 * in a settings window two clicks away. Empty for verbs with nothing to
	 * decide.
	 */
	TFunction<void(FMenuBuilder&)> Options;

	/**
	 * A label that carries a number - "Layer 2" - read each frame, so the
	 * state a verb cycles through is on the button that cycles it. Empty for
	 * verbs whose name is the whole story.
	 */
	TFunction<FText()> LiveLabel;

	/**
	 * The key that does the same thing, when there is one (S129a).
	 *
	 * "Overall tool when hovering something (a button..) then you show the
	 * description - good. It should now always start with (Shortcut) -
	 * Description so we know the shortcut key by hovering."
	 *
	 * The action, not the chord: the chord is whatever the scheme and his
	 * overrides make it, and a tooltip printing last week's key is worse
	 * than one printing none.
	 */
	ESurfAction Shortcut = ESurfAction::None;

	/** A repair verb (S247a): in the Actions window and the search, not in the family menus. */
	bool bRare = false;

	/** A window (S247f): listed under View › Windows, not among the View family's rows. */
	bool bWindow = false;
};

/**
 * Where each verb lives, and how it is moved (S74a). "All buttons are
 * moveable arrangeable as user like. Buttons cant get lose theyre either on
 * surfacingpro either on userbar": one pool, two surfaces, and a verb that
 * has never been arranged takes its default place - so a verb added to the
 * tool later lands somewhere rather than nowhere.
 *
 * A pool is passed as its ids, because that is all the arrangement is about.
 */
/**
 * The pool: every verb there is, once each (S74a). The bar, the SurfacingPro
 * window and the menu (S79) all draw from it, so a verb added to the tool
 * appears in all three without being listed anywhere else. Host is whichever
 * widget is asking - a name prompt has to be pushed from one; a menu with no
 * widget of its own passes nothing and the few verbs that ask for a name are
 * pushed from the level editor instead.
 */
namespace SurfVerbs
{
	TArray<FSurfVerb> BuildPool(TWeakObjectPtr<class USurfEdMode> Mode, TWeakPtr<class SWidget> Host);

	/**
	 * What a verb's tooltip says: "(Ctrl+B) - Round the crease along the
	 * selected lines." (S129a)
	 *
	 * Every surface that shows a tip goes through here - the bar, the panel
	 * and the menu - so the shortcut appears in all three and the rule is
	 * written once. Read when the tooltip is asked for, not when the button
	 * is built, because the key scheme is a setting he can change.
	 */
	SURFACINGPROEDITOR_API FText VerbTip(const FSurfVerb& Verb);
}

namespace SurfVerbLayout
{
	/** What a verb is called in the arrangement: its label, which is unique across the pool. */
	FString IdOf(const FSurfVerb& Verb);
	TArray<FString> PoolIds(const TArray<FSurfVerb>& Pool);

	/** Where this verb's button lives, as a row of icons - across, earlier, later, put back - for the Parameters window (S154a). */
	TSharedRef<class SWidget> MakeArrangeRow(const FSurfVerb& Verb, TArray<FString> Pool);

	/** True for the verbs that ship on the bar. */
	bool DefaultsToBar(const FString& Id);

	/** Quick Favorites (S156b): starred verbs, kept by id, shown on Q. */
	bool IsFavorite(const FString& Id);
	void ToggleFavorite(const FString& Id);

	/** The ids on one surface, in order: what was arranged first, then whatever was never arranged and belongs there. */
	TArray<FString> OrderedIds(bool bBar, const TArray<FString>& Pool);
	TArray<const FSurfVerb*> Ordered(bool bBar, const TArray<FSurfVerb>& Pool);

	bool OnBar(const FString& Id, const TArray<FString>& Pool);
	/** Whether a verb is on the shelf (S174a). */
	bool OnShelf(const FString& Id, const TArray<FString>& Pool);

	/** Move a verb to the other surface, or one place along its own. Both lists are written out, so an arrangement is explicit once touched. */
	void SendTo(const FString& Id, bool bBar, const TArray<FString>& Pool);
	void Nudge(const FString& Id, int32 Delta, const TArray<FString>& Pool);
	/** Id put just before Target, on whichever surface Target is on - a drop (S153g, A4). */
	void MoveBefore(const FString& Id, const FString& Target, const TArray<FString>& Pool, bool bAfter = false);
	/** Content wrapped as a drop target for a dragged chip: let go on it, the chip goes to the end of that surface (S162a). */
	TSharedRef<class SWidget> MakeDropZone(TSharedRef<class SWidget> Content, bool bBar, const TArray<FString>& Pool);
	/** Content that takes a dragged chip off the bar and the shelf (S166a): the window. */
	TSharedRef<class SWidget> MakeRemoveZone(TSharedRef<class SWidget> Content, const TArray<FString>& Pool);
	void RemoveFromSurfaces(const FString& Id, const TArray<FString>& Pool);

	/** The arrangement as it ships. */
	void Reset();

	/**
	 * Edit-UI mode (S82a). "user should enable edit mode on the window. then
	 * move things around... right click > edit UI - thats an overall mode can
	 * edit all uis then."
	 *
	 * While it is on, a button is a thing you move rather than a thing you
	 * press: a click sends it to the other surface, the arrows either side
	 * move it along, and nothing runs. Off again and the buttons are buttons.
	 * It is a state of the session, not a setting: leaving it on across a
	 * restart would be a tool that does not work and does not say why.
	 */
	bool IsEditing();
	void SetEditing(bool bOn);

	/**
	 * Words, icons or both for one section, from its right-click while the
	 * interface is arranged (S206a): "in edit ui mode should be possible to
	 * toggle icon/text/icon and text ... it will switch the section you
	 * clicked into".
	 */
	void AddShowAsLines(class FMenuBuilder& Menu, ESurfIconSurface Surface);
	/** The same lines as a menu of their own, at the cursor. */
	void OpenShowAs(ESurfIconSurface Surface);

	/** The shelf (S156d): the whole pool as a column beside the bar, unfolded by its chip. Not saved. */
	bool IsShelfOpen();
	void SetShelfOpen(bool bOpen);

	/** Arrangements kept by name, saved and brought back from any surface. */
	TArray<FString> LayoutNames();
	void SaveLayoutAs(const FString& Name, const TArray<FString>& Pool);
	bool UseLayout(const FString& Name);
	void ForgetLayout(const FString& Name);

	/** Ask for a name and save the arrangement under it. */
	void PromptSaveLayout(USurfEdMode* Mode);

	/** The interface section: the toggle, the arrangements, the way back. */
	void AppendInterfaceMenu(FMenuBuilder& Menu, USurfEdMode* Mode, bool bWithEditUI = true);

	/** Bumped by every change, so both surfaces notice and refill. */
	int32 Serial();
	/** The surfaces rebuild on the next tick (S166a). */
	void Touch();
}

/** The grid, for the docked panel. */
class SSurfToolPalette : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfToolPalette) {}
		SLATE_ARGUMENT(TWeakObjectPtr<USurfEdMode>, Mode)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** The whole pool, and the part of it that lives here, rebuilt when the arrangement changes (S74a). */
	void Refill();
	TSharedRef<SWidget> BuildSection(const FText& Heading, const TArray<const FSurfVerb*>& Verbs);
	TSharedRef<SWidget> BuildButton(const FSurfVerb& Verb);
	void OpenOptions(const FSurfVerb& Verb, const FGeometry& ButtonGeometry);

	TWeakObjectPtr<USurfEdMode> Mode;
	TArray<FSurfVerb> Pool;
	TSharedPtr<class SVerticalBox> Body;
	int32 LayoutSeen = -1;
};

/**
 * The last operation's numbers, live, in a frame at the bottom left of the
 * viewport - Blender's adjust-last-operation panel. There while the operation
 * is still the last thing in the history; changing a number undoes it and
 * runs it again.
 */
class SSurfAdjustFrame : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfAdjustFrame) {}
		SLATE_ARGUMENT(TWeakObjectPtr<USurfEdMode>, Mode)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** A press or a key inside the frame: the next settings change is the frame's doing (S166a). */
	virtual FReply OnPreviewMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override { bTouched = true; return FReply::Unhandled(); }
	virtual FReply OnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override { bTouched = true; return FReply::Unhandled(); }

private:
	bool bTouched = false;
	uint32 SettingsHash = 0;
	FString HashedFor;
	uint32 HashedSerial = 0;
};

/** The vertical bar, for inside the viewport. */
/** One thing the X search can find (S175a: kept between openings, it was rebuilt on every X). */
struct FSurfSearchEntry
{
	FText Label;
	FText Tip;
	TFunction<void()> Run;
	TFunction<bool()> IsEnabled;
	/** The verb behind it, when there is one: its right-click (S199a). */
	TSharedPtr<FSurfVerb> Verb;
	/** A setting (S208a): where it lives, said before its name; its value, said after; the number knob's property when it is one. */
	FText Section;
	TFunction<FText()> Value;
	class FProperty* Number = nullptr;
	bool bSetting = false;
};

class SSurfViewportBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSurfViewportBar) {}
		SLATE_ARGUMENT(TWeakObjectPtr<USurfEdMode>, Mode)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSurfViewportBar() override;

	/** The grip down the bar's right edge captures the mouse; these finish the drag. */
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
	/** Timed (S172b): what painting the bar costs a frame goes into the session record's frame line. */
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	/** "the sidebar should be resizable from the viewport directly not through settings" (S83a). */
	bool bResizingBar = false;
	float BarDragStartX = 0.0f;
	int32 BarDragStartWidth = 138;

	/** The grip at the foot of the column sets how long it may be (S162a). */
	bool bLengtheningBar = false;
	float BarDragStartY = 0.0f;
	int32 BarDragStartLength = 0;

	/** True while the bar itself is being dragged to the other edge (S91a), or down it (S165a). */
	bool bMovingBar = false;
	float BarTopOffset = 0.0f;

	/** How wide the top line may be before it starts to scroll. */
	FOptionalSize GetTopBarMaxWidth() const;

	TSharedRef<SWidget> BuildSubModeRow();

	/**
	 * One thin line across the top of the viewport (S90a): everything that is
	 * a state or a switch, and the name of what is being edited. It scrolls
	 * sideways rather than wrapping, so it is one line at any width and the
	 * side below it is nothing but verbs.
	 */
	TSharedRef<SWidget> BuildTopBar();

	/**
	 * The line under it (S152b): one pull-down per family - the same
	 * families the right button opens - then Windows and Interface. "Set up
	 * a new top bar just underneath current one. With each menu category
	 * same from right click."
	 */
	TSharedRef<SWidget> BuildMenuLine();
	/** How tall the SidePanel may stand before it scrolls (S177a). */
	FOptionalSize GetSidePanelMaxHeight() const;

	/** The numbers, small, down the bottom left corner. */
	TSharedRef<SWidget> BuildStatColumn();
	/** One line at the foot: the readings, then the keys of the moment (S158a). */
	TSharedRef<SWidget> BuildFootLine();

	/**
	 * What the last verb did, or would not do, in a panel of its own with a
	 * cross on it (S93a). It is the one thing here that is a sentence rather
	 * than a reading, and a sentence in the corner of a viewport is either
	 * read at once or in the way.
	 */
	TSharedRef<SWidget> BuildTipPanel();
	FText GetTip() const;
	EVisibility GetTipVisibility() const;

	/** The result already waved away; a newer one shows again. */
	int32 TipDismissed = -1;

	/**
	 * In and out, at the very start of the line (S99a): "topbar: in and out
	 * arrow buttons: extreme left of the bar". Where you are going is asked
	 * before what you are doing, so it is read before everything else.
	 */
	TSharedRef<SWidget> BuildNavSteps();

	/** The two things that change what you can see (S83a). */
	TSharedRef<SWidget> BuildNavRow();
	FText GetWhere() const;
	FSlateColor GetWhereColour() const;

	/**
	 * The title, in two pieces (S86a): the path down to what is open, grey,
	 * and the name of the thing itself, white. "Levelname/prefabs/ if we re
	 * editing in context, and only the prefab or level name if editing no
	 * context" - a prefab opened alone at the origin has no context, so
	 * there is no path to show.
	 */
	FText GetWherePath() const;
	FText GetWhereName() const;
	/** One row: soft selection, snapping and what it lands on (S78). */
	TSharedRef<SWidget> BuildSoftRow();
	TSharedRef<SWidget> BuildVerb(const FSurfVerb& Verb);

public:
	/** The numbers of the last operation, live; hidden when there is nothing to adjust. */
	static TSharedRef<SWidget> MakeAdjustSection(TWeakObjectPtr<USurfEdMode> Mode);

	/**
	 * The search frame at a screen position (X): every verb that can run,
	 * by name, filtered as you type; Enter runs the highlighted one, Up and
	 * Down move the highlight. Anything typed that names no verb - or
	 * anything with Ctrl or Shift held on Enter - goes to the session
	 * record as a note.
	 */
	void OpenSearch(const FVector2D& ScreenPosition);
	/** The same search over a part of the entries (S209a): the UV window's, say. Keep says which stay. */
	void OpenSearch(const FVector2D& ScreenPosition, TFunction<bool(const FSurfSearchEntry&)> Keep, TSharedPtr<SWidget> Parent = nullptr);

	/**
	 * When the search was opened, so the key that opened it does not type
	 * itself into it (S96a). Slate delivers the character to whatever has
	 * focus, and by then the box has it.
	 */
	double SearchOpenedAt = 0.0;
	/** The X search's list, built on the first X and kept. */
	TSharedPtr<TArray<FSurfSearchEntry>> SearchEntries;

	/**
	 * Prefab (S69d): a small prompt at the cursor for the name, started at
	 * the next free one; Enter makes the prefab under that name, Escape
	 * makes nothing.
	 */
	/** How tall the bar may be before it scrolls: the viewport's height, less a margin. */
	FOptionalSize GetBarMaxHeight() const;

	/** How wide the verbs may run before they wrap: the bar's width from the settings. */
	float GetVerbWrapWidth() const;

	void OpenPrefabName();
	/** Save as: a copy of the prefab being edited or selected, under a name asked for at the cursor (S71c). */
	void OpenSaveAsName();
	/** A one-line name prompt at the cursor; Enter commits, Escape closes. */
	void OpenNamePrompt(const FText& Title, const FString& Suggested, TFunction<void(const FString&)> Commit);

private:
	/** The bar's verbs, kept for the search. */
	TArray<FSurfVerb> QuickVerbs;

	/** The verbs that live on the bar, refilled when the arrangement changes (S74a). */
	void RefillVerbs();
	/** A section per family, each an aligned grid. Never folded, never moved (S89a). */
	TSharedPtr<class SVerticalBox> VerbFlow;
	int32 LayoutSeen = -1;
	/** What the sidebar holds, from the last refill (S193b): its width is only taken when something in it is open. */
	int32 SidebarFamilyMask = 0;
	bool bSidebarHasPinned = false;
	TWeakPtr<class IMenu> SearchMenu;
	TWeakPtr<class IMenu> PromptMenu;


	/** Pop a verb's options beside its button. Closes on a click anywhere else. */
	void OpenOptions(const FSurfVerb& Verb, const FGeometry& ButtonGeometry);

	/** The one line that says what is selected and which way it faces. */
	FText GetStatus() const;

	/** Texels a metre across the selection, and amber when they do not agree. */
	FText GetDensity() const;
	FSlateColor GetDensityColour() const;
	FText GetStack() const;
	FSlateColor GetStackColour() const;
	FSlateColor GetStatusColour() const;

	/** Whether the verbs are showing. Folded only when asked, never on hover. */
	EVisibility GetVerbVisibility() const;

	TWeakObjectPtr<USurfEdMode> Mode;

	/** Folded away by the button. Open by default. */
	bool bFolded = false;

	/** Backspace over a knob resets it; this is the hook ahead of all input that makes it so. */
	FDelegateHandle ResetListener;
};

/**
 * The Actions window (S193e): "need be able open the window with all
 * buttons. give it a name." Every verb by family, as a tab of its own that
 * docks anywhere; the same panel the mode's toolkit shows.
 */
namespace SurfActionsTab
{
	void Register();
	void Unregister();
	void Open();
}

/** The milliseconds spent painting the viewport bars since the last call (S172b), for the frame line. */
SURFACINGPROEDITOR_API double SurfTakeBarPaintMs();
