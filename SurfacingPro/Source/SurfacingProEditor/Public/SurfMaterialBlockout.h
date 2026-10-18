// Copyright MapModelers. All Rights Reserved.
//
// The blockout set (M-F2): materials for blocking a level out, drawn by
// code and shipped in the plugin's content, seeded into the library with
// the parents.
//
// A grid reads its size at a glance: each one is a square tile of its own
// size - 0.5, 1, 2 and 4 metres - with the metres printed in the middle,
// fine lines every 10 cm and firm lines every 50 cm, one hue per size so a
// wall reads as "the two-metre one" from across the room. Two strip
// materials band a surface every 25 or 50 cm along one axis, for stairs,
// ledges and anything measured in one direction. A flat grey fills what is
// not measured yet. Every one carries its surfacing info, so putting it on
// a patch sets the scale to the size it says and the tile is the size it
// prints.
//
// Nothing is copied from anywhere: the pixels are drawn here, the digits
// from a small font of our own.

#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialInstanceConstant;
class UTexture2D;

namespace SurfMaterialBlockout
{
	/** "/SurfacingPro/Materials/Blockout" */
	FString Folder();

	/** "/SurfacingPro/Materials/Blockout/M_SurfBlockout" */
	FString MasterPackage();

	/** One of the set: what it is for and how it is drawn. */
	struct FSurfBlockoutKind
	{
		/** The asset stem: "Grid_050", "Strip_025", "Fill". */
		FString Stem;
		/** Metres one tile covers (the material's DefaultScale); the size printed on a grid. */
		double TileMetres = 1.0;
		/** The hue the kind wears. */
		FLinearColor Colour = FLinearColor::White;
		/** A grid (lines and a label), a strip (bands along V), or a fill (flat). */
		enum class EShape : uint8 { Grid, Strip, Fill } Shape = EShape::Grid;
		/** For a strip: the band's height in metres. */
		double BandMetres = 0.25;
	};

	/** The seven kinds, in the order they are made. */
	const TArray<FSurfBlockoutKind>& Kinds();

	/** The texture's side in pixels. */
	int32 TextureSize();

	/** Draw one kind's pixels, BGRA, TextureSize by TextureSize. */
	void Draw(const FSurfBlockoutKind& Kind, TArray<FColor>& OutPixels);

	/** Print a digit, a point or an m at (X, Y) with a cell of Scale pixels; returns the width used. Exposed for the test. */
	int32 PrintGlyph(TCHAR Glyph, int32 X, int32 Y, int32 Scale, const FColor& Colour, TArray<FColor>& Pixels, int32 Side);

	struct FSurfBlockoutAssets
	{
		UMaterial* Master = nullptr;
		TArray<UTexture2D*> Textures;
		TArray<UMaterialInstanceConstant*> Instances;
	};

	/** Make the whole set from nothing into the plugin's content; saved when asked. Empty members, with the reason, when it cannot be. */
	FSurfBlockoutAssets Author(bool bSave, FString& OutError);

	/** The shipped set, loaded; whatever is missing stays null. */
	FSurfBlockoutAssets Load();
}
