// literouter.core — the public surface of the routing engine.
//
// Everything a front end needs lives here: the config model and its on-disk
// form, the routing/failover decision layer, the HTTP proxy itself, and the
// telemetry the console renders. `literouter.cli` imports this module and
// nothing else from the engine.
//
// The module deliberately exports no third-party type. httplib and nlohmann's
// json stay in the implementation units, so a front end can be rebuilt without
// the proxy's dependency graph leaking into its own translation units.
export module literouter.core;

export import std;

export namespace literouter {

// ─────────────────────────────────────────────────────────────────────────────
// Identity
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::string_view kVersion = "0.2.0";
inline constexpr std::string_view kUserAgent = "literouter/0.2.0";

// The config schema this build reads and writes. Bumping it is what gives a
// breaking rename somewhere to live: `migrateConfigJson` walks an older document
// up to this number, and a document from a NEWER build is refused rather than
// silently loaded minus the fields this build has never heard of — losing those
// fields on the next save is exactly the kind of quiet damage a tolerant reader
// would cause. Schema 3 is the single-user schema: the client-distribution
// fields (`clients`, provider `groups`, `ui_scale`, `language`) are gone, and a
// document that still carries them is migrated with a note naming what was
// dropped rather than silently shedding it.
inline constexpr int kConfigSchema = 3;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration model
//
// The on-disk file is JSON with a `schema` field; every struct below has a
// documented default so a hand-written minimal file (or an empty one) still
// loads. Secrets may be written literally or as a `${ENV_VAR}` reference —
// `resolveSecret` expands the latter at request time, which is what keeps an
// API key out of a file that might get synced or screenshotted.
// ─────────────────────────────────────────────────────────────────────────────

// Price per million tokens for a specific model, in US dollars.
struct ModelPricing {
    double price_in_per_million = 0.0;
    double price_out_per_million = 0.0;

    auto operator<=>(const ModelPricing &) const = default;
};

struct ProviderConfig {
    // Stable slug used by routes, logs and the management API. Unique.
    std::string id;
    // Human label. Falls back to `id` when empty.
    std::string name;
    // Upstream root, e.g. "https://api.openai.com/v1". A trailing slash is
    // tolerated; a path component is preserved and prefixed onto every call.
    std::string base_url;
    // Literal key, or "${SOME_ENV_VAR}", or empty for an unauthenticated relay.
    std::string api_key;
    bool enabled = true;
    // Lower wins. Ties are broken by declaration order, which keeps a
    // hand-edited file deterministic.
    int priority = 100;
    // Relative offset within the same priority band; only used when two
    // candidates share a priority, and only as a tie-break weight.
    int weight = 1;
    // Per-request deadline for the upstream leg, in seconds.
    int timeout_sec = 120;
    // Seconds before a connect attempt is abandoned.
    int connect_timeout_sec = 15;
    bool supports_stream = true;
    // Model ids this relay advertises. Used by /v1/models and by the fallback
    // matcher when a request names a model no route mentions.
    std::vector<std::string> models;
    // Extra headers sent upstream (organisation ids, referer, ...).
    std::map<std::string, std::string> headers;
    // Path appended to base_url. Overridable because a handful of relays put
    // chat completions somewhere other than /chat/completions.
    std::string chat_path = "/chat/completions";
    std::string embeddings_path = "/embeddings";
    // Upstream API protocol: "openai" (default), "anthropic", "gemini",
    // "openai_responses", "azure" (Azure OpenAI: same JSON as openai, different
    // path, `api-key` header and api-version query), "vertex" (Vertex AI: the
    // Gemini generateContent JSON, OAuth2 bearer, project/location path),
    // "bedrock" (AWS Bedrock Converse API: its own body, SigV4-signed), "ollama"
    // (its own /api/chat body, no auth by default).
    std::string protocol = "openai";
    // Azure: the `api-version` query value. Vertex: the API version segment.
    // Empty picks the protocol's own default.
    std::string api_version;
    // Bedrock region (`us-east-1`), or the Vertex location (`us-central1`).
    // Bedrock requires it; Vertex defaults to `us-central1` when empty.
    std::string region;
    // Vertex project id. Required by the Vertex path builder.
    std::string project;
    // Vertex: path to a service-account JSON key. Its private key signs the
    // OAuth2 assertion exchanged for an access token, which is cached until it
    // is nearly expired. Unreadable or malformed is a startup-time config error.
    std::string credentials_file;
    // Bedrock SigV4 credentials. `${ENV_VAR}` references work exactly as they
    // do for api_key and are resolved only when a request is signed.
    std::string aws_access_key;
    std::string aws_secret_key;
    // Bedrock: an STS session token, when the credentials above are temporary.
    std::string aws_session_token;
    // Relay-side protection: this bounds what literouter itself sends to one
    // relay. 0 disables each. A relay at its limit is skipped like an open breaker and
    // the next candidate is tried, so a busy relay degrades to a slower answer
    // rather than to a failure.
    int max_concurrent = 0;
    int requests_per_minute = 0;
    // What this relay charges per million tokens, in US dollars. 0 means "not
    // written down", and such a relay contributes nothing to the reported cost
    // rather than a made-up number: an estimate built on a guess is worse than
    // no estimate. Tokens are the relay's own report, so a relay that never
    // reports usage costs nothing here.
    double price_in_per_million = 0.0;
    double price_out_per_million = 0.0;
    // Model-specific pricing overrides. When a request matches a model here
    // (checked by upstream model name, then by client model name), these prices
    // take precedence over the relay's default prices above.
    std::map<std::string, ModelPricing> model_prices;
    // Free-form note shown in the console.
    std::string note;

    // Resolves the effective {input, output} prices in $/1M tokens for the given
    // model, falling back to the provider's default prices if no override is set.
    std::pair<double, double> pricesFor(std::string_view upstream_model,
                                        std::string_view client_model = "") const {
        if (!upstream_model.empty()) {
            if (auto it = model_prices.find(std::string(upstream_model)); it != model_prices.end()) {
                return {it->second.price_in_per_million, it->second.price_out_per_million};
            }
        }
        if (!client_model.empty() && client_model != upstream_model) {
            if (auto it = model_prices.find(std::string(client_model)); it != model_prices.end()) {
                return {it->second.price_in_per_million, it->second.price_out_per_million};
            }
        }
        return {price_in_per_million, price_out_per_million};
    }
};

// One hop of a failover chain: "ask this relay for this upstream model id".
struct RouteTarget {
    std::string provider;
    // Empty means "ask the upstream for the logical name unchanged".
    std::string model;
};

struct RouteConfig {
    // The name a client sends. Also what /v1/models advertises.
    std::string model;
    // Ordered: index 0 is tried first, each later entry is a failover.
    std::vector<RouteTarget> targets;
    bool enabled = true;
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 8787;
    // PEM chain and unencrypted private key. Both empty keep HTTP; both set
    // enable HTTPS. Absolute paths are required. Listener changes need restart.
    std::string tls_cert_file;
    std::string tls_key_file;
    // When set, /v1/* requires `Authorization: Bearer <this>` (or matching
    // `x-api-key`). Empty disables the check — the default, because binding to
    // loopback is already the boundary on a single-user machine.
    std::string api_key;
    // A model the routes do not mention is still forwarded to every enabled
    // relay whose `models` list contains it, ordered by priority. When this is
    // false such a request fails with 404 instead.
    bool pass_through_unknown = true;
    // Total candidates tried before giving up. 0 means "every candidate".
    int max_attempts = 0;
    // How the candidate chain is ordered before the first attempt:
    //   "priority" — the operator's own order (default);
    //   "fastest"  — relays with a measurement first, by p95 latency, so a slow
    //                relay stops being tried first just because it was declared
    //                first;
    //   "cheapest" — priced relays first, by input+output price per million;
    //                an unpriced relay sorts last because its cost is unknown
    //                rather than zero;
    //   "round_robin" — rotate the first relay between requests, so equal relays
    //                   share traffic instead of one always winning. A relay's
    //                   model fallback chain stays together, so this balances
    //                   relays, not individual model attempts.
    // Ties keep the priority order, and session affinity still wins over every
    // policy:
    // which relay has already seen this conversation is the more specific fact.
    std::string routing_policy = "priority";
    // How long the whole request may take, across every candidate, in seconds.
    // 0 disables it. `timeout_sec` bounds one attempt and `max_attempts` bounds
    // how many there are, which together can mean several minutes on a chain of
    // slow relays — this is the bound a client actually cares about. It is
    // checked between attempts and while waiting for a streamed answer to start,
    // never after the answer has begun: once bytes are committed the response
    // belongs to the client, and truncating it would be worse than finishing it.
    int request_deadline_sec = 0;
    // How long a conversation's preference for one relay is remembered, in
    // seconds. 0 disables it. With it on, a follow-up turn of a conversation a
    // relay has already answered is tried on that relay first, because the
    // provider can reuse the cached prefix of the prompt: on a long context that
    // is real money and real latency, and priority order cannot know it.
    int session_affinity_sec = 0;
    // Watch the config file and apply it when it changes, within a few seconds.
    // Off by default: a config that changes under a running proxy is a surprise
    // unless the operator asked for it.
    bool reload_on_change = false;
    // Consecutive upstream failures that trip a relay's breaker.
    int circuit_failure_threshold = 3;
    // How long a tripped breaker stays open, in seconds.
    int circuit_cooldown_sec = 30;
    // A relay that is already open is skipped — unless every candidate is
    // open, in which case the least-recently-failed one is tried anyway.
    bool skip_open_circuits = true;
    // Ring buffer depth for the request log.
    int log_capacity = 200;
    // Retain request/response bodies in the log (truncated). Off by default:
    // prompts are the user's data and this is a plain-text file in a UI.
    bool log_bodies = false;
    // Bytes of a body kept when log_bodies is on.
    int log_body_limit = 2048;
    // Keep telemetry across restarts: the counters, the per-relay stats and the
    // newest slice of the request log are written to `<state dir>/telemetry-<port>.json`
    // and read back at startup, so `status` and the console do not reset to zero
    // every time the process is restarted. Bodies are only included when
    // log_bodies is also on. Turn this off for a run that must leave nothing
    // behind on disk.
    bool persist_telemetry = true;
    // Serve the built-in web console at /ui. On by default because the console
    // is the only telemetry surface a headless machine has; turn it off and the
    // routes do not exist at all. Keep it off — or set api_key — when the
    // listener is reachable from more than this machine: the console exposes the
    // request log, which can carry prompts.
    bool web_ui = true;
    // How wide one traffic-trend bucket is, in seconds, and how many are kept.
    // 3600 x 24 is the historical hour-by-day chart; 60 x 120 is the last two
    // hours at minute resolution, which is what a relay being debugged right now
    // wants. Values below 60 are raised to 60, because the persisted file and
    // the chart's own labels both assume a whole number of minutes.
    int traffic_bucket_sec = 3600;
    int traffic_bucket_count = 24;
    // Local response cache. 0 disables it. When on, an identical non-streaming
    // request (same client, protocol, model and body) is answered from memory
    // until the TTL expires instead of being sent upstream — the cheapest way to
    // stop paying twice for the same question. Streaming requests are never
    // cached, because a cached answer cannot be replayed as an event stream
    // without inventing timing the client would notice.
    int response_cache_ttl_sec = 0;
    // Entries kept before the least recently used one is dropped. Bounds memory:
    // each entry is a whole answer.
    int response_cache_max_entries = 128;
    // OpenTelemetry OTLP/HTTP endpoint for metrics, e.g.
    // "http://127.0.0.1:4318". Empty disables export. The payload is sent to
    // `{endpoint}/v1/metrics` every 30 seconds as OTLP JSON.
    std::string otlp_endpoint;
};

// Scheme-aware listener URL, with IPv6 brackets; path may be empty.
std::string serverBaseUrl(const ServerConfig &server, std::string_view path = {});

struct AppConfig {
    int schema = kConfigSchema;
    ServerConfig server;
    std::vector<ProviderConfig> providers;
    std::vector<RouteConfig> routes;

    // Convenience: nullptr when no provider carries this id.
    const ProviderConfig *provider(std::string_view id) const;
    ProviderConfig *provider(std::string_view id);
    const RouteConfig *route(std::string_view model) const;
    // Every configured route's model name, sorted and de-duplicated.
    std::vector<std::string> logicalModels() const;
    std::vector<std::string> routedModels() const;
    // All known models: configured routes plus provider-advertised models.
    std::vector<std::string> allModels() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Paths
//
// Config lives with the user's other dotfiles ($XDG_CONFIG_HOME or
// ~/.config); the pid file and any future cache live under the state dir.
// LITEROUTER_CONFIG / LITEROUTER_STATE_DIR override both, which is what the
// test suite uses to stay off the real home directory.
// ─────────────────────────────────────────────────────────────────────────────

std::filesystem::path userHome();
std::filesystem::path defaultConfigDir();
std::filesystem::path defaultConfigPath();
std::filesystem::path defaultStateDir();
// Where an instance keeps its telemetry between runs. Named by the port, like
// the pid file, because two instances are two histories — sharing one file meant
// the second writer silently replaced the first one's counters.
std::filesystem::path defaultTelemetryPath(int port);

// Where a running instance of this build records itself: one file per port,
// since the port is what identifies an instance. Written after a successful
// bind and removed on stop(), so a file left behind by a crash is
// distinguishable from a live instance by whether anything answers on the port.
std::filesystem::path defaultPidPath(int port);

// The CA bundle used to verify an upstream's TLS certificate.
//
// This is not cosmetic. mcpp builds compat:openssl from source into its own
// prefix, so OpenSSL's COMPILED-IN default verify path points at that build
// tree rather than at the machine's trust store — and every real relay is
// https. Without this, `curl https://api.openai.com` answers while literouter
// reports "upstream certificate rejected", which is exactly the kind of
// difference that makes a tool feel broken.
//
// Resolution order, first hit wins:
//   1. LITEROUTER_CA_BUNDLE
//   2. SSL_CERT_FILE, then CURL_CA_BUNDLE  (what the rest of the machine uses)
//   3. the first well-known system bundle that exists
// An empty result means "nothing found", and the TLS library keeps its own
// defaults — which is also the ONLY outcome when tls support is not compiled
// in, so no caller has to know which build it is.
std::filesystem::path resolveCaBundle();

// ─────────────────────────────────────────────────────────────────────────────
// Secrets
// ─────────────────────────────────────────────────────────────────────────────

// Expands a config field into the value actually sent upstream:
//   "${OPENAI_API_KEY}" -> getenv("OPENAI_API_KEY"), empty when unset
//   "$OPENAI_API_KEY"   -> same
//   "sk-literal"        -> "sk-literal"
//   ""                  -> ""
// A `${NAME:-fallback}` form supplies a default when the variable is unset.
std::string resolveSecret(std::string_view raw);
// True when the field still references an environment variable, whatever its
// value — the console uses this to render "from $OPENAI_API_KEY" instead of a
// redacted blob.
bool isSecretReference(std::string_view raw);
// Which variable a reference names; empty when the field is a literal.
std::string secretReferenceName(std::string_view raw);
// "sk-abc…xyz" for display. Never called on the wire path.
std::string maskSecret(std::string_view value);

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────

struct ValidationIssue {
    enum class Level { Info, Warning, Error };
    Level level = Level::Error;
    // Where the problem is, e.g. "providers[2].base_url" — stable enough to
    // show next to a form field.
    std::string path;
    std::string message;
    std::string levelName() const;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    bool ok() const;                       // no Error-level issue
    std::size_t count(ValidationIssue::Level) const;
    std::string summary() const;           // "3 errors, 1 warning"
};

ValidationReport validate(const AppConfig &config);

// ─────────────────────────────────────────────────────────────────────────────
// Config schema migration
// ─────────────────────────────────────────────────────────────────────────────

struct ConfigMigration {
    int from_schema = kConfigSchema;
    int to_schema = kConfigSchema;
    // One line per normalisation actually performed, for a front end to show
    // rather than leaving the operator to diff two files.
    std::vector<std::string> notes;
    // True when the document was older than this build and had to be walked up.
    bool changed() const { return from_schema != to_schema; }
};

// Rewrites a config document onto the current schema and reports what it did.
// The returned document is what this build would write; `rewritten` receives it.
// An unreadable document and one from a newer build are both errors.
std::expected<ConfigMigration, std::string> migrateConfigJson(std::string_view text,
                                                             std::string &rewritten);

// ─────────────────────────────────────────────────────────────────────────────
// Config store
//
// Holds one AppConfig, knows where it came from, and writes it back
// atomically (temp file + rename in the same directory) so a crash mid-save
// cannot leave a half-written file behind.
// ─────────────────────────────────────────────────────────────────────────────

class ConfigStore {
public:
    ConfigStore() = default;

    // Missing file is not an error: it yields `seedDefault()` and
    // `existsOnDisk() == false`, so `literouter serve` on a fresh machine
    // starts and says what to do next instead of refusing.
    static std::expected<ConfigStore, std::string> load(const std::filesystem::path &path);
    // Loads the default path.
    static std::expected<ConfigStore, std::string> loadDefault();
    // A file on disk that fails to parse is an error the caller decides how to
    // handle; this loads it, and on failure returns a seeded default plus the
    // parse error attached.
    static std::expected<ConfigStore, std::string> loadOrSeed(const std::filesystem::path &path);

    static AppConfig seedDefault();

    const AppConfig &config() const { return config_; }
    AppConfig &config() { return config_; }

    const std::filesystem::path &path() const { return path_; }
    bool existsOnDisk() const { return exists_; }
    void setExistsOnDisk(bool v) { exists_ = v; }

    // Serialised JSON, indented, with secrets left as written (never expanded).
    std::string toJson() const;
    // Replaces the model from JSON. Rejects structurally wrong input; leaves
    // the previous model untouched when it does.
    std::expected<void, std::string> fromJson(std::string_view text);

    // Non-empty only when loadOrSeed() found a file it could not parse. The
    // store is still usable (it holds the seed), which is what lets the
    // console open on a corrupt file and offer to repair it rather than
    // refusing to start.
    const std::string &loadError() const { return loadError_; }

    // What load() had to change to bring the file onto this build's schema.
    // `changed()` is false for a file that already matched.
    const ConfigMigration &migration() const { return migration_; }
    bool needsMigration() const { return migration_.changed(); }

    // Atomic write. Creates parent directories. The file is replaced by a
    // rename of a fully written temp file in the same directory, so a reader
    // sees either the old bytes or the new ones and never a truncated file; a
    // failed save leaves the previous file in place. A path that names a
    // directory is refused rather than replaced. `saveAs` writes the same bytes
    // somewhere else without rebinding the store, which is what `--print-config`
    // style export needs; both are const because neither mutates the model.
    std::expected<void, std::string> save() const;
    std::expected<void, std::string> saveAs(const std::filesystem::path &path) const;

    // Last modification time observed by load(), so a front end can notice an
    // external edit.
    std::filesystem::file_time_type mtime{};

private:
    AppConfig config_ = seedDefault();
    std::filesystem::path path_ = defaultConfigPath();
    bool exists_ = false;
    std::string loadError_;
    ConfigMigration migration_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Upstream admission
//
// A per-relay gate on what literouter itself sends, which is a different
// question from whether a relay is healthy (Router). A relay can be perfectly
// healthy and still be one
// this proxy is already hammering with more concurrent streams than the operator
// paid for. Distinct from the breaker because the remedy differs: a breaker says
// "this relay is failing", a limit says "this relay is full".
// ─────────────────────────────────────────────────────────────────────────────

struct UpstreamRejection {
    std::string message;
    int retry_after_sec = 1;
};

// An admitted slot. Releasing happens on destruction, so a slot taken for a
// streamed answer is held until the stream ends — including when the client
// hangs up, because the chunked provider that owns this handle is destroyed
// either way.
class UpstreamSlot {
public:
    UpstreamSlot();
    ~UpstreamSlot();
    UpstreamSlot(const UpstreamSlot &) = delete;
    UpstreamSlot &operator=(const UpstreamSlot &) = delete;

private:
    friend class UpstreamLimiter;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class UpstreamLimiter {
public:
    UpstreamLimiter();
    ~UpstreamLimiter();
    UpstreamLimiter(const UpstreamLimiter &) = delete;
    UpstreamLimiter &operator=(const UpstreamLimiter &) = delete;

    // Replaces the limits. Relays that disappear keep their counters until the
    // slots they hold are released, which is what makes removing a relay from
    // the config safe while it is answering.
    void setConfig(const AppConfig &config);

    // Takes a slot for `provider`, or says why not. `retry_after_sec` is the
    // time until the oldest start in the rolling minute leaves the window.
    std::expected<std::shared_ptr<UpstreamSlot>, UpstreamRejection> admit(
        std::string_view provider, double now_unix);

    std::size_t active(std::string_view provider) const;
    // The limit in force for a relay, or 0 when it has none. Read under the same
    // lock as admit() so a caller cannot report a limit it is not enforcing.
    int limitFor(std::string_view provider) const;
    void reset();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Local response cache
//
// Exact-match, non-streaming only, and bounded on both axes (TTL and entry
// count). The key is a hash of the protocol, the model and the request body as
// the upstream would have received it, so a request that differs in any way
// that could change the answer is a different key.
// ─────────────────────────────────────────────────────────────────────────────

struct CachedResponse {
    int status = 0;
    std::string content_type;
    std::string body;
};

class ResponseCache {
public:
    ResponseCache();
    ~ResponseCache();
    ResponseCache(const ResponseCache &) = delete;
    ResponseCache &operator=(const ResponseCache &) = delete;

    // `ttl_sec <= 0` turns the cache off and drops what it held.
    void configure(int ttl_sec, int max_entries);
    bool enabled() const;

    static std::string keyFor(std::string_view protocol, std::string_view model,
                              std::string_view body);

    // A hit refreshes the entry's position in the LRU order, because a cache
    // that evicts what is being used is a cache with a worse hit rate than the
    // policy it claims to implement.
    std::optional<CachedResponse> lookup(std::string_view key, double now_unix);
    void store(std::string_view key, CachedResponse response, double now_unix);

    std::size_t size() const;
    std::uint64_t hits() const;
    std::uint64_t misses() const;
    void clear();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry
//
// A fixed-capacity ring of log entries plus per-relay counters. Every record
// carries a monotonically increasing `seq`; a front end remembers the last seq
// it rendered and asks only for what came after, which is what makes the
// console's live log cheap to poll from a UI frame.
// ─────────────────────────────────────────────────────────────────────────────

struct ProviderStat {
    std::string provider;
    // Attempts sent to this relay. Always equals successes + failures + aborted,
    // which is what lets a console explain a row instead of just showing it.
    std::uint64_t requests = 0;
    // The relay produced a usable answer.
    std::uint64_t successes = 0;
    // The relay failed us: a transport error or a retryable status.
    std::uint64_t failures = 0;
    // We stopped listening: the client hung up mid-stream, or its body never
    // reached the socket. Neither the relay's fault nor its credit.
    std::uint64_t aborted = 0;
    std::uint64_t retries_in = 0;    // requests this relay absorbed after another failed
    std::uint64_t bytes_out = 0;     // response bytes relayed downstream
    std::uint64_t bytes_in = 0;      // request bytes forwarded upstream
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
    double latency_ms_last = 0.0;
    double latency_ms_avg = 0.0;     // exponential moving average, alpha 0.25
    double latency_ms_p95 = 0.0;     // nearest-rank p95 over the last 64 attempts
    double last_used_unix = 0.0;
    // Accumulated from the relay's own token reports and the prices written down
    // for it, in US dollars. 0 when no price is configured — see ProviderConfig.
    double cost_usd = 0.0;
};

struct ProviderHealth {
    enum class State { Unknown, Healthy, Degraded, Open };
    std::string provider;
    State state = State::Unknown;
    int consecutive_failures = 0;
    std::uint64_t total_failures = 0;
    std::string last_error;
    double open_until_unix = 0.0;    // a tripped breaker's release time
    double cooldown_remaining = 0.0;
    std::string stateName() const;
};

struct LogEntry {
    std::uint64_t seq = 0;
    double time_unix = 0.0;
    // "info" | "warn" | "error"
    std::string level = "info";
    std::string request_id;
    // "chat" | "embeddings" | "audio" | "images" | "models" | "admin" | "system"
    std::string kind;
    std::string model;            // as the client asked
    std::string provider;         // the relay that answered
    std::string upstream_model;   // what it was asked for
    int status = 0;
    bool stream = false;
    bool failover = false;        // at least one candidate was skipped first
    int attempt = 1;
    int attempts_total = 1;
    double latency_ms = 0.0;
    // Where that latency went, in the order it was spent:
    //   wait_ms   — from the request arriving to this relay being tried, which
    //               is queueing plus whatever earlier candidates cost;
    //   ttfb_ms   — from the attempt starting to its first response byte (the
    //               relay's own thinking time);
    //   stream_ms — from the first byte to the last one whne the relay streamed
    //               its answer, and 0 when it arrived in one piece.
    // A buffered answer cannot be split further: httplib reports one number for
    // connect-plus-answer, so it reports that as ttfb.
    double wait_ms = 0.0;
    double ttfb_ms = 0.0;
    double stream_ms = 0.0;
    std::uint64_t bytes = 0;
    std::string message;
    std::string request_body;     // only when server.log_bodies
    std::string response_body;    // only when server.log_bodies
    std::string timeText() const;          // "HH:MM:SS.mmm"
    std::string dateText() const;          // "YYYY-MM-DD"
    std::string dateTimeText() const;      // "YYYY-MM-DD HH:MM:SS.mmm"
    std::string shortDateTimeText() const; // "MM-DD HH:MM:SS"
};

// One hour of traffic, for a trend rather than a snapshot: the console can show
// "what the last day looked like", which a single set of totals cannot.
//
// Buckets are floored to the hour in UTC — the chart labels them relative to
// now, so which hour boundary is used does not matter to a reader, and UTC keeps
// it free of a timezone the proxy would otherwise have to resolve.
struct TrafficBucket {
    double hour_unix = 0.0;
    // How wide this bucket is. Carried per bucket rather than assumed, because
    // server.traffic_bucket_sec can change between runs and a reader that
    // hard-coded 3600 would mislabel a restored history.
    int bucket_sec = 3600;
    std::uint64_t requests = 0;
    std::uint64_t successes = 0;
    std::uint64_t failures = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
    double cost_usd = 0.0;
};

struct Snapshot {
    bool running = false;
    std::string host;
    int port = 0;
    std::string base_url;         // "http://host:port"
    double started_unix = 0.0;
    double uptime_sec = 0.0;
    std::string config_path;
    std::string version;
    std::uint64_t total_requests = 0;
    std::uint64_t total_success = 0;
    std::uint64_t total_failure = 0;
    std::uint64_t active_requests = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
    // What the relays have cost in total. Only relays with a price configured
    // contribute, so this is a floor rather than a total.
    double cost_usd = 0.0;
    double latency_ms_avg = 0.0;
    std::uint64_t log_seq = 0;    // newest seq the server has issued
    std::vector<ProviderStat> providers;
    std::vector<ProviderHealth> health;
    int breakers_open = 0;
    // The most recent buckets, oldest first. Empty until there has been traffic.
    std::vector<TrafficBucket> hourly;
    // The bucket width the trend above was recorded at, and how many buckets the
    // server is configured to keep. Both travel with the snapshot because a
    // chart that assumed 3600 x 24 would mislabel a minute-resolution history.
    int traffic_bucket_sec = 3600;
    int traffic_bucket_count = 24;
    // Local response cache state, so a console can show whether the cache is
    // working rather than only whether it is configured — "enabled with no hits"
    // and "disabled" look identical from the config alone.
    bool cache_enabled = false;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t cache_entries = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Router
//
// Pure decision layer: given a requested model, produce the ordered candidate
// list; given outcomes, maintain per-relay health. It owns no sockets, which
// is what lets the same logic be exercised from a test without a network.
// ─────────────────────────────────────────────────────────────────────────────

struct Candidate {
    std::string provider;
    std::string model;      // the upstream id to request
    int priority = 0;
    bool skipped = false;   // breaker open at selection time
    // The relay that shares this candidate's priority, if any, was preferred.
};

class Router {
public:
    Router();

    // Replaces the model and clears nothing else — health survives a config
    // edit, so saving the file does not reset a working circuit.
    void setConfig(const AppConfig &config);
    const AppConfig &config() const;

    // Ordered failover chain for `model`. Never empty unless nothing can serve
    // it; the caller distinguishes "no candidates" (404) from "all open"
    // (503) by inspecting the candidates it did get.
    std::vector<Candidate> candidatesFor(std::string_view model) const;

    bool circuitOpen(std::string_view provider, double now_unix) const;
    // Model-scoped breaker state used by request routing. Provider-level
    // methods remain available for compatibility with administrative callers.
    bool circuitOpen(std::string_view provider, std::string_view model,
                     double now_unix) const;
    std::vector<ProviderHealth> health(double now_unix) const;
    int openBreakerCount(double now_unix) const;

    void recordSuccess(std::string_view provider, double latency_ms, double now_unix);
    void recordSuccess(std::string_view provider, std::string_view model,
                       double latency_ms, double now_unix);
    // `cooldown_hint_sec` is a window the upstream named in Retry-After: an
    // explicit "come back in N seconds" opens the breaker at once, for at least
    // the configured cooldown, rather than waiting for the strike counter to
    // fill — the relay has said when it will be ready, and believing it beats
    // hammering it while the window runs.
    void recordFailure(std::string_view provider, std::string reason, double now_unix,
                       double cooldown_hint_sec = 0.0);
    void recordFailure(std::string_view provider, std::string_view model,
                       std::string reason, double now_unix,
                       double cooldown_hint_sec = 0.0);
    // Called when a config removes a relay, so its counters do not linger.
    void forget(std::string_view provider);
    void resetHealth();

    // Reasons a request was moved to the next candidate, in order.
    static bool retryableStatus(int status);

private:
    const ProviderConfig *find(std::string_view id) const;
    ProviderHealth &slot(std::string_view id);
    // Whether a breaker's window is still running. The strike counter decides
    // when a window is *set* (recordFailure); what it means is this.
    static bool windowRunning(const ProviderHealth &state, double now_unix);
    static std::string modelKey(std::string_view provider, std::string_view model);

    AppConfig config_;
    std::map<std::string, ProviderHealth, std::less<>> health_;
    std::map<std::string, ProviderHealth, std::less<>> model_health_;
    mutable std::mutex mutex_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Upstream client
//
// One call, one relay, no routing: the layer the proxy uses per attempt and
// the console uses for its "Test" button.
// ─────────────────────────────────────────────────────────────────────────────

struct UpstreamResult {
    bool ok = false;             // a response was received at all
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;           // transport-level failure, when !ok
    double latency_ms = 0.0;
};

// Non-streaming POST. `path` is appended to the provider's base_url.
UpstreamResult upstreamPost(const ProviderConfig &provider,
                            std::string_view path,
                            std::string_view body,
                            int timeout_sec_override = 0);

// Buffered media POST; preserves binary body bytes and caller-supplied MIME type.
// Existing JSON callers continue using upstreamPost(). No protocol conversion.
UpstreamResult upstreamPostRaw(const ProviderConfig &provider,
                              std::string_view path,
                              std::string_view body,
                              std::string_view content_type,
                              std::string_view accept = "*/*",
                              int timeout_sec_override = 0);

// What `prompt_tokens` + `completion_tokens` cost on this relay, in US dollars,
// from the prices written down for it. The proxy accumulates this per attempt;
// a one-off caller (the CLI's bench and replay) asks for it directly. The two
// must agree, which is why the formula lives here and not in either of them.
double estimateCost(double price_in_per_million, double price_out_per_million,
                    std::uint64_t prompt_tokens, std::uint64_t completion_tokens);

// ── upstream connections ────────────────────────────────────────────────────
//
// A relay connection taken from the pool for the length of one response. What a
// connection *is* — an httplib client with a socket, a TLS session and its own
// timeouts — is the upstream unit's business, and the contract names no
// third-party type, so the handle is deliberately opaque. The holder treats it
// as a token: it came out of the pool, and the pool still owns it.
using UpstreamConnection = std::shared_ptr<void>;

// One client per relay per worker thread, already configured from the provider
// (timeouts, keep-alive, trust store) and reused by every request that thread
// makes. `root` is the scheme://host part of the relay's base_url, as
// splitBaseUrl() returned it.
//
// A transfer that ended cleanly needs no matching call: the connection never
// left the pool. A transfer that was aborted, or that failed in transport, must
// call retireUpstreamConnection() — the socket is not one to hand to the next
// request.
UpstreamConnection checkoutUpstreamConnection(std::string_view root,
                                              const ProviderConfig &provider);
void retireUpstreamConnection(std::string_view root, const ProviderConfig &provider);

// What the console's provider rows show.
struct ProviderProbe {
    bool reachable = false;
    int status = 0;
    double latency_ms = 0.0;
    std::string detail;
    // Model ids the relay reported, when it answered /models.
    std::vector<std::string> models;
};

// GET {base_url}/models. This is the "Test" action and also the way the
// console discovers what a relay actually serves.
ProviderProbe probeProvider(const ProviderConfig &provider, int timeout_sec = 15);

// Which CA bundle this process will verify upstream certificates against, as a
// human string. Empty means "the TLS library's own defaults". `doctor` prints
// it, because "certificate rejected" and "no trust store" are the same message
// from the relay's point of view and different problems to fix.
std::string caBundleSummary();

// ─────────────────────────────────────────────────────────────────────────────
// Protocol Adapters (OpenAI, Anthropic Claude, Google Gemini, OpenAI Responses)
// ─────────────────────────────────────────────────────────────────────────────

// Which wire format a protocol name actually speaks.
//
// Several names share one shape, and the proxy's "no conversion needed" fast
// path has to compare shapes rather than names: an `azure` relay speaks OpenAI's
// JSON behind a different path and header, so a chat request arriving in
// OpenAI's shape needs no conversion even though the two names differ. Getting
// this wrong is expensive in the quiet direction — a name comparison would send
// an unconverted body to a relay that cannot read it.
enum class WireShape {
    OpenAi,
    Anthropic,
    Gemini,
    Responses,
    Ollama,
    Bedrock,
};

WireShape wireShapeOf(std::string_view protocol);
std::string_view wireShapeName(WireShape shape);

// ─────────────────────────────────────────────────────────────────────────────
// Upstream authentication
//
// One header the upstream leg must carry. An ordered vector rather than a map
// because the order is the operator's: a relay's own `headers` entry has to be
// able to replace a default, and a map would silently pick one.
// ─────────────────────────────────────────────────────────────────────────────

struct UpstreamHeader {
    std::string name;
    std::string value;
};

// Every protocol-specific header one request to one relay needs: the credential
// in whatever form the protocol wants (`Authorization: Bearer`, `x-api-key`,
// `api-key`, a SigV4 signature), plus the version headers Anthropic and Gemini
// require. `method`, `path` and `body` are inputs because SigV4 signs all three,
// and `now_unix` is a parameter rather than a call to nowUnix() so a test can
// sign a fixed instant and compare bytes.
//
// An error means the credential could not be produced at all — an unreadable
// Vertex service-account file, a token endpoint that refused the exchange —
// which is a request-level failure, not something a second relay fixes.
std::expected<std::vector<UpstreamHeader>, std::string> upstreamAuthHeaders(
    const ProviderConfig &provider, std::string_view method, std::string_view path,
    std::string_view body, double now_unix);

// Determines the upstream HTTP path for chat requests according to protocol and model.
std::string resolveChatPath(const ProviderConfig &provider,
                            std::string_view upstream_model,
                            bool stream);

// Determines the upstream HTTP path for models probing according to protocol.
std::string resolveModelsPath(const ProviderConfig &provider);

// Adapts an OpenAI chat completion JSON request body into the upstream provider's protocol format.
std::string adaptChatRequest(const ProviderConfig &provider,
                             std::string_view upstream_model,
                             std::string_view openai_request_json,
                             bool stream);

// Adapts an upstream non-streaming response body into standard OpenAI Chat Completion JSON.
std::string adaptChatResponse(const ProviderConfig &provider,
                              std::string_view upstream_response,
                              std::string_view requested_model);

// Adapts an OpenAI Responses request (/v1/responses) to an OpenAI Chat Completion request (/v1/chat/completions).
std::string adaptResponsesToChat(std::string_view responses_request_json);

// Adapts an OpenAI Chat Completion response to an OpenAI Responses response (/v1/responses).
std::string adaptChatToResponses(std::string_view chat_completion_response_json,
                                 std::string_view requested_model);

// Adapts an Anthropic Claude Messages request (/v1/messages) to an OpenAI Chat Completion request.
std::string adaptAnthropicToChat(std::string_view anthropic_request_json);

// Adapts a Google Gemini generateContent request to an OpenAI Chat Completion request.
std::string adaptGeminiToChat(std::string_view gemini_request_json, std::string_view model);

// Adapts an OpenAI Chat Completion response to an Anthropic Claude Messages response (/v1/messages).
std::string adaptChatToAnthropic(std::string_view chat_completion_response_json,
                                 std::string_view requested_model);

// Adapts an OpenAI Chat Completion response to a Google Gemini generateContent response.
std::string adaptChatToGemini(std::string_view chat_completion_response_json,
                              std::string_view requested_model);

// Stateful SSE stream adapter that converts upstream chunks between protocols
// (OpenAI, Anthropic Claude Messages, Google Gemini).
// When from_protocol == to_protocol, chunks are passed through verbatim with zero conversion.
class StreamProtocolAdapter {
public:
    StreamProtocolAdapter(const ProviderConfig &provider,
                          std::string model,
                          std::string request_id);
    StreamProtocolAdapter(std::string from_protocol,
                          std::string to_protocol,
                          std::string model,
                          std::string request_id);
    ~StreamProtocolAdapter();

    StreamProtocolAdapter(const StreamProtocolAdapter &) = delete;
    StreamProtocolAdapter &operator=(const StreamProtocolAdapter &) = delete;
    StreamProtocolAdapter(StreamProtocolAdapter &&) noexcept;
    StreamProtocolAdapter &operator=(StreamProtocolAdapter &&) noexcept;

    // Feeds an incoming chunk from upstream and returns converted OpenAI SSE chunks to forward downstream.
    std::string feed(std::string_view chunk);

    // Called when upstream stream finishes. Emits any remaining final frames (e.g. data: [DONE]\n\n).
    std::string finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Stream usage
// ─────────────────────────────────────────────────────────────────────────────

// What a streamed answer reported it used.
struct TokenUsage {
    std::uint64_t prompt = 0;
    std::uint64_t completion = 0;
    bool reported = false; // distinguishes an explicit zero from missing usage
};

bool tokenUsageReported(std::string_view body);

// Reads the token counts out of a stream as it goes by.
//
// A streamed reply names its usage somewhere in the tail — OpenAI (when the
// client asked for `stream_options.include_usage`) in one final chunk,
// Anthropic split between `message_start` and `message_delta`, Gemini in a
// cumulative `usageMetadata` — so counting them means watching the stream pass,
// because the non-streaming reader (`accumulateUsage`) never gets a body.
//
// It is deliberately not a parser of every chunk: a chunk whose text holds no
// "usage" is not parsed at all, which is what keeps the same-protocol path's
// "no per-chunk JSON parse" promise true. Counts merge by taking the largest
// seen, since a protocol may report the same number cumulatively on every event
// while another reports it once at the end.
class StreamUsageObserver {
public:
    void feed(std::string_view chunk);
    TokenUsage usage() const;

private:
    void absorbLine(std::string_view line);

    // The tail of an event that has not been terminated yet: an SSE event can be
    // split across two chunks, and so can the word "usage".
    std::string carry_;
    std::uint64_t prompt_ = 0;
    std::uint64_t completion_ = 0;
    bool reported_ = false;
};


// ─────────────────────────────────────────────────────────────────────────────
// Proxy server
//
// Owns the listening socket and the worker pool. `start` binds and returns;
// the accept loop runs on its own thread. Everything the two front ends do —
// start, stop, hot-reload, read telemetry, stream the log — goes through this
// object; the CLI either creates one in-process (`serve`) or talks to a remote
// one over the admin endpoints.
// ─────────────────────────────────────────────────────────────────────────────

class ProxyServer {
public:
    ProxyServer();
    ~ProxyServer();
    ProxyServer(const ProxyServer &) = delete;
    ProxyServer &operator=(const ProxyServer &) = delete;

    // Binds `host:port` and starts serving. Returns an error string on failure
    // (port in use, bad address) rather than throwing.
    std::expected<void, std::string> start(const AppConfig &config);
    // Stops accepting, drains in-flight requests, joins the worker. Idempotent.
    void stop();
    bool running() const;

    // Listener host, port and TLS files take effect on the next start.
    // Hot-swaps the routing model. In-flight requests keep the config they
    // began with; the next request sees the new one.
    void updateConfig(const AppConfig &config);
    AppConfig config() const;

    // Where `POST {admin}/reload` reads from, and what the console displays as
    // the live config file. Not part of AppConfig because it is a property of
    // the running process, not of the routing model.
    void setConfigPath(std::string path);

    // Cheap enough to call once per UI frame.
    Snapshot snapshot() const;
    // `seq == 0` asks for the tail (the newest `limit` entries), which is what a
    // log view wants on first paint. A non-zero `seq` asks for what came after
    // it, which is what it wants on every later frame.
    std::vector<LogEntry> logsSince(std::uint64_t seq, std::size_t limit = 500) const;
    void clearLogs();
    // Drops counters and breaker state without touching the listener.
    void resetStats();

    // The address actually bound, which differs from the config when port was
    // 0 (kernel-assigned).
    std::string boundAddress() const;
    int boundPort() const;

    // Path prefix for the management API the CLI attaches through.
    static constexpr std::string_view kAdminPrefix = "/__literouter";
    static std::string adminUrl(const std::string &host, int port, std::string_view path);
    static std::string adminUrl(const ServerConfig &server, std::string_view path);

    // Parses an OpenAI-style usage block out of a response body, for the token
    // counters. Best-effort: a body without `usage` contributes nothing.
    static void accumulateUsage(std::string_view body, ProviderStat &stat);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Admin client
//
// What `literouter status` and `literouter log` use. Returns std::nullopt-ish
// results rather than throwing when nothing is listening — "no server running"
// is the common case, not an exception.
// ─────────────────────────────────────────────────────────────────────────────

struct AdminStatus {
    bool reachable = false;
    std::string error;
    Snapshot snapshot;
};

AdminStatus fetchStatus(std::string_view base_url, std::string_view api_key = {});

// Incremental log read. `since == 0` asks for the tail.
struct AdminLogs {
    bool reachable = false;
    std::string error;
    std::vector<LogEntry> entries;
    std::uint64_t seq = 0;   // newest seq the server has issued
};

AdminLogs fetchLogs(std::string_view base_url, std::uint64_t since, std::size_t limit = 200,
                    std::string_view api_key = {});

// Raw escape hatch for the management POSTs the CLI drives: `/reload`,
// `/reset-stats`, `/shutdown`, `/probe`. Returns the status and body verbatim so
// the caller can print a server-side validation report as it was written.
struct AdminReply {
    bool reachable = false;
    std::string error;
    int status = 0;
    std::string body;
};

AdminReply adminPost(std::string_view base_url, std::string_view path,
                     std::string_view body = {}, std::string_view api_key = {});

// ─────────────────────────────────────────────────────────────────────────────
// JSON codecs — shared by the config store, the admin API and the console.
// ─────────────────────────────────────────────────────────────────────────────

std::string toJsonString(const AppConfig &config);
std::expected<AppConfig, std::string> appConfigFromJson(std::string_view text);

std::string toJsonString(const Snapshot &snapshot);
std::string toJsonString(const LogEntry &entry);
std::string toJsonString(const ProviderStat &stat);
std::string toJsonString(const ProviderProbe &probe);

// The bearer the server expects for a request carrying `authorization` /
// `x-api-key`. Empty when the server has no key configured.
std::string extractApiKey(const std::map<std::string, std::string, std::less<>> &headers);

// Constant-time-ish comparison, so a key check is not a length oracle.
bool secureEquals(std::string_view a, std::string_view b);

// Splits a base_url into an httplib connection root and a path prefix:
//   "https://api.openai.com/v1/" -> { "https://api.openai.com", "/v1" }
// Returns false when the URL has no host. `scheme` receives "http"/"https".
bool splitBaseUrl(std::string_view base_url, std::string &root, std::string &prefix,
                  std::string &scheme);

// Joins a base URL prefix with an endpoint path, normalizing slashes and
// eliminating duplicate path segments (e.g. prefix "/v1" + path "/v1/messages" -> "/v1/messages").
std::string joinPath(std::string_view prefix, std::string_view path);

// ─────────────────────────────────────────────────────────────────────────────
// Small utilities the front ends share.
// ─────────────────────────────────────────────────────────────────────────────

std::string trim(std::string_view text);
std::string toLower(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);

// Truncates to `limit` bytes on a UTF-8 boundary and appends "…".
std::string truncateUtf8(std::string_view text, std::size_t limit);

// UTF-8 hygiene for anything that will be serialized. A `std::filesystem::path`
// is not guaranteed to hold UTF-8 — on Windows `path::string()` narrows through
// the ANSI code page — and nlohmann's strict `dump()` throws (and, uncaught,
// aborts) on bytes that are not valid UTF-8.
bool isValidUtf8(std::string_view text);
std::string toValidUtf8(std::string_view text);

// The conversion to use for a path that is about to be shown, logged or put in
// JSON. Never throws; returns an empty string for a path that cannot be
// expressed. Prefer this over `path.string()` everywhere.
std::string pathToUtf8(const std::filesystem::path &path);

// Masks things that look like credentials in text that is about to be kept:
// the bodies the log stores when server.log_bodies is on, which are prompts as
// often as not, and people paste keys into prompts.
//
// Deliberately narrow — known key prefixes, a bearer header, a JWT, a private
// key block, and an `api_key: <long value>` pair. A false positive costs a
// masked word in a log nobody reads; a false negative costs a leaked key, so
// the patterns are the ones that are almost never ordinary prose — and the
// point is not to be a secret scanner, only to stop the obvious ones from
// being written down.
std::string redactSecrets(std::string_view text);

// "1.2k", "3.4M" — for the console's compact metric tiles.
std::string humanCount(std::uint64_t value);
// "820ms", "12.4s"
std::string humanMillis(double ms);
std::string humanBytes(std::uint64_t bytes);
// "3m 12s"
std::string humanDuration(double seconds);
// "3m 12s", "1h 2m 5s" — like humanDuration, but it never drops the seconds.
// For a running total that a console repaints every second, a formatter that
// coarsens to minutes past the first hour is a clock that looks stopped.
std::string humanUptime(double seconds);

std::string hexId(std::size_t bytes = 8);
double nowUnix();
void ensureLocalTimezone();

// ─────────────────────────────────────────────────────────────────────────────
// Internationalisation (i18n)
// ─────────────────────────────────────────────────────────────────────────────

namespace i18n {

enum class Lang {
    Auto = 0,
    En,
    Zh
};

Lang parseLang(std::string_view code);
std::string_view langCode(Lang lang);
std::string_view langDisplayName(Lang lang);
Lang detectSystemLang();
Lang resolveLang(Lang lang);
void setLang(Lang lang);
Lang getLang();

std::string_view tr(std::string_view text);
std::string_view tr(std::string_view text, Lang lang);
const char* tr(const char* text);
const char* tr(const char* text, Lang lang);

} // namespace i18n

} // namespace literouter
