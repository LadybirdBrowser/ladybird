/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

class FontSourceStyleValue final : public StyleValueWithDefaultOperators<FontSourceStyleValue> {
public:
    struct Local {
        NonnullRefPtr<CSSStyleValue const> name;
    };
    using Source = Variant<Local, URL>;

    static ValueComparingNonnullRefPtr<FontSourceStyleValue const> create(Source source, Optional<FlyString> format, Vector<FontTech> tech)
    {
        return adopt_ref(*new (nothrow) FontSourceStyleValue(move(source), move(format), move(tech)));
    }
    virtual ~FontSourceStyleValue() override;

    Source const& source() const { return m_source; }
    Optional<FlyString> const& format() const { return m_format; }
    Vector<FontTech> const& tech() const { return m_tech; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(FontSourceStyleValue const&) const;

private:
    FontSourceStyleValue(Source source, Optional<FlyString> format, Vector<FontTech> tech);

    Source m_source;
    Optional<FlyString> m_format;
    Vector<FontTech> m_tech;
};

}
