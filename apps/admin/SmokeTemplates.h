#pragma once

// Headless checks for project templates: the engine templates/ folder,
// create (copy + rename) for every template, the VR switch, no seeding of
// samples into a fresh root (old roots untouched), Save as Template and the
// generator fallback. 0 when all pass.
int run_templates_smoke();

// ts_admin --bake-templates <templates dir>: rebuild each template's
// project/ folder from its built-in sample generator.
int run_bake_templates(const char* dir);
