# Routing & Failover Mechanics

This document explains the core decision engine of `literouter`: candidate chain generation, the "Header Gate" principle for streaming failover, and the circuit breaker state machine.

---

## Table of Contents

- [Architectural Separation](#architectural-separation)
- [Candidate Chain Generation](#candidate-chain-generation)
  - [Phase 1: Explicit Route Matching](#phase-1-explicit-route-matching)
  - [Phase 2: Global Pass-through](#phase-2-global-pass-through)
  - [Phase 3: 404 Fallback](#phase-3-404-fallback)
- [Header Gate & Lossless Streaming Failover](#header-gate--lossless-streaming-failover)
  - [Why Streaming Requires a Header Gate](#why-streaming-requires-a-header-gate)
  - [Retriable Status Codes & Decision Matrix](#retriable-status-codes--decision-matrix)
- [Circuit Breaker State Machine](#circuit-breaker-state-machine)
  - [Three-State Lifecycle](#three-state-lifecycle)
  - [Purity Principle: Preventing False Trips](#purity-principle-preventing-false-trips)
  - [All-Tripped Graceful Degrade](#all-tripped-graceful-degrade)
- [Attempt Budget Control (`max_attempts`)](#attempt-budget-control-max_attempts)

---

## Architectural Separation

In `literouter`, **decision logic is strictly decoupled from network I/O**:

- The `Router` class (in `core/src/lr_router.cpp`) is a pure deterministic state machine holding no network sockets or thread handles.
- Given a requested model name and provider health states, it produces an ordered array of `Candidate` targets.
- This design ensures that candidate selection, tie-breaking, and circuit breaker logic are 100% reproducible and testable via offline unit tests.

---

## Candidate Chain Generation

When a client sends a chat completion, embedding, or text completion request, `Router::candidatesFor(model)` builds the candidate list in three deterministic phases:

```text
Client requests model: "gpt-4o"
       │
       ▼
[Phase 1: Explicit Route Match]
 Does config.routes have model == "gpt-4o" && enabled == true ?
       ├── YES ──▶ Assemble candidate chain strictly matching targets order
       │
       └── NO  ──▶ [Phase 2: Global Pass-through]
                   Is server.pass_through_unknown == true ?
                         ├── YES ──▶ Scan all enabled providers
                         │           Filter those declaring "gpt-4o" in models
                         │           Sort: priority ASC -> weight DESC
                         │
                         └── NO  ──▶ [Phase 3: Unmatched Fallback]
                                     Return HTTP 404 model_not_found
```

### Phase 1: Explicit Route Matching

If a route is defined in the configuration, e.g.:

```json
{
  "model": "gpt-4o",
  "enabled": true,
  "targets": [
    { "provider": "openai-direct", "model": "gpt-4o-2024-08-06" },
    { "provider": "azure-openai", "model": "gpt-4o-eastus" },
    { "provider": "backup-relay" }
  ]
}
```

1. Candidates are tried **strictly in the order declared in `targets`**. This is authoritative human configuration and will not be reordered.
2. **Physical Model Mapping**: If `model` is specified on a target (e.g. `gpt-4o-2024-08-06`), the request payload model name is rewritten before dispatch. If omitted, the client's logical model name (`gpt-4o`) is forwarded.

### Phase 2: Global Pass-through

When a model is unlisted in `routes` and `pass_through_unknown: true`:

1. Scan all enabled providers (`enabled: true`);
2. Filter providers whose `models` array contains the requested model name;
3. **Deterministic Sorting**:
   - Primary key: `priority` ascending (lower number = higher priority);
   - Tie breaker: `weight` descending (higher number = earlier candidate);
   - Circuit-broken providers are deprioritized or omitted according to `skip_open_circuits`.

### Phase 3: 404 Fallback

If neither phase produces candidates, `literouter` returns a standard OpenAI-compatible 404 response:

```json
{
  "error": {
    "message": "The model `xxx` does not exist or you do not have access to it.",
    "type": "invalid_request_error",
    "param": "model",
    "code": "model_not_found"
  }
}
```

---

## Header Gate & Lossless Streaming Failover

### Why Streaming Requires a Header Gate

In Server-Sent Events (SSE) streaming, uncoordinated failover leads to severe corruptions:

> **Traditional Proxy Pitfall**: An upstream provider streams a partial answer (e.g. `"The capital of France is"`) and then disconnects. If the proxy fails over to a backup provider, the backup starts an entirely new generation from scratch (`"Hello! I am an AI assistant..."`), creating an interleaved, hallucinated mess in the client.

To solve this, `literouter` enforces the **Header Gate**:

```text
Client initiates SSE streaming request
       │
       ▼
 Attempt Candidate 1 (Provider A)
       │
       ├─▶ [Before first byte / headers reach client] Upstream error (timeout / 502 / 429)
       │   └── Gate remains OPEN ──▶ Seamlessly switch to Provider B (transparent to client)
       │
       └─▶ [First data packet arrives] ──▶ Emit HTTP 200 and headers to client
           └── 【HEADER GATE PERMANENTLY CLOSES】
               └── Lifecycle committed to Provider A
                   └── If connection drops later, terminate stream cleanly. NEVER stitch another stream!
```

### Retriable Status Codes & Decision Matrix

| Stage / Status Code | Failure Category | Trigger Failover? | Action / Explanation |
|---|---|:---:|---|
| **Network Failure** | Refused / DNS error / TLS handshake error / Timeout | ✅ **YES** | Upstream did not process request; safely retry next candidate |
| **HTTP 408** | Request Timeout | ✅ **YES** | Retry next candidate |
| **HTTP 409 / 425** | Conflict / Too Early | ✅ **YES** | Retry next candidate |
| **HTTP 429** | Rate Limited / Quota Exceeded | ✅ **YES** | Fail over to backup provider |
| **HTTP 500 ~ 599** | Server Error / Bad Gateway / Unavailable | ✅ **YES** | Upstream crashed or overloaded; fail over immediately |
| **HTTP 400** | Bad Request (malformed prompt / parameters) | ❌ **NO** | Client-side error; forward response directly to client |
| **HTTP 401 / 403** | Unauthorized / Forbidden (Invalid key or WAF block) | ❌ **NO** | Auth failure; forward to client to prompt key check |
| **HTTP 404** | Not Found (model or endpoint absent) | ❌ **NO** | Forward to client |
| **After First Byte** | Any exception after headers were sent to client | ❌ **NO** | **Gate is closed**; terminate stream to avoid cross-stream stitching |

---

## Circuit Breaker State Machine

Each provider maintains an independent circuit breaker state to prevent cascading latency spikes caused by downed upstreams.

### Three-State Lifecycle

```text
            consecutive_failures < threshold
             ┌────────────────┐
             │                │
             ▼                │
     ┌──────────────┐   failures >= threshold   ┌──────────────┐
     │   Healthy    │ ────────────────────────▶ │     Open     │
     │   (Closed)   │                           │  (Cooldown)  │
     └──────────────┘                           └──────────────┘
             ▲                                         │
             │                                         │ circuit_cooldown_sec elapsed
             │ Probe succeeded                         ▼
             │                                  ┌──────────────┐
             └───────────────────────────────── │   Degraded   │
                          Probe failed          │ (Half-Open)  │
                         ──────────────────────▶└──────────────┘
```

1. **Healthy (Closed)**: Normal routing. Requests dispatched by priority and weight.
2. **Open**: Triggered when retriable failures reach `circuit_failure_threshold` (default 3).
   - During `circuit_cooldown_sec` (default 30s), the provider is moved to the tail of candidate chains (or skipped if `skip_open_circuits` is active).
   - **Exception — the upstream named its own window**: a 429/5xx carrying `Retry-After` (seconds) opens the breaker on a **single** failure, for `max(circuit_cooldown_sec, Retry-After)` (capped at 24 hours). The relay has said when it will be ready, so hammering it while the window runs buys nothing; and a window shorter than the local policy is raised to the policy, so an upstream cannot talk the operator into coming back sooner than they allowed. The HTTP-date form is not parsed, and such a response falls back to the ordinary strike counting.
3. **Degraded (Half-Open)**: After cooldown expires, the provider transitions to degraded status.
   - The next request acts as a **Probe**;
   - If probe succeeds: state transitions immediately to **Healthy**, failure count reset to 0;
   - If probe fails: trips back to **Open** for another cooldown cycle.

### Purity Principle: Preventing False Trips

Client actions should never trip an upstream's health status:

- **Client Aborted Requests**: If the user cancels generation or closes the browser tab, the socket disconnection is **never counted as a circuit failure**.
- **Client Syntax Errors (HTTP 400)**: Invalid parameters or context window overruns are **never counted as a circuit failure**.

### All-Tripped Graceful Degrade

If all configured providers for a model are in the `Open` state, `literouter` will not blindly fail. Instead, it **selects the provider whose last failure is oldest** (longest elapsed cooldown) to attempt recovery.

---

## Attempt Budget Control (`max_attempts`)

### Candidate Ordering Policy (`routing_policy`)

The chain is ordered by `priority` (then `weight`) by default, which is the answer to "which relay did I declare should be tried first". If you would rather trust **measurements**, hand the decision to a policy:

| Value | Orders by | Fits |
|---|---|---|
| `priority` (default) | the declared order | you know the relays' trade-offs and want to keep control |
| `fastest` | measured **p95** latency (unmeasured last) | several relays serve the model and their latency differs |
| `cheapest` | input price + output price (unpriced last) | several relays serve the model and their price differs |

Three rules keep it from becoming "random order":

1. **ties keep the priority order** (the sort is stable), so a policy only moves a relay when it has a reason to;
2. **a relay with no measurement sorts after the measured ones** — it has not earned the front, and it is not disqualified either;
3. **session affinity outranks both**: a relay that has already answered this conversation still moves to the front, because a prompt-cache hit is the more specific and more certain benefit (see above).

`literouter bench --model <m>` shows what each relay's latency and price actually are before you pick a policy.

### Session Affinity (`session_affinity_sec`)

The candidate chain is ordered by `priority`/`weight`, which answers "which relay deserves to be tried first" and cannot answer a different question: **which relay has already seen this conversation**.

Providers cache repeated prompt prefixes, and a cache hit is both cheaper and faster to start answering. On a long context (a repository, a document, a long system prompt) that gap can be an order of magnitude in both time-to-first-token and spend. So `server.session_affinity_sec` (0, the default, disables it; around 300 is a sensible value for chat clients):

- it fingerprints a request by its **system prompt plus first message** — the parts a conversation keeps identical, and the parts a cache keys on — and maps that to the relay that last **answered it usefully**;
- when that relay is still a candidate and is not being skipped, it moves to the front of the chain; everything else keeps its order;
- only successes are recorded: pinning a conversation to whatever just answered "400 context length exceeded" would bake the failure in;
- entries expire and the table is bounded (expired first, then the oldest), so it cannot grow without limit.

Setting it back to 0 restores plain priority order.

`max_attempts` bounds how many relays are tried and `timeout_sec` bounds one attempt; their product is the worst case a client can wait, and ten candidates at 120 seconds each is twenty minutes. `server.request_deadline_sec` (0 disables it, the default) bounds the **whole request**:

- checked **between attempts**: with less than half a second left the remaining candidates are not tried at all, the request answers **504** (not 503 — the relays may be perfectly fine, they were simply not given the chance), and the log names the relay that was never tried;
- it also narrows the wait for a **streamed answer's first byte**: nothing has been sent to the client at that point, so giving up is a failover rather than a truncation;
- **an answer that has started is never cut short**, which is the header gate's rule again: bytes already committed belong to the client, and truncating them is worse than letting the answer finish. The real elapsed time can therefore exceed the deadline by one attempt's own timeout.

Validation warns below 5 seconds, where a second candidate never gets its turn.

Configured via `server.max_attempts`:
- `0` (default): Try every candidate in the chain until one succeeds or all fail.
- `N` (e.g. `2` or `3`): Limit attempts to at most `N` providers per request, preventing excessive wait times when multiple upstreams are unavailable.


---

## Relay-side Limits

`providers[].max_concurrent` / `requests_per_minute` answer "how much is literouter itself sending to this relay".

**A full relay is not a broken relay.** A candidate at its limit is therefore **skipped and the next one tried**, with no breaker failure counted and no breaker opened — a relay serving at full capacity with no headroom left should not be judged as failing. The log says `skipping a full relay` and names the reason ("already has N request(s) in flight" or "has taken N request(s) in the last minute").

When the whole chain is full the client gets **`429` with a real `Retry-After`** (the seconds until the oldest start in the rolling window leaves it) and the error code `relay_capacity_exceeded`. A 429 rather than a 503 because it carries an actionable "come back then", which is exactly what a client needs.

Two implementation details worth knowing:

- **Every start is recorded in the window**, even while no limit is set. Recording only when limited would give an operator a free first minute after turning a limit on — precisely when the limit was wanted.
- The window has a fixed ceiling, so the memory cost of counting is a **constant per relay**, unrelated to traffic.

## Local Response Cache

With `server.response_cache_ttl_sec` on (`0`, the default, disables it), an **identical** non-streaming request is answered from memory until the TTL expires instead of being sent upstream. A hit carries `X-Literouter-Cache: hit` (a store carries `miss`), the log records `cache` in the `provider` column, and the request total still goes up — but **no relay statistic moves and no tokens are charged**.

The key is the SHA-256 of ingress protocol + logical model + the normalised request body, joined with length prefixes so that moving a character across a field boundary is a different key.

**Not** cached: streaming requests (replaying a stored body as an event stream would mean inventing chunk boundaries and timing, and a client that measures time-to-first-token would be lied to), audio and images (the request is multipart and the answer may be binary), and any non-2xx answer (caching one 400 for the whole TTL turns a transient problem into a permanent one). Turning the cache off drops what it held, so re-enabling it cannot serve an answer from before it was switched off.

It **complements** session affinity rather than replacing it: affinity makes the **upstream's** prompt cache likelier to hit, and the local cache stops an exactly repeated request from going out at all. The first saves the price of that input prefix; the second saves the whole call.
