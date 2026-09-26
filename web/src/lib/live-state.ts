import type { LogEntry, ProviderProbe } from '@/lib/api'

export function probeSucceeded(probe: ProviderProbe): boolean {
  return probe.reachable && probe.status >= 200 && probe.status < 300
}

/** Repeated or overlapping log pages must not duplicate rows in the console. */
export function appendLogEntries(previous: LogEntry[], received: LogEntry[], limit: number): LogEntry[] {
  const entries = new Map(previous.map((entry) => [entry.seq, entry]))
  for (const entry of received) entries.set(entry.seq, entry)
  return [...entries.values()].sort((a, b) => a.seq - b.seq).slice(-limit)
}
