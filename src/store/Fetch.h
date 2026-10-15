#pragma once

// Fetch a URL into memory: file:// (and plain paths), http:// everywhere,
// https:// on Windows (WinHTTP, system certificate store). Used for the
// Trading Post's remote index and package downloads; no cookies, no auth,
// redirects followed (up to 5), a size cap, and a timeout.

#include <cstddef>
#include <string>

namespace ts {
namespace tombstone {
namespace store {

struct FetchOptions {
  std::size_t max_bytes = 64u * 1024u * 1024u;
  int timeout_ms = 15000;
};

bool fetch_url(const std::string& url, std::string* body, std::string* error,
               const FetchOptions& opt = {});

// "file:///C:/x/y.json" -> "C:/x/y.json"; plain paths come back as is;
// "" for other schemes.
std::string file_url_path(const std::string& url);
// Resolves `ref` against the URL of the document that named it ("a.dll"
// next to "http://h/p/index.json" -> "http://h/p/a.dll"). Absolute refs
// (with a scheme) come back unchanged.
std::string resolve_url(const std::string& base, const std::string& ref);
bool url_has_scheme(const std::string& url);

}  // namespace store
}  // namespace tombstone
}  // namespace ts
