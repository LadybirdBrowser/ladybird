/*
 * Copyright (c) 2026, Sam Atkins <sam@samatkins.co.uk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::CSS {

enum class QueryValueType : u8 {
    Boolean,
    Integer,
    Length,
    Ratio,
    Resolution,
};

}
