import { createContext, useContext, useId, useRef, useState, type ReactNode } from 'react'

import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Switch } from '@/components/ui/switch'
import { Textarea } from '@/components/ui/textarea'
import { cn } from '@/lib/utils'

const FieldContext = createContext<{ id: string; description?: string } | null>(null)

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
  const id = useId()
  const description = hint ? `${id}-hint` : undefined
  return (
    <FieldContext.Provider value={{ id, description }}>
      <div className={cn('flex min-w-0 flex-col gap-2', wide && 'sm:col-span-2')}>
        <Label htmlFor={id} className="text-sm font-medium leading-5">{label}</Label>
        {children}
        {hint ? <p id={description} className="text-xs leading-relaxed text-muted-foreground">{hint}</p> : null}
      </div>
    </FieldContext.Provider>
  )
}

export function TextField({
  value,
  onChange,
  placeholder,
  mono = true,
  type = 'text',
  ariaLabel,
}: {
  value: string
  onChange: (value: string) => void
  placeholder?: string
  mono?: boolean
  type?: string
  ariaLabel?: string
}) {
  const field = useContext(FieldContext)
  return (
    <Input
      id={field?.id}
      type={type}
      aria-label={ariaLabel}
      aria-describedby={field?.description}
      value={value ?? ''}
      placeholder={placeholder}
      spellCheck={false}
      onChange={(event) => onChange(event.target.value)}
      className={cn('h-10', mono && 'font-mono text-sm')}
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
  const field = useContext(FieldContext)
  return (
    <Input
      id={field?.id}
      aria-describedby={field?.description}
      type="number"
      value={Number.isFinite(value) ? value : ''}
      min={min}
      max={max}
      step={step}
      onChange={(event) => {
        const parsed = Number(event.target.value)
        onChange(Number.isFinite(parsed) ? parsed : 0)
      }}
      className="h-10 font-mono text-sm"
    />
  )
}

/** Holds the raw text of a multi-line field while the operator types.
 *
 *  A textarea cannot be driven directly by the parsed value: parsing drops
 *  blank and partial lines, so echoing it back mid-edit swallowed the newline
 *  the operator had just pressed and made a second entry impossible to type.
 *  The raw text therefore lives here and only the parse result travels
 *  outward. `render` maps a value to its canonical text; the box re-syncs only
 *  when that canonical text stops matching what our own text yields, which is
 *  what distinguishes an outside change (a load, a discard, a reopened dialog)
 *  from the operator's own keystrokes. */
function useRawText(canonical: string, canonicalize: (raw: string) => string) {
  const [text, setText] = useState(canonical)
  const seen = useRef(canonical)
  if (seen.current !== canonical) {
    seen.current = canonical
    // Ours if the text we hold already canonicalises to the incoming value;
    // otherwise it came from elsewhere and should replace what is in the box.
    if (canonicalize(text) !== canonical) setText(canonical)
  }
  return [text, setText] as const
}

const parseLines = (raw: string) =>
  raw
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line.length > 0)

export function LinesField({
  value,
  onChange,
  placeholder,
  rows = 4,
  ariaLabel,
}: {
  value: string[]
  onChange: (value: string[]) => void
  placeholder?: string
  rows?: number
  ariaLabel?: string
}) {
  const field = useContext(FieldContext)
  // `?? []`: a missing key must not be able to take the page down, whatever the
  // caller hands over.
  const [text, setText] = useRawText((value ?? []).join('\n'), (raw) => parseLines(raw).join('\n'))

  return (
    <Textarea
      id={field?.id}
      rows={rows}
      aria-label={ariaLabel}
      aria-describedby={field?.description}
      spellCheck={false}
      placeholder={placeholder}
      value={text}
      onChange={(event) => {
        setText(event.target.value)
        onChange(parseLines(event.target.value))
      }}
      className="min-h-0 resize-y font-mono text-sm leading-relaxed"
    />
  )
}

const parseHeaders = (raw: string): Record<string, string> => {
  const next: Record<string, string> = {}
  for (const line of raw.split('\n')) {
    const at = line.indexOf(':')
    if (at <= 0) continue
    const name = line.slice(0, at).trim()
    if (name) next[name] = line.slice(at + 1).trim()
  }
  return next
}

const renderHeaders = (value: Record<string, string>) =>
  Object.entries(value ?? {})
    .map(([name, headerValue]) => `${name}: ${headerValue}`)
    .join('\n')

export function HeadersField({
  value,
  onChange,
}: {
  value: Record<string, string>
  onChange: (value: Record<string, string>) => void
}) {
  const field = useContext(FieldContext)
  // Same reason as LinesField: a header is only parseable once its colon is
  // typed, and echoing the parse back before that dropped the name character
  // by character as it was being written.
  const [text, setText] = useRawText(renderHeaders(value ?? {}), (raw) =>
    renderHeaders(parseHeaders(raw)),
  )

  return (
    <Textarea
      id={field?.id}
      aria-describedby={field?.description}
      rows={3}
      spellCheck={false}
      placeholder="X-Title: literouter"
      value={text}
      onChange={(event) => {
        setText(event.target.value)
        onChange(parseHeaders(event.target.value))
      }}
      className="min-h-0 resize-y font-mono text-sm leading-relaxed"
    />
  )
}

export function SwitchField({
  checked,
  onChange,
  label,
  ariaLabel,
}: {
  checked: boolean
  onChange: (checked: boolean) => void
  label: string
  ariaLabel?: string
}) {
  const field = useContext(FieldContext)
  return (
    <div className="flex min-h-10 items-center gap-3">
      <Switch id={field?.id} aria-describedby={field?.description} aria-label={ariaLabel ?? label} checked={checked} onCheckedChange={onChange} />
      <span className="text-sm text-muted-foreground">{label}</span>
    </div>
  )
}
