/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Utf16View.h>

namespace JS::Intl {

// Table 2: Single units sanctioned for use in ECMAScript, https://tc39.es/ecma402/#table-sanctioned-single-unit-identifiers
constexpr auto sanctioned_single_unit_identifiers()
{
    return AK::Array {
        u"acre"sv,
        u"bit"sv,
        u"byte"sv,
        u"celsius"sv,
        u"centimeter"sv,
        u"day"sv,
        u"degree"sv,
        u"fahrenheit"sv,
        u"fluid-ounce"sv,
        u"foot"sv,
        u"gallon"sv,
        u"gigabit"sv,
        u"gigabyte"sv,
        u"gram"sv,
        u"hectare"sv,
        u"hour"sv,
        u"inch"sv,
        u"kilobit"sv,
        u"kilobyte"sv,
        u"kilogram"sv,
        u"kilometer"sv,
        u"liter"sv,
        u"megabit"sv,
        u"megabyte"sv,
        u"meter"sv,
        u"microsecond"sv,
        u"mile"sv,
        u"mile-scandinavian"sv,
        u"milliliter"sv,
        u"millimeter"sv,
        u"millisecond"sv,
        u"minute"sv,
        u"month"sv,
        u"nanosecond"sv,
        u"ounce"sv,
        u"percent"sv,
        u"petabyte"sv,
        u"pound"sv,
        u"second"sv,
        u"stone"sv,
        u"terabit"sv,
        u"terabyte"sv,
        u"week"sv,
        u"yard"sv,
        u"year"sv,
    };
}

}
