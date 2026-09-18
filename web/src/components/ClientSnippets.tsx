import { Check, Copy } from 'lucide-react'
import { useState } from 'react'

import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { useI18n } from '@/lib/i18n'
import { useStore } from '@/store'

/** The two editor integrations worth pasting, next to the endpoint they need.
 *
 *  The GUI's settings page already lists curl, the Python SDK and LangChain;
 *  these are the ones it does not, and they are the ones an operator sets up
 *  once and forgets. Everything is generated from the running config, so a
 *  changed port or key cannot leave the snippet behind. */
export function ClientSnippets() {
  const { t } = useI18n()
  const { loaded, snapshot } = useStore()
  const [copied, setCopied] = useState<string | null>(null)

  if (!loaded) {
    return null
  }
  const server = loaded.config.server
  const base = snapshot?.base_url || `http://${server.host}:${server.port}`
  const key = server.api_key ? 'sk-local' : 'unused'

  const continueJson = JSON.stringify(
    {
      models: [
        {
          title: 'literouter',
          provider: 'openai',
          model: 'gpt-4o',
          apiBase: `${base}/v1`,
          apiKey: key,
        },
      ],
    },
    null,
    2,
  )

  const copy = async (id: string, text: string) => {
    try {
      await navigator.clipboard.writeText(text)
      setCopied(id)
      window.setTimeout(() => setCopied(null), 2000)
    } catch {
      /* clipboard needs a secure context; the text is selectable anyway */
    }
  }

  const block = (id: string, title: string, text: string) => (
    <div className="flex flex-col gap-1.5">
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">{title}</span>
        <Button size="sm" variant="ghost" className="ml-auto" onClick={() => void copy(id, text)}>
          {copied === id ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
          {copied === id ? t('copied') : t('copy')}
        </Button>
      </div>
      <pre className="max-h-56 overflow-auto rounded-md bg-muted/40 p-3 font-mono text-[11px] leading-relaxed text-muted-foreground">
        {text}
      </pre>
    </div>
  )

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('clientSnippets')}</CardTitle>
        <span className="ml-1 text-xs text-muted-foreground">{t('clientSnippetsNote')}</span>
      </CardHeader>
      <CardContent className="grid gap-4 sm:grid-cols-2">
        {block('continue', t('snippetContinue'), continueJson)}
        {block(
          'cursor',
          t('snippetCursor'),
          [
            `Base URL   ${base}/v1`,
            `API key    ${server.api_key ? t('snippetCursorKey') : t('snippetCursorNoKey')}`,
            '',
            t('snippetCursorSteps'),
          ].join('\n'),
        )}
      </CardContent>
    </Card>
  )
}
