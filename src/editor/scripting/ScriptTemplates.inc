// Script templates for C++ and C# (ScriptComponents.cpp). {Class} and
// {stem} are filled in from the file name. Lua comes from
// scene/SampleScripts.cpp; graphs are built in code.

namespace cpp_templates {

constexpr const char* kBlank = R"cpp(// scripts/{stem}.cpp -- a TombStone C++ script. Every scripts/*.cpp builds
// into one module; save and the ride hot-reloads it. Props are read with
// prop("name", default) and show in the Inspector, overridable per entity.
// API: tombstone/script_sdk.h (find, player, position, move, set_visible,
// set_collider, destroy, get / set, toast, log, sound, burst...).
#include <tombstone/script_sdk.h>

#include <string>

struct {Class} : ts::Script {
  void on_start() override { log("{stem} saddled up, speed " + std::to_string(prop("speed", 60.0))); }
  void on_tick(double dt) override { (void)dt; }
  void on_trigger_enter(ts::Entity other) override { (void)other; }
  void on_trigger_exit(ts::Entity other) override { (void)other; }
  void on_interact(ts::Entity rider) override { (void)rider; }
};

TS_SCRIPT({Class}, "{stem}")
)cpp";

constexpr const char* kGate = R"cpp(// scripts/{stem}.cpp -- a gate: the action button opens it (no collider,
// hidden); close_after > 0 swings it shut again after that many seconds.
#include <tombstone/script_sdk.h>

#include <string>

struct {Class} : ts::Script {
  double shut_in = 0.0;  // seconds until it swings shut (0 = stays open)

  int slot_of(ts::Entity e) const {
    for (int s = 1; s <= 4; ++s) {
      if (player(s) == e) return s;
    }
    return 0;
  }
  void on_start() override { set("open", false); }
  void on_interact(ts::Entity rider) override {
    if (get("open") != 0.0) return;
    if (prop("locked", false)) {
      toast("Locked tight. Somebody's got the key.", slot_of(rider), 2.0);
      return;
    }
    set("open", true);
    set_collider(self(), false);
    set_visible(self(), false);
    toast(prop("open_line", "The gate creaks open."), slot_of(rider), 2.0);
    shut_in = prop("close_after", 0.0);
  }
  void on_tick(double dt) override {
    if (shut_in <= 0.0) return;
    shut_in -= dt;
    if (shut_in <= 0.0) {
      set("open", false);
      set_collider(self(), true);
      set_visible(self(), true);
      log("{stem} swung shut");
    }
  }
};

TS_SCRIPT({Class}, "{stem}")
)cpp";

constexpr const char* kPickup = R"cpp(// scripts/{stem}.cpp -- a pickup: the first rider through its trigger
// pockets `amount` gold (kept on the rider as "gold") and it leaves.
#include <tombstone/script_sdk.h>

#include <string>

struct {Class} : ts::Script {
  void on_trigger_enter(ts::Entity other) override {
    int slot = 0;
    for (int s = 1; s <= 4; ++s) {
      if (player(s) == other) slot = s;
    }
    if (slot == 0) return;  // only riders pocket it
    const double amount = prop("amount", 10.0);
    const double purse = get("gold", 0.0, other) + amount;
    set("gold", purse, other);
    toast("Picked up " + std::to_string(static_cast<int>(amount)) + " gold (" +
              std::to_string(static_cast<int>(purse)) + " in the purse)",
          slot, 2.5);
    destroy(self());
  }
};

TS_SCRIPT({Class}, "{stem}")
)cpp";

constexpr const char* kSign = R"cpp(// scripts/{stem}.cpp -- a sign: the action button shows `line` on the
// rider's HUD for `seconds`.
#include <tombstone/script_sdk.h>

struct {Class} : ts::Script {
  void on_interact(ts::Entity rider) override {
    int slot = 0;
    for (int s = 1; s <= 4; ++s) {
      if (player(s) == rider) slot = s;
    }
    toast(prop("line", "Tombstone, 3 miles. Mind the rattlers."), slot, prop("seconds", 3.0));
  }
};

TS_SCRIPT({Class}, "{stem}")
)cpp";

constexpr const char* kTicker = R"cpp(// scripts/{stem}.cpp -- a ticker: every `every` seconds it hops `hop`
// pixels up, then back down (a tumbleweed on the breeze).
#include <tombstone/script_sdk.h>

struct {Class} : ts::Script {
  double timer = 0.0;
  bool up = false;
  void on_tick(double dt) override {
    timer += dt;
    if (timer < prop("every", 2.0)) return;
    timer = 0.0;
    float x = 0.0f, y = 0.0f;
    if (!position(self(), &x, &y)) return;
    const float hop = static_cast<float>(prop("hop", 8.0));
    up = !up;
    move(self(), x, up ? y - hop : y + hop);
  }
};

TS_SCRIPT({Class}, "{stem}")
)cpp";

}  // namespace cpp_templates

namespace cs_templates {

constexpr const char* kBlank = R"cs(// scripts/{stem}.cs -- a TombStone C# script (needs the .NET 8 SDK). Every
// scripts/*.cs builds into one assembly; save and the ride hot-reloads it.
// Props are read with Prop("name", default) and show in the Inspector.
// API: TombStone.Script (Find, Player, Position, Move, SetVisible,
// SetCollider, Destroy, Get / Set, Toast, Log, Sound, Burst...).
using TombStone;

[Script("{stem}")]
public class {Class} : Script
{
    public override void OnStart() { Log("{stem} saddled up, speed " + Prop("speed", 60.0)); }
    public override void OnTick(double dt) { }
    public override void OnTriggerEnter(ulong other) { }
    public override void OnTriggerExit(ulong other) { }
    public override void OnInteract(ulong rider) { }
}
)cs";

constexpr const char* kGate = R"cs(// scripts/{stem}.cs -- a gate: the action button opens it (no collider,
// hidden); close_after > 0 swings it shut again after that many seconds.
using TombStone;

[Script("{stem}")]
public class {Class} : Script
{
    double shutIn;

    int SlotOf(ulong e)
    {
        for (int s = 1; s <= 4; s++) if (Player(s) == e) return s;
        return 0;
    }

    public override void OnStart() { Set("open", false); }

    public override void OnInteract(ulong rider)
    {
        if (Get("open") != 0) return;
        if (Prop("locked", false)) { Toast("Locked tight. Somebody's got the key.", SlotOf(rider)); return; }
        Set("open", true);
        SetCollider(Self, false);
        SetVisible(Self, false);
        Toast(Prop("open_line", "The gate creaks open."), SlotOf(rider));
        shutIn = Prop("close_after", 0.0);
    }

    public override void OnTick(double dt)
    {
        if (shutIn <= 0) return;
        shutIn -= dt;
        if (shutIn <= 0)
        {
            Set("open", false);
            SetCollider(Self, true);
            SetVisible(Self, true);
            Log("{stem} swung shut");
        }
    }
}
)cs";

constexpr const char* kPickup = R"cs(// scripts/{stem}.cs -- a pickup: the first rider through its trigger
// pockets `amount` gold (kept on the rider as "gold") and it leaves.
using TombStone;

[Script("{stem}")]
public class {Class} : Script
{
    public override void OnTriggerEnter(ulong other)
    {
        int slot = 0;
        for (int s = 1; s <= 4; s++) if (Player(s) == other) slot = s;
        if (slot == 0) return;  // only riders pocket it
        double amount = Prop("amount", 10.0);
        double purse = Get("gold", 0, other) + amount;
        Set("gold", purse, other);
        Toast("Picked up " + amount + " gold (" + purse + " in the purse)", slot, 2.5);
        Destroy(Self);
    }
}
)cs";

constexpr const char* kSign = R"cs(// scripts/{stem}.cs -- a sign: the action button shows `line` on the
// rider's HUD for `seconds`.
using TombStone;

[Script("{stem}")]
public class {Class} : Script
{
    public override void OnInteract(ulong rider)
    {
        int slot = 0;
        for (int s = 1; s <= 4; s++) if (Player(s) == rider) slot = s;
        Toast(Prop("line", "Tombstone, 3 miles. Mind the rattlers."), slot, Prop("seconds", 3.0));
    }
}
)cs";

constexpr const char* kTicker = R"cs(// scripts/{stem}.cs -- a ticker: every `every` seconds it hops `hop`
// pixels up, then back down (a tumbleweed on the breeze).
using TombStone;

[Script("{stem}")]
public class {Class} : Script
{
    double timer;
    bool up;

    public override void OnTick(double dt)
    {
        timer += dt;
        if (timer < Prop("every", 2.0)) return;
        timer = 0;
        if (!Position(Self, out float x, out float y)) return;
        float hop = (float)Prop("hop", 8.0);
        up = !up;
        Move(Self, x, up ? y - hop : y + hop);
    }
}
)cs";

}  // namespace cs_templates
