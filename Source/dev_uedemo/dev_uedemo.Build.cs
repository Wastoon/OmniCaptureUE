// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class dev_uedemo : ModuleRules
{
	public dev_uedemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json",
    							"JsonUtilities", "InputCore", "EnhancedInput","RenderCore", "RHI", "Renderer", "ImageWrapper"});
	}
}
