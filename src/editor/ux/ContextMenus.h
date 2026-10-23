#pragma once

// Shared right-click menus (Field Guide). Component headers in both
// Inspectors use ComponentHeaderMenu(); every item is a ux::MenuItem so the
// palette and --smoke-ux see it and it gets its tooltip.

#include <functional>
#include <string>

namespace ts::tombstone::editor::ux {

// What a component header's menu can do. Leave a hook empty to grey the
// item out (e.g. no Move up on the first component).
struct HeaderOps {
  std::function<void()> reset;   // back to the component's defaults
  std::function<void()> copy;    // copy its values
  std::function<void()> paste;   // paste copied values (same component)
  std::function<void()> remove;
  std::function<void()> move_up;
  std::function<void()> move_down;
  bool can_paste = false;
  std::string paste_from;        // "Collider on Crate" (shown in the item)
};

// The menu body (inside a popup).
void ComponentHeaderItems(const char* component, const HeaderOps& ops);
// Right-click on the item just drawn (the header) opens the menu.
void ComponentHeaderMenu(const char* component, const HeaderOps& ops);

// docs/<file>.md for a component / panel name ("Collider" -> physics.md).
std::string docs_file_for(const std::string& component);
std::string docs_url(const std::string& file);
// Open the docs page in the browser (friendly error if it can't).
void open_docs(const std::string& component);

}  // namespace ts::tombstone::editor::ux
