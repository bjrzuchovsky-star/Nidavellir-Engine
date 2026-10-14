#pragma once

// Headless --smoke checks for multiplayer: the byte codec and transports
// first, then (as they land) the protocol, server and clients over
// loopback. Returns 0 when every check passes.
int run_network_smoke();
// SmokeNetwork.cpp: ByteWriter / ByteReader, endpoints, ENet over
// loopback, the link simulator.
int run_transport_smoke();
// Loopback posse (server + 3 clients under lag) and the editor's Play with
// posse (Editor2DScreen + LocalPosse).
int run_editor_posse_smoke();
