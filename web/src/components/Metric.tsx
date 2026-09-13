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
}

export function Metric({ icon: Icon, label, value, sub, ratio, long, danger }: MetricProps) {
  return (
    <div className="group relative overflow-hidden rounded-xl border bg-card p-3.5 shadow-xs transition-all hover:-translate-y-px hover:border-border/80">
      <span
        className={cn(
          'absolute inset-x-0 top-0 h-px bg-gradient-to-r to-transparent opacity-60',
          danger ? 'from-destructive' : 'from-primary',
        )}
      />
      <div className="flex items-center gap-2 text-xs text-muted-foreground">
        <span
          className={cn(
            'flex size-[22px] items-center justify-center rounded-md',
            danger ? 'bg-destructive/15 text-destructive' : 'bg-primary/15 text-primary',
          )}
        >
          <Icon className="size-3.5" />
        </span>
        <span className="truncate">{label}</span>
      </div>
      <div
        className={cn(
          'mt-2 font-mono tracking-tight text-foreground',
          long ? 'text-base break-all' : 'text-2xl tnum',
        )}
      >
        {value}
      </div>
      {ratio !== undefined ? (
        <div className="mt-2 h-1 w-full overflow-hidden rounded-full bg-muted">
          <div
            className="h-full rounded-full bg-gradient-to-r from-ok to-primary transition-[width] duration-500"
            style={{ width: `${Math.round(ratio * 100)}%` }}
          />
        </div>
      ) : null}
      {sub ? <div className="mt-1.5 truncate font-mono text-[11px] text-muted-foreground">{sub}</div> : null}
    </div>
  )
}
