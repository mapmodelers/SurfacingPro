// Copyright MapModelers. All Rights Reserved.
//
// Every verb as a menu (S79).
//
// The bar is for the six things a hand presses all day and the SurfacingPro
// window is for arranging them. Neither is where you look for the verb you
// use once a week - "i want be able to right click and-or click some menu
// instead of having the whole surfacingpro window open". So: the same pool
// of verbs, as a menu with a section per family, on the right button in the
// viewport and in the editor's own menu bar.
//
// One pool, three surfaces (D96): a verb added to the tool appears in all of
// them without being listed anywhere else.

#pragma once

#include "CoreMinimal.h"
#include "SurfToolPalette.h"

class FMenuBuilder;
class USurfEdMode;

namespace SurfMenu
{
	/** The whole tree, for a menu builder that already exists. */
	void Build(FMenuBuilder& Menu, USurfEdMode* Mode);

	/**
	 * One family's verbs, flat, for a menu of its own (S152b): the line of
	 * family menus under the top line - "a new top bar just underneath
	 * current one, with each menu category same from right click".
	 */
	void BuildFamily(FMenuBuilder& Menu, USurfEdMode* Mode, ESurfVerbGroup Family);
	const FText& FamilyTitle(ESurfVerbGroup Family);

	/** The windows, flat. */
	void BuildWindows(FMenuBuilder& Menu);

	/** One verb as a menu row (left runs it, right opens its options), for menus built either way (S193c). */
	TSharedRef<class SWidget> MakeVerbRow(const FSurfVerb& Verb);

	/**
	 * The right-click on any verb, wherever it is (S196a): "i should be able
	 * right click fav/q menu anything". Options, the Q menu, its whole menu
	 * into the Q menu, the sidebar. At the cursor. Given the row it came
	 * from, a menu under it stays open (S197a: "is it possible it remains
	 * underneath?") until a choice is made.
	 */
	void OpenVerbContext(const FSurfVerb& Verb, TSharedPtr<class SWidget> Anchor = nullptr, int32 ShowAsSurface = -1);

	/**
	 * The lines of that right-click without the options (S199a): the Q menu,
	 * its whole menu, the sidebar, pin - for a button whose right-click has
	 * a menu of its own (Soft, Snap), added at its foot.
	 */
	void AddVerbLines(class FMenuBuilder& Menu, const FSurfVerb& Verb);

	/** The right-click on a menu itself (S196a): the whole menu into the Q menu, as one entry. */
	void OpenFamilyContext(ESurfVerbGroup Family, TSharedPtr<class SWidget> Anchor = nullptr);

	/** A whole menu's id in the Q list ("menu:Geometry"). */
	FString FamilyFavId(ESurfVerbGroup Family);

	/**
	 * A verb of the pool as a menu row, found by its name (S204a), so a menu
	 * entry that is a verb has the verb's right-click. Shown, when given, is
	 * the word on the row; the verb keeps its own name. False when the mode
	 * is not up or no verb has that name.
	 */
	bool AddVerbNamed(class FMenuBuilder& Menu, const FString& Label, const FText& Shown);

	/** The top strip's View menu: the View verbs and the SurfacingPro view, windows and settings (S193c). */
	TSharedRef<class SWidget> MakeViewMenu();

	/** Open it where the cursor is, over the viewport. */
	void OpenAtCursor(USurfEdMode* Mode, class FEditorViewportClient* ViewportClient);

	/** The starred verbs at the cursor (S156b): Q. Returns false with nothing starred. */
	bool OpenQuickFavorites(USurfEdMode* Mode, class FEditorViewportClient* ViewportClient);

	/** The SurfacingPro pull-down in the level editor's menu bar. */
	void RegisterMainMenu();
	void UnregisterMainMenu();
}
