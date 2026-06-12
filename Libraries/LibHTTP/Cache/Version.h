/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace HTTP {

// Increment this version when a breaking change is made to the cache entry file format.
// Index-only schema changes are handled by the CacheIndex schema migrations instead.
static constexpr inline u32 CACHE_VERSION = 7u;

}
