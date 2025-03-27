/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class UnicodeRangeStyleValue final : public StyleValueWithDefaultOperators<UnicodeRangeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<UnicodeRangeStyleValue> create(Gfx::UnicodeRange unicode_range)
    {
        return adopt_ref(*new (nothrow) UnicodeRangeStyleValue(unicode_range));
    }
    virtual ~UnicodeRangeStyleValue() override;

    Gfx::UnicodeRange const& unicode_range() const { return m_unicode_range; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(UnicodeRangeStyleValue const&) const;

private:
    UnicodeRangeStyleValue(Gfx::UnicodeRange);

    Gfx::UnicodeRange m_unicode_range;
};

}
