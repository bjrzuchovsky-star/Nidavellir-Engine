#include "store/Fetch.h"

#include "store/Json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ts {
namespace tombstone {
namespace store {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string percent_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

struct UrlParts {
  std::string scheme, host, path;
  int port = 0;
};

bool split_url(const std::string& url, UrlParts* u) {
  const std::size_t se = url.find("://");
  if (se == std::string::npos) return false;
  u->scheme = lower(url.substr(0, se));
  std::string rest = url.substr(se + 3);
  const std::size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  u->path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
    u->port = std::atoi(hostport.substr(colon + 1).c_str());
    hostport = hostport.substr(0, colon);
  }
  u->host = hostport;
  if (u->port == 0) u->port = u->scheme == "https" ? 443 : 80;
  return !u->host.empty();
}

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// WinHTTP follows redirects itself.
bool http_get(const std::string& url, std::string* body, std::string* error,
              const FetchOptions& opt) {
  UrlParts u;
  if (!split_url(url, &u) || (u.scheme != "http" && u.scheme != "https")) {
    *error = "unsupported URL: " + url;
    return false;
  }
  HINTERNET session = WinHttpOpen(L"TombStone-TradingPost/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = WinHttpOpen(L"TombStone-TradingPost/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) {
    *error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  WinHttpSetTimeouts(session, opt.timeout_ms, opt.timeout_ms, opt.timeout_ms, opt.timeout_ms);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, widen(u.host).c_str(),
                                  static_cast<INTERNET_PORT>(u.port), 0);
  HINTERNET req = nullptr;
  if (conn) {
    req = WinHttpOpenRequest(conn, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
  }
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                0, 0) &&
      WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      *error = "HTTP " + std::to_string(status) + " for " + url;
    } else {
      ok = true;
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
          ok = false;
          *error = "download interrupted: " + url;
          break;
        }
        if (avail == 0) break;
        if (body->size() + avail > opt.max_bytes) {
          ok = false;
          *error = "download larger than " + std::to_string(opt.max_bytes) + " bytes: " + url;
          break;
        }
        const std::size_t at = body->size();
        body->resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, body->data() + at, avail, &got)) {
          ok = false;
          *error = "download interrupted: " + url;
          break;
        }
        body->resize(at + got);
      }
    }
  } else {
    *error = "could not reach " + url + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
}

#else

bool http_get(const std::string& url, std::string* body, std::string* error,
              const FetchOptions& opt) {
  UrlParts u;
  if (!split_url(url, &u)) {
    *error = "bad URL: " + url;
    return false;
  }
  if (u.scheme != "http") {
    *error = u.scheme + ":// is not supported on this platform (http:// and file:// are)";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) {
    *error = "could not resolve " + u.host;
    return false;
  }
  int fd = -1;
  for (addrinfo* a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    *error = "could not connect to " + u.host + ":" + std::to_string(u.port);
    return false;
  }
  const std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host +
                          "\r\nUser-Agent: TombStone-TradingPost/1\r\nConnection: close\r\n\r\n";
  if (send(fd, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
    close(fd);
    *error = "send failed to " + u.host;
    return false;
  }
  std::string raw;
  char buf[16384];
  for (;;) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, opt.timeout_ms) <= 0) {
      close(fd);
      *error = "timed out reading " + url;
      return false;
    }
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      close(fd);
      *error = "read failed: " + url;
      return false;
    }
    if (n == 0) break;
    raw.append(buf, static_cast<std::size_t>(n));
    if (raw.size() > opt.max_bytes + 65536) {
      close(fd);
      *error = "download larger than " + std::to_string(opt.max_bytes) + " bytes: " + url;
      return false;
    }
  }
  close(fd);
  const std::size_t hdr_end = raw.find("\r\n\r\n");
  if (hdr_end == std::string::npos) {
    *error = "bad HTTP response from " + url;
    return false;
  }
  const std::string head = raw.substr(0, hdr_end);
  const std::size_t sp = head.find(' ');
  const int status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
  if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
    const std::string lh = lower(head);
    const std::size_t loc = lh.find("\r\nlocation:");
    if (loc != std::string::npos) {
      std::size_t a = loc + 11;
      while (a < head.size() && head[a] == ' ') ++a;
      const std::size_t b = head.find("\r\n", a);
      *error = "redirect:" + resolve_url(url, head.substr(a, b == std::string::npos ? b : b - a));
      return false;
    }
  }
  if (status != 200) {
    *error = "HTTP " + std::to_string(status) + " for " + url;
    return false;
  }
  *body = raw.substr(hdr_end + 4);
  if (body->size() > opt.max_bytes) {
    *error = "download larger than " + std::to_string(opt.max_bytes) + " bytes: " + url;
    return false;
  }
  return true;
}

#endif

}  // namespace

bool url_has_scheme(const std::string& url) {
  const std::size_t se = url.find("://");
  if (se == std::string::npos || se < 2) return false;  // "C://" is not a scheme
  for (std::size_t i = 0; i < se; ++i) {
    if (!std::isalpha(static_cast<unsigned char>(url[i]))) return false;
  }
  return true;
}

std::string file_url_path(const std::string& url) {
  if (lower(url.substr(0, 7)) == "file://") {
    std::string p = percent_decode(url.substr(7));
    if (p.rfind("localhost/", 0) == 0) p = p.substr(9);
    // file:///C:/x -> C:/x
    if (p.size() >= 3 && p[0] == '/' && std::isalpha(static_cast<unsigned char>(p[1])) && p[2] == ':') {
      p = p.substr(1);
    }
    return p;
  }
  if (url_has_scheme(url)) return {};
  return url;  // a plain path
}

std::string resolve_url(const std::string& base, const std::string& ref) {
  if (ref.empty() || url_has_scheme(ref)) return ref;
  if (!url_has_scheme(base)) {
    std::filesystem::path r(ref);
    if (r.is_absolute()) return ref;
    return (std::filesystem::path(base).parent_path() / r).generic_string();
  }
  if (ref[0] == '/') {
    if (lower(base.substr(0, 7)) == "file://") return "file://" + ref;
    UrlParts u;
    if (!split_url(base, &u)) return ref;
    const bool default_port =
        (u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443);
    return u.scheme + "://" + u.host + (default_port ? "" : ":" + std::to_string(u.port)) + ref;
  }
  const std::string b = base.substr(0, base.find_first_of("?#"));
  const std::size_t slash = b.rfind('/');
  return (slash == std::string::npos ? b + "/" : b.substr(0, slash + 1)) + ref;
}

bool fetch_url(const std::string& url, std::string* body, std::string* error,
               const FetchOptions& opt) {
  body->clear();
  const std::string path = file_url_path(url);
  if (!path.empty()) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
      if (error) *error = "not found: " + url;
      return false;
    }
    if (size > opt.max_bytes) {
      if (error) *error = "file larger than " + std::to_string(opt.max_bytes) + " bytes: " + url;
      return false;
    }
    return read_text_file(path, body, error);
  }
  std::string current = url;
  for (int hop = 0; hop < 5; ++hop) {
    std::string err;
    body->clear();
    if (http_get(current, body, &err, opt)) return true;
    if (err.rfind("redirect:", 0) == 0) {
      current = err.substr(9);
      continue;
    }
    if (error) *error = err;
    return false;
  }
  if (error) *error = "too many redirects: " + url;
  return false;
}

}  // namespace store
}  // namespace tombstone
}  // namespace ts
