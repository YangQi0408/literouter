import { KeyRound } from 'lucide-react'
import { useState } from 'react'

import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { getApiKey } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { useApiKey, useStore } from '@/store'

export function KeyDialog() {
  const { keyPrompt, closeKeyPrompt } = useStore()
  const { save, clear } = useApiKey()
  const { t } = useI18n()
  const [value, setValue] = useState('')

  return (
    <Dialog
      open={keyPrompt}
      onOpenChange={(open) => {
        if (!open) closeKeyPrompt()
        else setValue(getApiKey())
      }}
    >
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">
            <KeyRound className="size-4 text-primary" />
            {t('keyTitle')}
          </DialogTitle>
          <DialogDescription>{t('keyHint')}</DialogDescription>
        </DialogHeader>
        <div className="px-4">
          <Input
            type="password"
            autoFocus
            spellCheck={false}
            value={value}
            onChange={(event) => setValue(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') void save(value)
            }}
            className="h-9 font-mono text-xs"
          />
        </div>
        <DialogFooter>
          <Button variant="ghost" onClick={() => void clear()}>
            {t('keyClear')}
          </Button>
          <Button onClick={() => void save(value)}>{t('save')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
