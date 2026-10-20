// Shared assertion plumbing for the literouter core test binaries.
//
// mcpp discovers tests/**/*.cpp, builds one binary per file and reads the exit
// status as the result, so there is no framework here: a couple of counters, a
// line of output per failing check, and a summary the file's own main()
// returns. Named .h so mcpp does not try to build it as a test.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace lr_test {

inline int checks = 0;
inline int failures = 0;

inline void group(std::string_view name) {
    std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
    // Flushed per group: if a later check aborts the process, the output still
    // says which group it died in.
    std::fflush(stdout);
}

inline void check(bool ok, std::string_view expression, std::string_view file, int line,
                  std::string_view detail = {}) {
    ++checks;
    if (ok) {
        return;
    }
    ++failures;
    std::printf("FAIL %.*s:%d: %.*s", static_cast<int>(file.size()), file.data(), line,
                static_cast<int>(expression.size()), expression.data());
    if (!detail.empty()) {
        std::printf(" — %.*s", static_cast<int>(detail.size()), detail.data());
    }
    std::printf("\n");
}

// For the many checks whose value is a string: printing expected and actual is
// what makes a failure readable without re-running with a debugger.
inline void checkEqual(std::string_view actual, std::string_view expected, std::string_view what,
                       std::string_view file, int line) {
    ++checks;
    if (actual == expected) {
        return;
    }
    ++failures;
    std::printf("FAIL %.*s:%d: %.*s\n     expected [%.*s]\n     actual   [%.*s]\n",
                static_cast<int>(file.size()), file.data(), line, static_cast<int>(what.size()),
                what.data(), static_cast<int>(expected.size()), expected.data(),
                static_cast<int>(actual.size()), actual.data());
}

inline void checkEqual(long long actual, long long expected, std::string_view what,
                       std::string_view file, int line) {
    ++checks;
    if (actual == expected) {
        return;
    }
    ++failures;
    std::printf("FAIL %.*s:%d: %.*s\n     expected [%lld]\n     actual   [%lld]\n",
                static_cast<int>(file.size()), file.data(), line, static_cast<int>(what.size()),
                what.data(), expected, actual);
}

inline void note(std::string_view text) {
    std::printf("   %.*s\n", static_cast<int>(text.size()), text.data());
}

inline int summary(std::string_view subject) {
    std::printf("%.*s: %d checks, %d failed\n", static_cast<int>(subject.size()), subject.data(),
                checks, failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}

inline bool closeTo(double a, double b, double epsilon = 1e-9) {
    return std::fabs(a - b) <= epsilon;
}

// Saves an environment variable and puts it back when the scope ends, so a
// check that fails halfway cannot leave the environment (or a later check in
// the same binary) changed. `assign("")` on Windows is how _putenv_s spells
// "unset".
class EnvGuard {
public:
    explicit EnvGuard(const char *name) : name_(name) {
        if (const char *value = std::getenv(name_); value != nullptr) {
            previous_ = value;
        }
    }

    ~EnvGuard() {
        if (previous_) {
            assign(*previous_);
        } else {
            clear();
        }
    }

    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

    void assign(const std::string &value) const {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    void clear() const {
#ifdef _WIN32
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }

private:
    const char *name_;
    std::optional<std::string> previous_;
};

// Silences the standard proxy environment for the rest of the scope.
//
// Every stub relay in the core tests listens on loopback and is reached through
// the same upstream client the product uses, so a machine with HTTPS_PROXY
// exported — every developer inside a corporate network, and some CI images —
// would have the whole suite tunnel to 127.0.0.1 through a proxy and fail for a
// reason that has nothing to do with the code under test. Tests that are ABOUT
// the proxy set their own values on top of this, and each EnvGuard restores
// whatever the shell had when the scope ends.
//
// NO_PROXY is cleared rather than set to "*": "no proxy is configured" and "a
// proxy is configured but bypassed for everything" are different states, and a
// suite should run in the first one.
class ProxyEnvironment {
public:
    ProxyEnvironment() { clear(); }

    void clear() {
        literouter_.clear();
        https_upper_.clear();
        https_lower_.clear();
        http_upper_.clear();
        http_lower_.clear();
        no_proxy_upper_.clear();
        no_proxy_lower_.clear();
    }

    // Named accessors, so a resolution test can set one variable at a time
    // without reaching for the environment directly.
    EnvGuard &literouter() { return literouter_; }
    EnvGuard &httpsUpper() { return https_upper_; }
    EnvGuard &httpsLower() { return https_lower_; }
    EnvGuard &httpUpper() { return http_upper_; }
    EnvGuard &httpLower() { return http_lower_; }
    EnvGuard &noProxyUpper() { return no_proxy_upper_; }
    EnvGuard &noProxyLower() { return no_proxy_lower_; }

private:
    EnvGuard literouter_{"LITEROUTER_PROXY"};
    EnvGuard https_upper_{"HTTPS_PROXY"};
    EnvGuard https_lower_{"https_proxy"};
    EnvGuard http_upper_{"HTTP_PROXY"};
    EnvGuard http_lower_{"http_proxy"};
    EnvGuard no_proxy_upper_{"NO_PROXY"};
    EnvGuard no_proxy_lower_{"no_proxy"};
};

} // namespace lr_test

#define LR_GROUP(name) ::lr_test::group(name)
#define LR_CHECK(cond) ::lr_test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define LR_CHECK_MSG(cond, detail) \
    ::lr_test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__, (detail))
#define LR_CHECK_EQ(actual, expected) \
    ::lr_test::checkEqual((actual), (expected), #actual, __FILE__, __LINE__)
#define LR_NOTE(text) ::lr_test::note(text)
#define LR_SUMMARY(subject) ::lr_test::summary(subject)
