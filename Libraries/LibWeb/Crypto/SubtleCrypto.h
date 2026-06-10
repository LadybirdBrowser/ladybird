/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Export.h>

namespace Web::Crypto {

class SubtleCrypto final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SubtleCrypto, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SubtleCrypto);

public:
    [[nodiscard]] static GC::Ref<SubtleCrypto> create();

    virtual ~SubtleCrypto() override;

    GC::Ref<WebIDL::Promise> encrypt(JS::Realm&, AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, WebIDL::BufferSourceVariant const& data_parameter);
    GC::Ref<WebIDL::Promise> decrypt(JS::Realm&, AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, WebIDL::BufferSourceVariant const& data_parameter);
    GC::Ref<WebIDL::Promise> sign(JS::Realm&, AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, WebIDL::BufferSourceVariant const& data_parameter);
    GC::Ref<WebIDL::Promise> verify(JS::Realm&, AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, WebIDL::BufferSourceVariant const& signature, WebIDL::BufferSourceVariant const& data_parameter);

    GC::Ref<WebIDL::Promise> digest(JS::Realm&, AlgorithmIdentifier const& algorithm, WebIDL::BufferSourceVariant const& data);

    GC::Ref<WebIDL::Promise> generate_key(JS::Realm&, AlgorithmIdentifier algorithm, bool extractable, Vector<KeyUsage> key_usages);
    GC::Ref<WebIDL::Promise> derive_bits(JS::Realm&, AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, Optional<u32> length_optional);
    GC::Ref<WebIDL::Promise> derive_key(JS::Realm&, AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, AlgorithmIdentifier derived_key_type, bool extractable, Vector<KeyUsage> key_usages);

    GC::Ref<WebIDL::Promise> import_key(JS::Realm&, KeyFormat format, KeyDataType key_data, AlgorithmIdentifier algorithm, bool extractable, Vector<KeyUsage> key_usages);
    GC::Ref<WebIDL::Promise> export_key(JS::Realm&, KeyFormat format, GC::Ref<CryptoKey> key);

    GC::Ref<WebIDL::Promise> wrap_key(JS::Realm&, KeyFormat format, GC::Ref<CryptoKey> key, GC::Ref<CryptoKey> wrapping_key, AlgorithmIdentifier wrap_algorithm);
    GC::Ref<WebIDL::Promise> unwrap_key(JS::Realm&, KeyFormat format, WebIDL::BufferSourceVariant wrapped_key, GC::Ref<CryptoKey> unwrapping_key, AlgorithmIdentifier unwrap_algorithm, AlgorithmIdentifier unwrapped_key_algorithm, bool extractable, Vector<KeyUsage> key_usages);

    GC::Ref<WebIDL::Promise> encapsulate_key(JS::Realm&, AlgorithmIdentifier encapsulation_algorithm, GC::Ref<CryptoKey> encapsulation_key, AlgorithmIdentifier shared_key_algorithm, bool extractable, Vector<KeyUsage> key_usages);
    GC::Ref<WebIDL::Promise> encapsulate_bits(JS::Realm&, AlgorithmIdentifier encapsulation_algorithm, GC::Ref<CryptoKey> encapsulation_key);

    GC::Ref<WebIDL::Promise> decapsulate_key(JS::Realm&, AlgorithmIdentifier decapsulation_algorithm, GC::Ref<CryptoKey> decapsulation_key, WebIDL::BufferSourceVariant const& ciphertext, AlgorithmIdentifier shared_key_algorithm, bool extractable, Vector<KeyUsage> const& usages);
    GC::Ref<WebIDL::Promise> decapsulate_bits(JS::Realm&, AlgorithmIdentifier decapsulation_algorithm, GC::Ref<CryptoKey> decapsulation_key, WebIDL::BufferSourceVariant const& ciphertext);

private:
    SubtleCrypto();
};

struct NormalizedAlgorithmAndParameter {
    NonnullOwnPtr<AlgorithmMethods> methods;
    NonnullOwnPtr<AlgorithmParams> parameter;
};
WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> normalize_an_algorithm(JS::Realm&, AlgorithmIdentifier const& algorithm, String operation);

}

namespace Web::Bindings {

WEB_API JS::ThrowCompletionOr<GC::Ref<JS::Object>> create_algorithm_dictionary(JS::Realm&, String const& name);

WEB_API JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> import_key(
    JS::Realm&,
    Crypto::SubtleCrypto&,
    KeyFormat,
    Crypto::KeyDataType,
    Crypto::AlgorithmIdentifier,
    bool extractable,
    Vector<KeyUsage>);

WEB_API GC::Ref<WebIDL::Promise> generate_key(JS::Realm&, Crypto::SubtleCrypto&, Crypto::AlgorithmIdentifier, bool extractable, Vector<KeyUsage>);
WEB_API GC::Ref<WebIDL::Promise> derive_key(JS::Realm&, Crypto::SubtleCrypto&, Crypto::AlgorithmIdentifier, GC::Ref<Crypto::CryptoKey>, Crypto::AlgorithmIdentifier, bool extractable, Vector<KeyUsage>);
WEB_API GC::Ref<WebIDL::Promise> export_key(JS::Realm&, Crypto::SubtleCrypto&, KeyFormat, GC::Ref<Crypto::CryptoKey>);
WEB_API GC::Ref<WebIDL::Promise> wrap_key(JS::Realm&, Crypto::SubtleCrypto&, KeyFormat, GC::Ref<Crypto::CryptoKey>, GC::Ref<Crypto::CryptoKey>, Crypto::AlgorithmIdentifier);
WEB_API GC::Ref<WebIDL::Promise> unwrap_key(JS::Realm&, Crypto::SubtleCrypto&, KeyFormat, WebIDL::BufferSourceVariant, GC::Ref<Crypto::CryptoKey>, Crypto::AlgorithmIdentifier, Crypto::AlgorithmIdentifier, bool extractable, Vector<KeyUsage>);
WEB_API GC::Ref<WebIDL::Promise> encapsulate_key(JS::Realm&, Crypto::SubtleCrypto&, Crypto::AlgorithmIdentifier, GC::Ref<Crypto::CryptoKey>, Crypto::AlgorithmIdentifier, bool extractable, Vector<KeyUsage>);
WEB_API GC::Ref<WebIDL::Promise> decapsulate_key(JS::Realm&, Crypto::SubtleCrypto&, Crypto::AlgorithmIdentifier, GC::Ref<Crypto::CryptoKey>, WebIDL::BufferSourceVariant const&, Crypto::AlgorithmIdentifier, bool extractable, Vector<KeyUsage>);

}
