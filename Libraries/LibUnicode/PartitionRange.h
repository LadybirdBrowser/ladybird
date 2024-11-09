/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Unicode {

struct PartitionRange {
    // ICU does not contain a field enumeration for "literal" partitions. Define a custom field so that we may provide
    // a type for those partitions.
    static constexpr i32 LITERAL_FIELD = -1;

    constexpr bool contains(i32 position) const
    {
        return start <= position && position < end;
    }

    constexpr bool operator<(PartitionRange const& other) const
    {
        if (start < other.start)
            return true;
        if (start == other.start)
            return end > other.end;
        return false;
    }

    i32 field { LITERAL_FIELD };
    i32 start { 0 };
    i32 end { 0 };
};

}
