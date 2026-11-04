/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebView/Translator.h>

namespace Ladybird {

// Nothing where the platform can't translate pages, i.e. before macOS 26.4.
OwnPtr<WebView::Translator> create_platform_translator();

}
