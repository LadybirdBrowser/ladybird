/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/AvailableSpace.h>

namespace Web::Layout {

struct LayoutInput {
    explicit LayoutInput(AvailableSpace new_available_space)
        : available_space(move(new_available_space))
    {
    }

    AvailableSpace const available_space;
};

}
