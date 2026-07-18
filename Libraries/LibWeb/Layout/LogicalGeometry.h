/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

enum class LogicalAxis {
    Inline,
    Block,
};

struct LogicalSize {
    CSSPixels inline_size {};
    CSSPixels block_size {};

    bool operator==(LogicalSize const&) const = default;
};

struct LogicalOffset {
    CSSPixels inline_offset {};
    CSSPixels block_offset {};

    bool operator==(LogicalOffset const&) const = default;
};

struct LogicalRect {
    LogicalOffset offset {};
    LogicalSize size {};

    bool operator==(LogicalRect const&) const = default;
};

}
