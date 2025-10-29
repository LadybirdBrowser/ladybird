/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace RequestServer {

// Increment this version when a breaking change is made to the cache index or cache entry formats.
static constexpr inline u32 CACHE_VERSION = 2u;

}
