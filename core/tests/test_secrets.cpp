// Secret handling from lr_util.cpp: the ${ENV} reference forms a config file
// stores, and the masked form the console displays. The environment is the
// input here, so every variable this file touches is saved and restored.
#include "lr_test_check.h"

import literouter.core;

namespace {

constexpr const char *kVar = "LITEROUTER_TEST_SECRET";

using lr_test::EnvGuard;

void testResolveSecretLiteralAndEmpty() {
    LR_GROUP("resolveSecret: literal and empty");
    LR_CHECK_EQ(literouter::resolveSecret("sk-literal"), "sk-literal");
    LR_CHECK_EQ(literouter::resolveSecret("  sk-literal  "), "sk-literal");
    LR_CHECK_EQ(literouter::resolveSecret(""), "");
    LR_CHECK_EQ(literouter::resolveSecret("    "), "");
    // A `$` that does not open the field is just a character in a key.
    LR_CHECK_EQ(literouter::resolveSecret("abc$DEF"), "abc$DEF");
    LR_CHECK_EQ(literouter::resolveSecret("key-with${inside}braces-but-not-first"),
                "key-with${inside}braces-but-not-first");
}

void testResolveSecretEnvironment() {
    LR_GROUP("resolveSecret: environment references");
    const EnvGuard guard{kVar};

    guard.assign("sk-from-env");
    LR_CHECK_EQ(literouter::resolveSecret("${" + std::string{kVar} + "}"), "sk-from-env");
    LR_CHECK_EQ(literouter::resolveSecret("$" + std::string{kVar}), "sk-from-env");
    // Whitespace around the reference is trimmed before parsing.
    LR_CHECK_EQ(literouter::resolveSecret("  ${" + std::string{kVar} + "}  "), "sk-from-env");

    // Braced reference to an unset variable expands to nothing (not to the
    // reference text, and not to "undefined").
    guard.clear();
    LR_CHECK_EQ(literouter::resolveSecret("${" + std::string{kVar} + "}"), "");
    LR_CHECK_EQ(literouter::resolveSecret("$" + std::string{kVar}), "");
    // An empty variable is set-but-empty, which is still an answer: "".
    guard.assign("");
    LR_CHECK_EQ(literouter::resolveSecret("${" + std::string{kVar} + "}"), "");
}

void testResolveSecretFallback() {
    LR_GROUP("resolveSecret: ${NAME:-fallback}");
    const EnvGuard guard{kVar};
    const std::string reference = "${" + std::string{kVar} + ":-sk-fallback}";

    guard.assign("sk-from-env");
    LR_CHECK_EQ(literouter::resolveSecret(reference), "sk-from-env");

    guard.clear();
    LR_CHECK_EQ(literouter::resolveSecret(reference), "sk-fallback");
    // The fallback may itself contain characters that are not identifier bytes.
    LR_CHECK_EQ(literouter::resolveSecret("${" + std::string{kVar} + ":-a b:c/d}"), "a b:c/d");
    // An empty fallback is a legal way to spell "unset means no key".
    LR_CHECK_EQ(literouter::resolveSecret("${" + std::string{kVar} + ":-}"), "");
#ifndef _WIN32
    // An empty variable counts as set, so the fallback is not used. On Windows,
    // the CRT and OS do not represent empty environment variables (assigning ""
    // unsets the variable), so this distinction only exists on POSIX.
    guard.assign("");
    LR_CHECK_EQ(literouter::resolveSecret(reference), "");
#endif
}

void testResolveSecretMalformed() {
    LR_GROUP("resolveSecret: malformed references");
    // An unterminated "${" is treated as the literal text, not as a hole to
    // guess a name for. Same for a lone "$".
    LR_CHECK_EQ(literouter::resolveSecret("${UNTERMINATED"), "${UNTERMINATED");
    LR_CHECK_EQ(literouter::resolveSecret("${"), "${");
    LR_CHECK_EQ(literouter::resolveSecret("$"), "$");
    // "$$" names nothing, so it expands to nothing.
    LR_CHECK_EQ(literouter::resolveSecret("$$FOO"), "");
}

void testIsSecretReference() {
    LR_GROUP("isSecretReference");
    LR_CHECK(literouter::isSecretReference("${OPENAI_API_KEY}"));
    LR_CHECK(literouter::isSecretReference("$OPENAI_API_KEY"));
    LR_CHECK(literouter::isSecretReference("  ${OPENAI_API_KEY}  "));
    // Whatever the reference's value is, the field still reads as a reference:
    // that is what lets the console render "from $OPENAI_API_KEY".
    LR_CHECK(literouter::isSecretReference("$UNSET_FOR_SURE"));
    LR_CHECK(!literouter::isSecretReference("sk-literal"));
    LR_CHECK(!literouter::isSecretReference(""));
    LR_CHECK(!literouter::isSecretReference("   "));
    // A single "$" is too short to be a reference.
    LR_CHECK(!literouter::isSecretReference("$"));
}

void testSecretReferenceName() {
    LR_GROUP("secretReferenceName");
    LR_CHECK_EQ(literouter::secretReferenceName("${OPENAI_API_KEY}"), "OPENAI_API_KEY");
    LR_CHECK_EQ(literouter::secretReferenceName("$OPENAI_API_KEY"), "OPENAI_API_KEY");
    LR_CHECK_EQ(literouter::secretReferenceName("${OPENAI_API_KEY:-sk-x}"), "OPENAI_API_KEY");
    LR_CHECK_EQ(literouter::secretReferenceName("  ${A_B_1}  "), "A_B_1");
    // Not a reference, or a reference with no name in it.
    LR_CHECK_EQ(literouter::secretReferenceName("sk-literal"), "");
    LR_CHECK_EQ(literouter::secretReferenceName(""), "");
    LR_CHECK_EQ(literouter::secretReferenceName("$"), "");
    LR_CHECK_EQ(literouter::secretReferenceName("${"), "");
    LR_CHECK_EQ(literouter::secretReferenceName("${UNTERMINATED"), "");
    LR_CHECK_EQ(literouter::secretReferenceName("${:-fallback-only}"), "");
}

void testMaskSecret() {
    LR_GROUP("maskSecret");
    LR_CHECK_EQ(literouter::maskSecret(""), "(none)");

    // Short values are masked completely — there is nothing safe to show.
    LR_CHECK_EQ(literouter::maskSecret("a"), "*");
    LR_CHECK_EQ(literouter::maskSecret("abcd"), "****");
    LR_CHECK_EQ(literouter::maskSecret("12345678"), "********");

    // Longer values keep four bytes at each end, enough to tell two keys apart.
    LR_CHECK_EQ(literouter::maskSecret("123456789"), "1234****…6789");
    LR_CHECK_EQ(literouter::maskSecret("sk-abcdefghijkl"), "sk-a****…ijkl");
    // The middle of a long key is never echoed back.
    LR_CHECK(literouter::maskSecret("sk-SECRETMIDDLE0123").find("SECRETMIDDLE") ==
             std::string::npos);
}

} // namespace

int main() {
    testResolveSecretLiteralAndEmpty();
    testResolveSecretEnvironment();
    testResolveSecretFallback();
    testResolveSecretMalformed();
    testIsSecretReference();
    testSecretReferenceName();
    testMaskSecret();
    return LR_SUMMARY("test_secrets");
}
