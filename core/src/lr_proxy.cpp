// The proxy itself: listener, request pipeline, failover, SSE relay, telemetry.
//
// One design note the rest of the file follows from. Streaming failover has a
// constraint ordinary failover does not: once the client has received a byte of
// a streamed answer that answer is committed, because a retry would append the
// second attempt's tokens to the first's inside the same response body. So the
// gate sits at the response headers. An attempt may fail over only if it fails
// *before* headers arrive — connection error, timeout, or a retryable status.
// After that the stream belongs to the client and nothing is retried.
//
// That is why the upstream leg runs on its own thread while the server's thread
// holds the downstream sink, and why the two are joined by a bounded queue
// rather than a plain call.
module;

#include <httplib.h>

module literouter.core;

import std;
import nlohmann.json;

namespace literouter {

namespace {

using json = nlohmann::json;
namespace h = httplib;

constexpr std::size_t kMaxRequestBody = 16ull * 1024 * 1024;
// How much un-drained upstream data may sit in a bridge before the reader stops
// pulling. Bounded so a fast relay cannot balloon the process, high enough that
// an ordinary token cadence never blocks.
constexpr std::size_t kStreamHighWatermark = 64 * 1024;

// Duplicated from lr_upstream.cpp rather than shared: the two units have no
// common internal header on purpose (see the note in that file), and a switch
// over an enum is cheaper to repeat than to route through the module interface.
std::string errorText(h::Error error) {
    switch (error) {
    case h::Error::Success: return "success";
    case h::Error::Unknown: return "unknown error";
    case h::Error::Connection: return "connection failed";
    case h::Error::BindIPAddress: return "cannot bind local address";
    case h::Error::Read: return "read error";
    case h::Error::Write: return "write error";
    case h::Error::ExceedRedirectCount: return "too many redirects";
    case h::Error::Canceled: return "canceled";
    case h::Error::SSLConnection: return "TLS handshake failed";
    case h::Error::SSLLoadingCerts: return "cannot load CA certificates";
    case h::Error::SSLServerVerification: return "upstream certificate rejected";
    case h::Error::SSLServerHostnameVerification: return "upstream certificate hostname mismatch";
    case h::Error::Compression: return "compression error";
    case h::Error::ConnectionTimeout: return "connection timed out";
    case h::Error::ProxyConnection: return "proxy connection failed";
    case h::Error::ConnectionClosed: return "upstream closed the connection";
    case h::Error::Timeout: return "upstream timed out";
    case h::Error::ResourceExhaustion: return "out of resources";
    case h::Error::UnsupportedContentEncoding: return "unsupported content encoding";
    default: return "transport error";
    }
}

bool ciEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

std::string joinPath(std::string_view prefix, std::string_view path) {
    std::string out{prefix};
    if (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    if (path.empty()) {
        return out.empty() ? std::string{"/"} : out;
    }
    if (path.front() != '/') {
        out.push_back('/');
    }
    out.append(path);
    return out;
}

bool headerExists(const std::vector<std::pair<std::string, std::string>> &headers,
                  std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto &entry) {
        return ciEqual(entry.first, name);
    });
}

std::string headerValue(const std::vector<std::pair<std::string, std::string>> &headers,
                        std::string_view name) {
    for (const auto &[key, value] : headers) {
        if (ciEqual(key, name)) {
            return value;
        }
    }
    return {};
}

std::string headerValue(const std::map<std::string, std::string> &headers,
                        std::string_view name) {
    for (const auto &[key, value] : headers) {
        if (ciEqual(key, name)) {
            return value;
        }
    }
    return {};
}

template <typename HeadersT>
bool isHtmlResponse(const HeadersT &headers, std::string_view body) {
    const std::string ct = toLower(headerValue(headers, "content-type"));
    if (ct.find("text/html") != std::string::npos ||
        ct.find("application/xhtml+xml") != std::string::npos) {
        return true;
    }
    const std::string trimmed = trim(body);
    if (trimmed.size() >= 5) {
        const std::string prefix = toLower(std::string{trimmed.substr(0, std::min<std::size_t>(trimmed.size(), 64))});
        if (prefix.starts_with("<!doctype html") || prefix.starts_with("<html")) {
            return true;
        }
    }
    return false;
}

template <typename HeadersT>
bool isCloudflareBlocked(int status, const HeadersT &headers, std::string_view body) {
    const std::string cf_ray = headerValue(headers, "cf-ray");
    const std::string server = toLower(headerValue(headers, "server"));
    const bool cf_server = server.find("cloudflare") != std::string::npos || !cf_ray.empty();

    if (status == 403 || status == 503 || status == 429) {
        if (cf_server) {
            return true;
        }
        const std::string lower = toLower(body);
        if (lower.find("cloudflare") != std::string::npos &&
            (lower.find("attention required") != std::string::npos ||
             lower.find("sorry, you have been blocked") != std::string::npos ||
             lower.find("just a moment...") != std::string::npos ||
             lower.find("turnstile") != std::string::npos ||
             lower.find("cf-wrapper") != std::string::npos)) {
            return true;
        }
    }
    return false;
}

std::string stripTags(std::string_view html) {
    std::string out;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
        } else if (!in_tag) {
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                if (!out.empty() && out.back() != ' ') {
                    out.push_back(' ');
                }
            } else {
                out.push_back(c);
            }
        }
    }
    return trim(out);
}

std::string extractTagContent(std::string_view html, std::string_view tag) {
    const std::string lower = toLower(html);
    const std::string open = "<" + toLower(tag);
    const std::string close = "</" + toLower(tag) + ">";

    const std::size_t start = lower.find(open);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t open_close = html.find('>', start);
    if (open_close == std::string::npos) {
        return {};
    }
    const std::size_t end = lower.find(close, open_close + 1);
    if (end == std::string::npos) {
        return {};
    }
    return stripTags(html.substr(open_close + 1, end - open_close - 1));
}

template <typename HeadersT>
std::string summarizeHtmlError(std::string_view provider_id, int status,
                               const HeadersT &headers, std::string_view body) {
    const std::string cf_ray = headerValue(headers, "cf-ray");
    const bool is_cf = !cf_ray.empty() ||
                       toLower(headerValue(headers, "server")).find("cloudflare") != std::string::npos ||
                       toLower(body).find("cloudflare") != std::string::npos;

    const std::string title = extractTagContent(body, "title");
    const std::string h1 = extractTagContent(body, "h1");
    const std::string h2 = extractTagContent(body, "h2");

    std::string detail;
    if (!h1.empty()) {
        detail = h1;
        if (!h2.empty() && h2 != h1) {
            detail += " — " + h2;
        }
    } else if (!title.empty()) {
        detail = title;
    } else {
        detail = truncateUtf8(stripTags(body), 160);
    }

    std::string prefix;
    if (is_cf) {
        prefix = "Cloudflare WAF / protection blocked request";
    } else {
        prefix = std::format("Upstream returned HTTP {} with HTML error", status);
    }

    std::string msg;
    if (!detail.empty()) {
        msg = std::format("{} from `{}`: {}", prefix, provider_id, detail);
    } else {
        msg = std::format("{} from `{}`", prefix, provider_id);
    }

    if (!cf_ray.empty()) {
        msg += std::format(" (Ray ID: {})", cf_ray);
    }
    return msg;
}

// Headers that belong to one hop and must not be forwarded: the connection
// framing either side negotiated, and the length/encoding of a body the client
// library has already decoded.
bool isHopByHop(std::string_view name) {
    static constexpr std::array<std::string_view, 11> names{
        "connection",          "keep-alive", "proxy-authenticate", "proxy-authorization",
        "te",                  "trailer",    "transfer-encoding",  "upgrade",
        "content-length",      "content-encoding", "set-cookie",
    };
    return std::ranges::any_of(names, [name](std::string_view candidate) {
        return ciEqual(candidate, name);
    });
}

std::string contentTypeOf(const UpstreamResult &result) {
    if (const auto it = result.headers.find("content-type"); it != result.headers.end()) {
        return it->second;
    }
    return "application/json";
}

std::string contentTypeOf(const std::vector<std::pair<std::string, std::string>> &headers) {
    const std::string value = headerValue(headers, "content-type");
    return value.empty() ? std::string{"application/json"} : value;
}

// How much of a relay's own error body is worth keeping. Long enough for a real
// diagnostic, short enough that a misbehaving relay cannot make the proxy
// buffer a megabyte before answering.
constexpr std::size_t kMaxErrorBody = 64 * 1024;

json errorBody(std::string message, std::string type, std::string code) {
    json error = json::object();
    error["message"] = std::move(message);
    error["type"] = std::move(type);
    error["code"] = std::move(code);
    json root = json::object();
    root["error"] = std::move(error);
    return root;
}

void sendError(h::Response &res, int status, std::string message,
               std::string type = "invalid_request_error",
               std::string code = "invalid_request") {
    res.status = status;
    res.set_content(errorBody(std::move(message), std::move(type), std::move(code)).dump(),
                    "application/json");
}

// ── streaming bridge ─────────────────────────────────────────────────────────

// Handoff between the upstream reader thread and the server's response thread.
//
// The reader pushes chunks, the chunked-content provider pops them. The queue is
// bounded (the reader waits when it is full) so a fast relay cannot outrun a
// slow client's socket buffer. `aborted` is the downstream-to-upstream cancel
// signal: when the client hangs up, the provider sets it and calls
// `Client::stop()` so the reader is not left blocked on a socket nobody wants.
struct StreamBridge {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;

    bool headers_ready = false;
    bool finished = false;
    bool aborted = false;

    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;

    std::shared_ptr<h::Client> client;
    std::atomic<std::uint64_t> bytes_out{0};
};

// ── log ring ─────────────────────────────────────────────────────────────────

struct LogRing {
    std::deque<LogEntry> entries;
    std::size_t capacity = 400;
    std::uint64_t next_seq = 1;

    void setCapacity(std::size_t value) {
        capacity = std::max<std::size_t>(16, value);
        while (entries.size() > capacity) {
            entries.pop_front();
        }
    }

    void push(LogEntry entry) {
        entry.seq = next_seq++;
        if (entry.time_unix == 0.0) {
            entry.time_unix = nowUnix();
        }
        entries.push_back(std::move(entry));
        while (entries.size() > capacity) {
            entries.pop_front();
        }
    }

    std::vector<LogEntry> since(std::uint64_t seq, std::size_t limit) const {
        std::vector<LogEntry> out;
        if (seq == 0) {
            // `since=0` means "I have nothing; show me the tail". Returning the
            // OLDEST entries instead would make a freshly opened log page show
            // whatever happened at startup and never the request just made.
            const std::size_t skip = entries.size() > limit ? entries.size() - limit : 0;
            out.reserve(entries.size() - skip);
            for (std::size_t i = skip; i < entries.size(); ++i) {
                out.push_back(entries[i]);
            }
            return out;
        }
        for (const auto &entry : entries) {
            if (entry.seq > seq) {
                out.push_back(entry);
                if (out.size() >= limit) {
                    break;
                }
            }
        }
        return out;
    }
};

// Everything one request carries through its attempts. Held by value on the
// stack of the handler, so a request never reads another request's fields —
// which a member on the server would have allowed.
struct RequestContext {
    std::string id;
    std::string kind;
    std::string model;
    std::string body;
    bool stream = false;
    double started = 0.0;
    AppConfig config;
    // Every path that ends a request calls finish(), and for a streamed
    // response TWO lambdas can: the content provider when the stream completes,
    // and the resource releaser when httplib tears the response down. They hold
    // separate copies of this struct, so the flag has to be shared for "exactly
    // once" to mean anything — otherwise the second call double-counts the
    // request and logs a phantom abort.
    std::shared_ptr<std::atomic<bool>> reported =
        std::make_shared<std::atomic<bool>>(false);
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct ProxyServer::Impl {
    // Recreated on every start(). httplib's Server latches `is_decommissioned`
    // in stop() and refuses to bind again afterwards, so a console whose Stop
    // then Start would otherwise work exactly once. Owning it behind a pointer
    // and constructing a fresh one per start is the whole fix.
    std::unique_ptr<h::Server> server;
    Router router;

    mutable std::mutex config_mutex;
    AppConfig config;
    std::string config_path;

    std::thread runner;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    // Signed, and always released through releaseInFlight(): a counter that can
    // only be decremented is one bug away from wrapping to 4 billion.
    std::atomic<int> active_requests{0};
    std::atomic<int> bound_port{0};

    double started_unix = 0.0;

    mutable std::mutex telemetry_mutex;
    std::map<std::string, ProviderStat, std::less<>> stats;
    LogRing log;
    std::uint64_t total_requests = 0;
    std::uint64_t total_success = 0;
    std::uint64_t total_failure = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
    double latency_ms_avg = 0.0;

    AppConfig snapshotConfig() const {
        std::scoped_lock lock{config_mutex};
        return config;
    }

    std::string configPath() const {
        std::scoped_lock lock{config_mutex};
        return config_path;
    }

    void setConfigPath(std::string path) {
        std::scoped_lock lock{config_mutex};
        config_path = std::move(path);
    }

    // ── telemetry ────────────────────────────────────────────────────────────

    ProviderStat &statFor(std::string_view provider) {
        if (auto it = stats.find(provider); it != stats.end()) {
            return it->second;
        }
        ProviderStat fresh;
        fresh.provider = std::string{provider};
        return stats.emplace(std::string{provider}, std::move(fresh)).first->second;
    }

    void appendLog(LogEntry entry) {
        std::scoped_lock lock{telemetry_mutex};
        log.push(std::move(entry));
    }

    void releaseInFlight() {
        int current = active_requests.load(std::memory_order_relaxed);
        while (current > 0 && !active_requests.compare_exchange_weak(
                                  current, current - 1, std::memory_order_relaxed)) {
        }
    }

    // The one place a request is accounted for. Called exactly once per
    // request, on every path, including the ones that never reached a relay.
    void finish(const RequestContext &ctx, std::string provider, int status,
                std::uint64_t bytes, std::string message, bool failover = false,
                int attempt = 1, int attempts_total = 1) {
        if (ctx.reported->exchange(true)) {
            return;
        }

        LogEntry entry;
        entry.level = status >= 200 && status < 300 ? "info" : "error";
        entry.request_id = ctx.id;
        entry.kind = ctx.kind;
        entry.model = ctx.model;
        entry.provider = std::move(provider);
        entry.status = status;
        entry.stream = ctx.stream;
        entry.failover = failover;
        entry.attempt = attempt;
        entry.attempts_total = attempts_total;
        entry.latency_ms = (nowUnix() - ctx.started) * 1000.0;
        entry.bytes = bytes;
        entry.message = std::move(message);
        if (ctx.config.server.log_bodies) {
            const auto limit = static_cast<std::size_t>(ctx.config.server.log_body_limit);
            entry.request_body = truncateUtf8(ctx.body, limit);
        }

        std::scoped_lock lock{telemetry_mutex};
        ++total_requests;
        if (status >= 200 && status < 300) {
            ++total_success;
        } else if (status > 0) {
            ++total_failure;
        }
        bytes_out += bytes;
        if (entry.latency_ms > 0.0) {
            latency_ms_avg = latency_ms_avg == 0.0
                                 ? entry.latency_ms
                                 : latency_ms_avg * 0.85 + entry.latency_ms * 0.15;
        }
        log.push(std::move(entry));
        releaseInFlight();
    }

    void recordSystem(std::string message, std::string level = "info") {
        LogEntry entry;
        entry.level = std::move(level);
        entry.kind = "system";
        entry.message = std::move(message);
        appendLog(std::move(entry));
    }

    // A client that hangs up mid-stream is not a relay failure: counting it as
    // one would let three disconnects trip the breaker on a relay that was
    // serving perfectly. Hence three outcomes rather than a bool.
    enum class AttemptOutcome { Success, Failure, Aborted };

    void recordAttempt(std::string_view provider, AttemptOutcome outcome, double latency_ms,
                       std::uint64_t bytes, std::uint64_t prompt_tokens,
                       std::uint64_t completion_tokens) {
        std::scoped_lock lock{telemetry_mutex};
        auto &stat = statFor(provider);
        ++stat.requests;
        switch (outcome) {
        case AttemptOutcome::Success: ++stat.successes; break;
        case AttemptOutcome::Failure: ++stat.failures; break;
        case AttemptOutcome::Aborted: ++stat.aborted; break;
        }
        stat.bytes_out += bytes;
        stat.tokens_prompt += prompt_tokens;
        stat.tokens_completion += completion_tokens;
        stat.last_used_unix = nowUnix();
        if (latency_ms > 0.0) {
            stat.latency_ms_last = latency_ms;
            stat.latency_ms_avg =
                stat.latency_ms_avg == 0.0 ? latency_ms : stat.latency_ms_avg * 0.75 + latency_ms * 0.25;
        }
        tokens_prompt += prompt_tokens;
        tokens_completion += completion_tokens;
    }

    // A relay that answered a request an earlier relay had already failed.
    // Counted separately from `successes` because "this relay caught the fall"
    // is a different fact from "this relay was asked and worked".
    void noteAbsorbed(std::string_view provider) {
        std::scoped_lock lock{telemetry_mutex};
        ++statFor(provider).retries_in;
    }

    void logFailover(const RequestContext &ctx, const std::string &provider, int attempt,
                     int status, std::string message) {
        LogEntry entry;
        entry.level = "warn";
        entry.request_id = ctx.id;
        entry.kind = ctx.kind;
        entry.model = ctx.model;
        entry.provider = provider;
        entry.status = status;
        entry.attempt = attempt + 1;
        entry.failover = true;
        entry.message = std::move(message);
        if (ctx.config.server.log_bodies) {
            entry.request_body =
                truncateUtf8(ctx.body, static_cast<std::size_t>(ctx.config.server.log_body_limit));
        }
        appendLog(std::move(entry));
    }

    // ── auth ─────────────────────────────────────────────────────────────────

    static bool authorized(const h::Request &req, const AppConfig &cfg) {
        if (cfg.server.api_key.empty()) {
            return true;
        }
        std::map<std::string, std::string, std::less<>> headers;
        for (const auto &[name, value] : req.headers) {
            headers.emplace(toLower(name), value);
        }
        return secureEquals(extractApiKey(headers), cfg.server.api_key);
    }

    static void applyCors(h::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers",
                       "Authorization, Content-Type, x-api-key, openai-beta, openai-organization, "
                       "anthropic-version, anthropic-beta");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Max-Age", "86400");
    }

    // ── candidate ordering ───────────────────────────────────────────────────

    // Healthy first, author order second. The proxy only reaches a skipped
    // candidate once everything healthy has been tried, and by then "least
    // recently failed" is the best remaining guess.
    static std::vector<Candidate> order(std::vector<Candidate> candidates) {
        std::stable_partition(candidates.begin(), candidates.end(),
                              [](const Candidate &candidate) { return !candidate.skipped; });
        return candidates;
    }

    static std::size_t attemptBudget(const AppConfig &cfg, std::size_t candidates) {
        if (cfg.server.max_attempts <= 0) {
            return candidates;
        }
        return std::min<std::size_t>(candidates, static_cast<std::size_t>(cfg.server.max_attempts));
    }

    // ── the chat / embeddings pipeline ───────────────────────────────────────

    void serveJson(const h::Request &req, h::Response &res, std::string_view kind) {
        RequestContext ctx;
        ctx.id = hexId(6);
        ctx.kind = std::string{kind};
        ctx.body = req.body;
        ctx.started = nowUnix();
        ctx.config = snapshotConfig();

        std::string ingress_protocol = "openai";
        if (kind == "anthropic") {
            ingress_protocol = "anthropic";
        } else if (kind == "gemini" || kind == "gemini_stream") {
            ingress_protocol = "gemini";
        } else if (kind == "responses") {
            ingress_protocol = "openai_responses";
        }

        if (ingress_protocol == "gemini" && req.matches.size() > 1) {
            ctx.model = req.matches[1].str();
        }

        std::string effective_req_body = req.body;
        if (ingress_protocol == "openai_responses") {
            effective_req_body = adaptResponsesToChat(req.body);
        } else if (ingress_protocol == "anthropic") {
            effective_req_body = adaptAnthropicToChat(req.body);
        } else if (ingress_protocol == "gemini") {
            effective_req_body = adaptGeminiToChat(req.body, ctx.model);
        }

        const json body = json::parse(effective_req_body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            sendError(res, 400, "request body is not a JSON object");
            finish(ctx, {}, res.status, 0, "malformed JSON body");
            return;
        }

        if (ctx.model.empty()) {
            if (const auto it = body.find("model"); it != body.end() && it->is_string()) {
                ctx.model = it->get<std::string>();
            }
        }
        if (ctx.model.empty()) {
            sendError(res, 400, "`model` is required");
            finish(ctx, {}, res.status, 0, "missing model");
            return;
        }

        if (kind == "gemini_stream") {
            ctx.stream = true;
        } else if (ingress_protocol == "gemini") {
            ctx.stream = req.has_param("alt") && req.get_param_value("alt") == "sse";
        } else {
            ctx.stream = (kind == "chat" || kind == "responses" || kind == "anthropic") &&
                         body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>();
        }

        auto candidates = order(router.candidatesFor(ctx.model));
        if (candidates.empty()) {
            sendError(res, 404,
                      std::format("no relay can serve `{}`. Add it to a route, or list it in a "
                                  "relay's `models` array.",
                                  ctx.model),
                      "invalid_request_error", "model_not_found");
            finish(ctx, {}, res.status, 0, "no candidate");
            return;
        }

        const std::size_t budget = attemptBudget(ctx.config, candidates.size());
        std::string last_error;
        int last_status = 0;

        for (std::size_t attempt = 0; attempt < budget; ++attempt) {
            const Candidate &candidate = candidates[attempt];
            const ProviderConfig *provider = ctx.config.provider(candidate.provider);
            if (provider == nullptr || !provider->enabled) {
                continue;
            }

            const std::string upstream_model =
                candidate.model.empty() ? ctx.model : candidate.model;
            const bool effective_stream = ctx.stream && provider->supports_stream;

            const std::string egress_protocol = provider->protocol.empty() ? "openai" : provider->protocol;
            const bool same_protocol = ciEqual(ingress_protocol, egress_protocol);

            const std::string path =
                kind == "embeddings" ? provider->embeddings_path
                                     : resolveChatPath(*provider, upstream_model, effective_stream);
            std::string payload = req.body;
            if (kind != "embeddings") {
                if (same_protocol) {
                    // Direct passthrough! When model renaming is configured, rewrite model only if not Gemini (Gemini embeds in path)
                    if (!candidate.model.empty() && candidate.model != ctx.model && egress_protocol != "gemini") {
                        const json parsed_req = json::parse(req.body, nullptr, false);
                        if (!parsed_req.is_discarded() && parsed_req.is_object()) {
                            json patched = parsed_req;
                            patched["model"] = upstream_model;
                            payload = patched.dump();
                        } else {
                            payload = req.body;
                        }
                    } else {
                        payload = req.body;
                    }
                } else {
                    payload = adaptChatRequest(*provider, upstream_model, effective_req_body, effective_stream);
                }
            }

            if (ctx.stream) {
                last_status = relayStream(ctx, res, *provider, candidate, path, payload, attempt,
                                          budget, last_error, ingress_protocol);
                if (last_status == 0) {
                    return; // committed: the response is the client's now
                }
                continue;
            }

            UpstreamResult result = upstreamPost(*provider, path, payload);
            if (!result.ok) {
                last_error = std::format("{}: {}", provider->id, result.error);
                last_status = 502;
                router.recordFailure(provider->id, result.error, nowUnix());
                recordAttempt(provider->id, AttemptOutcome::Failure, result.latency_ms, 0, 0, 0);
                continue;
            }

            const bool is_cf_block = isCloudflareBlocked(result.status, result.headers, result.body);
            const bool is_html_err = result.status >= 400 && isHtmlResponse(result.headers, result.body);
            const bool retryable = Router::retryableStatus(result.status) || is_cf_block || (result.status == 403 && is_html_err);
            const bool more = attempt + 1 < budget;

            if (retryable && more) {
                last_error = is_cf_block
                                 ? std::format("{}: Cloudflare WAF blocked (HTTP {})", provider->id, result.status)
                                 : std::format("{}: HTTP {}", provider->id, result.status);
                last_status = result.status;
                router.recordFailure(provider->id, last_error, nowUnix());
                recordAttempt(provider->id, AttemptOutcome::Failure, result.latency_ms, 0, 0, 0);
                logFailover(ctx, provider->id, static_cast<int>(attempt), result.status,
                            std::format("{} — failing over", last_error));
                continue;
            }

            // Terminal: a success, or a status no other relay could fix.
            // Either way the answer is passed through — which is what lets a
            // relay's own 400 ("context length exceeded") reach the user
            // unedited instead of becoming a generic gateway error.
            //
            // If the upstream returned an HTML error page (e.g. Cloudflare WAF
            // block, Nginx 502 HTML), wrap it as standard OpenAI error JSON
            // so client libraries (OpenAI Python SDK, LangChain) do not crash
            // with JSONDecodeError.
            const bool good = result.status >= 200 && result.status < 300;
            const bool relay_behaved = good || (!retryable && !is_html_err && result.status != 403);
            if (relay_behaved) {
                router.recordSuccess(provider->id, result.latency_ms, nowUnix());
            } else {
                router.recordFailure(provider->id, std::format("HTTP {}", result.status),
                                     nowUnix());
            }

            ProviderStat usage;
            ProxyServer::accumulateUsage(result.body, usage);
            recordAttempt(provider->id,
                          relay_behaved ? AttemptOutcome::Success : AttemptOutcome::Failure,
                          result.latency_ms, result.body.size(), usage.tokens_prompt,
                          usage.tokens_completion);
            if (good && attempt > 0) {
                noteAbsorbed(provider->id);
            }

            std::string out_body = result.body;
            std::string out_content_type = contentTypeOf(result);
            if (is_html_err) {
                const std::string msg = summarizeHtmlError(provider->id, result.status,
                                                           result.headers, result.body);
                out_body = errorBody(msg, "upstream_error", is_cf_block ? "cf_blocked" : "upstream_error").dump();
                out_content_type = "application/json";
            } else if (good && kind != "embeddings") {
                if (same_protocol) {
                    out_body = result.body;
                    out_content_type = contentTypeOf(result);
                } else {
                    std::string canonical_resp = adaptChatResponse(*provider, result.body, ctx.model);
                    if (ingress_protocol == "openai") {
                        out_body = canonical_resp;
                    } else if (ingress_protocol == "openai_responses") {
                        out_body = adaptChatToResponses(canonical_resp, ctx.model);
                    } else if (ingress_protocol == "anthropic") {
                        out_body = adaptChatToAnthropic(canonical_resp, ctx.model);
                    } else if (ingress_protocol == "gemini") {
                        out_body = adaptChatToGemini(canonical_resp, ctx.model);
                    }
                    out_content_type = "application/json";
                }
            }

            res.status = result.status;
            for (const auto &[name, value] : result.headers) {
                if (!isHopByHop(name) && (!is_html_err || !ciEqual(name, "content-type"))) {
                    res.set_header(name, value);
                }
            }
            res.set_content(out_body, out_content_type);

            finish(ctx, provider->id, result.status, out_body.size(),
                   std::format("{} → {}", ctx.model, upstream_model),
                   /*failover=*/attempt > 0, static_cast<int>(attempt) + 1,
                   static_cast<int>(budget));
            return;
        }

        const int status = last_status != 0 && !Router::retryableStatus(last_status)
                               ? last_status
                               : 503;
        if (last_error.empty()) {
            last_error = std::format("every relay for `{}` was skipped or disabled", ctx.model);
        }
        sendError(res, status, last_error, "upstream_error", "all_relays_failed");
        finish(ctx, {}, status, 0, last_error, /*failover=*/budget > 1,
               static_cast<int>(budget), static_cast<int>(budget));
    }

    // Runs one streaming attempt. Returns 0 when the response was committed —
    // the caller must then return — and the HTTP status it failed with
    // otherwise.
    int relayStream(const RequestContext &ctx, h::Response &res, const ProviderConfig &provider,
                    const Candidate &candidate, const std::string &path,
                    const std::string &payload, std::size_t attempt, std::size_t budget,
                    std::string &last_error, const std::string &ingress_protocol) {
        std::string root;
        std::string prefix;
        std::string scheme;
        if (!splitBaseUrl(provider.base_url, root, prefix, scheme)) {
            last_error = std::format("{}: invalid base_url", provider.id);
            router.recordFailure(provider.id, "invalid base_url", nowUnix());
            recordAttempt(provider.id, AttemptOutcome::Failure, 0.0, 0, 0, 0);
            return 502;
        }

        auto bridge = std::make_shared<StreamBridge>();
        auto client = std::make_shared<h::Client>(root);
        client->set_follow_location(true);
        client->set_connection_timeout(provider.connect_timeout_sec, 0);
        client->set_read_timeout(provider.timeout_sec, 0);
        client->set_write_timeout(provider.timeout_sec, 0);
        client->set_keep_alive(true);
        // The streamed leg dials its own connection, so it needs the same trust
        // store the pooled path resolves — see resolveCaBundle().
        if (const auto bundle = resolveCaBundle(); !bundle.empty()) {
            client->set_ca_cert_path(bundle.string());
            client->enable_server_certificate_verification(true);
        }
        bridge->client = client;

        h::Request upstream;
        upstream.method = "POST";
        upstream.path = joinPath(prefix, path);
        upstream.body = payload;
        upstream.set_header("Content-Type", "application/json");
        upstream.set_header("Accept", "text/event-stream");
        upstream.set_header("User-Agent", std::string{kUserAgent});
        const std::string key = resolveSecret(provider.api_key);
        if (!key.empty()) {
            if (ciEqual(provider.protocol, "anthropic")) {
                upstream.set_header("x-api-key", key);
                upstream.set_header("anthropic-version", "2023-06-01");
            } else if (ciEqual(provider.protocol, "gemini")) {
                upstream.set_header("x-goog-api-key", key);
            } else {
                upstream.set_header("Authorization", "Bearer " + key);
            }
        }
        for (const auto &[name, value] : provider.headers) {
            if (!name.empty()) {
                upstream.set_header(name, value);
            }
        }

        upstream.response_handler = [bridge](const h::Response &response) {
            std::scoped_lock lock{bridge->mutex};
            bridge->status = response.status;
            bridge->headers.clear();
            for (const auto &[name, value] : response.headers) {
                bridge->headers.emplace_back(name, value);
            }
            bridge->headers_ready = true;
            bridge->cv.notify_all();
            // The body is wanted either way: on 2xx it is the answer, on a
            // retryable status it is the diagnostic that gets logged.
            return true;
        };

        upstream.content_receiver = [bridge](const char *data, std::size_t length, std::size_t,
                                             std::size_t) {
            std::unique_lock lock{bridge->mutex};
            if (bridge->aborted) {
                return false;
            }
            bridge->cv.wait(lock, [&] {
                return bridge->aborted || bridge->queued_bytes < kStreamHighWatermark;
            });
            if (bridge->aborted) {
                return false;
            }
            bridge->queue.emplace_back(data, length);
            bridge->queued_bytes += length;
            bridge->cv.notify_all();
            return true;
        };

        const double attempt_started = nowUnix();
        std::thread reader([bridge, client, upstream = std::move(upstream)]() mutable {
            auto result = client->send(upstream);
            std::scoped_lock lock{bridge->mutex};
            if (!result) {
                bridge->error = errorText(result.error());
            }
            bridge->finished = true;
            bridge->cv.notify_all();
        });

        // ── the failover gate ───────────────────────────────────────────────
        // Wait for headers-or-death. The deadline is generous because a relay
        // legitimately takes a while to first token on a large prompt; it
        // exists so a black-holed connection cannot pin a worker forever.
        {
            std::unique_lock lock{bridge->mutex};
            bridge->cv.wait_for(lock, std::chrono::seconds(std::max(10, provider.timeout_sec)),
                                [&] { return bridge->headers_ready || bridge->finished; });
        }

        const bool got_headers = [&] {
            std::scoped_lock lock{bridge->mutex};
            return bridge->headers_ready;
        }();

        if (!got_headers) {
            // Transport failure before any byte: safe to fail over.
            {
                std::scoped_lock lock{bridge->mutex};
                bridge->aborted = true;
                bridge->cv.notify_all();
            }
            client->stop();
            if (reader.joinable()) {
                reader.join();
            }

            std::string reason;
            {
                std::scoped_lock lock{bridge->mutex};
                reason = bridge->error.empty() ? "no response headers" : bridge->error;
            }
            last_error = std::format("{}: {}", provider.id, reason);
            router.recordFailure(provider.id, reason, nowUnix());
            recordAttempt(provider.id, AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, 0, 0, 0);
            logFailover(ctx, provider.id, static_cast<int>(attempt), 0,
                        std::format("{} — failing over", reason));
            return 502;
        }

        int status = 0;
        {
            std::scoped_lock lock{bridge->mutex};
            status = bridge->status;
        }

        if (Router::retryableStatus(status) && attempt + 1 < budget) {
            // Drain the error body first: it is the diagnostic the log keeps,
            // and on the last candidate it is what the client would have seen.
            std::string detail;
            {
                std::unique_lock lock{bridge->mutex};
                bridge->cv.wait_for(lock, std::chrono::seconds(10),
                                    [&] { return bridge->finished; });
                for (const auto &chunk : bridge->queue) {
                    detail.append(chunk);
                    if (detail.size() > 2048) {
                        break;
                    }
                }
                bridge->aborted = true;
                bridge->cv.notify_all();
            }
            client->stop();
            if (reader.joinable()) {
                reader.join();
            }

            last_error =
                std::format("{}: HTTP {} {}", provider.id, status, truncateUtf8(trim(detail), 160));
            router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix());
            recordAttempt(provider.id, AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, 0, 0, 0);
            logFailover(ctx, provider.id, static_cast<int>(attempt), status,
                        std::format("HTTP {} — failing over", status));
            return status;
        }

        // ── a committed error is not a stream ───────────────────────────────
        // A relay's own 4xx/5xx that survives the failover gate is final, and
        // its body is a JSON error, not an event stream. Answering it with a
        // chunked provider would mislabel the content type AND hand httplib two
        // contradictory framings — and on that path httplib's body writer does
        // not install the sink's done() callback, so the provider's own
        // stream-termination call threw bad_function_call and killed the
        // process. Drain it and hand it back exactly like the non-streaming
        // path does: same status, same body, same headers.
        if (status >= 400) {
            std::string body;
            {
                std::unique_lock lock{bridge->mutex};
                bridge->cv.wait_for(lock, std::chrono::seconds(10),
                                    [&] { return bridge->finished; });
                for (const auto &chunk : bridge->queue) {
                    body.append(chunk);
                    if (body.size() >= kMaxErrorBody) {
                        break;
                    }
                }
                bridge->aborted = true;
                bridge->cv.notify_all();
            }
            client->stop();
            if (reader.joinable()) {
                reader.join();
            }

            std::vector<std::pair<std::string, std::string>> error_headers;
            {
                std::scoped_lock lock{bridge->mutex};
                error_headers = bridge->headers;
            }

            const bool is_cf_block = isCloudflareBlocked(status, error_headers, body);
            const bool is_html_err = isHtmlResponse(error_headers, body);
            const bool retryable = Router::retryableStatus(status) || is_cf_block || (status == 403 && is_html_err);

            if (retryable && attempt + 1 < budget) {
                last_error = is_cf_block
                                 ? std::format("{}: Cloudflare WAF blocked (HTTP {})", provider.id, status)
                                 : std::format("{}: HTTP {}", provider.id, status);
                router.recordFailure(provider.id, last_error, nowUnix());
                recordAttempt(provider.id, AttemptOutcome::Failure,
                              (nowUnix() - attempt_started) * 1000.0, 0, 0, 0);
                logFailover(ctx, provider.id, static_cast<int>(attempt), status,
                            std::format("{} — failing over", last_error));
                return status;
            }

            const bool ok_status = status >= 200 && status < 300;
            const bool relay_behaved = ok_status || (!retryable && !is_html_err && status != 403);
            if (relay_behaved) {
                router.recordSuccess(provider.id, 0.0, nowUnix());
            } else {
                router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix());
            }
            recordAttempt(provider.id,
                          relay_behaved ? AttemptOutcome::Success : AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, body.size(), 0, 0);

            std::string out_body = body;
            std::string out_content_type = contentTypeOf(error_headers);
            if (is_html_err) {
                const std::string msg = summarizeHtmlError(provider.id, status,
                                                           error_headers, body);
                out_body = errorBody(msg, "upstream_error", is_cf_block ? "cf_blocked" : "upstream_error").dump();
                out_content_type = "application/json";
            } else if (out_body.empty()) {
                // A relay that failed with a bare status still owes the caller
                // the OpenAI error shape every other failure path produces.
                out_body = errorBody(std::format("{}", provider.id), "upstream_error",
                                     "upstream_error")
                               .dump();
                out_content_type = "application/json";
            }

            res.status = status;
            for (const auto &[name, value] : error_headers) {
                if (!isHopByHop(name) && (!is_html_err || !ciEqual(name, "content-type"))) {
                    res.set_header(name, value);
                }
            }
            res.set_content(out_body, out_content_type);
            finish(ctx, provider.id, status, out_body.size(),
                   std::format("stream rejected with HTTP {} — returned verbatim", status),
                   attempt > 0, static_cast<int>(attempt) + 1, static_cast<int>(budget));
            return 0;
        }

        // ── committed ───────────────────────────────────────────────────────
        // Past the gate the status is final, retryable or not. It still has to
        // be classified before it is recorded: committing a relay's 429 as a
        // success would hide exactly the relay that needs the breaker.
        const bool good = status >= 200 && status < 300;
        const bool relay_ok = good || !Router::retryableStatus(status);
        if (relay_ok) {
            router.recordSuccess(provider.id, 0.0, nowUnix());
        } else {
            router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix());
        }

        std::vector<std::pair<std::string, std::string>> headers;
        {
            std::scoped_lock lock{bridge->mutex};
            headers = bridge->headers;
        }
        res.status = status;
        for (const auto &[name, value] : headers) {
            if (!isHopByHop(name)) {
                res.set_header(name, value);
            }
        }

        // Keep the relay's own content type when it named one; SSE is the
        // default because that is what a streamed chat answer is.
        std::string stream_type = headerValue(headers, "content-type");
        if (stream_type.empty() || !startsWith(toLower(stream_type), "text/event-stream")) {
            stream_type = "text/event-stream";
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        const std::string provider_id = provider.id;
        const std::string upstream_model = candidate.model.empty() ? ctx.model : candidate.model;
        const bool failover = attempt > 0;
        const bool absorbed = failover && good;
        const bool stream_ok = relay_ok;
        const int attempt_number = static_cast<int>(attempt) + 1;
        const int total_attempts = static_cast<int>(budget);
        // Captured by value: the request context must outlive the handler call,
        // and by-reference would dangle the moment serveJson returns.
        const RequestContext request_ctx = ctx;

        const std::string egress_proto = provider.protocol.empty() ? "openai" : provider.protocol;
        const std::string ingress_proto = ingress_protocol.empty() ? "openai" : ingress_protocol;
        const bool same_protocol = ciEqual(ingress_proto, egress_proto);

        std::shared_ptr<StreamProtocolAdapter> adapter;
        if (!same_protocol) {
            adapter = std::make_shared<StreamProtocolAdapter>(egress_proto, ingress_proto, ctx.model, request_ctx.id);
            stream_type = "text/event-stream";
        }

        res.set_chunked_content_provider(
            stream_type,
            [this, bridge, client, provider_id, upstream_model, request_ctx, failover, absorbed,
             stream_ok, attempt_number, total_attempts, attempt_started,
             adapter](std::size_t, h::DataSink &sink) -> bool {
                for (;;) {
                    std::string chunk;
                    bool done = false;
                    {
                        std::unique_lock lock{bridge->mutex};
                        if (!bridge->queue.empty()) {
                            chunk = std::move(bridge->queue.front());
                            bridge->queue.pop_front();
                            bridge->queued_bytes -= chunk.size();
                        } else if (bridge->finished) {
                            done = true;
                        } else if (bridge->aborted) {
                            return false;
                        } else {
                            bridge->cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                                return !bridge->queue.empty() || bridge->finished || bridge->aborted;
                            });
                            continue;
                        }
                    }
                    // Wake the reader: it may have been parked on the
                    // high-water mark that this pop just cleared.
                    bridge->cv.notify_all();

                    if (done) {
                        if (adapter) {
                            const std::string fin = adapter->finish();
                            if (!fin.empty()) {
                                const bool writable = !sink.is_writable || sink.is_writable();
                                if (writable) {
                                    sink.write(fin.data(), fin.size());
                                    bridge->bytes_out += fin.size();
                                }
                            }
                        }
                        // httplib only installs the sink's done() callback on
                        // the two chunked body writers; the Content-Length
                        // writer calls the provider directly and leaves it
                        // empty. Terminating through an unset std::function
                        // throws bad_function_call inside the server's request
                        // thread, which std::terminate turns into a dead
                        // process — so this is checked, not assumed. The code
                        // above keeps the two framings from ever meeting, and
                        // this is the belt to that pair of braces.
                        if (sink.done) {
                            sink.done();
                        }
                        const double latency = (nowUnix() - attempt_started) * 1000.0;
                        const std::uint64_t bytes = bridge->bytes_out.load();
                        recordAttempt(provider_id,
                                      stream_ok ? AttemptOutcome::Success
                                                : AttemptOutcome::Failure,
                                      latency, bytes, 0, 0);
                        if (absorbed) {
                            noteAbsorbed(provider_id);
                        }
                        finish(request_ctx, provider_id, bridge->status, bytes,
                                std::format("stream complete · {} · {}", humanBytes(bytes),
                                           humanMillis(latency)),
                               failover, attempt_number, total_attempts);
                        return true;
                    }

                    if (chunk.empty()) {
                        continue;
                    }

                    std::string send_chunk;
                    if (adapter) {
                        send_chunk = adapter->feed(chunk);
                    } else {
                        send_chunk = std::move(chunk);
                    }

                    if (send_chunk.empty()) {
                        continue;
                    }

                    // A dead peer can also be caught before the write buffer
                    // notices, and a half-written chunk is worse than a clean
                    // close. Treated exactly like a failed write.
                    const bool writable = !sink.is_writable || sink.is_writable();
                    if (!writable || !sink.write(send_chunk.data(), send_chunk.size())) {
                        // The client hung up. Tell the reader, and unblock it
                        // from a socket nobody is waiting on any more.
                        {
                            std::scoped_lock lock{bridge->mutex};
                            bridge->aborted = true;
                        }
                        bridge->cv.notify_all();
                        client->stop();
                        const double latency = (nowUnix() - attempt_started) * 1000.0;
                        const std::uint64_t bytes = bridge->bytes_out.load();
                        recordAttempt(provider_id, AttemptOutcome::Aborted, latency, bytes, 0, 0);
                        finish(request_ctx, provider_id, 0, bytes,
                               "client disconnected before the stream ended", failover,
                               attempt_number, total_attempts);
                        return false;
                    }
                    bridge->bytes_out += send_chunk.size();
                }
            },
            [this, bridge, client, provider_id, request_ctx, failover, attempt_number,
             total_attempts](bool) {
                // Provider released: the reader must not outlive it.
                {
                    std::scoped_lock lock{bridge->mutex};
                    bridge->aborted = true;
                }
                bridge->cv.notify_all();
                client->stop();

                // finish() is idempotent, so this is a no-op on every path
                // that already accounted for the request. It is here for the
                // one path that has no other reporter: httplib released the
                // provider WITHOUT ever calling it — a client that vanished
                // between the handler returning and the body being written.
                // Without this the in-flight counter would never come down and
                // the console would report a permanently busy server.
                finish(request_ctx, provider_id, 0, bridge->bytes_out.load(),
                       "stream ended before any bytes were written", failover, attempt_number,
                       total_attempts);
            });

        // The provider owns the bridge now; the reader ends with the stream.
        reader.detach();
        return 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ProxyServer
// ─────────────────────────────────────────────────────────────────────────────

ProxyServer::ProxyServer() : impl_(std::make_unique<Impl>()) {}

ProxyServer::~ProxyServer() {
    stop();
}

bool ProxyServer::running() const {
    return impl_->running.load();
}

std::string ProxyServer::boundAddress() const {
    return impl_->snapshotConfig().server.host;
}

int ProxyServer::boundPort() const {
    return impl_->bound_port.load();
}

void ProxyServer::setConfigPath(std::string path) {
    impl_->setConfigPath(std::move(path));
}

std::expected<void, std::string> ProxyServer::start(const AppConfig &config) {
    if (impl_->running.load()) {
        return std::unexpected(std::string{"server is already running"});
    }

    {
        std::scoped_lock lock{impl_->config_mutex};
        impl_->config = config;
    }
    impl_->router.setConfig(config);
    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        impl_->log.setCapacity(static_cast<std::size_t>(std::max(16, config.server.log_capacity)));
    }
    impl_->stopping.store(false);

    auto &server = *(impl_->server = std::make_unique<h::Server>());
    server.set_payload_max_length(kMaxRequestBody);
    server.set_keep_alive_max_count(64);
    // 0 means "no write deadline". A streamed answer can legitimately idle for
    // a long time between tokens, and a write timeout would cut it off.
    server.set_write_timeout(0, 0);

    server.set_pre_routing_handler([this](const h::Request &req, h::Response &res) {
        Impl::applyCors(res);
        if (req.method == "OPTIONS") {
            res.status = 204;
            return h::Server::HandlerResponse::Handled;
        }
        if (Impl::authorized(req, impl_->snapshotConfig())) {
            return h::Server::HandlerResponse::Unhandled;
        }
        sendError(res, 401, "missing or invalid API key; send `Authorization: Bearer <key>`",
                  "authentication_error", "invalid_api_key");
        return h::Server::HandlerResponse::Handled;
    });

    server.set_exception_handler(
        [](const h::Request &, h::Response &res, std::exception_ptr ep) {
            std::string detail = "unknown error";
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception &ex) {
                detail = ex.what();
            } catch (...) {
            }
            sendError(res, 500, std::format("literouter internal error: {}", detail),
                      "internal_error", "internal_error");
        });

    server.set_error_handler([](const h::Request &, h::Response &res) {
        // A provider-backed response owns its own framing: injecting a body
        // here would leave httplib with a Content-Length and a live content
        // provider at the same time, and it resolves that contradiction by
        // taking the Content-Length path — where the provider's sink has no
        // done() callback. Both conditions matter.
        if (res.body.empty() && !res.content_provider_) {
            const int status = res.status > 0 ? res.status : 500;
            sendError(res, status, "the request could not be routed", "invalid_request_error",
                      "not_found");
        }
    });

    // ── OpenAI surface ──────────────────────────────────────────────────────

    const auto pipeline = [this](const h::Request &req, h::Response &res, std::string_view kind) {
        impl_->active_requests.fetch_add(1, std::memory_order_relaxed);
        try {
            impl_->serveJson(req, res, kind);
        } catch (...) {
            // A throw before finish() would otherwise leak the counter and make
            // the console report a permanently busy server.
            impl_->releaseInFlight();
            throw;
        }
    };

    server.Post("/v1/chat/completions",
                [pipeline](const h::Request &req, h::Response &res) { pipeline(req, res, "chat"); });
    // A few clients drop the /v1. Same handler, so behaviour cannot diverge
    // between the two spellings.
    server.Post("/chat/completions",
                [pipeline](const h::Request &req, h::Response &res) { pipeline(req, res, "chat"); });
    server.Post("/v1/embeddings", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "embeddings");
    });
    server.Post("/v1/completions",
                [pipeline](const h::Request &req, h::Response &res) { pipeline(req, res, "chat"); });
    server.Post("/v1/responses", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "responses");
    });
    server.Post("/responses", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "responses");
    });

    // Anthropic Messages API
    server.Post("/v1/messages", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "anthropic");
    });
    server.Post("/messages", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "anthropic");
    });

    // Google Gemini API
    server.Post(R"(/v1beta/models/(.*?):generateContent)", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "gemini");
    });
    server.Post(R"(/v1/models/(.*?):generateContent)", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "gemini");
    });
    server.Post(R"(/v1beta/models/(.*?):streamGenerateContent)", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "gemini_stream");
    });
    server.Post(R"(/v1/models/(.*?):streamGenerateContent)", [pipeline](const h::Request &req, h::Response &res) {
        pipeline(req, res, "gemini_stream");
    });

    server.Get("/v1beta/models", [this](const h::Request &, h::Response &res) {
        const AppConfig cfg = impl_->snapshotConfig();
        json models = json::array();
        for (const auto &name : cfg.logicalModels()) {
            json item = json::object();
            item["name"] = "models/" + name;
            item["version"] = "001";
            item["displayName"] = name;
            item["description"] = "Literouter model " + name;
            item["supportedGenerationMethods"] = json::array({"generateContent", "streamGenerateContent", "countTokens"});
            models.push_back(std::move(item));
        }
        res.status = 200;
        res.set_content(json{{"models", std::move(models)}}.dump(), "application/json");
    });

    server.Get("/v1/models", [this](const h::Request &, h::Response &res) {
        const AppConfig cfg = impl_->snapshotConfig();
        json data = json::array();
        for (const auto &name : cfg.logicalModels()) {
            json item = json::object();
            item["id"] = name;
            item["object"] = "model";
            item["owned_by"] = "literouter";
            if (const RouteConfig *route = cfg.route(name); route != nullptr) {
                json hops = json::array();
                for (const auto &target : route->targets) {
                    hops.push_back(target.model.empty()
                                       ? target.provider
                                       : std::format("{}:{}", target.provider, target.model));
                }
                item["literouter"] = {{"kind", "route"},
                                      {"targets", std::move(hops)},
                                      {"enabled", route->enabled}};
            } else {
                json relays = json::array();
                for (const auto &provider : cfg.providers) {
                    if (provider.enabled && std::ranges::find(provider.models, name) !=
                                                provider.models.end()) {
                        relays.push_back(provider.id);
                    }
                }
                item["literouter"] = {{"kind", "passthrough"}, {"targets", std::move(relays)}};
            }
            data.push_back(std::move(item));
        }
        res.status = 200;
        res.set_content(json{{"object", "list"}, {"data", std::move(data)}}.dump(),
                        "application/json");
    });

    server.Get(R"(/v1/models/(.+))", [this](const h::Request &req, h::Response &res) {
        const AppConfig cfg = impl_->snapshotConfig();
        const std::string name = req.matches[1];
        const auto known = cfg.logicalModels();
        if (std::ranges::find(known, name) == known.end()) {
            sendError(res, 404, std::format("unknown model `{}`", name), "invalid_request_error",
                      "model_not_found");
            return;
        }
        res.status = 200;
        res.set_content(json{{"id", name}, {"object", "model"}, {"owned_by", "literouter"}}.dump(),
                        "application/json");
    });

    server.Get("/health", [](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // ── management surface ──────────────────────────────────────────────────

    const std::string admin{kAdminPrefix};

    server.Get(admin + "/status", [this](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(toJsonString(snapshot()), "application/json");
    });

    server.Get(admin + "/logs", [this](const h::Request &req, h::Response &res) {
        std::uint64_t since = 0;
        std::size_t limit = 500;
        if (req.has_param("since")) {
            try {
                since = std::stoull(req.get_param_value("since"));
            } catch (...) {
            }
        }
        if (req.has_param("limit")) {
            try {
                limit = std::min<std::size_t>(2000, std::stoul(req.get_param_value("limit")));
            } catch (...) {
            }
        }
        json entries = json::array();
        for (const auto &entry : logsSince(since, limit)) {
            if (auto parsed = json::parse(toJsonString(entry), nullptr, false);
                !parsed.is_discarded()) {
                entries.push_back(std::move(parsed));
            }
        }
        json root = json::object();
        root["entries"] = std::move(entries);
        root["seq"] = impl_->log.next_seq > 0 ? impl_->log.next_seq - 1 : 0;
        res.status = 200;
        res.set_content(root.dump(), "application/json");
    });

    server.Post(admin + "/reload", [this](const h::Request &, h::Response &res) {
        const std::string path = impl_->configPath();
        if (path.empty()) {
            sendError(res, 400, "this server was started without a config path");
            return;
        }
        auto loaded = ConfigStore::load(path);
        if (!loaded) {
            sendError(res, 422, loaded.error(), "invalid_config", "config_invalid");
            return;
        }
        const ValidationReport report = validate(loaded->config());
        updateConfig(loaded->config());

        json issues = json::array();
        for (const auto &issue : report.issues) {
            issues.push_back({{"level", issue.levelName()},
                              {"path", issue.path},
                              {"message", issue.message}});
        }
        json root = json::object();
        root["ok"] = report.ok();
        root["summary"] = report.summary();
        root["issues"] = std::move(issues);
        res.status = report.ok() ? 200 : 422;
        res.set_content(root.dump(2), "application/json");
        impl_->recordSystem(report.ok() ? "config reloaded from disk"
                                        : "config reloaded with validation errors",
                            report.ok() ? "info" : "warn");
    });

    server.Post(admin + "/reset-stats", [this](const h::Request &, h::Response &res) {
        resetStats();
        res.status = 200;
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post(admin + "/shutdown", [this](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(R"({"ok":true,"message":"shutting down"})", "application/json");
        // Off the request thread: stop() joins the accept loop, and doing that
        // from inside a handler is the loop waiting on itself.
        std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop();
        }).detach();
    });

    // Probes an unsaved relay straight from the console's form, so "Test" can
    // work before the entry is written to disk.
    server.Post(admin + "/probe", [](const h::Request &req, h::Response &res) {
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            sendError(res, 400, "expected a provider object");
            return;
        }
        ProviderConfig provider;
        provider.base_url = body.value("base_url", std::string{});
        provider.api_key = body.value("api_key", std::string{});
        provider.timeout_sec = std::max(3, body.value("timeout_sec", 20));
        provider.connect_timeout_sec = std::min(20, provider.timeout_sec);
        if (const auto it = body.find("headers"); it != body.end() && it->is_object()) {
            // Explicit iterators: see the note in lr_json.cpp about
            // `iteration_proxy_value` and structured bindings.
            for (auto entry = it->begin(); entry != it->end(); ++entry) {
                if (entry.value().is_string()) {
                    provider.headers.emplace(entry.key(), entry.value().get<std::string>());
                }
            }
        }
        const ProviderProbe probe = probeProvider(provider, provider.timeout_sec);
        res.status = 200;
        res.set_content(toJsonString(probe), "application/json");
    });

    // ── bind ────────────────────────────────────────────────────────────────

    const std::string host = config.server.host;
    int bound = 0;
    if (config.server.port == 0) {
        bound = server.bind_to_any_port(host);
        if (bound <= 0) {
            return std::unexpected(std::format("cannot bind {}:0 — no free port", host));
        }
    } else {
        if (!server.bind_to_port(host, config.server.port)) {
            return std::unexpected(std::format(
                "cannot bind {}:{} — {} (is another literouter already listening?)", host,
                config.server.port, std::strerror(errno)));
        }
        bound = config.server.port;
    }
    impl_->bound_port.store(bound);
    impl_->started_unix = nowUnix();
    impl_->running.store(true);

    impl_->runner = std::thread([this] {
        impl_->server->listen_after_bind();
        impl_->running.store(false);
    });
    impl_->server->wait_until_ready();

    if (!impl_->running.load()) {
        if (impl_->runner.joinable()) {
            impl_->runner.join();
        }
        return std::unexpected(std::string{"the accept loop stopped during startup"});
    }

    impl_->recordSystem(std::format("listening on http://{}:{}", host, bound));
    return {};
}

void ProxyServer::stop() {
    if (!impl_ || impl_->stopping.exchange(true)) {
        return;
    }
    // A ProxyServer that was constructed but never started has no httplib Server
    // to stop. This is not a hypothetical edge: the console builds one at
    // startup and only creates the listener when the user presses Start, so
    // reaching through a null unique_ptr here crashed on every clean exit that
    // had not started a server first. The exchange above is what makes the
    // second stop() call (from the destructor) a no-op.
    if (!impl_->server) {
        impl_->running.store(false);
        return;
    }

    impl_->server->stop();
    if (impl_->runner.joinable()) {
        impl_->runner.join();
    }
    // Released rather than kept: the socket is closed, the accept loop is
    // joined, and no handler can run again, so holding the object would only
    // pin a decommissioned listener and its address for the process lifetime.
    impl_->server.reset();
    impl_->running.store(false);
    impl_->recordSystem("stopped");
}

void ProxyServer::updateConfig(const AppConfig &config) {
    {
        std::scoped_lock lock{impl_->config_mutex};
        impl_->config = config;
    }
    impl_->router.setConfig(config);
    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        impl_->log.setCapacity(static_cast<std::size_t>(std::max(16, config.server.log_capacity)));
    }
}

AppConfig ProxyServer::config() const {
    return impl_->snapshotConfig();
}

Snapshot ProxyServer::snapshot() const {
    Snapshot out;
    const AppConfig cfg = impl_->snapshotConfig();

    out.running = impl_->running.load();
    out.host = cfg.server.host;
    out.port = impl_->bound_port.load();
    out.base_url = std::format("http://{}:{}", out.host, out.port);
    out.started_unix = impl_->started_unix;
    out.uptime_sec = out.running ? nowUnix() - impl_->started_unix : 0.0;
    out.version = std::string{kVersion};
    out.breakers_open = impl_->router.openBreakerCount(nowUnix());
    out.active_requests = static_cast<std::uint64_t>(std::max(0, impl_->active_requests.load()));
    out.config_path = impl_->configPath();

    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        out.total_requests = impl_->total_requests;
        out.total_success = impl_->total_success;
        out.total_failure = impl_->total_failure;
        out.bytes_out = impl_->bytes_out;
        out.tokens_prompt = impl_->tokens_prompt;
        out.tokens_completion = impl_->tokens_completion;
        out.latency_ms_avg = impl_->latency_ms_avg;
        out.log_seq = impl_->log.next_seq > 0 ? impl_->log.next_seq - 1 : 0;
        for (const auto &provider : cfg.providers) {
            if (auto it = impl_->stats.find(provider.id); it != impl_->stats.end()) {
                out.providers.push_back(it->second);
            } else {
                ProviderStat empty;
                empty.provider = provider.id;
                out.providers.push_back(std::move(empty));
            }
        }
    }
    out.health = impl_->router.health(nowUnix());
    return out;
}

std::vector<LogEntry> ProxyServer::logsSince(std::uint64_t seq, std::size_t limit) const {
    std::scoped_lock lock{impl_->telemetry_mutex};
    return impl_->log.since(seq, limit);
}

void ProxyServer::clearLogs() {
    std::scoped_lock lock{impl_->telemetry_mutex};
    impl_->log.entries.clear();
}

void ProxyServer::resetStats() {
    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        impl_->stats.clear();
        impl_->total_requests = 0;
        impl_->total_success = 0;
        impl_->total_failure = 0;
        impl_->bytes_out = 0;
        impl_->tokens_prompt = 0;
        impl_->tokens_completion = 0;
        impl_->latency_ms_avg = 0.0;
    }
    impl_->router.resetHealth();
    impl_->recordSystem("counters reset");
}

std::string ProxyServer::adminUrl(const std::string &host, int port, std::string_view path) {
    return std::format("http://{}:{}{}", host, port, path);
}

void ProxyServer::accumulateUsage(std::string_view body, ProviderStat &stat) {
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) {
        return;
    }
    const auto pull = [&](const json &node) {
        if (!node.is_object()) {
            return;
        }
        const auto it = node.find("usage");
        if (it != node.end() && it->is_object()) {
            const auto number = [&](const char *key) -> std::uint64_t {
                const auto value = it->find(key);
                if (value == it->end() || !value->is_number()) {
                    return 0;
                }
                return value->get<std::uint64_t>();
            };
            // OpenAI spells these prompt/completion; the Messages-shaped relays
            // that also expose /v1/chat/completions spell them input/output.
            stat.tokens_prompt += number("prompt_tokens") + number("input_tokens");
            stat.tokens_completion += number("completion_tokens") + number("output_tokens");
        }
        const auto it_gemini = node.find("usageMetadata");
        if (it_gemini != node.end() && it_gemini->is_object()) {
            const auto number = [&](const char *key) -> std::uint64_t {
                const auto value = it_gemini->find(key);
                if (value == it_gemini->end() || !value->is_number()) {
                    return 0;
                }
                return value->get<std::uint64_t>();
            };
            stat.tokens_prompt += number("promptTokenCount");
            stat.tokens_completion += number("candidatesTokenCount");
        }
    };

    if (root.is_object()) {
        pull(root);
    } else if (root.is_array()) {
        for (const auto &item : root) {
            pull(item);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Admin client — what `literouter status` and `literouter log` use.
// ─────────────────────────────────────────────────────────────────────────────

AdminStatus fetchStatus(std::string_view base_url, std::string_view api_key) {
    AdminStatus out;

    std::string root;
    std::string prefix;
    std::string scheme;
    if (!splitBaseUrl(base_url, root, prefix, scheme)) {
        out.error = std::format("invalid address `{}`", base_url);
        return out;
    }

    h::Client client{root};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);

    h::Headers headers;
    headers.emplace("Accept", "application/json");
    if (!api_key.empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", api_key));
    }

    auto result =
        client.Get(std::format("{}{}/status", prefix, ProxyServer::kAdminPrefix), headers);
    if (!result) {
        out.error = errorText(result.error());
        return out;
    }
    if (result->status != 200) {
        out.error =
            std::format("HTTP {} from {}{}/status", result->status, base_url, ProxyServer::kAdminPrefix);
        return out;
    }

    const json parsed = json::parse(result->body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.error = "the server replied with something that is not a status object";
        return out;
    }
    out.reachable = true;

    Snapshot &s = out.snapshot;
    s.running = parsed.value("running", false);
    s.host = parsed.value("host", std::string{});
    s.port = parsed.value("port", 0);
    s.base_url = parsed.value("base_url", std::string{});
    s.uptime_sec = parsed.value("uptime_sec", 0.0);
    s.started_unix = parsed.value("started_unix", 0.0);
    s.config_path = parsed.value("config_path", std::string{});
    s.version = parsed.value("version", std::string{});
    s.total_requests = parsed.value("total_requests", std::uint64_t{0});
    s.total_success = parsed.value("total_success", std::uint64_t{0});
    s.total_failure = parsed.value("total_failure", std::uint64_t{0});
    s.active_requests = parsed.value("active_requests", std::uint64_t{0});
    s.bytes_out = parsed.value("bytes_out", std::uint64_t{0});
    s.tokens_prompt = parsed.value("tokens_prompt", std::uint64_t{0});
    s.tokens_completion = parsed.value("tokens_completion", std::uint64_t{0});
    s.latency_ms_avg = parsed.value("latency_ms_avg", 0.0);
    s.log_seq = parsed.value("log_seq", std::uint64_t{0});
    s.breakers_open = parsed.value("breakers_open", 0);

    if (const auto it = parsed.find("providers"); it != parsed.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (!item.is_object()) {
                continue;
            }
            ProviderStat stat;
            stat.provider = item.value("provider", std::string{});
            stat.requests = item.value("requests", std::uint64_t{0});
            stat.successes = item.value("successes", std::uint64_t{0});
            stat.failures = item.value("failures", std::uint64_t{0});
            stat.aborted = item.value("aborted", std::uint64_t{0});
            stat.retries_in = item.value("retries_in", std::uint64_t{0});
            stat.bytes_out = item.value("bytes_out", std::uint64_t{0});
            stat.bytes_in = item.value("bytes_in", std::uint64_t{0});
            stat.tokens_prompt = item.value("tokens_prompt", std::uint64_t{0});
            stat.tokens_completion = item.value("tokens_completion", std::uint64_t{0});
            stat.latency_ms_last = item.value("latency_ms_last", 0.0);
            stat.latency_ms_avg = item.value("latency_ms_avg", 0.0);
            stat.latency_ms_p95 = item.value("latency_ms_p95", 0.0);
            stat.last_used_unix = item.value("last_used_unix", 0.0);
            s.providers.push_back(std::move(stat));
        }
    }

    if (const auto it = parsed.find("health"); it != parsed.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (!item.is_object()) {
                continue;
            }
            ProviderHealth health;
            health.provider = item.value("provider", std::string{});
            const std::string state = item.value("state", std::string{"unknown"});
            health.state = state == "healthy"    ? ProviderHealth::State::Healthy
                           : state == "open"     ? ProviderHealth::State::Open
                           : state == "degraded" ? ProviderHealth::State::Degraded
                                                 : ProviderHealth::State::Unknown;
            health.consecutive_failures = item.value("consecutive_failures", 0);
            health.total_failures = item.value("total_failures", std::uint64_t{0});
            health.last_error = item.value("last_error", std::string{});
            health.cooldown_remaining = item.value("cooldown_remaining", 0.0);
            s.health.push_back(std::move(health));
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Admin client, part two: the log reader and the management POSTs.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// A client aimed at one running literouter, with the auth header applied once.
// `prefix` receives the base URL's own path component, so `--config` pointing at
// a reverse-proxied instance still works.
std::optional<h::Client> makeAdminClient(std::string_view base_url, std::string_view api_key,
                                         std::string &prefix, std::string &error) {
    std::string root;
    std::string scheme;
    if (!splitBaseUrl(base_url, root, prefix, scheme)) {
        error = std::format("invalid address `{}`", base_url);
        return std::nullopt;
    }
    h::Client client{root};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    if (!api_key.empty()) {
        h::Headers headers;
        headers.emplace("Authorization", std::format("Bearer {}", api_key));
        client.set_default_headers(std::move(headers));
    }
    return client;
}

} // namespace

AdminLogs fetchLogs(std::string_view base_url, std::uint64_t since, std::size_t limit,
                    std::string_view api_key) {
    AdminLogs out;

    std::string prefix;
    std::string error;
    auto client = makeAdminClient(base_url, api_key, prefix, error);
    if (!client) {
        out.error = std::move(error);
        return out;
    }

    auto result = client->Get(std::format("{}{}/logs?since={}&limit={}", prefix,
                                          ProxyServer::kAdminPrefix, since, limit));
    if (!result) {
        out.error = errorText(result.error());
        return out;
    }
    if (result->status != 200) {
        out.error = std::format("HTTP {} from {}{}/logs", result->status, base_url,
                                ProxyServer::kAdminPrefix);
        return out;
    }

    const json parsed = json::parse(result->body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.error = "the server replied with something that is not a log page";
        return out;
    }
    out.reachable = true;
    out.seq = parsed.value("seq", std::uint64_t{0});

    if (const auto it = parsed.find("entries"); it != parsed.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (!item.is_object()) {
                continue;
            }
            LogEntry entry;
            entry.seq = item.value("seq", std::uint64_t{0});
            entry.time_unix = item.value("time_unix", 0.0);
            entry.level = item.value("level", std::string{"info"});
            entry.request_id = item.value("request_id", std::string{});
            entry.kind = item.value("kind", std::string{});
            entry.model = item.value("model", std::string{});
            entry.provider = item.value("provider", std::string{});
            entry.upstream_model = item.value("upstream_model", std::string{});
            entry.status = item.value("status", 0);
            entry.stream = item.value("stream", false);
            entry.failover = item.value("failover", false);
            entry.attempt = item.value("attempt", 1);
            entry.attempts_total = item.value("attempts_total", 1);
            entry.latency_ms = item.value("latency_ms", 0.0);
            entry.bytes = item.value("bytes", std::uint64_t{0});
            entry.message = item.value("message", std::string{});
            entry.request_body = item.value("request_body", std::string{});
            entry.response_body = item.value("response_body", std::string{});
            out.entries.push_back(std::move(entry));
        }
    }
    return out;
}

AdminReply adminPost(std::string_view base_url, std::string_view path, std::string_view body,
                     std::string_view api_key) {
    AdminReply out;

    std::string prefix;
    std::string error;
    auto client = makeAdminClient(base_url, api_key, prefix, error);
    if (!client) {
        out.error = std::move(error);
        return out;
    }

    const std::string target = std::format("{}{}{}", prefix, ProxyServer::kAdminPrefix, path);
    auto result = body.empty() ? client->Post(target)
                               : client->Post(target, std::string{body}, "application/json");
    if (!result) {
        out.error = errorText(result.error());
        return out;
    }
    out.reachable = true;
    out.status = result->status;
    out.body = result->body;
    return out;
}

} // namespace literouter
