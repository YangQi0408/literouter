import { AlertTriangle, CircleAlert, Info } from 'lucide-react'

import type { IssueLevel, ValidationIssue } from '@/lib/api'
import { cn } from '@/lib/utils'

const STYLES: Record<IssueLevel, string> = {
  error: 'border-destructive/35 bg-destructive/10 text-destructive',
  warning: 'border-warn/35 bg-warn/10 text-warn',
  info: 'border-primary/30 bg-primary/10 text-primary',
}

function Icon({ level }: { level: IssueLevel }) {
  if (level === 'error') return <CircleAlert className="mt-px size-3.5 shrink-0" />
  if (level === 'warning') return <AlertTriangle className="mt-px size-3.5 shrink-0" />
  return <Info className="mt-px size-3.5 shrink-0" />
}

export function Issues({
  issues,
  className,
  emptyHint,
}: {
  issues: ValidationIssue[]
  className?: string
  emptyHint?: string
}) {
  if (!issues.length) {
    return emptyHint ? <p className={cn('text-xs text-muted-foreground', className)}>{emptyHint}</p> : null
  }
  return (
    <div className={cn('flex max-h-40 flex-col gap-1.5 overflow-auto', className)}>
      {issues.map((issue, index) => (
        <div
          key={`${issue.path}-${index}`}
          className={cn(
            'flex items-start gap-2 rounded-md border px-2.5 py-1.5 text-xs leading-snug',
            STYLES[issue.level] ?? STYLES.info,
          )}
        >
          <Icon level={issue.level} />
          <span className="font-mono text-[11px] opacity-70">{issue.path}</span>
          <span className="min-w-0 flex-1">{issue.message}</span>
        </div>
      ))}
    </div>
  )
}
