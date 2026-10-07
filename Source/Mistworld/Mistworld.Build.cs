// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Mistworld : ModuleRules
{
	public Mistworld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"NavigationSystem",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"Niagara",
			"UMG",
			"Slate"
		});

        PrivateDependencyModuleNames.AddRange(new string[] { "AnimGraphRuntime" });
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		PublicIncludePaths.AddRange(new string[] {
			"Mistworld",
			"Mistworld/Variant_Strategy",
			"Mistworld/Variant_Strategy/UI",
			"Mistworld/Variant_TwinStick",
			"Mistworld/Variant_TwinStick/AI",
			"Mistworld/Variant_TwinStick/Gameplay",
			"Mistworld/Variant_TwinStick/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
