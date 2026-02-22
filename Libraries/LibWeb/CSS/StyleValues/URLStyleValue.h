/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

class URLStyleValue final : public StyleValueWithDefaultOperators<URLStyleValue> {
public:
    static ValueComparingNonnullRefPtr<URLStyleValue const> create(URL const& url, ValueComparingRefPtr<StyleValue const> paint_fallback = {})
    {
        return adopt_ref(*new (nothrow) URLStyleValue(url, move(paint_fallback)));
    }

    virtual ~URLStyleValue() override = default;

    URL const& url() const { return m_url; }

    ValueComparingRefPtr<StyleValue const> const& paint_fallback() const { return m_paint_fallback; }

    bool properties_equal(URLStyleValue const& other) const { return m_url == other.m_url && m_paint_fallback == other.m_paint_fallback; }

    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override
    {
        builder.append(m_url.to_string());
        if (m_paint_fallback) {
            builder.append(' ');
            m_paint_fallback->serialize(builder, mode);
        }
    }

private:
    URLStyleValue(URL const& url, ValueComparingRefPtr<StyleValue const> paint_fallback = {})
        : StyleValueWithDefaultOperators(Type::URL)
        , m_url(url)
        , m_paint_fallback(move(paint_fallback))
    {
    }

    URL m_url;
    ValueComparingRefPtr<StyleValue const> m_paint_fallback;
};

}
