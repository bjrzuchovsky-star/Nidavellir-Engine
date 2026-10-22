#include "editor/assets/GmaImport.h"

#include "editor/ui/Theme.h"
#include "scene/JsonTree.h"
#include "export/Provenance.h"
#include "render3d/ModelImport.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace ts {
namespace tombstone {
namespace editor {
namespace gmaimport {

namespace fs = std::filesystem;
namespace gma = render3d::gma;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// "models/a/b.dx90.vtx" -> "models/a/b" (lower case); "" for non-model files.
std::string model_stem(const std::string& name) {
  const std::string l = lower(name);
  for (const char* e : {".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx", ".mdl", ".vvd", ".phy", ".ani"}) {
    const std::size_t n = std::strlen(e);
    if (l.size() > n && l.compare(l.size() - n, n, e) == 0) return l.substr(0, l.size() - n);
  }
  return {};
}

const char* kCategories[] = {"models", "materials", "sound", "other"};
const char* kCategoryLabel[] = {"Models", "Materials", "Sounds", "Other"};

}  // namespace

bool open(const std::string& gma_path, Plan* out, std::string* error) {
  *out = Plan{};
  out->gma_path = gma_path;
  if (!gma::read_file(gma_path, &out->archive, error)) return false;
  out->archive_crc_ok = gma::archive_crc_ok(out->archive);
  for (const gma::Entry& e : out->archive.files) {
    Item it;
    it.name = e.name;
    it.category = gma::category(e.name);
    it.size = e.size;
    it.crc_ok = gma::entry_crc_ok(out->archive, e);
    it.safe = !gma::safe_relative_path(e.name).empty();
    it.selected = it.crc_ok && it.safe;
    if (!it.crc_ok) ++out->bad_crc;
    out->items.push_back(std::move(it));
  }
  return true;
}

std::string addon_folder_name(const Plan& plan) {
  std::string src = plan.archive.name;
  if (src.empty()) src = u8(fs::u8path(plan.gma_path).stem());
  std::string out;
  for (unsigned char c : src) {
    if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
    else if (!out.empty() && out.back() != '_') out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.size() > 64) out.resize(64);
  return out.empty() ? std::string("Addon") : out;
}

std::string default_destination(const std::string& claim_dir, const Plan& plan) {
  const std::string base = "assets/Models/" + addon_folder_name(plan);
  std::error_code ec;
  std::string d = base;
  for (int n = 2; fs::exists(fs::u8path(claim_dir) / fs::u8path(d), ec) && n < 1000; ++n) {
    d = base + "_" + std::to_string(n);
  }
  return d;
}

void select(Plan* plan, const std::string& category, bool on) {
  for (Item& it : plan->items) {
    if (category.empty() || it.category == category) it.selected = on && it.crc_ok && it.safe;
  }
}

bool extract(const std::string& claim_dir, const Plan& plan, const std::string& dest_in, Result* out) {
  *out = Result{};
  out->dest = dest_in.empty() ? default_destination(claim_dir, plan) : dest_in;
  std::replace(out->dest.begin(), out->dest.end(), '\\', '/');
  while (!out->dest.empty() && out->dest.back() == '/') out->dest.pop_back();
  if (gma::safe_relative_path(out->dest).empty()) {
    out->errors.push_back("destination must be a folder inside the claim: " + out->dest);
    return false;
  }
  // A selected model brings its siblings.
  std::set<std::string> stems;
  for (const Item& it : plan.items) {
    if (it.selected && lower(it.name).size() > 4 && lower(it.name).compare(lower(it.name).size() - 4, 4, ".mdl") == 0) {
      stems.insert(model_stem(it.name));
    }
  }
  const fs::path root = fs::u8path(claim_dir) / fs::u8path(out->dest);
  std::map<std::string, exporter::provenance::Record> tags;
  const std::string when = exporter::provenance::now_stamp();
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    const Item& it = plan.items[i];
    const gma::Entry& e = plan.archive.files[i];
    const std::string stem = model_stem(it.name);
    const bool want = it.selected || (!stem.empty() && stems.count(stem));
    if (!want) continue;
    const std::string rel = gma::safe_relative_path(e.name);
    if (rel.empty()) {
      out->skipped.push_back(e.name + ": unsafe path, not extracted");
      continue;
    }
    if (!gma::entry_crc_ok(plan.archive, e)) {
      out->errors.push_back(e.name + ": CRC32 mismatch (damaged archive), not extracted");
      continue;
    }
    const fs::path dst = root / fs::u8path(rel);
    std::error_code ec;
    if (fs::exists(dst, ec)) {
      out->skipped.push_back(rel + ": already exists, kept");
      continue;
    }
    fs::create_directories(dst.parent_path(), ec);
    std::ofstream f(dst, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(plan.archive.data(e)), static_cast<std::streamsize>(e.size));
    if (!f) {
      out->errors.push_back(rel + ": could not write " + u8(dst));
      continue;
    }
    const std::string prel = out->dest + "/" + rel;
    out->written.push_back(prel);
    if (lower(rel).size() > 4 && lower(rel).compare(lower(rel).size() - 4, 4, ".mdl") == 0) out->models.push_back(prel);
    exporter::provenance::Record r;
    r.origin = "gma:" + (plan.archive.name.empty() ? addon_folder_name(plan) : plan.archive.name);
    r.format = "source-engine";
    r.author = plan.archive.author;
    r.addon = plan.archive.name;
    r.source = plan.gma_path;
    r.imported = when;
    tags[prel] = r;
  }
  if (!tags.empty()) {
    std::string err;
    if (!exporter::provenance::tag(claim_dir, tags, &err)) out->errors.push_back("provenance ledger: " + err);
  }
  return !out->written.empty();
}

// ---- Source materials panel ------------------------------------------------

bool draw_source_materials(assetdb::AssetDb& db, const assetdb::AssetDb::Entry& e, FolderBrowser* picker,
                           std::string* picking_for) {
  const json::Value& mats = e.meta.info["source_materials"];
  if (!mats.is_array() || mats.size() == 0) return false;
  bool changed = false;
  json::Value s = e.meta.settings;
  auto set_override = [&](const std::string& name, const std::string& file) {
    json::Value mo = s["material_overrides"].is_object() ? s["material_overrides"] : json::Value::object();
    json::Value next = json::Value::object();
    for (const auto& [k, v] : mo.members()) {
      if (k != lower(name)) next.set(k, v);
    }
    if (!file.empty()) next.set(lower(name), json::Value::string(file));
    s.set("material_overrides", next);
    std::string err;
    changed = db.set_settings(e.rel, s, &err);
  };
  const int missing = static_cast<int>(e.meta.info.num("missing_materials", 0));
  if (ImGui::TreeNode("srcmats", "Source materials (%zu, %d missing)", mats.size(), missing)) {
    for (std::size_t i = 0; i < mats.size(); ++i) {
      const json::Value& m = mats[i];
      const std::string name = m.str("name"), status = m.str("status");
      ImGui::PushID(static_cast<int>(i));
      const bool bad = status == "missing" || status == "partial";
      ImGui::TextColored(bad ? theme::Warning() : theme::TextMuted(), "%s", status.c_str());
      ImGui::SameLine();
      ImGui::TextUnformatted(name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Relink...")) {
        *picking_for = e.rel + "|" + name;
        picker->open_file(u8(fs::u8path(db.abs(e.rel)).parent_path()), {".vmt", ".vtf", ".png", ".jpg", ".tga"},
                          "Pick the .vmt / .vtf / image for '" + name + "'");
      }
      if (status == "override") {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) set_override(name, "");
      }
      if (!m.str("material").empty()) ImGui::TextDisabled("  %s", m.str("material").c_str());
      if (bad && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.str("searched").c_str());
      if (bad && !m.str("searched").empty()) ImGui::TextWrapped("  %s", m.str("searched").c_str());
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  if (!picking_for->empty() && picking_for->rfind(e.rel + "|", 0) == 0 && picker->draw("Relink Source material")) {
    std::string file = picker->take_result();
    const std::string name = picking_for->substr(e.rel.size() + 1);
    picking_for->clear();
    if (!file.empty()) {
      // Relative to the model's folder when inside the claim (moves with it).
      std::error_code ec;
      const fs::path model_dir = fs::u8path(db.abs(e.rel)).parent_path();
      const fs::path rel = fs::relative(fs::u8path(file), model_dir, ec);
      const fs::path claim_rel = fs::relative(fs::u8path(file), fs::u8path(db.project_dir()), ec);
      if (!ec && !claim_rel.empty() && u8(claim_rel).rfind("..", 0) != 0 && !rel.empty()) {
        const auto g = rel.generic_u8string();
        file.assign(g.begin(), g.end());
      }
      set_override(name, file);
    }
  }
  return changed;
}

// ---- Dialog ------------------------------------------------------------------

void Dialog::open_picker(const std::string& claim_dir) {
  claim_ = claim_dir;
  picking_ = true;
  browser_.open_file(std::string(), {".gma", ".bin"}, "Pick a Source addon (.gma, or a compressed .bin download)");
}

void Dialog::open_file(const std::string& claim_dir, const std::string& gma_path) {
  claim_ = claim_dir;
  picking_ = false;
  error_.clear();
  result_ = Result{};
  if (!open(gma_path, &plan_, &error_)) plan_.items.clear();
  const std::string d = error_.empty() ? default_destination(claim_, plan_) : std::string();
  std::memset(dest_, 0, sizeof(dest_));
  std::strncpy(dest_, d.c_str(), sizeof(dest_) - 1);
  open_ = true;
  request_popup_ = true;
}

bool Dialog::draw() {
  bool extracted = false;
  if (picking_) {
    if (browser_.draw("Import Source addon (.gma)")) {
      const std::string p = browser_.take_result();
      picking_ = false;
      if (!p.empty()) open_file(claim_, p);
    } else if (!browser_.is_open()) {
      picking_ = false;
    }
  }
  if (!open_) return false;
  if (request_popup_) {
    ImGui::OpenPopup("Import Source addon (.gma)##gma");
    request_popup_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Import Source addon (.gma)##gma", &open_)) return false;
  const gma::Archive& a = plan_.archive;
  ImGui::TextDisabled("%s", plan_.gma_path.c_str());
  if (!error_.empty()) {
    theme::StatusError(error_.c_str());
  } else {
    ImGui::Text("%s", a.name.empty() ? "(unnamed addon)" : a.name.c_str());
    ImGui::TextDisabled("by %s  |  %zu files%s%s", a.author.empty() ? "?" : a.author.c_str(), a.files.size(),
                        a.lzma ? "  |  was LZMA-compressed" : "", a.type.empty() ? "" : ("  |  " + a.type).c_str());
    if (plan_.bad_crc > 0) {
      theme::StatusError((std::to_string(plan_.bad_crc) + " file(s) fail their CRC32 check and will be skipped").c_str());
    } else {
      ImGui::TextDisabled("CRC32 verified for every file%s", plan_.archive_crc_ok ? "" : " (archive checksum differs)");
    }
    ImGui::BeginChild("##gmafiles", ImVec2(0, -110), ImGuiChildFlags_Borders);
    for (int c = 0; c < 4; ++c) {
      int total = 0, on = 0;
      for (const Item& it : plan_.items) {
        if (it.category != kCategories[c]) continue;
        ++total;
        on += it.selected ? 1 : 0;
      }
      if (!total) continue;
      bool all = on == total;
      ImGui::PushID(c);
      if (ImGui::Checkbox("##all", &all)) select(&plan_, kCategories[c], all);
      ImGui::SameLine();
      if (ImGui::TreeNode("cat", "%s (%d / %d)", kCategoryLabel[c], on, total)) {
        int k = 0;
        for (Item& it : plan_.items) {
          if (it.category != kCategories[c]) continue;
          ImGui::PushID(k++);
          ImGui::BeginDisabled(!it.crc_ok || !it.safe);
          ImGui::Checkbox("##f", &it.selected);
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::Text("%s%s", it.name.c_str(), !it.crc_ok ? "  (CRC mismatch)" : !it.safe ? "  (unsafe path)" : "");
          ImGui::PopID();
        }
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("Destination##gmadest", dest_, sizeof(dest_));
    ImGui::TextDisabled("Models also bring their .vvd / .vtx / .phy. Existing files are kept.");
    theme::StatusInfo(render3d::kModelRightsNote);
    if (theme::PrimaryButton("Import selected", ImVec2(160, 30))) {
      extract(claim_, plan_, dest_, &result_);
      extracted = !result_.written.empty();
      if (result_.errors.empty()) {
        open_ = false;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
  }
  if (theme::SecondaryButton("Close", ImVec2(100, 30))) {
    open_ = false;
    ImGui::CloseCurrentPopup();
  }
  for (const std::string& e : result_.errors) theme::StatusError(e.c_str());
  ImGui::EndPopup();
  return extracted;
}

}  // namespace gmaimport
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
