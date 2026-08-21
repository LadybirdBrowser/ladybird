/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Painting {

enum class ScrollDirection {
    Horizontal,
    Vertical,
};

enum class ScrollHandled {
    No,
    Yes,
};

enum class ScrollBlockDirection {
    No,
    Yes,
};

}
