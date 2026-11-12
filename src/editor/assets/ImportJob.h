#pragma once

// Background model import. Copying a large .blend, asking Blender to export
// it and fitting a collider used to run on the UI thread, so the editor
// froze and then died if the allocation failed. The job does that work on
// a worker; the UI polls state() and draws a progress bar and Cancel.

#include "editor/assets/ImportSort.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class ImportJob {
 public:
  struct Request {
    std::string project_dir;
    std::vector<std::string> sources;       // outside files to copy in
    std::string dest_folder;
    importsort::Settings settings;
    std::vector<std::string> reimport_rels;  // already in the project
    bool split = false;
    bool split_recenter = false;
    bool split_fit = true;
  };

  struct State {
    bool running = false;
    bool finished = false;
    bool ok = false;
    bool cancelled = false;
    int percent = 0;
    std::string phase;
    std::string error;
    std::vector<std::string> rels;  // files this job was importing
    importsort::Result copied;
    bool split = false;
    bool split_recenter = false;
    bool split_fit = true;
  };

  ImportJob() = default;
  ~ImportJob();
  ImportJob(const ImportJob&) = delete;
  ImportJob& operator=(const ImportJob&) = delete;

  void start(Request req);
  void cancel();
  State state() const;
  // Main thread. True once, when the worker has finished since the last take.
  bool take(State* out);

 private:
  void join();
  void run(Request req);

  mutable std::mutex mu_;
  State state_;
  std::atomic<bool> cancel_{false};
  std::thread thread_;
  bool taken_ = true;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
