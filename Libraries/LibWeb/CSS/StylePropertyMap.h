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
    WEB_PLATFORM_OBJECT(StylePropertyMap, StylePropertyMapReadOnly);
    GC_DECLARE_ALLOCATOR(StylePropertyMap);

public:
    [[nodiscard]] static GC::Ref<StylePropertyMap> create(JS::Realm&, GC::Ref<CSSStyleDeclaration>);

    virtual ~StylePropertyMap() override;

    WebIDL::ExceptionOr<void> set(FlyString const& property, Vector<Variant<GC::Root<CSSStyleValue>, String>> const& values);
    WebIDL::ExceptionOr<void> append(FlyString property, Vector<Variant<GC::Root<CSSStyleValue>, String>> const& values);
    WebIDL::ExceptionOr<void> delete_(FlyString property);
    WebIDL::ExceptionOr<void> clear();

private:
    explicit StylePropertyMap(JS::Realm&, GC::Ref<CSSStyleDeclaration>);

    CSSStyleDeclaration& declarations();

    virtual void initialize(JS::Realm&) override;
};

}
