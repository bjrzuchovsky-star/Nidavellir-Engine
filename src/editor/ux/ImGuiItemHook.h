#pragma once

// One function pointer behind Dear ImGui's item hooks (IMGUI_ENABLE_TEST_ENGINE
// is defined for ts_imgui). ImGui calls ImGuiTestEngineHook_ItemInfo with
// every labelled widget it submits while GImGui->TestEngineHookItems is true;
// the editor's Field Guide (ux/Tooltip.cpp) listens to give Inspector and
// toolbar widgets their tooltips by label, and --smoke-ux uses the same feed
// to audit menus. Null = nobody listening (the default, and ts_game).

struct ImGuiContext;
typedef unsigned int ImGuiID;
typedef int ImGuiItemStatusFlags;

using TsImGuiItemInfoHook = void (*)(ImGuiContext* ctx, ImGuiID id, const char* label,
                                     ImGuiItemStatusFlags flags);
extern TsImGuiItemInfoHook g_ts_imgui_item_info_hook;
