/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>

namespace Unicode {

struct DigitalFormat {
    Utf16String hours_minutes_separator { ":"_utf16 };
    Utf16String minutes_seconds_separator { ":"_utf16 };
    bool uses_two_digit_hours { false };
};

DigitalFormat digital_format(StringView locale);

}
