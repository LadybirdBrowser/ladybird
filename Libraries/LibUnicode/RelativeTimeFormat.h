/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

// These are just the subset of fields in the CLDR required for ECMA-402.
enum class TimeUnit {
    Second,
    Minute,
    Hour,
    Day,
    Week,
    Month,
    Quarter,
    Year,
};
Optional<TimeUnit> time_unit_from_string(StringView);
StringView time_unit_to_string(TimeUnit);

enum class NumericDisplay {
    Always,
    Auto,
};
NumericDisplay numeric_display_from_string(StringView);
StringView numeric_display_to_string(NumericDisplay);

class RelativeTimeFormat {
public:
    static NonnullOwnPtr<RelativeTimeFormat> create(StringView locale, Style style);
    virtual ~RelativeTimeFormat() = default;

    struct Partition {
        StringView type;
        String value;
        StringView unit;
    };

    virtual String format(double, TimeUnit, NumericDisplay) const = 0;
    virtual Vector<Partition> format_to_parts(double, TimeUnit, NumericDisplay) const = 0;

protected:
    RelativeTimeFormat() = default;
};

}
