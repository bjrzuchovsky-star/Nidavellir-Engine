#include "story/Dialogue.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace story {

namespace {

std::vector<std::string> strings(const json::Value& v) {
  std::vector<std::string> out;
  if (v.is_string()) out.push_back(v.as_string());
  for (const json::Value& x : v.items()) {
    if (x.is_string()) out.push_back(x.as_string());
  }
  return out;
}

json::Value string_array(const std::vector<std::string>& v) {
  json::Value a = json::Value::array();
  for (const auto& s : v) a.push(json::Value::string(s));
  return a;
}

}  // namespace

const DialogueNode* Dialogue::node(const std::string& nid) const {
  for (const DialogueNode& n : nodes) {
    if (n.id == nid) return &n;
  }
  return nullptr;
}

DialogueNode* Dialogue::node(const std::string& nid) {
  for (DialogueNode& n : nodes) {
    if (n.id == nid) return &n;
  }
  return nullptr;
}

bool parse_dialogue(const std::string& text, const std::string& file, Dialogue* out, std::string* error) {
  json::Value v;
  std::string err;
  if (!json::parse(text, &v, &err)) {
    if (error) *error = file + ": " + err;
    return false;
  }
  Dialogue d;
  d.file = file;
  d.id = v.str("id");
  if (d.id.empty()) {
    std::string stem = fs::path(file).filename().string();
    const std::size_t dot = stem.find('.');
    d.id = dot == std::string::npos ? stem : stem.substr(0, dot);
  }
  for (const auto& [k, s] : v["speakers"].members()) {
    Speaker sp;
    sp.name = s.is_string() ? s.as_string() : s.str("name", k);
    sp.portrait = s.str("portrait");
    d.speakers[k] = sp;
  }
  for (const json::Value& n : v["nodes"].items()) {
    DialogueNode node;
    node.id = n.str("id");
    if (node.id.empty()) {
      if (error) *error = file + ": a node needs an \"id\"";
      return false;
    }
    node.speaker = n.str("speaker");
    node.text = n.str("text");
    node.condition = n.str("if");
    node.actions = strings(n["actions"]);
    node.next = n.str("next");
    node.x = n.flt("x", 0);
    node.y = n.flt("y", 0);
    for (const json::Value& c : n["choices"].items()) {
      DialogueChoice ch;
      ch.text = c.str("text");
      ch.next = c.str("next");
      ch.condition = c.str("if");
      ch.actions = strings(c["actions"]);
      ch.once = c.flag("once", false);
      node.choices.push_back(std::move(ch));
    }
    d.nodes.push_back(std::move(node));
  }
  d.start = v.str("start", d.nodes.empty() ? std::string() : d.nodes.front().id);
  *out = std::move(d);
  return true;
}

json::Value dialogue_to_json(const Dialogue& d) {
  json::Value v = json::Value::object();
  v.set("id", json::Value::string(d.id));
  v.set("start", json::Value::string(d.start));
  json::Value sp = json::Value::object();
  for (const auto& [k, s] : d.speakers) {
    json::Value j = json::Value::object();
    j.set("name", json::Value::string(s.name));
    if (!s.portrait.empty()) j.set("portrait", json::Value::string(s.portrait));
    sp.set(k, j);
  }
  v.set("speakers", sp);
  json::Value nodes = json::Value::array();
  for (const DialogueNode& n : d.nodes) {
    json::Value j = json::Value::object();
    j.set("id", json::Value::string(n.id));
    if (!n.speaker.empty()) j.set("speaker", json::Value::string(n.speaker));
    j.set("text", json::Value::string(n.text));
    if (!n.condition.empty()) j.set("if", json::Value::string(n.condition));
    if (!n.actions.empty()) j.set("actions", string_array(n.actions));
    if (!n.choices.empty()) {
      json::Value cs = json::Value::array();
      for (const DialogueChoice& c : n.choices) {
        json::Value cj = json::Value::object();
        cj.set("text", json::Value::string(c.text));
        cj.set("next", json::Value::string(c.next));
        if (!c.condition.empty()) cj.set("if", json::Value::string(c.condition));
        if (!c.actions.empty()) cj.set("actions", string_array(c.actions));
        if (c.once) cj.set("once", json::Value::boolean(true));
        cs.push(cj);
      }
      j.set("choices", cs);
    } else {
      j.set("next", json::Value::string(n.next));
    }
    j.set("x", json::Value::number(std::round(n.x)));
    j.set("y", json::Value::number(std::round(n.y)));
    nodes.push(j);
  }
  v.set("nodes", nodes);
  return v;
}

std::vector<std::string> lint_dialogue(const Dialogue& d) {
  std::vector<std::string> out;
  const std::string pre = d.id + ": ";
  std::set<std::string> ids;
  for (const DialogueNode& n : d.nodes) {
    if (!ids.insert(n.id).second) out.push_back(pre + "node " + n.id + " defined twice");
  }
  if (d.nodes.empty()) out.push_back(pre + "no nodes");
  if (!d.start.empty() && !ids.count(d.start)) out.push_back(pre + "start node " + d.start + " is missing");
  auto check_cond = [&](const std::string& where, const std::string& c) {
    std::string e;
    if (!c.empty() && !check_condition(c, &e)) out.push_back(pre + where + ": " + e);
  };
  auto check_actions = [&](const std::string& where, const std::vector<std::string>& acts) {
    for (const std::string& a : acts) {
      Action x;
      std::string e;
      if (!parse_action(a, &x, &e)) out.push_back(pre + where + ": " + e);
    }
  };
  for (const DialogueNode& n : d.nodes) {
    if (!n.speaker.empty() && !d.speakers.count(n.speaker)) {
      out.push_back(pre + "node " + n.id + " speaker " + n.speaker + " is not in \"speakers\"");
    }
    if (!n.next.empty() && !ids.count(n.next)) out.push_back(pre + "node " + n.id + " -> missing " + n.next);
    check_cond("node " + n.id, n.condition);
    check_actions("node " + n.id, n.actions);
    for (std::size_t i = 0; i < n.choices.size(); ++i) {
      const DialogueChoice& c = n.choices[i];
      const std::string where = "node " + n.id + " choice " + std::to_string(i + 1);
      if (!c.next.empty() && !ids.count(c.next)) out.push_back(pre + where + " -> missing " + c.next);
      if (c.text.empty()) out.push_back(pre + where + " has no text");
      check_cond(where, c.condition);
      check_actions(where, c.actions);
    }
  }
  // Reachability from start.
  std::set<std::string> seen;
  std::vector<std::string> stack;
  if (ids.count(d.start)) stack.push_back(d.start);
  while (!stack.empty()) {
    const std::string id = stack.back();
    stack.pop_back();
    if (!seen.insert(id).second) continue;
    const DialogueNode* n = d.node(id);
    if (!n) continue;
    if (!n->next.empty()) stack.push_back(n->next);
    for (const DialogueChoice& c : n->choices) {
      if (!c.next.empty()) stack.push_back(c.next);
    }
  }
  for (const DialogueNode& n : d.nodes) {
    if (!seen.count(n.id)) out.push_back(pre + "node " + n.id + " is unreachable from " + d.start);
  }
  return out;
}

// ---- Runner -----------------------------------------------------------------

int DialogueRunner::load_dir(const std::string& project_dir, std::string* errors) {
  const fs::path dir = fs::path(project_dir) / "dialogue";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file(ec) && n.size() > 9 && n.substr(n.size() - 9) == ".dlg.json") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  int n = 0;
  for (const fs::path& p : files) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Dialogue d;
    std::string err;
    if (!parse_dialogue(ss.str(), fs::relative(p, project_dir, ec).generic_string(), &d, &err)) {
      if (errors) *errors += err + "\n";
      continue;
    }
    add(std::move(d));
    ++n;
  }
  return n;
}

void DialogueRunner::add(Dialogue d) {
  const std::string id = d.id;
  lib_[id] = std::move(d);
}

const Dialogue* DialogueRunner::find(const std::string& id) const {
  const auto it = lib_.find(id);
  return it == lib_.end() ? nullptr : &it->second;
}

std::vector<std::string> DialogueRunner::ids() const {
  std::vector<std::string> out;
  for (const auto& [k, v] : lib_) out.push_back(k);
  return out;
}

bool DialogueRunner::start(const std::string& id, const std::string& at_node, std::string* error) {
  const Dialogue* d = find(id);
  if (!d) {
    if (error) *error = "no dialogue \"" + id + "\"";
    return false;
  }
  if (active()) finish();
  dlg_ = id;
  const std::string first = at_node.empty() ? d->start : at_node;
  if (!d->node(first)) {
    dlg_.clear();
    if (error) *error = "dialogue " + id + " has no node \"" + first + "\"";
    return false;
  }
  enter(first, 0);
  return true;
}

std::string DialogueRunner::seen_key(const std::string& node, int choice) const {
  return dlg_ + "/" + node + "/" + std::to_string(choice);
}

bool DialogueRunner::enter(const std::string& node_id, int depth) {
  const Dialogue* d = find(dlg_);
  if (!d || node_id.empty() || depth > 64) {
    finish();
    return false;
  }
  const DialogueNode* n = d->node(node_id);
  if (!n) {
    finish();
    return false;
  }
  if (!n->condition.empty() && !eval_condition(n->condition, env)) return enter(n->next, depth + 1);
  const std::string here = dlg_;
  for (const std::string& a : n->actions) {
    if (run_action) run_action(a);
    if (dlg_ != here) return false;  // an action ended / replaced the talk
  }
  line_ = Line{};
  line_.node = n->id;
  const auto sp = d->speakers.find(n->speaker);
  line_.speaker = sp != d->speakers.end() ? sp->second.name : n->speaker;
  line_.portrait = sp != d->speakers.end() ? sp->second.portrait : std::string();
  line_.text = vars ? vars->format(n->text) : n->text;
  for (std::size_t i = 0; i < n->choices.size(); ++i) {
    const DialogueChoice& c = n->choices[i];
    if (c.once && taken_.count(seen_key(n->id, static_cast<int>(i)))) continue;
    if (!c.condition.empty() && !eval_condition(c.condition, env)) continue;
    line_.choices.push_back(vars ? vars->format(c.text) : c.text);
    line_.choice_index.push_back(static_cast<int>(i));
  }
  ++lines_shown_;
  // Choices all hidden and nowhere to go: still show the line; continuing
  // follows next (which may end the talk).
  return true;
}

bool DialogueRunner::choose(int visible_index) {
  if (!active() || visible_index < 0 || visible_index >= static_cast<int>(line_.choice_index.size())) return false;
  const Dialogue* d = find(dlg_);
  const DialogueNode* n = d ? d->node(line_.node) : nullptr;
  if (!n) return false;
  const int ci = line_.choice_index[static_cast<std::size_t>(visible_index)];
  const DialogueChoice& c = n->choices[static_cast<std::size_t>(ci)];
  taken_.insert(seen_key(n->id, ci));
  const std::string next = c.next;
  const std::string here = dlg_;
  for (const std::string& a : c.actions) {
    if (run_action) run_action(a);
    if (dlg_ != here) return true;
  }
  if (next.empty()) {
    finish();
    return true;
  }
  enter(next, 0);
  return true;
}

bool DialogueRunner::advance() {
  if (!active()) return false;
  if (!line_.choices.empty()) return false;
  const Dialogue* d = find(dlg_);
  const DialogueNode* n = d ? d->node(line_.node) : nullptr;
  if (!n || n->next.empty()) {
    finish();
    return true;
  }
  enter(n->next, 0);
  return true;
}

void DialogueRunner::stop() {
  if (active()) finish();
}

void DialogueRunner::finish() {
  if (!dlg_.empty()) ended_.push_back(dlg_);
  dlg_.clear();
  line_ = Line{};
}

std::vector<std::string> DialogueRunner::take_ended() {
  std::vector<std::string> out;
  out.swap(ended_);
  return out;
}

json::Value DialogueRunner::to_json() const {
  json::Value v = json::Value::object();
  json::Value t = json::Value::array();
  for (const auto& k : taken_) t.push(json::Value::string(k));
  v.set("taken", t);
  return v;
}

void DialogueRunner::from_json(const json::Value& v) {
  taken_.clear();
  for (const json::Value& k : v["taken"].items()) {
    if (k.is_string()) taken_.insert(k.as_string());
  }
  dlg_.clear();
  line_ = Line{};
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
