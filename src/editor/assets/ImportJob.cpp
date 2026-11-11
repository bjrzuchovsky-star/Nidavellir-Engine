#include "editor/assets/ImportJob.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/ModelSplit.h"
#include "render3d/ModelImport.h"

#include <filesystem>
#include <new>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

ImportJob::~ImportJob() { join(); }

void ImportJob::join() {
  if (thread_.joinable()) thread_.join();
}

void ImportJob::start(Request req) {
  join();
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    state_ = {};
    state_.running = true;
    state_.phase = "Starting import";
    state_.split = req.split;
    state_.split_recenter = req.split_recenter;
    state_.split_fit = req.split_fit;
    taken_ = false;
  }
  thread_ = std::thread([this, req = std::move(req)] { run(req); });
}

void ImportJob::cancel() { cancel_.store(true); }

ImportJob::State ImportJob::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

bool ImportJob::take(State* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!state_.finished || taken_) return false;
  taken_ = true;
  if (out) *out = state_;
  state_.running = false;
  return true;
}

void ImportJob::run(Request req) {
  auto set = [&](int pct, const std::string& phase) {
    std::lock_guard<std::mutex> lock(mu_);
    if (pct >= 0) state_.percent = pct;
    if (!phase.empty()) state_.phase = phase;
  };
  auto finish = [&](bool ok, bool cancelled, const std::string& error, importsort::Result copied) {
    std::lock_guard<std::mutex> lock(mu_);
    state_.running = false;
    state_.finished = true;
    state_.ok = ok;
    state_.cancelled = cancelled;
    state_.error = error;
    state_.copied = std::move(copied);
    state_.percent = cancelled ? state_.percent : 100;
    if (cancelled) state_.phase = "Cancelled";
    else if (!ok) state_.phase = "Import failed";
    else state_.phase = "Import finished";
  };
  importsort::Result copied;
  try {
    struct Hook {
      ImportJob* job;
      std::atomic<bool>* cancel;
    };
    Hook hook{this, &cancel_};
    render3d::ImportMonitor mon;
    mon.user = &hook;
    mon.cancelled = [](void* p) { return static_cast<Hook*>(p)->cancel->load(); };
    mon.progress = [](void* p, int pct, const char* phase) {
      auto* h = static_cast<Hook*>(p);
      std::lock_guard<std::mutex> lock(h->job->mu_);
      h->job->state_.percent = pct;
      if (phase && *phase) h->job->state_.phase = phase;
    };
    render3d::set_import_monitor(&mon);

    if (!req.sources.empty()) {
      set(4, "Copying files");
      if (!importsort::import_files(req.project_dir, req.sources, req.dest_folder, req.settings, &copied)) {
        render3d::set_import_monitor(nullptr);
        finish(false, cancel_.load(), copied.errors.empty() ? "Could not copy the file in." : copied.errors.front(),
               copied);
        return;
      }
    }
    if (cancel_.load()) {
      render3d::set_import_monitor(nullptr);
      for (const std::string& rel : copied.files) {
        std::error_code ec;
        fs::remove(fs::u8path(req.project_dir) / fs::u8path(rel), ec);
      }
      finish(false, true, "Import cancelled.", copied);
      return;
    }
    set(10, req.reimport_rels.empty() ? "Reading model" : "Reimporting");
    assetdb::AssetDb db;
    db.set_importer(assetdb::import_asset);
    db.open(req.project_dir);
    if (!req.reimport_rels.empty()) {
      db.scan();
      for (const std::string& rel : req.reimport_rels) {
        if (cancel_.load()) break;
        db.reimport(rel);
      }
    } else {
      db.scan();
    }
    render3d::set_import_monitor(nullptr);
    if (cancel_.load()) {
      finish(false, true, "Import cancelled.", copied);
      return;
    }
    std::string err;
    for (const assetdb::AssetDb::Entry& e : db.entries()) {
      if (!e.import_ok && !e.import_error.empty() && e.meta.kind == assetdb::Kind::Model) {
        bool ours = false;
        for (const std::string& rel : copied.mains)
          if (rel == e.rel) ours = true;
        for (const std::string& rel : req.reimport_rels)
          if (rel == e.rel) ours = true;
        if (ours) err = e.import_error;
      }
    }
    const bool ok = err.empty() || err.rfind("warning:", 0) == 0;
    finish(ok, false, ok ? std::string() : err, copied);
  } catch (const std::bad_alloc&) {
    render3d::set_import_monitor(nullptr);
    finish(false, false, "Out of memory while importing. The editor is still open.", copied);
  } catch (const std::exception& ex) {
    render3d::set_import_monitor(nullptr);
    finish(false, false, std::string("Import failed (") + ex.what() + ")", copied);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
