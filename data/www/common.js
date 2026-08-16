/*
 * common.js — shared runtime for every AQ-Meter page.
 * WebSocket link, device clock, automatic time sync, status indicators.
 * No external dependencies (the device is an offline WiFi AP).
 *
 * Pages may define these optional hooks:
 *   window.onSensor(msg)  // {type:'sensor', timestamp, co2, temp, rh}
 *   window.onStatus(msg)  // {type:'status', sensor, sd, oled, wifi, timeIsSet, uptime}
 *   window.onTime(msg)    // {type:'time', timestamp, uptime, interval}
 */
(function () {
  'use strict';

  const App = {
    ws: null,
    connecting: false,
    reconnectAttempts: 0,
    timeSet: false,
    deviceEpoch: 0,       // device wall clock (s) at last sync
    deviceEpochAtMs: 0    // performance.now() when deviceEpoch was captured
  };
  window.App = App;

  const $ = (id) => document.getElementById(id);
  window.$ = $;

  // ── WebSocket ──────────────────────────────────────────────
  function connect() {
    if (App.connecting) return;
    if (App.ws && App.ws.readyState === WebSocket.OPEN) return;
    App.connecting = true;

    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    let ws;
    try { ws = new WebSocket(`${proto}//${location.host}/ws`); }
    catch (e) { App.connecting = false; scheduleReconnect(); return; }
    App.ws = ws;

    ws.onopen = () => { App.connecting = false; App.reconnectAttempts = 0; setConn('connected'); };
    ws.onclose = () => { App.connecting = false; setConn('disconnected'); scheduleReconnect(); };
    ws.onerror = () => { App.connecting = false; setConn('error'); };
    ws.onmessage = (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      handle(m);
    };
  }

  function scheduleReconnect() {
    if (App.reconnectAttempts >= 6) return;
    const delay = Math.min(2000 * Math.pow(2, App.reconnectAttempts), 30000);
    App.reconnectAttempts++;
    setTimeout(connect, delay);
  }

  function handle(m) {
    switch (m.type) {
      case 'time':
        setDeviceTime(m.timestamp);
        if (typeof window.onTime === 'function') window.onTime(m);
        break;
      case 'sensor':
        if (typeof window.onSensor === 'function') window.onSensor(m);
        break;
      case 'status':
        updateStatus(m);
        if (typeof window.onStatus === 'function') window.onStatus(m);
        break;
    }
  }

  // ── Device clock ───────────────────────────────────────────
  function setDeviceTime(epoch) {
    if (!epoch) return;
    App.deviceEpoch = epoch;
    App.deviceEpochAtMs = performance.now();
    App.timeSet = true;
  }

  function nowEpoch() {
    if (!App.timeSet) return 0;
    return App.deviceEpoch + (performance.now() - App.deviceEpochAtMs) / 1000;
  }
  window.nowEpoch = nowEpoch;

  function tickClock() {
    const t = $('currentTime'), d = $('currentDate');
    if (!App.timeSet) {
      if (t) t.textContent = '--:--:--';
      if (d) d.textContent = '----/--/--';
      return;
    }
    const dt = new Date(nowEpoch() * 1000);
    if (t) t.textContent = dt.toLocaleTimeString();
    if (d) d.textContent = dt.toLocaleDateString();
  }

  // ── Status indicators ──────────────────────────────────────
  function updateStatus(s) {
    setInd('sensorStatus', 'sensorText', s.sensor, 'Sensor');
    setInd('sdStatus', 'sdText', s.sd, 'SD Card');
    setInd('wifiStatus', 'wifiText', s.wifi, 'WiFi');
    setInd('timeStatus', 'timeText', s.timeIsSet, 'Time');
  }

  function setInd(iconId, textId, ok, label) {
    const i = $(iconId), t = $(textId);
    if (i) i.style.color = ok ? '#28a745' : '#dc3545';
    if (t) t.textContent = label + (ok ? ' ✓' : ' ✗');
  }

  function setConn(state) {
    const el = $('wsStatus');
    if (!el) return;
    const map = { connected: '🔗 Connected', disconnected: '🔌 Disconnected', error: '❌ Error' };
    el.textContent = map[state] || state;
    el.className = 'connection-status ' + state;
  }

  // ── Time sync (auto on load) ───────────────────────────────
  async function ensureTimeSync() {
    try {
      const d = await (await fetch('/api/status')).json();
      if (d.timeIsSet) { App.timeSet = true; return true; }
    } catch (e) { /* fall through and try to set */ }

    try {
      const now = Math.floor(Date.now() / 1000);
      const res = await (await fetch('/api/timeset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ timestamp: now })
      })).json();
      if (res.success) { setDeviceTime(now); return true; }
    } catch (e) { /* ignore */ }
    return false;
  }
  window.ensureTimeSync = ensureTimeSync;

  // ── Helpers ────────────────────────────────────────────────
  window.formatBytes = function (b) {
    if (!b || isNaN(b)) return '0 B';
    const u = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(b) / Math.log(1024));
    return (b / Math.pow(1024, i)).toFixed(i ? 1 : 0) + ' ' + u[i];
  };

  window.showToast = function (msg, type) {
    let el = $('toast');
    if (!el) {
      el = document.createElement('div');
      el.id = 'toast';
      document.body.appendChild(el);
    }
    el.textContent = msg;
    el.className = 'toast ' + (type || 'info');
    el.style.display = 'block';
    clearTimeout(el._t);
    el._t = setTimeout(() => { el.style.display = 'none'; }, 3000);
  };

  // ── Boot ───────────────────────────────────────────────────
  window.initCommon = function () {
    connect();
    ensureTimeSync();
    setInterval(tickClock, 1000);
    tickClock();
  };
})();
