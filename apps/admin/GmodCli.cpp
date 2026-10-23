#include "GmodCli.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/GmaImport.h"
#include "export/Exporter.h"
#include "export/Provenance.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace ts {
namespace tombstone {

namespace fs = std::filesystem;

int run_import_gma_cli(int argc, char** argv) {
  std::string file, claim, dest, only;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--import-gma") file = next();
    else if (a == "--claim") claim = next();
    else if (a == "--dest") dest = next();
    else if (a == "--only") only = next();
    else if (a == "--list") list = true;
  }
  if (file.empty() || (claim.empty() && !list)) {
    std::cerr << "usage: ts_admin --import-gma <file.gma|.bin> --claim <dir> [--dest <rel>] "
                 "[--only models,materials,sound,other] [--list]\n";
    return 2;
  }
  editor::gmaimport::Plan plan;
  std::string err;
  if (!editor::gmaimport::open(file, &plan, &err)) {
    std::cerr << err << '\n';
    return 1;
  }
  const auto& a = plan.archive;
  std::cout << "addon: " << a.name << "  by " << a.author << "  (" << a.files.size() << " files, gma v" << a.version
            << (a.lzma ? ", was LZMA-compressed" : "") << ")\n";
  if (!a.type.empty()) std::cout << "type: " << a.type << '\n';
  for (const auto& it : plan.items) {
    std::cout << "  [" << it.category << "] " << it.name << "  " << it.size << " bytes"
              << (it.crc_ok ? "" : "  CRC MISMATCH") << (it.safe ? "" : "  UNSAFE PATH") << '\n';
  }
  std::cout << (plan.bad_crc ? std::to_string(plan.bad_crc) + " file(s) fail CRC32\n" : "CRC32 ok for every file\n");
  if (list) return plan.bad_crc ? 1 : 0;
  if (!fs::is_regular_file(fs::u8path(claim) / "project.json")) {
    std::cerr << claim << " is not a claim (no project.json)\n";
    return 2;
  }
  if (!only.empty()) {
    std::set<std::string> cats;
    std::stringstream ss(only);
    std::string c;
    while (std::getline(ss, c, ',')) cats.insert(c == "sounds" ? "sound" : c);
    for (auto& it : plan.items) it.selected = it.selected && cats.count(it.category);
  }
  editor::gmaimport::Result r;
  const bool ok = editor::gmaimport::extract(claim, plan, dest, &r);
  std::cout << "extracted " << r.written.size() << " file(s) into " << r.dest << '\n';
  for (const auto& s : r.skipped) std::cout << "  skipped: " << s << '\n';
  for (const auto& e : r.errors) std::cerr << "  error: " << e << '\n';
  // Import what landed (models, .vtf textures, sounds) and report materials.
  editor::assetdb::AssetDb db;
  db.set_importer(editor::assetdb::import_asset);
  db.open(claim);
  db.scan();
  for (const std::string& m : r.models) {
    const auto* e = db.by_path(m);
    if (!e) continue;
    std::cout << "model " << m << ": " << (e->import_ok ? "imported" : "FAILED " + e->import_error) << '\n';
    for (const auto& mv : e->meta.info["source_materials"].items()) {
      std::cout << "  material " << mv.str("name") << ": " << mv.str("status");
      if (!mv.str("material").empty()) std::cout << " (" << mv.str("material") << ")";
      std::cout << '\n';
      if (!mv.str("searched").empty()) std::cout << "    " << mv.str("searched") << '\n';
    }
  }
  std::cout << "Tagged in " << exporter::provenance::kLedgerFile
            << " - you need the rights to ship this content.\n";
  return ok && r.errors.empty() ? 0 : 1;
}

int run_ack_rights_cli(int argc, char** argv) {
  std::string claim, asset;
  bool revoke = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ack-rights" && i + 1 < argc) {
      claim = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') asset = argv[++i];
    } else if (a == "--revoke") {
      revoke = true;
    }
  }
  if (claim.empty()) {
    std::cerr << "usage: ts_admin --ack-rights <claim> [<asset rel>] [--revoke]\n";
    return 2;
  }
  std::string err;
  if (!exporter::provenance::acknowledge(claim, asset, !revoke, &err)) {
    std::cerr << err << '\n';
    return 1;
  }
  const auto shipped = exporter::provenance::shipped_third_party(claim, exporter::content_files(claim));
  int open = 0;
  for (const auto& s : shipped) {
    open += s.acknowledged ? 0 : 1;
    std::cout << (s.acknowledged ? "  [x] " : "  [ ] ") << s.path << "  (" << s.origin << ")\n";
  }
  std::cout << shipped.size() << " third-party file(s), " << open << " not acknowledged\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
