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

#include "lr_dump.h"

namespace literouter {

namespace {

using json = nlohmann::json;
namespace h = httplib;

// ── the built-in web console ─────────────────────────────────────────────────
//
// Plain HTML/CSS/JS, embedded in the binary so a headless machine needs nothing
// beside the executable. `__has_embed` is the standard preprocessor feature
// test: where `#embed` is unavailable the assets are read from
// $LITEROUTER_WEB_DIR per request instead, which keeps an ISO-strict compiler
// building at the cost of one directory to carry.
#if defined(__has_embed)
#  if __has_embed("../../web/dist/index.html") && __has_embed("../../web/dist/app.css") && \
      __has_embed("../../web/dist/app.js") && __has_embed("../../web/dist/favicon.svg")
#    define LR_WEB_EMBEDDED 1
#  endif
#endif

#ifdef LR_WEB_EMBEDDED
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wc23-extensions"
#  endif
constexpr unsigned char kWebIndexHtml[] = {
#embed "../../web/dist/index.html"
, 0u};
constexpr unsigned char kWebAppCss[] = {
#embed "../../web/dist/app.css"
, 0u};
constexpr unsigned char kWebAppJs[] = {
#embed "../../web/dist/app.js"
, 0u};
constexpr unsigned char kWebFaviconSvg[] = {
#embed "../../web/dist/favicon.svg"
, 0u};
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  endif

std::string_view embeddedText(const unsigned char *data, std::size_t size) {
    // The trailing 0 appended at each declaration is a terminator, not content.
    return {reinterpret_cast<const char *>(data), size - 1};
}
#endif

struct WebAsset {
    std::string_view name;
    std::string_view content_type;
};

// A whitelist rather than a lookup on the request path: these four names are
// every file the console ships, and the fallback branch joins one of them onto
// a directory, so nothing a caller types can reach outside it.
constexpr std::array<WebAsset, 4> kWebAssets{{
    {"index.html", "text/html; charset=utf-8"},
    {"app.css", "text/css; charset=utf-8"},
    {"app.js", "text/javascript; charset=utf-8"},
    {"favicon.svg", "image/svg+xml"},
}};

const WebAsset *webAssetFor(std::string_view name) {
    for (const auto &asset : kWebAssets) {
        if (asset.name == name) {
            return &asset;
        }
    }
    return nullptr;
}

// Writes one console file into `res`; false when the name is not one we serve.
bool serveWebAsset(std::string_view name, h::Response &res) {
    const WebAsset *asset = webAssetFor(name);
    if (asset == nullptr) {
        return false;
    }
#ifdef LR_WEB_EMBEDDED
    std::string_view body;
    if (name == "index.html") {
        body = embeddedText(kWebIndexHtml, sizeof(kWebIndexHtml));
    } else if (name == "app.css") {
        body = embeddedText(kWebAppCss, sizeof(kWebAppCss));
    } else if (name == "app.js") {
        body = embeddedText(kWebAppJs, sizeof(kWebAppJs));
    } else {
        body = embeddedText(kWebFaviconSvg, sizeof(kWebFaviconSvg));
    }
    res.status = 200;
    res.set_content(std::string{body}, std::string{asset->content_type});
    return true;
#else
    const char *dir = std::getenv("LITEROUTER_WEB_DIR");
    if (dir == nullptr || *dir == '\0') {
        return false;
    }
    std::ifstream in{std::filesystem::path{dir} / asset->name, std::ios::binary};
    if (!in) {
        return false;
    }
    std::string body{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    res.status = 200;
    res.set_content(std::move(body), std::string{asset->content_type});
    return true;
#endif
}

// Paths that belong to the console rather than to the client-facing API. The
// shell is static and holds no data, so it loads before the operator has typed
// a key; every byte it then fetches from the admin API still passes the check.
bool isConsolePath(std::string_view path) {
    return path == "/" || path == "/favicon.ico" || path == "/ui" || startsWith(path, "/ui/");
}

// Liveness and readiness, which answer two different questions and are
// therefore two endpoints.
//
// Liveness ("is this process alive?") answers before any credential is
// presented, because the things that ask it hold no key — a container runtime,
// a load balancer, a systemd unit — and gating it behind `server.api_key` turns
// every such probe into a 401 and reports a healthy proxy as down. It leaks
// nothing: the body is a constant, and it says only that the listener is up.
//
// Readiness ("should traffic be sent here?") is the one that may say no: a
// proxy whose config has no enabled relay can accept a connection and still
// cannot answer a single request, and a load balancer that keeps sending to it
// is sending to a proxy that will 503. `/health` is the historical name for the
// liveness probe and keeps meaning that, so an existing healthcheck does not
// silently change behaviour.
enum class HealthProbe { None, Live, Ready };

HealthProbe healthProbeFor(std::string_view path) {
    if (path == "/health" || path == "/health/live") {
        return HealthProbe::Live;
    }
    if (path == "/health/ready") {
        return HealthProbe::Ready;
    }
    return HealthProbe::None;
}

json validationJson(const ValidationReport &report) {
    json issues = json::array();
    for (const auto &issue : report.issues) {
        issues.push_back({{"level", issue.levelName()},
                          {"path", issue.path},
                          {"message", issue.message}});
    }
    json out = json::object();
    out["ok"] = report.ok();
    out["summary"] = report.summary();
    out["issues"] = std::move(issues);
    return out;
}

// ── session affinity ────────────────────────────────────────────────────────

// How much of a conversation is hashed to identify it. Long enough that two
// different chats do not collide on a shared greeting, short enough that a long
// system prompt is not hashed whole on every request.
constexpr std::size_t kAffinityKeyChars = 4096;

// A conversation's fingerprint: the system prompt plus the first message. Those
// are the parts a chat client keeps identical from turn to turn — and the parts
// a provider's prompt cache keys on — while the tail is exactly what changes.
std::string sessionKey(const json &body) {
    std::string material;
    const auto pull = [&material](const json &node) {
        if (node.is_string()) {
            material += node.get<std::string>();
        } else if (node.is_array()) {
            for (const auto &part : node) {
                if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                    material += part["text"].get<std::string>();
                }
            }
        }
    };
    if (const auto it = body.find("system"); it != body.end()) {
        pull(*it);
    }
    if (const auto it = body.find("messages");
        it != body.end() && it->is_array() && !it->empty()) {
        const auto &first = (*it)[0];
        if (first.is_object() && first.contains("content")) {
            pull(first["content"]);
        }
    }
    material.resize(std::min(material.size(), kAffinityKeyChars));
    if (material.empty()) {
        return {};
    }
    return std::format("{:016x}", std::hash<std::string>{}(material));
}

// ── Prometheus text exposition ──────────────────────────────────────────────

// A label value, escaped as the exposition format requires: a relay id is free
// text, and an unescaped quote in one would corrupt every line after it.
std::string metricLabel(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

// `# TYPE`/`# HELP` then one line per sample. Counters keep their `_total`
// suffix, gauges do not, and the same snapshot the console renders is what a
// scraper reads — one source of truth for both.
std::string metricsText(const Snapshot &snapshot) {
    std::string out;
    const auto header = [&out](std::string_view name, std::string_view type, std::string_view help) {
        out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
    };
    const auto sample = [&out](std::string_view name, const std::string &labels, double value) {
        if (value == std::floor(value) && std::abs(value) < 1e15) {
            out += std::format("{}{} {}\n", name, labels, static_cast<long long>(value));
        } else {
            out += std::format("{}{} {:.3f}\n", name, labels, value);
        }
    };
    const auto counter = [&out](std::string_view name, const std::string &labels,
                                std::uint64_t value) {
        out += std::format("{}{} {}\n", name, labels, value);
    };

    header("literouter_build_info", "gauge", "Build information; the value is always 1.");
    out += std::format("literouter_build_info{{version=\"{}\"}} 1\n", metricLabel(snapshot.version));

    header("literouter_running", "gauge", "1 while the listener is accepting requests.");
    sample("literouter_running", "", snapshot.running ? 1 : 0);
    header("literouter_uptime_seconds", "gauge", "Seconds since this listener started.");
    sample("literouter_uptime_seconds", "", snapshot.uptime_sec);
    header("literouter_active_requests", "gauge", "Requests currently in flight.");
    sample("literouter_active_requests", "", static_cast<double>(snapshot.active_requests));
    header("literouter_breakers_open", "gauge", "Relays whose breaker is open right now.");
    sample("literouter_breakers_open", "", snapshot.breakers_open);

    header("literouter_requests_total", "counter", "Requests finished, by outcome.");
    counter("literouter_requests_total", "", snapshot.total_requests);
    counter("literouter_successes_total", "", snapshot.total_success);
    counter("literouter_failures_total", "", snapshot.total_failure);
    header("literouter_log_entries_total", "counter",
           "Request log entries written since this instance started.");
    counter("literouter_log_entries_total", "", snapshot.log_seq);
    header("literouter_bytes_out_total", "counter", "Response bytes relayed to clients.");
    counter("literouter_bytes_out_total", "", snapshot.bytes_out);
    header("literouter_tokens_total", "counter", "Tokens reported by relays.");
    counter("literouter_tokens_prompt_total", "", snapshot.tokens_prompt);
    counter("literouter_tokens_completion_total", "", snapshot.tokens_completion);
    header("literouter_cost_usd_total", "counter",
           "Estimated spend, from the tokens relays reported and the prices configured for "
           "them. Relays without a price contribute nothing, so this is a floor.");
    sample("literouter_cost_usd_total", "", snapshot.cost_usd);
    header("literouter_latency_ms_avg", "gauge",
           "Exponentially weighted average request latency, in milliseconds.");
    sample("literouter_latency_ms_avg", "", snapshot.latency_ms_avg);

    header("literouter_relay_requests_total", "counter", "Attempts sent to a relay.");
    header("literouter_relay_successes_total", "counter", "Attempts a relay answered usefully.");
    header("literouter_relay_failures_total", "counter", "Attempts a relay failed.");
    header("literouter_relay_aborted_total", "counter",
           "Attempts the client abandoned, which count against nobody.");
    header("literouter_relay_retries_in_total", "counter",
           "Attempts a relay absorbed after another one failed.");
    header("literouter_relay_bytes_in_total", "counter", "Request bytes sent to a relay.");
    header("literouter_relay_bytes_out_total", "counter", "Response bytes received from a relay.");
    header("literouter_relay_tokens_prompt_total", "counter", "Prompt tokens a relay reported.");
    header("literouter_relay_tokens_completion_total", "counter",
           "Completion tokens a relay reported.");
    header("literouter_relay_latency_ms_last", "gauge", "Latency of a relay's last attempt.");
    header("literouter_relay_latency_ms_avg", "gauge", "A relay's average latency.");
    header("literouter_relay_latency_ms_p95", "gauge",
           "A relay's 95th percentile latency over its recent attempts.");
    header("literouter_relay_last_used_unixtime", "gauge", "When a relay was last tried.");
    header("literouter_relay_healthy", "gauge",
           "1 when a relay is usable, 0 while its breaker is open or it is disabled.");
    header("literouter_relay_cooldown_seconds", "gauge",
           "Seconds left in a relay's breaker window.");
    header("literouter_relay_cost_usd_total", "counter",
           "Estimated spend on this relay, from the tokens it reported and its configured "
           "prices.");
    for (const auto &stat : snapshot.providers) {
        const std::string labels = std::format("{{relay=\"{}\"}}", metricLabel(stat.provider));
        counter("literouter_relay_requests_total", labels, stat.requests);
        counter("literouter_relay_successes_total", labels, stat.successes);
        counter("literouter_relay_failures_total", labels, stat.failures);
        counter("literouter_relay_aborted_total", labels, stat.aborted);
        counter("literouter_relay_retries_in_total", labels, stat.retries_in);
        counter("literouter_relay_bytes_in_total", labels, stat.bytes_in);
        counter("literouter_relay_bytes_out_total", labels, stat.bytes_out);
        counter("literouter_relay_tokens_prompt_total", labels, stat.tokens_prompt);
        counter("literouter_relay_tokens_completion_total", labels, stat.tokens_completion);
        sample("literouter_relay_latency_ms_last", labels, stat.latency_ms_last);
        sample("literouter_relay_latency_ms_avg", labels, stat.latency_ms_avg);
        sample("literouter_relay_latency_ms_p95", labels, stat.latency_ms_p95);
        sample("literouter_relay_last_used_unixtime", labels, stat.last_used_unix);
        sample("literouter_relay_cost_usd_total", labels, stat.cost_usd);
    }
    for (const auto &health : snapshot.health) {
        const std::string labels = std::format("{{relay=\"{}\"}}", metricLabel(health.provider));
        sample("literouter_relay_healthy", labels,
               health.state == ProviderHealth::State::Healthy ? 1 : 0);
        sample("literouter_relay_cooldown_seconds", labels, health.cooldown_remaining);
    }
    header("literouter_cache_enabled", "gauge",
           "1 while the local response cache is storing answers.");
    sample("literouter_cache_enabled", "", snapshot.cache_enabled ? 1 : 0);
    header("literouter_cache_hits_total", "counter",
           "Requests answered from the local response cache without asking a relay.");
    counter("literouter_cache_hits_total", "", snapshot.cache_hits);
    header("literouter_cache_misses_total", "counter",
           "Cache lookups that found nothing usable, including expired entries.");
    counter("literouter_cache_misses_total", "", snapshot.cache_misses);
    header("literouter_cache_entries", "gauge", "Answers currently held in the cache.");
    sample("literouter_cache_entries", "", static_cast<double>(snapshot.cache_entries));
    return out;
}

constexpr std::size_t kMaxRequestBody = 64ull * 1024 * 1024;

#include "lr_media.h"
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
    res.set_content(dumpJson(errorBody(std::move(message), std::move(type), std::move(code))),
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
    // When the relay's first response byte arrived, which is where its own
    // thinking time ends and the streaming of the answer begins.
    double headers_unix = 0.0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
    // The transport's own code, not just its text: the retry below is only for
    // the one error that means nothing was sent, and a string cannot say that.
    h::Error error_code = h::Error::Success;

    std::shared_ptr<h::Client> client;
    std::atomic<std::uint64_t> bytes_out{0};
};

// A relay's recent latencies, for the p95 the console shows. Kept here rather
// than in ProviderStat because it is a window over the last attempts, not a
// lifetime total — and it has no business being written to the state file.
struct LatencyWindow {
    static constexpr std::size_t kDepth = 64;
    std::array<double, kDepth> samples{};
    std::size_t count = 0;
    std::size_t next = 0;

    void add(double ms) {
        samples[next] = ms;
        next = (next + 1) % kDepth;
        if (count < kDepth) {
            ++count;
        }
    }

    double p95() const {
        if (count == 0) {
            return 0.0;
        }
        std::array<double, kDepth> sorted{};
        std::copy_n(samples.begin(), count, sorted.begin());
        // Nearest-rank: the smallest sample at or above 95% of the window, so
        // the value is always one the relay actually served.
        const std::size_t rank =
            static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(count))) - 1;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(rank),
                         sorted.begin() + static_cast<long>(count));
        return sorted[rank];
    }
};

// The window an upstream asked for in `Retry-After`, in seconds.
//
// Only the delta-seconds form is read: it is what the APIs and relays this
// speaks to send, and leaving the date form unparsed costs nothing worse than
// the configured cooldown. The value is capped so a relay having a bad day
// cannot park itself for a week. Templated because the buffered path carries its
// headers as a map and the streamed bridge as a vector.
template <typename Headers>
double retryAfterSeconds(const Headers &headers) {
    constexpr double kMaxRetryAfter = 86400.0;
    const std::string raw = trim(headerValue(headers, "retry-after"));
    if (raw.empty()) {
        return 0.0;
    }
    long long seconds = 0;
    const auto *begin = raw.data();
    const auto *end = begin + raw.size();
    const auto parsed = std::from_chars(begin, end, seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != end || seconds <= 0) {
        return 0.0;
    }
    return std::min<double>(static_cast<double>(seconds), kMaxRetryAfter);
}

// This process's id, for the pid file. There is no portable spelling of it, and
// both headers arrive with httplib.h, which this unit already includes.
std::int64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

// ── single instance ─────────────────────────────────────────────────────────

// A literouter already answering on this address, if there is one.
//
// Identify the other end before binding so the error can name its pid and
// explain how to stop it. Older listeners used httplib's SO_REUSEPORT default;
// current listeners disable port sharing so an unverifiable HTTPS health probe
// cannot accidentally let two processes split traffic. A failed probe defers
// to the OS bind check rather than implying that the port is free.
std::string existingInstance(const ServerConfig &server,
                             const std::filesystem::path &pid_path) {
    const std::string &host = server.host;
    const int port = server.port;
    // 0.0.0.0 and :: are bind addresses, not destinations.
    const bool wildcard = host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]";
    ServerConfig target = server;
    if (wildcard) target.host = (host == "::" || host == "[::]") ? "::1" : "127.0.0.1";
    h::Client client{serverBaseUrl(target)};
    if (const auto bundle = resolveCaBundle(); !bundle.empty()) {
        // `path.string()`, not pathToUtf8(): this value is handed to OpenSSL,
        // which opens the file through the C library's narrow-path call — the
        // ANSI code page on Windows. UTF-8 bytes would be the wrong encoding.
        client.set_ca_cert_path(bundle.string());
    }
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(0, 400000);
    client.set_read_timeout(0, 400000);
    const auto res = client.Get("/health");
    if (!res || res->status != 200) {
        return {};
    }
    const std::string &body = res->body;
    if (body.find("\"status\"") == std::string::npos || body.find("ok") == std::string::npos) {
        return {};
    }

    // The pid file is a courtesy, not the detector: it is what turns "something
    // is there" into "that process is there".
    std::string holder;
    if (std::ifstream input{pid_path, std::ios::binary}; input) {
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        const json doc = json::parse(text, nullptr, false);
        if (!doc.is_discarded() && doc.is_object() && doc.value("pid", 0) > 0) {
            holder = std::format(" (pid {})", doc.value("pid", 0));
        }
    }
    return std::format("{}:{} already answers as a literouter listener{}; refusing to share the "
                       "port — stop that instance, or serve this one on another port",
                       host, port, holder);
}

// The listening process's own record: who is on this port, since when, and with
// which config. Written after the bind (the bound port is what identifies the
// instance) and removed by stop().
bool writePidFile(const std::filesystem::path &path, int port, const std::string &config_path) {
    // Called from start(), above which there is no handler: an exception here
    // would abort a proxy that has already bound its port and is about to
    // serve. `dumpJson` cannot throw on the bytes any more, but this stays
    // because the file is a courtesy — the same reason the failed-open branch
    // below returns false rather than failing the start.
    try {
        std::error_code ec;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        json doc = json::object();
        doc["pid"] = static_cast<std::int64_t>(currentProcessId());
        doc["port"] = port;
        doc["started_unix"] = nowUnix();
        doc["config"] = config_path;
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            return false; // a courtesy file is never a reason to refuse to listen
        }
        const std::string text = dumpJson(doc, 2) + "\n";
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        return static_cast<bool>(output);
    } catch (const std::exception &) {
        return false;
    } catch (...) {
        return false;
    }
}

// ── log ring ─────────────────────────────────────────────────────────────────

// The one place the persisted telemetry document is shaped, and the reason the
// two readers below exist at all: `fetchStatus` / `fetchLogs` parse the same
// objects off the wire that the state file writes to disk.
ProviderStat providerStatFromJson(const json &item) {
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
    return stat;
}

LogEntry logEntryFromJson(const json &item) {
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
    entry.wait_ms = item.value("wait_ms", 0.0);
    entry.ttfb_ms = item.value("ttfb_ms", 0.0);
    entry.stream_ms = item.value("stream_ms", 0.0);
    entry.bytes = item.value("bytes", std::uint64_t{0});
    entry.message = item.value("message", std::string{});
    entry.request_body = item.value("request_body", std::string{});
    entry.response_body = item.value("response_body", std::string{});
    return entry;
}

// How often a dirty telemetry document reaches the disk, and how much of the
// ring goes with it. A ring can be configured to 100k entries; rewriting that
// on a timer would be a write amplifier, and the file only has to be deep
// enough for a restarted console to open on the recent past.
constexpr auto kStateFlushInterval = std::chrono::seconds{3};
constexpr std::size_t kPersistedLogEntries = 500;

struct LogRing {
    std::deque<LogEntry> entries;
    // Overwritten by setCapacity() from server.log_capacity before anything can
    // be pushed; the initial value tracks the same default so the two cannot
    // disagree about what an unconfigured core does.
    std::size_t capacity = static_cast<std::size_t>(ServerConfig{}.log_capacity);
    std::uint64_t next_seq = 1;

    void setCapacity(std::size_t value) {
        capacity = std::max<std::size_t>(16, value);
        while (entries.size() > capacity) {
            entries.pop_front();
        }
    }

    // Used only for the ring read back from the state file: the sequence keeps
    // counting from where the previous process stopped, so a console that has
    // cached `log_seq` does not see it jump backwards.
    void restore(std::deque<LogEntry> restored, std::uint64_t next) {
        entries = std::move(restored);
        for (const auto &entry : entries) {
            next = std::max(next, entry.seq + 1);
        }
        next_seq = std::max<std::uint64_t>(next, 1);
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
    bool attempted_upstream = false;
    std::string id;
    std::string kind;
    std::string model;
    std::string body;
    // Which conversation this request belongs to, for prompt-cache affinity.
    // Empty when the feature is off or the body has nothing stable to key on.
    std::string affinity_key;
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
    // Two more gates in front of a relay, for two questions the breaker cannot
    // answer: is this relay full, and has this exact question already been
    // answered.
    UpstreamLimiter upstream_limiter;
    ResponseCache response_cache;

    mutable std::mutex config_mutex;
    AppConfig config;
    // Actual transport is separate from the next-start settings in config.
    ServerConfig bound_server;
    std::string config_path;

    std::thread runner;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex shutdown_mutex;
    std::thread shutdown_thread;
    bool shutdown_scheduled = false;
    // Signed, and always released through releaseInFlight(): a counter that can
    // only be decremented is one bug away from wrapping to 4 billion.
    std::atomic<int> active_requests{0};
    std::atomic<int> bound_port{0};

    double started_unix = 0.0;

    // ── prompt-cache affinity ────────────────────────────────────────────────
    //
    // Which relay last answered a given conversation, and until when. Bounded on
    // both axes: entries expire, and the table is capped so a busy proxy with
    // many conversations cannot grow it without limit.
    struct Affinity {
        std::string provider;
        double expires_unix = 0.0;
    };
    static constexpr std::size_t kMaxAffinityEntries = 256;
    mutable std::mutex affinity_mutex;
    std::map<std::string, Affinity, std::less<>> affinity;

    std::string affinityProvider(const std::string &key, double now_unix) {
        if (key.empty()) {
            return {};
        }
        std::scoped_lock lock{affinity_mutex};
        const auto it = affinity.find(key);
        if (it == affinity.end()) {
            return {};
        }
        if (it->second.expires_unix <= now_unix) {
            affinity.erase(it);
            return {};
        }
        return it->second.provider;
    }

    void rememberAffinity(const std::string &key, const std::string &provider, double ttl_sec,
                          double now_unix) {
        if (key.empty() || provider.empty() || ttl_sec <= 0.0) {
            return;
        }
        std::scoped_lock lock{affinity_mutex};
        if (affinity.size() >= kMaxAffinityEntries && !affinity.contains(key)) {
            // Drop what has already expired first; only then the oldest entry, so
            // a steady stream of new conversations cannot evict warm ones while
            // stale ones survive.
            std::erase_if(affinity, [now_unix](const auto &entry) {
                return entry.second.expires_unix <= now_unix;
            });
            if (affinity.size() >= kMaxAffinityEntries) {
                auto oldest = affinity.begin();
                for (auto it = affinity.begin(); it != affinity.end(); ++it) {
                    if (it->second.expires_unix < oldest->second.expires_unix) {
                        oldest = it;
                    }
                }
                affinity.erase(oldest);
            }
        }
        affinity[std::string{key}] = Affinity{provider, now_unix + ttl_sec};
    }

    // The last buckets of traffic, oldest first. Bounded by construction: the
    // oldest falls off when a new bucket starts. Both the width and the count
    // come from server.traffic_bucket_sec / server.traffic_bucket_count, so a
    // relay being debugged right now can be watched at minute resolution
    // instead of hour resolution.
    std::size_t traffic_bucket_count = 24;
    int traffic_bucket_sec = 3600;
    std::deque<TrafficBucket> hourly;

    mutable std::mutex telemetry_mutex;
    std::map<std::string, ProviderStat, std::less<>> stats;
    std::map<std::string, LatencyWindow, std::less<>> latency_windows;
    LogRing log;
    std::uint64_t total_requests = 0;
    std::uint64_t total_success = 0;
    std::uint64_t total_failure = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
    double cost_usd = 0.0;
    double latency_ms_avg = 0.0;

    // ── persisted telemetry ──────────────────────────────────────────────────
    //
    // Counters, per-relay stats and the request ring are all in memory, so
    // without this every restart reset `status` to zeros and emptied the log.
    // They are written to `<state dir>/telemetry-<port>.json` on a short timer and on
    // stop(), and read back once per process at the first start().
    //
    // The path is fixed when the server starts and the switch is what moves at
    // runtime, so a request thread reading it (markStateDirty is on the hot
    // path) never races with a live config edit.
    std::filesystem::path state_path;
    // Set only once this process has written its own pid file, so stop() can
    // never remove the record of an instance that refused to start next to.
    std::filesystem::path pid_path;
    std::atomic<bool> persist_enabled{false};
    bool state_restored = false;
    bool state_write_failed = false;
    std::atomic<bool> state_dirty{false};
    // A plain thread with a stop flag rather than std::jthread: this toolchain's
    // libc++ exports stop_token's out-of-line helpers under an ABI tag the
    // linker does not match, so <stop_token> does not link here.
    std::atomic<bool> flush_stop{false};
    std::mutex flush_wait_mutex;
    std::condition_variable flush_wait;
    std::thread flusher;

    // ── OTLP metrics export ──────────────────────────────────────────────────
    //
    // The Prometheus endpoint is pull-based, which is the wrong shape for a
    // machine behind a home router or inside a container nobody scrapes. This
    // pushes the same numbers to a collector the operator already runs. The
    // endpoint is read under its own lock and re-read every tick, so editing it
    // in the console takes effect without a restart.
    mutable std::mutex otlp_mutex;
    std::string otlp_endpoint;
    double last_otlp_export_unix = 0.0;
    bool otlp_write_failed = false;

    AppConfig snapshotConfig() const {
        std::scoped_lock lock{config_mutex};
        return config;
    }

    std::string configPath() const {
        std::scoped_lock lock{config_mutex};
        return config_path;
    }

    void scheduleShutdown(ProxyServer *owner) {
        std::scoped_lock lock{shutdown_mutex};
        if (shutdown_scheduled) {
            return;
        }
        shutdown_scheduled = true;
        shutdown_thread = std::thread([owner] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            owner->stop();
        });
    }

    void joinShutdownThread() {
        std::thread thread;
        {
            std::scoped_lock lock{shutdown_mutex};
            if (!shutdown_thread.joinable() ||
                shutdown_thread.get_id() == std::this_thread::get_id()) {
                return;
            }
            thread = std::move(shutdown_thread);
        }
        thread.join();
        std::scoped_lock lock{shutdown_mutex};
        shutdown_scheduled = false;
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
        markStateDirty();
    }

    void releaseInFlight() {
        int current = active_requests.load(std::memory_order_relaxed);
        while (current > 0 && !active_requests.compare_exchange_weak(
                                  current, current - 1, std::memory_order_relaxed)) {
        }
    }

    // The one place a request is accounted for. Called exactly once per
    // request, on every path, including the ones that never reached a relay.
    // What one finished request owes the log. A struct rather than eight more
    // positional arguments: `status, bytes, message, failover, attempt...` at a
    // call site said nothing about which was which, and adding the upstream
    // model and the response body made that worse rather than better.
    // The answer as the log would keep it: only when the operator asked for
    // bodies, and only up to the limit they set. The request body is capped the
    // same way inside finish().
    // Redacted before it is truncated, never after: a truncated key is still a
    // prefix of a key, while a masked one is not a key at all.
    std::string logged_body(const RequestContext &ctx, std::string_view body) const {
        if (!ctx.config.server.log_bodies || ctx.kind == "audio" || ctx.kind == "images") {
            return {};
        }
        return truncateUtf8(redactSecrets(body),
                            static_cast<std::size_t>(ctx.config.server.log_body_limit));
    }

    struct AttemptFacts {
        std::string provider;
        // What the relay was actually asked for, which differs from what the
        // client asked for whenever a route renames the model. Empty when no
        // relay was reached.
        std::string upstream_model;
        int status = 0;
        std::uint64_t bytes = 0;
        // The relay's own token report for this request, and what it cost at the
        // prices written down for it. Kept here as well as in recordAttempt()
        // because the hourly bucket needs them and it is accumulated per
        // *request*, while recordAttempt() sees every attempt.
        std::uint64_t prompt_tokens = 0;
        std::uint64_t completion_tokens = 0;
        double cost_usd = 0.0;
        bool usage_reported = false;
        std::string message;
        // Where the time went — see LogEntry for what each of the three means.
        double wait_ms = 0.0;
        double ttfb_ms = 0.0;
        double stream_ms = 0.0;
        // The answer as the client received it. Only kept when log_bodies is on
        // (the caller passes what it already capped).
        std::string response_body;
        bool failover = false;
        int attempt = 1;
        int attempts_total = 1;
    };

    void finish(const RequestContext &ctx, AttemptFacts facts) {
        if (ctx.reported->exchange(true)) {
            return;
        }
        LogEntry entry;
        entry.level = facts.status >= 200 && facts.status < 300 ? "info" : "error";
        entry.request_id = ctx.id;
        entry.kind = ctx.kind;
        entry.model = ctx.model;
        entry.provider = std::move(facts.provider);
        entry.upstream_model = std::move(facts.upstream_model);
        entry.status = facts.status;
        entry.stream = ctx.stream;
        entry.failover = facts.failover;
        entry.attempt = facts.attempt;
        entry.attempts_total = facts.attempts_total;
        entry.latency_ms = (nowUnix() - ctx.started) * 1000.0;
        entry.wait_ms = facts.wait_ms;
        entry.ttfb_ms = facts.ttfb_ms;
        entry.stream_ms = facts.stream_ms;
        entry.bytes = facts.bytes;
        entry.message = std::move(facts.message);
        entry.response_body = std::move(facts.response_body);
        if (ctx.config.server.log_bodies) {
            const auto limit = static_cast<std::size_t>(ctx.config.server.log_body_limit);
            entry.request_body = truncateUtf8(redactSecrets(ctx.body), limit);
        }

        std::scoped_lock lock{telemetry_mutex};
        ++total_requests;
        if (facts.status >= 200 && facts.status < 300) {
            ++total_success;
        } else if (facts.status > 0) {
            ++total_failure;
        }
        bytes_out += facts.bytes;
        noteHour(facts, facts.cost_usd, nowUnix());
        if (entry.latency_ms > 0.0) {
            latency_ms_avg = latency_ms_avg == 0.0
                                 ? entry.latency_ms
                                 : latency_ms_avg * 0.85 + entry.latency_ms * 0.15;
        }
        log.push(std::move(entry));
        markStateDirty();
        releaseInFlight();
    }

    // What a latency-aware policy orders by, read under the lock: building a
    // whole Snapshot to get two numbers per candidate would be absurd.
    double latencyMetric(std::string_view provider) const {
        std::scoped_lock lock{telemetry_mutex};
        const auto it = stats.find(provider);
        if (it == stats.end()) {
            return 0.0;
        }
        return it->second.latency_ms_p95 > 0.0 ? it->second.latency_ms_p95
                                               : it->second.latency_ms_avg;
    }

    // Called under the telemetry lock from finish(), which is the one place a
    // request — as opposed to an attempt — is accounted for, so a failover adds
    // one request here and two to the relay stats, exactly as the totals do.
    void noteHour(const AttemptFacts &facts, double cost_usd, double time_unix) {
        const double width = static_cast<double>(std::max(60, traffic_bucket_sec));
        const double hour = std::floor(time_unix / width) * width;
        if (hourly.empty() || hourly.back().hour_unix < hour) {
            hourly.push_back(TrafficBucket{});
            hourly.back().hour_unix = hour;
            hourly.back().bucket_sec = std::max(60, traffic_bucket_sec);
            while (hourly.size() > traffic_bucket_count) {
                hourly.pop_front();
            }
        }
        TrafficBucket &bucket = hourly.back();
        ++bucket.requests;
        if (facts.status >= 200 && facts.status < 300) {
            ++bucket.successes;
        } else if (facts.status > 0) {
            ++bucket.failures;
        }
        bucket.bytes_out += facts.bytes;
        bucket.tokens_prompt += facts.prompt_tokens;
        bucket.tokens_completion += facts.completion_tokens;
        bucket.cost_usd += cost_usd;
    }


    void recordSystem(std::string message, std::string level = "info") {
        LogEntry entry;
        entry.level = std::move(level);
        entry.kind = "system";
        entry.message = std::move(message);
        appendLog(std::move(entry));
    }

    // ── telemetry file ───────────────────────────────────────────────────────

    void setPersistence(bool enabled) {
        persist_enabled.store(enabled, std::memory_order_relaxed);
    }

    void markStateDirty() {
        if (persist_enabled.load(std::memory_order_relaxed)) {
            state_dirty.store(true, std::memory_order_relaxed);
        }
    }

    // Assembled under the telemetry lock, written outside it: a request must not
    // wait on a disk write to have its counters counted.
    std::string stateJson() const {
        json root = json::object();
        root["version"] = 1;
        root["saved_unix"] = nowUnix();

        const auto attach = [](json &array, const std::string &text) {
            json node = json::parse(text, nullptr, false);
            if (!node.is_discarded()) {
                array.push_back(std::move(node));
            }
        };

        std::scoped_lock lock{telemetry_mutex};
        json counters = json::object();
        counters["total_requests"] = total_requests;
        counters["total_success"] = total_success;
        counters["total_failure"] = total_failure;
        counters["bytes_out"] = bytes_out;
        counters["tokens_prompt"] = tokens_prompt;
        counters["tokens_completion"] = tokens_completion;
        counters["latency_ms_avg"] = latency_ms_avg;
        root["counters"] = std::move(counters);

        json providers = json::array();
        for (auto it = stats.begin(); it != stats.end(); ++it) {
            attach(providers, toJsonString(it->second));
        }
        root["providers"] = std::move(providers);

        json entries = json::array();
        const std::size_t skip = log.entries.size() > kPersistedLogEntries
                                     ? log.entries.size() - kPersistedLogEntries
                                     : 0;
        for (std::size_t i = skip; i < log.entries.size(); ++i) {
            attach(entries, toJsonString(log.entries[i]));
        }
        json ring = json::object();
        ring["next_seq"] = log.next_seq;
        ring["entries"] = std::move(entries);
        root["log"] = std::move(ring);

        // The hourly trend goes to disk with the rest, so a restart does not
        // erase the shape of the day.
        json hours = json::array();
        for (const auto &bucket : hourly) {
            json node = json::object();
            node["hour_unix"] = bucket.hour_unix;
            node["bucket_sec"] = bucket.bucket_sec;
            node["requests"] = bucket.requests;
            node["successes"] = bucket.successes;
            node["failures"] = bucket.failures;
            node["bytes_out"] = bucket.bytes_out;
            node["tokens_prompt"] = bucket.tokens_prompt;
            node["tokens_completion"] = bucket.tokens_completion;
            node["cost_usd"] = bucket.cost_usd;
            hours.push_back(std::move(node));
        }
        root["hourly"] = std::move(hours);

        return dumpJson(root, 2);
    }

    // Reported once per failure run rather than once per attempt: a full disk
    // would otherwise fill the very log it is failing to write.
    void reportStateProblem(std::string message) {
        if (state_write_failed) {
            return;
        }
        state_write_failed = true;
        recordSystem(std::move(message), "error");
    }

    void writeState() {
        if (!persist_enabled.load(std::memory_order_relaxed) || state_path.empty()) {
            return;
        }
        const std::string text = stateJson();
        std::error_code ec;
        std::filesystem::create_directories(state_path.parent_path(), ec);

        // Same temp-then-rename dance as the config file: a reader sees either
        // the previous document or the new one, never a half-written one.
        auto temp = state_path;
        temp += std::format(".tmp-{}", hexId(4));
        {
            std::ofstream output{temp, std::ios::binary | std::ios::trunc};
            if (!output) {
                reportStateProblem(
                    std::format("cannot write telemetry file {}", pathToUtf8(temp)));
                return;
            }
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output) {
                output.close();
                std::filesystem::remove(temp, ec);
                reportStateProblem(
                    std::format("write to telemetry file {} failed", pathToUtf8(temp)));
                return;
            }
        }
        std::filesystem::rename(temp, state_path, ec);
        if (ec) {
#ifdef _WIN32
            // Windows can refuse the replace above while the destination is
            // open elsewhere; remove-then-rename is the documented fallback.
            ec.clear();
            std::filesystem::remove(state_path, ec);
            ec.clear();
            std::filesystem::rename(temp, state_path, ec);
#endif
            if (ec) {
                std::filesystem::remove(temp, ec);
                reportStateProblem(std::format("cannot replace telemetry file {}: {}",
                                               pathToUtf8(state_path), ec.message()));
                return;
            }
        }
#ifndef _WIN32
        // The log can carry prompts when log_bodies is on, so this file is not
        // for other accounts to read.
        std::filesystem::permissions(state_path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
#endif
        state_write_failed = false;
    }

    // Never fatal: a state file that cannot be read is a run that starts with
    // empty counters, not a server that refuses to start.
    void loadState() {
        if (!persist_enabled.load(std::memory_order_relaxed) || state_path.empty()) {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(state_path, ec)) {
            return;
        }
        std::ifstream input{state_path, std::ios::binary};
        if (!input) {
            recordSystem(std::format("cannot read telemetry file {}", pathToUtf8(state_path)),
                         "error");
            return;
        }
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || root.value("version", 0) != 1) {
            recordSystem(std::format("ignoring {}: not a readable literouter telemetry file",
                                     pathToUtf8(state_path)),
                         "warning");
            return;
        }

        // Read into locals first. nlohmann's accessors throw when a hand-edited
        // file holds the wrong type for a key, and half-applied telemetry is
        // worse than none — so the members are only touched once the whole
        // document has been read successfully.
        std::uint64_t requests = 0;
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
        std::uint64_t bytes = 0;
        std::uint64_t prompt_tokens = 0;
        std::uint64_t completion_tokens = 0;
        double avg_latency = 0.0;
        std::map<std::string, ProviderStat, std::less<>> restored_stats;
        std::deque<LogEntry> entries;
        std::uint64_t next_seq = 1;
        try {
            if (const auto it = root.find("counters"); it != root.end() && it->is_object()) {
                requests = it->value("total_requests", std::uint64_t{0});
                successes = it->value("total_success", std::uint64_t{0});
                failures = it->value("total_failure", std::uint64_t{0});
                bytes = it->value("bytes_out", std::uint64_t{0});
                prompt_tokens = it->value("tokens_prompt", std::uint64_t{0});
                completion_tokens = it->value("tokens_completion", std::uint64_t{0});
                avg_latency = it->value("latency_ms_avg", 0.0);
            }
            if (const auto it = root.find("providers"); it != root.end() && it->is_array()) {
                for (const auto &item : *it) {
                    if (item.is_object()) {
                        ProviderStat stat = providerStatFromJson(item);
                        restored_stats[stat.provider] = std::move(stat);
                    }
                }
            }
            if (const auto it = root.find("log"); it != root.end() && it->is_object()) {
                if (const auto list = it->find("entries"); list != it->end() && list->is_array()) {
                    for (const auto &item : *list) {
                        if (item.is_object()) {
                            entries.push_back(logEntryFromJson(item));
                        }
                    }
                }
                next_seq = it->value("next_seq", std::uint64_t{1});
            }
        } catch (const std::exception &error) {
            recordSystem(std::format("ignoring {}: {}", pathToUtf8(state_path), error.what()),
                         "warning");
            return;
        }

        std::deque<TrafficBucket> restored_hours;
        if (const auto it = root.find("hourly"); it != root.end() && it->is_array()) {
            for (const auto &item : *it) {
                if (!item.is_object()) {
                    continue;
                }
                TrafficBucket bucket;
                bucket.hour_unix = item.value("hour_unix", 0.0);
                // A file written before the bucket width was configurable has
                // no `bucket_sec`, and it was recorded hourly.
                bucket.bucket_sec = item.value("bucket_sec", 3600);
                bucket.requests = item.value("requests", std::uint64_t{0});
                bucket.successes = item.value("successes", std::uint64_t{0});
                bucket.failures = item.value("failures", std::uint64_t{0});
                bucket.bytes_out = item.value("bytes_out", std::uint64_t{0});
                bucket.tokens_prompt = item.value("tokens_prompt", std::uint64_t{0});
                bucket.tokens_completion = item.value("tokens_completion", std::uint64_t{0});
                bucket.cost_usd = item.value("cost_usd", 0.0);
                restored_hours.push_back(bucket);
            }
        }

        const std::size_t restored = entries.size();
        {
            std::scoped_lock lock{telemetry_mutex};
            total_requests = requests;
            total_success = successes;
            total_failure = failures;
            bytes_out = bytes;
            tokens_prompt = prompt_tokens;
            tokens_completion = completion_tokens;
            latency_ms_avg = avg_latency;
            for (auto it = restored_stats.begin(); it != restored_stats.end(); ++it) {
                stats[it->first] = std::move(it->second);
            }
            log.restore(std::move(entries), next_seq);
            while (restored_hours.size() > traffic_bucket_count) {
                restored_hours.pop_front();
            }
            hourly = std::move(restored_hours);
        }
        recordSystem(std::format("restored {} log entries from {}", restored,
                                 pathToUtf8(state_path)));
    }

    // Swaps the routing model in one place, because two callers need it: the
    // console's edit and the file watcher below. In-flight requests keep the
    // config they began with; the next request sees this one.
    void applyConfig(const AppConfig &config) {
        {
            std::scoped_lock lock{config_mutex};
            this->config = config;
        }
        router.setConfig(config);
        upstream_limiter.setConfig(config);
        response_cache.configure(config.server.response_cache_ttl_sec,
                                config.server.response_cache_max_entries);
        traffic_bucket_sec = std::clamp(config.server.traffic_bucket_sec, 60, 86400);
        traffic_bucket_count =
            static_cast<std::size_t>(std::clamp(config.server.traffic_bucket_count, 2, 10000));
        {
            std::scoped_lock lock{telemetry_mutex};
            log.setCapacity(static_cast<std::size_t>(std::max(16, config.server.log_capacity)));
            // A narrower window than the trend currently holds trims it now
            // rather than at the next bucket boundary, so the chart reflects
            // the setting the operator just saved.
            while (hourly.size() > traffic_bucket_count) {
                hourly.pop_front();
            }
        }
        {
            std::scoped_lock lock{otlp_mutex};
            otlp_endpoint = config.server.otlp_endpoint;
        }
        // A live edit decides whether the next flush writes anything, and marking
        // it dirty is what makes switching the flag on take effect now rather than
        // at the next request.
        setPersistence(config.server.persist_telemetry);
        markStateDirty();
    }

    // ── config file watching ─────────────────────────────────────────────────
    //
    // Piggybacked on the flush tick rather than given a thread of its own: that
    // thread already exists, and three seconds is a fine resolution for a human
    // editing a file. Off unless server.reload_on_change is set.
    std::filesystem::file_time_type config_stamp{};
    std::uintmax_t config_size = 0;
    bool config_stamp_valid = false;

    void rememberConfigStamp() {
        const std::string path = configPath();
        if (path.empty()) {
            config_stamp_valid = false;
            return;
        }
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) {
            config_stamp_valid = false;
            return;
        }
        config_stamp = stamp;
        config_size = std::filesystem::file_size(path, ec);
        config_stamp_valid = !ec;
    }

    void checkConfigFile() {
        const std::string path = configPath();
        if (path.empty() || !snapshotConfig().server.reload_on_change) {
            return;
        }
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return; // the file went away; the server keeps the config it has
        }
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) {
            return;
        }
        if (config_stamp_valid && stamp == config_stamp && size == config_size) {
            return;
        }
        rememberConfigStamp();

        auto loaded = ConfigStore::load(path);
        if (!loaded) {
            recordSystem(std::format("config changed on disk but did not load: {}", loaded.error()),
                         "error");
            return;
        }
        // The console's own save writes the file too; reloading what is already
        // running would only log a line that says nothing happened.
        if (toJsonString(loaded->config()) == toJsonString(snapshotConfig())) {
            return;
        }
        applyConfig(loaded->config());
        recordSystem(std::format("config reloaded from {} (file changed)", path));
    }

    void startFlusher() {
        if (flusher.joinable()) {
            return;
        }
        flush_stop.store(false, std::memory_order_relaxed);
        flusher = std::thread([this] {
            std::unique_lock lock{flush_wait_mutex};
            while (true) {
                // The predicate version, so a stop request wakes this instead of
                // holding up shutdown for the rest of the interval.
                flush_wait.wait_for(lock, kStateFlushInterval,
                                    [this] { return flush_stop.load(std::memory_order_relaxed); });
                if (flush_stop.load(std::memory_order_relaxed)) {
                    return;
                }
                // This thread owns no caller, so an escaping exception is
                // std::terminate and takes the proxy with it. Telemetry is
                // bookkeeping: losing one tick is always better than dropping
                // every in-flight request.
                try {
                    if (state_dirty.exchange(false, std::memory_order_relaxed)) {
                        writeState();
                    }
                    checkConfigFile();
                    exportOtlp();
                } catch (const std::exception &e) {
                    recordSystem(std::format("telemetry flush failed: {}", e.what()), "error");
                } catch (...) {
                    recordSystem("telemetry flush failed: unknown error", "error");
                }
            }
        });
    }

    // ── OTLP ─────────────────────────────────────────────────────────────────

    static constexpr double kOtlpIntervalSec = 30.0;

    // One OTLP sum or gauge metric, with optional attributes. `monotonic` is
    // what tells a collector that a counter never decreases, which is the
    // difference between a rate() and a garbage chart.
    static json otlpMetric(std::string name, std::string unit, double value,
                           std::string_view start_nano, std::string_view now_nano,
                           bool gauge, const json &attributes) {
        json point = json::object();
        point["asDouble"] = value;
        point["timeUnixNano"] = std::string{now_nano};
        point["startTimeUnixNano"] = std::string{start_nano};
        if (!attributes.empty()) {
            point["attributes"] = attributes;
        }
        json metric = json::object();
        metric["name"] = std::move(name);
        metric["unit"] = std::move(unit);
        json body = json::object();
        body["dataPoints"] = json::array({std::move(point)});
        if (gauge) {
            metric["gauge"] = std::move(body);
        } else {
            // 2 is CUMULATIVE in the OTLP enumeration; 1 would be DELTA.
            body["aggregationTemporality"] = 2;
            body["isMonotonic"] = true;
            metric["sum"] = std::move(body);
        }
        return metric;
    }

    static json otlpAttribute(std::string_view key, std::string_view value) {
        return json::array({json{{"key", std::string{key}},
                                 {"value", json{{"stringValue", std::string{value}}}}}});
    }

    std::string otlpPayload() {
        std::uint64_t requests = 0;
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
        std::uint64_t bytes = 0;
        std::uint64_t prompt_tokens = 0;
        std::uint64_t completion_tokens = 0;
        double cost = 0.0;
        std::vector<ProviderStat> providers;
        {
            std::scoped_lock lock{telemetry_mutex};
            requests = total_requests;
            successes = total_success;
            failures = total_failure;
            bytes = bytes_out;
            prompt_tokens = tokens_prompt;
            completion_tokens = tokens_completion;
            cost = cost_usd;
            for (auto it = stats.begin(); it != stats.end(); ++it) {
                providers.push_back(it->second);
            }
        }
        const std::uint64_t active =
            static_cast<std::uint64_t>(std::max(0, active_requests.load()));
        const std::uint64_t cache_hits = response_cache.hits();
        const std::uint64_t cache_misses = response_cache.misses();
        const std::uint64_t cache_entries = response_cache.size();
        const std::uint64_t cache_enabled = response_cache.enabled() ? 1 : 0;

        // Nanosecond strings: OTLP JSON encodes a uint64 as a string, because a
        // JSON number cannot hold it. The window starts when the process did,
        // which is what makes a cumulative sum meaningful across a restart.
        const auto nano = [](double seconds) {
            return std::format("{}", static_cast<std::uint64_t>(seconds * 1e9));
        };
        const std::string start_nano = nano(started_unix > 0.0 ? started_unix : nowUnix());
        const std::string now_nano = nano(nowUnix());

        json metrics = json::array();
        metrics.push_back(otlpMetric("literouter.requests", "1",
                                     static_cast<double>(requests), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.successes", "1",
                                     static_cast<double>(successes), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.failures", "1",
                                     static_cast<double>(failures), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.bytes_out", "By",
                                     static_cast<double>(bytes), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.tokens", "1",
                                     static_cast<double>(prompt_tokens + completion_tokens),
                                     start_nano, now_nano, false, json::array()));
        metrics.push_back(otlpMetric("literouter.cost_usd", "USD", cost, start_nano, now_nano,
                                     false, json::array()));
        metrics.push_back(otlpMetric("literouter.active_requests", "1",
                                     static_cast<double>(active), start_nano, now_nano, true,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.breakers_open", "1",
                                     static_cast<double>(router.openBreakerCount(nowUnix())),
                                     start_nano, now_nano, true, json::array()));
        metrics.push_back(otlpMetric("literouter.cache_enabled", "1",
                                     static_cast<double>(cache_enabled), start_nano, now_nano, true,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.cache_hits", "1",
                                     static_cast<double>(cache_hits), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.cache_misses", "1",
                                     static_cast<double>(cache_misses), start_nano, now_nano, false,
                                     json::array()));
        metrics.push_back(otlpMetric("literouter.cache_entries", "1",
                                     static_cast<double>(cache_entries), start_nano, now_nano, true,
                                     json::array()));
        for (const auto &stat : providers) {
            const auto attributes = otlpAttribute("relay", stat.provider);
            metrics.push_back(otlpMetric("literouter.relay.requests", "1",
                                         static_cast<double>(stat.requests), start_nano, now_nano,
                                         false, attributes));
            metrics.push_back(otlpMetric("literouter.relay.failures", "1",
                                         static_cast<double>(stat.failures), start_nano, now_nano,
                                         false, attributes));
            metrics.push_back(otlpMetric("literouter.relay.cost_usd", "USD", stat.cost_usd,
                                         start_nano, now_nano, false, attributes));
            metrics.push_back(otlpMetric("literouter.relay.latency_ms_p95", "ms",
                                         stat.latency_ms_p95, start_nano, now_nano, true,
                                         attributes));
        }

        json payload = json::object();
        payload["resourceMetrics"] = json::array({json{
            {"resource", json{{"attributes", otlpAttribute("service.name", "literouter")}}},
            {"scopeMetrics", json::array({json{
                {"scope", json{{"name", "literouter"}, {"version", std::string{kVersion}}}},
                {"metrics", std::move(metrics)}}})}
        }});
        return dumpJson(payload);
    }

    void exportOtlp() {
        std::string endpoint;
        {
            std::scoped_lock lock{otlp_mutex};
            endpoint = otlp_endpoint;
        }
        if (endpoint.empty()) {
            otlp_write_failed = false;
            return;
        }
        const double now = nowUnix();
        if (last_otlp_export_unix > 0.0 && now - last_otlp_export_unix < kOtlpIntervalSec) {
            return;
        }
        last_otlp_export_unix = now;

        // A synthetic provider rather than a bespoke HTTP call: the collector is
        // just another HTTPS endpoint, and reusing the upstream unit means the
        // trust store, the timeouts and the error text are the same ones every
        // relay already goes through.
        ProviderConfig sink;
        sink.id = "otlp";
        sink.base_url = endpoint;
        sink.timeout_sec = 10;
        sink.connect_timeout_sec = 5;
        const UpstreamResult result =
            upstreamPostRaw(sink, "/v1/metrics", otlpPayload(), "application/json");
        if (!result.ok || result.status < 200 || result.status >= 300) {
            // Reported once per failure run, like the telemetry file: a collector
            // that is down would otherwise fill the log it cannot receive.
            if (!otlp_write_failed) {
                otlp_write_failed = true;
                recordSystem(result.ok
                                 ? std::format("OTLP export to {} failed with HTTP {}", endpoint,
                                               result.status)
                                 : std::format("OTLP export to {} failed: {}", endpoint,
                                               result.error),
                             "error");
            }
            return;
        }
        otlp_write_failed = false;
    }

    // Requests a stop and joins. No final write: used by the paths where there
    // is nothing worth persisting.
    void joinFlusher() {
        flush_stop.store(true, std::memory_order_relaxed);
        flush_wait.notify_all();
        if (flusher.joinable()) {
            flusher.join();
        }
    }

    // Joins and writes the final document — after the caller's last log entry,
    // so "stopped" is in the file.
    void stopFlusher() {
        joinFlusher();
        writeState();
    }

    // A client that hangs up mid-stream is not a relay failure: counting it as
    // one would let three disconnects trip the breaker on a relay that was
    // serving perfectly. Hence three outcomes rather than a bool.
    enum class AttemptOutcome { Success, Failure, Aborted };

    // `bytes` is what went downstream; `bytes_in` is what went upstream, which
    // is the number that tells an operator whether a relay is being fed the
    // context it was told to expect. The two prices are the relay's own, in
    // dollars per million tokens; 0 means "not written down", and then the
    // attempt costs nothing to the counter rather than costing a guess.
    void recordAttempt(std::string_view provider, AttemptOutcome outcome, double latency_ms,
                       std::uint64_t bytes, std::uint64_t prompt_tokens,
                       std::uint64_t completion_tokens, std::uint64_t bytes_in = 0,
                       double cost_usd = 0.0) {
        std::scoped_lock lock{telemetry_mutex};
        auto &stat = statFor(provider);
        ++stat.requests;
        switch (outcome) {
        case AttemptOutcome::Success: ++stat.successes; break;
        case AttemptOutcome::Failure: ++stat.failures; break;
        case AttemptOutcome::Aborted: ++stat.aborted; break;
        }
        stat.bytes_out += bytes;
        stat.bytes_in += bytes_in;
        // Priced at the call site rather than at display time: the operator may
        // correct a price later, and the cost already incurred is not re-derived
        // from the new one. The formula is estimateCost(), shared with the CLI's
        // one-off tools so the two cannot drift.
        stat.cost_usd += cost_usd;
        this->cost_usd += cost_usd;
        stat.tokens_prompt += prompt_tokens;
        stat.tokens_completion += completion_tokens;
        stat.last_used_unix = nowUnix();
        if (latency_ms > 0.0) {
            stat.latency_ms_last = latency_ms;
            stat.latency_ms_avg =
                stat.latency_ms_avg == 0.0 ? latency_ms : stat.latency_ms_avg * 0.75 + latency_ms * 0.25;
            auto &window = latency_windows[std::string{provider}];
            window.add(latency_ms);
            stat.latency_ms_p95 = window.p95();
        }
        tokens_prompt += prompt_tokens;
        tokens_completion += completion_tokens;
    }

    // Remembers which relay served a conversation, so its next turn is routed
    // back there and the provider can reuse the cached prompt prefix.
    void noteAffinity(const RequestContext &ctx, std::string_view provider,
                      AttemptOutcome outcome) {
        if (outcome != AttemptOutcome::Success) {
            return;
        }
        rememberAffinity(ctx.affinity_key, std::string{provider},
                         static_cast<double>(ctx.config.server.session_affinity_sec), nowUnix());
    }

    // A relay that answered a request an earlier relay had already failed.
    // Counted separately from `successes` because "this relay caught the fall"
    // is a different fact from "this relay was asked and worked".
    void noteAbsorbed(std::string_view provider) {
        std::scoped_lock lock{telemetry_mutex};
        ++statFor(provider).retries_in;
    }

    // A relay that was passed over, as its own entry: the request's own entry is
    // written once at the end and cannot say which candidate failed. This is the
    // line an operator reads when asking why a request moved on, so it carries
    // the attempt's latency and the model the relay was actually asked for —
    // without them a failover reads as "something went wrong, somewhere,
    // eventually".
    void logFailover(const RequestContext &ctx, const std::string &provider, int attempt,
                     std::string upstream_model, double latency_ms, double wait_ms, int status,
                     std::string message, int attempts_total) {
        LogEntry entry;
        entry.level = "warn";
        entry.request_id = ctx.id;
        entry.kind = ctx.kind;
        entry.model = ctx.model;
        entry.upstream_model = std::move(upstream_model);
        entry.provider = provider;
        entry.status = status;
        entry.stream = ctx.stream;
        entry.attempt = attempt + 1;
        entry.attempts_total = attempts_total > 0 ? attempts_total : 1;
        entry.failover = true;
        entry.latency_ms = latency_ms;
        entry.wait_ms = wait_ms;
        // A relay that never answered spent its whole latency getting there.
        entry.ttfb_ms = latency_ms;
        entry.message = std::move(message);
        if (ctx.config.server.log_bodies) {
            entry.request_body = truncateUtf8(
                redactSecrets(ctx.body),
                static_cast<std::size_t>(ctx.config.server.log_body_limit));
        }
        appendLog(std::move(entry));
    }

    // ── auth ─────────────────────────────────────────────────────────────────

    static std::string requestKey(const h::Request &req) {
        std::map<std::string, std::string, std::less<>> headers;
        for (const auto &[name, value] : req.headers) {
            headers.emplace(toLower(name), value);
        }
        return extractApiKey(headers);
    }

    // Single-user admission. Configured carriers: `server.api_key` is the one
    // credential this build knows. When it is empty the listener is open — the
    // documented behaviour, because binding to loopback is already the boundary
    // on a machine with one operator.
    static bool authorized(const AppConfig &cfg, const h::Request &req) {
        if (cfg.server.api_key.empty()) return true;
        return secureEquals(resolveSecret(cfg.server.api_key), requestKey(req));
    }

    bool admitClient(const h::Request &req, h::Response &res, RequestContext &ctx) {
        if (authorized(ctx.config, req)) return true;
        sendError(res, 401, "missing or invalid API key; send `Authorization: Bearer <key>`",
                  "authentication_error", "invalid_api_key");
        return false;
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

    void serveJson(const h::Request &req, h::Response &res, std::string_view kind,
                   const MediaRequest *multipart = nullptr) {
        const bool media = kind == "audio" || kind == "images";
        RequestContext ctx;
        ctx.id = hexId(6);
        ctx.kind = std::string{kind};
        ctx.body = multipart ? multipart->metadata() : req.body;
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

        std::string effective_req_body = ctx.body;
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
            finish(ctx, {.status = res.status, .message = "malformed JSON body"});
            return;
        }

        if (ctx.model.empty()) {
            if (const auto it = body.find("model"); it != body.end() && it->is_string()) {
                ctx.model = it->get<std::string>();
            }
        }
        if (ctx.model.empty() && kind == "images" && !body.contains("model")) {
            ctx.model = "dall-e-2";
        } else if (ctx.model.empty() && kind == "images" && multipart && multipart->model.empty()) {
            ctx.model = "dall-e-2";
        }
        if (ctx.model.empty()) {
            sendError(res, 400, "`model` is required");
            finish(ctx, {.status = res.status, .message = "missing model"});
            return;
        }

        if (media) {
            // Speech responses can be binary and start arriving before the
            // generation finishes, even without an explicit JSON stream flag.
            ctx.stream = req.path == "/v1/audio/speech" ||
                         (body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>());
        } else if (kind == "gemini_stream") {
            ctx.stream = true;
        } else if (ingress_protocol == "gemini") {
            ctx.stream = req.has_param("alt") && req.get_param_value("alt") == "sse";
        } else {
            ctx.stream = (kind == "chat" || kind == "responses" || kind == "anthropic") &&
                         body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>();
        }

        if (!admitClient(req, res, ctx)) {
            finish(ctx, {.status = res.status, .message = "client admission rejected"});
            return;
        }
        if (!media && ctx.config.server.session_affinity_sec > 0) {
            const auto key = sessionKey(body);
            if (!key.empty()) ctx.affinity_key = key;
        }

        // ── the local response cache ────────────────────────────────────────
        // Looked up before a candidate is chosen, because a hit needs no relay at
        // all. Streaming is excluded by construction: a cached answer is one
        // body, and the client asked for an event stream. Media is excluded
        // because its request is multipart and its answer can be binary.
        const bool cacheable = !media && !ctx.stream;
        const std::string cache_key =
            cacheable ? ResponseCache::keyFor(ingress_protocol, ctx.model, effective_req_body)
                      : std::string{};
        if (cacheable) {
            if (auto hit = response_cache.lookup(cache_key, nowUnix()); hit) {
                res.status = hit->status;
                res.set_header("X-Literouter-Cache", "hit");
                res.set_content(hit->body, hit->content_type);
                // Counted as a request, and counted against nobody: no relay was
                // asked, so no relay stat moves and no tokens are charged. That
                // is the point of the cache.
                finish(ctx, {.provider = "cache",
                             .status = hit->status,
                             .bytes = hit->body.size(),
                             .message = std::format("{} · served from the local response cache",
                                                    ctx.model)});
                return;
            }
        }

        auto candidates = order(router.candidatesFor(ctx.model));
        if (media && !candidates.empty()) {
            std::erase_if(candidates, [&](const Candidate &candidate) {
                const auto *provider = ctx.config.provider(candidate.provider);
                return provider == nullptr ||
                       (!provider->protocol.empty() && !ciEqual(provider->protocol, "openai") &&
                        !ciEqual(provider->protocol, "openai_compatible") &&
                        !ciEqual(provider->protocol, "openai_chat"));
            });
            if (candidates.empty()) {
                sendError(res, 400, "audio and image endpoints require an openai provider",
                          "invalid_request_error", "unsupported_media_protocol");
                finish(ctx, {.status = res.status, .message = "unsupported media protocol"});
                return;
            }

        }
        // The policy reorders the chain the operator's priority produced; it does
        // not replace it. Ties keep the priority order, and a relay with no
        // measurement yet sorts after the ones with one — it has not earned a
        // place at the front, but it is not disqualified either.
        if (ctx.config.server.routing_policy == "fastest" ||
            ctx.config.server.routing_policy == "cheapest") {
            const bool fastest = ctx.config.server.routing_policy == "fastest";
            const auto metric_of = [&](const Candidate &candidate) -> double {
                if (!fastest) {
                    const ProviderConfig *provider = ctx.config.provider(candidate.provider);
                    if (provider == nullptr ||
                        (provider->price_in_per_million <= 0.0 &&
                         provider->price_out_per_million <= 0.0)) {
                        return -1.0; // unpriced: unknown, so it sorts last
                    }
                    return provider->price_in_per_million + provider->price_out_per_million;
                }
                // p95 rather than the average: a relay that is usually fast and
                // occasionally terrible is not the one to try first. 0 means it
                // has never been measured.
                const double measured = latencyMetric(candidate.provider);
                return measured > 0.0 ? measured : -1.0;
            };
            std::stable_sort(candidates.begin(), candidates.end(),
                             [&](const Candidate &a, const Candidate &b) {
                                 const double left = metric_of(a);
                                 const double right = metric_of(b);
                                 if (left < 0.0 || right < 0.0) {
                                     return left >= 0.0 && right < 0.0; // measured first
                                 }
                                 return left < right;
                             });
        }
        if (!ctx.affinity_key.empty()) {
            const std::string warm = affinityProvider(ctx.affinity_key, nowUnix());
            if (!warm.empty()) {
                // Only the warm relay moves, and only if it is still a candidate
                // that is not being skipped; the rest of the chain keeps the order
                // the operator asked for.
                std::stable_partition(candidates.begin(), candidates.end(),
                                      [&warm](const Candidate &candidate) {
                                          return !candidate.skipped && candidate.provider == warm;
                                      });
            }
        }
        if (candidates.empty()) {
            sendError(res, 404,
                      std::format("no relay can serve `{}`. Add it to a route, or list it in a "
                                  "relay's `models` array.",
                                  ctx.model),
                      "invalid_request_error", "model_not_found");
            finish(ctx, {.status = res.status, .message = "no candidate"});
            return;
        }

        const std::size_t budget = attemptBudget(ctx.config, candidates.size());
        // The whole request's budget, as opposed to one attempt's: a chain of
        // slow relays can otherwise keep a client waiting for minutes, and the
        // client asked for an answer, not for an explanation of why not.
        const double deadline_unix =
            ctx.config.server.request_deadline_sec > 0
                ? ctx.started + static_cast<double>(ctx.config.server.request_deadline_sec)
                : 0.0;
        std::string last_error;
        int last_status = 0;
        bool deadline_hit = false;
        // Set when a candidate was passed over because the relay is at its own
        // concurrency or rate limit, which is a different answer to give the
        // client than "every relay failed".
        bool rate_limited = false;
        int last_retry_after = 1;

        for (std::size_t attempt = 0; attempt < budget; ++attempt) {
            const Candidate &candidate = candidates[attempt];
            const ProviderConfig *provider = ctx.config.provider(candidate.provider);
            if (provider == nullptr || !provider->enabled) {
                continue;
            }
            if (deadline_unix > 0.0 && deadline_unix - nowUnix() <= 0.5) {
                // Half a second is not enough to dial anything, so this is where
                // the request stops rather than where another relay is tried.
                deadline_hit = true;
                last_error = std::format(
                    "the {}s request deadline passed after {} attempt(s); `{}` was not tried",
                    ctx.config.server.request_deadline_sec, attempt, candidate.provider);
                break;
            }

            const std::string upstream_model =
                candidate.model.empty() ? ctx.model : candidate.model;
            const bool effective_stream = ctx.stream && provider->supports_stream;

            // ── relay-side admission ────────────────────────────────────────
            // A relay that is full is not a relay that failed, which is why this
            // does not touch the breaker: skipping it for this request and
            // letting the next candidate answer is the right remedy, and the
            // relay may be perfectly healthy again a second from now. The slot
            // lives until the attempt's response ends — for a stream, until the
            // last byte — so a limit of 4 really does mean four in flight.
            auto admitted = upstream_limiter.admit(provider->id, nowUnix());
            if (!admitted) {
                last_error = std::format("{}: {}", provider->id, admitted.error().message);
                last_status = 429;
                rate_limited = true;
                last_retry_after = std::max(last_retry_after, admitted.error().retry_after_sec);
                logFailover(ctx, provider->id, static_cast<int>(attempt), upstream_model, 0.0,
                            (nowUnix() - ctx.started) * 1000.0, 429,
                            std::format("{} — skipping a full relay", last_error),
                            static_cast<int>(budget));
                continue;
            }
            const std::shared_ptr<UpstreamSlot> slot = std::move(*admitted);

            const std::string egress_protocol = provider->protocol.empty() ? "openai" : provider->protocol;
            // Compared by wire shape, not by name: an `azure` relay speaks
            // OpenAI's JSON, so a request that arrived in OpenAI's shape needs no
            // conversion even though the two protocol names differ. Comparing
            // names here would send an unconverted body to a relay that cannot
            // read it.
            const bool same_protocol =
                media || wireShapeOf(ingress_protocol) == wireShapeOf(egress_protocol);

            const std::string path =
                media ? req.path.substr(3)
                      : (kind == "embeddings" ? provider->embeddings_path
                                              : resolveChatPath(*provider, upstream_model, effective_stream));
            std::string payload = req.body;
            const std::string request_type = multipart ? multipart->contentType() : "application/json";
            if (multipart) {
                payload = multipart->payload(upstream_model);
            } else if (kind != "embeddings") {
                if (same_protocol) {
                    // Direct passthrough! When model renaming is configured, rewrite model only if not Gemini (Gemini embeds in path)
                    if (!candidate.model.empty() && candidate.model != ctx.model && egress_protocol != "gemini") {
                        const json parsed_req = json::parse(req.body, nullptr, false);
                        if (!parsed_req.is_discarded() && parsed_req.is_object()) {
                            json patched = parsed_req;
                            patched["model"] = upstream_model;
                            payload = dumpJson(patched);
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

            ctx.attempted_upstream = true;
            if (ctx.stream) {
                last_status = relayStream(ctx, res, *provider, candidate, path, payload, attempt,
                                          budget, last_error, ingress_protocol, request_type, media,
                                          slot);
                if (last_status == 0) {
                    return; // committed: the response is the client's now
                }
                continue;
            }

            const double attempt_started = nowUnix();
            UpstreamResult result = media
                ? upstreamPostRaw(*provider, path, payload, request_type)
                : upstreamPost(*provider, path, payload);
            const auto waited_ms = [&] { return (attempt_started - ctx.started) * 1000.0; };
            if (!result.ok) {
                last_error = std::format("{}: {}", provider->id, result.error);
                last_status = 502;
                router.recordFailure(provider->id, result.error, nowUnix());
                // No response at all: the request never reached the relay's
                // socket, so it counts as nothing sent.
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
                router.recordFailure(provider->id, last_error, nowUnix(),
                                     retryAfterSeconds(result.headers));
                recordAttempt(provider->id, AttemptOutcome::Failure, result.latency_ms, 0, 0, 0,
                              payload.size());
                logFailover(ctx, provider->id, static_cast<int>(attempt), upstream_model,
                            result.latency_ms, waited_ms(), result.status,
                            std::format("{} — failing over", last_error),
                            static_cast<int>(budget));
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
                                     nowUnix(), retryAfterSeconds(result.headers));
            }

            ProviderStat usage;
            ProxyServer::accumulateUsage(result.body, usage);
            const double attempt_cost =
                estimateCost(provider->price_in_per_million, provider->price_out_per_million,
                             usage.tokens_prompt, usage.tokens_completion);
            recordAttempt(provider->id,
                          relay_behaved ? AttemptOutcome::Success : AttemptOutcome::Failure,
                          result.latency_ms, result.body.size(), usage.tokens_prompt,
                          usage.tokens_completion, payload.size(), attempt_cost);
            // Only a relay that answered usefully becomes the conversation's
            // home: pinning a conversation to whatever answered "400 context
            // length exceeded" would bake in the failure.
            noteAffinity(ctx, provider->id,
                                good ? AttemptOutcome::Success : AttemptOutcome::Failure);
            if (good && attempt > 0) {
                noteAbsorbed(provider->id);
            }

            std::string out_body = result.body;
            std::string out_content_type = contentTypeOf(result);
            if (is_html_err) {
                const std::string msg = summarizeHtmlError(provider->id, result.status,
                                                           result.headers, result.body);
                out_body = dumpJson(
                errorBody(msg, "upstream_error", is_cf_block ? "cf_blocked" : "upstream_error"));
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
            if (cacheable && good && response_cache.enabled()) {
                // Stored after the client-facing transformation, not before: the
                // cache must hold what this client would have been sent, or a
                // later hit would hand an Anthropic client an OpenAI body.
                response_cache.store(cache_key,
                                     CachedResponse{result.status, out_content_type, out_body},
                                     nowUnix());
                // Marked only when the cache actually stored something. A
                // disabled cache that still answered "miss" would be claiming a
                // decision it did not make, and an operator debugging a stale
                // answer would look in the wrong place.
                res.set_header("X-Literouter-Cache", "miss");
            }

            finish(ctx, {.provider = provider->id,
                         .upstream_model = upstream_model,
                         .status = result.status,
                         .bytes = out_body.size(),
                         .prompt_tokens = usage.tokens_prompt,
                         .completion_tokens = usage.tokens_completion,
                         .cost_usd = attempt_cost,
                         .usage_reported = tokenUsageReported(result.body),
                         .message = std::format("{} → {}", ctx.model, upstream_model),
                         // A buffered answer arrives in one piece: httplib reports one
                         // number for connect-plus-answer, so that number is the relay's
                         // time to first byte and there is no streaming phase to report.
                         .wait_ms = waited_ms(),
                         .ttfb_ms = result.latency_ms,
                         .response_body = logged_body(ctx, out_body),
                         .failover = attempt > 0,
                         .attempt = static_cast<int>(attempt) + 1,
                         .attempts_total = static_cast<int>(budget)});
            return;
        }

        // A request that ran out of time is a gateway timeout, not a service
        // that is unavailable: the relays may be perfectly fine, they were
        // simply not given the chance. The retryable-status rule below would
        // otherwise turn it into a 503, which says something less true.
        //
        // A relay-side limit is reported as 429 rather than 503 because it comes
        // with a real Retry-After and because the client's remedy is to slow
        // down and come back, which is exactly what a 429 says.
        const int status = deadline_hit
                               ? 504
                               : (rate_limited
                                      ? 429
                                      : (last_status != 0 && !Router::retryableStatus(last_status)
                                             ? last_status
                                             : 503));
        if (last_error.empty()) {
            last_error = std::format("every relay for `{}` was skipped or disabled", ctx.model);
        }
        sendError(res, status, last_error, rate_limited ? "rate_limit_error" : "upstream_error",
                  rate_limited ? "relay_capacity_exceeded" : "all_relays_failed");
        if (rate_limited) {
            res.set_header("Retry-After", std::to_string(last_retry_after));
        }
        finish(ctx, {.status = status,
                     .message = last_error,
                     .failover = budget > 1,
                     .attempt = static_cast<int>(budget),
                     .attempts_total = static_cast<int>(budget)});
    }

    // Runs one streaming attempt. Returns 0 when the response was committed —
    // the caller must then return — and the HTTP status it failed with
    // otherwise.
    int relayStream(const RequestContext &ctx, h::Response &res, const ProviderConfig &provider,
                    const Candidate &candidate, const std::string &path,
                    const std::string &payload, std::size_t attempt, std::size_t budget,
                    std::string &last_error, const std::string &ingress_protocol,
                    const std::string &request_type = "application/json",
                    bool opaque_response = false,
                    std::shared_ptr<UpstreamSlot> slot = nullptr) {
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
        // One transport attempt: take a pooled connection, build the request on
        // it, and start the reader that will deliver the answer. A lambda because
        // it can run a second time (see the retry at the gate), and because
        // duplicating twenty lines of request setup is how two copies drift.
        std::shared_ptr<h::Client> client;
        std::thread reader;
        const auto open_transport = [&] {
            // The connection comes from the same per-thread pool the buffered
            // path uses, so a streamed request does not pay for a TCP and TLS
            // handshake the previous one already paid for. Configuration is
            // therefore also shared with that path (follow_location, both
            // timeouts, keep-alive, the trust store) instead of being repeated
            // here, where it could drift.
            UpstreamConnection connection = checkoutUpstreamConnection(root, provider);
            client = std::shared_ptr<h::Client>(connection,
                                                static_cast<h::Client *>(connection.get()));
            bridge->client = client;
            {
                // A retry starts from a clean bridge: whatever the failed attempt
                // had queued is not part of the answer the client will get.
                std::scoped_lock lock{bridge->mutex};
                bridge->headers_ready = false;
                bridge->finished = false;
                bridge->aborted = false;
                bridge->error.clear();
                bridge->error_code = h::Error::Success;
                bridge->queue.clear();
                bridge->queued_bytes = 0;
            }

            h::Request upstream;
            upstream.method = "POST";
            upstream.path = joinPath(prefix, path);
            upstream.body = payload;
            upstream.set_header("Content-Type", "application/json");
            upstream.set_header("Accept", opaque_response ? "*/*" : "text/event-stream");
            upstream.set_header("User-Agent", std::string{kUserAgent});
            // The same auth unit the buffered path uses, so a signature or a
            // minted Vertex token is produced identically on both legs. A
            // credential that cannot be produced is reported through the bridge,
            // which is what turns it into a failover rather than a crash.
            auto auth = upstreamAuthHeaders(provider, "POST", upstream.path, payload, nowUnix());
            if (!auth) {
                std::scoped_lock lock{bridge->mutex};
                bridge->error = auth.error();
                bridge->finished = true;
                bridge->cv.notify_all();
            } else {
                for (const auto &header : *auth) {
                    upstream.set_header(header.name, header.value);
                }
            }
            for (const auto &[name, value] : provider.headers) {
                if (!name.empty()) {
                    upstream.set_header(name, value);
                }
            }

            // Provider headers cannot override a multipart boundary.
            if (opaque_response) {
                upstream.headers.erase("Content-Type");
                upstream.set_header("Content-Type", request_type);
            }

            upstream.response_handler = [bridge](const h::Response &response) {
                std::scoped_lock lock{bridge->mutex};
                bridge->status = response.status;
                bridge->headers_unix = nowUnix();
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

            reader = std::thread([bridge, client, upstream = std::move(upstream)]() mutable {
                auto result = client->send(upstream);
                std::scoped_lock lock{bridge->mutex};
                if (!result) {
                    bridge->error = errorText(result.error());
                    bridge->error_code = result.error();
                }
                bridge->finished = true;
                bridge->cv.notify_all();
            });
        };

        // What the relay was asked for: the route's model when it renames, the
        // client's otherwise. Declared before the gate because every outcome —
        // including the failovers, which log their own entry — records it.
        const std::string upstream_model = candidate.model.empty() ? ctx.model : candidate.model;

        // ── the failover gate ───────────────────────────────────────────────
        // Wait for headers-or-death. The deadline is generous because a relay
        // legitimately takes a while to first token on a large prompt; it
        // exists so a black-holed connection cannot pin a worker forever.
        const auto wait_for_gate = [&] {
            // Waiting for a relay to start answering is the one phase a request
            // deadline can still cut short: nothing has been sent to the client
            // yet, so giving up here is a failover, not a truncation.
            double seconds = static_cast<double>(std::max(10, provider.timeout_sec));
            if (ctx.config.server.request_deadline_sec > 0) {
                const double left = (ctx.started +
                                     static_cast<double>(ctx.config.server.request_deadline_sec)) -
                                    nowUnix();
                seconds = std::min(seconds, std::max(0.5, left));
            }
            std::unique_lock lock{bridge->mutex};
            bridge->cv.wait_for(lock, std::chrono::duration<double>(seconds),
                                [&] { return bridge->headers_ready || bridge->finished; });
            return bridge->headers_ready;
        };

        const double attempt_started = nowUnix();
        // Everything before this attempt — the proxy's own hand-off and whatever
        // earlier candidates cost — is "wait", and it is the number that makes a
        // failover chain's price visible.
        const auto waited_ms = [&] { return (attempt_started - ctx.started) * 1000.0; };
        open_transport();
        bool got_headers = wait_for_gate();
        if (!got_headers) {
            client->stop();
            if (reader.joinable()) {
                reader.join();
            }
            // `Error::Connection` is the one transport error that means nothing
            // was ever sent — the pooled socket was found dead and the reconnect
            // did not happen — which is why the buffered path retries it on a
            // fresh connection. This does the same, but only when there is no
            // other candidate to move to: otherwise failing over is both faster
            // and likelier to work, and the retry would only add a connect
            // timeout in front of a relay that is down.
            h::Error transport_error = h::Error::Success;
            {
                std::scoped_lock lock{bridge->mutex};
                transport_error = bridge->error_code;
            }
            if (transport_error == h::Error::Connection && attempt + 1 >= budget) {
                retireUpstreamConnection(root, provider);
                open_transport();
                got_headers = wait_for_gate();
            }
        }

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
            retireUpstreamConnection(root, provider);
            router.recordFailure(provider.id, reason, nowUnix());
            recordAttempt(provider.id, AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, 0, 0, 0);
            logFailover(ctx, provider.id, static_cast<int>(attempt), upstream_model,
                        (nowUnix() - attempt_started) * 1000.0, waited_ms(), 0,
                        std::format("{} — failing over", reason), static_cast<int>(budget));
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
            std::vector<std::pair<std::string, std::string>> retry_headers;
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
                retry_headers = bridge->headers;
                bridge->aborted = true;
                bridge->cv.notify_all();
            }
            client->stop();
            if (reader.joinable()) {
                reader.join();
            }

            last_error =
                std::format("{}: HTTP {} {}", provider.id, status, truncateUtf8(trim(detail), 160));
            // The transfer was cut short, so its socket is not one to hand back.
            retireUpstreamConnection(root, provider);
            // A streamed 429 has the same thing to say about when it will be
            // ready as a buffered one does.
            router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix(),
                                 retryAfterSeconds(retry_headers));
            recordAttempt(provider.id, AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, 0, 0, 0, payload.size());
            logFailover(ctx, provider.id, static_cast<int>(attempt), upstream_model,
                        (nowUnix() - attempt_started) * 1000.0, waited_ms(), status,
                        std::format("HTTP {} — failing over", status), static_cast<int>(budget));
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
            // Drained and stopped above: this socket is finished, not reusable.
            retireUpstreamConnection(root, provider);
            const bool retryable = Router::retryableStatus(status) || is_cf_block || (status == 403 && is_html_err);

            if (retryable && attempt + 1 < budget) {
                last_error = is_cf_block
                                 ? std::format("{}: Cloudflare WAF blocked (HTTP {})", provider.id, status)
                                 : std::format("{}: HTTP {}", provider.id, status);
                router.recordFailure(provider.id, last_error, nowUnix(),
                                     retryAfterSeconds(error_headers));
                recordAttempt(provider.id, AttemptOutcome::Failure,
                              (nowUnix() - attempt_started) * 1000.0, 0, 0, 0, payload.size());
                logFailover(ctx, provider.id, static_cast<int>(attempt), upstream_model,
                            (nowUnix() - attempt_started) * 1000.0, waited_ms(), status,
                            std::format("{} — failing over", last_error),
                            static_cast<int>(budget));
                return status;
            }

            const bool ok_status = status >= 200 && status < 300;
            const bool relay_behaved = ok_status || (!retryable && !is_html_err && status != 403);
            if (relay_behaved) {
                router.recordSuccess(provider.id, 0.0, nowUnix());
            } else {
                router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix(),
                                     retryAfterSeconds(error_headers));
            }
            ProviderStat error_usage;
            ProxyServer::accumulateUsage(body, error_usage);
            const double error_cost = estimateCost(provider.price_in_per_million,
                provider.price_out_per_million, error_usage.tokens_prompt, error_usage.tokens_completion);
            recordAttempt(provider.id,
                          relay_behaved ? AttemptOutcome::Success : AttemptOutcome::Failure,
                          (nowUnix() - attempt_started) * 1000.0, body.size(),
                          error_usage.tokens_prompt, error_usage.tokens_completion,
                          payload.size(), error_cost);

            std::string out_body = body;
            std::string out_content_type = contentTypeOf(error_headers);
            if (is_html_err) {
                const std::string msg = summarizeHtmlError(provider.id, status,
                                                           error_headers, body);
                out_body = dumpJson(
                errorBody(msg, "upstream_error", is_cf_block ? "cf_blocked" : "upstream_error"));
                out_content_type = "application/json";
            } else if (out_body.empty()) {
                // A relay that failed with a bare status still owes the caller
                // the OpenAI error shape every other failure path produces.
                out_body = dumpJson(errorBody(std::format("{}", provider.id), "upstream_error",
                                              "upstream_error"));
                out_content_type = "application/json";
            }

            res.status = status;
            for (const auto &[name, value] : error_headers) {
                if (!isHopByHop(name) && (!is_html_err || !ciEqual(name, "content-type"))) {
                    res.set_header(name, value);
                }
            }
            res.set_content(out_body, out_content_type);
            finish(ctx, {.provider = provider.id,
                         .upstream_model = upstream_model,
                         .status = status,
                         .bytes = out_body.size(),
                         .prompt_tokens = error_usage.tokens_prompt,
                         .completion_tokens = error_usage.tokens_completion,
                         .cost_usd = error_cost,
                         .usage_reported = tokenUsageReported(body),
                         .message = std::format(
                             "stream rejected with HTTP {} — returned verbatim", status),
                         .response_body = logged_body(ctx, out_body),
                         .failover = attempt > 0,
                         .attempt = static_cast<int>(attempt) + 1,
                         .attempts_total = static_cast<int>(budget)});
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
            std::vector<std::pair<std::string, std::string>> commit_headers;
            {
                std::scoped_lock lock{bridge->mutex};
                commit_headers = bridge->headers;
            }
            router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix(),
                                 retryAfterSeconds(commit_headers));
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
        if (stream_type.empty()) {
            stream_type = opaque_response ? "application/octet-stream" : "text/event-stream";
        } else if (!opaque_response && !startsWith(toLower(stream_type), "text/event-stream")) {
            stream_type = "text/event-stream";
        }
        const bool observe_sse = startsWith(toLower(stream_type), "text/event-stream");
        if (observe_sse) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("X-Accel-Buffering", "no");
        }

        const std::string provider_id = provider.id;
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
        // Shape, not name — see the note where the buffered path decides this.
        const bool same_protocol = wireShapeOf(ingress_proto) == wireShapeOf(egress_proto);

        std::shared_ptr<StreamProtocolAdapter> adapter;
        if (!opaque_response && !same_protocol) {
            adapter = std::make_shared<StreamProtocolAdapter>(egress_proto, ingress_proto, ctx.model, request_ctx.id);
            stream_type = "text/event-stream";
        }

        // Counting a streamed answer's tokens means watching the stream: the
        // usage block sits somewhere in the tail, and the reader the
        // non-streaming path uses never gets a body here to read it from.
        auto usage_observer = std::make_shared<StreamUsageObserver>();
        auto media_usage_filter = opaque_response ? std::make_shared<MediaUsageFilter>() : nullptr;

        // The log's copy of a streamed answer, for the same reason and with the
        // same bound: a stream is relayed as it arrives, so "the response body"
        // has to be accumulated on the way past. Shared with the provider below,
        // which is what actually sees the bytes that reach the client.
        const bool log_body = ctx.config.server.log_bodies && !opaque_response;
        const std::size_t log_body_limit =
            static_cast<std::size_t>(ctx.config.server.log_body_limit);
        auto streamed_body = std::make_shared<std::string>();
        const std::uint64_t payload_size = payload.size();

        res.set_chunked_content_provider(
            stream_type,
            [this, bridge, client, provider_id, upstream_model, request_ctx, failover, absorbed,
             stream_ok, attempt_number, total_attempts, attempt_started,
             adapter, usage_observer, provider_root = root, provider, log_body, log_body_limit,
             streamed_body, payload_size, observe_sse, media_usage_filter,
             slot = std::move(slot)](std::size_t, h::DataSink &sink) -> bool {
                // Where this attempt's time went. `headers_unix` is the first
                // response byte, which is the boundary between the relay thinking
                // and the answer arriving; before it, the whole elapsed time is
                // the relay's (or a failure's).
                const auto phases = [&bridge, attempt_started](double fallback_ttfb) {
                    double first_byte = 0.0;
                    {
                        std::scoped_lock lock{bridge->mutex};
                        first_byte = bridge->headers_unix;
                    }
                    const std::pair<double, double> out{
                        first_byte > 0.0 ? (first_byte - attempt_started) * 1000.0 : fallback_ttfb,
                        first_byte > 0.0 ? (nowUnix() - first_byte) * 1000.0 : 0.0};
                    return out;
                };
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
                        std::string transport_error;
                        {
                            std::scoped_lock lock{bridge->mutex};
                            transport_error = bridge->error;
                        }
                        if (!transport_error.empty()) {
                            // Headers have already committed this response. A
                            // truncated audio file or SSE answer must terminate
                            // as a transport error, never gain a success marker
                            // or concatenate bytes from a second provider.
                            retireUpstreamConnection(provider_root, provider);
                            router.recordFailure(provider_id, transport_error, nowUnix());
                            const double latency = (nowUnix() - attempt_started) * 1000.0;
                            const auto bytes = bridge->bytes_out.load();
                            const auto tokens = usage_observer->usage();
                            const auto measured = phases(latency);
                            const double cost = estimateCost(provider.price_in_per_million,
                                                             provider.price_out_per_million,
                                                             tokens.prompt, tokens.completion);
                            recordAttempt(provider_id, AttemptOutcome::Failure, latency, bytes,
                                          tokens.prompt, tokens.completion, payload_size, cost);
                            finish(request_ctx, {.provider = provider_id,
                                                 .upstream_model = upstream_model,
                                                 .status = 502,
                                                 .bytes = bytes,
                                                 .prompt_tokens = tokens.prompt,
                                                 .completion_tokens = tokens.completion,
                                                 .cost_usd = cost,
                                                 .message = "upstream stream interrupted: " + transport_error,
                                                 .wait_ms = (attempt_started - request_ctx.started) * 1000.0,
                                                 .ttfb_ms = measured.first,
                                                 .stream_ms = measured.second,
                                                 .response_body = log_body ? *streamed_body : std::string{},
                                                 .failover = failover,
                                                 .attempt = attempt_number,
                                                 .attempts_total = total_attempts});
                            return false;
                        }
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
                        const TokenUsage tokens = usage_observer->usage();
                        // A stream is the one case where the two halves of a
                        // relay's cost are separable: how long it took to start
                        // answering, and how long the answer took to arrive.
                        const std::pair<double, double> measured = phases(latency);
                        const double ttfb = measured.first;
                        const double streamed_for = measured.second;
                        const double attempt_cost =
                            estimateCost(provider.price_in_per_million,
                                         provider.price_out_per_million, tokens.prompt,
                                         tokens.completion);
                        recordAttempt(provider_id,
                                      stream_ok ? AttemptOutcome::Success
                                                : AttemptOutcome::Failure,
                                      latency, bytes, tokens.prompt, tokens.completion,
                                      payload_size, attempt_cost);
                        noteAffinity(request_ctx, provider_id,
                                     stream_ok ? AttemptOutcome::Success
                                               : AttemptOutcome::Failure);
                        if (absorbed) {
                            noteAbsorbed(provider_id);
                        }
                        finish(request_ctx, {.provider = provider_id,
                                             .upstream_model = upstream_model,
                                             .status = bridge->status,
                                             .bytes = bytes,
                                             .prompt_tokens = tokens.prompt,
                                             .completion_tokens = tokens.completion,
                                             .cost_usd = attempt_cost,
                                             .usage_reported = tokens.reported,
                                             .message = std::format("stream complete · {} · {}",
                                                                    humanBytes(bytes),
                                                                    humanMillis(latency)),
                                             .wait_ms = (attempt_started - request_ctx.started) * 1000.0,
                                             .ttfb_ms = ttfb,
                                             .stream_ms = streamed_for,
                                             .response_body = log_body ? *streamed_body
                                                                       : std::string{},
                                             .failover = failover,
                                             .attempt = attempt_number,
                                             .attempts_total = total_attempts});
                        return true;
                    }

                    if (chunk.empty()) {
                        continue;
                    }
                    // Watched before conversion: the counts are the upstream's.
                    if (observe_sse) {
                        if (media_usage_filter) usage_observer->feed(media_usage_filter->feed(chunk));
                        else usage_observer->feed(chunk);
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
                    if (writable && log_body && streamed_body->size() < log_body_limit) {
                        // Capped as it goes, so a long answer cannot make the log
                        // grow beyond what the operator allowed for an entry, and
                        // redacted for the same reason the request body is: a
                        // relay's answer can quote a key back.
                        const std::string masked = redactSecrets(std::string_view{send_chunk}.substr(
                            0, std::min(send_chunk.size(),
                                        log_body_limit - streamed_body->size())));
                        streamed_body->append(masked);
                    }
                    if (!writable || !sink.write(send_chunk.data(), send_chunk.size())) {
                        // The client hung up. Tell the reader, and unblock it
                        // from a socket nobody is waiting on any more.
                        {
                            std::scoped_lock lock{bridge->mutex};
                            bridge->aborted = true;
                        }
                        bridge->cv.notify_all();
                        client->stop();
                        // Same thread as the checkout, so the pool this retires
                        // from is the pool it came out of.
                        retireUpstreamConnection(provider_root, provider);
                        const double latency = (nowUnix() - attempt_started) * 1000.0;
                        const std::uint64_t bytes = bridge->bytes_out.load();
                        const std::pair<double, double> measured = phases(latency);
                        recordAttempt(provider_id, AttemptOutcome::Aborted, latency, bytes, 0, 0,
                                      payload_size);
                        finish(request_ctx,
                               {.provider = provider_id,
                                .upstream_model = upstream_model,
                                .status = 0,
                                .bytes = bytes,
                                .message = "client disconnected before the stream ended",
                                .wait_ms = (attempt_started - request_ctx.started) * 1000.0,
                                .ttfb_ms = measured.first,
                                .stream_ms = measured.second,
                                .response_body = log_body ? *streamed_body : std::string{},
                                .failover = failover,
                                .attempt = attempt_number,
                                .attempts_total = total_attempts});
                        return false;
                    }
                    bridge->bytes_out += send_chunk.size();
                }
            },
            [this, bridge, client, provider_id, upstream_model, request_ctx, failover,
             attempt_number, total_attempts, provider_root = root, provider](bool) {
                // This fires at the end of EVERY chunked response, not only when
                // something went wrong, which makes it the place that decides
                // whether the connection goes back to the pool or is retired.
                //
                // A transfer that finished left its socket in a clean state, so
                // the client must NOT be stopped here: `stop()` closes the
                // socket, and closing it after every streamed answer is exactly
                // what would make the pool pointless — the next request would
                // dial a fresh connection and pay the handshake the pool exists
                // to avoid. A release that arrives while the reader is still on
                // the socket is a different thing (the reader would otherwise
                // outlive the response), and that one is stopped and retired.
                bool finished = false;
                {
                    std::scoped_lock lock{bridge->mutex};
                    finished = bridge->finished;
                    if (!finished) {
                        bridge->aborted = true;
                    }
                }
                if (!finished) {
                    bridge->cv.notify_all();
                    client->stop();
                    retireUpstreamConnection(provider_root, provider);
                }

                // finish() is idempotent, so this is a no-op on every path
                // that already accounted for the request. It is here for the
                // one path that has no other reporter: httplib released the
                // provider WITHOUT ever calling it — a client that vanished
                // between the handler returning and the body being written.
                // Without this the in-flight counter would never come down and
                // the console would report a permanently busy server.
                finish(request_ctx, {.provider = provider_id,
                                     .upstream_model = upstream_model,
                                     .status = 0,
                                     .bytes = bridge->bytes_out.load(),
                                     .message = "stream ended before any bytes were written",
                                     .failover = failover,
                                     .attempt = attempt_number,
                                     .attempts_total = total_attempts});
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
    std::scoped_lock lock{impl_->config_mutex};
    return impl_->bound_server.host;
}

int ProxyServer::boundPort() const {
    return impl_->bound_port.load();
}

void ProxyServer::setConfigPath(std::string path) {
    impl_->setConfigPath(std::move(path));
}

std::expected<void, std::string> ProxyServer::start(const AppConfig &config) {
    ensureLocalTimezone();
    if (impl_->running.load()) {
        return std::unexpected(std::string{"server is already running"});
    }

    // TLS is a startup invariant, including --force: invalid credentials must
    // never silently turn a requested HTTPS listener into plaintext HTTP.
    for (const auto &issue : validate(config).issues) {
        if (issue.level == ValidationIssue::Level::Error && startsWith(issue.path, "server.tls_")) {
            return std::unexpected(issue.path + ": " + issue.message);
        }
    }
    {
        std::scoped_lock lock{impl_->config_mutex};
        impl_->config = config;
        impl_->bound_server = config.server;
    }
    impl_->router.setConfig(config);
    // The gates are configured here as well as in applyConfig(), because start()
    // does not go through applyConfig() and a freshly started server must not
    // run with the previous process's limits — or, on a first start, with none.
    impl_->upstream_limiter.setConfig(config);
    impl_->response_cache.configure(config.server.response_cache_ttl_sec,
                                   config.server.response_cache_max_entries);
    impl_->traffic_bucket_sec = std::clamp(config.server.traffic_bucket_sec, 60, 86400);
    impl_->traffic_bucket_count =
        static_cast<std::size_t>(std::clamp(config.server.traffic_bucket_count, 2, 10000));
    {
        std::scoped_lock lock{impl_->otlp_mutex};
        impl_->otlp_endpoint = config.server.otlp_endpoint;
    }
    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        impl_->log.setCapacity(static_cast<std::size_t>(std::max(16, config.server.log_capacity)));
    }
    impl_->setPersistence(config.server.persist_telemetry);
    // A verified health probe produces a friendlier diagnostic than bind's
    // address-in-use error; exclusive socket options below remain authoritative.
    if (config.server.port != 0) {
        const std::filesystem::path pid_path = defaultPidPath(config.server.port);
        if (const std::string clash = existingInstance(config.server, pid_path);
            !clash.empty()) {
            return std::unexpected(clash);
        }
    }
    impl_->stopping.store(false);

    if (config.server.tls_cert_file.empty()) {
        impl_->server = std::make_unique<h::Server>();
    } else {
        impl_->server = std::make_unique<h::SSLServer>([&config](h::tls::ctx_t context) {
            auto *ctx = static_cast<SSL_CTX *>(context);
            SSL_CTX_set_default_passwd_cb(ctx, [](char *, int, int, void *) -> int { return 0; });
            return SSL_CTX_use_certificate_chain_file(ctx, config.server.tls_cert_file.c_str()) == 1 &&
                   SSL_CTX_use_PrivateKey_file(ctx, config.server.tls_key_file.c_str(), SSL_FILETYPE_PEM) == 1 &&
                   SSL_CTX_check_private_key(ctx) == 1;
        });
        if (!impl_->server->is_valid()) {
            return std::unexpected(std::string{"cannot initialize HTTPS listener from TLS files"});
        }
    }
    auto &server = *impl_->server;
    // Do not share the listening port, even when a TLS probe cannot verify an
    // existing instance's private CA. SO_REUSEADDR still permits quick restarts.
    server.set_socket_options([](auto sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
#endif
    });
    server.set_payload_max_length(kMaxRequestBody);
    server.set_keep_alive_max_count(64);
    // 0 means "no write deadline". A streamed answer can legitimately idle for
    // a long time between tokens, and a write timeout would cut it off.
    server.set_write_timeout(0, 0);

    server.set_pre_routing_handler([this](const h::Request &req, h::Response &res) {
        // CORS belongs to the client-facing API only. `Access-Control-Allow-Origin: *`
        // on the management surface would let any page the operator visits read
        // this proxy's log — which can hold prompts — and POST /shutdown.
        const bool console_path = isConsolePath(req.path);
        if (console_path || startsWith(req.path, kAdminPrefix)) {
            res.set_header("Cache-Control", "no-store");
        } else {
            Impl::applyCors(res);
        }
        if (req.method == "OPTIONS") {
            res.status = 204;
            return h::Server::HandlerResponse::Handled;
        }
        // Liveness answers unauthenticated, as both protocol documents promise:
        // whatever runs a healthcheck does not carry the operator's key.
        if (healthProbeFor(req.path) != HealthProbe::None && req.method == "GET") {
            return h::Server::HandlerResponse::Unhandled;
        }
        // The console shell is static and holds no data: it has to load before
        // the operator can type a key. Every byte it then fetches from the
        // admin API still passes the check below. When the console is switched
        // off its paths fall through to the same check as anything else.
        if (console_path && req.method == "GET" && impl_->snapshotConfig().server.web_ui) {
            return h::Server::HandlerResponse::Unhandled;
        }
        if (Impl::authorized(impl_->snapshotConfig(), req)) {
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

    const auto pipeline = [this](const h::Request &req, h::Response &res, std::string_view kind,
                                 const MediaRequest *multipart = nullptr) {
        impl_->active_requests.fetch_add(1, std::memory_order_relaxed);
        try {
            impl_->serveJson(req, res, kind, multipart);
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

    // Read MIME incrementally into bounded ordered parts. httplib's ordinary
    // handler separates fields/files, losing their interleaving and raw body.
    // The content reader preserves duplicate image[] / timestamp fields and
    // all part headers while sharing the JSON pipeline's admission point.
    for (const char *path : {"/v1/audio/transcriptions", "/v1/audio/translations",
                             "/v1/audio/speech", "/v1/images/generations",
                             "/v1/images/edits", "/v1/images/variations"}) {
        server.Post(path, [pipeline](const h::Request &req, h::Response &res,
                                    const h::ContentReader &reader) {
            const std::string_view kind = startsWith(req.path, "/v1/audio/") ? "audio" : "images";
            const bool multipart_only = req.path == "/v1/audio/transcriptions" ||
                                        req.path == "/v1/audio/translations" ||
                                        req.path == "/v1/images/variations";
            const bool json_only = req.path == "/v1/audio/speech" ||
                                   req.path == "/v1/images/generations";
            if ((multipart_only && !req.is_multipart_form_data()) ||
                (json_only && req.is_multipart_form_data())) {
                sendError(res, 415, multipart_only ? "multipart/form-data is required" : "application/json is required");
                return;
            }
            if (req.is_multipart_form_data()) {
                auto media = readMediaMultipart(reader);
                if (!media) {
                    sendError(res, res.status == 413 ? 413 : 400, media.error());
                    return;
                }
                pipeline(req, res, kind, &*media);
            } else {
                h::Request buffered = req;
                if (!reader([&](const char *data, std::size_t size) {
                        if (size > kMaxRequestBody - buffered.body.size()) return false;
                        buffered.body.append(data, size);
                        return true;
                    })) {
                    sendError(res, res.status == 413 ? 413 : 400, "could not read media request");
                    return;
                }
                pipeline(buffered, res, kind);
            }
        });
    }

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
        res.set_content(dumpJson(json{{"models", std::move(models)}}), "application/json");
    });

    const auto modelsListHandler = [this](const h::Request &, h::Response &res) {
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
            }
            data.push_back(std::move(item));
        }
        res.status = 200;
        res.set_content(dumpJson(json{{"object", "list"}, {"data", std::move(data)}}),
                        "application/json");
    };
    server.Get("/v1/models", modelsListHandler);
    server.Get("/models", modelsListHandler);

    const auto modelDetailHandler = [this](const h::Request &req, h::Response &res) {
        const AppConfig cfg = impl_->snapshotConfig();
        const std::string name = req.matches[1];
        const auto known = cfg.logicalModels();
        if (std::ranges::find(known, name) == known.end()) {
            sendError(res, 404, std::format("unknown model `{}`", name), "invalid_request_error",
                      "model_not_found");
            return;
        }
        res.status = 200;
        res.set_content(
            dumpJson(json{{"id", name}, {"object", "model"}, {"owned_by", "literouter"}}),
            "application/json");
    };
    server.Get(R"(/v1/models/(.+))", modelDetailHandler);
    server.Get(R"(/models/(.+))", modelDetailHandler);

    server.Get("/health", [](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    server.Get("/health/live", [](const h::Request &, h::Response &res) {
        // Deliberately the same answer as /health: liveness asks whether the
        // process is alive, and a process that can answer this is.
        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    server.Get("/health/ready", [this](const h::Request &, h::Response &res) {
        // Readiness asks the different question: can this instance actually
        // serve a request? A listener with no enabled relay can accept a
        // connection and still cannot answer anything, and telling a load
        // balancer it is ready would be telling it to send work here to fail.
        const AppConfig cfg = impl_->snapshotConfig();
        const bool running = impl_->running.load();
        const bool has_relay = std::ranges::any_of(cfg.providers, [](const ProviderConfig &p) {
            return p.enabled;
        });
        const bool ready = running && has_relay;
        json body = json::object();
        body["status"] = ready ? "ready" : "not_ready";
        body["running"] = running;
        body["enabled_relays"] = has_relay;
        if (!ready) {
            body["reason"] = running ? "no enabled relay can serve a request"
                                     : "the listener is not accepting requests";
        }
        res.status = ready ? 200 : 503;
        res.set_content(dumpJson(body), "application/json");
    });

    // ── the built-in web console ────────────────────────────────────────────────
    //
    // Served from the same listener as everything else: a headless machine needs
    // no second process, no static-file daemon and no CORS configuration. The
    // switch is read per request, so `server.web_ui = false` — from a reload or
    // from the console itself — closes it immediately rather than at restart.

    const auto consoleOpen = [this](h::Response &res) {
        if (impl_->snapshotConfig().server.web_ui) {
            return true;
        }
        sendError(res, 404, "the web console is disabled (server.web_ui = false)",
                  "not_found", "console_disabled");
        return false;
    };

    server.Get("/", [consoleOpen](const h::Request &, h::Response &res) {
        if (!consoleOpen(res)) {
            return;
        }
        res.status = 302;
        res.set_header("Location", "/ui/");
    });
    server.Get("/favicon.ico", [consoleOpen](const h::Request &, h::Response &res) {
        if (!consoleOpen(res)) {
            return;
        }
        res.status = 302;
        res.set_header("Location", "/ui/favicon.svg");
    });
    // The console shell. `serveWebAsset` returns false when the asset cannot be
    // produced — which on the `$LITEROUTER_WEB_DIR` build means the variable is
    // unset or the file is missing. Ignoring that left a 200 with an empty body:
    // a blank page instead of an error, and the browser then reported a broken
    // script rather than the missing directory. A 500 that names the variable is
    // the only answer a reader can act on.
    const auto serveShell = [](h::Response &res) {
        if (serveWebAsset("index.html", res)) {
            return;
        }
        sendError(res, 500,
                  "the console is not embedded in this build and LITEROUTER_WEB_DIR does not "
                  "point at web/dist",
                  "server_error", "console_unavailable");
    };
    server.Get("/ui", [consoleOpen, serveShell](const h::Request &, h::Response &res) {
        if (consoleOpen(res)) {
            serveShell(res);
        }
    });
    server.Get("/ui/", [consoleOpen, serveShell](const h::Request &, h::Response &res) {
        if (consoleOpen(res)) {
            serveShell(res);
        }
    });
    server.Get(R"(/ui/([A-Za-z0-9._-]+))", [consoleOpen](const h::Request &req, h::Response &res) {
        if (!consoleOpen(res)) {
            return;
        }
        const std::string name = req.matches.size() > 1 ? req.matches[1].str() : std::string{};
        if (!serveWebAsset(name, res)) {
            sendError(res, 404, std::format("no such console asset `{}`", name),
                      "not_found", "no_such_asset");
        }
    });

    // ── management surface ──────────────────────────────────────────────────

    const std::string admin{kAdminPrefix};

    // The console's config view. A secret written literally into the file never
    // leaves the machine: it is blanked and labelled, while a `${VAR}` reference
    // passes through untouched — it names a variable, not a key.
    server.Get(admin + "/config", [this](const h::Request &, h::Response &res) {
        const AppConfig cfg = impl_->snapshotConfig();
        json doc = json::parse(toJsonString(cfg), nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            doc = json::object();
        }
        const auto hideKey = [](json &entry) {
            const auto raw = entry.value("api_key", std::string{});
            if (raw.empty()) return;
            // A fallback is an actual credential embedded in the reference.
            // Mask it like a stored literal; empty PUT values preserve the
            // original reference without exposing its fallback in the browser.
            if (isSecretReference(raw) && raw.find(":-") == std::string::npos)
                entry["api_key_source"] = "env";
            else { entry["api_key"] = ""; entry["api_key_source"] = "literal"; }
        };
        if (doc.contains("server")) hideKey(doc["server"]);
        if (auto providers = doc.find("providers");
            providers != doc.end() && providers->is_array()) {
            for (auto &entry : *providers) {
                if (!entry.is_object()) {
                    continue;
                }
                hideKey(entry);
            }
        }
        const std::string path = impl_->configPath();
        std::error_code ec;
        json root = json::object();
        root["path"] = path;
        root["exists"] = !path.empty() && std::filesystem::is_regular_file(path, ec);
        root["config"] = std::move(doc);
        root["validation"] = validationJson(validate(cfg));
        res.status = 200;
        res.set_content(dumpJson(root, 2), "application/json");
    });

    // Writes the whole config back. Editing the file's shape field by field
    // would mean a dozen endpoints and a dozen chances to leave the file
    // inconsistent; one atomic write validated as a whole cannot do that.
    //
    // Secrets round-trip without ever reaching the client: a provider whose
    // `api_key` arrives empty keeps the value already stored under that id
    // (which is what makes it safe for the console to PUT back what it got from
    // the redacting GET), and `api_key_clear: true` is the explicit way to
    // actually empty it.
    server.Put(admin + "/config", [this](const h::Request &req, h::Response &res) {
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            sendError(res, 400, "expected a config object", "invalid_request", "bad_config_body");
            return;
        }
        const auto nested = body.find("config");
        const json doc = (nested != body.end() && nested->is_object()) ? *nested : body;

        const AppConfig current = impl_->snapshotConfig();
        auto parsed = appConfigFromJson(dumpJson(doc));
        if (!parsed) {
            sendError(res, 422, parsed.error(), "invalid_config", "config_invalid");
            return;
        }

        // Which ids asked to be cleared, and which keep what the file has.
        std::set<std::string, std::less<>> clearing;
        if (const auto providers = doc.find("providers");
            providers != doc.end() && providers->is_array()) {
            for (const auto &entry : *providers) {
                if (entry.is_object() && entry.value("api_key_clear", false) &&
                    entry.contains("id") && entry["id"].is_string()) {
                    clearing.insert(entry["id"].get<std::string>());
                }
            }
        }
        for (auto &provider : parsed->providers) {
            if (clearing.contains(provider.id)) {
                provider.api_key.clear();
                continue;
            }
            if (provider.api_key.empty()) {
                if (const ProviderConfig *stored = current.provider(provider.id);
                    stored != nullptr && !stored->api_key.empty()) {
                    provider.api_key = stored->api_key;
                }
            }
        }

        const bool clear_server = doc.contains("server") && doc.at("server").value("api_key_clear", false);
        if (clear_server) parsed->server.api_key.clear();
        else if (parsed->server.api_key.empty()) parsed->server.api_key = current.server.api_key;

        const ValidationReport report = validate(*parsed);
        if (!report.ok()) {
            // Nothing is written and nothing takes effect: a half-applied bad
            // config would be the worst of both.
            json refused = validationJson(report);
            refused["ok"] = false;
            res.status = 422;
            res.set_content(dumpJson(refused, 2), "application/json");
            return;
        }

        const std::string path = impl_->configPath();
        bool saved = false;
        if (!path.empty()) {
            ConfigStore store;
            store.config() = *parsed;
            if (auto written = store.saveAs(path); !written) {
                sendError(res, 500, written.error(), "write_failed", "config_write_failed");
                return;
            }
            saved = true;
        }
        updateConfig(*parsed);

        json root = validationJson(report);
        root["ok"] = true;
        root["saved"] = saved;
        root["path"] = path;
        res.status = 200;
        res.set_content(dumpJson(root, 2), "application/json");
        impl_->recordSystem(saved ? "config written from the console"
                                  : "config updated in memory (no config path)",
                            report.count(ValidationIssue::Level::Warning) > 0 ? "warn" : "info");
    });

    server.Get(admin + "/status", [this](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(toJsonString(snapshot()), "application/json");
    });

    // The same numbers the console renders, in the format a scraper reads. On
    // the management surface, so it inherits the same key check as everything
    // else there: a scraper can send a bearer token, and these counters reveal
    // which relays exist and how they behave.
    server.Get(admin + "/metrics", [this](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(metricsText(snapshot()), "text/plain; version=0.0.4; charset=utf-8");
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
        res.set_content(dumpJson(root), "application/json");
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

        json root = validationJson(report);
        res.status = report.ok() ? 200 : 422;
        res.set_content(dumpJson(root, 2), "application/json");
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
        // from inside a handler is the loop waiting on itself. The owner keeps
        // this thread joinable, so a ProxyServer cannot be destroyed while the
        // delayed callback still holds its pointer.
        impl_->scheduleShutdown(this);
    });

    // Probes a relay two ways: by id, for a configured entry the console's
    // "Test" button points at, or from an inline object, so the same button
    // works on a form entry that has not been written to disk yet.
    server.Post(admin + "/probe", [this](const h::Request &req, h::Response &res) {
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            sendError(res, 400, R"(expected a provider object, or {"provider": "<id>"})");
            return;
        }
        ProviderConfig provider;
        // Probe a configured relay by id: the secret is resolved here, on the
        // machine that holds it, and never travels over the wire.
        if (const auto by_id = body.find("provider");
            by_id != body.end() && by_id->is_string()) {
            const std::string id = by_id->get<std::string>();
            const AppConfig cfg = impl_->snapshotConfig();
            const ProviderConfig *stored = cfg.provider(id);
            if (stored == nullptr) {
                sendError(res, 404, std::format("no provider `{}` in the running config", id),
                          "not_found", "no_such_provider");
                return;
            }
            provider = *stored;
            provider.api_key = resolveSecret(provider.api_key);
            provider.timeout_sec = std::clamp(provider.timeout_sec, 3, 30);
            res.status = 200;
            res.set_content(toJsonString(probeProvider(provider, provider.timeout_sec)),
                            "application/json");
            return;
        }
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
    {
        std::scoped_lock lock{impl_->config_mutex};
        impl_->bound_server.port = bound;
    }
    // The telemetry file belongs to the instance, and the port is what names it.
    // Computed here rather than before the bind for two reasons: a config asking
    // for port 0 has no name until the kernel picks one, and the accept loop has
    // not started yet, so nothing can be counted before the previous run's
    // numbers are back in place.
    //
    // Read once per process: a later start() (the console's Stop then Start)
    // already holds this process's telemetry in memory, and re-reading the file
    // would drag the counters backwards.
    impl_->state_path = defaultTelemetryPath(bound);
    if (!impl_->state_restored) {
        impl_->state_restored = true;
        impl_->loadState();
    }
    // Named by the port actually bound: a config asking for port 0 is otherwise
    // unidentifiable, and the file is what a later start reads to name the
    // process holding the port it wants.
    const std::filesystem::path pid_path = defaultPidPath(bound);
    if (writePidFile(pid_path, bound, impl_->configPath())) {
        impl_->pid_path = pid_path;
    }
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

    impl_->recordSystem("listening on " + snapshot().base_url);
    // Remember what the config file looks like now, so that a change is a change.
    impl_->rememberConfigStamp();
    // Started last: everything above can still fail and return, and a flusher
    // is only wanted once there is a server whose telemetry it can write.
    impl_->startFlusher();
    return {};
}

void ProxyServer::stop() {
    if (!impl_) {
        return;
    }
    const bool shouldStop = !impl_->stopping.exchange(true);
    if (!shouldStop) {
        impl_->joinShutdownThread();
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
        // Nothing was ever started, so there is nothing to write; this is also
        // the branch the destructor takes for a console that never pressed
        // Start.
        impl_->joinFlusher();
        impl_->joinShutdownThread();
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
    // The listener is gone, so the record of it should be too — a stale file
    // would otherwise be indistinguishable from one whose process is hung.
    if (!impl_->pid_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(impl_->pid_path, ec);
        impl_->pid_path.clear();
    }
    impl_->recordSystem("stopped");
    // Last, so the entry above is part of what gets written, and so no thread is
    // still holding the document when the object goes away.
    impl_->stopFlusher();
    impl_->joinShutdownThread();
}

void ProxyServer::updateConfig(const AppConfig &config) {
    impl_->applyConfig(config);
}

AppConfig ProxyServer::config() const {
    return impl_->snapshotConfig();
}

Snapshot ProxyServer::snapshot() const {
    Snapshot out;
    const AppConfig cfg = impl_->snapshotConfig();

    out.running = impl_->running.load();
    {
        std::scoped_lock lock{impl_->config_mutex};
        out.host = impl_->bound_server.host;
        out.port = impl_->bound_port.load();
        auto bound = impl_->bound_server;
        bound.port = out.port;
        out.base_url = serverBaseUrl(bound);
    }
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
        out.cost_usd = impl_->cost_usd;
        out.hourly.assign(impl_->hourly.begin(), impl_->hourly.end());
        out.traffic_bucket_sec = impl_->traffic_bucket_sec;
        out.traffic_bucket_count = static_cast<int>(impl_->traffic_bucket_count);
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
    out.cache_enabled = impl_->response_cache.enabled();
    out.cache_hits = impl_->response_cache.hits();
    out.cache_misses = impl_->response_cache.misses();
    out.cache_entries = impl_->response_cache.size();
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
    // Marked so a cleared log is not resurrected from the file by the next
    // start(): the operator asked for it gone.
    impl_->markStateDirty();
}

void ProxyServer::resetStats() {
    {
        std::scoped_lock lock{impl_->telemetry_mutex};
        impl_->stats.clear();
        impl_->latency_windows.clear();
        impl_->total_requests = 0;
        impl_->total_success = 0;
        impl_->total_failure = 0;
        impl_->bytes_out = 0;
        impl_->tokens_prompt = 0;
        impl_->tokens_completion = 0;
        impl_->cost_usd = 0.0;
        impl_->hourly.clear();
        impl_->latency_ms_avg = 0.0;
    }
    impl_->router.resetHealth();
    impl_->recordSystem("counters reset");
}

std::string ProxyServer::adminUrl(const std::string &host, int port, std::string_view path) {
    ServerConfig config;
    config.host = host;
    config.port = port;
    return serverBaseUrl(config, path);
}

std::string ProxyServer::adminUrl(const ServerConfig &server, std::string_view path) {
    return serverBaseUrl(server, path);
}

void ProxyServer::accumulateUsage(std::string_view body, ProviderStat &stat) {
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) {
        return;
    }
    const auto add = [](std::uint64_t a, std::uint64_t b) {
        return b > std::numeric_limits<std::uint64_t>::max() - a
            ? std::numeric_limits<std::uint64_t>::max() : a + b;
    };
    const auto pull = [&](const json &node) {
        if (!node.is_object()) {
            return;
        }
        const auto it = node.find("usage");
        if (it != node.end() && it->is_object()) {
            const auto number = [&](const char *key) -> std::uint64_t {
                const auto value = it->find(key);
                if (value == it->end()) return 0;
                if (value->is_number_unsigned()) return value->get<std::uint64_t>();
                if (value->is_number_float()) {
                    const double n = value->get<double>();
                    if (n >= 0 && n < 18446744073709551616.0 && std::floor(n) == n)
                        return static_cast<std::uint64_t>(n);
                }
                return 0;
            };
            // OpenAI spells these prompt/completion; the Messages-shaped relays
            // that also expose /v1/chat/completions spell them input/output.
            stat.tokens_prompt = add(stat.tokens_prompt, add(number("prompt_tokens"), number("input_tokens")));
            stat.tokens_completion = add(stat.tokens_completion, add(number("completion_tokens"), number("output_tokens")));
        }
        const auto it_gemini = node.find("usageMetadata");
        if (it_gemini != node.end() && it_gemini->is_object()) {
            const auto number = [&](const char *key) -> std::uint64_t {
                const auto value = it_gemini->find(key);
                if (value == it_gemini->end() || !value->is_number_unsigned()) return 0;
                return value->get<std::uint64_t>();
            };
            stat.tokens_prompt = add(stat.tokens_prompt, number("promptTokenCount"));
            stat.tokens_completion = add(stat.tokens_completion, number("candidatesTokenCount"));
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
    if (const auto bundle = resolveCaBundle(); !bundle.empty()) {
        // `path.string()`, not pathToUtf8(): this value is handed to OpenSSL,
        // which opens the file through the C library's narrow-path call — the
        // ANSI code page on Windows. UTF-8 bytes would be the wrong encoding.
        client.set_ca_cert_path(bundle.string());
    }
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);

    h::Headers headers;
    headers.emplace("Accept", "application/json");
    if (!api_key.empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", resolveSecret(api_key)));
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
            if (item.is_object()) {
                s.providers.push_back(providerStatFromJson(item));
            }
        }
    }

    if (const auto it = parsed.find("hourly"); it != parsed.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (!item.is_object()) {
                continue;
            }
            TrafficBucket bucket;
            bucket.hour_unix = item.value("hour_unix", 0.0);
            bucket.requests = item.value("requests", std::uint64_t{0});
            bucket.successes = item.value("successes", std::uint64_t{0});
            bucket.failures = item.value("failures", std::uint64_t{0});
            bucket.bytes_out = item.value("bytes_out", std::uint64_t{0});
            bucket.tokens_prompt = item.value("tokens_prompt", std::uint64_t{0});
            bucket.tokens_completion = item.value("tokens_completion", std::uint64_t{0});
            bucket.cost_usd = item.value("cost_usd", 0.0);
            s.hourly.push_back(std::move(bucket));
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
    if (const auto bundle = resolveCaBundle(); !bundle.empty()) {
        // `path.string()`, not pathToUtf8(): this value is handed to OpenSSL,
        // which opens the file through the C library's narrow-path call — the
        // ANSI code page on Windows. UTF-8 bytes would be the wrong encoding.
        client.set_ca_cert_path(bundle.string());
    }
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    if (!api_key.empty()) {
        h::Headers headers;
        headers.emplace("Authorization", std::format("Bearer {}", resolveSecret(api_key)));
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
            if (item.is_object()) {
                out.entries.push_back(logEntryFromJson(item));
            }
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
