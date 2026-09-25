import type { LucideIcon } from 'lucide-react'

import { cn } from '@/lib/utils'

export interface MetricProps {
  icon: LucideIcon
  label: string
  value: string
  sub?: string
  /** 0..1 — draws the progress line under the value (success rate). */
  ratio?: number
  long?: boolean
  danger?: boolean
  tone?: 'primary' | 'green' | 'amber' | 'blue'
}

const tones = {
  primary: 'bg-primary/10 text-primary',
  green: 'bg-ok/10 text-ok',
  amber: 'bg-warn/10 text-warn',
  blue: 'bg-sky-500/10 text-sky-600 dark:text-sky-400',
}

export function Metric({ icon: Icon, label, value, sub, ratio, long, danger, tone = 'primary' }: MetricProps) {
  return (
    <div className="relative flex min-w-0 flex-col rounded-2xl border border-border/80 bg-card p-4 shadow-xs sm:p-5">
      <div className="flex items-start justify-between gap-2">
        <span className="pt-1 text-xs font-medium text-muted-foreground sm:text-sm">{label}</span>
        <span
          className={cn(
            'flex size-8 shrink-0 items-center justify-center rounded-xl sm:size-9',
            danger ? 'bg-destructive/10 text-destructive' : tones[tone],
          )}
        >
          <Icon className="size-4" aria-hidden="true" />
        </span>
      </div>
      <div
        className={cn(
          'mt-2 break-words font-semibold tracking-tight text-foreground',
          long ? 'break-all text-base' : 'text-2xl tnum sm:text-[32px] sm:leading-10',
          danger && 'text-destructive',
        )}
      >
        {value}
      </div>
      {sub ? <div className="mt-2 text-[11px] leading-relaxed text-muted-foreground sm:text-xs">{sub}</div> : null}
      {ratio !== undefined ? (
        <div className="mt-3 h-1 w-full overflow-hidden rounded-full bg-muted" aria-hidden="true">
          <div
            className="h-full rounded-full bg-ok transition-[width] duration-500 motion-reduce:transition-none"
            style={{ width: `${Math.round(Math.max(0, Math.min(1, ratio)) * 100)}%` }}
          />
        </div>
      ) : null}
    </div>
  )
}
