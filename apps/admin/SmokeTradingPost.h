#pragma once

// Headless checks for the Trading Post (plugin store): JSON, SHA-256 and
// Ed25519 vectors, URL helpers, built-in + coming-soon listing, search and
// categories, local-folder install / uninstall into a temp plugin cache
// (found by the plugin search), a signed remote index over file:// and a
// loopback HTTP server, refusals (bad / untrusted signature, hash and size
// mismatch, mislabelled library), updates, and per-project enable with
// dependencies. 0 when all pass.
int run_trading_post_smoke();

// ts_admin --store-sign <index.json> <seed hex (64)>: prints the publisher
// public key and writes <index.json>.sig.
int run_store_sign(int argc, char** argv);
