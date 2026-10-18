// Copyright MapModelers. All Rights Reserved.
//
// The Flags window (S152d): what the bake does with the picked pieces -
// Detail, No collision, No shadow, No draw - as four ticks that read every
// picked patch and brush and write all of them at once. A nomad tab like
// Paint, so it stays up while the next thing is picked: "a full popup, may
// stay open while user go select other thing(s)".

#pragma once

#include "CoreMinimal.h"

namespace SurfFlagsTab
{
	/** "SurfacingProFlags", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** The SidePanel, on its Flags tab (S177a). */
	void Open();

	/** The panel's content, for the SidePanel. */
	TSharedRef<class SWidget> MakePanel();
}
