// Real sockets and binary MIME uploads exercise the same routing and header
// gate used in production, including the boundary between retry and commitment.
#include <httplib.h>
#include "lr_test_check.h"

import literouter.core;
import nlohmann.json;

namespace {
using namespace literouter;
using json = nlohmann::json;

struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("literouter-media-" + hexId(8));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct Reply {
    int status = 200;
    std::string body = R"({"text":"hello"})";
    std::string type = "application/json";
    bool chunked = false;
    bool cut = false;
};

struct Captured {
    std::string path;
    std::string body;
    std::string key;
    std::string type;
    std::vector<httplib::FormData> parts;
};

class MediaStub {
public:
    httplib::Server server;
    std::thread worker;
    int port = 0;
    std::atomic<int> requests{0};
    std::mutex mutex;
    Captured captured;
    Reply reply;

    bool start() {
        server.Post(R"(/api/v1/(audio|images)/.*)",
                    [this](const httplib::Request &req, httplib::Response &res,
                           const httplib::ContentReader &reader) {
            Captured seen;
            seen.path = req.path;
            seen.key = req.get_header_value("Authorization");
            seen.type = req.get_header_value("Content-Type");
            bool read = false;
            if (req.is_multipart_form_data()) {
                read = reader([&](const httplib::FormData &part) {
                    seen.parts.push_back(part);
                    return true;
                }, [&](const char *data, std::size_t size) {
                    seen.parts.back().content.append(data, size);
                    return true;
                });
            } else {
                read = reader([&](const char *data, std::size_t size) {
                    seen.body.append(data, size);
                    return true;
                });
            }
            Reply current;
            {
                std::scoped_lock lock{mutex};
                captured = std::move(seen);
                current = reply;
            }
            ++requests;
            if (!read) { res.status = 400; return; }
            res.status = current.status;
            res.set_header("X-Request-Id", "media-stub");
            res.set_header("Retry-After", "1");
            if (current.chunked) {
                res.set_chunked_content_provider(current.type,
                    [current](std::size_t offset, httplib::DataSink &sink) {
                        const auto split = current.body.size() / 2;
                        if (offset == 0) return sink.write(current.body.data(), split);
                        if (current.cut) return false;
                        if (!sink.write(current.body.data() + split, current.body.size() - split)) return false;
                        sink.done();
                        return true;
                    });
            } else {
                res.set_content(current.body, current.type);
            }
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) return false;
        worker = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        return true;
    }
    ~MediaStub() { server.stop(); if (worker.joinable()) worker.join(); }
    std::string url() const { return std::format("http://127.0.0.1:{}/api/v1", port); }
    void set(Reply next = {}) {
        std::scoped_lock lock{mutex};
        reply = std::move(next);
        captured = {};
        requests = 0;
    }
    Captured last() { std::scoped_lock lock{mutex}; return captured; }
};

AppConfig configuration(const MediaStub &a, const MediaStub &b) {
    AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = 0;
    cfg.server.api_key = "client-key";
    cfg.server.persist_telemetry = false;
    cfg.server.log_bodies = true;
    cfg.server.pass_through_unknown = false;
    for (const auto &[id, url] : std::vector<std::pair<std::string, std::string>>{{"a", a.url()}, {"b", b.url()}}) {
        ProviderConfig provider;
        provider.id = id;
        provider.base_url = url;
        provider.api_key = "upstream-key";
        provider.timeout_sec = 3;
        provider.connect_timeout_sec = 1;
        cfg.providers.push_back(provider);
    }
    for (const char *model : {"media", "dall-e-2"}) {
        RouteConfig route;
        route.model = model;
        route.targets = {{"a", "upstream-a"}, {"b", "upstream-b"}};
        cfg.routes.push_back(route);
    }
    return cfg;
}

std::string multipart(const std::string &binary, bool model = true, bool stream = false) {
    std::string body;
    const auto part = [&](std::string_view headers, std::string_view content) {
        body += "--test-boundary\r\n";
        body += headers;
        body += "\r\n\r\n";
        body += content;
        body += "\r\n";
    };
    part("Content-Disposition: form-data; name=\"file\"; filename=\"clip.wav\"\r\nContent-Type: audio/wav\r\nX-Part-Meta: original", binary);
    if (model) part("Content-Disposition: form-data; name=\"model\"", "media");
    part("Content-Disposition: form-data; name=\"timestamp_granularities[]\"", "word");
    part("Content-Disposition: form-data; name=\"image[]\"; filename=\"one.png\"\r\nContent-Type: image/png", binary);
    part("Content-Disposition: form-data; name=\"timestamp_granularities[]\"", "segment");
    part("Content-Disposition: form-data; name=\"image[]\"; filename*=UTF-8''two%20images.png\r\nContent-Type: image/png", binary + "second");
    if (stream) part("Content-Disposition: form-data; name=\"stream\"", "true");
    body += "--test-boundary--\r\n";
    return body;
}

bool settled(ProxyServer &proxy) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        if (proxy.snapshot().active_requests == 0) return true;
        std::this_thread::yield();
    }
    return false;
}
} // namespace

int main() {
    Scratch scratch;
    lr_test::EnvGuard state_dir{"LITEROUTER_STATE_DIR"};
    state_dir.assign(scratch.path.string());
    MediaStub first, second;
    LR_CHECK(first.start());
    LR_CHECK(second.start());
    auto cfg = configuration(first, second);
    ProxyServer proxy;
    LR_CHECK(proxy.start(cfg).has_value());
    httplib::Client client{proxy.boundAddress(), proxy.boundPort()};
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    client.set_default_headers({{"Authorization", "Bearer client-key"}});
    const std::string binary = std::string{"RIFF\0\xff\x80\r\n", 9} + "binary-payload";
    const std::string upload = multipart(binary);
    constexpr auto mime = "multipart/form-data; boundary=\"test-boundary\"";

    LR_GROUP("multipart endpoints preserve binary files, order, duplicate fields and model mapping");
    for (const char *endpoint : {"/v1/audio/transcriptions", "/v1/audio/translations", "/v1/images/edits", "/v1/images/variations"}) {
        first.set(); second.set(); proxy.resetStats();
        auto response = client.Post(endpoint, upload, mime);
        LR_CHECK(response && response->status == 200);
        if (response) LR_CHECK_EQ(response->body, R"({"text":"hello"})");
        const auto seen = first.last();
        LR_CHECK_EQ(seen.path, "/api" + std::string{endpoint});
        LR_CHECK_EQ(seen.key, "Bearer upstream-key");
        LR_CHECK_EQ(seen.parts.size(), 6);
        if (seen.parts.size() == 6) {
            LR_CHECK_EQ(seen.parts[0].content, binary);
            LR_CHECK_EQ(seen.parts[0].filename, "clip.wav");
            LR_CHECK_EQ(seen.parts[0].content_type, "audio/wav");
            const auto part_header = seen.parts[0].headers.find("X-Part-Meta");
            LR_CHECK(part_header != seen.parts[0].headers.end());
            if (part_header != seen.parts[0].headers.end()) LR_CHECK_EQ(part_header->second, "original");
            LR_CHECK_EQ(seen.parts[1].content, "upstream-a");
            LR_CHECK_EQ(seen.parts[2].content, "word");
            LR_CHECK_EQ(seen.parts[3].content, binary);
            LR_CHECK_EQ(seen.parts[4].content, "segment");
            LR_CHECK_EQ(seen.parts[5].content, binary + "second");
            LR_CHECK_EQ(seen.parts[5].filename, "two images.png");
        }
        LR_CHECK_EQ(second.requests, 0);
        LR_CHECK(settled(proxy));
        auto logs = proxy.logsSince(0);
        for (const auto &log : logs) {
            LR_CHECK(log.request_body.find("binary-payload") == std::string::npos);
        }
        auto admin = client.Get("/__literouter/logs");
        LR_CHECK(admin && admin->status == 200 && !json::parse(admin->body, nullptr, false).is_discarded());
    }

    LR_GROUP("text/subtitle transcriptions keep their upstream MIME and bytes");
    first.set({200, "1\n00:00:00,000 --> 00:00:00,500\nhello\n", "text/plain; charset=utf-8"});
    proxy.resetStats();
    auto subtitle = client.Post("/v1/audio/transcriptions", upload, mime);
    LR_CHECK(subtitle && subtitle->status == 200);
    if (subtitle) {
        LR_CHECK_EQ(subtitle->body, "1\n00:00:00,000 --> 00:00:00,500\nhello\n");
        LR_CHECK_EQ(subtitle->get_header_value("Content-Type"), "text/plain; charset=utf-8");
    }

    LR_GROUP("uploads larger than the former JSON limit reach the upstream intact");
    first.set(); proxy.resetStats();
    const std::string large_binary(6 * 1024 * 1024, '\xff');
    auto large = client.Post("/v1/images/edits", multipart(large_binary), mime);
    LR_CHECK(large && large->status == 200);
    const auto large_seen = first.last();
    LR_CHECK_EQ(large_seen.parts.size(), 6);
    if (large_seen.parts.size() == 6) LR_CHECK(large_seen.parts[0].content == large_binary);

    LR_GROUP("JSON image requests retain parameters and upstream-reported usage");
    first.set({200, R"({"data":[{"b64_json":"AAEA"}],"usage":{"input_tokens":12,"output_tokens":7}})"});
    proxy.resetStats();
    auto generated = client.Post("/v1/images/generations", R"({"model":"media","prompt":"a hill","size":"1024x1024","n":2})", "application/json");
    LR_CHECK(generated && generated->status == 200);
    const auto seen_json = json::parse(first.last().body);
    LR_CHECK_EQ(seen_json["model"].get<std::string>(), "upstream-a");
    LR_CHECK_EQ(seen_json["prompt"].get<std::string>(), "a hill");
    LR_CHECK_EQ(seen_json["n"].get<int>(), 2);
    LR_CHECK(settled(proxy));
    LR_CHECK_EQ(proxy.snapshot().tokens_prompt, 12);
    LR_CHECK_EQ(proxy.snapshot().tokens_completion, 7);
    auto json_edit = client.Post("/v1/images/edits", R"({"model":"media","images":[{"image_url":"data:image/png;base64,AAEA"}],"prompt":"change"})", "application/json");
    LR_CHECK(json_edit && json_edit->status == 200);
    LR_CHECK(json::parse(first.last().body)["images"].is_array());

    LR_GROUP("OpenAI image model default can itself be routed and renamed");
    first.set(); proxy.resetStats();
    auto defaults = client.Post("/v1/images/variations", multipart(binary, false), mime);
    LR_CHECK(defaults && defaults->status == 200);
    LR_CHECK_EQ(first.last().parts.back().content, "upstream-a");

    LR_GROUP("binary speech and streaming image events preserve body and MIME");
    first.set({200, binary, "audio/wav", true}); proxy.resetStats();
    auto speech = client.Post("/v1/audio/speech", R"({"model":"media","input":"hi","voice":"alloy","response_format":"wav"})", "application/json");
    LR_CHECK(speech && speech->status == 200);
    if (speech) {
        LR_CHECK_EQ(speech->body, binary);
        LR_CHECK_EQ(speech->get_header_value("Content-Type"), "audio/wav");
        LR_CHECK_EQ(speech->get_header_value("X-Request-Id"), "media-stub");
        LR_CHECK(!speech->has_header("Content-Length"));
    }
    LR_CHECK(settled(proxy));
    LR_CHECK_EQ(proxy.snapshot().bytes_out, binary.size());
    const std::string events = "event: image_generation.completed\ndata: {\"b64_json\":\"AAEA\",\"usage\":{\"input_tokens\":4,\"output_tokens\":8}}\n\n";
    first.set({200, events, "text/event-stream", true}); proxy.resetStats();
    auto stream = client.Post("/v1/images/generations", R"({"model":"media","prompt":"hill","stream":true})", "application/json");
    LR_CHECK(stream && stream->status == 200);
    if (stream) LR_CHECK_EQ(stream->body, events);
    LR_CHECK(settled(proxy));
    LR_CHECK_EQ(proxy.snapshot().tokens_prompt, 4);
    LR_CHECK_EQ(proxy.snapshot().tokens_completion, 8);
    const std::string large_event = "data: {\"b64_json\":\"" + std::string(1024 * 1024, 'A') +
                                    "\",\"usage\":{\"input_tokens\":9,\"output_tokens\":13}}\n\n";
    first.set({200, large_event, "text/event-stream", true}); proxy.resetStats();
    auto image_stream = client.Post("/v1/images/generations", R"({"model":"media","prompt":"hill","stream":true})", "application/json");
    LR_CHECK(image_stream && image_stream->status == 200 && image_stream->body == large_event);
    LR_CHECK(settled(proxy));
    LR_CHECK_EQ(proxy.snapshot().tokens_prompt, 9);
    LR_CHECK_EQ(proxy.snapshot().tokens_completion, 13);
    first.set({200, events, "text/event-stream", true}); proxy.resetStats();
    auto audio_stream = client.Post("/v1/audio/transcriptions", multipart(binary, true, true), mime);
    LR_CHECK(audio_stream && audio_stream->status == 200 && audio_stream->body == events);
    LR_CHECK(settled(proxy));

    LR_GROUP("retryable errors replay uploads and binary requests before commitment");
    first.set({503, R"({"error":{"message":"busy"}})"}); second.set(); proxy.resetStats();
    auto retried = client.Post("/v1/audio/transcriptions", upload, mime);
    LR_CHECK(retried && retried->status == 200);
    LR_CHECK_EQ(first.requests, 1); LR_CHECK_EQ(second.requests, 1);
    LR_CHECK_EQ(first.last().parts[0].content, second.last().parts[0].content);
    LR_CHECK_EQ(second.last().parts[1].content, "upstream-b");
    first.set({429, R"({"error":{"message":"busy"}})"});
    second.set({200, binary, "audio/mpeg", true}); proxy.resetStats();
    auto retried_speech = client.Post("/v1/audio/speech", R"({"model":"media","input":"hi","voice":"alloy"})", "application/json");
    LR_CHECK(retried_speech && retried_speech->status == 200 && retried_speech->body == binary);
    LR_CHECK_EQ(first.requests, 1); LR_CHECK_EQ(second.requests, 1);
    LR_CHECK(settled(proxy));

    LR_GROUP("terminal media errors retain upstream status, body and headers");
    const std::string error = R"({"error":{"message":"invalid voice"}})";
    first.set({400, error}); second.set(); proxy.resetStats();
    auto bad = client.Post("/v1/audio/speech", R"({"model":"media","input":"hi","voice":"bad"})", "application/json");
    LR_CHECK(bad && bad->status == 400 && bad->body == error);
    LR_CHECK_EQ(second.requests, 0);
    if (bad) LR_CHECK_EQ(bad->get_header_value("Retry-After"), "1");

    first.set({429, error}); second.set(); proxy.resetStats();
    auto single_cfg = cfg;
    single_cfg.server.max_attempts = 1;
    proxy.updateConfig(single_cfg);
    auto terminal_retry = client.Post("/v1/audio/speech", R"({"model":"media","input":"hi","voice":"alloy"})", "application/json");
    LR_CHECK(terminal_retry && terminal_retry->status == 429 && terminal_retry->body == error);
    LR_CHECK_EQ(second.requests, 0);
    if (terminal_retry) LR_CHECK_EQ(terminal_retry->get_header_value("Retry-After"), "1");
    proxy.updateConfig(cfg);

    LR_GROUP("connection failure before speech headers can use the next candidate");
    first.set(); second.set({200, binary, "audio/wav", true}); proxy.resetStats();
    auto unavailable_cfg = cfg;
    // Reserve then close a local socket to exercise connection refusal.
    httplib::Server unavailable;
    const auto unavailable_port = unavailable.bind_to_any_port("127.0.0.1");
    LR_CHECK(unavailable_port > 0);
    unavailable.stop();
    unavailable_cfg.providers[0].base_url = std::format("http://127.0.0.1:{}/api/v1", unavailable_port);
    proxy.updateConfig(unavailable_cfg);
    auto recovered = client.Post("/v1/audio/speech", R"({"model":"media","input":"hi","voice":"alloy"})", "application/json");
    LR_CHECK(recovered && recovered->status == 200 && recovered->body == binary);
    LR_CHECK_EQ(first.requests, 0); LR_CHECK_EQ(second.requests, 1);
    LR_CHECK(settled(proxy));
    proxy.updateConfig(cfg);

    LR_GROUP("committed truncated audio fails transport without invoking another provider");
    first.set({200, binary, "audio/wav", true, true}); second.set(); proxy.resetStats();
    httplib::Request request;
    request.method = "POST";
    request.path = "/v1/audio/speech";
    request.body = R"({"model":"media","input":"hi","voice":"alloy"})";
    request.set_header("Content-Type", "application/json");
    std::string received;
    int status = 0;
    request.response_handler = [&](const httplib::Response &response) { status = response.status; return true; };
    request.content_receiver = [&](const char *data, std::size_t size, std::size_t, std::size_t) {
        received.append(data, size); return true;
    };
    auto truncated = client.send(request);
    LR_CHECK(!truncated);
    LR_CHECK_EQ(status, 200);
    LR_CHECK_EQ(received, binary.substr(0, binary.size() / 2));
    LR_CHECK_EQ(second.requests, 0);
    LR_CHECK(settled(proxy));
    LR_CHECK_EQ(proxy.snapshot().total_failure, 1);

    LR_GROUP("unsupported protocols are skipped without consuming a media attempt");
    first.set(); second.set(); proxy.resetStats();
    cfg.providers[0].protocol = "anthropic";
    cfg.server.max_attempts = 1;
    proxy.updateConfig(cfg);
    auto compatible = client.Post("/v1/images/generations", R"({"model":"media","prompt":"hill"})", "application/json");
    LR_CHECK(compatible && compatible->status == 200);
    LR_CHECK_EQ(first.requests, 0); LR_CHECK_EQ(second.requests, 1);
    cfg.providers[1].protocol = "openai_responses";
    proxy.updateConfig(cfg);
    auto unsupported = client.Post("/v1/images/generations", R"({"model":"media","prompt":"hill"})", "application/json");
    LR_CHECK(unsupported && unsupported->status == 400);
    if (unsupported) LR_CHECK(unsupported->body.find("unsupported_media_protocol") != std::string::npos);
    LR_CHECK_EQ(first.requests, 0); LR_CHECK_EQ(second.requests, 1);

    LR_GROUP("validation and authentication reject uploads before upstream work");
    auto missing = client.Post("/v1/audio/transcriptions", multipart(binary, false), mime);
    LR_CHECK(missing && missing->status == 400);
    auto wrong_type = client.Post("/v1/audio/transcriptions", "{}", "application/json");
    LR_CHECK(wrong_type && wrong_type->status == 415);
    auto malformed = client.Post("/v1/images/edits", "--test-boundary\r\n", mime);
    LR_CHECK(malformed && malformed->status == 400);
    const std::string duplicate = "--test-boundary\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nmedia\r\n" + upload;
    auto duplicated = client.Post("/v1/images/edits", duplicate, mime);
    LR_CHECK(duplicated && duplicated->status == 400);
    httplib::Client unauthorized{proxy.boundAddress(), proxy.boundPort()};
    unauthorized.set_read_timeout(5, 0);
    auto denied = unauthorized.Post("/v1/audio/transcriptions", upload, mime);
    LR_CHECK(denied && denied->status == 401);
    LR_CHECK_EQ(first.requests, 0); LR_CHECK_EQ(second.requests, 1);
    proxy.stop();
    return LR_SUMMARY("test_media");
}
