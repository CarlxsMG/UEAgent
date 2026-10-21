// Copyright (c) 2026 CarlxsMG.

using UnrealBuildTool;

// The USD values the engine's USD wrapper has no conversion for (integer list ops, path expressions), read and built
// with the USD SDK itself for the UEAgent USD commands. The pxr headers need RTTI, which UEAgent is built without, so
// this is a module of its own, set up like the engine's USD modules (USDCore's USDUtilities.Build.cs); its API takes
// and returns UE types only.
public class UEAgentUsdSdk : ModuleRules
{
	public UEAgentUsdSdk(ReadOnlyTargetRules Target) : base(Target)
	{
		// The pxr headers use typeid and dynamic_cast.
		bUseRTTI = true;

		// One source file: a shared PCH would be an RTTI build of UnrealEd's, compiled for this module alone.
		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"UnrealUSDWrapper",  // UE::FVtValue (public API), the USD SDK headers and libraries, USE_USD_SDK
			});

		if (Target.bBuildEditor)
		{
			// As USDUtilities: the SDK's parsers throw and catch inside its own code.
			bEnableExceptions = true;
		}

		// The USD memory setup every module that includes the SDK needs (see UnrealUSDWrapper.Build.cs).
		UnrealBuildTool.Rules.UnrealUSDWrapper.CheckAndSetupUsdSdk(Target, this);
	}
}
