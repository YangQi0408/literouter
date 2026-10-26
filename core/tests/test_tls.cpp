// CA bundle resolution (lr_util.cpp). The bug this file exists for: mcpp builds
// OpenSSL from source, so its compiled-in default verify path points at the mcpp
// build tree instead of the machine's trust store — literouter then reports
// "upstream certificate rejected" for an endpoint curl reaches fine. The
// invariant that keeps that from coming back is "resolveCaBundle() is either
// empty or a file that exists".
#include "../src/lr_http.h"
#include <openssl/x509v3.h>

#include "lr_test_check.h"

import literouter.core;

namespace {

// A file that removes itself, standing in for a user-supplied bundle.
class TempFile {
public:
    TempFile() {
        path_ = std::filesystem::temp_directory_path() /
                ("literouter-ca-test-" + literouter::hexId(6) + ".pem");
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        out << "-----BEGIN CERTIFICATE-----\nnot a real certificate\n"
               "-----END CERTIFICATE-----\n";
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

// The three overrides, saved and restored together: the resolution order only
// means anything when the lower-precedence ones are in a known state, and a
// check that fails halfway must not leave the shell's environment changed.
class CaEnvironment {
public:
    void clearAll() {
        literouter_.clear();
        ssl_cert_file_.clear();
        curl_.clear();
    }

    lr_test::EnvGuard &literouter() { return literouter_; }
    lr_test::EnvGuard &sslCertFile() { return ssl_cert_file_; }
    lr_test::EnvGuard &curl() { return curl_; }

private:
    lr_test::EnvGuard literouter_{"LITEROUTER_CA_BUNDLE"};
    lr_test::EnvGuard ssl_cert_file_{"SSL_CERT_FILE"};
    lr_test::EnvGuard curl_{"CURL_CA_BUNDLE"};
};

// Every proxy variable, saved and restored together. The resolution order only
// means anything when the lower-precedence ones are in a known state, and a
// check that fails halfway must not leave the shell's environment changed.
// The clearing and the named accessors both live in lr_test::ProxyEnvironment.

bool fileExists(const std::filesystem::path &path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

// The invariant, asserted in one place because it is the whole point: whatever
// comes back must be usable, never a path that does not exist.
void checkResolvedPathIsUsable(std::string_view context) {
    const auto bundle = literouter::resolveCaBundle();
    LR_CHECK_MSG(bundle.empty() || fileExists(bundle),
                 std::string{context} + ": resolved to a path that is not a regular file: " +
                     bundle.string());
}

void testInvariantAndSystemFallback() {
    LR_GROUP("resolveCaBundle: the result is empty or a file that exists");
    CaEnvironment environment;
    environment.clearAll();

    checkResolvedPathIsUsable("with no overrides set");

    // A machine that has any of the common system bundles must not resolve to
    // nothing: falling through to the TLS library's own default is what broke
    // https in the first place.
    const std::vector<std::string> wellKnown{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/etc/ssl/ca-bundle.pem",
    };
    bool anyWellKnownExists = false;
    for (const auto &candidate : wellKnown) {
        anyWellKnownExists = anyWellKnownExists || fileExists(candidate);
    }
    if (anyWellKnownExists) {
        LR_CHECK_MSG(!literouter::resolveCaBundle().empty(),
                     "a system trust store exists but resolveCaBundle() found nothing");
    } else {
        LR_NOTE("no well-known system bundle on this machine; only the invariant is checked");
    }

    // The summary is the same answer, printable: empty means "the TLS library's
    // own defaults", which is what `doctor` shows.
    const auto bundle = literouter::resolveCaBundle();
    if (bundle.empty()) {
        LR_CHECK_EQ(literouter::caBundleSummary(), "");
    } else {
        LR_CHECK_EQ(literouter::caBundleSummary(), bundle.string());
    }
}

void testOverrideWins() {
    LR_GROUP("resolveCaBundle: LITEROUTER_CA_BUNDLE wins");
    CaEnvironment environment;
    environment.clearAll();

    const TempFile override_file;
    environment.literouter().assign(override_file.path().string());
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), override_file.path().string());
    LR_CHECK_EQ(literouter::caBundleSummary(), override_file.path().string());
    checkResolvedPathIsUsable("with a valid override set");
}

void testMissingOverrideIsSkipped() {
    LR_GROUP("resolveCaBundle: a missing override is skipped, never returned");
    CaEnvironment environment;
    environment.clearAll();

    // Handing OpenSSL a path that does not exist is how a bad override turns
    // into "verification is impossible" instead of "this override is ignored".
    const auto missing = std::filesystem::temp_directory_path() /
                         ("literouter-ca-missing-" + literouter::hexId(6) + ".pem");
    LR_CHECK(!fileExists(missing));

    environment.literouter().assign(missing.string());
    LR_CHECK_MSG(literouter::resolveCaBundle() != missing, "a nonexistent override was returned");
    checkResolvedPathIsUsable("with a nonexistent override set");

    // The same for the machine-wide variables.
    environment.literouter().clear();
    environment.sslCertFile().assign(missing.string());
    LR_CHECK(literouter::resolveCaBundle() != missing);
    environment.sslCertFile().clear();
    environment.curl().assign(missing.string());
    LR_CHECK(literouter::resolveCaBundle() != missing);
    checkResolvedPathIsUsable("with a nonexistent CURL_CA_BUNDLE set");
}

void testEmptyOverrideIsIgnored() {
    LR_GROUP("resolveCaBundle: an empty override changes nothing");
    CaEnvironment environment;
    environment.clearAll();

    const auto withoutEmpty = literouter::resolveCaBundle();
    environment.literouter().assign("");
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), withoutEmpty.string());
    environment.sslCertFile().assign("");
    environment.curl().assign("");
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), withoutEmpty.string());
    checkResolvedPathIsUsable("with empty overrides set");
}

void testPrecedence() {
    LR_GROUP("resolveCaBundle: resolution order");
    CaEnvironment environment;
    environment.clearAll();

    const TempFile first;
    const TempFile second;
    const TempFile third;

    // All three set: literouter's own variable is the most specific.
    environment.literouter().assign(first.path().string());
    environment.sslCertFile().assign(second.path().string());
    environment.curl().assign(third.path().string());
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), first.path().string());

    // Then the machine-wide SSL_CERT_FILE, which is what curl and everything
    // else on the machine already uses.
    environment.literouter().clear();
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), second.path().string());

    // Then CURL_CA_BUNDLE.
    environment.sslCertFile().clear();
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), third.path().string());

    // A higher-precedence non-file does not shadow a lower-precedence real one.
    const auto missing = std::filesystem::temp_directory_path() /
                         ("literouter-ca-gap-" + literouter::hexId(6) + ".pem");
    environment.literouter().assign(missing.string());
    LR_CHECK_EQ(literouter::resolveCaBundle().string(), third.path().string());
    checkResolvedPathIsUsable("with a broken override above a working one");
}

// ── upstream proxy resolution ────────────────────────────────────────────────
//
// The environment is the only input, so every branch is reachable from a test
// without a socket. The one invariant that matters is the same one
// resolveCaBundle() has: a variable that cannot be used must produce "no proxy"
// and a warning, never a proxy pointed somewhere unintended.

void testProxyAbsentMeansDirect() {
    LR_GROUP("resolveUpstreamProxy: no variable set means connect directly");
    lr_test::ProxyEnvironment environment;

    const auto proxy = literouter::resolveUpstreamProxy();
    LR_CHECK(!proxy.enabled);
    LR_CHECK(proxy.warning.empty());
    LR_CHECK_EQ(proxy.host, "");
}

void testProxyPrecedence() {
    LR_GROUP("resolveUpstreamProxy: LITEROUTER_PROXY > HTTPS_PROXY > HTTP_PROXY");
    lr_test::ProxyEnvironment environment;

    // HTTPS_PROXY is the one curl prefers for an https upstream, which is what
    // every real relay is.
    environment.httpUpper().assign("http://from-http:1111");
    auto proxy = literouter::resolveUpstreamProxy();
    LR_CHECK(proxy.enabled);
    LR_CHECK_EQ(proxy.host, "from-http");
    LR_CHECK_EQ(proxy.port, 1111);

    environment.httpsUpper().assign("http://from-https:2222");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "from-https");
    LR_CHECK_EQ(proxy.port, 2222);

    environment.literouter().assign("http://from-literal:3333");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "from-literal");
    LR_CHECK_EQ(proxy.port, 3333);

    // The lowercase spelling of a higher-precedence variable still beats the
    // uppercase spelling of a lower-precedence one.
    environment.literouter().clear();
    environment.httpsUpper().clear();
    environment.httpsLower().assign("http://from-https-lower:4444");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "from-https-lower");
    LR_CHECK_EQ(proxy.port, 4444);

    // HTTP_PROXY's lowercase form is last.
    environment.httpsLower().clear();
    environment.httpUpper().clear();
    environment.httpLower().assign("http://from-http-lower:5555");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "from-http-lower");
    LR_CHECK_EQ(proxy.port, 5555);
}

void testProxyUrlShapes() {
    LR_GROUP("resolveUpstreamProxy: bare host:port, credentials, IPv6, default port");
    lr_test::ProxyEnvironment environment;

    // A scheme is optional — `proxy.corp:3128` is as common as the URL form.
    environment.literouter().assign("proxy.corp:3128");
    auto proxy = literouter::resolveUpstreamProxy();
    LR_CHECK(proxy.enabled);
    LR_CHECK_EQ(proxy.scheme, "http");
    LR_CHECK_EQ(proxy.host, "proxy.corp");
    LR_CHECK_EQ(proxy.port, 3128);

    // Credentials are split on the LAST `@`, so a password containing one
    // survives, and both halves are percent-decoded.
    environment.literouter().assign("http://user:p%40ss%3Aword@proxy.corp:3128");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.username, "user");
    LR_CHECK_EQ(proxy.password, "p@ss:word");
    LR_CHECK_EQ(proxy.host, "proxy.corp");

    // A username with no password is still a username.
    environment.literouter().assign("http://solo@proxy.corp");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.username, "solo");
    LR_CHECK_EQ(proxy.password, "");
    LR_CHECK_EQ(proxy.host, "proxy.corp");

    // A trailing slash or path means nothing for CONNECT and is dropped.
    environment.literouter().assign("http://proxy.corp:8080/");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "proxy.corp");
    LR_CHECK_EQ(proxy.port, 8080);

    // HTTPS proxy transport is rejected, never silently downgraded to HTTP.
    environment.literouter().assign("http://proxy.corp");
    LR_CHECK_EQ(literouter::resolveUpstreamProxy().port, 8080);
    environment.literouter().assign("https://proxy.corp");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.scheme, "https");
    LR_CHECK_MSG(!proxy.enabled && !proxy.warning.empty(), "HTTPS proxy was downgraded");
    const literouter::ProviderConfig unused{.id = "unused", .base_url = "http://127.0.0.1:1"};
    const auto post = literouter::upstreamPost(unused, "/completions", "{}");
    LR_CHECK_MSG(!post.ok && post.error == proxy.warning, post.error);
    const auto probe = literouter::probeProvider(unused);
    LR_CHECK_MSG(!probe.reachable && probe.detail == proxy.warning, probe.detail);

    // A bracketed IPv6 literal keeps its brackets out of the port split.
    environment.literouter().assign("http://[::1]:3128");
    proxy = literouter::resolveUpstreamProxy();
    LR_CHECK_EQ(proxy.host, "::1");
    LR_CHECK_EQ(proxy.port, 3128);
}

void testProxyRejectsUnusableValues() {
    LR_GROUP("resolveUpstreamProxy: an unusable value is ignored, and said so");
    lr_test::ProxyEnvironment environment;

    // The invariant: a value that cannot be used yields NO proxy and a warning.
    // Returning a half-built proxy here is how a typo turns into every request
    // being tunnelled somewhere unintended.
    for (const char *bad : {"https://proxy.corp:443", "socks5://proxy.corp:1080", "http://proxy.corp:notaport",
                            "http://proxy.corp:99999", "http://proxy.corp:0", "http://",
                            "http://[::1", ":3128"}) {
        environment.literouter().assign(bad);
        const auto proxy = literouter::resolveUpstreamProxy();
        LR_CHECK_MSG(!proxy.enabled, std::format("`{}` was accepted as a proxy", bad));
        LR_CHECK_MSG(!proxy.warning.empty(),
                     std::format("`{}` was refused without a warning", bad));
    }

    // An empty value is not a mistake, it is "unset".
    environment.literouter().assign("");
    const auto proxy = literouter::resolveUpstreamProxy();
    LR_CHECK(!proxy.enabled);
    LR_CHECK(proxy.warning.empty());

    // A warning goes to stderr and can be pasted into an issue, so it must not
    // carry the proxy password — a malformed URL is exactly when the operator
    // has just typed one. Every refusal path is checked, because the redaction
    // is applied per message and a new message could forget it.
    for (const char *bad : {"https://user:s3cret@proxy.corp:443", "socks5://user:s3cret@proxy.corp:1080",
                            "http://user:s3cret@proxy.corp:notaport",
                            "http://user:s3cret@[::1", "http://user:s3cret@"}) {
        environment.literouter().assign(bad);
        const auto refused = literouter::resolveUpstreamProxy();
        LR_CHECK_MSG(!refused.enabled, std::format("`{}` was accepted", bad));
        LR_CHECK_MSG(refused.warning.find("s3cret") == std::string::npos,
                     std::format("the warning for `{}` leaked the password: {}", bad,
                                 refused.warning));
    }

    // The host stays visible wherever the value had one: it is what the
    // operator needs in order to see which value was refused. The two values
    // above that carry no host at all are excluded on purpose.
    for (const char *bad : {"socks5://user:s3cret@proxy.corp:1080",
                            "http://user:s3cret@proxy.corp:notaport"}) {
        environment.literouter().assign(bad);
        const auto refused = literouter::resolveUpstreamProxy();
        LR_CHECK_MSG(refused.warning.find("proxy.corp") != std::string::npos,
                     std::format("the warning for `{}` lost the host: {}", bad,
                                 refused.warning));
    }
}

void testNoProxyList() {
    LR_GROUP("resolveNoProxyList: comma-separated, trimmed, wildcard kept");
    lr_test::ProxyEnvironment environment;

    LR_CHECK(literouter::resolveNoProxyList().empty());

    environment.noProxyUpper().assign("localhost, 127.0.0.1,.corp.example");
    auto list = literouter::resolveNoProxyList();
    LR_CHECK_EQ(list.size(), 3u);
    LR_CHECK_EQ(list[0], "localhost");
    LR_CHECK_EQ(list[1], "127.0.0.1");
    LR_CHECK_EQ(list[2], ".corp.example");

    // Empty entries are dropped rather than handed to httplib as "" patterns.
    environment.noProxyUpper().assign("a,,  ,b,");
    list = literouter::resolveNoProxyList();
    LR_CHECK_EQ(list.size(), 2u);
    LR_CHECK_EQ(list[0], "a");
    LR_CHECK_EQ(list[1], "b");

    // `*` is preserved verbatim: httplib reads it as "bypass everything", so
    // it must not be trimmed away or treated as a hostname.
    environment.noProxyUpper().assign("*");
    list = literouter::resolveNoProxyList();
    LR_CHECK_EQ(list.size(), 1u);
    LR_CHECK_EQ(list[0], "*");

    // The lowercase spelling is honoured when the uppercase one is absent.
    environment.noProxyUpper().clear();
    environment.noProxyLower().assign("only.lower");
    list = literouter::resolveNoProxyList();
    LR_CHECK_EQ(list.size(), 1u);
    LR_CHECK_EQ(list[0], "only.lower");
}

void testDirectoriesAreNotBundles() {
    LR_GROUP("resolveCaBundle: a directory is not a bundle");
    CaEnvironment environment;
    environment.clearAll();

    // A directory named ca-certificates.crt would satisfy "exists" but not
    // OpenSSL, so the check has to be is_regular_file.
    const auto directory = std::filesystem::temp_directory_path() /
                           ("literouter-ca-dir-" + literouter::hexId(6));
    std::filesystem::create_directories(directory);
    environment.literouter().assign(directory.string());
    LR_CHECK_MSG(literouter::resolveCaBundle().string() != directory.string(),
                 "a directory was accepted as a CA bundle");
    checkResolvedPathIsUsable("with a directory as the override");
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}


// Generate a private CA and leaf at runtime: no expired fixture, no openssl
// executable dependency, and no private test key shipped with the application.
class TlsFixture {
public:
    explicit TlsFixture(bool expired = false, bool wrong_host = false) {
        directory_ = std::filesystem::temp_directory_path() /
                     literouter::pathFromUtf8("literouter-listener-tls-\u4e2d\u6587-" + literouter::hexId(6));
        std::filesystem::create_directories(directory_);
        ca_path = directory_ / "ca.pem";
        cert_path = directory_ / "chain.pem";
        key_path = directory_ / "key.pem";
        auto ca_key = newKey();
        auto key = newKey();
        if (!ca_key || !key) return;
        auto ca = newCert(ca_key.get(), nullptr, ca_key.get(), true, false, false);
        auto cert = newCert(key.get(), ca.get(), ca_key.get(), false, expired, wrong_host);
        if (!ca || !cert) return;
        valid = writeCert(ca_path, ca.get()) && writeCert(cert_path, cert.get());
        const std::unique_ptr<BIO, decltype(&BIO_free)> output{BIO_new(BIO_s_mem()), BIO_free};
        valid = valid && output && PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr,
                                                           nullptr, 0, nullptr, nullptr) == 1;
        if (valid) {
            char *bytes = nullptr;
            const auto size = BIO_get_mem_data(output.get(), &bytes);
            std::ofstream file{key_path, std::ios::binary};
            file.write(bytes, size);
            valid = static_cast<bool>(file);
        }
    }
    ~TlsFixture() {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
    literouter::AppConfig config() const {
        literouter::AppConfig config;
        config.server.port = 0;
        config.server.persist_telemetry = false;
        config.server.api_key = "tls-test-admin-key";
        config.server.tls_cert_file = literouter::pathToUtf8(cert_path);
        config.server.tls_key_file = literouter::pathToUtf8(key_path);
        return config;
    }
    bool valid = false;
    std::filesystem::path ca_path, cert_path, key_path;
private:
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
    std::filesystem::path directory_;
    static Key newKey() {
        const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
        EVP_PKEY *key = nullptr;
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) != 1 ||
            EVP_PKEY_keygen(ctx.get(), &key) != 1) return Key{nullptr, EVP_PKEY_free};
        return Key{key, EVP_PKEY_free};
    }
    static Cert newCert(EVP_PKEY *key, X509 *issuer, EVP_PKEY *issuer_key,
                        bool ca, bool expired, bool wrong_host) {
        Cert cert{X509_new(), X509_free};
        if (!cert) return cert;
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ca ? 1 : 2);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), expired ? -60 : 86400);
        X509_set_pubkey(cert.get(), key);
        X509_NAME *name = X509_get_subject_name(cert.get());
        const std::string cn = ca ? "literouter test CA" : "localhost";
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0);
        X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : name);
        X509V3_CTX ctx{};
        X509V3_set_ctx(&ctx, issuer ? issuer : cert.get(), cert.get(), nullptr, nullptr, 0);
        auto extension = [&](int nid, const char *value) {
            const std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> ext{
                X509V3_EXT_conf_nid(nullptr, &ctx, nid, value), X509_EXTENSION_free};
            return ext && X509_add_ext(cert.get(), ext.get(), -1) == 1;
        };
        bool ok = extension(NID_basic_constraints, ca ? "critical,CA:TRUE" : "critical,CA:FALSE") &&
                  extension(NID_key_usage, ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature,keyEncipherment");
        if (!ca) {
            ok = ok && extension(NID_ext_key_usage, "serverAuth") &&
                 extension(NID_subject_alt_name,
                           wrong_host ? "DNS:wrong.invalid" : "DNS:localhost,IP:127.0.0.1");
        }
        if (!ok || X509_sign(cert.get(), issuer_key, EVP_sha256()) <= 0) cert.reset();
        return cert;
    }
    static bool writeCert(const std::filesystem::path &path, X509 *cert) {
        const std::unique_ptr<BIO, decltype(&BIO_free)> output{BIO_new(BIO_s_mem()), BIO_free};
        if (!output || PEM_write_bio_X509(output.get(), cert) != 1) return false;
        char *bytes = nullptr;
        const auto size = BIO_get_mem_data(output.get(), &bytes);
        std::ofstream file{path, std::ios::binary};
        file.write(bytes, size);
        return static_cast<bool>(file);
    }
};

void testHttpsListener() {
    LR_GROUP("HTTPS listener: verified CA, authenticated management and actual bound URL");
    const TlsFixture fixture;
    LR_CHECK(fixture.valid);
    if (!fixture.valid) return;
    lr_test::EnvGuard state_dir{"LITEROUTER_STATE_DIR"};
    state_dir.assign(literouter::pathToUtf8(fixture.ca_path.parent_path() / "state"));
    CaEnvironment environment;
    environment.clearAll();
    environment.literouter().assign(literouter::pathToUtf8(fixture.ca_path));
    auto config = fixture.config();
    LR_CHECK(literouter::validate(config).ok());
    literouter::ProxyServer proxy;
    const auto configPath = fixture.ca_path.parent_path() / "gateway.json";
    proxy.setConfigPath(literouter::pathToUtf8(configPath));
    const auto started = proxy.start(config);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) return;
    const auto base = std::format("https://127.0.0.1:{}", proxy.boundPort());
    LR_CHECK_EQ(proxy.snapshot().base_url, base);
    httplib::Client client{base};
    std::ifstream caInput{fixture.ca_path, std::ios::binary};
    const std::string caPem{std::istreambuf_iterator<char>{caInput}, std::istreambuf_iterator<char>{}};
    client.set_ca_cert_store(httplib::tls::create_ca_store(caPem.data(), caPem.size()));
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    const auto health = client.Get("/health");
    LR_CHECK(health && health->status == 200);
    const auto console = client.Get("/ui/");
    LR_CHECK(console && console->status == 200);
    const auto unauthorized = client.Get("/__literouter/status");
    LR_CHECK(unauthorized && unauthorized->status == 401);
    const auto status = literouter::fetchStatus(base, config.server.api_key);
    LR_CHECK_MSG(status.reachable, status.error);
    LR_CHECK_EQ(status.snapshot.base_url, base);
    // Dynamic discovery must retain HTTPS and verify this exact config path.
    const auto dynamic = literouter::runningServerBaseUrl(config.server, configPath);
    LR_CHECK_MSG(dynamic && dynamic->starts_with("https://") &&
                 literouter::fetchStatus(*dynamic, config.server.api_key).reachable,
                 dynamic ? *dynamic : dynamic.error());
    const auto logs = literouter::fetchLogs(base, 0, 10, config.server.api_key);
    LR_CHECK_MSG(logs.reachable, logs.error);
    const auto reset = literouter::adminPost(base, "/reset-stats", "", config.server.api_key);
    LR_CHECK(reset.reachable && reset.status == 200);
    const httplib::Headers headers{{"Authorization", "Bearer " + config.server.api_key}};
    const auto models = client.Get("/v1/models", headers);
    LR_CHECK(models && models->status == 200);
    const auto stored = client.Get("/__literouter/config", headers);
    LR_CHECK(stored && stored->status == 200);
    LR_CHECK(stored && stored->body.find("BEGIN PRIVATE KEY") == std::string::npos);

    // A private CA is never trusted merely because the process serves it.
    environment.clearAll();
    LR_CHECK(!literouter::fetchStatus(base, config.server.api_key).reachable);
    const auto untrusted_logs = literouter::fetchLogs(base, 0, 10, config.server.api_key);
    LR_CHECK(!untrusted_logs.reachable);
    const auto untrusted_post = literouter::adminPost(base, "/reset-stats", "", config.server.api_key);
    LR_CHECK(!untrusted_post.reachable);
    httplib::Client plaintext{"127.0.0.1", proxy.boundPort()};
    plaintext.set_connection_timeout(1);
    plaintext.set_read_timeout(1);
    LR_CHECK(!plaintext.Get("/health"));

    // The health probe cannot validate this private CA now. The OS must still
    // prevent a second listener sharing the socket (including HTTP vs HTTPS).
    config.server.port = proxy.boundPort();
    literouter::ProxyServer other;
    LR_CHECK(!other.start(config));
    auto plain_config = config;
    plain_config.server.tls_cert_file.clear();
    plain_config.server.tls_key_file.clear();
    LR_CHECK(!other.start(plain_config));

    environment.literouter().assign(literouter::pathToUtf8(fixture.ca_path));
    // Editing to HTTP must save successfully but leave the active socket and
    // status HTTPS until stop/start. This exercises the real Web save path.
    plain_config.server.host = "localhost";
    plain_config.server.port = 0;
    const auto saved = client.Put("/__literouter/config", headers,
                                  literouter::toJsonString(plain_config), "application/json");
    LR_CHECK(saved && saved->status == 200);
    LR_CHECK_EQ(proxy.snapshot().base_url, base);
    LR_CHECK_EQ(proxy.boundAddress(), "127.0.0.1");
    LR_CHECK(proxy.config().server.tls_cert_file.empty());
    proxy.stop();
    const auto restarted = proxy.start(plain_config);
    LR_CHECK_MSG(restarted.has_value(), restarted ? "" : restarted.error());
    if (restarted) {
        LR_CHECK(literouter::startsWith(proxy.snapshot().base_url, "http://localhost:"));
        LR_CHECK(literouter::fetchStatus(proxy.snapshot().base_url, config.server.api_key).reachable);
    }
    proxy.stop();
}

void testInvalidListenerCredentials() {
    LR_GROUP("HTTPS listener: missing, malformed, expired, wrong key and hostname");
    const TlsFixture good;
    const TlsFixture expired{true};
    const TlsFixture wrong_host{false, true};
    LR_CHECK(good.valid && expired.valid && wrong_host.valid);
    if (!good.valid || !expired.valid || !wrong_host.valid) return;
    lr_test::EnvGuard state_dir{"LITEROUTER_STATE_DIR"};
    state_dir.assign(literouter::pathToUtf8(good.ca_path.parent_path() / "state"));
    auto bad = good.config();
    literouter::ProxyServer proxy;
    bad.server.tls_key_file.clear();
    LR_CHECK(!literouter::validate(bad).ok());
    LR_CHECK(!proxy.start(bad));
    bad = good.config();
    bad.server.tls_cert_file = "relative.pem";
    LR_CHECK(!proxy.start(bad));
    bad.server.tls_cert_file = literouter::pathToUtf8(good.cert_path) + ".missing";
    LR_CHECK(!proxy.start(bad));
    const TempFile malformed;
    bad.server.tls_cert_file = malformed.path().string();
    LR_CHECK(!proxy.start(bad));
    bad = good.config();
    bad.server.tls_key_file = literouter::pathToUtf8(wrong_host.key_path);
    LR_CHECK(!proxy.start(bad));
    LR_CHECK(!proxy.start(expired.config()));

    {
        const auto brokenChain = good.cert_path.parent_path() / "broken-chain.pem";
        std::ifstream source{good.cert_path, std::ios::binary};
        const std::string leaf{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
        std::ofstream staged{brokenChain, std::ios::binary};
        staged << leaf << "\n-----BEGIN CERTIFICATE-----\nnot-base64\n-----END CERTIFICATE-----\n";
        staged.close();
        auto broken = good.config();
        broken.server.tls_cert_file = literouter::pathToUtf8(brokenChain);
        LR_CHECK_MSG(!literouter::validate(broken).ok() && !proxy.start(broken),
                     "a malformed intermediate certificate was accepted as EOF");
    }

    // A valid chain is insufficient when SAN does not cover the URL hostname.
    CaEnvironment environment;
    environment.clearAll();
    environment.literouter().assign(literouter::pathToUtf8(wrong_host.ca_path));
    const auto started = proxy.start(wrong_host.config());
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (started) {
        LR_CHECK(!literouter::fetchStatus(proxy.snapshot().base_url,
                                         wrong_host.config().server.api_key).reachable);
    }
    proxy.stop();
}

void testListenerUrls() {
    LR_GROUP("listener URL helper keeps scheme and IPv6 literal brackets");
    literouter::ServerConfig config;
    LR_CHECK_EQ(literouter::serverBaseUrl(config, "/v1"), "http://127.0.0.1:8787/v1");
    config.host = "::1";
    config.tls_cert_file = "/test/chain.pem";
    LR_CHECK_EQ(literouter::serverBaseUrl(config), "https://[::1]:8787");
    LR_CHECK_EQ(literouter::ProxyServer::adminUrl(config, "/health"), "https://[::1]:8787/health");
    LR_CHECK_EQ(literouter::ProxyServer::adminUrl("::1", 42, "/health"), "http://[::1]:42/health");
}

} // namespace

int main() {
    testInvariantAndSystemFallback();
    testOverrideWins();
    testMissingOverrideIsSkipped();
    testEmptyOverrideIsIgnored();
    testPrecedence();
    testDirectoriesAreNotBundles();
    testProxyAbsentMeansDirect();
    testProxyPrecedence();
    testProxyUrlShapes();
    testProxyRejectsUnusableValues();
    testNoProxyList();
    testListenerUrls();
    testHttpsListener();
    testInvalidListenerCredentials();
    return LR_SUMMARY("test_tls");
}
