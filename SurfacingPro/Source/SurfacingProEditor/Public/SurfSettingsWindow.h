// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The settings live on the editor's own preferences page (S247a, redundancy
 * W3): Editor Preferences > Plugins > SurfacingPro. Our window and tab that
 * copied it are gone (S250b).
 */
namespace SurfSettingsWindow
{
	/** The editor's preferences page, at SurfacingPro. */
	void Open();
}
