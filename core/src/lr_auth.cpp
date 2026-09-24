// Per-protocol upstream authentication.
//
// One place, because the same headers have to be produced on three different
// code paths — the buffered call, the streamed one, and the console's probe —
// and three copies of "how do I sign a Bedrock request" is three chances for one
// of them to be subtly wrong in a way only the relay notices.
//
// The awkward members of this set are here for the same reason the simple ones
// are:
//
//   * Azure OpenAI authenticates with an `api-key` header and a mandatory
//     `api-version` query, not a bearer token;
//   * Vertex AI wants an OAuth2 bearer minted from a service-account key, which
//     means signing a JWT with RS256 and exchanging it — so this unit keeps a
//     small token cache, because minting one per request would add a round trip
//     to every call and the token is good for an hour;
//   * Bedrock wants SigV4 over the method, the path, a canonicalised header set
//     and the SHA-256 of the body, which is why this function is handed all
//     four rather than only the provider.
module;

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/bio.h>

module literouter.core;

import std;
import nlohmann.json;

#include "lr_dump.h"

namespace literouter {

namespace {

using json = nlohmann::json;

bool ciEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

// ── small encoders ───────────────────────────────────────────────────────────

std::string hexOf(const unsigned char *data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += std::format("{:02x}", data[i]);
    }
    return out;
}

std::string sha256Hex(std::string_view text) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), digest.data());
    return hexOf(digest.data(), digest.size());
}

constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64Url(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const auto block = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        out += kBase64Url[(block >> 18) & 63];
        out += kBase64Url[(block >> 12) & 63];
        out += kBase64Url[(block >> 6) & 63];
        out += kBase64Url[block & 63];
        i += 3;
    }
    const std::size_t left = bytes.size() - i;
    if (left == 1) {
        const auto block = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        out += kBase64Url[(block >> 18) & 63];
        out += kBase64Url[(block >> 12) & 63];
    } else if (left == 2) {
        const auto block = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out += kBase64Url[(block >> 18) & 63];
        out += kBase64Url[(block >> 12) & 63];
        out += kBase64Url[(block >> 6) & 63];
    }
    // No padding: JWT and every other base64url consumer in this file wants the
    // unpadded form.
    return out;
}

// ── SigV4 ────────────────────────────────────────────────────────────────────

std::string hmacSha256(std::string_view key, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, &length);
    return std::string(reinterpret_cast<char *>(out), length);
}

// RFC 3986: everything that is not unreserved must be percent-encoded, and `/`
// is only left alone in a path, never in a query value.
std::string uriEncode(std::string_view text, bool keep_slash) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (keep_slash && c == '/')) {
            out += c;
        } else {
            out += std::format("%{:02X}", static_cast<unsigned char>(c));
        }
    }
    return out;
}

// A SigV4 signature over one request. `headers` holds every header that will be
// signed; host must be in it, because AWS includes it in the canonical request.
std::string sigV4Authorization(const ProviderConfig &provider, std::string_view method,
                              std::string_view path, std::string_view query,
                              std::string_view body,
                              const std::vector<std::pair<std::string, std::string>> &headers,
                              std::string_view amz_date, std::string_view date_stamp,
                              std::string_view payload_hash) {
    const std::string access = resolveSecret(provider.aws_access_key);
    const std::string secret = resolveSecret(provider.aws_secret_key);
    const std::string region = provider.region.empty() ? "us-east-1" : provider.region;
    const std::string service = "bedrock";

    // Canonical headers: lower-cased names, trimmed values, sorted by name, each
    // terminated with a newline. Signed headers is the same set as a `;` list.
    std::vector<std::pair<std::string, std::string>> canonical;
    canonical.reserve(headers.size());
    for (const auto &[name, value] : headers) {
        canonical.emplace_back(toLower(name), trim(value));
    }
    std::ranges::sort(canonical, [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string canonical_headers;
    std::string signed_headers;
    for (const auto &[name, value] : canonical) {
        canonical_headers += std::format("{}:{}\n", name, value);
        if (!signed_headers.empty()) {
            signed_headers += ';';
        }
        signed_headers += name;
    }

    // The query string is canonicalised too: sorted by key then value, both
    // percent-encoded. Bedrock's own endpoints carry none, but a proxy in front
    // of one can add them and the signature has to survive that.
    std::string canonical_query;
    if (!query.empty()) {
        std::vector<std::pair<std::string, std::string>> params;
        std::string_view rest = query;
        while (!rest.empty()) {
            const auto amp = rest.find('&');
            const std::string_view pair = rest.substr(0, amp);
            const auto eq = pair.find('=');
            params.emplace_back(uriEncode(pair.substr(0, eq), false),
                                eq == std::string_view::npos
                                    ? std::string{}
                                    : uriEncode(pair.substr(eq + 1), false));
            if (amp == std::string_view::npos) {
                break;
            }
            rest = rest.substr(amp + 1);
        }
        std::ranges::sort(params);
        for (const auto &[key, value] : params) {
            if (!canonical_query.empty()) {
                canonical_query += '&';
            }
            canonical_query += key + "=" + value;
        }
    }

    const std::string canonical_request =
        std::format("{}\n{}\n{}\n{}\n{}\n{}", method, uriEncode(path, true), canonical_query,
                    canonical_headers, signed_headers, payload_hash);
    const std::string scope =
        std::format("{}/{}/{}/aws4_request", date_stamp, region, service);
    const std::string string_to_sign =
        std::format("AWS4-HMAC-SHA256\n{}\n{}\n{}", amz_date, scope, sha256Hex(canonical_request));

    const std::string k_date = hmacSha256("AWS4" + secret, date_stamp);
    const std::string k_region = hmacSha256(k_date, region);
    const std::string k_service = hmacSha256(k_region, service);
    const std::string k_signing = hmacSha256(k_service, "aws4_request");
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(string_to_sign.data());
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_length = 0;
    HMAC(EVP_sha256(), k_signing.data(), static_cast<int>(k_signing.size()), raw,
         string_to_sign.size(), signature, &signature_length);

    return std::format("AWS4-HMAC-SHA256 Credential={}/{}, SignedHeaders={}, Signature={}", access,
                       scope, signed_headers, hexOf(signature, signature_length));
}

// ── Vertex OAuth2 ────────────────────────────────────────────────────────────

struct VertexToken {
    std::string access_token;
    double expires_unix = 0.0;
};

std::mutex g_vertex_mutex;
std::map<std::string, VertexToken, std::less<>> g_vertex_tokens;

std::string readFile(const std::string &path, std::string &error) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        error = std::format("cannot read {}", path);
        return {};
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return text;
}

// Signs the assertion with RS256. Returns the signed JWT, or an error naming
// what about the key file was unusable — a Vertex relay that cannot mint a token
// is a configuration problem, and "authentication failed" from the relay would
// send the operator looking in the wrong place.
std::expected<std::string, std::string> mintVertexAssertion(const ProviderConfig &provider,
                                                            std::string_view token_uri,
                                                            double now_unix) {
    std::string error;
    const std::string text = readFile(provider.credentials_file, error);
    if (text.empty()) {
        return std::unexpected(error.empty() ? std::string{"credentials_file is empty"} : error);
    }
    const json key = json::parse(text, nullptr, false);
    if (key.is_discarded() || !key.is_object()) {
        return std::unexpected("service-account file is not JSON");
    }
    const std::string client_email = key.value("client_email", std::string{});
    const std::string private_key = key.value("private_key", std::string{});
    if (client_email.empty() || private_key.empty()) {
        return std::unexpected("service-account file has no client_email or private_key");
    }

    const json header{{"alg", "RS256"}, {"typ", "JWT"}};
    const json claims{{"iss", client_email},
                      {"scope", "https://www.googleapis.com/auth/cloud-platform"},
                      {"aud", std::string{token_uri}},
                      {"iat", static_cast<long long>(now_unix)},
                      {"exp", static_cast<long long>(now_unix) + 3600}};
    const std::string signing_input =
        base64Url(dumpJson(header)) + "." + base64Url(dumpJson(claims));

    const std::unique_ptr<BIO, decltype(&BIO_free)> bio{
        BIO_new_mem_buf(private_key.data(), static_cast<int>(private_key.size())), BIO_free};
    if (!bio) {
        return std::unexpected("cannot allocate an OpenSSL buffer for the private key");
    }
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey{
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
    if (!pkey) {
        return std::unexpected("private_key is not a readable PEM private key");
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(),
                                                                     EVP_MD_CTX_free};
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), signing_input.data(), signing_input.size()) != 1) {
        return std::unexpected("cannot initialise RS256 signing for the service-account key");
    }
    std::size_t signature_length = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &signature_length) != 1) {
        return std::unexpected("cannot size the RS256 signature");
    }
    std::string signature(signature_length, '\0');
    if (EVP_DigestSignFinal(ctx.get(), reinterpret_cast<unsigned char *>(signature.data()),
                            &signature_length) != 1) {
        return std::unexpected("cannot produce the RS256 signature");
    }
    signature.resize(signature_length);
    return signing_input + "." + base64Url(signature);
}

// Exchanges the assertion for an access token and caches it until it is nearly
// expired. The cache is process-wide and keyed by credential file, because a
// config reload must not throw away a token that is still good for fifty
// minutes.
std::expected<std::string, std::string> vertexAccessToken(const ProviderConfig &provider,
                                                          double now_unix) {
    std::string error;
    const std::string text = readFile(provider.credentials_file, error);
    if (text.empty()) {
        return std::unexpected(error.empty() ? std::string{"credentials_file is empty"} : error);
    }
    const json key = json::parse(text, nullptr, false);
    std::string token_uri = "https://oauth2.googleapis.com/token";
    if (key.is_object()) {
        token_uri = key.value("token_uri", token_uri);
    }

    const std::string cache_key = std::format("{}|{}", provider.credentials_file, token_uri);
    {
        std::scoped_lock lock{g_vertex_mutex};
        if (const auto it = g_vertex_tokens.find(cache_key); it != g_vertex_tokens.end()) {
            // Sixty seconds of slack: a token that expires while the request is
            // in flight is a 401 the operator would blame on the relay.
            if (it->second.expires_unix - 60.0 > now_unix) {
                return it->second.access_token;
            }
        }
    }

    auto assertion = mintVertexAssertion(provider, token_uri, now_unix);
    if (!assertion) {
        return std::unexpected(assertion.error());
    }
    const std::string form = std::format(
        "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion={}",
        uriEncode(*assertion, false));

    // The exchange is a plain HTTPS POST, so it goes through the same upstream
    // unit every relay does: the trust store, the timeouts and the error text
    // are then the ones the operator already understands.
    ProviderConfig sink;
    sink.id = "vertex-token";
    sink.base_url = token_uri;
    sink.timeout_sec = 15;
    sink.connect_timeout_sec = 10;
    const UpstreamResult result = upstreamPostRaw(sink, "", form,
                                                  "application/x-www-form-urlencoded");
    if (!result.ok) {
        return std::unexpected(std::format("Vertex token exchange failed: {}", result.error));
    }
    if (result.status < 200 || result.status >= 300) {
        return std::unexpected(std::format("Vertex token exchange returned HTTP {}: {}", result.status,
                                          truncateUtf8(trim(result.body), 200)));
    }
    const json body = json::parse(result.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::unexpected("Vertex token exchange returned a body that is not JSON");
    }
    const std::string token = body.value("access_token", std::string{});
    if (token.empty()) {
        return std::unexpected("Vertex token exchange returned no access_token");
    }
    const double lifetime = body.value("expires_in", 3600.0);
    {
        std::scoped_lock lock{g_vertex_mutex};
        g_vertex_tokens[cache_key] = VertexToken{token, now_unix + lifetime};
    }
    return token;
}

} // namespace

// ── the public entry point ───────────────────────────────────────────────────

std::expected<std::vector<UpstreamHeader>, std::string> upstreamAuthHeaders(
    const ProviderConfig &provider, std::string_view method, std::string_view path,
    std::string_view body, double now_unix) {
    std::vector<UpstreamHeader> headers;
    const std::string key = resolveSecret(provider.api_key);
    const std::string proto = toLower(provider.protocol);

    if (proto == "azure") {
        // Azure OpenAI takes the key in its own header and rejects a bearer.
        if (!key.empty()) {
            headers.push_back({"api-key", key});
        }
        return headers;
    }
    if (proto == "vertex") {
        auto token = vertexAccessToken(provider, now_unix);
        if (!token) {
            return std::unexpected(token.error());
        }
        headers.push_back({"Authorization", "Bearer " + *token});
        return headers;
    }
    if (proto == "bedrock") {
        // Bedrock's own endpoint form: /model/{id}/converse, with the region in
        // the host. The caller has already built `path` for this relay, so it is
        // signed as given.
        const std::string access = resolveSecret(provider.aws_access_key);
        if (access.empty() || resolveSecret(provider.aws_secret_key).empty()) {
            return std::unexpected("Bedrock needs aws_access_key and aws_secret_key");
        }
        const std::string region = provider.region.empty() ? "us-east-1" : provider.region;
        std::string host;
        std::string prefix;
        std::string scheme;
        if (!splitBaseUrl(provider.base_url, host, prefix, scheme)) {
            return std::unexpected(std::format("invalid base_url `{}`", provider.base_url));
        }
        // splitBaseUrl keeps the scheme; SigV4's host header does not.
        if (const auto pos = host.find("://"); pos != std::string::npos) {
            host = host.substr(pos + 3);
        }
        const auto slash = path.find('?');
        const std::string path_only{path.substr(0, slash)};
        const std::string query =
            slash == std::string_view::npos ? std::string{} : std::string{path.substr(slash + 1)};

        // The date is derived from the caller's clock so a test can pin it.
        const std::time_t seconds = static_cast<std::time_t>(now_unix);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &seconds);
#else
        gmtime_r(&seconds, &utc);
#endif
        const std::string amz_date = std::format("{:04d}{:02d}{:02d}T{:02d}{:02d}{:02d}Z",
                                                 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        const std::string date_stamp = amz_date.substr(0, 8);
        const std::string payload_hash = sha256Hex(body);

        std::vector<std::pair<std::string, std::string>> signed_set{
            {"host", host},
            {"x-amz-content-sha256", payload_hash},
            {"x-amz-date", amz_date},
        };
        const std::string session = resolveSecret(provider.aws_session_token);
        if (!session.empty()) {
            signed_set.emplace_back("x-amz-security-token", session);
        }
        const std::string authorization =
            sigV4Authorization(provider, method, path_only, query, body, signed_set, amz_date,
                               date_stamp, payload_hash);
        headers.push_back({"x-amz-date", amz_date});
        headers.push_back({"x-amz-content-sha256", payload_hash});
        if (!session.empty()) {
            headers.push_back({"x-amz-security-token", session});
        }
        headers.push_back({"Authorization", authorization});
        return headers;
    }

    if (!key.empty()) {
        if (proto == "anthropic") {
            headers.push_back({"x-api-key", key});
            headers.push_back({"anthropic-version", "2023-06-01"});
        } else if (proto == "gemini") {
            headers.push_back({"x-goog-api-key", key});
        } else {
            headers.push_back({"Authorization", "Bearer " + key});
        }
    } else if (proto == "anthropic") {
        // An Anthropic-shaped relay still needs the version header even with no
        // key (a local gateway), and omitting it turns every call into a 400.
        headers.push_back({"anthropic-version", "2023-06-01"});
    }
    return headers;
}

} // namespace literouter
