/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

enum class FocusTrigger : u8 {
    Click,
    Key,
    Script,
    Other,
};

}
