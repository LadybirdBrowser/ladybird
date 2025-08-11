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

    WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> get(String property);
    WebIDL::ExceptionOr<Vector<GC::Ref<CSSStyleValue>>> get_all(String property);
    WebIDL::ExceptionOr<bool> has(String property);
    WebIDL::UnsignedLong size() const;

protected:
    explicit StylePropertyMapReadOnly(JS::Realm&, GC::Ref<CSSStyleDeclaration>);
    explicit StylePropertyMapReadOnly(JS::Realm&, Optional<DOM::AbstractElement> = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<DOM::AbstractElement> m_source_element;
    GC::Ptr<CSSStyleDeclaration> m_source_declaration;

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
    // A StylePropertyMapReadOnly object has a [[declarations]] internal slot, which is a map reflecting the CSS
    // declaration block’s declarations.
    HashMap<FlyString, NonnullRefPtr<StyleValue>> m_declarations;
};

}
