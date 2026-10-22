#include "render3d/ModelImport.h"
#include "gfx/TextureCache.h"
#include "render3d/import/SourceFormats.h"
#include "render3d/import/SourceMaterials.h"
#include "editor/scripting/ScriptComponents.h"
#include "editor/AppFlow.h"

#include "editor/plugins/EditorPlugins.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/LoadingScreen.h"
#include "editor/screens/LoginScreen.h"
#include "editor/screens/ProjectManagerScreen.h"
#include "editor/screens/SettingsScreen.h"
#include "editor/update/UpdateService.h"

#include <cstdio>
#include <cstdlib>
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
  render3d::set_blender_setting(settings_.blender_path);
  set_script_editor_preference(settings_.script_editor);
  render3d::source::set_content_search_folders(render3d::source::split_folder_list(settings_.source_search_folders));
  gfx::set_extra_image_decoder(&render3d::source::decode_vtf_rgba);  // .vtf thumbnails
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
  reload_templates();
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

  // The update check may have written these while Settings was open.
  draft.update_last_check = settings_.update_last_check;
  draft.update_cached_version = settings_.update_cached_version;
  draft.update_cached_url = settings_.update_cached_url;
  settings_ = std::move(draft);
  render3d::set_blender_setting(settings_.blender_path);
  set_script_editor_preference(settings_.script_editor);
  render3d::source::set_content_search_folders(render3d::source::split_folder_list(settings_.source_search_folders));
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

  tick_updates();
  if (state_ == AppState::Quit) {
    return;  // the installer took over
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
        if (next == AppState::Editor2D || next == AppState::Editor3D) {
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
    } else if ((state_ == AppState::Editor2D || state_ == AppState::Editor3D) &&
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
    switch (pm->take_update_action()) {
      case UpdateAction::None:
        break;
      case UpdateAction::Update:
        if (update_fake_) {
          update_error_ = "That's a test notice (TS_FAKE_UPDATE): there's nothing to download.";
          std::cout << "[Update] Update pressed on the TS_FAKE_UPDATE notice; nothing to fetch\n";
        } else if (updates_ && !update_downloading_) {
          update_error_.clear();
          update_downloading_ = true;
          updates_->start_download();
          std::cout << "[Update] fetching v" << update_version_ << '\n';
        }
        break;
      case UpdateAction::ReleaseNotes: {
        const std::string url = update_url_.empty() ? update::kReleasesPageUrl : update_url_;
        std::string err;
        if (updates_ && updates_->options().dry_run) {
          std::cout << "[Update] (dry run) would open " << url << '\n';
        } else if (!update::open_url(url, &err)) {
          std::cout << "[Update] could not open " << url << ": " << err << '\n';
          update_error_ = "Couldn't open the browser; the notes are at " + url;
        }
        break;
      }
      case UpdateAction::Later:
        update_visible_ = false;
        update_later_ = true;
        std::cout << "[Update] Later: v" << update_version_ << " hidden for this session\n";
        break;
      case UpdateAction::Skip:
        update_visible_ = false;
        settings_.update_skip_version = update_version_;
        persist_settings();
        std::cout << "[Update] skipping v" << update_version_ << " from now on\n";
        break;
    }
    push_update_banner();
    if (pm->new_project_requested()) {
      const std::string name = pm->pending_new_project_name();
      const ProjectKind kind = pm->pending_new_project_kind();
      pm->clear_new_project_request();
      create_new_project(name, kind);
    }
    if (pm->template_create_requested()) {
      const std::string name = pm->pending_new_project_name();
      const std::string id = pm->pending_template_id();
      const int vr = pm->pending_template_vr();
      const int dimension = pm->pending_template_dimension();
      pm->clear_template_create_request();
      create_from_template(name, id, vr, dimension);
    }
    if (pm->save_template_requested()) {
      const auto idx = pm->highlighted_index();
      const std::string name = pm->pending_template_name();
      const std::string desc = pm->pending_template_description();
      pm->clear_save_template_request();
      if (idx.has_value()) save_project_as_template(*idx, name, desc);
    }
    if (pm->set_vr_requested()) {
      const auto idx = pm->highlighted_index();
      const bool on = pm->pending_vr();
      pm->clear_set_vr_request();
      if (idx.has_value()) set_project_vr(*idx, on);
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
         current_state() == AppState::Editor3D;
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

void AppFlow::reload_templates() {
  templates_.refresh(settings_.projects_root);
  for (const std::string& e : templates_.errors()) std::cout << "[AppFlow] template: " << e << '\n';
  if (auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get())) pm->set_templates(&templates_);
}

namespace {
void pm_report(IScreen* screen, const std::vector<ProjectInfo>* projects, const std::string& status,
               const std::string& error, const std::string& highlight_id = {}) {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen);
  if (!pm) return;
  if (projects) pm->set_projects(*projects);
  pm->set_status_message(status);
  pm->set_error_message(error);
  if (!highlight_id.empty()) pm->highlight_project_by_id(highlight_id);
}
}  // namespace

bool AppFlow::create_from_template(const std::string& name, const std::string& template_id,
                                   int vr, int dimension) {
  last_error_.clear();
  const ProjectTemplate* tpl = templates_.find(template_id);
  if (!tpl) {
    last_error_ = "No template \"" + template_id + "\"";
    pm_report(screen_.get(), nullptr, {}, last_error_);
    return false;
  }
  ProjectInfo created;
  std::string err;
  if (!TemplateStore::create_project(project_store_, *tpl, name, &created, &err, vr, dimension)) {
    last_error_ = err;
    status_message_.clear();
    std::cout << "[AppFlow] create from template " << template_id << " failed: " << err << '\n';
    pm_report(screen_.get(), nullptr, {}, last_error_);
    return false;
  }
  status_message_ = std::string("Broke ground on ") + kind_label(created) + " claim \"" +
                    created.name + "\" from " + tpl->name;
  std::cout << "[AppFlow] " << status_message_ << " at " << created.path << '\n';
  pm_report(screen_.get(), &project_store_.projects(), status_message_, {}, created.id);
  return true;
}

bool AppFlow::save_project_as_template(std::size_t index, const std::string& name,
                                       const std::string& description) {
  last_error_.clear();
  if (index >= project_store_.projects().size()) {
    last_error_ = "No such project";
    return false;
  }
  const ProjectInfo& p = project_store_.projects()[index];
  ProjectTemplate t;
  std::string err;
  if (!TemplateStore::save_as_template(p, TemplateStore::user_template_dir(settings_.projects_root),
                                       name, description, &t, &err)) {
    last_error_ = err;
    pm_report(screen_.get(), nullptr, {}, last_error_);
    return false;
  }
  reload_templates();
  status_message_ = "Saved \"" + p.name + "\" as template \"" + t.name + "\"";
  std::cout << "[AppFlow] " << status_message_ << " at " << t.dir << '\n';
  pm_report(screen_.get(), nullptr, status_message_, {});
  return true;
}

bool AppFlow::set_project_vr(std::size_t index, bool on) {
  last_error_.clear();
  if (index >= project_store_.projects().size()) {
    last_error_ = "No such project";
    return false;
  }
  const ProjectInfo p = project_store_.projects()[index];
  std::string err;
  if (!ProjectStore::set_project_vr(p.path, on, &err)) {
    last_error_ = err;
    pm_report(screen_.get(), nullptr, {}, last_error_);
    return false;
  }
  project_store_.refresh();
  status_message_ = "VR " + std::string(on ? "on" : "off") + " for \"" + p.name + "\"";
  pm_report(screen_.get(), &project_store_.projects(), status_message_, {}, p.id);
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
  } else if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    e3->request_back_to_projects();
    tick(0.0f);
  }
}

void AppFlow::request_quit() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    editor->request_quit();
    tick(0.0f);
    return;
  }
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    e3->request_quit();
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
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    return e3->save_scene(error_out);
  }
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
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    return e3->duplicate_selected();
  }
  return 0;
}

std::size_t AppFlow::editor_delete_selected() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->delete_selected();
  }
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    return e3->delete_selected();
  }
  return 0;
}

bool AppFlow::editor_undo() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->undo();
  }
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    return e3->undo();
  }
  return false;
}

bool AppFlow::editor_redo() {
  if (auto* editor = dynamic_cast<Editor2DScreen*>(screen_.get())) {
    return editor->redo();
  }
  if (auto* e3 = dynamic_cast<Editor3DScreen*>(screen_.get())) {
    return e3->redo();
  }
  return false;
}

Editor3DScreen* AppFlow::editor3d_screen() {
  return dynamic_cast<Editor3DScreen*>(screen_.get());
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
  if (state_ == AppState::ProjectManager) {
    begin_update_check();
    push_update_banner();
  }
}

// ---- update notifier (docs/updates.md) ---------------------------------------

void AppFlow::enable_update_checks(const update::ServiceOptions* opt) {
  updates_enabled_ = true;
  updates_ = std::make_unique<update::UpdateService>(opt ? *opt : update::ServiceOptions{});
  update_checked_ = false;
  update_check_running_ = false;
  update_visible_ = false;
  update_fake_ = false;
  update_downloading_ = false;
  update_later_ = false;
  update_error_.clear();
  if (state_ == AppState::ProjectManager) {
    begin_update_check();
    push_update_banner();
  }
}

void AppFlow::press_update_action_for_smoke(UpdateAction action) {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get());
  if (!pm) return;
  pm->request_update_action(action);
  handle_pending_screen_actions();
}

void AppFlow::begin_update_check() {
  if (!updates_enabled_ || !updates_ || update_check_running_ || update_downloading_) return;
  const std::string current = updates_->options().current_version;
  if (!update_checked_) {
    const std::string fake = update::fake_update_version();
    if (!fake.empty()) {
      update_checked_ = true;
      update_fake_ = true;
      update_version_ = fake;
      update_url_ = update::kReleasesPageUrl;
      update_visible_ = true;
      std::cout << "[Update] TS_FAKE_UPDATE=" << fake << ": showing the test notice\n";
      return;
    }
  }
  if (update_fake_ || update_later_) return;
  if (const char* off = std::getenv("TS_NO_UPDATE_CHECK"); off && *off && *off != '0') {
    if (!update_checked_) std::cout << "[Update] TS_NO_UPDATE_CHECK set; not checking\n";
    update_checked_ = true;
    return;
  }
  if (!settings_.check_updates_on_start) {
    if (!update_checked_) std::cout << "[Update] checks are off in Settings\n";
    update_checked_ = true;
    update_visible_ = false;
    return;
  }
  if (!update::url_supported(updates_->options().latest_url)) {
    if (!update_checked_) std::cout << "[Update] update checks need https (the Windows editor); skipped here\n";
    update_checked_ = true;
    return;
  }
  const std::int64_t now = update::now_unix();
  const std::int64_t last = std::atoll(settings_.update_last_check.c_str());
  if (!update::check_due(last, now)) {
    // Within 6 hours of the last check: use what it found.
    if (!update_checked_) {
      const std::string& cached = settings_.update_cached_version;
      if (!cached.empty() && update::should_notify(cached, current, settings_.update_skip_version)) {
        update::ReleaseInfo r;
        r.tag = "v" + cached;
        r.version = cached;
        r.html_url = settings_.update_cached_url;
        updates_->set_release(r);
        update_version_ = cached;
        update_url_ = r.html_url;
        update_visible_ = true;
      }
      std::cout << "[Update] last checked " << (now - last) / 60 << " min ago; latest then: "
                << (cached.empty() ? "(none)" : "v" + cached) << '\n';
    }
    update_checked_ = true;
    return;
  }
  update_checked_ = true;
  update_check_running_ = true;
  std::cout << "[Update] asking GitHub for the latest release\n";
  updates_->start_check();
}

void AppFlow::tick_updates() {
  if (!updates_) return;
  const std::string current = updates_->options().current_version;
  if (update_check_running_) {
    const auto st = updates_->check_state();
    if (st != update::UpdateService::Check::Running && st != update::UpdateService::Check::Idle) {
      update_check_running_ = false;
      settings_.update_last_check = std::to_string(update::now_unix());
      if (st == update::UpdateService::Check::Failed) {
        // Offline, rate limited, GitHub down: say nothing in the UI.
        std::cout << "[Update] check failed (quietly): " << updates_->last_error() << '\n';
      } else {
        const update::ReleaseInfo r = updates_->release();
        settings_.update_cached_version = r.version;
        settings_.update_cached_url = r.html_url;
        if (update::should_notify(r.tag, current, settings_.update_skip_version)) {
          update_version_ = r.version;
          update_url_ = r.html_url;
          update_visible_ = !update_later_;
          std::cout << "[Update] v" << r.version << " is out (running v" << current << ")\n";
        } else {
          std::cout << "[Update] latest is " << r.tag << "; v" << current << " is up to date"
                    << (update::is_newer(r.tag, current) ? " (skipped)" : "") << '\n';
        }
      }
      persist_settings();
    }
  }
  if (update_downloading_) {
    const auto d = updates_->download_state();
    if (d == update::UpdateService::Download::Ready) {
      update_downloading_ = false;
      const std::string path = updates_->downloaded_path();
      std::cout << "[Update] downloaded " << path << (updates_->verified() ? " (SHA-256 verified)" : " (no checksum to verify)")
                << '\n';
      std::string err;
      if (updates_->options().dry_run) {
        std::cout << "[Update] (dry run) would run " << path << " and close the editor\n";
        request_quit();
        return;
      }
      if (update::launch_installer(path, &err)) {
        std::cout << "[Update] installer started; closing the editor\n";
        request_quit();
        return;
      }
      update_error_ = "Couldn't start the installer: " + err;
      std::cout << "[Update] " << update_error_ << '\n';
    } else if (d == update::UpdateService::Download::Failed) {
      update_downloading_ = false;
      update_error_ = "Couldn't fetch the update: " + updates_->last_error();
      std::cout << "[Update] " << update_error_ << '\n';
    }
  }
  push_update_banner();
}

void AppFlow::push_update_banner() {
  auto* pm = dynamic_cast<ProjectManagerScreen*>(screen_.get());
  if (!pm) return;
  UpdateBanner b;
  b.visible = update_visible_;
  b.version = update_version_;
  b.current = updates_ ? updates_->options().current_version : update::current_version();
  b.busy = update_downloading_;
  b.fake = update_fake_;
  b.error = update_error_;
  if (update_downloading_ && updates_) {
    b.progress = updates_->download_progress();
    char buf[96];
    if (b.progress >= 0.0) {
      std::snprintf(buf, sizeof(buf), "Fetching v%s... %d%%", update_version_.c_str(),
                    static_cast<int>(b.progress * 100.0 + 0.5));
    } else {
      std::snprintf(buf, sizeof(buf), "Fetching v%s...", update_version_.c_str());
    }
    b.note = buf;
  }
  pm->set_update_banner(std::move(b));
}

std::unique_ptr<IScreen> AppFlow::make_screen(AppState state) {
  switch (state) {
    case AppState::Loading:
      return std::make_unique<LoadingScreen>();
    case AppState::Login:
      return std::make_unique<LoginScreen>(settings_.username,
                                           settings_.auto_login_dev);
    case AppState::ProjectManager:
    {
      auto pm = std::make_unique<ProjectManagerScreen>(
          project_store_.projects(), settings_.projects_root, status_message_);
      pm->set_templates(&templates_);
      return pm;
    }
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
    case AppState::Editor3D: {
      ProjectInfo project = active_project_
                                ? *active_project_
                                : ProjectInfo{.id = "unknown",
                                              .name = "Untitled 3D",
                                              .kind = ProjectKind::ThreeD};
      return std::make_unique<Editor3DScreen>(std::move(project));
    }
    case AppState::Quit:
      break;
  }
  return nullptr;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
