import { Badge } from '@/components/ui/badge'
import type { LogEntry } from '@/lib/api'
import { levelBadgeClass, statusBadgeClass } from '@/lib/present'
import { useI18n } from '@/lib/i18n'

export function LogTable({ entries }: { entries: LogEntry[] }) {
  const { t } = useI18n()

  return (
    <div className="max-h-[calc(100vh-15rem)] overflow-auto">
      <table>
        <thead>
          <tr>
            <th>{t('colTime')}</th>
            <th>{t('colLevel')}</th>
            <th>{t('colKind')}</th>
            <th>{t('colModel')}</th>
            <th>{t('colProvider')}</th>
            <th className="text-right">{t('colStatus')}</th>
            <th className="text-right">ms</th>
            <th>{t('colMessage')}</th>
          </tr>
        </thead>
        <tbody>
          {entries.length === 0 ? (
            <tr>
              <td colSpan={8} className="py-10 text-center text-muted-foreground">
                {t('empty')}
              </td>
            </tr>
          ) : (
            entries.map((entry) => (
              <tr key={entry.seq} className="transition-colors hover:bg-muted/40">
                <td className="font-mono text-[11px] whitespace-nowrap text-muted-foreground">
                  {entry.time}
                </td>
                <td>
                  <Badge variant="outline" className={levelBadgeClass(entry.level)}>
                    {entry.level}
                  </Badge>
                </td>
                <td className="text-xs text-muted-foreground">{entry.kind}</td>
                <td className="font-mono text-xs">
                  {entry.model}
                  {entry.stream ? (
                    <Badge variant="outline" className="ml-1.5 border-primary/30 text-primary">
                      sse
                    </Badge>
                  ) : null}
                  {entry.failover ? (
                    <Badge variant="outline" className="ml-1.5 border-warn/35 text-warn">
                      f/o
                    </Badge>
                  ) : null}
                </td>
                <td className="font-mono text-xs">{entry.provider}</td>
                <td className="text-right">
                  <Badge variant="outline" className={statusBadgeClass(entry.status)}>
                    {entry.status || '—'}
                  </Badge>
                </td>
                <td className="text-right font-mono text-xs tnum">
                  {entry.latency_ms ? Math.round(entry.latency_ms) : ''}
                </td>
                <td className="max-w-[34rem] text-xs break-words text-muted-foreground">
                  {entry.message}
                </td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  )
}
