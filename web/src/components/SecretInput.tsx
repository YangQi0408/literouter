import { Copy, Eye, EyeOff, KeyRound } from 'lucide-react'
import { useState } from 'react'
import { toast } from 'sonner'

import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import type { SecretConfig } from '@/lib/api'
import { generateApiKey, hasSecret } from '@/lib/secrets'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'

export function SecretInput({ value, onChange, label, generate = false, clear = false }: {
  value: SecretConfig
  onChange: (changes: Partial<SecretConfig>) => void
  label: string
  generate?: boolean
  clear?: boolean
}) {
  const { t } = useI18n()
  const [visible, setVisible] = useState(false)
  return (
    <div className="min-w-0 space-y-2.5">
      <div className="relative">
        <Input aria-label={label} type={visible ? 'text' : 'password'} value={value.api_key}
          autoComplete="off" spellCheck={false}
          placeholder={hasSecret(value) ? t('keyStored') : '${LITEROUTER_KEY}'}
          onChange={(event) => onChange({ api_key: event.target.value, api_key_clear: false })}
          className={cn('h-10 w-full min-w-0 font-mono text-xs', value.api_key ? 'pr-20' : 'pr-11')} />
        <div className="absolute inset-y-0 right-1 flex items-center gap-0.5">
          <Button type="button" size="icon" variant="ghost" className="size-8 text-muted-foreground" title={t(visible ? 'keyHide' : 'keyReveal')}
            aria-label={t(visible ? 'keyHide' : 'keyReveal')} aria-pressed={visible} onClick={() => setVisible(!visible)}>
            {visible ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
          </Button>
          {value.api_key ? <Button type="button" size="icon" variant="ghost" className="size-8 text-muted-foreground" title={t('copy')}
            aria-label={t('copy')} onClick={async () => {
              try { await navigator.clipboard.writeText(value.api_key); toast.success(t('copied')) }
              catch { toast.error(t('clipboardFailed')) }
            }}><Copy className="size-3.5" /></Button> : null}
        </div>
      </div>
      {generate || (clear && hasSecret(value)) || value.api_key_clear ? <div className="flex flex-wrap items-center gap-2">
        {generate ? <Button type="button" variant="outline" size="sm" onClick={() => {
          try { onChange({ api_key: generateApiKey(), api_key_clear: false }); setVisible(true) }
          catch { toast.error(t('keyGenerateFailed')) }
        }}><KeyRound className="size-3.5" />{t('generateKey')}</Button> : null}
        {clear && hasSecret(value) ? <Button type="button" variant="ghost" size="sm" className="text-muted-foreground hover:text-destructive"
          onClick={() => onChange({ api_key: '', api_key_clear: true })}>{t('keyClearAction')}</Button> : null}
        {value.api_key_clear ? <span role="status" className="rounded-lg bg-warn/10 px-2.5 py-1.5 text-xs text-warn">{t('keyWillClear')}</span> : null}
      </div> : null}
    </div>
  )
}
