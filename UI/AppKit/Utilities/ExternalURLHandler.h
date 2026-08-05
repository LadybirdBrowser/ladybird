/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWebView/ExternalURLHandler.h>

namespace Ladybird {

void resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback);

}
