import type { ReactNode } from 'react'

import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Switch } from '@/components/ui/switch'
import { Textarea } from '@/components/ui/textarea'
import { cn } from '@/lib/utils'

/** One labelled control in a config form. Labels are the config key itself —
 *  the console edits a file, so a field should name the field it writes. */
export function Field({
  label,
  hint,
  wide,
  children,
}: {
  label: string
  hint?: string
  wide?: boolean
  children: ReactNode
}) {
  return (
    <div className={cn('flex min-w-0 flex-col gap-1.5', wide && 'sm:col-span-2')}>
      <Label className="font-mono text-xs text-muted-foreground">{label}</Label>
      {children}
      {hint ? <p className="text-[11px] leading-snug text-muted-foreground/80">{hint}</p> : null}
    </div>
  )
}

export function TextField({
  value,
  onChange,
  placeholder,
  mono = true,
  type = 'text',
}: {
  value: string
  onChange: (value: string) => void
  placeholder?: string
  mono?: boolean
  type?: string
}) {
  return (
    <Input
      type={type}
      value={value}
      placeholder={placeholder}
      spellCheck={false}
      onChange={(event) => onChange(event.target.value)}
      className={cn('h-9', mono && 'font-mono text-xs')}
    />
  )
}

export function NumberField({
  value,
  onChange,
  min,
  max,
  step,
}: {
  value: number
  onChange: (value: number) => void
  min?: number
  max?: number
  step?: number
}) {
  return (
    <Input
      type="number"
      value={Number.isFinite(value) ? value : ''}
      min={min}
      max={max}
      step={step}
      onChange={(event) => {
        const parsed = Number(event.target.value)
        onChange(Number.isFinite(parsed) ? parsed : 0)
      }}
      className="h-9 font-mono text-xs"
    />
  )
}

export function LinesField({
  value,
  onChange,
  placeholder,
  rows = 4,
}: {
  value: string[]
  onChange: (value: string[]) => void
  placeholder?: string
  rows?: number
}) {
  return (
    <Textarea
      rows={rows}
      spellCheck={false}
      placeholder={placeholder}
      value={value.join('\n')}
      onChange={(event) =>
        onChange(
          event.target.value
            .split('\n')
            .map((line) => line.trim())
            .filter((line) => line.length > 0),
        )
      }
      className="min-h-0 font-mono text-xs"
    />
  )
}

export function HeadersField({
  value,
  onChange,
}: {
  value: Record<string, string>
  onChange: (value: Record<string, string>) => void
}) {
  const text = Object.entries(value)
    .map(([name, headerValue]) => `${name}: ${headerValue}`)
    .join('\n')
  return (
    <Textarea
      rows={3}
      spellCheck={false}
      placeholder="X-Title: literouter"
      value={text}
      onChange={(event) => {
        const next: Record<string, string> = {}
        for (const line of event.target.value.split('\n')) {
          const at = line.indexOf(':')
          if (at <= 0) continue
          const name = line.slice(0, at).trim()
          const headerValue = line.slice(at + 1).trim()
          if (name) next[name] = headerValue
        }
        onChange(next)
      }}
      className="min-h-0 font-mono text-xs"
    />
  )
}

export function SwitchField({
  checked,
  onChange,
  label,
}: {
  checked: boolean
  onChange: (checked: boolean) => void
  label: string
}) {
  return (
    <div className="flex h-9 items-center gap-3">
      <Switch checked={checked} onCheckedChange={onChange} />
      <span className="text-xs text-muted-foreground">{label}</span>
    </div>
  )
}
