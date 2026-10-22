add_library(ts_editor
  AppFlow.cpp
  AppFlow_Delete.cpp
  settings/Settings.cpp
  projects/ProjectStore.cpp
  projects/ProjectStore_Delete.cpp
  workspace/Workspace2D.cpp
  workspace/Workspace2D_History.cpp
  workspace/Workspace2D_Tiles.cpp
  workspace/Workspace2D_Tools.cpp
  workspace/Workspace2D_Xform.cpp
  workspace/SceneIO_Load.cpp
  workspace/SceneIO_Save.cpp
  assets/AssetLibrary.cpp
  assets/PrefabLibrary.cpp
  assets/AssetDb.cpp
  assets/AssetImport.cpp
  assets/ImportSort.cpp
  assets/AssetBrowser.cpp
  assets/GmaImport.cpp
  screens/Editor2DScreen_Assets.cpp
  editor3d/Editor3DScreen_Assets.cpp
  screens/Editor2DScreen_Prefabs.cpp
  editor3d/Editor3DScreen_Prefabs.cpp
  ui/FolderBrowser.cpp
  ui/Theme.cpp
  ui/PlayUi.cpp
  ui/QuadBlend.cpp
  screens/LoadingScreen.cpp
  screens/LoginScreen.cpp
  screens/ProjectManagerScreen.cpp
  screens/SettingsScreen.cpp
  screens/Editor2DScreen.cpp
  screens/Editor2DScreen_Panels.cpp
  screens/Editor2DScreen_Shell.cpp
  screens/Editor2DScreen_Viewport.cpp
  screens/Editor2DScreen_Tiles.cpp
  screens/Editor2DScreen_Tools.cpp
  screens/Editor2DScreen_TilePanels.cpp
  screens/Editor2DScreen_Play.cpp
  screens/Editor2DScreen_Collision.cpp
  screens/Editor2DScreen_Anim.cpp
  screens/Editor2DScreen_Stable.cpp
  screens/Editor2DScreen_Console.cpp
  screens/Editor2DScreen_Scripts.cpp
  screens/Editor2DScreen_Plugins.cpp
  editor3d/Gizmo3D.cpp
  editor3d/Editor3DScreen_Transform.cpp
  transform/ModalTransform.cpp
  transform/ModalKeys.cpp
  transform/GizmoShapes.cpp
  transform/Gizmo2D.cpp
  screens/Editor2DScreen_Transform.cpp
  editor3d/Editor3DScreen.cpp
  editor3d/Editor3DScreen_UI.cpp
  editor3d/Editor3DScreen_Panels.cpp
  editor3d/Editor3DScreen_Scripts.cpp
  editor3d/Editor3DScreen_VR.cpp
  editor3d/Editor3DScreen_Anim.cpp
  editor3d/Editor3DScreen_Tools.cpp
  editor3d/Editor3DScreen_Terrain.cpp
  plugins/EditorPlugins.cpp
  plugins/PluginNeeds.cpp
  plugins/EditorBuiltinPlugins.cpp
  plugins/FrontierPanels.cpp
  plugins/AiPanels.cpp
  plugins/ShipyardPanels.cpp
  plugins/PluginsPanel.cpp
  plugins/PluginPanelHost.cpp
  plugins/StoryPanels.cpp
  plugins/WeatherPanels.cpp
  plugins/ProfilerPanels.cpp
  plugins/TownHallPanel.cpp
  screens/Editor2DScreen_Posse.cpp
  screens/Editor2DScreen_Audio.cpp
  console/TelegraphLog.cpp
  launch/GameLauncher.cpp
  launch/OpenWithOs.cpp
  audio/EditorAudio.cpp
  fx/FxInspector.cpp
  screens/Editor2DScreen_Fx.cpp
  audio/MixerPanel.cpp
  audio/AudioWidgets.cpp
  export/FreightOffice.cpp
  templates/TemplateStore.cpp
  templates/EmptyTemplates.cpp
  templates/TemplateThumbs.cpp
  screens/ProjectManager_Templates.cpp
  store/TradingPostPanel.cpp
  vault/VaultProject.cpp
  vault/VaultPanel.cpp
  vault/FilePick.cpp
  update/UpdateCheck.cpp
  update/UpdateService.cpp
  scripting/BrandBook.cpp
  scripting/ScriptComponents.cpp
  tools/StringsPanel.cpp
  tools/AutoTilePanel.cpp
  tools/SpriteSlicerPanel.cpp
)

# Engine templates: the source tree's templates/ is the fallback when no
# <exe dir>/templates exists (ts_admin's build copies one there).
target_compile_definitions(ts_editor PRIVATE
  TS_TEMPLATES_SOURCE_DIR="${PROJECT_SOURCE_DIR}/templates")

target_include_directories(ts_editor
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_editor
  PUBLIC
    ts_core
    ts_store
    ts_scene
    ts_runtime
    ts_net
    ts_mp
    ts_mmo
    ts_plugin
    ts_plugins_builtin
    ts_render3d
    ts_world3d
    ts_ai3d
    ts_weather3d
    ts_story3d
    ts_debugtools
    ts_xr
    ts_gfx
    ts_imgui
    ts_audio
    ts_audio3d
    ts_ui
    ts_export
    ts_vault_lib
)

target_compile_features(ts_editor PUBLIC cxx_std_20)
# The update notifier compares GitHub's latest release with this (VERSION).
target_compile_definitions(ts_editor PRIVATE TS_ENGINE_VERSION="${PROJECT_VERSION}")

if(WIN32)
  # ShellExecuteW (open a script in the OS default editor).
  target_link_libraries(ts_editor PRIVATE shell32)
  # GetOpenFileNameW (Vault manager > Add files...).
  target_link_libraries(ts_editor PRIVATE comdlg32)
endif()
