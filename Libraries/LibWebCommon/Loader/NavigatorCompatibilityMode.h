/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web {

enum class NavigatorCompatibilityMode {
    Chrome,
    Gecko,
    WebKit
};

enum class BrowserIdentity : u8 {
    WebKit,
};

}
