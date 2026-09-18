import { Component, type ErrorInfo, type ReactNode } from 'react'

import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
/** Keeps one broken render from taking the whole console down.
 *
 *  Without this, any exception thrown while a page renders unmounts the entire
 *  tree and the operator is left looking at an empty window with no clue which
 *  view failed or why — which is exactly what a missing `headers` key on an
 *  OpenAI relay used to do when the edit dialog was opened. The boundary shows
 *  the message instead, and the page can be retried without reloading. */
export class ErrorBoundary extends Component<
  {
    children: ReactNode
    resetKey?: string
    // A class component cannot call the i18n hook, so the shell passes the
    // three strings it needs. The English defaults keep it usable on its own.
    labels?: { title: string; note: string; retry: string }
  },
  { error: Error | null }
> {
  state: { error: Error | null } = { error: null }

  static getDerivedStateFromError(error: Error) {
    return { error }
  }

  componentDidUpdate(previous: { resetKey?: string }) {
    // Switching tabs clears the failure: the next page is a different component
    // and may well render fine.
    if (previous.resetKey !== this.props.resetKey && this.state.error) {
      this.setState({ error: null })
    }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('literouter console: render failed', error, info.componentStack)
  }

  render() {
    if (!this.state.error) {
      return this.props.children
    }
    const labels = this.props.labels ?? {
      title: 'This view failed to render',
      note: 'The rest of the console is still working. Switch tabs, or retry.',
      retry: 'Retry',
    }
    return (
      <Card>
        <CardHeader>
          <CardTitle>{labels.title}</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-3">
          <p className="text-xs text-muted-foreground">{labels.note}</p>
          <pre className="overflow-auto rounded-md bg-muted/40 p-3 font-mono text-[11px] text-destructive">
            {this.state.error.message}
          </pre>
          <div>
            <Button size="sm" onClick={() => this.setState({ error: null })}>
              {labels.retry}
            </Button>
          </div>
        </CardContent>
      </Card>
    )
  }
}
