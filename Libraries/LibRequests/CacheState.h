/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Requests {

// https://fetch.spec.whatwg.org/#concept-response-cache-state
enum class CacheState : u8 {
    NotCached,
    Local,
    Validated,
};

}
