/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-current-browsing-context
// https://w3c.github.io/webdriver/#dfn-current-top-level-browsing-context
enum class SessionBrowsingContext : u8 {
    Current,
    CurrentTopLevel,
};

}
