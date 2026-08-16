/* dashboard.js — live readings + hourly CO2 trend. Uses common.js + chart-lite.js. */
(function () {
  'use strict';

  // Live push from the device (temp/rh already in real units).
  window.onSensor = function (m) {
    render(m.co2, m.temp, m.rh);
  };

  function render(co2, temp, rh) {
    const c = $('co2Value'); if (c) c.textContent = Math.round(co2);
    const t = $('tempValue'); if (t) t.textContent = Number(temp).toFixed(1);
    const h = $('humidityValue'); if (h) h.textContent = Math.round(rh);
    setStatus('co2Status', co2Status(co2));
    setStatus('tempStatus', tempStatus(temp));
    setStatus('humidityStatus', humidityStatus(rh));
    const lu = $('lastUpdated');
    if (lu) lu.textContent = new Date().toLocaleTimeString();
  }

  function setStatus(id, s) {
    const el = $(id);
    if (!el) return;
    el.textContent = s.label;
    el.className = 'metric-status ' + s.cls;
  }

  function co2Status(v) {
    if (v < 800) return { label: 'Excellent', cls: 'good' };
    if (v < 1000) return { label: 'Good', cls: 'good' };
    if (v < 1500) return { label: 'Moderate', cls: 'warning' };
    return { label: 'Poor', cls: 'bad' };
  }
  function tempStatus(v) {
    if (v >= 20 && v <= 25) return { label: 'Optimal', cls: 'good' };
    if (v >= 18 && v <= 28) return { label: 'Good', cls: 'good' };
    return { label: 'Suboptimal', cls: 'warning' };
  }
  function humidityStatus(v) {
    if (v >= 40 && v <= 60) return { label: 'Optimal', cls: 'good' };
    if (v >= 30 && v <= 70) return { label: 'Good', cls: 'good' };
    return { label: 'Suboptimal', cls: 'warning' };
  }

  // Populate immediately from the latest stored reading.
  async function loadLatest() {
    try {
      const d = await (await fetch('/api/measurements?limit=1')).json();
      if (d.success && d.data && d.data.length) {
        const r = d.data[0];
        render(r.co2, r.temperature / 10, r.humidity / 10);
      }
    } catch (e) { /* offline / not-yet-synced */ }
  }

  async function loadMiniChart() {
    const canvas = $('miniChart');
    if (!canvas) return;
    try {
      const now = Math.floor((window.nowEpoch() || Date.now() / 1000));
      const d = await (await fetch(`/api/measurements?from=${now - 3600}&to=${now}&limit=60`)).json();
      if (d.success && d.data) {
        const pts = d.data.map((p) => ({ x: p.timestamp, y: p.co2 }));
        LiteChart.draw(canvas, pts, { color: '#ff6b6b', fill: 'rgba(255,107,107,0.12)' });
      }
    } catch (e) { /* ignore */ }
  }

  // ── Settings ─────────────────────────────────────────────────
  function setStat(el, msg, cls) {
    if (!el) return;
    el.textContent = msg;
    el.className = 'status ' + cls;
  }

  async function loadSettings() {
    try {
      const s = await (await fetch('/api/settings')).json();
      if (s.success && $('intervalInput')) $('intervalInput').value = s.measureInterval;
    } catch (e) { /* ignore */ }
  }

  async function saveInterval() {
    const v = parseInt($('intervalInput').value, 10);
    const st = $('settingsStatus');
    if (!(v >= 10 && v <= 1800)) { setStat(st, 'Interval must be 10–1800 s', 'error'); return; }
    try {
      const r = await (await fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ interval: String(v) })
      })).json();
      setStat(st, r.message || (r.success ? 'Saved' : 'Failed'), r.success ? 'success' : 'error');
    } catch (e) { setStat(st, 'Failed', 'error'); }
  }

  // ── Calibration (endpoint only sets pending flags; sensor task applies) ──
  async function loadCalibration() {
    try {
      const c = await (await fetch('/api/calibration')).json();
      if (!c.success) return;
      if ($('ascToggle')) $('ascToggle').checked = !!c.asc;
      if ($('tempOffsetInput')) $('tempOffsetInput').value = Number(c.tempOffset).toFixed(1);
      if ($('lastCal')) {
        $('lastCal').textContent = c.lastCalibration
          ? new Date(c.lastCalibration * 1000).toLocaleString() : 'never';
      }
    } catch (e) { /* ignore */ }
  }

  async function postCalibration(params, okMsg) {
    const st = $('calStatus');
    try {
      const r = await (await fetch('/api/calibration', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(params)
      })).json();
      setStat(st, r.message || (r.success ? okMsg : 'Failed'), r.success ? 'success' : 'error');
      setTimeout(loadCalibration, 1500);   // reflect what the sensor task applied
    } catch (e) { setStat(st, 'Failed', 'error'); }
  }

  document.addEventListener('DOMContentLoaded', function () {
    initCommon();
    loadLatest();
    loadMiniChart();
    setInterval(loadMiniChart, 30000);
    window.addEventListener('resize', loadMiniChart);

    loadSettings();
    loadCalibration();

    if ($('intervalSave')) $('intervalSave').addEventListener('click', saveInterval);
    if ($('tempOffsetSave')) {
      $('tempOffsetSave').addEventListener('click',
        () => postCalibration({ tempOffset: $('tempOffsetInput').value }, 'Offset saved'));
    }
    if ($('ascToggle')) {
      $('ascToggle').addEventListener('change',
        () => postCalibration({ asc: $('ascToggle').checked ? '1' : '0' }, 'ASC updated'));
    }
    if ($('frcApply')) {
      $('frcApply').addEventListener('click', () => {
        const v = parseInt($('frcInput').value, 10);
        if (!(v >= 400 && v <= 2000)) { setStat($('calStatus'), 'FRC must be 400–2000 ppm', 'error'); return; }
        if (!confirm(`Recalibrate now to ${v} ppm?\nThe sensor must have been in stable outdoor air for at least 2 minutes.`)) return;
        postCalibration({ frc: String(v) }, 'Recalibration sent');
      });
    }
  });
})();
