// The two gates in front of a relay: the per-relay admission limiter and the
// local response cache. Both are pure enough to exercise without a socket, which
// is the point of testing them here rather than only through the proxy.
#include "lr_test_check.h"

import literouter.core;

namespace {

using namespace literouter;

ProviderConfig providerWithLimits(std::string id, int max_concurrent, int rpm) {
    ProviderConfig p;
    p.id = std::move(id);
    p.base_url = "https://" + p.id + ".example/v1";
    p.max_concurrent = max_concurrent;
    p.requests_per_minute = rpm;
    return p;
}

void concurrencyLimit() {
    LR_GROUP("a relay at its concurrency limit is refused, and a released slot is reusable");

    AppConfig config;
    config.providers = {providerWithLimits("a", 2, 0)};
    UpstreamLimiter limiter;
    limiter.setConfig(config);

    LR_CHECK_EQ(limiter.limitFor("a"), 2);
    LR_CHECK_EQ(limiter.active("a"), std::size_t{0});

    auto first = limiter.admit("a", 1000.0);
    auto second = limiter.admit("a", 1000.0);
    LR_CHECK(first.has_value());
    LR_CHECK(second.has_value());
    LR_CHECK_EQ(limiter.active("a"), std::size_t{2});

    auto third = limiter.admit("a", 1000.0);
    LR_CHECK_MSG(!third.has_value(), "a third slot must not exist");
    if (!third) {
        LR_CHECK_EQ(third.error().retry_after_sec, 1);
        LR_CHECK(third.error().message.find("in flight") != std::string::npos);
    }

    // Releasing one slot makes exactly one more available — and the release is
    // the slot's destructor, so a path that forgets to release is a bug this
    // catches rather than a permanent leak. The slot has to be held in a
    // variable: a temporary returned by admit() is destroyed at the end of the
    // statement, which is exactly the behaviour that makes the release
    // deterministic.
    first->reset();
    LR_CHECK_EQ(limiter.active("a"), std::size_t{1});
    auto replacement = limiter.admit("a", 1000.0);
    LR_CHECK(replacement.has_value());
    LR_CHECK_MSG(!limiter.admit("a", 1000.0).has_value(),
                 "a temporary slot is released immediately, so this is still full");

    second->reset();
    replacement->reset();
    LR_CHECK_EQ(limiter.active("a"), std::size_t{0});

    // A relay with no limit is unlimited, and its counter still tracks.
    AppConfig open;
    open.providers = {providerWithLimits("b", 0, 0)};
    limiter.setConfig(open);
    std::vector<std::shared_ptr<UpstreamSlot>> held;
    for (int i = 0; i < 50; ++i) {
        auto slot = limiter.admit("b", 1000.0);
        LR_CHECK(slot.has_value());
        if (slot) {
            held.push_back(*slot);
        }
    }
    LR_CHECK_EQ(limiter.active("b"), std::size_t{50});
    LR_CHECK_EQ(limiter.limitFor("b"), 0);
}

void rateLimit() {
    LR_GROUP("a relay's rolling minute is enforced and its Retry-After is real");

    AppConfig config;
    config.providers = {providerWithLimits("a", 0, 2)};
    UpstreamLimiter limiter;
    limiter.setConfig(config);

    const double start = 5000.0;
    auto first = limiter.admit("a", start);
    LR_CHECK(first.has_value());
    // The first slot is released immediately: this limit is about starts, not
    // about how long a request takes.
    if (first) {
        first->reset();
    }
    auto second = limiter.admit("a", start + 10.0);
    LR_CHECK(second.has_value());
    if (second) {
        second->reset();
    }

    auto third = limiter.admit("a", start + 20.0);
    LR_CHECK_MSG(!third.has_value(), "the third start inside the minute must be refused");
    if (!third) {
        // The window opens when the oldest start leaves it: 40 seconds after the
        // first, not a flat second.
        LR_CHECK_MSG(third.error().retry_after_sec == 40,
                     std::format("retry_after_sec is {}", third.error().retry_after_sec));
        LR_CHECK(third.error().message.find("last minute") != std::string::npos);
    }

    // Past the window the limit admits again: at +61 the start from 5000 has
    // left it, and the one from 5010 has not.
    auto later = limiter.admit("a", start + 61.0);
    LR_CHECK(later.has_value());
    if (later) {
        later->reset();
    }
    // Two starts are now inside the minute (5010 and 5061), so the next is
    // refused: the window really is rolling rather than reset on a boundary.
    LR_CHECK_MSG(!limiter.admit("a", start + 62.0).has_value(),
                 "a rolling window must not reset just because a slot was released");
    // At +71 the start from 5010 has left it too, leaving room again.
    LR_CHECK(limiter.admit("a", start + 71.0).has_value());
    LR_CHECK(!limiter.admit("a", start + 72.0).has_value());
}

void limitConfigEdits() {
    LR_GROUP("editing a relay's limits takes effect without losing its slots");

    AppConfig config;
    config.providers = {providerWithLimits("a", 3, 0)};
    UpstreamLimiter limiter;
    limiter.setConfig(config);

    auto held = limiter.admit("a", 1000.0);
    LR_CHECK(held.has_value());
    LR_CHECK_EQ(limiter.active("a"), std::size_t{1});

    // Tightening the limit keeps the slot that is already out: dropping it would
    // hand out capacity that is already in use.
    config.providers[0].max_concurrent = 1;
    limiter.setConfig(config);
    LR_CHECK_EQ(limiter.limitFor("a"), 1);
    LR_CHECK_EQ(limiter.active("a"), std::size_t{1});
    LR_CHECK_MSG(!limiter.admit("a", 1000.0).has_value(),
                 "a tightened limit must refuse the next request");

    // Removing the relay while it holds a slot keeps the counter until the slot
    // comes back, and stops admitting in the meantime.
    AppConfig empty;
    limiter.setConfig(empty);
    auto refused = limiter.admit("a", 1000.0);
    LR_CHECK_MSG(!refused.has_value(), "a relay that left the config must stop admitting");
    if (!refused) {
        LR_CHECK(refused.error().message.find("no longer in the configuration") !=
                 std::string::npos);
    }
    LR_CHECK_EQ(limiter.active("a"), std::size_t{1});
    held->reset();
    LR_CHECK_EQ(limiter.active("a"), std::size_t{0});

    // Its entry went with its last slot, so a re-added relay starts clean rather
    // than inheriting a generation of counters that are not its own.
    limiter.setConfig(config);
    LR_CHECK_EQ(limiter.limitFor("a"), 1);
    LR_CHECK(limiter.admit("a", 1000.0).has_value());

    // reset() drops the counters without touching limits.
    limiter.reset();
    LR_CHECK_EQ(limiter.active("a"), std::size_t{0});
}

void cacheBasics() {
    LR_GROUP("the response cache stores, expires and evicts");

    ResponseCache cache;
    LR_CHECK(!cache.enabled());
    // A store while disabled is a no-op rather than a silent enable.
    cache.store("k", CachedResponse{200, "application/json", "body"}, 100.0);
    LR_CHECK(!cache.lookup("k", 100.0).has_value());
    LR_CHECK_EQ(cache.size(), std::size_t{0});

    cache.configure(60, 4);
    LR_CHECK(cache.enabled());
    cache.store("k", CachedResponse{200, "application/json", "hello"}, 100.0);
    LR_CHECK_EQ(cache.size(), std::size_t{1});

    auto hit = cache.lookup("k", 130.0);
    LR_CHECK(hit.has_value());
    if (hit) {
        LR_CHECK_EQ(hit->status, 200);
        LR_CHECK_EQ(hit->content_type, "application/json");
        LR_CHECK_EQ(hit->body, "hello");
    }
    LR_CHECK_EQ(cache.hits(), std::uint64_t{1});
    LR_CHECK_EQ(cache.misses(), std::uint64_t{0});

    // An entry that has reached its TTL is gone, and the miss is counted: a
    // cache that reports hits for expired entries is lying to its operator.
    LR_CHECK(!cache.lookup("k", 160.0).has_value());
    LR_CHECK_EQ(cache.size(), std::size_t{0});
    LR_CHECK_EQ(cache.misses(), std::uint64_t{1});
    LR_CHECK(!cache.lookup("never-stored", 100.0).has_value());
    LR_CHECK_EQ(cache.misses(), std::uint64_t{2});

    // The TTL is inclusive of the instant it expires at.
    cache.store("edge", CachedResponse{200, "text/plain", "x"}, 200.0);
    LR_CHECK(cache.lookup("edge", 259.999).has_value());
    LR_CHECK(!cache.lookup("edge", 260.0).has_value());
}

void cacheEviction() {
    LR_GROUP("the cache evicts the least recently used entry, not the newest");

    ResponseCache cache;
    cache.configure(600, 2);
    cache.store("a", CachedResponse{200, "text/plain", "A"}, 10.0);
    cache.store("b", CachedResponse{200, "text/plain", "B"}, 11.0);
    // Touching "a" makes "b" the least recently used.
    LR_CHECK(cache.lookup("a", 12.0).has_value());
    cache.store("c", CachedResponse{200, "text/plain", "C"}, 13.0);
    LR_CHECK_EQ(cache.size(), std::size_t{2});
    LR_CHECK_MSG(cache.lookup("a", 14.0).has_value(), "the recently used entry was evicted");
    LR_CHECK_MSG(!cache.lookup("b", 14.0).has_value(), "the least recently used entry survived");
    LR_CHECK(cache.lookup("c", 14.0).has_value());

    // An expired entry is evicted before a live one, so a dead entry never costs
    // a live hit. `old` is expired at 111 and `live` is not.
    cache.configure(10, 2);
    cache.store("old", CachedResponse{200, "text/plain", "O"}, 100.0);
    cache.store("live", CachedResponse{200, "text/plain", "L"}, 105.0);
    cache.store("new", CachedResponse{200, "text/plain", "N"}, 111.0);
    LR_CHECK_MSG(cache.lookup("live", 111.0).has_value(),
                 "a live entry was evicted while an expired one held its slot");
    LR_CHECK(cache.lookup("new", 111.0).has_value());
    LR_CHECK_MSG(!cache.lookup("old", 111.0).has_value(), "the expired entry must be gone");

    // Turning the cache off drops what it held: a later re-enable must not serve
    // an answer from before the operator turned it off.
    cache.store("x", CachedResponse{200, "text/plain", "X"}, 200.0);
    cache.configure(0, 2);
    LR_CHECK(!cache.enabled());
    LR_CHECK_EQ(cache.size(), std::size_t{0});
    cache.configure(600, 2);
    LR_CHECK(!cache.lookup("x", 200.0).has_value());

    // A zero entry count is a cache that cannot store, which the validator
    // refuses to configure.
    cache.configure(600, 0);
    LR_CHECK(!cache.enabled());
}

void cacheKeys() {
    LR_GROUP("cache keys separate accounts, protocols, models and bodies");

    const std::string base = ResponseCache::keyFor("alice", "openai", "gpt-4o", "{\"a\":1}");
    LR_CHECK_EQ(base.size(), std::size_t{64}); // sha256, hex
    LR_CHECK_EQ(base, ResponseCache::keyFor("alice", "openai", "gpt-4o", "{\"a\":1}"));

    // Every field that could change the answer changes the key.
    LR_CHECK(base != ResponseCache::keyFor("bob", "openai", "gpt-4o", "{\"a\":1}"));
    LR_CHECK(base != ResponseCache::keyFor("alice", "anthropic", "gpt-4o", "{\"a\":1}"));
    LR_CHECK(base != ResponseCache::keyFor("alice", "openai", "gpt-4o-mini", "{\"a\":1}"));
    LR_CHECK(base != ResponseCache::keyFor("alice", "openai", "gpt-4o", "{\"a\":2}"));
    // Two accounts sharing nothing is the property that matters most: an answer
    // carrying one account's private context must never reach another's.
    LR_CHECK(ResponseCache::keyFor("", "openai", "m", "b") !=
             ResponseCache::keyFor("alice", "openai", "m", "b"));

    // Field boundaries are length-prefixed, so moving a character across a
    // boundary is a different key rather than the same concatenation.
    LR_CHECK(ResponseCache::keyFor("ab", "c", "m", "b") !=
             ResponseCache::keyFor("a", "bc", "m", "b"));
    LR_CHECK(ResponseCache::keyFor("a", "b", "cd", "e") !=
             ResponseCache::keyFor("a", "b", "c", "de"));
}

} // namespace

int main() {
    concurrencyLimit();
    rateLimit();
    limitConfigEdits();
    cacheBasics();
    cacheEviction();
    cacheKeys();
    return LR_SUMMARY("test_gates");
}
