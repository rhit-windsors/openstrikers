'use strict';
// The online lobby: everyone gathers here, picks a name and the fielders they
// will play, and the host starts the match. It only arranges the match. Once
// it starts, each launcher runs the game, and the games connect to each other
// on their own (UDP, src/net in the game). The lobby connection stays open,
// so everyone comes back to the same lobby after the match.
//
// The host keeps the state and is the only one who changes it. Players send
// requests and the host sends the whole state back after every change. Messages
// are JSON, one per line, over TCP. Nothing here needs Electron, so
// test/lobby.test.js can drive host and players in one process.

const net = require('node:net');
const dgram = require('node:dgram');
const os = require('node:os');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');

const PROTOCOL = 1;
const LOBBY_PORT = 52321; // TCP: this lobby
const BEACON_PORT = 52322; // UDP: "a lobby is open here" on the local network
const GAME_PORT = 52320; // UDP: the game's own netplay port
const MAX_PLAYERS = 8; // the game's limit: one machine per fielder at most
const SLOTS = 8; // home captain, 3 home sidekicks, away captain, 3 away sidekicks
const MAX_LOCAL_SLOTS = 4; // a machine drives at most one fielder per controller port
const MAX_NAME = 16;
const MAX_LINE = 64 * 1024;

const TEAMS = ['daisy', 'dk', 'luigi', 'mario', 'peach', 'waluigi', 'wario', 'yoshi'];
const SIDEKICKS = ['toad', 'koopa', 'hammerbros', 'birdo'];
const STADIUMS = ['mario', 'peach', 'dk', 'wario', 'yoshi', 'super', 'forbidden'];

function defaultMatch() {
  return {
    home: { team: 3, sidekick: 0 },
    away: { team: 2, sidekick: 1 },
    stadium: 0,
    minutes: 3,
    skill: 2,
    items: true,
    bowser: true,
    superStrikes: true,
    delay: 0, // 0: the game's default (4 frames)
  };
}

function cleanName(name) {
  const s = String(name ?? '').replace(/[\u0000-\u001f\u007f,]/g, '').trim();
  return s.slice(0, MAX_NAME);
}

function intIn(v, lo, hi) {
  return Number.isInteger(v) && v >= lo && v <= hi;
}

// Newline-delimited JSON on a socket.
function frame(socket, onMessage, onClose) {
  let buffer = '';
  socket.setEncoding('utf8');
  socket.setNoDelay(true);
  socket.on('data', (chunk) => {
    buffer += chunk;
    if (buffer.length > MAX_LINE && buffer.indexOf('\n') < 0) {
      socket.destroy();
      return;
    }
    let nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, nl);
      buffer = buffer.slice(nl + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        socket.destroy();
        return;
      }
      if (msg && typeof msg === 'object') onMessage(msg);
    }
  });
  let closed = false;
  const close = () => {
    if (!closed) {
      closed = true;
      onClose();
    }
  };
  socket.on('close', close);
  socket.on('error', close);
  return (msg) => {
    if (!socket.destroyed) socket.write(JSON.stringify(msg) + '\n');
  };
}

// The order the game seats machines in: the host first, then everyone else in
// the order they joined. `--net-slots` and `--net-seat` are built from it.
function planMatch(state) {
  const seatOf = new Map(state.players.map((p, i) => [p.id, i]));
  const slots = state.slots.map((id) => (id === null ? '-' : String(seatOf.get(id))));
  return {
    players: state.players.length,
    slots: slots.join(','),
    seats: Object.fromEntries(state.players.map((p, i) => [p.id, i])),
    match: state.match,
  };
}

// Why the host cannot start yet, or '' when it can.
function startProblem(state) {
  if (state.players.length < 2) return 'Waiting for at least one more player.';
  const host = state.players[0];
  const odd = state.players.filter((p) => p.build !== host.build);
  if (odd.length) {
    return `${odd.map((p) => p.name).join(', ')} ${odd.length > 1 ? 'have' : 'has'} a different game build or disc. ` +
      'Everyone needs the same openstrikers build and ISO.';
  }
  const idle = state.players.filter((p) => !state.slots.includes(p.id));
  if (idle.length) return `${idle.map((p) => p.name).join(', ')} still ${idle.length > 1 ? 'need' : 'needs'} to pick a fielder.`;
  const busy = state.players.filter((p) => p.inGame);
  if (busy.length) return `${busy.map((p) => p.name).join(', ')} ${busy.length > 1 ? 'are' : 'is'} still in the last match.`;
  return '';
}

class LobbyHost extends EventEmitter {
  constructor({ name, build, buildLabel = '', port = LOBBY_PORT, beacon = true }) {
    super();
    this.port = port;
    this.beaconEnabled = beacon;
    this.selfId = 0;
    this.nextId = 1;
    this.clients = new Map(); // id -> { send, socket }
    this.state = {
      phase: 'lobby',
      players: [{ id: 0, name: cleanName(name) || 'Host', host: true, build, buildLabel, ping: 0, inGame: false }],
      slots: Array(SLOTS).fill(null),
      match: defaultMatch(),
      problem: '',
    };
    this.state.slots[0] = 0;
    this.refresh();
  }

  listen() {
    return new Promise((resolve, reject) => {
      this.server = net.createServer((socket) => this.accept(socket));
      this.server.once('error', reject);
      this.server.listen(this.port, () => {
        this.server.off('error', reject);
        this.port = this.server.address().port;
        if (this.beaconEnabled) this.startBeacon();
        this.pingTimer = setInterval(() => this.ping(), 2000);
        resolve(this.port);
      });
    });
  }

  close() {
    clearInterval(this.pingTimer);
    clearInterval(this.beaconTimer);
    if (this.beaconSocket) this.beaconSocket.close();
    this.beaconSocket = null;
    for (const c of this.clients.values()) c.socket.destroy();
    this.clients.clear();
    if (this.server) this.server.close();
    this.server = null;
  }

  startBeacon() {
    this.beaconId = crypto.randomBytes(6).toString('hex');
    const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    socket.on('error', () => {}); // no broadcast on this network: joining by address still works
    socket.bind(() => {
      try {
        socket.setBroadcast(true);
      } catch {
        return;
      }
      const send = () => {
        const msg = Buffer.from(JSON.stringify({
          t: 'openstrikers-lobby', v: PROTOCOL, id: this.beaconId, host: this.state.players[0].name, port: this.port,
          players: this.state.players.length, phase: this.state.phase,
        }));
        for (const target of broadcastAddresses()) socket.send(msg, BEACON_PORT, target, () => {});
      };
      send();
      this.beaconTimer = setInterval(send, 1000);
    });
    this.beaconSocket = socket;
  }

  accept(socket) {
    let id = null;
    const send = frame(socket, (msg) => {
      if (id === null) {
        if (msg.t !== 'hello') return socket.destroy();
        if (msg.v !== PROTOCOL) {
          send({ t: 'reject', reason: 'This lobby runs a different launcher version.' });
          return socket.end();
        }
        if (this.state.players.length >= MAX_PLAYERS) {
          send({ t: 'reject', reason: 'The lobby is full (8 players).' });
          return socket.end();
        }
        if (this.state.phase !== 'lobby') {
          send({ t: 'reject', reason: 'A match is being played. Try again when it ends.' });
          return socket.end();
        }
        id = this.nextId++;
        this.clients.set(id, { send, socket });
        this.state.players.push({
          id, name: cleanName(msg.name) || `Player ${id + 1}`, host: false,
          build: String(msg.build ?? ''), buildLabel: String(msg.buildLabel ?? ''), ping: null, inGame: false,
        });
        // Give a newcomer the first free fielder, so a two-player lobby is
        // ready to start as soon as someone joins.
        const free = this.firstFreeSlot();
        if (free >= 0) this.state.slots[free] = id;
        send({ t: 'welcome', id });
        this.system(`${this.player(id).name} joined.`);
        this.changed();
        return;
      }
      this.handle(id, msg);
    }, () => {
      if (id === null) return;
      this.clients.delete(id);
      const p = this.player(id);
      this.state.players = this.state.players.filter((q) => q.id !== id);
      this.state.slots = this.state.slots.map((s) => (s === id ? null : s));
      if (p) this.system(`${p.name} left.`);
      this.changed();
    });
  }

  // The away captain first, so two players face each other by default.
  firstFreeSlot() {
    for (const s of [4, 0, 1, 5, 2, 6, 3, 7]) if (this.state.slots[s] === null) return s;
    return -1;
  }

  player(id) {
    return this.state.players.find((p) => p.id === id);
  }

  // A request from a player (or, for the host, from its own window).
  act(action) {
    this.handle(this.selfId, action);
  }

  handle(id, msg) {
    const p = this.player(id);
    if (!p) return;
    const host = id === this.selfId;
    switch (msg.t) {
      case 'name': {
        const name = cleanName(msg.name);
        if (name && name !== p.name) {
          this.system(`${p.name} is now ${name}.`);
          p.name = name;
        }
        break;
      }
      case 'slot': {
        // Claim a free fielder or let go of one of yours. The host can also
        // free anyone's, to sort out a player who has wandered off.
        if (!intIn(msg.slot, 0, SLOTS - 1) || this.state.phase !== 'lobby') return;
        const owner = this.state.slots[msg.slot];
        if (owner === id || (host && owner !== null && msg.release)) {
          this.state.slots[msg.slot] = null;
        } else if (owner === null) {
          const mine = this.state.slots.filter((s) => s === id).length;
          if (mine >= MAX_LOCAL_SLOTS) return this.tell(id, 'You can play at most four fielders, one per controller.');
          this.state.slots[msg.slot] = id;
        } else return;
        break;
      }
      case 'team': {
        // A side's captain picks its team; the host picks for a side with no
        // human captain.
        if (msg.side !== 'home' && msg.side !== 'away' || this.state.phase !== 'lobby') return;
        const captain = this.state.slots[msg.side === 'home' ? 0 : 4];
        if (!(captain === id || (host && captain === null) || (host && captain === this.selfId))) {
          return this.tell(id, 'Only that side\'s captain picks its team.');
        }
        const side = this.state.match[msg.side];
        if (intIn(msg.team, 0, TEAMS.length - 1)) side.team = msg.team;
        if (intIn(msg.sidekick, 0, SIDEKICKS.length - 1)) side.sidekick = msg.sidekick;
        break;
      }
      case 'match': {
        if (!host || this.state.phase !== 'lobby') return;
        const m = this.state.match;
        const f = msg.match ?? {};
        if (intIn(f.stadium, 0, STADIUMS.length - 1)) m.stadium = f.stadium;
        if (intIn(f.minutes, 1, 30)) m.minutes = f.minutes;
        if (intIn(f.skill, 0, 4)) m.skill = f.skill;
        if (intIn(f.delay, 0, 30)) m.delay = f.delay;
        for (const k of ['items', 'bowser', 'superStrikes']) if (typeof f[k] === 'boolean') m[k] = f[k];
        break;
      }
      case 'chat': {
        const text = String(msg.text ?? '').replace(/[\u0000-\u001f]/g, ' ').trim().slice(0, 300);
        if (text) this.broadcast({ t: 'chat', from: p.name, text });
        return;
      }
      case 'ingame':
        p.inGame = !!msg.running;
        if (host && !p.inGame && this.state.phase !== 'lobby') {
          // The match ends with the host's game.
          this.state.phase = 'lobby';
        }
        break;
      case 'pong':
        if (typeof msg.sent === 'number') p.ping = Math.max(0, Date.now() - msg.sent);
        break;
      case 'start':
        if (host) this.start();
        return;
      default:
        return;
    }
    this.changed();
  }

  start() {
    const problem = startProblem(this.state);
    if (problem) return this.tell(this.selfId, problem);
    const plan = planMatch(this.state);
    this.state.phase = 'playing';
    for (const p of this.state.players) p.inGame = true;
    for (const [id, c] of this.clients) c.send({ t: 'start', seat: plan.seats[id], plan });
    this.emit('start', { seat: 0, plan });
    this.changed();
  }

  ping() {
    for (const c of this.clients.values()) c.send({ t: 'ping', sent: Date.now() });
  }

  tell(id, text) {
    if (id === this.selfId) this.emit('notice', text);
    else this.clients.get(id)?.send({ t: 'notice', text });
  }

  system(text) {
    this.broadcast({ t: 'chat', from: null, text });
  }

  broadcast(msg) {
    for (const c of this.clients.values()) c.send(msg);
    if (msg.t === 'chat') this.emit('chat', msg);
  }

  refresh() {
    this.state.problem = this.state.phase === 'lobby' ? startProblem(this.state) : '';
  }

  changed() {
    this.refresh();
    for (const c of this.clients.values()) c.send({ t: 'state', state: this.state });
    this.emit('state', this.state);
  }
}

// Every network this computer is on, as directed broadcasts: 255.255.255.255
// leaves through one adapter only, which on a PC with a VPN or a virtual
// adapter is often not the one the other players are on.
function broadcastAddresses() {
  const out = new Set(['255.255.255.255']);
  for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list ?? []) {
      if (a.family !== 'IPv4' || a.internal || !a.netmask) continue;
      const ip = a.address.split('.').map(Number);
      const mask = a.netmask.split('.').map(Number);
      out.add(ip.map((b, i) => (b | (~mask[i] & 255))).join('.'));
    }
  }
  return [...out];
}

class LobbyClient extends EventEmitter {
  constructor({ address, port = LOBBY_PORT, name, build, buildLabel = '' }) {
    super();
    this.address = address;
    this.port = port;
    this.hello = { t: 'hello', v: PROTOCOL, name: cleanName(name), build, buildLabel };
    this.selfId = null;
    this.state = null;
  }

  // Resolves once the host has let us in.
  connect(timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
      // IPv4: the game's netplay socket is IPv4 only, and it joins the host
      // at the address this connection reached.
      const socket = net.connect({ host: this.address, port: this.port, family: 4 });
      this.socket = socket;
      let settled = false;
      const fail = (err) => {
        if (settled) return;
        settled = true;
        socket.destroy();
        reject(err);
      };
      const timer = setTimeout(() => fail(new Error(`No lobby answered at ${this.address}:${this.port}.`)), timeoutMs);
      socket.once('connect', () => {
        this.hostAddress = String(socket.remoteAddress).replace(/^::ffff:/, '');
        this.send(this.hello);
      });
      this.send = frame(socket, (msg) => {
        switch (msg.t) {
          case 'welcome':
            this.selfId = msg.id;
            break;
          case 'reject':
            clearTimeout(timer);
            return fail(new Error(msg.reason));
          case 'state':
            this.state = msg.state;
            if (!settled && this.selfId !== null) {
              settled = true;
              clearTimeout(timer);
              resolve();
            }
            this.emit('state', msg.state);
            break;
          case 'start':
            this.emit('start', { seat: msg.seat, plan: msg.plan });
            break;
          case 'chat':
            this.emit('chat', msg);
            break;
          case 'notice':
            this.emit('notice', msg.text);
            break;
          case 'ping':
            this.send({ t: 'pong', sent: msg.sent });
            break;
        }
      }, () => {
        clearTimeout(timer);
        if (!settled) {
          return fail(new Error(`Could not reach a lobby at ${this.address}:${this.port}.`));
        }
        if (!this.leaving) this.emit('closed', 'The host closed the lobby.');
      });
      socket.once('error', (err) => {
        if (!settled) fail(new Error(`Could not reach a lobby at ${this.address}:${this.port} (${err.code ?? err.message}).`));
      });
    });
  }

  act(action) {
    this.send(action);
  }

  close() {
    this.leaving = true;
    this.socket?.destroy();
  }
}

// Lists lobbies announced on the local network, forgetting ones not heard
// from for a few seconds.
class LobbyFinder extends EventEmitter {
  start() {
    this.found = new Map();
    this.socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    this.socket.on('error', () => this.emit('lobbies', []));
    this.socket.on('message', (buf, rinfo) => {
      let msg;
      try {
        msg = JSON.parse(buf.toString('utf8'));
      } catch {
        return;
      }
      if (msg?.t !== 'openstrikers-lobby' || msg.v !== PROTOCOL || !intIn(msg.port, 1, 65535)) return;
      // One lobby can arrive from several of the host's adapters: keep the
      // address first heard, unless it was a link-local one.
      const key = typeof msg.id === 'string' ? msg.id : `${rinfo.address}:${msg.port}`;
      const known = this.found.get(key);
      const address = known && !known.address.startsWith('169.254.') ? known.address : rinfo.address;
      this.found.set(key, {
        address, port: msg.port, host: cleanName(msg.host), players: msg.players | 0,
        phase: msg.phase === 'lobby' ? 'lobby' : 'playing', seen: Date.now(),
      });
      this.publish();
    });
    this.socket.bind(BEACON_PORT);
    this.timer = setInterval(() => this.publish(), 1500);
  }

  publish() {
    const now = Date.now();
    for (const [k, v] of this.found) if (now - v.seen > 4000) this.found.delete(k);
    this.emit('lobbies', [...this.found.values()].map(({ seen, ...rest }) => rest));
  }

  stop() {
    clearInterval(this.timer);
    this.socket?.close();
    this.socket = null;
  }
}

// "host" or "host:port" -> { address, port }, or null.
function parseAddress(text, defaultPort = LOBBY_PORT) {
  const m = String(text ?? '').trim().match(/^([^:\s/]+)(?::(\d{1,5}))?$/);
  if (!m) return null;
  const port = m[2] ? Number(m[2]) : defaultPort;
  return port >= 1 && port <= 65535 ? { address: m[1], port } : null;
}

module.exports = {
  PROTOCOL, LOBBY_PORT, BEACON_PORT, GAME_PORT, SLOTS, TEAMS, SIDEKICKS, STADIUMS,
  LobbyHost, LobbyClient, LobbyFinder, planMatch, startProblem, parseAddress, cleanName, defaultMatch,
};
