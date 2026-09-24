// Private: the nlohmann `dump()` this codebase should use everywhere.
//
// nlohmann's default error handler is `strict`, which *throws*
// `type_error.316` on a string that is not valid UTF-8. Nothing here catches
// that — the telemetry flusher runs on its own thread, where an escaping
// exception is `std::terminate` — so one bad byte (an upstream body, a provider
// id, a file name from a locale whose encoding is not UTF-8) would take the
// whole process down.
//
// `replace` swaps such a byte for U+FFFD and always returns a string. A mangled
// character in a log line beats an abort, and the alternative — validating every
// string before it enters a `json` node — is a rule that would have to be
// re-applied at each of the ~95 call sites and would rot at the first new one.
//
// Templated on the node type so it needs no alias and can be included at file
// scope, right after `import nlohmann.json;`.
template <class Json>
std::string dumpJson(const Json &node, int indent = -1) {
    return node.dump(indent, ' ', false, Json::error_handler_t::replace);
}
