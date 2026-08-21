/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::CSS {

class CSSVariableReferenceValue : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CSSVariableReferenceValue, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CSSVariableReferenceValue);

public:
    [[nodiscard]] static GC::Ref<CSSVariableReferenceValue> create(Utf16FlyString variable, GC::Ptr<CSSUnparsedValue> fallback = nullptr);
    static WebIDL::ExceptionOr<GC::Ref<CSSVariableReferenceValue>> construct_impl(Utf16String variable, GC::Ptr<CSSUnparsedValue> fallback);

    virtual ~CSSVariableReferenceValue() override;

    Utf16FlyString const& variable() const;
    WebIDL::ExceptionOr<void> set_variable(Utf16String);

    GC::Ptr<CSSUnparsedValue> fallback() const;
    WebIDL::ExceptionOr<void> set_fallback(GC::Ptr<CSSUnparsedValue>);
    static constexpr size_t fallback_offset() { return offsetof(CSSVariableReferenceValue, m_fallback); }

    WebIDL::ExceptionOr<Utf16String> to_string() const;

private:
    CSSVariableReferenceValue(Utf16FlyString variable, GC::Ptr<CSSUnparsedValue> fallback);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Utf16FlyString m_variable;
    GC::Ptr<CSSUnparsedValue> m_fallback;
};

}
