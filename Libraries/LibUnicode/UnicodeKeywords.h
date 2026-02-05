/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Unicode {

Vector<String> available_keyword_values(StringView locale, StringView key);

Vector<String> const& available_calendars();
Vector<String> available_calendars(StringView locale);

Vector<String> const& available_currencies();

Vector<String> const& available_collation_case_orderings();
Vector<String> const& available_collation_numeric_orderings();

Vector<String> const& available_collations();
Vector<String> available_collations(StringView locale);

Vector<String> const& available_hour_cycles();
Vector<String> available_hour_cycles(StringView locale);

Vector<String> const& available_number_systems();
Vector<String> available_number_systems(StringView locale);

}
