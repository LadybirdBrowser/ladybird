/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
class CSSStyleValue : public Bindings::Wrappable {
    WEB_WRAPPABLE(CSSStyleValue, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CSSStyleValue);

public:
    [[nodiscard]] static GC::Ref<CSSStyleValue> create(Utf16FlyString associated_property, NonnullRefPtr<StyleValue const>);

    virtual ~CSSStyleValue() override;

    Optional<Utf16FlyString> const& associated_property() const { return m_associated_property; }
    RefPtr<StyleValue const> const& source_value() const { return m_source_value; }

    static WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> parse(JS::Realm&, Utf16FlyString const& property, String css_text);
    static WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> parse_all(JS::Realm&, Utf16FlyString const& property, String css_text);

    enum class ParseMultiple : u8 {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, GC::RootVector<GC::Ref<CSSStyleValue>>>> parse_a_css_style_value(JS::Realm&, Utf16FlyString property, String css_text, ParseMultiple);

    virtual WebIDL::ExceptionOr<String> to_string() const;

    // FIXME: Temporary hack. Really we want to pass something like a CalculationContext with the valid types and ranges.
    enum class PerformTypeCheck : u8 {
        No,
        Yes,
    };
    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const;

protected:
    explicit CSSStyleValue();
    explicit CSSStyleValue(NonnullRefPtr<StyleValue const> source_value);

private:
    explicit CSSStyleValue(Utf16FlyString associated_property, NonnullRefPtr<StyleValue const> source_value);

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-associatedproperty-slot
    Optional<Utf16FlyString> m_associated_property;

    RefPtr<StyleValue const> m_source_value;
};

}
