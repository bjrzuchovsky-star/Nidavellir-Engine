#include "runtime/GraphScript.h"
#include "runtime/ScriptHost.h"

namespace ts {
namespace tombstone {
namespace runtime {

// Built-in languages beyond Lua. Compiled languages (C++ modules, C#)
// register here too once their hosts exist.
void register_builtin_script_languages() {
  register_script_language("graph", [](ScriptWorld& w, ScriptLibrary& l, const std::string& dir) {
    return std::make_unique<GraphScriptHost>(w, l, dir);
  });
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
