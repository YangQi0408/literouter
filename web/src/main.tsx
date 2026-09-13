import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import { App } from '@/App'
import { TooltipProvider } from '@/components/ui/tooltip'
import { I18nProvider } from '@/lib/i18n'
import { ThemeProvider } from '@/lib/theme'
import { StoreProvider } from '@/store'
import '@/index.css'

const container = document.getElementById('root')
if (!container) throw new Error('the console needs a #root element to mount into')

createRoot(container).render(
  <StrictMode>
    <ThemeProvider>
      <I18nProvider>
        <TooltipProvider delayDuration={300}>
          <StoreProvider>
            <App />
          </StoreProvider>
        </TooltipProvider>
      </I18nProvider>
    </ThemeProvider>
  </StrictMode>,
)
