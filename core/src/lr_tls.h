// Internal OpenSSL loaders. File access uses std::filesystem's native path
// overload; OpenSSL sees PEM bytes, never a lossy Windows ANSI filename.
#pragma once

namespace literouter::detail {
inline std::string tlsFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return {};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

inline bool loadCertificateChain(SSL_CTX *context, const std::filesystem::path &path) {
    const std::string pem = tlsFile(path);
    if (pem.empty() || pem.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    const std::unique_ptr<BIO, decltype(&BIO_free)> input{
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free};
    if (!input) return false;
    const std::unique_ptr<X509, decltype(&X509_free)> leaf{
        PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free};
    if (!leaf || SSL_CTX_use_certificate(context, leaf.get()) != 1) return false;
    while (X509 *next = PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr)) {
        if (SSL_CTX_add_extra_chain_cert(context, next) != 1) {
            X509_free(next);
            return false;
        }
    }
    const auto error = ERR_peek_last_error();
    if (error && !(ERR_GET_LIB(error) == ERR_LIB_PEM &&
                   ERR_GET_REASON(error) == PEM_R_NO_START_LINE && BIO_eof(input.get()))) {
        return false; // a truncated/malformed intermediate is not ordinary EOF
    }
    ERR_clear_error(); // ordinary end-of-PEM is not a later handshake error
    return true;
}

inline bool loadPrivateKey(SSL_CTX *context, const std::filesystem::path &path) {
    const std::string pem = tlsFile(path);
    if (pem.empty() || pem.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    const std::unique_ptr<BIO, decltype(&BIO_free)> input{
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free};
    if (!input) return false;
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
        PEM_read_bio_PrivateKey(input.get(), nullptr,
            [](char *, int, int, void *) -> int { return 0; }, nullptr), EVP_PKEY_free};
    return key && SSL_CTX_use_PrivateKey(context, key.get()) == 1 && SSL_CTX_check_private_key(context) == 1;
}
}
