/* charts.js — full-history chart page. Uses common.js + chart-lite.js. Offline. */
(function () {
  'use strict';

  const META = {
    co2:         { label: 'CO₂', unit: ' ppm', icon: '🫁', color: '#ff6b6b', fill: 'rgba(255,107,107,0.12)', yfmt: v => v.toFixed(0) },
    temperature: { label: 'Temperature', unit: ' °C', icon: '🌡️', color: '#4ecdc4', fill: 'rgba(78,205,196,0.12)', yfmt: v => v.toFixed(1) },
    humidity:    { label: 'Humidity', unit: ' %', icon: '💧', color: '#45b7d1', fill: 'rgba(69,183,209,0.12)', yfmt: v => v.toFixed(0) }
  };

  const LIMITS = { 3600: 60, 86400: 240, 604800: 240 };

  let type = 'co2';
  let range = '3600';
  let raw = [];      // untouched API rows: [{timestamp, co2, temperature, humidity}]
  let series = [];   // rows mapped through the CURRENT metric: [{x, y}]

  function valueOf(p) {
    if (type === 'co2') return p.co2;
    if (type === 'temperature') return p.temperature / 10;
    return p.humidity / 10;
  }

  async function load() {
    const now = Math.floor(window.nowEpoch() || Date.now() / 1000);
    const from = now - parseInt(range, 10);
    const limit = LIMITS[range] || 240;
    try {
      const d = await (await fetch(`/api/measurements?from=${from}&to=${now}&limit=${limit}`)).json();
      raw = (d.success && d.data) ? d.data : [];
    } catch (e) {
      raw = [];
    }
    redraw();
  }

  // Re-map the raw rows through whatever metric is selected NOW. Switching
  // CO₂→Temperature is instant and correct instead of relabelling stale values
  // until the next 30 s reload.
  function redraw() {
    const m = META[type];
    series = raw.map((p) => ({ x: p.timestamp, y: valueOf(p) }));
    $('chartTitle').textContent = `${m.icon} ${m.label}`;
    LiteChart.draw($('mainChart'), series, { color: m.color, fill: m.fill, yfmt: m.yfmt });

    const ys = series.map((p) => p.y);
    if (ys.length === 0) {
      ['currentValue', 'minValue', 'maxValue', 'avgValue'].forEach((id) => { $(id).textContent = '--'; });
      $('sampleCount').textContent = '0';
      $('chartStatus').textContent = '--';
      return;
    }
    const min = Math.min(...ys), max = Math.max(...ys);
    const avg = ys.reduce((a, b) => a + b, 0) / ys.length;
    const cur = ys[ys.length - 1];
    $('currentValue').textContent = m.yfmt(cur) + m.unit;
    $('minValue').textContent = m.yfmt(min) + m.unit;
    $('maxValue').textContent = m.yfmt(max) + m.unit;
    $('avgValue').textContent = m.yfmt(avg) + m.unit;
    $('sampleCount').textContent = String(ys.length);
    $('chartStatus').textContent = '';
  }

  function exportCSV() {
    if (!series.length) { showToast('No data to export', 'error'); return; }
    let csv = 'timestamp,iso,' + type + '\n';
    series.forEach((p) => {
      csv += `${p.x},${new Date(p.x * 1000).toISOString()},${p.y}\n`;
    });
    const url = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `${type}-${range}s.csv`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

  document.addEventListener('DOMContentLoaded', function () {
    initCommon();

    const params = new URLSearchParams(location.search);
    type = META[params.get('type')] ? params.get('type') : 'co2';
    range = LIMITS[params.get('range')] ? params.get('range') : '3600';
    $('chartType').value = type;
    $('timeRange').value = range;

    $('chartType').addEventListener('change', (e) => { type = e.target.value; redraw(); });
    $('timeRange').addEventListener('change', (e) => { range = e.target.value; load(); });
    $('refreshBtn').addEventListener('click', load);
    $('exportBtn').addEventListener('click', exportCSV);
    window.addEventListener('resize', redraw);

    // Wait briefly for the clock to sync so ranges are accurate, then load.
    setTimeout(load, 400);
    setInterval(load, 30000);
  });
})();
