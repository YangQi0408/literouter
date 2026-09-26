import { useState } from 'react'

import { AppShell, type Tab } from '@/components/AppShell'
import { KeyDialog } from '@/components/KeyDialog'
import { Toaster } from '@/components/ui/sonner'
import { useTheme } from '@/lib/theme'
import { useStore } from '@/store'
import { Logs } from '@/views/Logs'
import { Overview } from '@/views/Overview'
import { Providers } from '@/views/Providers'
import { Routes } from '@/views/Routes'
import { Settings, type SettingsSection } from '@/views/Settings'

export function App() {
  const [tab, setTab] = useState<Tab>('overview')
  const [logQuery, setLogQuery] = useState('')
  const [settingsSection, setSettingsSection] = useState<SettingsSection>('connection')
  const { theme } = useTheme()
  const { setLogsActive } = useStore()

  function switchTab(next: Tab) {
    if (next === 'logs') setLogQuery('')
    if (next === 'settings') setSettingsSection('connection')
    setTab(next)
    // The log cursor only advances while the log is on screen; polling it from
    // another tab would quietly drain a ring the operator is not reading.
    setLogsActive(next === 'logs')
  }

  function inspectRelay(provider: string) {
    setLogQuery(provider)
    setTab('logs')
    setLogsActive(true)
  }

  return (
    <>
      <AppShell tab={tab} onTab={switchTab}>
        {tab === 'overview' ? <Overview onNavigate={switchTab} onInspectRelay={inspectRelay} /> : null}
        {tab === 'providers' ? <Providers /> : null}
        {tab === 'routes' ? <Routes /> : null}
        {tab === 'logs' ? <Logs initialQuery={logQuery} onConfigureLogging={() => { switchTab('settings'); setSettingsSection('telemetry') }} /> : null}
        {tab === 'settings' ? <Settings initialSection={settingsSection} /> : null}
      </AppShell>
      <KeyDialog />
      <Toaster theme={theme} position="bottom-center" closeButton />
    </>
  )
}
