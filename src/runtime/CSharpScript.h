#pragma once

// C# gameplay scripts: every <project>/scripts/*.cs builds (dotnet build,
// .NET 8 SDK) into one assembly, ProjectScripts.dll, against the
// TombStone.Scripting binding (sdk/dotnet). The engine hosts .NET in
// process through hostfxr (no nethost link: the runtime is found at run
// time) and loads each build into a collectible AssemblyLoadContext, so a
// hot reload unloads the old one. Same hooks / world API as every language
// (CompiledScriptHost). Without the SDK, C# scripts are disabled with a
// clear Telegraph message (an export's prebuilt assembly needs only the
// .NET 8+ runtime).

#include "runtime/CompiledScript.h"

#include <string>

namespace ts {
namespace tombstone {
namespace runtime {

namespace csharp_scripts {

// The hint the editor and the Telegraph show when C# is unavailable.
extern const char* const kInstallHint;  // "install the .NET 8 SDK ... to enable C#"

struct DotnetInfo {
  std::string dotnet;     // dotnet executable
  std::string root;       // DOTNET_ROOT
  std::string hostfxr;    // host/fxr/<ver>/(lib)hostfxr
  std::string sdk;        // newest SDK >= 8 ("8.0.426"), empty = none
  std::string runtime;    // newest Microsoft.NETCore.App >= 8, empty = none
};
// Found once per process (cached; thread safe).
const DotnetInfo& dotnet_info();
// C# scripts can be built here (SDK >= 8 and a runtime to host)?
bool sdk_available(std::string* why);
// The TombStone.Scripting binding sources (sdk/dotnet/TombStone.Scripting), "" when missing.
std::string binding_sources_dir();
// "C# scripts disabled: ..." text (empty when available).
std::string disabled_reason(bool prebuilt);

bool has_sources(const std::string& project_dir);
// scripts/*.cs compiled into the assembly, project-relative, sorted.
// Skips generated folders and lists each canonical path once
// (case-insensitive on Windows). No wildcard: a glob feeds some copies twice.
std::vector<std::string> compile_files(const std::string& project_dir);
std::int64_t sources_stamp(const std::string& project_dir);
std::string prebuilt_assembly(const std::string& project_dir);  // <p>/dotnet/ProjectScripts.dll
std::string dev_assembly(const std::string& project_dir);       // <p>/.tombstone/dotnet/bin/...
// Files an export ships next to the assembly (dotnet/...): the binding and
// its runtimeconfig. Names relative to the build's bin folder.
std::vector<std::string> shipped_files(const std::string& project_dir);
// Generates <project>/.tombstone/dotnet/{binding,scripts} and runs dotnet build.
ScriptBuildResult build(const std::string& project_dir, bool force = false);
// Managed load contexts still alive (after a GC); -1 when .NET is not up.
int live_contexts();

}  // namespace csharp_scripts

class CSharpScriptHost final : public CompiledScriptHost {
 public:
  CSharpScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir);
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
