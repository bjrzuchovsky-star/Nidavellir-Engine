#include "team/TeamClient.h"

#include "team/TeamServer.h"

#include <algorithm>
#include <set>

namespace ts {
namespace tombstone {
namespace team {

namespace {

Value str(const std::string& s) { return Value::string(s); }
Value num(double d) { return Value::number(d); }

RemoteMember member_from(const Value& v) {
  RemoteMember m;
  m.id = static_cast<std::uint32_t>(v.num("id", 0));
  m.name = v.str("name");
  m.colour = v.str("colour");
  m.can_write = v.flag("can_write", true);
  m.scene = v.str("scene");
  return m;
}

}  // namespace

TeamClient::TeamClient() : t0_(std::chrono::steady_clock::now()) {}
TeamClient::~TeamClient() { leave(); }

double TeamClient::now() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

bool TeamClient::join(const JoinOptions& opt, std::string* error) {
  leave();
  // A fresh ride: nothing from an earlier (lost) connection carries over.
  pending_.clear();
  apply_.clear();
  previews_.clear();
  members_.clear();
  locks_.clear();
  welcome_pending_ = false;
  rode_back_ = false;
  opt_ = opt;
  error_.clear();
  warn_.clear();
  std::unique_ptr<net::Transport> t = net::make_enet_transport();
  if (opt_.wrap_transport) t = opt_.wrap_transport(std::move(t));
  std::string err;
  server_ = t->connect(opt_.host, opt_.port, &err);
  if (server_ == net::kNoPeer) {
    error_ = err.empty() ? "Could not reach " + opt_.host : err;
    if (error) *error = error_;
    state_ = State::Closed;
    return false;
  }
  transport_ = std::move(t);
  state_ = State::Connecting;
  connect_started_ = now();
  return true;
}

void TeamClient::send(const Value& msg, bool reliable) {
  if (!transport_ || server_ == net::kNoPeer) return;
  auto bytes = encode(msg);
  bytes_sent_ += bytes.size();
  transport_->send(server_, reliable ? net::Delivery::Reliable : net::Delivery::Unreliable, std::move(bytes));
}

void TeamClient::leave() {
  rpc_->close("You rode off from the posse.");
  if (!transport_) return;
  if (state_ == State::Joined) {
    Value bye = Value::object();
    bye.set("t", str("bye"));
    send(bye);
    transport_->disconnect(server_, 0);
    // Give the goodbye a moment on the wire.
    const double until = now() + 0.25;
    net::TransportEvent ev;
    while (now() < until) {
      transport_->flush();
      if (transport_->poll(&ev) && ev.type == net::TransportEvent::Type::Disconnected) break;
    }
  }
  transport_->close();
  transport_.reset();
  server_ = net::kNoPeer;
  if (state_ == State::Joined || state_ == State::Connecting) state_ = State::Closed;
  members_.clear();
  locks_.clear();
  pending_.clear();
}

void TeamClient::drop_now() {
  rpc_->close("The connection to the team host was lost.");
  if (!transport_) return;
  transport_->close();
  transport_.reset();
  server_ = net::kNoPeer;
  state_ = State::Closed;
  members_.clear();
  locks_.clear();
  pending_.clear();
}

void TeamClient::poll() {
  if (!transport_) return;
  net::TransportEvent ev;
  while (transport_ && transport_->poll(&ev)) {
    switch (ev.type) {
      case net::TransportEvent::Type::Connected: {
        Value h = Value::object();
        h.set("t", str("hello"));
        h.set("proto", num(kProtocolVersion));
        h.set("claim", str(opt_.claim));
        h.set("claim_name", str(opt_.claim_name));
        h.set("user", str(opt_.user));
        if (!opt_.key.empty()) h.set("key", str(opt_.key));
        if (!opt_.pass.empty()) h.set("pass", str(opt_.pass));
        if (!opt_.rejoin.empty()) h.set("rejoin", str(opt_.rejoin));
        h.set("engine", str(opt_.engine));
        Value f = Value::object();
        f.set("scene2d", num(opt_.scene2d));
        f.set("scene3d", num(opt_.scene3d));
        h.set("formats", std::move(f));
        h.set("dim", str(opt_.dim));
        h.set("scene", str(opt_.scene));
        h.set("seed", docs_to_json(opt_.seed));
        send(h);
        break;
      }
      case net::TransportEvent::Type::Disconnected:
        rpc_->close("The connection to the team host was lost.");
        if (state_ != State::Refused) {
          state_ = State::Closed;
          if (error_.empty()) error_ = "The connection to the team host was lost.";
        }
        transport_->close();
        transport_.reset();
        server_ = net::kNoPeer;
        members_.clear();
        locks_.clear();
        notes_.push_back(state_ == State::Refused ? "Team: " + error_
                                                  : "Team: lost the connection to the host.");
        return;
      case net::TransportEvent::Type::Received: {
        Value msg;
        if (decode(ev.data, &msg)) on_message(msg);
        break;
      }
    }
  }
  if (state_ == State::Joined && transport_) {
    for (Value& req : rpc_->take_outgoing()) send(req);
  }
  if (state_ == State::Connecting && now() - connect_started_ > 10.0) {
    error_ = "No answer from " + opt_.host + ":" + std::to_string(opt_.port) + ".";
    notes_.push_back("Team: " + error_);
    drop_now();
    return;
  }
  if (transport_) transport_->flush();
}

void TeamClient::on_message(const Value& msg) {
  const std::string t = msg.str("t");
  if (t == "refuse") {
    state_ = State::Refused;
    error_ = msg.str("reason", "Refused by the team host.");
    return;
  }
  if (t == "welcome") {
    me_ = static_cast<std::uint32_t>(msg.num("member", 0));
    my_name_ = msg.str("name");
    my_colour_ = msg.str("colour");
    can_write_ = msg.flag("can_write", true);
    id_lo_ = id_of(msg["ids"]["lo"]);
    id_hi_ = id_of(msg["ids"]["hi"]);
    session_ = msg["session"];
    rejoin_token_ = msg.str("rejoin");
    rode_back_ = msg.flag("back", false);
    warn_ = msg.str("warn");
    docs_from_json(msg["docs"], &confirmed_);
    seq_ = static_cast<std::uint64_t>(msg.num("snap_seq", 0));
    for (const Value& bv : msg["tail"].items()) {
      Batch b;
      if (batch_from_json(bv, &b) && b.seq > seq_) {
        confirmed_[b.scene].apply(b);
        seq_ = b.seq;
      }
    }
    members_.clear();
    for (const Value& mv : msg["members"].items()) {
      RemoteMember m = member_from(mv);
      if (m.id != me_) members_[m.id] = std::move(m);
    }
    locks_.clear();
    for (const auto& [k, v] : msg["locks"].members()) {
      locks_[k] = static_cast<std::uint32_t>(v.as_number());
    }
    state_ = State::Joined;
    welcome_pending_ = true;
    rpc_->open();
    want_sent_ = false;
    if (!want_.empty()) want_locks(want_);
    if (!warn_.empty()) notes_.push_back("Team: " + warn_);
    return;
  }
  if (state_ != State::Joined) return;
  if (t == "blob_r") {
    rpc_->deliver(msg);
    return;
  }
  if (t == "commit") {
    Batch b;
    if (batch_from_json(msg, &b)) on_commit(b);
    return;
  }
  if (t == "reject") {
    on_reject(static_cast<std::uint64_t>(msg.num("cid", 0)), msg.str("reason"));
    return;
  }
  if (t == "locks") {
    locks_.clear();
    for (const auto& [k, v] : msg["locks"].members()) {
      locks_[k] = static_cast<std::uint32_t>(v.as_number());
    }
    // Something we have selected came free (its holder let go or rode
    // off): ask again so the selection gets its lock back.
    if (can_write_) {
      for (const std::string& k : want_) {
        if (!locks_.count(k)) {
          want_sent_ = false;
          want_locks(want_);
          break;
        }
      }
    }
    return;
  }
  if (t == "lock_denied") {
    // Plain requests: the lock table already shows it. Take-overs say why.
    const std::string why = msg.str("reason");
    if (!why.empty()) notes_.push_back("Can't take it over: " + why);
    return;
  }
  if (t == "taken") {
    const auto by = static_cast<std::uint32_t>(msg.num("by", 0));
    const auto from = static_cast<std::uint32_t>(msg.num("from", 0));
    auto name = [&](std::uint32_t id) {
      if (id == me_) return std::string("you");
      const RemoteMember* rm = member(id);
      return rm ? rm->name : std::string("a rider");
    };
    if (from == me_) notes_.push_back(name(by) + " took over something you had idle (" + msg.str("key") + ").");
    else if (by == me_) notes_.push_back("You took it over from " + name(from) + ".");
    return;
  }
  if (t == "member") {
    RemoteMember m = member_from(msg["m"]);
    if (m.id == me_ || !m.id) return;
    auto it = members_.find(m.id);
    if (it == members_.end()) {
      notes_.push_back(m.name + " rode in.");
      members_[m.id] = std::move(m);
    } else {
      Value p = it->second.presence;
      const bool hp = it->second.has_presence;
      it->second = std::move(m);
      it->second.presence = std::move(p);
      it->second.has_presence = hp;
    }
    return;
  }
  if (t == "left") {
    auto it = members_.find(static_cast<std::uint32_t>(msg.num("member", 0)));
    if (it != members_.end()) {
      notes_.push_back(it->second.name + " rode off.");
      members_.erase(it);
    }
    return;
  }
  if (t == "prev" || t == "prev_end") {
    PreviewEvent e;
    e.member = static_cast<std::uint32_t>(msg.num("member", 0));
    e.scene = msg.str("scene");
    e.n = static_cast<std::uint64_t>(msg.num("n", 0));
    e.end = t == "prev_end";
    if (e.member == me_ || !e.member) return;
    if (e.end) {
      for (const Value& v : msg["ids"].items()) e.ids.push_back(id_of(v));
    } else {
      for (const Value& ov : msg["ops"].items()) {
        Op op;
        if (op_from_json(ov, &op) && op.kind == Op::Kind::Put) e.ops.push_back(std::move(op));
      }
    }
    previews_.push_back(std::move(e));
    return;
  }
  if (t == "pres") {
    auto it = members_.find(static_cast<std::uint32_t>(msg.num("member", 0)));
    if (it != members_.end()) {
      it->second.presence = msg;
      it->second.has_presence = true;
      it->second.scene = msg.str("scene", it->second.scene);
    }
    return;
  }
}

void TeamClient::on_commit(const Batch& b) {
  if (b.seq <= seq_) return;
  seq_ = b.seq;
  ++commits_seen_;
  bool created = false;
  for (const Op& op : b.ops) {
    if (op.kind == Op::Kind::Put && !confirmed_[b.scene].entity(op.id)) created = true;
  }
  confirmed_[b.scene].apply(b);
  if (b.from == me_) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const Batch& p) { return p.cid == b.cid; });
    if (it != pending_.end()) pending_.erase(it);
    return;
  }
  // Our pending batches on the same things come later in the server's
  // order, so they win: leave the editor showing them.
  std::set<std::string> mine;
  for (const Batch& p : pending_) {
    if (p.scene != b.scene) continue;
    for (const Op& op : p.ops) mine.insert(op.touch_key());
  }
  for (const Op& op : b.ops) {
    if (!mine.count(op.touch_key())) apply_.emplace_back(b.scene, op);
  }
  // A remote create lands before our own pending creates (the server
  // appends them after it): put the order the way it will end up.
  if (created && !mine.empty()) {
    SceneDoc view = confirmed_[b.scene];
    for (const Batch& p : pending_) {
      if (p.scene == b.scene) view.apply(p);
    }
    apply_.emplace_back(b.scene, Op::order(view.order()));
  }
}

void TeamClient::on_reject(std::uint64_t cid, const std::string& reason) {
  auto it = std::find_if(pending_.begin(), pending_.end(),
                         [&](const Batch& p) { return p.cid == cid; });
  if (it == pending_.end()) return;
  Batch gone = std::move(*it);
  pending_.erase(it);
  ++rejected_;
  notes_.push_back("Edit \"" + gone.label + "\" undone: " + reason + ".");
  // What the editor should show for those things: confirmed + the rest of
  // our pending batches.
  SceneDoc view = confirmed_[gone.scene];
  for (const Batch& p : pending_) {
    if (p.scene == gone.scene) view.apply(p);
  }
  std::set<std::string> done;
  for (const Op& op : gone.ops) {
    if (!done.insert(op.touch_key()).second) continue;
    switch (op.kind) {
      case Op::Kind::Put:
      case Op::Kind::Del:
        if (const Value* v = view.entity(op.id)) apply_.emplace_back(gone.scene, Op::put(op.id, *v));
        else apply_.emplace_back(gone.scene, Op::del(op.id));
        break;
      case Op::Kind::Order: apply_.emplace_back(gone.scene, Op::order(view.order())); break;
      case Op::Kind::Set: {
        auto pit = view.props.find(op.key);
        apply_.emplace_back(gone.scene,
                            Op::set(op.key, pit == view.props.end() ? Value::null() : pit->second));
        break;
      }
      case Op::Kind::Asset: {
        auto ait = view.assets.find(op.key);
        apply_.emplace_back(gone.scene, ait == view.assets.end()
                                            ? Op::asset(op.key, "", 0)
                                            : Op::asset(op.key, ait->second.sha256, ait->second.size));
        break;
      }
      case Op::Kind::Terrain: {
        auto tit = view.terrains.find(op.key);
        apply_.emplace_back(gone.scene, Op::terrain(op.key, op.id,
                                                    tit == view.terrains.end() ? Value::null()
                                                                               : terrain_full(tit->second)));
        break;
      }
    }
  }
  // The order may have changed around a re-created entity.
  apply_.emplace_back(gone.scene, Op::order(view.order()));
}

bool TeamClient::take_welcome(Docs* out) {
  if (!welcome_pending_) return false;
  welcome_pending_ = false;
  if (out) *out = confirmed_;
  return true;
}

std::uint64_t TeamClient::submit(const std::string& scene, const std::string& label,
                                 std::vector<Op> ops) {
  if (state_ != State::Joined || ops.empty()) return 0;
  Batch b;
  b.cid = next_cid_++;
  b.scene = scene;
  b.label = label;
  b.ops = std::move(ops);
  Value m = batch_to_json(b);
  m.set("t", str("op"));
  send(m);
  pending_.push_back(std::move(b));
  return pending_.back().cid;
}

std::vector<std::pair<std::string, Op>> TeamClient::take_apply() {
  std::vector<std::pair<std::string, Op>> out;
  out.swap(apply_);
  return out;
}

void TeamClient::want_locks(std::vector<std::string> keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  if (want_sent_ && keys == want_) return;
  want_ = std::move(keys);
  if (state_ != State::Joined) return;
  Value l = Value::object();
  l.set("t", str("lock"));
  Value arr = Value::array();
  for (const std::string& k : want_) arr.push(str(k));
  l.set("keys", std::move(arr));
  send(l);
  want_sent_ = true;
}

void TeamClient::take_locks(const std::vector<std::string>& keys) {
  if (state_ != State::Joined || keys.empty()) return;
  Value l = Value::object();
  l.set("t", str("take"));
  Value arr = Value::array();
  for (const std::string& k : keys) arr.push(str(k));
  l.set("keys", std::move(arr));
  send(l);
}

std::uint32_t TeamClient::lock_owner(const std::string& key) const {
  auto it = locks_.find(key);
  return it == locks_.end() ? 0 : it->second;
}

void TeamClient::send_presence(Value presence) {
  if (state_ != State::Joined) return;
  presence.set("t", str("pres"));
  send(presence, false);
}

std::uint64_t TeamClient::send_preview(const std::string& scene, const std::vector<Op>& puts) {
  if (state_ != State::Joined || !can_write_ || puts.empty()) return 0;
  Value m = Value::object();
  m.set("t", str("prev"));
  m.set("scene", str(scene));
  m.set("n", num(static_cast<double>(++preview_n_)));
  Value ops = Value::array();
  for (const Op& op : puts) {
    if (op.kind == Op::Kind::Put) ops.push(op_to_json(op));
  }
  m.set("ops", std::move(ops));
  send(m, false);
  return preview_n_;
}

void TeamClient::end_preview(const std::string& scene, const std::vector<std::uint64_t>& ids) {
  if (state_ != State::Joined || ids.empty()) return;
  Value m = Value::object();
  m.set("t", str("prev_end"));
  m.set("scene", str(scene));
  m.set("n", num(static_cast<double>(preview_n_)));
  Value arr = Value::array();
  for (std::uint64_t id : ids) arr.push(id_value(id));
  m.set("ids", std::move(arr));
  send(m);
}

std::vector<PreviewEvent> TeamClient::take_previews() {
  std::vector<PreviewEvent> out;
  out.swap(previews_);
  return out;
}

const RemoteMember* TeamClient::member(std::uint32_t id) const {
  auto it = members_.find(id);
  return it == members_.end() ? nullptr : &it->second;
}

std::vector<std::string> TeamClient::take_notes() {
  std::vector<std::string> out;
  out.swap(notes_);
  return out;
}

}  // namespace team
}  // namespace tombstone
}  // namespace ts
