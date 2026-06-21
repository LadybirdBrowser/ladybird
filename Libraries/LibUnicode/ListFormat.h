/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibUnicode/Locale.h>

namespace Unicode {

enum class ListFormatType {
    Conjunction,
    Disjunction,
    Unit,
};
ListFormatType list_format_type_from_string(StringView);
ListFormatType list_format_type_from_string(Utf16View);
Utf16String list_format_type_to_string(ListFormatType);

class ListFormat {
public:
    static NonnullOwnPtr<ListFormat> create(Utf16View locale, ListFormatType, Style);
    virtual ~ListFormat() = default;

    struct Partition {
        Utf16String type;
        Utf16String value;
    };

    virtual Utf16String format(ReadonlySpan<Utf16String> list) const = 0;
    virtual Vector<Partition> format_to_parts(ReadonlySpan<Utf16String> list) const = 0;

protected:
    ListFormat() = default;
};

}
