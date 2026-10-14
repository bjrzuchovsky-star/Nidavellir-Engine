#include "SmokeScripting.h"

#include "SmokeNetwork.h"

#include "scene/SceneData.h"
#include "scene/SceneJson.h"

#include <iostream>
#include <string>

namespace {

using namespace ts::tombstone;

int fail(const std::string& what) {
  std::cerr << "[smoke] scripting FAILED: " << what << '\n';
  return 1;
}

// scene.json v6: script components round-trip with typed props; v5 files
// load without one.
int check_v6_format() {
  const std::string v5 =
      "{\n  \"version\": 5,\n  \"tile_solidity\": [],\n  \"entities\": [\n"
      "    {\"id\": 1, \"name\": \"Gate\", \"x\": 0, \"y\": 0, \"w\": 32, "
      "\"h\": 32, \"collider\": {\"x\": 0, \"y\": 0, \"w\": 32, \"h\": 32, "
      "\"type\": \"solid\", \"body\": \"static\"}, \"animator\": {\"set\": "
      "\"assets/rider.anim.json\", \"clip\": \"\", \"default_clip\": \"\", "
      "\"speed\": 1, \"playing\": true}}\n  ]\n}\n";
  scene_json::SceneDoc doc;
  std::string err;
  if (!scene_json::parse(v5, &doc, &err) || doc.version != 5 ||
      doc.entities.size() != 1 || doc.entities[0].script ||
      !doc.entities[0].animator || !doc.entities[0].collider) {
    return fail("v5 scene should load untouched: " + err);
  }
  ScriptData sc;
  sc.path = "scripts/gate.lua";
  sc.set("reach", ScriptValue::of_number(0.1));
  sc.set("line", ScriptValue::of_text("Gate's \"open\"\n"));
  sc.set("locked", ScriptValue::of_bool(false));
  sc.set("big", ScriptValue::of_number(70000.0625));
  if (sc.props.size() != 4 || sc.props[0].name != "big" ||
      sc.props[3].name != "reach") {
    return fail("props should stay sorted by name");
  }
  doc.entities[0].script = sc;
  const std::string v6 = scene_json::write(doc);
  scene_json::SceneDoc back;
  const std::string vtag =
      "\"version\": " + std::to_string(scene_json::kSceneVersion);
  if (v6.find(vtag) == std::string::npos ||
      v6.find("\"script\": {\"path\": \"scripts/gate.lua\", \"props\": "
              "[{\"name\": \"big\", \"number\": 70000.0625}, {\"name\": "
              "\"line\", \"text\": \"Gate's \\\"open\\\"\\n\"}, {\"name\": "
              "\"locked\", \"bool\": false}, {\"name\": \"reach\", "
              "\"number\": 0.1}]}") == std::string::npos ||
      !scene_json::parse(v6, &back, &err) || back.version != scene_json::kSceneVersion ||
      back.entities != doc.entities || scene_json::write(back) != v6) {
    return fail("v6 script roundtrip: " + err + "\n" + v6);
  }
  // Duplicates and blank names are dropped; a prop without a value or a
  // malformed script object is refused.
  if (!scene_json::parse(
          "{\"entities\": [{\"id\": 1, \"name\": \"A\", \"script\": "
          "{\"path\": \"scripts/a.lua\", \"props\": [{\"name\": \"b\", "
          "\"number\": 2}, {\"name\": \"a\", \"bool\": true}, {\"name\": "
          "\"b\", \"number\": 3}, {\"name\": \"\", \"number\": 1}]}}]}",
          &back, &err) ||
      !back.entities[0].script || back.entities[0].script->props.size() != 2 ||
      back.entities[0].script->props[0].name != "a" ||
      back.entities[0].script->props[1].value.number != 2.0) {
    return fail("script props normalize: " + err);
  }
  if (scene_json::parse("{\"entities\": [{\"id\": 1, \"name\": \"A\", "
                        "\"script\": {\"path\": \"x\", \"props\": [{\"name\": "
                        "\"q\"}]}}]}",
                        &back, &err) ||
      scene_json::parse("{\"entities\": [{\"id\": 1, \"name\": \"A\", "
                        "\"script\": [1, 2]}]}",
                        &back, &err)) {
    return fail("malformed script objects should be refused");
  }
  return 0;
}

}  // namespace

int run_scripting_smoke() {
  if (check_v6_format() != 0) {
    return 1;
  }
  std::cout << "[smoke] scripting data OK (scene.json v6 script + typed props "
               "roundtrip, v5 -> v6 load, malformed refused)\n";
  if (run_script_runtime_smoke() != 0) {
    return 1;
  }
  if (run_script_editor_smoke() != 0) {
    return 1;
  }
  // Multiplayer last: it rides the scripted sample over loopback
  // (SmokeNetwork.cpp).
  return run_network_smoke();
}
