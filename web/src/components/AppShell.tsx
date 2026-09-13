import {
  ChartLine,
  Globe,
  KeyRound,
  Moon,
  Power,
  RefreshCw,
  Route as RouteIcon,
  Server,
  SlidersHorizontal,
  SquareTerminal,
  Sun,
  TriangleAlert,
  Zap,
} from 'lucide-react'
import type { ReactNode } from 'react'

import { Issues } from '@/components/Issues'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'
import { useI18n } from '@/lib/i18n'
import { useTheme } from '@/lib/theme'
import { countChanges } from '@/lib/diff'
import { useStore } from '@/store'

export type Tab = 'overview' | 'providers' | 'routes' | 'logs' | 'settings'

const NAV: { tab: Tab; icon: typeof ChartLine; label: string; sub: string }[] = [
  { tab: 'overview', icon: ChartLine, label: 'tabOverview', sub: 'subOverview' },
  { tab: 'providers', icon: Server, label: 'tabProviders', sub: 'subProviders' },
  { tab: 'routes', icon: RouteIcon, label: 'tabRoutes', sub: 'subRoutes' },
  { tab: 'logs', icon: SquareTerminal, label: 'tabLogs', sub: 'subLogs' },
  { tab: 'settings', icon: SlidersHorizontal, label: 'tabSettings', sub: 'subSettings' },
]

export function AppShell({
  tab,
  onTab,
  children,
}: {
  tab: Tab
  onTab: (tab: Tab) => void
  children: ReactNode
}) {
  const {
    snapshot,
    online,
    loaded,
    working,
    dirty,
    saving,
    saveIssues,
    save,
    discard,
    reload,
    resetStats,
    shutdown,
    consoleOff,
    update,
    openKeyPrompt,
  } = useStore()
  const { t, lang, setLang } = useI18n()
  const { theme, toggle } = useTheme()

  const active = NAV.find((item) => item.tab === tab) ?? NAV[0]!
  const changes = working && loaded ? countChanges(working, loaded.config) : 0

  return (
    <div className="flex min-h-screen flex-col lg:flex-row">
      {/* ── sidebar ──────────────────────────────────────────────────────── */}
      <aside className="flex items-center gap-3 border-b bg-sidebar px-3 py-2.5 lg:sticky lg:top-0 lg:h-screen lg:w-56 lg:flex-col lg:items-stretch lg:gap-0 lg:border-r lg:border-b-0 lg:px-2.5 lg:py-4">
        <div className="flex items-center gap-2.5 lg:px-2 lg:pb-4">
          <svg viewBox="0 0 16 16" className="size-5 text-primary" aria-hidden>
            <path d="M8 1.5 14.5 8 8 14.5 1.5 8Z" fill="none" stroke="currentColor" strokeWidth="1.6" />
            <circle cx="8" cy="8" r="2" fill="currentColor" />
          </svg>
          <div className="flex flex-col leading-tight">
            <strong className="text-[15px] tracking-tight">literouter</strong>
            <span className="font-mono text-[11px] text-muted-foreground">
              {snapshot?.version ?? '—'}
            </span>
          </div>
        </div>

        <nav className="flex flex-1 items-center gap-1 overflow-x-auto lg:flex-none lg:flex-col lg:items-stretch">
          {NAV.map((item) => {
            const Icon = item.icon
            const isActive = item.tab === tab
            return (
              <button
                key={item.tab}
                type="button"
                onClick={() => onTab(item.tab)}
                className={cn(
                  'flex shrink-0 items-center gap-2.5 rounded-lg px-3 py-2 text-[13.5px] whitespace-nowrap transition-colors',
                  isActive
                    ? 'bg-primary/10 text-foreground'
                    : 'text-muted-foreground hover:bg-muted/60 hover:text-foreground',
                )}
              >
                <Icon className={cn('size-4', isActive && 'text-primary')} />
                {t(item.label)}
              </button>
            )
          })}
        </nav>

        <div className="flex items-center gap-1 lg:mt-auto lg:border-t lg:pt-3">
          <Button size="icon" variant="ghost" title={t('themeTitle')} onClick={toggle}>
            {theme === 'dark' ? <Moon className="size-4" /> : <Sun className="size-4" />}
          </Button>
          <Button
            size="icon"
            variant="ghost"
            title="Language"
            onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')}
          >
            <Globe className="size-4" />
          </Button>
          <Button size="icon" variant="ghost" title={t('keyTitle')} onClick={openKeyPrompt}>
            <KeyRound className="size-4" />
          </Button>
        </div>
      </aside>

      {/* ── content ──────────────────────────────────────────────────────── */}
      <div className="flex min-w-0 flex-1 flex-col">
        <header className="sticky top-0 z-10 flex flex-wrap items-center gap-3 border-b bg-background/85 px-5 py-3.5 backdrop-blur">
          <div className="min-w-0">
            <h1 className="text-lg font-semibold tracking-tight">{t(active.label)}</h1>
            <p className="text-xs text-muted-foreground">{t(active.sub)}</p>
          </div>
          <div className="ml-auto flex flex-wrap items-center gap-2">
            <span className="mr-1 hidden items-center gap-2 text-xs text-muted-foreground sm:flex">
              <span
                className={cn(
                  'size-2 rounded-full',
                  online ? 'bg-ok shadow-[0_0_0_4px] shadow-ok/20' : 'bg-destructive',
                )}
              />
              {online ? t('connected') : t('disconnected')}
            </span>
            <Button variant="ghost" size="sm" onClick={() => void reload()}>
              <RefreshCw className="size-3.5" />
              {t('reload')}
            </Button>
            <Button variant="ghost" size="sm" onClick={() => void resetStats()}>
              <Zap className="size-3.5" />
              {t('resetStats')}
            </Button>
            <Button
              variant="ghost"
              size="sm"
              className="text-destructive hover:bg-destructive/10"
              onClick={() => {
                if (window.confirm(t('confirmShutdown'))) void shutdown()
              }}
            >
              <Power className="size-3.5" />
              {t('shutdown')}
            </Button>
          </div>
        </header>

        {consoleOff ? (
          <div className="flex flex-wrap items-center gap-3 border-b border-warn/35 bg-warn/10 px-5 py-2.5 text-xs text-warn">
            <TriangleAlert className="size-4" />
            {t('consoleDisabled')}
            <Button
              size="sm"
              variant="ghost"
              className="ml-auto text-warn hover:bg-warn/15"
              onClick={() =>
                update((draft) => {
                  draft.server.web_ui = true
                })
              }
            >
              {t('hintWebUi')}
            </Button>
          </div>
        ) : null}

        <main className="mx-auto w-full max-w-[1400px] flex-1 p-5 pb-28">{children}</main>
      </div>

      {/* ── unsaved changes ──────────────────────────────────────────────── */}
      {dirty ? (
        <div className="glass fixed inset-x-3 bottom-3 z-20 flex flex-col gap-3 rounded-xl border border-border/80 p-3.5 shadow-lg lg:inset-x-auto lg:left-1/2 lg:w-[min(980px,calc(100vw-2rem))] lg:-translate-x-1/2">
          <div className="flex flex-wrap items-center gap-3">
            <TriangleAlert className="size-4 text-warn" />
            <span className="text-sm">{t('unsaved', { n: changes })}</span>
            <div className="ml-auto flex gap-2">
              <Button variant="ghost" size="sm" onClick={discard} disabled={saving}>
                {t('discard')}
              </Button>
              <Button size="sm" onClick={() => void save()} disabled={saving}>
                {t('saveChanges')}
              </Button>
            </div>
          </div>
          {saveIssues.length ? <Issues issues={saveIssues} /> : null}
        </div>
      ) : null}
    </div>
  )
}
