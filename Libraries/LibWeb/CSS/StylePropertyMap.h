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

    WebIDL::ExceptionOr<void> set(String property, Vector<Variant<GC::Root<CSSStyleValue>, String>> values);
    WebIDL::ExceptionOr<void> append(String property, Vector<Variant<GC::Root<CSSStyleValue>, String>> values);
    WebIDL::ExceptionOr<void> delete_(String property);
    void clear();

private:
    explicit StylePropertyMap(JS::Realm&, GC::Ref<CSSStyleDeclaration>);

    virtual void initialize(JS::Realm&) override;
};

}
