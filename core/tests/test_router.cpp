// Router: the pure decision layer from lr_router.cpp. No sockets, no config
// file — candidate ordering, pass-through ranking, the circuit breaker and the
// health view are all exercised against hand-built AppConfig values.
#include "lr_test_check.h"

import literouter.core;

namespace {

using literouter::AppConfig;
using literouter::Candidate;
using literouter::ProviderConfig;
using literouter::ProviderHealth;
using literouter::RouteConfig;
using literouter::RouteTarget;
using literouter::Router;

ProviderConfig relay(std::string id, int priority = 100, int weight = 1) {
    ProviderConfig provider;
    provider.id = std::move(id);
    provider.name = provider.id;
    provider.base_url = "https://" + provider.id + ".example/v1";
    provider.priority = priority;
    provider.weight = weight;
    return provider;
}

std::vector<std::string> idsOf(const std::vector<Candidate> &candidates) {
    std::vector<std::string> out;
    out.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        out.push_back(candidate.provider);
    }
    return out;
}

std::vector<std::string> modelsOf(const std::vector<Candidate> &candidates) {
    std::vector<std::string> out;
    out.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        out.push_back(candidate.model);
    }
    return out;
}

const ProviderHealth *healthOf(const std::vector<ProviderHealth> &rows, std::string_view id) {
    for (const auto &row : rows) {
        if (row.provider == id) {
            return &row;
        }
    }
    return nullptr;
}

void testRouteOrderIsAuthorOrder() {
    LR_GROUP("candidatesFor: an authored route is followed in order");
    AppConfig config;
    config.providers = {relay("p1", 30), relay("p2", 10), relay("p3", 20)};
    RouteConfig route;
    route.model = "fast";
    // Deliberately not in priority order: a route's chain is authored, not
    // computed, so the file order is what the user gets.
    route.targets = {RouteTarget{.provider = "p3", .model = {}},
                     RouteTarget{.provider = "p1", .model = {}},
                     RouteTarget{.provider = "p2", .model = {}}};
    config.routes = {route};

    Router router;
    router.setConfig(config);
    const auto candidates = router.candidatesFor("fast");
    LR_CHECK(idsOf(candidates) == (std::vector<std::string>{"p3", "p1", "p2"}));
    // An empty target model means "ask upstream for the logical name".
    LR_CHECK(modelsOf(candidates) == (std::vector<std::string>{"fast", "fast", "fast"}));
    for (const auto &candidate : candidates) {
        LR_CHECK(!candidate.skipped);
    }

    // A target may rename the model for that hop only.
    config.routes[0].targets[1].model = "p1-upstream-name";
    router.setConfig(config);
    LR_CHECK(modelsOf(router.candidatesFor("fast")) ==
             (std::vector<std::string>{"fast", "p1-upstream-name", "fast"}));

    // Unknown providers and disabled hops drop out; the chain keeps its order.
    config.providers[1].enabled = false; // p2
    router.setConfig(config);
    config.routes[0].targets.push_back(RouteTarget{.provider = "ghost", .model = {}});
    router.setConfig(config);
    LR_CHECK(idsOf(router.candidatesFor("fast")) == (std::vector<std::string>{"p3", "p1"}));
}

void testDisabledProviderDroppedFromChain() {
    LR_GROUP("candidatesFor: a disabled relay is dropped from the chain");
    AppConfig config;
    config.providers = {relay("first"), relay("broken", 10), relay("third")};
    config.providers[1].enabled = false;
    RouteConfig route;
    route.model = "m";
    route.targets = {RouteTarget{.provider = "first", .model = {}},
                     RouteTarget{.provider = "broken", .model = {}},
                     RouteTarget{.provider = "third", .model = {}}};
    config.routes = {route};

    Router router;
    router.setConfig(config);
    LR_CHECK(idsOf(router.candidatesFor("m")) == (std::vector<std::string>{"first", "third"}));

    // A disabled route is ignored entirely, which is what makes the flag
    // useful for parking a chain without deleting it.
    config.routes[0].enabled = false;
    router.setConfig(config);
    LR_CHECK(router.candidatesFor("m").empty());
}

void testPassThroughOrdering() {
    LR_GROUP("candidatesFor: pass-through ranking");
    AppConfig config;
    ProviderConfig a = relay("a", 10, 1);
    ProviderConfig b = relay("b", 10, 5);
    ProviderConfig c = relay("c", 5, 1);
    ProviderConfig d = relay("d", 10, 5);
    ProviderConfig e = relay("e", 1, 1); // best priority, but does not serve "m"
    for (auto *provider : {&a, &b, &c, &d}) {
        provider->models = {"m"};
    }
    e.models = {"other"};
    config.providers = {a, b, c, d, e};

    Router router;
    router.setConfig(config);
    const auto candidates = router.candidatesFor("m");
    const auto ids = idsOf(candidates);
    // priority ascending, then weight descending, then declaration order:
    // c (5) beats the priority-10 band; inside that band b and d beat a.
    LR_CHECK(ids == (std::vector<std::string>{"c", "b", "d", "a"}));
    // With no route, the upstream model id is the logical name unchanged.
    LR_CHECK(modelsOf(candidates) == (std::vector<std::string>{"m", "m", "m", "m"}));
    // A relay that does not advertise the model is not a candidate at all.
    LR_CHECK(std::ranges::find(ids, "e") == ids.end());
}

void testPassThroughDisabled() {
    LR_GROUP("candidatesFor: pass_through_unknown = false");
    AppConfig config;
    ProviderConfig a = relay("a");
    a.models = {"public-model"};
    config.providers = {a};
    RouteConfig route;
    route.model = "routed";
    route.targets = {RouteTarget{.provider = "a", .model = {}}};
    config.routes = {route};
    config.server.pass_through_unknown = false;

    Router router;
    router.setConfig(config);
    LR_CHECK(router.candidatesFor("not-advertised").empty());
    // Even an advertised model is unroutable once pass-through is off: only a
    // route reaches a relay.
    LR_CHECK(router.candidatesFor("public-model").empty());
    LR_CHECK(idsOf(router.candidatesFor("routed")) == (std::vector<std::string>{"a"}));

    config.server.pass_through_unknown = true;
    router.setConfig(config);
    LR_CHECK(idsOf(router.candidatesFor("public-model")) == (std::vector<std::string>{"a"}));
}

void testRouteWinsOverPassThrough() {
    LR_GROUP("candidatesFor: a route wins over pass-through");
    AppConfig config;
    // "advertised" would rank first in pass-through (priority 1); the route
    // that claims the model still decides.
    ProviderConfig advertised = relay("advertised", 1, 1);
    advertised.models = {"shared"};
    ProviderConfig preferred = relay("preferred", 500, 1);
    ProviderConfig leftover = relay("leftover", 600, 1);
    config.providers = {advertised, preferred, leftover};
    RouteConfig route;
    route.model = "shared";
    route.targets = {RouteTarget{.provider = "preferred", .model = {}}};
    config.routes = {route};

    Router router;
    router.setConfig(config);
    LR_CHECK(idsOf(router.candidatesFor("shared")) == (std::vector<std::string>{"preferred"}));
}

void testCircuitBreaker() {
    LR_GROUP("circuit breaker");
    AppConfig config;
    config.server.circuit_failure_threshold = 3;
    config.server.circuit_cooldown_sec = 10;
    config.providers = {relay("p"), relay("q")};
    RouteConfig route;
    route.model = "m";
    route.targets = {RouteTarget{.provider = "p", .model = {}},
                     RouteTarget{.provider = "q", .model = {}}};
    config.routes = {route};

    Router router;
    router.setConfig(config);

    // The failure times are the real clock, because candidatesFor() reads the
    // clock itself when it decides whether a candidate is skipped.
    const double base = literouter::nowUnix();
    LR_CHECK(!router.circuitOpen("p", base));
    LR_CHECK(!router.circuitOpen("never-seen", base));
    LR_CHECK_EQ(router.openBreakerCount(base), 0);

    router.recordFailure("p", "boom", base);
    LR_CHECK_MSG(!router.circuitOpen("p", base), "one failure is below the threshold");
    router.recordFailure("p", "boom", base);
    LR_CHECK_MSG(!router.circuitOpen("p", base), "two failures are below the threshold");
    router.recordFailure("p", "boom", base);

    LR_CHECK(router.circuitOpen("p", base));
    LR_CHECK_EQ(router.openBreakerCount(base), 1);
    // The cooldown is measured from the failure that tripped it: base + 10.
    LR_CHECK(router.circuitOpen("p", base + 9.99));
    LR_CHECK_MSG(!router.circuitOpen("p", base + 10.0),
                 "the breaker closes once the cooldown elapsed");
    LR_CHECK_EQ(router.openBreakerCount(base + 10.0), 0);

    // An open breaker does not remove a candidate — it marks it skipped, so the
    // proxy still has something to try when every candidate is open.
    const auto skipped = router.candidatesFor("m");
    LR_CHECK(idsOf(skipped) == (std::vector<std::string>{"p", "q"}));
    LR_CHECK(skipped.size() == 2 && skipped[0].skipped && !skipped[1].skipped);

    // A success clears the count outright, so the next failure starts over.
    router.recordSuccess("p", 12.0, base + 1.0);
    LR_CHECK(!router.circuitOpen("p", base + 1.0));
    LR_CHECK_EQ(router.openBreakerCount(base + 1.0), 0);
    const auto healthyAgain = router.candidatesFor("m");
    LR_CHECK(healthyAgain.size() == 2 && !healthyAgain[0].skipped);
    router.recordFailure("p", "boom", base + 2.0);
    LR_CHECK(!router.circuitOpen("p", base + 2.0));

    // Health is per relay: q never failed, so it was never skipped.
    const auto rows = router.health(base + 2.0);
    const auto *q = healthOf(rows, "q");
    LR_CHECK(q != nullptr && q->state == ProviderHealth::State::Healthy);
    LR_CHECK(q != nullptr && q->consecutive_failures == 0);
}

void testHealthStates() {
    LR_GROUP("health()");
    AppConfig config;
    config.server.circuit_failure_threshold = 3;
    config.server.circuit_cooldown_sec = 10;
    config.providers = {relay("live"), relay("parked")};
    config.providers[1].enabled = false;

    Router router;
    router.setConfig(config);

    // One row per configured provider, in declaration order.
    auto rows = router.health(0.0);
    LR_CHECK_EQ(static_cast<long long>(rows.size()), 2);
    LR_CHECK(rows[0].provider == "live" && rows[1].provider == "parked");

    // A disabled relay is neither healthy nor failing.
    LR_CHECK(rows[0].state == ProviderHealth::State::Healthy);
    LR_CHECK(rows[1].state == ProviderHealth::State::Unknown);
    LR_CHECK_EQ(rows[0].stateName(), "healthy");
    LR_CHECK_EQ(rows[1].stateName(), "unknown");
    LR_CHECK_EQ(rows[0].consecutive_failures, 0);

    // Below the threshold: degraded, still usable.
    router.recordFailure("live", "connection failed", 100.0);
    rows = router.health(100.0);
    LR_CHECK(rows[0].state == ProviderHealth::State::Degraded);
    LR_CHECK_EQ(rows[0].consecutive_failures, 1);
    LR_CHECK_EQ(rows[0].total_failures, static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(rows[0].last_error, "connection failed");
    LR_CHECK_EQ(rows[0].stateName(), "degraded");

    // At the threshold the breaker trips while the cooldown runs.
    router.recordFailure("live", "connection failed", 100.0);
    router.recordFailure("live", "connection failed", 100.0);
    rows = router.health(100.0);
    LR_CHECK(rows[0].state == ProviderHealth::State::Open);
    LR_CHECK(lr_test::closeTo(rows[0].cooldown_remaining, 10.0, 1e-6));
    LR_CHECK(lr_test::closeTo(rows[0].open_until_unix, 110.0, 1e-6));
    LR_CHECK_EQ(rows[0].stateName(), "open");
    // A disabled relay reports Unknown even while the other side is open.
    LR_CHECK(rows[1].state == ProviderHealth::State::Unknown);

    // Halfway through the cooldown it is still open, with the remainder shown.
    rows = router.health(105.0);
    LR_CHECK(rows[0].state == ProviderHealth::State::Open);
    LR_CHECK(lr_test::closeTo(rows[0].cooldown_remaining, 5.0, 1e-6));

    // After the cooldown the next request is a probe, and reporting "degraded"
    // is honest: nothing has succeeded yet.
    rows = router.health(111.0);
    LR_CHECK(rows[0].state == ProviderHealth::State::Degraded);
    LR_CHECK(lr_test::closeTo(rows[0].cooldown_remaining, 0.0));
    LR_CHECK_EQ(rows[0].consecutive_failures, 3);
    LR_CHECK_EQ(rows[0].total_failures, static_cast<std::uint64_t>(3));

    // A long reason is truncated rather than growing the console row.
    router.recordFailure("parked", std::string(600, 'x'), 111.0);
    rows = router.health(111.0);
    LR_CHECK(rows[1].last_error.size() <= 243);
    LR_CHECK(literouter::endsWith(rows[1].last_error, "…"));

    // forget() drops a relay's state (used when a config removes it).
    router.forget("live");
    rows = router.health(111.0);
    LR_CHECK(rows.size() == 2 && rows[0].state == ProviderHealth::State::Healthy);
    LR_CHECK_EQ(rows[0].total_failures, static_cast<std::uint64_t>(0));
    LR_CHECK(!router.circuitOpen("live", 111.0));

    router.resetHealth();
    rows = router.health(111.0);
    LR_CHECK(rows.size() == 2 && rows[0].consecutive_failures == 0);
    LR_CHECK_EQ(rows[1].last_error, "");
}

void testRetryableStatus() {
    LR_GROUP("retryableStatus");
    for (const int status : {408, 409, 425, 429, 500, 501, 502, 503, 504, 599}) {
        LR_CHECK_MSG(Router::retryableStatus(status),
                     std::format("HTTP {} should be retryable", status));
    }
    for (const int status : {200, 201, 204, 301, 400, 401, 403, 404, 405, 418, 422, 451, 600}) {
        LR_CHECK_MSG(!Router::retryableStatus(status),
                     std::format("HTTP {} must not be retried", status));
    }
}

void testSetConfigReplacesModel() {
    LR_GROUP("setConfig / config()");
    Router router;
    AppConfig first;
    first.server.port = 1111;
    first.providers = {relay("p1")};
    router.setConfig(first);
    LR_CHECK_EQ(router.config().server.port, 1111);
    LR_CHECK_EQ(static_cast<long long>(router.config().providers.size()), 1);

    AppConfig second;
    second.server.port = 2222;
    second.providers = {relay("p2")};
    router.setConfig(second);
    LR_CHECK_EQ(router.config().server.port, 2222);
    LR_CHECK(router.config().provider("p1") == nullptr);
    LR_CHECK(router.config().provider("p2") != nullptr);

    // Health survives a config edit: saving the file must not un-trip a relay
    // that was just failing.
    AppConfig config;
    config.server.circuit_failure_threshold = 1;
    config.server.circuit_cooldown_sec = 30;
    config.providers = {relay("p1")};
    router.setConfig(config);
    router.recordFailure("p1", "boom", literouter::nowUnix());
    LR_CHECK(router.circuitOpen("p1", literouter::nowUnix()));
    router.setConfig(config);
    LR_CHECK(router.circuitOpen("p1", literouter::nowUnix()));
}

} // namespace

void testRetryAfterWindow() {
    LR_GROUP("a Retry-After window opens the breaker on the first failure");
    AppConfig config;
    config.server.circuit_failure_threshold = 3;
    config.server.circuit_cooldown_sec = 30;
    config.providers = {relay("p")};
    RouteConfig route;
    route.model = "m";
    route.targets = {RouteTarget{.provider = "p", .model = {}}};
    config.routes = {route};

    const double base = literouter::nowUnix();
    {
        // The upstream asked for longer than the operator configured: it wins.
        Router router;
        router.setConfig(config);
        router.recordFailure("p", "HTTP 429", base, 120.0);
        LR_CHECK_MSG(router.circuitOpen("p", base), "the named window did not open the breaker");
        LR_CHECK_EQ(router.openBreakerCount(base), 1);
        const auto health = router.health(base);
        LR_CHECK_EQ(static_cast<long long>(health.size()), 1);
        if (!health.empty()) {
            LR_CHECK(health[0].state == ProviderHealth::State::Open);
            LR_CHECK(health[0].cooldown_remaining > 30.0);
        }
        // And it is a window, not a permanent state: past it the relay is
        // degraded — the next request is the probe.
        LR_CHECK(!router.circuitOpen("p", base + 121.0));
        LR_CHECK_EQ(router.openBreakerCount(base + 121.0), 0);
    }
    {
        // A window shorter than the configured cooldown is raised to it: an
        // upstream cannot talk the operator into coming back sooner than the
        // policy allows.
        Router router;
        router.setConfig(config);
        router.recordFailure("p", "HTTP 429", base, 5.0);
        const auto health = router.health(base);
        LR_CHECK(!health.empty());
        if (!health.empty()) {
            LR_CHECK(health[0].state == ProviderHealth::State::Open);
            LR_CHECK(health[0].cooldown_remaining > 29.0);
            LR_CHECK(health[0].cooldown_remaining <= 30.0);
        }
    }
    {
        // Without a hint, one failure is still just one failure.
        Router router;
        router.setConfig(config);
        router.recordFailure("p", "HTTP 429", base);
        LR_CHECK(!router.circuitOpen("p", base));
        const auto health = router.health(base);
        LR_CHECK(!health.empty() && health[0].state == ProviderHealth::State::Degraded);
        LR_CHECK(!health.empty() && health[0].cooldown_remaining == 0.0);
    }
}

int main() {
    testRouteOrderIsAuthorOrder();
    testDisabledProviderDroppedFromChain();
    testPassThroughOrdering();
    testPassThroughDisabled();
    testRouteWinsOverPassThrough();
    testCircuitBreaker();
    testHealthStates();
    testRetryableStatus();
    testRetryAfterWindow();
    testSetConfigReplacesModel();
    return LR_SUMMARY("test_router");
}
