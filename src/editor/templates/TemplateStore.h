#pragma once

// Project templates ("Claim Patterns"): data-driven starting points for a
// new project. Each template is a folder with a template.json and,
// optionally, a project/ folder that is copied (and renamed) into the
// projects root when somebody breaks ground with it:
//
//   templates/ghost-town-3d/
//     template.json     {"id", "name", "dimension": "2d|3d", "vr": bool,
//                        "description", "plugins": [..], "thumbnail",
//                        "tags": [..], "order", "project": "project",
//                        "generator": "ghost-town-3d", "group": "empty|samples",
//                        "alt_dimension": "3d", "alt_project": "project-3d",
//                        "alt_generator": "truly-blank-3d"}
//     thumbnail.png     optional card picture
//     project/          optional; copied as the new project's folder
//
// No project/ folder = a blank claim of that dimension (plus the template's
// plugin list). "generator" names a built-in sample writer; it is used by
// `ts_admin --bake-templates` to (re)build project/ and as a fallback when
// project/ is missing.
//
// Engine templates ship next to ts_admin (<exe dir>/templates, copied by the
// build) with the source tree as a fallback; $TS_TEMPLATES_PATH adds more.
// "Save as Template" writes user templates to <projects root>/.templates
// (a dot folder, so the project scan skips it).

#include "editor/ProjectInfo.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class ProjectStore;

struct ProjectTemplate {
  std::string id;           // folder name (unique; user wins over engine)
  std::string name;
  std::string description;
  ProjectKind kind = ProjectKind::TwoD;  // TwoD or ThreeD
  bool vr = false;                       // 3D only: VR switch default
  std::vector<std::string> plugins;
  bool plugins_set = false;  // false = the default plugins for the kind
  std::vector<std::string> tags;
  std::string dir;           // template folder
  std::string thumbnail;     // absolute path, "" when none
  std::string project_dir;   // folder to copy, "" for a blank claim
  std::string generator;     // built-in sample writer id, "" for none
  int order = 1000;
  bool user = false;         // saved with "Save as Template"
  // Picker group: "empty" (clean starting points), "samples" (the default
  // for engine templates) or "mine" (user templates, always).
  std::string group = "samples";
  // Optional second dimension (Truly Blank: 2D or 3D). The create flow's
  // 2D / 3D switch picks it; the alt folder / generator build that one.
  bool has_alt = false;
  ProjectKind alt_kind = ProjectKind::ThreeD;
  std::string alt_project_dir;  // "" = run alt_generator
  std::string alt_generator;
  bool blank() const { return project_dir.empty() && generator.empty(); }
  bool empty_group() const { return group == "empty"; }
};

class TemplateStore {
 public:
  TemplateStore() = default;

  // Folders scanned, in order (later ids do not replace earlier ones,
  // except user templates, which are listed separately).
  static std::vector<std::string> engine_template_dirs();
  static std::string user_template_dir(const std::string& projects_root);

  // Scans engine dirs + the user folder for `projects_root`.
  void refresh(const std::string& projects_root);
  // Scans exactly these folders (smokes / tools).
  void refresh_dirs(const std::vector<std::string>& engine_dirs, const std::string& user_dir);

  const std::vector<ProjectTemplate>& templates() const { return templates_; }
  const ProjectTemplate* find(const std::string& id) const;
  const std::vector<std::string>& errors() const { return errors_; }

  static bool load_template(const std::string& dir, bool user, ProjectTemplate* out,
                            std::string* error);

  // Creates <root>/<slug(name)> from the template: copy project/ (or run the
  // generator, or write a blank project.json), then stamp project.json with
  // the new name, dimension, fresh created time, no last_opened and the
  // template's plugins. Refreshes the store. Fails if the folder exists.
  // vr: the create flow's VR switch (3D only); -1 = the template's default.
  // dimension: the 2D / 3D switch for templates with an alt dimension
  // (0 = 2D, 1 = 3D, -1 = the template's own).
  static bool create_project(ProjectStore& store, const ProjectTemplate& tpl,
                             const std::string& name, ProjectInfo* out, std::string* error,
                             int vr = -1, int dimension = -1);

  // Copies a project into <user dir>/<slug(name)>/project and writes
  // template.json (dimension + plugins from the project, the description
  // given, thumbnail.png from the project folder when it has one). Skips
  // editor-only clutter (imgui.ini, dot folders, export depots).
  static bool save_as_template(const ProjectInfo& project, const std::string& user_dir,
                               const std::string& name, const std::string& description,
                               ProjectTemplate* out, std::string* error);

  // Built-in writers: the samples ("sample-2d-platformer", "ghost-town-3d",
  // "haunted-saloon-vr", "frontier-survival", "sky-pirates") and the empty
  // templates (editor/templates/EmptyTemplates.h).
  static std::vector<std::string> generator_ids();
  static bool run_generator(const std::string& id, const std::string& dir,
                            const std::string& name, std::string* error);

  // ts_admin --bake-templates: (re)writes project/ (and the alt project) for
  // every engine template with a generator under `templates_root`. Returns
  // how many were baked. Thumbnails: see TemplateThumbs.h.
  static int bake(const std::string& templates_root, std::vector<std::string>* errors);

 private:
  std::vector<ProjectTemplate> templates_;
  std::vector<std::string> errors_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
