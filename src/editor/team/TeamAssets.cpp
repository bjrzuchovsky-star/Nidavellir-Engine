#include "editor/team/TeamAssets.h"

#include "vault/Sha256.h"

#include <filesystem>
#include <algorithm>
#include <exception>
#include <set>
#include <thread>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

long long mtime_of(const fs::path& p) {
  std::error_code ec;
  return static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count());
}

constexpr const char* kMetaPrefix = ".tombstone/meta/";

}  // namespace

bool team_meta_rel(const std::string& rel) {
  return rel.rfind(std::string(kMetaPrefix) + "assets/", 0) == 0 && rel.size() > 5 &&
         rel.compare(rel.size() - 5, 5, ".meta") == 0;
}

bool team_file_rel(const std::string& rel) {
  if (rel.find(".team-part") != std::string::npos) return false;
  return rel.rfind("assets/", 0) == 0 || team_meta_rel(rel);
}

std::string team_meta_asset(const std::string& rel) {
  if (!team_meta_rel(rel)) return {};
  const std::size_t n = std::string(kMetaPrefix).size();
  return rel.substr(n, rel.size() - n - 5);
}

bool TeamAssetSync::start(TeamAssetPass pass, std::shared_ptr<vault::Backend> backend) {
  if (running() || !backend) return false;
  cancel_ = false;
  done_.store(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    has_result_ = false;
    progress_ = TeamAssetProgress{};
    progress_.running = true;
    progress_.stage = "Scanning";
  }
  running_.store(true);
  // A plain thread, not std::async: wait_for on that future can come back
  // deferred, so the pass never starts and the editor reports no error.
  try {
    thread_ = std::thread([this, pass = std::move(pass), backend]() mutable {
      try {
        run(pass, *backend, this);
      } catch (const std::exception& ex) {
        pass.error = ex.what();
      } catch (...) {
        pass.error = "file sync failed";
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        result_ = std::move(pass);
        has_result_ = true;
        // Leave running set so the frame that is about to take this pass
        // still shows the file and byte counters.
        progress_.running = true;
      }
      running_.store(false);
      done_.store(true);
    });
  } catch (const std::exception&) {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mu_);
    progress_.running = false;
    return false;
  }
  return true;
}

bool TeamAssetSync::running() const {
  return running_.load() || done_.load();
}

bool TeamAssetSync::take(TeamAssetPass* out) {
  if (!done_.load()) return false;
  if (thread_.joinable()) thread_.join();
  TeamAssetPass p;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!has_result_) return false;
    p = std::move(result_);
    has_result_ = false;
    progress_.running = false;
    progress_.current.clear();
  }
  done_.store(false);
  running_.store(false);
  if (out) *out = std::move(p);
  return true;
}

void TeamAssetSync::wait() {
  if (thread_.joinable()) thread_.join();
}

void TeamAssetSync::cancel_and_wait() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  cancel_ = false;
  done_.store(false);
  running_.store(false);
  std::lock_guard<std::mutex> lock(mu_);
  has_result_ = false;
  progress_ = TeamAssetProgress{};
}

TeamAssetProgress TeamAssetSync::progress() const {
  std::lock_guard<std::mutex> lock(mu_);
  return progress_;
}

void TeamAssetSync::set_progress(const TeamAssetProgress& p) {
  std::lock_guard<std::mutex> lock(mu_);
  progress_ = p;
}

void TeamAssetSync::run(TeamAssetPass& p, vault::Backend& backend, TeamAssetSync* self) {
  const fs::path root = fs::u8path(p.root);
  std::error_code ec;
  TeamAssetProgress prog;
  prog.running = true;
  prog.stage = "Scanning";
  auto report = [&] {
    if (self) self->set_progress(prog);
  };
  auto cancelled = [&] { return self && self->cancel_.load(); };

  // 1. Scan assets/ (mtime + size decide whether to re-hash).
  // Sizes are published before the hash so a multi-megabyte send already
  // has files / bytes on the progress readout while the Vault call is in flight.
  struct Found {
    std::string rel;
    std::uint64_t size = 0;
    long long mtime = 0;
    std::string path;
  };
  std::vector<Found> found;
  for (const fs::path dir : {root / "assets", root / ".tombstone" / "meta" / "assets"}) {
    if (!fs::is_directory(dir, ec)) continue;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (cancelled()) return;
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = fs::relative(it->path(), root, ec).generic_string();
      if (rel.empty() || !team_file_rel(rel)) continue;
      Found f;
      f.rel = rel;
      f.size = static_cast<std::uint64_t>(it->file_size(ec));
      f.mtime = mtime_of(it->path());
      f.path = it->path().string();
      prog.bytes_total += f.size;
      ++prog.files_total;
      found.push_back(std::move(f));
    }
  }
  prog.stage = "Scanning";
  prog.bytes_done = 0;
  prog.files_done = 0;
  report();
  std::map<std::string, TeamLocalFile> now;
  for (const Found& src : found) {
    if (cancelled()) return;
    TeamLocalFile f;
    f.size = src.size;
    f.mtime = src.mtime;
    auto old = p.files.find(src.rel);
    if (old != p.files.end() && old->second.mtime == f.mtime && old->second.size == f.size &&
        !old->second.sha.empty()) {
      f.sha = old->second.sha;
    } else {
      prog.current = src.rel;
      report();
      f.sha = vault::sha256_file(src.path);
    }
    prog.bytes_done += src.size;
    ++prog.files_done;
    report();
    if (f.sha.empty()) {
      p.error = "could not read " + src.rel;
      p.notes.push_back("Could not read " + src.rel + " (not sent).");
      continue;
    }
    now[src.rel] = std::move(f);
  }
  p.files = now;
  // An asset changed here since it came from the session is ours now.
  for (auto it = p.fetched.begin(); it != p.fetched.end();) {
    auto f = p.files.find(*it);
    auto k = p.synced.find(*it);
    if (f == p.files.end() || (k != p.synced.end() && k->second != f->second.sha)) it = p.fetched.erase(it);
    else ++it;
  }
  // What the session confirmed is in step; sends it confirmed are done.
  for (auto it = p.sent.begin(); it != p.sent.end();) {
    auto s = p.session.find(it->first);
    if (s != p.session.end() && s->second.sha256 == it->second) it = p.sent.erase(it);
    else ++it;
  }

  struct Job {
    bool up = false;
    std::string rel;
    team::AssetRef ref;
  };
  std::vector<Job> jobs;
  std::set<std::string> handled;

  // 2. Deletes. A file the session and this realm agreed on ...
  for (auto it = p.synced.begin(); it != p.synced.end();) {
    const std::string rel = it->first;
    const std::string sha = it->second;
    auto s = p.session.find(rel);
    auto f = p.files.find(rel);
    // ... gone here, still the same in the session: delete it for everyone.
    if (f == p.files.end() && s != p.session.end() && s->second.sha256 == sha && !p.first) {
      if (p.can_write) {
        p.ops.push_back(team::Op::asset(rel, "", 0));
        if (!team_meta_rel(rel)) p.notes.push_back("Removed " + rel + " for the clan.");
        handled.insert(rel);
        it = p.synced.erase(it);
        continue;
      }
      // Viewers can't delete: it comes back below.
    }
    // ... gone from the session, unchanged here: someone deleted it.
    if (s == p.session.end() && f != p.files.end() && f->second.sha == sha && !p.first &&
        !p.sent.count(rel)) {
      const fs::path src = root / fs::u8path(rel);
      const fs::path dest = root / ".tombstone" / "team" / "trash" / fs::u8path(rel);
      fs::create_directories(dest.parent_path(), ec);
      fs::remove(dest, ec);
      fs::rename(src, dest, ec);
      if (ec) fs::remove(src, ec);
      if (!fs::exists(src, ec)) {
        p.files.erase(rel);
        p.arrived.push_back(rel);
        ++p.removed;
        if (!team_meta_rel(rel)) {
          p.notes.push_back("Removed " + rel + " (deleted by the clan; a copy is in .tombstone/team/trash).");
        }
      }
      handled.insert(rel);
      it = p.synced.erase(it);
      continue;
    }
    ++it;
  }

  // 3. Changed / new here go up; newer in the session come down.
  for (const auto& [rel, f] : p.files) {
    if (handled.count(rel)) continue;
    auto s = p.session.find(rel);
    if (s != p.session.end() && s->second.sha256 == f.sha) {
      p.synced[rel] = f.sha;
      continue;
    }
    auto known = p.synced.find(rel);
    const bool meta = team_meta_rel(rel);
    // A .meta never agreed with the session (made by this PC's asset scan
    // for a file that just came in, or an older copy): the session's wins,
    // so the GUID and import settings are the posse's.
    if (s != p.session.end() && (p.first || (known != p.synced.end() && known->second == f.sha) ||
                                 (meta && known == p.synced.end()))) {
      jobs.push_back({false, rel, s->second});  // unchanged here: theirs is newer
      continue;
    }
    // The .meta of a file fetched from the session waits for its author's.
    if (meta && s == p.session.end() && p.fetched.count(team_meta_asset(rel))) continue;
    if (!p.can_write) {
      if (!meta && s == p.session.end()) p.held_back.push_back(rel);
      continue;
    }
    auto sent = p.sent.find(rel);
    if (sent != p.sent.end() && sent->second == f.sha) continue;  // waiting for the commit
    // Both sides edited since the last agreed copy: the writer who syncs
    // last keeps the file, and the clan is told.
    if (!meta && s != p.session.end() && known != p.synced.end() && known->second != f.sha &&
        s->second.sha256 != known->second && s->second.sha256 != f.sha) {
      p.notes.push_back("Both sides changed " + rel + ". This copy is the one the clan keeps.");
    }
    jobs.push_back({true, rel, team::AssetRef{f.sha, f.size}});
  }
  for (const auto& [rel, a] : p.session) {
    if (!p.files.count(rel) && !handled.count(rel) && !a.sha256.empty()) jobs.push_back({false, rel, a});
  }

  // 4. Transfers. Fetched .meta files land before their assets, so the
  // asset scan finds the posse's GUID instead of minting one.
  std::stable_partition(jobs.begin(), jobs.end(), [](const Job& j) { return team_meta_rel(j.rel); });
  std::uint64_t job_bytes = 0;
  for (const Job& j : jobs) job_bytes += j.ref.size;
  // Keep the scan totals. Replacing them with only the jobs would drop the
  // counter to 0 for a frame and the smoke would miss the 3 MB send.
  if (job_bytes > prog.bytes_total) prog.bytes_total = job_bytes;
  if (static_cast<int>(jobs.size()) > prog.files_total) prog.files_total = static_cast<int>(jobs.size());
  prog.files_done = 0;
  prog.bytes_done = 0;
  report();
  std::vector<team::Op> ups;
  for (const Job& j : jobs) {
    if (cancelled()) break;
    prog.stage = j.up ? "Sending" : "Fetching";
    prog.current = j.rel;
    report();
    std::string err;
    if (j.up) {
      std::vector<std::string> missing;
      const std::string path = (root / fs::u8path(j.rel)).string();
      if (!backend.missing({j.ref.sha256}, &missing, &err)) {
        p.error = err;
        p.notes.push_back("Could not reach " + backend.label() + ": " + err);
        break;
      }
      if (!missing.empty() && !backend.upload(path, j.ref.sha256, j.ref.size, &err)) {
        p.error = err;
        p.notes.push_back("Could not send " + j.rel + " to " + backend.label() + ": " + err);
      } else {
        ups.push_back(team::Op::asset(j.rel, j.ref.sha256, j.ref.size));
        p.sent[j.rel] = j.ref.sha256;
        p.fetched.erase(j.rel);
        ++p.up;
      }
    } else {
      const fs::path dest = root / fs::u8path(j.rel);
      fs::create_directories(dest.parent_path(), ec);
      const fs::path part = dest.string() + ".team-part";
      if (!backend.download(j.ref.sha256, part.string(), &err)) {
        p.error = err;
        p.notes.push_back("Could not fetch " + j.rel + " from " + backend.label() + ": " + err);
        fs::remove(part, ec);
      } else {
        fs::rename(part, dest, ec);
        if (ec) {
          fs::remove(dest, ec);
          fs::rename(part, dest, ec);
        }
        TeamLocalFile f;
        f.size = j.ref.size;
        f.sha = j.ref.sha256;
        f.mtime = mtime_of(dest);
        p.files[j.rel] = f;
        p.synced[j.rel] = j.ref.sha256;
        if (!team_meta_rel(j.rel)) p.fetched.insert(j.rel);
        p.arrived.push_back(j.rel);
        ++p.down;
        if (!team_meta_rel(j.rel)) p.notes.push_back("Fetched " + j.rel + " from the clan.");
      }
    }
    ++prog.files_done;
    prog.bytes_done += j.ref.size;
    report();
  }
  p.ops.insert(p.ops.end(), ups.begin(), ups.end());
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
