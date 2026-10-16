'use strict';
// The launcher's main process: settings, the game process, and the lobby. The
// window (renderer/) only shows things and asks for them through preload.js.

const { app, BrowserWindow, ipcMain, dialog, shell, screen } = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { LobbyHost, LobbyClient, LobbyFinder, parseAddress, LOBBY_PORT } = require('./lobby');
const { defaultSettings, offlineArgs, onlineArgs } = require('./gameargs');

// --- where things live ---------------------------------------------------------

// The Win32 launcher kept its settings in %LOCALAPPDATA%\OpenStrikers; keep
// using that folder (and its logs) there. OPENSTRIKERS_LAUNCHER_HOME moves it,
// for tests.
const dataDir = process.env.OPENSTRIKERS_LAUNCHER_HOME ||
  (process.platform === 'win32' && process.env.LOCALAPPDATA
    ? path.join(process.env.LOCALAPPDATA, 'OpenStrikers')
    : path.join(app.getPath('appData'), 'OpenStrikers'));
const settingsFile = path.join(dataDir, 'launcher.json');
const logDir = path.join(dataDir, 'logs');
app.setPath('userData', path.join(dataDir, 'electron'));

const exeName = process.platform === 'win32' ? 'openstrikers.exe' : 'openstrikers';

// The game next to the launcher or in a folder above it, preferring a
// build-audio folder (it has sound) over build: that finds it beside a copied
// launcher, and in a checkout both when run from launcher/ and when packaged
// into launcher/dist/.
function findGame() {
  const starts = [process.resourcesPath, path.dirname(process.execPath), __dirname].filter(Boolean);
  for (const start of starts) {
    let dir = start;
    for (let up = 0; up < 5; ++up) {
      for (const sub of ['', 'build-audio', 'build']) {
        const candidate = path.join(dir, sub, exeName);
        // Windows names ignore case: never take the launcher for the game.
        if (fs.existsSync(candidate) && path.resolve(candidate).toLowerCase() !== path.resolve(process.execPath).toLowerCase()) {
          return candidate;
        }
      }
      dir = path.dirname(dir);
    }
  }
  return '';
}

// --- settings --------------------------------------------------------------------

// The old launcher's launcher.ini (UTF-16), read once so nobody has to pick
// their disc again.
function importIni() {
  const ini = path.join(dataDir, 'launcher.ini');
  if (!fs.existsSync(ini)) return {};
  try {
    const raw = fs.readFileSync(ini);
    const text = raw[0] === 0xff && raw[1] === 0xfe ? raw.toString('utf16le') : raw.toString('utf8');
    const v = {};
    for (const line of text.split(/\r?\n/)) {
      const m = line.match(/^\s*(\w+)\s*=(.*)$/);
      if (m) v[m[1]] = m[2].trim();
    }
    const out = {};
    if (v.Iso) out.iso = v.Iso;
    if (v.Executable) out.exe = v.Executable;
    const res = ['native', '1280x720', '1920x1080', '2560x1440', '3840x2160'][Number(v.Resolution)];
    if (res) out.resolution = res;
    if (v.Mode !== undefined) out.fullscreen = v.Mode === '0';
    if (v.Display !== undefined) out.display = Number(v.Display) || 0;
    const aspect = ['4:3', 'stretch', '16:9', 'auto'][Number(v.Aspect)];
    if (aspect) out.aspect = aspect;
    if (v.Vsync !== undefined) out.vsync = v.Vsync === '1';
    if (v.NetName) out.name = v.NetName;
    if (v.NetAddress) out.lastAddress = v.NetAddress;
    return out;
  } catch {
    return {};
  }
}

let settings;

function loadSettings() {
  let saved = null;
  try {
    saved = JSON.parse(fs.readFileSync(settingsFile, 'utf8'));
  } catch {
    saved = null;
  }
  settings = { ...defaultSettings(), ...(saved ?? importIni()) };
  if (!saved && !settings.iso && process.env.OPENSTRIKERS_ISO) settings.iso = process.env.OPENSTRIKERS_ISO;
  if (!settings.exe || !fs.existsSync(settings.exe)) settings.exe = findGame();
  if (!settings.name) settings.name = (process.env.USERNAME || process.env.USER || 'Player').slice(0, 16);
}

function saveSettings() {
  fs.mkdirSync(dataDir, { recursive: true });
  fs.writeFileSync(settingsFile, JSON.stringify(settings, null, 2));
}

// --- the game ----------------------------------------------------------------------

// Every machine in a match must run the same build on the same disc (the game
// checks too, but only once everyone has launched). The lobby compares this.
const buildCache = new Map();
function buildId() {
  let exeHash = '';
  try {
    const st = fs.statSync(settings.exe);
    const key = `${settings.exe}|${st.size}|${st.mtimeMs}`;
    if (!buildCache.has(key)) {
      buildCache.set(key, crypto.createHash('sha256').update(fs.readFileSync(settings.exe)).digest('hex'));
    }
    exeHash = buildCache.get(key);
  } catch {
    exeHash = 'no-game';
  }
  const disc = discInfo();
  const id = crypto.createHash('sha256').update(exeHash).update(disc.id || 'no-disc').digest('hex');
  return { id, label: id.slice(0, 8) };
}

// The disc's game code and version, and whether it is Super Mario Strikers.
function discInfo() {
  try {
    const fd = fs.openSync(settings.iso, 'r');
    const header = Buffer.alloc(32);
    const n = fs.readSync(fd, header, 0, 32, 0);
    fs.closeSync(fd);
    const magic = header.readUInt32BE(28) === 0xc2339f3d;
    const code = header.toString('latin1', 0, 6);
    if (n !== 32 || !magic || !code.startsWith('G4Q')) {
      return { ok: false, reason: 'This is not a Super Mario Strikers GameCube disc image (ISO/GCM, uncompressed).' };
    }
    return { ok: true, id: `${code}-${header[7]}`, code };
  } catch {
    return { ok: false, reason: settings.iso ? 'The disc image could not be read.' : 'Choose your disc image.' };
  }
}

function readiness() {
  const disc = discInfo();
  const exeOk = !!settings.exe && fs.existsSync(settings.exe);
  return {
    disc,
    exeOk,
    problem: !exeOk ? 'The game was not found. Choose openstrikers in Settings.' : disc.ok ? '' : disc.reason,
  };
}

let game = null; // { child, log, online }

function launch(args, online) {
  const ready = readiness();
  if (ready.problem) return { ok: false, error: ready.problem };
  if (game) return { ok: false, error: 'The game is already running.' };
  fs.mkdirSync(logDir, { recursive: true });
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const logPath = path.join(logDir, `game-${stamp}.log`);
  const fd = fs.openSync(logPath, 'w');
  let child;
  try {
    child = spawn(settings.exe, args, { cwd: path.dirname(settings.exe), stdio: ['ignore', fd, fd], detached: true });
  } catch (err) {
    fs.closeSync(fd);
    return { ok: false, error: `Could not start the game: ${err.message}` };
  }
  fs.closeSync(fd);
  game = { child, logPath, online };
  send('game', { running: true });
  child.once('error', (err) => finished(null, err.message));
  child.once('exit', (code) => finished(code));
  if (win && !win.isDestroyed()) win.minimize();
  return { ok: true };
}

// The game's exit codes (src/net/session.cpp): 4 desync, 5 a player left, 6 could not join.
const EXIT_REASONS = {
  2: 'The game could not start.',
  4: 'The match went out of sync between players.',
  5: 'The network match ended: a player left or the connection dropped.',
  6: 'Could not join the host\'s match.',
};

function finished(code, error) {
  if (!game) return;
  const { logPath } = game;
  game = null;
  let detail = '';
  if (code !== 0) {
    try {
      const lines = fs.readFileSync(logPath, 'utf8').split(/\r?\n/).filter((l) => /openstrikers: netplay|error|failed|cannot/i.test(l));
      detail = lines.slice(-3).join('\n');
    } catch {
      detail = '';
    }
  }
  const message = error ? `Could not start the game: ${error}` : code === 0 || code === null ? '' : (EXIT_REASONS[code] ?? `The game stopped (exit code ${code}).`);
  send('game', { running: false, code, message, detail, logPath });
  lobby?.act({ t: 'ingame', running: false });
  if (win && !win.isDestroyed()) {
    win.restore();
    win.focus();
  }
}

// --- the lobby ------------------------------------------------------------------------

let lobby = null; // LobbyHost or LobbyClient
let finder = null;

function wireLobby(l) {
  l.on('state', (state) => send('lobby', { state, selfId: l.selfId, host: l instanceof LobbyHost }));
  l.on('chat', (msg) => send('chat', msg));
  l.on('notice', (text) => send('notice', text));
  l.on('closed', (reason) => {
    lobby = null;
    send('lobby', null);
    send('notice', reason);
  });
  l.on('start', (start) => {
    const self = l.state.players.find((p) => p.id === l.selfId);
    const args = onlineArgs(settings, start, l.hostAddress ?? null, self?.name || settings.name);
    const result = launch(args, true);
    if (!result.ok) {
      send('notice', result.error);
      l.act({ t: 'ingame', running: false });
    }
  });
}

function leaveLobby() {
  lobby?.close();
  lobby = null;
}

// --- window --------------------------------------------------------------------------

let win = null;

function send(channel, payload) {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

function createWindow() {
  win = new BrowserWindow({
    width: 1180,
    height: 760,
    minWidth: 900,
    minHeight: 620,
    backgroundColor: '#0b1020',
    title: 'OpenStrikers',
    titleBarStyle: 'hidden',
    titleBarOverlay: { color: '#00000000', symbolColor: '#e8ecff', height: 40 },
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
    },
  });
  win.setMenuBarVisibility(false);
  win.once('ready-to-show', () => win.show());
  if (process.env.OPENSTRIKERS_LAUNCHER_CAPTURE) captureForTests(process.env.OPENSTRIKERS_LAUNCHER_CAPTURE);
  // Links open in the browser, never in the launcher.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https:\/\//.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  win.webContents.on('will-navigate', (e) => e.preventDefault());
  win.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
}

// For tests/launcher-smoke: run OPENSTRIKERS_LAUNCHER_SCRIPT (JavaScript, in
// the window) once it has loaded, wait, save a picture of the window to `file`
// and quit.
function captureForTests(file) {
  win.webContents.on('console-message', (e) => process.stderr.write(`window: ${e.message}
`));
  win.webContents.once('did-finish-load', async () => {
    const script = process.env.OPENSTRIKERS_LAUNCHER_SCRIPT;
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    await wait(800);
    if (script) await win.webContents.executeJavaScript(fs.readFileSync(script, 'utf8'));
    await wait(Number(process.env.OPENSTRIKERS_LAUNCHER_WAIT_MS) || 1500);
    const image = await win.webContents.capturePage();
    fs.writeFileSync(file, image.toPNG());
    if (!process.env.OPENSTRIKERS_LAUNCHER_STAY) app.quit();
  });
}

function publicSettings() {
  return { ...settings, ready: readiness(), build: buildId().label };
}

function registerIpc() {
  ipcMain.handle('settings:get', () => publicSettings());
  ipcMain.handle('settings:set', (_e, patch) => {
    const allowed = ['iso', 'exe', 'resolution', 'fullscreen', 'display', 'aspect', 'vsync', 'name', 'localNames', 'lastAddress'];
    for (const k of allowed) if (patch && k in patch) settings[k] = patch[k];
    saveSettings();
    if (patch && 'name' in patch && lobby) lobby.act({ t: 'name', name: settings.name });
    return publicSettings();
  });
  ipcMain.handle('settings:browse', async (_e, kind) => {
    const isIso = kind === 'iso';
    const result = await dialog.showOpenDialog(win, {
      title: isIso ? 'Choose your Super Mario Strikers disc image' : 'Choose openstrikers',
      defaultPath: (isIso ? settings.iso : settings.exe) || undefined,
      properties: ['openFile'],
      filters: isIso
        ? [{ name: 'GameCube disc images', extensions: ['iso', 'gcm'] }, { name: 'All files', extensions: ['*'] }]
        : process.platform === 'win32' ? [{ name: 'openstrikers', extensions: ['exe'] }] : [],
    });
    if (result.canceled || !result.filePaths[0]) return publicSettings();
    settings[isIso ? 'iso' : 'exe'] = result.filePaths[0];
    saveSettings();
    return publicSettings();
  });
  ipcMain.handle('displays', () => screen.getAllDisplays().map((d, i) => ({
    index: i,
    label: `${d.label || `Display ${i + 1}`} (${Math.round(d.size.width * d.scaleFactor)}x${Math.round(d.size.height * d.scaleFactor)})`,
    primary: d.id === screen.getPrimaryDisplay().id,
  })));
  ipcMain.handle('logs:open', () => {
    fs.mkdirSync(logDir, { recursive: true });
    shell.openPath(logDir);
  });
  ipcMain.handle('log:show', (_e, logPath) => {
    if (typeof logPath === 'string' && path.dirname(path.resolve(logPath)) === path.resolve(logDir)) shell.showItemInFolder(logPath);
  });
  ipcMain.handle('game:offline', () => launch(offlineArgs(settings), false));
  ipcMain.handle('game:running', () => !!game);

  ipcMain.handle('lobby:host', async () => {
    const ready = readiness();
    if (ready.problem) return { ok: false, error: ready.problem };
    leaveLobby();
    const build = buildId();
    const host = new LobbyHost({ name: settings.name, build: build.id, buildLabel: build.label });
    try {
      await host.listen();
    } catch (err) {
      host.close();
      return { ok: false, error: err.code === 'EADDRINUSE'
        ? `Port ${LOBBY_PORT} is in use: is another launcher already hosting on this computer?`
        : `Could not open the lobby: ${err.message}` };
    }
    lobby = host;
    wireLobby(host);
    host.changed();
    return { ok: true };
  });
  ipcMain.handle('lobby:join', async (_e, text) => {
    const ready = readiness();
    if (ready.problem) return { ok: false, error: ready.problem };
    const target = parseAddress(text);
    if (!target) return { ok: false, error: 'Type the host\'s address, like 192.168.1.20.' };
    leaveLobby();
    const build = buildId();
    const client = new LobbyClient({ ...target, name: settings.name, build: build.id, buildLabel: build.label });
    // Listening before we are in: the host greets newcomers before the
    // first state arrives.
    const early = [];
    const keep = (msg) => early.push(msg);
    client.on('chat', keep);
    try {
      await client.connect();
    } catch (err) {
      client.close();
      return { ok: false, error: err.message };
    }
    client.off('chat', keep);
    settings.lastAddress = String(text).trim();
    saveSettings();
    lobby = client;
    wireLobby(client);
    send('lobby', { state: client.state, selfId: client.selfId, host: false });
    for (const msg of early) send('chat', msg);
    return { ok: true };
  });
  ipcMain.handle('lobby:act', (_e, action) => {
    if (lobby && action && typeof action === 'object') lobby.act(action);
  });
  ipcMain.handle('lobby:leave', () => {
    leaveLobby();
    send('lobby', null);
  });
  ipcMain.handle('lan:watch', (_e, on) => {
    if (on && !finder) {
      finder = new LobbyFinder();
      finder.on('lobbies', (list) => send('lan', list));
      finder.start();
    } else if (!on && finder) {
      finder.stop();
      finder = null;
    }
  });
  // The addresses friends can reach this lobby at: the local network's, and a
  // VPN's like Tailscale (100.64.0.0/10). Virtual machine adapters (WSL,
  // Hyper-V, Docker) and link-local addresses only reach this computer.
  ipcMain.handle('addresses', () => {
    const out = [];
    for (const [name, list] of Object.entries(require('node:os').networkInterfaces())) {
      if (/vEthernet|WSL|Hyper-V|VirtualBox|VMware|docker|^br-|^veth|^virbr/i.test(name)) continue;
      for (const a of list ?? []) {
        if (a.family !== 'IPv4' || a.internal || a.address.startsWith('169.254.')) continue;
        const [x, y] = a.address.split('.').map(Number);
        const vpn = x === 100 && y >= 64 && y < 128;
        out.push({ name, address: a.address, kind: vpn ? 'VPN' : 'network' });
      }
    }
    return out.sort((a, b) => (a.kind === b.kind ? 0 : a.kind === 'network' ? -1 : 1));
  });
}

app.whenReady().then(() => {
  loadSettings();
  registerIpc();
  createWindow();
});

app.on('window-all-closed', () => {
  leaveLobby();
  finder?.stop();
  app.quit();
});
