/* files.js — SD file tools: list / preview / download / delete. Uses common.js. */
(function () {
  'use strict';

  const PREVIEWABLE = ['csv', 'txt', 'log', 'json', 'js', 'css', 'html'];

  function ext(name) { return (name.split('.').pop() || '').toLowerCase(); }

  function icon(name) {
    const e = ext(name);
    const map = { csv: '📊', txt: '📝', log: '📋', json: '🔧', js: '📜', html: '🌐', css: '🎨', gz: '📦' };
    return map[e] || '📄';
  }

  async function loadInfo() {
    try {
      const d = await (await fetch('/api/fs/info')).json();
      if (!d.success) return;
      const sd = d.sd || {};
      $('totalSpace').textContent = formatBytes(sd.total);
      $('usedSpace').textContent = formatBytes(sd.used);
      $('freeSpace').textContent = formatBytes(sd.free);
      $('fileCount').textContent = sd.fileCount != null ? sd.fileCount : '--';
      const pct = sd.total ? (sd.used / sd.total) * 100 : 0;
      $('storageProgress').style.width = pct.toFixed(1) + '%';
    } catch (e) { /* ignore */ }
  }

  let currentPath = '/';

  function parentPath(p) {
    if (p === '/' || p === '') return '/';
    const i = p.lastIndexOf('/');
    return i <= 0 ? '/' : p.slice(0, i);
  }

  function setPathLabel() {
    const el = $('currentPathLabel');
    if (el) el.textContent = currentPath;
  }

  async function loadList() {
    const body = $('fileList');
    body.innerHTML = '<tr><td colspan="4" class="loading-message">Loading…</td></tr>';
    try {
      const d = await (await fetch('/api/fs/list?path=' + encodeURIComponent(currentPath))).json();
      const entries = (d.success && d.files) ? d.files.slice() : [];
      currentPath = d.path || currentPath;
      setPathLabel();

      const dirs = entries.filter((f) => f.isDir).sort((a, b) => a.name.localeCompare(b.name));
      const files = entries.filter((f) => !f.isDir).sort((a, b) => a.name.localeCompare(b.name));

      body.innerHTML = '';
      if (currentPath !== '/') body.appendChild(upRow());
      dirs.forEach((f) => body.appendChild(dirRow(f)));
      files.forEach((f) => body.appendChild(row(f)));

      if (!dirs.length && !files.length && currentPath === '/') {
        body.innerHTML = '<tr><td colspan="4" class="empty-folder-message">No files</td></tr>';
      }
    } catch (e) {
      body.innerHTML = '<tr><td colspan="4" class="empty-folder-message">Failed to load</td></tr>';
    }
  }

  function navigate(path) { currentPath = path; loadList(); }

  function upRow() {
    const tr = document.createElement('tr');
    tr.className = 'directory-row parent-dir';
    tr.innerHTML =
      '<td class="file-icon">📂</td>' +
      '<td class="file-name"><span class="directory-name">.. (up)</span></td>' +
      '<td>—</td><td></td>';
    tr.querySelector('.directory-name').addEventListener('click', () => navigate(parentPath(currentPath)));
    return tr;
  }

  function dirRow(f) {
    const tr = document.createElement('tr');
    tr.className = 'directory-row';
    const p = f.path || (currentPath === '/' ? '/' + f.name : currentPath + '/' + f.name);
    tr.innerHTML =
      '<td class="file-icon">📁</td>' +
      `<td class="file-name"><span class="directory-name">${f.name}</span></td>` +
      '<td>—</td><td></td>';
    tr.querySelector('.directory-name').addEventListener('click', () => navigate(p));
    return tr;
  }

  function row(f) {
    const tr = document.createElement('tr');
    tr.className = 'file-row';
    const canPreview = PREVIEWABLE.includes(ext(f.name));
    const p = f.path || ('/' + f.name);
    tr.innerHTML =
      `<td class="file-icon">${icon(f.name)}</td>` +
      `<td class="file-name"><span class="file-name-text">${f.name}</span></td>` +
      `<td>${formatBytes(f.size)}</td>` +
      `<td class="file-actions">` +
        (canPreview ? `<button class="file-action" data-act="preview" title="Preview">👁️</button>` : '') +
        `<button class="file-action" data-act="download" title="Download">📥</button>` +
        `<button class="file-action delete-btn" data-act="delete" title="Delete">🗑️</button>` +
      `</td>`;
    tr.querySelectorAll('button').forEach((b) => {
      b.addEventListener('click', () => {
        const act = b.getAttribute('data-act');
        if (act === 'preview') preview(p, f.name);
        else if (act === 'download') download(p);
        else if (act === 'delete') del(p, f.name);
      });
    });
    return tr;
  }

  function download(path) {
    window.location.href = '/api/fs/download?path=' + encodeURIComponent(path);
  }

  async function preview(path, name) {
    const modal = $('previewModal');
    const title = $('previewTitle');
    const content = $('previewContent');
    title.textContent = name;
    content.innerHTML = '<div class="loading-message">Loading…</div>';
    modal.classList.remove('hidden');
    try {
      const d = await (await fetch('/api/fs/preview?path=' + encodeURIComponent(path) + '&lines=100')).json();
      if (!d.success) { content.innerHTML = '<div class="status error">Preview failed</div>'; return; }
      if (ext(name) === 'csv') content.innerHTML = csvTable(d.content);
      else content.innerHTML = '<pre>' + escapeHtml(d.content) + '</pre>';
    } catch (e) {
      content.innerHTML = '<div class="status error">Preview failed</div>';
    }
  }

  async function del(path, name) {
    if (!confirm(`Delete "${name}"?`)) return;
    try {
      const d = await (await fetch('/api/fs/delete?path=' + encodeURIComponent(path), { method: 'DELETE' })).json();
      if (d.success) { showToast('Deleted ' + name, 'info'); loadList(); loadInfo(); }
      else showToast(d.message || 'Delete failed', 'error');
    } catch (e) {
      showToast('Delete failed', 'error');
    }
  }

  function csvTable(text) {
    const rows = text.trim().split('\n').slice(0, 100);
    let html = '<table class="preview-table">';
    rows.forEach((line, i) => {
      const cells = line.split(',');
      html += '<tr>' + cells.map((c) => (i === 0 ? `<th>${escapeHtml(c)}</th>` : `<td>${escapeHtml(c)}</td>`)).join('') + '</tr>';
    });
    return html + '</table>';
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  }

  document.addEventListener('DOMContentLoaded', function () {
    initCommon();
    loadInfo();
    loadList();
    $('refreshBtn').addEventListener('click', () => { loadInfo(); loadList(); });
    $('previewClose').addEventListener('click', () => $('previewModal').classList.add('hidden'));
    $('previewModal').addEventListener('click', (e) => {
      if (e.target === $('previewModal')) $('previewModal').classList.add('hidden');
    });
  });
})();
