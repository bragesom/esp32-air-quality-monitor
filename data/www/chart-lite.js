/*
 * chart-lite.js — a minimal, dependency-free canvas line chart.
 * Works fully offline. Usage:
 *   LiteChart.draw(canvasEl, [{x: epochSeconds, y: value}, ...], {
 *     color: '#ff6b6b', fill: 'rgba(255,107,107,0.12)', yfmt: v => v.toFixed(0)
 *   });
 */
(function () {
  'use strict';

  function draw(canvas, points, opts) {
    opts = opts || {};
    const color = opts.color || '#007bff';
    const fill = opts.fill || 'rgba(0,123,255,0.12)';
    const yfmt = opts.yfmt || ((v) => v.toFixed(0));
    const ctx = canvas.getContext('2d');

    // High-DPI + responsive sizing based on the element's CSS box.
    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth || canvas.width || 400;
    const cssH = canvas.clientHeight || canvas.height || 200;
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    ctx.font = '11px system-ui, -apple-system, sans-serif';
    ctx.textBaseline = 'middle';

    const padL = 46, padR = 12, padT = 12, padB = 24;
    const w = cssW - padL - padR, h = cssH - padT - padB;

    if (!points || points.length === 0) {
      ctx.fillStyle = '#adb5bd';
      ctx.textAlign = 'center';
      ctx.fillText('No data for this range', cssW / 2, cssH / 2);
      return;
    }

    let xmin = points[0].x, xmax = points[0].x, ymin = points[0].y, ymax = points[0].y;
    for (const p of points) {
      if (p.x < xmin) xmin = p.x;
      if (p.x > xmax) xmax = p.x;
      if (p.y < ymin) ymin = p.y;
      if (p.y > ymax) ymax = p.y;
    }
    if (xmax === xmin) xmax = xmin + 1;
    const yPad = (ymax - ymin) * 0.1 || 1;
    ymin -= yPad; ymax += yPad;

    const X = (x) => padL + (x - xmin) / (xmax - xmin) * w;
    const Y = (y) => padT + h - (y - ymin) / (ymax - ymin) * h;

    // Horizontal grid + Y labels.
    ctx.strokeStyle = '#e9ecef';
    ctx.fillStyle = '#6c757d';
    ctx.lineWidth = 1;
    ctx.textAlign = 'right';
    for (let i = 0; i <= 4; i++) {
      const v = ymin + (ymax - ymin) * i / 4;
      const y = Y(v);
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(padL + w, y);
      ctx.stroke();
      ctx.fillText(yfmt(v), padL - 6, y);
    }

    // X time labels (start / middle / end).
    ctx.textAlign = 'center';
    for (let i = 0; i <= 2; i++) {
      const t = xmin + (xmax - xmin) * i / 2;
      const lbl = new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      let x = X(t);
      x = Math.min(Math.max(x, padL + 16), padL + w - 16);
      ctx.fillText(lbl, x, cssH - 8);
    }

    // Area fill.
    ctx.beginPath();
    ctx.moveTo(X(points[0].x), padT + h);
    for (const p of points) ctx.lineTo(X(p.x), Y(p.y));
    ctx.lineTo(X(points[points.length - 1].x), padT + h);
    ctx.closePath();
    ctx.fillStyle = fill;
    ctx.fill();

    // Line.
    ctx.beginPath();
    ctx.moveTo(X(points[0].x), Y(points[0].y));
    for (const p of points) ctx.lineTo(X(p.x), Y(p.y));
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  window.LiteChart = { draw };
})();
