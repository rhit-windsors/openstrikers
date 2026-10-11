#pragma once

#include <cstdint>

namespace net {

enum StateHashComponent {
    kHashRandom, // nlDefaultSeed
    kHashFrame,  // task state and the fixed-update clock
    kHashMatch,  // cGame state and match clock
    kHashBall,
    kHashHome,   // team 0: score, powerups, every player
    kHashAway,   // team 1
    kHashComponents,
};

struct StateHash {
    uint32_t combined;
    uint32_t component[kHashComponents];
};

// Reads the game's globals; safe at any point, including before a match exists.
StateHash computeStateHash();
const char* stateHashComponentName(int index);

} // namespace net
