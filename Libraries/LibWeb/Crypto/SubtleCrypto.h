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
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/SubtleCryptoPrototype.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/Crypto/CryptoKey.h>

namespace Web::Crypto {

class SubtleCrypto final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SubtleCrypto, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SubtleCrypto);

public:
    [[nodiscard]] static GC::Ref<SubtleCrypto> create(JS::Realm&);

    virtual ~SubtleCrypto() override;

    GC::Ref<WebIDL::Promise> encrypt(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter);
    GC::Ref<WebIDL::Promise> decrypt(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter);
    GC::Ref<WebIDL::Promise> sign(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter);
    GC::Ref<WebIDL::Promise> verify(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& signature, GC::Root<WebIDL::BufferSource> const& data_parameter);

    GC::Ref<WebIDL::Promise> digest(AlgorithmIdentifier const& algorithm, GC::Root<WebIDL::BufferSource> const& data);

    GC::Ref<WebIDL::Promise> generate_key(AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages);
    GC::Ref<WebIDL::Promise> derive_bits(AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, Optional<u32> length_optional);
    GC::Ref<WebIDL::Promise> derive_key(AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, AlgorithmIdentifier derived_key_type, bool extractable, Vector<Bindings::KeyUsage> key_usages);

    JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> import_key(Bindings::KeyFormat format, KeyDataType key_data, AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages);
    GC::Ref<WebIDL::Promise> export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key);

    GC::Ref<WebIDL::Promise> wrap_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key, GC::Ref<CryptoKey> wrapping_key, AlgorithmIdentifier wrap_algorithm);
    GC::Ref<WebIDL::Promise> unwrap_key(Bindings::KeyFormat format, KeyDataType wrapped_key, GC::Ref<CryptoKey> unwrapping_key, AlgorithmIdentifier unwrap_algorithm, AlgorithmIdentifier unwrapped_key_algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages);

private:
    explicit SubtleCrypto(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
};

struct NormalizedAlgorithmAndParameter {
    NonnullOwnPtr<AlgorithmMethods> methods;
    NonnullOwnPtr<AlgorithmParams> parameter;
};
WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> normalize_an_algorithm(JS::Realm&, AlgorithmIdentifier const& algorithm, String operation);

}
