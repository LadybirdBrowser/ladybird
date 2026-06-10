/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/SubtleCrypto.h>

namespace Web::Crypto {

struct CryptoKeyPair;
struct EncapsulatedBits;
struct EncapsulatedKey;

using KeyFormat = Bindings::KeyFormat;
using RsaOtherPrimesInfo = Bindings::RsaOtherPrimesInfo;
using JsonWebKey = Bindings::JsonWebKey;

JS::ThrowCompletionOr<JsonWebKey> parse_json_web_key(JS::Realm& realm, ReadonlyBytes data);
void resolve_crypto_key_promise(JS::Realm&, WebIDL::Promise&, GC::Ref<CryptoKey>);
JS::ThrowCompletionOr<GC::Ref<JS::Object>> encapsulated_bits(JS::Realm&, EncapsulatedBits const&);
JS::ThrowCompletionOr<GC::Ref<JS::Object>> encapsulated_key(JS::Realm&, EncapsulatedKey const&);
JS::Value crypto_key(JS::Realm&, GC::Ref<CryptoKey>);
JS::ThrowCompletionOr<GC::Ref<JS::Object>> crypto_key_pair(JS::Realm&, CryptoKeyPair const&);
JS::ThrowCompletionOr<GC::Ref<CryptoKey>> crypto_key_from_value(JS::VM&, JS::Value);

}
