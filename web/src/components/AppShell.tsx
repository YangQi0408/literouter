import {
  Activity, ArrowUpRight, Check, ChevronRight, CircleDot, Globe, KeyRound,
  LayoutDashboard, Loader2, Moon, MoreHorizontal, Power, RefreshCw,
  Route as RouteIcon, Save, Server, Settings2, SquareTerminal, Sun,
  TriangleAlert, WifiOff, X, Zap,
} from 'lucide-react'
import { useEffect, useState, type ReactNode } from 'react'

import { Issues } from '@/components/Issues'
import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from '@/components/ui/dialog'
import { ErrorBoundary } from '@/components/ErrorBoundary'
import { cn } from '@/lib/utils'
import { useI18n } from '@/lib/i18n'
import { useTheme } from '@/lib/theme'
import { countChanges } from '@/lib/diff'
import { useStore } from '@/store'

export type Tab = 'overview' | 'providers' | 'routes' | 'logs' | 'settings'

const NAV: { tab: Tab; icon: typeof Activity; label: string }[] = [
  { tab: 'overview', icon: LayoutDashboard, label: 'tabOverview' },
  { tab: 'providers', icon: Server, label: 'tabProviders' },
  { tab: 'routes', icon: RouteIcon, label: 'tabRoutes' },
  { tab: 'logs', icon: SquareTerminal, label: 'tabLogs' },
  { tab: 'settings', icon: Settings2, label: 'tabSettings' },
]

function Brand({ compact = false }: { compact?: boolean }) {
  return (
    <div className="flex items-center gap-3">
      <div className="flex size-9 shrink-0 items-center justify-center rounded-xl bg-[#8d7be8] text-white shadow-sm">
        <svg viewBox="0 0 24 24" className="size-6" fill="none" aria-hidden="true">
          <path d="M6 5v10a4 4 0 0 0 4 4h8M6 12h6a6 6 0 0 0 6-6" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
          <circle cx="6" cy="5" r="2" fill="currentColor" />
          <circle cx="18" cy="5" r="2" fill="currentColor" />
          <circle cx="18" cy="19" r="2" fill="currentColor" />
        </svg>
      </div>
      {!compact && <span className="text-xl font-semibold tracking-[-0.04em]">literouter<span className="text-[#a493ef]">.</span></span>}
    </div>
  )
}

export function AppShell({ tab, onTab, children }: { tab: Tab; onTab: (tab: Tab) => void; children: ReactNode }) {
  const {
    snapshot, online, loaded, working, dirty, saving, saveIssues, save, discard,
    reload, refresh, resetStats, shutdown, consoleOff, update, openKeyPrompt,
  } = useStore()
  const { t, lang, setLang } = useI18n()
  const { theme, toggle } = useTheme()
  const [controls, setControls] = useState(false)
  const [refreshing, setRefreshing] = useState(false)
  const active = NAV.find((item) => item.tab === tab) ?? NAV[0]!
  const changes = working && loaded ? countChanges(working, loaded.config) : 0

  useEffect(() => {
    if (!dirty) return
    const protectDraft = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', protectDraft)
    return () => window.removeEventListener('beforeunload', protectDraft)
  }, [dirty])

  async function refreshStatus() {
    setRefreshing(true)
    try { await refresh() } finally { setRefreshing(false) }
  }

  function navigate(next: Tab) {
    onTab(next)
    window.scrollTo({ top: 0, behavior: 'instant' })
  }

  return (
    <div className="min-h-dvh lg:pl-[232px]">
      <a href="#main-content" className="fixed top-3 left-3 z-[100] -translate-y-24 rounded-lg bg-primary px-4 py-3 text-primary-foreground focus:translate-y-0">{t('skipToContent')}</a>
      <aside className="fixed inset-y-0 left-0 z-30 hidden w-[232px] flex-col bg-sidebar px-4 py-7 text-sidebar-foreground lg:flex">
        <div className="px-3 text-white"><Brand /></div>
        <div className="mt-9 px-3 text-[10px] font-semibold tracking-[0.16em] uppercase text-sidebar-foreground/60">{t('workspaceLabel')}</div>
        <nav className="mt-3 space-y-1.5" aria-label={t('navigationLabel')}>
          {NAV.map(({ tab: page, icon: Icon, label }) => {
            const count = page === 'providers' ? working?.providers.length : page === 'routes' ? working?.routes.length : undefined
            return (
              <button key={page} type="button" aria-current={tab === page ? 'page' : undefined} onClick={() => navigate(page)}
                className={cn('flex min-h-11 w-full items-center gap-3 rounded-xl px-3 text-[13px] font-medium transition-colors', tab === page ? 'bg-white/10 text-white' : 'hover:bg-white/5 hover:text-white')}>
                <Icon className={cn('size-[18px]', tab === page && 'text-[#b5a5ff]')} strokeWidth={1.7} />
                {t(label)}
                {count !== undefined && <span className="ml-auto rounded-md bg-white/5 px-1.5 py-0.5 text-[10px] tnum text-sidebar-foreground">{count}</span>}
                {page === 'overview' && tab === page && <span className="ml-auto size-1.5 rounded-full bg-[#b5a5ff]" />}
              </button>
            )
          })}
        </nav>

        <div className="mt-auto pt-10">
          <div className="rounded-xl border border-white/8 bg-white/3 p-3.5">
            <div className="flex items-center gap-2 text-xs font-medium text-white"><CircleDot className="size-4 text-[#a493ef]" />{t('gatewayLabel')}</div>
            <div className="mt-2 truncate font-mono text-[10px] text-sidebar-foreground" title={snapshot?.base_url}>{snapshot?.base_url || '—'}</div>
            <div className="mt-3 flex items-center justify-between border-t border-white/8 pt-3 text-[10px]">
              <span className="flex items-center gap-1.5"><span className={cn('size-1.5 rounded-full', online ? 'bg-emerald-400' : 'bg-rose-400')} />{online ? t('connected') : t('disconnected')}</span>
              <span className="font-mono">{snapshot?.version ? `v${snapshot.version}` : '—'}</span>
            </div>
          </div>
          <div className="mt-4 flex items-center justify-between px-1">
            <Button size="icon" variant="ghost" className="text-sidebar-foreground hover:bg-white/10 hover:text-white" aria-label={t('themeTitle')} title={t('themeTitle')} onClick={toggle}>{theme === 'dark' ? <Sun /> : <Moon />}</Button>
            <Button size="icon" variant="ghost" className="text-sidebar-foreground hover:bg-white/10 hover:text-white" aria-label={t('languageTitle')} title={t('languageTitle')} onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')}><Globe /></Button>
            <Button size="icon" variant="ghost" className="text-sidebar-foreground hover:bg-white/10 hover:text-white" aria-label={t('keyTitle')} title={t('keyTitle')} onClick={openKeyPrompt}><KeyRound /></Button>
          </div>
        </div>
      </aside>

      <header className="sticky top-0 z-20 flex h-[72px] items-center gap-3 border-b bg-background/90 px-4 backdrop-blur-xl sm:px-7 lg:px-9">
        <div className="lg:hidden"><Brand compact /></div>
        <div className="flex min-w-0 items-center gap-2 text-xs sm:gap-3">
          <span className="hidden text-muted-foreground sm:inline">{t('localWorkspace')}</span>
          <ChevronRight className="hidden size-3.5 text-muted-foreground/60 sm:block" />
          <span className="truncate font-medium">{t(active.label)}</span>
        </div>
        <div className="ml-auto flex shrink-0 items-center gap-1.5 sm:gap-3">
          <span className={cn('flex items-center gap-2 rounded-full border px-2.5 py-1.5 text-[11px] font-medium', online ? 'border-ok/15 bg-ok/5 text-ok' : 'border-border text-muted-foreground')} role="status" aria-label={t('gatewayStatus')}>
            <span className={cn('size-1.5 rounded-full', online ? 'bg-ok' : 'bg-muted-foreground')} />
            <span className="hidden min-[380px]:inline">{online ? t('connected') : t('disconnected')}</span>
          </span>
          <div className="mx-1 hidden h-5 w-px bg-border sm:block" />
          <Button variant="ghost" size="icon" aria-label={t('refreshView')} title={t('refreshView')} disabled={refreshing} onClick={() => void refreshStatus()}><RefreshCw className={cn('size-4', refreshing && 'animate-spin')} /></Button>
          <Button variant="outline" size="icon" aria-label={t('serviceActions')} title={t('serviceActions')} onClick={() => setControls(true)}><MoreHorizontal className="size-4" /></Button>
        </div>
      </header>

      {consoleOff && <div className="flex flex-wrap items-center gap-3 border-b border-warn/20 bg-warn/5 px-5 py-3 text-xs text-warn"><TriangleAlert className="size-4" />{t('consoleDisabled')}<Button size="sm" variant="ghost" className="ml-auto text-warn" onClick={() => update((draft) => { draft.server.web_ui = true })}>{t('restoreConsoleDraft')}</Button></div>}
      {!dirty && saveIssues.length > 0 && <div className="mx-4 mt-5 sm:mx-7 lg:mx-9" role="alert"><Issues issues={saveIssues} /></div>}
      {!online && <div className="mx-4 mt-5 flex flex-wrap items-center gap-3 rounded-xl border border-warn/20 bg-warn/5 p-4 sm:mx-7 lg:mx-9">
        <WifiOff className="size-4 shrink-0 text-warn" />
        <div className="min-w-0 flex-1"><p className="text-sm font-medium">{t(snapshot ? 'connectionLost' : 'connectingGateway')}</p><p className="mt-1 text-xs leading-relaxed text-muted-foreground">{t(snapshot ? 'connectionLostNote' : 'connectionInitialNote')}</p></div>
        <Button size="sm" variant="outline" onClick={openKeyPrompt}><KeyRound className="size-3.5" />{t('accessLabel')}</Button>
      </div>}

      <main id="main-content" tabIndex={-1} className={cn('mx-auto w-full max-w-[1600px] px-4 pt-7 pb-28 outline-none sm:px-7 sm:pt-8 lg:px-9 lg:pb-12', dirty && 'pb-52 lg:pb-36')}>
        <ErrorBoundary resetKey={tab} labels={{ title: t('renderFailed'), note: t('renderFailedNote'), retry: t('retry') }}>
          <div key={tab} className="page-enter">{children}</div>
        </ErrorBoundary>
        <footer className="mt-10 flex flex-wrap items-center justify-between gap-2 border-t pt-5 text-[10px] text-muted-foreground">
          <span>literouter <span className="mx-1 opacity-40">/</span> {t('gatewayLabel')}</span>
          <span className="flex items-center gap-1.5">{dirty ? <CircleDot className="size-3 text-warn" /> : <Check className="size-3 text-ok" />}{dirty ? t('unsaved', { n: changes }) : t(loaded ? 'savedConfigLabel' : 'connectingGateway')}</span>
        </footer>
      </main>

      <nav className="mobile-nav glass fixed inset-x-0 bottom-0 z-30 grid grid-cols-5 border-t px-2 pt-2 lg:hidden" aria-label={t('navigationLabel')}>
        {NAV.map(({ tab: page, icon: Icon, label }) => <button key={page} type="button" aria-current={page === tab ? 'page' : undefined} onClick={() => navigate(page)} className={cn('flex min-h-12 min-w-0 flex-col items-center justify-center gap-1 rounded-xl px-1 py-1.5 text-[10px] transition-colors', page === tab ? 'bg-primary/9 font-semibold text-primary' : 'text-muted-foreground')}><Icon className="size-[18px]" strokeWidth={page === tab ? 2 : 1.7} /><span className="max-w-full truncate">{t(label)}</span></button>)}
      </nav>

      {/* Drafts survive navigation; the save bar remains reachable on phones. */}
      {dirty && <div className="draft-bar glass fixed inset-x-3 z-40 rounded-2xl border border-primary/20 p-3.5 shadow-[0_8px_40px_-12px_#29223c40] sm:p-4" aria-live="polite">
        <div className="flex flex-wrap items-center gap-3">
          <div className="hidden size-9 items-center justify-center rounded-xl bg-primary/10 text-primary sm:flex"><Save className="size-4" /></div>
          <div className="min-w-0"><div className="text-xs font-semibold sm:text-sm">{t('unsaved', { n: changes })}</div><div className="mt-0.5 hidden text-[11px] text-muted-foreground sm:block">{t('draftNote')}</div></div>
          <div className="ml-auto flex items-center gap-2"><Button variant="ghost" size="sm" onClick={discard} disabled={saving}><X className="hidden size-3.5 sm:block" />{t('discard')}</Button><Button size="sm" onClick={() => void save()} disabled={saving}>{saving ? <Loader2 className="size-3.5 animate-spin" /> : <Check className="size-3.5" />}{t(saving ? 'savingChanges' : 'saveChanges')}</Button></div>
        </div>
        {saveIssues.length > 0 && <div className="mt-3 max-h-[25dvh] overflow-y-auto"><Issues issues={saveIssues} /></div>}
      </div>}

      <Dialog open={controls} onOpenChange={setControls}>
        <DialogContent>
          <DialogHeader><DialogTitle>{t('serviceActions')}</DialogTitle><DialogDescription>{t('serviceActionsNote')}</DialogDescription></DialogHeader>
          <div className="grid grid-cols-3 gap-2 border-b pb-5">
            <Button variant="outline" className="h-auto flex-col gap-2 py-3 text-xs" onClick={toggle}>{theme === 'dark' ? <Sun /> : <Moon />}{t('appearanceLabel')}</Button>
            <Button variant="outline" className="h-auto flex-col gap-2 py-3 text-xs" onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')}><Globe />{t('languageLabel')}</Button>
            <Button variant="outline" className="h-auto flex-col gap-2 py-3 text-xs" onClick={() => { setControls(false); openKeyPrompt() }}><KeyRound />{t('accessLabel')}</Button>
          </div>
          <div className="space-y-2">
            {[
              { icon: RefreshCw, title: 'reload', note: 'reloadNote', action: () => { if (!dirty || window.confirm(t('confirmReloadDraft'))) { setControls(false); void reload() } } },
              { icon: Zap, title: 'resetStats', note: 'resetNote', action: () => { if (window.confirm(t('confirmResetStats'))) { setControls(false); void resetStats() } } },
              { icon: Power, title: 'shutdown', note: 'shutdownNote', action: () => { if (window.confirm(t('confirmShutdown'))) { setControls(false); void shutdown() } } },
            ].map(({ icon: Icon, title, note, action }) => <button type="button" key={title} disabled={!online || saving} onClick={action} className="flex w-full items-center gap-3 rounded-xl p-3 text-left transition-colors hover:bg-muted disabled:cursor-not-allowed disabled:opacity-50"><span className={cn('flex size-9 shrink-0 items-center justify-center rounded-lg bg-muted', title === 'shutdown' && 'bg-destructive/10 text-destructive')}><Icon className="size-4" /></span><span className="flex-1"><span className="block text-sm font-medium">{t(title)}</span><span className="mt-1 block text-xs leading-relaxed text-muted-foreground">{t(note)}</span></span><ArrowUpRight className="size-4 shrink-0 text-muted-foreground" /></button>)}
          </div>
        </DialogContent>
      </Dialog>
    </div>
  )
}
