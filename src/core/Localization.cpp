#include "core/Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace loc {

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void put_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string strip_bom(const std::string& s) {
  return s.rfind("\xEF\xBB\xBF", 0) == 0 ? s.substr(3) : s;
}

std::string escape_json(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

}  // namespace

std::vector<std::uint32_t> utf8_decode(const std::string& s) {
  std::vector<std::uint32_t> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(c);
      ++i;
      continue;
    }
    const int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    std::uint32_t cp = n == 1 ? (c & 0x1Fu) : n == 2 ? (c & 0x0Fu) : (c & 0x07u);
    bool ok = n > 0 && i + static_cast<std::size_t>(n) < s.size() + 0;
    for (int k = 1; ok && k <= n; ++k) {
      if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += static_cast<std::size_t>(n) + 1;
  }
  return out;
}

// --- parsers ----------------------------------------------------------------

bool parse_flat_json(const std::string& text_in, std::map<std::string, std::string>* out,
                     std::string* error) {
  const std::string t = strip_bom(text_in);
  std::size_t i = 0;
  auto ws = [&] {
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
  };
  auto fail = [&](const std::string& why) {
    if (error) *error = why + " at byte " + std::to_string(i);
    return false;
  };
  auto str = [&](std::string* s) {
    ws();
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    s->clear();
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c != '\\') {
        *s += c;
        continue;
      }
      if (i >= t.size()) return false;
      c = t[i++];
      switch (c) {
        case 'n': *s += '\n'; break;
        case 't': *s += '\t'; break;
        case 'r': *s += '\r'; break;
        case 'b': *s += '\b'; break;
        case 'f': *s += '\f'; break;
        case 'u': {
          if (i + 4 > t.size()) return false;
          std::uint32_t cp = static_cast<std::uint32_t>(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            const std::uint32_t lo = static_cast<std::uint32_t>(std::strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16));
            i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(*s, cp);
          break;
        }
        default: *s += c;
      }
    }
    if (i >= t.size()) return false;
    ++i;
    return true;
  };
  ws();
  if (i >= t.size() || t[i] != '{') return fail("expected {");
  ++i;
  ws();
  if (i < t.size() && t[i] == '}') return true;
  while (true) {
    std::string k, v;
    if (!str(&k)) return fail("expected a \"key\"");
    ws();
    if (i >= t.size() || t[i] != ':') return fail("expected :");
    ++i;
    if (!str(&v)) return fail("expected a \"text\" value for " + k);
    (*out)[k] = v;
    ws();
    if (i < t.size() && t[i] == ',') {
      ++i;
      continue;
    }
    if (i < t.size() && t[i] == '}') return true;
    return fail("expected , or }");
  }
}

std::string write_flat_json(const std::map<std::string, std::string>& strings) {
  std::string o = "{\n";
  std::size_t n = 0;
  for (const auto& [k, v] : strings) {
    o += "  \"" + escape_json(k) + "\": \"" + escape_json(v) + "\"";
    o += ++n < strings.size() ? ",\n" : "\n";
  }
  return o + "}\n";
}

bool parse_csv(const std::string& text_in, std::vector<std::vector<std::string>>* rows,
               std::string* error) {
  const std::string t = strip_bom(text_in);
  std::vector<std::string> row;
  std::string field;
  bool quoted = false, any = false;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < t.size() && t[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        field += c;
      }
      continue;
    }
    if (c == '"') {
      quoted = true;
      any = true;
    } else if (c == ',') {
      row.push_back(field);
      field.clear();
      any = true;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
      if (any || !field.empty()) {
        row.push_back(field);
        rows->push_back(row);
      }
      row.clear();
      field.clear();
      any = false;
    } else {
      field += c;
      any = true;
    }
  }
  if (quoted) {
    if (error) *error = "unterminated quote";
    return false;
  }
  if (any || !field.empty()) {
    row.push_back(field);
    rows->push_back(row);
  }
  return true;
}

std::string csv_field(const std::string& s) {
  if (s.find_first_of(",\"\r\n") == std::string::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) return s;
  std::string o = "\"";
  for (char c : s) {
    if (c == '"') o += '"';
    o += c;
  }
  return o + "\"";
}

// --- tables -----------------------------------------------------------------

bool StringTables::add_json(const std::string& lang, const std::string& text, std::string* error) {
  std::map<std::string, std::string> m;
  if (!parse_flat_json(text, &m, error)) return false;
  add_language(lang, "json");
  for (auto& [k, v] : m) set(lang, k, v);
  return true;
}

bool StringTables::add_csv(const std::string& text, std::string* error) {
  std::vector<std::vector<std::string>> rows;
  if (!parse_csv(text, &rows, error)) return false;
  if (rows.empty()) return true;
  const std::vector<std::string>& head = rows[0];
  if (head.size() < 2 || (head[0] != "key" && head[0] != "id")) {
    if (error) *error = "first row must be: key,<lang>,<lang>...";
    return false;
  }
  for (std::size_t c = 1; c < head.size(); ++c) {
    if (!head[c].empty()) add_language(head[c], "csv");
  }
  for (std::size_t r = 1; r < rows.size(); ++r) {
    const std::vector<std::string>& row = rows[r];
    if (row.empty() || row[0].empty() || row[0][0] == '#') continue;
    for (std::size_t c = 1; c < head.size() && c < row.size(); ++c) {
      if (!head[c].empty() && !row[c].empty()) set(head[c], row[0], row[c]);
    }
  }
  return true;
}

int StringTables::load_project(const std::string& project_dir, std::string* errors) {
  tables_.clear();
  std::error_code ec;
  const fs::path dir = fs::path(project_dir) / "strings";
  if (!fs::is_directory(dir, ec)) return 0;
  if (fs::is_regular_file(dir / "config.json", ec)) {
    std::map<std::string, std::string> cfg;
    if (parse_flat_json(read_all(dir / "config.json"), &cfg, nullptr) && cfg.count("base")) base = cfg["base"];
  }
  std::vector<fs::path> json, csv;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string ext = e.path().extension().string();
    if (ext == ".json" && e.path().stem() != "config") json.push_back(e.path());
    if (ext == ".csv") csv.push_back(e.path());
  }
  std::sort(json.begin(), json.end());
  std::sort(csv.begin(), csv.end());
  for (const fs::path& p : json) {
    std::string err;
    if (!add_json(p.stem().string(), read_all(p), &err) && errors) {
      *errors += "strings/" + p.filename().string() + ": " + err + "\n";
    }
  }
  for (const fs::path& p : csv) {
    std::string err;
    if (!add_csv(read_all(p), &err) && errors) *errors += "strings/" + p.filename().string() + ": " + err + "\n";
  }
  return static_cast<int>(tables_.size());
}

bool StringTables::save_project(const std::string& project_dir, std::string* error) const {
  std::error_code ec;
  const fs::path dir = fs::path(project_dir) / "strings";
  fs::create_directories(dir, ec);
  std::vector<const Table*> csv;
  for (const Table& t : tables_) {
    if (t.source == "csv") {
      csv.push_back(&t);
      continue;
    }
    std::ofstream o(dir / (t.lang + ".json"), std::ios::binary | std::ios::trunc);
    o << write_flat_json(t.strings);
    if (!o) {
      if (error) *error = "cannot write strings/" + t.lang + ".json";
      return false;
    }
  }
  if (!csv.empty()) {
    std::set<std::string> keys;
    for (const Table* t : csv) {
      for (const auto& [k, v] : t->strings) keys.insert(k);
    }
    std::string out = "key";
    for (const Table* t : csv) out += "," + csv_field(t->lang);
    out += "\r\n";
    for (const std::string& k : keys) {
      out += csv_field(k);
      for (const Table* t : csv) {
        const auto it = t->strings.find(k);
        out += "," + csv_field(it == t->strings.end() ? std::string() : it->second);
      }
      out += "\r\n";
    }
    std::ofstream o(dir / "strings.csv", std::ios::binary | std::ios::trunc);
    o << out;
    if (!o) {
      if (error) *error = "cannot write strings/strings.csv";
      return false;
    }
  }
  return true;
}

std::vector<std::string> StringTables::languages() const {
  std::vector<std::string> out;
  for (const Table& t : tables_) out.push_back(t.lang);
  std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) {
    if ((a == base) != (b == base)) return a == base;
    return a < b;
  });
  return out;
}

bool StringTables::has_language(const std::string& lang) const { return table(lang) != nullptr; }

void StringTables::add_language(const std::string& lang, const std::string& source) {
  if (lang.empty() || has_language(lang)) return;
  Table t;
  t.lang = lang;
  t.source = source;
  tables_.push_back(std::move(t));
}

const Table* StringTables::table(const std::string& lang) const {
  for (const Table& t : tables_) {
    if (t.lang == lang) return &t;
  }
  return nullptr;
}

std::vector<std::string> StringTables::keys() const {
  std::set<std::string> k;
  for (const Table& t : tables_) {
    for (const auto& [key, v] : t.strings) k.insert(key);
  }
  return {k.begin(), k.end()};
}

std::vector<std::string> StringTables::missing(const std::string& lang) const {
  std::vector<std::string> out;
  for (const std::string& k : keys()) {
    const std::string* s = find(lang, k);
    if (!s || s->empty()) out.push_back(k);
  }
  return out;
}

const std::string* StringTables::find(const std::string& lang, const std::string& key) const {
  const Table* t = table(lang);
  if (!t) return nullptr;
  const auto it = t->strings.find(key);
  return it == t->strings.end() ? nullptr : &it->second;
}

void StringTables::set(const std::string& lang, const std::string& key, const std::string& text) {
  add_language(lang);
  for (Table& t : tables_) {
    if (t.lang != lang) continue;
    if (text.empty()) t.strings.erase(key);
    else t.strings[key] = text;
  }
}

void StringTables::remove_key(const std::string& key) {
  for (Table& t : tables_) t.strings.erase(key);
}

// --- current language -------------------------------------------------------

namespace {
struct State {
  StringTables tables;
  std::string lang = "en";
  std::string project;
  std::uint64_t version = 1;
};
State& st() {
  static State s;
  return s;
}
fs::path choice_path() { return fs::path("TombStoneConfig") / "language.txt"; }
}  // namespace

void load_project(const std::string& project_dir) {
  State& s = st();
  const std::string keep = s.project == project_dir ? s.lang : std::string();
  s.project = project_dir;
  std::string errors;
  s.tables.load_project(project_dir, &errors);
  if (!errors.empty()) std::fprintf(stderr, "[loc] %s", errors.c_str());
  std::string want = keep;
  if (const char* e = std::getenv("TS_LANG"); e && keep.empty()) want = e;
  if (want.empty()) {
    std::string saved = read_all(choice_path());
    while (!saved.empty() && std::isspace(static_cast<unsigned char>(saved.back()))) saved.pop_back();
    want = saved;
  }
  if (want.empty() || !s.tables.has_language(want)) want = s.tables.base;
  s.lang = want;
  ++s.version;
}

void set_tables(StringTables tables) {
  State& s = st();
  s.tables = std::move(tables);
  if (!s.tables.has_language(s.lang)) s.lang = s.tables.base;
  ++s.version;
}

const StringTables& tables() { return st().tables; }

bool set_language(const std::string& lang) {
  State& s = st();
  if (!s.tables.has_language(lang)) return false;
  if (s.lang != lang) {
    s.lang = lang;
    ++s.version;
  }
  return true;
}

const std::string& language() { return st().lang; }
std::vector<std::string> languages() { return st().tables.languages(); }

std::string display_name(const std::string& lang) {
  const std::string* n = st().tables.find(lang, "language.name");
  return n && !n->empty() ? *n : lang;
}

bool save_language_choice(std::string* error) {
  std::error_code ec;
  fs::create_directories(choice_path().parent_path(), ec);
  std::ofstream o(choice_path(), std::ios::trunc);
  o << st().lang << '\n';
  if (!o && error) *error = "cannot write " + choice_path().string();
  return static_cast<bool>(o);
}

std::string format(const std::string& text, const std::vector<std::string>& args) {
  if (args.empty()) return text;
  std::string o;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{' && i + 2 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
        text[i + 2] == '}') {
      const std::size_t n = static_cast<std::size_t>(text[i + 1] - '0');
      if (n < args.size()) {
        o += args[n];
        i += 2;
        continue;
      }
    }
    o += text[i];
  }
  return o;
}

std::string tr(const std::string& key, const std::string& fallback) {
  const State& s = st();
  if (const std::string* t = s.tables.find(s.lang, key); t && !t->empty()) return *t;
  if (const std::string* t = s.tables.find(s.tables.base, key); t && !t->empty()) return *t;
  return fallback.empty() ? key : fallback;
}

std::uint64_t version() { return st().version; }

std::string all_text(const std::string& lang) {
  std::string out;
  if (const Table* t = st().tables.table(lang)) {
    for (const auto& [k, v] : t->strings) out += v;
  }
  for (const std::string& l : st().tables.languages()) out += display_name(l);
  return out;
}

const std::string& project_dir() { return st().project; }

std::vector<std::string> fallback_fonts(const std::string& project) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path fd = fs::path(project) / "fonts";
  if (!project.empty() && fs::is_directory(fd, ec)) {
    std::vector<std::string> mine;
    for (const auto& e : fs::directory_iterator(fd, ec)) {
      std::string ext = e.path().extension().string();
      for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") mine.push_back(e.path().string());
    }
    std::sort(mine.begin(), mine.end());
    out.insert(out.end(), mine.begin(), mine.end());
  }
#if defined(_WIN32)
  std::string win = "C:\\Windows";
  if (const char* r = std::getenv("SystemRoot")) win = r;
  const fs::path f = fs::path(win) / "Fonts";
  const char* sys[] = {"segoeui.ttf", "arial.ttf",   "msyh.ttc",    "YuGothM.ttc", "meiryo.ttc",
                       "msgothic.ttc", "malgun.ttf", "simsun.ttc",  "Nirmala.ttf", "LeelawUI.ttf",
                       "seguisym.ttf"};
#else
  const fs::path f = "/usr/share/fonts";
  const char* sys[] = {"truetype/dejavu/DejaVuSans.ttf", "truetype/noto/NotoSans-Regular.ttf",
                       "opentype/noto/NotoSansCJK-Regular.ttc", "truetype/droid/DroidSansFallbackFull.ttf",
                       "truetype/wqy/wqy-microhei.ttc", "noto-cjk/NotoSansCJK-Regular.ttc"};
#endif
  for (const char* s : sys) {
    if (fs::is_regular_file(f / s, ec)) out.push_back((f / s).string());
  }
  return out;
}

}  // namespace loc
}  // namespace tombstone
}  // namespace ts
