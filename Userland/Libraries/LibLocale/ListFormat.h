/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibLocale/Locale.h>

namespace Locale {

enum class ListFormatType {
    Conjunction,
    Disjunction,
    Unit,
};

ListFormatType list_format_type_from_string(StringView list_format_type);
StringView list_format_type_to_string(ListFormatType list_format_type);

struct ListFormatPart {
    StringView type;
    String value;
};

String format_list(StringView locale, ListFormatType, Style, ReadonlySpan<String> list);
Vector<ListFormatPart> format_list_to_parts(StringView locale, ListFormatType, Style, ReadonlySpan<String> list);

}
