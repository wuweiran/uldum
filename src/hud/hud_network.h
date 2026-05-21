#pragma once

#include <span>

#include "core/types.h"

namespace uldum::hud {

class Hud;

// Decode an `S_HUD_*` message payload (type byte + body, in protocol.h's
// wire format) and apply it to the target Hud. App-side bridge between
// NetworkManager's raw client receive and HUD mutation, so networking
// doesn't have to link the HUD library.
//
// Returns true if the message was a recognized HUD packet (regardless of
// whether it changed anything). Returns false for any non-HUD opcode so
// callers can fall through to the next dispatch path.
bool apply_network_message(Hud& hud, std::span<const u8> data);

} // namespace uldum::hud
