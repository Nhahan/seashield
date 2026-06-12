using System.IO;
using UnrealBuildTool;

// Bridge module compiling the repo's std-only client stack — protocol/ (wire
// format, reliable UDP) and client/core/ (session engine, interpolation) —
// straight from their single source of truth via the repo_* symlinks in this
// directory (charter §7 "protocol 라이브러리를 UE5 모듈로 링크"; §11 링크
// 스파이크). The sources are exception-free, RTTI-free and std-only, so they
// build under UE's default flags. macOS/Linux only (POSIX sockets) — which is
// the supported client platform set for P5.
public class SeaShieldCore : ModuleRules
{
	public SeaShieldCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bUseUnity = false; // The repo TUs rely on per-file anonymous namespaces.

		PublicDependencyModuleNames.Add("Core");

		// Repo root, so the symlinked sources' includes ("protocol/wire.h",
		// "client/core/...") resolve exactly as they do under CMake.
		PublicIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../..")));
	}
}
