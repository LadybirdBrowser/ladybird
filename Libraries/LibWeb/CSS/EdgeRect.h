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
#include <LibWeb/Export.h>

namespace Web::CSS {

struct WEB_API EdgeRect {
    LengthOrAuto top_edge;
    LengthOrAuto right_edge;
    LengthOrAuto bottom_edge;
    LengthOrAuto left_edge;
    CSSPixelRect resolved(Layout::Node const&, CSSPixelRect) const;
    bool operator==(EdgeRect const&) const = default;
};

}
