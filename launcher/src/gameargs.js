'use strict';
// Turns the launcher's settings, and a lobby's match plan, into the game's
// command line (see `openstrikers --help`).

const { TEAMS, SIDEKICKS, STADIUMS, GAME_PORT } = require('./lobby');

const RESOLUTIONS = ['native', '1280x720', '1920x1080', '2560x1440', '3840x2160'];
const ASPECTS = ['4:3', '16:9', 'stretch', 'auto'];

function defaultSettings() {
  return {
    iso: '',
    exe: '',
    resolution: '3840x2160',
    fullscreen: true,
    display: 0,
    aspect: '4:3',
    vsync: true,
    name: '',
    localNames: ['', '', '', ''],
    lastAddress: '',
  };
}

function displayArgs(s) {
  return [
    '--resolution', RESOLUTIONS.includes(s.resolution) ? s.resolution : 'native',
    s.fullscreen ? '--fullscreen' : '--windowed',
    '--display', String(Number.isInteger(s.display) && s.display >= 0 ? s.display : 0),
    '--aspect', ASPECTS.includes(s.aspect) ? s.aspect : 'auto',
    s.vsync ? '--vsync' : '--no-vsync',
  ];
}

// Play on this machine: the game's own menus, with names over the players on
// controller ports 1-4.
function offlineArgs(s) {
  const args = [s.iso, ...displayArgs(s)];
  const names = (s.localNames ?? []).slice(0, 4).map((n) => String(n ?? '').replace(/,/g, '').trim());
  if (names.some((n) => n)) args.push('--player-names', names.join(','));
  return args;
}

function matchArgs(m) {
  const onOff = (b) => (b ? 'on' : 'off');
  const args = [
    '--home', TEAMS[m.home.team], '--home-sidekick', SIDEKICKS[m.home.sidekick],
    '--away', TEAMS[m.away.team], '--away-sidekick', SIDEKICKS[m.away.sidekick],
    '--stadium', STADIUMS[m.stadium],
    '--match-minutes', String(m.minutes),
    '--skill', String(m.skill),
    '--items', onOff(m.items), '--bowser', onOff(m.bowser), '--super-strikes', onOff(m.superStrikes),
  ];
  if (m.delay > 0) args.push('--net-delay', String(m.delay));
  if (m.endAfterMatch) args.push('--end-after-match');
  return args;
}

// A lobby match. Seat 0 is the host, which listens; everyone else joins it at
// the address they reached the lobby on.
function onlineArgs(s, { seat, plan }, hostAddress, name) {
  const args = [s.iso, ...displayArgs(s), '--net-name', name];
  if (seat === 0) {
    args.push('--net-host', String(GAME_PORT), '--net-players', String(plan.players), '--net-slots', plan.slots,
      ...matchArgs(plan.match));
  } else {
    args.push('--net-join', `${hostAddress}:${GAME_PORT}`, '--net-seat', String(seat));
  }
  return args;
}

module.exports = { RESOLUTIONS, ASPECTS, defaultSettings, displayArgs, offlineArgs, onlineArgs, matchArgs };
