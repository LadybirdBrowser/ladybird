/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace WebView {

enum class HistoryVisitTransition : u8 {
    Omnibox,
    Link,
    Redirect,
    Reload,
    Restore,
    Other,
};

}
