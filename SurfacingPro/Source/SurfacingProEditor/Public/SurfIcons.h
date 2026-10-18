// Copyright MapModelers. All Rights Reserved.
//
// The icon mode (S195a): "a bank of icons large enough to cover all our
// buttons ... an icon mode available, toggleable." One open pack (Tabler,
// MIT, its notice in Resources/Icons/Tabler/LICENSE.txt), drawn in the
// theme's foreground; each surface - the sidebar, the Actions window, the
// menus - shows words, icons, or both, as its setting says. A verb with no
// icon keeps its word whatever the setting.

#pragma once

#include "CoreMinimal.h"
#include "SurfSettings.h"

class FSlateStyleSet;
struct FSlateBrush;

enum class ESurfIconSurface : uint8
{
	Sidebar,
	Actions,
	Menus,
};

namespace SurfIcons
{
	/** A brush per verb that has an icon, added to the style set as "SurfacingPro.Verb.<icon>". */
	void Register(FSlateStyleSet& Style);

	/** The icon of a verb, by its label; null when it has none. */
	SURFACINGPROEDITOR_API const FSlateBrush* ForVerb(const FString& Label);

	/** The icon's file name for a verb, by its label; empty when it has none. */
	SURFACINGPROEDITOR_API FString IconName(const FString& Label);

	/** One of the shipped icons by its file name ("box", "ghost-3"); null when missing (S199a). */
	SURFACINGPROEDITOR_API const FSlateBrush* Named(const FString& Icon);

	/** What a surface shows, as the settings say. */
	SURFACINGPROEDITOR_API ESurfIconMode ModeFor(ESurfIconSurface Surface);
}
