import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { api, getApiKey, setApiKey } from '@/lib/api'

vi.hoisted(() => {
  vi.stubGlobal('localStorage', { getItem: () => null, setItem: () => {}, removeItem: () => {} })
})

describe('console API credentials', () => {
  beforeEach(() => {
    vi.stubGlobal('localStorage', { getItem: vi.fn(), setItem: vi.fn(), removeItem: vi.fn() })
    setApiKey('')
  })
  afterEach(() => { vi.unstubAllGlobals() })

  it('uses the entered key for same-origin requests with a bounded wait', async () => {
    const fetcher = vi.fn().mockResolvedValue(new Response('{"data":[]}', { status: 200 }))
    vi.stubGlobal('fetch', fetcher)
    setApiKey(' new-key ')
    await expect(api.models()).resolves.toEqual({ data: [] })
    expect(fetcher).toHaveBeenCalledWith('/v1/models', expect.objectContaining({
      headers: { Accept: 'application/json', 'x-api-key': 'new-key' },
      signal: expect.any(AbortSignal),
    }))
  })

  it('ignores old authentication responses after the key changes', async () => {
    let respond!: (response: Response) => void
    vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>((resolve) => { respond = resolve })))
    setApiKey('old-key')
    const request = api.status()
    setApiKey('new-key')
    respond(new Response('{"error":{"message":"unauthorized"}}', { status: 401 }))
    await expect(request).rejects.toMatchObject({ name: 'AbortError' })
  })

  it('keeps a key usable when browser storage is disabled', () => {
    vi.stubGlobal('localStorage', { setItem: () => { throw new Error('disabled') } })
    expect(() => setApiKey('memory-key')).not.toThrow()
    expect(getApiKey()).toBe('memory-key')
  })
})
