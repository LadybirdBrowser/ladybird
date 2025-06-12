/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

struct ViewBox {
    double min_x { 0 };
    double min_y { 0 };
    double width { 0 };
    double height { 0 };
};

Optional<ViewBox> try_parse_view_box(StringView);

}
