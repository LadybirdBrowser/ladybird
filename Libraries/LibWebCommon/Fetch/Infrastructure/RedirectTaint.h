/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#response-redirect-taint
enum class RedirectTaint {
    SameOrigin,
    SameSite,
    CrossSite,
};

}
