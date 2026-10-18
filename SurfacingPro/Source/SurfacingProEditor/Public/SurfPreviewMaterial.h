// Copyright MapModelers. All Rights Reserved.
//
// The checker the preview wears until a real material is assigned.
//
// A surfacing tool whose default material is flat grey cannot be used: every
// verb here changes UVs, and with nothing to read them against the whole
// panel is guesswork. So the plugin ships its own, built in code rather than
// as an asset - no content to cook, no redirectors, and it works the first
// time a group appears in an empty project.
//
// The texture is designed to be read, not to look nice:
//   - an eight by eight checker, so density is countable
//   - a bright border on the tile edge, so tiling and scale are obvious
//   - one corner marked, so rotation, flips and swaps are visible at a glance
//
// Everything is transient. Nothing here is ever saved into the user's project.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;

namespace SurfPreviewMaterial
{
	/**
	 * The shared checker, built on first use and cached for the session.
	 * Null only if material compilation failed, which the caller treats as
	 * "no material" rather than an error.
	 */
	SURFACINGPROEDITOR_API UMaterialInterface* GetChecker();

	/**
	 * Build the checker now, at a safe moment - the mode calls this when it
	 * is entered. Compiling a material recreates render state everywhere it
	 * is used, which must never happen from inside a draw.
	 */
	SURFACINGPROEDITOR_API void WarmUp();

	/**
	 * Refill the checker between the greys the settings ask for, if they
	 * changed. Cheap (one small texture), safe anywhere a draw is not.
	 */
	SURFACINGPROEDITOR_API void RefreshLevels();

	/** Rebuild the checker for the shade the settings ask for (lit, or shaded by the view), if it changed, and hand it to every group. */
	SURFACINGPROEDITOR_API void RefreshShade();

	/** The greys the checker is currently filled between; -1 before it is built. */
	SURFACINGPROEDITOR_API void GetLevels(float& OutDarkest, float& OutLightest);

	/**
	 * The gizmo's material: unlit, translucent, two-sided, its colour and
	 * opacity from a vector parameter named Color - so one material serves
	 * every handle through a coloured render proxy. Built with the checker
	 * at WarmUp; null only if compilation failed (the gizmo draws lines).
	 */
	SURFACINGPROEDITOR_API UMaterialInterface* GetGizmoMaterial();

	/**
	 * The handles' colours: one instance of the gizmo material per tint,
	 * recoloured when the settings change, so a handful of instances serve
	 * every handle. Null if the material failed.
	 */
	enum class ESurfGizmoTint : uint8 { X, Y, Z, Neutral, Hot, Count };
	SURFACINGPROEDITOR_API UMaterialInterface* GetGizmoMaterial(ESurfGizmoTint Tint, const FLinearColor& Colour);

	/**
	 * The paint view (S69a): every surface shows the channel being painted,
	 * dark where it is empty and lit in the channel's colour where it is
	 * full, shaded by the view so the shape still reads. Built in code on
	 * first use, one per channel, cached for the session. Null only if
	 * compilation failed.
	 */
	SURFACINGPROEDITOR_API UMaterialInterface* GetPaintView(int32 Channel); // 0-3 a channel, 4 the colour itself

	/** Clay lit from the camera (S172b), its colour from the settings. Built by WarmUp; null before. */
	SURFACINGPROEDITOR_API UMaterialInterface* GetMatcap();

	/** Unlit, two-sided, lerping the backdrop's two colours by UV.y (0 top, 1 bottom). Built by WarmUp; null before. */
	SURFACINGPROEDITOR_API UMaterialInterface* GetBackdrop();

	/** Clay on everything in the scene (S173a): a post-process pass from the scene's normals, lit pixels only, the sky left alone. Built by WarmUp; null before. */
	SURFACINGPROEDITOR_API UMaterialInterface* GetMatcapPost();

	/** The backdrop's gradient as a pass into the empty sky of the unlit views (S250a), before the see-through things. Built by WarmUp; null before. */
	SURFACINGPROEDITOR_API UMaterialInterface* GetBackdropPost();

	/** The checker's own texture (S208a), for the UV window's backdrop when a patch wears nothing; null before WarmUp. */
	SURFACINGPROEDITOR_API class UTexture2D* GetCheckerTexture();

	/** The studio sphere as a 16-pixel icon for the menus (S182a): round, transparent outside. Built on first ask; outside a draw. */
	SURFACINGPROEDITOR_API class UTexture2D* GetMatcapIcon();

	/** One ball of the matcap looks (S188a), for the picker; its own texture, built on first ask. */
	SURFACINGPROEDITOR_API class UTexture2D* GetMatcapLookIcon(int32 Look);

	/** Push the settings' colours into the ones above. Outside a draw. */
	SURFACINGPROEDITOR_API void RefreshViewColours();

	/** The ghost (S71a): a pale see-through grey for what is in view but out of reach while a prefab is edited in place. */
	SURFACINGPROEDITOR_API UMaterialInterface* GetGhost();

	/** Called at module start-up: makes this the default for groups with no material. */
	void RegisterAsGroupDefault();

	void UnregisterAsGroupDefault();
}
