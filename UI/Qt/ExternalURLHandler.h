/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebView/ExternalURLHandler.h>

namespace Core {

class EventLoop;

}

namespace Ladybird {

void initialize_external_url_handler(Core::EventLoop&);
void resolve_external_url_handler(URL::URL const&, String ladybird_desktop_id, WebView::ExternalURLHandlerCallback);

}
