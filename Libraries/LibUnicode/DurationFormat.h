/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Unicode {

struct DigitalFormat {
    String hours_minutes_separator { ":"_string };
    String minutes_seconds_separator { ":"_string };
    bool uses_two_digit_hours { false };
};

DigitalFormat digital_format(StringView locale);

}
