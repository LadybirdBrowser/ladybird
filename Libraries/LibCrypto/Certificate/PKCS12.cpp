/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/Certificate/PKCS12.h>
#include <LibCrypto/OpenSSL.h>

#include <climits>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace Crypto::Certificate {

static ErrorOr<ByteBuffer> x509_to_pem(X509* cert)
{
    auto bio = TRY(OpenSSL_BIO::wrap(BIO_new(BIO_s_mem())));
    OPENSSL_TRY(PEM_write_bio_X509(bio.ptr(), cert));

    char* data = nullptr;
    long len = BIO_get_mem_data(bio.ptr(), &data);
    if (len <= 0 || !data)
        return Error::from_string_literal("Failed to read certificate PEM from BIO");

    return ByteBuffer::copy(data, static_cast<size_t>(len));
}

static ErrorOr<ByteBuffer> pkey_to_pem(EVP_PKEY* key)
{
    auto bio = TRY(OpenSSL_BIO::wrap(BIO_new(BIO_s_mem())));
    OPENSSL_TRY(PEM_write_bio_PrivateKey(bio.ptr(), key, nullptr, nullptr, 0, nullptr, nullptr));

    char* data = nullptr;
    long len = BIO_get_mem_data(bio.ptr(), &data);
    if (len <= 0 || !data)
        return Error::from_string_literal("Failed to read private key PEM from BIO");

    return ByteBuffer::copy(data, static_cast<size_t>(len));
}

// NOTE: Returns plaintext private key PEM in a heap-backed ByteBuffer.
// Key material is not zeroed on deallocation. Callers handling sensitive
// keys should clear the returned private_key_pem when done.
// FIXME: Use a zeroing buffer type when available.
ErrorOr<PKCS12Result> parse_pkcs12(ReadonlyBytes pkcs12_data, StringView password)
{
    if (pkcs12_data.size() > INT_MAX)
        return Error::from_string_literal("PKCS#12 data exceeds maximum supported size");

    auto bio_in = TRY(OpenSSL_BIO::wrap(
        BIO_new_mem_buf(pkcs12_data.data(), static_cast<int>(pkcs12_data.size()))));

    auto* p12_raw = d2i_PKCS12_bio(bio_in.ptr(), nullptr);
    if (!p12_raw) {
        ERR_clear_error();
        return Error::from_string_literal("Failed to decode PKCS#12 data (not a valid PKCS#12 file)");
    }
    auto p12 = TRY(OpenSSL_PKCS12::wrap(p12_raw));

    EVP_PKEY* pkey_raw = nullptr;
    X509* cert_raw = nullptr;
    STACK_OF(X509)* ca_raw = nullptr;

    // StringView is not guaranteed null-terminated; PKCS12_parse needs const char*.
    ByteString password_str(password);

    int parse_result = PKCS12_parse(p12.ptr(), password_str.characters(), &pkey_raw, &cert_raw, &ca_raw);

    ScopeGuard const free_pkey = [&] {
        if (pkey_raw)
            EVP_PKEY_free(pkey_raw);
    };
    ScopeGuard const free_cert = [&] {
        if (cert_raw)
            X509_free(cert_raw);
    };
    ScopeGuard const free_ca = [&] {
        if (ca_raw)
            sk_X509_pop_free(ca_raw, X509_free);
    };

    if (parse_result != 1) {
        // Drain the OpenSSL error queue silently — wrong password and corrupt
        // data are expected error paths, not worth logging to stderr.
        ERR_clear_error();
        return Error::from_string_literal("Failed to parse PKCS#12 data (wrong password or corrupt data)");
    }

    if (!cert_raw)
        return Error::from_string_literal("PKCS#12 file does not contain a certificate");
    if (!pkey_raw)
        return Error::from_string_literal("PKCS#12 file does not contain a private key");

    PKCS12Result result;
    result.certificate_pem = TRY(x509_to_pem(cert_raw));
    result.private_key_pem = TRY(pkey_to_pem(pkey_raw));

    if (ca_raw) {
        int ca_count = sk_X509_num(ca_raw);
        for (int i = 0; i < ca_count; ++i) {
            auto* ca_cert = sk_X509_value(ca_raw, i);
            TRY(result.ca_chain_pem.try_append(TRY(x509_to_pem(ca_cert))));
        }
    }

    return result;
}

}
