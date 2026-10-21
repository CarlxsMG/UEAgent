// Copyright (c) 2026 CarlxsMG.

using UnrealBuildTool;

// The Game Features commands' engine side (IUEAgentGameFeaturesBackend, declared in UEAgent). A module of
// its own because it links the GameFeatures plugin: UEAgent loads it only when the project enables it.
public class UEAgentGameFeatures : ModuleRules
{
	public UEAgentGameFeatures(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core", "CoreUObject", "Engine", "Json",
				"UEAgent",       // IUEAgentGameFeaturesBackend, mcp_common replies
				"GameFeatures",  // UGameFeaturesSubsystem
			});
	}
}
