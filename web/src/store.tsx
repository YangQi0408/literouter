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
import { administratorKeyAfterSave } from '@/lib/clients'
import { useI18n } from '@/lib/i18n'

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
    (error: unknown) => {
      setOnline(false)
      if (error instanceof ApiError && (error.status === 401 || error.status === 403)) {
        setKeyPrompt(true)
        return
      }
    },
    [],
  )

  const refresh = useCallback(async () => {
    try {
      const next = await api.status()
      setSnapshot(next)
      setOnline(true)
    } catch (error) {
      handleError(error)
    }
  }, [handleError])

  const loadModels = useCallback(async () => {
    try {
      const body = await api.models()
      setModels(body.data ?? [])
    } catch {
      /* the model list is decoration; the overview stays useful without it */
    }
  }, [])

  // `force` replaces the operator's draft with what the server holds. Only the
  // actions that mean "the draft is settled" pass it — save, reload, entering a
  // key. A plain re-read (the first paint, or an effect that happened to re-run)
  // leaves an edited draft alone: overwriting it would discard typing with no
  // undo and no warning.
  const loadConfig = useCallback(
    async (force = false) => {
      try {
        const body = await api.config()
        setLoaded(body)
        setWorking((prev) =>
          shouldReplaceDraft(prev, loadedRef.current?.config ?? null, force)
            ? structuredClone(body.config)
            : prev,
        )
        if (force) setSaveIssues([])
      } catch (error) {
        handleError(error)
      }
    },
    [handleError],
  )

  const pullLogs = useCallback(async () => {
    if (pausedRef.current) return
    try {
      const body = await api.logs(logsSince.current)
      if (body.entries.length) {
        // Advance only over entries actually received, so a truncated answer
        // leaves the rest for the next poll instead of skipping them.
        logsSince.current = body.entries[body.entries.length - 1]!.seq
        setLogs((prev) => [...prev, ...body.entries].slice(-LOG_LIMIT))
      }
    } catch (error) {
      handleError(error)
    }
  }, [handleError])

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
    if (!working) return
    setSaving(true)
    setSaveIssues([])
    try {
      const report = await api.saveConfig(working)
      toast.success(report.saved ? t('saved') : t('savedInMemory'))
      setConsoleOff(working.server.web_ui === false)
      const nextKey = administratorKeyAfterSave(loadedRef.current?.config.server ?? working.server, working.server)
      if (nextKey !== null) setApiKey(nextKey)
      const canonical = await api.config()
      setLoaded(canonical)
      setWorking((current) => settleSavedDraft(current, working, canonical.config))
      await refresh()
    } catch (error) {
      if (error instanceof ApiError && error.status === 422 && error.data) {
        const body = error.data as ValidationReport
        setSaveIssues(body.issues ?? [])
        toast.error(t('saveRefused'))
      } else {
        handleError(error)
        if (!(error instanceof ApiError && (error.status === 401 || error.status === 403))) {
          toast.error(t('saveFailed', { detail: error instanceof Error ? error.message : String(error) }))
        }
      }
    } finally {
      setSaving(false)
    }
  }, [working, t, loadConfig, refresh, handleError])

  const reload = useCallback(async () => {
    try {
      const report = await api.reload()
      await loadConfig(true)
      await refresh()
      toast.success(t('reloaded', { summary: report.summary }))
      if (!report.ok) setSaveIssues(report.issues ?? [])
    } catch (error) {
      if (error instanceof ApiError && error.status === 422 && error.data) {
        const body = error.data as ValidationReport
        toast.error(t('reloadFailed', { summary: body.summary ?? '' }))
        setSaveIssues(body.issues ?? [])
        await loadConfig()
      } else {
        handleError(error)
      }
    }
  }, [t, loadConfig, refresh, handleError])

  const resetStats = useCallback(async () => {
    try {
      await api.resetStats()
      toast.success(t('statsReset'))
      await refresh()
    } catch (error) {
      handleError(error)
    }
  }, [t, refresh, handleError])

  const shutdown = useCallback(async () => {
    try {
      await api.shutdown()
      toast(t('shuttingDown'))
    } catch (error) {
      handleError(error)
    }
  }, [t, handleError])

  const probe = useCallback(
    async (provider: string) => {
      try {
        const result = await api.probe(provider)
        if (result.reachable) {
          toast.success(
            t('probeOk', { ms: `${result.latency_ms.toFixed(1)}ms`, n: result.models.length }),
          )
        } else {
          toast.error(t('probeFail', { detail: result.detail || `status ${result.status}` }))
        }
        return result
      } catch (error) {
        handleError(error)
        throw error
      }
    },
    [t, handleError],
  )

  const clearLogs = useCallback(() => {
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
    void refresh()
    void loadConfig()
    void loadModels()
    const timer = window.setInterval(() => {
      void refresh()
      if (logsActiveRef.current) void pullLogs()
    }, POLL_MS)
    return () => window.clearInterval(timer)
  }, [refresh, loadConfig, loadModels, pullLogs])

  // The model list changes when a route or a relay's model list does.
  useEffect(() => {
    if (!dirty) void loadModels()
  }, [snapshot?.total_requests, dirty, loadModels])

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
  const { refresh, loadConfig, closeKeyPrompt } = useStore()
  const save = useCallback(
    async (key: string) => {
      setApiKey(key)
      closeKeyPrompt()
      await refresh()
      await loadConfig()
    },
    [refresh, loadConfig, closeKeyPrompt],
  )
  const clear = useCallback(async () => {
    setApiKey('')
    closeKeyPrompt()
    await refresh()
  }, [refresh, closeKeyPrompt])
  return { save, clear }
}
