// Copyright MapModelers. All Rights Reserved.
//
// The Paint window (S70b): everything the brush has, in one place - the
// brush itself, the channel (R, G, B, A, or the colour), the colour, the
// strength and the size, the view that shows the channel on the surfaces,
// and Fill and Clear. A nomad tab, so it docks beside the viewport and
// comes back with the layout, like the UV and Materials windows.

#pragma once

#include "CoreMinimal.h"

namespace SurfPaintTab
{
	/** "SurfacingProPaint", in the Window menu under the level editor. */
	extern const FName TabName;

	void Register();
	void Unregister();

	/** The SidePanel, on its Paint tab (S177a). */
	void Open();

	/** The panel's content, for the SidePanel. */
	TSharedRef<class SWidget> MakePanel();
}
