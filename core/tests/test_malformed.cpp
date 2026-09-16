// Malformed-upstream blast radius: what a relay is allowed to do to the proxy,
// and what the proxy owes the caller when it does.
//
// This file exists because of a specific crash. A streamed request whose
// committed upstream status was >= 400 killed the process with
// `std::bad_function_call`: httplib's error handler injected an error body into
// a response that still had a chunked content provider installed, httplib
// resolved that contradictory framing by taking the Content-Length path, and
// that path never installs the sink's done() callback — which the provider then
// called. The fix is in place, and the only way to keep it in place is to keep
// feeding the proxy the shapes that produced it.
//
// The relay behaviours below are all things a real, broken or hostile relay can
// do on the wire. Each one is asserted against two things: what the CLIENT
// receives, and whether the PROCESS is still serving afterwards.
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "lr_test_check.h"

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

// ── a relay that answers with raw bytes and then hangs up ────────────────────
//
// httplib's Server cannot produce any of the interesting malformed shapes: it
// will not lie about Content-Length, and it always terminates a chunked body.
// So these cases are served from a raw socket that writes exactly what it is
// given. That is the point — the proxy has to cope with a peer that is not
// playing by the rules, and it cannot be tested with a peer that is.
#ifndef _WIN32
class RawRelay {
public:
    RawRelay() = default;
    ~RawRelay() { stop(); }
    RawRelay(const RawRelay &) = delete;
    RawRelay &operator=(const RawRelay &) = delete;

    bool start(std::string canned) {
        canned_ = std::move(canned);
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
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::fcntl(listen_fd_, F_SETFL, ::fcntl(listen_fd_, F_GETFL, 0) | O_NONBLOCK);
        served_.store(0);
        thread_ = std::thread([this] {
            while (!stopped_.load()) {
                pollfd descriptor{listen_fd_, POLLIN, 0};
                if (::poll(&descriptor, 1, 25) <= 0) {
                    continue;
                }
                const int client = ::accept(listen_fd_, nullptr, nullptr);
                if (client < 0) {
                    continue;
                }
                timeval timeout{};
                timeout.tv_sec = 2;
                ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                char buffer[4096];
                ::recv(client, buffer, sizeof(buffer), 0);
                ::send(client, canned_.data(), canned_.size(), MSG_NOSIGNAL);
                served_.fetch_add(1);
                ::close(client);
            }
        });
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

    std::string baseUrl() const { return std::format("http://127.0.0.1:{}", port_); }
    int connectionsServed() const { return served_.load(); }

private:
    std::string canned_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopped_{false};
    std::atomic<int> served_{0};
    std::thread thread_;
};
#endif

// ── a well-behaved relay, so every scenario can also prove failover ──────────

class HealthyRelay {
public:
    HealthyRelay() {
        server_.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
            res.status = 200;
            res.set_content(R"({"id":"healthy","choices":[]})", "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~HealthyRelay() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    HealthyRelay(const HealthyRelay &) = delete;
    HealthyRelay &operator=(const HealthyRelay &) = delete;

    std::string baseUrl() const { return std::format("http://127.0.0.1:{}", port_); }
    int port() const { return port_; }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
};

// ── the shape of one exchange ────────────────────────────────────────────────

struct Exchange {
    bool client_ok = false;
    int status = 0;
    std::string content_type;
    std::string body;
};

Exchange post(httplib::Client &client, bool stream) {
    Exchange out;
    const std::string body = stream ? R"({"model":"m","stream":true})" : R"({"model":"m"})";
    auto result = client.Post("/v1/chat/completions",
                              httplib::Headers{{"Content-Type", "application/json"}}, body,
                              "application/json");
    if (!result) {
        return out;
    }
    out.client_ok = true;
    out.status = result->status;
    out.content_type = result->get_header_value("Content-Type");
    out.body = result->body;
    return out;
}

// The one assertion this whole file exists to make: whatever the relay did, the
// proxy is still in business.
bool stillServing(const std::string &host, int port) {
    httplib::Client probe{host, port};
    probe.set_connection_timeout(2, 0);
    probe.set_read_timeout(3, 0);
    auto result = probe.Get("/health");
    return result && result->status == 200;
}

// `requests == successes + failures + aborted` for every relay. Cheap, and it
// catches an accounting bug the moment a new path forgets one of the three.
bool accountingHolds(const literouter::Snapshot &snapshot, std::string &detail) {
    for (const auto &stat : snapshot.providers) {
        const auto total = stat.successes + stat.failures + stat.aborted;
        if (stat.requests != total) {
            detail = std::format("{}: requests={} but successes+failures+aborted={}",
                                 stat.provider, stat.requests, total);
            return false;
        }
    }
    return true;
}

const literouter::ProviderStat *statOf(const literouter::Snapshot &snapshot,
                                       std::string_view id) {
    for (const auto &stat : snapshot.providers) {
        if (stat.provider == id) {
            return &stat;
        }
    }
    return nullptr;
}

// ── the harness every scenario shares ────────────────────────────────────────

// One proxy, two relays: `one` is the malformed one under test, `two` is a
// healthy fallback so a failover is observable rather than merely survivable.
struct Fixture {
    HealthyRelay healthy;
    literouter::ProxyServer proxy;
    std::unique_ptr<httplib::Client> client;

    bool start(const std::string &relay_one_base_url) {
        literouter::AppConfig config;
        config.server.host = "127.0.0.1";
        config.server.port = 0;
        config.server.pass_through_unknown = false;
        config.server.log_capacity = 64;
        // Off: this suite asserts absolute counters ("the request was
        // double-counted or lost"), and these fixtures reuse one process, so a
        // state file written by the scenario before would be read back by the
        // next one and inflate them.
        config.server.persist_telemetry = false;

        literouter::ProviderConfig one;
        one.id = "one";
        one.base_url = relay_one_base_url;
        one.timeout_sec = 4;
        one.connect_timeout_sec = 2;

        literouter::ProviderConfig two;
        two.id = "two";
        two.base_url = healthy.baseUrl();
        two.timeout_sec = 4;
        two.connect_timeout_sec = 2;

        config.providers = {one, two};
        literouter::RouteConfig route;
        route.model = "m";
        route.targets = {literouter::RouteTarget{.provider = "one", .model = {}},
                         literouter::RouteTarget{.provider = "two", .model = {}}};
        config.routes = {route};

        if (auto started = proxy.start(config); !started) {
            std::printf("   proxy start failed: %s\n", started.error().c_str());
            return false;
        }
        client = std::make_unique<httplib::Client>("127.0.0.1", proxy.boundPort());
        client->set_connection_timeout(2, 0);
        client->set_read_timeout(8, 0);
        return true;
    }

    literouter::Snapshot finish(const char *scenario) {
        auto snapshot = proxy.snapshot();
        for (int i = 0; i < 30 && snapshot.active_requests > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            snapshot = proxy.snapshot();
        }
        std::string detail;
        LR_CHECK_MSG(accountingHolds(snapshot, detail), detail);
        LR_CHECK_MSG(snapshot.active_requests == 0,
                     std::format("{} left {} request(s) in flight", scenario,
                                 snapshot.active_requests));
        LR_CHECK_MSG(stillServing("127.0.0.1", proxy.boundPort()),
                     std::format("{} killed the proxy: it no longer answers /health", scenario));
        return snapshot;
    }
};

// ── scenarios ────────────────────────────────────────────────────────────────

// A relay that closes before sending everything it promised. httplib sees a
// short read, which is a transport failure, which is a retryable one — so the
// caller must get the NEXT relay's answer, not a truncated one.
void testTruncatedBodyFailsOver() {
    lr_test::group("a relay that promises 1000 bytes and sends 11 fails over cleanly");
#ifndef _WIN32
    RawRelay raw;
    LR_CHECK(raw.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                       "Content-Length: 1000\r\n\r\n{\"id\":\"x\"}"));

    Fixture fixture;
    if (!fixture.start(raw.baseUrl())) {
        return;
    }
    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_MSG(exchange.status == 200, "the caller got no answer at all");
    LR_CHECK_MSG(exchange.body.find("healthy") != std::string::npos,
                 std::format("the caller got something other than the second relay's body: [{}]",
                             literouter::truncateUtf8(exchange.body, 120)));

    const auto snapshot = fixture.finish("truncated body");
    if (const auto *one = statOf(snapshot, "one"); one != nullptr) {
        LR_CHECK_MSG(one->failures == 1, "the truncating relay was not recorded as a failure");
        LR_CHECK_MSG(one->successes == 0, "a truncated body was recorded as a success");
    }
    if (const auto *two = statOf(snapshot, "two"); two != nullptr) {
        LR_CHECK_MSG(two->retries_in == 1, "the second relay did not record the failover it caught");
    }
    raw.stop();
#endif
}

// Headers, then silence. Same class as the truncated body but with nothing at
// all after the header block — the proxy must not wait forever for a body that
// is never coming.
void testHeadersThenCloseFailsOver() {
    lr_test::group("a relay that sends headers then hangs up fails over cleanly");
#ifndef _WIN32
    RawRelay raw;
    LR_CHECK(raw.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                       "Content-Length: 1000\r\n"));

    Fixture fixture;
    if (!fixture.start(raw.baseUrl())) {
        return;
    }
    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_MSG(exchange.status == 200, "the caller got no answer at all");
    LR_CHECK_MSG(exchange.body.find("healthy") != std::string::npos,
                 std::format("expected the fallback relay's body, got [{}]",
                             literouter::truncateUtf8(exchange.body, 120)));
    fixture.finish("headers then close");
    raw.stop();
#endif
}

// A 200 with an empty body is not a transport failure and must not be retried:
// the relay answered, it just had nothing to say. The caller sees that verbatim.
void testEmpty200IsPassedThrough() {
    lr_test::group("a 200 with an empty body is passed through, not failed over");
    httplib::Server empty;
    empty.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
        res.status = 200; // deliberately no body
    });
    const int port = empty.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { empty.listen_after_bind(); });
    empty.wait_until_ready();

    Fixture fixture;
    if (!fixture.start(std::format("http://127.0.0.1:{}", port))) {
        empty.stop();
        thread.join();
        return;
    }

    for (const bool stream : {false, true}) {
        const Exchange exchange = post(*fixture.client, stream);
        LR_CHECK_MSG(exchange.client_ok, "the proxy dropped an empty-bodied request");
        LR_CHECK_MSG(exchange.status == 200, "an empty 200 did not stay a 200");
        LR_CHECK_MSG(exchange.body.empty(),
                     std::format("body was not empty: [{}]",
                                 literouter::truncateUtf8(exchange.body, 80)));
    }

    const auto snapshot = fixture.finish("empty 200");
    if (const auto *two = statOf(snapshot, "two"); two != nullptr) {
        LR_CHECK_MSG(two->requests == 0, "an empty 200 was retried on the second relay");
    }

    empty.stop();
    thread.join();
}

// A 200 whose body is not JSON at all. The proxy is a pipe, not a validator —
// it must hand the bytes over unchanged rather than inventing an error.
void testGarbageBodyIsPassedThrough() {
    lr_test::group("a 200 with a non-JSON body reaches the caller verbatim");
    httplib::Server garbage;
    garbage.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
        res.status = 200;
        res.set_content("this is not JSON at all\x01\x02", "application/json");
    });
    const int port = garbage.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { garbage.listen_after_bind(); });
    garbage.wait_until_ready();

    Fixture fixture;
    if (!fixture.start(std::format("http://127.0.0.1:{}", port))) {
        garbage.stop();
        thread.join();
        return;
    }

    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_MSG(exchange.status == 200, "a garbage body changed the status");
    LR_CHECK_MSG(exchange.body == "this is not JSON at all\x01\x02",
                 std::format("the body was rewritten: [{}]",
                             literouter::truncateUtf8(exchange.body, 80)));
    fixture.finish("garbage body");

    garbage.stop();
    thread.join();
}

// An SSE stream that ends without its terminating chunk. Everything that
// arrived belongs to the caller; the proxy must close the stream rather than
// hang the client waiting for a frame that never comes.
void testStreamWithoutTerminatorCloses() {
    lr_test::group("an SSE stream with no terminating chunk still closes");
#ifndef _WIN32
    RawRelay raw;
    LR_CHECK(raw.start("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"
                       "1e\r\ndata: {\"id\":\"one\"}\n\n\r\n"));

    Fixture fixture;
    if (!fixture.start(raw.baseUrl())) {
        return;
    }
    const Exchange exchange = post(*fixture.client, /*stream=*/true);
    LR_CHECK_MSG(exchange.client_ok, "the client never got a response object");
    LR_CHECK_MSG(exchange.status == 200, "a truncated stream changed the status");
    LR_CHECK_MSG(exchange.body.find("data:") != std::string::npos,
                 "the frames that did arrive were dropped");
    fixture.finish("stream without terminator");
    raw.stop();
#endif
}

// The scenario that killed the process: a streamed request committed to a
// non-2xx relay. Covered in more depth by test_proxy.cpp; repeated here because
// this file is the one that lists what a broken peer can do, and "answered with
// an error while streaming" belongs on that list.
void testCommittedErrorOnStreamSurvives() {
    lr_test::group("a streamed request committed to a 5xx relay survives and reports it");
    httplib::Server failing;
    failing.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
        res.status = 503;
        res.set_content(R"({"error":{"message":"upstream is down"}})", "application/json");
    });
    const int port = failing.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { failing.listen_after_bind(); });
    failing.wait_until_ready();

    Fixture fixture;
    if (!fixture.start(std::format("http://127.0.0.1:{}", port))) {
        failing.stop();
        thread.join();
        return;
    }

    // Single-hop effective route: the 503 is retryable, so the proxy will try
    // relay `two` — but `two` is healthy, so this exercises the retry rather
    // than the commit. To commit, aim at a model only relay `one` serves.
    literouter::AppConfig single = fixture.proxy.config();
    single.routes[0].targets = {literouter::RouteTarget{.provider = "one", .model = {}}};
    fixture.proxy.updateConfig(single);

    const Exchange exchange = post(*fixture.client, /*stream=*/true);
    LR_CHECK_MSG(exchange.client_ok, "the proxy dropped the committed-error stream");
    LR_CHECK_MSG(exchange.status == 503, "the relay's own status did not survive");
    LR_CHECK_MSG(exchange.content_type.find("application/json") != std::string::npos,
                 std::format("a committed error was mislabelled as [{}]", exchange.content_type));
    LR_CHECK_MSG(exchange.body.find("upstream is down") != std::string::npos,
                 std::format("the relay's error text was lost: [{}]",
                             literouter::truncateUtf8(exchange.body, 120)));
    fixture.finish("committed error on a stream");

    failing.stop();
    thread.join();
}

// A relay that returns HTTP 403 with Cloudflare WAF block HTML. The proxy must
// recognise Cloudflare WAF protection as an upstream failure and fail over to
// the healthy second relay.
void testCloudflareBlockedFailsOver() {
    lr_test::group("a Cloudflare 403 WAF block fails over to the next relay");
    httplib::Server cf_server;
    cf_server.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
        res.status = 403;
        res.set_header("Server", "cloudflare");
        res.set_header("CF-RAY", "test-ray-id-12345");
        res.set_content(
            "<!DOCTYPE html><html><head><title>Attention Required! | Cloudflare</title></head>"
            "<body><h1>Sorry, you have been blocked</h1><h2>You are unable to access example.com</h2></body></html>",
            "text/html");
    });
    const int port = cf_server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { cf_server.listen_after_bind(); });
    cf_server.wait_until_ready();

    Fixture fixture;
    if (!fixture.start(std::format("http://127.0.0.1:{}", port))) {
        cf_server.stop();
        thread.join();
        return;
    }

    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_MSG(exchange.status == 200, "a Cloudflare 403 block was not failed over");
    LR_CHECK_MSG(exchange.body.find("healthy") != std::string::npos, "the fallback relay was not called");

    const auto snapshot = fixture.finish("cloudflare 403 failover");
    if (const auto *one = statOf(snapshot, "one"); one != nullptr) {
        LR_CHECK_MSG(one->failures == 1, "the blocked relay was not counted as a failure");
        LR_CHECK_MSG(one->successes == 0, "the blocked relay was wrongly counted as a success");
    }
    if (const auto *two = statOf(snapshot, "two"); two != nullptr) {
        LR_CHECK_MSG(two->successes == 1, "the fallback relay was not credited with a success");
    }

    cf_server.stop();
    thread.join();
}

// When every relay fails or there is no fallback, a Cloudflare 403 block HTML
// must be converted into standard OpenAI JSON format instead of dumping raw HTML.
void testCloudflareBlockedTerminalConvertsToJson() {
    lr_test::group("a terminal Cloudflare 403 block converts HTML into OpenAI JSON");
    httplib::Server cf_server;
    cf_server.Post("/chat/completions", [](const httplib::Request &, httplib::Response &res) {
        res.status = 403;
        res.set_header("Server", "cloudflare");
        res.set_header("CF-RAY", "test-ray-id-12345");
        res.set_content(
            "<!DOCTYPE html><html><head><title>Attention Required! | Cloudflare</title></head>"
            "<body><h1>Sorry, you have been blocked</h1><h2>You are unable to access example.com</h2></body></html>",
            "text/html");
    });
    const int port = cf_server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { cf_server.listen_after_bind(); });
    cf_server.wait_until_ready();

    Fixture fixture;
    if (!fixture.start(std::format("http://127.0.0.1:{}", port))) {
        cf_server.stop();
        thread.join();
        return;
    }

    // Aim strictly at relay `one` so it commits as terminal.
    literouter::AppConfig single = fixture.proxy.config();
    single.routes[0].targets = {literouter::RouteTarget{.provider = "one", .model = {}}};
    fixture.proxy.updateConfig(single);

    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_EQ(exchange.status, 403);
    LR_CHECK_MSG(exchange.content_type.find("application/json") != std::string::npos,
                 std::format("terminal error had content type [{}] instead of application/json",
                             exchange.content_type));
    LR_CHECK_MSG(exchange.body.find("<!DOCTYPE") == std::string::npos,
                 "raw HTML was leaked to the caller");
    LR_CHECK_MSG(exchange.body.find("Sorry, you have been blocked") != std::string::npos,
                 "the error message did not contain the extracted heading");
    LR_CHECK_MSG(exchange.body.find("test-ray-id-12345") != std::string::npos,
                 "the error message did not contain the Cloudflare Ray ID");
    LR_CHECK_MSG(exchange.body.find("cf_blocked") != std::string::npos,
                 "the error code was not cf_blocked");

    const auto snapshot = fixture.finish("cloudflare 403 terminal JSON");
    if (const auto *one = statOf(snapshot, "one"); one != nullptr) {
        LR_CHECK_MSG(one->failures == 1, "terminal 403 was not recorded as failure");
        LR_CHECK_MSG(one->successes == 0, "terminal 403 was wrongly recorded as success");
    }

    cf_server.stop();
    thread.join();
}

// Finally: the proxy must still do normal work after all of the above. A suite
// that only proves "it survived" would pass against a proxy that answered
// nothing at all.
void testStillWorksAfterwards() {
    lr_test::group("the proxy still serves a normal request after every abuse above");
    Fixture fixture;
    if (!fixture.start(fixture.healthy.baseUrl())) {
        return;
    }
    const Exchange exchange = post(*fixture.client, /*stream=*/false);
    LR_CHECK(exchange.client_ok);
    LR_CHECK_MSG(exchange.status == 200, "a healthy request failed");
    LR_CHECK_MSG(exchange.body.find("healthy") != std::string::npos, "the wrong body came back");

    const auto snapshot = fixture.finish("normal request after abuse");
    LR_CHECK_MSG(snapshot.total_requests == 1, "the request was double-counted or lost");
    LR_CHECK_MSG(snapshot.total_success == 1, "a successful request was counted as something else");
}

} // namespace

int main() {
    // The fixtures below switch persistence off; the guard means a regression
    // still cannot write into the developer's own state directory.
    const lr_test::EnvGuard stateEnv{"LITEROUTER_STATE_DIR"};
    stateEnv.assign(
        (std::filesystem::temp_directory_path() / "literouter-test-malformed").string());

    testTruncatedBodyFailsOver();
    testHeadersThenCloseFailsOver();
    testEmpty200IsPassedThrough();
    testGarbageBodyIsPassedThrough();
    testStreamWithoutTerminatorCloses();
    testCommittedErrorOnStreamSurvives();
    testCloudflareBlockedFailsOver();
    testCloudflareBlockedTerminalConvertsToJson();
    testStillWorksAfterwards();
    return LR_SUMMARY("test_malformed");
}
