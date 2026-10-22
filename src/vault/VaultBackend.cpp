#include "vault/VaultBackend.h"

#include "vault/Http.h"
#include "vault/Sha256.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace ts {
namespace tombstone {
namespace vault {

namespace fs = std::filesystem;
using json::Value;

std::string default_local_library() {
  if (const char* env = std::getenv("TS_VAULT_DIR")) {
    if (*env) return env;
  }
  fs::path docs;
#if defined(_WIN32)
  PWSTR p = nullptr;
  if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p) == S_OK && p) docs = fs::path(p);
  if (p) CoTaskMemFree(p);
  if (docs.empty()) {
    if (const char* up = std::getenv("USERPROFILE")) docs = fs::path(up) / "Documents";
  }
#else
  if (const char* xdg = std::getenv("XDG_DOCUMENTS_DIR")) docs = xdg;
  if (docs.empty()) {
    if (const char* home = std::getenv("HOME")) docs = fs::path(home) / "Documents";
  }
#endif
  if (docs.empty()) docs = fs::current_path();
  return (docs / "TombStone" / "Vault").string();
}

std::unique_ptr<Backend> make_backend(const std::string& backend, const std::string& local_dir,
                                      const std::string& url, const std::string& api_key) {
  if (backend == "remote") return std::make_unique<RemoteBackend>(url, api_key);
  return std::make_unique<LocalBackend>(local_dir.empty() ? default_local_library() : local_dir);
}

// ---- local -------------------------------------------------------------------------

bool LocalBackend::ensure(std::string* error) {
  if (store_.is_open()) return true;
  return store_.open(dir_, error);
}

bool LocalBackend::ping(std::string* info, std::string* error) {
  if (!ensure(error)) return false;
  std::vector<Item> items;
  store_.list({}, &items, nullptr);
  if (info) *info = "local library, " + std::to_string(items.size()) + " item(s) in " + store_.root();
  return true;
}

bool LocalBackend::list(const Query& q, std::vector<Item>* out, std::string* error) {
  return ensure(error) && store_.list(q, out, error);
}

bool LocalBackend::get(const std::string& id, Item* out, std::string* error) {
  return ensure(error) && store_.get(id, out, error);
}

bool LocalBackend::missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
                           std::string* error) {
  if (!ensure(error)) return false;
  *out = store_.missing_blobs(shas);
  return true;
}

bool LocalBackend::upload(const std::string& path, const std::string& sha, std::uint64_t,
                          std::string* error) {
  return ensure(error) && store_.put_blob_file(path, sha, nullptr, error);
}

bool LocalBackend::download(const std::string& sha, const std::string& dest, std::string* error) {
  if (!ensure(error)) return false;
  if (!store_.has_blob(sha)) {
    if (error) *error = "blob " + sha + " is not in the library";
    return false;
  }
  std::error_code ec;
  const fs::path d = fs::u8path(dest);
  if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
  const fs::path tmp = d.string() + ".vault-tmp";
  fs::copy_file(fs::u8path(store_.blob_path(sha)), tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error) *error = "cannot write " + tmp.string() + ": " + ec.message();
    return false;
  }
  if (sha256_file(tmp.string()) != sha) {
    fs::remove(tmp, ec);
    if (error) *error = "library blob " + sha + " is damaged (hash mismatch)";
    return false;
  }
  fs::rename(tmp, d, ec);
  if (ec) {
    fs::remove(d, ec);
    fs::rename(tmp, d, ec);
  }
  if (ec) {
    if (error) *error = "cannot write " + dest + ": " + ec.message();
    return false;
  }
  return true;
}

bool LocalBackend::publish(const Item& meta, const Version& v, PublishResult* out, std::string* error) {
  return ensure(error) && store_.publish(meta, v, out, error);
}

// ---- remote ------------------------------------------------------------------------

RemoteBackend::RemoteBackend(std::string url, std::string api_key, RemoteOptions opt)
    : url_(std::move(url)), key_(std::move(api_key)), opt_(opt) {
  while (!url_.empty() && url_.back() == '/') url_.pop_back();
}

bool RemoteBackend::call(const std::string& method, const std::string& path, const std::string& body,
                         const std::string& content_type, int* status, Value* out, std::string* error) {
  Headers h;
  if (!key_.empty()) h["Authorization"] = "Bearer " + key_;
  h["Accept"] = "application/json";
  if (!content_type.empty()) h["Content-Type"] = content_type;
  ClientResult r;
  ClientOptions co;
  co.timeout_ms = opt_.timeout_ms;
  std::string err;
  if (!http_request(method, url_ + path, h, body, &r, &err, co)) {
    if (error) *error = err;
    return false;
  }
  *status = r.status;
  Value v;
  std::string perr;
  if (!r.body.empty() && json::parse(r.body, &v, &perr)) *out = v;
  else *out = Value::object();
  if (r.status == 401) {
    if (error) *error = "the Vault server rejected the API key (401): check it in the Vault settings";
    return false;
  }
  if (r.status == 403) {
    if (error) *error = "this API key may not change the library (403, read-only key)";
    return false;
  }
  if (r.status >= 400 && r.status != 409) {
    if (error) *error = "server: HTTP " + std::to_string(r.status) + (out->str("error").empty() ? "" : " " + out->str("error"));
    return false;
  }
  return true;
}

bool RemoteBackend::ping(std::string* info, std::string* error) {
  int st = 0;
  Value v;
  if (!call("GET", "/api/v1/health", "", "", &st, &v, error)) return false;
  if (v.str("server") != "ts_vault") {
    if (error) *error = url_ + " does not look like a ts_vault server";
    return false;
  }
  Value who;
  if (!call("GET", "/api/v1/whoami", "", "", &st, &who, error)) return false;
  if (info) {
    *info = "ts_vault " + v.str("version") + " at " + url_ + ", key '" + who.str("name") + "'" +
            (who.flag("can_write", false) ? "" : " (read-only)");
  }
  return true;
}

bool RemoteBackend::list(const Query& q, std::vector<Item>* out, std::string* error) {
  std::string qs = "?q=" + url_encode(q.text);
  if (!q.kind.empty()) qs += "&kind=" + url_encode(q.kind);
  if (!q.tag.empty()) qs += "&tag=" + url_encode(q.tag);
  if (!q.dim.empty()) qs += "&dim=" + url_encode(q.dim);
  int st = 0;
  Value v;
  if (!call("GET", "/api/v1/items" + qs, "", "", &st, &v, error)) return false;
  out->clear();
  for (const Value& j : v["items"].items()) {
    Item it;
    if (from_json(j, &it)) out->push_back(std::move(it));
  }
  return true;
}

bool RemoteBackend::get(const std::string& id, Item* out, std::string* error) {
  int st = 0;
  Value v;
  if (!call("GET", "/api/v1/items/" + url_encode(id), "", "", &st, &v, error)) return false;
  return from_json(v, out);
}

bool RemoteBackend::missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
                            std::string* error) {
  Value body = Value::object();
  body.set("sha256", strings_json(shas));
  int st = 0;
  Value v;
  if (!call("POST", "/api/v1/blobs/missing", json::write(body), "application/json", &st, &v, error)) return false;
  *out = json_strings(v["missing"]);
  return true;
}

bool RemoteBackend::upload(const std::string& path, const std::string& sha, std::uint64_t size,
                           std::string* error) {
  Value begin = Value::object();
  begin.set("sha256", Value::string(sha));
  begin.set("size", Value::number(static_cast<double>(size)));
  int st = 0;
  Value u;
  if (!call("POST", "/api/v1/uploads", json::write(begin), "application/json", &st, &u, error)) return false;
  if (u.flag("complete", false)) return true;
  const std::string id = u.str("id");
  std::uint64_t offset = static_cast<std::uint64_t>(u.num("received", 0));
  if (id.empty()) {
    if (error) *error = "server did not open an upload";
    return false;
  }
  if (offset > 0) ++stats_.resumed;
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  int sent_chunks = 0;
  std::string chunk;
  while (offset < size) {
    if (opt_.abort_after_chunks > 0 && sent_chunks >= opt_.abort_after_chunks) {
      if (error) *error = "upload interrupted (test hook) at " + std::to_string(offset) + " of " + std::to_string(size) + " bytes";
      return false;
    }
    const std::uint64_t n = std::min<std::uint64_t>(opt_.chunk_size, size - offset);
    chunk.resize(static_cast<std::size_t>(n));
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(chunk.data(), static_cast<std::streamsize>(n));
    if (static_cast<std::uint64_t>(in.gcount()) != n) {
      if (error) *error = path + " changed while uploading";
      return false;
    }
    Value r;
    if (!call("PUT", "/api/v1/uploads/" + id + "?offset=" + std::to_string(offset), chunk,
              "application/octet-stream", &st, &r, error)) {
      return false;
    }
    if (st == 409) {
      // The server has a different amount (an earlier attempt landed): continue from there.
      offset = static_cast<std::uint64_t>(r.num("received", 0));
      continue;
    }
    stats_.bytes_uploaded += n;
    ++stats_.chunks;
    ++sent_chunks;
    offset = static_cast<std::uint64_t>(r.num("received", static_cast<double>(offset + n)));
  }
  Value done;
  if (!call("POST", "/api/v1/uploads/" + id + "/complete", "", "application/json", &st, &done, error)) return false;
  if (st != 200) {
    if (error) *error = "upload of " + path + " did not complete (HTTP " + std::to_string(st) + ")";
    return false;
  }
  return true;
}

bool RemoteBackend::download(const std::string& sha, const std::string& dest, std::string* error) {
  std::error_code ec;
  const fs::path d = fs::u8path(dest);
  if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
  const fs::path part = d.string() + ".vault-part";
  // Resume a partial download of the same blob.
  std::uint64_t have = 0;
  {
    const fs::path tag = d.string() + ".vault-part.sha";
    std::ifstream t(tag);
    std::string prev;
    t >> prev;
    if (prev == sha && fs::exists(part, ec)) have = static_cast<std::uint64_t>(fs::file_size(part, ec));
    else fs::remove(part, ec);
    std::ofstream(tag, std::ios::trunc) << sha;
  }
  std::ofstream out(part, std::ios::binary | (have > 0 ? std::ios::app : std::ios::trunc));
  if (!out) {
    if (error) *error = "cannot write " + part.string();
    return false;
  }
  Headers h;
  if (!key_.empty()) h["Authorization"] = "Bearer " + key_;
  if (have > 0) h["Range"] = "bytes=" + std::to_string(have) + "-";
  ClientOptions co;
  co.timeout_ms = opt_.timeout_ms;
  bool restart = false;
  co.sink = [&](const char* p, std::size_t n) {
    out.write(p, static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
  };
  ClientResult r;
  std::string err;
  const bool ok = http_request("GET", url_ + "/api/v1/blobs/" + sha, h, "", &r, &err, co);
  out.close();
  stats_.bytes_downloaded += r.bytes_received;
  if (ok && have > 0 && r.status == 200) restart = true;  // server ignored the range
  if (!ok || (r.status != 200 && r.status != 206) || restart) {
    if (r.status == 401 && error) *error = "the Vault server rejected the API key (401)";
    else if (error) *error = err.empty() ? "download of " + sha + " failed (HTTP " + std::to_string(r.status) + ")" : err;
    if (r.status == 416 || restart || (r.status >= 400)) fs::remove(part, ec);
    return false;
  }
  if (sha256_file(part.string()) != sha) {
    fs::remove(part, ec);
    if (error) *error = "downloaded content does not match " + sha;
    return false;
  }
  fs::remove(d.string() + ".vault-part.sha", ec);
  fs::rename(part, d, ec);
  if (ec) {
    fs::remove(d, ec);
    fs::rename(part, d, ec);
  }
  if (ec) {
    if (error) *error = "cannot write " + dest + ": " + ec.message();
    return false;
  }
  return true;
}

bool RemoteBackend::publish(const Item& meta, const Version& v, PublishResult* out, std::string* error) {
  Value body = Value::object();
  body.set("item", to_json(meta, false, false));
  body.set("version", to_json(v, true));
  int st = 0;
  Value r;
  if (!call("POST", "/api/v1/items/" + url_encode(meta.id) + "/versions", json::write(body), "application/json",
            &st, &r, error)) {
    return false;
  }
  out->version = static_cast<int>(r.num("version", 0));
  out->created = r.flag("created", false);
  return out->version > 0;
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
