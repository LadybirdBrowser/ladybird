/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibWeb/CSS/Length.h>

namespace Web::CSS {

struct EdgeRect {
    Length top_edge;
    Length right_edge;
    Length bottom_edge;
    Length left_edge;
    CSSPixelRect resolved(Layout::Node const&, CSSPixelRect) const;
    bool operator==(EdgeRect const&) const = default;
};

}
