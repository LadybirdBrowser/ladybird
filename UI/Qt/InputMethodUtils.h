/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/ViewImplementation.h>

#include <QVariant>

namespace Ladybird {

QVariant input_method_query_for_state(WebView::ViewImplementation::InputMethodState const&, Qt::InputMethodQuery, double device_pixel_ratio);

}
