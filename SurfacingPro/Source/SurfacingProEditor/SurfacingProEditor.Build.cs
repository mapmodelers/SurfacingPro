// Copyright MapModelers. All Rights Reserved.

using UnrealBuildTool;

public class SurfacingProEditor : ModuleRules
{
	public SurfacingProEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SurfacingPro"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"ApplicationCore",
			"UnrealEd",
			"EditorFramework",
			"InteractiveToolsFramework",
			"EditorInteractiveToolsFramework",
			"PropertyEditor",
			"ToolMenus",
			"GeometryCore",
			"GeometryFramework",
			"MaterialEditor",
			"RenderCore",
			"RHI",
			"DeveloperSettings",
			"Settings",
			"Json",
			"Projects",
			"EditorInteractiveToolsFramework",
			"LevelEditor",
			"SceneOutliner",
			"MeshDescription",
			"StaticMeshDescription",
			"WorkspaceMenuStructure",
			"AssetRegistry",
			"AssetTools",
			"ImageCore",
			"ImageWrapper",
			"DesktopPlatform",
			"AppFramework",
			"AdvancedPreviewScene",
			"ContentBrowser"
		});
	}
}
