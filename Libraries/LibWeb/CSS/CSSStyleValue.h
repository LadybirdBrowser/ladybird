/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
class CSSStyleValue : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSStyleValue, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSStyleValue);

public:
    [[nodiscard]] static GC::Ref<CSSStyleValue> create(JS::Realm&, FlyString associated_property, NonnullRefPtr<StyleValue const>);

    virtual ~CSSStyleValue() override;

    virtual void initialize(JS::Realm&) override;

    Optional<FlyString> const& associated_property() const { return m_associated_property; }
    RefPtr<StyleValue const> const& source_value() const { return m_source_value; }

    static WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> parse(JS::VM&, FlyString const& property, String css_text);
    static WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> parse_all(JS::VM&, FlyString const& property, String css_text);

    virtual WebIDL::ExceptionOr<String> to_string() const;

protected:
    explicit CSSStyleValue(JS::Realm&);
    explicit CSSStyleValue(JS::Realm&, NonnullRefPtr<StyleValue const> source_value);

private:
    explicit CSSStyleValue(JS::Realm&, FlyString associated_property, NonnullRefPtr<StyleValue const> source_value);

    enum class ParseMultiple : u8 {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, GC::RootVector<GC::Ref<CSSStyleValue>>>> parse_a_css_style_value(JS::VM&, FlyString property, String css_text, ParseMultiple);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-associatedproperty-slot
    Optional<FlyString> m_associated_property;

    RefPtr<StyleValue const> m_source_value;
};

}
