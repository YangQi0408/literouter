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
import { Settings } from '@/views/Settings'

export function App() {
  const [tab, setTab] = useState<Tab>('overview')
  const { theme } = useTheme()
  const { setLogsActive } = useStore()

  function switchTab(next: Tab) {
    setTab(next)
    // The log cursor only advances while the log is on screen; polling it from
    // another tab would quietly drain a ring the operator is not reading.
    setLogsActive(next === 'logs')
  }

  return (
    <>
      <AppShell tab={tab} onTab={switchTab}>
        {tab === 'overview' ? <Overview onNavigate={switchTab} /> : null}
        {tab === 'providers' ? <Providers /> : null}
        {tab === 'routes' ? <Routes /> : null}
        {tab === 'logs' ? <Logs /> : null}
        {tab === 'settings' ? <Settings /> : null}
      </AppShell>
      <KeyDialog />
      <Toaster theme={theme} position="bottom-center" closeButton />
    </>
  )
}
