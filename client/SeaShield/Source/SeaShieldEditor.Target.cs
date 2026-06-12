using UnrealBuildTool;

public class SeaShieldEditorTarget : TargetRules
{
	public SeaShieldEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SeaShield");
	}
}
