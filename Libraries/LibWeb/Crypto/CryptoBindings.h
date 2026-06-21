/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

// FIXME: Generate these from IDL
namespace Web::Crypto {

// https://w3c.github.io/webcrypto/#JsonWebKey-dictionary
struct RsaOtherPrimesInfo {
    Optional<Utf16String> r;
    Optional<Utf16String> d;
    Optional<Utf16String> t;
};

// https://w3c.github.io/webcrypto/#JsonWebKey-dictionary
struct JsonWebKey {
    Optional<Utf16String> kty;
    Optional<Utf16String> use;
    Optional<Vector<Utf16String>> key_ops;
    Optional<Utf16String> alg;
    Optional<bool> ext;
    Optional<Utf16String> crv;
    Optional<Utf16String> x;
    Optional<Utf16String> y;
    Optional<Utf16String> d;
    Optional<Utf16String> n;
    Optional<Utf16String> e;
    Optional<Utf16String> p;
    Optional<Utf16String> q;
    Optional<Utf16String> dp;
    Optional<Utf16String> dq;
    Optional<Utf16String> qi;
    Optional<Vector<RsaOtherPrimesInfo>> oth;
    Optional<Utf16String> k;

    // https://wicg.github.io/webcrypto-modern-algos/#partial-JsonWebKey-dictionary
    // The following fields are defined in draft-ietf-cose-dilithium-07
    Optional<Utf16String> pub;
    Optional<Utf16String> priv;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> to_object(JS::Realm&);

    static JS::ThrowCompletionOr<JsonWebKey> parse(JS::Realm& realm, ReadonlyBytes data);
};

}
