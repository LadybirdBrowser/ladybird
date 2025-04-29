/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

class URLStyleValue final : public StyleValueWithDefaultOperators<URLStyleValue> {
public:
    static ValueComparingNonnullRefPtr<URLStyleValue const> create(URL const& url)
    {
        return adopt_ref(*new (nothrow) URLStyleValue(url));
    }

    virtual ~URLStyleValue() override = default;

    URL const& url() const { return m_url; }

    bool properties_equal(URLStyleValue const& other) const { return m_url == other.m_url; }

    virtual String to_string(SerializationMode) const override
    {
        return m_url.to_string();
    }

private:
    URLStyleValue(URL const& url)
        : StyleValueWithDefaultOperators(Type::URL)
        , m_url(url)
    {
    }

    URL m_url;
};

}
