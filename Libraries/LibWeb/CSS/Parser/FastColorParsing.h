/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <LibGfx/Color.h>

namespace Web::CSS::Parser {

Optional<Gfx::Color> parse_simple_color(Utf16View);

}
