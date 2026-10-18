// Copyright MapModelers. All Rights Reserved.

using UnrealBuildTool;

public class SurfacingPro : ModuleRules
{
	public SurfacingPro(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",
			"GeometryFramework",
			"InputCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"DeveloperSettings",
			"RenderCore",
			"RHI"
		});
	}
}
