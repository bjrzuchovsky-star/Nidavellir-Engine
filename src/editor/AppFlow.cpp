#include "editor/AppFlow.h"

#include "editor/plugins/EditorPlugins.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/screens/FrontierScreen.h"
#include "editor/screens/LoadingScreen.h"
#include "editor/screens/LoginScreen.h"
#include "editor/screens/ProjectManagerScreen.h"
#include "editor/screens/SettingsScreen.h"

#include <iostream>
#include <utility>

namespace ts {
namespace tombstone {
namespace editor {

AppFlow::AppFlow()
    : project_store_(SettingsStore::default_projects_root()) {}

AppFlow::~AppFlow() {
  if (screen_) {
    screen_->on_exit();
    screen_.reset();
  }
}

void AppFlow::start() {
  last_error_.clear();
  status_message_.clear();
  active_project_.reset();

  std::string load_err;
  settings_ = SettingsStore::load(&load_err);
  if (!load_err.empty()) {
    std::cout << "[AppFlow] settings load warning: " << load_err << '\n';
  }

  project_store_.set_projects_root(settings_.projects_root);
  if (!reload_projects()) {
    status_message_ = last_error_;
  } else {
    status_message_ = "Loaded " + std::to_string(project_store_.projects().size()) +
                      " project(s) from " + settings_.projects_root;
  }

  // Persist defaults on first run so the config file exists and is discoverable.
  persist_settings();

  transition_to(AppState::Loading);
}

bool AppFlow::reload_projects() {
  last_error_.clear();
  std::string ensure_err;
  if (!SettingsStore::ensure_projects_root(settings_.projects_root,
                                           &ensure_err)) {
    last_error_ = ensure_err.empty()
                      ? ("Missing or unusable projects_root: " +
                         settings_.projects_root)
                      : ensure_err;
    std::cout << "[AppFlow] projects_root error: " << last_error_ << '\n';
    return false;
  }

  project_store_.set_projects_root(settings_.projects_root);
  if (!project_store_.refresh()) {
    last_error_ = project_store_.last_error();
    std::cout << "[AppFlow] project scan failed: " << last_error_ << '\n';
    return false;
  }
  return true;
}

void AppFlow::persist_settings() {
  std::string err;
  if (!SettingsStore::save(settings_, &err)) {
    last_error_ = err;
    std::cout << "[AppFlow] settings save failed: " << err << '\n';
  }
}

bool AppFlow::try_apply_settings_from_screen() {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen || !settings_screen->apply_requested()) {
    return false;
  }

  Settings draft = settings_screen->draft();
  std::string ensure_err;
  if (!SettingsStore::ensure_projects_root(draft.projects_root, &ensure_err)) {
    last_error_ = ensure_err.empty()
                      ? ("Cannot use projects_root '" + draft.projects_root +
                         "' (missing or not writable).")
                      : ensure_err;
    status_message_ = last_error_;
    settings_screen->clear_apply_request();
    settings_screen->set_validation_error(last_error_);
    std::cout << "[AppFlow] settings apply blocked: " << last_error_ << '\n';
    return false;
  }

  settings_ = std::move(draft);
  persist_settings();
  if (!reload_projects()) {
    status_message_ = last_error_;
    settings_screen->clear_apply_request();
    settings_screen->set_validation_error(last_error_);
    return false;
  }

  status_message_ =
      "Settings saved; projects reloaded from " + settings_.projects_root;
  return true;
}

void AppFlow::tick(float delta_seconds) {
  if (!screen_ || state_ == AppState::Quit) {
    return;
  }

  handle_pending_screen_actions();

  const AppState next = screen_->on_update(delta_seconds);
  if (next != state_) {
    if (state_ == AppState::Login) {
      if (auto* login = dynamic_cast<LoginScreen*>(screen_.get())) {
        if (next == AppState::Settings) {
          settings_return_state_ = AppState::Login;
        }
        if (next == AppState::ProjectManager && !login->username().empty()) {
          settings_.username = login->username();
          persist_settings();
        }
      }
    } else if (state_ == AppState::ProjectManager) {
      if (next == AppState::Settings) {
        settings_return_state_ = AppState::ProjectManager;
      }
      if (auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get())) {
        if (next == AppState::Editor2D || next == AppState::EditorFrontier) {
          if (const ProjectInfo* selected = pm->selected_project()) {
            active_project_ = std::make_unique<ProjectInfo>(*selected);
            project_store_.touch_last_opened(selected->path);
            settings_.last_project_path = selected->path;
            persist_settings();
          }
        }
      }
    } else if (state_ == AppState::Settings) {
      if (auto* settings_screen =
              dynamic_cast<SettingsScreen*>(screen_.get())) {
        if (settings_screen->apply_requested()) {
          // Validate/persist before leaving; stay on Settings on failure.
          if (!try_apply_settings_from_screen()) {
            return;
          }
        }
      }
    } else if ((state_ == AppState::Editor2D ||
                state_ == AppState::EditorFrontier) &&
               next == AppState::ProjectManager) {
      active_project_.reset();
      status_message_.clear();
      reload_projects();
    }

    transition_to(next);
  }
}

void AppFlow::handle_pending_screen_actions() {
  if (auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get())) {
    if (pm->new_project_requested()) {
      const std::string name = pm->pending_new_project_name();
      const ProjectKind kind = pm->pending_new_project_kind();
      pm->clear_new_project_request();
      create_new_project(name, kind);
    }
    if (pm->delete_requested()) {
      const auto idx = pm->pending_delete_index();
      pm->clear_delete_request();
      if (idx.has_value()) {
        delete_project(*idx);
      }
    }
  }
}

bool AppFlow::try_login(const std::string& username,
                        const std::string& password) {
  auto* login = dynamic_cast<LoginScreen*>(screen_.get());
  if (!login) {
    return false;
  }
  if (!login->try_login(username, password)) {
    return false;
  }
  tick(0.0f);
  return true;
}

bool AppFlow::submit_dev_login() {
  auto* login = dynamic_cast<LoginScreen*>(screen_.get());
  if (!login) {
    return false;
  }
  if (!login->submit_dev_login()) {
    return false;
  }
  tick(0.0f);
  return true;
}

bool AppFlow::open_settings_from_login() {
  auto* login = dynamic_cast<LoginScreen*>(screen_.get());
  if (!login) {
    return false;
  }
  login->request_settings();
  tick(0.0f);
  return current_state() == AppState::Settings;
}

bool AppFlow::select_project(std::size_t index) {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get());
  if (!pm) {
    return false;
  }
  if (!pm->select_project(index)) {
    return false;
  }
  tick(0.0f);
  return current_state() == AppState::Editor2D ||
         current_state() == AppState::EditorFrontier;
}

bool AppFlow::create_new_project_2d(const std::string& name) {
  return create_new_project(name, ProjectKind::TwoD);
}

bool AppFlow::create_new_project(const std::string& name, ProjectKind kind) {
  last_error_.clear();
  ProjectInfo created;
  const std::vector<std::string> plugins = EditorPlugins::default_ids(kind);
  if (!project_store_.create_project(name, kind, &plugins, &created)) {
    last_error_ = project_store_.last_error();
    status_message_.clear();
    std::cout << "[AppFlow] create " << to_string(kind)
              << " project failed: " << last_error_ << '\n';
    if (auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get())) {
      pm->set_error_message(last_error_);
      pm->set_status_message({});
    }
    return false;
  }
  status_message_ = std::string("Created ") + to_string(kind) + " project \"" +
                    created.name + "\"";
  std::cout << "[AppFlow] " << status_message_ << " at " << created.path
            << '\n';

  if (auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get())) {
    pm->set_projects(project_store_.projects());
    pm->set_status_message(status_message_);
    pm->set_error_message({});
    pm->highlight_project_by_id(created.id);
  }
  return true;
}

bool AppFlow::open_settings_from_projects() {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get());
  if (!pm) {
    return false;
  }
  pm->request_settings();
  tick(0.0f);
  return current_state() == AppState::Settings;
}

bool AppFlow::logout() {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get());
  if (!pm) {
    return false;
  }
  active_project_.reset();
  pm->request_logout();
  tick(0.0f);
  return current_state() == AppState::Login;
}

bool AppFlow::settings_set_projects_root(std::string path) {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->set_projects_root(std::move(path));
  return true;
}

bool AppFlow::settings_set_username(std::string username) {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->set_username(std::move(username));
  return true;
}

bool AppFlow::settings_set_auto_login_dev(bool enabled) {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->set_auto_login_dev(enabled);
  return true;
}

bool AppFlow::settings_set_theme(std::string theme) {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->set_theme(std::move(theme));
  return true;
}

bool AppFlow::apply_settings_draft() {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->request_apply();
  if (!settings_screen->apply_requested()) {
    return false;
  }
  tick(0.0f);
  return current_state() == settings_return_state_;
}

bool AppFlow::cancel_settings() {
  auto* settings_screen = dynamic_cast<SettingsScreen*>(screen_.get());
  if (!settings_screen) {
    return false;
  }
  settings_screen->request_cancel();
  tick(0.0f);
  return current_state() == settings_return_state_;
}

void AppFlow::request_back_to_projects() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    editor->request_back_to_projects();
    tick(0.0f);
  } else if (auto* camp = dynamic_cast<FrontierScreen*>(screen_.get())) {
    camp->request_back_to_projects();
    tick(0.0f);
  }
}

void AppFlow::request_quit() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    editor->request_quit();
    tick(0.0f);
    return;
  }
  transition_to(AppState::Quit);
}


Workspace2D* AppFlow::editor_workspace() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return &editor->workspace();
  }
  return nullptr;
}

const Workspace2D* AppFlow::editor_workspace() const {
  if (auto* editor = dynamic_cast<const Editor2DScreen*>(screen_.get())) {
    return &editor->workspace();
  }
  return nullptr;
}

bool AppFlow::editor_save_scene(std::string* error_out) {
  auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get());
  if (!editor) {
    if (error_out) {
      *error_out = "Not in Editor2D";
    }
    return false;
  }
  return editor->save_scene(error_out);
}

std::size_t AppFlow::editor_duplicate_selected() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->duplicate_selected();
  }
  return 0;
}

std::size_t AppFlow::editor_delete_selected() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->delete_selected();
  }
  return 0;
}

bool AppFlow::editor_undo() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->undo();
  }
  return false;
}

bool AppFlow::editor_redo() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->redo();
  }
  return false;
}

FrontierScreen* AppFlow::frontier_screen() {
  return dynamic_cast<FrontierScreen*>(screen_.get());
}

Editor2DScreen* AppFlow::editor_screen() {
  return dynamic_cast<Editor2DScreen*>(screen_.get());
}

void AppFlow::transition_to(AppState next) {
  if (screen_) {
    screen_->on_exit();
    screen_.reset();
  }

  std::cout << "[AppFlow] " << to_string(state_) << " -> " << to_string(next)
            << '\n';
  state_ = next;

  if (state_ == AppState::Quit) {
    return;
  }

  screen_ = make_screen(state_);
  if (screen_) {
    screen_->on_enter();
  }
}

std::unique_ptr<IScreen> AppFlow::make_screen(AppState state) {
  switch (state) {
    case AppState::Loading:
      return std::make_unique<LoadingScreen>();
    case AppState::Login:
      return std::make_unique<LoginScreen>(settings_.username,
                                           settings_.auto_login_dev);
    case AppState::ProjectManager:
      return std::make_unique<ProjectManagerScreen>(
          project_store_.projects(), settings_.projects_root, status_message_);
    case AppState::Settings:
      return std::make_unique<SettingsScreen>(settings_, settings_return_state_);
    case AppState::Editor2D: {
      ProjectInfo project = active_project_
                                ? *active_project_
                                : ProjectInfo{.id = "unknown",
                                              .name = "Untitled 2D",
                                              .kind = ProjectKind::TwoD};
      return std::make_unique<Editor2DScreen>(std::move(project));
    }
    case AppState::EditorFrontier: {
      ProjectInfo project = active_project_
                                ? *active_project_
                                : ProjectInfo{.id = "unknown",
                                              .name = "Untitled 3D",
                                              .kind = ProjectKind::ThreeD};
      return std::make_unique<FrontierScreen>(std::move(project));
    }
    case AppState::Quit:
      break;
  }
  return nullptr;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
