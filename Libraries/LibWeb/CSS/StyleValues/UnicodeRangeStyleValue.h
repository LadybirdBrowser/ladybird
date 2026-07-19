/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class UnicodeRangeStyleValue final : public StyleValueWithDefaultOperators<UnicodeRangeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<UnicodeRangeStyleValue const> create(Gfx::UnicodeRange unicode_range)
    {
        return adopt_ref(*new (nothrow) UnicodeRangeStyleValue(unicode_range));
    }
    virtual ~UnicodeRangeStyleValue() override;

    Gfx::UnicodeRange unicode_range() const { return Gfx::UnicodeRange(m_value->unicode_range.min_code_point, m_value->unicode_range.max_code_point); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(UnicodeRangeStyleValue const&) const;

private:
    UnicodeRangeStyleValue(Gfx::UnicodeRange);
};

}
