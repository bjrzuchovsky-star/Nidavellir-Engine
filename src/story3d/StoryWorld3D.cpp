#include "story3d/StoryWorld3D.h"

#include "lua.h"

namespace ts {
namespace tombstone {
namespace story3d {

using world3d::World3D;

std::shared_ptr<StoryWorld3D> attach(World3D& w, const story::StoryFeatures& f) {
  if (!f.quests && !f.dialogue && !f.saves) return nullptr;
  if (find(w)) return nullptr;
  auto ext = std::make_shared<StoryWorld3D>(f);
  w.add_extension(ext);
  return ext;
}

StoryWorld3D* find(const World3D& w) { return dynamic_cast<StoryWorld3D*>(w.extension(StoryWorld3D::kName)); }

void StoryWorld3D::on_start(World3D& w) {
  story::StoryHost h;
  h.project_dir = w.project_dir();
  World3D* world = &w;
  h.ui = [world](runtime::UiCommand c) { world->script_ui(std::move(c)); };
  h.broadcast = [world](const char* hook, const std::string& text, double n) {
    world->script_broadcast(hook, text, n);
  };
  h.toast = [world](const std::string& text, double seconds) { world->script_toast(-1, text, seconds, 0); };
  h.log = [world](int level, const std::string& text) {
    runtime::LogEntry e;
    e.level = level >= 2 ? runtime::LogLevel::Error : level == 1 ? runtime::LogLevel::Warn : runtime::LogLevel::Info;
    e.channel = "story";
    e.text = text;
    world->script_log(std::move(e));
  };
  h.get_player = [world](float p[3]) {
    render3d::Vec3 v;
    if (!world->rider_position(&v)) return false;
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
    return true;
  };
  h.set_player = [world](const float p[3]) { return world->set_rider_position(render3d::Vec3{p[0], p[1], p[2]}); };
  rt_.start(std::move(h), features_);
  slot_ = &rt_;
  w.set_ui_tap(kName, [this](const std::string& wd, bool change, double v, const std::string& t) {
    return rt_.ui_event(wd, change, v, t);
  });
}

void StoryWorld3D::on_tick(World3D& /*w*/, double dt) { rt_.tick(dt); }

void StoryWorld3D::on_stop(World3D& w) {
  rt_.stop();
  slot_ = nullptr;
  w.set_ui_tap(kName, nullptr);
}

void StoryWorld3D::open_lua(World3D& /*w*/, lua_State* L, int sandbox) {
  story::push_story_lua(L, &slot_);
  lua_setfield(L, sandbox, "story");
  if (features_.quests) {
    story::push_quests_lua(L, &slot_);
    lua_setfield(L, sandbox, "quests");
  }
  if (features_.dialogue) {
    story::push_dialogue_lua(L, &slot_);
    lua_setfield(L, sandbox, "dialogue");
  }
  if (features_.saves) {
    story::push_saves_lua(L, &slot_);
    lua_setfield(L, sandbox, "saves");
  }
}

}  // namespace story3d
}  // namespace tombstone
}  // namespace ts
