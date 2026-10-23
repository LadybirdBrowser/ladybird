/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Audio {

// Names the group an output session plays as part of. A client names the group of each session it declares, and the
// AudioServer plays a group only while the client's controller authorizes it.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, PlaybackGroupId);

}
