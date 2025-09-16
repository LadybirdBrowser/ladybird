/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

using CSSUnparsedSegment = Variant<String, GC::Ref<CSSVariableReferenceValue>>;
using GCRootCSSUnparsedSegment = Variant<String, GC::Root<CSSVariableReferenceValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssunparsedvalue
class CSSUnparsedValue final : public CSSStyleValue {
    WEB_PLATFORM_OBJECT(CSSUnparsedValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSUnparsedValue);

public:
    [[nodiscard]] static GC::Ref<CSSUnparsedValue> create(JS::Realm&, Vector<GCRootCSSUnparsedSegment>);
    static WebIDL::ExceptionOr<GC::Ref<CSSUnparsedValue>> construct_impl(JS::Realm&, Vector<GCRootCSSUnparsedSegment>);

    virtual ~CSSUnparsedValue() override;

    WebIDL::UnsignedLong length() const;
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(u32, JS::Value) override;

    virtual WebIDL::ExceptionOr<String> to_string() const override;

private:
    explicit CSSUnparsedValue(JS::Realm&, Vector<CSSUnparsedSegment>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-tokens-slot
    // They have a [[tokens]] internal slot, which is a list of USVStrings and CSSVariableReferenceValue objects.
    // This list is the object’s values to iterate over.
    Vector<CSSUnparsedSegment> m_tokens;
};

}
