/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StringView.h>

namespace JS::Intl {

// Table 2: Single units sanctioned for use in ECMAScript, https://tc39.es/ecma402/#table-sanctioned-single-unit-identifiers
constexpr auto sanctioned_single_unit_identifiers()
{
    return AK::Array {
        "acre"_sv,
        "bit"_sv,
        "byte"_sv,
        "celsius"_sv,
        "centimeter"_sv,
        "day"_sv,
        "degree"_sv,
        "fahrenheit"_sv,
        "fluid-ounce"_sv,
        "foot"_sv,
        "gallon"_sv,
        "gigabit"_sv,
        "gigabyte"_sv,
        "gram"_sv,
        "hectare"_sv,
        "hour"_sv,
        "inch"_sv,
        "kilobit"_sv,
        "kilobyte"_sv,
        "kilogram"_sv,
        "kilometer"_sv,
        "liter"_sv,
        "megabit"_sv,
        "megabyte"_sv,
        "meter"_sv,
        "microsecond"_sv,
        "mile"_sv,
        "mile-scandinavian"_sv,
        "milliliter"_sv,
        "millimeter"_sv,
        "millisecond"_sv,
        "minute"_sv,
        "month"_sv,
        "nanosecond"_sv,
        "ounce"_sv,
        "percent"_sv,
        "petabyte"_sv,
        "pound"_sv,
        "second"_sv,
        "stone"_sv,
        "terabit"_sv,
        "terabyte"_sv,
        "week"_sv,
        "yard"_sv,
        "year"_sv,
    };
}

}
