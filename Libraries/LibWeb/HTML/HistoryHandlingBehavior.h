/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/History.h>
#include <LibWeb/Bindings/Navigation.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#history-handling-behavior
enum class HistoryHandlingBehavior {
    Push,
    Replace,
};

using NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior;
using ScrollRestorationMode = Bindings::ScrollRestoration;

}
