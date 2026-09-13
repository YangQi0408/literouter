'use strict';
// literouter web console.
//
// A thin client of the admin API the proxy already exposes; it holds no state
// the server does not own, and it is served by the same listener, so there is
// no CORS dance and nothing to install on the machine being watched.

// ── i18n ─────────────────────────────────────────────────────────────────────

const I18N = {
  en: {
    tabOverview: 'Overview', tabLogs: 'Logs', tabConfig: 'Config',
    providers: 'Providers', models: 'Models', reload: 'Reload', resetStats: 'Reset stats',
    shutdown: 'Shutdown', keyTitle: 'API key',
    keyHint: 'The server answered 401. Paste the key from server.api_key; it stays in this browser.',
    keyClear: 'Clear key', save: 'Save',
    search: 'search…', allLevels: 'all levels', allKinds: 'all kinds', pause: 'Pause', resume: 'Resume',
    clear: 'Clear', copy: 'Copy', copied: 'Copied to clipboard',
    configNote: 'Read-only view. Secrets written literally in the file are replaced by an empty string; ${VARS} are kept as written.',
    colProvider: 'Provider', colState: 'State', colRequests: 'Requests', colSuccess: 'Success',
    colLatency: 'Latency', colTokens: 'Tokens', colLastError: 'Last error',
    colTime: 'Time', colLevel: 'Level', colKind: 'Kind', colModel: 'Model', colStatus: 'Status',
    colLatencyShort: 'ms', colMessage: 'Message',
    tileRequests: 'Requests', tileSuccess: 'Success rate', tileTokens: 'Tokens', tileTraffic: 'Sent',
    tileLatency: 'Avg latency', tileUptime: 'Uptime', tileBreakers: 'Open breakers', tileEndpoint: 'Endpoint',
    inFlight: 'in flight', ok: 'ok', failed: 'failed', prompt: 'prompt', completion: 'completion',
    test: 'Test', testing: 'Testing…', probeOk: 'reachable · {ms} · {n} models', probeFail: 'unreachable: {detail}',
    reloaded: 'config reloaded · {summary}', reloadFailed: 'reload failed · {summary}',
    statsReset: 'counters reset', shuttingDown: 'shutting down…', confirmShutdown: 'Stop the proxy server?',
    modelsHint: '{n} routed', empty: 'nothing yet', connected: 'live', disconnected: 'no connection',
    version: 'version',
  },
  zh: {
    tabOverview: '概览', tabLogs: '日志', tabConfig: '配置',
    providers: '中转站', models: '模型', reload: '重载配置', resetStats: '重置统计',
    shutdown: '关闭服务', keyTitle: 'API 密钥',
    keyHint: '服务端返回 401。请填入 server.api_key 中的密钥，它只保存在本浏览器。',
    keyClear: '清除密钥', save: '保存',
    search: '搜索…', allLevels: '全部级别', allKinds: '全部类型', pause: '暂停', resume: '继续',
    clear: '清空', copy: '复制', copied: '已复制到剪贴板',
    configNote: '只读视图。文件中以明文写入的密钥会显示为空字符串；${VAR} 引用按原样保留。',
    colProvider: '中转站', colState: '状态', colRequests: '请求数', colSuccess: '成功率',
    colLatency: '延迟', colTokens: 'Token', colLastError: '最近错误',
    colTime: '时间', colLevel: '级别', colKind: '类型', colModel: '模型', colStatus: '状态',
    colLatencyShort: 'ms', colMessage: '消息',
    tileRequests: '请求总数', tileSuccess: '成功率', tileTokens: 'Token 总数', tileTraffic: '下发流量',
    tileLatency: '平均延迟', tileUptime: '运行时长', tileBreakers: '熔断器', tileEndpoint: '接入点',
    inFlight: '进行中', ok: '成功', failed: '失败', prompt: 'prompt', completion: 'completion',
    test: '探测', testing: '探测中…', probeOk: '可达 · {ms} · {n} 个模型', probeFail: '不可达：{detail}',
    reloaded: '配置已重载 · {summary}', reloadFailed: '重载失败 · {summary}',
    statsReset: '统计已重置', shuttingDown: '正在关闭…', confirmShutdown: '确定要关闭代理服务吗？',
    modelsHint: '{n} 条路由', empty: '暂无数据', connected: '已连接', disconnected: '未连接',
    version: '版本',
  },
};

const state = {
  tab: 'overview',
  key: localStorage.getItem('lr.key') || '',
  lang: localStorage.getItem('lr.lang') || ((navigator.language || '').toLowerCase().startsWith('zh') ? 'zh' : 'en'),
  status: null,
  models: null,
  config: null,
  logs: { since: 0, paused: false, entries: [], level: '', kind: '', q: '' },
  online: false,
  probeBusy: '',
};

const $ = (sel, root = document) => root.querySelector(sel);
const t = (key) => (I18N[state.lang] && I18N[state.lang][key]) || I18N.en[key] || key;

// ── formatting ───────────────────────────────────────────────────────────────

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function humanCount(value) {
  const n = Number(value || 0);
  if (n < 1000) return String(n);
  if (n < 1e6) return (n / 1e3).toFixed(n < 1e4 ? 1 : 0) + 'k';
  if (n < 1e9) return (n / 1e6).toFixed(n < 1e7 ? 1 : 0) + 'M';
  return (n / 1e9).toFixed(1) + 'G';
}

function humanBytes(value) {
  const n = Number(value || 0);
  if (n < 1024) return n + ' B';
  if (n < 1024 ** 2) return (n / 1024).toFixed(1) + ' KB';
  if (n < 1024 ** 3) return (n / 1024 ** 2).toFixed(1) + ' MB';
  return (n / 1024 ** 3).toFixed(2) + ' GB';
}

function humanMillis(ms) {
  const n = Number(ms || 0);
  if (n < 1000) return n.toFixed(n < 10 ? 1 : 0) + 'ms';
  return (n / 1000).toFixed(1) + 's';
}

function humanDuration(sec) {
  const s = Math.max(0, Math.floor(Number(sec || 0)));
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const r = s % 60;
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${r}s`;
  return `${r}s`;
}

function pct(part, whole) {
  if (!whole) return '—';
  return ((part / whole) * 100).toFixed(part === whole ? 0 : 1) + '%';
}

// ── api ──────────────────────────────────────────────────────────────────────

async function api(path, options = {}) {
  const headers = Object.assign({ Accept: 'application/json' }, options.headers || {});
  if (state.key) headers['x-api-key'] = state.key;
  if (options.body) headers['Content-Type'] = 'application/json';
  const res = await fetch(path, Object.assign({}, options, { headers }));
  if (res.status === 401) {
    openKeyModal();
    throw new Error('unauthorized');
  }
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  if (!res.ok) {
    const message = (data && data.error && data.error.message) || res.statusText;
    throw Object.assign(new Error(message), { status: res.status, data });
  }
  return data;
}

let toastTimer = 0;
function toast(message, kind = '') {
  const el = $('#toast');
  el.textContent = message;
  el.className = 'toast show ' + kind;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast ' + kind; }, 3200);
}

// ── overview ─────────────────────────────────────────────────────────────────

function tile(label, value, sub, cls) {
  return `<div class="tile"><div class="label">${esc(label)}</div>
    <div class="value${cls ? ' ' + cls : ''}">${esc(value)}</div>
    <div class="sub">${esc(sub || '')}</div></div>`;
}

function renderOverview(status) {
  const health = new Map((status.health || []).map((h) => [h.provider, h]));
  const tokens = (status.tokens_prompt || 0) + (status.tokens_completion || 0);

  $('#version').textContent = status.version || '';
  $('#tiles').innerHTML = [
    tile(t('tileRequests'), humanCount(status.total_requests),
         `${status.active_requests || 0} ${t('inFlight')}`),
    tile(t('tileSuccess'), pct(status.total_success, status.total_requests),
         `${humanCount(status.total_success)} ${t('ok')} / ${humanCount(status.total_failure)} ${t('failed')}`),
    tile(t('tileTokens'), humanCount(tokens),
         `${humanCount(status.tokens_prompt)} ${t('prompt')} · ${humanCount(status.tokens_completion)} ${t('completion')}`),
    tile(t('tileTraffic'), humanBytes(status.bytes_out), `${humanCount(status.total_requests)} req`),
    tile(t('tileLatency'), humanMillis(status.latency_ms_avg), 'ewma'),
    tile(t('tileUptime'), humanDuration(status.uptime_sec), `v${status.version || ''}`),
    tile(t('tileBreakers'), String(status.breakers_open || 0),
         `${(status.providers || []).length} ${t('colProvider')}`),
    tile(t('tileEndpoint'), status.base_url || '—', status.config_path || '', 'long'),
  ].join('');

  const rows = [];
  for (const p of status.providers || []) {
    const h = health.get(p.provider) || {};
    const busy = state.probeBusy === p.provider;
    rows.push(`<tr>
      <td class="mono">${esc(p.provider)}</td>
      <td><span class="badge ${esc(h.state || 'unknown')}">${esc(h.state || 'unknown')}</span>${
        h.cooldown_remaining > 0 ? ` <span class="hint">${humanDuration(h.cooldown_remaining)}</span>` : ''}</td>
      <td class="num">${humanCount(p.requests)}</td>
      <td class="num">${pct(p.successes, p.requests)}</td>
      <td class="num">${p.latency_ms_avg ? humanMillis(p.latency_ms_avg) : '—'}</td>
      <td class="num">${humanCount((p.tokens_prompt || 0) + (p.tokens_completion || 0))}</td>
      <td class="msg hint">${esc((h.last_error || '').slice(0, 120))}</td>
      <td><button class="ghost" data-probe="${esc(p.provider)}" ${busy ? 'disabled' : ''}>${
        busy ? esc(t('testing')) : esc(t('test'))}</button></td>
    </tr>`);
  }
  $('#providers-body').innerHTML = rows.join('') || `<tr><td colspan="8" class="hint">${esc(t('empty'))}</td></tr>`;
  $('#breakers').textContent = status.breakers_open ? `${status.breakers_open} open` : '';

  for (const button of document.querySelectorAll('[data-probe]')) {
    button.addEventListener('click', () => probe(button.dataset.probe));
  }
}

async function loadModels() {
  try {
    const body = await api('/v1/models');
    state.models = body;
    const list = body.data || [];
    $('#models-hint').textContent = t('modelsHint').replace('{n}', list.length);
    $('#models').innerHTML = list.map((m) => {
      const info = m.literouter || {};
      const targets = (info.targets || []).join(', ');
      return `<span class="chip" title="${esc(targets)}">${esc(m.id)}${
        targets ? ` <span class="muted">→ ${esc(targets)}</span>` : ''}</span>`;
    }).join('') || `<span class="hint">${esc(t('empty'))}</span>`;
  } catch { /* the models route needs the key too; the overview stays useful */ }
}

async function probe(provider) {
  state.probeBusy = provider;
  renderOverview(state.status || {});
  try {
    const result = await api('/__literouter/probe', {
      method: 'POST', body: JSON.stringify({ provider }),
    });
    if (result.reachable) {
      toast(t('probeOk').replace('{ms}', humanMillis(result.latency_ms))
                        .replace('{n}', (result.models || []).length), 'ok');
    } else {
      toast(t('probeFail').replace('{detail}', result.detail || 'error'), 'err');
    }
  } catch (error) {
    toast(String(error.message || error), 'err');
  }
  state.probeBusy = '';
  renderOverview(state.status || {});
}

// ── logs ─────────────────────────────────────────────────────────────────────

function logMatches(entry) {
  if (state.logs.level && entry.level !== state.logs.level) return false;
  if (state.logs.kind && entry.kind !== state.logs.kind) return false;
  const q = state.logs.q.trim().toLowerCase();
  if (!q) return true;
  return [entry.message, entry.model, entry.provider, entry.request_id, entry.upstream_model]
    .some((field) => String(field || '').toLowerCase().includes(q));
}

function renderLogs() {
  const shown = state.logs.entries.filter(logMatches).slice(-800).reverse();
  $('#logs-body').innerHTML = shown.map((e) => `<tr>
    <td class="mono hint">${esc(e.time || '')}</td>
    <td><span class="badge ${esc(e.level || 'info')}">${esc(e.level || 'info')}</span></td>
    <td class="hint">${esc(e.kind || '')}</td>
    <td class="mono">${esc(e.model || '')}${e.stream ? '<span class="badge stream">sse</span>' : ''}${
      e.failover ? '<span class="badge failover">f/o</span>' : ''}</td>
    <td class="mono">${esc(e.provider || '')}</td>
    <td class="num"><span class="badge ${e.status >= 200 && e.status < 300 ? 'ok' : 'err'}">${e.status || '—'}</span></td>
    <td class="num">${e.latency_ms ? Math.round(e.latency_ms) : ''}</td>
    <td class="msg">${esc(e.message || '')}</td>
  </tr>`).join('') || `<tr><td colspan="8" class="hint">${esc(t('empty'))}</td></tr>`;
}

async function pullLogs() {
  if (state.logs.paused) return;
  try {
    // `since` advances only over entries actually received, so a truncated
    // answer leaves the rest for the next poll instead of skipping them.
    const body = await api(`/__literouter/logs?since=${state.logs.since}&limit=500`);
    const entries = body.entries || [];
    if (entries.length) {
      state.logs.entries = state.logs.entries.concat(entries).slice(-2000);
      state.logs.since = entries[entries.length - 1].seq;
      renderLogs();
    }
  } catch { /* keep the last good view */ }
}

// ── config ───────────────────────────────────────────────────────────────────

function renderConfig() {
  const body = state.config;
  if (!body) return;
  $('#config-path').textContent = body.path || '';
  const issues = (body.validation && body.validation.issues) || [];
  $('#config-issues').innerHTML = issues.map((issue) =>
    `<div class="issue ${esc(String(issue.level || '').toLowerCase())}">
       <span class="p">${esc(issue.path || '')}</span>${esc(issue.message || '')}</div>`).join('');
  $('#config-json').textContent = JSON.stringify(body.config || {}, null, 2);
}

async function loadConfig() {
  try {
    state.config = await api('/__literouter/config');
    renderConfig();
  } catch (error) {
    toast(String(error.message || error), 'err');
  }
}

// ── actions ──────────────────────────────────────────────────────────────────

async function reload() {
  try {
    const body = await api('/__literouter/reload', { method: 'POST' });
    toast(t('reloaded').replace('{summary}', body.summary || ''), 'ok');
    refresh();
    loadConfig();
  } catch (error) {
    const summary = (error.data && error.data.summary) || error.message;
    toast(t('reloadFailed').replace('{summary}', summary), 'err');
  }
}

async function resetStats() {
  try {
    await api('/__literouter/reset-stats', { method: 'POST' });
    toast(t('statsReset'), 'ok');
    refresh();
  } catch (error) { toast(String(error.message || error), 'err'); }
}

async function shutdown() {
  if (!window.confirm(t('confirmShutdown'))) return;
  try {
    await api('/__literouter/shutdown', { method: 'POST' });
    toast(t('shuttingDown'));
  } catch (error) { toast(String(error.message || error), 'err'); }
}

// ── key modal & chrome ───────────────────────────────────────────────────────

function openKeyModal() {
  $('#key-input').value = state.key;
  $('#key-modal').classList.remove('hidden');
  $('#key-input').focus();
}

function applyLanguage() {
  document.documentElement.lang = state.lang === 'zh' ? 'zh-CN' : 'en';
  for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  for (const el of document.querySelectorAll('[data-i18n-placeholder]')) el.placeholder = t(el.dataset.i18nPlaceholder);
  for (const el of document.querySelectorAll('[data-i18n-title]')) el.title = t(el.dataset.i18nTitle);
  $('#conn-dot').title = state.online ? t('connected') : t('disconnected');
  $('#log-pause').textContent = state.logs.paused ? t('resume') : t('pause');
  if (state.status) renderOverview(state.status);
  if (state.config) renderConfig();
}

function switchTab(tab) {
  state.tab = tab;
  for (const button of document.querySelectorAll('.tab')) {
    button.classList.toggle('active', button.dataset.tab === tab);
  }
  for (const view of document.querySelectorAll('.view')) {
    view.classList.toggle('active', view.id === 'view-' + tab);
  }
  if (tab === 'logs') { renderLogs(); pullLogs(); }
  if (tab === 'config' && !state.config) loadConfig();
}

function setOnline(online) {
  state.online = online;
  const dot = $('#conn-dot');
  dot.className = 'dot ' + (online ? 'on' : 'off');
  dot.title = online ? t('connected') : t('disconnected');
}

// ── polling ──────────────────────────────────────────────────────────────────

async function refresh() {
  try {
    state.status = await api('/__literouter/status');
    setOnline(true);
    renderOverview(state.status);
    if (state.tab === 'logs') await pullLogs();
  } catch (error) {
    setOnline(false);
  }
}

function wire() {
  for (const button of document.querySelectorAll('.tab')) {
    button.addEventListener('click', () => switchTab(button.dataset.tab));
  }
  $('#lang-btn').addEventListener('click', () => {
    state.lang = state.lang === 'zh' ? 'en' : 'zh';
    localStorage.setItem('lr.lang', state.lang);
    applyLanguage();
    if (state.status) renderOverview(state.status);
  });
  $('#key-btn').addEventListener('click', openKeyModal);
  $('#key-save').addEventListener('click', () => {
    state.key = $('#key-input').value.trim();
    localStorage.setItem('lr.key', state.key);
    $('#key-modal').classList.add('hidden');
    refresh();
  });
  $('#key-clear').addEventListener('click', () => {
    state.key = '';
    localStorage.removeItem('lr.key');
    $('#key-input').value = '';
    $('#key-modal').classList.add('hidden');
    refresh();
  });
  $('#reload-btn').addEventListener('click', reload);
  $('#reset-btn').addEventListener('click', resetStats);
  $('#shutdown-btn').addEventListener('click', shutdown);

  $('#log-search').addEventListener('input', (e) => { state.logs.q = e.target.value; renderLogs(); });
  $('#log-level').addEventListener('change', (e) => { state.logs.level = e.target.value; renderLogs(); });
  $('#log-kind').addEventListener('change', (e) => { state.logs.kind = e.target.value; renderLogs(); });
  $('#log-pause').addEventListener('click', (e) => {
    state.logs.paused = !state.logs.paused;
    e.target.textContent = state.logs.paused ? t('resume') : t('pause');
    if (!state.logs.paused) pullLogs();
  });
  $('#log-clear').addEventListener('click', () => { state.logs.entries = []; renderLogs(); });
  $('#config-copy').addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText($('#config-json').textContent);
      toast(t('copied'), 'ok');
    } catch { /* clipboard needs a secure context; the text is selectable anyway */ }
  });
}

wire();
applyLanguage();
refresh();
loadModels();
setInterval(() => { refresh(); if (state.tab === 'logs') pullLogs(); }, 2000);
