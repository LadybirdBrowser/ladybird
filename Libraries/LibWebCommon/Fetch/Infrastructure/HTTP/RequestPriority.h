/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#request-priority
// A request has an associated priority, which is "high", "low", or "auto".
enum class RequestPriority {
    High,
    Low,
    Auto
};

}
