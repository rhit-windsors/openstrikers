'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { LobbyHost, LobbyClient, parseAddress, planMatch } = require('../src/lobby');
const { onlineArgs, offlineArgs, defaultSettings } = require('../src/gameargs');

async function waitFor(emitter, predicate, timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const [state] = await Promise.race([
      once(emitter, 'state'),
      new Promise((_, reject) => setTimeout(() => reject(new Error('timed out')), Math.max(1, deadline - Date.now()))),
    ]);
    if (predicate(state)) return state;
  }
}

async function lobby(names, build = 'b1') {
  const host = new LobbyHost({ name: names[0], build, port: 0, beacon: false });
  const port = await host.listen();
  const clients = [];
  for (const name of names.slice(1)) {
    const c = new LobbyClient({ address: '127.0.0.1', port, name, build });
    await c.connect();
    clients.push(c);
  }
  return { host, clients };
}

function closeAll({ host, clients }) {
  for (const c of clients) c.close();
  host.close();
}

test('players join, get a fielder each, and the host can start', async () => {
  const l = await lobby(['Riley', 'Sam']);
  try {
    const [sam] = l.clients;
    assert.equal(sam.state.players.length, 2);
    assert.equal(sam.state.slots[0], 0); // host: home captain
    assert.equal(sam.state.slots[4], sam.selfId); // first guest: away captain
    assert.equal(sam.state.problem, '');
    const started = once(sam, 'start');
    const hostStarted = once(l.host, 'start');
    l.host.act({ t: 'start' });
    const [{ seat, plan }] = await started;
    const [hostStart] = await hostStarted;
    assert.equal(seat, 1);
    assert.equal(hostStart.seat, 0);
    assert.equal(plan.players, 2);
    assert.equal(plan.slots, '0,-,-,-,1,-,-,-');
  } finally {
    closeAll(l);
  }
});

test('fielder picks, releases and the four-per-machine limit', async () => {
  const l = await lobby(['Riley', 'Sam', 'Alex']);
  try {
    const [sam, alex] = l.clients;
    // Alex took home sidekick 1 by default (after the two captains).
    assert.equal(alex.state.slots[1], alex.selfId);
    sam.act({ t: 'slot', slot: 1 }); // taken: ignored
    sam.act({ t: 'slot', slot: 5 });
    sam.act({ t: 'slot', slot: 6 });
    sam.act({ t: 'slot', slot: 7 });
    let s = await waitFor(sam, (st) => st.slots[7] === sam.selfId);
    assert.equal(s.slots[1], alex.selfId);
    const notice = once(sam, 'notice');
    l.host.act({ t: 'slot', slot: 3 });
    sam.act({ t: 'slot', slot: 2 }); // fifth: refused
    assert.match((await notice)[0], /at most four/);
    sam.act({ t: 'slot', slot: 4 }); // let go of the captaincy
    s = await waitFor(sam, (st) => st.slots[4] === null);
    assert.equal(planMatch(s).slots, '0,2,-,0,-,1,1,1');
  } finally {
    closeAll(l);
  }
});

test('only a captain picks their team; the host picks for computer sides', async () => {
  const l = await lobby(['Riley', 'Sam']);
  try {
    const [sam] = l.clients;
    sam.act({ t: 'team', side: 'home', team: 7 }); // not Sam's side
    sam.act({ t: 'team', side: 'away', team: 5, sidekick: 3 });
    let s = await waitFor(sam, (st) => st.match.away.team === 5);
    assert.equal(s.match.away.sidekick, 3);
    assert.equal(s.match.home.team, 3);
    sam.act({ t: 'slot', slot: 4 });
    await waitFor(sam, (st) => st.slots[4] === null);
    l.host.act({ t: 'team', side: 'away', team: 0 });
    s = await waitFor(sam, (st) => st.match.away.team === 0);
    // Match rules are the host's alone.
    sam.act({ t: 'match', match: { minutes: 9 } });
    l.host.act({ t: 'match', match: { minutes: 5, stadium: 6, items: false, delay: 7 } });
    s = await waitFor(sam, (st) => st.match.minutes === 5);
    assert.equal(s.match.stadium, 6);
    assert.equal(s.match.items, false);
    assert.equal(s.match.delay, 7);
  } finally {
    closeAll(l);
  }
});

test('a different build blocks the start, and leaving frees fielders', async () => {
  const l = await lobby(['Riley', 'Sam']);
  const odd = new LobbyClient({ address: '127.0.0.1', port: l.host.port, name: 'Odd', build: 'b2' });
  try {
    await odd.connect();
    let s = await waitFor(l.host, (st) => st.players.length === 3);
    assert.match(s.problem, /Odd has a different game build/);
    const oddSlot = s.slots.indexOf(odd.selfId);
    assert.ok(oddSlot >= 0);
    odd.close();
    s = await waitFor(l.host, (st) => st.players.length === 2);
    assert.equal(s.slots[oddSlot], null);
    assert.equal(s.problem, '');
  } finally {
    odd.close();
    closeAll(l);
  }
});

test('a lobby in a match turns newcomers away, and reopens when the host is back', async () => {
  const l = await lobby(['Riley', 'Sam']);
  try {
    l.host.act({ t: 'start' });
    await waitFor(l.clients[0], (st) => st.phase === 'playing');
    const late = new LobbyClient({ address: '127.0.0.1', port: l.host.port, name: 'Late', build: 'b1' });
    await assert.rejects(late.connect(), /match is being played/);
    l.clients[0].act({ t: 'ingame', running: false });
    l.host.act({ t: 'ingame', running: false });
    const s = await waitFor(l.clients[0], (st) => st.phase === 'lobby' && !st.players.some((p) => p.inGame));
    assert.equal(s.problem, '');
  } finally {
    closeAll(l);
  }
});

test('names are cleaned: no commas or control characters, 16 characters', async () => {
  const l = await lobby(['Riley', 'Sam']);
  try {
    l.clients[0].act({ t: 'name', name: '  A,b\u0007c  very long name indeed ' });
    const s = await waitFor(l.host, (st) => st.players[1].name !== 'Sam');
    assert.equal(s.players[1].name, 'Abc  very long n');
  } finally {
    closeAll(l);
  }
});

test('nothing is listening', async () => {
  const c = new LobbyClient({ address: '127.0.0.1', port: 1, name: 'x', build: 'b' });
  await assert.rejects(c.connect(2000), /Could not reach a lobby/);
});

test('addresses', () => {
  assert.deepEqual(parseAddress('192.168.1.20'), { address: '192.168.1.20', port: 52321 });
  assert.deepEqual(parseAddress(' pc.local:4000 '), { address: 'pc.local', port: 4000 });
  assert.equal(parseAddress('a b'), null);
  assert.equal(parseAddress('host:70000'), null);
  assert.equal(parseAddress(''), null);
});

test('game command lines', () => {
  const s = { ...defaultSettings(), iso: 'C:/g.iso', localNames: ['Riley', '', 'Al,ex', ''] };
  assert.deepEqual(offlineArgs(s), ['C:/g.iso', '--resolution', '3840x2160', '--fullscreen', '--display', '0',
    '--aspect', '4:3', '--vsync', '--player-names', 'Riley,,Alex,']);
  assert.ok(!offlineArgs({ ...s, localNames: ['', ''] }).includes('--player-names'));
  const plan = {
    players: 3, slots: '0,2,-,-,1,-,-,-',
    match: { home: { team: 4, sidekick: 3 }, away: { team: 1, sidekick: 0 }, stadium: 6, minutes: 5, skill: 3,
      items: true, bowser: false, superStrikes: true, delay: 0 },
  };
  const host = onlineArgs(s, { seat: 0, plan }, null, 'Riley');
  assert.deepEqual(host.slice(9), ['--net-name', 'Riley', '--net-host', '52320', '--net-players', '3',
    '--net-slots', '0,2,-,-,1,-,-,-', '--home', 'peach', '--home-sidekick', 'birdo', '--away', 'dk',
    '--away-sidekick', 'toad', '--stadium', 'forbidden', '--match-minutes', '5', '--skill', '3',
    '--items', 'on', '--bowser', 'off', '--super-strikes', 'on']);
  const guest = onlineArgs(s, { seat: 2, plan: { ...plan, match: { ...plan.match, delay: 6 } } }, '10.0.0.5', 'Sam');
  assert.deepEqual(guest.slice(9), ['--net-name', 'Sam', '--net-join', '10.0.0.5:52320', '--net-seat', '2']);
  const delayed = onlineArgs(s, { seat: 0, plan: { ...plan, match: { ...plan.match, delay: 6 } } }, null, 'R');
  assert.deepEqual(delayed.slice(-2), ['--net-delay', '6']);
});

test('a cup: everyone at home, the cup\'s fixture, results recorded', async () => {
  const l = await lobby(['Riley', 'Sam']);
  const saved = [];
  l.host.on('cup', (c) => saved.push(c && JSON.parse(JSON.stringify(c))));
  try {
    const [sam] = l.clients;
    assert.equal(sam.state.slots[4], sam.selfId);
    l.host.act({ t: 'mode', mode: 'cup' });
    let s = await waitFor(sam, (st) => st.mode === 'cup');
    assert.equal(s.slots[1], sam.selfId); // moved from the away captaincy
    assert.ok(s.slots.slice(4).every((x) => x === null));
    assert.match(s.problem, /Choose a cup/);
    sam.act({ t: 'slot', slot: 5 }); // refused: away is the computer's
    l.host.act({ t: 'team', side: 'home', team: 4, sidekick: 3 });
    l.host.act({ t: 'cupKind', kind: 0 });
    l.host.act({ t: 'startCup' });
    s = await waitFor(sam, (st) => !!st.cup);
    assert.equal(s.slots[5], null);
    assert.equal(s.problem, '');
    assert.equal(s.cup.entrants.find((e) => e.human).team, 4);
    l.host.act({ t: 'team', side: 'home', team: 1 }); // locked for the cup
    const hostStart = once(l.host, 'start');
    l.host.act({ t: 'start' });
    const [{ plan }] = await hostStart;
    assert.equal(plan.slots, '0,1,-,-,-,-,-,-');
    assert.equal(plan.match.home.team, 4);
    assert.equal(plan.match.home.sidekick, 3);
    assert.notEqual(plan.match.away.team, 4);
    assert.equal(plan.match.endAfterMatch, true);
    assert.ok(onlineArgs(defaultSettings(), { seat: 0, plan }, null, 'R').includes('--end-after-match'));
    // The host's game reports, then exits.
    l.host.act({ t: 'result', home: 3, away: 1, ot: false });
    l.host.act({ t: 'ingame', running: false });
    sam.act({ t: 'ingame', running: false });
    s = await waitFor(sam, (st) => st.cup.round === 1 && st.phase === 'lobby');
    const me = s.cup.entrants.findIndex((e) => e.human);
    const played = s.cup.rounds[0].find((m) => m.home === me || m.away === me);
    assert.deepEqual(played.home === me ? [played.result.home, played.result.away] : [played.result.away, played.result.home], [3, 1]);
    assert.ok(s.cup.rounds[0].every((m) => m.result));
    // A result with no cup match being played is ignored.
    l.host.act({ t: 'result', home: 5, away: 0 });
    assert.equal(l.host.state.cup.round, 1);
    assert.equal(saved.at(-1).round, 1);

    // A new lobby picks the saved cup up again.
    const again = new LobbyHost({ name: 'Riley', build: 'b1', port: 0, beacon: false, savedCup: saved.at(-1) });
    assert.equal(again.state.savedCup.progress, 'Round 2 of 3');
    again.act({ t: 'resumeCup' });
    assert.equal(again.state.mode, 'cup');
    assert.equal(again.state.cup.round, 1);
    assert.equal(again.state.match.home.team, 4);
    again.close();
  } finally {
    closeAll(l);
  }
});
