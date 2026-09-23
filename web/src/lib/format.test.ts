import { describe, expect, it } from 'vitest'

import { humanBytes, humanCount, humanDuration, humanMillis, humanUptime, pct, successRatio } from '@/lib/format'

/** These tables are not invented here: they are the same values core/tests/
 *  test_util.cpp pins for the C++ formatters, which the CLI uses.
 *  The two implementations exist because one of them has to run in a browser,
 *  and the only thing that keeps them in step is a test that says so. */
describe('humanUptime keeps counting to the second', () => {
  it.each([
    [-1, '—'],
    [0, '0s'],
    [0.9, '0s'],
    [59, '59s'],
    [60, '1m 0s'],
    [192, '3m 12s'],
    [3599, '59m 59s'],
    [3600, '1h 0m 0s'],
    [3725, '1h 2m 5s'],
    [86399, '23h 59m 59s'],
    [86400, '1d 0h 0m 0s'],
    [90061, '1d 1h 1m 1s'],
  ])('humanUptime(%i) = %s', (seconds, want) => {
    expect(humanUptime(seconds as number)).toBe(want)
  })

  it('does not round an hour into minutes the way humanDuration does', () => {
    // The whole reason it exists: a running total that a tile repaints every
    // second must not sit still for a minute at a time.
    expect(humanDuration(3725)).toBe('1h 2m')
    expect(humanUptime(3725)).toBe('1h 2m 5s')
  })
})

describe('humanDuration compacts', () => {
  it.each([
    [0, '0s'],
    [59, '59s'],
    [60, '1m 0s'],
    [3599, '59m 59s'],
    [3600, '1h 0m'],
    [3720, '1h 2m'],
    [86399, '23h 59m'],
    [86400, '1d 0h'],
  ])('humanDuration(%i) = %s', (seconds, want) => {
    expect(humanDuration(seconds)).toBe(want)
  })
})

describe('the compact metric formats', () => {
  it.each([
    [0, '0'],
    [999, '999'],
    [1500, '1.5k'],
    [12000, '12k'],
    [2500000, '2.5M'],
    [3000000000, '3.0G'],
  ])('humanCount(%i) = %s', (value, want) => {
    expect(humanCount(value)).toBe(want)
  })

  it.each([
    [512, '512 B'],
    [2048, '2.0 KB'],
    [5 * 1024 * 1024, '5.0 MB'],
  ])('humanBytes(%i) = %s', (value, want) => {
    expect(humanBytes(value)).toBe(want)
  })

  it.each([
    [0.4, '0.4ms'],
    [42, '42ms'],
    [1500, '1.5s'],
  ])('humanMillis(%i) = %s', (ms, want) => {
    expect(humanMillis(ms)).toBe(want)
  })
})

describe('percentages', () => {
  it('reads a whole as a whole', () => {
    expect(pct(5, 5)).toBe('100%')
    expect(pct(1, 2)).toBe('50.0%')
    // Not a division by zero, and not a fake 0%: nothing has happened yet.
    expect(pct(0, 0)).toBe('—')
  })

  it('keeps the bar inside its track', () => {
    expect(successRatio(0, 0)).toBe(0)
    expect(successRatio(1, 4)).toBe(0.25)
    // A partial count from a restart can exceed the total it is compared with.
    expect(successRatio(9, 8)).toBe(1)
  })
})
