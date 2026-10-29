#include "editor/assets/ModelSplit.h"

#include "core/AssetMeta.h"
#include "render3d/MeshSplit.h"
#include "render3d/ModelImport.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace editor {
namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string rel_of(const std::string& claim, const std::string& abs) {
  std::error_code ec;
  return fs::relative(fs::u8path(abs), fs::u8path(claim), ec).generic_string();
}

}  // namespace

bool split_model_asset(assetdb::AssetDb& db, const std::string& rel, const ModelSplitOptions& opt,
                       ModelSplitResult* out) {
  if (!out) return false;
  *out = {};
  const assetdb::AssetDb::Entry* src = db.by_path(rel);
  if (!src || src->meta.kind != assetdb::Kind::Model) {
    out->error = "That is not a model.";
    return false;
  }
  render3d::Model model;
  std::string err;
  const std::string abs = db.abs(rel);
  if (!render3d::load_model(abs, &model, &err)) {
    out->error = err.empty() ? "Could not read " + rel : err;
    return false;
  }
  const fs::path file = fs::u8path(abs);
  const fs::path dest = file.parent_path() / (file.stem().string() + "_parts");
  render3d::SplitWrite wopt;
  wopt.recentre = opt.recentre;
  std::vector<render3d::SplitPart> parts;
  std::vector<std::string> written;
  if (!render3d::write_model_parts(model, dest.string(), wopt, &parts, &written, &out->error)) {
    if (out->error.empty()) out->error = "Could not split " + rel;
    return false;
  }
  db.scan();
  const json::Value& from = src->meta.settings;
  // by_path may have moved; re-find after scan.
  const assetdb::AssetDb::Entry* again = db.by_path(rel);
  const json::Value settings = again ? again->meta.settings : from;
  for (const render3d::SplitPart& part : parts) {
    const std::string part_rel = rel_of(db.project_dir(), (dest / part.rel_gltf).string());
    json::Value s = json::Value::object();
    s.set("scale", json::Value::number(settings.num("scale", 1)));
    s.set("unit_scale", json::Value::number(1));
    s.set("up_axis", json::Value::string("y"));
    s.set("material_path", json::Value::string(settings.str("material_path")));
    s.set("lod", json::Value::number(0));
    const bool fit = opt.fit_collider && settings.flag("generate_collider", true);
    s.set("generate_collider", json::Value::boolean(fit));
    std::string mode = settings.str("collider", "auto");
    if (!fit) mode = "none";
    else if (mode == "none") mode = "auto";
    s.set("collider", json::Value::string(mode));
    s.set("import_animations", json::Value::boolean(false));
    s.set("hull_vertices", json::Value::number(settings.num("hull_vertices", 64)));
    s.set("decompose_max", json::Value::number(settings.num("decompose_max", 8)));
    s.set("pivot", json::Value::string(opt.recentre ? "recenter" : "keep"));
    const float origin[3] = {part.origin.x, part.origin.y, part.origin.z};
    s.set("origin", json::floats(origin, 3));
    s.set("split_from", json::Value::string(rel));
    s.set("split_node", json::Value::string(part.node));
    std::string set_err;
    if (!db.set_settings(part_rel, s, &set_err)) {
      out->error = set_err.empty() ? "Could not write the .meta for " + part_rel : set_err;
      return false;
    }
    out->rels.push_back(part_rel);
    out->names.push_back(part.name);
    out->origins.push_back(part.origin);
  }
  for (const std::string& w : written) {
    ModelSplitFile f;
    f.rel = rel_of(db.project_dir(), w);
    f.bytes = slurp(fs::u8path(w));
    if (!f.rel.empty() && !f.bytes.empty()) out->files.push_back(std::move(f));
  }
  for (const std::string& part_rel : out->rels) {
    const std::string meta = core::asset_meta::meta_path(db.project_dir(), part_rel);
    ModelSplitFile f;
    f.rel = rel_of(db.project_dir(), meta);
    f.bytes = slurp(fs::u8path(meta));
    if (!f.rel.empty() && !f.bytes.empty()) out->files.push_back(std::move(f));
  }
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
