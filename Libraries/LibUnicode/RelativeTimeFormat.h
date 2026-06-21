/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
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
Optional<TimeUnit> time_unit_from_string(Utf16View);
Utf16String time_unit_to_string(TimeUnit);

enum class NumericDisplay {
    Always,
    Auto,
};
NumericDisplay numeric_display_from_string(StringView);
NumericDisplay numeric_display_from_string(Utf16View);
Utf16String numeric_display_to_string(NumericDisplay);

class RelativeTimeFormat {
public:
    static NonnullOwnPtr<RelativeTimeFormat> create(Utf16View locale, Style style);
    virtual ~RelativeTimeFormat() = default;

    struct Partition {
        Utf16String type;
        Utf16String value;
        Utf16String unit;
    };

    virtual Utf16String format(double, TimeUnit, NumericDisplay) const = 0;
    virtual Vector<Partition> format_to_parts(double, TimeUnit, NumericDisplay) const = 0;

protected:
    RelativeTimeFormat() = default;
};

}
