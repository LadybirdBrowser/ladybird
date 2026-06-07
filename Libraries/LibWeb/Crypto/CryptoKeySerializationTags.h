/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Types.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

namespace Web::Crypto {

// HandleTag and KeyAlgorithmTag values are part of the stable storage serialization format.
// Do not reorder or reuse values; leave removed tags reserved.
enum class HandleTag : u8 {
    ByteBuffer = 0,
    RSAPublicKey = 1,
    RSAPrivateKey = 2,
    ECPublicKey = 3,
    ECPrivateKey = 4,
    MLDSAPublicKey = 5,
    MLDSAPrivateKey = 6,
    MLKEMPublicKey = 7,
    MLKEMPrivateKey = 8,
    OKPPublicKey = 9,
    OKPPrivateKey = 10,
};

enum class KeyAlgorithmTag : u8 {
    KeyAlgorithm = 0,
    RsaKeyAlgorithm = 1,
    RsaHashedKeyAlgorithm = 2,
    EcKeyAlgorithm = 3,
    AesKeyAlgorithm = 4,
    HmacKeyAlgorithm = 5,
    KmacKeyAlgorithm = 6,
};

struct HandleTagExpectation {
    HandleTag tag;
    HTML::Expectation expectation;
};

struct KeyAlgorithmTagExpectation {
    KeyAlgorithmTag tag;
    HTML::Expectation expectation;
};

// Only tags listed here can be decoded.
static constexpr Array<HandleTagExpectation, 11> s_handle_tag_expectations { {
    { HandleTag::ByteBuffer, HTML::Expectation::PositiveGolden },
    { HandleTag::RSAPublicKey, HTML::Expectation::PositiveGolden },
    { HandleTag::RSAPrivateKey, HTML::Expectation::PositiveGolden },
    { HandleTag::ECPublicKey, HTML::Expectation::PositiveGolden },
    { HandleTag::ECPrivateKey, HTML::Expectation::PositiveGolden },
    { HandleTag::MLDSAPublicKey, HTML::Expectation::PositiveGolden },
    { HandleTag::MLDSAPrivateKey, HTML::Expectation::PositiveGolden },
    { HandleTag::MLKEMPublicKey, HTML::Expectation::PositiveGolden },
    { HandleTag::MLKEMPrivateKey, HTML::Expectation::PositiveGolden },
    { HandleTag::OKPPublicKey, HTML::Expectation::PositiveGolden },
    { HandleTag::OKPPrivateKey, HTML::Expectation::PositiveGolden },
} };

// WebCrypto RSA keys always use RsaHashedKeyAlgorithm, so tag 1 is a documented reject.
static constexpr Array<KeyAlgorithmTagExpectation, 7> s_key_algorithm_tag_expectations { {
    { KeyAlgorithmTag::KeyAlgorithm, HTML::Expectation::PositiveGolden },
    { KeyAlgorithmTag::RsaKeyAlgorithm, HTML::Expectation::DocumentedReject },
    { KeyAlgorithmTag::RsaHashedKeyAlgorithm, HTML::Expectation::PositiveGolden },
    { KeyAlgorithmTag::EcKeyAlgorithm, HTML::Expectation::PositiveGolden },
    { KeyAlgorithmTag::AesKeyAlgorithm, HTML::Expectation::PositiveGolden },
    { KeyAlgorithmTag::HmacKeyAlgorithm, HTML::Expectation::PositiveGolden },
    { KeyAlgorithmTag::KmacKeyAlgorithm, HTML::Expectation::PositiveGolden },
} };

static constexpr bool handle_tag_is_in_table(HandleTag tag)
{
    for (auto const& entry : s_handle_tag_expectations) {
        if (entry.tag == tag)
            return true;
    }
    return false;
}

static constexpr bool key_algorithm_tag_is_in_table(KeyAlgorithmTag tag)
{
    for (auto const& entry : s_key_algorithm_tag_expectations) {
        if (entry.tag == tag)
            return true;
    }
    return false;
}

}
