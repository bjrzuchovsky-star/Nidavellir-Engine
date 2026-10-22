#pragma once

// Provenance of imported third-party content. Assets that came from Source
// engine formats (.mdl / .vtf / .vmt and their siblings) or were extracted
// from a .gma addon are tagged in the claim's ledger
// (<claim>/.tombstone/provenance.json, editor-only: never shipped):
//
//   {"provenance": 1,
//    "assets": {"assets/Models/Crate/models/crate.mdl":
//                {"origin": "gma:Crate Pack", "format": "source-engine",
//                 "author": "someone", "addon": "Crate Pack",
//                 "source": "D:/Downloads/crate.gma", "imported": "2026-10-22T23:58:00"}},
//    "acknowledged": {"assets/...": "2026-10-23T00:10:00"},
//    "acknowledged_all": "" }
//
// Ship It (validate_project / export) lists tagged files that would ship
// as a warning - "you need the rights to ship these" - until the user
// acknowledges them per asset or for the whole claim ("I have the
// rights"). Never a hard block. Files with Source extensions that carry
// no tag (copied in by hand, moved) still count, as origin
// "source-engine".

#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace exporter {
namespace provenance {

inline constexpr const char* kLedgerFile = ".tombstone/provenance.json";

struct Record {
  std::string origin;    // "source-engine" | "gma:<addon name>"
  std::string format;    // "source-engine"
  std::string author;    // .gma author string
  std::string addon;     // .gma addon name
  std::string source;    // file it was imported / extracted from
  std::string imported;  // local time stamp
};

struct Ledger {
  std::map<std::string, Record> assets;             // project-relative path -> record
  std::map<std::string, std::string> acknowledged;  // path -> when
  std::string acknowledged_all;                     // when ("" = no)
};

bool load(const std::string& claim_dir, Ledger* out);
bool save(const std::string& claim_dir, const Ledger& ledger, std::string* error = nullptr);
// Adds / replaces records (load + merge + save).
bool tag(const std::string& claim_dir, const std::map<std::string, Record>& records,
         std::string* error = nullptr);
// Per asset (`rel` non-empty) or the whole claim (`rel` empty).
bool acknowledge(const std::string& claim_dir, const std::string& rel, bool on,
                 std::string* error = nullptr);

// .mdl .vvd .vtx .phy .ani .vtf .vmt (Source engine formats).
bool is_source_format(const std::string& rel);

struct Shipped {
  std::string path;
  std::string origin;
  std::string author;
  bool acknowledged = false;
};
// Tagged / Source-format files among `content` (project-relative paths,
// e.g. exporter::content_files) - what would ship.
std::vector<Shipped> shipped_third_party(const std::string& claim_dir, const std::vector<std::string>& content);

std::string now_stamp();

}  // namespace provenance
}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
