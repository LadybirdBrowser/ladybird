/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackSizeListStyleValue.h"

namespace Web::CSS {

String GridTrackSizeListStyleValue::to_string(SerializationMode mode) const
{
    return m_grid_track_size_list.to_string(mode);
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::create(CSS::GridTrackSizeList grid_track_size_list)
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(move(grid_track_size_list)));
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::make_auto()
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(CSS::GridTrackSizeList()));
}

ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> GridTrackSizeListStyleValue::make_none()
{
    return adopt_ref(*new (nothrow) GridTrackSizeListStyleValue(CSS::GridTrackSizeList()));
}

}
