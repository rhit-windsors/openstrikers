'use strict';
// A cup for the lobby, run the way the game runs its own (extern/decomp
// src/Game/GameInfo.cpp, src/Game/DB/StatsTracker.cpp): a round robin between
// 4, 6 or 8 captains, twice over (home and away swapped) in the Super cups,
// and in the Bowser cups a knockout between the top four afterwards. The
// players are one team; their matches are played in the game, every other
// match is decided the way the game decides the ones you do not watch.
//
// Plain data in and out, so the host can send it to everyone as part of the
// lobby state, and save it to resume later.

const CUPS = [
  { id: 'mushroom', name: 'Mushroom Cup', teams: 4, twice: false, finalStadium: 1 },
  { id: 'flower', name: 'Flower Cup', teams: 6, twice: false, finalStadium: 0 },
  { id: 'star', name: 'Star Cup', teams: 8, twice: false, finalStadium: 3 },
  { id: 'bowser', name: 'Bowser Cup', teams: 8, twice: false, finalStadium: 5, knockout: true },
  { id: 'super-mushroom', name: 'Super Mushroom Cup', teams: 4, twice: true, finalStadium: 4 },
  { id: 'super-flower', name: 'Super Flower Cup', teams: 6, twice: true, finalStadium: 2 },
  { id: 'super-star', name: 'Super Star Cup', teams: 8, twice: true, finalStadium: 6 },
  { id: 'super-bowser', name: 'Super Bowser Cup', teams: 8, twice: true, finalStadium: 5, knockout: true },
];

// The game's fixtures (GameInfo.cpp *_TEAM_MATCHUPS), Teams/2 a round, by
// lineup position.
const MATCHUPS = {
  4: [[0, 1], [2, 3], [3, 1], [2, 0], [0, 3], [1, 2]],
  6: [[0, 1], [5, 4], [3, 2], [3, 5], [1, 4], [2, 0], [4, 2], [0, 3], [5, 1], [1, 3], [5, 2], [4, 0], [0, 5],
    [2, 1], [3, 4]],
  8: [[4, 0], [1, 5], [7, 3], [2, 6], [6, 1], [0, 5], [3, 4], [7, 2], [5, 3], [2, 4], [1, 7], [6, 0], [0, 7],
    [3, 6], [5, 2], [4, 1], [3, 2], [7, 6], [0, 1], [4, 5], [5, 7], [2, 0], [6, 4], [1, 3], [2, 1], [4, 7],
    [6, 5], [0, 3]],
};

const TEAM_COUNT = 8; // eTeamID 0..7; the Super Team is not offered
const SIDEKICK_COUNT = 4;
const STADIUM_COUNT = 7;

// mulberry32: small, seedable, so tests can pin every draw.
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function nextRandom(cup) {
  const r = rng(cup.seed)();
  cup.seed = (Math.floor(r * 4294967296) ^ 0x9e3779b9) >>> 0;
  return r;
}

function randomInt(cup, n) {
  return Math.floor(nextRandom(cup) * n);
}

// StatsTracker::SimulateGame: fifteen chances a side at 20%; level goes to
// overtime, a coin toss.
function simulate(cup) {
  let home = 0;
  let away = 0;
  for (let i = 0; i < 15; ++i) {
    if (nextRandom(cup) < 0.2) home++;
    if (nextRandom(cup) < 0.2) away++;
  }
  if (home !== away) return { home, away, ot: false };
  if (nextRandom(cup) < 0.5) home++;
  else away++;
  return { home, away, ot: true };
}

function pickStadium(cup, last) {
  const def = CUPS[cup.kind];
  if (last) return def.finalStadium;
  for (;;) {
    const s = randomInt(cup, STADIUM_COUNT);
    if (s !== def.finalStadium) return s;
  }
}

// The players' team is put in at random among captains drawn at random
// (SHCupChooseCaptain CreateLineup), each with a random sidekick.
function createCup({ kind, team, sidekick, seed = Date.now() }) {
  const def = CUPS[kind];
  if (!def) throw new Error('no such cup');
  const cup = { kind, seed: seed >>> 0, entrants: [], rounds: [], round: 0, knockout: null, done: false, eliminated: false };
  const others = [...Array(TEAM_COUNT).keys()].filter((t) => t !== team);
  for (let i = others.length - 1; i > 0; --i) {
    const j = randomInt(cup, i + 1);
    [others[i], others[j]] = [others[j], others[i]];
  }
  const lineup = others.slice(0, def.teams - 1).map((t) => ({ team: t, sidekick: randomInt(cup, SIDEKICK_COUNT), human: false }));
  lineup.splice(randomInt(cup, def.teams), 0, { team, sidekick, human: true });
  cup.entrants = lineup;

  const fixtures = MATCHUPS[def.teams];
  const perRound = def.teams / 2;
  const legs = def.twice ? 2 : 1;
  const totalRounds = (def.teams - 1) * legs;
  for (let leg = 0; leg < legs; ++leg) {
    for (let r = 0; r < def.teams - 1; ++r) {
      const round = [];
      for (let g = 0; g < perRound; ++g) {
        const [a, b] = fixtures[r * perRound + g];
        const [home, away] = leg === 0 ? [a, b] : [b, a];
        round.push({ home, away, stadium: 0, result: null });
      }
      const number = leg * (def.teams - 1) + r;
      const last = !def.knockout && number === totalRounds - 1;
      const stadium = pickStadium(cup, last);
      for (const m of round) m.stadium = stadium;
      cup.rounds.push(round);
    }
  }
  return cup;
}

function humanIndex(cup) {
  return cup.entrants.findIndex((e) => e.human);
}

// Every entrant's record: played, wins, overtime wins and losses, losses,
// goals, points (3 a win of either kind, 1 an overtime loss), best first.
function standings(cup) {
  const rows = cup.entrants.map((e, i) => ({ index: i, team: e.team, human: e.human, played: 0, won: 0, otWon: 0, otLost: 0, lost: 0, goalsFor: 0, goalsAgainst: 0, points: 0 }));
  for (const round of cup.rounds) {
    for (const m of round) {
      if (!m.result) continue;
      const { home, away, ot } = m.result;
      const h = rows[m.home];
      const a = rows[m.away];
      h.played++; a.played++;
      h.goalsFor += home; h.goalsAgainst += away;
      a.goalsFor += away; a.goalsAgainst += home;
      const [w, l] = home > away ? [h, a] : [a, h];
      if (ot) { w.otWon++; l.otLost++; w.points += 3; l.points += 1; } else { w.won++; l.lost++; w.points += 3; }
    }
  }
  return rows.sort((x, y) => y.points - x.points ||
    (y.goalsFor - y.goalsAgainst) - (x.goalsFor - x.goalsAgainst) ||
    y.goalsFor - x.goalsFor || x.index - y.index);
}

// The players' next match, or null when the cup is over:
// { stage: 'league'|'semi'|'final', round, home, away, stadium } (lineup indices).
function nextMatch(cup) {
  if (cup.done) return null;
  const me = humanIndex(cup);
  if (!cup.knockout) {
    const m = cup.rounds[cup.round].find((x) => x.home === me || x.away === me);
    return { stage: 'league', round: cup.round, ...m };
  }
  const k = cup.knockout;
  const stage = k.final ? 'final' : 'semi';
  const m = (k.final ? [k.final] : k.semis).find((x) => x.home === me || x.away === me);
  return m ? { stage, round: cup.round, ...m } : null;
}

function winnerOf(m) {
  return m.result.home > m.result.away ? m.home : m.away;
}

// Record the players' match (scores from their side: `us`, `them`) and play
// out everything else up to their next match. Forfeits count 3-0 to the other
// side, as in the game.
function recordResult(cup, { us, them, ot = false, forfeit = false }) {
  const m = nextMatch(cup);
  if (!m) throw new Error('the cup is over');
  const me = humanIndex(cup);
  if (forfeit) {
    // Whoever has -5 forfeited (StatsTracker::TrackWinner).
    const weLost = us < 0;
    us = weLost ? 0 : Math.max(us, 3);
    them = weLost ? Math.max(them, 3) : 0;
    ot = false;
  }
  if (!(us >= 0 && them >= 0) || us === them) throw new Error('a match needs a winner');
  const usHome = m.home === me;
  const result = { home: usHome ? us : them, away: usHome ? them : us, ot: !!ot };

  if (!cup.knockout) {
    for (const g of cup.rounds[cup.round]) {
      g.result = g.home === m.home && g.away === m.away ? result : simulate(cup);
    }
    advanceLeague(cup);
  } else {
    const k = cup.knockout;
    if (k.final) {
      k.final.result = result;
      finish(cup, winnerOf(k.final));
    } else {
      for (const g of k.semis) g.result = g.home === m.home && g.away === m.away ? result : simulate(cup);
      startFinal(cup);
    }
  }
  return cup;
}

function advanceLeague(cup) {
  if (cup.round + 1 < cup.rounds.length) {
    cup.round++;
    return;
  }
  const def = CUPS[cup.kind];
  if (!def.knockout) {
    finish(cup, standings(cup)[0].index);
    return;
  }
  // The top four go through: 1st v 4th and 2nd v 3rd (BOWSER_KNOCKOUT_ORDER).
  const top = standings(cup).slice(0, 4).map((r) => r.index);
  cup.round++;
  cup.knockout = {
    semis: [
      { home: top[0], away: top[3], stadium: pickStadium(cup, false), result: null },
      { home: top[1], away: top[2], stadium: pickStadium(cup, false), result: null },
    ],
    final: null,
  };
  if (!top.includes(humanIndex(cup))) {
    // Out: the rest is played without the players.
    cup.eliminated = true;
    for (const g of cup.knockout.semis) g.result = simulate(cup);
    startFinal(cup);
  }
}

function startFinal(cup) {
  const k = cup.knockout;
  cup.round++;
  k.final = { home: winnerOf(k.semis[0]), away: winnerOf(k.semis[1]), stadium: CUPS[cup.kind].finalStadium, result: null };
  const me = humanIndex(cup);
  if (k.final.home !== me && k.final.away !== me) {
    cup.eliminated = true;
    k.final.result = simulate(cup);
    finish(cup, winnerOf(k.final));
  }
}

function finish(cup, champion) {
  cup.done = true;
  cup.champion = champion;
}

// For the lobby: how far through, in words.
function progress(cup) {
  const def = CUPS[cup.kind];
  if (cup.done) return 'Finished';
  if (!cup.knockout) return `Round ${cup.round + 1} of ${cup.rounds.length}`;
  return cup.knockout.final ? 'Final' : 'Semi-finals';
}

// A cup restored from disk or the network, checked enough to use.
function validCup(cup) {
  return !!cup && typeof cup === 'object' && Number.isInteger(cup.kind) && CUPS[cup.kind] &&
    Array.isArray(cup.entrants) && cup.entrants.length === CUPS[cup.kind].teams &&
    Array.isArray(cup.rounds) && Number.isInteger(cup.round) && humanIndex(cup) >= 0;
}

module.exports = { CUPS, createCup, recordResult, nextMatch, standings, simulate, progress, humanIndex, validCup };
