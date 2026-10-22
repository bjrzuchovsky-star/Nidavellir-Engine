#pragma once

// The Vault's data model, shared by the store, the server, both backends
// and the editor.
//
// An item is one library entry: a texture, a model with its sibling files
// (.bin / .mtl / .vvd / .vtx / .phy), a sound, a font or a prefab. Its id
// is the main asset's GUID (from its .meta), so the same asset keeps one
// identity in every project. Each upload of changed content adds a
// version; a version lists its files (content-addressed by SHA-256, the
// .meta included) and the items it depends on (a model's textures, a
// prefab's sprites / models / nested prefabs) at the version it was
// uploaded with.

#include "scene/JsonTree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace vault {

// What every upload screen and doc shows (the same note as the importer).
inline constexpr const char* kVaultRightsNote =
    "You are responsible for having the rights to anything you upload to a Vault.";

struct FileRec {
  std::string path;    // project-relative, '/' separated ("assets/props/crate.gltf")
  std::string sha256;  // content address
  std::uint64_t size = 0;
  std::string role;    // "main" | "file" (siblings, extra textures) | "meta"
  std::string guid;    // main / meta: the asset GUID ("" otherwise)
};

struct Dep {
  std::string item;
  int version = 0;
};

struct Version {
  int version = 0;
  std::string created;   // ISO 8601 UTC
  std::string note;      // "what changed"
  std::string uploader;  // API key name (remote) / author (local)
  std::uint64_t size = 0;
  std::string thumbnail;  // blob sha of a small PNG ("" = none)
  std::vector<FileRec> files;
  std::vector<Dep> deps;
};

struct Item {
  std::string id;    // main asset GUID (32 hex) for assets; a fresh GUID for prefabs
  std::string name;
  std::string kind;  // texture | model | audio | font | prefab
  std::string author;
  std::string licence;      // free text / SPDX id ("CC0-1.0", "CC-BY-4.0", "Proprietary"...)
  std::string description;
  std::vector<std::string> tags;
  std::vector<std::string> dims;  // any of "2d", "3d", "vr"
  std::string thumbnail;          // latest version's thumbnail sha
  int latest = 0;
  std::string created, updated;
  std::vector<Version> versions;  // oldest first; may be empty in listings

  const Version* version(int v) const;  // 0 = latest
  const FileRec* main_file(int v = 0) const;
};

struct Query {
  std::string text;  // name / tags / author / description / id prefix
  std::string kind;  // "" = any
  std::string tag;   // "" = any
  std::string dim;   // "" = any ("2d" / "3d" / "vr")
};

bool item_matches(const Item& item, const Query& q);

json::Value to_json(const FileRec& f);
json::Value to_json(const Version& v, bool with_files = true);
json::Value to_json(const Item& item, bool with_versions = true, bool with_files = true);
bool from_json(const json::Value& j, FileRec* out);
bool from_json(const json::Value& j, Version* out);
bool from_json(const json::Value& j, Item* out);

json::Value strings_json(const std::vector<std::string>& v);
std::vector<std::string> json_strings(const json::Value& j);

// Item ids are 32 lowercase hex (asset GUIDs).
bool valid_item_id(const std::string& id);
// Normalised dimension tags ("2D" -> "2d"); unknown ones are dropped.
std::vector<std::string> normalise_dims(const std::vector<std::string>& in);
// "a, b ,c" -> {"a","b","c"} (lowercased, deduplicated).
std::vector<std::string> split_tags(const std::string& s);
std::string join(const std::vector<std::string>& v, const std::string& sep = ", ");
std::string utc_now_iso();
// A path inside a project: relative, no "..", no drive / root.
bool safe_rel_path(const std::string& rel);

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
