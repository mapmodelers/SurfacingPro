// Copyright MapModelers. All Rights Reserved.
//
// The Parameters window (S152e): what one verb has to set, and where its
// button lives - out of the menus and the right-click popups and into one
// place. "you have in the menu all the parameters, instead of having them in
// the menu, use param popup! This is an overall example." A right-click on
// any verb - on the bar, in the SurfacingPro window, in a menu - brings the
// window to that verb. A nomad tab, so it docks, floats and comes back.

#pragma once

#include "CoreMinimal.h"

struct FSurfVerb;

namespace SurfParamsTab
{
	/** "SurfacingProParameters", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** Open it, or bring it forward, on whatever verb it last showed. */
	void Open();

	/** Open it on this verb. */
	void Show(const FSurfVerb& Verb);

	/**
	 * A verb ran (S157h): the window turns to it, opening as a passing
	 * frame if it was closed - "it shall always open, and close on its own
	 * if user has used left click". Dismiss() is that left click, in the
	 * viewport; a frame opened by hand (Show, Open) stays.
	 */
	void Follow(const FString& Id);
	void Dismiss();
}
