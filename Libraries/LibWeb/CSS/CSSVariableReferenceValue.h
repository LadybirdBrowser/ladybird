/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::CSS {

class CSSVariableReferenceValue : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSVariableReferenceValue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSVariableReferenceValue);

public:
    [[nodiscard]] static GC::Ref<CSSVariableReferenceValue> create(JS::Realm&, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback = nullptr);
    static WebIDL::ExceptionOr<GC::Ref<CSSVariableReferenceValue>> construct_impl(JS::Realm&, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback);

    virtual ~CSSVariableReferenceValue() override;

    String variable() const;
    WebIDL::ExceptionOr<void> set_variable(FlyString);

    GC::Ptr<CSSUnparsedValue> fallback() const;
    WebIDL::ExceptionOr<void> set_fallback(GC::Ptr<CSSUnparsedValue>);

    WebIDL::ExceptionOr<String> to_string() const;

private:
    CSSVariableReferenceValue(JS::Realm&, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    FlyString m_variable;
    GC::Ptr<CSSUnparsedValue> m_fallback;
};

}
