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

// The liveness probe, which answers before any credential is presented.
//
// A healthcheck is run by something that holds no key — a container runtime, a
// load balancer, a systemd unit — so gating it behind `server.api_key` turns
// every such probe into a 401 and reports a healthy proxy as down. It leaks
// nothing: the body is a constant, and it says only that the listener is up.
bool isLivenessPath(std::string_view path) {
    return path == "/health";
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
// A bind cannot report this: httplib sets SO_REUSEPORT, so a second instance
// binds the same port happily and the kernel then spreads connections across
// both — two proxies, two configs, one address, and a console whose counters
// jump between them. The probe therefore identifies the other end rather than
// assuming anything about it: only a health answer shaped like ours counts, and
// whatever else holds the port is left to fail the bind with its own, accurate
// error. An empty return means the port is ours to take.
std::string existingInstance(const std::string &host, int port,
                             const std::filesystem::path &pid_path) {
    // 0.0.0.0 and :: are bind addresses, not destinations.
    const bool wildcard = host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]";
    h::Client client{wildcard ? std::string{"127.0.0.1"} : host, port};
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
    const std::string text = doc.dump(2) + "\n";
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return static_cast<bool>(output);
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
    std::map<std::string, LatencyWindow, std::less<>> latency_windows;
    LogRing log;
    std::uint64_t total_requests = 0;
    std::uint64_t total_success = 0;
    std::uint64_t total_failure = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t tokens_prompt = 0;
    std::uint64_t tokens_completion = 0;
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
        markStateDirty();
        releaseInFlight();
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

        return root.dump(2);
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
                reportStateProblem(std::format("cannot write telemetry file {}", temp.string()));
                return;
            }
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output) {
                output.close();
                std::filesystem::remove(temp, ec);
                reportStateProblem(std::format("write to telemetry file {} failed", temp.string()));
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
                                               state_path.string(), ec.message()));
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
            recordSystem(std::format("cannot read telemetry file {}", state_path.string()), "error");
            return;
        }
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || root.value("version", 0) != 1) {
            recordSystem(std::format("ignoring {}: not a readable literouter telemetry file",
                                     state_path.string()),
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
            recordSystem(std::format("ignoring {}: {}", state_path.string(), error.what()),
                         "warning");
            return;
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
        }
        recordSystem(std::format("restored {} log entries from {}", restored,
                                 state_path.string()));
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
                if (state_dirty.exchange(false, std::memory_order_relaxed)) {
                    writeState();
                }
            }
        });
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
            auto &window = latency_windows[std::string{provider}];
            window.add(latency_ms);
            stat.latency_ms_p95 = window.p95();
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
                router.recordFailure(provider->id, last_error, nowUnix(),
                                     retryAfterSeconds(result.headers));
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
                                     nowUnix(), retryAfterSeconds(result.headers));
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
            // A streamed 429 has the same thing to say about when it will be
            // ready as a buffered one does.
            router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix(),
                                 retryAfterSeconds(retry_headers));
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
                router.recordFailure(provider.id, last_error, nowUnix(),
                                     retryAfterSeconds(error_headers));
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
                router.recordFailure(provider.id, std::format("HTTP {}", status), nowUnix(),
                                     retryAfterSeconds(error_headers));
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

        // Counting a streamed answer's tokens means watching the stream: the
        // usage block sits somewhere in the tail, and the reader the
        // non-streaming path uses never gets a body here to read it from.
        auto usage_observer = std::make_shared<StreamUsageObserver>();

        res.set_chunked_content_provider(
            stream_type,
            [this, bridge, client, provider_id, upstream_model, request_ctx, failover, absorbed,
             stream_ok, attempt_number, total_attempts, attempt_started,
             adapter, usage_observer](std::size_t, h::DataSink &sink) -> bool {
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
                        const TokenUsage tokens = usage_observer->usage();
                        recordAttempt(provider_id,
                                      stream_ok ? AttemptOutcome::Success
                                                : AttemptOutcome::Failure,
                                      latency, bytes, tokens.prompt, tokens.completion);
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
                    // Watched before conversion: the counts are the upstream's.
                    usage_observer->feed(chunk);

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
    ensureLocalTimezone();
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
    impl_->setPersistence(config.server.persist_telemetry);
    // Before binding, because the bind cannot report it: httplib shares the port
    // between instances, and only one of them can be the one the operator means.
    if (config.server.port != 0) {
        const std::filesystem::path pid_path = defaultPidPath(config.server.port);
        if (const std::string clash = existingInstance(config.server.host, config.server.port, pid_path);
            !clash.empty()) {
            return std::unexpected(clash);
        }
    }
    impl_->stopping.store(false);

    auto &server = *(impl_->server = std::make_unique<h::Server>());
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
        if (isLivenessPath(req.path) && req.method == "GET") {
            return h::Server::HandlerResponse::Unhandled;
        }
        // The console shell is static and holds no data: it has to load before
        // the operator can type a key. Every byte it then fetches from the
        // admin API still passes the check below. When the console is switched
        // off its paths fall through to the same check as anything else.
        if (console_path && req.method == "GET" && impl_->snapshotConfig().server.web_ui) {
            return h::Server::HandlerResponse::Unhandled;
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
        res.set_content(json{{"object", "list"}, {"data", std::move(data)}}.dump(),
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
        res.set_content(json{{"id", name}, {"object", "model"}, {"owned_by", "literouter"}}.dump(),
                        "application/json");
    };
    server.Get(R"(/v1/models/(.+))", modelDetailHandler);
    server.Get(R"(/models/(.+))", modelDetailHandler);

    server.Get("/health", [](const h::Request &, h::Response &res) {
        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
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
    server.Get("/ui", [consoleOpen](const h::Request &, h::Response &res) {
        if (consoleOpen(res)) {
            serveWebAsset("index.html", res);
        }
    });
    server.Get("/ui/", [consoleOpen](const h::Request &, h::Response &res) {
        if (consoleOpen(res)) {
            serveWebAsset("index.html", res);
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
        if (auto providers = doc.find("providers");
            providers != doc.end() && providers->is_array()) {
            for (auto &entry : *providers) {
                if (!entry.is_object()) {
                    continue;
                }
                auto key = entry.find("api_key");
                if (key == entry.end() || !key->is_string()) {
                    continue;
                }
                const std::string raw = key->get<std::string>();
                if (raw.empty()) {
                    continue;
                }
                if (isSecretReference(raw)) {
                    entry["api_key_source"] = "env";
                    continue;
                }
                entry["api_key"] = "";
                entry["api_key_source"] = "literal";
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
        res.set_content(root.dump(2), "application/json");
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
        auto parsed = appConfigFromJson(doc.dump());
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

        const ValidationReport report = validate(*parsed);
        if (!report.ok()) {
            // Nothing is written and nothing takes effect: a half-applied bad
            // config would be the worst of both.
            json refused = validationJson(report);
            refused["ok"] = false;
            res.status = 422;
            res.set_content(refused.dump(2), "application/json");
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
        res.set_content(root.dump(2), "application/json");
        impl_->recordSystem(saved ? "config written from the console"
                                  : "config updated in memory (no config path)",
                            report.count(ValidationIssue::Level::Warning) > 0 ? "warn" : "info");
    });

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

        json root = validationJson(report);
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

    impl_->recordSystem(std::format("listening on http://{}:{}", host, bound));
    // Started last: everything above can still fail and return, and a flusher
    // is only wanted once there is a server whose telemetry it can write.
    impl_->startFlusher();
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
        // Nothing was ever started, so there is nothing to write; this is also
        // the branch the destructor takes for a console that never pressed
        // Start.
        impl_->joinFlusher();
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
    // A live edit decides whether the next flush writes anything, and marking
    // it dirty is what makes switching the flag on take effect now rather than
    // at the next request.
    impl_->setPersistence(config.server.persist_telemetry);
    impl_->markStateDirty();
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
            if (item.is_object()) {
                s.providers.push_back(providerStatFromJson(item));
            }
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
