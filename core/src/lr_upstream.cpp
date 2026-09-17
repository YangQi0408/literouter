// The single-hop upstream call. Nothing here knows about routes or failover —
// it takes one relay and one request and reports what came back, which is what
// makes it reusable for both the proxy's per-attempt work and the console's
// "Test" button.
module;

#include <httplib.h>

module literouter.core;

import std;
import nlohmann.json;

namespace literouter {

namespace {

namespace h = httplib;
using json = nlohmann::json;

bool ciEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

// Ordered, case-insensitively de-duplicated header list. A relay's own
// `headers` map must be able to override the defaults (some relays want a
// custom auth header instead of a bearer), and httplib's `Headers` is a
// multimap that would happily send both.
class HeaderSet {
public:
    void put(std::string key, std::string value) {
        for (auto &entry : entries_) {
            if (ciEqual(entry.first, key)) {
                entry.second = std::move(value);
                return;
            }
        }
        entries_.emplace_back(std::move(key), std::move(value));
    }

    void putIfAbsent(std::string key, std::string value) {
        for (const auto &entry : entries_) {
            if (ciEqual(entry.first, key)) {
                return;
            }
        }
        entries_.emplace_back(std::move(key), std::move(value));
    }

    bool has(std::string_view key) const {
        return std::ranges::any_of(entries_, [key](const auto &entry) {
            return ciEqual(entry.first, key);
        });
    }

    h::Headers toHttplib() const {
        h::Headers out;
        for (const auto &[key, value] : entries_) {
            out.emplace(key, value);
        }
        return out;
    }

    std::map<std::string, std::string, std::less<>> toMap() const {
        std::map<std::string, std::string, std::less<>> out;
        for (const auto &[key, value] : entries_) {
            out.emplace(toLower(key), value);
        }
        return out;
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

HeaderSet buildHeaders(const ProviderConfig &provider, std::string_view accept) {
    HeaderSet headers;
    headers.put("Content-Type", "application/json");
    headers.put("Accept", std::string{accept});
    headers.put("User-Agent", std::string{kUserAgent});

    const std::string key = resolveSecret(provider.api_key);
    if (!key.empty()) {
        if (ciEqual(provider.protocol, "anthropic")) {
            headers.put("x-api-key", key);
            headers.put("anthropic-version", "2023-06-01");
        } else if (ciEqual(provider.protocol, "gemini")) {
            headers.put("x-goog-api-key", key);
        } else {
            headers.put("Authorization", "Bearer " + key);
        }
    }
    // Relays with a non-bearer scheme express it here; `put` means their
    // Authorization replaces the default rather than joining it.
    for (const auto &[name, value] : provider.headers) {
        if (!name.empty()) {
            headers.put(name, value);
        }
    }
    return headers;
}

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
    case h::Error::UnsupportedMultipartBoundaryChars: return "unsupported multipart boundary";
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

// `data[].id` is the OpenAI shape. A relay that answers with a bare array, or
// with `models[].name`, is still read — the point of a probe is discovery, not
// strictness.
std::vector<std::string> parseModelIds(std::string_view body) {
    std::vector<std::string> out;
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) {
        return out;
    }

    const json *list = nullptr;
    if (root.is_array()) {
        list = &root;
    } else if (root.is_object()) {
        for (const char *key : {"data", "models"}) {
            if (const auto it = root.find(key); it != root.end() && it->is_array()) {
                list = &*it;
                break;
            }
        }
    }
    if (list == nullptr) {
        return out;
    }

    for (const auto &item : *list) {
        if (item.is_string()) {
            std::string id = item.get<std::string>();
            if (startsWith(id, "models/")) {
                id = id.substr(7);
            }
            out.push_back(std::move(id));
            continue;
        }
        if (!item.is_object()) {
            continue;
        }
        for (const char *key : {"id", "name", "model"}) {
            if (const auto it = item.find(key); it != item.end() && it->is_string()) {
                std::string id = it->get<std::string>();
                if (startsWith(id, "models/")) {
                    id = id.substr(7);
                }
                out.push_back(std::move(id));
                break;
            }
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

void configure(h::Client &client, const ProviderConfig &provider) {
    client.set_follow_location(true);
    // Blocking mode: the proxy's own worker thread is the concurrency unit, so
    // httplib should not peel off a second one per request.
    client.set_connection_timeout(provider.connect_timeout_sec, 0);
    client.set_read_timeout(provider.timeout_sec, 0);
    client.set_write_timeout(provider.timeout_sec, 0);
    client.set_keep_alive(true);
    // An enterprise relay behind a private CA is the one case where the trust
    // store is a decision rather than a default; LITEROUTER_CA_BUNDLE covers it,
    // and resolveCaBundle() covers the far more common case of mcpp's
    // source-built OpenSSL not knowing where this machine keeps its roots.
    if (const auto bundle = resolveCaBundle(); !bundle.empty()) {
        client.set_ca_cert_path(bundle.string());
        client.enable_server_certificate_verification(true);
    }
}

// ── upstream connection reuse ────────────────────────────────────────────────
//
// Dialing a fresh TLS connection for every chat request costs a full handshake
// in front of a call that may take a second — real money on a proxy whose whole
// job is forwarding HTTP. Reusing the socket is the obvious fix, but httplib's
// blocking Client owns exactly ONE socket and asserts that concurrent requests
// on it come from the same thread, so a Client cannot be shared between server
// workers.
//
// A per-thread pool is what that constraint leaves: each worker thread keeps its
// own warm connection per relay, used sequentially, with no lock anywhere.
// Keyed by everything that changes what the socket actually is — the relay root,
// the two timeouts, and the CA bundle — so editing a relay's timeout or exporting
// LITEROUTER_CA_BUNDLE does not silently keep using a socket configured
// differently.
//
// Both legs draw from it. The buffered one has always done so; the streamed one
// used to dial its own connection per attempt, which is to say the leg that
// carries nearly all the traffic was the one paying the handshake. Reuse is safe
// for it because httplib probes a socket before reusing it (`is_socket_alive`,
// plus a TLS peer-closed check) and reconnects non-gracefully when the relay
// dropped the idle connection — so a pooled socket that has gone stale costs a
// reconnect, not a failed request.
thread_local std::map<std::string, std::shared_ptr<h::Client>, std::less<>> g_connection_pool;

constexpr std::size_t kMaxPooledConnectionsPerThread = 8;

// Cache the CA bundle path to avoid stat()ing up to 9 filesystem paths on
// every upstream request. Resolved once per process; if the user changes
// LITEROUTER_CA_BUNDLE they restart the proxy anyway.
const std::string& cachedCaBundlePath() {
    static const std::string path = resolveCaBundle().string();
    return path;
}

std::string poolKey(std::string_view root, const ProviderConfig &provider) {
    return std::format("{}|{}|{}|{}", root, provider.connect_timeout_sec, provider.timeout_sec,
                       cachedCaBundlePath());
}

// A strong reference to this thread's client for `key`, dialing one if the pool
// has none. Returned by value so a caller that outlives the next pool mutation
// (the streamed leg holds it for the length of a response) keeps it alive.
std::shared_ptr<h::Client> acquireClientHandle(std::string_view root,
                                               const ProviderConfig &provider,
                                               const std::string &key) {
    if (auto it = g_connection_pool.find(key); it != g_connection_pool.end()) {
        return it->second;
    }
    // A config being edited repeatedly would otherwise leave one dead entry per
    // distinct timeout value for the life of the thread.
    if (g_connection_pool.size() >= kMaxPooledConnectionsPerThread) {
        g_connection_pool.clear();
    }
    auto client = std::make_shared<h::Client>(std::string{root});
    configure(*client, provider);
    return g_connection_pool.emplace(key, std::move(client)).first->second;
}

h::Client &acquireClient(std::string_view root, const ProviderConfig &provider,
                         const std::string &key) {
    // The pool entry is what keeps this reference valid: nothing between here
    // and the request that follows mutates the pool.
    return *acquireClientHandle(root, provider, key);
}

void evictClient(const std::string &key) {
    g_connection_pool.erase(key);
}

// A half-open keep-alive socket surfaces as `Error::Connection` (the stale
// socket failed its liveness check and the reconnect did not happen). Retrying
// once on a FRESH connection is free of duplicate-side-effect risk precisely
// because that error means no TCP connection existed to carry the request. Every
// other transport error is left alone — a `Write` or `Read` failure may well
// mean the upstream already processed the call, and re-sending an LLM request is
// the caller's money.

} // namespace

UpstreamResult upstreamPost(const ProviderConfig &provider, std::string_view path,
                            std::string_view body, int timeout_sec_override) {
    UpstreamResult out;
    const double started = nowUnix();

    std::string root;
    std::string prefix;
    std::string scheme;
    if (!splitBaseUrl(provider.base_url, root, prefix, scheme)) {
        out.error = std::format("invalid base_url `{}`", provider.base_url);
        return out;
    }

    // A per-call override (the console's Test button) must not poison a pooled
    // connection configured for the relay's normal timeout, so it bypasses the
    // pool entirely.
    const bool pooled = timeout_sec_override <= 0;
    const std::string key = poolKey(root, provider);
    const auto headers = buildHeaders(provider, "application/json").toHttplib();
    const std::string target = joinPath(prefix, path);

    const auto attempt = [&](h::Client &client) {
        return client.Post(target, headers, std::string{body}, "application/json");
    };

    h::Result result;
    if (pooled) {
        result = attempt(acquireClient(root, provider, key));
        if (!result && result.error() == h::Error::Connection) {
            // Stale pooled socket: drop it and dial once more. See the note on
            // evictClient().
            evictClient(key);
            result = attempt(acquireClient(root, provider, key));
        }
    } else {
        h::Client client{root};
        configure(client, provider);
        client.set_read_timeout(timeout_sec_override, 0);
        client.set_write_timeout(timeout_sec_override, 0);
        result = attempt(client);
    }

    out.latency_ms = (nowUnix() - started) * 1000.0;
    if (!result) {
        out.ok = false;
        out.error = errorText(result.error());
        if (pooled) {
            // Any transport failure retires the socket; the next request dials
            // clean rather than inheriting whatever went wrong.
            evictClient(key);
        }
        return out;
    }

    out.ok = true;
    out.status = result->status;
    out.body = result->body;
    for (const auto &[name, value] : result->headers) {
        out.headers.emplace(toLower(name), value);
    }
    return out;
}

UpstreamConnection checkoutUpstreamConnection(std::string_view root,
                                              const ProviderConfig &provider) {
    return acquireClientHandle(root, provider, poolKey(root, provider));
}

void retireUpstreamConnection(std::string_view root, const ProviderConfig &provider) {
    evictClient(poolKey(root, provider));
}

ProviderProbe probeProvider(const ProviderConfig &provider, int timeout_sec) {
    ProviderProbe probe;
    const double started = nowUnix();

    std::string root;
    std::string prefix;
    std::string scheme;
    if (!splitBaseUrl(provider.base_url, root, prefix, scheme)) {
        probe.detail = std::format("invalid base_url `{}`", provider.base_url);
        return probe;
    }

    h::Client client{root};
    configure(client, provider);
    client.set_keep_alive(false); // a one-shot probe should not leave a socket behind
    client.set_read_timeout(timeout_sec, 0);
    client.set_write_timeout(timeout_sec, 0);

    const std::string models_path = resolveModelsPath(provider);
    auto result = client.Get(joinPath(prefix, models_path),
                             buildHeaders(provider, "application/json").toHttplib());

    probe.latency_ms = (nowUnix() - started) * 1000.0;
    if (!result) {
        probe.detail = errorText(result.error());
        return probe;
    }

    probe.reachable = true;
    probe.status = result->status;

    if (result->status >= 200 && result->status < 300) {
        // `data[].id` is the OpenAI shape; a relay that answers with a bare
        // array or a `models[].name` still gets read, because the point here
        // is discovery, not strictness.
        if (auto parsed = parseModelIds(result->body); !parsed.empty()) {
            probe.models = std::move(parsed);
            probe.detail = std::format("{} models advertised", probe.models.size());
        } else {
            probe.detail = "reachable, but no model list in the reply";
        }
    } else if (result->status == 401 || result->status == 403) {
        probe.detail = "reachable, but the key was rejected";
    } else {
        probe.detail = truncateUtf8(trim(result->body), 200);
        if (probe.detail.empty()) {
            probe.detail = std::format("HTTP {}", result->status);
        }
    }
    return probe;
}

} // namespace literouter
