#pragma once

#include <string>

// Headless checks for project templates: the engine templates/ folder,
// create (copy + rename) for every template, the VR switch, no seeding of
// samples into a fresh root (old roots untouched), Save as Template and the
// generator fallback. 0 when all pass.
int run_templates_smoke();

// ts_admin --bake-templates <templates dir>: rebuild each template's
// project/ folder from its built-in sample generator.
int run_bake_templates(const char* dir);

// ts_admin --bake-thumbnails <templates dir>: renders thumbnail.png for
// every template (2D: runtime draw list; 3D: GL, needs a display).
int run_bake_thumbnails(const char* dir);

// Part of --smoke-templates: creates a claim from every engine template in
// a temp folder, opens it in the 2D / 3D editor (headless), checks what it
// holds and rides Play for a few frames (VR: the simulator rig too).
int run_template_play_smoke(std::string* note);
