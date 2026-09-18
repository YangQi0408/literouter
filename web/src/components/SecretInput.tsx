import { Copy, Eye, EyeOff, KeyRound } from 'lucide-react'
import { useState } from 'react'
import { toast } from 'sonner'

import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import type { SecretConfig } from '@/lib/api'
import { generateApiKey, hasSecret } from '@/lib/clients'
import { useI18n } from '@/lib/i18n'

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
    <div className="flex flex-wrap items-center gap-2">
      <Input aria-label={label} type={visible ? 'text' : 'password'} value={value.api_key}
        autoComplete="off" spellCheck={false}
        placeholder={hasSecret(value) ? t('keyStored') : '${CLIENT_KEY}'}
        onChange={(event) => onChange({ api_key: event.target.value, api_key_clear: false })}
        className="h-9 min-w-40 flex-1 font-mono text-xs" />
      <Button type="button" size="icon" variant="ghost" title={t('showKey')}
        aria-label={t('showKey')} onClick={() => setVisible(!visible)}>
        {visible ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
      </Button>
      {value.api_key ? <Button type="button" size="icon" variant="ghost" title={t('copy')}
        aria-label={t('copy')} onClick={async () => {
          try { await navigator.clipboard.writeText(value.api_key); toast.success(t('copied')) }
          catch { toast.error(t('clipboardFailed')) }
        }}>
        <Copy className="size-4" />
      </Button> : null}
      {generate ? <Button type="button" variant="outline" size="sm" onClick={() => {
        try { onChange({ api_key: generateApiKey(), api_key_clear: false }); setVisible(true) }
        catch { toast.error(t('keyGenerateFailed')) }
      }}><KeyRound className="size-3.5" />{t('generateKey')}</Button> : null}
      {clear && hasSecret(value) ? <Button type="button" variant="ghost" size="sm"
        onClick={() => onChange({ api_key: '', api_key_clear: true })}>{t('keyClearAction')}</Button> : null}
      {value.api_key_clear ? <span className="text-xs text-warn">{t('keyWillClear')}</span> : null}
    </div>
  )
}
