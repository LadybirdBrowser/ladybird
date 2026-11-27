/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Infrastructure/FetchParams.h>

namespace Web::Fetch::Fetching {

[[nodiscard]] bool webdriver_bidi_bypasses_cache(Infrastructure::FetchParams const&);

void update_request_headers_for_webdriver_bidi(Infrastructure::FetchParams const&);

}
