/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>

namespace Media {

struct IndexEntry {
    size_t position;
    AK::Duration timestamp;
};

}
