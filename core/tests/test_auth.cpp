// Upstream authentication: the credential each protocol puts on the wire.
//
// The SigV4 case is checked against a signature computed independently (a
// separate implementation of the AWS algorithm, over the same method, path,
// body and instant), because "the header looks plausible" is not evidence that
// a signature is right — the relay is the only other party that can tell, and
// it is not in this process. A hard-coded expected value is what turns that
// into a test that can fail.
#include <httplib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "lr_test_check.h"

import nlohmann.json;
import literouter.core;

namespace {

using literouter::ProviderConfig;
using literouter::UpstreamHeader;

std::string headerValue(const std::vector<UpstreamHeader> &headers, std::string_view name) {
    for (const auto &header : headers) {
        if (literouter::toLower(header.name) == literouter::toLower(name)) {
            return header.value;
        }
    }
    return {};
}

bool hasHeader(const std::vector<UpstreamHeader> &headers, std::string_view name) {
    for (const auto &header : headers) {
        if (literouter::toLower(header.name) == literouter::toLower(name)) {
            return true;
        }
    }
    return false;
}

// The instant the SigV4 expectation was computed for: 2023-11-14T22:13:20Z.
constexpr double kSigV4Instant = 1700000000.0;

void testSimpleProtocolHeaders() {
    LR_GROUP("each protocol presents its credential the way it expects");

    {
        ProviderConfig openai;
        openai.protocol = "openai";
        openai.api_key = "sk-openai";
        const auto headers =
            literouter::upstreamAuthHeaders(openai, "POST", "/v1/chat/completions", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "authorization"), "Bearer sk-openai");
        }
    }
    {
        ProviderConfig anthropic;
        anthropic.protocol = "anthropic";
        anthropic.api_key = "sk-ant";
        const auto headers = literouter::upstreamAuthHeaders(anthropic, "POST", "/v1/messages", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "x-api-key"), "sk-ant");
            // The version header is not optional: without it the relay answers
            // 400 regardless of the key being right.
            LR_CHECK_EQ(headerValue(*headers, "anthropic-version"), "2023-06-01");
            LR_CHECK_MSG(!hasHeader(*headers, "authorization"),
                         "an Anthropic relay must not also get a bearer");
        }
    }
    {
        // A keyless Anthropic-shaped local gateway still needs the version.
        ProviderConfig anthropic;
        anthropic.protocol = "anthropic";
        const auto headers = literouter::upstreamAuthHeaders(anthropic, "POST", "/v1/messages", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "anthropic-version"), "2023-06-01");
            LR_CHECK(!hasHeader(*headers, "x-api-key"));
        }
    }
    {
        ProviderConfig gemini;
        gemini.protocol = "gemini";
        gemini.api_key = "AIza-example";
        const auto headers = literouter::upstreamAuthHeaders(gemini, "POST", "/v1beta/models/x", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "x-goog-api-key"), "AIza-example");
            LR_CHECK(!hasHeader(*headers, "authorization"));
        }
    }
    {
        // Azure rejects a bearer and wants its key in `api-key`.
        ProviderConfig azure;
        azure.protocol = "azure";
        azure.api_key = "az-key";
        const auto headers = literouter::upstreamAuthHeaders(azure, "POST", "/openai/deployments/d/chat/completions", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "api-key"), "az-key");
            LR_CHECK_MSG(!hasHeader(*headers, "authorization"),
                         "Azure OpenAI refuses a bearer token");
        }
    }
    {
        // Ollama is unauthenticated by default, and must not be handed a header
        // it would reject.
        ProviderConfig ollama;
        ollama.protocol = "ollama";
        const auto headers = literouter::upstreamAuthHeaders(ollama, "POST", "/api/chat", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK(headers->empty());
        }
    }
    {
        // A `${VAR}` reference is resolved here, at request time — and an unset
        // variable yields no header rather than a literal "${VAR}".
        const lr_test::EnvGuard guard{"LITEROUTER_TEST_AUTH_KEY"};
        ProviderConfig openai;
        openai.protocol = "openai";
        openai.api_key = "${LITEROUTER_TEST_AUTH_KEY}";
        guard.assign("sk-from-env");
        auto headers = literouter::upstreamAuthHeaders(openai, "POST", "/x", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_EQ(headerValue(*headers, "authorization"), "Bearer sk-from-env");
        }
        guard.clear();
        headers = literouter::upstreamAuthHeaders(openai, "POST", "/x", "{}", 0.0);
        LR_CHECK(headers.has_value());
        if (headers) {
            LR_CHECK_MSG(headers->empty(), "an unset reference must produce no credential");
        }
    }
}

void testBedrockSigV4() {
    LR_GROUP("Bedrock SigV4 matches an independently computed signature");

    ProviderConfig bedrock;
    bedrock.protocol = "bedrock";
    bedrock.base_url = "https://bedrock-runtime.us-east-1.amazonaws.com";
    bedrock.region = "us-east-1";
    bedrock.aws_access_key = "AKIDEXAMPLE";
    bedrock.aws_secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

    const std::string path = "/model/anthropic.claude-3-5-sonnet-20241022-v2:0/converse";
    const std::string body = R"({"messages":[]})";
    const auto headers =
        literouter::upstreamAuthHeaders(bedrock, "POST", path, body, kSigV4Instant);
    LR_CHECK_MSG(headers.has_value(), headers ? "" : headers.error());
    if (!headers) {
        return;
    }

    LR_CHECK_EQ(headerValue(*headers, "x-amz-date"), "20231114T221320Z");
    // The payload hash is over the body exactly as it will be sent.
    LR_CHECK_EQ(headerValue(*headers, "x-amz-content-sha256"),
                "5e4ce7b36ba37b78a5d5f9fd08e6b7b54ba6879d651aa46ec9e1d6fa24ebe30a");
    // Computed independently over the same method, path, body and instant. The
    // path is the subtle part: its colon must be percent-encoded in the
    // canonical URI (AWS encodes every byte outside unreserved-and-`/`, the same
    // rule botocore applies), and a Bedrock model id always contains one — so an
    // implementation that signed the path verbatim would look right here and be
    // rejected by every real relay.
    LR_CHECK_EQ(
        headerValue(*headers, "authorization"),
        "AWS4-HMAC-SHA256 "
        "Credential=AKIDEXAMPLE/20231114/us-east-1/bedrock/aws4_request, "
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
        "Signature=56380140b9de434bf40654e0c4e501a69502cc10dd27bd800602c1fdb95d5eff");
    // No session token when none is configured: an empty one would be signed and
    // then rejected.
    LR_CHECK(!hasHeader(*headers, "x-amz-security-token"));

    // A temporary credential adds the session token to both the request and the
    // signature, which changes the signature.
    bedrock.aws_session_token = "session-token-value";
    const auto with_token =
        literouter::upstreamAuthHeaders(bedrock, "POST", path, body, kSigV4Instant);
    LR_CHECK(with_token.has_value());
    if (with_token) {
        LR_CHECK_EQ(headerValue(*with_token, "x-amz-security-token"), "session-token-value");
        LR_CHECK_MSG(
            headerValue(*with_token, "authorization") != headerValue(*headers, "authorization"),
            "adding a signed header must change the signature");
        LR_CHECK(headerValue(*with_token, "authorization").find(
                     "x-amz-security-token") != std::string::npos);
    }

    // A different body is a different signature: this is what makes the
    // signature cover the payload rather than only the URL.
    const auto other_body =
        literouter::upstreamAuthHeaders(bedrock, "POST", path, R"({"messages":[1]})", kSigV4Instant);
    LR_CHECK(other_body.has_value());
    if (other_body) {
        LR_CHECK(headerValue(*other_body, "authorization") !=
                 headerValue(*headers, "authorization"));
    }

    // Missing credentials are refused before anything is signed.
    ProviderConfig incomplete;
    incomplete.protocol = "bedrock";
    incomplete.base_url = "https://bedrock-runtime.us-east-1.amazonaws.com";
    incomplete.region = "us-east-1";
    incomplete.aws_access_key = "AKIDEXAMPLE";
    const auto refused =
        literouter::upstreamAuthHeaders(incomplete, "POST", path, body, kSigV4Instant);
    LR_CHECK_MSG(!refused.has_value(), "a half-configured Bedrock relay must not be signed");
    if (!refused) {
        LR_CHECK(refused.error().find("aws_access_key") != std::string::npos);
    }
}

// A service-account key generated here rather than shipped: no private test key
// lives in the repository, and there is no expired fixture to rotate.
class ServiceAccount {
public:
    bool write(const std::filesystem::path &path, const std::string &token_uri) {
        const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
        EVP_PKEY *raw = nullptr;
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) != 1 ||
            EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
            return false;
        }
        const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{raw, EVP_PKEY_free};
        const std::unique_ptr<BIO, decltype(&BIO_free)> bio{
            BIO_new(BIO_s_mem()), BIO_free};
        if (!bio || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                            nullptr) != 1) {
            return false;
        }
        char *data = nullptr;
        const long length = BIO_get_mem_data(bio.get(), &data);
        if (length <= 0) {
            return false;
        }
        const std::string pem{data, static_cast<std::size_t>(length)};
        const nlohmann::json document{{"type", "service_account"},
                                      {"project_id", "demo"},
                                      {"private_key", pem},
                                      {"client_email", "lr-test@demo.iam.gserviceaccount.com"},
                                      {"token_uri", token_uri}};
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return false;
        }
        out << document.dump(2);
        return static_cast<bool>(out);
    }
};

void testVertexOAuth2() {
    LR_GROUP("Vertex mints and caches an OAuth2 token from a service-account key");

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("literouter-auth-test-" + literouter::hexId(6));
    std::filesystem::create_directories(directory);
    const auto credentials = directory / "service-account.json";

    {
        ProviderConfig vertex;
        vertex.protocol = "vertex";
        vertex.base_url = "https://us-central1-aiplatform.googleapis.com";
        vertex.project = "demo";
        vertex.credentials_file = credentials.string();
        const auto missing = literouter::upstreamAuthHeaders(vertex, "POST", "/x", "{}", 0.0);
        LR_CHECK_MSG(!missing.has_value(), "a missing key file must be an error, not a 401 later");
        if (!missing) {
            LR_CHECK(missing.error().find("cannot read") != std::string::npos);
        }
    }

    // The token endpoint is a real HTTP server, so the exchange is exercised
    // rather than stubbed: the assertion is signed, posted, and its reply parsed.
    httplib::Server token_server;
    std::atomic<int> exchanges{0};
    std::string last_assertion;
    token_server.Post("/token", [&](const httplib::Request &req, httplib::Response &res) {
        ++exchanges;
        const auto marker = req.body.find("assertion=");
        last_assertion = marker == std::string::npos ? std::string{} : req.body.substr(marker + 10);
        res.status = 200;
        res.set_content(R"({"access_token":"ya29.test-token","expires_in":3600,"token_type":"Bearer"})",
                        "application/json");
    });
    const int port = token_server.bind_to_any_port("127.0.0.1");
    LR_CHECK_MSG(port > 0, "the stub token endpoint did not bind");
    if (port <= 0) {
        std::filesystem::remove_all(directory);
        return;
    }
    std::thread token_thread([&token_server] { token_server.listen_after_bind(); });
    token_server.wait_until_ready();

    ServiceAccount account;
    LR_CHECK_MSG(account.write(credentials, std::format("http://127.0.0.1:{}/token", port)),
                 "the service-account fixture could not be written");

    ProviderConfig vertex;
    vertex.protocol = "vertex";
    vertex.base_url = "https://us-central1-aiplatform.googleapis.com";
    vertex.project = "demo";
    vertex.credentials_file = credentials.string();
    vertex.region = "us-central1";

    const double now = literouter::nowUnix();
    const auto headers = literouter::upstreamAuthHeaders(vertex, "POST", "/x", "{}", now);
    LR_CHECK_MSG(headers.has_value(), headers ? "" : headers.error());
    if (headers) {
        LR_CHECK_EQ(headerValue(*headers, "authorization"), "Bearer ya29.test-token");
    }
    LR_CHECK_EQ(exchanges.load(), 1);
    // The assertion is a three-part JWT whose signature is over the first two.
    const auto first_dot = last_assertion.find('.');
    const auto second_dot =
        first_dot == std::string::npos ? std::string::npos : last_assertion.find('.', first_dot + 1);
    LR_CHECK_MSG(second_dot != std::string::npos,
                 "the assertion is not a JWT: " + last_assertion.substr(0, 80));
    LR_CHECK_MSG(last_assertion.size() > second_dot + 1,
                 "the assertion has no signature segment");

    // A second call inside the token's lifetime reuses it: minting a token per
    // request would add a round trip to every single call.
    const auto again = literouter::upstreamAuthHeaders(vertex, "POST", "/x", "{}", now + 10.0);
    LR_CHECK(again.has_value());
    if (again) {
        LR_CHECK_EQ(headerValue(*again, "authorization"), "Bearer ya29.test-token");
    }
    LR_CHECK_MSG(exchanges.load() == 1, "a cached token must not be re-exchanged");

    // And past its expiry it is renewed, which is what keeps a long-running
    // proxy working rather than answering 401s an hour in.
    const auto renewed = literouter::upstreamAuthHeaders(vertex, "POST", "/x", "{}", now + 7200.0);
    LR_CHECK(renewed.has_value());
    LR_CHECK_MSG(exchanges.load() == 2, "an expired token must be re-exchanged");

    token_server.stop();
    if (token_thread.joinable()) {
        token_thread.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

} // namespace

int main() {
    testSimpleProtocolHeaders();
    testBedrockSigV4();
    testVertexOAuth2();
    return LR_SUMMARY("test_auth");
}
