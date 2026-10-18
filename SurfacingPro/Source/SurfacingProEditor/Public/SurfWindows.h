// Copyright MapModelers. All Rights Reserved.
//
// Where a small window of ours opens (S152e): "are we able to automatically
// size and position window like this one here? perhaps same for paint window
// (stacked up onto it or right next to it if more?)". A tab that opens
// floating is sized and set at the bottom-right of the level viewport, above
// the ones already standing there; one that is docked stays where it was
// docked.

#pragma once

#include "CoreMinimal.h"

class SDockTab;

namespace SurfWindows
{
	/**
	 * Open the nomad tab, or bring it forward. The first time it opens as a
	 * floating window it is given Size (in slate units) and a place by the
	 * viewport; after that it is wherever it was left.
	 */
	TSharedPtr<SDockTab> OpenPlaced(FName TabName, FVector2D Size);

	/**
	 * The tab's window kept off the taskbar when it floats (S154g), a
	 * moment after the tab is spawned - a layout restoring it at startup
	 * never goes through OpenPlaced. Safe to call for a docked tab.
	 */
	void KeepOffTaskbar(TSharedPtr<SDockTab> Tab);

	/**
	 * A frame of our own (S156e): a floating window with a 16-pixel strip -
	 * the name, a cross, a drag - and no dock-tab row. "Only thing the name
	 * tab can't be hidden even when there is no other tab and that
	 * screenspace is lost." Sized for its content up to Size, placed by the
	 * viewport like the tabs were, off the taskbar. Opening an open frame
	 * brings it forward. Windows > Dock opens the same content as a tab.
	 */
	TSharedPtr<SWindow> OpenFrame(FName Id, const FText& Title, TFunction<TSharedRef<SWidget>()> MakeContent, FVector2D Size);
	bool IsFrameOpen(FName Id);
	/** True when the tab sits in the editor's own window (docked by him), false when it floats. */
	bool TabIsDockedInEditor(const TSharedPtr<SDockTab>& Tab);
	void CloseFrame(FName Id);
	void CloseAllFrames();

	/**
	 * A window's menu-bar pull-down (S197a): the word and a small chevron,
	 * flat, the menu built when it opens. The UVs and Materials windows
	 * carry their verbs this way instead of rows of buttons.
	 */
	TSharedRef<class SWidget> MenuBarPullDown(const FText& Label, const FText& Tip, TFunction<TSharedRef<class SWidget>()> Content);
}
