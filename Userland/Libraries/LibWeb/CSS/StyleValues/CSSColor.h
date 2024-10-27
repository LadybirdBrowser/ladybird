/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csscolor
class CSSColor final : public CSSColorValue {
public:
    virtual ~CSSColor() override = default;

    static ValueComparingNonnullRefPtr<CSSColor> create(StringView color_space, ValueComparingNonnullRefPtr<CSSStyleValue> c1, ValueComparingNonnullRefPtr<CSSStyleValue> c2, ValueComparingNonnullRefPtr<CSSStyleValue> c3, ValueComparingRefPtr<CSSStyleValue> alpha = {});

    virtual bool equals(CSSStyleValue const&) const override;
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;
    virtual String to_string() const override;

    static constexpr Array s_supported_color_space = { "xyz-d50"sv };

private:
    CSSColor(ColorType color_type, ValueComparingNonnullRefPtr<CSSStyleValue> c1, ValueComparingNonnullRefPtr<CSSStyleValue> c2, ValueComparingNonnullRefPtr<CSSStyleValue> c3, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(color_type)
        , m_properties { .channels = { move(c1), move(c2), move(c3) }, .alpha = move(alpha) }
    {
    }

    struct Properties {
        Array<ValueComparingNonnullRefPtr<CSSStyleValue>, 3> channels;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

} // Web::CSS
