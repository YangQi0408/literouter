import { ArrowRight, Eye, EyeOff, KeyRound, LockKeyhole } from 'lucide-react'
import { useState } from 'react'

import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { getApiKey } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { useApiKey, useStore } from '@/store'

export function KeyDialog() {
  const { keyPrompt, closeKeyPrompt } = useStore()
  const { save, clear } = useApiKey()
  const { t } = useI18n()
  const [value, setValue] = useState('')
  const [visible, setVisible] = useState(false)

  return (
    <Dialog open={keyPrompt} onOpenChange={(open) => { if (!open) closeKeyPrompt() }}>
      <DialogContent className="gap-7 sm:max-w-md" onOpenAutoFocus={() => {
        setValue(getApiKey())
        setVisible(false)
      }}>
        <DialogHeader className="text-left">
          <div className="mb-3 flex size-12 items-center justify-center rounded-2xl border border-primary/15 bg-primary/8 text-primary"><KeyRound className="size-6" /></div>
          <DialogTitle className="text-xl tracking-tight">{t('keyConnect')}</DialogTitle>
          <DialogDescription className="leading-relaxed">{t('keyHint')}</DialogDescription>
        </DialogHeader>
        <form onSubmit={(event) => { event.preventDefault(); void save(value) }} className="space-y-6">
          <div className="space-y-2.5">
            <label htmlFor="gateway-api-key" className="text-sm font-medium">{t('administratorKey')}</label>
            <div className="relative">
              <Input id="gateway-api-key" type={visible ? 'text' : 'password'} autoFocus autoComplete="off" spellCheck={false} value={value} onChange={(event) => setValue(event.target.value)} className="h-11 pr-11 font-mono text-sm" />
              <button type="button" aria-label={t(visible ? 'keyHide' : 'keyReveal')} onClick={() => setVisible(!visible)} className="absolute right-2 top-1/2 flex size-8 -translate-y-1/2 items-center justify-center rounded-lg text-muted-foreground hover:bg-muted focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring">{visible ? <EyeOff className="size-4" /> : <Eye className="size-4" />}</button>
            </div>
            <p className="flex items-center gap-1.5 text-xs text-muted-foreground"><LockKeyhole className="size-3" />{t('keyBrowserNote')}</p>
          </div>
          <DialogFooter className="gap-2">
            <Button type="button" variant="ghost" onClick={() => void clear()}>{t('keyClear')}</Button>
            <Button type="submit">{t('keyConnectAction')}<ArrowRight className="size-4" /></Button>
          </DialogFooter>
        </form>
      </DialogContent>
    </Dialog>
  )
}
