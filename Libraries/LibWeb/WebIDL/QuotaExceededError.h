/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#dictdef-quotaexceedederroroptions
struct QuotaExceededErrorOptions {
    Optional<double> quota;
    Optional<double> requested;
};

// https://webidl.spec.whatwg.org/#quotaexceedederror
class WEB_API QuotaExceededError final : public DOMException {
    WEB_PLATFORM_OBJECT(QuotaExceededError, DOMException);
    GC_DECLARE_ALLOCATOR(QuotaExceededError);

public:
    static GC::Ref<QuotaExceededError> create(JS::Realm&, Utf16String const& message);
    static GC::Ref<QuotaExceededError> create(JS::Realm&);

    static ExceptionOr<GC::Ref<QuotaExceededError>> construct_impl(JS::Realm&, Utf16String const& message = {}, QuotaExceededErrorOptions const& options = {});

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

    // https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quota
    Optional<double> const& quota() const { return m_quota; }

    // https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quota
    Optional<double> const& requested() const { return m_requested; }

protected:
    QuotaExceededError(JS::Realm&, Utf16String const& message);
    QuotaExceededError(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

private:
    // https://webidl.spec.whatwg.org/#quotaexceedederror-quota
    Optional<double> m_quota;

    // https://webidl.spec.whatwg.org/#quotaexceedederror-requested
    Optional<double> m_requested;
};

}
