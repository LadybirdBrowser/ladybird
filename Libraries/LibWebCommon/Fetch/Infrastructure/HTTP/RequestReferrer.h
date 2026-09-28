/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibURL/URL.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-request-referrer
// A request has an associated referrer, which is "no-referrer", "client", or a URL.
enum class RequestReferrer {
    NoReferrer,
    Client,
};

using RequestReferrerType = Variant<RequestReferrer, URL::URL>;

}
