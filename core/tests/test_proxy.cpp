// End-to-end proxy tests: a real ProxyServer on 127.0.0.1 talking to real stub
// relays that can be programmed to answer, rate-limit, error, stream or hang up.
//
// Nothing here sleeps-and-hopes: every wait is a poll with a deadline, every
// client has a timeout, and every server this test starts is stopped and joined
// before main returns.
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "lr_test_check.h"

import nlohmann.json;
import literouter.core;

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using json = nlohmann::json;

constexpr const char *kRouteModel = "route-model";
constexpr const char *kCloserModel = "closer-model";
constexpr const char *kPassModel = "pass-model";

// A scratch directory for the telemetry file these tests make a real server
// write. Without it a run would leave the developer's own
// ~/.local/state/literouter/telemetry-<port>.json rewritten by the fixture.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                std::format("literouter-test-{}", literouter::hexId(6));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool writeFile(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return static_cast<bool>(output);
}

// ── stub relay ───────────────────────────────────────────────────────────────

// Behaves like an OpenAI-compatible relay whose mood is set per assertion group.
class StubRelay {
public:
    enum class Mode { Normal, RateLimit, ServerError, BadRequest, EmptyError, Stream };

    StubRelay(std::string path_prefix, std::string normal_body)
        : path_prefix_(std::move(path_prefix)), normal_body_(std::move(normal_body)) {}

    ~StubRelay() { stop(); }

    StubRelay(const StubRelay &) = delete;
    StubRelay &operator=(const StubRelay &) = delete;

    bool start() {
        // Each stub answers under its own prefix, so a request that leaks to
        // the wrong relay is visible in the counters *and* in the path it was
        // asked for.
        server_.Post(path_prefix_ + "/chat/completions",
                     [this](const httplib::Request &req, httplib::Response &res) {
                         serveChat(req, res);
                     });
        server_.Post(path_prefix_ + "/v1/messages",
                     [this](const httplib::Request &req, httplib::Response &res) {
                         serveChat(req, res);
                     });
        server_.Post(R"()" + path_prefix_ + R"(/v1beta/models/(.*?):generateContent)",
                     [this](const httplib::Request &req, httplib::Response &res) {
                         serveChat(req, res);
                     });
        server_.Get(path_prefix_ + "/models", [this](const httplib::Request &, httplib::Response &res) {
            ++model_requests_;
            res.status = 200;
            res.set_content(R"({"object":"list","data":[{"id":"stub-model","object":"model"}]})",
                            "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            return false;
        }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            // Still join: stop() is called from the destructor too, and the
            // first caller may only have flagged the server.
        }
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void setMode(Mode mode) { mode_.store(mode); }
    void setRetryAfter(std::optional<int> seconds) { retry_after_ = seconds; }
    void resetCounters() {
        chat_requests_.store(0);
        model_requests_.store(0);
    }
    int chatRequests() const { return chat_requests_.load(); }
    int modelRequests() const { return model_requests_.load(); }

    std::string baseUrl() const {
        return std::format("http://127.0.0.1:{}{}", port_, path_prefix_);
    }
    const std::string &normalBody() const { return normal_body_; }
    const std::string &errorBody() const { return error_body_; }
    const std::string &badRequestBody() const { return bad_request_body_; }
    const std::string &serverErrorBody() const { return server_error_body_; }

    // The SSE transcript this relay streams, chunk by chunk.
    std::vector<std::string> sseChunks() const {
        return {"data: {\"id\":\"chunk-1\",\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n",
                "data: {\"id\":\"chunk-2\",\"choices\":[{\"delta\":{\"content\":\"lo \"}}]}\n\n",
                "data: {\"id\":\"chunk-3\",\"choices\":[{\"delta\":{\"content\":\"there\"}}]}\n\n",
                // What OpenAI sends when the client asked for
                // stream_options.include_usage. It is relayed untouched, and it
                // is the only place a streamed answer states its token counts.
                "data: {\"id\":\"chunk-4\",\"choices\":[],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7}}\n\n",
                "data: [DONE]\n\n"};
    }

    std::string lastChatBody() const {
        std::scoped_lock lock{body_mutex_};
        return last_chat_body_;
    }

    std::string lastChatPath() const {
        std::scoped_lock lock{body_mutex_};
        return last_chat_path_;
    }

private:
    void serveChat(const httplib::Request &req, httplib::Response &res) {
        ++chat_requests_;
        {
            std::scoped_lock lock{body_mutex_};
            last_chat_body_ = req.body;
            last_chat_path_ = req.path;
        }
        switch (mode_.load()) {
        case Mode::Normal:
            res.status = 200;
            res.set_content(normal_body_, "application/json");
            return;
        case Mode::RateLimit:
            res.status = 429;
            // Only when the test asked for one: the other groups assert what a
            // 429 does without the relay naming a window.
            if (retry_after_) {
                res.set_header("Retry-After", std::to_string(*retry_after_));
            }
            res.set_content(error_body_, "application/json");
            return;
        case Mode::ServerError:
            res.status = 500;
            res.set_content(server_error_body_, "application/json");
            return;
        case Mode::EmptyError:
            // A bare status with no body at all: the relay's worst manners.
            res.status = 500;
            return;
        case Mode::BadRequest:
            res.status = 400;
            res.set_content(bad_request_body_, "application/json");
            return;
        case Mode::Stream:
            serveStream(res);
            return;
        }
    }

    void serveStream(httplib::Response &res) {
        const auto chunks = std::make_shared<std::vector<std::string>>(sseChunks());
        res.set_chunked_content_provider(
            "text/event-stream", [chunks](std::size_t, httplib::DataSink &sink) {
                for (const auto &chunk : *chunks) {
                    if (!sink.write(chunk.data(), chunk.size())) {
                        return false;
                    }
                    // A real relay emits tokens over time; the delay also makes
                    // the bridge's queueing observable instead of a single
                    // memcpy that would pass even if the pump were bypassed.
                    std::this_thread::sleep_for(5ms);
                }
                sink.done();
                return true;
            });
    }

    std::string path_prefix_;
    std::string normal_body_;
    const std::string error_body_ =
        R"({"error":{"message":"rate limit reached for gpt-4o","type":"rate_limit_error","code":"rate_limit_exceeded"}})";
    const std::string bad_request_body_ =
        R"({"error":{"message":"context length exceeded: 200000 > 128000","type":"invalid_request_error","code":"context_length_exceeded"}})";
    const std::string server_error_body_ =
        R"({"error":{"message":"upstream exploded","type":"server_error","code":"internal_error"}})";
    httplib::Server server_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
    std::atomic<Mode> mode_{Mode::Normal};
    std::optional<int> retry_after_;
    std::atomic<int> chat_requests_{0};
    std::atomic<int> model_requests_{0};
    int port_ = 0;
    mutable std::mutex body_mutex_;
    std::string last_chat_body_;
    std::string last_chat_path_;
};

#ifndef _WIN32
// Accepts the connection and hangs up without sending a byte. This is the one
// upstream failure nothing built on httplib::Server can reproduce, because that
// server always writes a response, and it is the failure the proxy's
// "no headers yet, so fail over" gate exists for.
class HangingRelay {
public:
    ~HangingRelay() { stop(); }
    HangingRelay(const HangingRelay &) = delete;
    HangingRelay &operator=(const HangingRelay &) = delete;

    HangingRelay() = default;

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            return false;
        }
        port_ = ntohs(address.sin_port);
        // Non-blocking accept: the loop must never be parked in accept() when
        // stop() wants it gone.
        ::fcntl(listen_fd_, F_SETFL, ::fcntl(listen_fd_, F_GETFL, 0) | O_NONBLOCK);
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    int connections() const { return connections_.load(); }
    std::string baseUrl() const { return std::format("http://127.0.0.1:{}", port_); }

private:
    void loop() {
        while (!stopped_.load()) {
            pollfd descriptor{listen_fd_, POLLIN, 0};
            if (::poll(&descriptor, 1, 25) <= 0) {
                continue;
            }
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            connections_.fetch_add(1);
            timeval timeout{};
            timeout.tv_sec = 2;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char buffer[2048];
            // Let the request arrive first, so the client fails on the read
            // rather than on a half-written request.
            ::recv(client, buffer, sizeof(buffer), 0);
            // A hard close (RST) rather than a FIN: an aborted connection is
            // unambiguous, whereas an empty 200 would look like an answer.
            linger reset{1, 0};
            ::setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
            ::close(client);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopped_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
};
#endif

// ── stderr capture ───────────────────────────────────────────────────────────

#ifdef _WIN32
// Not implemented on Windows; text() is always empty and the assertion below
// becomes a no-op there rather than a platform-specific branch in the group.
class StderrCapture {
public:
    std::string text() { return {}; }
};
#else
// Captures anything the process writes to fd 2 inside a scope. The bug this
// pins announced itself as a libc++abi line on stderr immediately before it
// killed the process, so "stderr stayed empty" is the canary for it coming
// back.
class StderrCapture {
public:
    StderrCapture() {
        saved_ = ::dup(2);
        file_ = ::tmpfile();
        if (file_ != nullptr && saved_ >= 0) {
            std::fflush(stderr);
            active_ = ::dup2(::fileno(file_), 2) >= 0;
        }
    }
    ~StderrCapture() {
        if (active_) {
            std::fflush(stderr);
            ::dup2(saved_, 2);
        }
        if (saved_ >= 0) {
            ::close(saved_);
        }
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }
    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;

    std::string text() {
        if (!active_) {
            return {};
        }
        std::fflush(stderr);
        std::string out;
        if (std::fseek(file_, 0, SEEK_SET) != 0) {
            return out;
        }
        char buffer[512];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) {
            out.append(buffer, read);
        }
        return out;
    }

private:
    int saved_ = -1;
    FILE *file_ = nullptr;
    bool active_ = false;
};
#endif

// ── proxy fixture ────────────────────────────────────────────────────────────

literouter::AppConfig buildProxyConfig(const std::string &alpha, const std::string &beta,
                                       const std::string &hanger) {
    using literouter::AppConfig;
    using literouter::ProviderConfig;
    using literouter::RouteConfig;
    using literouter::RouteTarget;

    AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0; // the kernel picks one
    // Off, so an unrouted model is a 404 in this fixture; the pass-through
    // ranking itself is covered by test_router.
    config.server.pass_through_unknown = false;
    config.server.circuit_failure_threshold = 3;
    config.server.circuit_cooldown_sec = 30;
    config.server.log_capacity = 400;

    ProviderConfig first;
    first.id = "alpha";
    first.name = "Alpha stub relay";
    first.base_url = alpha;
    first.priority = 10;
    // One literal key and one reference, so the config endpoint's redaction can
    // be asserted instead of described.
    first.api_key = "sk-literal-in-file";
    first.models = {kPassModel};

    ProviderConfig second;
    second.id = "beta";
    second.name = "Beta stub relay";
    second.base_url = beta;
    second.api_key = "${LR_TEST_KEY}";
    second.priority = 20;
    second.models = {kPassModel};

    ProviderConfig hanging;
    hanging.id = "hanger";
    hanging.name = "Hangs up";
    hanging.base_url = hanger;
    hanging.priority = 30;
    hanging.timeout_sec = 5;
    hanging.connect_timeout_sec = 2;

    ProviderConfig anthropic_p;
    anthropic_p.id = "anthropic-stub";
    anthropic_p.name = "Anthropic stub relay";
    anthropic_p.protocol = "anthropic";
    anthropic_p.base_url = alpha;
    anthropic_p.priority = 40;

    ProviderConfig gemini_p;
    gemini_p.id = "gemini-stub";
    gemini_p.name = "Gemini stub relay";
    gemini_p.protocol = "gemini";
    gemini_p.base_url = alpha;
    gemini_p.priority = 50;

    config.providers = {first, second, hanging, anthropic_p, gemini_p};

    RouteConfig routed;
    routed.model = kRouteModel;
    routed.targets = {RouteTarget{.provider = "alpha", .model = {}},
                      RouteTarget{.provider = "beta", .model = {}}};

    RouteConfig closers;
    closers.model = kCloserModel;
    closers.targets = {RouteTarget{.provider = "hanger", .model = {}},
                       RouteTarget{.provider = "beta", .model = {}}};

    RouteConfig routed_claude;
    routed_claude.model = "claude-routed";
    routed_claude.targets = {RouteTarget{.provider = "anthropic-stub", .model = {}}};

    RouteConfig routed_gemini;
    routed_gemini.model = "gemini-routed";
    routed_gemini.targets = {RouteTarget{.provider = "gemini-stub", .model = {}}};

    config.routes = {routed, closers, routed_claude, routed_gemini};
    return config;
}

struct Hit {
    bool transport_ok = false;
    int status = 0;
    std::string body;
    std::string content_type;
    std::string acao; // Access-Control-Allow-Origin, empty when absent
};

Hit postJson(int port, const std::string &path, const std::string &body,
             const std::string &key = {}) {
    httplib::Client client{"127.0.0.1", port};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    httplib::Headers headers{{"Content-Type", "application/json"}};
    if (!key.empty()) {
        headers.emplace("Authorization", "Bearer " + key);
    }
    auto result = client.Post(path, headers, body, "application/json");
    Hit hit;
    if (!result) {
        return hit;
    }
    hit.transport_ok = true;
    hit.status = result->status;
    hit.body = result->body;
    hit.content_type = result->get_header_value("Content-Type");
    hit.acao = result->get_header_value("Access-Control-Allow-Origin");
    return hit;
}

Hit putJson(int port, const std::string &path, const std::string &body,
            const std::string &key = {}) {
    httplib::Client client{"127.0.0.1", port};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    httplib::Headers headers{{"Content-Type", "application/json"}};
    if (!key.empty()) {
        headers.emplace("Authorization", "Bearer " + key);
    }
    auto result = client.Put(path, headers, body, "application/json");
    Hit hit;
    if (!result) {
        return hit;
    }
    hit.transport_ok = true;
    hit.status = result->status;
    hit.body = result->body;
    hit.content_type = result->get_header_value("Content-Type");
    hit.acao = result->get_header_value("Access-Control-Allow-Origin");
    return hit;
}

Hit getPath(int port, const std::string &path, const std::string &key = {}) {
    httplib::Client client{"127.0.0.1", port};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
    if (!key.empty()) {
        headers.emplace("Authorization", "Bearer " + key);
    }
    auto result = client.Get(path, headers);
    Hit hit;
    if (!result) {
        return hit;
    }
    hit.transport_ok = true;
    hit.status = result->status;
    hit.body = result->body;
    hit.content_type = result->get_header_value("Content-Type");
    hit.acao = result->get_header_value("Access-Control-Allow-Origin");
    return hit;
}

std::string chatRequest(std::string_view model, bool stream = false) {
    if (stream) {
        return std::format(R"({{"model":"{}","stream":true,"messages":[{{"role":"user","content":"hi"}}]}})",
                           model);
    }
    return std::format(R"({{"model":"{}","messages":[{{"role":"user","content":"hi"}}]}})", model);
}

// Polls /health instead of sleeping: start() already waited for the listener,
// this only proves the socket really answers before the first assertion.
bool waitForHealth(int port, const std::string &key = {}) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const Hit hit = getPath(port, "/health", key);
        if (hit.transport_ok && hit.status == 200) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

const literouter::ProviderStat *statOf(const literouter::Snapshot &snapshot,
                                       std::string_view provider) {
    for (const auto &stat : snapshot.providers) {
        if (stat.provider == provider) {
            return &stat;
        }
    }
    return nullptr;
}

const literouter::ProviderHealth *healthOf(const literouter::Snapshot &snapshot,
                                           std::string_view provider) {
    for (const auto &health : snapshot.health) {
        if (health.provider == provider) {
            return &health;
        }
    }
    return nullptr;
}

std::string join(const std::vector<std::string> &parts) {
    std::string out;
    for (const auto &part : parts) {
        out += part;
    }
    return out;
}

// ── the assertions, one group each ───────────────────────────────────────────
//
// 1..10 are the ten the task lists; 4b/6b/11/12 are the failover and lifecycle
// paths a stub that only ever answers 200 cannot reach.

void group1PlainRequest(StubRelay &relay_a, StubRelay &relay_b, int port) {
    LR_GROUP("1. a plain request is served by the first relay, byte-identically");
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const std::string request = chatRequest(kRouteModel);
    const Hit hit = postJson(port, "/v1/chat/completions", request);
    LR_CHECK_MSG(hit.transport_ok, "the proxy did not answer at all");
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(hit.body == relay_a.normalBody(), "the first relay's body was altered");
    LR_CHECK_EQ(static_cast<long long>(hit.body.size()),
                static_cast<long long>(relay_a.normalBody().size()));
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_MSG(relay_b.chatRequests() == 0, "the second relay was called for a healthy first");
    // The client's body is forwarded upstream unchanged.
    LR_CHECK_EQ(relay_a.lastChatBody(), request);
    LR_CHECK(hit.content_type.find("application/json") != std::string::npos);
}

void group2RateLimited(StubRelay &relay_a, StubRelay &relay_b, literouter::ProxyServer &proxy) {
    LR_GROUP("2. a 429 on the first relay fails over to the second");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::RateLimit);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(hit.body == relay_b.normalBody(), "the answer came from the wrong relay");
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);

    const literouter::Snapshot snapshot = proxy.snapshot();
    const auto *alpha = healthOf(snapshot, "alpha");
    LR_CHECK(alpha != nullptr);
    if (alpha != nullptr) {
        LR_CHECK_MSG(alpha->state == literouter::ProviderHealth::State::Degraded,
                     "one failure should leave the relay degraded, not open");
        LR_CHECK_EQ(alpha->consecutive_failures, 1);
        LR_CHECK_EQ(alpha->stateName(), "degraded");
    }
    const auto *beta = statOf(snapshot, "beta");
    LR_CHECK(beta != nullptr);
    if (beta != nullptr) {
        LR_CHECK_EQ(beta->requests, static_cast<std::uint64_t>(1));
        LR_CHECK_EQ(beta->retries_in, static_cast<std::uint64_t>(1));
        LR_CHECK_EQ(beta->successes, static_cast<std::uint64_t>(1));
    }
    const auto *alphaStat = statOf(snapshot, "alpha");
    LR_CHECK(alphaStat != nullptr && alphaStat->failures == 1);
}

void group3BadRequest(StubRelay &relay_a, StubRelay &relay_b, literouter::ProxyServer &proxy) {
    LR_GROUP("3. a 400 is passed through verbatim and is not retried");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::BadRequest);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 400);
    LR_CHECK_MSG(hit.body == relay_a.badRequestBody(),
                 "the relay's own error text must reach the caller unedited");
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_MSG(relay_b.chatRequests() == 0, "a 400 must not be retried on the next relay");

    const literouter::Snapshot snapshot = proxy.snapshot();
    const auto *alpha = statOf(snapshot, "alpha");
    LR_CHECK(alpha != nullptr);
    if (alpha != nullptr) {
        // The request failed, but the relay behaved: three malformed calls must
        // not trip the breaker on a healthy upstream.
        LR_CHECK_EQ(alpha->requests, static_cast<std::uint64_t>(1));
        LR_CHECK_EQ(alpha->successes, static_cast<std::uint64_t>(1));
        LR_CHECK_EQ(alpha->failures, static_cast<std::uint64_t>(0));
    }
    const auto *alphaHealth = healthOf(snapshot, "alpha");
    LR_CHECK(alphaHealth != nullptr &&
             alphaHealth->state == literouter::ProviderHealth::State::Healthy);
    // The caller's request still counts as a failure at the top level.
    LR_CHECK_EQ(snapshot.total_failure, static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(snapshot.total_success, static_cast<std::uint64_t>(0));
}

void group4Streaming(StubRelay &relay_a, StubRelay &relay_b, literouter::ProxyServer &proxy) {
    LR_GROUP("4. a streamed answer is relayed chunk by chunk");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::Stream);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true));
    LR_CHECK_MSG(hit.transport_ok, "the streamed request got no response at all");
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK(hit.content_type.find("text/event-stream") != std::string::npos);

    const std::string expected = join(relay_a.sseChunks());
    LR_CHECK_MSG(hit.body == expected, "the relayed stream is not the relay's own transcript");

    // In order, and terminated: the ordering is checked against the positions
    // so a body that contains the chunks in the wrong order still fails.
    std::size_t cursor = 0;
    bool ordered = true;
    for (const auto &chunk : relay_a.sseChunks()) {
        const auto at = hit.body.find(chunk, cursor);
        if (at == std::string::npos) {
            ordered = false;
            break;
        }
        cursor = at + chunk.size();
    }
    LR_CHECK_MSG(ordered, "the SSE chunks arrived out of order or were dropped");
    LR_CHECK(literouter::endsWith(hit.body, "data: [DONE]\n\n"));
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK(relay_a.lastChatBody().find("\"stream\":true") != std::string::npos);

    // The counts a streamed answer reports in its final chunk are accounted for
    // even though the chunk itself is passed through untouched — the body above
    // is byte-identical to the relay's own transcript, and these two numbers
    // come out of it.
    const literouter::Snapshot streamed = proxy.snapshot();
    LR_CHECK_EQ(streamed.tokens_prompt, static_cast<std::uint64_t>(11));
    LR_CHECK_EQ(streamed.tokens_completion, static_cast<std::uint64_t>(7));

    LR_GROUP("4b. a streamed request fails over when the first relay 429s");
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::RateLimit);
    relay_b.setMode(StubRelay::Mode::Stream);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const Hit failedOver = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true));
    LR_CHECK_EQ(failedOver.status, 200);
    LR_CHECK_EQ(failedOver.body, join(relay_b.sseChunks()));
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const literouter::Snapshot snapshot = proxy.snapshot();
    const auto *beta = statOf(snapshot, "beta");
    LR_CHECK(beta != nullptr && beta->retries_in == 1);
    const auto *alphaHealth = healthOf(snapshot, "alpha");
    LR_CHECK(alphaHealth != nullptr &&
             alphaHealth->state == literouter::ProviderHealth::State::Degraded);
}

void group5ModelList(StubRelay &relay_a, literouter::ProxyServer &proxy) {
    LR_GROUP("5. GET /v1/models lists exactly logicalModels()");
    const int port = proxy.boundPort();
    const Hit hit = getPath(port, "/v1/models");
    LR_CHECK_EQ(hit.status, 200);
    const json parsed = json::parse(hit.body, nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "the model list is not JSON");
    if (parsed.is_discarded()) {
        return;
    }
    std::vector<std::string> listed;
    for (const auto &item : parsed.at("data")) {
        listed.push_back(item.at("id").get<std::string>());
        LR_CHECK_EQ(item.at("object").get<std::string>(), "model");
    }
    const std::vector<std::string> expected = proxy.config().logicalModels();
    LR_CHECK(listed == expected);
    LR_CHECK(std::ranges::find(listed, kRouteModel) != listed.end());
    LR_CHECK(std::ranges::find(listed, kCloserModel) != listed.end());
    LR_CHECK(std::ranges::find(listed, kPassModel) == listed.end()); // Unrouted models are excluded

    // /models without /v1 prefix also works and returns identical data
    const Hit hitNoV1 = getPath(port, "/models");
    LR_CHECK_EQ(hitNoV1.status, 200);
    LR_CHECK_EQ(hitNoV1.body, hit.body);
    // The route's hops are exposed for the console, in author order.
    for (const auto &item : parsed.at("data")) {
        if (item.at("id").get<std::string>() == kRouteModel) {
            LR_CHECK_EQ(item.at("literouter").at("kind").get<std::string>(), "route");
            LR_CHECK_EQ(item.at("literouter").at("targets").at(0).get<std::string>(), "alpha");
            LR_CHECK_EQ(item.at("literouter").at("targets").at(1).get<std::string>(), "beta");
        }
    }
    LR_CHECK_EQ(relay_a.modelRequests(), 0); // /v1/models is answered locally
}

void group6UnknownModel(StubRelay &relay_a, literouter::ProxyServer &proxy,
                        const literouter::AppConfig &config) {
    LR_GROUP("6. an unrouted model with pass_through_unknown = false is a 404");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kPassModel));
    LR_CHECK_EQ(hit.status, 404);
    const json parsed = json::parse(hit.body, nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "the 404 body is not JSON");
    if (parsed.is_discarded()) {
        return;
    }
    LR_CHECK(parsed.contains("error"));
    const json &error = parsed.at("error");
    LR_CHECK(error.contains("message") && error.at("message").is_string());
    LR_CHECK_MSG(error.at("message").get<std::string>().find(kPassModel) != std::string::npos,
                 "the message should name the model that could not be served");
    LR_CHECK_EQ(error.at("type").get<std::string>(), "invalid_request_error");
    LR_CHECK_EQ(error.at("code").get<std::string>(), "model_not_found");
    LR_CHECK_MSG(relay_a.chatRequests() == 0, "a 404 must not have reached a relay");

    LR_GROUP("6b. the same request succeeds once pass-through is on");
    literouter::AppConfig withPassThrough = config;
    withPassThrough.server.pass_through_unknown = true;
    proxy.updateConfig(withPassThrough);
    relay_a.resetCounters();
    const Hit served = postJson(port, "/v1/chat/completions", chatRequest(kPassModel));
    LR_CHECK_EQ(served.status, 200);
    LR_CHECK_EQ(served.body, relay_a.normalBody());
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    proxy.updateConfig(config);

    // Back to the fixture: the flag is off again.
    const Hit refused = postJson(port, "/v1/chat/completions", chatRequest(kPassModel));
    LR_CHECK_EQ(refused.status, 404);
}

void group7Auth(StubRelay &relay_a, literouter::ProxyServer &proxy,
                const literouter::AppConfig &config) {
    LR_GROUP("7. server.api_key is enforced");
    const int port = proxy.boundPort();
    literouter::AppConfig secured = config;
    secured.server.api_key = "sk-local-test";
    proxy.updateConfig(secured);
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();

    const auto snapshotBefore = proxy.snapshot();
    const Hit anonymous = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(anonymous.status, 401);
    const json refused = json::parse(anonymous.body, nullptr, false);
    LR_CHECK(!refused.is_discarded());
    if (!refused.is_discarded()) {
        LR_CHECK_EQ(refused.at("error").at("type").get<std::string>(), "authentication_error");
        LR_CHECK_EQ(refused.at("error").at("code").get<std::string>(), "invalid_api_key");
    }
    LR_CHECK_EQ(static_cast<long long>(relay_a.chatRequests()), 0);

    const Hit wrongKey =
        postJson(port, "/v1/chat/completions", chatRequest(kRouteModel), "sk-wrong");
    LR_CHECK_EQ(wrongKey.status, 401);

    const Hit models = getPath(port, "/v1/models");
    LR_CHECK_EQ(models.status, 401);

    // Liveness stays open while the key is set. Whatever runs a healthcheck — a
    // container runtime, a load balancer, a systemd unit — carries no key, so
    // gating this reported a healthy proxy as down. Both protocol documents
    // describe it as unauthenticated.
    const Hit health = getPath(port, "/health");
    LR_CHECK_EQ(health.status, 200);
    const json alive = json::parse(health.body, nullptr, false);
    LR_CHECK(!alive.is_discarded());
    if (!alive.is_discarded()) {
        LR_CHECK_EQ(alive.value("status", std::string{}), "ok");
    }
    // It answers the same with a wrong key rather than rejecting it: the body is
    // a constant and tells a caller nothing it could not learn by connecting.
    LR_CHECK_EQ(getPath(port, "/health", "sk-wrong").status, 200);

    const Hit authorized =
        postJson(port, "/v1/chat/completions", chatRequest(kRouteModel), "sk-local-test");
    LR_CHECK_EQ(authorized.status, 200);
    LR_CHECK_EQ(authorized.body, relay_a.normalBody());
    LR_CHECK_EQ(relay_a.chatRequests(), 1);

    // A rejected request never reached the routing pipeline, so it is not in
    // the request counters.
    const auto snapshotAfter = proxy.snapshot();
    LR_CHECK_EQ(snapshotAfter.total_requests - snapshotBefore.total_requests,
                static_cast<std::uint64_t>(1));

    proxy.updateConfig(config);
    const Hit openAgain = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(openAgain.status, 200);
}

void group8Breaker(StubRelay &relay_a, StubRelay &relay_b, literouter::ProxyServer &proxy) {
    LR_GROUP("8. the breaker opens and the relay is skipped");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::ServerError);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    for (int attempt = 0; attempt < 3; ++attempt) {
        const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_MSG(hit.status == 200 && hit.body == relay_b.normalBody(),
                     "every request should still be answered by the second relay");
    }
    LR_CHECK_EQ(relay_a.chatRequests(), 3);
    LR_CHECK_EQ(relay_b.chatRequests(), 3);

    const literouter::Snapshot tripped = proxy.snapshot();
    const auto *alpha = healthOf(tripped, "alpha");
    LR_CHECK(alpha != nullptr && alpha->state == literouter::ProviderHealth::State::Open);
    LR_CHECK(alpha != nullptr && alpha->consecutive_failures >= 3);
    LR_CHECK(tripped.breakers_open >= 1);
    const auto *alphaStat = statOf(tripped, "alpha");
    LR_CHECK(alphaStat != nullptr && alphaStat->requests == 3);
    LR_CHECK(alphaStat != nullptr && alphaStat->failures == 3);

    // The fourth request never reaches the open relay.
    relay_a.resetCounters();
    relay_b.resetCounters();
    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == 0, "an open breaker must skip its relay");
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const literouter::Snapshot after = proxy.snapshot();
    const auto *afterStat = statOf(after, "alpha");
    LR_CHECK(afterStat != nullptr && afterStat->requests == 3);
}

void group9Counters(StubRelay &relay_a, literouter::ProxyServer &proxy) {
    LR_GROUP("9. snapshot counters add up");
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();

    const literouter::Snapshot before = proxy.snapshot();
    const Hit ok = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    const Hit notFound = postJson(port, "/v1/chat/completions", chatRequest(kPassModel));
    LR_CHECK_EQ(ok.status, 200);
    LR_CHECK_EQ(notFound.status, 404);

    const literouter::Snapshot after = proxy.snapshot();
    const auto requests = after.total_requests - before.total_requests;
    const auto successes = after.total_success - before.total_success;
    const auto failures = after.total_failure - before.total_failure;
    LR_CHECK_EQ(requests, static_cast<std::uint64_t>(2));
    LR_CHECK_EQ(successes, static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(failures, static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(requests, successes + failures);
    LR_CHECK_MSG(after.log_seq > before.log_seq, "the log sequence did not advance");
    LR_CHECK_EQ(after.active_requests, static_cast<std::uint64_t>(0));
    LR_CHECK(after.bytes_out > before.bytes_out);
    LR_CHECK_EQ(after.running, true);
    LR_CHECK_EQ(after.version, std::string{literouter::kVersion});
    LR_CHECK_EQ(after.base_url, std::format("http://127.0.0.1:{}", port));
}

void group11CommittedStreamError(StubRelay &relay_a, StubRelay &relay_b,
                                 literouter::ProxyServer &proxy) {
    LR_GROUP("11. an error committed on a streamed request is answered, not aborted");
    const int port = proxy.boundPort();
    proxy.resetStats();
    StderrCapture capture;

    struct Scenario {
        StubRelay::Mode alpha_mode;
        StubRelay::Mode beta_mode;
        int status;
        std::string expected_body; // empty when the proxy must synthesise one
        std::string message_fragment;
    };
    const std::vector<Scenario> scenarios = {
        // Both hops limited: the second one's 429 is the last candidate, so it
        // commits. This is the case that used to kill the process.
        {StubRelay::Mode::RateLimit, StubRelay::Mode::RateLimit, 429, relay_b.errorBody(),
         "rate limit reached"},
        // A 400 is never retried, so it commits on the first hop.
        {StubRelay::Mode::BadRequest, StubRelay::Mode::Normal, 400, relay_a.badRequestBody(),
         "context length exceeded"},
        {StubRelay::Mode::ServerError, StubRelay::Mode::ServerError, 500,
         relay_b.serverErrorBody(), "upstream exploded"},
        // A bare status with no body: the caller still gets the OpenAI shape.
        {StubRelay::Mode::EmptyError, StubRelay::Mode::EmptyError, 500, std::string{}, std::string{}},
    };

    for (const auto &scenario : scenarios) {
        relay_a.setMode(scenario.alpha_mode);
        relay_b.setMode(scenario.beta_mode);
        relay_a.resetCounters();
        relay_b.resetCounters();

        const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true));
        LR_CHECK_MSG(hit.transport_ok,
                     std::format("the proxy did not answer a streamed HTTP {} — is it still alive?",
                                 scenario.status));
        LR_CHECK_EQ(hit.status, scenario.status);
        LR_CHECK_MSG(hit.content_type.find("application/json") != std::string::npos,
                     std::format("a committed HTTP {} must not be labelled {}", scenario.status,
                                 hit.content_type));
        LR_CHECK_MSG(hit.content_type.find("text/event-stream") == std::string::npos,
                     "a committed error is not an event stream");

        const json parsed = json::parse(hit.body, nullptr, false);
        LR_CHECK_MSG(!parsed.is_discarded(),
                     std::format("the HTTP {} body is not JSON: [{}]", scenario.status, hit.body));
        if (parsed.is_discarded() || !parsed.contains("error")) {
            continue;
        }
        const json &error = parsed.at("error");
        LR_CHECK(error.contains("message") && error.at("message").is_string());
        if (!scenario.expected_body.empty()) {
            // Verbatim: the relay's own error text is what the caller sees.
            LR_CHECK_EQ(hit.body, scenario.expected_body);
        } else {
            LR_CHECK_EQ(error.at("type").get<std::string>(), "upstream_error");
            LR_CHECK_EQ(error.at("code").get<std::string>(), "upstream_error");
            LR_CHECK_MSG(!error.at("message").get<std::string>().empty(),
                         "a synthesised error still needs a message");
        }
        const std::string message = error.at("message").get<std::string>();
        LR_CHECK_MSG(scenario.message_fragment.empty() ||
                         message.find(scenario.message_fragment) != std::string::npos,
                     std::format("message did not mention `{}`: {}", scenario.message_fragment,
                                 message));
    }

    // (ii) Still serving: a broken stream must not have poisoned the listener or
    // the worker pool.
    LR_CHECK_MSG(waitForHealth(port), "the proxy stopped answering /health");
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
    const Hit plain = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(plain.status, 200);
    LR_CHECK_EQ(plain.body, relay_a.normalBody());
    relay_a.setMode(StubRelay::Mode::Stream);
    const Hit streamed = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true));
    LR_CHECK_EQ(streamed.status, 200);
    LR_CHECK_EQ(streamed.body, join(relay_a.sseChunks()));

    // (iii) Every relay's outcome counters still balance, so an attempt can
    // never be counted without an outcome.
    const literouter::Snapshot snapshot = proxy.snapshot();
    for (const auto &stat : snapshot.providers) {
        LR_CHECK_MSG(stat.requests == stat.successes + stat.failures + stat.aborted,
                     std::format("relay `{}`: requests={} successes={} failures={} aborted={}",
                                 stat.provider, stat.requests, stat.successes, stat.failures,
                                 stat.aborted));
    }

    LR_CHECK_MSG(capture.text().empty(),
                 "the process wrote to stderr while answering committed stream errors: " +
                     capture.text());
}

void group17LatencyPercentile(StubRelay &relay_a, literouter::ProxyServer &proxy) {
    LR_GROUP("17. the p95 latency is computed from the attempts, not left at zero");
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);

    // resetStats() drops the window along with the totals, so the numbers below
    // can only be made of what this group serves.
    proxy.resetStats();
    const auto *cleared = statOf(proxy.snapshot(), "alpha");
    LR_CHECK(cleared != nullptr && cleared->latency_ms_p95 == 0.0);

    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    const auto *one = statOf(proxy.snapshot(), "alpha");
    LR_CHECK(one != nullptr);
    if (one != nullptr) {
        // One sample: the percentile is that sample, exactly. It used to be a
        // field that was serialized, typed in the console, and never computed.
        LR_CHECK_MSG(one->latency_ms_p95 == one->latency_ms_last,
                     "a single sample's p95 must be the sample itself");
        LR_CHECK(one->latency_ms_p95 > 0.0);
    }

    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    const auto *two = statOf(proxy.snapshot(), "alpha");
    LR_CHECK(two != nullptr);
    if (two != nullptr) {
        LR_CHECK_EQ(two->requests, static_cast<std::uint64_t>(2));
        // Two samples are too few for a nearest-rank p95 to be anything but the
        // slower of them, and the slower of two cannot be slower than the last.
        LR_CHECK(two->latency_ms_p95 >= two->latency_ms_last);
    }
}

void group18RetryAfter(StubRelay &relay_a, StubRelay &relay_b, literouter::ProxyServer &proxy) {
    LR_GROUP("18. a relay that names a Retry-After window is believed, not hammered");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::RateLimit);
    relay_a.setRetryAfter(600);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    // The failover still serves the client — honouring the header must not turn
    // a 429 into a refusal.
    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);

    // One 429 was enough: the relay said "back in 600 seconds", so the breaker
    // opened for that window rather than waiting for three strikes.
    const literouter::Snapshot after = proxy.snapshot();
    const auto *alpha = healthOf(after, "alpha");
    LR_CHECK(alpha != nullptr && alpha->state == literouter::ProviderHealth::State::Open);
    LR_CHECK(alpha != nullptr && alpha->cooldown_remaining > 30.0);
    LR_CHECK_EQ(after.breakers_open, 1);

    // So the next request goes straight to the relay that is still answering.
    const Hit second = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(second.status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == 1,
                 "the relay that asked for 600 seconds of quiet was tried again anyway");
    LR_CHECK_EQ(relay_b.chatRequests(), 2);

    // Left tidy for the groups after this one: resetStats() clears breaker state
    // too, and a 600-second window would otherwise outlive the test.
    relay_a.setRetryAfter(std::nullopt);
    relay_a.setMode(StubRelay::Mode::Normal);
    proxy.resetStats();
}

std::int64_t thisProcessId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

void group19SingleInstance(const literouter::AppConfig &base) {
    LR_GROUP("19. a second instance refuses to share the port");
    literouter::AppConfig config = base;
    config.server.port = 0; // the kernel picks one; the pid file will name it

    literouter::ProxyServer first;
    const auto started = first.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = first.boundPort();
    LR_CHECK(port > 0);

    // While it listens, the instance has a record of itself, named by the port it
    // actually got.
    const std::filesystem::path pid_file = literouter::defaultPidPath(port);
    std::string pid_text;
    {
        std::ifstream input{pid_file, std::ios::binary};
        pid_text.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    }
    const json pid_doc = json::parse(pid_text, nullptr, false);
    LR_CHECK_MSG(!pid_doc.is_discarded(), "no readable pid file for a running instance");
    if (!pid_doc.is_discarded()) {
        LR_CHECK_EQ(pid_doc.value("port", 0), port);
        LR_CHECK_EQ(pid_doc.value("pid", static_cast<std::int64_t>(0)), thisProcessId());
    }

    // httplib shares a port between instances, so a bind would have succeeded
    // here and the two would then have split the traffic between them.
    literouter::AppConfig clash = base;
    clash.server.port = port;
    literouter::ProxyServer second;
    const auto refused = second.start(clash);
    LR_CHECK_MSG(!refused.has_value(), "a second instance started on an occupied port");
    if (!refused) {
        LR_CHECK_MSG(refused.error().find("already answers") != std::string::npos,
                     "the refusal does not say what is there: " + refused.error());
        LR_CHECK_MSG(refused.error().find(std::to_string(port)) != std::string::npos,
                     "the refusal does not name the port: " + refused.error());
        LR_CHECK_MSG(refused.error().find("pid") != std::string::npos,
                     "the refusal does not name the holder: " + refused.error());
    }
    LR_CHECK(!second.running());
    // And it did not take over the record of the instance that is still running.
    LR_CHECK_MSG(std::filesystem::exists(pid_file),
                 "the refused start removed the running instance's pid file");

    first.stop();
    LR_CHECK_MSG(!std::filesystem::exists(pid_file), "stop() left the pid file behind");

    // The port is free again, so this is a legitimate restart rather than a
    // conflict — the guard must not outlive the instance it protects.
    const auto again = second.start(clash);
    LR_CHECK_MSG(again.has_value(), again ? "" : again.error());
    if (again) {
        LR_CHECK(second.running());
        second.stop();
        LR_CHECK(!std::filesystem::exists(pid_file));
    }
}

void group10Restart(literouter::ProxyServer &proxy, const literouter::AppConfig &config) {
    LR_GROUP("10. stop() is clean and a second start()/stop() cycle works");
    proxy.stop();
    LR_CHECK(!proxy.running());
    proxy.stop(); // idempotent
    LR_CHECK(!proxy.running());

    const auto restarted = proxy.start(config);
    LR_CHECK_MSG(restarted.has_value(), restarted ? "" : restarted.error());
    if (!restarted) {
        return;
    }
    LR_CHECK(proxy.running());
    LR_CHECK_EQ(proxy.boundAddress(), "127.0.0.1");
    LR_CHECK(proxy.boundPort() > 0);
    LR_CHECK_MSG(waitForHealth(proxy.boundPort()), "the restarted server never answered /health");

    const Hit hit = postJson(proxy.boundPort(), "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);

    proxy.stop();
    LR_CHECK(!proxy.running());
    // A stopped server refuses to be half-alive: the port is free again.
    const Hit dead = getPath(proxy.boundPort(), "/health");
    LR_CHECK_MSG(!dead.transport_ok || dead.status != 200,
                 "the listener still answers after stop()");
}

// A ProxyServer that is constructed and destroyed without ever having been
// started. This was a real crash, not a hypothetical one: the console builds one
// at startup and only creates its listener when the user presses Start, so
// closing the window without starting a server dereferenced a null listener
// during static destruction. Anything that embeds the engine and decides at
// runtime whether to listen hits the same path.
void group13NeverStarted(StubRelay &relay) {
    LR_GROUP("13. a never-started server survives construction, stop() and destruction");
    {
        literouter::ProxyServer untouched;
        LR_CHECK(!untouched.running());
        // Querying a server that was never started must be boring, not fatal.
        const auto idle = untouched.snapshot();
        LR_CHECK(!idle.running);
        LR_CHECK_EQ(static_cast<long long>(idle.total_requests), 0);
        LR_CHECK(untouched.logsSince(0, 10).empty());
        untouched.stop(); // no listener to stop; must be a no-op
        LR_CHECK(!untouched.running());
        untouched.stop(); // and idempotent on top of that
    } // the destructor runs here — the line that used to segfault

    // The same sequence must still be startable afterwards: "stop before start"
    // must not latch a state that start() cannot clear.
    literouter::ProxyServer proxy;
    proxy.stop();
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    literouter::ProviderConfig provider;
    provider.id = "solo";
    provider.base_url = relay.baseUrl();
    provider.timeout_sec = 4;
    provider.connect_timeout_sec = 2;
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = "m";
    route.targets = {literouter::RouteTarget{.provider = "solo", .model = {}}};
    config.routes = {route};

    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    LR_CHECK(proxy.running());
    LR_CHECK_MSG(waitForHealth(proxy.boundPort()),
                 "start() after a pre-start stop() did not listen");
    proxy.stop();
    LR_CHECK(!proxy.running());
}

#ifndef _WIN32
void group12HangingRelay(HangingRelay &hanger, StubRelay &relay_b,
                         literouter::ProxyServer &proxy) {
    LR_GROUP("12. a relay that hangs up before headers fails over");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_b.resetCounters();
    const int connectionsBefore = hanger.connections();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kCloserModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_EQ(hit.body, relay_b.normalBody());
    LR_CHECK_MSG(hanger.connections() > connectionsBefore, "the hanging relay was never contacted");
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const literouter::Snapshot snapshot = proxy.snapshot();
    const auto *beta = statOf(snapshot, "beta");
    LR_CHECK(beta != nullptr && beta->retries_in == 1);
    const auto *hangerStat = statOf(snapshot, "hanger");
    LR_CHECK(hangerStat != nullptr && hangerStat->failures == 1);

    LR_GROUP("12b. the same happens on the streaming path before any byte is committed");
    proxy.resetStats();
    relay_b.setMode(StubRelay::Mode::Stream);
    relay_b.resetCounters();
    const int before = hanger.connections();
    const Hit streamed = postJson(port, "/v1/chat/completions", chatRequest(kCloserModel, true));
    LR_CHECK_EQ(streamed.status, 200);
    LR_CHECK_MSG(streamed.body == join(relay_b.sseChunks()),
                 "a pre-headers failure on a stream must still fail over");
    LR_CHECK(hanger.connections() > before);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
}

#endif

void group14MultiProtocolIngress(StubRelay &relay_a, int port) {
    LR_GROUP("14. multi-protocol ingress and conditional passthrough");
    relay_a.setMode(StubRelay::Mode::Normal);

    // 1. Anthropic client hitting Anthropic provider -> SAME PROTOCOL PASSTHROUGH
    {
        relay_a.resetCounters();
        std::string raw_anthropic_req = R"({"model":"claude-routed","max_tokens":100,"messages":[{"role":"user","content":"hello claude"}]})";
        Hit hit = postJson(port, "/v1/messages", raw_anthropic_req);
        LR_CHECK(hit.transport_ok);
        LR_CHECK_EQ(hit.status, 200);
        LR_CHECK_EQ(relay_a.chatRequests(), 1);
        LR_CHECK(relay_a.lastChatPath().ends_with("/v1/messages"));
        // Upstream received byte-for-byte identical body!
        LR_CHECK_EQ(relay_a.lastChatBody(), raw_anthropic_req);
        // Client received verbatim response body!
        LR_CHECK_EQ(hit.body, relay_a.normalBody());
    }

    // 2. Anthropic client hitting OpenAI provider (kRouteModel) -> ADAPTATION REQUIRED
    {
        relay_a.resetCounters();
        std::string raw_anthropic_req = R"({"model":"route-model","max_tokens":100,"messages":[{"role":"user","content":"hello openai"}]})";
        Hit hit = postJson(port, "/v1/messages", raw_anthropic_req);
        LR_CHECK(hit.transport_ok);
        LR_CHECK_EQ(hit.status, 200);
        LR_CHECK_EQ(relay_a.chatRequests(), 1);
        LR_CHECK(relay_a.lastChatPath().ends_with("/chat/completions"));
        // Upstream received adapted OpenAI request!
        LR_CHECK(relay_a.lastChatBody().find(R"("messages":)") != std::string::npos);
        // Downstream received Anthropic-adapted response!
        LR_CHECK(hit.body.find(R"("type":"message")") != std::string::npos);
        LR_CHECK(hit.body.find(R"("role":"assistant")") != std::string::npos);
        LR_CHECK(hit.body.find("héllo from alpha — β") != std::string::npos);
    }

    // 3. Gemini client hitting Gemini provider -> SAME PROTOCOL PASSTHROUGH
    {
        relay_a.resetCounters();
        std::string raw_gemini_req = R"({"contents":[{"parts":[{"text":"hello gemini"}],"role":"user"}]})";
        Hit hit = postJson(port, "/v1beta/models/gemini-routed:generateContent", raw_gemini_req);
        LR_CHECK(hit.transport_ok);
        LR_CHECK_EQ(hit.status, 200);
        LR_CHECK_EQ(relay_a.chatRequests(), 1);
        LR_CHECK(relay_a.lastChatPath().find(":generateContent") != std::string::npos);
        // Upstream received byte-for-byte identical body!
        LR_CHECK_EQ(relay_a.lastChatBody(), raw_gemini_req);
        // Client received verbatim response body!
        LR_CHECK_EQ(hit.body, relay_a.normalBody());
    }

    // 4. Gemini models endpoint: GET /v1beta/models
    {
        httplib::Client client{"127.0.0.1", port};
        auto res = client.Get("/v1beta/models");
        LR_CHECK(res);
        LR_CHECK_EQ(res->status, 200);
        LR_CHECK(res->body.find(R"("models":)") != std::string::npos);
        LR_CHECK(res->body.find("models/claude-routed") != std::string::npos);
        LR_CHECK(res->body.find("models/gemini-routed") != std::string::npos);
    }
}

} // namespace

void group15WebConsole(StubRelay &relay_a, literouter::ProxyServer &proxy) {
    LR_GROUP("15. the built-in console and the admin API behind it");
    const int port = proxy.boundPort();

    // The console is served by the proxy itself, so a headless machine needs no
    // second process, no static-file daemon and no CORS configuration.
    const Hit index = getPath(port, "/ui/");
    LR_CHECK_EQ(index.status, 200);
    LR_CHECK_MSG(index.body.find("literouter console") != std::string::npos,
                 "the console shell is served");
    LR_CHECK_MSG(index.content_type.starts_with("text/html"), "…as HTML");
    LR_CHECK_MSG(index.body.find(R"(src="/ui/app.js")") != std::string::npos,
                 "the console references its script under /ui/");
    LR_CHECK_MSG(index.body.find(R"(href="/ui/app.css")") != std::string::npos,
                 "the console references its stylesheet under /ui/");

    const Hit script = getPath(port, "/ui/app.js");
    LR_CHECK_EQ(script.status, 200);
    LR_CHECK_MSG(script.body.find("/__literouter/status") != std::string::npos,
                 "…and so is its script");
    LR_CHECK_MSG(script.content_type.starts_with("text/javascript"), "…as JS");

    const Hit missing = getPath(port, "/ui/does-not-exist.js");
    LR_CHECK_EQ(missing.status, 404);

    // `/` redirects rather than duplicating the page, so a bookmark on either
    // spelling keeps working.
    {
        httplib::Client client{"127.0.0.1", port};
        client.set_follow_location(false);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        auto result = client.Get("/");
        LR_CHECK(result && result->status == 302);
        if (result) {
            LR_CHECK_EQ(result->get_header_value("Location"), "/ui/");
        }
    }

    // The config view never ships a literal secret; a `${VAR}` reference is not
    // a secret and passes through as written.
    const Hit config_hit = getPath(port, "/__literouter/config");
    LR_CHECK_EQ(config_hit.status, 200);
    LR_CHECK_MSG(config_hit.body.find("sk-literal-in-file") == std::string::npos,
                 "a literal api_key must not appear in the admin config response");
    const json doc = json::parse(config_hit.body, nullptr, false);
    LR_CHECK_MSG(!doc.is_discarded(), "the config view is JSON");
    if (!doc.is_discarded()) {
        // Started without a path in this fixture, so the view says so instead
        // of inventing one.
        LR_CHECK_EQ(doc.at("path").get<std::string>(), "");
        LR_CHECK_EQ(doc.at("exists").get<bool>(), false);
        bool saw_literal = false;
        bool saw_env = false;
        for (const auto &provider : doc.at("config").at("providers")) {
            const std::string id = provider.at("id").get<std::string>();
            if (id == "alpha") {
                saw_literal = provider.at("api_key").get<std::string>().empty() &&
                              provider.at("api_key_source").get<std::string>() == "literal";
            }
            if (id == "beta") {
                saw_env = provider.at("api_key").get<std::string>() == "${LR_TEST_KEY}" &&
                          provider.at("api_key_source").get<std::string>() == "env";
            }
        }
        LR_CHECK_MSG(saw_literal, "a literal key is blanked and labelled");
        LR_CHECK_MSG(saw_env, "an env reference is passed through");
        LR_CHECK(doc.at("validation").contains("summary"));
    }

    // CORS is for the client-facing API. The management surface is same-origin
    // for the console, and must not be readable by an arbitrary page — the log
    // it exposes can carry prompts.
    LR_CHECK_MSG(getPath(port, "/__literouter/status").acao.empty(),
                 "the admin API advertises no CORS origin");
    LR_CHECK_EQ(getPath(port, "/v1/models").acao, "*");

    // Probing a configured relay by id resolves the secret server-side, on the
    // machine that holds it; the stub advertises one model, so a true answer is
    // a real relay response rather than a fabricated one.
    const Hit probe = postJson(port, "/__literouter/probe", R"({"provider":"alpha"})");
    LR_CHECK_EQ(probe.status, 200);
    const json probed = json::parse(probe.body, nullptr, false);
    LR_CHECK_MSG(!probed.is_discarded(), "a probe answer is JSON");
    if (!probed.is_discarded()) {
        LR_CHECK(probed.at("reachable").get<bool>());
        LR_CHECK_EQ(probed.at("models").size(), 1u);
    }
    LR_CHECK_EQ(relay_a.modelRequests() > 0, true); // the probe really left the process
    LR_CHECK_EQ(postJson(port, "/__literouter/probe", R"({"provider":"nope"})").status, 404);

    // ── PUT /__literouter/config & server.web_ui ─────────────────────────────────

    // 1. Invalid payload / semantic failure -> 400 / 422, running config unchanged
    {
        const Hit bad_body = putJson(port, "/__literouter/config", "not-json");
        LR_CHECK_EQ(bad_body.status, 400);

        // Break a provider: base_url empty is invalid
        json bad_doc = doc.at("config");
        bad_doc["providers"][0]["base_url"] = "";
        const Hit bad_sem = putJson(port, "/__literouter/config", bad_doc.dump());
        LR_CHECK_EQ(bad_sem.status, 422);
        const json bad_res = json::parse(bad_sem.body, nullptr, false);
        LR_CHECK(!bad_res.is_discarded() && !bad_res.at("ok").get<bool>());

        // Verify config in proxy memory was preserved
        const Hit check_hit = getPath(port, "/__literouter/config");
        const json check_doc = json::parse(check_hit.body, nullptr, false);
        LR_CHECK(!check_doc.at("config").at("providers")[0].at("base_url").get<std::string>().empty());
    }

    // 2. Secret preservation: PUT with redacted api_key: "" keeps stored secret
    {
        json valid_doc = doc.at("config");
        // alpha's api_key is "" in redacted doc
        LR_CHECK(valid_doc.at("providers")[0].at("api_key").get<std::string>().empty());
        json wrapped;
        wrapped["config"] = valid_doc;
        const Hit put_ok = putJson(port, "/__literouter/config", wrapped.dump());
        LR_CHECK_EQ(put_ok.status, 200);
        const json put_res = json::parse(put_ok.body, nullptr, false);
        LR_CHECK(put_res.at("ok").get<bool>());
        LR_CHECK_EQ(put_res.at("saved").get<bool>(), false);

        // Alpha's key is still literal
        const Hit check_hit = getPath(port, "/__literouter/config");
        const json check_doc = json::parse(check_hit.body, nullptr, false);
        bool alpha_literal = false;
        for (const auto &p : check_doc.at("config").at("providers")) {
            if (p.at("id").get<std::string>() == "alpha") {
                alpha_literal = p.value("api_key_source", "") == "literal";
            }
        }
        LR_CHECK_MSG(alpha_literal, "alpha's literal key was preserved on round-trip");
    }

    // 3. Explicit api_key_clear: true clears the secret
    {
        json clear_doc = doc.at("config");
        clear_doc.at("providers")[0]["api_key_clear"] = true;
        const Hit put_clear = putJson(port, "/__literouter/config", clear_doc.dump());
        LR_CHECK_EQ(put_clear.status, 200);

        const Hit check_hit = getPath(port, "/__literouter/config");
        const json check_doc = json::parse(check_hit.body, nullptr, false);
        bool alpha_cleared = false;
        for (const auto &p : check_doc.at("config").at("providers")) {
            if (p.at("id").get<std::string>() == "alpha") {
                alpha_cleared = !p.contains("api_key_source") && p.at("api_key").get<std::string>().empty();
            }
        }
        LR_CHECK_MSG(alpha_cleared, "alpha's key was explicitly cleared");
    }

    // 4. server.web_ui = false disables /ui/ immediately without proxy restart
    {
        json no_web = doc.at("config");
        no_web["server"]["web_ui"] = false;
        const Hit put_no_web = putJson(port, "/__literouter/config", no_web.dump());
        LR_CHECK_EQ(put_no_web.status, 200);

        const Hit ui_off = getPath(port, "/ui/");
        LR_CHECK_EQ(ui_off.status, 404);
        LR_CHECK(ui_off.body.find("console_disabled") != std::string::npos);

        const Hit root_off = getPath(port, "/");
        LR_CHECK_EQ(root_off.status, 404);

        // Turn web_ui back on
        no_web["server"]["web_ui"] = true;
        const Hit put_on = putJson(port, "/__literouter/config", no_web.dump());
        LR_CHECK_EQ(put_on.status, 200);

        const Hit ui_on = getPath(port, "/ui/");
        LR_CHECK_EQ(ui_on.status, 200);
    }
}

void group16TelemetryFile(StubRelay &relay_a, const literouter::AppConfig &base) {
    LR_GROUP("16. telemetry reaches the state file and comes back on the next start");
    literouter::AppConfig config = base;
    // The kernel picks the port: other groups hold the fixture's own port, and
    // this group starts servers of its own.
    config.server.port = 0;
    // The fixture ships with persistence off; this is the group that wants it.
    config.server.persist_telemetry = true;
    relay_a.setMode(StubRelay::Mode::Normal);

    std::error_code ec;

    // 1. A run that serves one request leaves its counters and its log behind —
    // in the file named after the port it ended up on.
    literouter::ProxyServer first;
    int port = 0;
    {
        const auto started = first.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        port = first.boundPort();
        // Everything after this talks to the same instance's files, so the rest
        // of the group starts on the port the first one actually got.
        config.server.port = port;
        const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_EQ(hit.status, 200);
        LR_CHECK_EQ(first.snapshot().total_requests, static_cast<std::uint64_t>(1));
        first.stop();
    }

    const std::filesystem::path state_file = literouter::defaultTelemetryPath(port);
    LR_CHECK_MSG(state_file.string().find("literouter-test-") != std::string::npos,
                 "LITEROUTER_STATE_DIR is not isolated: " + state_file.string());

    LR_CHECK_MSG(std::filesystem::is_regular_file(state_file, ec),
                 "stop() left no telemetry file behind");
    std::uint64_t saved_requests = 0;
    std::uint64_t saved_seq = 0;
    std::size_t saved_entries = 0;
    std::uint64_t relay_requests = 0;
    {
        std::ifstream input{state_file, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        const json doc = json::parse(text, nullptr, false);
        LR_CHECK(!doc.is_discarded());
        if (!doc.is_discarded()) {
            LR_CHECK_EQ(doc.value("version", 0), 1);
            if (const auto it = doc.find("counters"); it != doc.end() && it->is_object()) {
                saved_requests = it->value("total_requests", std::uint64_t{0});
            }
            // The per-relay numbers are the other half of what the console
            // shows, so they have to be in the file too.
            if (const auto it = doc.find("providers"); it != doc.end() && it->is_array()) {
                for (const auto &item : *it) {
                    if (item.is_object() && item.value("provider", std::string{}) == "alpha") {
                        relay_requests = item.value("requests", std::uint64_t{0});
                    }
                }
            }
            if (const auto it = doc.find("log"); it != doc.end() && it->is_object()) {
                saved_seq = it->value("next_seq", std::uint64_t{0});
                if (const auto list = it->find("entries");
                    list != it->end() && list->is_array()) {
                    saved_entries = list->size();
                }
            }
        }
    }
    LR_CHECK_EQ(saved_requests, static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(relay_requests, static_cast<std::uint64_t>(1));
    LR_CHECK(saved_seq >= 1);
    LR_CHECK(saved_entries >= 1);

    // 2. The next process reads them instead of starting from zero, and the log
    // sequence never goes backwards across the restart.
    {
        literouter::ProxyServer second;
        const auto started = second.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        const literouter::Snapshot restored = second.snapshot();
        LR_CHECK_EQ(restored.total_requests, static_cast<std::uint64_t>(1));
        LR_CHECK_EQ(restored.total_success, static_cast<std::uint64_t>(1));
        LR_CHECK_MSG(restored.log_seq >= saved_seq,
                     std::format("log_seq went backwards: {} < {}", restored.log_seq, saved_seq));

        const auto entries = second.logsSince(0, 500);
        LR_CHECK_MSG(!entries.empty(), "the restored log came back empty");
        bool relay_seen = false;
        for (const auto &entry : entries) {
            if (entry.provider == "alpha") {
                relay_seen = true;
            }
        }
        LR_CHECK_MSG(relay_seen, "the restored log lost the entry that named its relay");
        // Uptime is per-run even when the counters are not.
        LR_CHECK(restored.uptime_sec >= 0.0);
        second.stop();
    }

    // 3. Turning it off is what makes a run leave nothing behind — and that run
    // must not read the previous file either.
    {
        literouter::AppConfig quiet = config;
        quiet.server.persist_telemetry = false;
        std::filesystem::remove(state_file, ec);
        literouter::ProxyServer off;
        const auto started = off.start(quiet);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (started) {
            LR_CHECK_EQ(off.snapshot().total_requests, static_cast<std::uint64_t>(0));
            postJson(off.boundPort(), "/v1/chat/completions", chatRequest(kRouteModel));
            off.stop();
        }
        LR_CHECK_MSG(!std::filesystem::exists(state_file, ec),
                     "persist_telemetry = false still wrote a state file");
    }

    // 4. A file this build cannot read is a run with empty counters, never a
    // server that refuses to start.
    {
        std::filesystem::remove(state_file, ec);
        LR_CHECK(writeFile(state_file, "this is not json"));
        literouter::ProxyServer tolerating;
        const auto started = tolerating.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (started) {
            LR_CHECK_EQ(tolerating.snapshot().total_requests, static_cast<std::uint64_t>(0));
            tolerating.stop();
        }
        // And the next write replaces the junk with a readable document.
        std::ifstream input{state_file, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        LR_CHECK(!json::parse(text, nullptr, false).is_discarded());
    }

    // 5. Valid JSON holding the wrong types is the same story: the accessors
    // throw, and a throw must not reach start().
    {
        std::filesystem::remove(state_file, ec);
        LR_CHECK(writeFile(state_file,
                           "{\"version\":1,\"counters\":{\"total_requests\":\"many\"},"
                           "\"log\":{\"next_seq\":\"soon\",\"entries\":[]}}"));
        literouter::ProxyServer tolerant;
        const auto started = tolerant.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (started) {
            LR_CHECK_EQ(tolerant.snapshot().total_requests, static_cast<std::uint64_t>(0));
            tolerant.stop();
        }
    }
}

int main() {
    // A real server writes its telemetry where LITEROUTER_STATE_DIR points; the
    // guard is what keeps a test run out of the developer's own state dir.
    const lr_test::EnvGuard stateEnv{"LITEROUTER_STATE_DIR"};
    TempDir stateDir;
    stateEnv.assign(stateDir.path().string());

    // Non-ASCII in alpha's answer, so "byte-identical" would catch a codec or a
    // re-encoding that a pure-ASCII body would hide.
    StubRelay relay_a{
        "/alpha/v1",
        R"({"id":"stub-alpha","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"héllo from alpha — β"}}],"usage":{"prompt_tokens":11,"completion_tokens":7,"total_tokens":18}})"};
    StubRelay relay_b{
        "/beta/v1",
        R"({"id":"stub-beta","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"hello from beta"}}],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}})"};
#ifndef _WIN32
    HangingRelay hanger;
#else
    StubRelay hanger{"", "{}"};
#endif

    if (!relay_a.start() || !relay_b.start() || !hanger.start()) {
        std::printf("could not start a stub relay — aborting\n");
        return 1;
    }

    int result = 0;
    {
        literouter::ProxyServer proxy;
        literouter::AppConfig config =
            buildProxyConfig(relay_a.baseUrl(), relay_b.baseUrl(), hanger.baseUrl());
        // The shared fixture does not persist: group 16 uses the state file, and
        // two servers writing the same path would make both of them flaky.
        config.server.persist_telemetry = false;
        // A fixture that does not validate would make every failure below
        // ambiguous.
        LR_CHECK_MSG(literouter::validate(config).ok(),
                     "the test fixture config must be valid: " +
                         literouter::validate(config).summary());

        const auto started = proxy.start(config);
        if (!started) {
            std::printf("the proxy did not start: %s\n", started.error().c_str());
            return 1;
        }
        LR_CHECK(proxy.running());
        LR_CHECK_MSG(waitForHealth(proxy.boundPort()), "the proxy never answered /health");

        group1PlainRequest(relay_a, relay_b, proxy.boundPort());
        group2RateLimited(relay_a, relay_b, proxy);
        group3BadRequest(relay_a, relay_b, proxy);
        group4Streaming(relay_a, relay_b, proxy);
        group5ModelList(relay_a, proxy);
        group6UnknownModel(relay_a, proxy, config);
        group7Auth(relay_a, proxy, config);
        group8Breaker(relay_a, relay_b, proxy);
        group9Counters(relay_a, proxy);
        group11CommittedStreamError(relay_a, relay_b, proxy);
#ifndef _WIN32
        // Windows has no raw-socket closer in this file, so the two cases that
        // need one are skipped there.
        group12HangingRelay(hanger, relay_b, proxy);
#endif
        group14MultiProtocolIngress(relay_a, proxy.boundPort());
        group15WebConsole(relay_a, proxy);
        group16TelemetryFile(relay_a, config);
        group17LatencyPercentile(relay_a, proxy);
        group18RetryAfter(relay_a, relay_b, proxy);
        group19SingleInstance(config);
        // Runs last on purpose: it deliberately leaves the proxy stopped.
        group10Restart(proxy, config);

        proxy.stop();
        LR_CHECK(!proxy.running());

        // Last, and inside the summary: it exercises lifecycle edges that are
        // only interesting once the ordinary paths are known good.
        group13NeverStarted(relay_a);

        result = LR_SUMMARY("test_proxy");
    }

    relay_a.stop();
    relay_b.stop();
    hanger.stop();
    return result;
}
