// Outbound credential boundaries, exercised only against ephemeral loopback servers.
#include "../src/lr_http.h"
#include "lr_test_check.h"
#include "lr_test_socket.h"

import literouter.core;

namespace {
class Relay {
public:
    httplib::Server server;
    std::thread thread;
    int port = 0;
    ~Relay() { server.stop(); if (thread.joinable()) thread.join(); }
    bool start() {
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) return false;
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        return true;
    }
    std::string url() const { return std::format("http://127.0.0.1:{}", port); }
};
}

int main() {
    LR_GROUP("transport polling observes local shutdown without the full read timeout");
    {
        int port = 0;
        const auto listener = lr_test::listenLoopback(&port);
        const auto client = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        const bool connected = listener != lr_invalid_socket && client != lr_invalid_socket &&
            ::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        const auto peer = connected
            ? lrAccept(listener) : lr_invalid_socket;
        if (peer != lr_invalid_socket) {
            std::atomic<bool> waiting{false};
            double elapsed = 0;
            std::thread reader{[&] {
                waiting = true;
                const auto before = std::chrono::steady_clock::now();
                httplib::detail::select_read(client, 5, 0);
                elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
            }};
            while (!waiting.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
#if defined(_WIN32)
            lrCancelSocket(client, SD_BOTH);
#else
            lrShutdownBoth(client);
#endif
            reader.join();
            LR_CHECK_MSG(elapsed < 2, std::format("local shutdown needed {}s to wake transport polling", elapsed));
        } else {
            LR_CHECK_MSG(false, "loopback polling fixture could not connect");
        }
        if (peer != lr_invalid_socket) lrClose(peer);
        if (client != lr_invalid_socket) {
#if defined(_WIN32)
            lrCloseSocket(client);
#else
            lrClose(client);
#endif
        }
        if (listener != lr_invalid_socket) lrClose(listener);
    }
    lr_test::ProxyEnvironment proxyEnv;
    const lr_test::EnvGuard stateEnv{"LITEROUTER_STATE_DIR"};
    const auto state = std::filesystem::temp_directory_path() /
                       ("literouter-upstream-" + literouter::hexId(6));
    std::filesystem::create_directories(state);
    stateEnv.assign(literouter::pathToUtf8(state));
    Relay target, redirect;
    std::atomic<int> targetCalls{0};
    target.server.set_pre_routing_handler([&](const auto &, auto &res) {
        ++targetCalls;
        res.set_content(R"({"choices":[],"data":[]})", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
    LR_CHECK_MSG(target.start(), "redirect target did not bind");
    std::string location = target.url() + "/target";
    std::mutex locationMutex;
    redirect.server.set_pre_routing_handler([&](const auto &, auto &res) {
        std::scoped_lock lock{locationMutex};
        res.status = 307;
        res.set_header("Location", location);
        return httplib::Server::HandlerResponse::Handled;
    });
    LR_CHECK_MSG(redirect.start(), "redirect source did not bind");
    LR_GROUP("redirects never replay prompts or credentials on buffered, streamed or probe paths");
    for (const char *protocol : {"openai", "anthropic", "gemini", "azure"}) {
        literouter::ProviderConfig provider;
        provider.id = "redirect";
        provider.protocol = protocol;
        provider.base_url = redirect.url();
        provider.api_key = "fake-upstream-key";
        provider.chat_path = "/redirect";
        provider.timeout_sec = 2;
        provider.connect_timeout_sec = 2;
        provider.headers = {{"X-Private-Credential", "fake-custom-secret"}};
        const auto buffered = literouter::upstreamPost(provider, "/redirect", R"({"model":"m"})");
        LR_CHECK_MSG(!buffered.ok && buffered.error.find("redirects are disabled") != std::string::npos,
                     std::string{protocol} + ": " + buffered.error);
        const auto probe = literouter::probeProvider(provider, 2);
        LR_CHECK_MSG(probe.status == 307 && probe.detail.find("redirects are disabled") != std::string::npos,
                     std::string{protocol} + ": " + probe.detail);
        literouter::AppConfig config;
        config.server.port = 0;
        config.server.persist_telemetry = false;
        config.providers = {provider};
        config.routes = {{.model = "m", .targets = {{.provider = provider.id}}}};
        literouter::ProxyServer gateway;
        const auto started = gateway.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (started) {
            httplib::Client client{"127.0.0.1", gateway.boundPort()};
            client.set_read_timeout(5, 0);
            const auto reply = client.Post("/v1/chat/completions",
                R"({"model":"m","messages":[],"stream":true})", "application/json");
            LR_CHECK_MSG(reply && reply->status >= 400 &&
                         reply->body.find("redirects are disabled") != std::string::npos,
                         reply ? reply->body : "no streamed refusal");
            gateway.stop();
        }
        LR_CHECK_MSG(targetCalls.load() == 0, "a redirected target received credentials or a prompt");
    }
    LR_GROUP("same-origin redirects are refused consistently instead of looping");
    {
        std::scoped_lock lock{locationMutex};
        location = "/redirect";
    }
    literouter::ProviderConfig provider;
    provider.base_url = redirect.url();
    const auto result = literouter::upstreamPost(provider, "/redirect", "{}");
    LR_CHECK_MSG(!result.ok && result.error.find("redirects are disabled") != std::string::npos, result.error);
    LR_GROUP("stream credential failures do not send an unsigned request");
    literouter::AppConfig unsignedConfig;
    unsignedConfig.server.port = 0;
    unsignedConfig.server.persist_telemetry = false;
    provider.id = "unsigned";
    provider.protocol = "bedrock";
    provider.base_url = target.url();
    provider.region = "us-east-1";
    provider.aws_access_key.clear();
    provider.aws_secret_key.clear();
    unsignedConfig.providers = {provider};
    unsignedConfig.routes = {{.model = "m", .targets = {{.provider = "unsigned"}}}};
    literouter::ProxyServer unsignedGateway;
    const auto unsignedStart = unsignedGateway.start(unsignedConfig);
    LR_CHECK_MSG(unsignedStart.has_value(), unsignedStart ? "" : unsignedStart.error());
    if (unsignedStart) {
        httplib::Client client{"127.0.0.1", unsignedGateway.boundPort()};
        client.set_read_timeout(5);
        const auto reply = client.Post("/v1/chat/completions", R"({"model":"m","messages":[],"stream":true})",
                                       "application/json");
        LR_CHECK_MSG(reply && reply->status >= 400 && targetCalls.load() == 0,
                     "credential failure sent a request to the upstream");
        unsignedGateway.stop();
    }
    LR_GROUP("invalid authorities cannot construct a null management client");
    for (const char *address : {"http://127.0.0.1:0", "http://127.0.0.1:65536", "http://:80",
                                "http://user:pass@127.0.0.1", "http://[::1", "http://host:"}) {
        const auto status = literouter::fetchStatus(address);
        const auto logs = literouter::fetchLogs(address, 0);
        const auto reply = literouter::adminPost(address, "/shutdown");
        LR_CHECK_MSG(!status.reachable && !logs.reachable && !reply.reachable,
                     std::string{address} + " did not fail closed");
    }
    LR_GROUP("dynamic ports are discovered only for a verified matching config");
    literouter::AppConfig config;
    config.server.port = 0;
    config.server.persist_telemetry = false;
    const auto configPath = state / "config.json";
    literouter::ProxyServer gateway;
    gateway.setConfigPath(literouter::pathToUtf8(configPath));
    const auto started = gateway.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (started) {
        const auto url = literouter::runningServerBaseUrl(config.server, configPath);
        LR_CHECK_MSG(url && *url == gateway.snapshot().base_url, url ? *url : url.error());
        const auto other = literouter::runningServerBaseUrl(config.server, state / "other.json");
        LR_CHECK_MSG(!other, "discovery selected another config's listener");
        auto httpsExpected = config.server;
        httpsExpected.tls_cert_file = "https-required";
        const auto downgrade = literouter::runningServerBaseUrl(httpsExpected, configPath);
        LR_CHECK_MSG(!downgrade, "a plaintext PID record downgraded HTTPS management");
        literouter::ProxyServer second;
        second.setConfigPath(literouter::pathToUtf8(configPath));
        const auto secondStarted = second.start(config);
        LR_CHECK_MSG(secondStarted.has_value(), secondStarted ? "" : secondStarted.error());
        if (secondStarted) {
            LR_CHECK_MSG(!literouter::runningServerBaseUrl(config.server, configPath),
                         "ambiguous dynamic instances silently selected one listener");
            second.stop();
        }
        gateway.stop();
        LR_CHECK_MSG(!literouter::runningServerBaseUrl(config.server, configPath), "a stopped PID record was used");
    }
    for (const auto &host : {std::string{"0.0.0.0"}, std::string{"::"}}) {
        auto server = config.server;
        server.host = host;
        server.port = 9876;
        server.tls_cert_file = "https";
        const auto url = literouter::runningServerBaseUrl(server, {});
        LR_CHECK_MSG(url && *url == (host == "::" ? "https://[::1]:9876" : "https://127.0.0.1:9876"),
                     url ? *url : url.error());
    }
    std::error_code ec;
    std::filesystem::remove_all(state, ec);
    return LR_SUMMARY("test_upstream");
}
