#pragma once

// The host side of determinism: input recording and replay, and lockstep
// netplay. A session that is active makes the game deterministic -- fixed
// frame delta, an agreed random seed, disc reads completing at fixed points,
// and every controller's input supplied per frame -- and checks that it stays
// that way with a per-frame state hash.
//
//   --net-host [port] / --net-join host[:port]   play over the network
//   OPENSTRIKERS_NET_RECORD=<file>   play normally and record every frame's
//                                    input and state hash
//   OPENSTRIKERS_NET_REPLAY=<file>   feed a recording back and compare the
//                                    state hash every frame; exits when the
//                                    recording ends (code 3 on any mismatch)
//   OPENSTRIKERS_NET_JITTER_MS=<n>   sleep a random 0..n ms every frame, to show
//                                    that wall-clock timing no longer matters

#include "launch_options.h"

namespace net {

// Call once from main() after the disc is open and before game_main(). For a
// network session this connects, and waits until every player is in.
void sessionStart(const LaunchOptions& options);

// True while the game must be deterministic (a session is active).
bool sessionDeterministic();

// The game's 16:9 mode while a session fixes it (0 or 1), else -1. The game
// moves the camera for 16:9, so it is part of the match, not a local choice.
int sessionWidescreen();

} // namespace net
