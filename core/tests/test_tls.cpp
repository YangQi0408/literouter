// CA bundle resolution (lr_util.cpp). The bug this file exists for: mcpp builds
// OpenSSL from source, so its compiled-in default verify path points at the mcpp
// build tree instead of the machine's trust store — literouter then reports
// "upstream certificate rejected" for an endpoint curl reaches fine. The
// invariant that keeps that from coming back is "resolveCaBundle() is either
// empty or a file that exists".
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

} // namespace

int main() {
    testInvariantAndSystemFallback();
    testOverrideWins();
    testMissingOverrideIsSkipped();
    testEmptyOverrideIsIgnored();
    testPrecedence();
    testDirectoriesAreNotBundles();
    return LR_SUMMARY("test_tls");
}
