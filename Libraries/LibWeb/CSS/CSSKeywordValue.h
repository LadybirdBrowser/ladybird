/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-csskeywordish
using CSSKeywordish = Variant<String, GC::Ref<CSSKeywordValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue
class CSSKeywordValue final : public CSSStyleValue {
    WEB_WRAPPABLE(CSSKeywordValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSKeywordValue);

public:
    [[nodiscard]] static GC::Ref<CSSKeywordValue> create(FlyString value);
    static WebIDL::ExceptionOr<GC::Ref<CSSKeywordValue>> construct_impl(FlyString value);

    virtual ~CSSKeywordValue() override = default;

    FlyString const& value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(FlyString value);

    void serialize(StringBuilder&) const;
    virtual WebIDL::ExceptionOr<String> to_string() const override;
    virtual WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const override;

private:
    explicit CSSKeywordValue(FlyString value);

    FlyString m_value;
};

GC::Ref<CSSKeywordValue> rectify_a_keywordish_value(CSSKeywordish const&);

}
