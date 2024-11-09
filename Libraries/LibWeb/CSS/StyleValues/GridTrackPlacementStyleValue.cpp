/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackPlacementStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridTrackPlacementStyleValue> GridTrackPlacementStyleValue::create(CSS::GridTrackPlacement grid_track_placement)
{
    return adopt_ref(*new (nothrow) GridTrackPlacementStyleValue(grid_track_placement));
}

String GridTrackPlacementStyleValue::to_string() const
{
    return m_grid_track_placement.to_string();
}

}
