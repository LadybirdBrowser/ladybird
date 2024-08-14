/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

class URLStyleValue final : public StyleValueWithDefaultOperators<URLStyleValue> {
public:
    static ValueComparingNonnullRefPtr<URLStyleValue> create(URL::URL const& url)
    {
        return adopt_ref(*new (nothrow) URLStyleValue(url));
    }

    virtual ~URLStyleValue() override = default;

    URL::URL const& url() const { return m_url; }

    bool properties_equal(URLStyleValue const& other) const { return m_url == other.m_url; }

    virtual String to_string() const override
    {
        return serialize_a_url(MUST(m_url.to_string()));
    }

private:
    URLStyleValue(URL::URL const& url)
        : StyleValueWithDefaultOperators(Type::URL)
        , m_url(url)
    {
    }

    URL::URL m_url;
};

}
