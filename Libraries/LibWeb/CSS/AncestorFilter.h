/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::CSS {

// Separate otherwise identical string hashes by selector component kind,
// e.g. `.article` versus `<article>`.
static constexpr u32 ancestor_filter_hash_for_tag_name(u32 hash)
{
    return hash * 13;
}

static constexpr u32 ancestor_filter_hash_for_id(u32 hash)
{
    return hash * 17;
}

static constexpr u32 ancestor_filter_hash_for_class(u32 hash)
{
    return hash * 19;
}

static constexpr u32 ancestor_filter_hash_for_attribute(u32 hash)
{
    return hash * 23;
}

}
