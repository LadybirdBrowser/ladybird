/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>

namespace Unicode {

Vector<Utf16String> available_keyword_values(Utf16View locale, Utf16View key);

Vector<Utf16String> const& available_calendars();
Vector<Utf16String> available_calendars(Utf16View locale);

Vector<Utf16String> const& available_currencies();

Vector<Utf16String> const& available_collation_case_orderings();
Vector<Utf16String> const& available_collation_numeric_orderings();

Vector<Utf16String> const& available_collations();
Vector<Utf16String> available_collations(Utf16View locale);

Vector<Utf16String> const& available_hour_cycles();
Vector<Utf16String> available_hour_cycles(Utf16View locale);

Vector<Utf16String> const& available_number_systems();
Vector<Utf16String> available_number_systems(Utf16View locale);

}
