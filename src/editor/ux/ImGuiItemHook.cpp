// Compiled into ts_imgui: the hooks Dear ImGui expects when
// IMGUI_ENABLE_TEST_ENGINE is defined. Only ItemInfo does anything, and only
// when the editor installs g_ts_imgui_item_info_hook.
#include "imgui.h"
#include "imgui_internal.h"

#include "editor/ux/ImGuiItemHook.h"

TsImGuiItemInfoHook g_ts_imgui_item_info_hook = nullptr;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID, const ImRect&, const ImGuiLastItemData*) {}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label,
                                  ImGuiItemStatusFlags flags) {
  if (g_ts_imgui_item_info_hook) g_ts_imgui_item_info_hook(ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
