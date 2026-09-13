import { Check, Copy } from 'lucide-react'
import { useState } from 'react'

import { Field, NumberField, SwitchField, TextField } from '@/components/fields'
import { Issues } from '@/components/Issues'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { useI18n } from '@/lib/i18n'
import { useStore } from '@/store'

export function Settings() {
  const { working, update, loaded, serverReport, saveIssues } = useStore()
  const { t } = useI18n()
  const [copied, setCopied] = useState(false)

  if (!working) return null
  const server = working.server
  const patch = (changes: Partial<typeof server>) =>
    update((draft) => {
      draft.server = { ...draft.server, ...changes }
    })
  const issues = saveIssues.length ? saveIssues : (serverReport?.issues ?? [])
  const json = JSON.stringify(working, null, 2)

  async function copy() {
    try {
      await navigator.clipboard.writeText(json)
      setCopied(true)
      window.setTimeout(() => setCopied(false), 2000)
    } catch {
      /* clipboard needs a secure context; the text is selectable anyway */
    }
  }

  return (
    <div className="flex flex-col gap-5">
      <Card>
        <CardHeader>
          <CardTitle>{t('tabSettings')}</CardTitle>
          <span className="ml-auto font-mono text-xs text-muted-foreground">
            {loaded?.path || ''}
          </span>
        </CardHeader>
        <CardContent className="grid gap-4 sm:grid-cols-2">
          <Field label="host">
            <TextField value={server.host} onChange={(host) => patch({ host })} />
          </Field>
          <Field label="port">
            <NumberField value={server.port} min={0} max={65535} onChange={(port) => patch({ port })} />
          </Field>

          <Field label="api_key" hint={t('keyHint')} wide>
            <Input
              value={server.api_key}
              placeholder="${LITEROUTER_KEY}"
              spellCheck={false}
              className="h-9 font-mono text-xs"
              onChange={(event) => patch({ api_key: event.target.value })}
            />
          </Field>

          <Field label="web_ui" hint={t('hintWebUi')}>
            <SwitchField
              checked={server.web_ui}
              onChange={(web_ui) => patch({ web_ui })}
              label={server.web_ui ? 'on' : 'off'}
            />
          </Field>
          <Field label="language">
            <Select value={server.language} onValueChange={(language) => patch({ language })}>
              <SelectTrigger className="h-9 w-full font-mono text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="auto">auto</SelectItem>
                <SelectItem value="en">en</SelectItem>
                <SelectItem value="zh">zh</SelectItem>
              </SelectContent>
            </Select>
          </Field>

          <Field label="pass_through_unknown">
            <SwitchField
              checked={server.pass_through_unknown}
              onChange={(pass_through_unknown) => patch({ pass_through_unknown })}
              label="pass_through_unknown"
            />
          </Field>
          <Field label="skip_open_circuits">
            <SwitchField
              checked={server.skip_open_circuits}
              onChange={(skip_open_circuits) => patch({ skip_open_circuits })}
              label="skip_open_circuits"
            />
          </Field>

          <Field label="max_attempts">
            <NumberField
              value={server.max_attempts}
              min={0}
              onChange={(max_attempts) => patch({ max_attempts })}
            />
          </Field>
          <Field label="ui_scale">
            <NumberField
              value={server.ui_scale}
              step={0.05}
              onChange={(ui_scale) => patch({ ui_scale })}
            />
          </Field>

          <Field label="circuit_failure_threshold">
            <NumberField
              value={server.circuit_failure_threshold}
              min={1}
              onChange={(circuit_failure_threshold) => patch({ circuit_failure_threshold })}
            />
          </Field>
          <Field label="circuit_cooldown_sec">
            <NumberField
              value={server.circuit_cooldown_sec}
              min={1}
              onChange={(circuit_cooldown_sec) => patch({ circuit_cooldown_sec })}
            />
          </Field>

          <Field label="log_capacity">
            <NumberField
              value={server.log_capacity}
              min={16}
              onChange={(log_capacity) => patch({ log_capacity })}
            />
          </Field>
          <Field label="log_bodies" hint={t('hintLogBodies')}>
            <SwitchField
              checked={server.log_bodies}
              onChange={(log_bodies) => patch({ log_bodies })}
              label="log_bodies"
            />
          </Field>

          <Field label="log_body_limit">
            <NumberField
              value={server.log_body_limit}
              min={64}
              onChange={(log_body_limit) => patch({ log_body_limit })}
            />
          </Field>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>{t('tabConfig')}</CardTitle>
          <span className="ml-1 text-xs text-muted-foreground">{t('configNote')}</span>
          <Button size="sm" variant="ghost" className="ml-auto" onClick={() => void copy()}>
            {copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
            {copied ? t('copied') : t('copy')}
          </Button>
        </CardHeader>
        {issues.length ? (
          <CardContent>
            <Issues issues={issues} />
          </CardContent>
        ) : null}
        <pre className="max-h-[46vh] overflow-auto bg-muted/40 p-4 font-mono text-xs leading-relaxed text-muted-foreground">
          {json}
        </pre>
      </Card>
    </div>
  )
}
