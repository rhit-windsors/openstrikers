'use strict';
/* global launcher */

// Display names, in the game's eTeamID / eSidekickID / eStadiumID order (the
// same order as src/lobby.js).
const TEAMS = [
  ['Daisy', '#f39c32'], ['Donkey Kong', '#a0652f'], ['Luigi', '#2fbf5b'], ['Mario', '#e8423a'],
  ['Peach', '#ff8fc8'], ['Waluigi', '#8e4fc9'], ['Wario', '#f1c40f'], ['Yoshi', '#7ed957'],
  ['Super Team', '#8fa3b8'],
];
// The robots: every player can take a super strike, and their sidekicks are fixed.
const SUPER_TEAM = 8;
const SIDEKICKS = ['Toad', 'Koopa', 'Hammer Bros', 'Birdo'];
const STADIUMS = ['Mario Stadium', "Peach's Castle", 'DK Jungle', "Wario's Factory", "Yoshi's Island", 'Super Stadium', 'The Battle Dome'];
const CUPS = ['Mushroom Cup', 'Flower Cup', 'Star Cup', 'Bowser Cup', 'Super Mushroom Cup', 'Super Flower Cup', 'Super Star Cup', 'Super Bowser Cup'];
const SKILLS = ['Training', 'Rookie', 'Professional', 'Superstar', 'Legend'];
const ROLES = ['Captain', 'Sidekick', 'Sidekick', 'Sidekick'];
const MINUTES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30];

const $ = (id) => document.getElementById(id);

// Builds elements without ever parsing player-supplied text as HTML.
function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'style') el.style.cssText = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k in el && typeof v !== 'string') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) el.append(c);
  return el;
}

function options(select, items, value) {
  select.replaceChildren(...items.map(([v, label]) => h('option', { value: String(v) }, label)));
  select.value = String(value);
}

let settings = null;
let lobby = null; // { state, selfId, host }
let gameRunning = false;

// --- toasts --------------------------------------------------------------------------

let toastTimer = null;
function toast(text, { info = false, action = null, ms = 7000 } = {}) {
  const t = $('toast');
  t.className = info ? 'toast info' : 'toast';
  t.replaceChildren(text);
  if (action) {
    t.append(' ', h('button', { class: 'btn btn-ghost', onclick: action.run }, action.label));
  }
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, ms);
}

// --- navigation ------------------------------------------------------------------------

function show(view) {
  for (const b of document.querySelectorAll('.nav')) b.classList.toggle('active', b.dataset.view === view);
  for (const v of document.querySelectorAll('.view')) v.classList.toggle('active', v.id === `view-${view}`);
  launcher.watchLan(view === 'online' && !lobby);
}
for (const b of document.querySelectorAll('.nav')) b.addEventListener('click', () => show(b.dataset.view));

// --- settings ------------------------------------------------------------------------------

async function save(patch) {
  settings = await launcher.setSettings(patch);
  renderReadiness();
}

function renderReadiness() {
  $('build').textContent = settings.build ? `build ${settings.build}` : '';
  $('iso').value = settings.iso || '';
  $('exe').value = settings.exe || '';
  const disc = settings.ready.disc;
  $('disc-status').textContent = disc.ok ? `Super Mario Strikers (${disc.code})` : disc.reason;
  $('disc-status').style.color = disc.ok ? 'var(--ok)' : 'var(--bad)';
  $('play-problem').textContent = settings.ready.problem;
  $('online-problem').textContent = settings.ready.problem;
  updateButtons();
}

function updateButtons() {
  const blocked = !!settings.ready.problem || gameRunning;
  $('play-offline').disabled = blocked;
  $('host').disabled = blocked;
  $('join').disabled = blocked;
}

async function loadSettings() {
  settings = await launcher.settings();
  $('name').value = settings.name;
  $('address').value = settings.lastAddress || '';
  $('resolution').value = settings.resolution;
  $('mode').value = settings.fullscreen ? '1' : '0';
  $('aspect').value = settings.aspect;
  $('vsync').checked = settings.vsync;
  $('msaa').value = String(settings.msaa ?? 1);
  const displays = await launcher.displays();
  options($('display'), displays.map((d) => [d.index, `${d.index + 1}: ${d.label}${d.primary ? ' - main' : ''}`]),
    Math.min(settings.display, Math.max(0, displays.length - 1)));

  $('local-names').replaceChildren(...[0, 1, 2, 3].map((i) => {
    const input = h('input', { maxlength: '16', spellcheck: 'false', autocomplete: 'off', value: settings.localNames?.[i] ?? '' });
    input.placeholder = `Player ${i + 1}`;
    input.addEventListener('change', () => {
      const names = [0, 1, 2, 3].map((k) => settings.localNames?.[k] ?? '');
      names[i] = input.value.replace(/,/g, '').trim();
      save({ localNames: names });
    });
    return h('div', { class: 'port' }, h('label', {}, h('span', { class: `dot c${i + 1}` }), `Controller ${i + 1}`), input);
  }));
  renderReadiness();
}

$('resolution').addEventListener('change', (e) => save({ resolution: e.target.value }));
$('mode').addEventListener('change', (e) => save({ fullscreen: e.target.value === '1' }));
$('display').addEventListener('change', (e) => save({ display: Number(e.target.value) }));
$('aspect').addEventListener('change', (e) => save({ aspect: e.target.value }));
$('vsync').addEventListener('change', (e) => save({ vsync: e.target.checked }));
$('msaa').addEventListener('change', (e) => save({ msaa: Number(e.target.value) }));
$('name').addEventListener('change', (e) => {
  const name = e.target.value.replace(/,/g, '').trim();
  if (name) save({ name });
  else e.target.value = settings.name;
});
for (const b of document.querySelectorAll('[data-browse]')) {
  b.addEventListener('click', async () => {
    settings = await launcher.browse(b.dataset.browse);
    renderReadiness();
  });
}
$('open-logs').addEventListener('click', () => launcher.openLogs());

// --- play here ----------------------------------------------------------------------------

$('play-offline').addEventListener('click', async () => {
  const r = await launcher.playOffline();
  if (!r.ok) toast(r.error);
});

launcher.onGame((g) => {
  gameRunning = g.running;
  $('game-status').textContent = g.running ? 'Game running' : '';
  $('game-status').classList.toggle('live', g.running);
  updateButtons();
  if (!g.running && g.message) {
    toast(g.detail ? `${g.message}\n${g.detail}` : g.message, {
      ms: 15000,
      action: g.logPath ? { label: 'Show log', run: () => launcher.showLog(g.logPath) } : null,
    });
  }
  if (lobby) renderLobby();
});

// --- online: finding a lobby ---------------------------------------------------------------------

$('host').addEventListener('click', async () => {
  $('host').disabled = true;
  const r = await launcher.host();
  updateButtons();
  if (!r.ok) toast(r.error);
});

async function join(address) {
  $('join').disabled = true;
  $('online-problem').textContent = '';
  const r = await launcher.join(address);
  updateButtons();
  if (!r.ok) $('online-problem').textContent = r.error;
}
$('join').addEventListener('click', () => join($('address').value));
$('address').addEventListener('keydown', (e) => { if (e.key === 'Enter') join($('address').value); });

launcher.onLan((list) => {
  const ul = $('lan');
  if (!list.length) {
    ul.replaceChildren(h('li', { class: 'empty' }, 'No lobbies found on your network yet. Joining by address always works.'));
    return;
  }
  ul.replaceChildren(...list.map((l) => h('li', {},
    h('div', {}, h('div', { class: 'who' }, `${l.host}'s lobby`),
      h('div', { class: 'meta' }, `${l.address} - ${l.players} player${l.players === 1 ? '' : 's'}${l.phase === 'lobby' ? '' : ' - match in progress'}`)),
    h('button', {
      class: 'btn btn-primary', disabled: l.phase !== 'lobby' || gameRunning,
      onclick: () => join(l.port === 52321 ? l.address : `${l.address}:${l.port}`),
    }, 'Join'))));
});

// --- online: the lobby --------------------------------------------------------------------------

launcher.onLobby(async (l) => {
  const wasIn = !!lobby;
  lobby = l;
  $('online-entry').hidden = !!l;
  $('lobby').hidden = !l;
  if (!l) {
    $('chat-log').replaceChildren();
    show(document.querySelector('.nav.active')?.dataset.view ?? 'online');
    return;
  }
  if (!wasIn) {
    launcher.watchLan(false);
    show('online');
    if (l.host) {
      const addrs = await launcher.addresses();
      $('lobby-addresses').textContent = addrs.length
        ? `Friends join with ${addrs.map((a) => (a.kind === 'VPN' ? `${a.address} (VPN)` : a.address)).join(' or ')}`
        : 'This computer has no network address.';
    } else {
      $('lobby-addresses').textContent = '';
    }
  }
  renderLobby();
});

function playerById(id) {
  return lobby.state.players.find((p) => p.id === id);
}

function canPickTeam(side) {
  const captain = lobby.state.slots[side === 'home' ? 0 : 4];
  if (lobby.state.mode === 'cup' && (side === 'away' || lobby.state.cup)) return false;
  return lobby.state.phase === 'lobby' &&
    (captain === lobby.selfId || (lobby.host && (captain === null || captain === lobby.selfId)));
}

// In a cup the other half is the computer's: the next opponent, drawn by the cup.
function renderOpponent() {
  const next = lobby.state.cupView?.next;
  const head = h('div', { class: 'team-head', style: `--team: ${next ? TEAMS[next.team][1] : '#888'}` },
    h('div', { class: 'side-name' }, 'Opponent', h('b', {}, next ? next.name : lobby.state.cup ? '-' : 'Drawn by the cup')),
    next ? h('div', { class: 'hint' }, `with ${SIDEKICKS[next.sidekick]}`) : null);
  const slots = [0, 1, 2, 3].map((k) => h('div', { class: 'slot cpu locked away-cpu' },
    h('span', { class: `dot c${k + 1}` }),
    h('span', { class: 'role' }, k === 0 ? ROLES[0] : `${ROLES[k]} ${k}`),
    h('span', { class: 'who' }, 'Computer')));
  $('half-away').replaceChildren(head, ...slots);
}

function renderHalf(side) {
  const { state, selfId, host } = lobby;
  if (side === 'away' && state.mode === 'cup') return renderOpponent();
  const base = side === 'home' ? 0 : 4;
  const m = state.match[side];
  const editable = canPickTeam(side);
  const team = h('select', {
    disabled: !editable,
    title: editable ? '' : state.mode === 'cup' && state.cup ? 'A cup keeps its team' : 'The captain picks the team',
  });
  options(team, TEAMS.map(([n], i) => [i, n]), m.team);
  team.addEventListener('change', () => launcher.act({ t: 'team', side, team: Number(team.value) }));
  const robots = m.team === SUPER_TEAM;
  const sidekick = h('select', { disabled: !editable || robots, title: robots ? 'The Super Team is all robots' : '' });
  if (robots) options(sidekick, [[0, 'Robots']], 0);
  else options(sidekick, SIDEKICKS.map((n, i) => [i, n]), m.sidekick);
  sidekick.addEventListener('change', () => launcher.act({ t: 'team', side, sidekick: Number(sidekick.value) }));

  const slots = [0, 1, 2, 3].map((k) => {
    const slot = base + k;
    const owner = state.slots[slot];
    const p = owner === null ? null : playerById(owner);
    const mine = owner === selfId;
    const open = state.phase === 'lobby';
    let action = '';
    if (open && owner === null) action = 'Play here';
    else if (open && mine) action = 'Leave spot';
    else if (open && host) action = 'Free';
    return h('button', {
      class: `slot${owner === null ? ' cpu' : ''}${mine ? ' mine' : ''}${k === 0 ? ' captain' : ''}${action ? '' : ' locked'}`,
      onclick: () => {
        if (!action) return;
        if (owner !== null && !mine) {
          if (!confirm(`Free ${p?.name ?? 'this player'}'s spot? They become a computer player until someone takes it.`)) return;
          launcher.act({ t: 'slot', slot, release: true });
        } else launcher.act({ t: 'slot', slot });
      },
    },
    h('span', { class: `dot c${k + 1}` }),
    h('span', { class: 'role' }, robots ? `Robot ${k + 1}` : k === 0 ? ROLES[0] : `${ROLES[k]} ${k}`),
    h('span', { class: 'who' }, p ? p.name : 'Computer'),
    h('span', { class: 'action' }, action));
  });

  const head = h('div', { class: 'team-head', style: `--team: ${TEAMS[m.team][1]}` },
    h('div', { class: 'side-name' }, state.mode === 'cup' ? 'Your team' : side === 'home' ? 'Home' : 'Away', h('b', {}, TEAMS[m.team][0])),
    team, sidekick);
  $(`half-${side}`).replaceChildren(head, ...slots);
}

function renderMatch() {
  const { state, host } = lobby;
  const m = state.match;
  const editable = host && state.phase === 'lobby';
  const set = (patch) => launcher.act({ t: 'match', match: patch });
  const select = (label, items, value, key) => {
    const s = h('select', { disabled: !editable });
    options(s, items, value);
    s.addEventListener('change', () => set({ [key]: Number(s.value) }));
    return h('div', { class: 'field' }, h('label', {}, label), s);
  };
  const check = (label, key) => {
    const c = h('input', { type: 'checkbox', checked: m[key], disabled: !editable });
    c.addEventListener('change', () => set({ [key]: c.checked }));
    return h('label', { class: 'check' }, c, label);
  };
  $('match').replaceChildren(
    h('h2', {}, host ? 'Match' : 'Match (the host decides)'),
    state.mode === 'cup' ? null : select('Stadium', STADIUMS.map((n, i) => [i, n]), m.stadium, 'stadium'),
    select('Length', MINUTES.map((n) => [n, `${n} minute${n === 1 ? '' : 's'}`]), m.minutes, 'minutes'),
    select('Computer skill', SKILLS.map((n, i) => [i, n]), m.skill, 'skill'),
    select('Input delay', [[0, 'Default (4 frames)'], ...[2, 3, 4, 5, 6, 7, 8, 10, 12].map((n) => [n, `${n} frames (${Math.round(n * 16.7)} ms)`])], m.delay, 'delay'),
    check('Items', 'items'),
    check('Bowser attacks', 'bowser'),
    check('Super strikes', 'superStrikes'),
  );
}

function renderPlayers() {
  const { state, selfId } = lobby;
  const hostBuild = state.players[0]?.build;
  $('player-count').textContent = `${state.players.length} / 8`;
  $('players').replaceChildren(...state.players.map((p) => h('li', {},
    h('span', { class: 'pname' }, p.name, p.id === selfId ? ' (you)' : ''),
    p.host ? h('span', { class: 'tag host' }, 'Host') : null,
    p.build !== hostBuild ? h('span', { class: 'tag bad', title: `build ${p.buildLabel}` }, 'Different build') : null,
    p.inGame ? h('span', { class: 'tag live' }, 'In match') : null,
    !p.host && p.ping !== null ? h('span', { class: 'ping' }, `${p.ping} ms`) : null)));
}

function renderLobby() {
  const { state, host } = lobby;
  const hostName = state.players[0]?.name ?? 'Host';
  $('lobby-title').textContent = host ? 'Your lobby' : `${hostName}'s lobby`;
  renderHalf('home');
  renderHalf('away');
  renderMatch();
  renderPlayers();
  const start = $('start');
  start.hidden = !host || (state.mode === 'cup' && (!state.cup || state.cup.done));
  start.textContent = state.mode === 'cup' ? 'Play next match' : 'Start match';
  let text = state.problem;
  if (state.phase !== 'lobby') text = 'Match in progress.';
  else if (!host && !text) text = `Waiting for ${hostName} to start the ${state.mode === 'cup' ? 'next cup match' : 'match'}.`;
  renderModeSwitch();
  renderCup();
  $('start-problem').textContent = text;
  start.disabled = state.phase !== 'lobby' || !!state.problem || gameRunning;
}

function renderModeSwitch() {
  const { state, host } = lobby;
  const locked = !host || state.phase !== 'lobby';
  const button = (mode, label) => h('button', {
    class: state.mode === mode ? 'on' : '',
    disabled: locked,
    title: host ? '' : 'The host chooses',
    onclick: () => { if (state.mode !== mode) launcher.act({ t: 'mode', mode }); },
  }, label);
  $('mode-switch').replaceChildren(button('match', 'Exhibition'), button('cup', 'Co-op cup'));
}

function ordinal(n) {
  const tens = n % 100;
  if (tens >= 11 && tens <= 13) return `${n}th`;
  return `${n}${['th', 'st', 'nd', 'rd'][n % 10] ?? 'th'}`;
}

function renderCup() {
  const { state, host } = lobby;
  const card = $('cup');
  card.hidden = state.mode !== 'cup';
  if (card.hidden) return;
  const open = state.phase === 'lobby';
  if (!state.cup) {
    const kind = h('select', { disabled: !host || !open });
    options(kind, CUPS.map((n, i) => [i, n]), state.cupKind);
    kind.addEventListener('change', () => launcher.act({ t: 'cupKind', kind: Number(kind.value) }));
    card.replaceChildren(
      h('div', { class: 'cup-head' }, h('h2', {}, 'Co-op cup')),
      h('p', { class: 'hint' }, "Everyone plays for one team, through a cup against the computer: a league like the game's own cups, and in the Bowser cups a knockout between the top four. Pick your team above."),
      h('div', { class: 'cup-setup' },
        kind,
        host ? h('button', { class: 'btn btn-primary', disabled: !open, onclick: () => launcher.act({ t: 'startCup' }) }, 'Start cup') : null,
        host && state.savedCup ? h('button', { class: 'btn', disabled: !open, onclick: () => launcher.act({ t: 'resumeCup' }) },
          `Resume ${CUPS[state.savedCup.kind]} (${state.savedCup.progress.toLowerCase()})`) : null,
        host ? null : h('span', { class: 'hint' }, 'The host chooses the cup.')));
    return;
  }
  const v = state.cupView;
  const parts = [h('div', { class: 'cup-head' }, h('h2', {}, v.name), h('span', { class: 'progress' }, v.progress))];
  if (v.done) {
    const us = v.table.find((row) => row.human);
    let text;
    if (v.champion.human) text = `Champions! You won the ${v.name}.`;
    else if (v.knockout) text = `${v.champion.name} won the ${v.name}.${v.eliminated ? ' You were knocked out.' : ''}`;
    else text = `${v.champion.name} won the ${v.name}. You finished ${ordinal(us.position)}.`;
    parts.push(h('div', { class: `cup-banner${v.champion.human ? '' : ' out'}` }, text));
  } else if (v.next) {
    const stage = v.next.stage === 'final' ? 'Final' : v.next.stage === 'semi' ? 'Semi-final' : 'Next match';
    parts.push(h('div', { class: 'cup-next' },
      h('div', {}, h('div', { class: 'label' }, stage),
        h('div', { class: 'vs' }, `${TEAMS[state.match.home.team][0]} vs ${v.next.name}`),
        h('div', { class: 'where' }, `${STADIUMS[v.next.stadium]} - their sidekicks: ${SIDEKICKS[v.next.sidekick]}`))));
  }
  // In a Bowser cup's league, the top four go through to the knockout.
  const through = v.knockout && !state.cup.knockout ? 4 : 0;
  const table = h('table', { class: 'table' },
    h('thead', {}, h('tr', {}, ...['#', 'Team', 'P', 'W', 'OTW', 'OTL', 'L', 'GF', 'GA', 'Pts'].map((t) => h('th', {}, t)))),
    h('tbody', {}, ...v.table.map((row) => h('tr', { class: `${row.human ? 'us' : ''}${row.position <= through ? ' through' : ''}` },
      h('td', {}, String(row.position)), h('td', {}, row.human ? `${row.name} (you)` : row.name),
      ...[row.played, row.won, row.otWon, row.otLost, row.lost, row.goalsFor, row.goalsAgainst].map((n) => h('td', {}, String(n))),
      h('td', { class: 'pts' }, String(row.points))))));
  const latest = h('div', {},
    h('h3', {}, v.latest.length ? v.latest[0].stage : 'Results'),
    v.latest.length
      ? h('ul', { class: 'results' }, ...v.latest.map((m) => h('li', {},
        h('span', { class: `h${m.homeHuman ? ' us' : ''}` }, m.home),
        h('span', { class: 'score' }, `${m.result.home}-${m.result.away}${m.result.ot ? ' OT' : ''}`),
        h('span', { class: m.awayHuman ? 'us' : '' }, m.away))))
      : h('p', { class: 'hint' }, 'No matches played yet.'));
  parts.push(h('div', { class: 'cup-body' }, table, latest));
  if (host) {
    parts.push(h('div', { class: 'cup-foot' },
      v.done ? h('button', { class: 'btn btn-primary', disabled: !open, onclick: () => launcher.act({ t: 'startCup' }) }, 'Play another cup') : null,
      h('button', {
        class: 'btn btn-ghost', disabled: !open,
        onclick: () => { if (v.done || confirm(`Abandon the ${v.name}?`)) launcher.act({ t: 'endCup' }); },
      }, v.done ? 'Choose another cup' : 'Abandon cup')));
  }
  card.replaceChildren(...parts);
}

$('start').addEventListener('click', () => launcher.act({ t: 'start' }));
$('leave').addEventListener('click', () => launcher.leave());

launcher.onChat((msg) => {
  const log = $('chat-log');
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.append(msg.from === null
    ? h('li', { class: 'sys' }, msg.text)
    : h('li', {}, h('span', { class: 'from' }, msg.from), msg.text));
  while (log.children.length > 200) log.firstChild.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
});
$('chat-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const input = $('chat-input');
  if (input.value.trim()) launcher.act({ t: 'chat', text: input.value });
  input.value = '';
});

launcher.onNotice((text) => toast(text, { info: true }));

loadSettings().then(async () => {
  gameRunning = await launcher.gameRunning();
  updateButtons();
});
