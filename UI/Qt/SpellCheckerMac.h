/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebView/SpellChecker.h>

namespace Ladybird {

NonnullOwnPtr<WebView::SpellChecker> create_platform_spell_checker();

}
