/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// Opaque-response blocking, from the draft of https://github.com/whatwg/fetch/pull/1442

// Whether the opaque-response-safelist check is to decide if response to request is blocked.
[[nodiscard]] bool opaque_response_safelist_check_applies(Request const&, Response const&);

// The opaque-response-safelist check splits where it waits for the first bytes of the response's body: The first part
// answers when the response's headers and status settle it, and the second part takes those bytes, or none where
// reading them failed.
[[nodiscard]] Optional<bool> opaque_response_safelist_check_before_reading_bytes(Response const&);
[[nodiscard]] bool opaque_response_safelist_check_given_bytes(Response const&, Optional<ReadonlyBytes>);

}
