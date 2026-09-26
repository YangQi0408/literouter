import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react'
import { toast } from 'sonner'

import {
  api,
  ApiError,
  getApiKey,
  setApiKey,
  type AppConfig,
  type ConfigResponse,
  type LogEntry,
  type ModelInfo,
  type ProviderProbe,
  type Snapshot,
  type ValidationIssue,
  type ValidationReport,
} from '@/lib/api'
import { shouldReplaceDraft, settleSavedDraft } from '@/lib/draft'
import { useI18n } from '@/lib/i18n'
import { appendLogEntries, probeSucceeded } from '@/lib/live-state'
import { administratorKeyAfterSave } from '@/lib/secrets'

/** Everything the console knows lives here: telemetry polled from the server,
 *  the config as the server holds it, the config as the operator is editing it,
 *  and the request log's incremental cursor. Components read it and call the
 *  actions; nothing else talks to the API directly. */

interface StoreValue {
  // telemetry
  snapshot: Snapshot | null
  online: boolean
  models: ModelInfo[]

  // configuration
  loaded: ConfigResponse | null
  working: AppConfig | null
  dirty: boolean
  saving: boolean
  saveIssues: ValidationIssue[]
  serverReport: ValidationReport | null
  consoleOff: boolean

  // request log
  logs: LogEntry[]
  logsPaused: boolean
  setLogsActive: (active: boolean) => void
  setLogsPaused: (paused: boolean) => void
  clearLogs: () => void

  // credentials
  keyPrompt: boolean
  openKeyPrompt: () => void
  closeKeyPrompt: () => void

  // actions
  refresh: () => Promise<void>
  loadConfig: () => Promise<void>
  update: (mutate: (draft: AppConfig) => void) => void
  save: () => Promise<void>
  discard: () => void
  reload: () => Promise<void>
  resetStats: () => Promise<void>
  shutdown: () => Promise<void>
  probe: (provider: string) => Promise<ProviderProbe>
}

const StoreContext = createContext<StoreValue | null>(null)

const POLL_MS = 2000
const LOG_LIMIT = 1500

export function StoreProvider({ children }: { children: ReactNode }) {
  const { t } = useI18n()
  const translateRef = useRef(t)
  translateRef.current = t

  const [snapshot, setSnapshot] = useState<Snapshot | null>(null)
  const [online, setOnline] = useState(false)
  const [models, setModels] = useState<ModelInfo[]>([])

  const [loaded, setLoaded] = useState<ConfigResponse | null>(null)
  const [working, setWorking] = useState<AppConfig | null>(null)
  const [saving, setSaving] = useState(false)
  const [saveIssues, setSaveIssues] = useState<ValidationIssue[]>([])
  const [consoleOff, setConsoleOff] = useState(false)

  const [logs, setLogs] = useState<LogEntry[]>([])
  const [logsPaused, setLogsPaused] = useState(false)
  const [logsActive, setLogsActive] = useState(false)

  const [keyPrompt, setKeyPrompt] = useState(false)

  const logsSince = useRef(0)
  const logGeneration = useRef(0)
  const logsInFlight = useRef(false)
  const statusInFlight = useRef<Promise<void> | null>(null)
  const statusKey = useRef('')
  const configGeneration = useRef(0)
  const configInFlight = useRef<number | null>(null)
  const configKey = useRef('')
  const savingRef = useRef(false)
  const pendingSavedDraft = useRef<AppConfig | null>(null)
  const snapshotRef = useRef<Snapshot | null>(null)
  const pausedRef = useRef(false)
  pausedRef.current = logsPaused

  // What the server last handed us, readable from a callback without making
  // that callback depend on it — `loadConfig` compares against this to tell an
  // edited draft from an untouched one.
  const loadedRef = useRef<ConfigResponse | null>(null)
  loadedRef.current = loaded

  // Same trick for the log tab: the poll timer reads this instead of listing
  // `logsActive` as a dependency, so switching tabs no longer tears the timer
  // down and re-runs the whole effect.
  const logsActiveRef = useRef(false)
  logsActiveRef.current = logsActive

  const dirty = useMemo(() => {
    if (!working || !loaded) return false
    return JSON.stringify(working) !== JSON.stringify(loaded.config)
  }, [working, loaded])

  const handleError = useCallback(
    (error: unknown, notify = false) => {
      if (error instanceof DOMException && error.name === 'AbortError') return
      if (error instanceof ApiError && (error.status === 401 || error.status === 403)) {
        setOnline(false)
        setKeyPrompt(true)
      }
      if (notify) toast.error(translateRef.current('operationFailed', {
        detail: error instanceof Error ? error.message : String(error),
      }), { id: 'gateway-operation-error' })
    },
    [],
  )

  const pollStatus = useCallback(async (notify = false) => {
    const key = getApiKey()
    if (statusInFlight.current && statusKey.current === key) return statusInFlight.current
    statusKey.current = key
    const request = (async () => {
      try {
        const next = await api.status()
        const previous = snapshotRef.current
        if ((previous && previous.started_unix !== next.started_unix) || next.log_seq < logsSince.current) {
          logGeneration.current += 1
          logsSince.current = 0
          setLogs([])
        }
        snapshotRef.current = next
        setSnapshot(next)
        setOnline(true)
      } catch (error) {
        if (!(error instanceof DOMException && error.name === 'AbortError')) setOnline(false)
        handleError(error, notify)
      }
    })()
    statusInFlight.current = request
    try { await request } finally {
      if (statusInFlight.current === request) statusInFlight.current = null
    }
  }, [handleError])

  const loadModels = useCallback(async (notify = false) => {
    try {
      const body = await api.models()
      setModels(body.data ?? [])
    } catch (error) {
      handleError(error, notify)
    }
  }, [handleError])

  // `force` replaces the operator's draft with what the server holds. Only the
  // actions that explicitly discard the draft pass it, such as Reload. A
  // plain re-read (first paint, refresh, or entering a key)
  // leaves an edited draft alone: overwriting it would discard typing with no
  // undo and no warning.
  const loadConfig = useCallback(
    async (force = false, notify = false) => {
      if (savingRef.current) return
      const key = getApiKey()
      if (configInFlight.current !== null && configKey.current === key && !force) return
      const generation = ++configGeneration.current
      configInFlight.current = generation
      configKey.current = key
      try {
        const body = await api.config()
        if (generation !== configGeneration.current) return
        const previous = loadedRef.current
        const submitted = pendingSavedDraft.current
        pendingSavedDraft.current = null
        loadedRef.current = body
        setLoaded(body)
        setConsoleOff(body.config.server.web_ui === false)
        setWorking((prev) =>
          submitted ? settleSavedDraft(prev, submitted, body.config)
            : shouldReplaceDraft(prev, previous?.config ?? null, force)
            ? structuredClone(body.config)
            : prev,
        )
        if (force) setSaveIssues([])
      } catch (error) {
        handleError(error, notify)
      } finally {
        if (configInFlight.current === generation) configInFlight.current = null
      }
    },
    [handleError],
  )

  const pullLogs = useCallback(async (notify = false) => {
    if (pausedRef.current || logsInFlight.current) return
    logsInFlight.current = true
    const generation = logGeneration.current
    try {
      const body = await api.logs(logsSince.current)
      if (generation !== logGeneration.current || pausedRef.current) return
      if (body.seq < logsSince.current) {
        logsSince.current = 0
        logGeneration.current += 1
        setLogs([])
        return
      }
      if (body.entries.length) {
        // Advance only over entries actually received, so a truncated answer
        // leaves the rest for the next poll instead of skipping them.
        logsSince.current = body.entries[body.entries.length - 1]!.seq
        setLogs((prev) => appendLogEntries(prev, body.entries, LOG_LIMIT))
      }
    } catch (error) {
      handleError(error, notify)
    } finally {
      logsInFlight.current = false
    }
  }, [handleError])

  const refresh = useCallback(async () => {
    await Promise.all([pollStatus(true), loadConfig(false, true), loadModels(true)])
    if (logsActiveRef.current) await pullLogs(true)
  }, [pollStatus, loadConfig, loadModels, pullLogs])

  const update = useCallback((mutate: (draft: AppConfig) => void) => {
    setWorking((prev) => {
      if (!prev) return prev
      const draft = structuredClone(prev)
      mutate(draft)
      return draft
    })
  }, [])

  const discard = useCallback(() => {
    if (!loaded) return
    setWorking(structuredClone(loaded.config))
    setSaveIssues([])
  }, [loaded])

  const save = useCallback(async () => {
    if (!working || savingRef.current) return
    const submitted = structuredClone(working)
    let savedThisAttempt = false
    savingRef.current = true
    configGeneration.current += 1
    setSaving(true)
    setSaveIssues([])
    try {
      const report = await api.saveConfig(submitted)
      savedThisAttempt = true
      pendingSavedDraft.current = submitted
      toast.success(report.saved ? t('saved') : t('savedInMemory'))
      setConsoleOff(submitted.server.web_ui === false)
      const nextKey = administratorKeyAfterSave(loadedRef.current?.config.server ?? submitted.server, submitted.server)
      if (nextKey !== null) setApiKey(nextKey)
      const canonical = await api.config()
      pendingSavedDraft.current = null
      loadedRef.current = canonical
      setLoaded(canonical)
      setWorking((current) => settleSavedDraft(current, submitted, canonical.config))
      await Promise.all([pollStatus(), loadModels()])
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') return
      if (error instanceof ApiError && error.status === 422 && error.data) {
        const body = error.data as ValidationReport
        setSaveIssues(body.issues ?? [])
        toast.error(t('saveRefused'))
      } else {
        handleError(error)
        if (!(error instanceof ApiError && (error.status === 401 || error.status === 403))) {
          toast.error(t(savedThisAttempt ? 'saveRefreshFailed' : 'saveFailed', { detail: error instanceof Error ? error.message : String(error) }))
        }
      }
    } finally {
      savingRef.current = false
      setSaving(false)
    }
  }, [working, t, pollStatus, loadModels, handleError])

  const reload = useCallback(async () => {
    try {
      const report = await api.reload()
      await loadConfig(true)
      await Promise.all([pollStatus(), loadModels()])
      toast.success(t('reloaded', { summary: report.summary }))
      if (!report.ok) setSaveIssues(report.issues ?? [])
    } catch (error) {
      if (error instanceof ApiError && error.status === 422 && error.data) {
        const body = error.data as ValidationReport
        toast.error(t('reloadFailed', { summary: body.summary ?? '' }))
        setSaveIssues(body.issues ?? [])
        await loadConfig()
      } else {
        handleError(error, true)
      }
    }
  }, [t, loadConfig, pollStatus, loadModels, handleError])

  const resetStats = useCallback(async () => {
    try {
      await api.resetStats()
      toast.success(t('statsReset'))
      await pollStatus()
    } catch (error) {
      handleError(error, true)
    }
  }, [t, pollStatus, handleError])

  const shutdown = useCallback(async () => {
    try {
      await api.shutdown()
      toast(t('shuttingDown'))
      setOnline(false)
    } catch (error) {
      handleError(error, true)
    }
  }, [t, handleError])

  const probe = useCallback(
    async (provider: string) => {
      try {
        const result = await api.probe(provider)
        if (probeSucceeded(result)) {
          toast.success(
            t('probeOk', { ms: `${result.latency_ms.toFixed(1)}ms`, n: result.models.length }),
          )
        } else {
          toast.error(t('probeFail', { detail: result.detail || `status ${result.status}` }))
        }
        return result
      } catch (error) {
        handleError(error, true)
        throw error
      }
    },
    [t, handleError],
  )

  const clearLogs = useCallback(() => {
    logGeneration.current += 1
    logsSince.current = Math.max(logsSince.current, snapshotRef.current?.log_seq ?? 0)
    setLogs([])
  }, [])

  const openKeyPrompt = useCallback(() => setKeyPrompt(true), [])
  const closeKeyPrompt = useCallback(() => setKeyPrompt(false), [])

  // One timer drives both polls; the log poll only runs while its view is open,
  // read through a ref. Listing `logsActive` as a dependency instead would tear
  // this effect down and re-run it on every switch into or out of the log tab —
  // and the initial `loadConfig()` below would land on top of whatever the
  // operator had typed.
  useEffect(() => {
    void pollStatus()
    void loadConfig(false, true)
    void loadModels()
    let ticks = 0
    const timer = window.setInterval(() => {
      void pollStatus()
      if (logsActiveRef.current) void pullLogs()
      if (++ticks % 5 === 0) {
        void loadConfig()
        void loadModels()
      }
    }, POLL_MS)
    return () => window.clearInterval(timer)
  }, [pollStatus, loadConfig, loadModels, pullLogs])

  // Entering the log page or resuming it should fetch immediately, rather than
  // wait for the next two-second polling tick.
  useEffect(() => {
    if (logsActive && !logsPaused) void pullLogs()
  }, [logsActive, logsPaused, pullLogs])

  const value: StoreValue = {
    snapshot,
    online,
    models,
    loaded,
    working,
    dirty,
    saving,
    saveIssues,
    serverReport: loaded?.validation ?? null,
    consoleOff,
    logs,
    logsPaused,
    setLogsActive,
    setLogsPaused,
    clearLogs,
    keyPrompt,
    openKeyPrompt,
    closeKeyPrompt,
    refresh,
    loadConfig,
    update,
    save,
    discard,
    reload,
    resetStats,
    shutdown,
    probe,
  }

  return <StoreContext.Provider value={value}>{children}</StoreContext.Provider>
}

export function useStore(): StoreValue {
  const value = useContext(StoreContext)
  if (!value) throw new Error('useStore must be used inside StoreProvider')
  return value
}

/** Applies a key entered in the dialog and re-reads everything. */
export function useApiKey() {
  const { refresh, closeKeyPrompt } = useStore()
  const save = useCallback(
    async (key: string) => {
      setApiKey(key)
      closeKeyPrompt()
      await refresh()
    },
    [refresh, closeKeyPrompt],
  )
  const clear = useCallback(async () => {
    setApiKey('')
    closeKeyPrompt()
    await refresh()
  }, [refresh, closeKeyPrompt])
  return { save, clear }
}
