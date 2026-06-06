/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/QuotaExceededError.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(QuotaExceededError);

QuotaExceededError::QuotaExceededError()
    : DOMException()
{
}

QuotaExceededError::QuotaExceededError(Utf16String const& message)
    : DOMException("QuotaExceededError"_fly_string, message)
{
}

GC::Ref<QuotaExceededError> QuotaExceededError::create()
{
    return GC::Heap::the().allocate<QuotaExceededError>();
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(Utf16String const& message)
{
    return GC::Heap::the().allocate<QuotaExceededError>(message);
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(JS::Realm&)
{
    return create();
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(JS::Realm&, Utf16String const& message)
{
    return create(message);
}

// https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quotaexceedederror
ExceptionOr<GC::Ref<QuotaExceededError>> QuotaExceededError::construct_impl(JS::Realm& realm, Utf16String const& message, Bindings::QuotaExceededErrorOptions const& options)
{
    auto& vm = realm.vm();

    // 1. Set this’s name to "QuotaExceededError".
    // 2. Set this’s message to message.
    // NB: Done in constructor.
    auto error = create(message);

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
WebIDL::ExceptionOr<void> QuotaExceededError::serialization_steps(JS::Realm& realm, HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Run the DOMException serialization steps given value and serialized.
    MUST(DOMException::serialization_steps(realm, serialized, for_storage, memory));

    // 2. Set serialized.[[Quota]] to value’s quota.
    serialized.encode(m_quota);

    // 3. Set serialized.[[Requested]] to value’s requested.
    serialized.encode(m_requested);

    return {};
}

// https://webidl.spec.whatwg.org/#ref-for-quotaexceedederror⑦
WebIDL::ExceptionOr<void> QuotaExceededError::deserialization_steps(JS::Realm& realm, HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    // 1. Run the DOMException deserialization steps given serialized and value.
    MUST(DOMException::deserialization_steps(realm, serialized, memory));

    // 2. Set value’s quota to serialized.[[Quota]].
    m_quota = serialized.decode<Optional<double>>();

    // 3. Set value’s requested to serialized.[[Requested]].
    m_requested = serialized.decode<Optional<double>>();

    return {};
}

}
