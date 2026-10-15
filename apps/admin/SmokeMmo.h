#pragma once
// Headless checks for the persistent world (ts_mmo): crypto, SQLite store,
// protocol, zones, a live server over ENet (logins, interest, movement and
// anti-cheat, chat, pickups, resume, admin, persistence, Strongbox), the
// bot load test and the editor's Town Hall panel. 0 = pass.
int run_mmo_smoke();
