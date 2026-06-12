using UnrealBuildTool;

public class SeaShield : ModuleRules
{
	public SeaShield(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "UMG", "Slate", "SlateCore",
			"Niagara", "SeaShieldCore"
		});

		// Water plugin API (ocean body / gerstner waves wiring in
		// SeaLevelSetupLibrary — the asset-reference types are not scriptable).
		PrivateDependencyModuleNames.Add("Water");
	}
}
