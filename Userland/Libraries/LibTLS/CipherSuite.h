/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibTLS/Extensions.h>

namespace TLS {

// Defined in RFC 5246 section 7.4.1.4.1
struct SignatureAndHashAlgorithm {
    HashAlgorithm hash;
    SignatureAlgorithm signature;
};

enum class KeyExchangeAlgorithm {
    Invalid,
    // Defined in RFC 5246 section 7.4.2 / RFC 4279 section 4
    RSA_PSK,
    // Defined in RFC 5246 section 7.4.3
    DHE_DSS,
    DHE_RSA,
    DH_anon,
    RSA,
    DH_DSS,
    DH_RSA,
    // Defined in RFC 4492 section 2
    ECDHE_RSA,
    ECDH_ECDSA,
    ECDH_RSA,
    ECDHE_ECDSA,
    ECDH_anon,
};

// Defined in RFC 5246 section 7.4.1.4.1
constexpr SignatureAlgorithm signature_for_key_exchange_algorithm(KeyExchangeAlgorithm algorithm)
{
    switch (algorithm) {
    case KeyExchangeAlgorithm::RSA:
    case KeyExchangeAlgorithm::DHE_RSA:
    case KeyExchangeAlgorithm::DH_RSA:
    case KeyExchangeAlgorithm::RSA_PSK:
    case KeyExchangeAlgorithm::ECDH_RSA:
    case KeyExchangeAlgorithm::ECDHE_RSA:
        return SignatureAlgorithm::RSA;
    case KeyExchangeAlgorithm::DHE_DSS:
    case KeyExchangeAlgorithm::DH_DSS:
        return SignatureAlgorithm::DSA;
    case KeyExchangeAlgorithm::ECDH_ECDSA:
    case KeyExchangeAlgorithm::ECDHE_ECDSA:
        return SignatureAlgorithm::ECDSA;
    case KeyExchangeAlgorithm::DH_anon:
    case KeyExchangeAlgorithm::ECDH_anon:
    default:
        return SignatureAlgorithm::ANONYMOUS;
    }
}

enum class CipherAlgorithm {
    Invalid,
    AES_128_CBC,
    AES_128_GCM,
    AES_128_CCM,
    AES_128_CCM_8,
    AES_256_CBC,
    AES_256_GCM,
};

constexpr size_t cipher_key_size(CipherAlgorithm algorithm)
{
    switch (algorithm) {
    case CipherAlgorithm::AES_128_CBC:
    case CipherAlgorithm::AES_128_GCM:
    case CipherAlgorithm::AES_128_CCM:
    case CipherAlgorithm::AES_128_CCM_8:
        return 128;
    case CipherAlgorithm::AES_256_CBC:
    case CipherAlgorithm::AES_256_GCM:
        return 256;
    case CipherAlgorithm::Invalid:
    default:
        return 0;
    }
}

}
