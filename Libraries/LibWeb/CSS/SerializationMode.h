/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::CSS {

enum class SerializationMode : u8 {
    Normal,
    ResolvedValue,
};

}
