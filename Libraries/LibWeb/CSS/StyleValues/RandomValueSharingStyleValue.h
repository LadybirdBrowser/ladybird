/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RandomValueSharingStyleValue : public StyleValueWithDefaultOperators<RandomValueSharingStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_fixed(NonnullRefPtr<StyleValue const> const& fixed_value)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue(fixed_value, false, {}, false));
    }

    virtual ~RandomValueSharingStyleValue() override = default;

    double random_base_value() const;

private:
    friend class StyleValue;

    explicit RandomValueSharingStyleValue(RefPtr<StyleValue const> fixed_value, bool is_auto, Optional<Utf16FlyString> name, bool element_shared)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing, make_random_value_sharing_data(fixed_value, is_auto, name, element_shared))
    {
    }

    explicit RandomValueSharingStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing, data)
    {
    }

    static StyleValueFFI::StyleValueData const* make_random_value_sharing_data(RefPtr<StyleValue const> const& fixed_value, bool is_auto, Optional<Utf16FlyString> const& name, bool element_shared)
    {
        // The Rust allocation takes ownership of one strong reference to the fixed value data.
        auto const* fixed_value_data = fixed_value ? StyleValueFFI::rust_style_value_retain(fixed_value->rust_style_value_data()) : nullptr;
        return StyleValueFFI::rust_style_value_create_random_value_sharing(fixed_value_data, is_auto, name.has_value(), name.has_value() ? name->to_raw_leaked() : 0, element_shared);
    }

    ValueComparingRefPtr<StyleValue const> fixed_value() const { return wrap_rust_child_or_null(m_value->random_value_sharing.fixed_value); }
    bool is_auto() const { return m_value->random_value_sharing.is_auto; }
    Optional<Utf16FlyString> name() const
    {
        if (!m_value->random_value_sharing.has_name)
            return {};
        return css_string_from_rust(&m_value->random_value_sharing.name);
    }
    bool element_shared() const { return m_value->random_value_sharing.element_shared; }
};

}
