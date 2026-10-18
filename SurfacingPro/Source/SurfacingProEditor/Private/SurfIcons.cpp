// Copyright MapModelers. All Rights Reserved.

#include "SurfIcons.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

namespace SurfIcons
{
	namespace
	{
		/** Verb label -> icon file (Resources/Icons/Tabler/<icon>.svg). One idea per icon, none used twice. */
		const TMap<FString, FString>& Table()
		{
			static const TMap<FString, FString> Icons = {
			{ TEXT("Actions"), TEXT("layout-grid") },
			{ TEXT("Align"), TEXT("layout-align-left") },
			{ TEXT("All"), TEXT("select-all") },
			{ TEXT("Apply"), TEXT("check") },
			{ TEXT("Bake"), TEXT("flame") },
			{ TEXT("Bevel"), TEXT("border-radius") },
			{ TEXT("Bevel Line"), TEXT("vector-bezier-2") },
			{ TEXT("Box"), TEXT("square-dashed") },
			{ TEXT("Bridge"), TEXT("building-bridge") },
			{ TEXT("Brush"), TEXT("circle-dashed") },
			{ TEXT("Close"), TEXT("arrow-bar-up") },
			{ TEXT("Colour IDs"), TEXT("palette") },
			{ TEXT("Continue"), TEXT("player-track-next") },
			{ TEXT("Copy Up"), TEXT("copy-plus") },
			{ TEXT("Crease"), TEXT("angle") },
			{ TEXT("Cut"), TEXT("scissors") },
			{ TEXT("Delete"), TEXT("trash") },
			{ TEXT("Density"), TEXT("chart-dots-3") },
			{ TEXT("Duplicate"), TEXT("copy") },
			{ TEXT("Edit UI"), TEXT("layout-board") },
			{ TEXT("Even"), TEXT("layout-distribute-horizontal") },
			{ TEXT("Every Other"), TEXT("line-dashed") },
			{ TEXT("Extrude"), TEXT("box-align-top") },
			{ TEXT("Face Outward"), TEXT("arrow-up-from-arc") },
			{ TEXT("Fit"), TEXT("arrows-maximize") },
			{ TEXT("Flags"), TEXT("flag") },
			{ TEXT("Flatten"), TEXT("line") },
			{ TEXT("Flip"), TEXT("flip-horizontal") },
			{ TEXT("Flow"), TEXT("route") },
			{ TEXT("Freeze"), TEXT("snowflake") },
			{ TEXT("Grid"), TEXT("grid-3x3") },
			{ TEXT("Group"), TEXT("border-corners") },
			{ TEXT("Ungroup"), TEXT("marquee-off") },
			{ TEXT("Grow"), TEXT("arrows-diagonal") },
			{ TEXT("Hide"), TEXT("eye-off") },
			{ TEXT("Inflate"), TEXT("circle-plus") },
			{ TEXT("Inherit Face"), TEXT("copy-check") },
			{ TEXT("Invert"), TEXT("switch-horizontal") },
			{ TEXT("Isolate"), TEXT("focus-centered") },
			{ TEXT("Join"), TEXT("link") },
			{ TEXT("Lasso"), TEXT("lasso") },
			{ TEXT("Lattice"), TEXT("box-model-2") },
			{ TEXT("Layer"), TEXT("stack-2") },
			{ TEXT("Level"), TEXT("stack") },
			{ TEXT("Linked"), TEXT("hierarchy") },
			{ TEXT("Loop"), TEXT("separator-vertical") },
			{ TEXT("Material Manager"), TEXT("tool") },
			{ TEXT("Matcap"), TEXT("sun") },
			{ TEXT("Material Library"), TEXT("sphere") },
			{ TEXT("Merge Containers"), TEXT("arrow-merge") },
			{ TEXT("Names"), TEXT("tag") },
			{ TEXT("None"), TEXT("square-off") },
			{ TEXT("Notes"), TEXT("notes") },
			{ TEXT("Open"), TEXT("arrow-bar-down") },
			{ TEXT("Origin"), TEXT("focus-2") },
			{ TEXT("Paint"), TEXT("brush") },
			{ TEXT("From Cavity"), TEXT("circle-half-2") },
			{ TEXT("Smooth Paint"), TEXT("ripple") },
			{ TEXT("Contrast"), TEXT("brightness-half") },
			{ TEXT("Levels"), TEXT("adjustments-horizontal") },
			{ TEXT("Invert Paint"), TEXT("switch-3") },
			{ TEXT("Patch"), TEXT("grid-pattern") },
			{ TEXT("Prefab"), TEXT("packages") },
			{ TEXT("Assets"), TEXT("folders") },
			{ TEXT("Prefab from Selection"), TEXT("package-import") },
			{ TEXT("Vertices"), TEXT("point") },
			{ TEXT("Edges"), TEXT("vector") },
			{ TEXT("Faces"), TEXT("polygon") },
			{ TEXT("Pieces"), TEXT("cube") },
			{ TEXT("Auto"), TEXT("pointer") },
			{ TEXT("Meshes"), TEXT("box-multiple") },
			{ TEXT("Context"), TEXT("frame") },
			{ TEXT("Game"), TEXT("device-gamepad-2") },
			{ TEXT("Overlay"), TEXT("vector-off") },
			{ TEXT("Paint View"), TEXT("paint-sphere") },
			{ TEXT("Pack"), TEXT("puzzle") },
			{ TEXT("Stack"), TEXT("stack-3") },
			{ TEXT("Rectify"), TEXT("grid-4x4") },
			{ TEXT("Straighten"), TEXT("arrows-horizontal") },
			{ TEXT("Prefabs"), TEXT("building-warehouse") },
			{ TEXT("Primitive"), TEXT("cube-plus") },
			{ TEXT("Project"), TEXT("cube-send") },
			{ TEXT("Push"), TEXT("arrow-big-down") },
			{ TEXT("Random"), TEXT("dice-5") },
			{ TEXT("Replace"), TEXT("replace") },
			{ TEXT("Respace"), TEXT("spacing-horizontal") },
			{ TEXT("Same"), TEXT("equal") },
			{ TEXT("Separate"), TEXT("arrows-split") },
			{ TEXT("Settings"), TEXT("settings") },
			{ TEXT("Shrink"), TEXT("arrows-diagonal-minimize-2") },
			{ TEXT("Side Panel"), TEXT("layout-sidebar-right") },
			{ TEXT("Smooth"), TEXT("wave-sine") },
			{ TEXT("Snap"), TEXT("magnet") },
			{ TEXT("Soft"), TEXT("target") },
			{ TEXT("Normals"), TEXT("normals-sphere") },
			{ TEXT("Solidify"), TEXT("box") },
			{ TEXT("Statistics"), TEXT("chart-bar") },
			{ TEXT("Steps"), TEXT("stairs") },
			{ TEXT("Subdivide"), TEXT("layout-grid-add") },
			{ TEXT("Swap UV"), TEXT("arrows-exchange") },
			{ TEXT("Textures"), TEXT("texture") },
			{ TEXT("UV Smooth"), TEXT("blur") },
			{ TEXT("UVs"), TEXT("grid-dots") },
			{ TEXT("Unhide"), TEXT("eye") },
			{ TEXT("Unique"), TEXT("fingerprint") },
			{ TEXT("Unpack"), TEXT("package-export") },
			{ TEXT("Weld"), TEXT("arrows-join") },
			{ TEXT("Wires"), TEXT("wall") },
			};
			return Icons;
		}

		FName BrushName(const FString& Icon)
		{
			return FName(*(FString(TEXT("SurfacingPro.Verb.")) + Icon));
		}
	}

	void Register(FSlateStyleSet& Style)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SurfacingPro"));
		if (!Plugin.IsValid())
		{
			return;
		}
		const FString Dir = Plugin->GetBaseDir() / TEXT("Resources/Icons/Tabler");
		TSet<FString> Done;
		// Icons of the interface itself, not of a verb (S199a).
		for (const TCHAR* Own : { TEXT("box"), TEXT("ghost-3"), TEXT("magnet"), TEXT("target"), TEXT("focus-centered") })
		{
			const FString File = Dir / (FString(Own) + TEXT(".svg"));
			if (FPaths::FileExists(File))
			{
				Done.Add(Own);
				Style.Set(BrushName(Own), new FSlateVectorImageBrush(File, FVector2D(16.0, 16.0)));
			}
		}
		// The prefab asset's own icon in the content browser and the outliner
		// (S228a: "just give them a unique icon rather than the blueprint
		// icon") - the editor finds ClassIcon.<Class> in any registered style.
		{
			const FString File = Dir / TEXT("packages.svg");
			if (FPaths::FileExists(File))
			{
				Style.Set(TEXT("ClassIcon.SurfPrefab"), new FSlateVectorImageBrush(File, FVector2D(16.0, 16.0)));
				Style.Set(TEXT("ClassThumbnail.SurfPrefab"), new FSlateVectorImageBrush(File, FVector2D(64.0, 64.0)));
			}
		}
		// Our own drawings (S230a), beside the Tabler set: the falloff profiles,
		// the Paint view's ball and the smooth Normals ball (S249a).
		{
			const FString OwnDir = Plugin->GetBaseDir() / TEXT("Resources/Icons/Own");
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(OwnDir / TEXT("*.svg")), true, false);
			for (const FString& File : Files)
			{
				const FString Name = FPaths::GetBaseFilename(File);
				Style.Set(BrushName(Name), new FSlateVectorImageBrush(OwnDir / File, FVector2D(16.0, 16.0)));
				Done.Add(Name);
			}
		}
		for (const TPair<FString, FString>& Pair : Table())
		{
			if (Done.Contains(Pair.Value))
			{
				continue;
			}
			Done.Add(Pair.Value);
			const FString File = Dir / (Pair.Value + TEXT(".svg"));
			if (FPaths::FileExists(File))
			{
				Style.Set(BrushName(Pair.Value), new FSlateVectorImageBrush(File, FVector2D(16.0, 16.0)));
			}
		}
	}

	FString IconName(const FString& Label)
	{
		const FString* Found = Table().Find(Label);
		return Found ? *Found : FString();
	}

	const FSlateBrush* Named(const FString& Icon)
	{
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("SurfacingProStyle"));
		return Style ? Style->GetOptionalBrush(BrushName(Icon), nullptr, nullptr) : nullptr;
	}

	const FSlateBrush* ForVerb(const FString& Label)
	{
		const FString Icon = IconName(Label);
		if (Icon.IsEmpty())
		{
			return nullptr;
		}
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("SurfacingProStyle"));
		return Style ? Style->GetOptionalBrush(BrushName(Icon), nullptr, nullptr) : nullptr;
	}

	ESurfIconMode ModeFor(ESurfIconSurface Surface)
	{
		const USurfSettings* Settings = GetDefault<USurfSettings>();
		if (!Settings)
		{
			return ESurfIconMode::Words;
		}
		switch (Surface)
		{
		case ESurfIconSurface::Sidebar: return Settings->SidebarIconMode;
		case ESurfIconSurface::Actions: return Settings->ActionsIconMode;
		default:                        return Settings->MenuIconMode;
		}
	}
}
