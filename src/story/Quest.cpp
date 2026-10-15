#include "story/Quest.h"

#include "story/Vars.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace story {

namespace {

bool parse_one(const json::Value& v, const std::string& file, QuestDef* q, std::string* error) {
  q->id = v.str("id");
  if (q->id.empty()) {
    if (error) *error = file + ": a quest needs an \"id\"";
    return false;
  }
  q->title = v.str("title", q->id);
  q->description = v.str("description");
  q->giver = v.str("giver");
  q->next = v.str("next");
  q->auto_complete = v.flag("auto_complete", true);
  q->file = file;
  for (const json::Value& r : v["requires"].items()) {
    if (r.is_string()) q->prereqs.push_back(r.as_string());
  }
  for (const json::Value& r : v["rewards"].items()) {
    if (r.is_string()) q->rewards.push_back(r.as_string());
  }
  for (const json::Value& o : v["objectives"].items()) {
    ObjectiveDef d;
    d.id = o.str("id");
    d.text = o.str("text", d.id);
    d.count = static_cast<int>(o.num("count", 1));
    d.optional = o.flag("optional", false);
    d.hidden = o.flag("hidden", false);
    if (d.id.empty()) {
      if (error) *error = file + ": quest " + q->id + " has an objective without an \"id\"";
      return false;
    }
    q->objectives.push_back(std::move(d));
  }
  return true;
}

}  // namespace

const ObjectiveDef* QuestDef::objective(const std::string& oid) const {
  for (const ObjectiveDef& o : objectives) {
    if (o.id == oid) return &o;
  }
  return nullptr;
}

bool parse_quests(const std::string& text, const std::string& file, std::vector<QuestDef>* out,
                  std::string* error) {
  json::Value v;
  std::string err;
  if (!json::parse(text, &v, &err)) {
    if (error) *error = file + ": " + err;
    return false;
  }
  if (v.has("quests")) {
    for (const json::Value& q : v["quests"].items()) {
      QuestDef d;
      if (!parse_one(q, file, &d, error)) return false;
      out->push_back(std::move(d));
    }
    return true;
  }
  QuestDef d;
  if (!parse_one(v, file, &d, error)) return false;
  out->push_back(std::move(d));
  return true;
}

json::Value quest_to_json(const QuestDef& q) {
  json::Value v = json::Value::object();
  v.set("id", json::Value::string(q.id));
  v.set("title", json::Value::string(q.title));
  if (!q.giver.empty()) v.set("giver", json::Value::string(q.giver));
  if (!q.description.empty()) v.set("description", json::Value::string(q.description));
  if (!q.prereqs.empty()) {
    json::Value r = json::Value::array();
    for (const auto& x : q.prereqs) r.push(json::Value::string(x));
    v.set("requires", r);
  }
  json::Value objs = json::Value::array();
  for (const ObjectiveDef& o : q.objectives) {
    json::Value j = json::Value::object();
    j.set("id", json::Value::string(o.id));
    j.set("text", json::Value::string(o.text));
    if (o.count != 1) j.set("count", json::Value::number(o.count));
    if (o.optional) j.set("optional", json::Value::boolean(true));
    if (o.hidden) j.set("hidden", json::Value::boolean(true));
    objs.push(j);
  }
  v.set("objectives", objs);
  if (!q.rewards.empty()) {
    json::Value r = json::Value::array();
    for (const auto& x : q.rewards) r.push(json::Value::string(x));
    v.set("rewards", r);
  }
  if (!q.next.empty()) v.set("next", json::Value::string(q.next));
  if (!q.auto_complete) v.set("auto_complete", json::Value::boolean(false));
  return v;
}

std::vector<std::string> lint_quests(const std::vector<QuestDef>& quests, const std::vector<std::string>& known) {
  std::vector<std::string> out;
  std::set<std::string> ids;
  for (const QuestDef& q : quests) {
    if (!ids.insert(q.id).second) out.push_back("quest " + q.id + " is defined twice");
  }
  ids.insert(known.begin(), known.end());
  for (const QuestDef& q : quests) {
    for (const auto& r : q.prereqs) {
      if (!ids.count(r)) out.push_back("quest " + q.id + " requires unknown quest " + r);
      if (r == q.id) out.push_back("quest " + q.id + " requires itself");
    }
    if (!q.next.empty() && !ids.count(q.next)) out.push_back("quest " + q.id + " chains to unknown quest " + q.next);
    std::set<std::string> oids;
    for (const ObjectiveDef& o : q.objectives) {
      if (!oids.insert(o.id).second) out.push_back("quest " + q.id + ": objective " + o.id + " twice");
      if (o.count < 1) out.push_back("quest " + q.id + ": objective " + o.id + " count < 1");
    }
    if (q.objectives.empty()) out.push_back("quest " + q.id + " has no objectives");
    for (const auto& r : q.rewards) {
      Action a;
      std::string e;
      if (!parse_action(r, &a, &e)) out.push_back("quest " + q.id + ": " + e);
    }
  }
  return out;
}

const char* quest_status_name(QuestStatus s) {
  switch (s) {
    case QuestStatus::Inactive: return "inactive";
    case QuestStatus::Active: return "active";
    case QuestStatus::Completed: return "completed";
    case QuestStatus::Failed: return "failed";
  }
  return "inactive";
}

bool quest_status_from(const std::string& s, QuestStatus* out) {
  for (QuestStatus x : {QuestStatus::Inactive, QuestStatus::Active, QuestStatus::Completed, QuestStatus::Failed}) {
    if (s == quest_status_name(x)) {
      *out = x;
      return true;
    }
  }
  return false;
}

int QuestLog::load_dir(const std::string& project_dir, std::string* errors) {
  const fs::path dir = fs::path(project_dir) / "quests";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file(ec) && n.size() > 11 && n.substr(n.size() - 11) == ".quest.json") {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  int n = 0;
  for (const fs::path& p : files) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<QuestDef> qs;
    std::string err;
    const std::string rel = fs::relative(p, project_dir, ec).generic_string();
    if (!parse_quests(ss.str(), rel, &qs, &err)) {
      if (errors) *errors += err + "\n";
      continue;
    }
    for (QuestDef& q : qs) {
      add(std::move(q));
      ++n;
    }
  }
  return n;
}

void QuestLog::add(QuestDef q) {
  const std::string id = q.id;
  defs_[id] = std::move(q);
}

const QuestDef* QuestLog::def(const std::string& id) const {
  const auto it = defs_.find(id);
  return it == defs_.end() ? nullptr : &it->second;
}

std::vector<const QuestDef*> QuestLog::defs() const {
  std::vector<const QuestDef*> out;
  for (const auto& [k, v] : defs_) out.push_back(&v);
  return out;
}

bool QuestLog::start(const std::string& id, std::string* why) {
  const QuestDef* d = def(id);
  if (!d) {
    if (why) *why = "no quest \"" + id + "\"";
    return false;
  }
  Entry& e = ent(id);
  if (e.status == QuestStatus::Active || e.status == QuestStatus::Completed) {
    if (why) *why = "quest " + id + " is already " + quest_status_name(e.status);
    return false;
  }
  for (const auto& r : d->prereqs) {
    if (status(r) != QuestStatus::Completed) {
      if (why) *why = "quest " + id + " needs " + r + " completed first";
      return false;
    }
  }
  e.status = QuestStatus::Active;
  e.progress.clear();
  e.order = next_order_++;
  if (tracked_.empty() || status(tracked_) != QuestStatus::Active) tracked_ = id;
  events_.push_back({Event::Kind::Started, id, {}, 0});
  check_done(id);  // a quest with nothing required finishes at once
  return true;
}

bool QuestLog::progress(const std::string& quest, const std::string& objective, int n) {
  return set_progress(quest, objective, objective_progress(quest, objective) + n);
}

bool QuestLog::set_progress(const std::string& quest, const std::string& objective, int value) {
  const QuestDef* d = def(quest);
  if (!d || status(quest) != QuestStatus::Active) return false;
  const ObjectiveDef* o = d->objective(objective);
  if (!o) return false;
  value = std::clamp(value, 0, o->count);
  int& cur = ent(quest).progress[objective];
  if (cur == value) return true;
  cur = value;
  events_.push_back({Event::Kind::Objective, quest, objective, value});
  check_done(quest);
  return true;
}

void QuestLog::check_done(const std::string& id) {
  const QuestDef* d = def(id);
  if (d && d->auto_complete && status(id) == QuestStatus::Active && ready(id)) complete(id);
}

bool QuestLog::complete(const std::string& id) {
  const QuestDef* d = def(id);
  if (!d || status(id) != QuestStatus::Active) return false;
  ent(id).status = QuestStatus::Completed;
  events_.push_back({Event::Kind::Completed, id, {}, 0});
  if (run_action) {
    for (const std::string& r : d->rewards) run_action(r);
  }
  if (tracked_ == id) tracked_.clear();
  if (!d->next.empty()) start(d->next);
  if (tracked_.empty()) {
    for (const std::string& q : journal_order()) {
      if (status(q) == QuestStatus::Active) {
        tracked_ = q;
        break;
      }
    }
  }
  return true;
}

bool QuestLog::fail(const std::string& id) {
  if (status(id) != QuestStatus::Active) return false;
  ent(id).status = QuestStatus::Failed;
  if (tracked_ == id) tracked_.clear();
  events_.push_back({Event::Kind::Failed, id, {}, 0});
  return true;
}

void QuestLog::reset() {
  state_.clear();
  events_.clear();
  tracked_.clear();
  next_order_ = 1;
}

QuestStatus QuestLog::status(const std::string& id) const {
  const auto it = state_.find(id);
  return it == state_.end() ? QuestStatus::Inactive : it->second.status;
}

int QuestLog::objective_progress(const std::string& quest, const std::string& objective) const {
  const auto it = state_.find(quest);
  if (it == state_.end()) return 0;
  const auto p = it->second.progress.find(objective);
  return p == it->second.progress.end() ? 0 : p->second;
}

bool QuestLog::objective_done(const std::string& quest, const std::string& objective) const {
  const QuestDef* d = def(quest);
  const ObjectiveDef* o = d ? d->objective(objective) : nullptr;
  return o && objective_progress(quest, objective) >= o->count;
}

bool QuestLog::ready(const std::string& id) const {
  const QuestDef* d = def(id);
  if (!d) return false;
  for (const ObjectiveDef& o : d->objectives) {
    if (!o.optional && !objective_done(id, o.id)) return false;
  }
  return true;
}

const QuestLog::Entry* QuestLog::entry(const std::string& id) const {
  const auto it = state_.find(id);
  return it == state_.end() ? nullptr : &it->second;
}

std::vector<std::string> QuestLog::journal_order() const {
  std::vector<std::pair<std::string, const Entry*>> v;
  for (const auto& [k, e] : state_) {
    if (e.status != QuestStatus::Inactive && def(k)) v.push_back({k, &e});
  }
  auto rank = [](QuestStatus s) { return s == QuestStatus::Active ? 0 : s == QuestStatus::Completed ? 1 : 2; };
  std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
    if (rank(a.second->status) != rank(b.second->status)) return rank(a.second->status) < rank(b.second->status);
    return a.second->order > b.second->order;
  });
  std::vector<std::string> out;
  for (const auto& x : v) out.push_back(x.first);
  return out;
}

std::vector<QuestLog::Event> QuestLog::take_events() {
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

json::Value QuestLog::to_json() const {
  json::Value v = json::Value::object();
  json::Value qs = json::Value::object();
  for (const auto& [k, e] : state_) {
    if (e.status == QuestStatus::Inactive) continue;
    json::Value j = json::Value::object();
    j.set("status", json::Value::string(quest_status_name(e.status)));
    j.set("order", json::Value::number(static_cast<double>(e.order)));
    json::Value p = json::Value::object();
    for (const auto& [o, n] : e.progress) p.set(o, json::Value::number(n));
    j.set("progress", p);
    qs.set(k, j);
  }
  v.set("quests", qs);
  v.set("tracked", json::Value::string(tracked_));
  return v;
}

void QuestLog::from_json(const json::Value& v) {
  state_.clear();
  events_.clear();
  next_order_ = 1;
  for (const auto& [k, j] : v["quests"].members()) {
    Entry e;
    quest_status_from(j.str("status", "inactive"), &e.status);
    e.order = static_cast<std::uint64_t>(j.num("order", 0));
    for (const auto& [o, n] : j["progress"].members()) e.progress[o] = static_cast<int>(n.as_number());
    next_order_ = std::max(next_order_, e.order + 1);
    state_[k] = std::move(e);
  }
  tracked_ = v.str("tracked");
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
