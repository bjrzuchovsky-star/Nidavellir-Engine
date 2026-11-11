#include "editor/console/TelegraphLog.h"
#include "editor/ux/ShotOpts.h"

#include <cctype>
#include <utility>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

std::string lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

std::size_t TelegraphLog::slot(runtime::LogLevel level) {
  switch (level) {
    case runtime::LogLevel::Warn:
      return 1;
    case runtime::LogLevel::Error:
      return 2;
    case runtime::LogLevel::Info:
    default:
      return 0;
  }
}

void TelegraphLog::add(runtime::LogEntry entry) {
  // --shot-clean: keep internal paths (.tombstone/meta and the like) off
  // the Telegraph so a store screenshot is not a debug log.
  if (shot::hide_log_line(entry.text)) return;
  const std::size_t s = slot(entry.level);
  Line line;
  line.entry = std::move(entry);
  line.seq = ++seq_;
  lines_.push_back(std::move(line));
  ++counts_[s];
  ++unseen_[s];
  while (lines_.size() > kMaxLines) {
    --counts_[slot(lines_.front().entry.level)];
    lines_.pop_front();
  }
}

void TelegraphLog::add(runtime::LogLevel level, std::string channel,
                       std::string text) {
  runtime::LogEntry e;
  e.level = level;
  e.channel = std::move(channel);
  e.text = std::move(text);
  add(std::move(e));
}

void TelegraphLog::clear() {
  lines_.clear();
  counts_ = {};
  unseen_ = {};
}

std::size_t TelegraphLog::count(runtime::LogLevel level) const {
  return counts_[slot(level)];
}

std::size_t TelegraphLog::unseen(runtime::LogLevel level) const {
  return unseen_[slot(level)];
}

void TelegraphLog::mark_seen() { unseen_ = {}; }

bool TelegraphLog::passes(const Line& line) const {
  switch (line.entry.level) {
    case runtime::LogLevel::Info:
      if (!show_info) return false;
      break;
    case runtime::LogLevel::Warn:
      if (!show_warn) return false;
      break;
    case runtime::LogLevel::Error:
      if (!show_error) return false;
      break;
  }
  if (search.empty()) {
    return true;
  }
  return lower(runtime::format_log(line.entry, true)).find(lower(search)) !=
         std::string::npos;
}

std::vector<std::size_t> TelegraphLog::visible() const {
  std::vector<std::size_t> out;
  out.reserve(lines_.size());
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (passes(lines_[i])) {
      out.push_back(i);
    }
  }
  return out;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
