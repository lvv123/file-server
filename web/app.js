'use strict';

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const state = {
  dir: '/',
  searching: false,
};

function joinPath(dir, name) {
  if (!dir || dir === '/') return '/' + name;
  return dir.replace(/\/+$/, '') + '/' + name;
}

function parentOf(filePath) {
  const idx = filePath.lastIndexOf('/');
  if (idx <= 0) return '/';
  return filePath.slice(0, idx);
}

function formatSize(bytes) {
  if (bytes === null || bytes === undefined) return '';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = Number(bytes);
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${value} ${units[0]}` : `${value.toFixed(1)} ${units[unit]}`;
}

let toastTimer = null;
function toast(message, isError) {
  const el = document.getElementById('toast');
  el.textContent = message;
  el.classList.toggle('error', !!isError);
  el.hidden = false;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

async function api(method, url, body) {
  const options = { method, headers: {} };
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(url, options);
  let payload = null;
  const text = await response.text();
  if (text) {
    try { payload = JSON.parse(text); } catch (e) { payload = null; }
  }
  if (!response.ok || !payload || payload.ok !== true) {
    const message = payload && payload.error ? payload.error.message : `HTTP ${response.status}`;
    const error = new Error(message);
    error.status = response.status;
    error.code = payload && payload.error ? payload.error.code : 'HTTP_ERROR';
    throw error;
  }
  return payload.data;
}

// ---------------------------------------------------------------------------
// Directory browsing
// ---------------------------------------------------------------------------

function renderCrumbs() {
  const nav = document.getElementById('crumbs');
  nav.textContent = '';
  const rootLink = document.createElement('a');
  rootLink.textContent = '根目录';
  rootLink.onclick = () => navigate('/');
  nav.appendChild(rootLink);

  const parts = state.dir.split('/').filter(Boolean);
  let acc = '';
  for (const part of parts) {
    acc += '/' + part;
    const sep = document.createElement('span');
    sep.className = 'sep';
    sep.textContent = '/';
    nav.appendChild(sep);
    const link = document.createElement('a');
    link.textContent = part;
    const target = acc;
    link.onclick = () => navigate(target);
    nav.appendChild(link);
  }
}

function makeButton(label, cls, onClick) {
  const btn = document.createElement('button');
  btn.className = 'btn small ' + (cls || '');
  btn.textContent = label;
  btn.onclick = (event) => { event.stopPropagation(); onClick(); };
  return btn;
}

function renderEntries(entries) {
  const tbody = document.getElementById('listing');
  tbody.textContent = '';
  const empty = document.getElementById('empty');
  empty.hidden = entries.length > 0;

  for (const entry of entries) {
    const tr = document.createElement('tr');

    const nameCell = document.createElement('td');
    const nameBox = document.createElement('div');
    nameBox.className = 'name';
    const icon = document.createElement('span');
    icon.className = 'icon';
    icon.textContent = entry.is_dir ? '📁' : '📄';
    const label = document.createElement('span');
    label.textContent = entry.name;
    nameBox.appendChild(icon);
    nameBox.appendChild(label);
    nameBox.onclick = () => {
      if (entry.is_dir) navigate(entry.path);
    };
    nameCell.appendChild(nameBox);

    const sizeCell = document.createElement('td');
    sizeCell.className = 'num';
    sizeCell.textContent = entry.is_dir ? '' : formatSize(entry.size);

    const timeCell = document.createElement('td');
    timeCell.className = 'muted';
    timeCell.textContent = entry.mtime_text || '';

    const opsCell = document.createElement('td');
    opsCell.className = 'ops';
    if (!entry.is_dir) {
      const link = document.createElement('a');
      link.href = '/api/download?path=' + encodeURIComponent(entry.path);
      link.className = 'btn small';
      link.textContent = '下载';
      opsCell.appendChild(link);
      opsCell.appendChild(document.createTextNode(' '));
    }
    opsCell.appendChild(makeButton('重命名', 'ghost', () => doRename(entry)));
    opsCell.appendChild(makeButton('删除', 'danger', () => doDelete(entry)));

    tr.appendChild(nameCell);
    tr.appendChild(sizeCell);
    tr.appendChild(timeCell);
    tr.appendChild(opsCell);
    tbody.appendChild(tr);
  }
}

async function navigate(path) {
  state.dir = path || '/';
  state.searching = false;
  document.getElementById('search-view').hidden = true;
  document.getElementById('browser').hidden = false;
  document.getElementById('btn-clear').hidden = true;
  renderCrumbs();
  try {
    const data = await api('GET', '/api/files?path=' + encodeURIComponent(state.dir));
    state.dir = data.path;
    renderCrumbs();
    renderEntries(data.entries);
  } catch (error) {
    toast(error.message, true);
  }
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

async function doMkdir() {
  const name = prompt('新建文件夹名称：');
  if (!name) return;
  try {
    await api('POST', '/api/mkdir', { path: joinPath(state.dir, name) });
    toast('文件夹已创建');
    navigate(state.dir);
  } catch (error) {
    toast(error.message, true);
  }
}

async function doRename(entry) {
  const name = prompt('新的名称：', entry.name);
  if (!name || name === entry.name) return;
  try {
    await api('POST', '/api/rename', {
      from: entry.path,
      to: joinPath(parentOf(entry.path), name),
    });
    toast('已重命名');
    navigate(state.dir);
  } catch (error) {
    toast(error.message, true);
  }
}

async function doDelete(entry) {
  if (!confirm(`确定删除 “${entry.name}” 吗？此操作不可恢复。`)) return;
  try {
    await api('DELETE', '/api/files?path=' + encodeURIComponent(entry.path));
    toast('已删除');
    navigate(state.dir);
  } catch (error) {
    toast(error.message, true);
  }
}

// Rebuilds the server-side index from what is actually on disk. Useful after
// copying files into the data directory by hand.
async function doResync() {
  const button = document.getElementById('btn-resync');
  button.disabled = true;
  try {
    const data = await api('GET', '/api/resync');
    const removed = data.removed ? `，移除 ${data.removed} 条失效记录` : '';
    toast(`索引已同步：扫描 ${data.scanned} 项${removed}`);
    navigate(state.dir);
  } catch (error) {
    toast(error.message, true);
  } finally {
    button.disabled = false;
  }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

async function doSearch() {
  const q = document.getElementById('q').value.trim();
  if (!q) return;
  try {
    const data = await api('GET', '/api/search?q=' + encodeURIComponent(q));
    state.searching = true;
    document.getElementById('browser').hidden = true;
    document.getElementById('search-view').hidden = false;
    document.getElementById('btn-clear').hidden = false;

    const tbody = document.getElementById('search-results');
    tbody.textContent = '';
    if (data.results.length === 0) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 4;
      td.className = 'empty';
      td.textContent = '没有匹配的文件';
      tr.appendChild(td);
      tbody.appendChild(tr);
    }
    for (const hit of data.results) {
      const tr = document.createElement('tr');
      const nameCell = document.createElement('td');
      nameCell.textContent = hit.name;
      const sizeCell = document.createElement('td');
      sizeCell.className = 'num';
      sizeCell.textContent = formatSize(hit.size);
      const locCell = document.createElement('td');
      locCell.className = 'muted';
      locCell.textContent = parentOf(hit.path);
      const opsCell = document.createElement('td');
      opsCell.className = 'ops';
      const link = document.createElement('a');
      link.href = '/api/download?path=' + encodeURIComponent(hit.path);
      link.className = 'btn small';
      link.textContent = '下载';
      opsCell.appendChild(link);
      tr.appendChild(nameCell);
      tr.appendChild(sizeCell);
      tr.appendChild(locCell);
      tr.appendChild(opsCell);
      tbody.appendChild(tr);
    }
  } catch (error) {
    toast(error.message, true);
  }
}

function clearSearch() {
  document.getElementById('q').value = '';
  navigate(state.dir);
}

// ---------------------------------------------------------------------------
// Chunked upload with resume
// ---------------------------------------------------------------------------

const sessionKey = (targetPath, file) =>
  `fsrv-session:${targetPath}:${file.size}:${file.lastModified}`;

// How many chunk requests a single file keeps in flight. Each chunk is an
// independent request and the server writes them concurrently, so overlapping
// them hides the per-request latency (and the per-chunk fsync) instead of
// leaving the connection idle between chunks.
const UPLOAD_CONCURRENCY = 4;

function chunkLength(fileSize, chunkSize, index) {
  return Math.min(chunkSize, fileSize - index * chunkSize);
}

async function sendChunk(file, sessionId, index, chunkSize) {
  const start = index * chunkSize;
  const end = Math.min(start + chunkSize, file.size);
  const blob = file.slice(start, end);
  let lastError = null;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    try {
      const response = await fetch('/api/upload/chunk', {
        method: 'PUT',
        headers: {
          'X-Session-Id': sessionId,
          'X-Chunk-Index': String(index),
        },
        body: blob,
      });
      if (response.ok) return;
      let message = `HTTP ${response.status}`;
      try {
        const payload = await response.json();
        if (payload && payload.error) message = payload.error.message;
      } catch (e) { /* ignore */ }
      lastError = new Error(message);
    } catch (e) {
      lastError = e;
    }
  }
  throw lastError || new Error('分片上传失败');
}

function createUploadRow(file) {
  const container = document.getElementById('uploads');
  const row = document.createElement('div');
  row.className = 'upload-row';
  const meta = document.createElement('div');
  meta.className = 'meta';
  const name = document.createElement('span');
  name.textContent = file.name;
  const status = document.createElement('span');
  status.textContent = '准备中…';
  meta.appendChild(name);
  meta.appendChild(status);
  const bar = document.createElement('div');
  bar.className = 'bar';
  const fill = document.createElement('div');
  bar.appendChild(fill);
  row.appendChild(meta);
  row.appendChild(bar);
  container.appendChild(row);

  return {
    row,
    status,
    fill,
    setProgress(done, total, text) {
      const pct = total === 0 ? 100 : Math.floor((done / total) * 100);
      fill.style.width = pct + '%';
      status.textContent = text !== undefined
        ? text
        : `${formatSize(done)} / ${formatSize(total)} (${pct}%)`;
    },
    finish(text, failed) {
      status.textContent = text;
      row.classList.add(failed ? 'failed' : 'done');
      if (!failed) fill.style.width = '100%';
      setTimeout(() => row.remove(), failed ? 15000 : 4000);
    },
  };
}

async function uploadOne(file, dir) {
  const targetPath = joinPath(dir, file.name);
  const ui = createUploadRow(file);
  const key = sessionKey(targetPath, file);

  try {
    let sessionId = null;
    let chunkSize = 0;
    let totalChunks = 0;
    let received = new Set();

    const savedId = localStorage.getItem(key);
    if (savedId) {
      try {
        const info = await api('GET', '/api/upload/session?id=' + encodeURIComponent(savedId));
        if (info.path === targetPath && info.total_size === file.size) {
          sessionId = info.session_id;
          chunkSize = info.chunk_size;
          totalChunks = info.total_chunks;
          received = new Set(info.received);
        }
      } catch (e) {
        localStorage.removeItem(key);
      }
    }

    if (!sessionId) {
      const info = await api('POST', '/api/upload/init', {
        path: targetPath,
        total_size: file.size,
      });
      sessionId = info.session_id;
      chunkSize = info.chunk_size;
      totalChunks = info.total_chunks;
      localStorage.setItem(key, sessionId);
    }

    let doneBytes = 0;
    for (const index of received) doneBytes += chunkLength(file.size, chunkSize, index);
    ui.setProgress(doneBytes, file.size);

    const pending = [];
    for (let index = 0; index < totalChunks; index += 1) {
      if (!received.has(index)) pending.push(index);
    }

    let nextSlot = 0;
    let failure = null;
    const workers = [];
    const workerCount = Math.min(UPLOAD_CONCURRENCY, pending.length);
    for (let w = 0; w < workerCount; w += 1) {
      workers.push((async () => {
        for (;;) {
          if (failure) return;
          const slot = nextSlot;
          nextSlot += 1;
          if (slot >= pending.length) return;
          const index = pending[slot];
          try {
            await sendChunk(file, sessionId, index, chunkSize);
          } catch (error) {
            if (!failure) failure = error;
            return;
          }
          doneBytes += chunkLength(file.size, chunkSize, index);
          ui.setProgress(doneBytes, file.size);
        }
      })());
    }
    await Promise.all(workers);
    if (failure) throw failure;

    ui.setProgress(doneBytes, file.size, '正在合并…');
    await api('POST', '/api/upload/complete', { session_id: sessionId });
    localStorage.removeItem(key);
    ui.finish('上传完成');
  } catch (error) {
    ui.finish('上传失败：' + error.message, true);
    toast(`${file.name}: ${error.message}`, true);
  }
}

async function uploadFiles(fileList) {
  const dir = state.dir;
  for (const file of fileList) {
    await uploadOne(file, dir);
  }
  if (!state.searching) navigate(dir);
}

// ---------------------------------------------------------------------------
// Admin panel
// ---------------------------------------------------------------------------

function addCard(container, label, value) {
  const card = document.createElement('div');
  card.className = 'card';
  const l = document.createElement('div');
  l.className = 'label';
  l.textContent = label;
  const v = document.createElement('div');
  v.className = 'value';
  v.textContent = value;
  card.appendChild(l);
  card.appendChild(v);
  container.appendChild(card);
}

async function openAdmin() {
  try {
    const status = await api('GET', '/admin/status');
    const cards = document.getElementById('admin-status');
    cards.textContent = '';
    addCard(cards, '磁盘可用', status.disk.free_text);
    addCard(cards, '磁盘总量', status.disk.total_text);
    addCard(cards, '文件总数', String(status.file_count));
    addCard(cards, '活跃上传会话', String(status.active_sessions));
    addCard(cards, '近 24h 审计', String(status.recent_audits));

    const audit = await api('GET', '/admin/audit?limit=100');
    const tbody = document.getElementById('audit-rows');
    tbody.textContent = '';
    for (const record of audit.records) {
      const tr = document.createElement('tr');
      const cells = [
        record.time,
        record.client_ip,
        record.action,
        record.path,
        record.result === 'success' ? '成功' : '失败',
        `${record.duration_ms} ms`,
      ];
      for (let i = 0; i < cells.length; i += 1) {
        const td = document.createElement('td');
        td.textContent = cells[i] === null ? '' : cells[i];
        if (i === 5) td.className = 'num';
        if (i === 4) td.className = record.result === 'success' ? '' : 'muted';
        tr.appendChild(td);
      }
      tbody.appendChild(tr);
    }
    document.getElementById('admin').hidden = false;
  } catch (error) {
    toast(error.message, true);
    document.getElementById('admin').hidden = true;
  }
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

function wire() {
  document.getElementById('btn-mkdir').onclick = doMkdir;
  document.getElementById('btn-resync').onclick = doResync;
  document.getElementById('btn-search').onclick = doSearch;
  document.getElementById('btn-clear').onclick = clearSearch;
  document.getElementById('btn-admin').onclick = openAdmin;
  document.getElementById('btn-admin-close').onclick = () => {
    document.getElementById('admin').hidden = true;
  };
  document.getElementById('q').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') doSearch();
  });

  const input = document.getElementById('file-input');
  document.getElementById('btn-upload').onclick = () => input.click();
  input.onchange = () => {
    const files = Array.from(input.files || []);
    input.value = '';
    if (files.length) uploadFiles(files);
  };

  document.getElementById('listing').addEventListener('click', (event) => {
    event.stopPropagation();
  });
}

wire();
navigate('/');
