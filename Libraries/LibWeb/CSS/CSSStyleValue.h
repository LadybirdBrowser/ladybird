/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
class CSSStyleValue : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSStyleValue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSStyleValue);

public:
    [[nodiscard]] static GC::Ref<CSSStyleValue> create(JS::Realm&, String associated_property, String constructed_from_string);

    virtual ~CSSStyleValue() override = default;

    virtual void initialize(JS::Realm&) override;

    static WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> parse(JS::VM&, String property, String css_text);
    static WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> parse_all(JS::VM&, String property, String css_text);

    virtual WebIDL::ExceptionOr<String> to_string() const;

protected:
    explicit CSSStyleValue(JS::Realm&);

private:
    explicit CSSStyleValue(JS::Realm&, String associated_property, String constructed_from_string);

    enum class ParseMultiple : u8 {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, GC::RootVector<GC::Ref<CSSStyleValue>>>> parse_a_css_style_value(JS::VM&, String property, String css_text, ParseMultiple);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-associatedproperty-slot
    Optional<String> m_associated_property;

    Optional<String> m_constructed_from_string;
};

}
