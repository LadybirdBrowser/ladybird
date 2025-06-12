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
        "acre"sv,
        "bit"sv,
        "byte"sv,
        "celsius"sv,
        "centimeter"sv,
        "day"sv,
        "degree"sv,
        "fahrenheit"sv,
        "fluid-ounce"sv,
        "foot"sv,
        "gallon"sv,
        "gigabit"sv,
        "gigabyte"sv,
        "gram"sv,
        "hectare"sv,
        "hour"sv,
        "inch"sv,
        "kilobit"sv,
        "kilobyte"sv,
        "kilogram"sv,
        "kilometer"sv,
        "liter"sv,
        "megabit"sv,
        "megabyte"sv,
        "meter"sv,
        "microsecond"sv,
        "mile"sv,
        "mile-scandinavian"sv,
        "milliliter"sv,
        "millimeter"sv,
        "millisecond"sv,
        "minute"sv,
        "month"sv,
        "nanosecond"sv,
        "ounce"sv,
        "percent"sv,
        "petabyte"sv,
        "pound"sv,
        "second"sv,
        "stone"sv,
        "terabit"sv,
        "terabyte"sv,
        "week"sv,
        "yard"sv,
        "year"sv,
    };
}

}
