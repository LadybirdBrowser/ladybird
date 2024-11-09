/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
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
    JS_DECLARE_ALLOCATOR(SubtleCrypto);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SubtleCrypto> create(JS::Realm&);

    virtual ~SubtleCrypto() override;

    JS::NonnullGCPtr<WebIDL::Promise> encrypt(AlgorithmIdentifier const& algorithm, JS::NonnullGCPtr<CryptoKey> key, JS::Handle<WebIDL::BufferSource> const& data_parameter);
    JS::NonnullGCPtr<WebIDL::Promise> decrypt(AlgorithmIdentifier const& algorithm, JS::NonnullGCPtr<CryptoKey> key, JS::Handle<WebIDL::BufferSource> const& data_parameter);
    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> sign(AlgorithmIdentifier const& algorithm, JS::NonnullGCPtr<CryptoKey> key, JS::Handle<WebIDL::BufferSource> const& data_parameter);
    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> verify(AlgorithmIdentifier const& algorithm, JS::NonnullGCPtr<CryptoKey> key, JS::Handle<WebIDL::BufferSource> const& signature, JS::Handle<WebIDL::BufferSource> const& data_parameter);

    JS::NonnullGCPtr<WebIDL::Promise> digest(AlgorithmIdentifier const& algorithm, JS::Handle<WebIDL::BufferSource> const& data);

    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> generate_key(AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages);
    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> derive_bits(AlgorithmIdentifier algorithm, JS::NonnullGCPtr<CryptoKey> base_key, u32 length);
    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> derive_key(AlgorithmIdentifier algorithm, JS::NonnullGCPtr<CryptoKey> base_key, AlgorithmIdentifier derived_key_type, bool extractable, Vector<Bindings::KeyUsage> key_usages);

    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> import_key(Bindings::KeyFormat format, KeyDataType key_data, AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages);
    JS::ThrowCompletionOr<JS::NonnullGCPtr<WebIDL::Promise>> export_key(Bindings::KeyFormat format, JS::NonnullGCPtr<CryptoKey> key);

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
