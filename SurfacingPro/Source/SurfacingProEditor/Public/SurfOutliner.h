// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Collapsing the editor's outliner.
 *
 * The outliner opens fully expanded on every map load, and a map with a few
 * hundred pieces in it opens as a wall of names that has to be scrolled past
 * before anything can be found. There is no setting for this, but the widget
 * has a CollapseAll, and the widget can be found.
 */
namespace SurfOutliner
{
	/** Collapse every outliner currently open. Returns how many were collapsed. */
	int32 CollapseAll();

	/**
	 * Collapse everything, then open the level being worked in.
	 *
	 * Collapsing the lot hides the thing you actually came for. One level
	 * open, one deep, is the state you would have set by hand.
	 */
	int32 Tidy();

	/** Collapse shortly after a map opens, if the setting asks for it. */
	void RegisterMapOpenHook();
	void UnregisterMapOpenHook();
}
