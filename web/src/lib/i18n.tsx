import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from 'react'

export type Lang = 'en' | 'zh'

type Vars = Record<string, string | number>

const en: Record<string, string> = {
  // shell
  tabOverview: 'Overview',
  tabProviders: 'Providers',
  tabRoutes: 'Routes',
  tabLogs: 'Logs',
  tabSettings: 'Settings',
  subOverview: 'Live traffic, relay health and recent activity',
  subProviders: 'Upstream relays, credentials and reachability',
  subRoutes: 'Model mapping and the ordered failover chain',
  subLogs: 'Every request this proxy has handled',
  subSettings: 'Listener, console, circuit breaker and logging',
  reload: 'Reload',
  resetStats: 'Reset',
  shutdown: 'Shutdown',
  themeTitle: 'Switch theme',
  keyTitle: 'API key',
  keyHint:
    'The server answered 401. Paste the key from server.api_key; it stays in this browser only.',
  keyClear: 'Clear key',
  connected: 'live',
  disconnected: 'no connection',

  // metrics
  metricRequests: 'Requests',
  metricSuccess: 'Success rate',
  metricTokens: 'Tokens',
  metricSent: 'Sent',
  metricLatency: 'Avg latency',
  metricUptime: 'Uptime',
  metricBreakers: 'Open breakers',
  metricEndpoint: 'Endpoint',
  inFlight: 'in flight',
  prompt: 'prompt',
  completion: 'completion',
  relays: 'relays',

  // tables
  relayHealth: 'Relay health',
  models: 'Models',
  routed: '{n} routed',
  lastError: 'Last error',
  empty: 'nothing to show',
  test: 'Test',
  testing: 'Testing…',
  colProvider: 'Provider',
  colState: 'State',
  colRequests: 'Requests',
  colSuccess: 'Success',
  colLatency: 'Latency',
  colTokens: 'Tokens',
  colBaseUrl: 'Base URL',
  colKey: 'API key',
  colEnabled: 'Enabled',
  colTargets: 'Targets',
  colTime: 'Time',
  colLevel: 'Level',
  colKind: 'Kind',
  colModel: 'Model',
  colStatus: 'Status',
  colMessage: 'Message',

  // providers / routes
  add: 'Add',
  edit: 'Edit',
  remove: 'Remove',
  cancel: 'Cancel',
  apply: 'Apply',
  newProvider: 'New relay',
  editProvider: 'Edit relay',
  newRoute: 'New route',
  editRoute: 'Edit route',
  confirmDeleteProvider: 'Delete relay `{id}`?',
  confirmDeleteRoute: 'Delete route `{model}`?',
  keyStored: 'stored (unchanged)',
  keyEnv: 'from {name}',
  keyNone: 'not set',
  keyClearAction: 'Clear stored key',
  providerHint: 'Relays are tried in priority order; lower wins.',
  routeHint: 'Targets are tried in the listed order — that order is the failover chain.',
  targetProvider: 'relay',
  targetModel: 'upstream model (optional)',
  addTarget: 'Add target',
  moveUp: 'Move up',
  moveDown: 'Move down',
  model: 'Model',

  // logs
  search: 'Search…',
  allLevels: 'all levels',
  allKinds: 'all kinds',
  pause: 'Pause',
  resume: 'Resume',
  clear: 'Clear',

  // settings / saving
  discard: 'Discard',
  saveChanges: 'Save changes',
  unsaved: '{n} unsaved change(s)',
  consoleDisabled: 'The web console was just switched off; re-enable server.web_ui to come back.',
  saved: 'config saved',
  savedInMemory: 'config applied (this server has no config file to write)',
  saveRefused: 'the server refused the config',
  reloaded: 'config reloaded · {summary}',
  reloadFailed: 'reload failed · {summary}',
  statsReset: 'counters reset',
  shuttingDown: 'shutting down…',
  confirmShutdown: 'Stop the proxy server?',
  probeOk: 'reachable · {ms} · {n} models',
  probeFail: 'unreachable: {detail}',
  copy: 'Copy',
  save: 'Save',
  copied: 'Copied to clipboard',
  configNote: 'Read-only view of what the server holds. Literal secrets are blanked.',
  copiedKey: 'copied',

  // field hints (labels are the config keys themselves)
  hintId: 'stable slug used by routes',
  hintBaseUrl: 'https://api.example.com/v1',
  hintApiKey: 'a literal, or ${ENV_VAR}; leave as stored to keep it',
  hintModels: 'one model id per line',
  hintHeaders: 'one Name: value per line',
  hintWebUi: 'serve this console at /ui',
  hintLogBodies: 'store request/response bodies in the log',
}

const zh: Record<string, string> = {
  tabOverview: '概览',
  tabProviders: '中转站',
  tabRoutes: '路由',
  tabLogs: '日志',
  tabSettings: '设置',
  subOverview: '实时流量、中转站健康与最近活动',
  subProviders: '上游中转站、凭据与可达性',
  subRoutes: '模型映射与故障转移顺序',
  subLogs: '本代理处理过的全部请求',
  subSettings: '监听、控制台、熔断与日志',
  reload: '重载配置',
  resetStats: '重置统计',
  shutdown: '关闭服务',
  themeTitle: '切换主题',
  keyTitle: 'API 密钥',
  keyHint: '服务端返回 401。请填入 server.api_key；它只保存在本浏览器。',
  keyClear: '清除密钥',
  connected: '已连接',
  disconnected: '未连接',

  metricRequests: '请求总数',
  metricSuccess: '成功率',
  metricTokens: 'Token 总数',
  metricSent: '下发流量',
  metricLatency: '平均延迟',
  metricUptime: '运行时长',
  metricBreakers: '熔断器',
  metricEndpoint: '接入点',
  inFlight: '进行中',
  prompt: 'prompt',
  completion: 'completion',
  relays: '个中转站',

  relayHealth: '中转站健康',
  models: '模型',
  routed: '{n} 条路由',
  lastError: '最近错误',
  empty: '暂无数据',
  test: '探测',
  testing: '探测中…',
  colProvider: '中转站',
  colState: '状态',
  colRequests: '请求数',
  colSuccess: '成功率',
  colLatency: '延迟',
  colTokens: 'Token',
  colBaseUrl: 'Base URL',
  colKey: 'API 密钥',
  colEnabled: '启用',
  colTargets: '候选链',
  colTime: '时间',
  colLevel: '级别',
  colKind: '类型',
  colModel: '模型',
  colStatus: '状态',
  colMessage: '消息',

  add: '新增',
  edit: '编辑',
  remove: '移除',
  cancel: '取消',
  apply: '应用',
  newProvider: '新增中转站',
  editProvider: '编辑中转站',
  newRoute: '新增路由',
  editRoute: '编辑路由',
  confirmDeleteProvider: '确定删除中转站 `{id}`？',
  confirmDeleteRoute: '确定删除路由 `{model}`？',
  keyStored: '已保存（未修改）',
  keyEnv: '来自 {name}',
  keyNone: '未设置',
  keyClearAction: '清除已保存的密钥',
  providerHint: '按 priority 从小到大依次尝试。',
  routeHint: '候选按列表顺序尝试，这个顺序就是故障转移链。',
  targetProvider: '中转站',
  targetModel: '上游模型（可选）',
  addTarget: '添加候选',
  moveUp: '上移',
  moveDown: '下移',
  model: '模型',

  search: '搜索…',
  allLevels: '全部级别',
  allKinds: '全部类型',
  pause: '暂停',
  resume: '继续',
  clear: '清空',

  discard: '放弃修改',
  saveChanges: '保存修改',
  unsaved: '{n} 处未保存的修改',
  consoleDisabled: 'Web 控制台刚刚被关闭；重新打开 server.web_ui 才能回来。',
  saved: '配置已保存',
  savedInMemory: '配置已生效（该服务没有可写入的配置文件）',
  saveRefused: '服务端拒绝了该配置',
  reloaded: '配置已重载 · {summary}',
  reloadFailed: '重载失败 · {summary}',
  statsReset: '统计已重置',
  shuttingDown: '正在关闭…',
  confirmShutdown: '确定要关闭代理服务吗？',
  probeOk: '可达 · {ms} · {n} 个模型',
  probeFail: '不可达：{detail}',
  copy: '复制',
  save: '保存',
  copied: '已复制到剪贴板',
  configNote: '服务端当前配置的只读视图；明文密钥会被打码。',
  copiedKey: '已复制',

  hintId: '路由引用的稳定标识',
  hintBaseUrl: 'https://api.example.com/v1',
  hintApiKey: '明文或 ${ENV_VAR}；保持原样即不修改',
  hintModels: '每行一个模型 id',
  hintHeaders: '每行一个 Name: value',
  hintWebUi: '在 /ui 提供本控制台',
  hintLogBodies: '在日志中保存请求/响应体',
}

const dictionaries: Record<Lang, Record<string, string>> = { en, zh }

interface I18nValue {
  lang: Lang
  setLang: (lang: Lang) => void
  t: (key: string, vars?: Vars) => string
}

const I18nContext = createContext<I18nValue | null>(null)

function detectLang(): Lang {
  const saved = localStorage.getItem('lr.lang')
  if (saved === 'en' || saved === 'zh') return saved
  return (navigator.language || '').toLowerCase().startsWith('zh') ? 'zh' : 'en'
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(detectLang)

  const setLang = useCallback((next: Lang) => {
    setLangState(next)
    localStorage.setItem('lr.lang', next)
    document.documentElement.lang = next === 'zh' ? 'zh-CN' : 'en'
  }, [])

  const t = useCallback(
    (key: string, vars?: Vars) => {
      const template = dictionaries[lang][key] ?? dictionaries.en[key] ?? key
      if (!vars) return template
      return template.replace(/\{(\w+)\}/g, (_, name: string) => String(vars[name] ?? `{${name}}`))
    },
    [lang],
  )

  const value = useMemo(() => ({ lang, setLang, t }), [lang, setLang, t])
  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>
}

export function useI18n(): I18nValue {
  const value = useContext(I18nContext)
  if (!value) throw new Error('useI18n must be used inside I18nProvider')
  return value
}
