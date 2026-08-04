/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-csskeywordish
using CSSKeywordish = Variant<Utf16String, GC::Ref<CSSKeywordValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue
class CSSKeywordValue final : public CSSStyleValue {
    WEB_WRAPPABLE(CSSKeywordValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSKeywordValue);

public:
    [[nodiscard]] static GC::Ref<CSSKeywordValue> create(Utf16FlyString value);
    static WebIDL::ExceptionOr<GC::Ref<CSSKeywordValue>> construct_impl(Utf16String value);

    virtual ~CSSKeywordValue() override = default;

    Utf16FlyString const& value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(Utf16String value);

    void serialize(Utf16StringBuilder&) const;
    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;
    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const override;

private:
    explicit CSSKeywordValue(Utf16FlyString value);

    Utf16FlyString m_value;
};

GC::Ref<CSSKeywordValue> rectify_a_keywordish_value(CSSKeywordish const&);

}
