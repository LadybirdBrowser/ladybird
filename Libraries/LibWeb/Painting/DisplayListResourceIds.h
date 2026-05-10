/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FontResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ImageFrameResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ExternalContentResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, VideoFrameResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FilterResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, DisplayListResourceId);

}
