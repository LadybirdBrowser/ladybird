/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-response-cache-state
// A response has an associated cache state (the empty string, "local", or "validated").
enum class ResponseCacheState {
    Local,
    Validated,
};

}
