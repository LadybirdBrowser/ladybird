/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Bindings/QuotaExceededError.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(QuotaExceededError);

QuotaExceededError::QuotaExceededError(Utf16String const& message)
    : DOMException("QuotaExceededError"_fly_string, message)
{
}

QuotaExceededError::QuotaExceededError(Utf16FlyString name, Utf16String const& message, Optional<double> quota, Optional<double> requested)
    : DOMException(move(name), message)
    , m_quota(quota)
    , m_requested(requested)
{
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(Utf16String const& message)
{
    return GC::Heap::the().allocate<QuotaExceededError>(message);
}

GC::Ref<QuotaExceededError> QuotaExceededError::create(Utf16FlyString name, Utf16String const& message, Optional<double> quota, Optional<double> requested)
{
    return GC::Heap::the().allocate<QuotaExceededError>(move(name), message, quota, requested);
}

// https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quotaexceedederror
ExceptionOr<GC::Ref<QuotaExceededError>> QuotaExceededError::create(JS::VM& vm, Utf16String const& message, QuotaExceededErrorOptions const& options)
{
    // 1. Set this’s name to "QuotaExceededError".
    // 2. Set this’s message to message.
    // NB: Done in constructor.
    auto error = create(message);

    // 3. If options["quota"] is present:
    if (options.quota.has_value()) {
        // 1. If options["quota"] is less than 0, then throw a RangeError.
        if (options.quota.value() < 0)
            return vm.throw_completion<JS::RangeError>("Quota cannot be less than 0"_utf16);

        // 2. Set this’s quota to options["quota"].
        error->m_quota = options.quota;
    }

    // 4. If options["requested"] is present:
    if (options.requested.has_value()) {
        // 1. If options["requested"] is less than 0, then throw a RangeError.
        if (options.requested.value() < 0)
            return vm.throw_completion<JS::RangeError>("Requested cannot be less than 0"_utf16);

        // 2. Set this’s requested to options["requested"].
        error->m_requested = options.requested;
    }

    // 5. If this’s quota is not null, this’s requested is not null, and this’s requested is less than this’s quota, then throw a RangeError.
    if (error->m_quota.has_value() && error->m_requested.has_value() && error->m_requested.value() < error->m_quota.value())
        return vm.throw_completion<JS::RangeError>("Requested cannot be less than quota"_utf16);

    return error;
}

}

// What the serialization steps of QuotaExceededError, which run in Rust (Libraries/LibWeb/Bindings/Rust/src/webidl),
// read of a QuotaExceededError and create one with. Rust lends the strings that it creates one with.
extern "C" {

void web_quota_exceeded_error_quota_and_requested(JSObject*, bool* has_quota, double* quota, bool* has_requested, double* requested);
JSObject* web_quota_exceeded_error_create(void* realm, FlatPtr name, FlatPtr message, bool has_quota, double quota, bool has_requested, double requested);
}

void web_quota_exceeded_error_quota_and_requested(JSObject* object, bool* has_quota, double* quota, bool* has_requested, double* requested)
{
    auto& error = Web::Bindings::impl_from<Web::WebIDL::QuotaExceededError>(*JS::HostABI::object_from_abi(object));
    *has_quota = error.quota().has_value();
    *quota = error.quota().value_or(0);
    *has_requested = error.requested().has_value();
    *requested = error.requested().value_or(0);
}

JSObject* web_quota_exceeded_error_create(void* realm, FlatPtr name, FlatPtr message, bool has_quota, double quota, bool has_requested, double requested)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto name_string = Utf16String::from_raw(name);
    auto error = Web::WebIDL::QuotaExceededError::create(Utf16FlyString::from_utf16(name_string.utf16_view()), Utf16String::from_raw(message),
        has_quota ? Optional<double> { quota } : OptionalNone {}, has_requested ? Optional<double> { requested } : OptionalNone {});
    return JS::HostABI::object_to_abi(Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(js_realm), js_realm, error).ptr());
}
