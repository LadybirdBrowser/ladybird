/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::Fetch::Engine {

// The agent publishes it as an immutable snapshot for the fetch engine.
struct NetworkConfiguration final : public AtomicRefCounted<NetworkConfiguration> {
    [[nodiscard]] static String traversable_name(HTML::CrossProcessId id) { return MUST(String::formatted("{}-{}", id.namespace_id, id.local_id)); }

    bool bypass_cache_by_default { false };

    HashMap<String, bool> bypass_cache_by_traversable;

    Vector<HTTP::Header> default_headers;

    Vector<HTTP::Header> default_user_context_headers;

    HashMap<String, Vector<HTTP::Header>> traversable_headers;
};

}
