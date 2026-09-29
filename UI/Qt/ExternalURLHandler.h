/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/String.h>
#include <LibCore/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/ExternalURLHandler.h>

namespace Ladybird {

#if defined(AK_OS_LINUX)
void initialize_external_url_handler(Core::EventLoop&);
void resolve_external_url_handler(URL::URL const&, String ladybird_desktop_id, WebView::ExternalURLHandlerCallback);
#elif defined(AK_OS_MACOS)
void resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback);
#endif

}
