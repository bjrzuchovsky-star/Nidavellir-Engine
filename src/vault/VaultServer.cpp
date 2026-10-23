#include "vault/VaultServer.h"

#include "vault/Sha256.h"

#include <cstdlib>
#include <filesystem>

#ifndef TS_VAULT_VERSION
#define TS_VAULT_VERSION "0.0.0"
#endif

namespace ts {
namespace tombstone {
namespace vault {

using json::Value;

namespace {

void reply(HttpResponse* res, int status, const Value& v) {
  res->status = status;
  res->content_type = "application/json";
  res->body = json::write(v);
}

void error_reply(HttpResponse* res, int status, const std::string& msg) {
  Value v = Value::object();
  v.set("error", Value::string(msg));
  reply(res, status, v);
}

std::vector<std::string> segments(const std::string& path) {
  std::vector<std::string> out;
  std::size_t start = 1;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!seg.empty()) out.push_back(seg);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return out;
}

Value upload_json(const UploadSession& u) {
  Value v = Value::object();
  v.set("id", Value::string(u.id));
  v.set("sha256", Value::string(u.sha256));
  v.set("size", Value::number(static_cast<double>(u.size)));
  v.set("received", Value::number(static_cast<double>(u.received)));
  v.set("complete", Value::boolean(u.complete));
  return v;
}

}  // namespace

bool VaultServer::start(const ServerOptions& opt, std::string* error) {
  log_ = opt.log;
  return http_.start(opt, [this](const HttpRequest& rq, HttpResponse* rs) { handle(rq, rs); }, error);
}

void VaultServer::handle(const HttpRequest& req, HttpResponse* res) {
  const std::vector<std::string> seg = segments(req.path);
  if (seg.size() < 2 || seg[0] != "api" || seg[1] != "v1") {
    if (req.path == "/" && req.method == "GET") {
      res->content_type = "text/plain";
      res->body = "ts_vault " TS_VAULT_VERSION " - TombStone asset library server. API under /api/v1.\n";
      return;
    }
    return error_reply(res, 404, "not found");
  }
  const std::vector<std::string> r(seg.begin() + 2, seg.end());
  if (r.size() == 1 && r[0] == "health" && req.method == "GET") {
    Value v = Value::object();
    v.set("ok", Value::boolean(true));
    v.set("server", Value::string("ts_vault"));
    v.set("version", Value::string(TS_VAULT_VERSION));
    return reply(res, 200, v);
  }
  // Auth.
  std::string key = req.header("x-api-key");
  const std::string auth = req.header("authorization");
  if (key.empty() && auth.size() > 7 && (auth.compare(0, 7, "Bearer ") == 0 || auth.compare(0, 7, "bearer ") == 0)) {
    key = auth.substr(7);
  }
  KeyInfo who;
  if (!store_.check_key(key, &who)) {
    if (log_) log_(req.peer + " rejected: " + (key.empty() ? "no API key" : "unknown API key"));
    res->headers["WWW-Authenticate"] = "Bearer realm=\"ts_vault\"";
    return error_reply(res, 401, "missing or unknown API key");
  }
  auto need_write = [&]() {
    if (who.can_write) return true;
    error_reply(res, 403, "this API key is read-only");
    return false;
  };
  auto parse_body = [&](Value* out) {
    std::string err;
    if (!json::parse(req.body, out, &err) || !out->is_object()) {
      error_reply(res, 400, "body must be a JSON object: " + err);
      return false;
    }
    return true;
  };
  std::string err;
  if (r.size() == 1 && r[0] == "whoami" && req.method == "GET") {
    Value v = Value::object();
    v.set("name", Value::string(who.name));
    v.set("can_write", Value::boolean(who.can_write));
    return reply(res, 200, v);
  }
  if (r.size() == 1 && r[0] == "team" && req.method == "GET") {
    if (team_info_) {
      res->status = 200;
      res->content_type = "application/json";
      res->body = team_info_();
      return;
    }
    Value v = Value::object();
    v.set("enabled", Value::boolean(false));
    return reply(res, 200, v);
  }
  if (!r.empty() && r[0] == "items") {
    if (r.size() == 1 && req.method == "GET") {
      Query q;
      auto qv = [&](const char* k) {
        const auto it = req.query.find(k);
        return it == req.query.end() ? std::string() : it->second;
      };
      q.text = qv("q");
      q.kind = qv("kind");
      q.tag = qv("tag");
      q.dim = qv("dim");
      std::vector<Item> items;
      if (!store_.list(q, &items, &err)) return error_reply(res, 500, err);
      Value arr = Value::array();
      for (const Item& it : items) arr.push(to_json(it, true, false));
      Value v = Value::object();
      v.set("items", arr);
      return reply(res, 200, v);
    }
    if (r.size() == 2 && req.method == "GET") {
      Item it;
      if (!store_.get(r[1], &it, &err)) return error_reply(res, 404, err);
      return reply(res, 200, to_json(it, true, true));
    }
    if (r.size() == 2 && req.method == "PATCH") {
      if (!need_write()) return;
      Value body;
      if (!parse_body(&body)) return;
      Item meta;
      from_json(body, &meta);
      meta.id = r[1];
      Item after;
      if (!store_.update_item(meta, &after, &err)) {
        return error_reply(res, err.rfind("no such item", 0) == 0 ? 404 : 422, err);
      }
      if (log_) log_("edited " + meta.id + " by " + who.name);
      return reply(res, 200, to_json(after, true, true));
    }
    if ((r.size() == 2 || (r.size() == 4 && r[2] == "versions")) && req.method == "DELETE") {
      if (!need_write()) return;
      bool conflict = false;
      const int ver = r.size() == 4 ? std::atoi(r[3].c_str()) : 0;
      if (r.size() == 4 && ver <= 0) return error_reply(res, 400, "bad version number");
      const bool ok = r.size() == 2 ? store_.remove_item(r[1], &conflict, &err)
                                    : store_.remove_version(r[1], ver, &conflict, &err);
      if (!ok) return error_reply(res, conflict ? 409 : (err.rfind("no such item", 0) == 0 ? 404 : 422), err);
      if (log_) log_("deleted " + r[1] + (ver ? " v" + std::to_string(ver) : std::string()) + " by " + who.name);
      Value out = Value::object();
      out.set("ok", Value::boolean(true));
      return reply(res, 200, out);
    }
    if (r.size() == 3 && r[2] == "versions" && req.method == "POST") {
      if (!need_write()) return;
      Value body;
      if (!parse_body(&body)) return;
      Item meta;
      from_json(body["item"], &meta);
      meta.id = r[1];
      Version v;
      from_json(body["version"], &v);
      v.uploader = who.name;
      PublishResult pr;
      if (!store_.publish(meta, v, &pr, &err)) return error_reply(res, 422, err);
      if (log_) log_("published " + meta.id + " v" + std::to_string(pr.version) + (pr.created ? "" : " (unchanged)") + " by " + who.name);
      Value out = Value::object();
      out.set("version", Value::number(pr.version));
      out.set("created", Value::boolean(pr.created));
      return reply(res, 200, out);
    }
    return error_reply(res, 405, "method not allowed");
  }
  if (!r.empty() && r[0] == "blobs") {
    if (r.size() == 2 && r[1] == "missing" && req.method == "POST") {
      Value body;
      if (!parse_body(&body)) return;
      Value v = Value::object();
      v.set("missing", strings_json(store_.missing_blobs(json_strings(body["sha256"]))));
      return reply(res, 200, v);
    }
    if (r.size() == 2 && (req.method == "GET" || req.method == "HEAD")) {
      if (!is_sha256_hex(r[1]) || !store_.has_blob(r[1])) return error_reply(res, 404, "no such blob");
      const std::uint64_t size = store_.blob_size(r[1]);
      std::uint64_t from = 0;
      const std::string range = req.header("range");
      if (range.rfind("bytes=", 0) == 0) {
        from = std::strtoull(range.c_str() + 6, nullptr, 10);
        if (from > size) {
          res->headers["Content-Range"] = "bytes */" + std::to_string(size);
          return error_reply(res, 416, "range past the end");
        }
        res->status = 206;
        res->headers["Content-Range"] = "bytes " + std::to_string(from) + "-" +
                                        std::to_string(size == 0 ? 0 : size - 1) + "/" + std::to_string(size);
      }
      res->content_type = "application/octet-stream";
      res->headers["Accept-Ranges"] = "bytes";
      res->headers["ETag"] = "\"" + r[1] + "\"";
      res->file_path = store_.blob_path(r[1]);
      res->file_offset = from;
      res->file_length = size - from;
      return;
    }
    return error_reply(res, 405, "method not allowed");
  }
  if (!r.empty() && r[0] == "uploads") {
    if (!need_write()) return;
    UploadSession u;
    if (r.size() == 1 && req.method == "POST") {
      Value body;
      if (!parse_body(&body)) return;
      if (!store_.begin_upload(body.str("sha256"), static_cast<std::uint64_t>(body.num("size", 0)), &u, &err)) {
        return error_reply(res, 400, err);
      }
      return reply(res, 200, upload_json(u));
    }
    if (r.size() == 2 && req.method == "GET") {
      if (!store_.upload_status(r[1], &u)) return error_reply(res, 404, "unknown upload");
      return reply(res, 200, upload_json(u));
    }
    if (r.size() == 2 && req.method == "PUT") {
      const auto it = req.query.find("offset");
      const std::uint64_t offset = it == req.query.end() ? 0 : std::strtoull(it->second.c_str(), nullptr, 10);
      const int code = store_.append_upload(r[1], offset, req.body, &u, &err);
      if (code != 200) {
        Value v = upload_json(u);
        v.set("error", Value::string(err));
        return reply(res, code, v);
      }
      return reply(res, 200, upload_json(u));
    }
    if (r.size() == 3 && r[2] == "complete" && req.method == "POST") {
      const int code = store_.finish_upload(r[1], &err);
      if (code != 200) return error_reply(res, code, err);
      Value v = Value::object();
      v.set("ok", Value::boolean(true));
      return reply(res, 200, v);
    }
    return error_reply(res, 405, "method not allowed");
  }
  error_reply(res, 404, "not found");
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
