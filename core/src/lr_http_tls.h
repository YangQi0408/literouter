#pragma once
// Must be included after lr_tls.h and the module's standard-library import.
namespace literouter::detail {
inline void applyCaBundle(httplib::Client &client, const std::filesystem::path &path) {
    const auto pem = tlsFile(path);
    auto *store = static_cast<X509_STORE *>(httplib::tls::create_ca_store(pem.data(), pem.size()));
    if (!store) store = X509_STORE_new();
    if (!store) throw std::runtime_error("cannot allocate TLS certificate store");
    // An unreadable/malformed explicit bundle installs an empty store: fail
    // verification rather than silently falling back to unrelated system roots.
    client.set_ca_cert_store(store);
}
}
