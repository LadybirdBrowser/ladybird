/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Bindings {

struct QuotaExceededErrorOptions;

}

namespace Web::WebIDL {

using QuotaExceededErrorOptions = Bindings::QuotaExceededErrorOptions;

// https://webidl.spec.whatwg.org/#quotaexceedederror
class WEB_API QuotaExceededError final : public DOMException {
    WEB_WRAPPABLE(QuotaExceededError, DOMException);
    GC_DECLARE_ALLOCATOR(QuotaExceededError);

public:
    static GC::Ref<QuotaExceededError> create(Utf16String const& message);
    static GC::Ref<QuotaExceededError> create();

    static ExceptionOr<GC::Ref<QuotaExceededError>> create(JS::VM&, Utf16String const& message, QuotaExceededErrorOptions const&);

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

    // https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quota
    Optional<double> const& quota() const { return m_quota; }

    // https://webidl.spec.whatwg.org/#dom-quotaexceedederror-quota
    Optional<double> const& requested() const { return m_requested; }

protected:
    explicit QuotaExceededError(Utf16String const& message);
    QuotaExceededError();

private:
    // https://webidl.spec.whatwg.org/#quotaexceedederror-quota
    Optional<double> m_quota;

    // https://webidl.spec.whatwg.org/#quotaexceedederror-requested
    Optional<double> m_requested;
};

}
