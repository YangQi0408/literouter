// End-to-end proxy tests: a real ProxyServer on 127.0.0.1 talking to real stub
// relays that can be programmed to answer, rate-limit, error, stream or hang up.
//
// Nothing here sleeps-and-hopes: every wait is a poll with a deadline, every
// client has a timeout, and every server this test starts is stopped and joined
// before main returns.
#include <httplib.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
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

bool writeVertexCredentials(const std::filesystem::path &path, const std::string &token_uri) {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
    EVP_PKEY *raw = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1 ||
        EVP_PKEY_keygen(context.get(), &raw) != 1) return false;
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{raw, EVP_PKEY_free};
    const std::unique_ptr<BIO, decltype(&BIO_free)> buffer{BIO_new(BIO_s_mem()), BIO_free};
    if (!buffer || PEM_write_bio_PrivateKey(buffer.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return false;
    }
    char *data = nullptr;
    const long length = BIO_get_mem_data(buffer.get(), &data);
    if (length <= 0) return false;
    const json document{{"type", "service_account"}, {"project_id", "local-test"},
                        {"private_key", std::string{data, static_cast<std::size_t>(length)}},
                        {"client_email", "literouter@local-test.iam.gserviceaccount.com"},
                        {"token_uri", token_uri}};
    return writeFile(path, document.dump());
}

// ── stub relay ───────────────────────────────────────────────────────────────

// Behaves like an OpenAI-compatible relay whose mood is set per assertion group.
class StubRelay {
public:
    enum class Mode { Normal, RateLimit, ServerError, BadRequest, EmptyError, Stream, HeaderEcho, RequestEcho };

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
        server_.Post(path_prefix_ + "/embeddings",
                     [this](const httplib::Request &req, httplib::Response &res) {
                         serveChat(req, res);
                     });
        server_.Post(path_prefix_ + "/v1/responses",
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
        server_.Post(path_prefix_ + R"(/v1/projects/(.*?)/locations/(.*?)/publishers/google/models/(.*?):generateContent)",
                     [this](const httplib::Request &req, httplib::Response &res) {
                         serveChat(req, res);
                     });
        server_.Post(path_prefix_ + "/token", [](const httplib::Request &, httplib::Response &res) {
            res.set_content(R"({"access_token":"local-vertex-token","expires_in":3600,"token_type":"Bearer"})",
                            "application/json");
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
    // A relay that answers slowly on purpose, which is the only way to give the
    // latency-aware policy something to order by.
    void setDelayMs(int ms) { delay_ms_.store(ms); }
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

    httplib::Headers lastChatHeaders() const {
        std::scoped_lock lock{body_mutex_};
        return last_chat_headers_;
    }

private:
    void serveChat(const httplib::Request &req, httplib::Response &res) {
        ++chat_requests_;
        {
            std::scoped_lock lock{body_mutex_};
            last_chat_body_ = req.body;
            last_chat_path_ = req.path;
            last_chat_headers_ = req.headers;
        }
        switch (mode_.load()) {
        case Mode::Normal:
            if (const int delay = delay_ms_.load(); delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
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
        case Mode::HeaderEcho: {
            const std::string content = req.get_header_value("X-Custom-Feature") + "|" +
                                        req.get_header_value("User-Agent");
            const json response{{"id", "header-response"}, {"object", "chat.completion"},
                                {"choices", json::array({json{{"index", 0},
                                    {"message", json{{"role", "assistant"}, {"content", content}}}}})}};
            res.status = 200;
            res.set_content(response.dump(), "application/json");
            return;
        }
        case Mode::RequestEcho: {
            const std::string content = req.path + "|" + req.body;
            json response;
            if (req.path.ends_with("/responses")) {
                response = {{"id", "resp_echo"}, {"object", "response"}, {"status", "completed"},
                            {"output", json::array({json{{"type", "message"}, {"role", "assistant"},
                                {"content", json::array({json{{"type", "output_text"}, {"text", content}}})}}})}};
            } else {
                response = {{"id", "chat_echo"}, {"object", "chat.completion"},
                            {"choices", json::array({json{{"index", 0},
                                {"message", json{{"role", "assistant"}, {"content", content}}}}})}};
            }
            res.set_content(response.dump(), "application/json");
            return;
        }
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
    std::atomic<int> delay_ms_{0};
    std::optional<int> retry_after_;
    std::atomic<int> chat_requests_{0};
    std::atomic<int> model_requests_{0};
    int port_ = 0;
    mutable std::mutex body_mutex_;
    std::string last_chat_body_;
    std::string last_chat_path_;
    httplib::Headers last_chat_headers_;
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
    std::string transport_error;
    int status = 0;
    std::string body;
    std::string content_type;
    std::size_t content_type_count = 0;
    std::string acao; // Access-Control-Allow-Origin, empty when absent
    // "hit" / "miss" / empty. A cache that cannot say whether it answered from
    // memory is a cache nobody can debug.
    std::string cache;
    std::string retry_after;
};

Hit postJson(int port, const std::string &path, const std::string &body,
             const std::string &key = {}, const httplib::Headers &extra_headers = {}, bool compress = false) {
    httplib::Client client{"127.0.0.1", port};
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    client.set_compress(compress);
    httplib::Headers headers = extra_headers;
    if (headers.find("Content-Type") == headers.end()) {
        headers.emplace("Content-Type", "application/json");
    }
    if (!key.empty()) {
        headers.emplace("Authorization", "Bearer " + key);
    }
    auto result = client.Post(path, headers, body, "");
    Hit hit;
    if (!result) {
        hit.transport_error = httplib::to_string(result.error());
        return hit;
    }
    hit.transport_ok = true;
    hit.status = result->status;
    hit.body = result->body;
    hit.content_type = result->get_header_value("Content-Type");
    hit.content_type_count = result->get_header_value_count("Content-Type");
    hit.acao = result->get_header_value("Access-Control-Allow-Origin");
    hit.cache = result->get_header_value("X-Literouter-Cache");
    hit.retry_after = result->get_header_value("Retry-After");
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
    hit.cache = result->get_header_value("X-Literouter-Cache");
    hit.retry_after = result->get_header_value("Retry-After");
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
    hit.cache = result->get_header_value("X-Literouter-Cache");
    hit.retry_after = result->get_header_value("Retry-After");
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
    LR_GROUP("2. a 429 falls through the first relay's models, then to the second relay");
    const int port = proxy.boundPort();
    proxy.resetStats();
    relay_a.setMode(StubRelay::Mode::RateLimit);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(hit.body == relay_b.normalBody(), "the answer came from the wrong relay");
    // alpha has one advertised fallback model behind the route target, so a
    // 429 from the preferred model is followed by one attempt on that model
    // before the chain reaches beta.
    LR_CHECK_EQ(relay_a.chatRequests(), 2);
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
    LR_CHECK(alphaStat != nullptr && alphaStat->requests == 2 && alphaStat->failures == 2);
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
    LR_CHECK_EQ(relay_a.chatRequests(), 2);
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
    LR_CHECK_EQ(relay_a.chatRequests(), 6);
    LR_CHECK_EQ(relay_b.chatRequests(), 3);

    const literouter::Snapshot tripped = proxy.snapshot();
    const auto *alpha = healthOf(tripped, "alpha");
    LR_CHECK(alpha != nullptr && alpha->state == literouter::ProviderHealth::State::Open);
    LR_CHECK(alpha != nullptr && alpha->consecutive_failures >= 3);
    LR_CHECK(tripped.breakers_open >= 1);
    const auto *alphaStat = statOf(tripped, "alpha");
    LR_CHECK(alphaStat != nullptr && alphaStat->requests == 6);
    LR_CHECK(alphaStat != nullptr && alphaStat->failures == 6);

    // The fourth request never reaches the open relay.
    relay_a.resetCounters();
    relay_b.resetCounters();
    const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == 0, "an open breaker must skip its relay");
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const literouter::Snapshot after = proxy.snapshot();
    const auto *afterStat = statOf(after, "alpha");
    LR_CHECK(afterStat != nullptr && afterStat->requests == 6);
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
    const literouter::Snapshot dbg = proxy.snapshot();
    const auto *two = statOf(dbg, "alpha");
    if (two == nullptr || two->requests != 2) {
        for (const auto &s : dbg.providers) {
            std::printf("   FAILDBG relay=%s requests=%llu successes=%llu cost=%.9f\n",
                        s.provider.c_str(), static_cast<unsigned long long>(s.requests),
                        static_cast<unsigned long long>(s.successes), s.cost_usd);
        }
        for (const auto &h : dbg.health) {
            std::printf("   FAILDBG health=%s state=%s failures=%llu\n", h.provider.c_str(),
                        h.stateName().c_str(),
                        static_cast<unsigned long long>(h.consecutive_failures));
        }
    }
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
    LR_CHECK_EQ(relay_a.chatRequests(), 2);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);

    // One 429 was enough: the relay said "back in 600 seconds", so the breaker
    // opened for that window rather than waiting for three strikes.
    const literouter::Snapshot after = proxy.snapshot();
    const auto *alpha = healthOf(after, "alpha");
    LR_CHECK(alpha != nullptr && alpha->state == literouter::ProviderHealth::State::Open);
    LR_CHECK(alpha != nullptr && alpha->cooldown_remaining > 30.0);
    LR_CHECK_EQ(after.breakers_open, 1);

    // So the next request goes straight to the relay that is still answering.
    const int alpha_requests_before_second = relay_a.chatRequests();
    const Hit second = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(second.status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == alpha_requests_before_second,
                 "the relay that asked for 600 seconds of quiet was tried again anyway");
    LR_CHECK_EQ(relay_b.chatRequests(), 2);

    // Left tidy for the groups after this one: resetStats() clears breaker state
    // too, and a 600-second window would otherwise outlive the test.
    relay_a.setRetryAfter(std::nullopt);
    relay_a.setMode(StubRelay::Mode::Normal);
    proxy.resetStats();
}

#ifndef _WIN32
// A relay that counts the connections it is asked to serve.
//
// Counting is the only way to see whether the proxy reused a socket: httplib's
// server keeps its accepted sockets to itself and reports nothing about them, so
// this speaks HTTP/1.1 over raw sockets instead. One thread per connection,
// because the pool may hold more than one open at a time, and a per-request
// response that keeps the socket alive — which is what a relay does.
class CountingRelay {
public:
    CountingRelay() = default;
    ~CountingRelay() { stop(); }
    CountingRelay(const CountingRelay &) = delete;
    CountingRelay &operator=(const CountingRelay &) = delete;

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        // Unqualified on purpose: Darwin declares htonl/ntohs as function-like
        // macros whose bodies are parenthesised expressions, and `::htonl(..)`
        // does not survive that expansion. The sibling HangingRelay above is
        // written the same way for the same reason.
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // the kernel picks one
        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            return false;
        }
        port_ = ntohs(address.sin_port);
        if (::listen(listen_fd_, 64) != 0) {
            return false;
        }
        running_ = true;
        acceptor_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        {
            std::scoped_lock lock{fds_mutex_};
            for (const int fd : open_fds_) {
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::vector<std::thread> workers;
        {
            std::scoped_lock lock{workers_mutex_};
            workers.swap(workers_);
        }
        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // Accept and then say nothing at all, holding the connection open. The only
    // shape that makes a caller *wait*, as opposed to failing immediately, which
    // is what the deadline field exists to bound.
    void setSilent(bool value) { silent_.store(value); }

    std::string baseUrl() const { return std::format("http://127.0.0.1:{}/v1", port_); }
    int connections() const { return connections_.load(); }
    int requests() const { return requests_.load(); }
    int mostRequestsOnOneConnection() const { return most_on_one_.load(); }

private:
    void acceptLoop() {
        while (running_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) {
                    return;
                }
                continue;
            }
            timeval timeout{2, 0}; // an idle keep-alive socket must not pin a thread
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ++connections_;
            {
                std::scoped_lock lock{fds_mutex_};
                open_fds_.push_back(fd);
            }
            std::scoped_lock lock{workers_mutex_};
            workers_.emplace_back([this, fd] { serve(fd); });
        }
    }

    // Serves requests on one connection until the peer goes away.
    void serve(int fd) {
        while (silent_.load() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        int served = 0;
        for (;;) {
            std::string buffer;
            if (!readRequest(fd, buffer)) {
                break;
            }
            ++requests_;
            if (++served > most_on_one_.load()) {
                most_on_one_.store(served);
            }
            if (!writeResponse(fd)) {
                break;
            }
        }
        ::close(fd);
        std::scoped_lock lock{fds_mutex_};
        std::erase(open_fds_, fd);
    }

    static bool readRequest(int fd, std::string &buffer) {
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            char temp[4096];
            const ssize_t got = ::recv(fd, temp, sizeof(temp), 0);
            if (got <= 0) {
                return false;
            }
            buffer.append(temp, static_cast<std::size_t>(got));
            header_end = buffer.find("\r\n\r\n");
            if (buffer.size() > (1u << 20)) {
                return false; // a request bigger than any this test sends
            }
        }
        const std::string head = literouter::toLower(buffer.substr(0, header_end));
        std::size_t body_length = 0;
        if (const auto at = head.find("content-length:"); at != std::string::npos) {
            try {
                body_length = std::stoul(head.substr(at + 15));
            } catch (...) {
                return false;
            }
        }
        const std::size_t header_size = header_end + 4;
        while (buffer.size() - header_size < body_length) {
            char temp[4096];
            const ssize_t got = ::recv(fd, temp, sizeof(temp), 0);
            if (got <= 0) {
                return false;
            }
            buffer.append(temp, static_cast<std::size_t>(got));
        }
        return true;
    }

    // Content-Length rather than chunked, with keep-alive: httplib's client reads
    // exactly that many bytes and hands the socket back to the pool rejoined,
    // which is the behaviour under test.
    static bool writeResponse(int fd) {
        const std::string body =
            "data: {\"id\":\"counted\",\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
            "data: [DONE]\n\n";
        const std::string head = std::format(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\n"
            "Connection: keep-alive\r\n\r\n",
            body.size());
        const std::string response = head + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            const ssize_t wrote =
                ::send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (wrote <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> requests_{0};
    std::atomic<int> most_on_one_{0};
    std::atomic<bool> silent_{false};
    std::thread acceptor_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    std::mutex fds_mutex_;
    std::vector<int> open_fds_;
};

// One provider, one route, one counting relay, and `rounds` sequential requests
// through the proxy. Returns the relay's observations.
struct ReuseRun {
    int connections = 0;
    int requests = 0;
    int most_on_one = 0;
};

ReuseRun measureReuse(CountingRelay &relay, bool stream, int rounds) {
    ReuseRun run;
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false; // this fixture starts servers; no state files
    literouter::ProviderConfig provider;
    provider.id = "counted";
    provider.base_url = relay.baseUrl();
    provider.timeout_sec = 10;
    provider.connect_timeout_sec = 2;
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "counted", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    if (const auto started = proxy.start(config); !started) {
        std::printf("   counting-relay proxy did not start: %s\n", started.error().c_str());
        return run;
    }
    const int port = proxy.boundPort();
    for (int i = 0; i < rounds; ++i) {
        const Hit hit = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, stream));
        if (hit.status != 200) {
            std::printf("   request %d of the reuse run answered %d\n", i, hit.status);
            break;
        }
    }
    proxy.stop();
    run.connections = relay.connections();
    run.requests = relay.requests();
    run.most_on_one = relay.mostRequestsOnOneConnection();
    return run;
}
#endif // !_WIN32

// Every field a console renders must have been written by the traffic that was
// supposed to write it. This group exists because four fields were not:
// `latency_ms_p95` (serialized, typed in the console, never computed),
// `ProviderStat.bytes_in`, `LogEntry.upstream_model`
// and `LogEntry.response_body` (the docs promise it and the drawer renders it).
// All four read as "no data yet" in the UI, which is exactly how a broken field
// hides: nothing errors, something is just permanently empty.
void group21NoFieldLies(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("21. every field the console shows is written by the traffic that should write it");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.log_bodies = true; // so the body fields have something to hold
    config.server.log_body_limit = 4096;
    config.server.persist_telemetry = false;
    config.server.circuit_failure_threshold = 2;

    literouter::ProviderConfig alpha;
    alpha.id = "alpha";
    alpha.base_url = relay_a.baseUrl();
    alpha.timeout_sec = 10;
    alpha.connect_timeout_sec = 2;
    literouter::ProviderConfig beta;
    beta.id = "beta";
    beta.base_url = relay_b.baseUrl();
    beta.timeout_sec = 10;
    beta.connect_timeout_sec = 2;
    config.providers = {alpha, beta};

    // The route renames the model, which is the case that makes
    // `upstream_model` worth showing at all: the client asks for one name and
    // the relay is asked for another.
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "alpha", .model = "alpha-real-model"},
                     literouter::RouteTarget{.provider = "beta", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
    proxy.resetStats();

    const Hit ok = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(ok.status, 200);

    relay_a.setMode(StubRelay::Mode::RateLimit);
    const Hit failed_over = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(failed_over.status, 200);
    relay_a.setMode(StubRelay::Mode::Normal);

    const Hit unrouted = postJson(port, "/v1/chat/completions", chatRequest(kPassModel));
    LR_CHECK_EQ(unrouted.status, 404);

    relay_a.setMode(StubRelay::Mode::Stream);
    const Hit streamed = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true));
    LR_CHECK_EQ(streamed.status, 200);
    relay_a.setMode(StubRelay::Mode::Normal);

    // The accounting for a streamed answer happens on the server side of the
    // socket, so a client can be holding the whole body before the provider's
    // done callback has run. Every wait in this suite is a poll with a deadline
    // for exactly this reason.
    const auto waitForRequests = [&proxy](std::uint64_t expected) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (proxy.snapshot().total_requests >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    LR_CHECK_MSG(waitForRequests(4), "the four requests this group made were not all accounted for");

    // ── the counters ────────────────────────────────────────────────────────
    const literouter::Snapshot after = proxy.snapshot();
    LR_CHECK(after.total_requests >= 4);
    LR_CHECK(after.total_success >= 2);
    LR_CHECK(after.total_failure >= 1);
    LR_CHECK_MSG(after.bytes_out > 0, "bytes_out never written");
    LR_CHECK_MSG(after.tokens_prompt > 0, "tokens_prompt never written");
    LR_CHECK_MSG(after.tokens_completion > 0, "tokens_completion never written");
    LR_CHECK_MSG(after.latency_ms_avg > 0.0, "latency_ms_avg never written");
    // The hourly trend: one bucket for the hour this group ran in, carrying the
    // same traffic the totals do. A trend field nothing writes is exactly the
    // kind of field this group exists to catch.
    LR_CHECK_MSG(!after.hourly.empty(), "Snapshot.hourly was never written");
    if (!after.hourly.empty()) {
        const literouter::TrafficBucket &hour = after.hourly.back();
        LR_CHECK_MSG(hour.requests >= 4,
                     std::format("the current hour holds {} request(s), the totals say {}",
                                 hour.requests, after.total_requests));
        LR_CHECK_MSG(hour.successes + hour.failures == hour.requests,
                     "the hour's outcomes do not add up to its requests");
        LR_CHECK_MSG(hour.bytes_out > 0, "the hour recorded no bytes");
        LR_CHECK_MSG(hour.tokens_prompt > 0, "the hour recorded no tokens");
    }

    for (const auto &stat : after.providers) {
        if (stat.requests == 0) {
            continue; // a relay this traffic never reached
        }
        const std::string who = " (relay " + stat.provider + ")";
        LR_CHECK_MSG(stat.bytes_out > 0, "ProviderStat.bytes_out never written" + who);
        LR_CHECK_MSG(stat.bytes_in > 0, "ProviderStat.bytes_in never written" + who);
        LR_CHECK_MSG(stat.latency_ms_last > 0.0, "latency_ms_last never written" + who);
        LR_CHECK_MSG(stat.latency_ms_avg > 0.0, "latency_ms_avg never written" + who);
        LR_CHECK_MSG(stat.latency_ms_p95 > 0.0, "latency_ms_p95 never written" + who);
        LR_CHECK_MSG(stat.last_used_unix > 0.0, "last_used_unix never written" + who);
        LR_CHECK_MSG(stat.successes + stat.failures + stat.aborted == stat.requests,
                     "the relay's outcomes do not add up to its attempts" + who);
    }

    bool saw_failover_stat = false;
    for (const auto &stat : after.providers) {
        if (stat.retries_in > 0) {
            saw_failover_stat = true;
        }
    }
    LR_CHECK_MSG(saw_failover_stat, "retries_in never written, after a failover");

    for (const auto &health : after.health) {
        if (health.provider == "alpha") {
            LR_CHECK_MSG(health.total_failures > 0, "ProviderHealth.total_failures never written");
            LR_CHECK_MSG(!health.last_error.empty(), "ProviderHealth.last_error never written");
        }
    }

    // ── the log the drawer renders ──────────────────────────────────────────
    const auto entries = proxy.logsSince(0, 500);
    LR_CHECK(!entries.empty());
    bool saw_upstream_model = false;
    bool saw_renamed = false;
    bool saw_text_body = false;
    bool saw_stream = false;
    bool saw_failover = false;
    for (const auto &entry : entries) {
        if (entry.kind == "system") {
            // The proxy's own notes (listening, config reloaded, stopped) are not
            // requests: they carry a message and nothing else, by design.
            continue;
        }
        LR_CHECK_MSG(!entry.request_id.empty(), "LogEntry.request_id never written");
        LR_CHECK_MSG(!entry.kind.empty(), "LogEntry.kind never written");
        LR_CHECK_MSG(!entry.model.empty(), "LogEntry.model never written");
        LR_CHECK_MSG(!entry.message.empty(), "LogEntry.message never written");
        LR_CHECK_MSG(entry.time_unix > 0.0, "LogEntry.time_unix never written");
        LR_CHECK_MSG(entry.status > 0, "LogEntry.status never written");
        // An entry that never reached a relay (an unrouted model, a malformed
        // body) has no upstream call to have taken time, so its latency and its
        // upstream model are legitimately nothing.
        const std::string where = std::format(" [kind={} provider={} status={}]", entry.kind,
                                              entry.provider, entry.status);
        if (entry.provider.empty()) {
            continue;
        }
        LR_CHECK_MSG(entry.latency_ms > 0.0, "LogEntry.latency_ms never written" + where);
        LR_CHECK_MSG(!entry.upstream_model.empty(),
                     "LogEntry.upstream_model never written; the console draws a row for it" +
                         where);
        LR_CHECK_MSG(!entry.request_body.empty(),
                     "LogEntry.request_body never written, with log_bodies on" + where);
        const std::string serialized = literouter::toJsonString(entry);
        const json wire_entry = json::parse(serialized, nullptr, false);
        LR_CHECK_MSG(wire_entry.is_object() &&
                         wire_entry.value("request_body", std::string{}) == entry.request_body,
                     "the admin log JSON lost request_body" + where + ": " + serialized);
        if (entry.upstream_model != entry.model) {
            saw_renamed = true;
        }
        if (entry.upstream_model == "alpha-real-model") {
            saw_upstream_model = true;
        }
        if (!entry.response_body.empty()) {
            saw_text_body = true;
            LR_CHECK_MSG(wire_entry.is_object() &&
                             wire_entry.value("response_body", std::string{}) == entry.response_body,
                         "the admin log JSON lost response_body" + where + ": " + serialized);
        }
        // The phases: a buffered answer is one number (the relay's time to its
        // first byte, since httplib cannot separate connect from answer), and a
        // streamed one splits into "started answering" and "finished answering".
        LR_CHECK_MSG(entry.ttfb_ms > 0.0, "LogEntry.ttfb_ms never written" + where);
        LR_CHECK_MSG(entry.ttfb_ms <= entry.latency_ms + 1.0,
                     "ttfb is larger than the request it belongs to" + where);
        if (entry.attempt > 1) {
            LR_CHECK_MSG(entry.wait_ms > 0.0,
                         "an entry that followed an earlier candidate recorded no wait" + where);
        }
        if (entry.stream) {
            saw_stream = true;
            LR_CHECK_MSG(entry.stream_ms > 0.0,
                         "a streamed entry recorded no streaming phase" + where);
            LR_CHECK_MSG(!entry.response_body.empty(),
                         "a streamed answer wrote no response body, with log_bodies on");
        } else {
            LR_CHECK_MSG(entry.stream_ms == 0.0,
                         "a buffered entry recorded a streaming phase" + where);
        }
        if (entry.failover) {
            saw_failover = true;
        }
    }
    LR_CHECK_MSG(saw_upstream_model, "the renamed upstream model never reached the log");
    LR_CHECK_MSG(saw_renamed, "the log never distinguishes what the client asked for from what "
                              "the relay was asked for");
    LR_CHECK_MSG(saw_text_body, "LogEntry.response_body never written, with log_bodies on");
    LR_CHECK_MSG(saw_stream, "no log entry recorded a streamed request");
    LR_CHECK_MSG(saw_failover, "no log entry recorded a failover");

    // Credentials pasted into a prompt are masked on the way into the log: a log
    // that leaks the very key it was used to debug is worse than no log.
    const std::string leaky =
        R"({"model":"route-model","messages":[{"role":"user","content":"use sk-proj-abcdefghijklmnopqrstuvwx"},)"
        R"({"role":"user","content":"Authorization: Bearer abcdefghijklmnopqrstuvwxyz"}]})";
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", leaky).status, 200);
    bool saw_masked = false;
    for (const auto &entry : proxy.logsSince(0, 500)) {
        if (entry.request_body.find("[redacted]") != std::string::npos) {
            saw_masked = true;
        }
        LR_CHECK_MSG(entry.request_body.find("sk-proj-abcdefghijklmnopqrstuvwx") ==
                         std::string::npos,
                     "a key was written to the log verbatim");
        LR_CHECK_MSG(entry.request_body.find("abcdefghijklmnopqrstuvwxyz") == std::string::npos,
                     "a bearer token was written to the log verbatim");
    }
    LR_CHECK_MSG(saw_masked, "no log entry recorded a masked credential");

    // Bodies are a choice, not a default: with the switch off the same traffic
    // must not put prompts in the log.
    literouter::AppConfig quiet = config;
    quiet.server.log_bodies = false;
    proxy.updateConfig(quiet);
    // Only what is logged AFTER the switch: the ring still holds the entries
    // written while bodies were on, and they are none of this assertion's
    // business.
    const std::uint64_t before_quiet = proxy.snapshot().log_seq;
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    const auto quiet_entries = proxy.logsSince(before_quiet, 500);
    LR_CHECK(!quiet_entries.empty());
    for (const auto &entry : quiet_entries) {
        if (entry.kind == "system") {
            continue;
        }
        LR_CHECK_MSG(entry.request_body.empty(), "a body was logged with log_bodies off");
        LR_CHECK_MSG(entry.response_body.empty(), "a body was logged with log_bodies off");
    }

    proxy.stop();
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

#ifndef _WIN32
void group20ConnectionReuse() {
    LR_GROUP("20. a streamed request reuses its relay connection");

    // The pool is per worker thread, so reuse can only show up as "one
    // connection carried more than one request". Pigeonhole: with `rounds`
    // requests spread over at most `workers` threads, some thread served two of
    // them — asking for twice the worker count makes the assertion independent
    // of how many threads httplib's pool has here and of how the scheduler
    // spread the requests. The buffers leg is the control: it has pooled its
    // connections all along, so it says the counters measure what this test
    // thinks they measure.
    const unsigned hardware = std::thread::hardware_concurrency();
    const int workers = static_cast<int>(std::max(8u, hardware > 0 ? hardware - 1 : 0u));
    const int rounds = workers * 2;

    {
        CountingRelay relay;
        const bool up = relay.start();
        LR_CHECK_MSG(up, "the counting relay could not bind");
        if (up) {
            const ReuseRun buffered = measureReuse(relay, /*stream=*/false, rounds);
            LR_CHECK_EQ(buffered.requests, rounds);
            LR_CHECK_MSG(buffered.most_on_one >= 2,
                         std::format("buffered (control): {} connection(s) for {} request(s), "
                                     "most on one connection {}",
                                     buffered.connections, buffered.requests, buffered.most_on_one));
            LR_CHECK(buffered.connections < buffered.requests);
        }
    }
    {
        CountingRelay relay;
        const bool up = relay.start();
        LR_CHECK_MSG(up, "the counting relay could not bind");
        if (up) {
            const ReuseRun streamed = measureReuse(relay, /*stream=*/true, rounds);
            LR_CHECK_EQ(streamed.requests, rounds);
            LR_CHECK_MSG(streamed.most_on_one >= 2,
                         std::format("streamed: {} connection(s) for {} request(s), most on one "
                                     "connection {} — the pooled connection was not reused",
                                     streamed.connections, streamed.requests, streamed.most_on_one));
            LR_CHECK(streamed.connections < streamed.requests);
        }
    }
}
#endif // !_WIN32

#ifndef _WIN32
// The deadline bounds the whole request, which `timeout_sec` × `max_attempts`
// does not: a chain of slow relays used to be able to keep a client waiting for
// minutes, and none of the individual bounds said otherwise.
void group23RequestDeadline(StubRelay &relay_b) {
    LR_GROUP("23. a request deadline stops the chain, not just one attempt");
    CountingRelay silent;
    const bool up = silent.start();
    LR_CHECK_MSG(up, "the silent relay could not bind");
    if (!up) {
        return;
    }
    silent.setSilent(true);

    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    config.server.request_deadline_sec = 3;

    literouter::ProviderConfig first;
    first.id = "silent";
    first.base_url = silent.baseUrl();
    // Longer than the deadline on purpose: the deadline has to win, not the
    // relay's own timeout.
    first.timeout_sec = 30;
    first.connect_timeout_sec = 5;
    literouter::ProviderConfig second;
    second.id = "beta";
    second.base_url = relay_b.baseUrl();
    second.timeout_sec = 10;
    second.connect_timeout_sec = 2;
    config.providers = {first, second};

    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "silent", .model = {}},
                     literouter::RouteTarget{.provider = "beta", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    relay_b.setMode(StubRelay::Mode::Normal);

    // Streamed, because that is the phase a deadline can cut: waiting for a
    // first byte is a failover, while truncating an answer that has started
    // would not be.
    const double before = literouter::nowUnix();
    const Hit hit = postJson(proxy.boundPort(), "/v1/chat/completions",
                             chatRequest(kRouteModel, /*stream=*/true));
    const double elapsed = literouter::nowUnix() - before;

    LR_CHECK_EQ(hit.status, 504);
    LR_CHECK_MSG(elapsed >= 2.0 && elapsed < 8.0,
                 std::format("the request took {}s against a 3s deadline", elapsed));

    // The log has to say which relay was never tried: "why did this fail" is the
    // question an operator has to act on.
    bool saw_deadline = false;
    for (const auto &entry : proxy.logsSince(0, 200)) {
        if (entry.message.find("deadline") != std::string::npos) {
            saw_deadline = true;
            LR_CHECK_EQ(entry.status, 504);
        }
    }
    LR_CHECK_MSG(saw_deadline, "no log entry explains the deadline");

    // The same chain without a deadline waits on the relay's own timeout, which
    // is exactly what the field exists to bound.
    silent.setSilent(false);
    literouter::AppConfig patient = config;
    patient.server.request_deadline_sec = 0;
    proxy.updateConfig(patient);
    proxy.stop();
}
#endif // !_WIN32


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

// `POST /__literouter/shutdown` — the console's power button — has to leave the
// object in the state a supervisor can notice. It always stopped the listener,
// but the CLI's `serve` loop only ever watched its SIGINT flag, so the process
// stayed alive with the port closed and the pid file still on disk: a hung proxy
// that no signal was coming for, while the documented behaviour is "gracefully
// drains connections and terminates the proxy process".
//
// The loop now also watches `running()`, so this pins the property it reads:
// after the endpoint answers, the listener is gone, `running()` is false, and
// the pid file that marks the instance is removed — all without a signal.
void group34ShutdownEndpoint(StubRelay &relay) {
    LR_GROUP("34. POST /__literouter/shutdown stops the listener, not just the port");

    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0; // the pid file is then named by the port actually bound
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

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    LR_CHECK_MSG(waitForHealth(port), "the proxy never answered /health");
    const std::filesystem::path pid_file = literouter::defaultPidPath(port);
    LR_CHECK_MSG(std::filesystem::exists(pid_file), "a running instance has no pid file");

    const Hit asked = postJson(port, "/__literouter/shutdown", "{}");
    LR_CHECK_EQ(asked.status, 200);

    // The handler stops off the request thread after a short delay, so wait for
    // the state rather than assuming it changed before the reply came back.
    bool stopped = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (!proxy.running()) {
            stopped = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    LR_CHECK_MSG(stopped, "the shutdown endpoint answered but the proxy is still running");

    // The listener has to be gone, not merely flagged: a supervisor that only
    // read the flag would leave a process nothing can reach.
    const Hit dead = getPath(port, "/health");
    LR_CHECK_MSG(!dead.transport_ok || dead.status != 200,
                 "the listener still answers /health after the shutdown endpoint");

    // And the record of the instance goes with it, or a later start on the same
    // port reads a pid file for a process that is not there.
    LR_CHECK_MSG(!std::filesystem::exists(pid_file),
                 "the shutdown endpoint left the pid file behind");

    proxy.stop(); // idempotent on top of the handler's own stop
    LR_CHECK(!proxy.running());
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

// Cost accounting: the two prices an operator writes down, multiplied by the
// tokens a relay reports. The interesting assertions are the exact arithmetic
// and the relay that has no price — a missing price must contribute nothing
// rather than a plausible-looking zero.
void group25CostAccounting(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("25. cost is the relay's tokens times the price written down for it");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;

    literouter::ProviderConfig priced;
    priced.id = "priced";
    priced.base_url = relay_a.baseUrl();
    priced.timeout_sec = 10;
    priced.connect_timeout_sec = 2;
    priced.price_in_per_million = 3.0;
    priced.price_out_per_million = 15.0;
    literouter::ProviderConfig unpriced;
    unpriced.id = "unpriced";
    unpriced.base_url = relay_b.baseUrl();
    unpriced.timeout_sec = 10;
    unpriced.connect_timeout_sec = 2;
    config.providers = {priced, unpriced};

    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "priced", .model = {}},
                     literouter::RouteTarget{.provider = "unpriced", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
    proxy.resetStats();
    LR_CHECK_EQ(proxy.snapshot().cost_usd, 0.0);

    // The stub reports 11 prompt and 7 completion tokens, buffered and streamed.
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    relay_a.setMode(StubRelay::Mode::Stream);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true)).status, 200);
    relay_a.setMode(StubRelay::Mode::Normal);

    // 2 × (11/1e6 × 3 + 7/1e6 × 15) = 2 × 0.000138
    constexpr double kPerAttempt = (11.0 / 1'000'000.0) * 3.0 + (7.0 / 1'000'000.0) * 15.0;
    const literouter::Snapshot snapshot = proxy.snapshot();
    LR_CHECK_MSG(std::abs(snapshot.cost_usd - 2 * kPerAttempt) < 1e-12,
                 std::format("total cost is {:.9f}, expected {:.9f}", snapshot.cost_usd,
                             2 * kPerAttempt));
    const auto *alpha = statOf(snapshot, "priced");
    LR_CHECK(alpha != nullptr);
    if (alpha != nullptr) {
        LR_CHECK_MSG(std::abs(alpha->cost_usd - 2 * kPerAttempt) < 1e-12,
                     std::format("relay cost is {:.9f}, expected {:.9f}", alpha->cost_usd,
                                 2 * kPerAttempt));
        LR_CHECK_EQ(alpha->tokens_prompt, static_cast<std::uint64_t>(22));
        LR_CHECK_EQ(alpha->tokens_completion, static_cast<std::uint64_t>(14));
    }

    // A relay with no price recorded contributes nothing, and the failover to it
    // does not invent one.
    relay_a.setMode(StubRelay::Mode::RateLimit);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    relay_a.setMode(StubRelay::Mode::Normal);
    const literouter::Snapshot after = proxy.snapshot();
    const auto *beta = statOf(after, "unpriced");
    LR_CHECK(beta != nullptr);
    if (beta != nullptr) {
        LR_CHECK_EQ(beta->requests, static_cast<std::uint64_t>(1));
        LR_CHECK_MSG(beta->cost_usd == 0.0,
                     "an unpriced relay reported a cost out of nowhere");
    }
    LR_CHECK_MSG(std::abs(after.cost_usd - 2 * kPerAttempt) < 1e-12,
                 "the unpriced relay moved the total");

    // Model-specific pricing overrides the relay's default prices when specified.
    priced.model_prices["custom-model"] = literouter::ModelPricing{
        .price_in_per_million = 10.0,
        .price_out_per_million = 50.0,
    };
    literouter::RouteConfig custom_route;
    custom_route.model = "custom-model";
    custom_route.targets = {literouter::RouteTarget{.provider = "priced", .model = "custom-model"}};
    config.routes.push_back(custom_route);
    config.providers[0] = priced;
    proxy.updateConfig(config);
    proxy.resetStats();

    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest("custom-model")).status, 200);
    constexpr double kCustomAttempt = (11.0 / 1'000'000.0) * 10.0 + (7.0 / 1'000'000.0) * 50.0;
    const literouter::Snapshot model_snap = proxy.snapshot();
    LR_CHECK_MSG(std::abs(model_snap.cost_usd - kCustomAttempt) < 1e-12,
                 std::format("model-specific cost is {:.9f}, expected {:.9f}",
                             model_snap.cost_usd, kCustomAttempt));

    proxy.stop();
}

// Ordering the chain by what the relays have actually been doing, instead of
// only by the order someone wrote down. Both policies are opt-in; the default
// stays the operator's own order.
void group28RoutingPolicy(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("28. a routing policy can order the chain by latency or by price");
    const auto start_proxy = [](literouter::AppConfig &config, const std::string &policy,
                                StubRelay &a, StubRelay &b, double price_a, double price_b) {
        config.server.host = "127.0.0.1";
        config.server.port = 0;
        config.server.pass_through_unknown = false;
        config.server.persist_telemetry = false;
        config.server.routing_policy = policy;
        literouter::ProviderConfig first;
        first.id = "first";
        first.base_url = a.baseUrl();
        first.timeout_sec = 10;
        first.connect_timeout_sec = 2;
        first.priority = 10; // declared first, so priority order prefers it
        first.price_in_per_million = price_a;
        first.price_out_per_million = price_a;
        literouter::ProviderConfig second;
        second.id = "second";
        second.base_url = b.baseUrl();
        second.timeout_sec = 10;
        second.connect_timeout_sec = 2;
        second.priority = 20;
        second.price_in_per_million = price_b;
        second.price_out_per_million = price_b;
        config.providers = {first, second};
        literouter::RouteConfig route;
        route.model = kRouteModel;
        route.targets = {literouter::RouteTarget{.provider = "first", .model = {}},
                         literouter::RouteTarget{.provider = "second", .model = {}}};
        config.routes = {route};
    };

    // ── fastest ─────────────────────────────────────────────────────────────
    {
        literouter::AppConfig config;
        start_proxy(config, "fastest", relay_a, relay_b, 0.0, 0.0);
        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        const int port = proxy.boundPort();
        relay_a.resetCounters();
        relay_b.resetCounters();
        relay_a.setDelayMs(60);
        relay_a.setMode(StubRelay::Mode::Normal);
        relay_b.setMode(StubRelay::Mode::Normal);

        // Turn one: neither relay has been measured, so priority decides.
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        LR_CHECK_EQ(relay_a.chatRequests(), 1);
        LR_CHECK_EQ(relay_b.chatRequests(), 0);

        // Turn two: the slow relay fails, so the fast one gets measured.
        relay_a.setMode(StubRelay::Mode::RateLimit);
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        LR_CHECK_EQ(relay_b.chatRequests(), 1);
        relay_a.setMode(StubRelay::Mode::Normal);

        // Turn three: both have been measured now, and the fast one is first even
        // though it was declared second.
        const int slow_before = relay_a.chatRequests();
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        LR_CHECK_MSG(relay_a.chatRequests() == slow_before,
                     "the 60ms relay was tried first despite a measured-fast alternative");
        LR_CHECK_EQ(relay_b.chatRequests(), 2);
        proxy.stop();
    }

    // ── cheapest ────────────────────────────────────────────────────────────
    {
        literouter::AppConfig config;
        // Declared first and ten times the price of the other one.
        start_proxy(config, "cheapest", relay_a, relay_b, 10.0, 1.0);
        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        const int port = proxy.boundPort();
        relay_a.resetCounters();
        relay_b.resetCounters();
        relay_a.setDelayMs(0);
        relay_a.setMode(StubRelay::Mode::Normal);
        relay_b.setMode(StubRelay::Mode::Normal);

        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        LR_CHECK_MSG(relay_b.chatRequests() == 1 && relay_a.chatRequests() == 0,
                     std::format("the cheap relay was not preferred: first={} second={}",
                                 relay_a.chatRequests(), relay_b.chatRequests()));
        proxy.stop();
    }

    // ── the default is still the operator's order ───────────────────────────
    {
        literouter::AppConfig config;
        start_proxy(config, "priority", relay_a, relay_b, 10.0, 1.0);
        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        relay_a.resetCounters();
        relay_b.resetCounters();
        LR_CHECK_EQ(postJson(proxy.boundPort(), "/v1/chat/completions", chatRequest(kRouteModel)).status,
                    200);
        LR_CHECK_MSG(relay_a.chatRequests() == 1,
                     "the default policy stopped honouring the declared order");
        proxy.stop();
    }

    // ── round-robin ─────────────────────────────────────────────────────────
    {
        literouter::AppConfig config;
        start_proxy(config, "round_robin", relay_a, relay_b, 10.0, 1.0);
        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        const int port = proxy.boundPort();
        relay_a.resetCounters();
        relay_b.resetCounters();
        relay_a.setDelayMs(0);
        relay_a.setMode(StubRelay::Mode::Normal);
        relay_b.setMode(StubRelay::Mode::Normal);

        std::vector<int> starts;
        for (int request = 0; request < 4; ++request) {
            const int a_before = relay_a.chatRequests();
            const int b_before = relay_b.chatRequests();
            LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
            starts.push_back(relay_a.chatRequests() > a_before ? 0 : 1);
            LR_CHECK_EQ(relay_a.chatRequests() + relay_b.chatRequests(), request + 1);
        }
        LR_CHECK_MSG(starts == (std::vector<int>{0, 1, 0, 1}),
                     "round-robin did not alternate the first relay");
        LR_CHECK_EQ(relay_a.chatRequests(), 2);
        LR_CHECK_EQ(relay_b.chatRequests(), 2);

        // A round-robin cursor must not rotate an open relay ahead of a healthy
        // one. The breaker state survives the policy and remains the first
        // ordering invariant, with configuration threshold 1 for this probe.
        relay_a.setMode(StubRelay::Mode::RateLimit);
        config.server.circuit_failure_threshold = 1;
        proxy.updateConfig(config);
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        const int a_after_trip = relay_a.chatRequests();
        const int b_after_trip = relay_b.chatRequests();
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
        LR_CHECK_MSG(relay_a.chatRequests() == a_after_trip,
                     "round-robin retried the open relay before the healthy one");
        LR_CHECK_EQ(relay_b.chatRequests(), b_after_trip + 1);
        relay_a.setMode(StubRelay::Mode::Normal);
        proxy.stop();
    }

    // A single relay with its own model fallback chain still has multiple
    // candidates. Grouping by relay must not make the candidate list disappear
    // when there is nobody to rotate against.
    {
        literouter::AppConfig config;
        config.server.host = "127.0.0.1";
        config.server.port = 0;
        config.server.pass_through_unknown = false;
        config.server.persist_telemetry = false;
        config.server.routing_policy = "round_robin";
        literouter::ProviderConfig first;
        first.id = "first";
        first.base_url = relay_a.baseUrl();
        first.timeout_sec = 10;
        first.connect_timeout_sec = 2;
        first.priority = 10;
        first.models = {"fallback-model"};
        config.providers = {first};
        literouter::RouteConfig route;
        route.model = kRouteModel;
        route.targets = {literouter::RouteTarget{.provider = "first", .model = {}}};
        config.routes = {route};

        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) {
            return;
        }
        relay_a.resetCounters();
        relay_a.setMode(StubRelay::Mode::Normal);
        LR_CHECK_EQ(postJson(proxy.boundPort(), "/v1/chat/completions", chatRequest(kRouteModel)).status,
                    200);
        LR_CHECK_EQ(relay_a.chatRequests(), 1);
        proxy.stop();
    }

    // Price and historical latency must not undo the breaker's healthy-first
    // order. With one attempt, choosing the open relay would strand a healthy
    // backup indefinitely.
    for (const std::string policy : {"fastest", "cheapest"}) {
        literouter::AppConfig config;
        start_proxy(config, policy, relay_a, relay_b, 1.0, 10.0);
        config.server.max_attempts = 1;
        config.server.circuit_failure_threshold = 1;
        relay_a.setDelayMs(0);
        relay_b.setDelayMs(0);
        relay_a.setMode(StubRelay::Mode::Normal);
        relay_b.setMode(StubRelay::Mode::Normal);
        relay_a.resetCounters();
        relay_b.resetCounters();

        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) continue;

        const int port = proxy.boundPort();
        const Hit warm = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_MSG(warm.status == 200 && relay_a.chatRequests() == 1,
                     std::format("{} did not warm the preferred relay: status={} requests={}",
                                 policy, warm.status, relay_a.chatRequests()));
        relay_a.setMode(StubRelay::Mode::RateLimit);
        const Hit failed = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_MSG(failed.status == 429,
                     std::format("{} did not observe the failure: status={}", policy, failed.status));
        const int failed_attempts = relay_a.chatRequests();
        const Hit recovered = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_MSG(recovered.status == 200 && relay_a.chatRequests() == failed_attempts &&
                         relay_b.chatRequests() == 1,
                     std::format("{} reordered an open breaker: status={} failed={} backup={}",
                                 policy, recovered.status, relay_a.chatRequests(), relay_b.chatRequests()));
        proxy.stop();
    }
    relay_a.setMode(StubRelay::Mode::Normal);
}

void group35ForwardedHeaders(StubRelay &relay) {
    LR_GROUP("35. business headers survive both transports without local credentials or hop metadata");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.persist_telemetry = false;
    config.server.pass_through_unknown = false;
    config.server.api_key = "local-credential";
    literouter::ProviderConfig provider;
    provider.id = "headers";
    provider.base_url = relay.baseUrl();
    provider.api_key = "upstream-credential";
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = provider.id, .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) return;

    {
        httplib::Client client{"127.0.0.1", proxy.boundPort()};
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        const std::string requested = "authorization, content-type, x-custom-feature, traceparent";
        const httplib::Headers preflight{{"Origin", "https://client.example"},
                                         {"Access-Control-Request-Method", "POST"},
                                         {"Access-Control-Request-Headers", requested}};
        const auto api = client.Options("/v1/chat/completions", preflight);
        LR_CHECK_MSG(api && api->status == 204 &&
                         api->get_header_value("Access-Control-Allow-Headers") == requested,
                     api ? std::format("custom-header preflight returned {} / {}", api->status,
                                        api->get_header_value("Access-Control-Allow-Headers"))
                         : "custom-header preflight failed to connect");
        const auto admin = client.Options("/__literouter/config", preflight);
        LR_CHECK_MSG(admin && !admin->has_header("Access-Control-Allow-Origin") &&
                         !admin->has_header("Access-Control-Allow-Headers"),
                     admin ? "admin preflight exposed cross-origin headers" : "admin preflight failed to connect");
    }

    const httplib::Headers incoming{
        {"uSeR-aGeNt", "codex-test/1.0"}, {"originator", "codex_cli_rs"},
        {"OpenAI-Beta", "responses=experimental"}, {"oPeNaI-bEtA", "assistants=v2"},
        {"OpenAI-Organization", "org-client"},
        {"OpenAI-Project", "proj-client"}, {"anthropic-version", "2023-06-01"},
        {"anthropic-beta", "tools-test"}, {"AnThRoPiC-bEtA", "prompt-caching-test"},
        {"X-Client-Request-ID", "client-request"},
        {"X-Stainless-Runtime", "python"}, {"session_id", "client-session"},
        {"x-api-key", "local-anthropic-credential"}, {"api-key", "local-azure-credential"},
        {"x-goog-api-key", "local-gemini-credential"}, {"Cookie", "local-session=secret"},
        {"Proxy-Authorization", "Basic local-proxy-credential"},
        {"X-Forwarded-For", "203.0.113.10"}, {"Forwarded", "for=203.0.113.10"},
        {"X-Custom-Feature", "new-relay-capability"}, {"x-CUSTOM-feature", "second-value"},
        {"Traceparent", "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01"},
        {"Connection", "keep-alive, X-Hop-Private, x-HOP-second, Content-MD5"},
        // httplib's typed-header API rejects outer whitespace. Put it around
        // the comma instead so trimming is exercised by a request it can send.
        {"cOnNeCtIoN", "X-Hop-Third ,\t X-Hop-Fourth"}, {"X-Hop-Private", "hop-only"},
        {"x-hop-second", "second-hop-only"}, {"X-Hop-Third", "third-hop-only"},
        {"X-Hop-Fourth", "fourth-hop-only"},
        {"Keep-Alive", "timeout=987"}, {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"}, {"Trailer", "X-Late"}, {"Upgrade", "client-protocol"},
        {"Content-Type", "application/json; charset=us-ascii"},
        {"Content-MD5", "old-body-digest"}, {"Content-Digest", "sha-256=:old-digest:"},
        {"Accept", "application/x-client-only"}, {"Accept-Encoding", "br"},
        {"Host", "client.example"}, {"X-Amz-Date", "20000101T000000Z"},
        {"X-Amz-Content-Sha256", "old-body-sha"}, {"X-Amz-Security-Token", "local-aws-token"},
    };

    for (const bool anthropic : {false, true}) {
        for (const bool configured : {false, true}) {
            config.providers[0].protocol = anthropic ? "anthropic" : "openai";
            config.providers[0].headers.clear();
            if (configured) {
                config.providers[0].headers = {
                    {"USER-agent", "configured-agent"}, {"ORIGINATOR", "configured-originator"},
                    {"x-CUSTOM-feature", "configured-feature"},
                    {"Content-Type", "application/json; charset=utf-8"},
                    {anthropic ? "X-API-KEY" : "AUTHORIZATION", "configured-credential"},
                    {anthropic ? "Anthropic-Beta" : "OPENAI-BETA", "configured-beta"},
                };
            }
            proxy.updateConfig(config);
            for (const bool stream : {false, true}) {
                relay.setMode(stream ? StubRelay::Mode::Stream : StubRelay::Mode::Normal);
                json request = json::parse(chatRequest(kRouteModel, stream));
                if (anthropic) request["max_tokens"] = 16;
                const Hit hit = postJson(proxy.boundPort(),
                                         anthropic ? "/v1/messages" : "/v1/chat/completions",
                                         request.dump(), "local-credential", incoming);
                const std::string context = std::format("{} stream={} configured={}",
                                                        anthropic ? "anthropic" : "openai",
                                                        stream, configured);
                LR_CHECK_MSG(hit.transport_ok && hit.status == 200,
                             std::format("{} failed: status={} transport={} body={}",
                                         context, hit.status, hit.transport_error, hit.body));
                if (!hit.transport_ok) continue;
                LR_CHECK_MSG(hit.content_type_count == 1,
                             std::format("{} emitted {} Content-Type headers", context, hit.content_type_count));
                LR_CHECK_MSG(hit.content_type == (stream ? "text/event-stream" : "application/json"),
                             std::format("{} returned Content-Type {}", context, hit.content_type));

                const auto headers = relay.lastChatHeaders();
                const auto expect = [&](const std::string &name, const std::string &value) {
                    const auto found = headers.find(name);
                    const std::string actual = found == headers.end() ? "<missing>" : found->second;
                    LR_CHECK_MSG(headers.count(name) == 1 && actual == value,
                                 std::format("{} {} count={} value={}", context, name, headers.count(name), actual));
                };
                const auto absent = [&](const std::string &name) {
                    LR_CHECK_MSG(headers.count(name) == 0,
                                 std::format("{} leaked {} (count={})", context, name, headers.count(name)));
                };
                expect("User-Agent", configured ? "configured-agent" : "codex-test/1.0");
                expect("originator", configured ? "configured-originator" : "codex_cli_rs");
                expect("X-Client-Request-ID", "client-request");
                expect("X-Stainless-Runtime", "python");
                expect("session_id", "client-session");
                expect("X-Custom-Feature", configured ? "configured-feature" : "new-relay-capability");
                expect("Traceparent", "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01");
                expect("Content-Type", configured ? "application/json; charset=utf-8" : "application/json");
                expect("Content-Length", std::to_string(relay.lastChatBody().size()));
                expect("Accept", stream ? "text/event-stream" : "application/json");
                std::string root;
                std::string prefix;
                std::string scheme;
                const bool valid_url = literouter::splitBaseUrl(relay.baseUrl(), root, prefix, scheme);
                LR_CHECK_MSG(valid_url, "invalid stub URL: " + relay.baseUrl());
                if (valid_url) expect("Host", root.substr(root.find("://") + 3));
                const auto encoding = headers.find("Accept-Encoding");
                LR_CHECK_MSG(encoding == headers.end() || encoding->second != "br",
                             context + " reused the client's compression negotiation");
                if (anthropic) {
                    expect("x-api-key", configured ? "configured-credential" : "upstream-credential");
                    expect("anthropic-version", "2023-06-01");
                    expect("anthropic-beta", configured ? "configured-beta" : "tools-test, prompt-caching-test");
                    absent("Authorization");
                    absent("OpenAI-Beta");
                    absent("OpenAI-Organization");
                    absent("OpenAI-Project");
                } else {
                    expect("Authorization", configured ? "configured-credential" : "Bearer upstream-credential");
                    expect("OpenAI-Beta", configured ? "configured-beta" : "responses=experimental, assistants=v2");
                    expect("OpenAI-Organization", "org-client");
                    expect("OpenAI-Project", "proj-client");
                    absent("x-api-key");
                    absent("anthropic-version");
                    absent("anthropic-beta");
                }
                for (const std::string name : {"api-key", "x-goog-api-key", "Cookie", "Proxy-Authorization",
                                               "Forwarded", "X-Forwarded-For", "X-Hop-Private", "X-Hop-Second",
                                               "X-Hop-Third", "X-Hop-Fourth", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Upgrade",
                                               "Content-MD5", "Content-Digest", "X-Amz-Date", "X-Amz-Content-Sha256",
                                               "X-Amz-Security-Token", "Expect"}) {
                    absent(name);
                }
            }
        }
    }
    proxy.stop();
    relay.setMode(StubRelay::Mode::Normal);
}

void group36EmbeddingAlias(StubRelay &relay) {
    LR_GROUP("36. embedding routes rename the model while preserving the input");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.persist_telemetry = false;
    config.server.pass_through_unknown = false;
    literouter::ProviderConfig provider;
    provider.id = "embeddings";
    provider.base_url = relay.baseUrl();
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = "embedding-alias";
    route.targets = {literouter::RouteTarget{.provider = provider.id, .model = "text-embedding-3-small"}};
    config.routes = {route};
    relay.setMode(StubRelay::Mode::Normal);

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) return;
    const json request{{"model", "embedding-alias"}, {"input", json::array({"one", "two"})},
                       {"encoding_format", "base64"}, {"dimensions", 256}};
    const Hit hit = postJson(proxy.boundPort(), "/v1/embeddings", request.dump());
    LR_CHECK_MSG(hit.status == 200, std::format("embedding request failed: status={} body={}", hit.status, hit.body));
    const std::string forwarded_body = relay.lastChatBody();
    const json forwarded = json::parse(forwarded_body, nullptr, false);
    json expected = request;
    expected["model"] = "text-embedding-3-small";
    LR_CHECK_MSG(forwarded == expected,
                 std::format("embedding alias/input mismatch: actual={} expected={}", forwarded_body, expected.dump()));
    LR_CHECK_MSG(relay.lastChatPath().ends_with("/embeddings"),
                 "embedding request reached " + relay.lastChatPath());
    proxy.stop();
}

void group37LargeRequestWithoutExpect(StubRelay &relay) {
    LR_GROUP("37. large buffered and streamed requests send the complete body without Expect");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.persist_telemetry = false;
    config.server.pass_through_unknown = false;
    literouter::ProviderConfig provider;
    provider.id = "large-request";
    provider.base_url = relay.baseUrl();
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = provider.id, .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) return;

    for (const auto [stream, compress] : {std::pair{false, false}, std::pair{false, true},
                                          std::pair{true, false}, std::pair{true, true}}) {
#ifndef CPPHTTPLIB_ZLIB_SUPPORT
        if (compress) continue;
#endif
        relay.setMode(stream ? StubRelay::Mode::Stream : StubRelay::Mode::Normal);
        json request = json::parse(chatRequest(kRouteModel, stream));
        // Exceeds httplib's former 1 KiB auto-Expect threshold. Relays that do
        // not implement 100-continue can close before reading this prompt.
        request["messages"][0]["content"] = std::string(44 * 1024, 'x');
        const std::string body = request.dump();
        const Hit hit = postJson(proxy.boundPort(), "/v1/chat/completions", body, {},
                                 {{"Expect", "100-continue"}}, compress);
        LR_CHECK_MSG(hit.transport_ok && hit.status == 200,
                     std::format("large stream={} compressed={} request failed: status={} body={}",
                                 stream, compress, hit.status, hit.body));
        const auto headers = relay.lastChatHeaders();
        LR_CHECK_MSG(headers.count("Expect") == 0,
                     std::format("large stream={} request added {} Expect headers", stream, headers.count("Expect")));
        LR_CHECK_MSG(headers.count("Content-Encoding") == 0,
                     std::format("large stream={} compressed={} request retained its input encoding", stream, compress));
        const std::string received = relay.lastChatBody();
        LR_CHECK_MSG(received == body,
                     std::format("large stream={} request changed/truncated: received={} expected={} bytes",
                                 stream, received.size(), body.size()));
        LR_CHECK_MSG(json::parse(received, nullptr, false).is_object(),
                     std::format("large stream={} request was not complete JSON ({} bytes)", stream, received.size()));
    }
    proxy.stop();
    relay.setMode(StubRelay::Mode::Normal);
}

void group38StreamingCapability(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("38. streaming capability filters candidates before the attempt budget");
    struct Request {
        std::string path;
        std::string body;
    };
    const std::vector<Request> requests{
        {"/v1/chat/completions", chatRequest(kRouteModel, true)},
        {"/v1/responses", json{{"model", kRouteModel}, {"input", "hi"}, {"stream", true}}.dump()},
        {"/v1/messages", json{{"model", kRouteModel}, {"max_tokens", 16}, {"stream", true},
                              {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}}.dump()},
    };
    for (const std::string first_protocol : {"openai", "anthropic"}) {
        literouter::AppConfig config;
        config.server.host = "127.0.0.1";
        config.server.port = 0;
        config.server.persist_telemetry = false;
        config.server.pass_through_unknown = false;
        config.server.max_attempts = 1;
        literouter::ProviderConfig first;
        first.id = "without-streaming";
        first.base_url = relay_a.baseUrl();
        first.protocol = first_protocol;
        first.supports_stream = false;
        literouter::ProviderConfig second;
        second.id = "with-streaming";
        second.base_url = relay_b.baseUrl();
        config.providers = {first, second};
        literouter::RouteConfig route;
        route.model = kRouteModel;
        route.targets = {literouter::RouteTarget{.provider = first.id, .model = {}},
                         literouter::RouteTarget{.provider = second.id, .model = {}}};
        config.routes = {route};
        relay_a.setMode(StubRelay::Mode::Normal);
        relay_b.setMode(StubRelay::Mode::Stream);
        relay_a.resetCounters();
        relay_b.resetCounters();

        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) continue;
        for (const auto &request : requests) {
            const int backup_before = relay_b.chatRequests();
            const Hit hit = postJson(proxy.boundPort(), request.path, request.body);
            LR_CHECK_MSG(hit.status == 200 && hit.content_type == "text/event-stream" &&
                             relay_a.chatRequests() == 0 && relay_b.chatRequests() == backup_before + 1,
                         std::format("{} via {} consumed the only attempt on an incapable relay: status={} first={} backup={}",
                                     request.path, first_protocol, hit.status, relay_a.chatRequests(), relay_b.chatRequests()));
        }

        config.providers[1].supports_stream = false;
        proxy.updateConfig(config);
        const int backup_before = relay_b.chatRequests();
        for (const auto &request : requests) {
            const Hit hit = postJson(proxy.boundPort(), request.path, request.body);
            const json error = json::parse(hit.body, nullptr, false);
            LR_CHECK_MSG(hit.status == 400 && error.is_object() && error.contains("error") &&
                             error["error"].value("code", std::string{}) == "unsupported_stream",
                         std::format("{} without streaming support returned {}: {}", request.path, hit.status, hit.body));
        }
        LR_CHECK_MSG(relay_a.chatRequests() == 0 && relay_b.chatRequests() == backup_before,
                     std::format("unsupported streaming reached a relay: first={} backup={}",
                                 relay_a.chatRequests(), relay_b.chatRequests()));

        // Turning off streaming support does not disable buffered traffic.
        config.providers[0].protocol = "openai";
        proxy.updateConfig(config);
        const Hit buffered = postJson(proxy.boundPort(), "/v1/chat/completions", chatRequest(kRouteModel));
        LR_CHECK_MSG(buffered.status == 200 && relay_a.chatRequests() == 1,
                     std::format("non-streaming relay was also disabled for buffered requests: status={} calls={}",
                                 buffered.status, relay_a.chatRequests()));
        const Hit unknown = postJson(proxy.boundPort(), "/v1/chat/completions", chatRequest("unknown-model", true));
        LR_CHECK_MSG(unknown.status == 404,
                     std::format("unknown streaming model returned {} instead of 404", unknown.status));
        proxy.stop();
    }
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
}

// A config file that changes under a running proxy, for operators who keep the
// file open in an editor rather than using the console.
void group27ConfigHotReload(StubRelay &relay_a) {
    LR_GROUP("27. a changed config file is applied without a restart");
    TempDir dir;
    const auto path = dir.path() / "config.json";

    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    config.server.log_capacity = 64;
    config.server.reload_on_change = true;
    literouter::ProviderConfig provider;
    provider.id = "alpha";
    provider.base_url = relay_a.baseUrl();
    provider.timeout_sec = 10;
    provider.connect_timeout_sec = 2;
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "alpha", .model = {}}};
    config.routes = {route};

    {
        literouter::ConfigStore seed;
        seed.config() = config;
        const auto written = seed.saveAs(path);
        LR_CHECK_MSG(written.has_value(), written ? "" : written.error());
    }

    literouter::ProxyServer proxy;
    proxy.setConfigPath(path.string());
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    LR_CHECK_EQ(proxy.config().server.log_capacity, 64);

    // The operator edits the file: one field, the way an editor would save it.
    literouter::AppConfig edited = config;
    edited.server.log_capacity = 500;
    {
        literouter::ConfigStore store;
        store.config() = edited;
        const auto written = store.saveAs(path);
        LR_CHECK_MSG(written.has_value(), written ? "" : written.error());
    }

    // The watcher runs on the flush tick, so this is a poll with a deadline
    // rather than a sleep of a guessed length.
    bool applied = false;
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (proxy.config().server.log_capacity == 500) {
            applied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    LR_CHECK_MSG(applied, "the file change was not applied");

    bool saw_reload = false;
    for (const auto &entry : proxy.logsSince(0, 200)) {
        if (entry.message.find("reloaded from") != std::string::npos) {
            saw_reload = true;
        }
    }
    LR_CHECK_MSG(saw_reload, "no log entry says the config was reloaded");

    // A server that is not watching must not pick the change up: this is opt-in
    // because a config that moves under a running proxy is a surprise otherwise.
    literouter::AppConfig quiet = edited;
    quiet.server.reload_on_change = false;
    quiet.server.log_capacity = 128;
    proxy.updateConfig(quiet);
    {
        literouter::ConfigStore store;
        store.config() = quiet;
        const auto written = store.saveAs(path);
        LR_CHECK_MSG(written.has_value(), written ? "" : written.error());
    }
    literouter::AppConfig later = quiet;
    later.server.log_capacity = 777;
    {
        literouter::ConfigStore store;
        store.config() = later;
        const auto written = store.saveAs(path);
        LR_CHECK_MSG(written.has_value(), written ? "" : written.error());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    LR_CHECK_MSG(proxy.config().server.log_capacity == 128,
                 "a proxy with reload_on_change off applied a file change anyway");

    proxy.stop();
}

// Prompt-cache affinity: a follow-up turn of a conversation a relay has already
// answered is worth sending back there, because the provider can then reuse the
// cached prefix. Priority order cannot know which relay is warm, so the test
// makes a relay warm by failing over to it and then checks that the *next* turn
// goes there even though priority would prefer the other one.
void group26SessionAffinity(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("26. a conversation sticks to the relay that answered it");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    config.server.session_affinity_sec = 300;

    literouter::ProviderConfig preferred;
    preferred.id = "preferred";
    preferred.base_url = relay_a.baseUrl();
    preferred.timeout_sec = 10;
    preferred.connect_timeout_sec = 2;
    literouter::ProviderConfig other;
    other.id = "other";
    other.base_url = relay_b.baseUrl();
    other.timeout_sec = 10;
    other.connect_timeout_sec = 2;
    config.providers = {preferred, other};

    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "preferred", .model = {}},
                     literouter::RouteTarget{.provider = "other", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.resetCounters();
    relay_b.resetCounters();
    relay_b.setMode(StubRelay::Mode::Normal);

    // A conversation, as a client sends it: a system prompt and a first message
    // that stay identical from turn to turn, with the tail changing.
    const auto turn = [](int exchange) {
        json body = json::object();
        body["model"] = kRouteModel;
        body["messages"] = json::array({
            {{"role", "system"}, {"content", "You are a careful assistant."}},
            {{"role", "user"}, {"content", "How do I configure a route?"}},
            {{"role", "assistant"}, {"content", "You add a route entry."}},
            {{"role", "user"}, {"content", std::format("Follow-up number {}", exchange)}},
        });
        return body.dump();
    };

    // Turn one: the preferred relay answers, so the conversation belongs to it.
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", turn(1)).status, 200);
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_EQ(relay_b.chatRequests(), 0);

    // Now the preferred relay starts rate-limiting. Turn two fails over to the
    // other relay, which becomes the warm one.
    relay_a.setMode(StubRelay::Mode::RateLimit);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", turn(2)).status, 200);
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const int per_turn = relay_a.chatRequests(); // one answered, one rate-limited

    // Turn three: priority would try the rate-limiting relay again, but the
    // conversation is warm on the other one. That is the whole feature.
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", turn(3)).status, 200);
    // The count is relative because the *failed* attempt above was a request to
    // the preferred relay too: what matters is that this turn did not add one.
    LR_CHECK_MSG(relay_a.chatRequests() == per_turn,
                 std::format("the preferred relay was tried again ({} requests, was {}) even "
                             "though the conversation is warm elsewhere",
                             relay_a.chatRequests(), per_turn));
    LR_CHECK_EQ(relay_b.chatRequests(), 2);

    // A different conversation is not pinned: it goes to the preferred relay,
    // which is still the operator's first choice.
    relay_a.setMode(StubRelay::Mode::Normal);
    json other_body = json::object();
    other_body["model"] = kRouteModel;
    other_body["messages"] = json::array({
        {{"role", "system"}, {"content", "You are a terse assistant."}},
        {{"role", "user"}, {"content", "Something else entirely"}},
    });
    const std::string other_chat = other_body.dump();
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", other_chat).status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == per_turn + 1,
                 "a different conversation was pinned instead of taking first choice");

    // Turning the window off restores plain priority order for a warm
    // conversation too.
    literouter::AppConfig plain = config;
    plain.server.session_affinity_sec = 0;
    proxy.updateConfig(plain);
    relay_a.setMode(StubRelay::Mode::Normal);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", turn(4)).status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == per_turn + 2,
                 "turning the affinity window off did not restore priority order");

    proxy.stop();
}

// Prometheus exposition: the console is for a person, this is for a graph, and
// both must read the same numbers or one of them is lying.
void group24MetricsEndpoint(StubRelay &relay_a) {
    LR_GROUP("24. the metrics endpoint exposes the same numbers as the console");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    literouter::ProviderConfig provider;
    provider.id = "alpha";
    provider.base_url = relay_a.baseUrl();
    provider.timeout_sec = 10;
    provider.connect_timeout_sec = 2;
    config.providers = {provider};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "alpha", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    proxy.resetStats();
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);

    const Hit scrape = getPath(port, "/__literouter/metrics");
    LR_CHECK_EQ(scrape.status, 200);
    LR_CHECK_MSG(scrape.content_type.find("text/plain") != std::string::npos,
                 "the scraper got " + scrape.content_type);
    const std::string &body = scrape.body;

    // The shape a scraper needs before it can read anything: a TYPE line per
    // metric family, and `_total` on the counters.
    LR_CHECK(body.find("# TYPE literouter_requests_total counter") != std::string::npos);
    LR_CHECK(body.find("# TYPE literouter_relay_latency_ms_avg gauge") != std::string::npos);
    LR_CHECK(body.find("literouter_build_info{version=\"") != std::string::npos);

    // And the numbers themselves, against the snapshot the console would render.
    const literouter::Snapshot snapshot = proxy.snapshot();
    LR_CHECK(body.find(std::format("literouter_requests_total {}", snapshot.total_requests)) !=
             std::string::npos);
    LR_CHECK(body.find(std::format("literouter_relay_requests_total{{relay=\"alpha\"}} {}",
                                   snapshot.providers[0].requests)) != std::string::npos);
    LR_CHECK_MSG(body.find("literouter_relay_healthy{relay=\"alpha\"} 1") != std::string::npos,
                 "a healthy relay was not reported as healthy:\n" + body);
    LR_CHECK(body.find("literouter_bytes_out_total 0") == std::string::npos ||
             snapshot.bytes_out == 0);

    // Every non-comment line must be parseable as `name[{labels}] value`, because
    // one malformed line makes the whole scrape fail.
    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const std::size_t newline = body.find('\n', cursor);
        const std::string line =
            body.substr(cursor, newline == std::string::npos ? std::string::npos : newline - cursor);
        cursor = newline == std::string::npos ? body.size() + 1 : newline + 1;
        if (line.empty() || literouter::startsWith(line, "#")) {
            continue;
        }
        const auto at = line.rfind(' ');
        LR_CHECK_MSG(at != std::string::npos, "no value on a metrics line: " + line);
        if (at == std::string::npos) {
            continue;
        }
        const std::string value = line.substr(at + 1);
        LR_CHECK_MSG(!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) ||
                                        value[0] == '-' || value[0] == '+'),
                     "a metrics value is not a number: " + line);
    }

    proxy.stop();
}

// The fourth ingress. It had no streamed coverage at all — and its streamed
// form had no conversion branch either, so a client asking for
// `stream: true` on /v1/responses received an empty body.
void group22ResponsesIngress(StubRelay &relay_a, int port) {
    LR_GROUP("22. the Responses ingress, streamed and not");
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();

    const std::string request =
        R"({"model":"route-model","input":[{"role":"user","content":[{"type":"input_text","text":"hi"}]}]})";
    const Hit plain = postJson(port, "/v1/responses", request);
    LR_CHECK_EQ(plain.status, 200);
    LR_CHECK_MSG(plain.body.find("\"object\":\"response\"") != std::string::npos,
                 "the answer was not converted back into the Responses shape: " +
                     literouter::truncateUtf8(plain.body, 200));

    relay_a.setMode(StubRelay::Mode::Stream);
    const std::string streamed =
        R"({"model":"route-model","stream":true,"input":[{"role":"user","content":[{"type":"input_text","text":"hi"}]}]})";
    const Hit hit = postJson(port, "/v1/responses", streamed);
    LR_CHECK_EQ(hit.status, 200);
    LR_CHECK_MSG(hit.body.find("response.output_text.delta") != std::string::npos,
                 "a streamed Responses request produced no output events: " +
                     literouter::truncateUtf8(hit.body, 200));
    LR_CHECK_MSG(hit.body.find("response.completed") != std::string::npos,
                 "the streamed Responses answer never completed: " +
                     literouter::truncateUtf8(hit.body, 200));
    relay_a.setMode(StubRelay::Mode::Normal);
}

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

    // 5. Vertex is Gemini's wire shape too: model aliases belong in its URL,
    // never in an extra JSON model field rejected by generateContent.
    {
        TempDir credentials;
        const auto key_file = credentials.path() / "vertex.json";
        const bool written = writeVertexCredentials(key_file, relay_a.baseUrl() + "/token");
        LR_CHECK_MSG(written, "could not create the local Vertex service-account fixture");
        if (!written) return;
        literouter::AppConfig config;
        config.server.host = "127.0.0.1";
        config.server.port = 0;
        config.server.persist_telemetry = false;
        literouter::ProviderConfig vertex;
        vertex.id = "vertex-local";
        vertex.protocol = "vertex";
        vertex.base_url = relay_a.baseUrl();
        vertex.project = "local-test";
        vertex.region = "test-region";
        vertex.credentials_file = key_file.string();
        config.providers = {vertex};
        literouter::RouteConfig route;
        route.model = "vertex-alias";
        route.targets = {{.provider = vertex.id, .model = "vertex-upstream"}};
        config.routes = {route};
        literouter::ProxyServer vertex_proxy;
        const auto started = vertex_proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (!started) return;
        const std::string raw = R"({ "contents": [{"role":"user","parts":[{"text":"hello vertex"}]}], "vendor_option": "preserved" })";
        const Hit hit = postJson(vertex_proxy.boundPort(), "/v1beta/models/vertex-alias:generateContent", raw);
        LR_CHECK_MSG(hit.transport_ok && hit.status == 200,
                     std::format("Vertex passthrough failed: status={} body={}", hit.status, hit.body));
        const json forwarded = json::parse(relay_a.lastChatBody(), nullptr, false);
        LR_CHECK_MSG(forwarded.is_object() && !forwarded.contains("model") && relay_a.lastChatBody() == raw,
                     "Vertex model alias changed its Gemini body: " + relay_a.lastChatBody());
        LR_CHECK_MSG(relay_a.lastChatPath().ends_with("/models/vertex-upstream:generateContent"),
                     "Vertex model alias was not applied to its URL: " + relay_a.lastChatPath());
        vertex_proxy.stop();
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

    // What the binary serves must be the build in web/dist, byte for byte. The
    // embedded copy is what a release actually ships — the directory is not
    // installed beside it — so a stale `web/dist` that was never rebuilt is a
    // console that silently differs from its own source, and nothing else in
    // the suite compares the two. Skipped when the checkout has no `web/dist`
    // (a build from a source tarball), and on the `$LITEROUTER_WEB_DIR` path,
    // where the assets come from disk by construction.
    {
        const auto asset_matches = [&](const std::string &url, const std::filesystem::path &file) {
            std::ifstream in{file, std::ios::binary};
            if (!in) {
                return true; // no dist to compare against
            }
            const std::string on_disk{std::istreambuf_iterator<char>{in},
                                      std::istreambuf_iterator<char>{}};
            const Hit hit = getPath(port, url);
            if (hit.status != 200 || hit.body != on_disk) {
                LR_NOTE(std::format("{} does not match {}", url, file.string()));
                return false;
            }
            return true;
        };
        LR_CHECK_MSG(asset_matches("/ui/", "web/dist/index.html"),
                     "the embedded index.html differs from web/dist");
        LR_CHECK_MSG(asset_matches("/ui/app.css", "web/dist/app.css"),
                     "the embedded app.css differs from web/dist");
        LR_CHECK_MSG(asset_matches("/ui/app.js", "web/dist/app.js"),
                     "the embedded app.js differs from web/dist");
        LR_CHECK_MSG(asset_matches("/ui/favicon.svg", "web/dist/favicon.svg"),
                     "the embedded favicon.svg differs from web/dist");
    }

    // The console shell is either embedded in the binary or read from
    // $LITEROUTER_WEB_DIR, and the two builds must both answer without ever
    // producing a 200 that carries no body. That empty 200 was the bug on the
    // fallback path: the handler ignored serveWebAsset()'s false, so a build
    // without the assets and without the variable served a blank page, which a
    // browser reports as a broken script rather than as a missing directory.
    //
    // Both builds are covered by one check because the branch is a compile-time
    // one. Embedded: the variable is irrelevant and both requests return the
    // real shell. Fallback: unset is an actionable 500, and a variable pointing
    // at the real web/dist serves the file on disk byte for byte.
    {
        const lr_test::EnvGuard webDirEnv{"LITEROUTER_WEB_DIR"};
        webDirEnv.clear();
        const Hit without = getPath(port, "/ui/");
        const Hit withDist = getPath(port, "/ui/");

        LR_CHECK_MSG(without.status == 200 || without.status == 500,
                     "the console shell answered with an unexpected status");

        if (without.status == 200) {
            // Embedded build: the shell is in the binary, so it is served
            // whether or not the variable is set.
            LR_CHECK_MSG(!without.body.empty(), "a 200 console shell carried no body");
            LR_CHECK_MSG(without.body.find("literouter console") != std::string::npos,
                         "the embedded shell is not the console page");
            LR_CHECK_EQ(withDist.status, 200);
        } else {
            // Fallback build with no variable: the failure has to be reported.
            LR_CHECK_MSG(without.body.find("LITEROUTER_WEB_DIR") != std::string::npos,
                         "the 500 does not name the variable that would fix it");
            webDirEnv.assign("web/dist");
            const Hit configured = getPath(port, "/ui/");
            LR_CHECK_EQ(configured.status, 200);
            LR_CHECK_MSG(!configured.body.empty(), "a configured web dir served no body");
            std::ifstream onDisk{"web/dist/index.html", std::ios::binary};
            LR_CHECK_MSG(static_cast<bool>(onDisk), "web/dist/index.html is missing");
            if (onDisk) {
                const std::string expected{std::istreambuf_iterator<char>{onDisk},
                                           std::istreambuf_iterator<char>{}};
                LR_CHECK_EQ(configured.body, expected);
            }
            // The script too, so the shell is not the only file that resolves.
            const Hit scriptFromDisk = getPath(port, "/ui/app.js");
            LR_CHECK_EQ(scriptFromDisk.status, 200);
            LR_CHECK_MSG(scriptFromDisk.body.find("/__literouter/status") != std::string::npos,
                         "app.js from $LITEROUTER_WEB_DIR is not the console script");
        }
    }

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

        // The trend survives too: a restart that erased the shape of the day
        // would make the chart useless exactly when someone is investigating.
        LR_CHECK_MSG(!restored.hourly.empty(),
                     "the hourly trend did not come back from the state file");
        if (!restored.hourly.empty()) {
            LR_CHECK_MSG(restored.hourly.back().requests >= 1,
                         "the restored hour holds no requests");
        }

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

// The local response cache, end to end: a repeated question must not be paid for
// twice.
void group29ResponseCache(StubRelay &relay_a) {
    LR_GROUP("29. a repeated request is answered from the local cache");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    // Off by default, which is the first thing to prove: a cache nobody asked
    // for is a stale answer nobody can explain.
    LR_CHECK_EQ(config.server.response_cache_ttl_sec, 0);

    literouter::ProviderConfig relay;
    relay.id = "cached";
    relay.base_url = relay_a.baseUrl();
    relay.timeout_sec = 10;
    relay.connect_timeout_sec = 2;
    config.providers = {relay};

    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "cached", .model = {}}};
    config.routes = {route};

    config.server.api_key = "admin-key";
    config.server.response_cache_ttl_sec = 300;
    config.server.response_cache_max_entries = 8;

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    const std::string request = chatRequest(kRouteModel);

    // First: a miss, and the answer is stored.
    const Hit first = postJson(port, "/v1/chat/completions", request, "admin-key");
    LR_CHECK_EQ(first.status, 200);
    LR_CHECK_EQ(first.cache, "miss");
    LR_CHECK_EQ(relay_a.chatRequests(), 1);

    // Second: a hit, byte-identical, and the relay is not asked again.
    const Hit second = postJson(port, "/v1/chat/completions", request, "admin-key");
    LR_CHECK_EQ(second.status, 200);
    LR_CHECK_EQ(second.cache, "hit");
    LR_CHECK_MSG(second.body == first.body, "a cached answer must be byte-identical");
    LR_CHECK_MSG(relay_a.chatRequests() == 1,
                 std::format("the relay was asked {} times", relay_a.chatRequests()));

    // The counters say so too, and no relay statistic moved for the hit.
    const literouter::Snapshot snapshot = proxy.snapshot();
    LR_CHECK_EQ(snapshot.cache_hits, std::uint64_t{1});
    LR_CHECK_EQ(snapshot.cache_misses, std::uint64_t{1});
    LR_CHECK_EQ(snapshot.cache_entries, std::uint64_t{1});
    LR_CHECK(snapshot.cache_enabled);
    const auto *stat = statOf(snapshot, "cached");
    LR_CHECK(stat != nullptr);
    if (stat != nullptr) {
        LR_CHECK_MSG(stat->requests == 1,
                     std::format("a cache hit must not count as a relay attempt ({})",
                                 stat->requests));
    }
    // The request is still counted, and the log says where it came from.
    LR_CHECK_EQ(snapshot.total_requests, std::uint64_t{2});
    bool saw_cache_entry = false;
    for (const auto &entry : proxy.logsSince(0, 500)) {
        if (entry.provider == "cache") {
            saw_cache_entry = true;
        }
    }
    LR_CHECK_MSG(saw_cache_entry, "the log must name the cache as the responder");

    // A different body is a different question.
    const std::string different =
        std::format(R"({{"model":"{}","messages":[{{"role":"user","content":"something else"}}]}})",
                    kRouteModel);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", different, "admin-key").cache, "miss");

    // A streamed request is never cached: a cached answer cannot be replayed as
    // an event stream without inventing timing the client would notice.
    const Hit streamed =
        postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true), "admin-key");
    LR_CHECK_EQ(streamed.status, 200);
    LR_CHECK_MSG(streamed.cache.empty(),
                 "a streaming response must carry no cache marker: " + streamed.cache);
    const std::uint64_t entries_before = proxy.snapshot().cache_entries;
    postJson(port, "/v1/chat/completions", chatRequest(kRouteModel, true), "admin-key");
    LR_CHECK_EQ(proxy.snapshot().cache_entries, entries_before);

    // A non-2xx answer is not stored: caching a relay's 400 would make a
    // transient upstream problem permanent for the TTL. A fresh body, because a
    // question already in the cache would be answered from it and never reach
    // the relay at all.
    relay_a.setMode(StubRelay::Mode::BadRequest);
    const std::string never_cached =
        std::format(R"({{"model":"{}","messages":[{{"role":"user","content":"not seen before"}}]}})",
                    kRouteModel);
    const Hit bad = postJson(port, "/v1/chat/completions", never_cached, "admin-key");
    LR_CHECK_EQ(bad.status, 400);
    LR_CHECK(bad.cache.empty());
    // And it really was not stored: asking again still reaches the relay.
    const int before_repeat = relay_a.chatRequests();
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", never_cached, "admin-key").status, 400);
    LR_CHECK_MSG(relay_a.chatRequests() == before_repeat + 1,
                 "a failed answer must not be cached");
    relay_a.setMode(StubRelay::Mode::Normal);

    // Identical JSON can select different relay behavior through business
    // headers. Only the effective forwarded values belong in the key.
    relay_a.setMode(StubRelay::Mode::HeaderEcho);
    const std::string header_request =
        std::format(R"({{"model":"{}","messages":[{{"role":"user","content":"header-sensitive"}}]}})",
                    kRouteModel);
    const httplib::Headers header_a{{"X-Custom-Feature", "alpha"}, {"User-Agent", "agent-a"}};
    const httplib::Headers header_a_reordered{{"user-AGENT", "agent-a"}, {"x-CUSTOM-feature", "alpha"},
                                             {"Cookie", "not-forwarded=different"}};
    const httplib::Headers header_b{{"X-Custom-Feature", "beta"}, {"User-Agent", "agent-a"}};
    const httplib::Headers header_c{{"X-Custom-Feature", "beta"}, {"User-Agent", "agent-b"}};
    const int before_headers = relay_a.chatRequests();
    const Hit variant_a = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_a);
    const Hit repeated_a = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_a_reordered);
    const Hit variant_b = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_b);
    const Hit variant_c = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_c);
    LR_CHECK_MSG(variant_a.cache == "miss" && repeated_a.cache == "hit" &&
                     repeated_a.body == variant_a.body,
                 std::format("header order/case changed the cache key: first={} repeat={}",
                             variant_a.cache, repeated_a.cache));
    LR_CHECK_MSG(variant_b.cache == "miss" && variant_b.body.find("beta|agent-a") != std::string::npos &&
                     variant_b.body != variant_a.body,
                 "a custom header reused the wrong cached answer: " + variant_b.body);
    LR_CHECK_MSG(variant_c.cache == "miss" && variant_c.body.find("beta|agent-b") != std::string::npos &&
                     variant_c.body != variant_b.body,
                 "User-Agent reused the wrong cached answer: " + variant_c.body);
    LR_CHECK_MSG(relay_a.chatRequests() == before_headers + 3,
                 std::format("header variants made {} relay calls, expected 3", relay_a.chatRequests() - before_headers));

    literouter::AppConfig overridden = config;
    overridden.providers[0].headers = {{"X-Custom-Feature", "fixed"}, {"User-Agent", "fixed-agent"}};
    proxy.updateConfig(overridden);
    const Hit fixed_first = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_a);
    const Hit fixed_repeat = postJson(port, "/v1/chat/completions", header_request, "admin-key", header_c);
    LR_CHECK_MSG(fixed_first.cache == "miss" && fixed_repeat.cache == "hit" &&
                     fixed_first.body.find("fixed|fixed-agent") != std::string::npos &&
                     fixed_repeat.body == fixed_first.body,
                 std::format("configured headers did not define the cached variant: first={} repeat={} body={}",
                             fixed_first.cache, fixed_repeat.cache, fixed_repeat.body));

    // Fields absent from the Chat intermediate still affect a same-protocol
    // Responses call. A previous response id selects a different conversation.
    relay_a.setMode(StubRelay::Mode::RequestEcho);
    literouter::AppConfig responses_config = config;
    responses_config.providers[0].protocol = "openai_responses";
    proxy.updateConfig(responses_config);
    json continuation{{"model", kRouteModel}, {"input", "continue"}, {"previous_response_id", "resp_first"}};
    const int before_contexts = relay_a.chatRequests();
    const Hit context_first = postJson(port, "/v1/responses", continuation.dump(), "admin-key");
    continuation["previous_response_id"] = "resp_second";
    const Hit context_second = postJson(port, "/v1/responses", continuation.dump(), "admin-key");
    const Hit context_repeat = postJson(port, "/v1/responses", continuation.dump(), "admin-key");
    LR_CHECK_MSG(context_first.status == 200 && context_first.cache == "miss" &&
                     context_second.status == 200 && context_second.cache == "miss" &&
                     context_repeat.cache == "hit" && context_repeat.body == context_second.body,
                 std::format("Responses contexts shared a cache entry: first={}/{} second={}/{} repeat={}",
                             context_first.status, context_first.cache, context_second.status,
                             context_second.cache, context_repeat.cache));
    LR_CHECK_MSG(context_first.body != context_second.body &&
                     context_second.body.find("resp_second") != std::string::npos &&
                     relay_a.chatRequests() == before_contexts + 2,
                 "the second Responses context did not reach the relay: " + context_second.body);

    // applyConfig keeps enabled cache entries; changing the relay's custom
    // path must select a new cache namespace while unchanged requests still hit.
    proxy.updateConfig(config);
    const std::string path_request = json{{"model", kRouteModel},
        {"messages", json::array({json{{"role", "user"}, {"content", "path-sensitive"}}})}}.dump();
    const int before_paths = relay_a.chatRequests();
    const Hit old_path = postJson(port, "/v1/chat/completions", path_request, "admin-key");
    literouter::AppConfig changed_path = config;
    changed_path.providers[0].chat_path = "/embeddings";
    proxy.updateConfig(changed_path);
    const Hit new_path = postJson(port, "/v1/chat/completions", path_request, "admin-key");
    const Hit repeated_path = postJson(port, "/v1/chat/completions", path_request, "admin-key");
    LR_CHECK_MSG(old_path.status == 200 && old_path.cache == "miss" &&
                     new_path.status == 200 && new_path.cache == "miss" &&
                     repeated_path.cache == "hit" && repeated_path.body == new_path.body,
                 std::format("custom endpoint edit reused a cache entry: old={}/{} new={}/{} repeat={}",
                             old_path.status, old_path.cache, new_path.status, new_path.cache, repeated_path.cache));
    LR_CHECK_MSG(old_path.body != new_path.body && relay_a.lastChatPath().ends_with("/embeddings") &&
                     relay_a.chatRequests() == before_paths + 2,
                 "the changed endpoint did not receive the request: " + new_path.body);
    relay_a.setMode(StubRelay::Mode::Normal);

    // Turning the cache off drops what it held.
    literouter::AppConfig off = config;
    off.server.response_cache_ttl_sec = 0;
    proxy.updateConfig(off);
    LR_CHECK(!proxy.snapshot().cache_enabled);
    LR_CHECK_EQ(proxy.snapshot().cache_entries, std::uint64_t{0});
    const int before_uncached = relay_a.chatRequests();
    const Hit uncached = postJson(port, "/v1/chat/completions", request, "admin-key");
    LR_CHECK_EQ(uncached.status, 200);
    LR_CHECK_MSG(uncached.cache.empty(), "a disabled cache must not mark anything");
    // It really did go to the relay, which is the other half of "not cached".
    LR_CHECK_EQ(relay_a.chatRequests(), before_uncached + 1);

    proxy.stop();
}

// Relay-side protection: a relay that is full is skipped rather than failed, and
// when every candidate is full the client is told to come back.
void group30RelayLimits(StubRelay &relay_a, StubRelay &relay_b) {
    LR_GROUP("30. a full relay is skipped, and a full chain answers 429 with a Retry-After");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;

    literouter::ProviderConfig limited;
    limited.id = "limited";
    limited.base_url = relay_a.baseUrl();
    limited.timeout_sec = 10;
    limited.connect_timeout_sec = 2;
    // One request per minute: the second request in this group cannot start on
    // this relay, which is the whole point.
    limited.requests_per_minute = 1;
    literouter::ProviderConfig backup;
    backup.id = "backup";
    backup.base_url = relay_b.baseUrl();
    backup.timeout_sec = 10;
    backup.connect_timeout_sec = 2;
    config.providers = {limited, backup};

    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "limited", .model = {}},
                     literouter::RouteTarget{.provider = "backup", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    relay_b.setMode(StubRelay::Mode::Normal);
    relay_a.resetCounters();
    relay_b.resetCounters();

    // The first request spends the limited relay's minute.
    const Hit first = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(first.status, 200);
    LR_CHECK_EQ(relay_a.chatRequests(), 1);
    LR_CHECK_EQ(relay_b.chatRequests(), 0);

    // The second is refused by the limiter and answered by the next candidate,
    // with no breaker tripped: the relay is full, not broken.
    const Hit second = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(second.status, 200);
    LR_CHECK_MSG(relay_a.chatRequests() == 1,
                 std::format("the full relay was tried again ({} times)", relay_a.chatRequests()));
    LR_CHECK_EQ(relay_b.chatRequests(), 1);
    const literouter::Snapshot after = proxy.snapshot();
    const auto *health = healthOf(after, "limited");
    LR_CHECK(health != nullptr);
    if (health != nullptr) {
        LR_CHECK_MSG(health->state != literouter::ProviderHealth::State::Open,
                     "a full relay must not have its breaker tripped");
        LR_CHECK_EQ(health->consecutive_failures, 0);
    }
    // And the request is recorded as a failover, with the reason in the log.
    bool saw_skip = false;
    for (const auto &entry : proxy.logsSince(0, 500)) {
        if (entry.message.find("skipping a full relay") != std::string::npos) {
            saw_skip = true;
        }
    }
    LR_CHECK_MSG(saw_skip, "the log must say why the first candidate was passed over");

    // Now take the backup's capacity away too: the chain is full, and the
    // client gets a 429 with a real Retry-After rather than a 503 that says
    // nothing about when to come back.
    literouter::AppConfig both_limited = config;
    both_limited.providers[1].requests_per_minute = 1;
    proxy.updateConfig(both_limited);
    const Hit refused = postJson(port, "/v1/chat/completions", chatRequest(kRouteModel));
    LR_CHECK_EQ(refused.status, 429);
    LR_CHECK_MSG(!refused.retry_after.empty(), "a full chain must name a Retry-After");
    LR_CHECK_MSG(refused.body.find("relay_capacity_exceeded") != std::string::npos,
                 "the error code must say it was capacity: " + refused.body);

    // A concurrency limit is accepted and a lone request still succeeds; the
    // limit itself is asserted directly in test_gates, where it can be observed
    // without racing the relay.
    literouter::AppConfig concurrent = config;
    concurrent.providers[0].requests_per_minute = 0;
    concurrent.providers[0].max_concurrent = 1;
    concurrent.providers[1].max_concurrent = 1;
    proxy.updateConfig(concurrent);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);

    proxy.stop();
}

// Liveness and readiness answer different questions, and the second one is
// allowed to say no.
void group31HealthProbes(StubRelay &relay_a) {
    LR_GROUP("31. liveness answers, readiness reports whether a relay can serve");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;

    literouter::ProviderConfig relay;
    relay.id = "only";
    relay.base_url = relay_a.baseUrl();
    relay.timeout_sec = 10;
    relay.connect_timeout_sec = 2;
    config.providers = {relay};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "only", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();

    // Both probes answer without a credential, because the things that run them
    // hold none.
    for (const char *path : {"/health", "/health/live", "/health/ready"}) {
        const Hit hit = getPath(port, path);
        LR_CHECK_MSG(hit.transport_ok && hit.status == 200,
                     std::format("{} answered {} (transport {})", path, hit.status,
                                 hit.transport_ok));
    }
    const Hit live = getPath(port, "/health/live");
    LR_CHECK(live.body.find("ok") != std::string::npos);
    const Hit ready = getPath(port, "/health/ready");
    LR_CHECK_MSG(ready.body.find("ready") != std::string::npos, ready.body);
    LR_CHECK_MSG(ready.body.find("enabled_relays\":true") != std::string::npos, ready.body);

    // With no enabled relay the instance can accept a connection and still
    // cannot serve anything, which is exactly what readiness is for.
    literouter::AppConfig disabled = config;
    disabled.providers[0].enabled = false;
    proxy.updateConfig(disabled);
    const Hit live_after = getPath(port, "/health/live");
    LR_CHECK_MSG(live_after.status == 200, "liveness is about the process, not the config");
    const Hit not_ready = getPath(port, "/health/ready");
    LR_CHECK_EQ(not_ready.status, 503);
    LR_CHECK_MSG(not_ready.body.find("no enabled relay") != std::string::npos, not_ready.body);

    proxy.stop();
}

// The traffic trend's bucket width and count are the operator's choice, so a
// relay being debugged right now can be watched at minute resolution.
void group32TrafficBuckets(StubRelay &relay_a) {
    LR_GROUP("32. the traffic trend follows server.traffic_bucket_sec and its count");
    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    // One minute wide, six of them: the last six minutes rather than the last
    // day.
    config.server.traffic_bucket_sec = 60;
    config.server.traffic_bucket_count = 6;
    LR_CHECK_MSG(literouter::validate(config).ok(),
                 "minute buckets must validate: " + literouter::validate(config).summary());

    literouter::ProviderConfig relay;
    relay.id = "trend";
    relay.base_url = relay_a.baseUrl();
    relay.timeout_sec = 10;
    relay.connect_timeout_sec = 2;
    config.providers = {relay};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "trend", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    for (int i = 0; i < 3; ++i) {
        LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);
    }

    const literouter::Snapshot snapshot = proxy.snapshot();
    LR_CHECK_EQ(snapshot.traffic_bucket_sec, 60);
    LR_CHECK_MSG(!snapshot.hourly.empty(), "a request must produce a bucket");
    if (!snapshot.hourly.empty()) {
        const auto &bucket = snapshot.hourly.back();
        // The width travels with the bucket, so a reader that restores a history
        // recorded at a different width cannot mislabel it.
        LR_CHECK_EQ(bucket.bucket_sec, 60);
        LR_CHECK_EQ(bucket.requests, std::uint64_t{3});
        // Floored to the minute, not to the hour.
        LR_CHECK_MSG(std::fmod(bucket.hour_unix, 60.0) == 0.0,
                     std::format("bucket start {} is not on a minute", bucket.hour_unix));
    }
    LR_CHECK_MSG(snapshot.hourly.size() <= 6, "the configured count is the cap");

    // Narrowing the window trims what is already held rather than waiting for
    // the next bucket boundary.
    literouter::AppConfig narrow = config;
    narrow.server.traffic_bucket_count = 2;
    proxy.updateConfig(narrow);
    LR_CHECK(proxy.snapshot().hourly.size() <= 2);

    proxy.stop();
}

// The OTLP push: the same numbers the pull endpoint exposes, sent to a
// collector the operator already runs.
void group33OtlpExport(StubRelay &relay_a) {
    LR_GROUP("33. metrics are pushed to an OTLP endpoint as OTLP JSON");

    // A stub collector. It records what it received, which is the only way to
    // assert the payload's shape rather than the exporter's opinion of it.
    httplib::Server collector;
    std::atomic<int> received{0};
    std::string payload;
    std::mutex payload_mutex;
    std::string path_seen;
    collector.Post("/v1/metrics", [&](const httplib::Request &req, httplib::Response &res) {
        {
            std::scoped_lock lock{payload_mutex};
            payload = req.body;
            path_seen = req.path;
        }
        ++received;
        res.status = 200;
        res.set_content("{}", "application/json");
    });
    const int collector_port = collector.bind_to_any_port("127.0.0.1");
    LR_CHECK_MSG(collector_port > 0, "the stub collector did not bind");
    if (collector_port <= 0) {
        return;
    }
    std::thread collector_thread([&collector] { collector.listen_after_bind(); });
    collector.wait_until_ready();

    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.pass_through_unknown = false;
    config.server.persist_telemetry = false;
    config.server.otlp_endpoint = std::format("http://127.0.0.1:{}", collector_port);
    LR_CHECK_MSG(literouter::validate(config).ok(),
                 "an otlp endpoint must validate: " + literouter::validate(config).summary());

    literouter::ProviderConfig relay;
    relay.id = "exported";
    relay.base_url = relay_a.baseUrl();
    relay.timeout_sec = 10;
    relay.connect_timeout_sec = 2;
    config.providers = {relay};
    literouter::RouteConfig route;
    route.model = kRouteModel;
    route.targets = {literouter::RouteTarget{.provider = "exported", .model = {}}};
    config.routes = {route};

    literouter::ProxyServer proxy;
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) {
        collector.stop();
        if (collector_thread.joinable()) {
            collector_thread.join();
        }
        return;
    }
    const int port = proxy.boundPort();
    relay_a.setMode(StubRelay::Mode::Normal);
    LR_CHECK_EQ(postJson(port, "/v1/chat/completions", chatRequest(kRouteModel)).status, 200);

    // The exporter runs on the flush tick, which is a few seconds.
    for (int attempt = 0; attempt < 200 && received.load() == 0; ++attempt) {
        std::this_thread::sleep_for(50ms);
    }
    LR_CHECK_MSG(received.load() > 0, "nothing was ever pushed to the collector");
    {
        std::scoped_lock lock{payload_mutex};
        LR_CHECK_EQ(path_seen, "/v1/metrics");
        const auto document = json::parse(payload, nullptr, false);
        LR_CHECK_MSG(!document.is_discarded(), "the OTLP body must be JSON: " + payload);
        if (!document.is_discarded()) {
            LR_CHECK(document.contains("resourceMetrics"));
            const auto &scope = document["resourceMetrics"][0]["scopeMetrics"][0];
            LR_CHECK(scope.contains("metrics"));
            bool saw_requests = false;
            bool saw_relay = false;
            for (const auto &metric : scope["metrics"]) {
                const std::string name = metric.value("name", std::string{});
                if (name == "literouter.requests") {
                    saw_requests = true;
                    LR_CHECK(metric.contains("sum"));
                    // CUMULATIVE, and declared monotonic: a collector that reads
                    // a counter as a gauge charts nonsense.
                    LR_CHECK_EQ(metric["sum"].value("aggregationTemporality", 0), 2);
                    LR_CHECK(metric["sum"].value("isMonotonic", false));
                }
                if (name == "literouter.relay.requests") {
                    saw_relay = true;
                    LR_CHECK(metric["sum"]["dataPoints"][0].contains("attributes"));
                }
            }
            LR_CHECK_MSG(saw_requests, "the headline counter is missing from the export");
            LR_CHECK_MSG(saw_relay, "the per-relay counter is missing from the export");
        }
    }

    // Clearing the endpoint stops the push: an operator who turns it off must
    // not keep leaking metrics to a collector.
    literouter::AppConfig off = config;
    off.server.otlp_endpoint.clear();
    proxy.updateConfig(off);
    const int before = received.load();
    std::this_thread::sleep_for(500ms);
    LR_CHECK_EQ(received.load(), before);

    proxy.stop();
    collector.stop();
    if (collector_thread.joinable()) {
        collector_thread.join();
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
        group21NoFieldLies(relay_a, relay_b);
        group22ResponsesIngress(relay_a, proxy.boundPort());
        group24MetricsEndpoint(relay_a);
        group25CostAccounting(relay_a, relay_b);
        group26SessionAffinity(relay_a, relay_b);
        group27ConfigHotReload(relay_a);
        group28RoutingPolicy(relay_a, relay_b);
        group29ResponseCache(relay_a);
        group30RelayLimits(relay_a, relay_b);
        group31HealthProbes(relay_a);
        group32TrafficBuckets(relay_a);
        group33OtlpExport(relay_a);
        group35ForwardedHeaders(relay_a);
        group36EmbeddingAlias(relay_a);
        group37LargeRequestWithoutExpect(relay_a);
        group38StreamingCapability(relay_a, relay_b);
#ifndef _WIN32
        group23RequestDeadline(relay_b);
#endif
#ifndef _WIN32
        group20ConnectionReuse();
#endif
        // Runs last on purpose: it deliberately leaves the proxy stopped.
        group10Restart(proxy, config);

        proxy.stop();
        LR_CHECK(!proxy.running());

        // Last, and inside the summary: it exercises lifecycle edges that are
        // only interesting once the ordinary paths are known good.
        group13NeverStarted(relay_a);

        // Last of all: it deliberately stops the server, and the group above
        // has already finished with the shared fixture.
        group34ShutdownEndpoint(relay_a);

        result = LR_SUMMARY("test_proxy");
    }

    relay_a.stop();
    relay_b.stop();
    hanger.stop();
    return result;
}
