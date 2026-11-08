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
#include <LibWeb/Bindings/SubtleCrypto.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

using KeyFormat = Bindings::KeyFormat;

struct RsaOtherPrimesInfo {
    Optional<Utf16String> d {};
    Optional<Utf16String> r {};
    Optional<Utf16String> t {};
};

struct JsonWebKey {
    Optional<Utf16String> alg {};
    Optional<Utf16String> crv {};
    Optional<Utf16String> d {};
    Optional<Utf16String> dp {};
    Optional<Utf16String> dq {};
    Optional<Utf16String> e {};
    Optional<bool> ext {};
    Optional<Utf16String> k {};
    Optional<Vector<Utf16String>> key_ops {};
    Optional<Utf16String> kty {};
    Optional<Utf16String> n {};
    Optional<Vector<RsaOtherPrimesInfo>> oth {};
    Optional<Utf16String> p {};
    Optional<Utf16String> priv {};
    Optional<Utf16String> pub {};
    Optional<Utf16String> q {};
    Optional<Utf16String> qi {};
    Optional<Utf16String> use {};
    Optional<Utf16String> x {};
    Optional<Utf16String> y {};
};

JS::ThrowCompletionOr<GC::Ref<CryptoKey>> crypto_key_from_value(JS::VM&, JS::Value);

}
