import { clsx, type ClassValue } from 'clsx'
import { twMerge } from 'tailwind-merge'

/** shadcn's class combiner: conditional classes, last-one-wins for Tailwind. */
export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs))
}
