'use strict';
/* global launcher */

// Display names, in the game's eTeamID / eSidekickID / eStadiumID order (the
// same order as src/lobby.js).
const TEAMS = [
  ['Daisy', '#f39c32'], ['Donkey Kong', '#a0652f'], ['Luigi', '#2fbf5b'], ['Mario', '#e8423a'],
  ['Peach', '#ff8fc8'], ['Waluigi', '#8e4fc9'], ['Wario', '#f1c40f'], ['Yoshi', '#7ed957'],
];
const SIDEKICKS = ['Toad', 'Koopa', 'Hammer Bros', 'Birdo'];
const STADIUMS = ['Mario Stadium', "Peach's Castle", 'DK Jungle', "Wario's Factory", "Yoshi's Island", 'Super Stadium', 'The Battle Dome'];
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
  return lobby.state.phase === 'lobby' &&
    (captain === lobby.selfId || (lobby.host && (captain === null || captain === lobby.selfId)));
}

function renderHalf(side) {
  const { state, selfId, host } = lobby;
  const base = side === 'home' ? 0 : 4;
  const m = state.match[side];
  const editable = canPickTeam(side);
  const team = h('select', { disabled: !editable, title: editable ? '' : 'The captain picks the team' });
  options(team, TEAMS.map(([n], i) => [i, n]), m.team);
  team.addEventListener('change', () => launcher.act({ t: 'team', side, team: Number(team.value) }));
  const sidekick = h('select', { disabled: !editable });
  options(sidekick, SIDEKICKS.map((n, i) => [i, n]), m.sidekick);
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
    h('span', { class: 'role' }, k === 0 ? ROLES[0] : `${ROLES[k]} ${k}`),
    h('span', { class: 'who' }, p ? p.name : 'Computer'),
    h('span', { class: 'action' }, action));
  });

  const head = h('div', { class: 'team-head', style: `--team: ${TEAMS[m.team][1]}` },
    h('div', { class: 'side-name' }, side === 'home' ? 'Home' : 'Away', h('b', {}, TEAMS[m.team][0])),
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
    select('Stadium', STADIUMS.map((n, i) => [i, n]), m.stadium, 'stadium'),
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
  start.hidden = !host;
  let text = state.problem;
  if (state.phase !== 'lobby') text = 'Match in progress.';
  else if (!host && !text) text = `Waiting for ${hostName} to start the match.`;
  $('start-problem').textContent = text;
  start.disabled = state.phase !== 'lobby' || !!state.problem || gameRunning;
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
