/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/QuotaExceededErrorPrototype.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(QuotaExceededError);

QuotaExceededError::QuotaExceededError(JS::Realm& realm)
    : DOMException(realm)
{
}

QuotaExceededError::QuotaExceededError(JS::Realm& realm, Utf16String const& message)
    : DOMException(realm, "QuotaExceededError"_fly_string, message)
{
}

void QuotaExceededError::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(QuotaExceededError);
    Base::initialize(realm);
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(JS::Realm& realm)
{
    return realm.create<QuotaExceededError>(realm);
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(JS::Realm& realm, Utf16String const& message)
{
    return realm.create<QuotaExceededError>(realm, message);
}

// https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quotaexceedederror
ExceptionOr<GC::Ref<QuotaExceededError>> QuotaExceededError::construct_impl(JS::Realm& realm, Utf16String const& message, QuotaExceededErrorOptions const& options)
{
    auto& vm = realm.vm();

    // 1. Set this’s name to "QuotaExceededError".
    // 2. Set this’s message to message.
    // NB: Done in constructor.
    auto error = realm.create<QuotaExceededError>(realm, message);

    // 3. If options["quota"] is present:
    if (options.quota.has_value()) {
        // 1. If options["quota"] is less than 0, then throw a RangeError.
        if (options.quota.value() < 0)
            return vm.throw_completion<JS::RangeError>("Quota cannot be less than 0"sv);

        // 2. Set this’s quota to options["quota"].
        error->m_quota = options.quota;
    }

    // 4. If options["requested"] is present:
    if (options.requested.has_value()) {
        // 1. If options["requested"] is less than 0, then throw a RangeError.
        if (options.requested.value() < 0)
            return vm.throw_completion<JS::RangeError>("Requested cannot be less than 0"sv);

        // 2. Set this’s requested to options["requested"].
        error->m_requested = options.requested;
    }

    // 5. If this’s quota is not null, this’s requested is not null, and this’s requested is less than this’s quota, then throw a RangeError.
    if (error->m_quota.has_value() && error->m_requested.has_value() && error->m_requested.value() < error->m_quota.value())
        return vm.throw_completion<JS::RangeError>("Requested cannot be less than quota"sv);

    return error;
}

// https://webidl.spec.whatwg.org/#ref-for-quotaexceedederror⑦
WebIDL::ExceptionOr<void> QuotaExceededError::serialization_steps(HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Run the DOMException serialization steps given value and serialized.
    MUST(DOMException::serialization_steps(serialized, for_storage, memory));

    // 2. Set serialized.[[Quota]] to value’s quota.
    serialized.encode(m_quota);

    // 3. Set serialized.[[Requested]] to value’s requested.
    serialized.encode(m_requested);

    return {};
}

// https://webidl.spec.whatwg.org/#ref-for-quotaexceedederror⑦
WebIDL::ExceptionOr<void> QuotaExceededError::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    // 1. Run the DOMException deserialization steps given serialized and value.
    MUST(DOMException::deserialization_steps(serialized, memory));

    // 2. Set value’s quota to serialized.[[Quota]].
    m_quota = serialized.decode<Optional<double>>();

    // 3. Set value’s requested to serialized.[[Requested]].
    m_requested = serialized.decode<Optional<double>>();

    return {};
}

}
