/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/IterationDecision.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
class StylePropertyMapReadOnly : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(StylePropertyMapReadOnly, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(StylePropertyMapReadOnly);

public:
    [[nodiscard]] static GC::Ref<StylePropertyMapReadOnly> create_computed_style(DOM::AbstractElement);

    virtual ~StylePropertyMapReadOnly() override;

    WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> get(Utf16String property);
    WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> get_all(Utf16String property);
    WebIDL::ExceptionOr<bool> has(Utf16String property);
    WebIDL::UnsignedLong size() const;

    ReadonlySpan<Utf16FlyString> property_names() const;
    struct IterationEntry {
        Utf16FlyString name;
        size_t value_offset;
        size_t value_count;
    };
    void collect_iteration_entries(Vector<IterationEntry>&, GC::RootVector<GC::Ref<CSSStyleValue>>&);
    using ForEachCallback = Function<IterationDecision(Utf16String const&, ReadonlySpan<GC::Ref<CSSStyleValue>>)>;
    void for_each(ForEachCallback);

protected:
    using Source = Variant<DOM::AbstractElement, GC::Ref<CSSStyleDeclaration>>;
    explicit StylePropertyMapReadOnly(Source);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    static RefPtr<StyleValue const> get_style_value(Source&, PropertyNameAndID const& property);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
    // A StylePropertyMapReadOnly object has a [[declarations]] internal slot, which is a map reflecting the CSS
    // declaration block’s declarations.
    // NB: We just directly refer to our source, at least for now.
    Source m_declarations;

private:
    mutable Vector<Utf16FlyString> m_cached_property_names;
    mutable Optional<u64> m_cached_declaration_revision;
    mutable GC::Ptr<DOM::Document> m_cached_document;
    mutable RefPtr<CustomPropertyData const> m_cached_custom_property_data;
    mutable size_t m_cached_registration_generation { 0 };
};

}
