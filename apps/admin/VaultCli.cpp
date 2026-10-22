// ts_admin --vault: push / pull / list / status / update against the
// library in the editor settings (or --library DIR / --server URL --key K).
#include "VaultCli.h"

#include "editor/settings/Settings.h"
#include "editor/vault/VaultProject.h"
#include "vault/VaultBackend.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace tv = ts::tombstone::vault;

namespace ts {
namespace tombstone {

namespace {

void usage() {
  std::cerr <<
      "ts_admin --vault push   --project DIR --asset REL [--name N] [--tags a,b] [--dims 2d,3d,vr]\n"
      "                        [--author A] [--licence L] [--note TEXT] [--description TEXT] [--thumbnail PNG]\n"
      "ts_admin --vault pull   --project DIR --item ID|NAME [--version N] [--pin] [--force]\n"
      "ts_admin --vault list   [--query Q] [--kind K] [--tag T] [--dim D]\n"
      "ts_admin --vault status --project DIR\n"
      "ts_admin --vault update --project DIR\n"
      "Library: the editor settings, or --library DIR (local folder) or --server URL --key KEY\n"
      "(or TS_VAULT_KEY in the environment; the key is never printed).\n";
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

int run_vault_cli(int argc, char** argv) {
  std::string cmd;
  std::string project, asset, item, query, kind, tag, dim, library, server, key;
  int version = 0;
  bool pin = false, force = false;
  ed::vaultp::PushOptions po;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--vault") cmd = next();
    else if (a == "--project") project = next();
    else if (a == "--asset") asset = next();
    else if (a == "--item") item = next();
    else if (a == "--version") version = std::atoi(next().c_str());
    else if (a == "--pin") pin = true;
    else if (a == "--force") force = true;
    else if (a == "--query") query = next();
    else if (a == "--kind") kind = next();
    else if (a == "--tag") tag = next();
    else if (a == "--dim") dim = next();
    else if (a == "--library") library = next();
    else if (a == "--server") server = next();
    else if (a == "--key") key = next();
    else if (a == "--name") po.name = next();
    else if (a == "--tags") po.tags = tv::split_tags(next());
    else if (a == "--dims") po.dims = tv::normalise_dims(tv::split_tags(next()));
    else if (a == "--author") po.author = next();
    else if (a == "--licence" || a == "--license") po.licence = next();
    else if (a == "--note") po.note = next();
    else if (a == "--description") po.description = next();
    else if (a == "--thumbnail") po.thumbnail_png = next();
  }
  if (cmd != "push" && cmd != "pull" && cmd != "list" && cmd != "status" && cmd != "update") {
    usage();
    return 2;
  }
  const ed::Settings s = ed::SettingsStore::load();
  if (key.empty()) {
    if (const char* k = std::getenv("TS_VAULT_KEY")) key = k;
  }
  std::unique_ptr<tv::Backend> backend;
  if (!server.empty()) backend = tv::make_backend("remote", "", server, key.empty() ? s.vault_api_key : key);
  else if (!library.empty()) backend = tv::make_backend("local", library, "", "");
  else backend = tv::make_backend(s.vault_backend, s.vault_local_dir, s.vault_url, key.empty() ? s.vault_api_key : key);
  std::cout << "Vault: " << backend->label() << '\n';
  std::string err;
  if (cmd == "list") {
    tv::Query q{query, kind, tag, dim};
    std::vector<tv::Item> items;
    if (!backend->list(q, &items, &err)) {
      std::cerr << "vault list failed: " << err << '\n';
      return 1;
    }
    for (const tv::Item& it : items) {
      std::cout << it.id << "  " << it.kind << "  v" << it.latest << "  " << it.name
                << (it.dims.empty() ? "" : "  [" + tv::join(it.dims, "/") + "]")
                << (it.author.empty() ? "" : "  by " + it.author)
                << (it.licence.empty() ? "" : "  (" + it.licence + ")")
                << (it.tags.empty() ? "" : "  #" + tv::join(it.tags, " #")) << '\n';
    }
    std::cout << items.size() << " item(s)\n";
    return 0;
  }
  if (project.empty()) {
    std::cerr << "--project DIR is required for " << cmd << '\n';
    return 2;
  }
  if (cmd == "push") {
    if (asset.empty()) {
      std::cerr << "--asset REL (project-relative, e.g. assets/props/crate.gltf) is required\n";
      return 2;
    }
    std::cout << tv::kVaultRightsNote << '\n';
    ed::vaultp::PushResult r;
    if (!ed::vaultp::push_asset(project, nullptr, asset, *backend, po, &r, &err)) {
      std::cerr << "vault push failed: " << err << '\n';
      return 1;
    }
    std::cout << "pushed " << tv::join(r.pushed) << " (" << r.bytes << " bytes uploaded)\nitem " << r.item << " v"
              << r.version << '\n';
    return 0;
  }
  if (cmd == "pull") {
    if (item.empty()) {
      std::cerr << "--item ID|NAME is required\n";
      return 2;
    }
    if (!tv::valid_item_id(item)) {
      std::vector<tv::Item> items;
      if (!backend->list({}, &items, &err)) {
        std::cerr << "vault pull failed: " << err << '\n';
        return 1;
      }
      std::vector<std::string> hits;
      for (const tv::Item& it : items) {
        if (lower(it.name) == lower(item)) hits.push_back(it.id);
      }
      if (hits.size() != 1) {
        std::cerr << (hits.empty() ? "no item named '" : "several items named '") << item << "' (use the id)\n";
        return 1;
      }
      item = hits[0];
    }
    ed::vaultp::PullOptions opt;
    opt.version = version;
    opt.pin = pin;
    opt.force = force;
    ed::vaultp::PullResult r;
    if (!ed::vaultp::pull_item(project, nullptr, *backend, item, opt, &r, &err)) {
      std::cerr << "vault pull failed: " << err << '\n';
      return 1;
    }
    std::cout << "pulled " << tv::join(r.items) << " -> " << r.main << " (" << r.written.size() << " file(s) written)\n";
    return 0;
  }
  if (cmd == "status" || cmd == "update") {
    if (cmd == "update") {
      std::vector<std::string> done;
      if (!ed::vaultp::update_all(project, nullptr, *backend, &done, &err)) {
        std::cerr << "vault update failed: " << err << '\n';
        return 1;
      }
      std::cout << (done.empty() ? "nothing to update" : "updated " + tv::join(done)) << '\n';
    }
    std::vector<ed::vaultp::ItemStatus> st;
    if (!ed::vaultp::project_status(project, *backend, &st, &err)) {
      std::cerr << "vault status failed: " << err << '\n';
      return 1;
    }
    for (const auto& x : st) {
      std::cout << x.origin << "  " << x.name << "  v" << x.version << (x.pinned ? " (pinned)" : "")
                << (x.update_available ? "  UPDATE v" + std::to_string(x.latest) : "")
                << (x.modified ? "  changed here" : "") << (x.missing ? "  not in this library" : "") << "  " << x.main
                << '\n';
    }
    return 0;
  }
  return 2;
}

}  // namespace tombstone
}  // namespace ts
