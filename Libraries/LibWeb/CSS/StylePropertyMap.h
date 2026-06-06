/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StylePropertyMapReadOnly.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymap
class StylePropertyMap : public StylePropertyMapReadOnly {
    WEB_WRAPPABLE(StylePropertyMap, StylePropertyMapReadOnly);
    GC_DECLARE_ALLOCATOR(StylePropertyMap);

public:
    [[nodiscard]] static GC::Ref<StylePropertyMap> create(GC::Ref<CSSStyleDeclaration>);

    virtual ~StylePropertyMap() override;

    WebIDL::ExceptionOr<void> set(JS::Realm&, Utf16FlyString property, ReadonlySpan<Variant<GC::Ref<CSSStyleValue>, String>> values);
    WebIDL::ExceptionOr<void> append(JS::Realm&, Utf16FlyString property, ReadonlySpan<Variant<GC::Ref<CSSStyleValue>, String>> values);
    WebIDL::ExceptionOr<void> delete_(JS::Realm&, Utf16FlyString property);
    WebIDL::ExceptionOr<void> clear(JS::Realm&);

private:
    explicit StylePropertyMap(GC::Ref<CSSStyleDeclaration>);

    CSSStyleDeclaration& declarations();
};

}
