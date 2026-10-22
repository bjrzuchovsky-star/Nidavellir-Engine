#pragma once

// Minimal HTTP/1.1 for The Vault: a client (plain sockets for http://;
// WinHTTP for https:// on Windows) and a small threaded server (one
// request per connection, "Connection: close"). Headless; no TLS in the
// server (put a reverse proxy in front for https, see docs/vault.md).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ts {
namespace tombstone {
namespace vault {

// Header names are stored lowercase.
using Headers = std::map<std::string, std::string>;

struct HttpRequest {
  std::string method;  // "GET", "POST", "PUT", "DELETE"
  std::string target;  // "/api/v1/items?q=crate"
  std::string path;    // "/api/v1/items" (percent-decoded)
  std::map<std::string, std::string> query;
  Headers headers;
  std::string body;
  std::string peer;    // remote address
  std::string header(const std::string& lower_name) const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  Headers headers;
  std::string body;
  // Stream a file instead of `body` (blobs): [file_offset, file_offset + file_length).
  std::string file_path;
  std::uint64_t file_offset = 0;
  std::uint64_t file_length = 0;
};

const char* http_reason(int status);
std::string url_decode(const std::string& s);
std::string url_encode(const std::string& s);

struct UrlParts {
  std::string scheme, host, path;  // path includes the query
  int port = 0;
};
bool split_url(const std::string& url, UrlParts* out);

// ---- client ------------------------------------------------------------------

struct ClientOptions {
  int timeout_ms = 30000;
  std::size_t max_body = 64u * 1024u * 1024u;
  // When set, the response body is handed here in pieces instead of
  // collected (downloads); return false to abort.
  std::function<bool(const char* data, std::size_t n)> sink;
};

struct ClientResult {
  int status = 0;  // 0 = no response (connection error)
  Headers headers;
  std::string body;
  std::uint64_t bytes_received = 0;
};

// False only on transport errors (no HTTP status); check result.status.
bool http_request(const std::string& method, const std::string& url, const Headers& headers,
                  const std::string& body, ClientResult* out, std::string* error,
                  const ClientOptions& opt = {});

// ---- server ------------------------------------------------------------------

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse*)>;

struct ServerOptions {
  std::string host = "127.0.0.1";  // "0.0.0.0" to listen on every interface
  int port = 8740;                 // 0 = pick a free port (tests)
  std::size_t max_body = 16u * 1024u * 1024u;
  int max_connections = 64;
  int timeout_ms = 30000;
  // Access log line per request (never contains headers or bodies).
  std::function<void(const std::string&)> log;
};

class HttpServer {
 public:
  HttpServer() = default;
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool start(const ServerOptions& opt, HttpHandler handler, std::string* error);
  void stop();
  int port() const { return port_; }
  bool running() const { return running_; }
  // Blocks until stop() is called from elsewhere (or a signal handler).
  void wait();

 private:
  void accept_loop();
  void serve(std::intptr_t sock, std::string peer);

  ServerOptions opt_;
  HttpHandler handler_;
  std::intptr_t listen_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<int> active_{0};
  std::thread thread_;
  std::mutex mu_;
};

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
