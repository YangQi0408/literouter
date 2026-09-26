import type { AppConfig } from '@/lib/api'
import { normalizeConfig, PROVIDER_DEFAULTS, ROUTE_DEFAULTS, SERVER_DEFAULTS } from '@/lib/normalize'

export class ConfigImportError extends Error {
  constructor(public readonly path: string) {
    super(path)
    this.name = 'ConfigImportError'
  }
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === 'object' && !Array.isArray(value)

function checkObject(value: unknown, defaults: object, path: string) {
  if (!isRecord(value)) throw new ConfigImportError(path)
  for (const [key, sample] of Object.entries(defaults)) {
    const candidate = value[key]
    if (candidate === undefined || candidate === null) continue
    const valid = Array.isArray(sample) ? Array.isArray(candidate)
      : typeof sample === 'object' ? isRecord(candidate)
      : typeof candidate === typeof sample && (typeof candidate !== 'number' || Number.isFinite(candidate))
    if (!valid) throw new ConfigImportError(`${path}.${key}`)
  }
  for (const key of ['api_key_clear']) {
    if (value[key] !== undefined && typeof value[key] !== 'boolean') throw new ConfigImportError(`${path}.${key}`)
  }
}

/** Reject malformed imports before they replace a usable draft. Omitted fields
 * still receive the same defaults as an older server configuration. */
export function readConfigImport(raw: unknown): AppConfig {
  if (!isRecord(raw) || !['server', 'providers', 'routes'].some((key) => key in raw)) {
    throw new ConfigImportError('')
  }
  if (raw.schema !== undefined && (typeof raw.schema !== 'number' || !Number.isInteger(raw.schema))) {
    throw new ConfigImportError('schema')
  }
  if (raw.server !== undefined) checkObject(raw.server, SERVER_DEFAULTS, 'server')
  for (const [key, defaults] of [['providers', PROVIDER_DEFAULTS], ['routes', ROUTE_DEFAULTS]] as const) {
    const list = raw[key]
    if (list === undefined) continue
    if (!Array.isArray(list)) throw new ConfigImportError(key)
    list.forEach((item, index) => {
      const path = `${key}[${index}]`
      checkObject(item, defaults, path)
      if (key === 'providers') {
        if (Array.isArray(item.models) && item.models.some((model: unknown) => typeof model !== 'string')) {
          throw new ConfigImportError(`${path}.models`)
        }
        if (isRecord(item.headers) && Object.values(item.headers).some((value) => typeof value !== 'string')) {
          throw new ConfigImportError(`${path}.headers`)
        }
        if (isRecord(item.model_prices)) {
          for (const [model, price] of Object.entries(item.model_prices)) {
            checkObject(price, { price_in_per_million: 0, price_out_per_million: 0 }, `${path}.model_prices.${model}`)
          }
        }
      } else if (Array.isArray(item.targets)) {
        item.targets.forEach((target: unknown, targetIndex: number) => {
          checkObject(target, { provider: '', model: '' }, `${path}.targets[${targetIndex}]`)
        })
      }
    })
  }
  return normalizeConfig(raw)
}
