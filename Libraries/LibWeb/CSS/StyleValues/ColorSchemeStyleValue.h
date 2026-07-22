/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ColorSchemeStyleValue final : public StyleValueWithDefaultOperators<ColorSchemeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ColorSchemeStyleValue const> create(Vector<Utf16FlyString> schemes, bool only)
    {
        return adopt_ref(*new (nothrow) ColorSchemeStyleValue(move(schemes), only));
    }
    static ValueComparingNonnullRefPtr<ColorSchemeStyleValue const> normal()
    {
        return adopt_ref(*new (nothrow) ColorSchemeStyleValue({}, false));
    }
    virtual ~ColorSchemeStyleValue() override = default;

    Vector<Utf16FlyString> schemes() const
    {
        auto const& list = m_value->color_scheme.schemes;
        Vector<Utf16FlyString> schemes;
        schemes.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            schemes.unchecked_append(Utf16FlyString::from_raw(list.pointer[i].raw));
        return schemes;
    }
    bool only() const { return m_value->color_scheme.only; }
    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ColorSchemeStyleValue const& other) const { return schemes() == other.schemes() && only() == other.only(); }

private:
    ColorSchemeStyleValue(Vector<Utf16FlyString> schemes, bool only)
        : StyleValueWithDefaultOperators(Type::ColorScheme, make_color_scheme_data(schemes, only))
    {
    }

    static StyleValueFFI::StyleValueData* make_color_scheme_data(Vector<Utf16FlyString> const& schemes, bool only)
    {
        // The Rust allocation takes ownership of one leaked reference to each scheme name.
        Vector<size_t> raws;
        Vector<u8> scheme_codes;
        raws.ensure_capacity(schemes.size());
        scheme_codes.ensure_capacity(schemes.size());
        for (auto const& scheme : schemes) {
            raws.unchecked_append(scheme.to_raw_leaked());
            scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
        }
        return StyleValueFFI::rust_style_value_create_color_scheme(raws.data(), scheme_codes.data(), raws.size(), only);
    }
};

}
