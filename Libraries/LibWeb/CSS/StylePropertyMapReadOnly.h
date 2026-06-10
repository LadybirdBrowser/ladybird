/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
class StylePropertyMapReadOnly : public Bindings::Wrappable {
    WEB_WRAPPABLE(StylePropertyMapReadOnly, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(StylePropertyMapReadOnly);

public:
    [[nodiscard]] static GC::Ref<StylePropertyMapReadOnly> create_computed_style(DOM::AbstractElement);

    virtual ~StylePropertyMapReadOnly() override;

    WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> get(String property);
    WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> get_all(String property);
    WebIDL::ExceptionOr<bool> has(String property);
    WebIDL::UnsignedLong size() const;

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
};

}
