#include "vault/Http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ts {
namespace tombstone {
namespace vault {

namespace {

#if defined(_WIN32)
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
void close_sock(sock_t s) { close(s); }
#endif

void net_init() {
#if defined(_WIN32)
  static const bool ok = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)ok;
#else
  static const bool ok = [] {
    signal(SIGPIPE, SIG_IGN);  // a peer hanging up must not kill the server
    return true;
  }();
  (void)ok;
#endif
}

void set_timeouts(sock_t s, int ms) {
#if defined(_WIN32)
  DWORD t = static_cast<DWORD>(ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool send_all(sock_t s, const char* p, std::size_t n) {
  while (n > 0) {
    const int chunk = static_cast<int>(std::min<std::size_t>(n, 1 << 20));
#if defined(_WIN32)
    const int k = send(s, p, chunk, 0);
#else
    const int k = static_cast<int>(send(s, p, static_cast<std::size_t>(chunk), MSG_NOSIGNAL));
#endif
    if (k <= 0) return false;
    p += k;
    n -= static_cast<std::size_t>(k);
  }
  return true;
}

int recv_some(sock_t s, char* buf, int n) {
  return static_cast<int>(recv(s, buf, n, 0));
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Parses "Name: value" lines after the first line of `head`.
void parse_headers(const std::string& head, Headers* out) {
  std::size_t pos = head.find("\r\n");
  while (pos != std::string::npos && pos + 2 < head.size()) {
    const std::size_t next = head.find("\r\n", pos + 2);
    const std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) (*out)[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    pos = next;
  }
}

std::string peer_name(const sockaddr_in& a) {
  char buf[64] = {};
  inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
  return buf;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

bool winhttp_request(const std::string& method, const UrlParts& u, const Headers& headers,
                     const std::string& body, ClientResult* out, std::string* error,
                     const ClientOptions& opt) {
  HINTERNET session = WinHttpOpen(L"TombStone-Vault/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = WinHttpOpen(L"TombStone-Vault/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) {
    *error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  WinHttpSetTimeouts(session, opt.timeout_ms, opt.timeout_ms, opt.timeout_ms, opt.timeout_ms);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, widen(u.host).c_str(), static_cast<INTERNET_PORT>(u.port), 0);
  HINTERNET req = nullptr;
  if (conn) {
    req = WinHttpOpenRequest(conn, widen(method).c_str(), widen(u.path).c_str(), nullptr,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  }
  std::string hdrs;
  for (const auto& [k, v] : headers) hdrs += k + ": " + v + "\r\n";
  const std::wstring whdrs = widen(hdrs);
  if (req &&
      WinHttpSendRequest(req, whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
                         whdrs.empty() ? 0 : static_cast<DWORD>(-1L),
                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
      WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    out->status = static_cast<int>(status);
    DWORD hlen = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &hlen, WINHTTP_NO_HEADER_INDEX);
    if (hlen > 0) {
      std::wstring raw(hlen / sizeof(wchar_t), L'\0');
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                              raw.data(), &hlen, WINHTTP_NO_HEADER_INDEX)) {
        parse_headers(narrow(raw), &out->headers);
      }
    }
    ok = true;
    std::string buf;
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(req, &avail)) {
        ok = false;
        *error = "connection interrupted";
        break;
      }
      if (avail == 0) break;
      buf.resize(avail);
      DWORD got = 0;
      if (!WinHttpReadData(req, buf.data(), avail, &got)) {
        ok = false;
        *error = "connection interrupted";
        break;
      }
      out->bytes_received += got;
      if (opt.sink && out->status >= 200 && out->status < 300) {
        if (!opt.sink(buf.data(), got)) {
          ok = false;
          *error = "download aborted";
          break;
        }
      } else {
        if (out->body.size() + got > opt.max_body) {
          ok = false;
          *error = "response too large";
          break;
        }
        out->body.append(buf.data(), got);
      }
    }
  } else {
    *error = "could not reach https://" + u.host + ":" + std::to_string(u.port) + " (WinHTTP error " +
             std::to_string(GetLastError()) + ")";
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}
#endif

}  // namespace

std::string HttpRequest::header(const std::string& lower_name) const {
  const auto it = headers.find(lower_name);
  return it == headers.end() ? std::string() : it->second;
}

const char* http_reason(int s) {
  switch (s) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
  }
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string url_encode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

bool split_url(const std::string& url, UrlParts* u) {
  const std::size_t se = url.find("://");
  if (se == std::string::npos) return false;
  u->scheme = lower(url.substr(0, se));
  const std::string rest = url.substr(se + 3);
  const std::size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  u->path = slash == std::string::npos ? "/" : rest.substr(slash);
  u->port = 0;
  const std::size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
    u->port = std::atoi(hostport.substr(colon + 1).c_str());
    hostport = hostport.substr(0, colon);
  }
  u->host = hostport;
  if (u->port == 0) u->port = u->scheme == "https" ? 443 : 80;
  return !u->host.empty() && (u->scheme == "http" || u->scheme == "https");
}

// ---- client ------------------------------------------------------------------

bool http_request(const std::string& method, const std::string& url, const Headers& headers,
                  const std::string& body, ClientResult* out, std::string* error,
                  const ClientOptions& opt) {
  *out = ClientResult{};
  std::string dummy;
  if (!error) error = &dummy;
  UrlParts u;
  if (!split_url(url, &u)) {
    *error = "bad URL (want http:// or https://): " + url;
    return false;
  }
  if (u.scheme == "https") {
#if defined(_WIN32)
    return winhttp_request(method, u, headers, body, out, error, opt);
#else
    *error = "https:// needs the Windows editor; on this platform use http:// (see docs/vault.md)";
    return false;
#endif
  }
  net_init();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) {
    *error = "could not resolve " + u.host;
    return false;
  }
  sock_t s = kBadSock;
  for (addrinfo* a = res; a; a = a->ai_next) {
    s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kBadSock) continue;
    set_timeouts(s, opt.timeout_ms);
    if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
    close_sock(s);
    s = kBadSock;
  }
  freeaddrinfo(res);
  if (s == kBadSock) {
    *error = "could not connect to " + u.host + ":" + std::to_string(u.port);
    return false;
  }
  std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                    "\r\nUser-Agent: TombStone-Vault/1\r\nConnection: close\r\n";
  bool has_type = false;
  for (const auto& [k, v] : headers) {
    req += k + ": " + v + "\r\n";
    if (lower(k) == "content-type") has_type = true;
  }
  if (!body.empty() || method == "POST" || method == "PUT") {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!has_type) req += "Content-Type: application/octet-stream\r\n";
  }
  req += "\r\n";
  if (!send_all(s, req.data(), req.size()) || (!body.empty() && !send_all(s, body.data(), body.size()))) {
    close_sock(s);
    *error = "send failed to " + u.host;
    return false;
  }
  std::string raw;
  char buf[65536];
  std::size_t hdr_end = std::string::npos;
  while (hdr_end == std::string::npos) {
    const int n = recv_some(s, buf, sizeof(buf));
    if (n <= 0) {
      close_sock(s);
      *error = n == 0 ? "connection closed before a response" : "timed out waiting for " + u.host;
      return false;
    }
    raw.append(buf, static_cast<std::size_t>(n));
    hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos && raw.size() > 65536) {
      close_sock(s);
      *error = "bad HTTP response";
      return false;
    }
  }
  const std::string head = raw.substr(0, hdr_end);
  const std::size_t sp = head.find(' ');
  out->status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
  parse_headers(head, &out->headers);
  std::string rest = raw.substr(hdr_end + 4);
  const bool ok_status = out->status >= 200 && out->status < 300;
  const bool to_sink = static_cast<bool>(opt.sink) && ok_status;
  bool ok = true;
  auto take = [&](const char* d, std::size_t n) {
    out->bytes_received += n;
    if (to_sink) {
      if (!opt.sink(d, n)) {
        *error = "download aborted";
        return false;
      }
      return true;
    }
    if (out->body.size() + n > opt.max_body) {
      *error = "response too large";
      return false;
    }
    out->body.append(d, n);
    return true;
  };
  const auto te = out->headers.find("transfer-encoding");
  if (te != out->headers.end() && lower(te->second).find("chunked") != std::string::npos) {
    // Chunked (reverse proxies may re-encode): decode as it arrives.
    std::string pending = rest;
    for (;;) {
      std::size_t le = pending.find("\r\n");
      while (le == std::string::npos) {
        const int n = recv_some(s, buf, sizeof(buf));
        if (n <= 0) { ok = false; *error = "connection interrupted"; break; }
        pending.append(buf, static_cast<std::size_t>(n));
        le = pending.find("\r\n");
      }
      if (!ok) break;
      const std::size_t size = std::strtoull(pending.substr(0, le).c_str(), nullptr, 16);
      pending.erase(0, le + 2);
      if (size == 0) break;
      while (pending.size() < size + 2) {
        const int n = recv_some(s, buf, sizeof(buf));
        if (n <= 0) { ok = false; *error = "connection interrupted"; break; }
        pending.append(buf, static_cast<std::size_t>(n));
      }
      if (!ok) break;
      if (!take(pending.data(), size)) { ok = false; break; }
      pending.erase(0, size + 2);
    }
  } else {
    const auto cl = out->headers.find("content-length");
    const bool known = cl != out->headers.end();
    const std::uint64_t want = known ? std::strtoull(cl->second.c_str(), nullptr, 10) : ~0ull;
    std::uint64_t got = 0;
    if (!rest.empty()) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(rest.size(), want));
      ok = take(rest.data(), n);
      got += n;
    }
    while (ok && got < want) {
      const int n = recv_some(s, buf, static_cast<int>(std::min<std::uint64_t>(sizeof(buf), want - got)));
      if (n == 0 && !known) break;
      if (n <= 0) {
        ok = false;
        *error = "connection interrupted after " + std::to_string(got) + " bytes";
        break;
      }
      ok = take(buf, static_cast<std::size_t>(n));
      got += static_cast<std::uint64_t>(n);
    }
  }
  close_sock(s);
  return ok;
}

// ---- server ------------------------------------------------------------------

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const ServerOptions& opt, HttpHandler handler, std::string* error) {
  net_init();
  opt_ = opt;
  handler_ = std::move(handler);
  std::string host = opt.host.empty() || opt.host == "localhost" ? "127.0.0.1" : opt.host;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<unsigned short>(opt.port));
  if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
    if (error) *error = "listen address must be an IPv4 address (e.g. 0.0.0.0 or 127.0.0.1): " + opt.host;
    return false;
  }
  sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == kBadSock) {
    if (error) *error = "socket() failed";
    return false;
  }
#if !defined(_WIN32)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close_sock(s);
    if (error) *error = "cannot listen on " + host + ":" + std::to_string(opt.port) + " (port in use?)";
    return false;
  }
  socklen_t len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  if (listen(s, 64) != 0) {
    close_sock(s);
    if (error) *error = "listen() failed";
    return false;
  }
  listen_ = static_cast<std::intptr_t>(s);
  running_ = true;
  thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  // Wake accept() with a throwaway loopback connection.
  sock_t w = socket(AF_INET, SOCK_STREAM, 0);
  if (w != kBadSock) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port_));
    const std::string host = opt_.host == "0.0.0.0" || opt_.host.empty() || opt_.host == "localhost"
                                 ? "127.0.0.1" : opt_.host;
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    set_timeouts(w, 1000);
    connect(w, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    close_sock(w);
  }
  if (thread_.joinable()) thread_.join();
  close_sock(static_cast<sock_t>(listen_));
  listen_ = -1;
  // Let in-flight requests finish (bounded).
  for (int i = 0; i < 400 && active_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void HttpServer::wait() {
  while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void HttpServer::accept_loop() {
  while (running_) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    sock_t c = accept(static_cast<sock_t>(listen_), reinterpret_cast<sockaddr*>(&peer), &plen);
    if (!running_) {
      if (c != kBadSock) close_sock(c);
      break;
    }
    if (c == kBadSock) continue;
    if (active_ >= opt_.max_connections) {
      const std::string busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      send_all(c, busy.data(), busy.size());
      close_sock(c);
      continue;
    }
    ++active_;
    std::thread([this, c, p = peer_name(peer)] {
      serve(static_cast<std::intptr_t>(c), p);
      --active_;
    }).detach();
  }
}

void HttpServer::serve(std::intptr_t sock, std::string peer) {
  const sock_t s = static_cast<sock_t>(sock);
  set_timeouts(s, opt_.timeout_ms);
  HttpRequest req;
  req.peer = std::move(peer);
  HttpResponse res;
  std::string raw;
  char buf[65536];
  std::size_t hdr_end = std::string::npos;
  bool bad = false;
  while (hdr_end == std::string::npos) {
    const int n = recv_some(s, buf, sizeof(buf));
    if (n <= 0) {
      close_sock(s);
      return;
    }
    raw.append(buf, static_cast<std::size_t>(n));
    hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos && raw.size() > 65536) {
      bad = true;
      break;
    }
  }
  auto respond = [&](const HttpResponse& r) {
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + http_reason(r.status) + "\r\n";
    const bool file = !r.file_path.empty();
    const std::uint64_t len = file ? r.file_length : r.body.size();
    head += "Content-Type: " + r.content_type + "\r\nContent-Length: " + std::to_string(len) +
            "\r\nConnection: close\r\nServer: ts_vault\r\n";
    for (const auto& [k, v] : r.headers) head += k + ": " + v + "\r\n";
    head += "\r\n";
    if (!send_all(s, head.data(), head.size())) return;
    if (req.method == "HEAD") return;
    if (!file) {
      send_all(s, r.body.data(), r.body.size());
      return;
    }
    std::ifstream in(std::filesystem::u8path(r.file_path), std::ios::binary);
    in.seekg(static_cast<std::streamoff>(r.file_offset));
    std::uint64_t left = r.file_length;
    while (in && left > 0) {
      in.read(buf, static_cast<std::streamsize>(std::min<std::uint64_t>(sizeof(buf), left)));
      const std::streamsize n = in.gcount();
      if (n <= 0 || !send_all(s, buf, static_cast<std::size_t>(n))) break;
      left -= static_cast<std::uint64_t>(n);
    }
  };
  auto finish = [&](const HttpResponse& r) {
    respond(r);
    if (opt_.log) {
      opt_.log(req.peer + " " + (req.method.empty() ? "-" : req.method) + " " +
               (req.path.empty() ? "-" : req.path) + " " + std::to_string(r.status));
    }
#if defined(_WIN32)
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    close_sock(s);
  };
  if (bad) {
    res.status = 400;
    res.body = R"({"error":"request headers too large"})";
    finish(res);
    return;
  }
  const std::string head = raw.substr(0, hdr_end);
  {
    const std::string first = head.substr(0, head.find("\r\n"));
    const std::size_t a = first.find(' ');
    const std::size_t b = a == std::string::npos ? std::string::npos : first.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos) {
      res.status = 400;
      res.body = R"({"error":"bad request line"})";
      finish(res);
      return;
    }
    req.method = first.substr(0, a);
    req.target = first.substr(a + 1, b - a - 1);
  }
  parse_headers(head, &req.headers);
  const std::size_t q = req.target.find('?');
  req.path = url_decode(req.target.substr(0, q));
  if (q != std::string::npos) {
    const std::string qs = req.target.substr(q + 1);
    std::size_t pos = 0;
    while (pos <= qs.size()) {
      const std::size_t amp = qs.find('&', pos);
      const std::string kv = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      if (!kv.empty()) {
        const std::size_t eq = kv.find('=');
        req.query[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
      }
      if (amp == std::string::npos) break;
      pos = amp + 1;
    }
  }
  std::uint64_t want = 0;
  if (req.headers.count("content-length")) want = std::strtoull(req.header("content-length").c_str(), nullptr, 10);
  if (lower(req.header("transfer-encoding")).find("chunked") != std::string::npos) {
    res.status = 411;
    res.body = "{\"error\":\"send a Content-Length; chunked request bodies are not supported\"}";
    finish(res);
    return;
  }
  if (want > opt_.max_body) {
    res.status = 413;
    res.body = R"({"error":"request body too large"})";
    finish(res);
    return;
  }
  if (want > 0 && lower(req.header("expect")) == "100-continue") {
    const std::string cont = "HTTP/1.1 100 Continue\r\n\r\n";
    send_all(s, cont.data(), cont.size());
  }
  req.body = raw.substr(hdr_end + 4);
  if (req.body.size() > want) req.body.resize(static_cast<std::size_t>(want));
  while (req.body.size() < want) {
    const int n = recv_some(s, buf, static_cast<int>(std::min<std::uint64_t>(sizeof(buf), want - req.body.size())));
    if (n <= 0) {
      // Client went away mid-body (an interrupted upload): nothing to answer.
      close_sock(s);
      return;
    }
    req.body.append(buf, static_cast<std::size_t>(n));
  }
  try {
    handler_(req, &res);
  } catch (const std::exception& e) {
    res = HttpResponse{};
    res.status = 500;
    res.body = std::string(R"({"error":"internal error"})");
    if (opt_.log) opt_.log(std::string("handler exception: ") + e.what());
  }
  finish(res);
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
