#pragma once
// The `dump()` the CLI should use everywhere it prints JSON.
//
// nlohmann's default error handler is `strict`, which *throws* on a string that
// is not valid UTF-8 — and a CLI that throws out of `main` prints a libc++abi
// abort message instead of the JSON the caller asked for. That is reachable
// without any hostile input: a relay id, a model name or an upstream body can
// carry bytes that are not UTF-8, and a config path on Windows goes through the
// ANSI code page.
//
// `replace` turns such a byte into U+FFFD and always returns a string. Core has
// the same helper in `core/src/lr_dump.h`; the two exist because the CLI does
// not have core's source directory on its include path, and a shared header is
// not worth an exported dependency on nlohmann (the core module deliberately
// exports no third-party type).
//
// Include AFTER `import nlohmann.json;`. Templated on the node type so it needs
// no alias.
template <class Json>
std::string dumpJson(const Json &node, int indent = -1) {
    return node.dump(indent, ' ', false, Json::error_handler_t::replace);
}
