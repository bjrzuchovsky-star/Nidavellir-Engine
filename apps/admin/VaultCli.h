#pragma once

// ts_admin --vault push|pull|list|status|update (The Vault from the
// command line) and --smoke-vault. See docs/vault.md.
namespace ts {
namespace tombstone {

int run_vault_cli(int argc, char** argv);
int run_vault_smoke();
// (ts_admin --vault-shot <png> renders the panel over vault_demo(); main.cpp.)

}  // namespace tombstone
}  // namespace ts

#include <string>

namespace ts {
namespace tombstone {

// Builds a small demo library + claim under `root` (shot / docs): several
// textures, a model with its texture, a prefab and a sound, one item pulled
// with a newer version waiting. Returns the claim and library folders.
bool vault_demo(const std::string& root, std::string* project, std::string* library, std::string* model_item,
                std::string* error);

}  // namespace tombstone
}  // namespace ts
