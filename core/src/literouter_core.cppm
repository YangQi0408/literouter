// literouter.core — the public surface of the routing engine.
//
// Everything a front end needs lives here: the config model and its on-disk
// form, the routing/failover decision layer, the HTTP proxy itself, and the
// telemetry the console renders. Both `literouter.cli` and `literouter.gui`
// import this module and nothing else from the engine.
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

inline constexpr std::string_view kVersion = "0.1.0";
inline constexpr std::string_view kUserAgent = "literouter/0.1.0";

// ─────────────────────────────────────────────────────────────────────────────
// Configuration model
//
// The on-disk file is JSON with a `schema` field; every struct below has a
// documented default so a hand-written minimal file (or an empty one) still
// loads. Secrets may be written literally or as a `${ENV_VAR}` reference —
// `resolveSecret` expands the latter at request time, which is what keeps an
// API key out of a file that might get synced or screenshotted.
// ─────────────────────────────────────────────────────────────────────────────

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
    int connect_timeout_sec = 5;
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
    // Upstream API protocol: "openai" (default), "anthropic", "gemini", "openai_responses".
    std::string protocol = "openai";
    // Free-form note shown in the console.
    std::string note;
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
    // Serve the built-in web console at /ui. On by default because the console
    // is the only telemetry surface a headless machine has; turn it off and the
    // routes do not exist at all. Keep it off — or set api_key — when the
    // listener is reachable from more than this machine: the console exposes the
    // request log, which can carry prompts.
    bool web_ui = true;
    // Interface language ("auto", "en", "zh"). Default "auto" detects from system locale.
    std::string language = "auto";
    // UI display scale (e.g. 1.0 = 100%, 0.8 = 80%, 1.25 = 125%). 0.0 or 1.0 means default.
    double ui_scale = 1.0;
};

struct AppConfig {
    int schema = 1;
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
std::filesystem::path defaultPidPath();

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

    // Atomic write. Creates parent directories. `saveAs` writes the same bytes
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
    double latency_ms_p95 = 0.0;     // over the last window
    double last_used_unix = 0.0;
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
    // "chat" | "embeddings" | "models" | "admin" | "system"
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
    std::uint64_t bytes = 0;
    std::string message;
    std::string request_body;     // only when server.log_bodies
    std::string response_body;    // only when server.log_bodies
    std::string timeText() const;          // "HH:MM:SS.mmm"
    std::string dateText() const;          // "YYYY-MM-DD"
    std::string dateTimeText() const;      // "YYYY-MM-DD HH:MM:SS.mmm"
    std::string shortDateTimeText() const; // "MM-DD HH:MM:SS"
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
    double latency_ms_avg = 0.0;
    std::uint64_t log_seq = 0;    // newest seq the server has issued
    std::vector<ProviderStat> providers;
    std::vector<ProviderHealth> health;
    int breakers_open = 0;
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
    std::vector<ProviderHealth> health(double now_unix) const;
    int openBreakerCount(double now_unix) const;

    void recordSuccess(std::string_view provider, double latency_ms, double now_unix);
    void recordFailure(std::string_view provider, std::string reason, double now_unix);
    // Called when a config removes a relay, so its counters do not linger.
    void forget(std::string_view provider);
    void resetHealth();

    // Reasons a request was moved to the next candidate, in order.
    static bool retryableStatus(int status);

private:
    const ProviderConfig *find(std::string_view id) const;
    ProviderHealth &slot(std::string_view id);

    AppConfig config_;
    std::map<std::string, ProviderHealth, std::less<>> health_;
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
// Proxy server
//
// Owns the listening socket and the worker pool. `start` binds and returns;
// the accept loop runs on its own thread. Everything the two front ends do —
// start, stop, hot-reload, read telemetry, stream the log — goes through this
// object, and the GUI holds one in-process while the CLI either creates one
// (`serve`) or talks to a remote one over the admin endpoints.
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

// "1.2k", "3.4M" — for the console's compact metric tiles.
std::string humanCount(std::uint64_t value);
// "820ms", "12.4s"
std::string humanMillis(double ms);
std::string humanBytes(std::uint64_t bytes);
// "3m 12s"
std::string humanDuration(double seconds);

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
