/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class FontSourceStyleValue final : public StyleValueWithDefaultOperators<FontSourceStyleValue> {
public:
    struct Local {
        NonnullRefPtr<CSSStyleValue> name;
    };
    using Source = Variant<Local, URL::URL>;

    static ValueComparingNonnullRefPtr<FontSourceStyleValue> create(Source source, Optional<FlyString> format)
    {
        return adopt_ref(*new (nothrow) FontSourceStyleValue(move(source), move(format)));
    }
    virtual ~FontSourceStyleValue() override;

    Source const& source() const { return m_source; }
    Optional<FlyString> const& format() const { return m_format; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(FontSourceStyleValue const&) const;

private:
    FontSourceStyleValue(Source source, Optional<FlyString> format);

    Source m_source;
    Optional<FlyString> m_format;
};

}
