/** The same compact formatting the C++ front ends use, so a number reads the
 *  same in the console as it does in `literouter status`. */

export function humanCount(value: number): string {
  const n = Number(value || 0)
  if (n < 1000) return String(n)
  if (n < 1e6) return `${(n / 1e3).toFixed(n < 1e4 ? 1 : 0)}k`
  if (n < 1e9) return `${(n / 1e6).toFixed(n < 1e7 ? 1 : 0)}M`
  return `${(n / 1e9).toFixed(1)}G`
}

export function humanBytes(value: number): string {
  const n = Number(value || 0)
  if (n < 1024) return `${n} B`
  if (n < 1024 ** 2) return `${(n / 1024).toFixed(1)} KB`
  if (n < 1024 ** 3) return `${(n / 1024 ** 2).toFixed(1)} MB`
  return `${(n / 1024 ** 3).toFixed(2)} GB`
}

export function humanMillis(ms: number): string {
  const n = Number(ms || 0)
  if (n < 1000) return `${n.toFixed(n < 10 ? 1 : 0)}ms`
  return `${(n / 1000).toFixed(1)}s`
}

export function humanDuration(seconds: number): string {
  const s = Math.max(0, Math.floor(Number(seconds) || 0))
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  const r = s % 60
  if (d) return `${d}d ${h}h`
  if (h) return `${h}h ${m}m`
  if (m) return `${m}m ${r}s`
  return `${r}s`
}

export function pct(part: number, whole: number): string {
  if (!whole) return '—'
  return `${((part / whole) * 100).toFixed(part === whole ? 0 : 1)}%`
}

export function successRatio(part: number, whole: number): number {
  if (!whole) return 0
  return Math.max(0, Math.min(1, part / whole))
}
