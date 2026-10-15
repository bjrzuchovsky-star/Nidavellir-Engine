#pragma once

// Localisation (ts_core, no UI): per-language string tables loaded from a
// project's strings/ folder, one process-wide current language and tr().
//
//   strings/en.json      { "menu.start": "Start", "language.name": "English" }
//   strings/fr.json      { "menu.start": "Commencer", ... }
//   strings/strings.csv  key,en,fr,ja   (RFC 4180 quoting, UTF-8, BOM ok)
//
// JSON and CSV can be mixed; a later file adds to / overrides earlier ones
// (files are read in name order, CSV after JSON). "language.name" is the
// name the language picker shows. tr() falls back: current language ->
// base language (project "en" unless strings/config says otherwise) ->
// the caller's fallback -> the key itself. "{0}".."{9}" are replaced by args.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace loc {

struct Table {
  std::string lang;                           // "en", "fr", "pt-BR"
  std::map<std::string, std::string> strings;  // key -> UTF-8 text
  std::string source;                          // "json" | "csv"
};

class StringTables {
 public:
  // Reads <project>/strings/. Missing folder = no tables (not an error).
  // Parse problems go to *errors (one line each); returns tables loaded.
  int load_project(const std::string& project_dir, std::string* errors = nullptr);
  // Parse one file's text into the tables (tests, the editor).
  bool add_json(const std::string& lang, const std::string& text, std::string* error);
  bool add_csv(const std::string& text, std::string* error);
  // Writes every table back: CSV-sourced languages to strings/strings.csv,
  // the rest to strings/<lang>.json (sorted keys, deterministic).
  bool save_project(const std::string& project_dir, std::string* error) const;

  std::vector<std::string> languages() const;  // base first, then sorted
  bool has_language(const std::string& lang) const;
  void add_language(const std::string& lang, const std::string& source = "json");
  const Table* table(const std::string& lang) const;
  // Every key in any table (sorted).
  std::vector<std::string> keys() const;
  // Keys some language has that `lang` lacks (or has empty).
  std::vector<std::string> missing(const std::string& lang) const;
  const std::string* find(const std::string& lang, const std::string& key) const;
  void set(const std::string& lang, const std::string& key, const std::string& text);
  void remove_key(const std::string& key);
  void clear() { tables_.clear(); }
  bool empty() const { return tables_.empty(); }

  std::string base = "en";

 private:
  std::vector<Table> tables_;
};

// Parsers (exposed for tests).
bool parse_flat_json(const std::string& text, std::map<std::string, std::string>* out,
                     std::string* error);
std::string write_flat_json(const std::map<std::string, std::string>& strings);
bool parse_csv(const std::string& text, std::vector<std::vector<std::string>>* rows,
               std::string* error);
std::string csv_field(const std::string& s);

// --- process-wide current language ---------------------------------------

// Loads the project's tables and picks the language: $TS_LANG, else the
// player's saved choice (TombStoneConfig/language.txt), else the base.
void load_project(const std::string& project_dir);
// Replace the tables directly (tests, editor preview).
void set_tables(StringTables tables);
const StringTables& tables();
bool set_language(const std::string& lang);  // false when unknown
const std::string& language();
std::vector<std::string> languages();
// "language.name" of lang (else the code).
std::string display_name(const std::string& lang);
// Saves the current language as the player's choice.
bool save_language_choice(std::string* error = nullptr);

std::string tr(const std::string& key, const std::string& fallback = std::string());
std::string format(const std::string& text, const std::vector<std::string>& args);
// Bumps whenever the tables or the language change (UI re-binds text keys).
std::uint64_t version();

// Every string of `lang` (font atlases bake the glyphs it needs).
std::string all_text(const std::string& lang);
// Fonts to fall back on for glyphs the bundled faces lack: the project's
// fonts/*.ttf|otf|ttc (sorted), then common OS fonts that exist.
std::vector<std::string> fallback_fonts(const std::string& project_dir);
const std::string& project_dir();

// UTF-8 -> code points (invalid bytes become U+FFFD).
std::vector<std::uint32_t> utf8_decode(const std::string& s);

}  // namespace loc
}  // namespace tombstone
}  // namespace ts
