#include "editor/team/TeamAssets.h"

#include "vault/Sha256.h"

#include <filesystem>
#include <algorithm>
#include <set>

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
  if (job_.valid()) return false;  // finished but not taken yet
  cancel_ = false;
  TeamAssetProgress p;
  p.running = true;
  p.stage = "Scanning";
  set_progress(p);
  job_ = std::async(std::launch::async, [this, pass = std::move(pass), backend]() mutable {
    run(pass, *backend, this);
    TeamAssetProgress done = progress();
    done.running = false;
    done.current.clear();
    set_progress(done);
    return std::move(pass);
  });
  return true;
}

bool TeamAssetSync::running() const {
  return job_.valid() && job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

bool TeamAssetSync::take(TeamAssetPass* out) {
  if (!job_.valid() || running()) return false;
  TeamAssetPass p = job_.get();
  if (out) *out = std::move(p);
  return true;
}

void TeamAssetSync::wait() {
  if (job_.valid()) job_.wait();
}

void TeamAssetSync::cancel_and_wait() {
  cancel_ = true;
  if (job_.valid()) job_.get();
  cancel_ = false;
  TeamAssetProgress idle;
  set_progress(idle);
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
  std::map<std::string, TeamLocalFile> now;
  for (const fs::path dir : {root / "assets", root / ".tombstone" / "meta" / "assets"}) {
    if (!fs::is_directory(dir, ec)) continue;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (cancelled()) return;
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = fs::relative(it->path(), root, ec).generic_string();
      if (rel.empty() || !team_file_rel(rel)) continue;
      TeamLocalFile f;
      f.size = static_cast<std::uint64_t>(it->file_size(ec));
      f.mtime = mtime_of(it->path());
      auto old = p.files.find(rel);
      if (old != p.files.end() && old->second.mtime == f.mtime && old->second.size == f.size) {
        f.sha = old->second.sha;
      } else {
        prog.current = rel;
        report();
        f.sha = vault::sha256_file(it->path().string());
      }
      if (!f.sha.empty()) now[rel] = std::move(f);
    }
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

  // 2. Deletes. A file the session and this claim agreed on ...
  for (auto it = p.synced.begin(); it != p.synced.end();) {
    const std::string rel = it->first;
    const std::string sha = it->second;
    auto s = p.session.find(rel);
    auto f = p.files.find(rel);
    // ... gone here, still the same in the session: delete it for everyone.
    if (f == p.files.end() && s != p.session.end() && s->second.sha256 == sha && !p.first) {
      if (p.can_write) {
        p.ops.push_back(team::Op::asset(rel, "", 0));
        if (!team_meta_rel(rel)) p.notes.push_back("Removed " + rel + " for the posse.");
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
          p.notes.push_back("Removed " + rel + " (deleted by the posse; a copy is in .tombstone/team/trash).");
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
    jobs.push_back({true, rel, team::AssetRef{f.sha, f.size}});
  }
  for (const auto& [rel, a] : p.session) {
    if (!p.files.count(rel) && !handled.count(rel) && !a.sha256.empty()) jobs.push_back({false, rel, a});
  }

  // 4. Transfers. Fetched .meta files land before their assets, so the
  // asset scan finds the posse's GUID instead of minting one.
  std::stable_partition(jobs.begin(), jobs.end(), [](const Job& j) { return team_meta_rel(j.rel); });
  prog.files_total = static_cast<int>(jobs.size());
  prog.files_done = 0;
  prog.bytes_total = 0;
  for (const Job& j : jobs) prog.bytes_total += j.ref.size;
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
        if (!team_meta_rel(j.rel)) p.notes.push_back("Fetched " + j.rel + " from the posse.");
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
