// A compact fingerprint of the game state, taken once a frame.
//
// Lockstep netplay only works if every machine computes the same game from the
// same input, and when one does not, the useful question is which part of the
// game went wrong first. So the fingerprint is split into components -- the
// random generator, the frame clock, the match state, the ball, each team's
// players -- and both input replay and netplay compare them component by
// component. Values are hashed by their bits, not compared with a tolerance:
// determinism here means bit-identical floats.

#include "net/state_hash.h"

#include "Game/AI/Fielder.h"
#include "Game/Ball.h"
#include "Game/CharacterTemplate.h"
#include "Game/FixedUpdateTask.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include "NL/nlMath.h"
#include "NL/nlTask.h"

#include <cstring>

extern cTeam* g_pTeams[];

namespace net {
namespace {

struct Fnv {
    u32 h = 2166136261u;
    void bytes(const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 16777619u;
        }
    }
    template <typename T>
    void add(const T& v) {
        bytes(&v, sizeof(v));
    }
    void vec(const nlVector3& v) { bytes(v.e, sizeof(v.e)); }
};

// Players are named by their slot, never by address: addresses differ between
// processes.
s32 playerSlot(const cPlayer* p) {
    if (p == nullptr) {
        return -1;
    }
    for (int t = 0; t < 2; ++t) {
        if (g_pTeams[t] == nullptr) {
            continue;
        }
        for (int i = 0; i < 5; ++i) {
            if (g_pTeams[t]->m_pPlayers[i] == p) {
                return t * 5 + i;
            }
        }
    }
    return -2;
}

u32 hashTeam(int side) {
    Fnv f;
    cTeam* team = g_pTeams[side];
    if (team == nullptr) {
        return 0;
    }
    f.add(team->m_nScore);
    f.add(team->m_nCurrentPowerUp);
    f.add(team->mfPowerupMeter);
    f.add(team->mfPowerupTimer);
    f.add(team->mpCurrentSituation);
    for (int i = 0; i < 5; ++i) {
        cPlayer* p = team->m_pPlayers[i];
        if (p == nullptr) {
            f.add(-1);
            continue;
        }
        f.vec(p->m_v3Position);
        f.vec(p->m_v3Velocity);
        f.add(p->m_aActualFacingDirection);
        f.add(p->m_aActualMovementDirection);
        f.add(p->m_fActualSpeed);
        f.add(p->m_eMovementState);
        f.add(p->m_eAnimID);
        // Which pad drives this player, by port: control swaps are game state.
        s32 pad = -1;
        if (p->m_pController != nullptr && p->m_pController->m_pGlobalPad != nullptr) {
            pad = p->m_pController->m_pGlobalPad->m_padIndex;
        }
        f.add(pad);
        if (p->m_eClassType == FIELDER) {
            const cFielder* fielder = static_cast<const cFielder*>(p);
            f.add(fielder->m_eActionState);
            f.add(fielder->m_eFielderDesireState);
        } else if (p->m_eClassType == GOALIE) {
            f.add(static_cast<const Goalie*>(p)->mGoalieActionState);
        }
    }
    return f.h;
}

} // namespace

StateHash computeStateHash() {
    StateHash out{};

    {
        Fnv f;
        f.add(nlDefaultSeed);
        out.component[kHashRandom] = f.h;
    }
    {
        Fnv f;
        if (nlTaskManager::m_pInstance != nullptr) {
            f.add(nlTaskManager::m_pInstance->m_CurrState);
        }
        f.add(FixedUpdateTask::mSimulationTime);
        f.add(FixedUpdateTask::mAccumulatedDeltaT);
        f.add(FixedUpdateTask::mTimeScale);
        out.component[kHashFrame] = f.h;
    }
    if (g_pGame != nullptr) {
        Fnv f;
        f.add(g_pGame->m_eGameState);
        if (g_pGame->m_pGameClock != nullptr) {
            f.add(g_pGame->m_pGameClock->m_fTimer);
        }
        out.component[kHashMatch] = f.h;
    }
    if (g_pBall != nullptr) {
        Fnv f;
        f.vec(g_pBall->m_v3Position);
        f.vec(g_pBall->m_v3Velocity);
        f.add(playerSlot(g_pBall->m_pOwner));
        out.component[kHashBall] = f.h;
    }
    if (g_pGame != nullptr) {
        out.component[kHashHome] = hashTeam(0);
        out.component[kHashAway] = hashTeam(1);
    }

    Fnv all;
    all.bytes(out.component, sizeof(out.component));
    out.combined = all.h;
    return out;
}

const char* stateHashComponentName(int index) {
    static const char* const kNames[kHashComponents] = {"random", "frame", "match", "ball", "home", "away"};
    return index >= 0 && index < kHashComponents ? kNames[index] : "?";
}

} // namespace net
