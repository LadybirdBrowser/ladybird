/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

struct PaintConfig {
    bool paint_overlay { false };
    bool should_show_line_box_borders { false };
    Optional<Gfx::IntRect> canvas_fill_rect {};

    bool operator==(PaintConfig const& other) const = default;
};

}
