'use strict';
// The only things the window can ask of the main process.
const { contextBridge, ipcRenderer } = require('electron');

const listen = (channel) => (fn) => {
  const handler = (_e, payload) => fn(payload);
  ipcRenderer.on(channel, handler);
  return () => ipcRenderer.off(channel, handler);
};

contextBridge.exposeInMainWorld('launcher', {
  platform: process.platform,
  settings: () => ipcRenderer.invoke('settings:get'),
  setSettings: (patch) => ipcRenderer.invoke('settings:set', patch),
  browse: (kind) => ipcRenderer.invoke('settings:browse', kind),
  displays: () => ipcRenderer.invoke('displays'),
  addresses: () => ipcRenderer.invoke('addresses'),
  openLogs: () => ipcRenderer.invoke('logs:open'),
  showLog: (p) => ipcRenderer.invoke('log:show', p),
  playOffline: () => ipcRenderer.invoke('game:offline'),
  gameRunning: () => ipcRenderer.invoke('game:running'),
  host: () => ipcRenderer.invoke('lobby:host'),
  join: (address) => ipcRenderer.invoke('lobby:join', address),
  act: (action) => ipcRenderer.invoke('lobby:act', action),
  leave: () => ipcRenderer.invoke('lobby:leave'),
  watchLan: (on) => ipcRenderer.invoke('lan:watch', on),
  onLobby: listen('lobby'),
  onChat: listen('chat'),
  onNotice: listen('notice'),
  onGame: listen('game'),
  onLan: listen('lan'),
});
