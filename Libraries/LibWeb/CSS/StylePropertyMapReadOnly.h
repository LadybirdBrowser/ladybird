/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
class StylePropertyMapReadOnly : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(StylePropertyMapReadOnly, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(StylePropertyMapReadOnly);

public:
    [[nodiscard]] static GC::Ref<StylePropertyMapReadOnly> create_computed_style(JS::Realm&, DOM::AbstractElement);

    virtual ~StylePropertyMapReadOnly() override;

    WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> get(Utf16FlyString property);
    WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> get_all(Utf16FlyString property);
    WebIDL::ExceptionOr<bool> has(Utf16FlyString property);
    WebIDL::UnsignedLong size() const;

protected:
    using Source = Variant<DOM::AbstractElement, GC::Ref<CSSStyleDeclaration>>;
    explicit StylePropertyMapReadOnly(JS::Realm&, Source);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    static RefPtr<StyleValue const> get_style_value(Source&, PropertyNameAndID const& property);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
    // A StylePropertyMapReadOnly object has a [[declarations]] internal slot, which is a map reflecting the CSS
    // declaration block’s declarations.
    // NB: We just directly refer to our source, at least for now.
    Source m_declarations;
};

}
