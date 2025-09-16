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
using CSSKeywordish = Variant<String, GC::Root<CSSKeywordValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue
class CSSKeywordValue final : public CSSStyleValue {
    WEB_PLATFORM_OBJECT(CSSKeywordValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSKeywordValue);

public:
    [[nodiscard]] static GC::Ref<CSSKeywordValue> create(JS::Realm&, FlyString value);
    static WebIDL::ExceptionOr<GC::Ref<CSSKeywordValue>> construct_impl(JS::Realm&, FlyString value);

    virtual ~CSSKeywordValue() override = default;

    FlyString const& value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(FlyString value);

    virtual WebIDL::ExceptionOr<String> to_string() const override;

private:
    explicit CSSKeywordValue(JS::Realm&, FlyString value);

    virtual void initialize(JS::Realm&) override;

    FlyString m_value;
};

GC::Ref<CSSKeywordValue> rectify_a_keywordish_value(JS::Realm&, CSSKeywordish const&);

}
