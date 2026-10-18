// Copyright MapModelers. All Rights Reserved.
//
// The painted mask (D14, re-cut by D64).
//
// A layer is order: a patch lifted a little above another, drawn by its own
// material's alpha. What the paint brush writes - the control points'
// colour, one channel at a time, red by default (D71) - is there for any
// material that chooses to read it. Two ship, both made by code so they can
// be made again:
//
//   MF_SurfPaintedMask  the one node a user plugs into their own material's
//                       Opacity Mask: the weight as painted, or sharpened
//                       against the material's own height when asked.
//   M_SurfPainted       a ready-made master for those who would rather not
//                       touch their materials: colour, normal, roughness,
//                       height, masked by the function.

#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialFunction;

namespace SurfMaterialPainted
{
	/** "/SurfacingPro/Materials/Functions/MF_SurfPaintedMask" */
	FString FunctionPackage();

	/** "/SurfacingPro/Materials/M_SurfPainted" */
	FString MasterPackage();

	struct FSurfPaintedAssets
	{
		UMaterialFunction* Function = nullptr;
		UMaterial* Master = nullptr;
	};

	/** Make both from nothing, into their packages; saved when asked. Null members, with the reason, when they cannot be. */
	FSurfPaintedAssets Author(bool bSave, FString& OutError);

	/** The shipped two, loaded; null members when they are not there. */
	FSurfPaintedAssets Load();
}
