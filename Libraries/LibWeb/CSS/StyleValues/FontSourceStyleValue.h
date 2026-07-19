/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

class FontSourceStyleValue final : public StyleValueWithDefaultOperators<FontSourceStyleValue> {
public:
    struct Local {
        NonnullRefPtr<StyleValue const> name;
    };
    using Source = Variant<Local, URL>;

    static ValueComparingNonnullRefPtr<FontSourceStyleValue const> create(Source source, Optional<Utf16FlyString> format, Vector<FontTech> tech)
    {
        return adopt_ref(*new (nothrow) FontSourceStyleValue(move(source), move(format), move(tech)));
    }
    virtual ~FontSourceStyleValue() override;

    Source source() const;
    Optional<Utf16FlyString> format() const
    {
        if (!m_value->font_source.has_format)
            return {};
        return Utf16FlyString::from_raw(m_value->font_source.format.raw);
    }
    Vector<FontTech> tech() const
    {
        auto const& list = m_value->font_source.tech;
        Vector<FontTech> tech;
        tech.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            tech.unchecked_append(static_cast<FontTech>(list.pointer[i]));
        return tech;
    }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(FontSourceStyleValue const&) const;

private:
    FontSourceStyleValue(Source source, Optional<Utf16FlyString> format, Vector<FontTech> tech);

    static StyleValueFFI::StyleValueData* make_font_source_data(Source const&, Optional<Utf16FlyString> const&, Vector<FontTech> const&);
};

}
