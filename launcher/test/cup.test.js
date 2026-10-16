'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { CUPS, createCup, recordResult, nextMatch, standings, humanIndex, progress } = require('../src/cup');

const kind = (id) => CUPS.findIndex((c) => c.id === id);

function play(cup, win = true) {
  return recordResult(cup, win ? { us: 2, them: 1 } : { us: 0, them: 3 });
}

test('a Mushroom Cup is a three-round round robin of four', () => {
  const cup = createCup({ kind: kind('mushroom'), team: 3, sidekick: 2, seed: 1 });
  assert.equal(cup.entrants.length, 4);
  assert.equal(new Set(cup.entrants.map((e) => e.team)).size, 4);
  assert.equal(cup.entrants[humanIndex(cup)].team, 3);
  assert.equal(cup.rounds.length, 3);
  // Everyone meets everyone once.
  const pairs = new Set(cup.rounds.flat().map((m) => [m.home, m.away].sort().join()));
  assert.equal(pairs.size, 6);
  // The last round is at the cup's own stadium.
  assert.ok(cup.rounds[2].every((m) => m.stadium === 1));
  for (let r = 0; r < 3; ++r) {
    assert.equal(progress(cup), `Round ${r + 1} of 3`);
    const m = nextMatch(cup);
    assert.ok(m.home === humanIndex(cup) || m.away === humanIndex(cup));
    play(cup);
  }
  assert.equal(cup.done, true);
  assert.equal(nextMatch(cup), null);
  // Three wins: nobody can catch 9 points.
  assert.equal(cup.champion, humanIndex(cup));
  const table = standings(cup);
  assert.equal(table[0].human, true);
  assert.equal(table[0].points, 9);
  assert.equal(table.reduce((n, r) => n + r.played, 0), 12);
});

test('points: 3 a win, 3 an overtime win, 1 an overtime loss', () => {
  const cup = createCup({ kind: kind('mushroom'), team: 0, sidekick: 0, seed: 7 });
  recordResult(cup, { us: 1, them: 2, ot: true });
  const me = standings(cup).find((r) => r.human);
  assert.equal(me.otLost, 1);
  assert.equal(me.points, 1);
  assert.throws(() => recordResult(cup, { us: 1, them: 1 }), /winner/);
  recordResult(cup, { us: -5, them: 0, forfeit: true });
  const after = standings(cup).find((r) => r.human);
  assert.equal(after.lost, 1);
  assert.equal(after.goalsAgainst >= 5, true);
});

test('Super cups play everyone twice, home and away swapped', () => {
  const cup = createCup({ kind: kind('super-flower'), team: 5, sidekick: 1, seed: 3 });
  assert.equal(cup.rounds.length, 10);
  const first = cup.rounds[0][0];
  const back = cup.rounds[5][0];
  assert.deepEqual([back.home, back.away], [first.away, first.home]);
  while (!cup.done) play(cup);
  assert.equal(standings(cup).find((r) => r.human).played, 10);
});

test('Bowser Cup: a league of eight, then the top four knock out', () => {
  const cup = createCup({ kind: kind('bowser'), team: 6, sidekick: 3, seed: 11 });
  assert.equal(cup.rounds.length, 7);
  for (let r = 0; r < 7; ++r) play(cup);
  assert.ok(cup.knockout);
  const top = standings(cup).slice(0, 4).map((r) => r.index);
  assert.equal(top[0], humanIndex(cup)); // seven wins
  assert.deepEqual([cup.knockout.semis[0].home, cup.knockout.semis[0].away], [top[0], top[3]]);
  assert.equal(nextMatch(cup).stage, 'semi');
  play(cup);
  assert.equal(nextMatch(cup).stage, 'final');
  assert.equal(nextMatch(cup).stadium, 5);
  play(cup);
  assert.equal(cup.done, true);
  assert.equal(cup.champion, humanIndex(cup));
});

test('losing everything in a Bowser Cup ends it without you', () => {
  const cup = createCup({ kind: kind('bowser'), team: 1, sidekick: 0, seed: 5 });
  for (let r = 0; r < 7; ++r) play(cup, false);
  assert.equal(cup.done, true);
  assert.equal(cup.eliminated, true);
  assert.notEqual(cup.champion, humanIndex(cup));
  assert.ok(cup.knockout.final.result);
});

test('the draw is the seed\'s: the same seed, the same cup', () => {
  const a = createCup({ kind: kind('star'), team: 2, sidekick: 2, seed: 99 });
  const b = createCup({ kind: kind('star'), team: 2, sidekick: 2, seed: 99 });
  assert.deepEqual(a, b);
  play(a);
  play(b);
  assert.deepEqual(a, b);
  // and it survives JSON (it goes over the network and to disk).
  const c = JSON.parse(JSON.stringify(a));
  play(a);
  play(c);
  assert.deepEqual(a, c);
});
